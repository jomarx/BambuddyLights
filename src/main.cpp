// BambuddyLights - ESP8266 stack tower light for a Bambuddy server.
//
// Green  : printers that are free        (solid = 1, blinking = 2 or more)
// Orange : printers that need attention  (solid = 1, blinking = 2 or more)
// Red    : printers with an error        (solid = 1, blinking = 2 or more)
//
// See README.md for wiring, setup and the connection-status patterns.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <Ticker.h>
#include <WiFiManager.h>

// ---------------------------------------------------------------- pins ----
static const uint8_t PIN_GREEN = D1;   // GPIO5
static const uint8_t PIN_ORANGE = D2;  // GPIO4
static const uint8_t PIN_RED = D5;     // GPIO14
static const uint8_t PIN_BUZZER = D6;  // GPIO12, reserved, held off
static const uint8_t PIN_BUTTON = D3;  // GPIO0, to GND. Do not hold while powering on.

// Level that turns a lamp on. HIGH suits a ULN2003 or an active-high relay
// board; build with -DLAMP_ON_LEVEL=LOW for active-low relay modules.
#ifndef LAMP_ON_LEVEL
#define LAMP_ON_LEVEL HIGH
#endif

// ------------------------------------------------------------- settings ----
static const char *HOSTNAME = "bambulight";
static const char *ADMIN_USER = "admin";
static const char *CONFIG_FILE = "/config.json";
static const char *WIFI_SETUP_FLAG = "/wifisetup";
static const uint8_t MAX_PRINTERS = 32;
static const uint32_t HTTP_TIMEOUT_MS = 3000;
static const uint32_t LIST_REFRESH_MS = 60000;
static const uint32_t WIFI_LOST_RESTART_MS = 60000;
static const uint32_t FACTORY_RESET_HOLD_MS = 5000;
static const uint16_t PORTAL_TIMEOUT_S = 180;
static const uint8_t FAILS_BEFORE_SERVER_LOST = 3;
// Bambuddy allows 100 reads a minute; stay at 80 when polling per printer.
static const uint32_t MIN_MS_PER_REQUEST = 750;

struct Config {
  char host[64] = "";
  uint16_t port = 8000;
  char apiKey[129] = "";
  char adminPass[65] = "";
  uint16_t pollSec = 5;
  bool offlineIsAttention = true;
  bool heartbeat = true;
};
static Config cfg;

// --------------------------------------------------------------- lights ----
enum Mode : uint8_t { M_SETUP_AP, M_WIFI, M_SERVER, M_AUTH, M_NORMAL };
enum Lamp : uint8_t { L_OFF, L_SOLID, L_BLINK };

static volatile Mode mode = M_WIFI;
static volatile Lamp lampGreen = L_OFF, lampOrange = L_OFF, lampRed = L_OFF;
static volatile uint32_t lampTestUntil = 0;  // all lamps on until this millis()
static Ticker lightTicker;

static void writeLamps(bool g, bool o, bool r) {
  digitalWrite(PIN_GREEN, g ? LAMP_ON_LEVEL : !LAMP_ON_LEVEL);
  digitalWrite(PIN_ORANGE, o ? LAMP_ON_LEVEL : !LAMP_ON_LEVEL);
  digitalWrite(PIN_RED, r ? LAMP_ON_LEVEL : !LAMP_ON_LEVEL);
}

static void startLampTest(uint32_t ms) { lampTestUntil = (millis() + ms) | 1; }

// Runs every 50 ms from a timer, so patterns keep moving during HTTP calls.
static void lightTick() {
  static uint32_t t = 0;
  t++;
  if (lampTestUntil) {
    if ((int32_t)(millis() - lampTestUntil) < 0) {
      writeLamps(true, true, true);
      return;
    }
    lampTestUntil = 0;
  }
  bool g = false, o = false, r = false;
  switch (mode) {
    case M_SETUP_AP: {  // chase green -> orange -> red
      uint8_t s = (t / 6) % 3;
      g = s == 0;
      o = s == 1;
      r = s == 2;
      break;
    }
    case M_WIFI:  // green <-> orange
      g = (t / 8) % 2 == 0;
      o = !g;
      break;
    case M_SERVER:  // orange <-> red
      o = (t / 8) % 2 == 0;
      r = !o;
      break;
    case M_AUTH: {  // red double flash every 2 s
      uint8_t p = t % 40;
      r = p < 3 || (p >= 6 && p < 9);
      break;
    }
    case M_NORMAL: {
      bool blink = (t / 10) % 2 == 0;
      g = lampGreen == L_SOLID || (lampGreen == L_BLINK && blink);
      o = lampOrange == L_SOLID || (lampOrange == L_BLINK && blink);
      r = lampRed == L_SOLID || (lampRed == L_BLINK && blink);
      // Everything busy: short green blip every 3 s so the device looks alive.
      // Can be turned off (relay boards click on every blip).
      if (cfg.heartbeat && lampGreen == L_OFF && lampOrange == L_OFF && lampRed == L_OFF) g = t % 60 < 2;
      break;
    }
  }
  writeLamps(g, o, r);
}

static Lamp lampForCount(uint8_t n) { return n == 0 ? L_OFF : (n == 1 ? L_SOLID : L_BLINK); }

// --------------------------------------------------------------- config ----
static void loadConfig() {
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return;
  JsonDocument doc;
  if (!deserializeJson(doc, f)) {
    strlcpy(cfg.host, doc["host"] | "", sizeof(cfg.host));
    cfg.port = doc["port"] | 8000;
    strlcpy(cfg.apiKey, doc["apiKey"] | "", sizeof(cfg.apiKey));
    strlcpy(cfg.adminPass, doc["adminPass"] | "", sizeof(cfg.adminPass));
    cfg.pollSec = doc["pollSec"] | 5;
    cfg.offlineIsAttention = doc["offlineIsAttention"] | true;
    cfg.heartbeat = doc["heartbeat"] | true;
  }
  f.close();
}

static void saveConfig() {
  JsonDocument doc;
  doc["host"] = cfg.host;
  doc["port"] = cfg.port;
  doc["apiKey"] = cfg.apiKey;
  doc["adminPass"] = cfg.adminPass;
  doc["pollSec"] = cfg.pollSec;
  doc["offlineIsAttention"] = cfg.offlineIsAttention;
  doc["heartbeat"] = cfg.heartbeat;
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

static void setHost(const String &value) {
  String host = value;
  host.trim();
  if (host.startsWith("http://")) host.remove(0, 7);
  while (host.endsWith("/")) host.remove(host.length() - 1);
  strlcpy(cfg.host, host.c_str(), sizeof(cfg.host));
}

static uint16_t parsePort(const String &value) {
  long port = value.toInt();
  return port > 0 && port < 65536 ? port : 8000;
}

static uint16_t parsePoll(const String &value) { return constrain(value.toInt(), 2, 300); }

// ------------------------------------------------------------- printers ----
enum Category : uint8_t { C_UNKNOWN, C_BUSY, C_FREE, C_ATTENTION, C_ERROR };

struct Printer {
  int id;
  char name[25];
  Category cat;
  bool connected;
  char state[12];
};
static Printer printers[MAX_PRINTERS];
static uint8_t printerCount = 0;
static uint8_t printersSkipped = 0;  // active printers beyond MAX_PRINTERS
static uint8_t countFree = 0, countAttention = 0, countError = 0;
static uint32_t lastListMs = 0;
static bool haveList = false;
static bool forceListRefresh = true;
static bool useBulk = true;  // false once the server says the bulk route doesn't exist
static uint8_t failCount = 0;
static bool everOk = false;
static int lastHttpCode = 0;
static uint32_t lastOkMs = 0;

static String baseUrl() { return "http://" + String(cfg.host) + ":" + String(cfg.port) + "/api/v1"; }

// Opens a GET request. HTTP/1.0 keeps the body un-chunked so ArduinoJson can
// parse it straight from the socket with a filter (status bodies are large).
static int httpGet(HTTPClient &http, WiFiClient &client, const String &url) {
  client.setTimeout(HTTP_TIMEOUT_MS);
  http.useHTTP10(true);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return -1;
  if (cfg.apiKey[0]) http.addHeader("X-API-Key", cfg.apiKey);
  return http.GET();
}

// Only the status fields the lights need. Built once, used for every response.
static JsonDocument &statusFilter() {
  static JsonDocument f;
  if (f.isNull()) {
    f["id"] = true;
    f["connected"] = true;
    f["state"] = true;
    f["awaiting_plate_clear"] = true;
    f["hms_errors"][0]["severity"] = true;
  }
  return f;
}

static JsonDocument &bulkFilter() {
  static JsonDocument f;
  if (f.isNull()) f[0] = statusFilter();
  return f;
}

static JsonDocument &listFilter() {
  static JsonDocument f;
  if (f.isNull()) {
    f[0]["id"] = true;
    f[0]["name"] = true;
    f[0]["is_active"] = true;
  }
  return f;
}

static Printer *findPrinter(int id) {
  for (uint8_t i = 0; i < printerCount; i++)
    if (printers[i].id == id) return &printers[i];
  return nullptr;
}

// One PrinterStatus object -> category.
static void classify(JsonObjectConst s, Printer &p) {
  const char *state = s["state"] | "";
  bool awaitingPlate = s["awaiting_plate_clear"] | false;
  // Bambu HMS alert level: 1 = error (stopped), 2 = warning (paused), 3 = notification.
  // Bambuddy already drops the HMS echoes a user cancel produces.
  bool hmsError = false, hmsWarning = false;
  for (JsonObjectConst h : s["hms_errors"].as<JsonArrayConst>()) {
    int sev = h["severity"] | 0;
    if (sev == 1) hmsError = true;
    if (sev == 2) hmsWarning = true;
  }
  p.connected = s["connected"] | false;
  strlcpy(p.state, state, sizeof(p.state));

  if (!p.connected) {
    p.cat = awaitingPlate || cfg.offlineIsAttention ? C_ATTENTION : C_BUSY;
  } else if (hmsError) {
    p.cat = C_ERROR;  // a real failure carries an HMS / print_error of level 1
  } else if (!strcmp(state, "PAUSE") || hmsWarning || awaitingPlate) {
    p.cat = C_ATTENTION;
  } else if (!strcmp(state, "IDLE") || !strcmp(state, "FINISH") || !strcmp(state, "FAILED")) {
    p.cat = C_FREE;  // FAILED with no error left is a cancelled print
  } else {
    p.cat = C_BUSY;  // RUNNING, PREPARE, SLICING, ...
  }
}

// GET /printers/ -> [{id, name, is_active, ...}]
static int fetchPrinterList() {
  WiFiClient client;
  HTTPClient http;
  int code = httpGet(http, client, baseUrl() + "/printers/");
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(listFilter()));
    if (err || !doc.is<JsonArray>()) {
      code = -2;
    } else {
      uint8_t n = 0, skipped = 0;
      for (JsonObject p : doc.as<JsonArray>()) {
        if (!(p["is_active"] | true)) continue;
        if (n >= MAX_PRINTERS) {
          skipped++;
          continue;
        }
        printers[n].id = p["id"] | 0;
        strlcpy(printers[n].name, p["name"] | "?", sizeof(printers[n].name));
        printers[n].cat = C_UNKNOWN;
        printers[n].connected = false;
        printers[n].state[0] = 0;
        n++;
      }
      printerCount = n;
      printersSkipped = skipped;
      if (skipped) Serial.printf("Warning: %u printers over the limit of %u are not shown\n", skipped, MAX_PRINTERS);
      haveList = true;
      lastListMs = millis();
      forceListRefresh = false;
    }
  }
  http.end();
  return code;
}

// GET /bulk/printer-statuses?ids=1,2,3 -> [PrinterStatus, ...], all printers in one request.
static int fetchBulkStatus() {
  String ids;
  for (uint8_t i = 0; i < printerCount; i++) {
    if (i) ids += ',';
    ids += printers[i].id;
  }
  WiFiClient client;
  HTTPClient http;
  int code = httpGet(http, client, baseUrl() + "/bulk/printer-statuses?ids=" + ids);
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(bulkFilter()));
    if (err || !doc.is<JsonArray>()) {
      code = -2;
    } else {
      // A printer left out of the answer (removed, or out of the key's scope) stays unknown.
      for (uint8_t i = 0; i < printerCount; i++) printers[i].cat = C_UNKNOWN;
      for (JsonObjectConst s : doc.as<JsonArrayConst>()) {
        Printer *p = findPrinter(s["id"] | -1);
        if (p) classify(s, *p);
      }
    }
  }
  http.end();
  return code;
}

// GET /printers/{id}/status, for Bambuddy versions without the bulk route.
static int fetchPrinterStatus(Printer &p) {
  WiFiClient client;
  HTTPClient http;
  int code = httpGet(http, client, baseUrl() + "/printers/" + String(p.id) + "/status");
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getStream(), DeserializationOption::Filter(statusFilter()))) code = -2;
    else classify(doc.as<JsonObjectConst>(), p);
  }
  http.end();
  return code;
}

static void serviceIO();  // keeps the web page and button alive between requests

static void pollFailed(int code) {
  lastHttpCode = code;
  if (code == 401 || code == 403) {
    mode = M_AUTH;
    return;
  }
  if (failCount < 255) failCount++;
  if (!everOk || failCount >= FAILS_BEFORE_SERVER_LOST) mode = M_SERVER;
}

static void pollRound() {
  if (!cfg.host[0]) {
    mode = M_SERVER;  // nothing configured yet; set it at http://bambulight.local/settings
    return;
  }
  if (!haveList || forceListRefresh || millis() - lastListMs > LIST_REFRESH_MS) {
    int code = fetchPrinterList();
    if (code != 200) return pollFailed(code);
    serviceIO();
  }

  int code = 200;
  if (printerCount && useBulk) {
    code = fetchBulkStatus();
    if (code == 404 || code == 405) {
      useBulk = false;  // older Bambuddy: fall back to one request per printer
      Serial.println(F("Bulk status route not available, polling per printer"));
    } else if (code != 200) {
      return pollFailed(code);
    }
  }
  if (printerCount && !useBulk) {
    // One unreadable printer must not hide the others: it is marked unknown
    // (shown as needing attention) and the round carries on.
    uint8_t ok = 0;
    for (uint8_t i = 0; i < printerCount; i++) {
      int c = fetchPrinterStatus(printers[i]);
      if (c == 200) {
        ok++;
      } else {
        printers[i].cat = C_UNKNOWN;
        if (c == 401 || c == 403) return pollFailed(c);
        if (c == 404) forceListRefresh = true;  // printer removed; new list next round
        code = c;
      }
      serviceIO();
    }
    if (!ok) return pollFailed(code);
  }

  uint8_t nFree = 0, nAttention = 0, nError = 0;
  for (uint8_t i = 0; i < printerCount; i++) {
    switch (printers[i].cat) {
      case C_FREE: nFree++; break;
      case C_ERROR: nError++; break;
      case C_ATTENTION:
      case C_UNKNOWN: nAttention++; break;
      default: break;
    }
  }
  countFree = nFree;
  countAttention = nAttention;
  countError = nError;
  lampGreen = lampForCount(nFree);
  lampOrange = lampForCount(nAttention);
  lampRed = lampForCount(nError);
  failCount = 0;
  everOk = true;
  lastHttpCode = code;
  lastOkMs = millis();
  mode = M_NORMAL;
}

static uint32_t pollIntervalMs() {
  uint32_t wanted = (uint32_t)constrain(cfg.pollSec, 2, 300) * 1000UL;
  uint8_t requests = useBulk ? 2 : printerCount + 1;
  return max(wanted, (uint32_t)requests * MIN_MS_PER_REQUEST);
}

static void resetPolling() {
  forceListRefresh = true;
  useBulk = true;
  everOk = false;
  failCount = 0;
}

// --------------------------------------------------------- LAN web page ----
static ESP8266WebServer server(80);
static ESP8266HTTPUpdateServer updater;
static bool webStarted = false;

static String htmlEscape(const char *s) {
  String out;
  for (; *s; s++) {
    switch (*s) {
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '&': out += F("&amp;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += *s;
    }
  }
  return out;
}

static const char *categoryName(Category c) {
  switch (c) {
    case C_FREE: return "free";
    case C_ATTENTION: return "needs attention";
    case C_ERROR: return "error";
    case C_BUSY: return "busy";
    default: return "no data";
  }
}

static const char *modeName() {
  switch (mode) {
    case M_SETUP_AP: return "setup mode";
    case M_WIFI: return "connecting to WiFi";
    case M_SERVER: return cfg.host[0] ? "cannot reach Bambuddy" : "Bambuddy address not set";
    case M_AUTH: return "Bambuddy rejected the API key";
    default: return "connected";
  }
}

static String pageStart(const __FlashStringHelper *title, bool refresh) {
  String h = F("<!doctype html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (refresh) h += F("<meta http-equiv='refresh' content='5'>");
  h += F("<title>BambuddyLights</title><style>"
         "body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 16px}"
         "td,th{text-align:left;padding:4px 12px 4px 0}nav a{margin-right:12px}"
         "label{display:block;margin-top:10px}input[type=text],input[type=password],input[type=number]"
         "{width:100%;box-sizing:border-box;padding:6px}button{margin-top:12px;padding:6px 14px}"
         "form.inline{display:inline;margin-right:8px}.warn{color:#b00}</style></head><body><h2>");
  h += title;
  h += F("</h2><nav><a href='/'>Status</a><a href='/settings'>Settings</a><a href='/update'>Firmware</a></nav>");
  return h;
}

static void handleStatus() {
  String h = pageStart(F("BambuddyLights"), true);
  h += F("<p>Status: <b>");
  h += modeName();
  h += F("</b><br>Server: ");
  h += htmlEscape(baseUrl().c_str());
  h += F("<br>Last HTTP code: ");
  h += lastHttpCode;
  h += useBulk ? F("<br>Polling: one request for all printers") : F("<br>Polling: one request per printer");
  if (everOk) {
    h += F("<br>Last update: ");
    h += (millis() - lastOkMs) / 1000;
    h += F(" s ago");
  }
  h += F("</p><p>Free: ");
  h += countFree;
  h += F(" &nbsp; Needs attention: ");
  h += countAttention;
  h += F(" &nbsp; Error: ");
  h += countError;
  h += F("</p>");
  if (printersSkipped) {
    h += F("<p class='warn'>");
    h += printersSkipped;
    h += F(" printers are over the limit of ");
    h += MAX_PRINTERS;
    h += F(" and are not watched.</p>");
  }
  h += F("<table><tr><th>Printer</th><th>State</th><th>Light</th></tr>");
  for (uint8_t i = 0; i < printerCount; i++) {
    h += F("<tr><td>");
    h += htmlEscape(printers[i].name);
    h += F("</td><td>");
    if (printers[i].cat == C_UNKNOWN) h += F("-");
    else h += printers[i].connected ? htmlEscape(printers[i].state) : String(F("offline"));
    h += F("</td><td>");
    h += categoryName(printers[i].cat);
    h += F("</td></tr>");
  }
  h += F("</table><form method='post' action='/lamptest'><button>Lamp test</button></form></body></html>");
  server.send(200, "text/html", h);
}

static void redirectTo(const char *path) {
  server.sendHeader("Location", path);
  server.send(303, "text/plain", "");
}

static void handleLampTest() {
  startLampTest(3000);
  redirectTo("/");
}

// Settings, WiFi change and firmware update need the device password.
static bool requireAdmin() {
  if (!cfg.adminPass[0]) {
    String h = pageStart(F("Settings locked"), false);
    h += F("<p>No device password is set, so settings and firmware updates are turned off.</p>"
           "<p>Hold the button for 5 seconds to factory reset, then set a device password on the setup page.</p>"
           "</body></html>");
    server.send(403, "text/html", h);
    return false;
  }
  if (!server.authenticate(ADMIN_USER, cfg.adminPass)) {
    server.requestAuthentication(DIGEST_AUTH, "BambuddyLights");
    return false;
  }
  return true;
}

static void handleSettingsGet() {
  if (!requireAdmin()) return;
  String h = pageStart(F("Settings"), false);
  h += F("<form method='post' action='/settings'>"
         "<label>Bambuddy IP or hostname<input type='text' name='host' maxlength='63' value='");
  h += htmlEscape(cfg.host);
  h += F("'></label><label>Bambuddy port<input type='number' name='port' min='1' max='65535' value='");
  h += cfg.port;
  h += F("'></label><label>Bambuddy API key ");
  h += cfg.apiKey[0] ? F("(set; leave blank to keep)") : F("(not set)");
  h += F("<input type='password' name='key' maxlength='128' autocomplete='off'></label>"
         "<label><input type='checkbox' name='clearkey'> Remove the saved API key</label>"
         "<label>Check every (seconds)<input type='number' name='poll' min='2' max='300' value='");
  h += cfg.pollSec;
  h += F("'></label><label><input type='checkbox' name='offline'");
  if (cfg.offlineIsAttention) h += F(" checked");
  h += F("> Offline printer shows orange</label><label><input type='checkbox' name='heartbeat'");
  if (cfg.heartbeat) h += F(" checked");
  h += F("> Green blip when every printer is busy (turn off for relay boards)</label>"
         "<label>New device password (leave blank to keep)"
         "<input type='password' name='adminpass' maxlength='64' autocomplete='new-password'></label>"
         "<button>Save</button></form><hr>"
         "<form class='inline' method='post' action='/wifi'><button>Change WiFi</button></form>"
         "<form class='inline' method='post' action='/restart'><button>Restart</button></form>"
         "<p>Change WiFi restarts into the setup access point.</p></body></html>");
  server.send(200, "text/html", h);
}

static void handleSettingsPost() {
  if (!requireAdmin()) return;
  setHost(server.arg("host"));
  cfg.port = parsePort(server.arg("port"));
  if (server.hasArg("clearkey")) cfg.apiKey[0] = 0;
  else if (server.arg("key").length()) strlcpy(cfg.apiKey, server.arg("key").c_str(), sizeof(cfg.apiKey));
  cfg.pollSec = parsePoll(server.arg("poll"));
  cfg.offlineIsAttention = server.hasArg("offline");
  cfg.heartbeat = server.hasArg("heartbeat");
  if (server.arg("adminpass").length()) {
    strlcpy(cfg.adminPass, server.arg("adminpass").c_str(), sizeof(cfg.adminPass));
    updater.updateCredentials(ADMIN_USER, cfg.adminPass);
  }
  saveConfig();
  resetPolling();
  redirectTo("/");
}

static void handleWifiChange() {
  if (!requireAdmin()) return;
  File f = LittleFS.open(WIFI_SETUP_FLAG, "w");
  if (f) f.close();
  server.send(200, "text/plain", "Restarting into setup mode. Join the BambuLight WiFi network.");
  delay(500);
  ESP.restart();
}

static void handleRestart() {
  if (!requireAdmin()) return;
  server.send(200, "text/plain", "Restarting.");
  delay(500);
  ESP.restart();
}

static void handleUpdateLocked() { requireAdmin(); }

static void startWebServer() {
  server.on("/", HTTP_GET, handleStatus);
  server.on("/lamptest", HTTP_POST, handleLampTest);
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/wifi", HTTP_POST, handleWifiChange);
  server.on("/restart", HTTP_POST, handleRestart);
  // The update server turns authentication off when the password is empty,
  // so it is only installed when a device password exists.
  if (cfg.adminPass[0]) updater.setup(&server, "/update", ADMIN_USER, cfg.adminPass);
  else server.on("/update", handleUpdateLocked);
  server.begin();
  webStarted = true;
}

// ------------------------------------------------------- setup portal ----
static WiFiManager wm;
static WiFiManagerParameter *pHost, *pPort, *pKey, *pPoll, *pOffline, *pHeartbeat, *pAdmin;
static bool portalMode = false;  // true while the setup access point is up

// Saved from the setup page. Secrets are never pre-filled there (the access
// point is open), so a blank key or password keeps the saved one.
static void readPortalParams() {
  setHost(pHost->getValue());
  cfg.port = parsePort(pPort->getValue());
  if (pKey->getValue()[0]) strlcpy(cfg.apiKey, pKey->getValue(), sizeof(cfg.apiKey));
  if (pAdmin->getValue()[0]) strlcpy(cfg.adminPass, pAdmin->getValue(), sizeof(cfg.adminPass));
  cfg.pollSec = parsePoll(pPoll->getValue());
  cfg.offlineIsAttention = atoi(pOffline->getValue()) != 0;
  cfg.heartbeat = atoi(pHeartbeat->getValue()) != 0;
  saveConfig();
}

static void addPortalParams() {
  static char portStr[6], pollStr[6];
  snprintf(portStr, sizeof(portStr), "%u", cfg.port);
  snprintf(pollStr, sizeof(pollStr), "%u", cfg.pollSec);
  const char *keyLabel = cfg.apiKey[0] ? "Bambuddy API key (saved; leave blank to keep)"
                                       : "Bambuddy API key (blank if login is off)";
  const char *adminLabel = cfg.adminPass[0] ? "Device password (saved; leave blank to keep)"
                                            : "Device password, user 'admin' (needed to change settings later)";
  pHost = new WiFiManagerParameter("host", "Bambuddy IP or hostname", cfg.host, 63);
  pPort = new WiFiManagerParameter("port", "Bambuddy port", portStr, 5, "type='number' min='1' max='65535'");
  pKey = new WiFiManagerParameter("key", keyLabel, "", 128, "type='password' autocomplete='off'");
  pPoll = new WiFiManagerParameter("poll", "Check every (seconds)", pollStr, 3, "type='number' min='2' max='300'");
  pOffline = new WiFiManagerParameter("offline", "Offline printer shows orange (1 = yes, 0 = no)",
                                      cfg.offlineIsAttention ? "1" : "0", 1, "type='number' min='0' max='1'");
  pHeartbeat = new WiFiManagerParameter("heartbeat", "Green blip when all busy (1 = yes, 0 = no; 0 for relay boards)",
                                        cfg.heartbeat ? "1" : "0", 1, "type='number' min='0' max='1'");
  pAdmin = new WiFiManagerParameter("adminpass", adminLabel, "", 64, "type='password' autocomplete='new-password'");
  wm.addParameter(pHost);
  wm.addParameter(pPort);
  wm.addParameter(pKey);
  wm.addParameter(pPoll);
  wm.addParameter(pOffline);
  wm.addParameter(pHeartbeat);
  wm.addParameter(pAdmin);
}

// --------------------------------------------------------------- button ----
static void checkButton() {
  static uint32_t pressedAt = 0;
  if (digitalRead(PIN_BUTTON) == HIGH) {
    pressedAt = 0;
    return;
  }
  if (!pressedAt) pressedAt = millis() | 1;
  if (millis() - pressedAt < FACTORY_RESET_HOLD_MS) return;

  // Held for 5 s: wipe WiFi and Bambuddy settings, then start over.
  startLampTest(5000);
  wm.resetSettings();
  LittleFS.remove(CONFIG_FILE);
  LittleFS.remove(WIFI_SETUP_FLAG);
  delay(1000);
  ESP.restart();
}

static void serviceIO() {
  if (webStarted) server.handleClient();
  checkButton();
  MDNS.update();
  yield();
}

// ---------------------------------------------------------------- setup ----
void setup() {
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_ORANGE, OUTPUT);
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  writeLamps(false, false, false);

  Serial.begin(115200);
  Serial.println(F("\nBambuddyLights starting"));

  startLampTest(1000);
  lightTicker.attach_ms(50, lightTick);
  delay(1000);

  LittleFS.begin();
  loadConfig();
  addPortalParams();

  static std::vector<const char *> menu = {"wifi", "sep", "restart"};
  wm.setMenu(menu);
  wm.setTitle("BambuddyLights");
  wm.setHostname(HOSTNAME);
  wm.setDebugOutput(false);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setConnectTimeout(30);
  wm.setSaveParamsCallback(readPortalParams);
  wm.setAPCallback([](WiFiManager *) { mode = M_SETUP_AP; });

  char apName[20];
  snprintf(apName, sizeof(apName), "BambuLight-%04X", (unsigned)(ESP.getChipId() & 0xFFFF));

  bool connected = false;
  mode = M_WIFI;
  if (LittleFS.exists(WIFI_SETUP_FLAG)) {
    // "Change WiFi" from the settings page: go straight to the setup portal.
    LittleFS.remove(WIFI_SETUP_FLAG);
    wm.startConfigPortal(apName);
  } else {
    connected = wm.autoConnect(apName);
  }

  if (connected) {
    Serial.print(F("WiFi connected, IP "));
    Serial.println(WiFi.localIP());
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    startWebServer();
    mode = M_SERVER;
  } else {
    // No saved WiFi, or it could not connect: the setup access point is up.
    portalMode = true;
    mode = M_SETUP_AP;
    Serial.print(F("Setup access point: "));
    Serial.println(apName);
  }
}

// ----------------------------------------------------------------- loop ----
void loop() {
  if (portalMode) {
    wm.process();
    checkButton();
    // Portal closed: either new settings were saved or it timed out.
    // Restart either way; a timeout simply retries the saved WiFi.
    if (!wm.getConfigPortalActive()) {
      delay(500);
      ESP.restart();
    }
    return;
  }

  serviceIO();

  static uint32_t wifiLostAt = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiLostAt) wifiLostAt = millis() | 1;
    mode = M_WIFI;
    // Still down after a minute: restart, which retries and then offers setup.
    if (millis() - wifiLostAt > WIFI_LOST_RESTART_MS) ESP.restart();
    return;
  }
  if (wifiLostAt) {
    wifiLostAt = 0;
    mode = M_SERVER;
    forceListRefresh = true;
  }

  static uint32_t lastPoll = 0;
  static bool firstPoll = true;
  if (firstPoll || millis() - lastPoll >= pollIntervalMs()) {
    firstPoll = false;
    pollRound();
    lastPoll = millis();
  }
}
