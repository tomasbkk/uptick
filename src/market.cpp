#include "uptick.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

Quote g_quotes[UPTICK_MAX_FEEDS];
SemaphoreHandle_t g_quoteMutex = nullptr;

#ifndef UPTICK_TLS_INSECURE
// Provided by the Arduino ESP32 core's embedded Mozilla CA bundle. If your
// core version does not export this symbol the link will fail - build with
// -DUPTICK_TLS_INSECURE to fall back while you sort it out, but do not ship
// that way: it accepts any certificate.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
#endif

static uint32_t s_lastAttemptMs[UPTICK_MAX_FEEDS] = {0};

// ---------------------------------------------------------------- parsing

// Both APIs return an array of arrays where index 4 is the close - Binance as
// quoted strings, Coinbase as bare numbers. Pulling that one field out of the
// socket by hand costs ~40 lines and zero allocation; handing the whole
// response to ArduinoJson would mean holding 64 x 12 fields of JSON in RAM at
// the same moment a TLS session is open, which is the wrong trade on a part
// with 512 KB of internal SRAM.
static int extractField(Stream& s, int wantIdx, float* out, int maxOut)
{
  int  depth = 0, field = 0, n = 0;
  bool inStr = false, capture = false;
  char buf[24];
  uint8_t blen = 0;

  const uint32_t deadline = millis() + 15000;   // whole-response budget
  uint32_t       lastData = millis();           // idle budget

  auto flush = [&]()
  {
    if (!capture) return;
    buf[blen] = 0;
    if (n < maxOut) out[n++] = atof(buf);
    capture = false;
    blen    = 0;
  };

  while (millis() < deadline)
  {
    if (!s.available())
    {
      // Stream has no connected(), and the concrete client type behind it
      // differs between ESP32 core versions, so a stall stands in for
      // detecting a closed socket. The parser normally returns earlier, on
      // the closing bracket of the outer array.
      if (millis() - lastData > 3000) break;
      delay(5);
      continue;
    }

    lastData = millis();
    char c   = (char)s.read();

    if (inStr)
    {
      if (c == '"') { inStr = false; continue; }
      if (capture && blen < sizeof(buf) - 1) buf[blen++] = c;
      continue;
    }

    switch (c)
    {
      case '"':
        inStr = true;
        if (depth == 2 && field == wantIdx) { capture = true; blen = 0; }
        break;

      case '[':
        if (++depth == 2) { field = 0; capture = false; blen = 0; }
        break;

      case ',':
        if (depth == 2) { flush(); field++; }
        break;

      case ']':
        if (depth == 2) flush();
        if (--depth <= 0) return n;     // end of the outer array
        break;

      default:
        // Bare numeric field (Coinbase).
        if (depth == 2 && field == wantIdx &&
            (isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E'))
        {
          if (!capture) { capture = true; blen = 0; }
          if (blen < sizeof(buf) - 1) buf[blen++] = c;
        }
        break;
    }
  }
  return n;
}

// ---------------------------------------------------------------- adapters

static uint32_t coinbaseGranularity(const String& interval)
{
  // Coinbase only accepts this fixed set. 4h has no equivalent, so it rounds
  // up to 6h rather than silently returning an error body.
  if (interval == "5m")  return 300;
  if (interval == "15m") return 900;
  if (interval == "1h")  return 3600;
  if (interval == "4h")  return 21600;
  if (interval == "1d")  return 86400;
  return 3600;
}

// The bar width actually returned, which is not always what was asked for:
// Coinbase has no 4h granularity and rounds up to 6h.
static uint32_t feedBarSeconds(const Feed& f)
{
  if (f.source == "coinbase") return coinbaseGranularity(f.interval);

  if (f.interval == "5m")  return 300;
  if (f.interval == "15m") return 900;
  if (f.interval == "1h")  return 3600;
  if (f.interval == "4h")  return 14400;
  if (f.interval == "1d")  return 86400;
  return 3600;
}

static String buildUrl(const Feed& f)
{
  if (f.source == "coinbase")
  {
    return "https://api.exchange.coinbase.com/products/" + f.symbol +
           "/candles?granularity=" + String(coinbaseGranularity(f.interval));
  }

  return "https://api.binance.com/api/v3/klines?symbol=" + f.symbol +
         "&interval=" + f.interval +
         "&limit=" + String(UPTICK_MAX_POINTS);
}

// ---------------------------------------------------------------- fetch

static void prepareClient(WiFiClientSecure& client, HTTPClient& https)
{
#ifdef UPTICK_TLS_INSECURE
  client.setInsecure();
#else
  client.setCACertBundle(rootca_crt_bundle_start);
#endif
  client.setTimeout(12);

  https.setConnectTimeout(8000);
  https.setTimeout(12000);
  https.setUserAgent("uptick/" UPTICK_VERSION);
}

// Second request, only reached when the charted window is shorter than 24h.
// At the default 1h x 64 bars it never runs - the reference close is already
// in the candles we just fetched.
static bool fetch24h(const Feed& f, float& pct)
{
  WiFiClientSecure client;
  HTTPClient       https;
  prepareClient(client, https);

  const bool coinbase = (f.source == "coinbase");

  String url = coinbase
    ? "https://api.exchange.coinbase.com/products/" + f.symbol + "/stats"
    : "https://api.binance.com/api/v3/ticker/24hr?symbol=" + f.symbol;

  if (!https.begin(client, url)) return false;

  if (https.GET() != HTTP_CODE_OK)
  {
    https.end();
    return false;
  }

  // An object, not an array, so ArduinoJson filtering works here - it can
  // select by key even though it cannot select by array index.
  JsonDocument filter;
  if (coinbase) { filter["open"] = true; filter["last"] = true; }
  else          { filter["priceChangePercent"] = true; }

  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, https.getStream(),
                                           DeserializationOption::Filter(filter));
  https.end();
  if (e) return false;

  if (coinbase)
  {
    float open = atof(doc["open"] | "0");
    float last = atof(doc["last"] | "0");
    if (open <= 0.0f) return false;
    pct = (last - open) / open * 100.0f;
  }
  else
  {
    if (doc["priceChangePercent"].isNull()) return false;
    pct = atof(doc["priceChangePercent"] | "0");
  }
  return true;
}

static bool fetchFeed(const Feed& f, Quote& q, bool want24h)
{
  WiFiClientSecure client;
  HTTPClient       https;
  prepareClient(client, https);

  String url = buildUrl(f);
  if (!https.begin(client, url))
  {
    Serial.printf("[mkt] %s: begin failed\n", f.symbol.c_str());
    return false;
  }

  int code = https.GET();
  if (code != HTTP_CODE_OK)
  {
    Serial.printf("[mkt] %s: http %d\n", f.symbol.c_str(), code);
    https.end();
    return false;
  }

  float raw[UPTICK_MAX_POINTS];
  int   n = extractField(https.getStream(), 4, raw, UPTICK_MAX_POINTS);
  https.end();

  if (n < 2)
  {
    Serial.printf("[mkt] %s: only %d point(s)\n", f.symbol.c_str(), n);
    return false;
  }

  // Binance is oldest-first, Coinbase newest-first. Normalise to oldest-first
  // so the renderer never has to care which exchange it came from.
  if (f.source == "coinbase")
  {
    for (int i = 0; i < n / 2; i++)
    {
      float t          = raw[i];
      raw[i]           = raw[n - 1 - i];
      raw[n - 1 - i]   = t;
    }
  }

  float lo = raw[0], hi = raw[0];
  for (int i = 1; i < n; i++)
  {
    if (raw[i] < lo) lo = raw[i];
    if (raw[i] > hi) hi = raw[i];
  }

  // Derive the 24h change from the candles when the window already covers it -
  // at the default 1h interval the reference is just 24 bars back, so the
  // common case costs no extra request. Only short intervals fall through to
  // the dedicated endpoint.
  float change24h = 0.0f;
  bool  has24h    = false;

  if (want24h)
  {
    uint32_t bars = 86400UL / feedBarSeconds(f);
    if (bars >= 1 && (int)bars < n)
    {
      float ref = raw[n - 1 - bars];
      if (ref > 0.0f)
      {
        change24h = (raw[n - 1] - ref) / ref * 100.0f;
        has24h    = true;
      }
    }
    else
    {
      has24h = fetch24h(f, change24h);
      if (!has24h) Serial.printf("[mkt] %s: 24h lookup failed\n", f.symbol.c_str());
    }
  }

  {
    QuoteLock lk;
    q.count = n;
    memcpy(q.points, raw, sizeof(float) * n);
    q.lo    = lo;
    q.hi    = hi;
    q.price = raw[n - 1];

    // Always computed, because it is free and because it is what the chart
    // actually shows. The 24h figure is optional and sits alongside it.
    q.changePct = (raw[0] > 0.0f) ? ((raw[n - 1] - raw[0]) / raw[0]) * 100.0f : 0.0f;
    q.change24h = change24h;
    q.has24h    = has24h;

    q.valid     = true;
    q.fetchedMs = millis();
    q.failures  = 0;
  }

  Serial.printf("[mkt] %s %.4f %+.2f%% (%d pts)\n",
                f.symbol.c_str(), q.price, q.changePct, n);
  return true;
}

// ---------------------------------------------------------------- lifecycle

void marketBegin()
{
  g_quoteMutex = xSemaphoreCreateMutex();
}

void marketPoll()
{
  if (g_net.apMode || WiFi.status() != WL_CONNECTED) return;

  // Cached copy; only re-taken when the config actually changes. This runs off
  // the network task loop at a few hundred hertz, so the deep copy matters.
  static Config   cfg;
  static uint32_t seenRev = 0;
  if (seenRev != g_cfgRev)
  {
    CfgLock lk;
    cfg     = g_cfg;
    seenRev = g_cfgRev;
  }

  // Refresh whichever feed is most overdue, one per call. That naturally
  // staggers the requests instead of firing N TLS handshakes back to back.
  int      pick     = -1;
  uint32_t oldest   = 0;
  uint32_t now      = millis();

  for (uint8_t i = 0; i < cfg.feedCount; i++)
  {
    uint32_t age = now - s_lastAttemptMs[i];

    uint32_t due = (uint32_t)cfg.refreshSec * 1000UL;
    if (s_lastAttemptMs[i] != 0)
    {
      // Back off on repeated failure so a bad symbol or a blocked region does
      // not hammer the API every refresh interval forever.
      QuoteLock lk;
      uint32_t f = g_quotes[i].failures;
      if (f > 0) due *= (f > 4 ? 16 : (1UL << f));
    }

    if (s_lastAttemptMs[i] == 0 || age >= due)
    {
      if (pick < 0 || age > oldest) { pick = i; oldest = age; }
    }
  }

  if (pick < 0) return;

  s_lastAttemptMs[pick] = now;
  if (!fetchFeed(cfg.feeds[pick], g_quotes[pick], cfg.changeWindow == "24h"))
  {
    QuoteLock lk;
    g_quotes[pick].failures++;
  }
}
