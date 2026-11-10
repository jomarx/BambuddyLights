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
// board; change to LOW for active-low relay modules.
#ifndef LAMP_ON_LEVEL
#define LAMP_ON_LEVEL HIGH
#endif

// ------------------------------------------------------------- settings ----
static const char *HOSTNAME = "bambulight";
static const char *CONFIG_FILE = "/config.json";
static const uint8_t MAX_PRINTERS = 16;
static const uint32_t HTTP_TIMEOUT_MS = 4000;
static const uint32_t LIST_REFRESH_MS = 60000;
static const uint32_t WIFI_LOST_RESTART_MS = 60000;
static const uint32_t FACTORY_RESET_HOLD_MS = 5000;
static const uint16_t PORTAL_TIMEOUT_S = 180;
static const uint8_t FAILS_BEFORE_SERVER_LOST = 3;
// Bambuddy allows 100 reads a minute; stay at 80.
static const uint32_t MIN_MS_PER_REQUEST = 750;

struct Config {
  char host[64] = "";
  uint16_t port = 8000;
  char apiKey[129] = "";
  uint16_t pollSec = 5;
  bool offlineIsAttention = true;
};
static Config cfg;

// --------------------------------------------------------------- lights ----
enum Mode : uint8_t { M_LAMP_TEST, M_SETUP_AP, M_WIFI, M_SERVER, M_AUTH, M_NORMAL };
enum Lamp : uint8_t { L_OFF, L_SOLID, L_BLINK };

static volatile Mode mode = M_LAMP_TEST;
static volatile Lamp lampGreen = L_OFF, lampOrange = L_OFF, lampRed = L_OFF;
static Ticker lightTicker;

static void writeLamps(bool g, bool o, bool r) {
  digitalWrite(PIN_GREEN, g ? LAMP_ON_LEVEL : !LAMP_ON_LEVEL);
  digitalWrite(PIN_ORANGE, o ? LAMP_ON_LEVEL : !LAMP_ON_LEVEL);
  digitalWrite(PIN_RED, r ? LAMP_ON_LEVEL : !LAMP_ON_LEVEL);
}

// Runs every 50 ms from a timer, so patterns keep moving during HTTP calls.
static void lightTick() {
  static uint32_t t = 0;
  t++;
  bool g = false, o = false, r = false;
  switch (mode) {
    case M_LAMP_TEST:
      g = o = r = true;
      break;
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
      if (lampGreen == L_OFF && lampOrange == L_OFF && lampRed == L_OFF) g = t % 60 == 0;
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
    cfg.pollSec = doc["pollSec"] | 5;
    cfg.offlineIsAttention = doc["offlineIsAttention"] | true;
  }
  f.close();
}

static void saveConfig() {
  JsonDocument doc;
  doc["host"] = cfg.host;
  doc["port"] = cfg.port;
  doc["apiKey"] = cfg.apiKey;
  doc["pollSec"] = cfg.pollSec;
  doc["offlineIsAttention"] = cfg.offlineIsAttention;
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

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
static uint8_t countFree = 0, countAttention = 0, countError = 0;
static uint32_t lastListMs = 0;
static bool haveList = false;
static bool forceListRefresh = true;
static uint8_t failCount = 0;
static bool everOk = false;
static int lastHttpCode = 0;
static uint32_t lastOkMs = 0;

static String baseUrl() {
  String host = cfg.host;
  host.trim();
  if (host.startsWith("http://")) host.remove(0, 7);
  while (host.endsWith("/")) host.remove(host.length() - 1);
  return "http://" + host + ":" + String(cfg.port) + "/api/v1";
}

// Opens a GET request. HTTP/1.0 keeps the body un-chunked so ArduinoJson can
// parse it straight from the socket with a filter (the status body is large).
static int httpGet(HTTPClient &http, WiFiClient &client, const String &url) {
  client.setTimeout(HTTP_TIMEOUT_MS);
  http.useHTTP10(true);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return -1;
  if (cfg.apiKey[0]) http.addHeader("X-API-Key", cfg.apiKey);
  return http.GET();
}

// GET /printers/ -> [{id, name, is_active, ...}]
static int fetchPrinterList() {
  WiFiClient client;
  HTTPClient http;
  int code = httpGet(http, client, baseUrl() + "/printers/");
  if (code == 200) {
    JsonDocument filter;
    filter[0]["id"] = true;
    filter[0]["name"] = true;
    filter[0]["is_active"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    if (err || !doc.is<JsonArray>()) {
      code = -2;
    } else {
      uint8_t n = 0;
      for (JsonObject p : doc.as<JsonArray>()) {
        if (n >= MAX_PRINTERS) break;
        if (!(p["is_active"] | true)) continue;
        printers[n].id = p["id"] | 0;
        strlcpy(printers[n].name, p["name"] | "?", sizeof(printers[n].name));
        printers[n].cat = C_UNKNOWN;
        printers[n].connected = false;
        printers[n].state[0] = 0;
        n++;
      }
      printerCount = n;
      haveList = true;
      lastListMs = millis();
      forceListRefresh = false;
    }
  }
  http.end();
  return code;
}

// GET /printers/{id}/status -> {connected, state, awaiting_plate_clear, hms_errors[{severity}]}
static int fetchPrinterStatus(Printer &p) {
  WiFiClient client;
  HTTPClient http;
  int code = httpGet(http, client, baseUrl() + "/printers/" + String(p.id) + "/status");
  if (code == 200) {
    JsonDocument filter;
    filter["connected"] = true;
    filter["state"] = true;
    filter["awaiting_plate_clear"] = true;
    filter["hms_errors"][0]["severity"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    if (err) {
      code = -2;
    } else {
      const char *state = doc["state"] | "";
      bool awaitingPlate = doc["awaiting_plate_clear"] | false;
      // Bambu HMS alert level: 1 = error (stopped), 2 = warning (paused), 3 = notification.
      bool hmsError = false, hmsWarning = false;
      for (JsonObject h : doc["hms_errors"].as<JsonArray>()) {
        int sev = h["severity"] | 0;
        if (sev == 1) hmsError = true;
        if (sev == 2) hmsWarning = true;
      }
      p.connected = doc["connected"] | false;
      strlcpy(p.state, state, sizeof(p.state));

      if (!p.connected) {
        p.cat = awaitingPlate || cfg.offlineIsAttention ? C_ATTENTION : C_BUSY;
      } else if (!strcmp(state, "FAILED") || hmsError) {
        p.cat = C_ERROR;
      } else if (!strcmp(state, "PAUSE") || hmsWarning || awaitingPlate) {
        p.cat = C_ATTENTION;
      } else if (!strcmp(state, "IDLE") || !strcmp(state, "FINISH")) {
        p.cat = C_FREE;
      } else {
        p.cat = C_BUSY;  // RUNNING, PREPARE, SLICING, ...
      }
    }
  }
  http.end();
  return code;
}

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
    mode = M_SERVER;  // nothing configured yet; set it at http://bambulight.local
    return;
  }
  if (!haveList || forceListRefresh || millis() - lastListMs > LIST_REFRESH_MS) {
    int code = fetchPrinterList();
    if (code != 200) return pollFailed(code);
  }
  uint8_t nFree = 0, nAttention = 0, nError = 0;
  for (uint8_t i = 0; i < printerCount; i++) {
    int code = fetchPrinterStatus(printers[i]);
    if (code == 404) {  // printer was removed; pick up the new list next round
      forceListRefresh = true;
      printers[i].cat = C_UNKNOWN;
      continue;
    }
    if (code != 200) return pollFailed(code);
    if (printers[i].cat == C_FREE) nFree++;
    if (printers[i].cat == C_ATTENTION) nAttention++;
    if (printers[i].cat == C_ERROR) nError++;
    yield();
  }
  countFree = nFree;
  countAttention = nAttention;
  countError = nError;
  lampGreen = lampForCount(nFree);
  lampOrange = lampForCount(nAttention);
  lampRed = lampForCount(nError);
  failCount = 0;
  everOk = true;
  lastHttpCode = 200;
  lastOkMs = millis();
  mode = M_NORMAL;
}

static uint32_t pollIntervalMs() {
  uint32_t wanted = (uint32_t)constrain(cfg.pollSec, 2, 300) * 1000UL;
  uint32_t floorMs = (uint32_t)(printerCount + 1) * MIN_MS_PER_REQUEST;
  return max(wanted, floorMs);
}

// ----------------------------------------------------------- web portal ----
static WiFiManager wm;
static WiFiManagerParameter *pHost, *pPort, *pKey, *pPoll, *pOffline;
static bool portalMode = false;  // true while the setup access point is up
static uint32_t lampTestUntil = 0;
static Mode modeBeforeTest = M_NORMAL;

static void readParams() {
  strlcpy(cfg.host, pHost->getValue(), sizeof(cfg.host));
  int port = atoi(pPort->getValue());
  cfg.port = port > 0 && port < 65536 ? port : 8000;
  strlcpy(cfg.apiKey, pKey->getValue(), sizeof(cfg.apiKey));
  int poll = atoi(pPoll->getValue());
  cfg.pollSec = constrain(poll, 2, 300);
  cfg.offlineIsAttention = atoi(pOffline->getValue()) != 0;
  saveConfig();
  forceListRefresh = true;
  everOk = false;
  failCount = 0;
}

static String htmlEscape(const char *s) {
  String out;
  for (; *s; s++) {
    if (*s == '<') out += F("&lt;");
    else if (*s == '>') out += F("&gt;");
    else if (*s == '&') out += F("&amp;");
    else out += *s;
  }
  return out;
}

static const char *categoryName(Category c) {
  switch (c) {
    case C_FREE: return "free";
    case C_ATTENTION: return "needs attention";
    case C_ERROR: return "error";
    case C_BUSY: return "busy";
    default: return "unknown";
  }
}

static const char *modeName() {
  switch (mode) {
    case M_SETUP_AP: return "setup mode";
    case M_WIFI: return "connecting to WiFi";
    case M_SERVER: return cfg.host[0] ? "cannot reach Bambuddy" : "Bambuddy address not set";
    case M_AUTH: return "Bambuddy rejected the API key";
    case M_NORMAL: return "connected";
    default: return "lamp test";
  }
}

static void handleStatus() {
  String h = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<meta http-equiv='refresh' content='5'><title>BambuddyLights</title>"
               "<style>body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 16px}"
               "td,th{text-align:left;padding:4px 12px 4px 0}a{margin-right:12px}</style></head><body>"
               "<h2>BambuddyLights</h2><p>Status: <b>");
  h += modeName();
  h += F("</b><br>Server: ");
  h += htmlEscape(baseUrl().c_str());
  h += F("<br>Last HTTP code: ");
  h += lastHttpCode;
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
  h += F("</p><table><tr><th>Printer</th><th>State</th><th>Light</th></tr>");
  for (uint8_t i = 0; i < printerCount; i++) {
    h += F("<tr><td>");
    h += htmlEscape(printers[i].name);
    h += F("</td><td>");
    h += printers[i].connected ? htmlEscape(printers[i].state) : String(F("offline"));
    h += F("</td><td>");
    h += categoryName(printers[i].cat);
    h += F("</td></tr>");
  }
  h += F("</table><p><a href='/'>Menu</a><a href='/param'>Settings</a><a href='/lamptest'>Lamp test</a></p></body></html>");
  wm.server->send(200, "text/html", h);
}

static void handleLampTest() {
  if (mode != M_LAMP_TEST) modeBeforeTest = mode;
  mode = M_LAMP_TEST;
  lampTestUntil = millis() + 3000;
  wm.server->sendHeader("Location", "/status");
  wm.server->send(302, "text/plain", "");
}

static void bindRoutes() {
  wm.server->on("/status", handleStatus);
  wm.server->on("/lamptest", handleLampTest);
}

// --------------------------------------------------------------- button ----
static void checkButton() {
  static uint32_t pressedAt = 0;
  if (digitalRead(PIN_BUTTON) == HIGH) {
    pressedAt = 0;
    return;
  }
  if (!pressedAt) pressedAt = millis();
  if (millis() - pressedAt < FACTORY_RESET_HOLD_MS) return;

  // Held for 5 s: wipe WiFi and Bambuddy settings, then start over.
  mode = M_LAMP_TEST;
  wm.resetSettings();
  LittleFS.remove(CONFIG_FILE);
  delay(1000);
  ESP.restart();
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

  lightTicker.attach_ms(50, lightTick);
  mode = M_LAMP_TEST;
  delay(1000);

  LittleFS.begin();
  loadConfig();

  static char portStr[6], pollStr[4];
  snprintf(portStr, sizeof(portStr), "%u", cfg.port);
  snprintf(pollStr, sizeof(pollStr), "%u", cfg.pollSec);
  pHost = new WiFiManagerParameter("host", "Bambuddy IP or hostname", cfg.host, 63);
  pPort = new WiFiManagerParameter("port", "Bambuddy port", portStr, 5, "type='number' min='1' max='65535'");
  pKey = new WiFiManagerParameter("key", "Bambuddy API key (blank if login is off)", cfg.apiKey, 128, "type='password'");
  pPoll = new WiFiManagerParameter("poll", "Check every (seconds)", pollStr, 3, "type='number' min='2' max='300'");
  pOffline = new WiFiManagerParameter("offline", "Offline printer shows orange (1 = yes, 0 = no)",
                                      cfg.offlineIsAttention ? "1" : "0", 1, "type='number' min='0' max='1'");
  wm.addParameter(pHost);
  wm.addParameter(pPort);
  wm.addParameter(pKey);
  wm.addParameter(pPoll);
  wm.addParameter(pOffline);

  static std::vector<const char *> menu = {"custom", "wifi", "param", "info", "update", "sep", "restart"};
  wm.setMenu(menu);
  wm.setCustomMenuHTML("<form action='/status' method='get'><button>Light status</button></form><br/>");
  wm.setTitle("BambuddyLights");
  wm.setHostname(HOSTNAME);
  wm.setDebugOutput(false);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setConnectTimeout(30);
  wm.setSaveParamsCallback(readParams);
  wm.setSaveConfigCallback(readParams);
  wm.setWebServerCallback(bindRoutes);
  wm.setAPCallback([](WiFiManager *) { mode = M_SETUP_AP; });

  char apName[20];
  snprintf(apName, sizeof(apName), "BambuLight-%04X", (unsigned)(ESP.getChipId() & 0xFFFF));

  mode = M_WIFI;
  if (wm.autoConnect(apName)) {
    Serial.print(F("WiFi connected, IP "));
    Serial.println(WiFi.localIP());
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    wm.startWebPortal();  // settings, status and firmware update on the LAN
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
  wm.process();
  checkButton();

  if (portalMode) {
    // Portal closed: either new settings were saved or it timed out.
    // Restart either way; a timeout simply retries the saved WiFi.
    if (!wm.getConfigPortalActive()) {
      delay(500);
      ESP.restart();
    }
    return;
  }

  MDNS.update();

  if (lampTestUntil) {
    if ((int32_t)(millis() - lampTestUntil) < 0) return;
    lampTestUntil = 0;
    mode = modeBeforeTest;
  }

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
