#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

// ============================================================================
//                         DISPLAY INITIALIZATION
// ============================================================================
// GoldenMorning 1.3" I2C SH1106 OLED (PB8 = SCL, PB9 = SDA)
U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ============================================================================
//             EXACT SEQUENTIAL PIN DEFINITIONS MATCHING YOUR SOLDERING
// ============================================================================
// Pin 1: Common Ground (GND)
const int PIN_SW_UP    = PA0; // Pin 2: UP
const int PIN_SW_LEFT  = PA1; // Pin 3: LEFT (Decrease Value -)
const int PIN_SW_DOWN  = PA2; // Pin 4: DOWN
const int PIN_SW_RIGHT = PA3; // Pin 5: RIGHT (Increase Value +)
const int PIN_SW_OK    = PA4; // Pin 6: OK (Start / Resume)
const int PIN_SW_BACK  = PA5; // Pin 7: BACK (Stop / Pause / Reset)
const int PIN_SW_EMG   = PA6; // Pin 8: EMG / Extra Burst Trigger

// Right Header - Stepper Driver & Indicator LED
const int PIN_PUL      = PA8;  // Step Pulse -> DM860H PUL+
const int PIN_DIR      = PA9;  // Direction  -> DM860H DIR+
const int PIN_LED      = PC13; // Onboard Blue LED (Active LOW)

// ============================================================================
//                     USER-FRIENDLY CONFIGURATION
// ============================================================================
// 1. Angle: 0 to 400 degrees (default 360)
int targetAngle = 360;

// 2. Preset Speed Mode: 0 = SLOW, 1 = NORMAL, 2 = FAST, 3 = CUSTOM
int speedMode = 1; // Default NORMAL
const char* speedNames[] = {"SLOW", "NORMAL", "FAST", "CUSTOM"};

// 3. Direct Speed Delay in Microseconds (Adjustable in 50 us steps)
uint32_t cruiseDelayUs = 1000; // Default matches NORMAL (1000 us)

// 4. Target Cycles: default 1000 (Adjustable 100 to 10000)
int targetCycles = 1000;

// Hardware constants
// 1600 microstepping * 10:1 planetary gearbox = 16,000 steps per 360° (~44.44 steps/deg)
const uint32_t RAMP_STEPS     = 1000;
const uint32_t START_DELAY_US = 3500;
const uint32_t DWELL_MS       = 800; // Pause between reversals

// Menu Navigation: 0=Mode, 1=Delay, 2=Angle, 3=Cycles
int currentSelection = 0;
bool isRunning       = false;
uint32_t doneCycles  = 0;

// ============================================================================
//                         STEPPER PULSE GENERATOR
// ============================================================================
void sendStep(uint32_t delayUs) {
  digitalWrite(PIN_PUL, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_PUL, LOW);
  delayMicroseconds(delayUs);
}

void run3PhaseSwing(bool forward, uint32_t cruiseSteps, uint32_t cruiseDelay) {
  digitalWrite(PIN_DIR, forward ? HIGH : LOW);
  delayMicroseconds(50);

  // Phase 1: Soft Launch Ramp
  for (uint32_t s = 0; s < RAMP_STEPS; s++) {
    if (digitalRead(PIN_SW_BACK) == LOW) { isRunning = false; return; }
    uint32_t d = START_DELAY_US - ((START_DELAY_US - cruiseDelay) * s / RAMP_STEPS);
    sendStep(d);
  }

  // Phase 2: Constant Speed Cruise
  for (uint32_t s = 0; s < cruiseSteps; s++) {
    if (digitalRead(PIN_SW_BACK) == LOW) { isRunning = false; return; }
    sendStep(cruiseDelay);
  }

  // Phase 3: Soft Stop Ramp
  for (uint32_t s = 0; s < RAMP_STEPS; s++) {
    if (digitalRead(PIN_SW_BACK) == LOW) { isRunning = false; return; }
    uint32_t rem = RAMP_STEPS - s;
    uint32_t d = START_DELAY_US - ((START_DELAY_US - cruiseDelay) * rem / RAMP_STEPS);
    sendStep(d);
  }
}

// ============================================================================
//                         OLED DISPLAY SCREENS
// ============================================================================
void drawMenuScreen() {
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(16, 10, "WRIST RIG SETUP");
  u8g2.drawHLine(0, 13, 128);

  // Line 0: Speed Preset Mode
  u8g2.drawStr(0, 24, (currentSelection == 0) ? "> MODE :" : "  MODE :");
  u8g2.drawStr(64, 24, speedNames[speedMode]);

  // Line 1: Speed Delay (us)
  u8g2.drawStr(0, 36, (currentSelection == 1) ? "> DELAY:" : "  DELAY:");
  u8g2.setCursor(64, 36);
  u8g2.print(cruiseDelayUs);
  u8g2.print("us");

  // Line 2: Angle
  u8g2.drawStr(0, 48, (currentSelection == 2) ? "> ANGLE:" : "  ANGLE:");
  u8g2.setCursor(64, 48);
  u8g2.print(targetAngle);
  u8g2.print((char)176);

  // Line 3: Cycles
  u8g2.drawStr(0, 60, (currentSelection == 3) ? "> CYC  :" : "  CYC  :");
  u8g2.setCursor(64, 60);
  u8g2.print(targetCycles);
}

void drawRunningScreen() {
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(12, 10, "ENDURANCE TESTING");
  u8g2.drawHLine(0, 12, 128);

  // Large Cycle Number
  u8g2.setFont(u8g2_font_9x15B_tr);
  u8g2.setCursor(6, 27);
  u8g2.print(doneCycles);
  u8g2.print(" / ");
  u8g2.print(targetCycles);

  // Details Line
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.setCursor(6, 41);
  u8g2.print(targetAngle);
  u8g2.print((char)176);
  u8g2.print(" | ");
  u8g2.print(cruiseDelayUs);
  u8g2.print("us (");
  u8g2.print(speedNames[speedMode]);
  u8g2.print(")");

  // Visual Progress Bar
  u8g2.drawFrame(6, 47, 116, 10);
  int progress = (doneCycles * 112) / targetCycles;
  if (progress > 112) progress = 112;
  u8g2.drawBox(8, 49, progress, 6);

  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(22, 63, "Press BACK to Pause");
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
//                         BUTTON HANDLER & NAVIGATION
// ============================================================================
void handleButtons() {
  // 1. EMG Replica Burst Trigger (Pin PA6)
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

  // 2. BACK Button (Pin PA5): Pause / Stop
  if (digitalRead(PIN_SW_BACK) == LOW) {
    delay(200);
    isRunning = false;
    updateOLED();
    return;
  }

  // 3. OK Button (Pin PA4): Start / Resume Test
  if (digitalRead(PIN_SW_OK) == LOW) {
    delay(200);
    if (!isRunning) {
      if (doneCycles >= (uint32_t)targetCycles) doneCycles = 0; // Restart if completed
      isRunning = true;
      updateOLED();
    }
    return;
  }

  if (isRunning) return; // Freeze menu adjustments while motor is running

  // 4. UP Button (Pin PA0): Move selection Up
  if (digitalRead(PIN_SW_UP) == LOW) {
    delay(150);
    currentSelection = (currentSelection > 0) ? currentSelection - 1 : 3;
    updateOLED();
    return;
  }

  // 5. DOWN Button (Pin PA2): Move selection Down
  if (digitalRead(PIN_SW_DOWN) == LOW) {
    delay(150);
    currentSelection = (currentSelection < 3) ? currentSelection + 1 : 0;
    updateOLED();
    return;
  }

  // 6. RIGHT Button (Pin PA3): Increase Value (+)
  if (digitalRead(PIN_SW_RIGHT) == LOW) {
    delay(120);
    if (currentSelection == 0) { // MODE
      if (speedMode < 2) {
        speedMode++;
        if (speedMode == 0) cruiseDelayUs = 2000;
        if (speedMode == 1) cruiseDelayUs = 1000;
        if (speedMode == 2) cruiseDelayUs = 500;
      }
    } else if (currentSelection == 1) { // DELAY (Increase delay = slower speed)
      if (cruiseDelayUs < 3000) {
        cruiseDelayUs += 50;
        speedMode = 3; // Switch label to CUSTOM
      }
    } else if (currentSelection == 2) { // ANGLE (+10 deg)
      if (targetAngle < 400) targetAngle += 10;
    } else if (currentSelection == 3) { // CYCLES (+100 cycles)
      if (targetCycles < 10000) targetCycles += 100;
    }
    updateOLED();
    return;
  }

  // 7. LEFT Button (Pin PA1): Decrease Value (-)
  if (digitalRead(PIN_SW_LEFT) == LOW) {
    delay(120);
    if (currentSelection == 0) { // MODE
      if (speedMode > 0) {
        if (speedMode == 3) speedMode = 2; // Return from CUSTOM to FAST
        else speedMode--;
        if (speedMode == 0) cruiseDelayUs = 2000;
        if (speedMode == 1) cruiseDelayUs = 1000;
        if (speedMode == 2) cruiseDelayUs = 500;
      }
    } else if (currentSelection == 1) { // DELAY (Decrease delay = faster speed)
      if (cruiseDelayUs > 250) {
        cruiseDelayUs -= 50;
        speedMode = 3; // Switch label to CUSTOM
      }
    } else if (currentSelection == 2) { // ANGLE (-10 deg)
      if (targetAngle > 0) targetAngle -= 10;
    } else if (currentSelection == 3) { // CYCLES (-100 cycles)
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

  // Set all 7 switches with internal pull-up (active LOW)
  pinMode(PIN_SW_UP,    INPUT_PULLUP);
  pinMode(PIN_SW_LEFT,  INPUT_PULLUP);
  pinMode(PIN_SW_DOWN,  INPUT_PULLUP);
  pinMode(PIN_SW_RIGHT, INPUT_PULLUP);
  pinMode(PIN_SW_OK,    INPUT_PULLUP);
  pinMode(PIN_SW_BACK,  INPUT_PULLUP);
  pinMode(PIN_SW_EMG,   INPUT_PULLUP);

  Serial.begin(115200);

  // Hardware I2C for GoldenMorning OLED
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
    digitalWrite(PIN_LED, LOW); // LED ON while moving

    // Calculate pulse count for selected angle (16000 steps per 360°)
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

    digitalWrite(PIN_LED, HIGH); // LED OFF during dwell
    doneCycles++;
    updateOLED();

    Serial.print("Cycle: ");
    Serial.print(doneCycles);
    Serial.print(" / ");
    Serial.println(targetCycles);
  } else if (doneCycles >= (uint32_t)targetCycles && isRunning) {
    isRunning = false;
    updateOLED();
    Serial.println(">> TARGET TEST CYCLES COMPLETED. <<");
  }
}