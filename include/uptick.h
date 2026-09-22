#pragma once
#include <Arduino.h>

#define UPTICK_NAME      "uptick"
#define UPTICK_VERSION   "0.3.1"
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
  bool     rotate180;
  String   mode;         // "single_chart" | "multi_price"
  String   changeWindow; // "chart" = across the charted window, "24h" = fixed
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
  float    changePct  = 0;      // across the charted window; always available
  float    change24h  = 0;      // fixed 24h window
  bool     has24h     = false;  // only populated when the setting asks for it
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
