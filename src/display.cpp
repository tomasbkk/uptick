#include "uptick.h"
#include "pins.h"
#include <Adafruit_Protomatter.h>
#include <Fonts/TomThumb.h>

// Heap-allocated rather than a static global, because Protomatter takes the
// colour depth as a constructor argument and depth is a user setting. It is
// built once in displayBegin() from the stored config; changing the setting
// restarts the device rather than tearing down a live DMA chain.
static Adafruit_Protomatter* matrix = nullptr;

static bool              s_ok         = false;
static SemaphoreHandle_t s_panelMutex = nullptr;
static uint8_t           s_brightness = 40;
static uint16_t          s_refreshHz  = 0;
static uint16_t          s_refreshLow = 0xFFFF;
static uint16_t          s_refreshPk  = 0;
static uint32_t          s_refreshDip = 0;
static uint8_t           s_taskCore   = 0xFF;
static uint8_t           s_beginCore  = 0xFF;

uint16_t displayRefreshHz() { return s_refreshHz; }

// -1 means "no single feed is on screen": either the layout shows all of them
// at once, or it is a test screen showing none. The fetcher treats that as
// permission to poll any feed.
static volatile int8_t s_visibleFeed = -1;

int displayVisibleFeed() { return s_visibleFeed; }

DisplayStats displayGetStats()
{
  DisplayStats st;
  st.hz        = s_refreshHz;
  st.low       = (s_refreshLow == 0xFFFF) ? 0 : s_refreshLow;
  st.peak      = s_refreshPk;
  st.dips      = s_refreshDip;
  st.taskCore  = s_taskCore;
  st.beginCore = s_beginCore;
  return st;
}

void displayResetStats()
{
  s_refreshLow = 0xFFFF;
  s_refreshPk  = 0;
  s_refreshDip = 0;
}

// Everything that can change what is on screen. Compared with memcmp, so it is
// a plain POD with no padding surprises - zero it before filling.
struct FrameKey
{
  uint32_t cfgRev;
  uint32_t palRev;
  uint32_t fetchedMs;
  uint8_t  feedIdx;
  uint8_t  apMode;
  uint8_t  showInfo;
  uint8_t  stale;
  uint8_t  valid;
  uint8_t  pad[3];
};

// Writing to flash on the S3 can starve the matrix DMA and glitch a frame, so
// config saves bracket the write with these to keep show() - which rebuilds
// bitplanes and touches DMA descriptors - from running at the same moment.
//
// These deliberately do NOT call matrix->stop() / matrix->resume(). On the S3,
// stop() kills the refresh and resume() does not bring it back when the pair
// is called from a different task than the one driving the panel: the display
// goes black and stays black until reset. Holding the mutex is enough. Any
// corruption that does slip through is one frame and the next redraw fixes it.
void displayPause()
{
  if (!s_ok) return;
  xSemaphoreTake(s_panelMutex, portMAX_DELAY);
}

void displayResume()
{
  if (!s_ok) return;
  xSemaphoreGive(s_panelMutex);
}

// ---------------------------------------------------------------- colour

// Protomatter has no brightness control, so brightness is a scale applied to
// every colour before it is drawn. The 200 cap means "white" is never actually
// 255,255,255 - indistinguishable at this pixel pitch, and it keeps the worst
// case current well clear of what a 3 A supply will give up.
static const uint8_t MAX_COMPONENT = 200;

// The slider is perceptual, the panel is linear, and the 565 conversion throws
// away anything below 8/255 on red and blue. Scaling straight by percent made
// the bottom of the slider render pure black - at which point you cannot see
// the panel well enough to drag it back up, and a reset is the only way out.
//
// So the 1-100 slider maps onto an 8-100 output range with a square curve:
// the curve gives it a natural feel, the floor guarantees the darkest setting
// is still legible.
static const uint8_t MIN_SCALE = 8;

static uint16_t col(uint8_t r, uint8_t g, uint8_t b)
{
  const uint8_t  pct    = s_brightness < 1 ? 1 : s_brightness;
  const uint32_t curved = (uint32_t)pct * pct;                          // 1..10000
  const uint8_t  scale  = MIN_SCALE + (uint8_t)((curved * (100 - MIN_SCALE)) / 10000);

  auto s = [scale](uint8_t v) -> uint8_t
  {
    uint16_t capped = v < MAX_COMPONENT ? v : MAX_COMPONENT;
    return (uint8_t)((uint32_t)capped * scale / 100);
  };
  return matrix->color565(s(r), s(g), s(b));
}

// The palette, as 8-bit RGB before brightness scaling. Order must match
// PAL_NAMES, and the panel test screen draws them in this order.
struct Rgb { uint8_t r, g, b; };

enum PalIdx {
  P_LABEL, P_PRICE, P_PCT_UP, P_PCT_DOWN,
  P_FILL_UP, P_TRACE_UP, P_FILL_DOWN, P_TRACE_DOWN,
  P_DIM, P_ACCENT, P_STALE,
  P_COUNT
};

static const char* const PAL_NAMES[P_COUNT] = {
  "LABEL", "PRICE", "PCT_UP", "PCT_DOWN",
  "FILL_UP", "TRACE_UP", "FILL_DOWN", "TRACE_DOWN",
  "DIM", "ACCENT", "STALE"
};

static const Rgb PAL_FACTORY[P_COUNT] = {
  { 255, 255, 255 },   // LABEL      - symbol, top left
  { 255, 255, 255 },   // PRICE      - price line
  {  40, 220,  90 },   // PCT_UP     - percentage when rising
  { 240,  70,  60 },   // PCT_DOWN   - percentage when falling
  {  40, 220,  90 },   // FILL_UP    - chart fill when rising
  {  90, 255, 150 },   // TRACE_UP   - chart trace when rising
  { 240,  70,  60 },   // FILL_DOWN  - chart fill when falling
  { 255, 150, 140 },   // TRACE_DOWN - chart trace when falling
  { 110, 120, 130 },   // DIM        - secondary text
  {  60, 140, 255 },   // ACCENT     - setup screens
  { 230, 160,  40 }    // STALE      - label when data is old
};

static Rgb      s_pal[P_COUNT];
static uint32_t s_palRev = 1;

uint8_t     displayPaletteCount()          { return P_COUNT; }
const char* displayPaletteName(uint8_t i)  { return i < P_COUNT ? PAL_NAMES[i] : ""; }

void displayPaletteGet(uint8_t i, uint8_t& r, uint8_t& g, uint8_t& b)
{
  if (i >= P_COUNT) { r = g = b = 0; return; }
  r = s_pal[i].r; g = s_pal[i].g; b = s_pal[i].b;
}

bool displayPaletteSet(const char* name, uint8_t r, uint8_t g, uint8_t b)
{
  for (uint8_t i = 0; i < P_COUNT; i++)
  {
    if (strcmp(name, PAL_NAMES[i]) != 0) continue;
    s_pal[i] = { r, g, b };
    s_palRev++;
    return true;
  }
  return false;
}

void displayPaletteReset()
{
  memcpy(s_pal, PAL_FACTORY, sizeof(s_pal));
  s_palRev++;
}

static uint16_t P(uint8_t i) { return col(s_pal[i].r, s_pal[i].g, s_pal[i].b); }

static uint16_t C_LABEL()      { return P(P_LABEL);      }
static uint16_t C_PRICE()      { return P(P_PRICE);      }
static uint16_t C_PCT_UP()     { return P(P_PCT_UP);     }
static uint16_t C_PCT_DOWN()   { return P(P_PCT_DOWN);   }
static uint16_t C_FILL_UP()    { return P(P_FILL_UP);    }
static uint16_t C_TRACE_UP()   { return P(P_TRACE_UP);   }
static uint16_t C_FILL_DOWN()  { return P(P_FILL_DOWN);  }
static uint16_t C_TRACE_DOWN() { return P(P_TRACE_DOWN); }
static uint16_t C_DIM()        { return P(P_DIM);        }
static uint16_t C_ACCENT()     { return P(P_ACCENT);     }
static uint16_t C_STALE()      { return P(P_STALE);      }

// ---------------------------------------------------------------- text

// The built-in GLCD font positions from the glyph top-left; custom GFX fonts
// position from the baseline. Both helpers take a top edge so callers can do
// plain row arithmetic and not think about it.
static const int16_t BIG_W = 6, BIG_H = 7;
static const int16_t TINY_W = 4, TINY_H = 5;

static int16_t bigWidth(const char* s)  { return (int16_t)strlen(s) * BIG_W - 1; }
static int16_t tinyWidth(const char* s) { return (int16_t)strlen(s) * TINY_W - 1; }

static void drawBig(int16_t x, int16_t yTop, const char* s, uint16_t c)
{
  matrix->setFont(NULL);
  matrix->setTextColor(c);
  matrix->setCursor(x, yTop);
  matrix->print(s);
}

static void drawTiny(int16_t x, int16_t yTop, const char* s, uint16_t c)
{
  matrix->setFont(&TomThumb);
  matrix->setTextColor(c);
  matrix->setCursor(x, yTop + TINY_H);
  matrix->print(s);
  matrix->setFont(NULL);
}

// ---------------------------------------------------------------- numbers

// Inserts thousands separators into the integer part in place, leaving any
// currency prefix and decimal tail alone. No-op when there is no room.
static void addThousands(char* s, size_t cap)
{
  char* intStart = s;
  while (*intStart && !isdigit((unsigned char)*intStart)) intStart++;

  char* intEnd = intStart;
  while (isdigit((unsigned char)*intEnd)) intEnd++;

  const int digits = (int)(intEnd - intStart);
  const int commas = (digits - 1) / 3;
  if (commas <= 0) return;

  const size_t len = strlen(s);
  if (len + commas + 1 > cap) return;

  // Shift the decimal tail (and the terminator) right to make room.
  memmove(intEnd + commas, intEnd, len - (size_t)(intEnd - s) + 1);

  char* src   = intEnd - 1;
  char* dst   = intEnd + commas - 1;
  int   count = 0;
  while (src >= intStart)
  {
    *dst-- = *src--;
    if (++count % 3 == 0 && src >= intStart) *dst-- = ',';
  }
}

static void formatPrice(char* out, size_t n, float p, uint8_t decimals)
{
  uint8_t d = decimals;
  if (d == 0)
  {
    if      (p >= 1000.0f) d = 0;
    else if (p >= 1.0f)    d = 2;
    else                   d = 4;
  }
  snprintf(out, n, "$%.*f", d, p);
  addThousands(out, n);
}

static void formatPriceShort(char* out, size_t n, float p)
{
  if      (p >= 1000000.0f) snprintf(out, n, "$%.2fM", p / 1000000.0f);
  else if (p >= 1000.0f)    snprintf(out, n, "$%.1fK", p / 1000.0f);
  else                      snprintf(out, n, "$%.2f",  p);
}

// Ladder: full precision in the readable font, then the small font, then
// abbreviate. "$214,460.00" is 66 px in the 5x7 font - one pixel too wide for
// the panel - so this is the difference between a price and a truncated price.
static void drawPriceLine(int16_t yTop, float price, uint8_t decimals, uint16_t c)
{
  char buf[24];
  formatPrice(buf, sizeof(buf), price, decimals);

  if (bigWidth(buf) <= PANEL_W)  { drawBig(0, yTop, buf, c); return; }
  if (tinyWidth(buf) <= PANEL_W) { drawTiny(0, yTop + 1, buf, c); return; }

  formatPriceShort(buf, sizeof(buf), price);
  if (bigWidth(buf) <= PANEL_W) drawBig(0, yTop, buf, c);
  else                          drawTiny(0, yTop + 1, buf, c);
}

// ---------------------------------------------------------------- chart

// Solid fill in the trend colour, under a lighter trace along its top edge.
static void drawChart(const Quote& q, int16_t yTop, int16_t yBot,
                      uint16_t fill, uint16_t trace)
{
  if (q.count < 2) return;

  const int16_t span = yBot - yTop;

  int16_t colY[PANEL_W];
  for (int16_t x = 0; x < PANEL_W; x++)
  {
    // Resample onto the panel width. With limit=64 from the exchange this is
    // a straight 1:1 copy, but a short response still fills the width.
    float t    = (float)x * (q.count - 1) / (float)(PANEL_W - 1);
    int   i    = (int)t;
    float frac = t - i;

    float v = (i + 1 < q.count) ? q.points[i] + (q.points[i + 1] - q.points[i]) * frac
                                : q.points[q.count - 1];

    // Scale to the visible window, never to zero, or intraday moves render as
    // a flat line across the bottom.
    float norm = (q.hi > q.lo) ? (v - q.lo) / (q.hi - q.lo) : 0.5f;
    colY[x] = yBot - (int16_t)lroundf(norm * span);
  }

  // Fill first, then lay the trace over it.
  for (int16_t x = 0; x < PANEL_W; x++)
    matrix->drawLine(x, colY[x], x, yBot, fill);

  // The trace runs along the fill's top edge as a staircase, never as a line
  // between column centres. drawLine() from (x-1, colY[x-1]) to (x, colY[x])
  // spreads a steep move across both columns, so half of it lands above the
  // previous column's fill as stray pixels floating over the chart.
  //
  // Instead each vertical step is drawn in whichever column's fill actually
  // covers those rows: if this column is taller the step is ours, if the
  // previous one was taller the step belongs to it.
  for (int16_t x = 0; x < PANEL_W; x++)
  {
    matrix->drawPixel(x, colY[x], trace);

    if (x == 0) continue;

    if (colY[x] < colY[x - 1])
      matrix->drawLine(x, colY[x], x, colY[x - 1], trace);
    else if (colY[x] > colY[x - 1])
      matrix->drawLine(x - 1, colY[x - 1], x - 1, colY[x], trace);
  }
}

// ---------------------------------------------------------------- screens

static void drawSetup()
{
  matrix->fillScreen(0);
  drawTiny(0,  1, "SETUP - JOIN", C_ACCENT());
  drawTiny(0,  9, g_net.ssid.c_str(), C_LABEL());
  drawTiny(0, 17, "THEN OPEN", C_DIM());
  drawTiny(0, 25, g_net.ip.c_str(), C_LABEL());
}

// Shown briefly after a successful join. The panel is the one part of this
// device that is always visible, so it should answer "what address is it on"
// without needing a serial cable or the router's admin page.
static void drawJoined()
{
  matrix->fillScreen(0);
  drawTiny(0,  1, "CONNECTED",          C_ACCENT());
  drawTiny(0,  9, g_net.ssid.c_str(),   C_DIM());
  drawTiny(0, 17, g_net.ip.c_str(),     C_LABEL());
  drawTiny(0, 25, UPTICK_NAME ".local", C_DIM());
}

static void drawWaiting(const Feed& f)
{
  matrix->fillScreen(0);
  drawBig(0, 0, f.label.c_str(), C_LABEL());
  drawTiny(0, 12, "WAITING FOR", C_DIM());
  drawTiny(0, 20, "DATA", C_DIM());
}

// The panel always prints the 24h change, independent of how far back the
// chart reaches - that keeps the number comparable across feeds whose charts
// cover different spans. The chart-window figure is the fallback for sources
// that cannot supply a 24h number.
static float displayChange(const Quote& q)
{
  return q.has24h ? q.change24h : q.changePct;
}

static void drawSingleChart(const Config& cfg, const Feed& f, const Quote& q, bool stale)
{
  matrix->fillScreen(0);

  const float shown = displayChange(q);

  // The number is tinted by the number, the chart by the chart. These disagree
  // routinely now - up over a day, down over the last hour - and colouring a
  // visibly falling line green to match a positive 24h figure would be a lie.
  const uint16_t tint    = shown >= 0.0f ? C_PCT_UP() : C_PCT_DOWN();
  const bool     chartUp = q.changePct >= 0.0f;

  char pct[16];
  snprintf(pct, sizeof(pct), "%+.2f%%", shown);

  // Row 0-6: label left, change right. Drop both to the small font only if
  // they would collide - a big move like +123.45% is what pushes it over.
  const char* label  = f.label.c_str();
  int16_t     labelW = bigWidth(label);
  int16_t     pctW   = bigWidth(pct);

  if (labelW + pctW + 2 <= PANEL_W)
  {
    drawBig(0, 0, label, stale ? C_STALE() : C_LABEL());
    drawBig(PANEL_W - pctW, 0, pct, tint);
  }
  else
  {
    drawTiny(0, 1, label, stale ? C_STALE() : C_LABEL());
    drawTiny(PANEL_W - tinyWidth(pct), 1, pct, tint);
  }

  // Rows 8-14: price. Row 7 is left blank so the two lines do not touch.
  drawPriceLine(8, q.price, f.decimals, C_PRICE());

  // Rows 16-31: chart. Dark fill, lighter trace along its top edge.
  drawChart(q, 16, PANEL_H - 1,
            chartUp ? C_FILL_UP()  : C_FILL_DOWN(),
            chartUp ? C_TRACE_UP() : C_TRACE_DOWN());
}

// Two ramps of decreasing component value, so the point where a colour stops
// rendering can be read off the panel instead of guessed. Top half green,
// bottom half red, brightest on the left. Count visible swatches from the left
// to find the threshold at the current brightness.
static const uint8_t RAMP[8] = { 255, 180, 130, 95, 70, 50, 35, 25 };

static void drawRampTest()
{
  matrix->fillScreen(0);
  for (uint8_t i = 0; i < 8; i++)
  {
    matrix->fillRect(i * 8, 0,  8, 15, col(0, RAMP[i], 0));
    matrix->fillRect(i * 8, 16, 8, 16, col(RAMP[i], 0, 0));
  }
}

// Every colour the UI actually uses, as 16x16 swatches, so a brightness level
// can be judged on the real palette rather than on abstract ramps:
//
//   WHITE   DIM     ACCENT  STALE
//   UP      UP_HI   DOWN    DOWN_HI
//
// The pairs that matter are UP next to UP_HI and DOWN next to DOWN_HI - those
// are the chart fill and its trace, and they have to stay distinguishable.
static void drawPaletteTest()
{
  matrix->fillScreen(0);

  // Same order as PAL_NAMES, four across and three down, so the web page's
  // picker grid and the panel read left to right the same way.
  for (uint8_t i = 0; i < P_COUNT; i++)
  {
    const int16_t x = (i % 4) * 16;
    const int16_t y = (i / 4) * 11;
    matrix->fillRect(x, y, 16, 10, P(i));
  }
}

static void drawMultiPrice(const Config& cfg)
{
  matrix->fillScreen(0);

  const uint8_t rows = cfg.feedCount < 5 ? cfg.feedCount : 5;
  for (uint8_t i = 0; i < rows; i++)
  {
    Quote q;
    {
      QuoteLock lk;
      q = g_quotes[i];
    }

    int16_t y = i * 6 + 1;
    drawTiny(0, y, cfg.feeds[i].label.c_str(), C_LABEL());

    if (!q.valid)
    {
      drawTiny(PANEL_W - tinyWidth("--"), y, "--", C_DIM());
      continue;
    }

    char buf[20];
    formatPrice(buf, sizeof(buf), q.price, cfg.feeds[i].decimals);
    if (tinyWidth(buf) > PANEL_W - tinyWidth(cfg.feeds[i].label.c_str()) - 3)
      formatPriceShort(buf, sizeof(buf), q.price);

    drawTiny(PANEL_W - tinyWidth(buf), y, buf,
             displayChange(q) >= 0.0f ? C_PCT_UP() : C_PCT_DOWN());
  }
}

// ---------------------------------------------------------------- lifecycle

bool displayBegin()
{
  s_panelMutex = xSemaphoreCreateMutex();

  uint8_t depth;
  {
    CfgLock lk;
    depth = g_cfg.bitDepth;
  }
  if (depth < PANEL_BIT_DEPTH_MIN || depth > PANEL_BIT_DEPTH_MAX)
    depth = PANEL_BIT_DEPTH_DEFAULT;

  matrix = new Adafruit_Protomatter(
    PANEL_W, depth,
    1, HUB75_RGB,
    HUB75_ADDR_COUNT, HUB75_ADDR,
    HUB75_CLK, HUB75_LAT, HUB75_OE,
    true);                          // double buffered

  if (!matrix)
  {
    Serial.println("[panel] allocation failed");
    return false;
  }

  ProtomatterStatus st = matrix->begin();
  if (st != PROTOMATTER_OK)
  {
    Serial.printf("[panel] protomatter init failed: %d\n", (int)st);
    return false;
  }

  s_ok        = true;
  s_beginCore = xPortGetCoreID();
  displayPaletteReset();

  // Adafruit_GFX wraps by default, and it decides using the glyph ADVANCE, not
  // its ink: a right-aligned string whose last character ends flush at column
  // 63 still trips the check and that character is redrawn at x=0 on the next
  // line, in its own colour. That is what was painting the tint colour through
  // the "$" on the price line. Every string here is laid out to fit, so wrap
  // can only ever do damage.
  matrix->setTextWrap(false);

  matrix->fillScreen(0);
  drawTiny(0, 12, "UPTICK " UPTICK_VERSION, C_ACCENT());
  matrix->show();

  Serial.printf("[panel] %ux%u, %u-bit\n", PANEL_W, PANEL_H, depth);
  return true;
}

void displayTask(void* arg)
{
  (void)arg;

  uint8_t  feedIdx   = 0;
  uint32_t lastSwap  = millis();
  s_taskCore = xPortGetCoreID();

  uint32_t taskStart    = millis();
  uint32_t lastHzSample = millis();

  FrameKey lastKey;
  memset(&lastKey, 0xFF, sizeof(lastKey));

  // How long the address stays up after joining. Long enough to read and type,
  // short enough not to be annoying on every power cycle.
  const uint32_t INFO_MS = 8000;

  for (;;)
  {
    if (!s_ok)
    {
      // No panel: the web UI is still fully usable, so idle rather than spin.
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    // Cached config copy, refreshed only when the revision moves. Config holds
    // Strings, so copying it per frame would mean constant heap churn.
    // Config first, released, then the quote - the two locks are never held at
    // the same time, so they cannot deadlock against each other.
    static Config   cfg;
    static uint32_t seenRev = 0;
    if (seenRev != g_cfgRev)
    {
      CfgLock lk;
      cfg     = g_cfg;
      seenRev = g_cfgRev;
    }

    s_brightness = cfg.brightness;
    matrix->setRotation(cfg.rotate180 ? 2 : 0);

    if (feedIdx >= cfg.feedCount) feedIdx = 0;

    if (cfg.feedCount > 1 && millis() - lastSwap > (uint32_t)cfg.dwellSec * 1000UL)
    {
      lastSwap = millis();
      feedIdx  = (feedIdx + 1) % cfg.feedCount;
    }

    // Only single_chart shows one feed at a time. Everything else - the multi
    // price list, the test screens, the setup screens - leaves the fetcher
    // free to poll whatever is due.
    s_visibleFeed = (cfg.mode == "single_chart") ? (int8_t)feedIdx : (int8_t)-1;

    Quote q;
    {
      QuoteLock lk;
      q = g_quotes[feedIdx];
    }

    // Nothing on this panel animates. Content changes when a quote lands, when
    // the feed cycles, or when a setting moves - otherwise the frame is
    // identical to the last one. Redrawing it 30 times a second meant 30
    // bitplane rebuilds and 30 DMA buffer swaps per second, each one a chance
    // to glitch the refresh; gating on an actual change makes that twice a
    // minute instead.
    FrameKey key;
    memset(&key, 0, sizeof(key));
    key.cfgRev    = g_cfgRev;
    key.palRev    = s_palRev;
    key.fetchedMs = q.fetchedMs;
    key.feedIdx   = feedIdx;
    key.apMode    = g_net.apMode;
    key.valid     = q.valid;

    // Stale means "we tried and could not refresh this", not "this is old".
    // Age alone is a bad signal now that polling is gated on what is visible:
    // a feed is only refreshed when its turn comes round, so being a rotation
    // old is the normal schedule working, not a fault.
    //
    // The age term stays only as a catch-all for the case that records no
    // failure at all - the link dropping, so no fetch is ever attempted and
    // the price just ages. The threshold is far beyond anything the rotation
    // can produce, so it cannot fire on healthy operation.
    const uint32_t rotationMs = (uint32_t)cfg.dwellSec * 1000UL * cfg.feedCount;
    const uint32_t deadMs     = (uint32_t)cfg.refreshSec * 10000UL + rotationMs * 3;
    const bool     stale      = q.valid &&
                                (q.failures > 0 || millis() - q.fetchedMs > deadMs);

    const bool showInfo = !g_net.apMode && (millis() - taskStart < INFO_MS);

    key.stale    = stale    ? 1 : 0;
    key.showInfo = showInfo ? 1 : 0;

    // Sampled whether or not the frame is redrawn: it measures the panel's own
    // refresh, which runs off DMA and has nothing to do with this loop.
    // Sampled every 200 ms, not every second. A flicker is a stall of a few
    // tens of milliseconds; averaged over a full second that is a ~5% dent in
    // the rate and invisible in the numbers. At 200 ms the same stall halves
    // the window's rate and shows up clearly.
    if (millis() - lastHzSample >= 200)
    {
      // getFrameCount() returns frames since the LAST call, so it has to be
      // divided by the interval that actually elapsed. Assuming 1000 ms made
      // a delayed sample look like a higher refresh rate rather than a lower
      // one - exactly backwards, and it reported a peak of 214 Hz on a panel
      // running at 136.
      const uint32_t now     = millis();
      const uint32_t elapsed = now - lastHzSample;
      const uint32_t frames  = matrix->getFrameCount();

      s_refreshHz  = elapsed ? (uint16_t)((frames * 1000UL) / elapsed) : 0;
      lastHzSample = now;

      // The first couple of samples after boot are partial, so ignore zeroes.
      if (s_refreshHz > 0)
      {
        if (s_refreshHz > s_refreshPk)  s_refreshPk  = s_refreshHz;
        if (s_refreshHz < s_refreshLow) s_refreshLow = s_refreshHz;

        // A dip is a second in which the panel refreshed materially less than
        // its best. That is what a flicker actually is, counted rather than
        // eyeballed.
        if (s_refreshPk && s_refreshHz < (uint16_t)(s_refreshPk * 8 / 10))
          s_refreshDip++;
      }
    }

    if (memcmp(&key, &lastKey, sizeof(key)) != 0)
    {
      lastKey = key;

      xSemaphoreTake(s_panelMutex, portMAX_DELAY);

      if (cfg.mode == "palette_test")      drawPaletteTest();
      else if (cfg.mode == "ramp_test")    drawRampTest();
      else if (g_net.apMode)               drawSetup();
      else if (showInfo)                   drawJoined();
      else if (cfg.mode == "multi_price")  drawMultiPrice(cfg);
      else if (!q.valid)                   drawWaiting(cfg.feeds[feedIdx]);
      else                                 drawSingleChart(cfg, cfg.feeds[feedIdx], q, stale);

      matrix->show();
      xSemaphoreGive(s_panelMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(33));
  }
}
