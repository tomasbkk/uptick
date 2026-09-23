#include "uptick.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

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
static uint32_t s_lastAnyFetchMs = 0;

// Cached in RAM: reading NVS is a flash access, and flash accesses stall the
// panel's refresh. Loaded once at boot, updated when the key is changed.
static String s_cgKey;
static volatile bool s_paused = false;

void marketSetPaused(bool paused)
{
  s_paused = paused;
  Serial.printf("[mkt] fetching %s\n", paused ? "paused" : "resumed");
}

bool marketPaused() { return s_paused; }

bool marketHasApiKey() { return s_cgKey.length() > 0; }

void marketSetApiKey(const String& key)
{
  Preferences prefs;
  prefs.begin("uptick", false);
  if (key.isEmpty()) prefs.remove("cgkey");
  else               prefs.putString("cgkey", key);
  prefs.end();

  s_cgKey = key;
  Serial.printf("[mkt] coingecko key %s\n", key.isEmpty() ? "cleared" : "set");
}

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

  // Keeps the LAST maxOut values rather than the first. Binance returns
  // exactly what was asked for, but CoinGecko chooses its own granularity and
  // can return hundreds of points - and the recent end is the interesting one.
  auto flush = [&]()
  {
    if (!capture) return;
    buf[blen] = 0;

    const float v = atof(buf);
    if (n < maxOut)
    {
      out[n] = v;
    }
    else
    {
      memmove(out, out + 1, sizeof(float) * (maxOut - 1));
      out[maxOut - 1] = v;
    }
    n++;

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

// Yahoo nests the series three arrays deep and returns the quote fields in no
// fixed order - volume, high, close, open and low have all been observed in
// different positions - so the index-based extractor above cannot be used on
// it. This walks the stream once matching two literal keys: the change against
// the previous close, which arrives early inside "meta", and the close series
// inside indicators.quote[0]. One request therefore yields the price, the
// day's move and the chart, with no second lookup.
static int extractYahoo(Stream& s, float* out, int maxOut, float& pct, bool& hasPct,
                        char* ccy, size_t ccyLen)
{
  static const char KEY_PCT[]   = "\"regularMarketChangePercent\":";
  static const char KEY_CLOSE[] = "\"close\":[";
  static const char KEY_CCY[]   = "\"currency\":\"";
  const uint8_t LEN_PCT   = sizeof(KEY_PCT) - 1;
  const uint8_t LEN_CLOSE = sizeof(KEY_CLOSE) - 1;
  const uint8_t LEN_CCY   = sizeof(KEY_CCY) - 1;

  uint8_t mp = 0, mc = 0, mu = 0;  // how much of each key has matched so far
  bool    inPct = false, inArr = false, inCcy = false;
  char    buf[24];
  uint8_t blen = 0;
  int     n    = 0;

  hasPct = false;

  const uint32_t deadline = millis() + 15000;
  uint32_t       lastData = millis();

  auto isNum = [](char c)
  {
    return isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.' ||
           c == 'e' || c == 'E';
  };

  // Keeps the newest maxOut values. A wide range comes back with several
  // hundred bars and the recent end is the one that reaches the panel.
  auto push = [&](float v)
  {
    if (n < maxOut)
    {
      out[n] = v;
    }
    else
    {
      memmove(out, out + 1, sizeof(float) * (maxOut - 1));
      out[maxOut - 1] = v;
    }
    n++;
  };

  while (millis() < deadline)
  {
    if (!s.available())
    {
      if (millis() - lastData > 3000) break;
      delay(5);
      continue;
    }

    lastData = millis();
    char c   = (char)s.read();

    if (inPct)
    {
      if (isNum(c) && blen < sizeof(buf) - 1) { buf[blen++] = c; continue; }

      // Guarded on blen because the field can come back null, and an empty
      // buffer would otherwise be reported as a confident 0.00%.
      buf[blen] = 0;
      if (blen) { pct = atof(buf); hasPct = true; }
      inPct = false;
      blen  = 0;
      // Falls through: this character still has to reach the matchers.
    }

    if (inCcy)
    {
      // Not every listing is priced in dollars - a Frankfurt line quotes EUR,
      // a London one quotes pence - and the panel must not print "$" over it.
      if (c != '"' && blen < ccyLen - 1 && blen < sizeof(buf) - 1) { buf[blen++] = c; continue; }
      buf[blen] = 0;
      if (blen) { strncpy(ccy, buf, ccyLen - 1); ccy[ccyLen - 1] = 0; }
      inCcy = false;
      blen  = 0;
      continue;
    }

    if (inArr)
    {
      if (isNum(c) && blen < sizeof(buf) - 1) { buf[blen++] = c; continue; }

      // A bar with no trade comes back as null, which captures nothing. Those
      // are dropped rather than interpolated - one column fewer is closer to
      // the truth than a price that never existed.
      if (blen) { buf[blen] = 0; push(atof(buf)); blen = 0; }

      if (c == ']') return n;      // end of the close array; nothing after it is wanted
      continue;
    }

    mp = (c == KEY_PCT[mp])   ? mp + 1 : (c == KEY_PCT[0]   ? 1 : 0);
    mc = (c == KEY_CLOSE[mc]) ? mc + 1 : (c == KEY_CLOSE[0] ? 1 : 0);
    mu = (c == KEY_CCY[mu])   ? mu + 1 : (c == KEY_CCY[0]   ? 1 : 0);

    if (mp == LEN_PCT)   { mp = 0; inPct = true; blen = 0; }
    if (mc == LEN_CLOSE) { mc = 0; inArr = true; blen = 0; }
    if (mu == LEN_CCY)   { mu = 0; inCcy = true; blen = 0; }
  }
  return n;
}

// ---------------------------------------------------------------- adapters

// CoinGecko does not take a candle size - it picks granularity from the range
// requested: 1 day gives 5-minutely, 2 to 90 days gives hourly, beyond that
// daily. So the interval maps onto a range whose granularity is closest, and
// the last 64 points of whatever comes back are charted. 4h has no equivalent
// and behaves as hourly.
static uint16_t coingeckoDays(const String& interval)
{
  if (interval == "5m" || interval == "15m") return 1;    // 5-minutely
  if (interval == "1h" || interval == "4h")  return 3;    // hourly
  return 365;                                             // daily
}

// Yahoo takes a range and a bar width as two separate parameters, and only
// serves intraday bars over short ranges. Each range here is chosen to return
// at least the 64 bars the panel has columns for - anything beyond that is
// trimmed to the newest 64 by the parser.
//
// There is no 4h bar, so it rounds to daily, the same compromise Coinbase
// forces by rounding 4h up to 6h.
static const char* yahooRange(const String& interval)
{
  if (interval == "5m")  return "1d";
  if (interval == "15m") return "5d";
  if (interval == "1h")  return "1mo";
  return "3mo";                                   // 4h and 1d
}

static const char* yahooInterval(const String& interval)
{
  if (interval == "5m")  return "5m";
  if (interval == "15m") return "15m";
  if (interval == "1h")  return "60m";
  return "1d";                                    // 4h and 1d
}

// Tickers carry characters that are not legal unencoded in a URL: "^GSPC" for
// an index, "EURUSD=X" for a pair, and Binance's batch takes a JSON array in
// its query string. Everything outside the unreserved set is encoded rather
// than reasoned about; all the APIs here decode it back.
static String urlEncode(const String& s)
{
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++)
  {
    const char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_' || c == '~')
    {
      out += c;
    }
    else
    {
      char esc[4];
      snprintf(esc, sizeof(esc), "%%%02X", (unsigned char)c);
      out += esc;
    }
  }
  return out;
}

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

  // Yahoo has no 4h bar; both 4h and 1d come back daily.
  if (f.source == "yahoo")
    return (f.interval == "5m") ? 300 : (f.interval == "15m") ? 900
         : (f.interval == "1h") ? 3600 : 86400;

  if (f.source == "coingecko")
  {
    // Granularity follows the range: 1 day is 5-minutely, up to 90 is hourly,
    // beyond that daily.
    const uint16_t days = coingeckoDays(f.interval);
    return days <= 1 ? 300 : (days <= 90 ? 3600 : 86400);
  }

  if (f.interval == "5m")  return 300;
  if (f.interval == "15m") return 900;
  if (f.interval == "1h")  return 3600;
  if (f.interval == "4h")  return 14400;
  if (f.interval == "1d")  return 86400;
  return 3600;
}

static String buildUrl(const Feed& f)
{
  if (f.source == "yahoo")
  {
    // Symbol is the Yahoo ticker, which is not only stocks: "AAPL", "SPY",
    // "^GSPC", "EURUSD=X", "GC=F" and "SAP.DE" all resolve through the same
    // endpoint with the same fields.
    return "https://query1.finance.yahoo.com/v8/finance/chart/" + urlEncode(f.symbol) +
           "?range="     + String(yahooRange(f.interval)) +
           "&interval="  + String(yahooInterval(f.interval));
  }

  if (f.source == "coingecko")
  {
    // Symbol is the CoinGecko coin id here - "bitcoin", not "BTCUSDT".
    return "https://api.coingecko.com/api/v3/coins/" + f.symbol +
           "/market_chart?vs_currency=usd&days=" + String(coingeckoDays(f.interval));
  }

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

  // Every parser here reads getStream() raw, and getStream() does not undo
  // chunked transfer encoding - the chunk-size lines arrive in the byte stream
  // mixed into the JSON. CoinGecko and Coinbase both chunk over HTTP/1.1, which
  // silently broke their 24h lookups (the parse stopped at the first chunk
  // header) and could splice hex digits into a chart price wherever a chunk
  // boundary fell mid-number. HTTP/1.0 has no chunked encoding at all; all six
  // endpoints in use were checked to answer it with a plain body.
  https.useHTTP10(true);
}

// Second request, only reached when the charted window is shorter than 24h.
// At the default 1h x 64 bars it never runs - the reference close is already
// in the candles we just fetched.
static bool fetch24h(const Feed& f, float& pct)
{
  WiFiClientSecure client;
  HTTPClient       https;
  prepareClient(client, https);

  const bool coinbase  = (f.source == "coinbase");
  const bool coingecko = (f.source == "coingecko");

  String url =
    coingecko ? "https://api.coingecko.com/api/v3/simple/price?ids=" + f.symbol +
                "&vs_currencies=usd&include_24hr_change=true"
  : coinbase  ? "https://api.exchange.coinbase.com/products/" + f.symbol + "/stats"
              : "https://api.binance.com/api/v3/ticker/24hr?symbol=" + f.symbol;

  if (!https.begin(client, url)) return false;
  if (coingecko && s_cgKey.length()) https.addHeader("x-cg-demo-api-key", s_cgKey);

  if (https.GET() != HTTP_CODE_OK)
  {
    https.end();
    return false;
  }

  // An object, not an array, so ArduinoJson filtering works here - it can
  // select by key even though it cannot select by array index.
  JsonDocument filter;
  if      (coingecko) { filter[f.symbol]["usd_24h_change"] = true; }
  else if (coinbase)  { filter["open"] = true; filter["last"] = true; }
  else                { filter["priceChangePercent"] = true; }

  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, https.getStream(),
                                           DeserializationOption::Filter(filter));
  https.end();
  if (e) return false;

  if (coingecko)
  {
    JsonVariantConst v = doc[f.symbol]["usd_24h_change"];
    if (v.isNull()) return false;
    pct = v.as<float>();
    return true;
  }

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

// Unlike the three exchange APIs, Yahoo sends no Access-Control-Allow-Origin,
// so the web page cannot check a ticker from the browser the way it does for
// the others - the device has to fetch it on the page's behalf. One TLS
// handshake per click of "check", which is a manual action and rare.
bool marketProbeYahoo(const String& symbol, float& price, String& name, String& currency)
{
  WiFiClientSecure client;
  HTTPClient       https;
  prepareClient(client, https);

  // The narrowest range there is: nothing here needs the series, only meta.
  const String url = "https://query1.finance.yahoo.com/v8/finance/chart/" +
                     urlEncode(symbol) + "?range=1d&interval=1d";

  if (!https.begin(client, url)) return false;

  if (https.GET() != HTTP_CODE_OK)   // 404 is how an unknown ticker answers
  {
    https.end();
    return false;
  }

  // An array in a filter applies its single element to every entry, so this
  // selects meta out of result[0] without ArduinoJson having to hold the
  // timestamp and close arrays at the same time as an open TLS session.
  JsonDocument filter;
  JsonObject   meta = filter["chart"]["result"][0]["meta"].to<JsonObject>();
  meta["regularMarketPrice"] = true;
  meta["shortName"]          = true;
  meta["currency"]           = true;

  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, https.getStream(),
                                           DeserializationOption::Filter(filter));
  https.end();
  if (e) return false;

  JsonVariantConst m = doc["chart"]["result"][0]["meta"];
  if (m["regularMarketPrice"].isNull()) return false;

  price    = m["regularMarketPrice"].as<float>();
  name     = m["shortName"] | "";
  currency = m["currency"]  | "";
  return true;
}

static bool fetchFeed(const Feed& f, Quote& q)
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

  const bool yahoo = (f.source == "yahoo");

  float raw[UPTICK_MAX_POINTS];
  float dayPct  = 0.0f;
  bool  haveDay = false;
  char  ccy[8]  = "USD";        // the crypto sources are all USD pairs
  int   n;

  if (yahoo)
  {
    n = extractYahoo(https.getStream(), raw, UPTICK_MAX_POINTS, dayPct, haveDay,
                     ccy, sizeof(ccy));
  }
  else
  {
    // Binance klines and Coinbase candles both put the close at index 4.
    // CoinGecko's market_chart is [[timestamp, price], ...], so index 1.
    n = extractField(https.getStream(), (f.source == "coingecko") ? 1 : 4,
                     raw, UPTICK_MAX_POINTS);
  }
  https.end();

  if (n > UPTICK_MAX_POINTS) n = UPTICK_MAX_POINTS;   // flush kept the newest

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

  if (yahoo)
  {
    // A rolling 24h window is meaningless for something that trades 6.5 hours
    // a day: the honest equivalent is the move against the previous close,
    // which is also the number every quote page prints. Yahoo supplies it in
    // the response already parsed above, so there is no second request.
    change24h = dayPct;
    has24h    = haveDay;
    if (!has24h) Serial.printf("[mkt] %s: no change field\n", f.symbol.c_str());
  }
  else
  {
    const uint32_t bars = 86400UL / feedBarSeconds(f);
    if (bars >= 1 && (int)bars < n)
    {
      const float ref = raw[n - 1 - bars];
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
    strncpy(q.ccy, ccy, sizeof(q.ccy) - 1);
    q.ccy[sizeof(q.ccy) - 1] = 0;

    // Always computed, because it is free and because it is what the chart
    // actually shows. The 24h figure is optional and sits alongside it.
    q.changePct = (raw[0] > 0.0f) ? ((raw[n - 1] - raw[0]) / raw[0]) * 100.0f : 0.0f;
    q.change24h = change24h;
    q.has24h    = has24h;

    q.valid     = true;
    q.fetchedMs = millis();
    q.failures  = 0;
  }

  // Both figures, because they legitimately differ: the panel prints the 24h
  // change while the chart is coloured by its own window.
  Serial.printf("[mkt] %s %.4f  24h %+.2f%%%s  chart %+.2f%%  (%d pts)\n",
                f.symbol.c_str(), q.price,
                has24h ? change24h : 0.0f, has24h ? "" : "?",
                q.changePct, n);
  return true;
}

// ---------------------------------------------------------------- batch

// Layouts that show many feeds at once - the price list now, the scrolling
// ticker next - print a price and a change per feed and no chart. That is what
// the batch endpoints return, so one request per source replaces one per feed:
//
//   binance    /api/v3/ticker/24hr?type=MINI&symbols=[...]   every symbol at once
//   yahoo      /v8/finance/spark?symbols=...                 20 per request at most
//   coingecko  /api/v3/simple/price?ids=...                  every id at once
//   coinbase   no batch endpoint, so one /stats request per product
//
// Thirty feeds across Binance and Yahoo is two or three requests a cycle,
// against thirty.
//
// Symbols are deduplicated before they go out. Binance refuses a list that
// names one twice (-1151), and the same symbol can legitimately sit in two
// feeds with different chart spans; each result is applied to every feed that
// carries it.
//
// Binance also refuses the whole list over one symbol it does not recognise
// (-1121). That is why the web page checks new symbols before it will save
// them. A coin delisted after it was added would still take every Binance feed
// stale with it - visibly, at least, since they would all show it.

static bool     s_due[UPTICK_MAX_FEEDS];   // still owed a price this cycle
static uint32_t s_cycleMs = 0;
static bool     s_cycled  = false;

static const uint8_t YAHOO_SPARK_MAX = 20;   // spark answers 400 above this

// The distinct symbols among this cycle's outstanding feeds for one source,
// in feed order.
static uint8_t collectSymbols(const Config& cfg, const char* source, String* out, uint8_t maxOut)
{
  uint8_t n = 0;
  for (uint8_t i = 0; i < cfg.feedCount && n < maxOut; i++)
  {
    if (!s_due[i] || cfg.feeds[i].source != source) continue;

    bool seen = false;
    for (uint8_t k = 0; k < n && !seen; k++) seen = (out[k] == cfg.feeds[i].symbol);
    if (!seen) out[n++] = cfg.feeds[i].symbol;
  }
  return n;
}

// Records one symbol's price against every outstanding feed that carries it.
// The chart and its window change are left as they were - a batch has neither.
static void applyQuote(const Config& cfg, const char* source, const String& sym,
                       float price, float pct, bool hasPct)
{
  for (uint8_t i = 0; i < cfg.feedCount; i++)
  {
    if (!s_due[i] || cfg.feeds[i].source != source || cfg.feeds[i].symbol != sym) continue;
    s_due[i] = false;

    QuoteLock lk;
    Quote& q = g_quotes[i];
    q.price     = price;
    q.change24h = pct;
    q.has24h    = hasPct;
    q.valid     = true;
    q.fetchedMs = millis();
    q.failures  = 0;

    // Spark carries no currency, and a Yahoo feed only learns its own from a
    // chart fetch. Until it has had one, "unknown" prints no symbol at all,
    // which beats a "$" in front of a price quoted in euros.
    if (!strcmp(source, "yahoo") && q.count == 0) q.ccy[0] = 0;
  }
}

// Whatever was asked for and did not come back failed this cycle. Clears it
// either way, so a failed request never leaves the cycle stuck on it.
static void failRequested(const Config& cfg, const char* source, const String* syms, uint8_t n)
{
  for (uint8_t i = 0; i < cfg.feedCount; i++)
  {
    if (!s_due[i] || cfg.feeds[i].source != source) continue;

    for (uint8_t k = 0; k < n; k++)
    {
      if (cfg.feeds[i].symbol != syms[k]) continue;
      s_due[i] = false;
      QuoteLock lk;
      g_quotes[i].failures++;
      break;
    }
  }
}

// status, when given, receives the HTTP code (0 if the request never got that
// far) so a caller can tell a meaningful refusal from a failure to ask.
static bool getJson(const String& url, JsonDocument& filter, JsonDocument& doc,
                    const char* tag, bool geckoKey = false, int* status = nullptr)
{
  if (status) *status = 0;

  WiFiClientSecure client;
  HTTPClient       https;
  prepareClient(client, https);

  if (!https.begin(client, url))
  {
    Serial.printf("[mkt] %s: begin failed\n", tag);
    return false;
  }
  if (geckoKey && s_cgKey.length()) https.addHeader("x-cg-demo-api-key", s_cgKey);

  const int code = https.GET();
  if (status) *status = code;
  if (code != HTTP_CODE_OK)
  {
    Serial.printf("[mkt] %s: http %d\n", tag, code);
    https.end();
    return false;
  }

  // Objects and arrays of objects throughout, so key filtering works and the
  // document holds only the two or three fields wanted from each entry.
  DeserializationError e = deserializeJson(doc, https.getStream(),
                                           DeserializationOption::Filter(filter));
  https.end();
  if (e)
  {
    Serial.printf("[mkt] %s: %s\n", tag, e.c_str());
    return false;
  }
  return true;
}

static void batchBinance(const Config& cfg)
{
  String syms[UPTICK_MAX_FEEDS];
  const uint8_t n = collectSymbols(cfg, "binance", syms, UPTICK_MAX_FEEDS);
  if (!n) return;

  String list = "[";
  for (uint8_t k = 0; k < n; k++)
  {
    if (k) list += ",";
    list += "\"" + syms[k] + "\"";
  }
  list += "]";

  JsonDocument filter;
  JsonObject   want = filter.add<JsonObject>();   // one element filters them all
  want["symbol"]    = true;
  want["lastPrice"] = true;
  want["openPrice"] = true;

  JsonDocument doc;
  uint8_t      got = 0;
  if (getJson("https://api.binance.com/api/v3/ticker/24hr?type=MINI&symbols=" + urlEncode(list),
              filter, doc, "binance batch"))
  {
    for (JsonObjectConst o : doc.as<JsonArrayConst>())
    {
      const float last = atof(o["lastPrice"] | "0");
      const float open = atof(o["openPrice"] | "0");
      if (last <= 0.0f) continue;

      // MINI leaves out priceChangePercent. This is the same figure: open is
      // the price at the start of the same rolling 24 hours.
      applyQuote(cfg, "binance", String(o["symbol"] | ""), last,
                 open > 0.0f ? (last - open) / open * 100.0f : 0.0f, open > 0.0f);
      got++;
    }
  }
  failRequested(cfg, "binance", syms, n);
  Serial.printf("[mkt] binance batch: %u of %u\n", got, n);
}

static void batchYahoo(const Config& cfg)
{
  String syms[YAHOO_SPARK_MAX];
  const uint8_t n = collectSymbols(cfg, "yahoo", syms, YAHOO_SPARK_MAX);
  if (!n) return;

  String       list;
  JsonDocument filter;
  for (uint8_t k = 0; k < n; k++)
  {
    if (k) list += ",";
    list += urlEncode(syms[k]);
    filter[syms[k]]["close"]              = true;
    filter[syms[k]]["chartPreviousClose"] = true;
  }

  JsonDocument doc;
  uint8_t      got = 0;
  if (getJson("https://query1.finance.yahoo.com/v8/finance/spark?range=1d&interval=1d&symbols=" + list,
              filter, doc, "yahoo batch"))
  {
    for (uint8_t k = 0; k < n; k++)
    {
      // Keyed by the plain symbol as sent. An unknown ticker is simply absent
      // rather than failing the request.
      JsonVariantConst s = doc[syms[k]];
      if (s.isNull()) continue;

      float price = 0.0f;
      for (JsonVariantConst v : s["close"].as<JsonArrayConst>())
        if (!v.isNull()) price = v.as<float>();
      if (price <= 0.0f) continue;

      // Against the previous close, as the chart path reports it - a rolling
      // 24h means nothing for something that trades 6.5 hours a day.
      const float prev = s["chartPreviousClose"] | 0.0f;
      applyQuote(cfg, "yahoo", syms[k], price,
                 prev > 0.0f ? (price - prev) / prev * 100.0f : 0.0f, prev > 0.0f);
      got++;
    }
  }
  failRequested(cfg, "yahoo", syms, n);
  Serial.printf("[mkt] yahoo batch: %u of %u\n", got, n);
}

static void batchCoinGecko(const Config& cfg)
{
  String ids[UPTICK_MAX_FEEDS];
  const uint8_t n = collectSymbols(cfg, "coingecko", ids, UPTICK_MAX_FEEDS);
  if (!n) return;

  String       list;
  JsonDocument filter;
  for (uint8_t k = 0; k < n; k++)
  {
    if (k) list += ",";
    list += urlEncode(ids[k]);
    filter[ids[k]]["usd"]            = true;
    filter[ids[k]]["usd_24h_change"] = true;
  }

  JsonDocument doc;
  uint8_t      got = 0;
  if (getJson("https://api.coingecko.com/api/v3/simple/price?vs_currencies=usd&include_24hr_change=true&ids=" + list,
              filter, doc, "coingecko batch", true))
  {
    for (uint8_t k = 0; k < n; k++)
    {
      JsonVariantConst c = doc[ids[k]];            // unknown ids are absent
      const float price  = c["usd"] | 0.0f;
      if (price <= 0.0f) continue;

      JsonVariantConst pct = c["usd_24h_change"];
      applyQuote(cfg, "coingecko", ids[k], price, pct | 0.0f, !pct.isNull());
      got++;
    }
  }
  failRequested(cfg, "coingecko", ids, n);
  Serial.printf("[mkt] coingecko batch: %u of %u\n", got, n);
}

// Coinbase has no batch endpoint, so this is one product per call.
static void singleCoinbase(const Config& cfg)
{
  String sym[1];
  if (!collectSymbols(cfg, "coinbase", sym, 1)) return;

  JsonDocument filter;
  filter["open"] = true;
  filter["last"] = true;

  JsonDocument doc;
  if (getJson("https://api.exchange.coinbase.com/products/" + urlEncode(sym[0]) + "/stats",
              filter, doc, "coinbase"))
  {
    const float last = atof(doc["last"] | "0");
    const float open = atof(doc["open"] | "0");
    if (last > 0.0f)
      applyQuote(cfg, "coinbase", sym[0], last,
                 open > 0.0f ? (last - open) / open * 100.0f : 0.0f, open > 0.0f);
  }
  failRequested(cfg, "coinbase", sym, 1);
}

bool marketCheckYahoo(const String* syms, uint8_t n, bool* found)
{
  for (uint8_t k = 0; k < n; k++) found[k] = false;

  for (uint8_t start = 0; start < n; start += YAHOO_SPARK_MAX)
  {
    const uint8_t end = (n - start > YAHOO_SPARK_MAX) ? start + YAHOO_SPARK_MAX : n;

    String       list;
    JsonDocument filter;
    for (uint8_t k = start; k < end; k++)
    {
      if (k > start) list += ",";
      list += urlEncode(syms[k]);
      filter[syms[k]]["close"] = true;
    }

    JsonDocument doc;
    int          code;
    if (!getJson("https://query1.finance.yahoo.com/v8/finance/spark?range=1d&interval=1d&symbols=" + list,
                 filter, doc, "yahoo check", false, &code))
    {
      // Spark leaves unknown tickers out of a mixed list, but when it knows
      // none of them it answers 404 instead. That is an answer - this whole
      // chunk is missing - not a failure to ask.
      if (code == 404) continue;
      return false;
    }

    for (uint8_t k = start; k < end; k++) found[k] = !doc[syms[k]].isNull();
  }
  return true;
}

// One request per call, so the web server is serviced between them. A cycle
// starts every poll interval and runs until every feed has been answered.
static void pollBatch(const Config& cfg, uint32_t now)
{
  bool pending = false;
  for (uint8_t i = 0; i < cfg.feedCount && !pending; i++) pending = s_due[i];

  if (!pending)
  {
    if (s_cycled && now - s_cycleMs < (uint32_t)cfg.refreshSec * 1000UL) return;
    for (uint8_t i = 0; i < cfg.feedCount; i++) s_due[i] = true;
    s_cycleMs = now;
    s_cycled  = true;
  }

  static const char* const ORDER[] = { "binance", "yahoo", "coingecko", "coinbase" };
  for (const char* src : ORDER)
  {
    for (uint8_t i = 0; i < cfg.feedCount; i++)
    {
      if (!s_due[i] || cfg.feeds[i].source != src) continue;

      if      (!strcmp(src, "binance"))   batchBinance(cfg);
      else if (!strcmp(src, "yahoo"))     batchYahoo(cfg);
      else if (!strcmp(src, "coingecko")) batchCoinGecko(cfg);
      else                                singleCoinbase(cfg);
      return;
    }
  }

  // Only a source nothing here recognises can reach this. Drop it rather than
  // leave it outstanding, which would stop the next cycle from ever starting.
  for (uint8_t i = 0; i < cfg.feedCount; i++) s_due[i] = false;
}

// ---------------------------------------------------------------- lifecycle

// Called when the feed list changes. The attempt timers are indexed by feed
// position, so after a reorder they describe whoever used to hold that slot.
// Clearing them makes every feed due, and display-gated polling then refreshes
// each one as its turn comes round rather than all at once. The batch cycle is
// restarted for the same reason, which also puts fresh prices up straight
// after a save.
void marketConfigChanged()
{
  for (uint8_t i = 0; i < UPTICK_MAX_FEEDS; i++)
  {
    s_lastAttemptMs[i] = 0;
    s_due[i]           = false;
  }
  s_lastAnyFetchMs = 0;
  s_cycled         = false;
}

void marketBegin()
{
  g_quoteMutex = xSemaphoreCreateMutex();

  Preferences prefs;
  prefs.begin("uptick", true);
  s_cgKey = prefs.getString("cgkey", "");
  prefs.end();
}

static void pollCharts(const Config& cfg, uint32_t now);

void marketPoll()
{
  if (s_paused) return;
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

  const uint32_t now = millis();

  // Only the chart layout needs candles, and it shows one feed at a time.
  // Every other layout shows prices for many feeds at once, which is what the
  // batch endpoints are for.
  if (cfg.mode == "single_chart") pollCharts(cfg, now);
  else                            pollBatch(cfg, now);
}

static void pollCharts(const Config& cfg, uint32_t now)
{
  const uint8_t feeds = cfg.feedCount ? cfg.feedCount : 1;

  // Poll the feed on screen, plus the one after it while that has no data.
  //
  // A feed nobody is looking at does not need fresh data, so the rest of the
  // list is left alone - that keeps the request rate at the rotation, not the
  // feed count times the poll interval. Refreshing the visible feed also means
  // the update lands while it is on screen, so a price moving is something you
  // actually see happen.
  //
  // The exception is a feed with nothing to show at all. Visible-only meant
  // each feed's first turn after boot, or after being added, opened on
  // "waiting for data" while its fetch ran; fetching it during the previous
  // feed's dwell brings it on screen with its chart already drawn. Once it has
  // data it goes back to refreshing on its own turn.
  //
  // Effective refresh is still the rotation period rather than the poll
  // interval: a feed that comes due just after leaving the screen waits until
  // it is next up again.
  const int visible = displayVisibleFeed();
  int upNext = (visible >= 0 && cfg.feedCount > 1) ? (visible + 1) % cfg.feedCount : -1;
  if (upNext >= 0)
  {
    QuoteLock lk;
    if (g_quotes[upNext].valid) upNext = -1;
  }

  // With one feed eligible there is nothing to space out, and a minimum gap
  // would be actively harmful - it could block the visible feed's only window
  // and strand it stale for a whole rotation. Spacing applies only when every
  // feed is a candidate.
  if (visible < 0)
  {
    const uint32_t minGap = (uint32_t)cfg.refreshSec * 1000UL / feeds;
    if (s_lastAnyFetchMs && (now - s_lastAnyFetchMs) < minGap) return;
  }

  // Refresh whichever eligible feed is most overdue, one per call. The visible
  // feed is checked first, so when both have never been fetched - the first
  // turn after boot - it wins the tie and the next one follows a moment later.
  int      pick   = -1;
  uint32_t oldest = 0;

  for (uint8_t n = 0; n < cfg.feedCount; n++)
  {
    const uint8_t i = (visible >= 0) ? (uint8_t)((visible + n) % cfg.feedCount) : n;
    if (visible >= 0 && (int)i != visible && (int)i != upNext) continue;

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
  s_lastAnyFetchMs      = now;
  if (!fetchFeed(cfg.feeds[pick], g_quotes[pick]))
  {
    QuoteLock lk;
    g_quotes[pick].failures++;
  }
}
