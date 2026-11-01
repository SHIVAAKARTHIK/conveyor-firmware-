// ============================================================================
// MediDispense conveyor dispensing station - firmware
//
// Built up one tested milestone at a time - see the setup guide in the conveyor-docs repo for the
// full H1-H6 build order. Each milestone adds to this file rather than
// replacing it, so by H6 this sketch is the complete station firmware.
//
// H1 (done): join WiFi, print IP, blink the onboard LED as a heartbeat.
// H2 (this milestone): drive the conveyor belt forward/back/stop via the
//   L298N, controlled interactively over Serial - no sensors involved yet,
//   so you can confirm wiring and belt direction before anything else.
// ============================================================================

#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "setup_portal.h"  // settings kept on the board + the WiFi/backend setup page

unsigned long lastBlinkAt = 0;
bool ledOn = false;

// --- H2: conveyor belt control -----------------------------------------------

bool beltRunning = false;  // mirrors what the L298N is being told; reported by GET /status

void beltSetup() {
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  // PIN_ENA doesn't need pinMode() - analogWrite() attaches it to the LEDC
  // PWM hardware automatically on first use (ESP32 core 3.x).

  // PWM frequency, found by testing on the real hardware (jumper OFF, speed
  // 120): 1000 Hz turns the belt smoothly (a faint whine is the trade-off);
  // 15 kHz and 20 kHz only made the motor buzz without turning - the L298N is
  // an older bipolar H-bridge and can't switch that fast into this motor.
  // Use the [ and ] serial keys to try other frequencies without reflashing.
  analogWriteFrequency(PIN_ENA, 1000);
}

// Coasts to a stop: PWM duty 0 disables the L298N's H-bridge output
// (high-impedance), so the belt rolls to a halt rather than being actively
// braked. Fine for a conveyor - the box is sitting on the belt, and an
// abrupt stop is more likely to tip it than a gentle one.
void beltStop() {
  beltRunning = false;
  analogWrite(PIN_ENA, 0);
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
}

// speed: -255..255. Positive runs the belt in the loading -> stations
// direction (subject to BELT_FORWARD); negative reverses it. This is the one
// function every later milestone will call.
void beltRun(int speed) {
  speed = constrain(speed, -255, 255);
  bool forward = (speed >= 0) == BELT_FORWARD;
  beltRunning = (speed != 0);

  digitalWrite(PIN_IN1, forward ? HIGH : LOW);
  digitalWrite(PIN_IN2, forward ? LOW : HIGH);
  analogWrite(PIN_ENA, abs(speed));
}

int beltSpeed = BELT_SPEED;

// --- H4: pusher servos ---------------------------------------------------------

Servo servos[2];
const int servoPins[2] = {PIN_SERVO1, PIN_SERVO2};
int servoRest[2] = {SERVO1_REST, SERVO2_REST};
int servoPush[2] = {SERVO1_PUSH, SERVO2_PUSH};
int servoAngle[2] = {SERVO1_REST, SERVO2_REST};
int selectedServo = 0;  // which servo the calibration keys act on (0 = Bed 1)

void servoSetup() {
  // ESP32Servo runs its PWM on separate LEDC timers from the belt's, but ask
  // it explicitly to leave the belt's timer alone.
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  for (int i = 0; i < 2; i++) {
    servos[i].setPeriodHertz(50);  // standard hobby-servo signal
    servos[i].attach(servoPins[i], 500, 2400);  // pulse range for an SG90
    servos[i].write(servoRest[i]);  // start parked at REST, never mid-swing
  }
}

void moveServo(int i, int angle) {
  servoAngle[i] = constrain(angle, 0, 180);
  servos[i].write(servoAngle[i]);
}

// One full push: swing to PUSH, hold long enough for the box to slide off,
// then return to REST. Blocking is fine for calibration; the real state
// machine will do this without delay().
void pushOnce(int i) {
  moveServo(i, servoPush[i]);
  delay(700);
  moveServo(i, servoRest[i]);
  delay(500);
}

// --- H3: IR box sensors ---------------------------------------------------------

const int irPins[2] = {PIN_IR1, PIN_IR2};
int irStable[2] = {-1, -1};  // last confirmed level (-1 = not read yet)
int irChanging[2] = {0, 0};  // consecutive samples that disagree with irStable
bool irFell[2] = {false, false};  // set when a sensor goes LOW (something in front); cleared by whoever acts on it
unsigned long irLastSampleAt = 0;

void irSetup() {
  // Pull-up so an unplugged sensor reads a steady HIGH instead of floating
  // (a floating pin flickers and would spam the log).
  for (int i = 0; i < 2; i++) pinMode(irPins[i], INPUT_PULLUP);
}

// Call every loop(). Samples every 10 ms and only reports a change after 5
// samples in a row (50 ms) agree, so a flicker never counts as a box.
void irUpdate() {
  unsigned long now = millis();
  if (now - irLastSampleAt < 10) return;
  irLastSampleAt = now;
  for (int i = 0; i < 2; i++) {
    int level = digitalRead(irPins[i]);
    if (irStable[i] == -1) {
      irStable[i] = level;
    } else if (level == irStable[i]) {
      irChanging[i] = 0;
    } else if (++irChanging[i] >= 5) {
      irStable[i] = level;
      irChanging[i] = 0;
      if (level == LOW) irFell[i] = true;
      Serial.printf("IR%d (Bed %d): %s\n", i + 1, i + 1, level ? "HIGH" : "LOW");
    }
  }
}

// Bench test (toggle with 't'): sensor A going LOW pushes servo 1, sensor B
// pushes servo 2. Lets one sensor and one servo be tried together before
// anything is mounted.
bool autoPush = false;

// H6 state, declared here so the HTTP status route (further down) can report it.
// The delivery loop itself is in the H6 section just before setup().
enum H6State { H6_IDLE, H6_LOADING, H6_RUNNING };
H6State h6State = H6_IDLE;
bool h6Enabled = true;  // the real delivery loop; 'h' toggles, bench test 't' switches it off
String jobId;
int jobBed = 0;  // 1 or 2

const char* h6StateName() {
  switch (h6State) {
    case H6_IDLE: return "idle";
    case H6_LOADING: return "loading";
    default: return "running";
  }
}

const char* modeName() {
  if (h6Enabled) return "delivery";
  return autoPush ? "bench" : "manual";
}

void beep(int ms);  // defined with the buzzer code below

void autoPushService() {
  for (int i = 0; i < 2; i++) {
    if (!irFell[i]) continue;
    irFell[i] = false;
    if (!autoPush) continue;
    Serial.printf("AUTO: sensor %c blocked -> push servo %d\n", 'A' + i, i + 1);
    // Stop the belt first so the box doesn't keep drifting during the push;
    // it stays stopped afterwards, like the real delivery loop will.
    if (beltRunning) {
      beltStop();
      Serial.println("AUTO: belt stopped");
    }
    pushOnce(i);
    beep(300);  // "delivered"
    Serial.printf("AUTO: delivered to bed %d\n", i + 1);
    // pushOnce() blocks ~1.2 s and the paddle may have crossed a sensor's view;
    // resync so that isn't counted as a new box.
    for (int j = 0; j < 2; j++) {
      irStable[j] = digitalRead(irPins[j]);
      irChanging[j] = 0;
      irFell[j] = false;
    }
  }
}

// --- H5: start button + buzzer ---------------------------------------------------

void buzzerSetup() {
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, BUZZER_ON_LEVEL == HIGH ? LOW : HIGH);  // silent
}

// Blocking beep - fine for now; the state machine will use a timer instead.
void beep(int ms) {
  digitalWrite(PIN_BUZZER, BUZZER_ON_LEVEL);
  delay(ms);
  digitalWrite(PIN_BUZZER, BUZZER_ON_LEVEL == HIGH ? LOW : HIGH);
}

bool buttonStable = false;   // true while the button is confirmed pressed
int buttonAgree = 0;         // consecutive samples disagreeing with buttonStable
unsigned long buttonPresses = 0;  // confirmed presses since boot (shown in /status for debugging)
unsigned long buttonLastSampleAt = 0;

void buttonSetup() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
}

// Call every loop(). Debounced: a press or release only counts after 5
// samples (50 ms) agree. Returns true once per confirmed PRESS.
bool buttonPressed() {
  unsigned long now = millis();
  if (now - buttonLastSampleAt < 10) return false;
  buttonLastSampleAt = now;
  bool down = digitalRead(PIN_BUTTON) == LOW;
  if (down == buttonStable) {
    buttonAgree = 0;
    return false;
  }
  if (++buttonAgree < 5) return false;
  buttonStable = down;
  buttonAgree = 0;
  Serial.println(down ? "BUTTON: pressed" : "BUTTON: released");
  return down;
}
void printServoHelp() {
  Serial.println("--- Servo calibration (H4) ---");
  Serial.println("  1 / 2 = select Bed 1 / Bed 2 servo");
  Serial.println("  . = angle +5    , = angle -5    (> = +1, < = -1)");
  Serial.println("  z = save current angle as REST   x = save current angle as PUSH");
  Serial.println("  p = run a push (REST -> PUSH -> REST) on the selected servo");
  Serial.println("  v = show saved angles");
}

void printBeltHelp() {
  Serial.println();
  Serial.println("--- Conveyor belt test (H2) ---");
  Serial.println("  f = run forward (loading -> stations)");
  Serial.println("  b = run backward");
  Serial.println("  s = stop");
  Serial.println("  + = faster    - = slower");
  Serial.println("  ] = PWM frequency up    [ = PWM frequency down");
  Serial.println("  ? = show this help again");
  Serial.printf("Speed: %d/255\n", beltSpeed);
}

void handleBeltCommand(char c) {
  switch (c) {
    case 'f':
      beltRun(beltSpeed);
      Serial.printf("belt forward  (speed %d)\n", beltSpeed);
      break;
    case 'b':
      beltRun(-beltSpeed);
      Serial.printf("belt backward (speed %d)\n", beltSpeed);
      break;
    case 's':
    case ' ':
      beltStop();
      Serial.println("belt stop");
      break;
    case '+':
      // Step of 1 - the motor's low end isn't very linear (stalls at 10,
      // already moving at 15), so pinpointing BELT_SPEED needs single-unit
      // steps right in that narrow window.
      beltSpeed = min(255, beltSpeed + 1);
      Serial.printf("speed -> %d/255\n", beltSpeed);
      break;
    case '-':
      beltSpeed = max(0, beltSpeed - 1);
      Serial.printf("speed -> %d/255\n", beltSpeed);
      break;
    case '[':
    case ']': {
      // Live PWM-frequency tuning: the L298N + this motor stall (buzz without
      // turning) at some frequencies, so step through a list instead of
      // reflashing for every guess. Re-send f/b afterwards to apply it.
      static const int FREQS[] = {200, 500, 1000, 2000, 4000, 8000, 15000, 20000};
      static int freqIdx = 2;  // starts at the 1000 Hz set in beltSetup()
      const int last = sizeof(FREQS) / sizeof(FREQS[0]) - 1;
      freqIdx = constrain(freqIdx + (c == ']' ? 1 : -1), 0, last);
      beltStop();
      analogWriteFrequency(PIN_ENA, FREQS[freqIdx]);
      Serial.printf("PWM frequency -> %d Hz (belt stopped - send f to run)\n", FREQS[freqIdx]);
      break;
    }
    case '1':
    case '2':
      selectedServo = c - '1';
      Serial.printf("servo %d selected (Bed %d), at %d deg\n", selectedServo + 1, selectedServo + 1, servoAngle[selectedServo]);
      break;
    case '.':
    case ',':
    case '>':
    case '<': {
      int step = (c == '.' || c == ',') ? 5 : 1;
      if (c == ',' || c == '<') step = -step;
      moveServo(selectedServo, servoAngle[selectedServo] + step);
      Serial.printf("servo %d -> %d deg\n", selectedServo + 1, servoAngle[selectedServo]);
      break;
    }
    case 'z':
      servoRest[selectedServo] = servoAngle[selectedServo];
      Serial.printf("servo %d REST saved = %d\n", selectedServo + 1, servoRest[selectedServo]);
      break;
    case 'x':
      servoPush[selectedServo] = servoAngle[selectedServo];
      Serial.printf("servo %d PUSH saved = %d\n", selectedServo + 1, servoPush[selectedServo]);
      break;
    case 'p':
      Serial.printf("servo %d push...\n", selectedServo + 1);
      pushOnce(selectedServo);
      Serial.println("done");
      break;
    case 'v':
      Serial.printf("servo 1: REST=%d PUSH=%d | servo 2: REST=%d PUSH=%d\n",
                    servoRest[0], servoPush[0], servoRest[1], servoPush[1]);
      Serial.println("(copy these into SERVOx_REST / SERVOx_PUSH in config.h to keep them)");
      break;
    case 'm': {
      // Diagnostic: cycle the IR pins between pull-up / pull-down / floating.
      // A pin really driven by the sensor reads the same in all three modes;
      // an unconnected pin just follows the pull setting.
      static int mode = 0;
      mode = (mode + 1) % 3;
      const int m = mode == 0 ? INPUT_PULLUP : (mode == 1 ? INPUT_PULLDOWN : INPUT);
      const char* name = mode == 0 ? "PULLUP" : (mode == 1 ? "PULLDOWN" : "FLOATING");
      for (int i = 0; i < 2; i++) pinMode(irPins[i], m);
      delay(50);
      Serial.printf("IR pin mode %s: IR1 raw = %d, IR2 raw = %d\n", name, digitalRead(irPins[0]), digitalRead(irPins[1]));
      break;
    }
    case 'k':
      Serial.println("beep");
      beep(300);
      break;
    case 'i':
      Serial.printf("IR1 (Bed 1) = %s | IR2 (Bed 2) = %s\n",
                    irStable[0] ? "HIGH" : "LOW", irStable[1] ? "HIGH" : "LOW");
      break;
    case 't':
      autoPush = !autoPush;
      if (autoPush) {
        h6Enabled = false;  // the bench test and the delivery loop never run together
        h6State = H6_IDLE;
      }
      Serial.printf("bench test %s (sensor A -> servo 1, sensor B -> servo 2)\n",
                    autoPush ? "ON" : "OFF");
      break;
    case 'u':
      beltStop();
      runSetupPortal("requested from the serial console", true);
      break;
    case 'c':
      clearSettings();
      Serial.println("Saved settings cleared - restarting with the defaults from config.h.");
      delay(300);
      ESP.restart();
      break;
    case 'h':
      h6Enabled = !h6Enabled;
      if (h6Enabled) autoPush = false;
      h6State = H6_IDLE;
      Serial.printf("delivery loop %s\n", h6Enabled ? "ON (polls the backend, button starts a run)" : "OFF");
      break;
    case '?':
      printBeltHelp();
      printServoHelp();
      Serial.println("--- IR sensors (H3) ---");
      Serial.println("  i = show both IR levels (changes are also printed automatically)");
      Serial.println("  t = toggle bench test: sensor A pushes servo 1, sensor B pushes servo 2");
      break;
    default:
      break;  // ignore newlines / unknown characters
  }
}

// --- Local HTTP control server -------------------------------------------------
// Lets a browser on the same WiFi (the Technician page's "Live hardware
// control" panel, or just curl) drive the station directly - no laptop
// tethered over USB. Every route just calls the same functions the Serial
// commands already use. Reachable at http://conveyor.local as well as
// whatever IP the router hands out, via mDNS below.
//
// No authentication - acceptable for a private demo WiFi network, not for
// anything public. Documented as a stated limitation, same as the rest of
// this project's plain-HTTP-on-a-LAN design.

WebServer server(80);

void sendCorsHeaders() {
  // The server serves one client at a time. Without this, a keep-alive client (a browser tab polling
  // /status, curl) keeps its connection open and every other request waits ~5 s behind it.
  server.sendHeader("Connection", "close");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// Browsers preflight non-trivial cross-origin requests with an OPTIONS call
// before the real one - answer it on every known route so the Technician
// page's fetch() calls aren't silently blocked by CORS.
void handleOptions() {
  sendCorsHeaders();
  server.send(204);
}

void handleStatus() {
  sendCorsHeaders();
  String json = "{";
  json += "\"belt_running\":" + String(beltRunning ? "true" : "false") + ",";
  json += "\"belt_speed\":" + String(beltSpeed) + ",";
  json += "\"ir1\":\"" + String(irStable[0] ? "HIGH" : "LOW") + "\",";
  json += "\"ir2\":\"" + String(irStable[1] ? "HIGH" : "LOW") + "\",";
  json += "\"servo1_deg\":" + String(servoAngle[0]) + ",";
  json += "\"servo2_deg\":" + String(servoAngle[1]) + ",";
  json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"mode\":\"" + String(modeName()) + "\",";
  json += "\"h6_state\":\"" + String(h6StateName()) + "\",";
  json += "\"settings\":\"" + String(cfg.fromFlash ? "saved" : "built-in") + "\",";
  json += "\"job_bed\":" + String(h6State == H6_IDLE ? 0 : jobBed) + ",";
  json += "\"button\":\"" + String(digitalRead(PIN_BUTTON) == LOW ? "PRESSED" : "open") + "\",";
  json += "\"button_presses\":" + String(buttonPresses) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleStart() {
  sendCorsHeaders();
  beltRun(beltSpeed);
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"start\"}");
}

// The on-screen Dispatch / Stop are handled like the machine's button, but the backend calls
// they trigger take a second or more, so the web request only sets a flag and replies at once;
// loop() does the work (same as a button press). Blocking inside the web handler stalls the server.
bool webButtonRequested = false;

void handleStop() {
  sendCorsHeaders();
  if (h6Enabled && h6State == H6_RUNNING) {
    // Same as pressing the machine's button during a run: stop, raise the jam alert, fail the job.
    webButtonRequested = true;
    server.send(200, "application/json", "{\"ok\":true,\"action\":\"emergency-stop\"}");
    return;
  }
  beltStop();
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"stop\"}");
}

// POST /dispatch: the on-screen equivalent of pressing the machine's button when a box is loaded.
void handleDispatch() {
  sendCorsHeaders();
  if (!h6Enabled || h6State == H6_RUNNING) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"a delivery is already running\"}");
    return;
  }
  webButtonRequested = true;
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"dispatch\"}");
}

// POST /push?bed=1 or ?bed=2. Blocks for ~1.2s (pushOnce() uses delay()),
// same as the Serial 'p' key already does - the HTTP response only comes
// back once the push has actually finished, which is the correct behaviour
// here (the caller wants to know it really happened, not just that the
// request was received).
void handlePush() {
  sendCorsHeaders();
  if (!server.hasArg("bed")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ?bed=1 or ?bed=2\"}");
    return;
  }
  int bed = server.arg("bed").toInt();
  if (bed != 1 && bed != 2) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bed must be 1 or 2\"}");
    return;
  }
  pushOnce(bed - 1);
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"push\",\"bed\":" + String(bed) + "}");
}

void handleBeep() {
  sendCorsHeaders();
  beep(300);
  server.send(200, "application/json", "{\"ok\":true,\"action\":\"beep\"}");
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    handleOptions();
    return;
  }
  sendCorsHeaders();
  server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
}

void webServerSetup() {
  if (MDNS.begin("conveyor")) {
    Serial.println("mDNS responder started: http://conveyor.local");
  } else {
    Serial.println("mDNS setup failed - control routes still reachable by IP");
  }

  server.on("/status", HTTP_GET, handleStatus);
  server.on("/status", HTTP_OPTIONS, handleOptions);
  server.on("/start", HTTP_POST, handleStart);
  server.on("/start", HTTP_OPTIONS, handleOptions);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/stop", HTTP_OPTIONS, handleOptions);
  server.on("/dispatch", HTTP_POST, handleDispatch);
  server.on("/dispatch", HTTP_OPTIONS, handleOptions);
  server.on("/push", HTTP_POST, handlePush);
  server.on("/push", HTTP_OPTIONS, handleOptions);
  server.on("/beep", HTTP_POST, handleBeep);
  server.on("/beep", HTTP_OPTIONS, handleOptions);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP control server started on port 80:");
  Serial.println("  GET  /status");
  Serial.println("  POST /start");
  Serial.println("  POST /stop");
  Serial.println("  POST /push?bed=1|2");
  Serial.println("  POST /beep");
}

// --- H1: WiFi + heartbeat -----------------------------------------------------

// --- H6: the real delivery loop ----------------------------------------------------
// IDLE:    every few seconds ask the backend for the next waiting job.
// LOADING: a job is waiting - the operator loads the box and presses the button;
//          that press tells the backend the run has started.
// RUNNING: belt on. When the sensor at the job's bed sees the box, stop the belt,
//          push, beep, and tell the backend the box arrived. Back to IDLE.
// The other bed's sensor is ignored, so a Bed 2 box rides past sensor A.

String authToken;
String backendBase = BACKEND_URL;  // replaced by the laptop's name lookup when that works
bool backendReachable = false;     // false until a call succeeds; triggers a fresh name lookup
unsigned long h6LastResolveAt = 0;
unsigned long h6LastPollAt = 0;
unsigned long h6RunStartedAt = 0;
String skipJobId;  // a job we cannot route; ignored for a while so it doesn't spam
unsigned long skipUntil = 0;
const unsigned long H6_POLL_MS = 4000;
const unsigned long H6_RUN_TIMEOUT_MS = 40000;  // box never reached its sensor

// The laptop's address changes on this router, so find it by name (mDNS, e.g.
// KARTHIK.local) and fall back to the address written in config.h.
void resolveBackend() {
  h6LastResolveAt = millis();
  if (cfg.backendHost.length() == 0) {
    backendBase = cfg.backendUrl;  // no name given: use the address only
    return;
  }
  IPAddress ip = MDNS.queryHost(cfg.backendHost.c_str(), 2000);
  if (ip != IPAddress(0, 0, 0, 0)) {
    String url = "http://" + ip.toString() + ":" + String(BACKEND_PORT);
    if (url != backendBase) {
      backendBase = url;
      Serial.printf("H6: found the backend at %s\n", url.c_str());
    }
  } else if (backendBase != cfg.backendUrl) {
    backendBase = cfg.backendUrl;
    Serial.printf("H6: name lookup failed, using the saved address (%s)\n", cfg.backendUrl.c_str());
  }
}

int backendRequest(const char* method, const String& path, const String& body, String& response) {
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);  // never let a WiFi hiccup freeze the station
  if (!http.begin(backendBase + path)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  if (authToken.length() > 0) http.addHeader("Authorization", "Bearer " + authToken);
  int code = (strcmp(method, "GET") == 0) ? http.GET() : http.POST(body);
  // A 204 has no body and no length; getString() would sit on the open connection until the 5 s timeout,
  // freezing the whole loop (and the web server) on every idle poll.
  response = (code > 0 && code != 204) ? http.getString() : String();
  http.end();
  backendReachable = (code > 0);
  return code;
}

bool backendLogin() {
  JsonDocument req;
  req["email"] = cfg.stationEmail;
  req["password"] = cfg.stationPass;
  String body;
  serializeJson(req, body);
  authToken = "";  // do not send a stale token while logging in
  String response;
  int code = backendRequest("POST", "/auth/login", body, response);
  if (code != 200) {
    Serial.printf("H6: backend login failed (HTTP %d)\n", code);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    Serial.println("H6: login reply was not valid JSON");
    return false;
  }
  authToken = doc["access_token"].as<String>();
  if (authToken.length() == 0) return false;
  Serial.println("H6: logged in to the backend");
  return true;
}

// Calls the backend, logging in first if needed and once more if the token expired.
int backendCall(const char* method, const String& path, const String& body, String& response) {
  if (authToken.length() == 0 && !backendLogin()) return -1;
  int code = backendRequest(method, path, body, response);
  if (code == 401 && backendLogin()) code = backendRequest(method, path, body, response);
  return code;
}

// Tells the dashboard what the belt is doing. 'jammed' raises an alert for staff.
void h6ReportConveyor(const char* motorStatus, const char* position, float speed) {
  JsonDocument req;
  req["motor_status"] = motorStatus;
  req["position"] = position;
  req["speed"] = speed;
  String body, response;
  serializeJson(req, body);
  int code = backendCall("POST", "/conveyor/simulate", body, response);
  if (code != 200) Serial.printf("H6: conveyor status report failed (HTTP %d)\n", code);
}

// Closes the current job in the backend as failed, so a stopped run does not stay
// "in transit" forever. Best effort: a technician can still mark it failed on the web page.
void h6FailJob(const char* why) {
  String response;
  int code = backendCall("POST", "/deliveries/" + jobId + "/fail", "", response);
  Serial.printf("H6: %s - delivery %s marked failed (HTTP %d)\n", why, jobId.c_str(), code);
}

void h6ResyncSensors() {
  // pushOnce() blocks and the paddle may cross a sensor's view; don't count that as a box.
  for (int j = 0; j < 2; j++) {
    irStable[j] = digitalRead(irPins[j]);
    irChanging[j] = 0;
    irFell[j] = false;
  }
}

void h6ErrorBeeps() {
  for (int i = 0; i < 3; i++) {
    beep(150);
    delay(150);
  }
}

void h6PollJob() {
  String response;
  int code = backendCall("GET", "/conveyor/next-job", "", response);
  if (code == 204) return;  // nothing waiting
  if (code != 200) {
    Serial.printf("H6: next-job failed (HTTP %d)\n", code);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    Serial.println("H6: next-job reply was not valid JSON");
    return;
  }
  String id = doc["delivery_id"].as<String>();
  int bed = doc["bed_no"].as<String>().toInt();
  String patient = doc["patient_name"].as<String>();
  if (id == skipJobId && millis() < skipUntil) return;
  if (bed != 1 && bed != 2) {
    Serial.printf("H6: job %s is for bed '%s' - this station only serves beds 1 and 2, skipping\n",
                  id.c_str(), doc["bed_no"].as<String>().c_str());
    skipJobId = id;
    skipUntil = millis() + 60000;
    return;
  }
  jobId = id;
  jobBed = bed;
  h6State = H6_LOADING;
  beep(120);
  Serial.printf("H6: job %s for %s, Bed %d - load the box and press the button\n",
                jobId.c_str(), patient.c_str(), jobBed);
}

void h6Deliver() {
  beltStop();
  Serial.printf("H6: box at bed %d sensor - belt stopped, pushing\n", jobBed);
  pushOnce(jobBed - 1);
  beep(300);  // "delivered"
  h6ResyncSensors();
  String response;
  int code = -1;
  for (int attempt = 0; attempt < 3 && code != 200; attempt++) {
    code = backendCall("POST", "/deliveries/" + jobId + "/mark-arrived", "", response);
    if (code != 200) delay(1000);
  }
  Serial.printf("H6: delivery %s marked arrived (HTTP %d)\n", jobId.c_str(), code);
  h6ReportConveyor("idle", "bed-bay", 0);
  h6State = H6_IDLE;
}

void h6OnButton() {
  if (h6State == H6_IDLE) {
    // The previous job is done: a press now fetches the next waiting job (instead of waiting for
    // the 4 s poll) and falls through to start it. No job waiting = nothing happens.
    h6PollJob();
    if (h6State != H6_LOADING) {
      Serial.println("H6: button pressed but no delivery is waiting");
      return;
    }
  }
  if (h6State == H6_LOADING) {
    String response;
    int code = backendCall("POST", "/deliveries/" + jobId + "/dispatch", "", response);
    if (code != 200) {
      Serial.printf("H6: dispatch refused (HTTP %d) - back to idle\n", code);
      h6ErrorBeeps();
      h6State = H6_IDLE;
      return;
    }
    irFell[0] = irFell[1] = false;  // only a box that arrives from now on counts
    h6RunStartedAt = millis();
    h6State = H6_RUNNING;
    beltRun(beltSpeed);
    Serial.printf("H6: dispatched - belt running for Bed %d\n", jobBed);
    h6ReportConveyor("running", "loading-bay", 1.0);
  } else if (h6State == H6_RUNNING) {
    beltStop();  // pressing during a run is an emergency stop
    h6ReportConveyor("jammed", "emergency-stop", 0);
    h6FailJob("emergency stop");
    h6State = H6_IDLE;
  }
}

void h6Update() {
  unsigned long now = millis();
  if (h6State == H6_IDLE) {
    if (now - h6LastPollAt >= H6_POLL_MS) {
      h6LastPollAt = now;
      // Look the laptop up again when the last call failed, and at least every minute.
      if (h6LastResolveAt == 0 || !backendReachable || now - h6LastResolveAt > 60000) resolveBackend();
      h6PollJob();
    }
  } else if (h6State == H6_RUNNING) {
    int target = jobBed - 1;
    irFell[1 - target] = false;  // ignore the other bed's sensor
    if (irFell[target]) {
      irFell[target] = false;
      h6Deliver();
    } else if (now - h6RunStartedAt > H6_RUN_TIMEOUT_MS) {
      beltStop();
      Serial.println("H6: timed out - the box never reached its sensor");
      h6ErrorBeeps();
      h6ReportConveyor("jammed", "belt", 0);
      h6FailJob("timeout");
      h6State = H6_IDLE;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);  // gives the Serial Monitor time to attach after upload
  Serial.println();
  Serial.println("=== MediDispense conveyor - H3-H5 + HTTP control (belt + servos + IR + button + buzzer + WiFi + web server) ===");

  pinMode(ONBOARD_LED_PIN, OUTPUT);
  beltSetup();
  beltStop();  // never start with the belt in an undefined state
  servoSetup();
  irSetup();
  buzzerSetup();
  buttonSetup();

  loadSettings();
  backendBase = cfg.backendUrl;
  Serial.printf("Settings: %s\n", cfg.fromFlash ? "saved on the board (setup page)" : "built-in defaults from config.h");

  // Holding the push button for 3 s while powering on opens the setup page.
  bool holdForSetup = false;
  if (digitalRead(PIN_BUTTON) == LOW) {
    unsigned long heldSince = millis();
    while (digitalRead(PIN_BUTTON) == LOW && millis() - heldSince < 3000) delay(20);
    holdForSetup = (millis() - heldSince >= 3000);
  }
  if (settingsArePlaceholders()) runSetupPortal("first start: no WiFi has been set up yet", false);
  if (holdForSetup) runSetupPortal("button held while powering on", true);

  Serial.printf("Connecting to WiFi \"%s\"", cfg.ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid.c_str(), cfg.wifiPass.c_str());

  // While connecting, blink fast (5 Hz) so you can tell at a glance the
  // board is alive and still trying - not yet successful. Give up after 30 s
  // and open the setup page, so a changed or missing WiFi can be fixed without a cable.
  unsigned long connectStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectStart < 30000) {
    digitalWrite(ONBOARD_LED_PIN, ledOn ? HIGH : LOW);
    ledOn = !ledOn;
    delay(100);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println();
    runSetupPortal("could not join the saved WiFi", true);
  }

  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  webServerSetup();

  printBeltHelp();
  printServoHelp();
  Serial.println("--- IR sensors (H3) ---");
  Serial.println("  i = show both IR levels (changes are also printed automatically)");
  Serial.println("  t = toggle bench test: sensor A pushes servo 1, sensor B pushes servo 2");
  Serial.println("--- Delivery loop (H6) ---");
  Serial.println("  h = toggle the real delivery loop (on at boot): polls the backend, button starts a run");
  Serial.println("--- Setup ---");
  Serial.println("  u = open the setup page (WiFi MediDispense-Setup)   c = clear saved settings");
  Serial.println("  Also: hold the push button for 3 s while powering on.");
}

void loop() {
  // Slow steady heartbeat once connected - see H1 above.
  unsigned long now = millis();
  if (now - lastBlinkAt >= 1000) {
    lastBlinkAt = now;
    ledOn = !ledOn;
    digitalWrite(ONBOARD_LED_PIN, ledOn ? HIGH : LOW);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi dropped - restarting to reconnect cleanly.");
    beltStop();  // never leave the belt running unattended
    delay(1000);
    ESP.restart();
  }

  irUpdate();
  if (autoPush) autoPushService();  // bench test only: it would otherwise swallow the sensor events H6 needs
  if (h6Enabled) h6Update();
  if (buttonPressed()) {
    buttonPresses++;
    beep(120);  // H5: audible feedback on every press
    if (h6Enabled) {
      h6OnButton();
    } else if (autoPush) {
      // Bench test: button starts a run; pressing while running is an emergency stop.
      if (beltRunning) {
        beltStop();
        Serial.println("BUTTON: belt stopped (emergency stop)");
      } else {
        beltRun(beltSpeed);
        Serial.println("BUTTON: belt started");
      }
    }
  }
  server.handleClient();
  if (webButtonRequested) {
    webButtonRequested = false;
    if (h6Enabled) h6OnButton();
  }

  if (Serial.available() > 0) {
    handleBeltCommand(Serial.read());
  }
}
