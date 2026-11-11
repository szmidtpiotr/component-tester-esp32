// Component tester bridge: ESP32-C3 <-> AVR component tester (ComponentTester 1.57m).
// The tester runs with the VT100 pseudo display: its whole UI (results, menus, tools,
// self-adjustment) arrives as VT100 text at 9600 8N1 on PD6. We keep a 40x24 virtual
// screen here and expose it over HTTP. A measurement is triggered by resetting the
// tester (ISP /RESET) or by pressing its test button (PD7), both through a diode so
// the ESP only ever pulls those 5 V lines low.
//
// HTTP API (port 80, mDNS tester.local):
//   GET  /              tiny UI with a live screen
//   GET  /status        uptime, Wi-Fi, screen counters
//   GET  /screen        current screen: {rows:[...], version, stable_ms}
//   GET  /history       last stable screens (newest last)
//   POST /measure       ?mode=reset|button|next&timeout=ms -> parsed result (JSON)
//   GET  /last          last parsed result
//   POST /button        ?ms=150 short, ?ms=600 long, ?count=2&gap=150 double (menu)
//   POST /reset         reset the tester only
//   GET  /results       stored results (?limit=50), survive restarts (LittleFS)
//   GET  /result?id=N   one stored result;  POST /results/clear  wipe history
//   GET  /wifi/scan     visible networks;  POST /wifi (ssid, pass) save + restart
//   POST /update        firmware.bin over Wi-Fi (multipart field "firmware")
//   GET  /openapi.json  API description;   GET /docs  Swagger UI
// No credentials are compiled in: Wi-Fi comes from NVS and is set from a phone. On first boot
// (or without a reachable network) the device opens an open AP "Tester-Setup" with a captive
// portal at 192.168.4.1.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <Update.h>
#include <LittleFS.h>
#include <time.h>
#include "assets.h"

#define FW_VERSION   "1.0.0"
#define PIN_RX       4     // tester PD6 (via 10k/20k divider)
#define PIN_BUTTON   5     // tester PD7 (via 1N4148, cathode to ESP)
#define PIN_RESET    6     // tester /RESET on ISP header (via 1N4148, cathode to ESP)
#define HOSTNAME     "tester"
#define COLS 40
#define ROWS 24
#define STABLE_MS 600      // screen unchanged this long = finished drawing

WebServer server(80);
DNSServer dns;
Preferences prefs;
static String wifiSsid, wifiPass;
static bool apMode = false;
static uint32_t staUpAt = 0, downSince = 0;
#define AP_SSID "Tester-Setup"

// ---- VT100 virtual screen -------------------------------------------------
static char scr[ROWS][COLS + 1];
static int cx = 0, cy = 0;
static uint32_t scrVersion = 0;      // bumps on every change
static uint32_t lastChange = 0;
static uint32_t clears = 0;          // number of Esc[2J seen
enum { S_TEXT, S_ESC, S_CSI } vtState = S_TEXT;
static char csi[16]; static int csiLen = 0;

static void touch() { scrVersion++; lastChange = millis(); }
static void clearScreen() { for (int r = 0; r < ROWS; r++) { memset(scr[r], ' ', COLS); scr[r][COLS] = 0; } cx = cy = 0; clears++; touch(); }
static void clearLine(int from) { for (int c = from; c < COLS; c++) scr[cy][c] = ' '; touch(); }

static void csiDone(char fin) {
  csi[csiLen] = 0;
  int a = 0, b = 0; bool second = false; bool any = false;
  for (int i = 0; i < csiLen; i++) {
    char c = csi[i];
    if (c >= '0' && c <= '9') { any = true; if (second) b = b * 10 + (c - '0'); else a = a * 10 + (c - '0'); }
    else if (c == ';') second = true;
  }
  switch (fin) {
    case 'H': case 'f': cy = constrain((any ? a : 1) - 1, 0, ROWS - 1); cx = constrain((second ? b : 1) - 1, 0, COLS - 1); break;
    case 'J': if (a == 2) clearScreen(); break;
    case 'K': clearLine(a == 2 ? 0 : cx); break;
    default: break;                  // colours (m), cursor visibility (?25l/h) ignored
  }
}

static void vtFeed(char c) {
  switch (vtState) {
    case S_TEXT:
      if (c == 0x1b) { vtState = S_ESC; return; }
      if (c == '\r') { cx = 0; return; }
      if (c == '\n') { if (cy < ROWS - 1) cy++; return; }
      if ((uint8_t)c < 0x20) return;
      if (cx < COLS) { scr[cy][cx++] = c; touch(); }
      return;
    case S_ESC:
      if (c == '[') { vtState = S_CSI; csiLen = 0; } else vtState = S_TEXT;
      return;
    case S_CSI:
      if ((c >= '0' && c <= '9') || c == ';' || c == '?') { if (csiLen < 15) csi[csiLen++] = c; return; }
      csiDone(c); vtState = S_TEXT; return;
  }
}

static void pumpSerial() { while (Serial1.available()) vtFeed((char)Serial1.read()); }

static void screenRows(std::vector<String>& out) {
  out.clear();
  for (int r = 0; r < ROWS; r++) { String s(scr[r]); s.trim(); out.push_back(s); }
  while (!out.empty() && out.back().length() == 0) out.pop_back();
}

// ---- history of stable screens --------------------------------------------
static const int HIST = 20;
static String hist[HIST]; static uint32_t histAt[HIST]; static int histN = 0;
static uint32_t histVersion = 0;
static void trackStable() {
  if (scrVersion != histVersion && millis() - lastChange > STABLE_MS) {
    histVersion = scrVersion;
    std::vector<String> rows; screenRows(rows);
    String s; for (auto& r : rows) { s += r; s += '\n'; }
    if (s.length() == 0) return;
    hist[histN % HIST] = s; histAt[histN % HIST] = millis(); histN++;
  }
}

// ---- tester control -------------------------------------------------------
static void waitMs(uint32_t ms) { uint32_t t = millis(); while (millis() - t < ms) { pumpSerial(); delay(1); } }
static void pullLow(int pin, uint32_t ms) {
  digitalWrite(pin, LOW); pinMode(pin, OUTPUT);
  waitMs(ms);
  pinMode(pin, INPUT);               // release: diode blocks, board pull-up wins
}
static void resetTester()            { pullLow(PIN_RESET, 20); }
static void pressButton(uint32_t ms) { pullLow(PIN_BUTTON, ms); }

static bool isTransient(const std::vector<String>& rows) {
  if (rows.empty()) return true;
  for (auto& r : rows) {
    if (r.indexOf("Component Tester") >= 0 || r.startsWith("v1.")) return true;   // banner
    if (r.indexOf("Probing") >= 0 || r.indexOf("Testing") >= 0) return true;
  }
  return false;
}

// ---- result ---------------------------------------------------------------
static JsonDocument last;
static uint32_t lastAt = 0;          // 0 = nothing measured/stored yet

// ---- persistent results (LittleFS, one JSON object per line) --------------
#define RES_FILE "/results.jsonl"
#define RES_KEEP 200                 // compact to this many when the file grows past 1.5x
static uint32_t nextId = 1, resStored = 0;
static bool fsOk = false;

static void storeInit() {
  fsOk = LittleFS.begin(true);       // formats the "spiffs" partition on first use
  if (!fsOk) { Serial.println("LittleFS mount failed"); return; }
  File f = LittleFS.open(RES_FILE, "r");
  if (!f) return;
  String lastLine;
  while (f.available()) { String l = f.readStringUntil('\n'); if (l.length() > 2) { lastLine = l; resStored++; } }
  f.close();
  if (lastLine.length() && !deserializeJson(last, lastLine)) { nextId = (last["id"] | 0) + 1; lastAt = 1; }
}

static void storeCompact() {
  File f = LittleFS.open(RES_FILE, "r"); if (!f) return;
  std::vector<String> keep;
  uint32_t skip = resStored > RES_KEEP ? resStored - RES_KEEP : 0, i = 0;
  while (f.available()) { String l = f.readStringUntil('\n'); if (l.length() <= 2) continue; if (i++ >= skip) keep.push_back(l); }
  f.close();
  File w = LittleFS.open("/results.tmp", "w"); for (auto& l : keep) { w.print(l); w.print('\n'); } w.close();
  LittleFS.remove(RES_FILE); LittleFS.rename("/results.tmp", RES_FILE);
  resStored = keep.size();
}

static void storeAppend(JsonDocument& d) {
  d["id"] = nextId++;
  time_t now = time(nullptr);
  d["ts"] = now > 1700000000 ? (uint32_t)now : 0;    // 0 = clock not synced yet
  if (!fsOk) return;
  File f = LittleFS.open(RES_FILE, "a"); if (!f) return;
  serializeJson(d, f); f.print('\n'); f.close();
  if (++resStored > RES_KEEP * 3 / 2) storeCompact();
}

static void parseRows(const std::vector<String>& rows, JsonDocument& doc) {
  JsonArray arr = doc["lines"].to<JsonArray>();
  JsonObject vals = doc["values"].to<JsonObject>();
  bool first = true;
  for (size_t i = 0; i < rows.size(); i++) {
    const String& s = rows[i];
    arr.add(s);
    if (s.length() == 0) continue;
    if (first) { doc["type"] = s; first = false; continue; }
    int eq = s.indexOf('='), sp = s.indexOf(' ');
    if (eq > 0 && (sp < 0 || eq < sp)) vals[s.substring(0, eq)] = s.substring(eq + 1);
    else if (sp > 0) vals[s.substring(0, sp)] = s.substring(sp + 1);
    else vals[String("line") + i] = s;
  }
}

// ---- HTTP -----------------------------------------------------------------
static void sendJson(int code, JsonDocument& d) { String s; serializeJson(d, s); server.send(code, "application/json", s); }

static void hMeasure() {
  String mode = server.hasArg("mode") ? server.arg("mode") : "reset";
  uint32_t timeout = server.hasArg("timeout") ? server.arg("timeout").toInt() : 20000;
  uint32_t t0 = millis();
  uint32_t v0 = scrVersion, c0 = clears;
  if (mode == "reset") resetTester(); else if (mode == "button") pressButton(150);
  std::vector<String> rows;
  bool ok = false;
  while (millis() - t0 < timeout) {
    pumpSerial();
    // a fresh screen: drawn after the trigger (new clear), finished, and not the banner
    if (clears != c0 && scrVersion != v0 && millis() - lastChange > STABLE_MS) {
      screenRows(rows);
      if (!isTransient(rows)) { ok = true; break; }
    }
    delay(2);
  }
  JsonDocument d;
  d["ok"] = ok; d["mode"] = mode; d["ms"] = millis() - t0;
  if (!ok) {
    screenRows(rows);
    d["error"] = "no fresh result screen";
    JsonArray a = d["lines"].to<JsonArray>(); for (auto& l : rows) a.add(l);
    sendJson(504, d); return;
  }
  parseRows(rows, d);
  d.remove("ok"); d.remove("mode");                 // stored record = what was measured
  storeAppend(d);
  last = d; lastAt = millis();
  d["ok"] = true; d["mode"] = mode;
  sendJson(200, d);
}

static void hLast() {
  if (lastAt == 0) { server.send(404, "application/json", "{\"error\":\"no measurement yet\"}"); return; }
  sendJson(200, last);
}

static void hScreen() {
  std::vector<String> rows; screenRows(rows);
  JsonDocument d;
  JsonArray a = d["rows"].to<JsonArray>(); for (auto& r : rows) a.add(r);
  d["version"] = scrVersion; d["stable_ms"] = millis() - lastChange; d["clears"] = clears;
  sendJson(200, d);
}

static void hHistory() {
  JsonDocument d; JsonArray a = d["screens"].to<JsonArray>();
  int n = histN < HIST ? histN : HIST;
  for (int i = histN - n; i < histN; i++) {
    JsonObject o = a.add<JsonObject>();
    o["age_s"] = (millis() - histAt[i % HIST]) / 1000; o["text"] = hist[i % HIST];
  }
  sendJson(200, d);
}

static void hButton() {
  uint32_t ms = server.hasArg("ms") ? server.arg("ms").toInt() : 150;
  uint32_t count = server.hasArg("count") ? server.arg("count").toInt() : 1;
  uint32_t gap = server.hasArg("gap") ? server.arg("gap").toInt() : 150;
  if (ms > 5000) ms = 5000;
  if (count < 1) count = 1;
  if (count > 5) count = 5;
  for (uint32_t i = 0; i < count; i++) { if (i) waitMs(gap); pressButton(ms); }
  waitMs(STABLE_MS + 200);           // let the tester redraw, then return the screen
  hScreen();
}

static void hReset() { resetTester(); waitMs(200); hScreen(); }

static void hResults() {
  // ?limit=50 (max 200): newest last. Records: {id, ts (unix, 0 = clock unknown), ms, type, lines, values}
  int limit = server.hasArg("limit") ? server.arg("limit").toInt() : 50;
  if (limit < 1) limit = 1;
  if (limit > RES_KEEP) limit = RES_KEEP;
  std::vector<String> ring(limit); uint32_t n = 0;
  File f = fsOk ? LittleFS.open(RES_FILE, "r") : File();
  if (f) { while (f.available()) { String l = f.readStringUntil('\n'); if (l.length() > 2) ring[n++ % limit] = l; } f.close(); }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"total\":" + String(n) + ",\"results\":[");
  uint32_t first = n > (uint32_t)limit ? n - limit : 0;
  for (uint32_t i = first; i < n; i++) { if (i > first) server.sendContent(","); server.sendContent(ring[i % limit]); }
  server.sendContent("]}");
  server.sendContent("");
}

static void hResult() {
  uint32_t id = server.arg("id").toInt();
  File f = fsOk ? LittleFS.open(RES_FILE, "r") : File();
  if (f) while (f.available()) {
    String l = f.readStringUntil('\n');
    int p = l.indexOf("\"id\":"); if (p < 0) continue;
    if ((uint32_t)l.substring(p + 5).toInt() == id) { f.close(); server.send(200, "application/json", l); return; }
  }
  if (f) f.close();
  server.send(404, "application/json", "{\"error\":\"no such result\"}");
}

static void hResultsClear() {
  if (fsOk) LittleFS.remove(RES_FILE);
  resStored = 0; lastAt = 0; last.clear();
  server.send(200, "application/json", "{\"cleared\":true}");
}

static void sendGz(const char* type, const uint8_t* data, size_t len) {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, type, (const char*)data, len);
}

static const char DOCS[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><title>Component Tester - API</title>
<link rel=stylesheet href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css">
<div id=s></div><script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
<script>SwaggerUIBundle({url:'/openapi.json',dom_id:'#s'})</script>)HTML";

static void hScan() {
  int n = WiFi.scanNetworks();
  JsonDocument d; JsonArray a = d["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    String ss = WiFi.SSID(i); if (!ss.length()) continue;
    bool dup = false; for (JsonObject o : a) if (o["ssid"] == ss) { dup = true; break; }
    if (dup) continue;
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = ss; o["rssi"] = WiFi.RSSI(i); o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(200, d);
}

static void hWifiSave() {
  String ss = server.arg("ssid"), pw = server.arg("pass");
  if (!ss.length()) { server.send(400, "application/json", "{\"error\":\"ssid required\"}"); return; }
  prefs.putString("ssid", ss); prefs.putString("pass", pw);
  server.send(200, "application/json", "{\"saved\":true,\"restart_in_ms\":1000}");
  waitMs(1000); ESP.restart();
}

static void hUpdateDone() {
  bool ok = !Update.hasError();
  server.send(ok ? 200 : 500, "application/json", ok ? "{\"updated\":true}" : "{\"error\":\"update failed\"}");
  if (ok) { waitMs(500); ESP.restart(); }
}
static void hUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
  else if (up.status == UPLOAD_FILE_WRITE) Update.write(up.buf, up.currentSize);
  else if (up.status == UPLOAD_FILE_END) Update.end(true);
}

static void startAP() {
  // Stop the background STA reconnect first: an ESP32 that keeps scanning for a missing
  // network hops channels and the setup AP stops answering DHCP ("IP configuration failure").
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, nullptr, 1);                    // open, channel 1
  dns.start(53, "*", WiFi.softAPIP());
  apMode = true;
  Serial.println("AP " AP_SSID " at 192.168.4.1");
}

static void hStatus() {
  JsonDocument d;
  d["fw"] = FW_VERSION; d["uptime_s"] = millis() / 1000;
  d["mode"] = (WiFi.status() == WL_CONNECTED) ? "sta" : (apMode ? "ap" : "sta");
  d["ap_active"] = apMode;
  d["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : wifiSsid;
  d["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  d["rssi"] = WiFi.RSSI(); d["mac"] = WiFi.macAddress();
  d["screen_version"] = scrVersion; d["clears"] = clears;
  d["results_stored"] = resStored; d["fs_ok"] = fsOk;
  if (fsOk) { d["fs_used_kb"] = LittleFS.usedBytes() / 1024; d["fs_total_kb"] = LittleFS.totalBytes() / 1024; }
  time_t now = time(nullptr); d["time"] = now > 1700000000 ? (uint32_t)now : 0;
  sendJson(200, d);
}



void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT);
  pinMode(PIN_RESET, INPUT);
  for (int r = 0; r < ROWS; r++) { memset(scr[r], ' ', COLS); scr[r][COLS] = 0; }
  Serial1.begin(9600, SERIAL_8N1, PIN_RX, -1);

  storeInit();
  prefs.begin("tester", false);
  wifiSsid = prefs.getString("ssid", "");              // empty on first boot -> setup AP
  wifiPass = prefs.getString("pass", "");
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  if (wifiSsid.length()) WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t t = millis();
  while (wifiSsid.length() && WiFi.status() != WL_CONNECTED && millis() - t < 20000) { pumpSerial(); delay(50); }
  if (WiFi.status() == WL_CONNECTED) staUpAt = millis(); else startAP();
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");   // timestamps for results
  Serial.printf("WiFi %s ip=%s\n", WiFi.status() == WL_CONNECTED ? "OK" : "AP",
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "192.168.4.1");
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, [] { sendGz("text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN); });
  server.on("/openapi.json", HTTP_GET, [] { sendGz("application/json", OPENAPI_JSON_GZ, OPENAPI_JSON_GZ_LEN); });
  server.on("/docs", HTTP_GET, [] { server.send_P(200, "text/html", DOCS); });
  server.on("/results", HTTP_GET, hResults);
  server.on("/result", HTTP_GET, hResult);
  server.on("/results/clear", HTTP_POST, hResultsClear);
  server.on("/wifi/scan", HTTP_GET, hScan);
  server.on("/wifi", HTTP_POST, hWifiSave);
  server.on("/update", HTTP_POST, hUpdateDone, hUpdateUpload);
  // Captive-portal probes (Android, iOS/macOS, Windows, Firefox): any answer other than the
  // expected one makes the phone pop up the login page; we redirect straight to Wi-Fi settings.
  for (const char* u : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                        "/connecttest.txt", "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt"})
    server.on(u, HTTP_ANY, [] {
      if (apMode) { server.sendHeader("Location", "http://192.168.4.1/#settings"); server.send(302, "text/plain", ""); }
      else server.send(204);
    });
  server.onNotFound([] {                                // captive portal: everything -> UI
    if (apMode) { server.sendHeader("Location", "http://192.168.4.1/#settings"); server.send(302); }
    else server.send(404, "application/json", "{\"error\":\"not found\"}");
  });
  server.on("/status", HTTP_GET, hStatus);
  server.on("/screen", HTTP_GET, hScreen);
  server.on("/history", HTTP_GET, hHistory);
  server.on("/measure", HTTP_POST, hMeasure);
  server.on("/last", HTTP_GET, hLast);
  server.on("/button", HTTP_POST, hButton);
  server.on("/reset", HTTP_POST, hReset);
  server.begin();
}

void loop() {
  pumpSerial();
  trackStable();
  server.handleClient();
  if (apMode) dns.processNextRequest();
  if (WiFi.status() == WL_CONNECTED) {
    if (!staUpAt) staUpAt = millis();
    downSince = 0;
    if (apMode && millis() - staUpAt > 60000) {       // home Wi-Fi back for a minute: close the open AP
      dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); apMode = false;
    }
  } else {
    staUpAt = 0;
    static uint32_t lastTry = 0;
    if (!downSince) downSince = millis();
    static uint32_t tryStarted = 0;
    if (apMode) {
      // In setup mode retry the saved network rarely, only when no phone is connected,
      // and give each attempt 20 s before stopping the scan again.
      if (tryStarted && millis() - tryStarted > 20000) { WiFi.disconnect(false, false); tryStarted = 0; }
      if (!tryStarted && WiFi.softAPgetStationNum() == 0 && millis() - lastTry > 300000 && wifiSsid.length()) {
        lastTry = tryStarted = millis(); WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
      }
    } else if (millis() - lastTry > 15000 && wifiSsid.length()) {
      lastTry = millis(); WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
    if (!apMode && millis() - downSince > 60000) startAP();   // lost the network for a minute
  }
  delay(1);
}
