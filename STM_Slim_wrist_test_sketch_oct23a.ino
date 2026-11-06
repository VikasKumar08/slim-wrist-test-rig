/**
 * ============================================================================
 * Project:      SLIM WRIST DURABILITY & ENDURANCE TEST RIG
 * Platform:     STM32F411CEU6 (WeAct Black Pill)
 * Display:      GoldenMorning 1.3" I2C OLED (SH1106, 128x64)
 * Driver/Motor: DM860H Digital Microstep Driver + NEMA 23 + 10:1 Planetary Gearbox
 * Resolution:   16,000 steps / 360 degrees (~44.444 steps/degree)
 * Features:     3-Phase Motion, Live Estimated Remaining Time (ETA),
 *               Remaining Cycles Countdown, Preset/Custom Delay Control
 * ============================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

// ============================================================================
//                         DISPLAY INITIALIZATION
// ============================================================================
// GoldenMorning 1.3" I2C SH1106 OLED (Hardware I2C: PB8 = SCL, PB9 = SDA)
U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ============================================================================
//             EXACT SEQUENTIAL PIN DEFINITIONS MATCHING YOUR SOLDERING
// ============================================================================
// Pin 1: Common Ground (GND rail on perfboard)
const int PIN_SW_UP    = PA0; // Pin 2: UP
const int PIN_SW_LEFT  = PA1; // Pin 3: LEFT  (Decrement -)
const int PIN_SW_DOWN  = PA2; // Pin 4: DOWN
const int PIN_SW_RIGHT = PA3; // Pin 5: RIGHT (Increment +)
const int PIN_SW_OK    = PA4; // Pin 6: OK    (Start / Resume)
const int PIN_SW_BACK  = PA5; // Pin 7: BACK  (Pause / Stop / Reset)
const int PIN_SW_EMG   = PA6; // Pin 8: EMG   (Manual trigger / prosthetic burst)

// Right Header - Stepper Control & Status
const int PIN_PUL      = PA8;  // Step Pulse -> DM860H PUL+
const int PIN_DIR      = PA9;  // Direction  -> DM860H DIR+
const int PIN_LED      = PC13; // Onboard Blue LED (Active LOW)

// ============================================================================
//                     USER-CONFIGURABLE PARAMETERS
// ============================================================================
// Rotation Angle: 0 to 400 degrees (Default: 360 deg)
int targetAngle = 360;

// Speed Presets: 0=SLOW (2000us), 1=NORMAL (1000us), 2=FAST (500us), 3=CUSTOM
int speedMode = 1;
const char* speedNames[] = {"SLOW", "NORMAL", "FAST", "CUSTOM"};

// Cruising Step Delay in microseconds (Adjustable in 50us steps)
uint32_t cruiseDelayUs = 1000;

// Target Cycle Count: 100 to 10,000 (Default: 1000)
int targetCycles = 1000;

// Mechanical & Ramping Constants
const uint32_t RAMP_STEPS     = 1000; // Step count for smooth acceleration/deceleration
const uint32_t START_DELAY_US = 3500; // Launch/landing speed delay
const uint32_t DWELL_MS       = 800;  // Pause duration between reversals (ms)

// Runtime State Machine
int currentSelection = 0; // 0=MODE, 1=DELAY, 2=ANGLE, 3=CYCLES
bool isRunning       = false;
uint32_t doneCycles  = 0;

// ============================================================================
//                         STEPPER PULSE GENERATOR
// ============================================================================
void sendStep(uint32_t delayUs) {
  digitalWrite(PIN_PUL, HIGH);
  delayMicroseconds(10); // Optocoupler latching threshold
  digitalWrite(PIN_PUL, LOW);
  delayMicroseconds(delayUs);
}

// 3-Phase Motion: Startup Ramp -> Cruise Rotation -> Stopping Ramp
void run3PhaseSwing(bool forward, uint32_t cruiseSteps, uint32_t cruiseDelay) {
  digitalWrite(PIN_DIR, forward ? HIGH : LOW);
  delayMicroseconds(50); // Direction setup time

  // Phase 1: Soft Launch Acceleration
  for (uint32_t s = 0; s < RAMP_STEPS; s++) {
    if (digitalRead(PIN_SW_BACK) == LOW) { isRunning = false; return; }
    uint32_t d = START_DELAY_US - ((START_DELAY_US - cruiseDelay) * s / RAMP_STEPS);
    sendStep(d);
  }

  // Phase 2: Pure Constant-Speed Cruising
  for (uint32_t s = 0; s < cruiseSteps; s++) {
    if (digitalRead(PIN_SW_BACK) == LOW) { isRunning = false; return; }
    sendStep(cruiseDelay);
  }

  // Phase 3: Soft Stop Deceleration
  for (uint32_t s = 0; s < RAMP_STEPS; s++) {
    if (digitalRead(PIN_SW_BACK) == LOW) { isRunning = false; return; }
    uint32_t rem = RAMP_STEPS - s;
    uint32_t d = START_DELAY_US - ((START_DELAY_US - cruiseDelay) * rem / RAMP_STEPS);
    sendStep(d);
  }
}

// ============================================================================
//                         ETA / TIME ESTIMATION HELPER
// ============================================================================
uint32_t calculateRemainingTimeSec() {
  if (doneCycles >= (uint32_t)targetCycles) return 0;
  uint32_t remainingCycles = targetCycles - doneCycles;

  // Single stroke cruise steps (16,000 steps per 360 degrees)
  uint32_t cruiseSteps = (uint32_t)((16000.0 / 360.0) * targetAngle);

  // Time for 1 acceleration ramp: RAMP_STEPS * average delay in seconds
  float rampTimeSec = (float)RAMP_STEPS * (((float)(START_DELAY_US + cruiseDelayUs) / 2.0) + 10.0) / 1000000.0;
  // Time for cruise phase
  float cruiseTimeSec = (float)cruiseSteps * ((float)cruiseDelayUs + 10.0) / 1000000.0;

  // 1 full cycle = 2 strokes (Forward + Backward) + 2 dwells
  float strokeTimeSec = (2.0 * rampTimeSec) + cruiseTimeSec;
  float cycleTimeSec  = (2.0 * strokeTimeSec) + ((2.0 * (float)DWELL_MS) / 1000.0);

  return (uint32_t)(remainingCycles * cycleTimeSec);
}

// ============================================================================
//                         OLED DISPLAY RENDERING
// ============================================================================
void drawMenuScreen() {
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(16, 10, "WRIST RIG SETUP");
  u8g2.drawHLine(0, 13, 128);

  // Parameter Lines
  u8g2.drawStr(0, 24, (currentSelection == 0) ? "> MODE :" : "  MODE :");
  u8g2.drawStr(64, 24, speedNames[speedMode]);

  u8g2.drawStr(0, 36, (currentSelection == 1) ? "> DELAY:" : "  DELAY:");
  u8g2.setCursor(64, 36);
  u8g2.print(cruiseDelayUs);
  u8g2.print("us");

  u8g2.drawStr(0, 48, (currentSelection == 2) ? "> ANGLE:" : "  ANGLE:");
  u8g2.setCursor(64, 48);
  u8g2.print(targetAngle);
  u8g2.print((char)176);

  u8g2.drawStr(0, 60, (currentSelection == 3) ? "> CYC  :" : "  CYC  :");
  u8g2.setCursor(64, 60);
  u8g2.print(targetCycles);
}

void drawRunningScreen() {
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(12, 10, "ENDURANCE TESTING");
  u8g2.drawHLine(0, 12, 128);

  // Line 1: Cycles Completed & Total Target
  u8g2.setFont(u8g2_font_7x14B_tr);
  u8g2.setCursor(4, 25);
  u8g2.print("CYC: ");
  u8g2.print(doneCycles);
  u8g2.print(" / ");
  u8g2.print(targetCycles);

  // Line 2: Remaining Cycles & Remaining Time (ETA)
  uint32_t remCycles = (targetCycles > doneCycles) ? (targetCycles - doneCycles) : 0;
  uint32_t totalSec  = calculateRemainingTimeSec();
  uint32_t hrs = totalSec / 3600;
  uint32_t mins = (totalSec % 3600) / 60;
  uint32_t secs = totalSec % 60;

  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.setCursor(4, 37);
  u8g2.print("REM: ");
  u8g2.print(remCycles);
  u8g2.print(" | ");

  // Format ETA HH:MM:SS
  if (hrs < 10) u8g2.print("0");
  u8g2.print(hrs);
  u8g2.print(":");
  if (mins < 10) u8g2.print("0");
  u8g2.print(mins);
  u8g2.print(":");
  if (secs < 10) u8g2.print("0");
  u8g2.print(secs);

  // Line 3: Configuration info
  u8g2.setCursor(4, 48);
  u8g2.print(targetAngle);
  u8g2.print((char)176);
  u8g2.print(" | ");
  u8g2.print(cruiseDelayUs);
  u8g2.print("us (");
  u8g2.print(speedNames[speedMode]);
  u8g2.print(")");

  // Visual Progress Bar
  u8g2.drawFrame(4, 52, 120, 8);
  int progress = (doneCycles * 116) / targetCycles;
  if (progress > 116) progress = 116;
  u8g2.drawBox(6, 54, progress, 4);

  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(22, 64, "Press BACK to Pause");
}

void updateOLED() {
  u8g2.firstPage();
  do {
    if (isRunning) {
      drawRunningScreen();
    } else {
      drawMenuScreen();
    }
  } while (u8g2.nextPage());
}

// ============================================================================
//                         KEYPAD & MENU SCANNER
// ============================================================================
void handleButtons() {
  // 1. EMG Burst Test (Pin PA6)
  if (digitalRead(PIN_SW_EMG) == LOW) {
    delay(150);
    for (int i = 0; i < 4; i++) {
      digitalWrite(PIN_LED, LOW);
      delay(40);
      digitalWrite(PIN_LED, HIGH);
      delay(40);
    }
    return;
  }

  // 2. BACK Button (Pin PA5): Pause / Stop / Menu
  if (digitalRead(PIN_SW_BACK) == LOW) {
    delay(200);
    isRunning = false;
    updateOLED();
    return;
  }

  // 3. OK Button (Pin PA4): Start / Resume
  if (digitalRead(PIN_SW_OK) == LOW) {
    delay(200);
    if (!isRunning) {
      if (doneCycles >= (uint32_t)targetCycles) doneCycles = 0; // Reset counter if finished
      isRunning = true;
      updateOLED();
    }
    return;
  }

  if (isRunning) return; // Prevent setting changes while motor is moving

  // 4. UP Button (Pin PA0): Move Selection Up
  if (digitalRead(PIN_SW_UP) == LOW) {
    delay(150);
    currentSelection = (currentSelection > 0) ? currentSelection - 1 : 3;
    updateOLED();
    return;
  }

  // 5. DOWN Button (Pin PA2): Move Selection Down
  if (digitalRead(PIN_SW_DOWN) == LOW) {
    delay(150);
    currentSelection = (currentSelection < 3) ? currentSelection + 1 : 0;
    updateOLED();
    return;
  }

  // 6. RIGHT Button (Pin PA3): Increment (+)
  if (digitalRead(PIN_SW_RIGHT) == LOW) {
    delay(120);
    if (currentSelection == 0) { // MODE
      if (speedMode < 2) {
        speedMode++;
        if (speedMode == 0) cruiseDelayUs = 2000;
        if (speedMode == 1) cruiseDelayUs = 1000;
        if (speedMode == 2) cruiseDelayUs = 500;
      }
    } else if (currentSelection == 1) { // DELAY
      if (cruiseDelayUs < 3000) {
        cruiseDelayUs += 50;
        speedMode = 3; // Switched to CUSTOM
      }
    } else if (currentSelection == 2) { // ANGLE (+10 deg)
      if (targetAngle < 400) targetAngle += 10;
    } else if (currentSelection == 3) { // CYCLES (+100)
      if (targetCycles < 10000) targetCycles += 100;
    }
    updateOLED();
    return;
  }

  // 7. LEFT Button (Pin PA1): Decrement (-)
  if (digitalRead(PIN_SW_LEFT) == LOW) {
    delay(120);
    if (currentSelection == 0) { // MODE
      if (speedMode > 0) {
        if (speedMode == 3) speedMode = 2;
        else speedMode--;
        if (speedMode == 0) cruiseDelayUs = 2000;
        if (speedMode == 1) cruiseDelayUs = 1000;
        if (speedMode == 2) cruiseDelayUs = 500;
      }
    } else if (currentSelection == 1) { // DELAY
      if (cruiseDelayUs > 250) {
        cruiseDelayUs -= 50;
        speedMode = 3; // Switched to CUSTOM
      }
    } else if (currentSelection == 2) { // ANGLE (-10 deg)
      if (targetAngle > 0) targetAngle -= 10;
    } else if (currentSelection == 3) { // CYCLES (-100)
      if (targetCycles > 100) targetCycles -= 100;
    }
    updateOLED();
    return;
  }
}

// ============================================================================
//                                SETUP
// ============================================================================
void setup() {
  pinMode(PIN_PUL, OUTPUT);
  pinMode(PIN_DIR, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_PUL, LOW);
  digitalWrite(PIN_DIR, LOW);
  digitalWrite(PIN_LED, HIGH);

  // Set up all 7 tactile switches with internal pull-ups
  pinMode(PIN_SW_UP,    INPUT_PULLUP);
  pinMode(PIN_SW_LEFT,  INPUT_PULLUP);
  pinMode(PIN_SW_DOWN,  INPUT_PULLUP);
  pinMode(PIN_SW_RIGHT, INPUT_PULLUP);
  pinMode(PIN_SW_OK,    INPUT_PULLUP);
  pinMode(PIN_SW_BACK,  INPUT_PULLUP);
  pinMode(PIN_SW_EMG,   INPUT_PULLUP);

  Serial.begin(115200);

  // Start I2C1 for GoldenMorning OLED
  Wire.setSCL(PB8);
  Wire.setSDA(PB9);
  u8g2.begin();

  updateOLED();
}

// ============================================================================
//                                MAIN LOOP
// ============================================================================
void loop() {
  handleButtons();

  if (isRunning && (doneCycles < (uint32_t)targetCycles)) {
    digitalWrite(PIN_LED, LOW); // LED ON while motor is running

    // Calculate step count for target angle
    uint32_t steps = (uint32_t)((16000.0 / 360.0) * targetAngle);

    if (steps > 0) {
      // 1. Forward Stroke
      run3PhaseSwing(true, steps, cruiseDelayUs);
      if (!isRunning) { updateOLED(); return; }
      delay(DWELL_MS);

      // 2. Backward Stroke
      run3PhaseSwing(false, steps, cruiseDelayUs);
      if (!isRunning) { updateOLED(); return; }
      delay(DWELL_MS);
    }

    digitalWrite(PIN_LED, HIGH); // LED OFF during dwell pause
    doneCycles++;
    updateOLED();

    // Telemetry output to Serial
    uint32_t totalSec = calculateRemainingTimeSec();
    Serial.print("Cycle: ");
    Serial.print(doneCycles);
    Serial.print(" / ");
    Serial.print(targetCycles);
    Serial.print(" | ETA: ");
    Serial.print(totalSec / 3600);
    Serial.print("h ");
    Serial.print((totalSec % 3600) / 60);
    Serial.println("m");
  } else if (doneCycles >= (uint32_t)targetCycles && isRunning) {
    isRunning = false;
    updateOLED();
    Serial.println(">> TARGET TEST CYCLES COMPLETED. <<");
  }
}
