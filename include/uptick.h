#pragma once
#include <Arduino.h>

#define UPTICK_NAME      "uptick"
#define UPTICK_VERSION   "0.12.1"
#define UPTICK_SCHEMA    1
#define UPTICK_MAX_FEEDS 8

// One chart point per panel column, so nothing ever needs resampling.
#define UPTICK_MAX_POINTS 64

// ---------------------------------------------------------------- config

struct Feed
{
  String  source;    // "binance" or "coinbase"; a stock API later
  String  symbol;    // exchange-native symbol, e.g. "BTCUSDT"
  String  label;     // what the panel shows, e.g. "BTC"
  String  interval;  // candle width for the chart, e.g. "1h"
  uint8_t decimals;  // price decimals; 0 = pick from magnitude
};

struct Config
{
  uint8_t  schema;
  uint8_t  brightness;   // 1-100
  uint8_t  bitDepth;     // 3-6; applied at boot, Protomatter takes it in its ctor
  bool     rotate180;
  bool     wifiSleep;    // radio power save; off measurably steadies the panel
  String   mode;         // "single_chart" | "multi_price"
  uint16_t dwellSec;     // seconds per feed when cycling
  uint16_t refreshSec;   // market data poll interval, per feed
  String   tz;
  uint8_t  feedCount;
  Feed     feeds[UPTICK_MAX_FEEDS];
};

extern Config g_cfg;
extern SemaphoreHandle_t g_cfgMutex;

// Bumped on every change to g_cfg. Readers cache their own copy and only
// re-take it when this moves - copying Config means deep-copying ~40 Strings,
// which is not something the render loop should do 30 times a second.
extern volatile uint32_t g_cfgRev;

struct CfgLock
{
  CfgLock()  { xSemaphoreTake(g_cfgMutex, portMAX_DELAY); }
  ~CfgLock() { xSemaphoreGive(g_cfgMutex); }
};

void   configDefaults(Config& c);
bool   configBegin();
bool   configSave();
String configToJson();
bool   configFromJson(const String& body, String& err);

// ---------------------------------------------------------------- quotes

struct Quote
{
  bool     valid      = false;
  uint32_t fetchedMs  = 0;      // millis() at last success, 0 = never
  uint32_t failures   = 0;      // consecutive, reset on success
  float    price      = 0;
  float    changePct  = 0;      // across the charted window; drives chart colour
  float    change24h  = 0;      // fixed 24h window; what the panel prints
  bool     has24h     = false;  // false only when the source could not supply it
  uint16_t count      = 0;
  float    points[UPTICK_MAX_POINTS];
  float    lo         = 0;
  float    hi         = 0;
};

extern Quote g_quotes[UPTICK_MAX_FEEDS];
extern SemaphoreHandle_t g_quoteMutex;

// Never nest this inside CfgLock. Both readers copy config first, release,
// then take this - so the two locks are never held at once and cannot deadlock.
struct QuoteLock
{
  QuoteLock()  { xSemaphoreTake(g_quoteMutex, portMAX_DELAY); }
  ~QuoteLock() { xSemaphoreGive(g_quoteMutex); }
};

void marketBegin();

// Live kill switch for the fetcher, for isolating what disturbs the panel.
// Not persisted - a reset resumes fetching.
void marketSetPaused(bool paused);
bool marketPaused();

// CoinGecko demo key. Kept in NVS rather than config.json for the same reason
// the WiFi password is: /api/config is readable over plain HTTP on the LAN.
void   marketSetApiKey(const String& key);
bool   marketHasApiKey();
void marketPoll();   // non-blocking-ish; fetches at most one feed per call

// ---------------------------------------------------------------- net

struct NetState
{
  bool   apMode      = false;
  bool   provisioned = false;
  String ssid;
  String ip;
};
extern NetState g_net;

void netBegin();
void netTask(void* arg);            // pinned to core 0

// ---------------------------------------------------------------- display

bool displayBegin();
void displayTask(void* arg);        // pinned to core 1

// Bracket every flash write with these: they keep the renderer off the panel
// while LittleFS writes, which can otherwise starve the matrix DMA.
void displayPause();
void displayResume();

// Measured panel refresh, sampled once a second. A steady figure means the
// refresh ISR is running freely; dips are what visible flicker actually is.
uint16_t displayRefreshHz();

// Which feed is on screen, or -1 when the layout shows all of them (or none,
// as on the test screens). The fetcher uses this to poll only what is visible.
int displayVisibleFeed();

// Refresh stability, so flicker can be counted instead of watched for.
// peak is the best rate seen, low the worst, dips the number of one-second
// samples that came in under 90% of peak. Cores are reported to confirm the
// task split rather than assume it.
struct DisplayStats
{
  uint16_t hz;
  uint16_t low;
  uint16_t peak;
  uint32_t dips;
  uint8_t  taskCore;
  uint8_t  beginCore;
};
DisplayStats displayGetStats();
void         displayResetStats();

// Live palette override, for tuning colours against the real panel instead of
// guessing at RGB triples. Applied in RAM only - the web page remembers the
// picks and exports them as source once a set is settled on.
uint8_t     displayPaletteCount();
const char* displayPaletteName(uint8_t i);
void        displayPaletteGet(uint8_t i, uint8_t& r, uint8_t& g, uint8_t& b);
bool        displayPaletteSet(const char* name, uint8_t r, uint8_t g, uint8_t b);
void        displayPaletteReset();
