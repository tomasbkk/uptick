#include "uptick.h"
#include "pins.h"
#include <Adafruit_Protomatter.h>
#include <Fonts/TomThumb.h>

static Adafruit_Protomatter matrix(
  PANEL_W, PANEL_BIT_DEPTH,
  1, HUB75_RGB,
  HUB75_ADDR_COUNT, HUB75_ADDR,
  HUB75_CLK, HUB75_LAT, HUB75_OE,
  true);                            // double buffered

static bool              s_ok         = false;
static SemaphoreHandle_t s_panelMutex = nullptr;
static uint8_t           s_brightness = 40;
static uint16_t          s_refreshHz  = 0;

uint16_t displayRefreshHz() { return s_refreshHz; }

// Writing to flash on the S3 can starve the matrix DMA and glitch a frame, so
// config saves bracket the write with these to keep show() - which rebuilds
// bitplanes and touches DMA descriptors - from running at the same moment.
//
// These deliberately do NOT call matrix.stop() / matrix.resume(). On the S3,
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
  return matrix.color565(s(r), s(g), s(b));
}

static uint16_t C_WHITE()  { return col(255, 255, 255); }
static uint16_t C_DIM()    { return col(110, 120, 130); }
static uint16_t C_STALE()  { return col(230, 160,  40); }
static uint16_t C_ACCENT() { return col( 60, 140, 255); }
static uint16_t C_UP()     { return col( 40, 220,  90); }
static uint16_t C_DOWN()   { return col(240,  70,  60); }

// Trace colours: the same hues washed toward white, so the line stays legible
// on top of a solid fill of the base colour.
static uint16_t C_UP_HI()   { return col(170, 255, 200); }
static uint16_t C_DOWN_HI() { return col(255, 185, 175); }

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
  matrix.setFont(NULL);
  matrix.setTextColor(c);
  matrix.setCursor(x, yTop);
  matrix.print(s);
}

static void drawTiny(int16_t x, int16_t yTop, const char* s, uint16_t c)
{
  matrix.setFont(&TomThumb);
  matrix.setTextColor(c);
  matrix.setCursor(x, yTop + TINY_H);
  matrix.print(s);
  matrix.setFont(NULL);
}

// ---------------------------------------------------------------- numbers

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

// The area under the trace is filled solid in the trend colour, and the trace
// itself is drawn in a paler version on top.
//
// The obvious alternative - a solid fill in a dimmed colour with the trace at
// full - does not work on this panel. Protomatter gamma-corrects, so a colour
// dim enough to sit behind the trace crushes to zero at low bit depth and the
// fill vanishes entirely. Making the fill the normal colour and the trace the
// bright one keeps every pixel well clear of that floor.
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
    matrix.drawLine(x, colY[x], x, yBot, fill);

  for (int16_t x = 0; x < PANEL_W; x++)
  {
    // Join steep moves so the trace stays continuous instead of dotted.
    if (x > 0) matrix.drawLine(x - 1, colY[x - 1], x, colY[x], trace);
    else       matrix.drawPixel(x, colY[x], trace);
  }
}

// ---------------------------------------------------------------- screens

static void drawSetup()
{
  matrix.fillScreen(0);
  drawTiny(0,  1, "SETUP - JOIN", C_ACCENT());
  drawTiny(0,  9, g_net.ssid.c_str(), C_WHITE());
  drawTiny(0, 17, "THEN OPEN", C_DIM());
  drawTiny(0, 25, g_net.ip.c_str(), C_WHITE());
}

// Shown briefly after a successful join. The panel is the one part of this
// device that is always visible, so it should answer "what address is it on"
// without needing a serial cable or the router's admin page.
static void drawJoined()
{
  matrix.fillScreen(0);
  drawTiny(0,  1, "CONNECTED",          C_ACCENT());
  drawTiny(0,  9, g_net.ssid.c_str(),   C_DIM());
  drawTiny(0, 17, g_net.ip.c_str(),     C_WHITE());
  drawTiny(0, 25, UPTICK_NAME ".local", C_DIM());
}

static void drawWaiting(const Feed& f)
{
  matrix.fillScreen(0);
  drawBig(0, 0, f.label.c_str(), C_WHITE());
  drawTiny(0, 12, "WAITING FOR", C_DIM());
  drawTiny(0, 20, "DATA", C_DIM());
}

// Which percentage the panel shows. Falls back to the chart-window figure
// until the next refresh fills the 24h one in, so flipping the setting never
// blanks the number or shows a bogus zero.
static float displayChange(const Config& cfg, const Quote& q)
{
  if (cfg.changeWindow == "24h" && q.has24h) return q.change24h;
  return q.changePct;
}

static void drawSingleChart(const Config& cfg, const Feed& f, const Quote& q, bool stale)
{
  matrix.fillScreen(0);

  const float shown = displayChange(cfg, q);

  // The number is tinted by the number; the chart is tinted by the chart. When
  // the setting is 24h these can disagree - price up over a day, down over the
  // last hour - and colouring a visibly falling line green would be a lie.
  const uint16_t tint      = shown >= 0.0f        ? C_UP() : C_DOWN();
  const bool     chartUp   = q.changePct >= 0.0f;
  const uint16_t chartTint = chartUp ? C_UP() : C_DOWN();

  char pct[16];
  snprintf(pct, sizeof(pct), "%+.2f%%", shown);

  // Row 0-6: label left, change right. Drop both to the small font only if
  // they would collide - a big move like +123.45% is what pushes it over.
  const char* label  = f.label.c_str();
  int16_t     labelW = bigWidth(label);
  int16_t     pctW   = bigWidth(pct);

  if (labelW + pctW + 2 <= PANEL_W)
  {
    drawBig(0, 0, label, stale ? C_STALE() : C_WHITE());
    drawBig(PANEL_W - pctW, 0, pct, tint);
  }
  else
  {
    drawTiny(0, 1, label, stale ? C_STALE() : C_WHITE());
    drawTiny(PANEL_W - tinyWidth(pct), 1, pct, tint);
  }

  // Rows 7-14: price.
  drawPriceLine(7, q.price, f.decimals, C_WHITE());

  // Rows 16-31: chart. Solid fill in the trend colour, pale trace on top.
  drawChart(q, 16, PANEL_H - 1, chartTint, chartUp ? C_UP_HI() : C_DOWN_HI());
}

static void drawMultiPrice(const Config& cfg)
{
  matrix.fillScreen(0);

  const uint8_t rows = cfg.feedCount < 5 ? cfg.feedCount : 5;
  for (uint8_t i = 0; i < rows; i++)
  {
    Quote q;
    {
      QuoteLock lk;
      q = g_quotes[i];
    }

    int16_t y = i * 6 + 1;
    drawTiny(0, y, cfg.feeds[i].label.c_str(), C_WHITE());

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
             displayChange(cfg, q) >= 0.0f ? C_UP() : C_DOWN());
  }
}

// ---------------------------------------------------------------- lifecycle

bool displayBegin()
{
  s_panelMutex = xSemaphoreCreateMutex();

  ProtomatterStatus st = matrix.begin();
  if (st != PROTOMATTER_OK)
  {
    Serial.printf("[panel] protomatter init failed: %d\n", (int)st);
    return false;
  }

  s_ok = true;
  matrix.fillScreen(0);
  drawTiny(0, 12, "UPTICK " UPTICK_VERSION, C_ACCENT());
  matrix.show();

  Serial.printf("[panel] %ux%u, %u-bit\n", PANEL_W, PANEL_H, PANEL_BIT_DEPTH);
  return true;
}

void displayTask(void* arg)
{
  (void)arg;

  uint8_t  feedIdx   = 0;
  uint32_t lastSwap  = millis();
  uint32_t taskStart    = millis();
  uint32_t lastHzSample = millis();

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
    matrix.setRotation(cfg.rotate180 ? 2 : 0);

    if (feedIdx >= cfg.feedCount) feedIdx = 0;

    if (cfg.feedCount > 1 && millis() - lastSwap > (uint32_t)cfg.dwellSec * 1000UL)
    {
      lastSwap = millis();
      feedIdx  = (feedIdx + 1) % cfg.feedCount;
    }

    Quote q;
    {
      QuoteLock lk;
      q = g_quotes[feedIdx];
    }

    // Three missed refreshes is enough to say so rather than keep presenting a
    // stale number as current.
    bool stale = q.valid &&
                 (millis() - q.fetchedMs > (uint32_t)cfg.refreshSec * 3000UL);

    {
      xSemaphoreTake(s_panelMutex, portMAX_DELAY);

      const bool showInfo = !g_net.apMode && (millis() - taskStart < INFO_MS);

      if (g_net.apMode)                    drawSetup();
      else if (showInfo)                   drawJoined();
      else if (cfg.mode == "multi_price")  drawMultiPrice(cfg);
      else if (!q.valid)                   drawWaiting(cfg.feeds[feedIdx]);
      else                                 drawSingleChart(cfg, cfg.feeds[feedIdx], q, stale);

      matrix.show();
      xSemaphoreGive(s_panelMutex);
    }

    // getFrameCount() returns refreshes since the last call, so sampling it on
    // a one second boundary gives the panel's actual refresh rate. Watching
    // this during a flicker is the difference between measuring and guessing.
    if (millis() - lastHzSample >= 1000)
    {
      s_refreshHz  = matrix.getFrameCount();
      lastHzSample = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(33));   // ~30 fps content updates
  }
}
