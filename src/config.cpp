#include "uptick.h"
#include "pins.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

static const char* CFG_PATH = "/config.json";

Config g_cfg;
SemaphoreHandle_t g_cfgMutex = nullptr;
volatile uint32_t g_cfgRev   = 1;

void configDefaults(Config& c)
{
  c.schema     = UPTICK_SCHEMA;
  c.brightness = PANEL_BRIGHTNESS_DEFAULT;
  c.rotate180  = false;
  // Default off: measured on this panel, power save costs a few percent of
  // refresh stability, and the ~40 mA it saves is irrelevant on mains power.
  c.wifiSleep  = false;
  c.mode         = "single_chart";
  c.theme        = "none";
  c.dwellSec   = 15;
  c.scrollSpeed = SCROLL_PPS_DEFAULT;
  c.pauseSec    = PAUSE_SEC_DEFAULT;
  c.refreshSec = POLL_SEC_DEFAULT;
  // Factory feeds. decimals 0 means "pick from magnitude", which gives whole
  // dollars above 1000 and cents below, so it suits all four.
  c.feedCount  = 4;
  c.feeds[0]   = { "binance", "BTCUSDT", "BTC", "1h", 0 };
  c.feeds[1]   = { "binance", "ETHUSDT", "ETH", "1h", 0 };
  c.feeds[2]   = { "binance", "SOLUSDT", "SOL", "1h", 0 };
  c.feeds[3]   = { "binance", "ZECUSDT", "ZEC", "1h", 0 };
}

static void feedToJson(const Feed& f, JsonObject o)
{
  o["source"]   = f.source;
  o["symbol"]   = f.symbol;
  o["label"]    = f.label;
  o["interval"] = f.interval;
  o["decimals"] = f.decimals;
}

// The label a feed gets when none is given. Binance and Coinbase symbols carry
// their quote currency - BTCUSDT, BTC-USD - which only costs width on a 64 px
// panel when it is a dollar, so a dollar quote is dropped; any other quote
// stays, since "ETH" over a price in bitcoin would misrepresent it. Stock
// symbols and CoinGecko ids are used as they are.
//
// Mirrors autoLabel() in the web page, which fills labels before it saves.
// This copy covers a config POSTed to /api/config directly.
static String autoLabel(const Feed& f)
{
  static const char* const USD_QUOTES[] = { "FDUSD", "USDT", "USDC", "BUSD", "TUSD", "USD" };

  if (f.source == "binance")
  {
    for (const char* q : USD_QUOTES)
    {
      const unsigned n = strlen(q);
      if (f.symbol.length() > n && f.symbol.endsWith(q))
        return f.symbol.substring(0, f.symbol.length() - n);
    }
  }

  if (f.source == "coinbase")
  {
    const int dash = f.symbol.indexOf('-');
    if (dash > 0)
    {
      const String quote = f.symbol.substring(dash + 1);
      for (const char* q : USD_QUOTES)
        if (quote == q) return f.symbol.substring(0, dash);
    }
  }

  return f.symbol;
}

static void feedFromJson(JsonObjectConst o, Feed& f)
{
  f.source   = o["source"]   | "binance";
  f.symbol   = o["symbol"]   | "BTCUSDT";
  f.label    = o["label"]    | "";
  f.interval = o["interval"] | "1h";
  f.decimals = o["decimals"] | 0;
  if (f.label.isEmpty()) f.label = autoLabel(f);
}

// The palette is stored as the colours that differ from factory, by name -
// "LABEL": "#eeff00". Only those, so a colour nobody changed follows the
// factory set if that is ever retuned. The palette itself lives in display.cpp;
// the config only carries it to and from flash.
static void paletteToJson(JsonObject o)
{
  for (uint8_t i = 0; i < displayPaletteCount(); i++)
  {
    uint8_t r, g, b, fr, fg, fb;
    displayPaletteGet(i, r, g, b);
    displayPaletteFactory(i, fr, fg, fb);
    if (r == fr && g == fg && b == fb) continue;

    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", r, g, b);
    o[displayPaletteName(i)] = hex;
  }
}

static bool parseHex(const char* s, uint8_t& r, uint8_t& g, uint8_t& b)
{
  if (!s || s[0] != '#' || strlen(s) != 7) return false;
  char* end = nullptr;
  const unsigned long v = strtoul(s + 1, &end, 16);
  if (*end) return false;
  r = (uint8_t)(v >> 16);
  g = (uint8_t)(v >> 8);
  b = (uint8_t)v;
  return true;
}

// A document with no palette means factory, so what is on flash always says
// exactly what the panel shows. Unknown names and malformed colours are
// skipped rather than refused - they cannot be worth losing a config over.
static void paletteFromJson(JsonObjectConst o)
{
  displayPaletteReset();
  for (JsonPairConst kv : o)
  {
    uint8_t r, g, b;
    if (parseHex(kv.value().as<const char*>(), r, g, b))
      displayPaletteSet(kv.key().c_str(), r, g, b);
  }
}

String configToJson()
{
  JsonDocument d;
  {
    CfgLock lk;
    d["schema"]     = g_cfg.schema;
    d["brightness"] = g_cfg.brightness;
    d["rotate180"]  = g_cfg.rotate180;
    d["wifiSleep"]  = g_cfg.wifiSleep;
    d["mode"]         = g_cfg.mode;
    d["theme"]        = g_cfg.theme;
    d["dwellSec"]   = g_cfg.dwellSec;
    d["scrollSpeed"] = g_cfg.scrollSpeed;
    d["pauseSec"]    = g_cfg.pauseSec;
    d["refreshSec"] = g_cfg.refreshSec;

    JsonArray feeds = d["feeds"].to<JsonArray>();
    for (uint8_t i = 0; i < g_cfg.feedCount; i++)
      feedToJson(g_cfg.feeds[i], feeds.add<JsonObject>());
  }
  paletteToJson(d["palette"].to<JsonObject>());

  String out;
  serializeJson(d, out);
  return out;
}

bool configFromJson(const String& body, String& err)
{
  JsonDocument d;
  DeserializationError e = deserializeJson(d, body);
  if (e) { err = e.c_str(); return false; }

  // Validate into a scratch copy first so a bad POST cannot leave the live
  // config half-written while the render task is reading it.
  Config c;
  configDefaults(c);

  c.schema     = UPTICK_SCHEMA;
  // "bitDepth" and "tz" keys from an older config are ignored rather than
  // rejected: depth is fixed by the build now, and the timezone was never used
  // for anything. Refusing the whole document over either would lose
  // everything else in a backup restore.
  c.brightness = constrain((int)(d["brightness"] | PANEL_BRIGHTNESS_DEFAULT), 1, 100);
  c.rotate180  = d["rotate180"] | false;
  c.wifiSleep  = d["wifiSleep"] | false;
  c.mode         = d["mode"]         | "single_chart";
  // An unknown theme falls back to none rather than failing the document - a
  // backup from a later version that has more themes should still restore.
  c.theme        = d["theme"]        | "none";
  if (c.theme != "snow") c.theme = "none";
  c.dwellSec   = constrain((int)(d["dwellSec"]   | 15), 3, 3600);
  c.scrollSpeed = constrain((int)(d["scrollSpeed"] | SCROLL_PPS_DEFAULT), SCROLL_PPS_MIN, SCROLL_PPS_MAX);
  c.pauseSec    = constrain((int)(d["pauseSec"] | PAUSE_SEC_DEFAULT), 0, PAUSE_SEC_MAX);
  c.refreshSec = constrain((int)(d["refreshSec"] | POLL_SEC_DEFAULT), 10, 3600);

  // "multi_price" is what the vertical ticker was called before it scrolled.
  // A stored config or backup that still says so is translated, not refused -
  // refusing it at boot would throw the whole document away, feeds included.
  if (c.mode == "multi_price") c.mode = "ticker_vertical";

  if (c.mode != "single_chart" && c.mode != "ticker" && c.mode != "ticker_vertical" &&
      c.mode != "palette_test" && c.mode != "ramp_test") {
    err = "unknown mode";
    return false;
  }

  JsonArrayConst feeds = d["feeds"].as<JsonArrayConst>();
  if (feeds.isNull() || feeds.size() == 0) { err = "at least one feed required"; return false; }
  if (feeds.size() > UPTICK_MAX_FEEDS)     { err = "too many feeds";            return false; }

  c.feedCount = 0;
  for (JsonObjectConst o : feeds) {
    Feed f;
    feedFromJson(o, f);
    if (f.symbol.isEmpty()) { err = "feed with empty symbol"; return false; }
    c.feeds[c.feedCount++] = f;
  }

  // Quotes are indexed by feed position, so reordering or deleting a feed
  // would otherwise leave the previous occupant's price sitting under the new
  // feed's label until the next fetch - BTC's number under ETH's name. Each
  // quote is carried across to wherever its feed moved to, matched on source
  // and symbol; anything unmatched starts empty and refetches on its turn.
  //
  // Bracketed by displayPause because the config and the quotes are behind
  // separate mutexes that must never be held together, so there is no way to
  // swap both atomically - pausing means no frame can observe the moment they
  // disagree. Note this must be released before configSave(), which pauses
  // again and would deadlock on a non-recursive mutex.
  //
  // The scratch copy is heap-allocated: Quote is ~300 bytes and eight of them
  // would be a third of the network task's 8 KB stack.
  Quote* prev = (Quote*)malloc(sizeof(Quote) * UPTICK_MAX_FEEDS);

  displayPause();
  {
    String  prevSrc[UPTICK_MAX_FEEDS], prevSym[UPTICK_MAX_FEEDS];
    uint8_t prevCount;

    {
      CfgLock lk;
      prevCount = g_cfg.feedCount;
      for (uint8_t i = 0; i < prevCount; i++)
      {
        prevSrc[i] = g_cfg.feeds[i].source;
        prevSym[i] = g_cfg.feeds[i].symbol;
      }
      g_cfg = c;
      g_cfgRev++;
    }

    {
      QuoteLock lk;
      if (prev)
      {
        for (uint8_t i = 0; i < UPTICK_MAX_FEEDS; i++) prev[i] = g_quotes[i];
        for (uint8_t i = 0; i < UPTICK_MAX_FEEDS; i++) g_quotes[i] = Quote();

        for (uint8_t i = 0; i < c.feedCount; i++)
          for (uint8_t j = 0; j < prevCount; j++)
            if (prevSrc[j] == c.feeds[i].source && prevSym[j] == c.feeds[i].symbol)
            {
              g_quotes[i] = prev[j];
              break;
            }
      }
      else
      {
        // Out of memory is not a reason to show a wrong price. Clearing costs
        // one refetch per feed and is always truthful.
        for (uint8_t i = 0; i < UPTICK_MAX_FEEDS; i++) g_quotes[i] = Quote();
      }
    }
  }
  displayResume();

  free(prev);

  // The poller indexes its attempt timers by position too, so they are reset
  // rather than carried: the worst case is one early refetch, against a feed
  // that never comes due because it inherited a recent timestamp.
  marketConfigChanged();

  paletteFromJson(d["palette"].as<JsonObjectConst>());

  // Applied to RAM regardless - the panel and the poller pick it up either
  // way. Only persistence can fail here, and saying so is the difference
  // between "my settings reset" and a fixable error.
  if (!configSave())
  {
    err = "settings applied but could not be written to flash";
    return false;
  }
  return true;
}

bool configSave()
{
  String json = configToJson();

  // Write to a temp file and rename, so a reset mid-write cannot leave a
  // truncated config.json behind. The panel is held off across the whole
  // sequence, because a flash write can starve the matrix DMA.
  //
  // Every exit from here must resume, including the failure paths: pause takes
  // the panel mutex, so leaking it freezes the render task forever and the
  // display stops updating until a reset.
  displayPause();

  size_t n  = 0;
  bool   ok = false;

  File f = LittleFS.open("/config.tmp", "w");
  if (!f)
  {
    Serial.println("[cfg] open for write failed");
  }
  else
  {
    n = f.print(json);
    f.close();

    if (n != json.length())
    {
      Serial.println("[cfg] short write");
    }
    else
    {
      LittleFS.remove(CFG_PATH);
      ok = LittleFS.rename("/config.tmp", CFG_PATH);
      if (!ok) Serial.println("[cfg] rename failed");
    }
  }

  displayResume();

  if (ok) Serial.printf("[cfg] saved %u bytes\n", (unsigned)n);
  return ok;
}

bool configBegin()
{
  g_cfgMutex = xSemaphoreCreateMutex();
  configDefaults(g_cfg);

  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) {
    Serial.println("[cfg] no config.json, writing defaults");
    return configSave();
  }

  String body = f.readString();
  f.close();

  JsonDocument probe;
  if (deserializeJson(probe, body)) {
    Serial.println("[cfg] config.json is corrupt, falling back to defaults");
    return configSave();
  }

  // Migration hook: schema bumps land here rather than silently loading a
  // shape the current firmware no longer understands.
  uint8_t schema = probe["schema"] | 0;
  if (schema != UPTICK_SCHEMA) {
    Serial.printf("[cfg] schema %u != %u, rewriting defaults\n", schema, UPTICK_SCHEMA);
    return configSave();
  }

  String err;
  if (!configFromJson(body, err)) {
    Serial.printf("[cfg] rejected stored config (%s), using defaults\n", err.c_str());
    configDefaults(g_cfg);
    return configSave();
  }

  Serial.printf("[cfg] loaded, %u feed(s)\n", g_cfg.feedCount);
  return true;
}
