#include "uptick.h"
#include "pins.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

NetState g_net;

static WebServer   http(80);
static DNSServer   dns;
static Preferences prefs;

static const uint32_t JOIN_TIMEOUT_MS = 20000;

// ---------------------------------------------------------------- helpers

static void sendJson(int code, const String& body)
{
  http.sendHeader("Cache-Control", "no-store");
  http.send(code, "application/json", body);
}

static void sendOk()
{
  sendJson(200, "{\"ok\":true}");
}

static void sendErr(int code, const String& msg)
{
  JsonDocument d;
  d["ok"]    = false;
  d["error"] = msg;
  String out;
  serializeJson(d, out);
  sendJson(code, out);
}

// ---------------------------------------------------------------- handlers

static void handleStatus()
{
  JsonDocument d;
  d["name"]        = UPTICK_NAME;
  d["version"]     = UPTICK_VERSION;
  d["mode"]        = g_net.apMode ? "ap" : "sta";
  d["provisioned"] = g_net.provisioned;
  d["ssid"]        = g_net.ssid;
  d["ip"]          = g_net.ip;
  d["rssi"]        = g_net.apMode ? 0 : WiFi.RSSI();
  d["heapFree"]    = ESP.getFreeHeap();
  d["psramFree"]   = ESP.getFreePsram();
  d["uptimeSec"]   = (uint32_t)(millis() / 1000);
  d["refreshHz"]   = displayRefreshHz();
  d["hasApiKey"]   = marketHasApiKey();
  d["fetchPaused"] = marketPaused();
  d["wifiSleep"]   = WiFi.getSleep();

  // Where things actually run, rather than where they are meant to.
  const DisplayStats ds = displayGetStats();
  d["bitDepth"]    = ds.depth;
  d["coreDisplay"] = ds.taskCore;
  d["coreNet"]     = xPortGetCoreID();

  // Least free stack each task has ever had, in bytes. Worth watching after
  // anything that grows what the tasks hold - the feed limit above all.
  d["stackNet"]     = uxTaskGetStackHighWaterMark(nullptr);
  d["stackDisplay"] = ds.stackFree;
  d["tickerDrawUs"] = ds.tickerUs;
  d["maxFeeds"]     = UPTICK_MAX_FEEDS;

  String out;
  serializeJson(d, out);
  sendJson(200, out);
}

static void handleGetConfig()
{
  sendJson(200, configToJson());
}

static void handlePostConfig()
{
  if (!http.hasArg("plain")) { sendErr(400, "empty body"); return; }

  String err;
  if (!configFromJson(http.arg("plain"), err)) { sendErr(400, err); return; }

  // Power save was only ever applied on joining, so saving a change to it did
  // nothing until the next restart. Everything else in the config is picked up
  // live by the tasks that read it; this is the one setting nothing polls.
  WiFi.setSleep(g_cfg.wifiSleep);

  Serial.println("[net] config updated");
  sendOk();
}

// Separate from the config POST so the web UI can drag a brightness slider
// without round-tripping the whole document on every pixel of movement.
static void handleBrightness()
{
  if (!http.hasArg("plain")) { sendErr(400, "empty body"); return; }

  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { sendErr(400, "bad json"); return; }
  if (d["brightness"].isNull())              { sendErr(400, "missing brightness"); return; }

  uint8_t b = constrain((int)d["brightness"], 1, 100);
  {
    CfgLock lk;
    g_cfg.brightness = b;
    g_cfgRev++;
  }
  sendOk();   // deliberately not persisted; the config POST is what saves
}

// Live ticker speed, for the slider - the same arrangement as brightness:
// applied at once so the crawl can be judged while dragging, and persisted
// only by the config POST.
static void handleSpeed()
{
  if (!http.hasArg("plain")) { sendErr(400, "empty body"); return; }

  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { sendErr(400, "bad json"); return; }
  if (d["speed"].isNull())                   { sendErr(400, "missing speed"); return; }

  const uint8_t v = constrain((int)d["speed"], SCROLL_PPS_MIN, SCROLL_PPS_MAX);
  {
    CfgLock lk;
    g_cfg.scrollSpeed = v;
    g_cfgRev++;
  }
  sendOk();
}

// Live vertical-ticker pause, for its slider - as speed and brightness.
static void handlePause()
{
  if (!http.hasArg("plain")) { sendErr(400, "empty body"); return; }

  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { sendErr(400, "bad json"); return; }
  if (d["pause"].isNull())                   { sendErr(400, "missing pause"); return; }

  const uint8_t v = constrain((int)d["pause"], 0, PAUSE_SEC_MAX);
  {
    CfgLock lk;
    g_cfg.pauseSec = v;
    g_cfgRev++;
  }
  sendOk();
}

// Diagnostic: what is actually on LittleFS. An empty list with a plausible
// totalBytes means the image never landed; totalBytes of 0 means the partition
// itself is wrong.
static void listDir(const String& path, JsonArray arr, uint8_t depth)
{
  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) return;

  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) {
      if (depth < 4) listDir(f.path(), arr, depth + 1);
    }
    else {
      JsonObject o = arr.add<JsonObject>();
      o["path"] = f.path();
      o["size"] = (uint32_t)f.size();
    }
  }
}

static void handleFiles()
{
  JsonDocument d;
  d["totalBytes"] = LittleFS.totalBytes();
  d["usedBytes"]  = LittleFS.usedBytes();
  listDir("/", d["files"].to<JsonArray>(), 0);

  String out;
  serializeJson(d, out);
  sendJson(200, out);
}

// Live mode switch, deliberately not persisted - same reasoning as brightness.
// Flipping between the test screens and the real one while tuning colours
// should not write flash on every click, and a reset should land you back on
// whatever mode you actually saved.
// Live, like the mode: applies to the panel at once, and Save keeps it.
static void handleTheme()
{
  if (!http.hasArg("plain")) { sendErr(400, "empty body"); return; }

  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { sendErr(400, "bad json"); return; }

  const String t = d["theme"] | "";
  if (t != "none" && t != "snow") { sendErr(400, "unknown theme"); return; }

  {
    CfgLock lk;
    g_cfg.theme = t;
    g_cfgRev++;
  }
  sendOk();
}

// Santa flies every few minutes on his own; this is for not having to wait.
static void handleSanta()
{
  displaySantaNow();
  sendOk();
}

static void handleMode()
{
  if (!http.hasArg("plain")) { sendErr(400, "empty body"); return; }

  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { sendErr(400, "bad json"); return; }

  String m = d["mode"] | "";
  if (m == "multi_price") m = "ticker_vertical";   // the old name, as in config.cpp
  if (m != "single_chart" && m != "ticker" && m != "ticker_vertical" &&
      m != "palette_test" && m != "ramp_test")
  {
    sendErr(400, "unknown mode");
    return;
  }

  {
    CfgLock lk;
    g_cfg.mode = m;
    g_cfgRev++;
  }
  sendOk();
}

// Factory reset of the settings document only - WiFi credentials live in NVS
// and are deliberately left alone, so this cannot strand the device off the
// network.
static void handleDefaults()
{
  {
    CfgLock lk;
    configDefaults(g_cfg);
    g_cfgRev++;
  }
  displayPaletteReset();           // defaults mean factory colours as well

  if (!configSave()) { sendErr(500, "could not write defaults"); return; }

  Serial.println("[net] settings restored to defaults");
  sendOk();
}

// Write-only: the key goes in and never comes back out, /api/status reports
// only whether one is set.
static void handleApiKey()
{
  if (!http.hasArg("plain")) { sendErr(400, "empty body"); return; }

  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { sendErr(400, "bad json"); return; }

  marketSetApiKey(d["key"] | "");
  sendOk();
}

// Ticker validation on the page's behalf. Only yahoo feeds come through here:
// the three exchange APIs all send CORS headers, so the browser checks those
// itself and the device never sees them.
static void handleCheck()
{
  if (g_net.apMode || WiFi.status() != WL_CONNECTED)
  {
    sendErr(503, "not on a network yet");
    return;
  }

  // ?symbols=A,B,C - existence only, for the check the page runs before it
  // saves a feed list. Answers which were found and which were not, so the
  // page can point at the bad one rather than refusing the whole list.
  if (http.hasArg("symbols"))
  {
    String  syms[UPTICK_MAX_FEEDS];
    uint8_t n   = 0;
    String  all = http.arg("symbols");

    int from = 0;
    while (from <= (int)all.length() && n < UPTICK_MAX_FEEDS)
    {
      int comma = all.indexOf(',', from);
      if (comma < 0) comma = all.length();
      String s = all.substring(from, comma);
      s.trim();
      if (s.length()) syms[n++] = s;
      from = comma + 1;
    }
    if (!n) { sendErr(400, "no symbols"); return; }

    bool found[UPTICK_MAX_FEEDS];
    if (!marketCheckYahoo(syms, n, found))
    {
      // Distinct from "not found": the page saves anyway with a warning,
      // because Yahoo being unreachable says nothing about the symbols.
      sendErr(502, "could not reach Yahoo");
      return;
    }

    JsonDocument d;
    d["ok"] = true;
    JsonArray yes = d["found"].to<JsonArray>();
    JsonArray no  = d["missing"].to<JsonArray>();
    for (uint8_t k = 0; k < n; k++) (found[k] ? yes : no).add(syms[k]);

    String out;
    serializeJson(d, out);
    sendJson(200, out);
    return;
  }

  // ?symbol=X - the "check" button: price, name and currency for one ticker.
  const String symbol = http.arg("symbol");
  if (symbol.isEmpty()) { sendErr(400, "no symbol"); return; }

  float  price = 0;
  String name, currency;
  if (!marketProbeYahoo(symbol, price, name, currency))
  {
    sendErr(404, "no such symbol");
    return;
  }

  JsonDocument d;
  d["ok"]       = true;
  d["price"]    = price;
  d["name"]     = name;
  d["currency"] = currency;

  String out;
  serializeJson(d, out);
  sendJson(200, out);
}

// Pauses or resumes all market fetching - for troubleshooting, or to keep the
// device off the network for a while. Not persisted; a restart resumes it.
static void handleFetch()
{
  if (!http.hasArg("plain")) { sendErr(400, "empty body"); return; }

  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { sendErr(400, "bad json"); return; }

  marketSetPaused(d["paused"] | false);
  sendOk();
}

static void handleWifiSleep()
{
  if (!http.hasArg("plain")) { sendErr(400, "empty body"); return; }

  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { sendErr(400, "bad json"); return; }

  const bool sleep = d["sleep"] | true;
  WiFi.setSleep(sleep);
  Serial.printf("[net] wifi power save %s\n", sleep ? "on" : "off");
  sendOk();
}

// Live palette tuning. GET returns every entry as hex, POST takes any subset
// by name. Not persisted: this is for finding a good set against the real
// panel, after which the values get baked in as factory defaults.
static void handlePaletteGet()
{
  JsonDocument d;
  for (uint8_t i = 0; i < displayPaletteCount(); i++)
  {
    uint8_t r, g, b;
    displayPaletteGet(i, r, g, b);

    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);
    d[displayPaletteName(i)] = hex;
  }

  String out;
  serializeJson(d, out);
  sendJson(200, out);
}

static void handlePalettePost()
{
  if (!http.hasArg("plain")) { sendErr(400, "empty body"); return; }

  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { sendErr(400, "bad json"); return; }

  if (d["reset"] | false) { displayPaletteReset(); sendOk(); return; }

  uint8_t applied = 0;
  for (JsonPairConst kv : d.as<JsonObjectConst>())
  {
    const char* hex = kv.value().as<const char*>();
    if (!hex || *hex != '#' || strlen(hex) != 7) continue;

    const long v = strtol(hex + 1, nullptr, 16);
    if (displayPaletteSet(kv.key().c_str(),
                          (uint8_t)((v >> 16) & 0xFF),
                          (uint8_t)((v >> 8)  & 0xFF),
                          (uint8_t)(v & 0xFF)))
      applied++;
  }

  if (!applied) { sendErr(400, "no recognised colour names"); return; }
  sendOk();
}

static void handleScan()
{
  int n = WiFi.scanNetworks();

  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  for (int i = 0; i < n && i < 30; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();

  String out;
  serializeJson(d, out);
  sendJson(200, out);
}

static void handleWifi()
{
  if (!http.hasArg("plain")) { sendErr(400, "empty body"); return; }

  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { sendErr(400, "bad json"); return; }

  String ssid = d["ssid"] | "";
  String pass = d["pass"] | "";
  if (ssid.isEmpty()) { sendErr(400, "ssid required"); return; }

  prefs.begin("uptick", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  Serial.printf("[net] credentials stored for %s, restarting\n", ssid.c_str());
  sendOk();

  // Let the response actually leave the socket before we pull the rug.
  delay(400);
  ESP.restart();
}

static void handleForget()
{
  prefs.begin("uptick", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();

  sendOk();
  delay(400);
  ESP.restart();
}

// ---------------------------------------------------------------- OTA

static void handleUpdateDone()
{
  http.sendHeader("Connection", "close");
  sendJson(Update.hasError() ? 500 : 200,
           Update.hasError() ? "{\"ok\":false}" : "{\"ok\":true}");
  delay(400);
  ESP.restart();
}

static void handleUpdateUpload()
{
  HTTPUpload& up = http.upload();

  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[ota] start: %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  }
  else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  }
  else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("[ota] ok, %u bytes\n", (unsigned)up.totalSize);
    else                  Update.printError(Serial);
  }
}

// ---------------------------------------------------------------- portal

// The web UI is read off LittleFS once at boot and served from RAM thereafter.
// Reading from LittleFS goes through spi_flash_read, which disables the
// instruction cache on both cores for the duration and stalls anything not in
// IRAM, so a page load served from flash would hold up both tasks each time.
//
// The buffer is forced into internal RAM. Plain malloc() hands out PSRAM for
// an allocation this size on a board with BOARD_HAS_PSRAM, and while PSRAM
// reads go through the cache rather than disabling it, PSRAM shares the octal
// SPI bus with flash on this part. Internal RAM touches neither, and about
// 43 KB out of ~247 KB free is cheap for a buffer read on every page load.
static char*  s_indexBuf = nullptr;
static size_t s_indexLen = 0;

static bool preloadIndex()
{
  File f = LittleFS.open("/www/index.html", "r");
  if (!f) { Serial.println("[web] index.html missing"); return false; }

  s_indexLen = f.size();
  s_indexBuf = (char*)heap_caps_malloc(s_indexLen + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_indexBuf) s_indexBuf = (char*)malloc(s_indexLen + 1);   // PSRAM beats flash
  if (!s_indexBuf)
  {
    Serial.printf("[web] cannot allocate %u bytes for index\n", (unsigned)s_indexLen);
    s_indexLen = 0;
    f.close();
    return false;
  }

  f.readBytes(s_indexBuf, s_indexLen);
  s_indexBuf[s_indexLen] = 0;
  f.close();

  Serial.printf("[web] index cached in RAM, %u bytes\n", (unsigned)s_indexLen);
  return true;
}

static void handleNotFound();

static void handleRoot()
{
  if (!s_indexBuf) { handleNotFound(); return; }
  http.sendHeader("Cache-Control", "no-store");
  http.send(200, "text/html", s_indexBuf);
}

static void handleNotFound()
{
  const String uri = http.uri();
  const bool   root = (uri == "/" || uri == "/index.html");

  // In setup mode every unknown host resolves here via the wildcard DNS, so a
  // phone connectivity check gets a redirect and pops the config page. Never
  // redirect the root to itself though - when the web UI is missing from the
  // filesystem that turns a fixable error into an infinite redirect.
  if (g_net.apMode && !root) {
    http.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    http.send(302, "text/plain", "");
    return;
  }

  if (root) {
    http.send(200, "text/html",
      F("<!doctype html><meta charset=\"utf-8\"><title>uptick</title>"
        "<body style=\"font:14px ui-monospace,monospace;background:#0d1117;color:#e6edf3;padding:24px\">"
        "<h2>uptick</h2>"
        "<p>Firmware is running, but the web UI is not on the filesystem.</p>"
        "<p>Flash it with <code>pio run -t uploadfs</code>.</p>"
        "<p><a style=\"color:#2f81f7\" href=\"/api/files\">/api/files</a> lists what is actually there.</p>"
        "</body>"));
    return;
  }

  http.send(404, "text/plain", "not found");
}

static String apName()
{
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[24];
  snprintf(buf, sizeof(buf), "Uptick-%02X%02X", mac[4], mac[5]);
  return String(buf);
}

static void startAP()
{
  g_net.apMode      = true;
  g_net.provisioned = false;
  g_net.ssid        = apName();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(g_net.ssid.c_str());
  g_net.ip = WiFi.softAPIP().toString();

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());

  Serial.printf("[net] setup AP %s at %s\n", g_net.ssid.c_str(), g_net.ip.c_str());
}

static bool joinSTA(const String& ssid, const String& pass)
{
  g_net.apMode = false;
  g_net.ssid   = ssid;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(UPTICK_NAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());

  Serial.printf("[net] joining %s", ssid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < JOIN_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[net] join failed");
    return false;
  }

  g_net.provisioned = true;
  g_net.ip          = WiFi.localIP().toString();

  {
    CfgLock lk;
    WiFi.setSleep(g_cfg.wifiSleep);
  }
  Serial.printf("[net] joined, ip %s rssi %d\n", g_net.ip.c_str(), WiFi.RSSI());

  // UTC only - nothing here shows a local time. The clock is for comparing
  // against the trading sessions Yahoo reports, which are Unix times too.
  // SNTP runs in the background and keeps it synced from here on.
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  if (MDNS.begin(UPTICK_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[net] mdns: http://%s.local/\n", UPTICK_NAME);
  }
  return true;
}

// ---------------------------------------------------------------- lifecycle

void netBegin()
{
  prefs.begin("uptick", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid.isEmpty() || !joinSTA(ssid, pass)) startAP();

  http.on("/api/status",     HTTP_GET,  handleStatus);
  http.on("/api/config",     HTTP_GET,  handleGetConfig);
  http.on("/api/config",     HTTP_POST, handlePostConfig);
  http.on("/api/brightness", HTTP_POST, handleBrightness);
  http.on("/api/speed",      HTTP_POST, handleSpeed);
  http.on("/api/pause",      HTTP_POST, handlePause);
  http.on("/api/mode",       HTTP_POST, handleMode);
  http.on("/api/theme",      HTTP_POST, handleTheme);
  http.on("/api/santa",      HTTP_POST, handleSanta);
  http.on("/api/defaults",   HTTP_POST, handleDefaults);
  http.on("/api/apikey",     HTTP_POST, handleApiKey);
  http.on("/api/fetch",      HTTP_POST, handleFetch);
  http.on("/api/check",      HTTP_GET,  handleCheck);
  http.on("/api/palette",    HTTP_GET,  handlePaletteGet);
  http.on("/api/palette",    HTTP_POST, handlePalettePost);
  http.on("/api/wifisleep",  HTTP_POST, handleWifiSleep);
  http.on("/api/files",      HTTP_GET,  handleFiles);
  http.on("/api/scan",       HTTP_GET,  handleScan);
  http.on("/api/wifi",       HTTP_POST, handleWifi);
  http.on("/api/forget",     HTTP_POST, handleForget);
  http.on("/api/update",     HTTP_POST, handleUpdateDone, handleUpdateUpload);

  preloadIndex();
  http.on("/",           HTTP_GET, handleRoot);
  http.on("/index.html", HTTP_GET, handleRoot);
  http.onNotFound(handleNotFound);

  http.begin();
  Serial.printf("[net] http up on %s\n", g_net.ip.c_str());
}

void netTask(void* arg)
{
  (void)arg;

  uint32_t lastCheck = 0;
  uint8_t  dropCount = 0;

  for (;;) {
    if (g_net.apMode) dns.processNextRequest();
    http.handleClient();
    marketPoll();

    // The matrix runs off DMA on the other core, so blocking here costs no
    // frames - but a silently dead link would leave the panel showing a stale
    // price forever, which is worse than showing nothing.
    if (millis() - lastCheck > 10000) {
      lastCheck = millis();
      if (!g_net.apMode) {
        if (WiFi.status() == WL_CONNECTED) {
          dropCount = 0;
          g_net.ip  = WiFi.localIP().toString();
        }
        else if (++dropCount >= 18) {   // roughly 3 minutes down
          Serial.println("[net] link down too long, restarting");
          ESP.restart();
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
