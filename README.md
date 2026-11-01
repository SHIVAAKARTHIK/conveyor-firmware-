# MediDispense conveyor station firmware

ESP32 firmware for a small conveyor that carries a medicine box to one of two
beds. It gets its jobs from the MediDispense backend over WiFi, runs the belt,
stops it at the right bed's sensor, pushes the box into that bed's chute with a
servo, and reports the result. No cable to a laptop is needed during operation.

Related repos: [conveyor-backend](https://github.com/SHIVAAKARTHIK/conveyor-backend)
(FastAPI) and [conveyor-admin-web](https://github.com/SHIVAAKARTHIK/conveyor-admin-web)
(Next.js web app).

## What it does

1. **Idle:** every 4 s it asks the backend for the next job (`GET /conveyor/next-job`).
2. **Loading:** when a job exists it beeps and waits for a box to be loaded.
3. **Running:** one press of the push button (or **Dispatch** on the web page)
   calls `POST /deliveries/{id}/dispatch` and starts the belt. A press while
   idle fetches the next waiting job and starts it straight away.
4. The sensor of **that job's own bed** stops the belt (the other bed's sensor
   is ignored), that bed's servo pushes the box, the buzzer sounds, and the
   board calls `POST /deliveries/{id}/mark-arrived`.
5. Safety: a press during a run is an emergency stop (jam alert, delivery
   failed); a run times out after 40 s; if WiFi drops the belt stops and the
   board restarts.

The backend is found by computer name over mDNS (`BACKEND_HOST`), falling back
to `BACKEND_URL`. The board itself is reachable as `http://conveyor.local`.

## Wiring (ESP32 DevKit V1, 30-pin)

| Device | Pin |
|---|---|
| L298N `ENA` (speed, PWM 1 kHz) / `IN1` / `IN2` | D25 / D26 / D27 |
| Servo 1 (Bed 1) / Servo 2 (Bed 2) signal | D18 / D19 |
| IR sensor A (Bed 1) / IR sensor B (Bed 2) | D4 / D32 (LOW = box seen) |
| Buzzer (active, HIGH = on) | D23 |
| Push button (`INPUT_PULLUP`, other leg to GND) | D15 |

Shared rails: **3V3** for both sensors; **VIN** (5 V) for both servos and the
buzzer; one GND pin for sensors, button and buzzer; the other GND pin for the
servos and the L298N. The 12 V adapter powers only the L298N (remove its `ENA`
jumper). Don't hold the button while powering on (D15 is read at boot).

## Build and flash

Needs the ESP32 Arduino core (3.x) and the libraries **ArduinoJson** and
**ESP32Servo**.

```bash
cp config.h.example config.h      # copy only; you do not have to edit it
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload  -p COM3 --fqbn esp32:esp32:esp32 .
```

## Setup page (WiFi, backend, login)

You do not edit `config.h` to set the WiFi any more. The board keeps its
settings in its own flash and has a setup page for them.

1. On a new board the **onboard LED blinks fast** and the board opens its own
   WiFi network **`MediDispense-Setup`** (password `medidispense`).
2. Join it from a phone or laptop and open **http://192.168.4.1**.
3. Choose your WiFi (2.4 GHz only) and enter its password, the **backend
   computer's name** (`hostname` on that computer) and optionally its address,
   and the **conveyor's login** (a technician account in the backend).
4. Press **Save and restart**. The board joins your WiFi, finds the backend and
   starts polling for jobs. Reconnect your phone to your normal WiFi.

The setup page opens again by itself if the board cannot join its WiFi within
30 seconds (the WiFi changed or is gone), or on purpose when you **hold the push
button for 3 seconds while powering on**. It is only on the board's own
network, so nobody on your normal WiFi can change the settings. If nothing is
saved within 10 minutes and a working setup exists, the board restarts with it.

`config.h` still holds the **defaults** the board falls back to when nothing is
saved. `config.h` is ignored by Git. Serial console: `u` opens the setup page,
`c` clears the saved settings.

## Local HTTP control

| Route | Method | Action |
|---|---|---|
| `/status` | GET | Belt, speed, both sensors, servo angles, WiFi, mode, job, button counter, whether settings are saved |
| `/start`, `/stop` | POST | Run or stop the belt (`/stop` during a delivery is the emergency stop) |
| `/dispatch` | POST | Same as pressing the button |
| `/push?bed=1` or `2` | POST | Run that bed's servo push |
| `/beep` | POST | Sound the buzzer |

These routes use plain HTTP with no password and are meant for a private
network only. The web app's hardware panel uses them.

## Serial console (115200 baud)

`f` / `b` / `s` belt forward, backward, stop; `+` / `-` speed; `1` / `2` select
servo; `.` / `,` angle up or down; `z` / `x` save the current angle as rest or
push; `p` run a push; `k` beep; `i` show both sensors; `t` bench test (a sensor
pushes its own servo); `h` toggle the delivery loop; `u` setup page; `c` clear
saved settings; `?` help.

## Milestones

| | Content | Status |
|---|---|---|
| H1 | WiFi, heartbeat LED | working |
| H2 | Belt via L298N with PWM speed | working |
| H3 | Debounced IR sensors | working, both |
| H4 | Servo rest and push angles | working, both (angles are defaults, still to calibrate) |
| H5 | Push button and buzzer | working |
| H6 | Full delivery loop with the backend | working for Bed 1 and Bed 2 |

Defaults: belt PWM 1 kHz at speed 90/255, sensor sampling every 10 ms with 5
agreeing samples, servos 0° rest and 90° push.
