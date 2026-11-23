#pragma once
#include <Arduino.h>
#include <time.h>

#define UPTICK_NAME      "uptick"
#define UPTICK_VERSION   "0.30.0"
#define UPTICK_SCHEMA    1
#define UPTICK_MAX_FEEDS 32
#define UPTICK_MAX_CITIES 8
#define UPTICK_WX_DAYS    5

// One chart point per panel column, so nothing ever needs resampling.
#define UPTICK_MAX_POINTS 64

// ---------------------------------------------------------------- config

struct Feed
{
  String  source;    // "binance", "coinbase", "coingecko" or "yahoo"
  String  symbol;    // source-native symbol, e.g. "BTCUSDT", "bitcoin", "AAPL"
  String  label;     // what the panel shows, e.g. "BTC"
  String  interval;  // candle width for the chart, e.g. "1h"
  uint8_t decimals;  // price decimals; 0 = pick from magnitude
};

// A place to show the weather for. Coordinates come from the web page's city
// search; name is what the panel shows and can be edited.
struct City
{
  String name;
  float  lat;
  float  lon;
};

struct Config
{
  uint8_t  schema;
  uint8_t  brightness;   // 1-100, applied as OE duty cycle
  bool     rotate180;
  bool     wifiSleep;    // radio power save; off keeps fetching steadier
  String   mode;         // "single_chart" | "ticker" | "ticker_vertical"
  String   theme;        // "none" | "snow"; an animated layer behind the feeds
  uint16_t dwellSec;     // seconds per feed when cycling
  uint8_t  scrollSpeed;  // both tickers, pixels per second
  uint8_t  pauseSec;     // vertical ticker: hold at each entry, 0 = continuous
  uint16_t refreshSec;   // market data poll interval, per feed
  uint8_t  feedCount;
  Feed     feeds[UPTICK_MAX_FEEDS];

  // What the panel rotates through, in this order. With the tickers on, the
  // weather and the clock play after every full pass of them; without, the
  // others take turns, and one on its own stays up.
  bool     showTickers;
  bool     showWeather;
  bool     showClock;
  String   weatherUnits; // "c" | "f"
  uint8_t  weatherSec;   // seconds each weather screen stays up
  uint8_t  cityCount;
  City     cities[UPTICK_MAX_CITIES];

  String   clockTheme;   // "matrix"
  uint16_t clockSec;     // seconds the clock stays up in the rotation
  uint8_t  clockCity;    // whose local time it shows, from cities; UTC with none
};

extern Config g_cfg;
extern SemaphoreHandle_t g_cfgMutex;

// Bumped on every change to g_cfg. Readers cache their own copy and only
// re-take it when this moves - copying Config means deep-copying ~40 Strings,
// which is not something the render loop should do 30 times a second.
extern volatile uint32_t g_cfgRev;

// Null-tolerant on purpose. These run during setup() as well as from the
// tasks, and a handle that has not been created yet would otherwise block
// forever on a null semaphore - a dark panel and no serial output past the
// banner. Nothing is concurrent before the tasks are started, so skipping the
// take there is safe rather than merely convenient.
struct CfgLock
{
  CfgLock()  { if (g_cfgMutex) xSemaphoreTake(g_cfgMutex, portMAX_DELAY); }
  ~CfgLock() { if (g_cfgMutex) xSemaphoreGive(g_cfgMutex); }
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

  // Quote currency. The crypto sources are all USD pairs; Yahoo is not, so a
  // Frankfurt or London listing must not be printed with a "$" in front of it.
  // A fixed array rather than a String because the render task copies the
  // whole Quote every frame and must not allocate to do it.
  char     ccy[8]     = "USD";

  float    points[UPTICK_MAX_POINTS];
  float    lo         = 0;
  float    hi         = 0;

  // Yahoo chart fetches only; all zero otherwise, and for crypto, which never
  // closes. The exchange's pre, regular and post sessions for the current
  // trading day as Unix start/end pairs, when the price shown last traded, and
  // which session it came from. The display works out from these whether the
  // market is open, and so which icon, if any, goes beside the price.
  uint32_t sess[3][2] = {};
  uint32_t tradeTs    = 0;
  uint8_t  session    = 0;      // SESSION_*
};

enum : uint8_t { SESSION_REGULAR, SESSION_PRE, SESSION_POST };

// True once SNTP has set the clock. Before that time() counts from 1970, and
// nothing that compares against an exchange's hours can be trusted.
inline bool clockValid() { return time(nullptr) > 1700000000; }

extern Quote g_quotes[UPTICK_MAX_FEEDS];
extern SemaphoreHandle_t g_quoteMutex;

// Never nest this inside CfgLock. Both readers copy config first, release,
// then take this - so the two locks are never held at once and cannot deadlock.
struct QuoteLock
{
  QuoteLock()  { if (g_quoteMutex) xSemaphoreTake(g_quoteMutex, portMAX_DELAY); }
  ~QuoteLock() { if (g_quoteMutex) xSemaphoreGive(g_quoteMutex); }
};

void marketBegin();

// Live switch that stops all market fetching. Not persisted - a restart
// resumes it.
void marketSetPaused(bool paused);
bool marketPaused();

// CoinGecko demo key. Kept in NVS rather than config.json for the same reason
// the WiFi password is: /api/config is readable over plain HTTP on the LAN.
void   marketSetApiKey(const String& key);
bool   marketHasApiKey();
void marketPoll();   // non-blocking-ish; fetches at most one feed per call

// Drops the per-feed attempt timers, which are indexed by position and so are
// meaningless once the feed list has been reordered or edited.
void marketConfigChanged();

// Yahoo sends no CORS header, so the web page cannot validate a ticker itself
// the way it does against the exchange APIs. Blocking, ~1 second; only ever
// called from the "check" button.
bool marketProbeYahoo(const String& symbol, float& price, String& name, String& currency);

// Which of these Yahoo tickers exist, for validating a feed list before it is
// saved. Spark answers twenty at a time and leaves out any it does not know,
// so this is one request per twenty tickers rather than one each. False only
// when Yahoo could not be asked.
bool marketCheckYahoo(const String* syms, uint8_t n, bool* found);

// ---------------------------------------------------------------- weather

// One day of a city's forecast. Temperatures are rounded to whole degrees in
// the configured unit; code is the WMO weather code Open-Meteo reports.
struct WeatherDay
{
  uint8_t code;
  int8_t  hi;
  int8_t  lo;
  uint8_t dow;              // 0 = Sunday, in the city's own time zone
};

struct CityWeather
{
  bool       valid     = false;
  uint32_t   fetchedMs = 0;
  float      temp      = 0;  // now, in the configured unit
  uint8_t    code      = 0;  // now, WMO code
  bool       isDay     = true;
  int32_t    utcOffset = 0;  // seconds, the city's own
  int16_t    sunrise   = -1; // today's, minutes after local midnight; -1 unknown
  int16_t    sunset    = -1;
  uint8_t    days      = 0;
  WeatherDay day[UPTICK_WX_DAYS];
};

extern CityWeather       g_weather[UPTICK_MAX_CITIES];
extern SemaphoreHandle_t g_weatherMutex;

// Null-tolerant, like the other two.
struct WeatherLock
{
  WeatherLock()  { if (g_weatherMutex) xSemaphoreTake(g_weatherMutex, portMAX_DELAY); }
  ~WeatherLock() { if (g_weatherMutex) xSemaphoreGive(g_weatherMutex); }
};

void weatherBegin();

// Net task. Fetches every city in one request: every 15 minutes, a minute
// after a failure, and straight away when the cities or the units change -
// which it notices itself, so config code does not have to tell it.
void weatherPoll();

// Shared with the weather fetch: the same TLS setup every market request uses.
class WiFiClientSecure;
class HTTPClient;
void marketPrepareClient(WiFiClientSecure& client, HTTPClient& https);

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

// Keeps the renderer from drawing while held. The panel's refresh no longer
// needs protecting - it is a circular DMA chain in internal RAM and runs
// through flash writes untouched - but config.cpp still uses this to swap the
// config and the quotes without a frame landing in between.
// The clock's themes, by config name ("matrix", "flip", ...), so config
// validation and the display share one list.
uint8_t     displayClockThemeCount();
const char* displayClockThemeName(uint8_t i);

// Sends Santa across the panel on the next frame, if the snow theme is on.
// Otherwise he flies whenever it next is, a few minutes after it comes on.
void displaySantaNow();

void displayPause();
void displayResume();

// Panel refresh rate. Computed by the driver from the clock, depth and scan
// rather than sampled: with no CPU in the refresh loop there is nothing to
// vary, so a measured figure would only ever read back this same number.
uint16_t displayRefreshHz();

// Which feed is on screen, or -1 when the layout shows all of them (or none,
// as on the test screens). The fetcher uses this to poll only what is visible.
int displayVisibleFeed();

// Cores are reported to confirm the task split rather than assume it.
// stackFree is the least free stack the render task has ever had, in bytes.
// tickerUs is how long the last ticker frame took to draw, against a budget of
// 1000000 / scrollSpeed - an overrun would quietly slow the crawl.
struct DisplayStats
{
  uint16_t hz;
  uint8_t  depth;
  uint8_t  taskCore;
  uint32_t stackFree;
  uint32_t tickerUs;
};
DisplayStats displayGetStats();

// The panel's colours, by name. Set applies at once and is RAM only - the
// pickers use it for live preview. What persists is the config: configToJson
// stores every colour that differs from factory, and configFromJson applies
// them, so Save is what keeps a palette.
uint8_t     displayPaletteCount();
const char* displayPaletteName(uint8_t i);
void        displayPaletteGet(uint8_t i, uint8_t& r, uint8_t& g, uint8_t& b);
void        displayPaletteFactory(uint8_t i, uint8_t& r, uint8_t& g, uint8_t& b);
bool        displayPaletteSet(const char* name, uint8_t r, uint8_t g, uint8_t b);
void        displayPaletteReset();
