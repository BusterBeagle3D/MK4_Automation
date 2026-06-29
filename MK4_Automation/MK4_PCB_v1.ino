#define ENCODER_DO_NOT_USE_INTERRUPTS
#include <Encoder.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <avr/wdt.h>
#include <Servo.h>
#include <EEPROM.h>

// -------------------- LCD: Auto-pick address (Option A) --------------------
LiquidCrystal_I2C* lcd = nullptr;

// Try these addresses in order 
const uint8_t LCD_CANDIDATE_ADDRS[] = { 
  0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
  0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F
};

bool i2cDevicePresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

uint8_t pickLcdAddress() {
  for (uint8_t i = 0; i < (sizeof(LCD_CANDIDATE_ADDRS) / sizeof(LCD_CANDIDATE_ADDRS[0])); i++) {
    uint8_t addr = LCD_CANDIDATE_ADDRS[i];
    if (i2cDevicePresent(addr)) return addr;
  }
  return 0; // none found
}
// --------------------------------------------------------------------------

const int clkPin = 2;
const int dtPin = 4;
const int ENCODER_BTN_PIN = 7;
const int visePin = 9;  
const int injectPin = 6;
const int hopperPin = 10;  
const int servoPin = 11; // Control for the part ejection
const int partDropPin = 15; // Optical sensor for part drop detection (A1)
const int pneumaticInjection = 5; // Pneumatic ejection control
const int hallSensorPin = 16; // Hall sensor pin (A2)
const int injectHallSensorPin = 14; // New Hall effect sensor for injection (A0)
const int BUZZER_PIN = 3;

bool partDropped = false; // Global variable to track part drop

int menuIndex = 0;
unsigned int injectTime = 0;
float viseHoldTime = 0;
float shotSize = 0.0;
unsigned int numOfParts = 0;
float cyclePause = 0;
unsigned int noDetectionCount = 0; // Count the number of cycles without detection
unsigned long hallSensorStartTime = 0; // Track the start time when Hall sensor is detected

unsigned long shotEndTime;
unsigned long viseRemainingTime;
unsigned long cycleRemainingTime;

int lastClk = HIGH;
int lastDt = HIGH;

enum MenuState {
  MAIN_MENU,
  START_JOB
};

MenuState menuState = MAIN_MENU;

unsigned long startTime = 0; // To store the start time for LED

int currentPart = 0; // Track the current part in the sequence
bool isSequenceActive = false; // Flag to track if the sequence is active
unsigned long lastSequenceTime = 0; // Store the last time the sequence started
int partsLeft = 0; // Number of parts left to process

// --- NEW: Simple noise-filtered read for part-drop (LOW = detected with pull-up)
bool partDroppedRead() {
  const uint32_t windowMs = 15;
  const uint16_t minHits  = 8;
  uint16_t hits = 0;
  uint32_t start = millis();
  while (millis() - start < windowMs) {
    if (digitalRead(partDropPin) == LOW) hits++;
  }
  return hits >= minHits;
}

void startupTune() {

  delay(1000);   // <-- 4 second pause before the tune starts

  const int notes[] = {
    392,  // G
    440,  // A
    392,  // G
    349,  // F
    523,   // High C
    261,
    174
  };

  const int durations[] = {
    140,
    140,
    140,
    160,
    400,
    400,
    200  
  };

  for (int i = 0; i < 7; i++) {
    tone(BUZZER_PIN, notes[i]);
    delay(durations[i]);
    noTone(BUZZER_PIN);
    delay(70);
  }
}

void jobDoneChime() {
  // C major arpeggio: C, E, G, high C
  const int notes[] = {
    523,  // C5
    659,  // E5
    784,  // G5
    1047  // C6 (high C)
  };

  const int durations[] = {
    140,
    140,
    140,
    320   // longer final note
  };

  for (int i = 0; i < 4; i++) {
    tone(BUZZER_PIN, notes[i]);
    delay(durations[i]);
    noTone(BUZZER_PIN);
    delay(50);
  }
}

void errorTone() {
  // Louder descending error tone (resonant range)
  const int notes[] = { 2800, 2200, 1600 };
  const int dur[]   = { 160, 180, 260 };

  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, notes[i]);
    delay(dur[i]);
    noTone(BUZZER_PIN);
    delay(90);
  }
}

void updateLCD() {
  // Calculate the base row for menu items based on menuIndex
  int baseRow = max(0, menuIndex - 3); // Adjusted to show 4 items

  // Print menu items with adjusted row
  for (int i = 0; i < 4; i++) {
    lcd->setCursor(0, i);
    lcd->print("                    "); // Clear the entire row
    lcd->setCursor(0, i);
    if (baseRow + i == 9) {
      lcd->print(" ");
    } else {
      if (i == menuIndex - baseRow) {
        lcd->print(">");
      }
      if (baseRow + i == 0) {
        lcd->print("Inject Time: ");
        lcd->print(injectTime);
        lcd->print("s");
        lcd->print("  ");
      } else if (baseRow + i == 1) {
        lcd->print("Vise Hold: ");
        lcd->print(viseHoldTime, 0);
        lcd->print("s");
        lcd->print("  ");
      } else if (baseRow + i == 2) {
        lcd->print("Hopper Time: ");
        lcd->print(shotSize, 0); // Display shotSize with no decimal place
        lcd->print("s");
        lcd->print("  ");
      } else if (baseRow + i == 3) {
        lcd->print("# of Parts: ");
        lcd->print(numOfParts);
        lcd->print("  ");
      } else if (baseRow + i == 4) {
        lcd->print("Cycle Pause: ");
        lcd->print(cyclePause, 0);
        lcd->print("s");
        lcd->print("  ");
      } else if (baseRow + i == 5) {
        lcd->print("Manual Run Injection");
      } else if (baseRow + i == 6) {
        lcd->print("Manual Run Hopper");
      } else if (baseRow + i == 7) {
        lcd->print("START");
      } else if (baseRow + i == 8) {
        lcd->print("RESET");
      }
    }
  }
}

void servoMotorRun(int pin, bool &partDroppedGlobal) {
  Servo myservo;
  myservo.attach(pin);                 // FIX: use the parameter pin

  // Tune these:
  const int SERVO_STOP    = 90;        // true stop is often 88–92
  const int SERVO_FWD     = 0;         // one direction
  const int SERVO_REV     = 180;       // opposite direction

  const unsigned long forwardMs = 2000;  // how long to go "out"  (increase = farther)
  const unsigned long pauseMs   = 0;   // stop pause
  unsigned long returnMs        = 2000;  // how long to come back (tune to land on "home")

  // Ensure a known start state
  myservo.write(SERVO_STOP);
  delay(100);

  // Go forward
  myservo.write(SERVO_FWD);
  unsigned long t0 = millis();
  while (millis() - t0 < forwardMs) {
    if (partDroppedRead()) { partDroppedGlobal = true; break; }
  }

  // Stop
  myservo.write(SERVO_STOP);
  delay(pauseMs);

  // Go back (only if you still want to return even after a drop, remove this if-block)
  myservo.write(SERVO_REV);
  t0 = millis();
  while (millis() - t0 < returnMs) {
    if (partDroppedRead()) { partDroppedGlobal = true; } // keep updating if you want
  }

  // Final stop + detach
  myservo.write(SERVO_STOP);
  delay(50);
  myservo.detach();

  if (partDroppedGlobal) {
    lcd->clear();
    lcd->setCursor((20 - 14) / 2, 1);
    lcd->print("Part Dropped!");
    delay(1000);
  }
}

void runSosMorseCode(int pin) {
  const int dotTime = 133;  // Duration of a dot in milliseconds (50% faster)
  const int dashTime = 400; // Duration of a dash in milliseconds (50% faster)
  const int gapTime = 133;  // Gap between symbols (50% faster)

  unsigned long partDropStartTime = millis();
  bool _partDropped = false;

  // S: ... (3 dots)
  for (int i = 0; i < 3; i++) {
    digitalWrite(pin, HIGH);
    delay(dotTime);
    digitalWrite(pin, LOW);
    delay(gapTime);

    if (partDroppedRead()) { // LOW = detected
      _partDropped = true;
      break;
    }
  }

  // O: --- (3 dashes)
  for (int i = 0; i < 3 && !_partDropped; i++) {
    digitalWrite(pin, HIGH);
    delay(dashTime);
    digitalWrite(pin, LOW);
    delay(gapTime);

    if (partDroppedRead()) {
      _partDropped = true;
      break;
    }
  }

  // S: ... (3 dots)
  for (int i = 0; i < 3 && !_partDropped; i++) {
    digitalWrite(pin, HIGH);
    delay(dotTime);
    digitalWrite(pin, LOW);
    delay(gapTime);

    if (partDroppedRead()) {
      _partDropped = true;
      break;
    }
  }
}

void valueAdjustment();
void adjustValue(int direction);

void setup() {
  // I2C up first, so we can detect LCD address
  Wire.begin();

  uint8_t lcdAddr = pickLcdAddress();
  if (lcdAddr == 0) {
    // LCD not found at any candidate address
    // Optional: beep forever to indicate failure
    pinMode(BUZZER_PIN, OUTPUT);
    while (true) {
      tone(BUZZER_PIN, 1200); delay(120);
      noTone(BUZZER_PIN);     delay(380);
    }
  }

  lcd = new LiquidCrystal_I2C(lcdAddr, 20, 4);
  lcd->init();
  lcd->backlight();

  pinMode(BUZZER_PIN, OUTPUT);   // move this BEFORE the tune

  lcd->setCursor((20 - 15) / 2, 1);
  lcd->print("Buster Beagle 3D");
  lcd->setCursor((20 - 3) / 2, 2);
  lcd->print("MK4");
  lcd->setCursor((20 - 9) / 2, 3);
  lcd->print("VER 2.0.0");

  startupTune();

  delay(500);

  lcd->clear();
  pinMode(clkPin, INPUT_PULLUP);
  pinMode(dtPin, INPUT_PULLUP);
  pinMode(ENCODER_BTN_PIN, INPUT_PULLUP);
  pinMode(visePin, OUTPUT);
  pinMode(injectPin, OUTPUT);
  pinMode(hopperPin, OUTPUT);
  pinMode(servoPin, OUTPUT);
  //pinMode(partDropPin, INPUT);
  pinMode(partDropPin, INPUT_PULLUP); // CHANGED: default HIGH, detection pulls LOW
  pinMode(pneumaticInjection, OUTPUT);
  pinMode(hallSensorPin, INPUT); // Hall sensor as input with pull-up
  pinMode(injectHallSensorPin, INPUT_PULLUP); // Injection Hall sensor with pull-up

  digitalWrite(visePin, LOW);   // Open the vise initially
  digitalWrite(injectPin, LOW); // Open the Pneumatic Cylinder initially
  digitalWrite(hopperPin, LOW); // Turn off Hopper initially

  EEPROM.get(0, injectTime);
  EEPROM.get(4, viseHoldTime);
  if (isnan(viseHoldTime) || viseHoldTime <= 0) viseHoldTime = 1; // Ensure it's not NaN or zero
  EEPROM.get(8, shotSize);
  if (isnan(shotSize) || shotSize <= 0) shotSize = 1; // Ensure it's not NaN or zero
  EEPROM.get(12, numOfParts);
  EEPROM.get(16, cyclePause);
  if (isnan(cyclePause) || cyclePause <= 0) cyclePause = 1; // Ensure it's not NaN or zero

  if (injectTime == 0xFFFF || injectTime == 0) injectTime = 20; // Initial inject time in seconds
  if (viseHoldTime == 0xFFFF || viseHoldTime == 0) viseHoldTime = 30; // Initial vise hold time in seconds
  if (shotSize == 0xFFFF || shotSize == 0) shotSize = 20; // Initial shot size in seconds
  if (numOfParts == 0xFFFF || numOfParts == 0) numOfParts = 10; // Initial number of parts
  if (cyclePause == 0xFFFF || cyclePause == 0) cyclePause = 60; // Initial cycle pause in seconds

  updateLCD();
}

void loop() {
  int newClk = digitalRead(clkPin);
  int newDt = digitalRead(dtPin);

  // Process rotary encoder change
  if (newClk == LOW) {
    if (newDt == HIGH) {
      if (menuState == MAIN_MENU) {
        menuIndex = min(menuIndex + 1, 9); // Adjusted for the new line
        updateLCD();
      }
    } else if (newDt == LOW) {
      if (menuState == MAIN_MENU) {
        menuIndex = max(menuIndex - 1, 0); // Adjusted for the new line
        updateLCD();
      }
    }
  }

  lastClk = newClk;
  lastDt = newDt;

  // Existing button press logic and sequence logic (unchanged)
  boolean buttonState = digitalRead(ENCODER_BTN_PIN);
  if (buttonState == LOW && menuState == MAIN_MENU) {
    // Button is pressed while on MAIN_MENU
    if (menuIndex == 7) {
      // Save current settings to EEPROM before starting the job
      EEPROM.put(0, injectTime);
      EEPROM.put(4, viseHoldTime);
      EEPROM.put(8, shotSize);
      EEPROM.put(12, numOfParts);
      EEPROM.put(16, cyclePause);

      // Switch to START_JOB state and initialize the sequence
      menuState = START_JOB;
      lcd->clear();
      lcd->setCursor((20 - 12) / 2, 1); // Centered horizontally
      lcd->print("Closing Vise");
      lcd->setCursor((20 - 18) / 2, 2);
      lcd->print("Waiting for sensor");
      startTime = millis(); // Store the start time for LED
      currentPart = 0;
      isSequenceActive = true;
      lastSequenceTime = millis();
      digitalWrite(visePin, HIGH);  // Vise Engaged

      // Wait for Hall sensor to be triggered
      lcd->clear();
      lcd->setCursor((20 - 12) / 2, 1);
      lcd->print("Closing Vise");
      lcd->setCursor((20 - 18) / 2, 2);
      lcd->print("Waiting for sensor");

      // Turn on hopper for 3 seconds as vise starts to close
      digitalWrite(hopperPin, HIGH);
      delay(3000);
      digitalWrite(hopperPin, LOW);

      while (digitalRead(hallSensorPin) == HIGH) {
        // Optionally update the LCD or handle other tasks
      }

      lcd->clear();
      lcd->setCursor((20 - 16) / 2, 1);
      lcd->print("Sensor Triggered");
      delay(500);
      lcd->clear();
      lcd->setCursor((20 - 11) / 2, 1); // Centered horizontally
      lcd->print("Vise Locked");
      delay(3000);

      // Proceed to injection immediately // Brief delay for user feedback before proceeding
      digitalWrite(injectPin, LOW); // Injection off
      digitalWrite(hopperPin, LOW); // Hopper Off
      menuIndex = 0; // Set the cursor to "Working?"
      partsLeft = numOfParts; // Initialize partsLeft
    } else if (menuIndex == 5) {
      // Record the duration the encoder button is pressed for injectTime adjustment
      unsigned long pressStartTime = millis();
      digitalWrite(injectPin, HIGH); // Turn on Pin 12 while the button is held down
      while (digitalRead(ENCODER_BTN_PIN) == LOW) {
        // Wait for the button to be released
      }
      digitalWrite(injectPin, LOW); // Turn off Pin 12 once the button is released
      unsigned long pressDuration = (millis() - pressStartTime) / 1000; // Calculate duration in seconds
      injectTime = pressDuration;
      updateLCD();
    } else if (menuIndex == 6) {
      // Record the duration the encoder button is pressed for shotSize adjustment
      unsigned long pressStartTime = millis();
      digitalWrite(hopperPin, HIGH); // Turn on Pin 8 while the button is held down
      while (digitalRead(ENCODER_BTN_PIN) == LOW) {
        // Wait for the button to be released
      }
      digitalWrite(hopperPin, LOW); // Turn off Pin 8 once the button is released
      unsigned long pressDuration = (millis() - pressStartTime) / 1000; // Calculate duration in seconds
      shotSize = pressDuration;
      updateLCD();
    } else if (menuIndex == 8) {
      // Reset all values to specific reset state
      injectTime = 20;
      viseHoldTime = 30;
      shotSize = 20;
      numOfParts = 10;
      cyclePause = 60;

      // Update saved values in EEPROM to default values
      EEPROM.put(0, injectTime);
      EEPROM.put(4, viseHoldTime);
      EEPROM.put(8, shotSize);
      EEPROM.put(12, numOfParts);
      EEPROM.put(16, cyclePause);

      // Update LCD to reflect reset values
      updateLCD();

      updateLCD();
    } else if (menuIndex != 6) {
      // Button is pressed, enter value adjustment mode
      valueAdjustment();
    }
  }

  // Check if it's time to turn on PIN 12 10 seconds after PIN 13 turns on
  if (menuState == START_JOB && isSequenceActive && currentPart == 0 && millis() - startTime >= 5000) {
    digitalWrite(injectPin, HIGH); // Turn on Pin 12
    lcd->clear();
    lcd->setCursor((20 - 9) / 2, 1); // Centered horizontally
    lcd->print("Injecting");
    currentPart++;

    // Measure Hall sensor time during injection
    unsigned long injectionStartTime = millis();
    unsigned long hallSensorActiveTime = 0;
    bool hallSensorDetected = false;

    while (millis() - injectionStartTime < injectTime * 1000) {
      unsigned long timeLeft = (injectTime * 1000) - (millis() - injectionStartTime);
      lcd->setCursor((20 - 13) / 2, 2);
      lcd->print("Time Left: ");
      lcd->print(timeLeft / 1000);
      lcd->print("s  ");

      if (digitalRead(injectHallSensorPin) == LOW) { // Assuming LOW means detected
        if (!hallSensorDetected) {
          hallSensorDetected = true;
          hallSensorStartTime = millis();
        }
      } else {
        if (hallSensorDetected) {
          hallSensorActiveTime += millis() - hallSensorStartTime;
          hallSensorDetected = false;
        }
      }
    }

    // Ensure to add any remaining active time if sensor is still detected at the end
    if (hallSensorDetected) {
      hallSensorActiveTime += millis() - hallSensorStartTime;
    }

    // Check if no sensor detection has happened for the cycle
    if (hallSensorActiveTime > 0) {
      noDetectionCount = 0; // Reset counter if detection occurs
    } else {
      noDetectionCount++; // Increment no-detection counter only if no detection
    }

    // Adjust shot size based on Hall sensor detection time
    if (hallSensorActiveTime < 5000 && hallSensorActiveTime > 0) {
      shotSize += 10; // Increase shot size by 10 seconds
    } else if (hallSensorActiveTime == 0) {
      shotSize = max(shotSize - 10, 0); // Decrease shot size by 10 seconds, but not below zero
    }
  }

  // Check if it's time to turn off PIN 12 after injectTime
  if (menuState == START_JOB && isSequenceActive && currentPart == 1 && millis() - startTime >= (injectTime * 1000 + 10000)) {
    digitalWrite(injectPin, LOW);
    currentPart++;
  }

  // Check if no sensor detection has happened for 3 cycles
  if (noDetectionCount >= 3) {
    digitalWrite(injectPin, LOW);
    isSequenceActive = false; // Pause the cycle
    lcd->clear();
    lcd->setCursor((20 - 19) / 2, 1);
    lcd->print("Please check Hopper");
    lcd->setCursor((20 - 15) / 2, 2);
    lcd->print("OK to continue?");
    errorTone();

    // Wait for user input to continue
    while (digitalRead(ENCODER_BTN_PIN) == HIGH) {
      // Wait until the button is pressed
    }

    // Reset no-detection counter and continue the sequence
    noDetectionCount = 0;
    isSequenceActive = true;
  }

  // Check if it's time to turn on PIN 8 for shotSize seconds
  if (menuState == START_JOB && isSequenceActive && currentPart == 2) {
    digitalWrite(hopperPin, HIGH); // Turn on Pin 8

    // Calculate the time when the shot should end
    shotEndTime = millis() + (shotSize * 1000);

    lcd->clear();
    lcd->setCursor((20 - 14) / 2, 1); // Centered horizontally
    lcd->print("Refill Chamber");
    currentPart++;

    while (millis() < shotEndTime) {
      unsigned long timeLeft = shotEndTime - millis();
      lcd->setCursor((20 - 13) / 2, 2);
      lcd->print("Time Left: ");
      lcd->print(timeLeft / 1000);
      lcd->print("s  ");
    }

    digitalWrite(hopperPin, LOW); // Turn off Pin 8

    lcd->clear();
    lcd->setCursor((20 - 9) / 2, 1); // Centered horizontally
    lcd->print("Hold Vise");
    viseRemainingTime = millis() + (viseHoldTime * 1000);
    while (millis() < viseRemainingTime) {
      unsigned long remainingTime = viseRemainingTime - millis();

      lcd->setCursor((20 - 13) / 2, 2);
      lcd->print("Parts Left: ");
      lcd->print(partsLeft);
      lcd->setCursor((20 - 13) / 2, 3);
      lcd->print("Time Left: ");
      lcd->print(remainingTime / 1000);
      lcd->print("s  ");
    }
    currentPart++;
  }

  // Check if it's time to turn off PIN 8 and end the sequence
  if (menuState == START_JOB && isSequenceActive && currentPart == 3 && millis() - startTime >= ((shotSize + injectTime) * 1000 + 10000)) {
    digitalWrite(hopperPin, LOW);
    currentPart++;
  }

  // Check if it's time to end the sequence after viseHoldTime
  if (menuState == START_JOB && isSequenceActive && currentPart == 4 && millis() - startTime >= ((viseHoldTime + injectTime + shotSize) * 1000 + 10000)) {
    partsLeft--; // Decrement partsLeft
    if (partsLeft > 0) {
      // If there are more parts left, reset the sequence
      lcd->clear();
      lcd->setCursor((20 - 12) / 2, 1); // Centered horizontally
      lcd->print("Opening Vise");
      lcd->setCursor((20 - 13) / 2, 2); // Centered horizontally on the fourth row
      lcd->print("Parts Left: ");
      lcd->print(partsLeft + 1);
      currentPart = 0;
      digitalWrite(visePin, LOW);
      delay(5000);

      lcd->clear();
      lcd->setCursor((20 - 13) / 2, 1); // Centered horizontally
      lcd->print("Ejecting Part");
      lcd->setCursor((20 - 13) / 2, 2); // Centered horizontally on the fourth row
      lcd->print("Parts Left: ");
      lcd->print(partsLeft + 1);

      delay(100); // ensure myservo timing window

      digitalWrite(pneumaticInjection, HIGH); // Activate pneumatic ejection
      lcd->clear();
      lcd->setCursor((20 - 16) / 2, 1);
      lcd->print("Waiting for part");
      lcd->setCursor((20 - 7) / 2, 2);
      lcd->print("to drop");

      runSosMorseCode(pneumaticInjection); // Run SOS Morse code on pneumatic ejection pin while waiting for part drop

      partDropped = false;  // Reset partDropped globally before checking
      unsigned long waitStart = millis();

      // Check if the part is already dropped BEFORE running the servo
      while (millis() - waitStart < 10000) {
        if (partDroppedRead()) { // LOW = detected
          partDropped = true;
          break;
        }
      }

      // Run the servo ONLY if the part has NOT dropped
      if (!partDropped) {
        servoMotorRun(servoPin, partDropped); // Pass the global flag to be updated

        // Re-check after running the servo for another 3 seconds
        waitStart = millis();
        while (millis() - waitStart < 3000) {
          if (partDroppedRead()) {
            partDropped = true;
            break;
          }
        }
      }

      if (!partDropped) {
        // Only show "Part Not Detected" message if part never dropped
        lcd->clear();
        lcd->setCursor((20 - 17) / 2, 1);
        lcd->print("Part not detected");
        lcd->setCursor((20 - 7) / 2, 2);
        lcd->print("Resume?");
        errorTone();

        while (digitalRead(ENCODER_BTN_PIN) == HIGH) {
          // Wait for user confirmation
        }
      }

      // Ensure cycle pause only happens once
      if (partDropped || digitalRead(ENCODER_BTN_PIN) == LOW) {
        lcd->clear();
        lcd->setCursor((20 - 16) / 2, 0);
        lcd->print("Pausing for next");
        lcd->setCursor((20 - 4) / 2, 1);
        lcd->print("Part");

        cycleRemainingTime = millis() + (cyclePause * 1000);
        while (millis() < cycleRemainingTime) {
          unsigned long cycleRest = cycleRemainingTime - millis();

          lcd->setCursor((20 - 13) / 2, 2);
          lcd->print("Parts Left: ");
          lcd->print(partsLeft);
          lcd->setCursor((20 - 13) / 2, 3);
          lcd->print("Time Left: ");
          lcd->print(cycleRest / 1000);
          lcd->print("s  ");
        }
      }

      digitalWrite(pneumaticInjection, LOW); // Deactivate pneumatic ejection
      delay(1000);

      startTime = millis(); // Store the start time for the next part
      digitalWrite(visePin, HIGH); // Turn on PIN13 again

      // Wait for Hall sensor to be triggered
      lcd->clear();
      lcd->setCursor((20 - 12) / 2, 1);
      lcd->print("Closing Vise");
      lcd->setCursor((20 - 18) / 2, 2);
      lcd->print("Waiting for sensor");

      // Turn on hopper for 3 seconds as vise starts to close
      digitalWrite(hopperPin, HIGH);
      delay(3000);
      digitalWrite(hopperPin, LOW);

      while (digitalRead(hallSensorPin) == HIGH) {
        // Optionally update the LCD or handle other tasks
      }

      lcd->clear();
      lcd->setCursor((20 - 16) / 2, 1);
      lcd->print("Sensor Triggered");
      delay(500); // Brief delay for user feedback before proceeding
      lcd->clear();
      lcd->setCursor((20 - 11) / 2, 1); // Centered horizontally
      lcd->print("Vise Locked");
      delay(3000);
    } else {
      // Ensure last part is ejected before returning to the menu
      lcd->clear();
      lcd->setCursor((20 - 13) / 2, 1); // Centered horizontally
      lcd->print("Eject Last Part");
      digitalWrite(visePin, LOW);
      delay(5000);
      digitalWrite(pneumaticInjection, HIGH); // Activate pneumatic ejection
      delay(500);  // Brief delay to ensure activation

      // Run SOS Morse code while waiting for part drop
      runSosMorseCode(pneumaticInjection);

      digitalWrite(pneumaticInjection, LOW); // Deactivate ejection
      delay(500); // Brief delay before checking part drop

      partDropped = false;
      unsigned long waitStart = millis();

      // Wait up to 10 seconds for part to drop
      while (millis() - waitStart < 10000) {
        if (partDroppedRead()) { // LOW = detected
          partDropped = true;
          break;
        }
      }

      // Run servo motor only if the part did not drop
      if (!partDropped) {
        servoMotorRun(servoPin, partDropped);
      }

      // Final check after servo action
      waitStart = millis();
      while (millis() - waitStart < 3000) {
        if (partDroppedRead()) {
          partDropped = true;
          break;
        }
      }

      if (!partDropped) {
        lcd->clear();
        lcd->setCursor((20 - 17) / 2, 1);
        lcd->print("Part not detected");
        lcd->setCursor((20 - 7) / 2, 2);
        lcd->print("Resume?");

        while (digitalRead(ENCODER_BTN_PIN) == HIGH) {
          // Wait for user confirmation
        }
      }

      // Now return to main menu after last part is ejected
      jobDoneChime();

      isSequenceActive = false;
      currentPart = 0;
      digitalWrite(visePin, LOW); // Open the vise
      menuState = MAIN_MENU;
      lcd->clear();
      updateLCD();
    }
  }

  // Update the display to show parts left
  if (menuState == START_JOB) {
    lcd->setCursor((20 - 13) / 2, 2); // Centered horizontally on the fourth row
    lcd->print("Parts Left: ");
    lcd->print(partsLeft);
  }
}

void valueAdjustment() {
  int encoderValue = 0;
  int lastEncoderValue = digitalRead(clkPin);

  while (true) {
    int newEncoderValue = digitalRead(clkPin);
    if (newEncoderValue != lastEncoderValue) {
      lastEncoderValue = newEncoderValue;
      int dtValue = digitalRead(dtPin);

      if (newEncoderValue == LOW && dtValue == HIGH) {
        adjustValue(1);
      }
      if (newEncoderValue == LOW && dtValue == LOW) {
        adjustValue(-1);
      }
      updateLCD();
    }

    int buttonState = digitalRead(ENCODER_BTN_PIN);
    if (buttonState == LOW) {
      break;
    }
  }
}

void adjustValue(int direction) {
  int multiplier = 1.8; // Increase by a factor to make it more sensitive
  switch (menuIndex) {
    case 4: // Cycle Pause
      if (direction > 0 && cyclePause < 65535) {
        cyclePause += direction;
      } else if (direction < 0 && cyclePause > 0) {
        cyclePause += direction;
      }
      break;
    case 0: // Inject Time
      if (direction > 0 && injectTime < 65535) {
        injectTime += direction;
      } else if (direction < 0 && injectTime > 0) {
        injectTime += direction;
      }
      break;
    case 1: // Vise Hold
      if (direction > 0 && viseHoldTime < 65535) {
        viseHoldTime += direction;
      } else if (direction < 0 && viseHoldTime > 0) {
        viseHoldTime += direction;
      }
      break;
    case 2: // Shot Size
      if (direction > 0 && shotSize < 65535) {
        shotSize += 1 * direction; // Adjust by 1 seconds
      } else if (direction < 0 && shotSize > 0) {
        shotSize += 1 * direction; // Adjust by -1 seconds
      }
      break;
    case 3: // # of Parts
      if (direction > 0 && numOfParts < 65535) {
        numOfParts += direction * multiplier;
      } else if (direction < 0 && numOfParts > 0) {
        numOfParts += direction * multiplier;
      }

      // Increase the number of parts by 10 every 2 seconds if the button is held down
      unsigned long pressStartTime = millis();
      while (digitalRead(ENCODER_BTN_PIN) == LOW) {
        if (millis() - pressStartTime >= 2000) {
          numOfParts = min(numOfParts + 10, 65535);
          pressStartTime = millis(); // Reset the timer
          updateLCD();
        }
      }
      break;
  }
  updateLCD();
}
