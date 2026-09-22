#include "uptick.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>

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
  Serial.printf("[net] joined, ip %s rssi %d\n", g_net.ip.c_str(), WiFi.RSSI());

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
  http.on("/api/files",      HTTP_GET,  handleFiles);
  http.on("/api/scan",       HTTP_GET,  handleScan);
  http.on("/api/wifi",       HTTP_POST, handleWifi);
  http.on("/api/forget",     HTTP_POST, handleForget);
  http.on("/api/update",     HTTP_POST, handleUpdateDone, handleUpdateUpload);

  http.serveStatic("/",           LittleFS, "/www/index.html");
  http.serveStatic("/index.html", LittleFS, "/www/index.html");
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
