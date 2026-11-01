#pragma once

// ---------------------------------------------------------------------------
// Setup portal: change the WiFi, the backend computer and the station login without
// editing config.h or reflashing.
//
// The values are kept in the board's flash (Preferences / NVS). On start the board uses the saved
// values; if nothing is saved it falls back to the defaults compiled in from config.h.
//
// The board opens its own WiFi network "MediDispense-Setup" (password below) and serves a form at
// http://192.168.4.1 when:
//   - there is no saved setup and config.h still has its placeholder WiFi name (first start),
//   - it cannot join the saved WiFi within 30 s (the WiFi changed or is gone),
//   - the push button is held for 3 s while powering on, or
//   - 'u' is typed in the serial console (for testing).
// The portal is only on the board's own network, never on your normal WiFi, so nobody on that
// WiFi can change the settings. Passwords are never printed to the serial console.
// ---------------------------------------------------------------------------

#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#ifndef BACKEND_HOST
#define BACKEND_HOST ""
#endif

void beep(int ms);  // defined with the buzzer code in the main sketch
extern WebServer server;  // the normal control server, defined in the main sketch

struct Settings {
  String ssid, wifiPass, backendHost, backendUrl, stationEmail, stationPass;
  bool fromFlash = false;
};
Settings cfg;

static const char* PORTAL_AP_NAME = "MediDispense-Setup";
static const char* PORTAL_AP_PASS = "medidispense";
static const char* PLACEHOLDER_WIFI = "YOUR_WIFI_NAME";
static const char* PLACEHOLDER_STATION_PASS = "THE_STATION_ACCOUNT_PASSWORD";

void loadSettings() {
  cfg.ssid = WIFI_SSID;
  cfg.wifiPass = WIFI_PASSWORD;
  cfg.backendHost = BACKEND_HOST;
  cfg.backendUrl = BACKEND_URL;
  cfg.stationEmail = STATION_EMAIL;
  cfg.stationPass = STATION_PASSWORD;
  Preferences p;
  if (!p.begin("medi", true)) return;  // nothing has ever been saved
  if (p.isKey("ssid")) {
    cfg.fromFlash = true;
    cfg.ssid = p.getString("ssid", cfg.ssid);
    cfg.wifiPass = p.getString("wpass", cfg.wifiPass);
    cfg.backendHost = p.getString("bhost", cfg.backendHost);
    cfg.backendUrl = p.getString("burl", cfg.backendUrl);
    cfg.stationEmail = p.getString("semail", cfg.stationEmail);
    cfg.stationPass = p.getString("spass", cfg.stationPass);
  }
  p.end();
}

void saveSettings() {
  Preferences p;
  p.begin("medi", false);
  p.putString("ssid", cfg.ssid);
  p.putString("wpass", cfg.wifiPass);
  p.putString("bhost", cfg.backendHost);
  p.putString("burl", cfg.backendUrl);
  p.putString("semail", cfg.stationEmail);
  p.putString("spass", cfg.stationPass);
  p.end();
}

void clearSettings() {
  Preferences p;
  p.begin("medi", false);
  p.clear();
  p.end();
}

// True while config.h still holds its example WiFi name and nothing has been saved.
bool settingsArePlaceholders() { return !cfg.fromFlash && cfg.ssid == PLACEHOLDER_WIFI; }

static String htmlEscape(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

static WebServer portal(80);
static String portalNetworks;  // <option> list built from a scan when the portal starts

static String portalPage(const String& message, bool isError) {
  String h;
  h.reserve(4200);
  h += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>MediDispense conveyor setup</title><style>"
         "body{font-family:Segoe UI,Arial,sans-serif;background:#fff;color:#1b1f24;margin:0;padding:16px;max-width:520px}"
         "h1{font-size:20px;margin:0 0 4px}p{color:#55606b;margin:4px 0 12px;font-size:14px}"
         "label{display:block;font-weight:600;font-size:14px;margin:12px 0 3px}"
         "input,select{width:100%;padding:10px;font-size:16px;border:1px solid #b9c2ca;border-radius:6px;box-sizing:border-box}"
         "small{color:#6b7782;font-size:12px}"
         "button{margin-top:18px;width:100%;padding:12px;font-size:16px;background:#0b6e63;color:#fff;border:0;border-radius:6px}"
         ".msg{padding:10px;border-radius:6px;margin:10px 0;font-size:14px}"
         ".err{background:#fdf0ee;border:1px solid #c0392b}.ok{background:#f0f8f4;border:1px solid #5fa38a}"
         "</style></head><body><h1>MediDispense conveyor setup</h1>"
         "<p>Tell the conveyor which WiFi to join and where the backend computer is. It restarts after you save.</p>");
  if (message.length()) {
    h += "<div class='msg ";
    h += isError ? "err" : "ok";
    h += "'>" + htmlEscape(message) + "</div>";
  }
  h += F("<form method='POST' action='/save'>"
         "<label>WiFi network</label><select name='ssid_pick'>");
  h += portalNetworks;
  h += F("</select><label>...or type its name</label><input name='ssid_typed' placeholder='only if it is not in the list' autocapitalize='off'>"
         "<label>WiFi password</label><input name='wifi_pass' type='password' autocomplete='off'>"
         "<small>Leave empty to keep the saved password for the same network, or if the network has no password. 2.4 GHz only.</small>"
         "<label>Backend computer name</label><input name='backend_host' value='");
  h += htmlEscape(cfg.backendHost);
  h += F("' placeholder='run  hostname  on that computer' autocapitalize='off'>"
         "<label>Backend computer address (backup)</label><input name='backend_url' value='");
  h += htmlEscape(cfg.backendUrl);
  h += F("' placeholder='http://192.168.1.10:8000' autocapitalize='off'>"
         "<small>Used if the name is not found. Find it with  ipconfig  on that computer.</small>"
         "<label>Conveyor login (email)</label><input name='station_email' value='");
  h += htmlEscape(cfg.stationEmail);
  h += F("' autocapitalize='off'>"
         "<label>Conveyor login (password)</label><input name='station_pass' type='password' autocomplete='off'>"
         "<small>Leave empty to keep the saved one for the same email.</small>"
         "<button type='submit'>Save and restart</button></form></body></html>");
  return h;
}

static void portalRoot() { portal.send(200, "text/html", portalPage("", false)); }

static void portalRedirect() {
  portal.sendHeader("Location", "http://192.168.4.1/");
  portal.send(302, "text/plain", "");
}

static void portalSave() {
  String ssid = portal.arg("ssid_typed");
  ssid.trim();
  if (ssid.length() == 0) ssid = portal.arg("ssid_pick");
  ssid.trim();
  String pass = portal.arg("wifi_pass");
  String host = portal.arg("backend_host");
  host.trim();
  if (host.endsWith(".local")) host.remove(host.length() - 6);
  String url = portal.arg("backend_url");
  url.trim();
  String email = portal.arg("station_email");
  email.trim();
  String spass = portal.arg("station_pass");

  String problem;
  if (ssid.length() == 0 || ssid == PLACEHOLDER_WIFI) problem = "Choose or type the WiFi network name.";
  else if (ssid.length() > 32) problem = "A WiFi name can be at most 32 characters.";
  else if (pass.length() > 63) problem = "A WiFi password can be at most 63 characters.";
  else if (host.length() == 0 && url.length() == 0) problem = "Give the backend computer's name or its address (or both).";
  else if (email.indexOf('@') < 1) problem = "Enter the conveyor's login email.";

  if (problem.length() == 0) {
    if (pass.length() == 0 && ssid == cfg.ssid) pass = cfg.wifiPass;  // same network: keep the saved password
    if (spass.length() == 0 && email == cfg.stationEmail) spass = cfg.stationPass;
    if (spass.length() == 0 || spass == PLACEHOLDER_STATION_PASS) problem = "Enter the conveyor login's password.";
  }
  if (problem.length() == 0 && url.length() > 0 && !url.startsWith("http")) url = "http://" + url;

  if (problem.length() > 0) {
    portal.send(400, "text/html", portalPage(problem, true));
    return;
  }

  cfg.ssid = ssid;
  cfg.wifiPass = pass;
  cfg.backendHost = host;
  cfg.backendUrl = url;
  cfg.stationEmail = email;
  cfg.stationPass = spass;
  saveSettings();
  Serial.printf("SETUP: saved. WiFi \"%s\", backend \"%s\" / %s, login %s. Restarting.\n", ssid.c_str(), host.c_str(),
                url.c_str(), email.c_str());
  portal.send(200, "text/html",
              "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>Saved</title></head><body style='font-family:Segoe UI,Arial,sans-serif;padding:16px;max-width:520px'>"
              "<h1 style='font-size:20px'>Saved</h1><p>The conveyor is restarting and will join your WiFi. "
              "Reconnect this phone or computer to your normal WiFi.</p>"
              "<p>If the conveyor cannot join it, the <b>MediDispense-Setup</b> network will appear again after about 30 seconds.</p>"
              "</body></html>");
  delay(1500);
  ESP.restart();
}

// Never returns: it ends by restarting (after a save, or after 10 idle minutes when a usable setup exists).
[[noreturn]] void runSetupPortal(const char* why, bool hasUsableSetup) {
  server.close();  // free port 80 if the normal control server is already running (e.g. opened from the console)
  Serial.printf("SETUP MODE (%s).\n", why);
  Serial.printf("  1. Join the WiFi \"%s\" (password \"%s\")\n  2. Open http://192.168.4.1\n", PORTAL_AP_NAME, PORTAL_AP_PASS);
  WiFi.mode(WIFI_AP_STA);

  int found = WiFi.scanNetworks();
  portalNetworks = "";
  String seen = "|";
  for (int i = 0; i < found && i < 20; i++) {
    String name = WiFi.SSID(i);
    if (name.length() == 0 || seen.indexOf("|" + name + "|") >= 0) continue;
    seen += name + "|";
    portalNetworks += "<option value=\"" + htmlEscape(name) + "\"";
    if (name == cfg.ssid) portalNetworks += " selected";
    portalNetworks += ">" + htmlEscape(name) + "</option>";
  }
  if (portalNetworks.length() == 0) portalNetworks = "<option value=''>(no networks found: type the name below)</option>";

  WiFi.softAP(PORTAL_AP_NAME, PORTAL_AP_PASS);
  portal.on("/", HTTP_GET, portalRoot);
  portal.on("/save", HTTP_POST, portalSave);
  portal.onNotFound(portalRedirect);
  portal.begin();

  beep(120);
  unsigned long startedAt = millis();
  unsigned long lastBlink = 0;
  bool led = false;
  while (true) {
    portal.handleClient();
    if (millis() - lastBlink > 250) {  // fast blink = waiting for setup
      lastBlink = millis();
      led = !led;
      digitalWrite(ONBOARD_LED_PIN, led ? HIGH : LOW);
    }
    if (hasUsableSetup && millis() - startedAt > 600000UL) {
      Serial.println("SETUP: nothing saved in 10 minutes - restarting with the current settings.");
      delay(200);
      ESP.restart();
    }
    delay(2);
  }
}
