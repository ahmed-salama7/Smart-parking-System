// =============================================================================
//  Smart Parking Management System  —  HARDWARE BUILD (real sensors, no buttons)
//
//  MCU      : Arduino Uno
//  Sensors  : 2x HC-SR04 ultrasonic  (entrance + exit)
//  Actuator : SG90 servo             (entry/exit barrier)
//  Display  : 16x2 I2C LCD
//  Feedback : Green / Red / Yellow LEDs + passive buzzer
//
//  -- POWER INTEGRITY NOTE ----------------------------------------------------
//  The SG90's inrush current when the gate moves can drag the 5 V rail down far
//  enough to brown out the logic and reset the board. Mitigations on this build:
//    * 470 uF bulk capacitor across the servo's 5 V/GND, placed close to it,
//      to absorb the transient.
//    * Servo powered from a dedicated 5 V supply (NOT the Arduino 5 V pin),
//      with all grounds tied together.
//  ---------------------------------------------------------------------------
// =============================================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

// -----------------------------------------------------------------------------
//  Pin map   (entrance/exit button pins removed — sensors now drive detection)
// -----------------------------------------------------------------------------
const uint8_t ENTRANCE_TRIG = 2;
const uint8_t ENTRANCE_ECHO = 3;

const uint8_t EXIT_TRIG     = 10;
const uint8_t EXIT_ECHO     = 11;

const uint8_t GREEN_LED     = 5;
const uint8_t RED_LED       = 6;
const uint8_t YELLOW_LED    = 7;
const uint8_t BUZZER_PIN    = 8;
const uint8_t SERVO_PIN     = 9;

// -----------------------------------------------------------------------------
//  Tunable parameters
// -----------------------------------------------------------------------------
const uint8_t  TOTAL_SPACES       = 4;
const uint16_t DETECT_CM          = 20;    // Vehicle present if distance < this
const uint8_t  CONFIRM_READS      = 3;     // Consecutive hits needed to accept
const uint8_t  SENSOR_SAMPLES     = 5;     // Samples per measurement (median of)
const uint16_t SAMPLE_DELAY_MS    = 10;    // Gap between samples
const uint16_t GATE_OPEN_HOLD_MS  = 1000;  // Hold after the zone clears
const uint16_t GATE_MAX_OPEN_MS   = 8000;  // Safety: force-close after this
const uint16_t FULL_WARN_MS       = 1500;  // "FULL" message display time
const uint16_t DEBOUNCE_MS        = 1000;  // Min ms between accepted triggers
const uint16_t ECHO_TIMEOUT_US    = 20000; // pulseIn timeout (~340 cm max)
const uint16_t NO_ECHO            = 9999;  // Sentinel: nothing in range

const uint8_t  GATE_CLOSED_DEG    = 0;
const uint8_t  GATE_OPEN_DEG      = 90;

const uint16_t TONE_OK_HZ         = 1500;
const uint16_t TONE_OK_MS         = 150;
const uint16_t TONE_WARN_HZ       = 450;
const uint16_t TONE_WARN_MS       = 200;
const uint16_t TONE_WARN_PAUSE_MS = 250;

// -----------------------------------------------------------------------------
//  System-state enum — makes the finite state machine explicit
// -----------------------------------------------------------------------------
enum SystemState : uint8_t {
  STATE_IDLE,
  STATE_VEHICLE_ENTERING,
  STATE_VEHICLE_EXITING,
  STATE_LOT_FULL_WARN
};

// -----------------------------------------------------------------------------
//  Globals
// -----------------------------------------------------------------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);   // If the LCD stays blank, try 0x3F
Servo             gateServo;
SystemState       currentState   = STATE_IDLE;
uint8_t           availableSlots = TOTAL_SPACES;

uint32_t          lastEntranceMs = 0;   // Debounce timestamps
uint32_t          lastExitMs     = 0;

uint8_t           entranceHits   = 0;   // Consecutive-detection counters
uint8_t           exitHits       = 0;

// -----------------------------------------------------------------------------
//  setup()
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(9600);

  pinMode(ENTRANCE_TRIG, OUTPUT);
  pinMode(ENTRANCE_ECHO, INPUT);

  pinMode(EXIT_TRIG, OUTPUT);
  pinMode(EXIT_ECHO, INPUT);

  pinMode(GREEN_LED,  OUTPUT);
  pinMode(RED_LED,    OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  gateServo.attach(SERVO_PIN);
  closeGate();

  lcd.init();
  lcd.backlight();

  Serial.println(F("=== Smart Parking System READY ==="));
  updateDisplay();
}

// -----------------------------------------------------------------------------
//  loop()  — sample sensors, confirm detections, dispatch events
// -----------------------------------------------------------------------------
void loop() {
  uint32_t now = millis();

  uint16_t entranceDist = getMedianDistance(ENTRANCE_TRIG, ENTRANCE_ECHO);
  uint16_t exitDist     = getMedianDistance(EXIT_TRIG,     EXIT_ECHO);

  // Count consecutive "present" reads; any miss resets the counter.
  // This rejects the single-frame noise spikes HC-SR04 sensors produce.
  entranceHits = isVehiclePresent(entranceDist) ? entranceHits + 1 : 0;
  exitHits     = isVehiclePresent(exitDist)     ? exitHits + 1     : 0;

  // --- ENTRANCE ---
  if (entranceHits >= CONFIRM_READS && (now - lastEntranceMs) > DEBOUNCE_MS) {
    lastEntranceMs = now;
    entranceHits   = 0;

    if (availableSlots > 0) handleVehicleEntry();
    else                    handleLotFull();
  }

  // --- EXIT  (else-if prevents simultaneous entry+exit processing) ---
  else if (exitHits >= CONFIRM_READS && (now - lastExitMs) > DEBOUNCE_MS) {
    lastExitMs = now;
    exitHits   = 0;

    if (availableSlots < TOTAL_SPACES) handleVehicleExit();
  }
}

// -----------------------------------------------------------------------------
//  Event handlers
// -----------------------------------------------------------------------------
void handleVehicleEntry() {
  currentState = STATE_VEHICLE_ENTERING;
  Serial.print(F("ENTRY  — slots before: ")); Serial.println(availableSlots);

  playSuccessTone();
  setLEDs(false, false, true);        // Yellow = gate moving

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Slots left: ")); lcd.print(availableSlots);
  lcd.setCursor(0, 1); lcd.print(F("Car entering... "));

  openGate();
  waitForClear(ENTRANCE_TRIG, ENTRANCE_ECHO);   // Hold until the car passes
  delay(GATE_OPEN_HOLD_MS);
  closeGate();

  availableSlots = constrain(availableSlots - 1, 0, TOTAL_SPACES);
  Serial.print(F("ENTRY done — slots now: ")); Serial.println(availableSlots);

  currentState = STATE_IDLE;
  updateDisplay();
}

void handleVehicleExit() {
  currentState = STATE_VEHICLE_EXITING;
  Serial.print(F("EXIT   — slots before: ")); Serial.println(availableSlots);

  playSuccessTone();
  setLEDs(false, false, true);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Vehicle leaving "));
  lcd.setCursor(0, 1); lcd.print(F("Opening gate... "));

  openGate();
  waitForClear(EXIT_TRIG, EXIT_ECHO);
  delay(GATE_OPEN_HOLD_MS);
  closeGate();

  availableSlots = constrain(availableSlots + 1, 0, TOTAL_SPACES);
  Serial.print(F("EXIT done — slots now: ")); Serial.println(availableSlots);

  currentState = STATE_IDLE;
  updateDisplay();
}

void handleLotFull() {
  currentState = STATE_LOT_FULL_WARN;
  Serial.println(F("LOT FULL — entry denied"));

  playWarningTone();
  setLEDs(false, true, false);        // Red only

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("  PARKING FULL  "));
  lcd.setCursor(0, 1); lcd.print(F(" ACCESS DENIED! "));
  delay(FULL_WARN_MS);

  currentState = STATE_IDLE;
  updateDisplay();
}

// -----------------------------------------------------------------------------
//  Sensor helpers
// -----------------------------------------------------------------------------

// Median of SENSOR_SAMPLES readings (cm). Median rejects the occasional wild
// spike far better than a mean does — important for noisy HC-SR04 echoes.
uint16_t getMedianDistance(uint8_t trigPin, uint8_t echoPin) {
  uint16_t s[SENSOR_SAMPLES];

  for (uint8_t i = 0; i < SENSOR_SAMPLES; i++) {
    s[i] = getRawDistance(trigPin, echoPin);
    delay(SAMPLE_DELAY_MS);
  }

  // Insertion sort (tiny N, cheap on an Uno)
  for (uint8_t i = 1; i < SENSOR_SAMPLES; i++) {
    uint16_t key = s[i];
    int8_t   j   = i - 1;
    while (j >= 0 && s[j] > key) {
      s[j + 1] = s[j];
      j--;
    }
    s[j + 1] = key;
  }

  return s[SENSOR_SAMPLES / 2];        // Middle element = median
}

// Single HC-SR04 measurement with timeout guard.
// Returns distance in cm, or NO_ECHO if nothing is in range.
// NOTE: distance is uint16_t. The old uint8_t version overflowed for anything
// past ~2.5 m, so a far wall could wrap around and read as a *near* object —
// a false "vehicle present". uint16_t fixes that.
uint16_t getRawDistance(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  uint32_t dur = pulseIn(echoPin, HIGH, ECHO_TIMEOUT_US);
  if (dur == 0) return NO_ECHO;        // No echo = nothing within range
  return (uint16_t)(dur * 0.0343f / 2.0f);
}

// Vehicle present if the measured distance is under the threshold.
bool isVehiclePresent(uint16_t distanceCm) {
  return (distanceCm < DETECT_CM);
}

// Block until the sensor zone is clear, with a safety timeout so a parked car
// or a stuck sensor can't freeze the gate open and lock up the whole system.
void waitForClear(uint8_t trigPin, uint8_t echoPin) {
  uint32_t start = millis();
  while (isVehiclePresent(getMedianDistance(trigPin, echoPin))) {
    if (millis() - start > GATE_MAX_OPEN_MS) {
      Serial.println(F("WARN: gate-open timeout — forcing close"));
      return;
    }
    delay(100);
  }
}

// -----------------------------------------------------------------------------
//  Gate control
// -----------------------------------------------------------------------------
void openGate()  { gateServo.write(GATE_OPEN_DEG);   }
void closeGate() { gateServo.write(GATE_CLOSED_DEG); }

// -----------------------------------------------------------------------------
//  LED control — single function keeps the LED state consistent
// -----------------------------------------------------------------------------
void setLEDs(bool green, bool red, bool yellow) {
  digitalWrite(GREEN_LED,  green  ? HIGH : LOW);
  digitalWrite(RED_LED,    red    ? HIGH : LOW);
  digitalWrite(YELLOW_LED, yellow ? HIGH : LOW);
}

// -----------------------------------------------------------------------------
//  Display
// -----------------------------------------------------------------------------
void updateDisplay() {
  bool isFull = (availableSlots == 0);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Slots left: ")); lcd.print(availableSlots);

  lcd.setCursor(0, 1);
  lcd.print(isFull ? F("Status: FULL") : F("Status: OPEN"));

  setLEDs(!isFull, isFull, false);    // Green if open, Red if full, Yellow off

  Serial.print(F("Display updated — "));
  Serial.print(availableSlots);
  Serial.println(isFull ? F(" slots — FULL") : F(" slots — OPEN"));
}

// -----------------------------------------------------------------------------
//  Acoustic feedback
// -----------------------------------------------------------------------------
void playSuccessTone() {
  tone(BUZZER_PIN, TONE_OK_HZ, TONE_OK_MS);
}

void playWarningTone() {
  tone(BUZZER_PIN, TONE_WARN_HZ, TONE_WARN_MS);
  delay(TONE_WARN_PAUSE_MS);
  tone(BUZZER_PIN, TONE_WARN_HZ, TONE_WARN_MS);
}
