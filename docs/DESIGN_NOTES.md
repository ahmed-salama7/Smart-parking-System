# Design Notes — Smart Parking System

Engineering design documentation for the Smart Parking System: an Arduino-based
parking-management prototype that detects vehicles with IR sensors, controls a
servo gate via a finite-state-machine firmware, and reports live occupancy on an
LCD with traffic-light and buzzer feedback.

---

## 1. Design Goals

The project set out to satisfy four requirements:

1. Detect vehicle presence at the entrance and exit in real time.
2. Automatically open or deny gate access depending on available capacity.
3. Display live parking status to drivers (free slots, OPEN / FULL).
4. Achieve all of the above on a low-cost (~$22) Arduino platform with reliable,
   repeatable operation.

The guiding principle throughout was **reliability over features** — a simpler
system that never miscounts is more valuable than a complex one that occasionally
fails.

---

## 2. System Architecture

The system is organised as a classic three-stage embedded pipeline:

```
INPUT                  PROCESSING                 OUTPUT
─────                  ──────────                 ──────
Entrance IR  ─┐                              ┌─►  Servo gate (SG90)
              ├─►   Arduino Uno (ATmega328P)  ├─►  16×2 LCD (I²C)
Exit IR     ─┘      • Debounced events        ├─►  Traffic LEDs (R/Y/G)
                    • Finite state machine     └─►  Buzzer
                    • Occupancy + capacity
```

The Arduino Uno acts as the edge controller. All modules share a single
regulated 5 V rail and a common ground.

---

## 3. Component Selection & Rationale

| Function | Part chosen | Why this part |
|---|---|---|
| Controller | Arduino Uno R3 (ATmega328P) | Deterministic, instant-on, no OS, ample I/O, lowest cost for the job |
| Vehicle detection | FC-51 IR obstacle sensor ×2 | Clean active-LOW digital output (onboard LM393 comparator), no distance math, cheap, fixed beam path suits a gate |
| Gate actuator | TowerPro SG90 servo | Sufficient torque for a light barrier, simple PWM control, very low cost |
| Display | 16×2 LCD + PCF8574 I²C backpack | I²C cuts pin usage from 16 lines to 2 (SDA/SCL), freeing GPIO |
| Status indicator | Traffic-light LED module | Common-cathode with onboard SMD resistors — drive directly from GPIO, no external parts |
| Audio feedback | Active piezo buzzer module | Simple `tone()` control for entry chime and full-lot alarm |

**Key decision — IR over ultrasonic:** the FC-51 modules output a clean digital
LOW/HIGH, so the microcontroller does no distance calculation, needs no averaging
filter, and has no echo-timeout risk. For a gate, where the detection point is
fixed and known, this is more robust and cheaper than ultrasonic ranging.

---

## 4. Firmware Design

The firmware is written in embedded C++ and structured as an **explicit finite
state machine** rather than ad-hoc conditional logic.

**States:** `IDLE`, `VEHICLE_ENTERING`, `VEHICLE_EXITING`, `LOT_FULL_WARN`.

**Control flow:** the controller idles while monitoring both sensors. On an
entrance trigger it checks capacity — if a slot is free it opens the gate and
increments the count; if the lot is full it denies access with the buzzer and a
"PARKING FULL" message. The exit path is symmetrical and decrements the count.
All paths return to `IDLE` after updating the display and LEDs.

**Three robustness measures were designed in deliberately:**

- **Debounce timer (300 ms).** Recorded with `millis()`, this rejects repeat
  triggers from a single slow-moving vehicle so one car is counted exactly once.
- **`constrain()` guard.** The occupancy count is clamped between 0 and total
  capacity, making it mathematically impossible to under- or over-count
  regardless of sensor behaviour.
- **Mutually-exclusive dispatch (`else-if`).** Entrance and exit events can never
  be processed in the same loop cycle, preventing race conditions.

**Code-quality practices:** all magic numbers are named constants, all LED state
is set through a single `setLEDs()` function (one source of truth), string
literals are stored in flash with the `F()` macro to save SRAM, and serial debug
output is provided throughout for diagnostics.

---

## 5. Engineering Challenge — Power Integrity

This was the most significant problem encountered, and the most instructive.

**Symptom.** When the servo moved to operate the gate, the LCD would glitch
mid-message and the Arduino would occasionally reset.

**Diagnosis.** The SG90 draws a sharp inrush current the instant it starts
moving. On the breadboard — with thin jumper wires and no local energy storage —
that current spike pulled the shared 5 V rail down far enough to brown out the
logic and reset the microcontroller. The fault correlated exactly with gate
movement, which identified the servo as the source.

**Solution.** A **470 µF electrolytic bulk capacitor** placed directly across the
5 V rail next to the servo. The capacitor acts as a local energy reservoir,
supplying the inrush current so the rest of the circuit sees a stable supply.
This was implemented properly on the PCB, alongside a ground pour on both copper
layers and a widened trace to the servo.

**Result.** Power-related glitches dropped from ~6 per 50 gate cycles to 0.

This is the textbook reason decoupling and bulk capacitors exist; encountering it
firsthand turned a theoretical rule into practical understanding.

---

## 6. PCB Design

The breadboard prototype was translated into a custom PCB using KiCad.

- **Form factor:** a two-layer Arduino carrier shield. Because every part is a
  pre-built module, the board is a carrier that breaks each Arduino pin out to a
  labelled connector rather than a component-level board.
- **Connectors:** pluggable headers / screw terminals for every module, so parts
  remain reusable and the board is repairable. None are soldered directly.
- **Power integrity:** ground pour on both layers, a widened 5 V trace to the
  servo, and the 470 µF bulk capacitor positioned at the servo connector.
- **Workflow:** schematic capture → footprint assignment → two-layer routing →
  DRC → 3D render for a final placement check before fabrication.

---

## 7. Validation & Results

Testing was treated as a structured activity with defined trials and recorded
numerical results.

| Metric | Result |
|---|---|
| Entrance sensor detection accuracy | 100% (20/20 trials) |
| Exit sensor detection accuracy | 95% (19/20 trials) |
| Rail glitches before bulk capacitor | 6 per 50 gate cycles |
| Rail glitches after bulk capacitor | 0 per 50 gate cycles |
| Full entry-cycle time | ~2.5 s (dominated by vehicle pass-through) |
| Total BOM cost (required parts) | ~$21.70 |

The exit sensor's 95% reflects occasional beam interference from the mounting
angle, correctable with a small physical adjustment.

---

## 8. Known Limitations & Future Work

**Current limitations:**

- Single-gate, single-zone prototype.
- Blocking `delay()` / wait-for-clear logic means the system focuses on one event
  at a time (acceptable for one gate, but not ideal at scale).
- The gate holds open indefinitely if a vehicle stalls in the beam — a safe
  failure mode, but not a timed one.

**Future work:**

- **IoT upgrade:** add an ESP8266 to stream live occupancy over Wi-Fi to an MQTT
  broker, with a mobile app showing drivers availability before arrival. This is
  a pure addition — no change to the existing firmware or hardware.
- **RFID access control** for authenticated entry.
- **Multi-level scaling:** replicate the state machine per gate, reporting to a
  central controller.
- **Fully non-blocking firmware:** replace remaining `delay()` calls with a
  `millis()`-based scheduler so both sensors stay responsive during gate motion.

---

## 9. Notes on the Simulation

The physical build used FC-51 IR sensors. The Wokwi simulation used **HC-SR04
ultrasonic sensors instead**, because the FC-51 module is not available in Wokwi.
This introduced fluctuating distance readings in simulation, handled with a
five-sample averaging filter and a validity check discarding readings above
200 cm. This filtering is a **simulation-only** concern — the real FC-51 modules
output a clean digital signal that needs no filtering. The simulation was used to
validate the control logic before hardware testing and as a reproducible,
shareable demonstration of the system's behaviour.
