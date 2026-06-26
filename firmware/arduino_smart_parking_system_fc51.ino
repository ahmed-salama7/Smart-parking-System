// =============================================================================
//  Smart Parking Management System  —  REAL BOARD (FC-51 IR sensors)
//
//  MCU      : Arduino Uno
//  Sensors  : 2x FC-51 IR obstacle sensors  (entrance + exit, active-LOW)
//  Actuator : SG90 servo                     (entry/exit barrier)
//  Display  : 16x2 I2C LCD
//  Feedback : Green / Red / Yellow LEDs       (traffic-light style)
//
//  This is the firmware for the PHYSICAL prototype. FC-51 modules output a
//  clean digital obstacle signal (LOW when a vehicle breaks the IR beam), so no
//  distance measurement is needed. The Wokwi simulation of this project uses
//  HC-SR04 ultrasonic sensors instead (the FC-51 module isn't available in
//  Wokwi) and lives in firmware/simulation/.
// =============================================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

// -----------------------------------------------------------------------------
//  Pin map
// -----------------------------------------------------------------------------
const uint8_t ENTRANCE_IR = 2;    // FC-51 OUT — LOW when a vehicle is detected
const uint8_t EXIT_IR     = 3;    // FC-51 OUT — LOW when a vehicle is detected

const uint8_t GREEN_LED   = 5;
const uint8_t RED_LED     = 6;
const uint8_t YELLOW_LED  = 7;
const uint8_t SERVO_PIN   = 9;

// -----------------------------------------------------------------------------
//  Tunable parameters
// -----------------------------------------------------------------------------
const uint8_t  TOTAL_SPACES       = 4;
const uint8_t  CONFIRM_READS      = 4;     // Consecutive "detected" reads needed
const uint8_t  DEBOUNCE_SAMPLE_MS = 8;     // Gap between confirmation samples
const uint16_t GATE_OPEN_HOLD_MS  = 1000;  // Hold after the beam clears
const uint16_t GATE_MAX_OPEN_MS   = 8000;  // Safety: force-close after this
const uint16_t FULL_WARN_MS       = 1500;  // "FULL" message display time
const uint16_t DEBOUNCE_MS        = 1000;  // Min ms between accepted triggers

const uint8_t  GATE_CLOSED_DEG    = 0;
const uint8_t  GATE_OPEN_DEG      = 90;

// FC-51 output polarity: LOW = obstacle present
const uint8_t  VEHICLE_DETECTED   = LOW;

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

// -----------------------------------------------------------------------------
//  setup()
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(9600);

  pinMode(ENTRANCE_IR, INPUT);
  pinMode(EXIT_IR,     INPUT);

  pinMode(GREEN_LED,  OUTPUT);
  pinMode(RED_LED,    OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);

  gateServo.attach(SERVO_PIN);
  closeGate();

  lcd.init();
  lcd.backlight();

  Serial.println(F("=== Smart Parking System READY (FC-51) ==="));
  updateDisplay();
}

// -----------------------------------------------------------------------------
//  loop()  — poll both sensors, dispatch entry/exit events
// -----------------------------------------------------------------------------
void loop() {
  uint32_t now = millis();

  // --- ENTRANCE ---
  if (vehiclePresent(ENTRANCE_IR) && (now - lastEntranceMs) > DEBOUNCE_MS) {
    lastEntranceMs = now;
    if (availableSlots > 0) handleVehicleEntry();
    else                    handleLotFull();
  }
  // --- EXIT  (else-if prevents simultaneous entry+exit processing) ---
  else if (vehiclePresent(EXIT_IR) && (now - lastExitMs) > DEBOUNCE_MS) {
    lastExitMs = now;
    if (availableSlots < TOTAL_SPACES) handleVehicleExit();
  }
}

// -----------------------------------------------------------------------------
//  Event handlers
// -----------------------------------------------------------------------------
void handleVehicleEntry() {
  currentState = STATE_VEHICLE_ENTERING;
  Serial.print(F("ENTRY  - slots before: ")); Serial.println(availableSlots);

  setLEDs(false, false, true);        // Yellow = gate moving

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Slots left: ")); lcd.print(availableSlots);
  lcd.setCursor(0, 1); lcd.print(F("Car entering... "));

  openGate();
  waitForClear(ENTRANCE_IR);          // Hold until the car passes the beam
  delay(GATE_OPEN_HOLD_MS);
  closeGate();

  availableSlots = constrain(availableSlots - 1, 0, TOTAL_SPACES);
  Serial.print(F("ENTRY done - slots now: ")); Serial.println(availableSlots);

  currentState = STATE_IDLE;
  updateDisplay();
}

void handleVehicleExit() {
  currentState = STATE_VEHICLE_EXITING;
  Serial.print(F("EXIT   - slots before: ")); Serial.println(availableSlots);

  setLEDs(false, false, true);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Vehicle leaving "));
  lcd.setCursor(0, 1); lcd.print(F("Opening gate... "));

  openGate();
  waitForClear(EXIT_IR);
  delay(GATE_OPEN_HOLD_MS);
  closeGate();

  availableSlots = constrain(availableSlots + 1, 0, TOTAL_SPACES);
  Serial.print(F("EXIT done - slots now: ")); Serial.println(availableSlots);

  currentState = STATE_IDLE;
  updateDisplay();
}

void handleLotFull() {
  currentState = STATE_LOT_FULL_WARN;
  Serial.println(F("LOT FULL - entry denied"));

  setLEDs(false, true, false);        // Red only

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("  PARKING FULL  "));
  lcd.setCursor(0, 1); lcd.print(F(" NO SPACE LEFT! "));
  delay(FULL_WARN_MS);

  currentState = STATE_IDLE;
  updateDisplay();
}

// -----------------------------------------------------------------------------
//  Sensor helper — software-debounced read of an FC-51 module
// -----------------------------------------------------------------------------
// Returns true only if the sensor reads "detected" for CONFIRM_READS samples in
// a row. A raw IR module can blip on ambient light or a passing hand; requiring
// several consecutive reads rejects those single-frame false triggers.
bool vehiclePresent(uint8_t irPin) {
  for (uint8_t i = 0; i < CONFIRM_READS; i++) {
    if (digitalRead(irPin) != VEHICLE_DETECTED) return false;
    delay(DEBOUNCE_SAMPLE_MS);
  }
  return true;
}

// Block until the beam is clear again, with a safety timeout so a stalled car
// or a stuck sensor can't hold the gate open and freeze the whole system.
void waitForClear(uint8_t irPin) {
  uint32_t start = millis();
  while (digitalRead(irPin) == VEHICLE_DETECTED) {
    if (millis() - start > GATE_MAX_OPEN_MS) {
      Serial.println(F("WARN: gate-open timeout - forcing close"));
      return;
    }
    delay(50);
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

  setLEDs(!isFull, isFull, false);    // Green if open, Red if full

  Serial.print(F("Display updated - "));
  Serial.print(availableSlots);
  Serial.println(isFull ? F(" slots - FULL") : F(" slots - OPEN"));
}
