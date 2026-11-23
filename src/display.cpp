#include "uptick.h"
#include "pins.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Fonts/TomThumb.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <driver/gpio.h>

// The panel is driven by a circular DMA chain: every bit-plane of every row,
// latch and OE timing included, is laid out in internal RAM as a ring of
// descriptors that the LCD peripheral walks forever. No interrupt fires
// between bit-planes and no CPU is involved in refresh at all, so nothing the
// firmware does - TLS, WiFi, flash writes - can make the panel flicker.
static MatrixPanel_I2S_DMA* matrix = nullptr;

static bool              s_ok         = false;
static SemaphoreHandle_t s_panelMutex = nullptr;
static uint16_t          s_refreshHz  = 0;
static uint8_t           s_taskCore   = 0xFF;
static volatile uint32_t s_stackFree  = 0;
static volatile uint32_t s_tickerUs   = 0;

// When the splash went up. The display task holds it until SPLASH_MS has
// passed, counted from here rather than from the task start - the WiFi join
// runs in between, and when that is slow the splash has already been up long
// enough.
static uint32_t          s_splashAt   = 0;
static const uint32_t    SPLASH_MS    = 5000;

// Brightness last written to the OE bits. setBrightness8 walks the whole of
// both DMA buffers, so it runs when the value changes, not every frame.
static int16_t           s_appliedBri = -1;

uint16_t displayRefreshHz() { return s_refreshHz; }

// -1 means "no single feed is on screen": either the layout shows all of them
// at once, or it is a test screen showing none. The fetcher treats that as
// permission to poll any feed.
static volatile int8_t s_visibleFeed = -1;

int displayVisibleFeed() { return s_visibleFeed; }

DisplayStats displayGetStats()
{
  DisplayStats st;
  st.hz       = s_refreshHz;
  st.depth    = PANEL_BIT_DEPTH;
  st.taskCore = s_taskCore;
  st.stackFree = s_stackFree;
  st.tickerUs  = s_tickerUs;
  return st;
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
  uint8_t  market;
  uint8_t  pad[2];
};

// Holding this keeps the render task from drawing. It used to protect the
// refresh from flash writes; the DMA chain lives in internal RAM and needs no
// cache, so it runs straight through them now. What still needs it is
// config.cpp, which swaps the config and the quotes under separate mutexes and
// must not let a frame be drawn from one new and one old.
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

// Colors go to the panel as they are, never scaled for brightness. Scaling in
// software loses dark colors below what the panel can show and shifts hues,
// because the components do not scale in proportion - red drifts orange.
//
// Brightness is the panel's OE duty cycle, set once by setBrightness8 and
// preserved by every draw call (they clear only the six RGB bits of each DMA
// word; OE is bit 7). The palette is therefore the palette at every
// brightness, and the driver's CIE 1931 table handles perceptual linearity.
static uint16_t col(uint8_t r, uint8_t g, uint8_t b)
{
  return matrix->color565(r, g, b);
}

// Slider percent to the driver's 0-255 duty. Linear, so "5%" means 5% duty
// and a stored value keeps its meaning. No floor is needed: the driver keeps
// at least one pixel-clock of output per bit-plane for any non-zero value, so
// the bottom of the slider is dim but never dark.
static void applyBrightness(uint8_t pct)
{
  if (pct < 1)   pct = 1;
  if (pct > 100) pct = 100;
  if (s_appliedBri == pct) return;

  matrix->setBrightness8((uint8_t)(((uint16_t)pct * 255 + 50) / 100));
  s_appliedBri = pct;
}

// Double buffering, and the one rule that comes with it here.
//
// A flip only rewires the last descriptor of each chain to point at the other
// buffer, so the DMA moves across when it finishes the frame it is on - which
// can be up to a full frame after flipDMABuffer() returns. Until then, the
// buffer that drawing now targets is still the one on screen, and writing to
// it tears. The driver's own wait for this is commented out on the S3, so it is
// enforced here: no drawing until two frame periods have passed since a flip.
//
// The render loop sleeps far longer than that today, so this costs nothing -
// it exists for scrolling, where frames come fast enough for it to matter.
static uint32_t s_frameUs    = 0;
static uint32_t s_lastFlipUs = 0;

static void flip()
{
  matrix->flipDMABuffer();
  s_lastFlipUs = micros();
}

static void waitForFlip()
{
  const uint32_t guardUs = 2 * s_frameUs;
  const uint32_t sinceUs = micros() - s_lastFlipUs;
  if (sinceUs >= guardUs) return;
  vTaskDelay(pdMS_TO_TICKS((guardUs - sinceUs) / 1000 + 1));
}

// The palette, as 8-bit RGB before brightness scaling. Order must match
// PAL_NAMES, and the panel test screen draws them in this order.
struct Rgb { uint8_t r, g, b; };

enum PalIdx {
  P_LABEL, P_PRICE, P_PCT_UP, P_PCT_DOWN,
  P_FILL_UP, P_TRACE_UP, P_FILL_DOWN, P_TRACE_DOWN,
  P_DIM, P_ACCENT, P_STALE, P_DIVIDER,
  P_SUN, P_MOON, P_CLOSED, P_SNOW,
  P_COUNT
};

static const char* const PAL_NAMES[P_COUNT] = {
  "LABEL", "PRICE", "PCT_UP", "PCT_DOWN",
  "FILL_UP", "TRACE_UP", "FILL_DOWN", "TRACE_DOWN",
  "DIM", "ACCENT", "STALE", "DIVIDER",
  "SUN", "MOON", "CLOSED", "SNOW"
};

// Tuned on the panel with the palette pickers and exported from the web page.
static const Rgb PAL_FACTORY[P_COUNT] = {
  { 238, 255,   0 },   // LABEL      - symbol, top left
  { 255, 255, 255 },   // PRICE      - price line
  {  40, 220,  90 },   // PCT_UP     - percent, rising
  { 240,  70,  60 },   // PCT_DOWN   - percent, falling
  {   0,  82,  23 },   // FILL_UP    - chart fill, rising
  {   0, 255,  94 },   // TRACE_UP   - chart trace, rising
  {  66,   0,   0 },   // FILL_DOWN  - chart fill, falling
  { 255,   0,   0 },   // TRACE_DOWN - chart trace, falling
  { 110, 120, 130 },   // DIM        - secondary text
  {  60, 140, 255 },   // ACCENT     - setup screens
  { 230, 160,  40 },   // STALE      - old data
  {   3,   1, 116 },   // DIVIDER    - ticker section line, black hides it
  { 255, 170,   0 },   // SUN        - pre-market icon
  { 150, 170, 255 },   // MOON       - after-hours icon
  { 140, 140, 150 },   // CLOSED     - market-closed icon
  { 200, 220, 255 },   // SNOW       - near snowflakes; far ones are a dimmer shade
};

// Starts at factory before anything else runs. The stored config is loaded -
// and its saved colours applied - before the panel is brought up, so the
// palette cannot wait for displayBegin() to initialise it.
static Rgb      s_pal[P_COUNT];
static const bool s_palInit __attribute__((unused)) = []() { memcpy(s_pal, PAL_FACTORY, sizeof(s_pal)); return true; }();
static uint32_t s_palRev = 1;

uint8_t     displayPaletteCount()          { return P_COUNT; }
const char* displayPaletteName(uint8_t i)  { return i < P_COUNT ? PAL_NAMES[i] : ""; }

void displayPaletteGet(uint8_t i, uint8_t& r, uint8_t& g, uint8_t& b)
{
  if (i >= P_COUNT) { r = g = b = 0; return; }
  r = s_pal[i].r; g = s_pal[i].g; b = s_pal[i].b;
}

void displayPaletteFactory(uint8_t i, uint8_t& r, uint8_t& g, uint8_t& b)
{
  if (i >= P_COUNT) { r = g = b = 0; return; }
  r = PAL_FACTORY[i].r; g = PAL_FACTORY[i].g; b = PAL_FACTORY[i].b;
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
static uint16_t C_DIVIDER()    { return P(P_DIVIDER);    }
static uint16_t C_SUN()        { return P(P_SUN);        }
static uint16_t C_MOON()       { return P(P_MOON);       }
static uint16_t C_CLOSED()     { return P(P_CLOSED);     }
static uint16_t C_SNOW()       { return P(P_SNOW);       }

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

// A change figure behind an up or down triangle, in either font - the way an
// exchange board shows direction, instead of a sign. The 5x7 font has the
// triangles as CP437 0x1E and 0x1F. TomThumb covers printable ASCII only, so
// there the triangle is drawn, 5 wide by 3 tall, centred on its 5 px digits.
static const char ARROW_UP   = 0x1E;
static const char ARROW_DOWN = 0x1F;

static int16_t arrowPctWidth(const char* digits, bool big)
{
  return big ? (int16_t)(strlen(digits) + 1) * BIG_W - 1
             : 6 + tinyWidth(digits);
}

static void drawArrowPct(int16_t x, int16_t yTop, const char* digits, bool up, bool big, uint16_t c)
{
  if (big)
  {
    char s[16];
    snprintf(s, sizeof(s), "%c%s", up ? ARROW_UP : ARROW_DOWN, digits);
    drawBig(x, yTop, s, c);
    return;
  }

  if (up) matrix->fillTriangle(x + 2, yTop + 1, x, yTop + 3, x + 4, yTop + 3, c);
  else    matrix->fillTriangle(x, yTop + 1, x + 4, yTop + 1, x + 2, yTop + 3, c);
  drawTiny(x + 6, yTop, digits, c);
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

// "$" only when the quote really is in dollars. A Yahoo feed can come back in
// EUR or in pence, and the 5x7 font has no glyph for those anyway, so anything
// else is printed bare rather than mislabelled. The crypto sources are all USD
// pairs, so they are unaffected.
static const char* ccyPrefix(const Quote& q)
{
  return strcmp(q.ccy, "USD") == 0 ? "$" : "";
}

static void formatPrice(char* out, size_t n, float p, uint8_t decimals, const char* cur)
{
  uint8_t d = decimals;
  if (d == 0)
  {
    if      (p >= 1000.0f) d = 0;
    else if (p >= 1.0f)    d = 2;
    else                   d = 4;
  }
  snprintf(out, n, "%s%.*f", cur, d, p);
  addThousands(out, n);
}

static void formatPriceShort(char* out, size_t n, float p, const char* cur)
{
  if      (p >= 1000000.0f) snprintf(out, n, "%s%.2fM", cur, p / 1000000.0f);
  else if (p >= 1000.0f)    snprintf(out, n, "%s%.1fK", cur, p / 1000.0f);
  else                      snprintf(out, n, "%s%.2f",  cur, p);
}

// Ladder: full precision in the readable font, then the small font, then
// abbreviate. "$214,460.00" is 66 px in the 5x7 font - one pixel too wide for
// the panel - so this is the difference between a price and a truncated price.
static void drawPriceLine(int16_t yTop, float price, uint8_t decimals, uint16_t c,
                          const char* cur, int16_t maxW = PANEL_W)
{
  char buf[24];
  formatPrice(buf, sizeof(buf), price, decimals, cur);

  if (bigWidth(buf) <= maxW)  { drawBig(0, yTop, buf, c); return; }
  if (tinyWidth(buf) <= maxW) { drawTiny(0, yTop + 1, buf, c); return; }

  formatPriceShort(buf, sizeof(buf), price, cur);
  if (bigWidth(buf) <= maxW) drawBig(0, yTop, buf, c);
  else                       drawTiny(0, yTop + 1, buf, c);
}

// ---------------------------------------------------------------- market state

// Whether a stock's market is open, for the icon beside its price on the chart
// screen. Nothing for crypto, which never closes, or while the clock has not
// synced yet.
enum : uint8_t { MKT_OPEN, MKT_PRE, MKT_POST, MKT_CLOSED };

// Decided at draw time rather than at fetch time, so the icon changes when the
// session does, not when the feed next comes round.
//
// Within a session the price must also come from it: a quote whose last trade
// is from before this session began means nothing has traded yet, which is
// what a holiday looks like - Yahoo can still report a regular session for it.
// It also covers the first minutes of pre-market, and a chart whose extended
// fetch failed, which shows the regular close.
static uint8_t marketState(const Quote& q)
{
  if (!q.sess[1][1] || !clockValid()) return MKT_OPEN;

  const uint32_t now = (uint32_t)time(nullptr);
  auto in = [&](uint8_t i, uint32_t t) { return t >= q.sess[i][0] && t < q.sess[i][1]; };

  if (in(1, now)) return q.tradeTs >= q.sess[1][0] ? MKT_OPEN : MKT_CLOSED;
  if (in(0, now)) return (q.session == SESSION_PRE  && in(0, q.tradeTs)) ? MKT_PRE  : MKT_CLOSED;
  if (in(2, now)) return (q.session == SESSION_POST && in(2, q.tradeTs)) ? MKT_POST : MKT_CLOSED;
  return MKT_CLOSED;
}

// 7 rows, the height of the big font's digits, so an icon sits level with the
// price beside it. Rows are whole bytes, leftmost pixel in the top bit, so the
// 9-wide sun takes two bytes a row and the 7-wide icons one.
static const int16_t ICON_H = 7;

// Rising over the horizon: pre-market. Six single-pixel rays in a fan, each
// kept a pixel clear of the sun and of each other so they read as separate
// points rather than merging into a blob. That needs the 9 columns.
static const int16_t ICON_SUN_W = 9;
static const uint8_t ICON_SUN[7 * 2] = {
  0x14, 0x00,   // ...#.#...
  0x41, 0x00,   // .#.....#.
  0x1C, 0x00,   // ...###...
  0xBE, 0x80,   // #.#####.#
  0x3E, 0x00,   // ..#####..
  0x00, 0x00,   // .........
  0xFF, 0x80,   // #########
};

static const int16_t ICON_W = 7;         // the moon and the padlock

static const uint8_t ICON_MOON[7] = {    // crescent: after hours
  0x38,   // ..###..
  0x70,   // .###...
  0xE0,   // ###....
  0xE0,   // ###....
  0xE0,   // ###....
  0x70,   // .###...
  0x38,   // ..###..
};

static const uint8_t ICON_CLOSED[7] = {  // padlock: market closed
  0x38,   // ..###..
  0x44,   // .#...#.
  0x44,   // .#...#.
  0xFE,   // #######
  0xEE,   // ###.###
  0xEE,   // ###.###
  0xFE,   // #######
};

static int16_t marketIconWidth(uint8_t state)
{
  return state == MKT_PRE ? ICON_SUN_W : ICON_W;
}

// Right-aligned, so every icon ends at the panel edge whatever its width.
static void drawMarketIcon(int16_t yTop, uint8_t state)
{
  const int16_t x = PANEL_W - marketIconWidth(state);
  switch (state)
  {
    case MKT_PRE:    matrix->drawBitmap(x, yTop, ICON_SUN,    ICON_SUN_W, ICON_H, C_SUN());    break;
    case MKT_POST:   matrix->drawBitmap(x, yTop, ICON_MOON,   ICON_W,     ICON_H, C_MOON());   break;
    case MKT_CLOSED: matrix->drawBitmap(x, yTop, ICON_CLOSED, ICON_W,     ICON_H, C_CLOSED()); break;
  }
}

// ---------------------------------------------------------------- chart

// The row of the chart's top edge in every column, which is also where snow
// settles on it. False when there are too few points to draw a chart.
//
// noinline, and the flat-window check is an if outside the loop rather than a
// ?: inside it: inlined into drawSingleChart, that conditional float made the
// Xtensa GCC fail with an internal compiler error ("insn does not satisfy its
// constraints"), and which caller it was inlined into decided whether it did.
static bool __attribute__((noinline)) chartColumns(const Quote& q, int16_t yTop, int16_t yBot,
                                                   int16_t* colY)
{
  if (q.count < 2) return false;

  const int16_t span = yBot - yTop;

  // Scale to the visible window, never to zero, or intraday moves render as a
  // flat line across the bottom. A window with no range at all sits at mid
  // height instead.
  float scale = 0.0f, base = 0.5f;
  if (q.hi > q.lo)
  {
    scale = 1.0f / (q.hi - q.lo);
    base  = 0.0f;
  }

  for (int16_t x = 0; x < PANEL_W; x++)
  {
    // Resample onto the panel width. With limit=64 from the exchange this is
    // a straight 1:1 copy, but a short response still fills the width.
    float t    = (float)x * (q.count - 1) / (float)(PANEL_W - 1);
    int   i    = (int)t;
    float frac = t - i;

    float v = (i + 1 < q.count) ? q.points[i] + (q.points[i + 1] - q.points[i]) * frac
                                : q.points[q.count - 1];

    const float norm = base + (v - q.lo) * scale;
    colY[x] = yBot - (int16_t)lroundf(norm * span);
  }
  return true;
}

// Solid fill in the trend color, under a lighter trace along its top edge.
static void drawChart(const int16_t* colY, int16_t yBot, uint16_t fill, uint16_t trace)
{
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

// ---------------------------------------------------------------- themes

// Falling snow, the Christmas theme. It is drawn straight after the frame is
// cleared and before anything else, so it falls behind the content: flakes
// show between the letters and never cover a figure.
//
// Two layers give it depth. Near flakes are brighter, fall faster and sway
// wider; far ones are a dimmer shade of the same color, slower and narrower.
// Positions are floats moved by the real time since the last frame rather than
// a fixed step per frame - the tickers run at whatever frame rate their scroll
// speed sets, and the snow has to fall at the same pace in every mode.
//
// On the chart screen the near flakes land on the chart and settle there (see
// snowLand); the far ones keep falling and pass behind it, which is what sells
// the depth.
struct Flake
{
  float   x, y;     // pixels, before the sway; y below the panel means respawn
  float   vx, vy;   // drift and fall, pixels per second
  float   phase;    // position in its sideways sway, radians
  float   rate;     // sway speed, radians per second
  float   amp;      // sway width, pixels either side
  bool    near;
};

static const uint8_t SNOW_NEAR = 8;
static const uint8_t SNOW_FAR  = 14;
static const uint8_t SNOW_FPS  = 30;     // chart mode's frame rate while it snows

// Settled snow. At most PILE_MAX pixels deep in a column, and never up into
// the header rows above PILE_TOP_MIN. One pixel melts every MELT_SEC, so a
// feed that stays up for a long time keeps a dusting rather than filling in.
static const uint8_t PILE_MAX     = 3;
static const int16_t PILE_TOP_MIN = 16;
static const float   MELT_SEC     = 2.5f;

static Flake          s_snow[SNOW_NEAR + SNOW_FAR];
static bool           s_snowOn     = false;    // set by the task from cfg.theme every pass
static bool           s_snowReady  = false;    // flakes placed since the theme came on
static uint32_t       s_snowLastUs = 0;

static const int16_t* s_surf    = nullptr;     // chart top per column, while a chart is drawing
static const void*    s_surfKey = nullptr;     // which feed's chart the pile is lying on
static uint8_t        s_pile[PANEL_W];
static float          s_meltAcc = 0;

// xorshift32. The flakes need nothing better than "looks random".
static uint32_t s_rng = 0x9E3779B9;

static float rnd(float lo, float hi)
{
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return lo + (hi - lo) * (float)(s_rng & 0xFFFF) / 65535.0f;
}

// anywhere places the flake at any height, so turning the theme on shows a
// screen that is already snowing rather than one that fills from the top.
static void flakeSpawn(Flake& f, bool anywhere)
{
  f.x = rnd(0, PANEL_W);
  if (anywhere) f.y = rnd(-2, PANEL_H);
  else          f.y = rnd(-4, -1);

  if (f.near)
  {
    f.vy   = rnd(4.0f, 6.0f);
    f.vx   = rnd(-1.0f, 1.0f);
    f.amp  = rnd(2.0f, 3.5f);
    f.rate = rnd(0.6f, 1.2f);
  }
  else
  {
    f.vy   = rnd(2.0f, 3.5f);
    f.vx   = rnd(-0.6f, 0.6f);
    f.amp  = rnd(1.0f, 2.0f);
    f.rate = rnd(0.5f, 1.0f);
  }
  f.phase = rnd(0, 6.2832f);
}

static void santaReset();   // below, with the rest of Santa
static void decoReset();    // below, with the decorations

static void snowReset()
{
  s_rng ^= esp_random() | 1;
  for (uint8_t i = 0; i < SNOW_NEAR + SNOW_FAR; i++)
  {
    s_snow[i].near = i < SNOW_NEAR;
    flakeSpawn(s_snow[i], true);
  }
  memset(s_pile, 0, sizeof(s_pile));
  s_surfKey    = nullptr;
  santaReset();
  s_meltAcc    = 0;
  s_snowLastUs = micros();
  s_snowReady  = true;
}

// The chart hands over its column tops before it clears, so the snow knows
// where to land; nullptr for every other screen. key names the feed whose
// chart it is - a different one starts with bare ground, since the snow was
// lying on a chart that is no longer there.
static void snowSurface(const int16_t* colY, const void* key)
{
  if (key != s_surfKey)
  {
    memset(s_pile, 0, sizeof(s_pile));
    s_meltAcc = 0;
    s_surfKey = key;
    decoReset();
  }
  s_surf = colY;
}

// Highest settled row in a column: the chart's top edge, less any snow on it.
static int16_t groundTop(int16_t x)
{
  return s_surf[x] - s_pile[x];
}

// A flake has reached the ground at column x. It rolls downhill first: while
// a neighbour lies two or more rows lower it moves there, so snow gathers in
// the chart's dips instead of balancing on its peaks - the way it slides off
// anything steeper than it can rest on. A full column, or one already up
// against the header, takes no more; that flake just melts on arrival.
static void snowLand(int16_t x)
{
  for (uint8_t step = 0; step < 8; step++)
  {
    int16_t best = x;
    if (x > 0           && groundTop(x - 1) > groundTop(best) + 1) best = x - 1;
    if (x < PANEL_W - 1 && groundTop(x + 1) > groundTop(best) + 1) best = x + 1;
    if (best == x) break;
    x = best;
  }

  if (s_pile[x] >= PILE_MAX)            return;
  if (groundTop(x) - 1 < PILE_TOP_MIN)  return;
  s_pile[x]++;
}

// Takes one pixel off a random column that has any. A few tries rather than a
// search: if the ground is nearly bare, missing a melt does not matter.
static void snowMelt()
{
  for (uint8_t tries = 0; tries < 8; tries++)
  {
    const int16_t x = (int16_t)rnd(0, PANEL_W - 1);
    if (s_pile[x]) { s_pile[x]--; return; }
  }
}

// noinline so the per-frame loop exists once rather than being copied into
// each screen that clears through clearFrame.
static void __attribute__((noinline)) drawSnow()
{
  if (!s_snowReady) snowReset();
  if (!s_surf) s_surfKey = nullptr;        // off the chart: bare ground when it returns

  // Capped, so a pause - a mode switch, a slow frame - does not throw every
  // flake a screen's height in one step.
  const uint32_t nowUs = micros();
  float dt = (nowUs - s_snowLastUs) / 1000000.0f;
  s_snowLastUs = nowUs;
  if (dt > 0.1f) dt = 0.1f;

  // The far layer is the same color at under half the value. The panel's
  // gamma table makes that look a good deal dimmer than half, which is the
  // point - it reads as distance.
  const Rgb&     s     = s_pal[P_SNOW];
  const uint16_t cNear = C_SNOW();
  const uint16_t cFar  = col(s.r * 45 / 100, s.g * 45 / 100, s.b * 45 / 100);

  for (Flake& f : s_snow)
  {
    f.y += f.vy * dt;
    f.x += f.vx * dt;
    if (f.x < 0)        f.x += PANEL_W;
    if (f.x >= PANEL_W) f.x -= PANEL_W;

    // The sway advances by dt too, rather than being a sine of the uptime,
    // which as a float loses the precision to move smoothly after a few days.
    f.phase += f.rate * dt;
    if (f.phase > 6.2832f) f.phase -= 6.2832f;

    int16_t x = (int16_t)lroundf(f.x + sinf(f.phase) * f.amp);
    x = (int16_t)((x % PANEL_W + PANEL_W) % PANEL_W);
    const int16_t y = (int16_t)floorf(f.y);

    // Lands when it reaches the row above the ground. Only within a couple of
    // rows of it, though: a flake that is already well below - placed there
    // when the theme came on, or overtaken by a new chart that rose past it -
    // is behind the chart, and carries on falling out of sight.
    if (f.near && s_surf && y >= 0)
    {
      const int16_t top = groundTop(x);
      if (y >= top - 1 && y <= top + 1)
      {
        snowLand(x);
        flakeSpawn(f, false);
        continue;
      }
    }

    if (f.y >= PANEL_H) { flakeSpawn(f, false); continue; }
    if (y >= 0) matrix->drawPixel(x, y, f.near ? cNear : cFar);
  }

  if (!s_surf) return;

  s_meltAcc += dt;
  while (s_meltAcc >= MELT_SEC)
  {
    s_meltAcc -= MELT_SEC;
    snowMelt();
  }

  // The settled snow, stacked up from the chart's top edge. The chart is drawn
  // after this, but only from its top edge down, so it never covers the pile.
  for (int16_t x = 0; x < PANEL_W; x++)
    for (uint8_t k = 1; k <= s_pile[x]; k++)
      matrix->drawPixel(x, s_surf[x] - k, cNear);
}

// Every feed screen starts here instead of a bare fillScreen, so a theme's
// background layer goes under all of them the same way. The setup, splash and
// test screens clear for themselves and never show it.
static void clearFrame()
{
  matrix->fillScreen(0);
  if (s_snowOn) drawSnow();
  s_surf = nullptr;                      // only good for the frame that set it
}

// ---------------------------------------------------------------- santa

// Santa in his sleigh behind two reindeer, Rudolph leading, flying across the
// panel every few minutes while it snows. Unlike the snow he is drawn over
// everything, after the screen's own content: he is up for a few seconds at a
// time, and behind the figures he would be mostly hidden.
//
// Drawn facing right, and mirrored when he flies left so the reindeer always
// lead. Two frames, swapped four times a second, make the reindeer gallop.
// One character per pixel; '.' is transparent. The colors are the
// character's own, not palette entries, the same as the boot logo.
static const int16_t SANTA_W = 34;
static const int16_t SANTA_H = 11;

static const char* const SANTA[2][SANTA_H] = {
  {   // legs out
    "....WRR...........................",
    ".....RRRR............A.A......A.A.",
    ".....WWWW.............AA.......AA.",
    "DD...WSSS.............BBB......BBN",
    "DD...WWWW.......BBBBBBB..BBBBBBB..",
    "DD..RRRRRRGGGGGGBBBBBBGGGBBBBBB...",
    "DYYYYYYYYYYYY.Y.B....B...B....B...",
    "DDDDDDDDDDDDDY.B......B.B......B..",
    ".DDDDDDDDDDDD.....................",
    "..Y.......Y...Y...................",
    "YYYYYYYYYYYYYY....................",
  },
  {   // legs under
    "....WRR...........................",
    ".....RRRR............A.A......A.A.",
    ".....WWWW.............AA.......AA.",
    "DD...WSSS.............BBB......BBN",
    "DD...WWWW.......BBBBBBB..BBBBBBB..",
    "DD..RRRRRRGGGGGGBBBBBBGGGBBBBBB...",
    "DYYYYYYYYYYYY.Y..B..B.....B..B....",
    "DDDDDDDDDDDDDY....B.B......B.B....",
    ".DDDDDDDDDDDD.....................",
    "..Y.......Y...Y...................",
    "YYYYYYYYYYYYYY....................",
  },
};

static uint16_t santaColor(char c)
{
  switch (c)
  {
    case 'R': return col(230,  20,  20);   // suit and hat
    case 'D': return col(140,   0,   0);   // sleigh, darker so Santa stands out
    case 'W': return col(255, 255, 255);   // fur trim and beard
    case 'S': return col(255, 170, 120);   // face
    case 'Y': return col(255, 200,   0);   // sleigh rim and runners
    case 'G': return col(200, 160,  40);   // reins
    case 'B': return col(150,  80,  30);   // reindeer
    case 'A': return col(210, 170, 110);   // antlers
    case 'N': return col(255,   0,   0);   // Rudolph's nose
  }
  return 0;
}

static const float SANTA_PPS = 12.0f;      // pixels per second; about 8 s to cross

static bool          s_santaUp    = false;
static bool          s_santaLeft  = false; // flying right to left
static int16_t       s_santaY     = 0;
static uint32_t      s_santaStart = 0;
static uint32_t      s_santaDue   = 0;
static volatile bool s_santaKick  = false; // from the web page's button

// First flight soon after the theme comes on, so turning it on shows him
// rather than leaving it to chance; after that every two to five minutes.
static void santaSchedule(bool first)
{
  const float sec = first ? rnd(20, 40) : rnd(120, 300);
  s_santaDue = millis() + (uint32_t)(sec * 1000.0f);
}

static void santaReset()
{
  s_santaUp = false;
  santaSchedule(true);
}

void displaySantaNow() { s_santaKick = true; }

static void drawSanta()
{
  const uint32_t now = millis();

  if (!s_santaUp)
  {
    if (!s_santaKick && (int32_t)(now - s_santaDue) < 0) return;
    s_santaKick  = false;
    s_santaUp    = true;
    s_santaLeft  = rnd(0, 1) < 0.5f;
    s_santaY     = (int16_t)rnd(1, PANEL_H - SANTA_H - 1);
    s_santaStart = now;
  }

  // Position from the time since take-off, so the frame rate of the mode he
  // is flying over does not change his speed.
  const float   t      = (now - s_santaStart) / 1000.0f;
  const int16_t travel = (int16_t)(t * SANTA_PPS);
  if (travel > PANEL_W + SANTA_W)
  {
    s_santaUp = false;
    santaSchedule(false);
    return;
  }

  int16_t left = travel - SANTA_W;                     // entering from the left
  if (s_santaLeft) left = PANEL_W - travel;            // entering from the right

  const int16_t top   = s_santaY + (int16_t)lroundf(sinf(t * 3.0f));   // a gentle bob
  const uint8_t frame = (uint8_t)(t * 4.0f) & 1;

  for (int16_t r = 0; r < SANTA_H; r++)
  {
    const int16_t y = top + r;
    if (y < 0 || y >= PANEL_H) continue;
    const char* row = SANTA[frame][r];
    for (int16_t c = 0; c < SANTA_W; c++)
    {
      const char ch = row[s_santaLeft ? SANTA_W - 1 - c : c];
      if (ch == '.') continue;
      const int16_t x = left + c;
      if (x < 0 || x >= PANEL_W) continue;
      matrix->drawPixel(x, y, santaColor(ch));
    }
  }
}

// ---------------------------------------------------------------- decorations

// A snowman or a Christmas tree standing on the chart while it snows. Each
// time a chart comes on screen one of the two is picked at random, and it
// stands wherever the chart leaves room for it - or is left out if nowhere
// does.
//
// It stands on the lowest point of the ground under it, so it never floats;
// where the ground under it is higher it overlaps the chart, which reads as
// standing in front of a slope. Ground more uneven than DECO_UNEVEN would bury
// too much of it, and its top may not reach the header rows, so those spots
// do not count. Drawn after the chart: in front of the chart and the falling
// snow, behind Santa.
//
// 10 rows tall: measured against a dozen real 64-bar charts, that and ground
// no more than DECO_UNEVEN rows uneven finds room on about five charts in six.
// At 12 rows it was half of them.
//
// Same format as Santa: a character per pixel, '.' transparent. 'K' is drawn
// black on purpose - eyes and buttons are holes, and they must not show the
// snow or the chart behind them.
static const int16_t DECO_W      = 7;
static const int16_t DECO_H      = 10;
static const int16_t DECO_UNEVEN = 3;     // rows between the lowest and highest ground under it
static const int16_t DECO_TOP    = 15;    // the blank row between the price and the chart

static const char* const SNOWMAN[DECO_H] = {
  "..RRR..",
  ".RRRRR.",
  ".WKWKW.",
  ".WWWOO.",
  "T.GGG.T",
  ".TWWWT.",
  "WWWKWWW",
  "WWWWWWW",
  "WWWKWWW",
  ".WWWWW.",
};

// '1', '2' and '3' are the ornaments, which take turns through red, yellow and
// blue so the tree twinkles.
static const char* const TREE[DECO_H] = {
  "...Y...",
  "..YYY..",
  "..P1P..",
  ".PPPPP.",
  "..PP3..",
  ".P2PPP.",
  "PPPP1PP",
  ".PPPPP.",
  "P3PPP2P",
  "...T...",
};

enum : uint8_t { DECO_NONE, DECO_SNOWMAN, DECO_TREE };

static uint8_t s_decoKind = DECO_NONE;
static int16_t s_decoX    = -1;           // left column, -1 until a spot is found

// A new chart came up: pick which of the two it gets, and find it a spot.
static void decoReset()
{
  s_decoKind = rnd(0, 1) < 0.5f ? DECO_SNOWMAN : DECO_TREE;
  s_decoX    = -1;
}

// The lowest ground under a footprint starting at x - the largest row number -
// or -1 if the footprint is no place to stand.
static int16_t decoGround(const int16_t* colY, int16_t x)
{
  int16_t lo = colY[x], hi = colY[x];
  for (int16_t c = 1; c < DECO_W; c++)
  {
    if (colY[x + c] > lo) lo = colY[x + c];
    if (colY[x + c] < hi) hi = colY[x + c];
  }
  if (lo - hi > DECO_UNEVEN)   return -1;
  if (lo - DECO_H < DECO_TOP)  return -1;
  return lo;
}

static uint16_t decoColor(char c, uint8_t twinkle)
{
  // The ornaments shift one color along every twinkle step.
  if (c >= '1' && c <= '3') c = "RYL"[(c - '1' + twinkle) % 3];

  switch (c)
  {
    case 'W': return col(255, 255, 255);   // snow
    case 'R': return col(230,  20,  20);   // hat, ornament
    case 'O': return col(255, 120,   0);   // carrot
    case 'G': return col(  0, 150,  30);   // scarf
    // Branches. A pine teal rather than a plain green, which blended into a
    // rising chart's green fill and trace - the tree is usually standing in
    // front of one.
    case 'P': return col(  0, 130, 110);
    case 'T': return col(120,  70,  30);   // arms, trunk
    case 'Y': return col(255, 210,   0);   // star, ornament
    case 'L': return col( 40,  90, 255);   // ornament
  }
  return 0;                                // 'K'
}

static void drawDecoration(const int16_t* colY)
{
  if (s_decoKind == DECO_NONE) return;

  // Stays put while it still fits, so a chart that shifts a little on a new
  // fetch does not send it hopping about. Otherwise a spot is picked at random
  // from all that fit; one column in from either edge, so it never looks cut
  // off.
  int16_t ground = (s_decoX >= 0) ? decoGround(colY, s_decoX) : -1;
  if (ground < 0)
  {
    int16_t fits[PANEL_W];
    uint8_t n = 0;
    for (int16_t x = 1; x + DECO_W < PANEL_W; x++)
      if (decoGround(colY, x) >= 0) fits[n++] = x;
    if (!n) { s_decoX = -1; return; }

    s_decoX = fits[(uint8_t)rnd(0, n - 0.001f)];
    ground  = decoGround(colY, s_decoX);
  }

  const char* const* spr     = (s_decoKind == DECO_SNOWMAN) ? SNOWMAN : TREE;
  const uint8_t      twinkle = (uint8_t)((millis() / 600) % 3);
  const int16_t      top     = ground - DECO_H;

  for (int16_t r = 0; r < DECO_H; r++)
    for (int16_t c = 0; c < DECO_W; c++)
    {
      const char ch = spr[r][c];
      if (ch != '.') matrix->drawPixel(s_decoX + c, top + r, decoColor(ch, twinkle));
    }
}

// ---------------------------------------------------------------- weather

// Two screens per city: now - an animated icon, the temperature, today's high
// and low, the name and the condition - and the week, five columns of
// day, icon, high and low. They play once after every full pass of the
// tickers, or on their own when the tickers are switched off; see the task.
//
// The icon and temperature colors are the weather's own rather than palette
// entries, the same as Santa: a sun is yellow whatever the label color is.

enum : uint8_t { WX_CLEAR, WX_PARTLY, WX_CLOUDY, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_THUNDER };

// WMO weather codes, as Open-Meteo reports them, grouped by what to draw.
static uint8_t wxKind(uint8_t code)
{
  if (code == 0)                                  return WX_CLEAR;
  if (code <= 2)                                  return WX_PARTLY;
  if (code == 3)                                  return WX_CLOUDY;
  if (code == 45 || code == 48)                   return WX_FOG;
  if (code >= 51 && code <= 57)                   return WX_DRIZZLE;
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return WX_RAIN;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86)   return WX_SNOW;
  if (code >= 95)                                 return WX_THUNDER;
  return WX_CLOUDY;
}

// The words under the name on the now screen. The small font fits 16
// characters across the panel, which every one of these does.
static const char* wxText(uint8_t code)
{
  switch (code)
  {
    case 0:  return "CLEAR";
    case 1:  return "MAINLY CLEAR";
    case 2:  return "PARTLY CLOUDY";
    case 3:  return "OVERCAST";
    case 45: case 48: return "FOG";
    case 51: case 53: case 55: return "DRIZZLE";
    case 56: case 57: return "FREEZING DRIZZLE";
    case 61: case 63: return "RAIN";
    case 65: return "HEAVY RAIN";
    case 66: case 67: return "FREEZING RAIN";
    case 71: case 73: return "SNOW";
    case 75: return "HEAVY SNOW";
    case 77: return "SNOW GRAINS";
    case 80: case 81: return "SHOWERS";
    case 82: return "HEAVY SHOWERS";
    case 85: case 86: return "SNOW SHOWERS";
    case 95: return "THUNDERSTORM";
    case 96: case 99: return "THUNDER, HAIL";
  }
  return "";
}

static uint16_t wxColor(char c)
{
  switch (c)
  {
    case 'Y': return col(255, 190,   0);   // sun
    case 'r': return col(255, 140,   0);   // sun's rays
    case 'M': return col(230, 225, 170);   // moon
    case '*': return col(255, 255, 200);   // star
    case 'W': return col(200, 205, 215);   // light cloud
    case 'D': return col(120, 128, 140);   // grey cloud
    case 'T': return col( 85,  90, 105);   // storm cloud
    case 'B': return col( 60, 140, 255);   // rain
    case 'S': return col(220, 230, 255);   // snow
    case 'F': return col(140, 145, 155);   // fog
    case 'Z': return col(255, 230,   0);   // lightning
    case 'H': return col(255, 140,  60);   // high
    case 'L': return col( 90, 160, 255);   // low
  }
  return 0;
}

// Draws a sprite of character rows; '.' is transparent and every other
// character is a color from wxColor, or 'X' for the color passed in.
static void wxSprite(int16_t x, int16_t y, const char* const* rows, uint8_t h, uint16_t xColor = 0)
{
  for (uint8_t r = 0; r < h; r++)
    for (uint8_t c = 0; rows[r][c]; c++)
    {
      const char ch = rows[r][c];
      if (ch == '.') continue;
      matrix->drawPixel(x + c, y + r, ch == 'X' ? xColor : wxColor(ch));
    }
}

// A cloud, 13x7, in whichever shade is passed: two bumps and rounded ends,
// which read as a cloud where overlapping circles came out as a dome on a box.
static const char* const WX_CLOUD[7] = {
  "....XXX......",
  "...XXXXX.XX..",
  "..XXXXXXXXXX.",
  ".XXXXXXXXXXXX",
  "XXXXXXXXXXXXX",
  "XXXXXXXXXXXXX",
  ".XXXXXXXXXXX.",
};

static const char* const WX_BOLT[5] = { "..ZZ", ".ZZ.", "ZZ..", ".ZZ.", "Z..." };

// The week screen's icons, 7x7 and still.
static const char* const WX_SMALL[8][7] = {
  { "...r...", ".r...r.", "..YYY..", "rYYYYYr", "..YYY..", ".r...r.", "...r..." },   // clear
  { "r.r....", ".YY....", "rYYWW..", "..WWWW.", ".WWWWWW", "WWWWWWW", ".WWWWW." },   // partly
  { ".......", "...WW..", ".WWWWW.", "WWWWWWW", "WWWWWWW", ".WWWWW.", "......." },   // cloudy
  { ".......", "FFFFF..", ".......", ".FFFFFF", ".......", "FFFFF..", "......." },   // fog
  { "..DD...", ".DDDDD.", "DDDDDDD", ".......", ".B...B.", "...B...", "......." },   // drizzle
  { "..DD...", ".DDDDD.", "DDDDDDD", ".......", "B.B.B..", ".B.B.B.", "B.B.B.." },   // rain
  { "..DD...", ".DDDDD.", "DDDDDDD", ".......", ".S..S..", "...S..S", "S..S..." },   // snow
  { "..TT...", ".TTTTT.", "TTTTTTT", "...Z...", "..ZZ...", "...ZZ..", "...Z..." },   // thunder
};

static void wxSun(int16_t cx, int16_t cy, int16_t r, float t)
{
  matrix->fillCircle(cx, cy, r, wxColor('Y'));

  // Eight rays; the straight and the diagonal four take turns being long, so
  // the sun seems to turn.
  const uint8_t phase = (uint8_t)(t * 2.5f) & 1;
  for (uint8_t k = 0; k < 8; k++)
  {
    const float   a  = k * 0.7854f;
    const int16_t r1 = r + ((k & 1) == phase ? 3 : 2);
    for (int16_t rr = r + 2; rr <= r1; rr++)
      matrix->drawPixel(cx + (int16_t)lroundf(cosf(a) * rr), cy + (int16_t)lroundf(sinf(a) * rr),
                        wxColor('r'));
  }
}

// A crescent: a disc with a second, black disc cut out of its top right.
static void wxMoon(int16_t cx, int16_t cy, int16_t r)
{
  matrix->fillCircle(cx, cy, r, wxColor('M'));
  matrix->fillCircle(cx + 3, cy - 2, r, 0);
}

// Rain, drizzle or snow falling from under a cloud, in the 16x8 band below
// it. Each column has its own phase so the drops do not fall in step.
static void wxFalling(int16_t x, int16_t y, uint8_t kind, float t)
{
  static const uint8_t COLS[5]   = { 1, 4, 7, 10, 13 };
  static const uint8_t DCOLS[3]  = { 2, 7, 12 };
  const bool    drizzle = (kind == WX_DRIZZLE);
  const uint8_t n       = drizzle ? 3 : 5;
  float speed = 10.0f;
  if (kind == WX_SNOW) speed = 3.0f;
  if (drizzle)         speed = 6.0f;

  for (uint8_t k = 0; k < n; k++)
  {
    const int16_t cx = x + (drizzle ? DCOLS[k] : COLS[k]);
    const int16_t py = y + 8 + (int16_t)fmodf(t * speed + k * 3.7f, 8.0f);
    if (kind == WX_SNOW)  matrix->drawPixel(cx + (int16_t)lroundf(sinf(t * 2.0f + k)), py, wxColor('S'));
    else if (drizzle)     matrix->drawPixel(cx, py, wxColor('B'));
    else                  matrix->drawFastVLine(cx, py, 2, wxColor('B'));
  }
}

// The now screen's 16x16 icon. t is seconds, for the animation.
static void wxIcon(int16_t x, int16_t y, uint8_t kind, bool day, float t)
{
  switch (kind)
  {
    case WX_CLEAR:
      if (day) { wxSun(x + 7, y + 7, 3, t); break; }
      wxMoon(x + 6, y + 8, 5);
      {
        // Three stars, one of them out at any moment, taking turns.
        static const int8_t STARS[3][2] = { { 12, 2 }, { 14, 7 }, { 11, 12 } };
        for (uint8_t k = 0; k < 3; k++)
          if (((uint8_t)(t * 3.0f) + k) % 3) matrix->drawPixel(x + STARS[k][0], y + STARS[k][1], wxColor('*'));
      }
      break;

    case WX_PARTLY:
      if (day) wxSun(x + 5, y + 5, 2, t);
      else     wxMoon(x + 5, y + 6, 4);
      wxSprite(x + 2 + (int16_t)lroundf(sinf(t * 0.8f)), y + 8, WX_CLOUD, 7, wxColor('W'));
      break;

    case WX_CLOUDY:
    {
      // Two clouds drifting past each other.
      const int16_t dx = (int16_t)lroundf(sinf(t * 0.6f) * 1.4f);
      wxSprite(x + 3 - dx, y + 1, WX_CLOUD, 7, wxColor('D'));
      wxSprite(x + dx,     y + 8, WX_CLOUD, 7, wxColor('W'));
      break;
    }

    case WX_FOG:
    {
      static const uint8_t ROWS[4] = { 3, 6, 9, 12 };
      static const uint8_t LENS[4] = { 10, 12, 10, 12 };
      for (uint8_t k = 0; k < 4; k++)
        matrix->drawFastHLine(x + 1 + (k & 1) + (int16_t)lroundf(sinf(t * 0.9f + k * 1.3f) * 1.5f),
                              y + ROWS[k], LENS[k], wxColor('F'));
      break;
    }

    default:   // drizzle, rain, snow, thunder: a cloud with something under it
      wxSprite(x + 1, y, WX_CLOUD, 7, wxColor(kind == WX_THUNDER ? 'T' : 'D'));
      wxFalling(x, y, kind == WX_THUNDER ? WX_RAIN : kind, t);
      if (kind == WX_THUNDER && ((uint8_t)(t * 3.0f) % 4) == 0)
        wxSprite(x + 6, y + 8, WX_BOLT, 5);
      break;
  }
}

static const char* const WX_DOW[7] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };

// Seconds, for the icon animations. Wrapped every hour so the float keeps
// enough precision to move smoothly however long the device has been up.
static float wxTime()
{
  return (millis() % 3600000UL) / 1000.0f;
}

// Shown in place of a city's screens until its first fetch lands, and when
// the weather is on but no city has been added.
static void drawWeatherWaiting(const char* name)
{
  clearFrame();
  if (name && *name)
  {
    if (bigWidth(name) <= PANEL_W) drawBig(0, 4, name, C_LABEL());
    else                           drawTiny(0, 5, name, C_LABEL());
    drawTiny(0, 18, "WAITING FOR", C_DIM());
    drawTiny(0, 25, "WEATHER", C_DIM());
  }
  else
  {
    drawTiny(0,  4, "NO CITY YET", C_ACCENT());
    drawTiny(0, 13, "ADD ONE ON THE", C_DIM());
    drawTiny(0, 20, "WEATHER TAB", C_DIM());
  }
}

static void drawWeatherNow(const char* name, const CityWeather& w)
{
  clearFrame();
  wxIcon(0, 0, wxKind(w.code), w.isDay, wxTime());

  // The temperature in the logo's font, with a small square ring for the
  // degree sign - the font has none.
  char t[8];
  snprintf(t, sizeof(t), "%ld", lroundf(w.temp));
  matrix->setFont(&FreeSansBold9pt7b);
  matrix->setTextColor(C_PRICE());
  matrix->setCursor(18, 13);
  matrix->print(t);
  const int16_t after = matrix->getCursorX();
  matrix->setFont(NULL);
  matrix->drawRect(after + 1, 1, 3, 3, C_PRICE());

  // Today's high and low at the right edge, behind the same triangles the
  // tickers use - left out if a long temperature has taken the room.
  if (w.days)
  {
    char hi[6], lo[6];
    snprintf(hi, sizeof(hi), "%d", w.day[0].hi);
    snprintf(lo, sizeof(lo), "%d", w.day[0].lo);
    const int16_t wHi = arrowPctWidth(hi, false), wLo = arrowPctWidth(lo, false);
    if (after + 5 <= PANEL_W - (wHi > wLo ? wHi : wLo))
    {
      drawArrowPct(PANEL_W - wHi, 2, hi, true,  false, wxColor('H'));
      drawArrowPct(PANEL_W - wLo, 9, lo, false, false, wxColor('L'));
    }
  }

  if (bigWidth(name) <= PANEL_W) drawBig(0, 18, name, C_LABEL());
  else                           drawTiny(0, 19, name, C_LABEL());
  drawTiny(0, 27, wxText(w.code), C_DIM());
}

static void drawWeatherWeek(const char* name, const CityWeather& w)
{
  clearFrame();

  // Five columns 12 px apart: the small font's two-letter day, a 7x7 icon,
  // then the high over the low. Today, the first, has its day in the label
  // color.
  //
  // Seven 9 px columns fit too, but only for two-character temperatures: a
  // third character - -10 and below, or 100 and up - ran straight into the
  // next day's figure. Twelve leaves room for three with a gap, so every city
  // gets the same five days whatever its climate.
  const int16_t pitch = 12;
  const int16_t left  = 2;

  for (uint8_t i = 0; i < w.days; i++)
  {
    const WeatherDay& d = w.day[i];
    const int16_t     x = left + i * pitch;
    char hi[6], lo[6];
    snprintf(hi, sizeof(hi), "%d", d.hi);
    snprintf(lo, sizeof(lo), "%d", d.lo);

    drawTiny(x, 0, WX_DOW[d.dow % 7], i == 0 ? C_LABEL() : C_DIM());
    wxSprite(x, 6, WX_SMALL[wxKind(d.code)], 7);
    drawTiny(x, 14, hi, wxColor('H'));
    drawTiny(x, 20, lo, wxColor('L'));
  }

  // The city along the bottom, in capitals: the small font's lower case has
  // descenders that would run off the panel.
  char up[25];
  snprintf(up, sizeof(up), "%s", name);
  for (char* p = up; *p; p++) *p = toupper((unsigned char)*p);
  drawTiny((PANEL_W - tinyWidth(up)) / 2, 27, up, C_LABEL());
}

// Which city and screen the weather is on. The task moves these along.
static uint8_t  s_wxCity   = 0;
static uint8_t  s_wxScreen = 0;     // 0 now, 1 week
static uint32_t s_wxAt     = 0;     // when the current screen went up
static const uint8_t WX_FPS = 20;

static void drawWeather(const Config& cfg)
{
  if (!cfg.cityCount) { drawWeatherWaiting(nullptr); return; }

  const uint8_t i = s_wxCity < cfg.cityCount ? s_wxCity : 0;
  CityWeather w;
  {
    WeatherLock lk;
    w = g_weather[i];
  }
  const char* name = cfg.cities[i].name.c_str();

  if (!w.valid)            drawWeatherWaiting(name);
  else if (s_wxScreen == 0) drawWeatherNow(name, w);
  else                      drawWeatherWeek(name, w);
}

// ---------------------------------------------------------------- clock

// The clock, with the date, in a choice of themes. Every theme gets the same
// ClockTime - the clock city's local time - and draws the whole panel itself.
//
// The time is the clock city's: the device keeps UTC from SNTP, and the city's
// UTC offset comes with its weather, which follows daylight saving. With no
// city, or before its first fetch, it shows UTC and says so; before SNTP has
// set the clock at all, the time reads --:--.

struct ClockTime
{
  bool    ok;        // false until SNTP has set the clock
  bool    utc;       // no city offset was available
  uint8_t h, m, s;
  float   sub;       // fraction of the current second, for smooth hands
  uint8_t wday, mday, mon;
  int16_t sunrise;   // minutes after local midnight, -1 unknown
  int16_t sunset;
};

static ClockTime clockNow(const Config& cfg)
{
  ClockTime ct;
  memset(&ct, 0, sizeof(ct));
  ct.sunrise = ct.sunset = -1;
  ct.utc     = true;
  if (!clockValid()) return ct;

  int32_t off = 0;
  if (cfg.cityCount)
  {
    const uint8_t i = cfg.clockCity < cfg.cityCount ? cfg.clockCity : 0;
    WeatherLock lk;
    if (g_weather[i].valid)
    {
      off        = g_weather[i].utcOffset;
      ct.utc     = false;
      ct.sunrise = g_weather[i].sunrise;
      ct.sunset  = g_weather[i].sunset;
    }
  }

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const time_t t = tv.tv_sec + off;
  struct tm tm;
  gmtime_r(&t, &tm);

  ct.ok   = true;
  ct.h    = tm.tm_hour;
  ct.m    = tm.tm_min;
  ct.s    = tm.tm_sec;
  ct.sub  = tv.tv_usec / 1000000.0f;
  ct.wday = tm.tm_wday % 7;
  ct.mday = tm.tm_mday;
  ct.mon  = tm.tm_mon % 12;
  return ct;
}

static const char* const CLOCK_DOW[7]  = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char* const CLOCK_MON[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                           "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

// "16:17", with a space for the colon on odd seconds when blink is set.
static void clockHM(const ClockTime& ct, char* out, size_t n, bool blink)
{
  if (!ct.ok) { snprintf(out, n, "--:--"); return; }
  snprintf(out, n, "%02d%c%02d", ct.h, (blink && (ct.s & 1)) ? ' ' : ':', ct.m);
}

// "MON 23 NOV", or "MON 23" short, with " UTC" when there was no city offset.
static void clockDate(const ClockTime& ct, char* out, size_t n, bool shortForm)
{
  if (!ct.ok) { snprintf(out, n, "SETTING TIME"); return; }
  if (shortForm) snprintf(out, n, "%s %d%s", CLOCK_DOW[ct.wday], ct.mday, ct.utc ? " UTC" : "");
  else           snprintf(out, n, "%s %d %s%s", CLOCK_DOW[ct.wday], ct.mday, CLOCK_MON[ct.mon],
                          ct.utc ? " UTC" : "");
}

// The digits the drawn themes build from: the 5x7 font's shapes, one byte a
// row, leftmost column in bit 4. Flip, Tetris and the analog date scale or
// rebuild them; the text themes use the font itself.
static const uint8_t DIG5[10][7] = {
  { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
  { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
  { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
  { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
  { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
  { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
  { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
  { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
  { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
  { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
};

static bool digitBit(uint8_t d, uint8_t col, uint8_t row)
{
  return (DIG5[d % 10][row] >> (4 - col)) & 1;
}

static void drawDigit(int16_t x, int16_t y, uint8_t d, uint8_t scale, uint16_t c)
{
  for (uint8_t r = 0; r < 7; r++)
    for (uint8_t k = 0; k < 5; k++)
      if (digitBit(d, k, r)) matrix->fillRect(x + k * scale, y + r * scale, scale, scale, c);
}

// Text with a one-pixel black outline, so whatever a theme draws behind it
// stops short of the figures. size 2 is the 5x7 font doubled - the blocky
// clock digits.
static void drawOutlined(int16_t x, int16_t yTop, const char* s, uint8_t size, uint16_t c)
{
  matrix->setFont(NULL);
  matrix->setTextSize(size);
  for (int8_t dy = -1; dy <= 1; dy++)
    for (int8_t dx = -1; dx <= 1; dx++)
    {
      if (!dx && !dy) continue;
      matrix->setTextColor(0);
      matrix->setCursor(x + dx, yTop + dy);
      matrix->print(s);
    }
  matrix->setTextColor(c);
  matrix->setCursor(x, yTop);
  matrix->print(s);
  matrix->setTextSize(1);
}

static void drawTinyOutlined(int16_t x, int16_t yTop, const char* s, uint16_t c)
{
  for (int8_t dy = -1; dy <= 1; dy++)
    for (int8_t dx = -1; dx <= 1; dx++)
      if (dx || dy) drawTiny(x + dx, yTop + dy, s, 0);
  drawTiny(x, yTop, s, c);
}

static void drawTinyCentered(int16_t yTop, const char* s, uint16_t c)
{
  drawTiny((PANEL_W - tinyWidth(s)) / 2, yTop, s, c);
}

// Seconds since the last call, capped so a pause does not jump the animation.
static float clockDt(uint32_t& lastUs)
{
  const uint32_t nowUs = micros();
  float dt = (nowUs - lastUs) / 1000000.0f;
  lastUs = nowUs;
  if (dt > 0.1f) dt = 0.1f;
  return dt;
}

// The themes, in the order the web page lists them.
enum : uint8_t { CT_MATRIX, CT_FLIP, CT_RETRO, CT_PONG, CT_TETRIS, CT_SKY, CT_ANALOG, CT_PLATFORMER,
                 CT_COUNT };
static const char* const CLOCK_THEMES[CT_COUNT] = {
  "matrix", "flip", "retro", "pong", "tetris", "sky", "analog", "platformer"
};

uint8_t displayClockThemeCount()           { return CT_COUNT; }
const char* displayClockThemeName(uint8_t i) { return i < CT_COUNT ? CLOCK_THEMES[i] : ""; }

static const uint8_t CLOCK_FPS = 30;

// ---------------------------------------------------------------- matrix

// Green rain streaming down the panel behind large pale digits. Each column
// has at most one drop: a near-white head with a tail fading to dark green,
// its own speed and length, and a pause after it leaves before the next.
struct Drop
{
  float   y;       // the head's row; the tail runs up from it
  float   speed;   // rows per second
  float   wait;    // seconds until the next drop, while none is falling
  uint8_t len;
  bool    on;
};

static Drop     s_rain[PANEL_W];
static uint32_t s_rainLastUs = 0;

static void dropStart(Drop& d)
{
  d.on    = true;
  d.len   = (uint8_t)rnd(5, 15);
  d.speed = rnd(10.0f, 26.0f);
  d.y     = -1;
}

// Starts mid-flow rather than from an empty panel: about half the columns
// already have a drop somewhere on them.
static void matrixReset()
{
  for (Drop& d : s_rain)
  {
    if (rnd(0, 1) < 0.5f)
    {
      dropStart(d);
      d.y = rnd(0, PANEL_H + d.len);
    }
    else
    {
      d.on   = false;
      d.wait = rnd(0, 2.0f);
    }
  }
  s_rainLastUs = micros();
}

static void __attribute__((noinline)) clockMatrix(const ClockTime& ct)
{
  const float dt = clockDt(s_rainLastUs);

  for (int16_t x = 0; x < PANEL_W; x++)
  {
    Drop& d = s_rain[x];
    if (!d.on)
    {
      d.wait -= dt;
      if (d.wait <= 0) dropStart(d);
      continue;
    }

    d.y += d.speed * dt;
    if (d.y - d.len > PANEL_H)
    {
      d.on   = false;
      d.wait = rnd(0.2f, 2.5f);
      continue;
    }

    // Head nearly white, then the tail fading on a square curve, so most of
    // it is a dim green and only the front is bright.
    const int16_t head = (int16_t)d.y;
    for (uint8_t k = 0; k < d.len; k++)
    {
      const int16_t y = head - k;
      if (y < 0 || y >= PANEL_H) continue;
      if (k == 0) { matrix->drawPixel(x, y, col(200, 255, 200)); continue; }
      const float f = 1.0f - (float)k / d.len;
      matrix->drawPixel(x, y, col(0, (uint8_t)(40 + 200 * f * f), (uint8_t)(10 + 40 * f * f)));
    }
  }

  char hm[8], date[20];
  clockHM(ct, hm, sizeof(hm), true);
  clockDate(ct, date, sizeof(date), false);
  // "16:17" doubled is 59 px - 12 a character less the blank column after
  // the last - so it centres at x 3.
  drawOutlined(3, 3, hm, 2, col(190, 255, 200));
  drawTinyOutlined((PANEL_W - tinyWidth(date)) / 2, 24, date, col(0, 170, 70));
}

// ---------------------------------------------------------------- flip

// Split-flap: each digit on its own card, split across the middle. When one
// changes, the top half of the old digit folds down over 0.2 s - squashing
// toward the split, uncovering the new digit's top half behind it - and then
// the new digit's bottom half folds down over the old one's.
static const int16_t FLIP_X[4] = { 2, 16, 35, 49 };
static const int16_t FLIP_Y    = 2;
static const int16_t FLIP_W    = 13;
static const int16_t FLIP_H    = 20;
static const int16_t FLIP_SPLIT = 10;      // card row of the split
static const uint32_t FLIP_MS  = 400;

static uint8_t  s_flipCur[4], s_flipOld[4];
static uint32_t s_flipAt[4];

// One pixel of a card showing digit d: black at the corners and the split,
// the digit doubled from (2,3), the card's own grey elsewhere. d 10 is a card
// with no digit, for "--:--".
static uint16_t flipPixel(uint8_t d, int16_t cx, int16_t cy)
{
  if ((cx == 0 || cx == FLIP_W - 1) && (cy == 0 || cy == FLIP_H - 1)) return 0;
  if (cy == FLIP_SPLIT) return 0;
  if (d < 10 && cx >= 2 && cx < 12 && cy >= 3 && cy < 17 && digitBit(d, (cx - 2) / 2, (cy - 3) / 2))
    return col(235, 235, 235);
  return col(45, 45, 55);
}

static void flipReset(const ClockTime& ct)
{
  const uint8_t d[4] = { (uint8_t)(ct.h / 10), (uint8_t)(ct.h % 10), (uint8_t)(ct.m / 10), (uint8_t)(ct.m % 10) };
  for (uint8_t i = 0; i < 4; i++)
  {
    s_flipCur[i] = s_flipOld[i] = ct.ok ? d[i] : 10;
    s_flipAt[i]  = 0;
  }
}

static void __attribute__((noinline)) clockFlip(const ClockTime& ct)
{
  const uint32_t now  = millis();
  const uint8_t  d[4] = { (uint8_t)(ct.h / 10), (uint8_t)(ct.h % 10), (uint8_t)(ct.m / 10), (uint8_t)(ct.m % 10) };

  for (uint8_t i = 0; i < 4; i++)
  {
    const uint8_t want = ct.ok ? d[i] : 10;
    if (want != s_flipCur[i])
    {
      s_flipOld[i] = s_flipCur[i];
      s_flipCur[i] = want;
      s_flipAt[i]  = now | 1;              // never 0, which means "not flipping"
    }

    const int16_t x = FLIP_X[i];
    float p = 1.0f;
    if (s_flipAt[i] && now - s_flipAt[i] < FLIP_MS) p = (now - s_flipAt[i]) / (float)FLIP_MS;
    const uint8_t nu = s_flipCur[i], old = s_flipOld[i];

    for (int16_t cy = 0; cy < FLIP_H; cy++)
      for (int16_t cx = 0; cx < FLIP_W; cx++)
      {
        uint16_t c;
        if (p >= 1.0f)
        {
          c = flipPixel(nu, cx, cy);
        }
        else if (p < 0.5f)
        {
          // First half: the old top half is the flap, squashing toward the
          // split; above it the new top half is already showing.
          const int16_t h = (int16_t)lroundf(FLIP_SPLIT * (1.0f - p * 2.0f));
          if (cy < FLIP_SPLIT - h)       c = flipPixel(nu, cx, cy);
          else if (cy < FLIP_SPLIT)      c = flipPixel(old, cx, (cy - (FLIP_SPLIT - h)) * FLIP_SPLIT / h);
          else                           c = flipPixel(old, cx, cy);
        }
        else
        {
          // Second half: the new bottom half unfolds down over the old one.
          const int16_t span = FLIP_H - FLIP_SPLIT - 1;
          const int16_t h    = (int16_t)lroundf(span * (p - 0.5f) * 2.0f);
          if (cy <= FLIP_SPLIT)                c = flipPixel(nu, cx, cy);
          else if (cy - FLIP_SPLIT - 1 < h)    c = flipPixel(nu, cx, FLIP_SPLIT + 1 + (cy - FLIP_SPLIT - 1) * span / h);
          else                                 c = flipPixel(old, cx, cy);
        }
        matrix->drawPixel(x + cx, FLIP_Y + cy, c);
      }
  }

  const uint16_t dot = col(235, 235, 235);
  matrix->fillRect(31, 8, 2, 2, dot);
  matrix->fillRect(31, 15, 2, 2, dot);

  char date[20];
  clockDate(ct, date, sizeof(date), false);
  drawTinyCentered(25, date, col(150, 150, 160));
}

// ---------------------------------------------------------------- retro

// A bedside alarm clock: red seven-segment digits with the unlit segments
// faintly showing, and a colon that blinks with the seconds.
static const char* const SEG7[10] = {
  "abcdef", "bc", "abged", "abgcd", "fgbc", "afgcd", "afgedc", "abc", "abcdefg", "abcdfg"
};

static void drawSeven(int16_t x, int16_t y, int8_t d, uint16_t on, uint16_t off)
{
  struct R { char s; int8_t x, y, w, h; };
  static const R SEGS[7] = {
    { 'a', 2, 0, 7, 2 }, { 'b', 9, 2, 2, 8 }, { 'c', 9, 12, 2, 8 }, { 'd', 2, 20, 7, 2 },
    { 'e', 0, 12, 2, 8 }, { 'f', 0, 2, 2, 8 }, { 'g', 2, 10, 7, 2 },
  };
  for (const R& r : SEGS)
  {
    const bool lit = d >= 0 && strchr(SEG7[d], r.s);
    matrix->fillRect(x + r.x, y + r.y, r.w, r.h, lit ? on : off);
  }
}

static void __attribute__((noinline)) clockRetro(const ClockTime& ct)
{
  const uint16_t on  = col(255, 40, 30);
  const uint16_t off = col(40, 5, 5);
  static const int16_t X[4] = { 5, 18, 35, 48 };
  const int8_t d[4] = { (int8_t)(ct.h / 10), (int8_t)(ct.h % 10), (int8_t)(ct.m / 10), (int8_t)(ct.m % 10) };
  for (uint8_t i = 0; i < 4; i++) drawSeven(X[i], 1, ct.ok ? d[i] : -1, on, off);

  const uint16_t colon = (ct.ok && !(ct.s & 1)) ? on : off;
  matrix->fillRect(31, 7, 2, 2, colon);
  matrix->fillRect(31, 15, 2, 2, colon);

  char date[20];
  clockDate(ct, date, sizeof(date), false);
  drawTinyCentered(26, date, col(200, 30, 20));
}

// ---------------------------------------------------------------- pong

// Two paddles playing on their own, the hours as the left score and the
// minutes as the right. When the minute turns the left paddle lets the ball
// through, so the right side scores; when the hour turns the right paddle
// does. Otherwise neither ever misses.
static float    s_pbx, s_pby, s_pvx, s_pvy;   // ball, its top left, and its speed
static float    s_plY, s_prY;                 // paddle tops
static int8_t   s_pongH = -1, s_pongM = -1;   // the score on screen
static int8_t   s_pongMiss = 0;               // -1 left lets it through, 1 right does
static uint32_t s_pongLastUs = 0;
static const int16_t PADDLE_H = 6;

static void pongServe(int8_t dir)
{
  s_pbx = 31;
  s_pby = rnd(8, 22);
  s_pvx = dir * 30.0f;
  s_pvy = rnd(-14.0f, 14.0f);
}

static void pongReset(const ClockTime& ct)
{
  s_pongH = ct.h;
  s_pongM = ct.m;
  s_pongMiss = 0;
  s_plY = s_prY = 13;
  pongServe(1);
  s_pongLastUs = micros();
}

// Moves a paddle toward where it wants to be, at a paddle's top speed.
static void pongTrack(float& y, float target, float dt)
{
  const float step = 45.0f * dt;
  if (target > y + step)      y += step;
  else if (target < y - step) y -= step;
  else                        y = target;
  if (y < 0) y = 0;
  if (y > PANEL_H - PADDLE_H) y = PANEL_H - PADDLE_H;
}

static void __attribute__((noinline)) clockPong(const ClockTime& ct)
{
  const float dt = clockDt(s_pongLastUs);

  if (ct.ok && !s_pongMiss && (ct.h != s_pongH || ct.m != s_pongM))
  {
    if (ct.h != s_pongH) s_pongMiss = 1;      // the hour turned: the left side scores
    else                 s_pongMiss = -1;     // the minute turned: the right side does
  }

  s_pbx += s_pvx * dt;
  s_pby += s_pvy * dt;
  if (s_pby < 0)  { s_pby = -s_pby;          s_pvy = -s_pvy; }
  if (s_pby > 30) { s_pby = 60 - s_pby;      s_pvy = -s_pvy; }

  // Each paddle follows the ball's centre - except one that is to miss,
  // which heads for the far end of its side.
  // (No float ?: here or below: see chartColumns for what that did to the
  // compiler.)
  const float aim  = s_pby + 1 - PADDLE_H / 2.0f;
  float       away = 0;
  if (s_pby < 13) away = PANEL_H - PADDLE_H;
  float targetL = aim, targetR = aim;
  if (s_pongMiss == -1) targetL = away;
  if (s_pongMiss == 1)  targetR = away;
  pongTrack(s_plY, targetL, dt);
  pongTrack(s_prY, targetR, dt);

  // Returns. A paddle that is not meant to miss never does: it is moved
  // under the ball, which only ever takes a pixel or two.
  if (s_pvx < 0 && s_pbx <= 2 && s_pongMiss != -1)
  {
    s_plY = constrain(aim, 0.0f, (float)(PANEL_H - PADDLE_H));
    s_pbx = 4 - s_pbx;
    s_pvx = -s_pvx;
    s_pvy = constrain(s_pvy + rnd(-8.0f, 8.0f), -22.0f, 22.0f);
  }
  if (s_pvx > 0 && s_pbx >= 60 && s_pongMiss != 1)
  {
    s_prY = constrain(aim, 0.0f, (float)(PANEL_H - PADDLE_H));
    s_pbx = 120 - s_pbx;
    s_pvx = -s_pvx;
    s_pvy = constrain(s_pvy + rnd(-8.0f, 8.0f), -22.0f, 22.0f);
  }

  // A point: the score catches up with the time, and the side that missed
  // serves again.
  if (s_pbx < -4 || s_pbx > PANEL_W + 2)
  {
    s_pongH = ct.h;
    s_pongM = ct.m;
    pongServe(s_pbx < 0 ? -1 : 1);
    s_pongMiss = 0;
  }

  const uint16_t white = col(220, 220, 220);
  for (int16_t y = 0; y < PANEL_H; y += 4) matrix->drawFastVLine(31, y, 2, col(70, 70, 70));

  char h[4], m[4];
  if (s_pongH >= 0 && ct.ok) { snprintf(h, sizeof(h), "%02d", s_pongH); snprintf(m, sizeof(m), "%02d", s_pongM); }
  else                       { snprintf(h, sizeof(h), "--");            snprintf(m, sizeof(m), "--"); }
  drawBig(31 - 3 - bigWidth(h), 1, h, white);
  drawBig(31 + 4, 1, m, white);

  char date[20];
  clockDate(ct, date, sizeof(date), false);
  drawTinyCentered(26, date, col(90, 90, 100));

  matrix->drawFastVLine(1, (int16_t)s_plY, PADDLE_H, white);
  matrix->drawFastVLine(62, (int16_t)s_prY, PADDLE_H, white);
  matrix->fillRect((int16_t)s_pbx, (int16_t)s_pby, 2, 2, white);
}

// ---------------------------------------------------------------- tetris

// Each digit built from falling blocks: when it changes, the old one clears
// and the new one's 2x2 cells drop in one at a time, bottom row first, each
// landing where it belongs. Cells are colored four at a time, so the digit
// looks stacked from tetrominoes.
struct TetDigit
{
  int8_t  d;          // the digit being built, -1 for none
  uint8_t n;          // its cells
  uint8_t landed;
  float   fy;         // the falling cell's row
  uint8_t cell[35];   // column + 5 * row, bottom row first
};

static TetDigit s_tet[4];
static uint32_t s_tetLastUs = 0;
static const int16_t TET_X[4] = { 3, 15, 39, 51 };
static const int16_t TET_Y    = 3;

static void tetStart(TetDigit& t, int8_t d)
{
  t.d = d; t.n = 0; t.landed = 0; t.fy = -2;
  if (d < 0) return;
  for (int8_t r = 6; r >= 0; r--)
    for (uint8_t k = 0; k < 5; k++)
      if (digitBit(d, k, r)) t.cell[t.n++] = k + 5 * r;
}

static void tetrisReset()
{
  for (TetDigit& t : s_tet) tetStart(t, -1);
  s_tetLastUs = micros();
}

static uint16_t tetColor(uint8_t digitPos, uint8_t cellIdx)
{
  static const uint8_t C[7][3] = {
    { 0, 220, 220 }, { 240, 220, 0 }, { 170, 0, 240 }, { 0, 220, 0 },
    { 240, 0, 0 },   { 0, 80, 255 },  { 255, 140, 0 },
  };
  const uint8_t* c = C[(digitPos * 2 + cellIdx / 4) % 7];
  return col(c[0], c[1], c[2]);
}

static void __attribute__((noinline)) clockTetris(const ClockTime& ct)
{
  const float  dt   = clockDt(s_tetLastUs);
  const int8_t d[4] = { (int8_t)(ct.h / 10), (int8_t)(ct.h % 10), (int8_t)(ct.m / 10), (int8_t)(ct.m % 10) };

  for (uint8_t i = 0; i < 4; i++)
  {
    TetDigit& t = s_tet[i];
    if (ct.ok && t.d != d[i]) tetStart(t, d[i]);

    if (t.landed < t.n)
    {
      const uint8_t c      = t.cell[t.landed];
      const float   target = TET_Y + (c / 5) * 2;
      t.fy += 90.0f * dt;
      if (t.fy >= target) { t.landed++; t.fy = -2; }
      else matrix->fillRect(TET_X[i] + (c % 5) * 2, (int16_t)t.fy, 2, 2, tetColor(i, t.landed));
    }

    for (uint8_t k = 0; k < t.landed; k++)
    {
      const uint8_t c = t.cell[k];
      matrix->fillRect(TET_X[i] + (c % 5) * 2, TET_Y + (c / 5) * 2, 2, 2, tetColor(i, k));
    }
  }

  const uint16_t dot = col(200, 200, 200);
  matrix->fillRect(31, 7, 2, 2, dot);
  matrix->fillRect(31, 13, 2, 2, dot);

  char date[20];
  clockDate(ct, date, sizeof(date), false);
  drawTinyCentered(24, date, col(120, 120, 140));
}

// ---------------------------------------------------------------- sky

// The sky as it is outside: the colors follow the clock city's real sunrise
// and sunset, from the weather fetch, through dawn and dusk. The sun arcs
// from one horizon to the other through the day, the moon and the stars
// through the night, over a line of dark hills.
struct SkyKey { uint8_t top[3], bot[3]; };
static const SkyKey SKY_NIGHT = { {  0,  0, 12 }, {   5,   5, 35 } };
static const SkyKey SKY_DAWN  = { { 30, 20, 80 }, { 220,  90, 40 } };
static const SkyKey SKY_DAY   = { {  0, 30,110 }, {  30,  90,170 } };
static const SkyKey SKY_DUSK  = { { 20, 10, 70 }, { 200,  70, 30 } };

static uint8_t mix8(uint8_t a, uint8_t b, float f) { return (uint8_t)(a + (b - a) * f); }

static void skyBlend(const SkyKey& a, const SkyKey& b, float f, SkyKey& out)
{
  for (uint8_t k = 0; k < 3; k++)
  {
    out.top[k] = mix8(a.top[k], b.top[k], f);
    out.bot[k] = mix8(a.bot[k], b.bot[k], f);
  }
}

static void __attribute__((noinline)) clockSky(const ClockTime& ct)
{
  // Six and six when no city has said otherwise.
  const int16_t rise = ct.sunrise >= 0 ? ct.sunrise : 360;
  const int16_t set  = ct.sunset  >= 0 ? ct.sunset  : 1080;
  const float   m    = ct.h * 60 + ct.m + ct.s / 60.0f;

  // An hour either side of sunrise goes night to dawn to day; an hour either
  // side of sunset, day to dusk to night.
  SkyKey k = SKY_NIGHT;
  if (m >= rise - 60 && m < rise)      skyBlend(SKY_NIGHT, SKY_DAWN, (m - (rise - 60)) / 60.0f, k);
  else if (m >= rise && m < rise + 60) skyBlend(SKY_DAWN, SKY_DAY, (m - rise) / 60.0f, k);
  else if (m >= rise + 60 && m < set - 60) k = SKY_DAY;
  else if (m >= set - 60 && m < set)   skyBlend(SKY_DAY, SKY_DUSK, (m - (set - 60)) / 60.0f, k);
  else if (m >= set && m < set + 60)   skyBlend(SKY_DUSK, SKY_NIGHT, (m - set) / 60.0f, k);

  for (int16_t y = 0; y < PANEL_H; y++)
  {
    const float f = y / (float)(PANEL_H - 1);
    matrix->drawFastHLine(0, y, PANEL_W, col(mix8(k.top[0], k.bot[0], f), mix8(k.top[1], k.bot[1], f),
                                             mix8(k.top[2], k.bot[2], f)));
  }

  const bool day = m >= rise && m < set;
  if (day)
  {
    const float f = (m - rise) / (float)(set - rise);
    matrix->fillCircle(4 + (int16_t)(f * 56), 24 - (int16_t)(sinf(f * 3.1416f) * 20), 3, col(255, 200, 40));
  }
  else
  {
    // The night runs from sunset to the next sunrise, across midnight.
    const float len   = 1440 - (set - rise);
    float       since = m - set;
    if (m < set) since += 1440;
    const float f = since / len;

    // Stars only once it is properly dark, each twinkling on its own beat.
    if (m >= set + 40 || m < rise - 40)
    {
      static const int8_t STARS[9][2] = { { 5, 3 }, { 20, 9 }, { 44, 2 }, { 58, 7 }, { 33, 14 },
                                          { 9, 18 }, { 52, 17 }, { 27, 4 }, { 14, 11 } };
      const uint32_t t = millis() / 400;
      for (uint8_t i = 0; i < 9; i++)
        if ((t + i * 7) % 11) matrix->drawPixel(STARS[i][0], STARS[i][1], col(200, 200, 220));
    }
    // A crescent drawn pixel by pixel - the disc, less a second disc offset
    // up and right. Covering it with a sky-colored disc instead left a dark
    // patch, since the sky is a gradient and no one color matches it.
    const int16_t mx = 4 + (int16_t)(f * 56), my = 24 - (int16_t)(sinf(f * 3.1416f) * 20);
    for (int16_t dy = -3; dy <= 3; dy++)
      for (int16_t dx = -3; dx <= 3; dx++)
        if (dx * dx + dy * dy <= 10 && (dx - 2) * (dx - 2) + (dy + 1) * (dy + 1) > 10)
          matrix->drawPixel(mx + dx, my + dy, col(230, 225, 170));
  }

  // Hills, fixed: two sine waves make a skyline that never looks ruled.
  const uint16_t hill = day ? col(10, 40, 15) : col(3, 12, 5);
  for (int16_t x = 0; x < PANEL_W; x++)
  {
    const int16_t h = 29 - (int16_t)(2 + 2 * sinf(x / 6.0f) + sinf(x / 2.3f));
    matrix->drawFastVLine(x, h, PANEL_H - h, hill);
  }

  char hm[8], date[20];
  clockHM(ct, hm, sizeof(hm), true);
  clockDate(ct, date, sizeof(date), false);
  drawOutlined(3, 3, hm, 2, col(255, 255, 255));
  drawTinyCentered(21, date, col(230, 230, 240));
}

// ---------------------------------------------------------------- analog

// A round face on the left, hour, minute and a sweeping red second hand;
// the weekday, the day of the month and the month on the right.
static void __attribute__((noinline)) clockAnalog(const ClockTime& ct)
{
  const int16_t cx = 15, cy = 15;
  matrix->drawCircle(cx, cy, 15, col(50, 50, 60));
  for (uint8_t k = 0; k < 12; k++)
  {
    const float a = k * 0.5236f - 1.5708f;
    const bool  q = (k % 3) == 0;
    const uint16_t c = q ? col(220, 220, 220) : col(120, 120, 130);
    matrix->drawPixel(cx + (int16_t)lroundf(cosf(a) * 13), cy + (int16_t)lroundf(sinf(a) * 13), c);
    if (q) matrix->drawPixel(cx + (int16_t)lroundf(cosf(a) * 12), cy + (int16_t)lroundf(sinf(a) * 12), c);
  }

  if (ct.ok)
  {
    auto hand = [&](float turns, int16_t len, uint16_t c)
    {
      const float a = turns * 6.2832f - 1.5708f;
      matrix->drawLine(cx, cy, cx + (int16_t)lroundf(cosf(a) * len), cy + (int16_t)lroundf(sinf(a) * len), c);
    };
    const float sec = ct.s + ct.sub;
    hand(((ct.h % 12) + ct.m / 60.0f) / 12.0f, 7, col(240, 240, 240));
    hand((ct.m + sec / 60.0f) / 60.0f, 11, col(240, 240, 240));
    hand(sec / 60.0f, 12, col(255, 40, 30));
  }
  matrix->drawPixel(cx, cy, col(255, 40, 30));

  // The right half is 31 columns from x 33.
  if (!ct.ok) { drawTiny(36, 13, "SYNC", C_DIM()); return; }
  const char* dow = CLOCK_DOW[ct.wday];
  drawBig(33 + (31 - bigWidth(dow)) / 2, 2, dow, C_LABEL());

  const uint8_t d1 = ct.mday / 10, d2 = ct.mday % 10;
  const int16_t w  = d1 ? 22 : 10;
  const int16_t x  = 33 + (31 - w) / 2;
  if (d1) drawDigit(x, 11, d1, 2, col(255, 255, 255));
  drawDigit(d1 ? x + 12 : x, 11, d2, 2, col(255, 255, 255));

  if (ct.utc) drawTiny(33 + (31 - tinyWidth("UTC")) / 2, 27, "UTC", C_DIM());
  else        drawTiny(33 + (31 - tinyWidth(CLOCK_MON[ct.mon])) / 2, 27, CLOCK_MON[ct.mon], col(150, 150, 160));
}

// ---------------------------------------------------------------- platformer

// A little plumber in a red cap runs in along a brick ground, jumps and hits
// a "?" box with his head; a mushroom rises out of it, slides off and drops,
// and he chases it down and grows - then runs off, and it starts again. The
// art is drawn for this clock in the style of the old platform games, not
// copied from any of them.
//
// Each round is a script played from its start time, every position worked
// out from the seconds into it, so the frame rate cannot change its pace.
static const char* const PL_SMALL[2][8] = {
  { "..RRRR..", ".RRRRRRR", ".KKSSES.", ".SSSSSSS", "..RBBR..", ".BBBBBB.", ".BB..BB.", "KK....KK" },
  { "..RRRR..", ".RRRRRRR", ".KKSSES.", ".SSSSSSS", "..RBBR..", "..BBBB..", "..BBBB..", "..KKKK.." },
};
static const char* const PL_BIG[2][16] = {
  { "..RRRR..", ".RRRRRRR", ".KKSSES.", ".KSSSSSS", ".SSSSSS.", "..SSSS..", ".RRBBRR.", "RRRBBRRR",
    "SSRBBRSS", "SSBYBYSS", "..BBBB..", ".BBBBBB.", ".BBB.BBB", ".BB...BB", "KKK...KK", "KKK..KKK" },
  { "..RRRR..", ".RRRRRRR", ".KKSSES.", ".KSSSSSS", ".SSSSSS.", "..SSSS..", ".RRBBRR.", "RRRBBRRR",
    "SSRBBRSS", "SSBYBYSS", "..BBBB..", "..BBBB..", "..BBBB..", "..BBBB..", "..KKKK..", "..KKKK.." },
};
static const char* const PL_MUSH[8] = {
  "..RRRR..", ".RRWWRR.", "RRWWWWRR", "RWRRRRWR", "RRRRRRRR", ".TTETET.", ".TTTTTT.", "..TTTT..",
};
static const char* const PL_QBOX[8] = {
  ".DDDDDD.", "DQQWWQQD", "DQWQQWQD", "DQQQQWQD", "DQQQWQQD", "DQQQQQQD", "DQQQWQQD", ".DDDDDD.",
};
static const char* const PL_USED[8] = {
  ".DDDDDD.", "DUUUUUUD", "DUUUUUUD", "DUUUUUUD", "DUUUUUUD", "DUUUUUUD", "DUUUUUUD", ".DDDDDD.",
};

static uint16_t plColor(char c)
{
  switch (c)
  {
    case 'R': return col(220,  30,  30);   // cap, shirt, mushroom
    case 'S': return col(255, 180, 120);   // skin
    case 'K': return col(110,  50,  10);   // hair, shoes
    case 'B': return col( 40,  70, 230);   // overalls
    case 'Y': return col(255, 210,   0);   // buttons
    case 'W': return col(255, 255, 255);   // spots, "?"
    case 'T': return col(240, 210, 160);   // mushroom stem
    case 'E': return col( 20,  20,  20);   // eyes
    case 'Q': return col(250, 160,   0);   // box
    case 'D': return col(130,  60,  10);   // box edge
    case 'U': return col(150,  80,  30);   // spent box
  }
  return 0;
}

static void plSprite(int16_t x, int16_t y, const char* const* rows, uint8_t h)
{
  for (uint8_t r = 0; r < h; r++)
    for (uint8_t c = 0; c < 8; c++)
    {
      const char ch = rows[r][c];
      if (ch == '.') continue;
      const int16_t px = x + c, py = y + r;
      if (px >= 0 && px < PANEL_W && py >= 0 && py < PANEL_H) matrix->drawPixel(px, py, plColor(ch));
    }
}

static uint32_t s_plStart = 0;

static const int16_t PL_BOX_X  = 44;
static const int16_t PL_BOX_Y  = 8;
static const int16_t PL_FLOOR  = 28;       // first row of the ground
static const float   PL_RUN    = 18.0f;    // px per second
static const float   PL_ROUND  = 9.5f;     // seconds, pause included

static void __attribute__((noinline)) clockPlatformer(const ClockTime& ct)
{
  float t = (millis() - s_plStart) / 1000.0f;
  if (t >= PL_ROUND) { s_plStart = millis(); t = 0; }

  // The script. Small, he is 8 rows tall and stands at y 20; big, 16 and 12.
  const float HIT = 3.2f;                   // head meets the box
  bool    big   = false;
  bool    run   = false;
  int16_t hx    = -8, hy = PL_FLOOR - 8;
  if (t < 2.9f)       { hx = (int16_t)(-8 + PL_RUN * t); run = true; }
  else if (t < 3.5f)  { hx = PL_BOX_X; hy -= (int16_t)lroundf(4 * sinf((t - 2.9f) / 0.6f * 3.1416f)); }
  else if (t < 5.4f)  { hx = PL_BOX_X; }
  else if (t < 5.86f) { hx = (int16_t)(PL_BOX_X + PL_RUN * (t - 5.4f)); run = true; }
  else if (t < 6.76f)
  {
    // Caught it: flickering between sizes, as growing goes in these games.
    hx  = 52;
    big = ((int)((t - 5.86f) / 0.1f)) & 1;
  }
  else                { hx = (int16_t)(52 + PL_RUN * (t - 6.76f)); big = true; run = true; }

  // The box: bumped up two rows just after the hit, spent from then on.
  const bool    spent = t >= HIT;
  const int16_t boxY  = (t >= HIT && t < HIT + 0.15f) ? PL_BOX_Y - 2 : PL_BOX_Y;

  // The mushroom: rises out of the box top, slides off its right edge,
  // drops to the ground and keeps going, until he catches it.
  bool    mush = false;
  int16_t mx = PL_BOX_X, my = PL_BOX_Y;
  if (t >= HIT && t < 5.86f)
  {
    mush = true;
    if (t < 3.8f)       { my = PL_BOX_Y - (int16_t)(8 * (t - HIT) / 0.6f); }
    else if (t < 4.8f)  { my = PL_BOX_Y - 8; mx = (int16_t)(PL_BOX_X + 8 * (t - 3.8f)); }
    else if (t < 5.3f)  { const float f = (t - 4.8f) / 0.5f;
                          my = PL_BOX_Y - 8 + (int16_t)((PL_FLOOR - 8 - (PL_BOX_Y - 8)) * f * f);
                          mx = (int16_t)(52 + 8 * (t - 4.8f)); }
    else                { my = PL_FLOOR - 8; mx = (int16_t)(56 + 8 * (t - 5.3f)); }
  }

  // The time first, so the mushroom rises in front of nothing and the
  // plumber passes in front of the date.
  char hm[8], date[16];
  clockHM(ct, hm, sizeof(hm), true);
  clockDate(ct, date, sizeof(date), true);
  drawBig(2, 1, hm, col(255, 255, 255));
  drawTiny(2, 10, date, C_DIM());

  // The ground: a band of bricks with mortar lines, staggered row to row.
  for (int16_t y = PL_FLOOR; y < PANEL_H; y++) matrix->drawFastHLine(0, y, PANEL_W, col(190, 90, 30));
  matrix->drawFastHLine(0, PL_FLOOR, PANEL_W, col(240, 150, 80));
  matrix->drawFastHLine(0, PL_FLOOR + 2, PANEL_W, col(90, 35, 5));
  for (int16_t x = 0; x < PANEL_W; x += 8)
  {
    matrix->drawFastVLine(x, PL_FLOOR, 2, col(90, 35, 5));
    matrix->drawFastVLine(x + 4, PL_FLOOR + 3, 1, col(90, 35, 5));
  }

  // The mushroom is drawn before the box, so while it rises it comes out
  // from behind the box's top edge rather than in front of it.
  if (mush) plSprite(mx, my, PL_MUSH, 8);
  plSprite(PL_BOX_X, boxY, spent ? PL_USED : PL_QBOX, 8);

  const uint8_t frame = run ? ((uint8_t)(t / 0.12f) & 1) : 1;
  if (big) plSprite(hx, PL_FLOOR - 16, PL_BIG[frame], 16);
  else     plSprite(hx, hy, PL_SMALL[frame], 8);
}

// ---------------------------------------------------------------- clock face

static uint8_t s_clockTheme = 0xFF;          // the theme last drawn, to reset on a change
static bool    s_clockBack  = false;         // the clock has just come round again in the rotation
static bool    s_pongLive   = false;         // a pong game is under way, to carry on with

static uint8_t clockThemeIndex(const Config& cfg)
{
  for (uint8_t i = 0; i < CT_COUNT; i++)
    if (cfg.clockTheme == CLOCK_THEMES[i]) return i;
  return CT_MATRIX;
}

// Starts the themes afresh: called when the clock comes round in the
// rotation and when the theme changes, so each starts from its beginning
// rather than mid-animation - Tetris builds its digits, the platformer plays
// its scene from the top.
//
// Except pong, which is a game in progress rather than an animation: it
// carries on where it left off, as if paused while the tickers had the panel.
// The score catches up through its usual rule - a minute that turned while it
// was away makes a side miss. Only the first time, or choosing pong again
// after another theme, starts a new game.
static void clockReset(const Config& cfg)
{
  const ClockTime ct = clockNow(cfg);
  const uint8_t   th = clockThemeIndex(cfg);
  if (th != CT_PONG || !s_pongLive || th != s_clockTheme) pongReset(ct);
  s_pongLive = (th == CT_PONG);

  s_clockTheme = th;
  s_clockBack  = false;
  matrixReset();
  flipReset(ct);
  tetrisReset();
  s_plStart = millis();
}

static void drawClock(const Config& cfg)
{
  if (s_clockBack || clockThemeIndex(cfg) != s_clockTheme) clockReset(cfg);

  const ClockTime ct = clockNow(cfg);
  matrix->fillScreen(0);
  switch (s_clockTheme)
  {
    case CT_FLIP:       clockFlip(ct);       break;
    case CT_RETRO:      clockRetro(ct);      break;
    case CT_PONG:       clockPong(ct);       break;
    case CT_TETRIS:     clockTetris(ct);     break;
    case CT_SKY:        clockSky(ct);        break;
    case CT_ANALOG:     clockAnalog(ct);     break;
    case CT_PLATFORMER: clockPlatformer(ct); break;
    default:            clockMatrix(ct);     break;
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

// The boot splash: the wordmark from the web page, "up" in white and "tick" in
// its green, with the firmware version tucked into the bottom-right corner.
// The colours are the brand's, not the palette's, so tuning the palette never
// changes the logo.
//
// FreeSansBold 9pt sets "uptick" 52 pixels wide and 17 tall, 12 above the
// baseline and 5 below it for the p. Its ink starts one pixel right of the
// cursor, hence the -1 when centring. The logo is centred in the 27 rows above
// the version line.
static void drawSplash()
{
  const int16_t LOGO_W = 52, LOGO_H = 17, LOGO_ASCENT = 12;
  const int16_t verTop = PANEL_H - TINY_H;
  const int16_t logoTop = (verTop - LOGO_H) / 2;

  matrix->fillScreen(0);
  matrix->setFont(&FreeSansBold9pt7b);
  matrix->setCursor((PANEL_W - LOGO_W) / 2 - 1, logoTop + LOGO_ASCENT);
  matrix->setTextColor(col(255, 255, 255));
  matrix->print("up");
  matrix->setTextColor(col(63, 185, 80));   // --up on the web page
  matrix->print("tick");
  matrix->setFont(NULL);

  const char* ver = "v" UPTICK_VERSION;
  drawTiny(PANEL_W - tinyWidth(ver), verTop, ver, col(110, 120, 130));
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
  clearFrame();
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

// Stale means "we tried and could not refresh this", not "this is old". Age
// alone is a bad signal when polling is gated on what is visible: a feed is
// only refreshed when its turn comes round, so being a rotation old is the
// normal schedule working, not a fault.
//
// The age term stays only as a catch-all for the case that records no failure
// at all - the link dropping, so no fetch is ever attempted and the price just
// ages. The threshold is far beyond anything the rotation can produce, so it
// cannot fire on healthy operation.
static bool isStale(const Config& cfg, const Quote& q)
{
  const uint32_t rotationMs = (uint32_t)cfg.dwellSec * 1000UL * cfg.feedCount;
  const uint32_t deadMs     = (uint32_t)cfg.refreshSec * 10000UL + rotationMs * 3;
  return q.valid && (q.failures > 0 || millis() - q.fetchedMs > deadMs);
}

// Label and change on one line, the price on the line below: the top half of
// the chart screen, and a whole entry in the vertical ticker, so the two read
// alike. Takes 15 rows from yTop - 7, a blank, 7.
//
// withCurrency is for the chart screen only. The tickers print figures without
// a symbol, because a Yahoo feed that has only been batch-fetched does not know
// its currency, and a list where BTC has a "$" and TSLA does not reads as a
// mistake.
static void drawQuoteHeader(const Feed& f, const Quote& q, bool stale, int16_t yTop,
                            bool withCurrency, uint8_t market = MKT_OPEN)
{
  const char*    label  = f.label.c_str();
  const uint16_t cLabel = stale ? C_STALE() : C_LABEL();

  // A quote with neither a 24h figure nor a chart has no change to show, and
  // printing 0.00% for it would claim a flat day that nobody measured.
  if (q.has24h || q.count >= 2)
  {
    // The number is tinted by the number, the chart by the chart - the two
    // disagree routinely, and colouring a falling line green to match a
    // positive 24h figure would be a lie. Direction is the triangle; the
    // figure itself is unsigned.
    const float    shown = displayChange(q);
    const bool     up    = shown >= 0.0f;
    const uint16_t tint  = up ? C_PCT_UP() : C_PCT_DOWN();

    char pct[16];
    snprintf(pct, sizeof(pct), "%.2f%%", fabsf(shown));

    // Label left, change right. Both drop to the small font only if they would
    // collide - a big move like 123.45% is what pushes it over.
    const int16_t pctW = arrowPctWidth(pct, true);
    if (bigWidth(label) + pctW + 2 <= PANEL_W)
    {
      drawBig(0, yTop, label, cLabel);
      drawArrowPct(PANEL_W - pctW, yTop, pct, up, true, tint);
    }
    else
    {
      drawTiny(0, yTop + 1, label, cLabel);
      drawArrowPct(PANEL_W - arrowPctWidth(pct, false), yTop + 1, pct, up, false, tint);
    }
  }
  else
  {
    drawBig(0, yTop, label, cLabel);
  }

  // One row left blank so the two lines do not touch.
  // The market icon goes at the right edge, under the change figure, and the
  // price gives up that width to it - it drops to the small font sooner rather
  // than running underneath.
  if (market == MKT_OPEN)
  {
    drawPriceLine(yTop + 8, q.price, f.decimals, C_PRICE(), withCurrency ? ccyPrefix(q) : "");
  }
  else
  {
    drawPriceLine(yTop + 8, q.price, f.decimals, C_PRICE(), withCurrency ? ccyPrefix(q) : "",
                  PANEL_W - marketIconWidth(market) - 2);
    drawMarketIcon(yTop + 8, market);
  }
}

static void drawSingleChart(const Config& cfg, const Feed& f, const Quote& q, bool stale)
{
  // The chart's shape is worked out before the frame is cleared, so the snow,
  // drawn as part of the clear, knows where to land.
  int16_t    colY[PANEL_W];
  const bool haveChart = chartColumns(q, 16, PANEL_H - 1, colY);
  snowSurface(haveChart ? colY : nullptr, &f);
  clearFrame();

  // Rows 0-14: label, change and price, with the market icon for stocks.
  drawQuoteHeader(f, q, stale, 0, true, marketState(q));

  // Rows 16-31: chart, colored by its own window rather than the 24h figure.
  if (!haveChart) return;
  const bool chartUp = q.changePct >= 0.0f;
  drawChart(colY, PANEL_H - 1,
            chartUp ? C_FILL_UP()  : C_FILL_DOWN(),
            chartUp ? C_TRACE_UP() : C_TRACE_DOWN());

  // A snowman or a tree standing on it, while it snows.
  if (s_snowOn) drawDecoration(colY);
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

  // Same order as PAL_NAMES, four across and four down, so the web page's
  // picker grid and the panel read left to right the same way.
  for (uint8_t i = 0; i < P_COUNT; i++)
  {
    const int16_t x = (i % 4) * 16;
    const int16_t y = (i / 4) * 8;
    matrix->fillRect(x, y, 16, 7, P(i));
  }
}


// ---------------------------------------------------------------- ticker

// Two tapes crawling right to left, the way an exchange board or a business
// channel's crawl does. The first half of the feed list runs along the top,
// the second half along the bottom, so reordering the feeds is how to choose
// which goes where. A single feed gets one row, centred.
//
// Each row is a 16 px section holding two lines of the 5x7 font, 7 + 1 + 7 + 1,
// so every entry is a small block - label and change above, price below:
//
//   BTC ^1.73%   ETH v2.52%
//   84,434       2,676
//
// The change sits behind an up or down triangle coloured by direction. There
// is no currency symbol: a tape is read as figures, and a prefix would only be
// right for the feeds priced in dollars. A block is only as wide as its longer
// line, which makes it around 40% narrower than the same entry on one line -
// so more of them pass at the same speed.
//
// An entry's text is frozen when it enters at the right edge. A price that
// changed mid-crawl would change the entry's width while it moved and shunt
// everything behind it; instead the next pass carries the new figure, which is
// how a real tape behaves. Each row holds only the entries currently in view,
// and takes the next feed as soon as there is room behind the last one.

static const int16_t TICK_GAP   = 12;   // blank columns between entries
static const uint8_t TICK_SLOTS = 6;    // most entries a row can have in view

struct TickEntry
{
  int16_t  x;               // left edge; negative while it leaves
  int16_t  w;               // width of the wider line, plus the gap after it
  char     label[12];
  char     price[16];
  char     pct[12];         // triangle included; empty when there is no change figure
  uint16_t cLabel, cPrice, cPct;
};

struct TickRow
{
  TickEntry e[TICK_SLOTS];
  uint8_t   count;
  uint8_t   next;           // next feed to enter, counted from the row's first
};

static TickRow s_rows[2];

// The rotation's hook into the tickers. When anything else is to play after
// them (s_lapWanted, set by the task), each ticker reports the end of a full
// pass through the feeds in s_lapDone, and the task moves on to the weather
// or the clock.
//
// The horizontal tape has no natural end, so it is given one: once the top
// row has fed in its last feed, s_tickDrain stops both rows taking any more,
// the tape scrolls off empty, and the pass is over. Afterwards it restarts
// full from the left, the way it does when the mode is first chosen.
static bool s_lapWanted = false;
static bool s_lapDone   = false;
static bool s_tickDrain = false;

static void tickerReset()
{
  memset(s_rows, 0, sizeof(s_rows));
  s_tickDrain = false;
}

static void tickerEntry(const Config& cfg, uint8_t feed, int16_t x, TickEntry& t)
{
  Quote q;
  {
    QuoteLock lk;
    q = g_quotes[feed];
  }

  const Feed& f = cfg.feeds[feed];
  t.x      = x;
  t.cLabel = isStale(cfg, q) ? C_STALE() : C_LABEL();
  t.cPrice = C_PRICE();
  t.cPct   = C_DIM();
  t.pct[0] = 0;
  snprintf(t.label, sizeof(t.label), "%s", f.label.c_str());

  if (!q.valid)
  {
    snprintf(t.price, sizeof(t.price), "--");
  }
  else
  {
    formatPrice(t.price, sizeof(t.price), q.price, f.decimals, "");

    // The 24h figure where there is one, the chart's own window otherwise -
    // the same rule every other screen uses. A feed with neither shows none.
    if (q.has24h || q.count >= 2)
    {
      const float chg = displayChange(q);
      snprintf(t.pct, sizeof(t.pct), "%c%.2f%%", chg >= 0.0f ? ARROW_UP : ARROW_DOWN, fabsf(chg));
      t.cPct = chg >= 0.0f ? C_PCT_UP() : C_PCT_DOWN();
    }
  }

  // The 5x7 font advances 6 px a character, the space before the change
  // included. The block is as wide as its longer line.
  const int16_t upper = (int16_t)(strlen(t.label) + (t.pct[0] ? 1 + strlen(t.pct) : 0)) * BIG_W;
  const int16_t lower = (int16_t)strlen(t.price) * BIG_W;
  t.w = (upper > lower ? upper : lower) + TICK_GAP;
}

// Moves one section a pixel left, retires what has left, feeds the right edge
// and draws what is in view. first and n are its slice of the feed list; yTop
// is the top of its 16 px band.
static void tickerRow(const Config& cfg, TickRow& r, uint8_t first, uint8_t n, int16_t yTop,
                      bool* wrapped = nullptr)
{
  for (uint8_t k = 0; k < r.count; k++) r.e[k].x--;

  while (r.count && r.e[0].x + r.e[0].w <= 0)
  {
    memmove(&r.e[0], &r.e[1], sizeof(TickEntry) * (r.count - 1));
    r.count--;
  }

  if (n && !s_tickDrain)
  {
    if (r.next >= n) r.next = 0;          // the list shrank under us

    // An empty row starts at the left edge, so switching to the ticker shows
    // something at once rather than waiting for the first entry to crawl in.
    while (r.count < TICK_SLOTS)
    {
      const int16_t at = r.count ? r.e[r.count - 1].x + r.e[r.count - 1].w : 0;
      if (at > PANEL_W) break;

      tickerEntry(cfg, first + r.next, at, r.e[r.count++]);
      r.next = (r.next + 1) % n;
      if (r.next == 0 && wrapped) *wrapped = true;   // every feed in this row has entered
    }
  }

  for (uint8_t k = 0; k < r.count; k++)
  {
    const TickEntry& t = r.e[k];

    drawBig(t.x, yTop, t.label, t.cLabel);
    if (t.pct[0]) drawBig(t.x + (int16_t)(strlen(t.label) + 1) * BIG_W, yTop, t.pct, t.cPct);
    drawBig(t.x, yTop + 8, t.price, t.cPrice);
  }
}

static void drawTicker(const Config& cfg)
{
  clearFrame();

  bool          wrapped = false;
  const uint8_t n       = cfg.feedCount;
  if (n >= 2)
  {
    // Top takes the odd one out.
    const uint8_t top = (n + 1) / 2;
    tickerRow(cfg, s_rows[0], 0,   top,     0, &wrapped);
    tickerRow(cfg, s_rows[1], top, n - top, 16);

    // Row 15 is the blank row between the sections, so a line there touches
    // neither. It is a palette colour so it can be tuned live - or set to
    // black, which takes it away.
    matrix->drawFastHLine(0, 15, PANEL_W, C_DIVIDER());
  }
  else
  {
    tickerRow(cfg, s_rows[0], 0, n, 8, &wrapped);
    s_rows[1].count = 0;                  // so its old entries cannot return
  }

  // The top row has fed in every feed: stop feeding, and once the tape has
  // scrolled off empty the pass is complete.
  if (wrapped && s_lapWanted) s_tickDrain = true;
  if (s_tickDrain && !s_rows[0].count && !s_rows[1].count) s_lapDone = true;
}

// ---------------------------------------------------------------- vertical ticker

// The whole feed list rising up the panel and looping, two entries in view.
// Each entry is the same block as the top of the chart screen - label and
// change above, price below - 16 rows including the blank row that separates
// it from the next.
//
// With pauseSec at 0 the list crawls continuously at scrollSpeed. With a pause
// it holds each time an entry reaches the top, then rises one entry and holds
// again, the way a departure board does - easier to read, since vertical
// motion moves every character at once where a horizontal crawl brings text
// in along the direction it is read.
//
// Unlike the horizontal tape, entries are drawn live every frame rather than
// frozen: moving vertically, a price that changes width shifts nothing else.

static const int16_t V_BLOCK = 16;

static uint8_t  s_vTop    = 0;       // feed in the top slot
static uint8_t  s_vOff    = 0;       // rows the list has risen past it
static uint32_t s_vHoldTo = 0;       // millis() the current pause ends

static uint32_t s_vLapAt = 0;          // when the list last started from the top

static void verticalReset(const Config& cfg)
{
  s_vTop    = 0;
  s_vOff    = 0;
  s_vHoldTo = millis() + (uint32_t)cfg.pauseSec * 1000UL;
  s_vLapAt  = millis();
}

static void drawVerticalEntry(const Config& cfg, uint8_t feed, int16_t yTop)
{
  Quote q;
  {
    QuoteLock lk;
    q = g_quotes[feed];
  }

  const Feed& f = cfg.feeds[feed];
  if (!q.valid)
  {
    drawBig(0, yTop,     f.label.c_str(), C_LABEL());
    drawBig(0, yTop + 8, "--",            C_DIM());
    return;
  }
  drawQuoteHeader(f, q, isStale(cfg, q), yTop, false);
}

static void drawVertical(const Config& cfg)
{
  clearFrame();

  const uint8_t n = cfg.feedCount;
  if (!n) return;

  // Two entries fill the panel, so a list that short has nowhere to go.
  if (n <= 2)
  {
    for (uint8_t k = 0; k < n; k++) drawVerticalEntry(cfg, k, n == 1 ? 8 : k * V_BLOCK);

    // Nothing moves, so a pass is a feed's dwell time instead.
    if (s_lapWanted && millis() - s_vLapAt > (uint32_t)cfg.dwellSec * 1000UL) s_lapDone = true;
    return;
  }

  if (s_vTop >= n) s_vTop = 0;           // the list shrank under us

  const bool holding = cfg.pauseSec && s_vOff == 0 &&
                       (int32_t)(millis() - s_vHoldTo) < 0;
  if (!holding && ++s_vOff >= V_BLOCK)
  {
    s_vOff    = 0;
    s_vTop    = (s_vTop + 1) % n;
    if (s_vTop == 0 && s_lapWanted) s_lapDone = true;   // back at the top: a full pass
    s_vHoldTo = millis() + (uint32_t)cfg.pauseSec * 1000UL;
  }

  // Three slots: the entry leaving at the top, the one below it, and the one
  // rising in from the bottom edge. Rows above the panel are clipped.
  for (uint8_t k = 0; k < 3; k++)
    drawVerticalEntry(cfg, (s_vTop + k) % n, k * V_BLOCK - s_vOff);
}

// ---------------------------------------------------------------- lifecycle

bool displayBegin()
{
  s_panelMutex = xSemaphoreCreateMutex();

  HUB75_I2S_CFG::i2s_pins pins = {
    HUB75_R1, HUB75_G1, HUB75_B1, HUB75_R2, HUB75_G2, HUB75_B2,
    HUB75_A,  HUB75_B,  HUB75_C,  HUB75_D,  HUB75_E,
    HUB75_LAT, HUB75_OE, HUB75_CLK
  };

  HUB75_I2S_CFG mx(PANEL_W, PANEL_H, 1, pins);
  mx.double_buff = true;
  mx.i2sspeed    = (HUB75_I2S_CFG::clk_speed)HUB75_CLOCK_HZ;
  mx.setPixelColorDepthBits(PANEL_BIT_DEPTH);

  matrix = new MatrixPanel_I2S_DMA(mx);
  if (!matrix)
  {
    Serial.println("[panel] allocation failed");
    return false;
  }

  // Allocates both DMA buffers in internal RAM and starts the chain. Failure
  // here is almost always that allocation - it wants one contiguous-enough
  // block per buffer, so it is done at boot before the heap fragments.
  const uint32_t heapBefore = ESP.getFreeHeap();
  if (!matrix->begin())
  {
    Serial.println("[panel] DMA init failed - not enough internal RAM?");
    return false;
  }

  // begin() forces every line to the maximum drive strength; bring them back
  // down. See HUB75_DRIVE in pins.h for why, and what it cost WiFi.
  const int8_t lines[] = {
    HUB75_R1, HUB75_G1, HUB75_B1, HUB75_R2, HUB75_G2, HUB75_B2,
    HUB75_A,  HUB75_B,  HUB75_C,  HUB75_D,  HUB75_E,
    HUB75_LAT, HUB75_OE, HUB75_CLK
  };
  for (int8_t pin : lines)
    if (pin >= 0) gpio_set_drive_capability((gpio_num_t)pin, (gpio_drive_cap_t)HUB75_DRIVE);

  s_ok        = true;
  s_refreshHz = (uint16_t)matrix->calculated_refresh_rate;
  s_frameUs   = s_refreshHz ? 1000000UL / s_refreshHz : 10000;

  uint8_t bri;
  bool    rot;
  {
    CfgLock lk;
    bri = g_cfg.brightness;
    rot = g_cfg.rotate180;
  }
  applyBrightness(bri);
  matrix->setRotation(rot ? 2 : 0);   // or the splash comes up upside down

  // Adafruit_GFX wraps by default, and it decides using the glyph ADVANCE, not
  // its ink: a right-aligned string whose last character ends flush at column
  // 63 still trips the check and that character is redrawn at x=0 on the next
  // line, in its own colour. That is what was painting the tint colour through
  // the "$" on the price line. Every string here is laid out to fit, so wrap
  // can only ever do damage.
  matrix->setTextWrap(false);

  drawSplash();
  flip();
  s_splashAt = millis();

  Serial.printf("[panel] %ux%u, %u-bit, %u Hz, %lu MHz clock, %lu bytes DMA\n",
                PANEL_W, PANEL_H, PANEL_BIT_DEPTH, s_refreshHz,
                (unsigned long)(HUB75_CLOCK_HZ / 1000000UL),
                (unsigned long)(heapBefore - ESP.getFreeHeap()));
  return true;
}

enum : uint8_t { ANIM_NONE, ANIM_HORIZONTAL, ANIM_VERTICAL, ANIM_CHART, ANIM_WEATHER, ANIM_CLOCK };

// The parts of the rotation, in the order they play.
enum : uint8_t { SEG_TICKERS, SEG_WEATHER, SEG_CLOCK, SEG_COUNT };
static const char* const SEG_NAMES[SEG_COUNT] = { "tickers", "weather", "clock" };
static uint8_t  s_seg   = SEG_TICKERS;
static uint32_t s_segAt = 0;

void displayTask(void* arg)
{
  (void)arg;

  uint8_t  feedIdx   = 0;
  uint32_t lastSwap  = millis();
  s_taskCore = xPortGetCoreID();

  // Leave the splash up for its full time before the first real frame.
  while (s_ok && millis() - s_splashAt < SPLASH_MS)
    vTaskDelay(pdMS_TO_TICKS(50));

  uint32_t taskStart = millis();

  FrameKey lastKey;
  memset(&lastKey, 0xFF, sizeof(lastKey));

  uint8_t    lastAnim  = ANIM_NONE;
  TickType_t wake      = xTaskGetTickCount();

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
      // A reorder must not change what is on screen halfway through a dwell,
      // so the feed showing now is followed to wherever it moved to and keeps
      // its turn. The new order takes effect from the next swap onward, which
      // is also when the dwell timer would have fired anyway.
      //
      // If that feed was deleted outright the index is clamped below and the
      // next swap picks up the new order regardless.
      const String curSrc = (feedIdx < cfg.feedCount) ? cfg.feeds[feedIdx].source : String();
      const String curSym = (feedIdx < cfg.feedCount) ? cfg.feeds[feedIdx].symbol : String();

      {
        CfgLock lk;
        cfg     = g_cfg;
        seenRev = g_cfgRev;
      }

      if (curSym.length())
        for (uint8_t i = 0; i < cfg.feedCount; i++)
          if (cfg.feeds[i].source == curSrc && cfg.feeds[i].symbol == curSym)
          {
            feedIdx = i;
            break;
          }
    }

    applyBrightness(cfg.brightness);
    matrix->setRotation(cfg.rotate180 ? 2 : 0);

    if (feedIdx >= cfg.feedCount) feedIdx = 0;

    // The rotation: tickers, then weather, then clock, then round again,
    // skipping whatever is switched off. The tickers end after a full pass
    // through the feeds, the weather after every city's two screens, the clock
    // after clockSec; a part that is the only one on just stays up. The test
    // screens are never interrupted.
    const uint32_t now      = millis();
    const bool     testMode = cfg.mode == "palette_test" || cfg.mode == "ramp_test";
    const bool     tickOn   = testMode || cfg.showTickers;
    const bool     clkOn    = !testMode && cfg.showClock;
    // Weather with no city has nothing to show in a rotation - but alone it
    // says how to add one, rather than the panel showing nothing.
    const bool     wxOn     = !testMode && cfg.showWeather &&
                              (cfg.cityCount > 0 || (!cfg.showTickers && !cfg.showClock));
    const bool     segOn[SEG_COUNT] = { tickOn, wxOn, clkOn };

    s_lapWanted = tickOn && !testMode && (wxOn || clkOn);
    if (!s_lapWanted)
    {
      s_lapDone   = false;
      s_tickDrain = false;
    }

    // Moves to the next part that is on, and starts it from the beginning.
    auto advance = [&]()
    {
      uint8_t next = s_seg;
      for (uint8_t k = 1; k <= SEG_COUNT; k++)
      {
        const uint8_t cand = (s_seg + k) % SEG_COUNT;
        if (segOn[cand]) { next = cand; break; }
      }
      // The only part on comes round to itself. The weather then simply goes
      // round its cities again, but the clock must not be disturbed: treating
      // it as coming round restarted every theme each clockSec - Tetris
      // rebuilding, the platformer replaying, pong serving a new game - on a
      // panel showing nothing but the clock.
      if (next == s_seg && s_seg == SEG_CLOCK)
      {
        s_segAt = now;
        return;
      }

      const bool moved = next != s_seg;
      s_seg    = next;
      s_segAt  = now;
      s_wxCity = 0;
      s_wxScreen = 0;
      s_wxAt   = now;
      if (s_seg == SEG_CLOCK) s_clockBack = true;    // see clockReset
      if (s_seg == SEG_TICKERS)
      {
        // The scrolling tickers restart through the change of animation
        // below; the chart picks up at its first feed.
        s_lapDone = false;
        feedIdx   = 0;
        lastSwap  = now;
      }
      if (moved) Serial.printf("[disp] showing %s\n", SEG_NAMES[s_seg]);
    };

    if (!segOn[s_seg]) advance();          // what was showing has been switched off

    // The chart's pass ends when the last feed's turn does. A single feed has
    // no rotation, so its dwell is the pass.
    if (s_seg == SEG_TICKERS && now - lastSwap > (uint32_t)cfg.dwellSec * 1000UL &&
        (cfg.feedCount > 1 || s_lapWanted))
    {
      lastSwap = now;
      const bool lap = feedIdx + 1 >= cfg.feedCount;
      feedIdx = (feedIdx + 1) % cfg.feedCount;
      if (lap && s_lapWanted && cfg.mode == "single_chart") s_lapDone = true;
    }

    if (s_seg == SEG_TICKERS && s_lapWanted && s_lapDone) advance();

    // Each city's now and week screens in turn, weatherSec each.
    if (s_seg == SEG_WEATHER && now - s_wxAt >= (uint32_t)cfg.weatherSec * 1000UL)
    {
      s_wxAt = now;
      if (++s_wxScreen > 1) { s_wxScreen = 0; s_wxCity++; }
      if (s_wxCity >= cfg.cityCount) advance();
    }

    if (s_seg == SEG_CLOCK && now - s_segAt >= (uint32_t)cfg.clockSec * 1000UL) advance();

    // Only the chart shows one feed at a time. Everything else - the tickers,
    // the weather, the clock, the test and setup screens - leaves the fetcher
    // free to poll whatever is due.
    s_visibleFeed = (cfg.mode == "single_chart" && s_seg == SEG_TICKERS) ? (int8_t)feedIdx : (int8_t)-1;

    Quote q;
    {
      QuoteLock lk;
      q = g_quotes[feedIdx];
    }

    // The static screens - the chart with no theme, the setup and info screens -
    // change only when a quote lands, when the feed cycles, or when a setting
    // moves; otherwise the frame is identical to the last one. This key is
    // everything that can change them, so they are redrawn only when it moves.
    // The animated screens below skip it.
    FrameKey key;
    memset(&key, 0, sizeof(key));
    key.cfgRev    = g_cfgRev;
    key.palRev    = s_palRev;
    key.fetchedMs = q.fetchedMs;
    key.feedIdx   = feedIdx;
    key.apMode    = g_net.apMode;
    key.valid     = q.valid;
    key.market    = marketState(q);

    const bool stale    = isStale(cfg, q);
    const bool showInfo = !g_net.apMode && (millis() - taskStart < INFO_MS);

    key.stale    = stale    ? 1 : 0;
    key.showInfo = showInfo ? 1 : 0;

    // Snow restarts from a fresh, already-falling screen each time the theme
    // is turned on, rather than resuming wherever it was left.
    const bool snow = cfg.theme == "snow";
    if (!snow) s_snowReady = false;
    s_snowOn = snow;

    // The two tickers are the screens that animate, so they run on their own
    // clock and skip the change-gating below entirely - and so does the chart
    // while it snows, since the snow moves even when the chart does not. They
    // stand aside for the setup and just-joined screens, which matter more.
    const uint8_t anim = (g_net.apMode || showInfo)              ? ANIM_NONE
                       : s_seg == SEG_WEATHER                   ? ANIM_WEATHER
                       : s_seg == SEG_CLOCK                     ? ANIM_CLOCK
                       : cfg.mode == "ticker"                   ? ANIM_HORIZONTAL
                       : cfg.mode == "ticker_vertical"          ? ANIM_VERTICAL
                       : (snow && cfg.mode == "single_chart")   ? ANIM_CHART
                                                                : ANIM_NONE;
    if (anim != ANIM_NONE)
    {
      if (anim != lastAnim)
      {
        // Start the tape afresh, and the frame clock with it -
        // vTaskDelayUntil with a stale reference would otherwise race through
        // the frames it thinks it missed.
        if (anim == ANIM_HORIZONTAL)    tickerReset();
        else if (anim == ANIM_VERTICAL) verticalReset(cfg);
        wake = xTaskGetTickCount();
      }
      lastAnim = anim;

      xSemaphoreTake(s_panelMutex, portMAX_DELAY);
      waitForFlip();
      const uint32_t t0 = micros();
      if (anim == ANIM_HORIZONTAL)    drawTicker(cfg);
      else if (anim == ANIM_VERTICAL) drawVertical(cfg);
      else if (anim == ANIM_WEATHER)  drawWeather(cfg);
      else if (anim == ANIM_CLOCK)    drawClock(cfg);
      else if (!q.valid)              drawWaiting(cfg.feeds[feedIdx]);
      else                            drawSingleChart(cfg, cfg.feeds[feedIdx], q, stale);
      if (snow) drawSanta();          // over the content, unlike the snow
      s_tickerUs = micros() - t0;
      flip();
      xSemaphoreGive(s_panelMutex);

      // The tickers move one pixel per frame, so their frame period is what
      // sets the scroll speed. The chart has nothing to scroll and runs at a
      // fixed rate just for the snow.
      s_stackFree = uxTaskGetStackHighWaterMark(nullptr);
      const uint16_t fps = (anim == ANIM_CHART)   ? SNOW_FPS
                         : (anim == ANIM_WEATHER) ? WX_FPS
                         : (anim == ANIM_CLOCK)   ? CLOCK_FPS
                         : (cfg.scrollSpeed ? cfg.scrollSpeed : 20);
      const TickType_t period = pdMS_TO_TICKS(1000 / fps);
      vTaskDelayUntil(&wake, period ? period : 1);
      continue;
    }

    // Leaving a ticker: the static screen has to be drawn afresh, whether or
    // not anything in its key has moved since it was last up.
    if (lastAnim != ANIM_NONE)
    {
      lastAnim = ANIM_NONE;
      memset(&lastKey, 0xFF, sizeof(lastKey));
    }

    // A static frame has no reason to be rebuilt until something in its key
    // moves.
    if (memcmp(&key, &lastKey, sizeof(key)) != 0)
    {
      lastKey = key;

      xSemaphoreTake(s_panelMutex, portMAX_DELAY);
      waitForFlip();

      if (cfg.mode == "palette_test")      drawPaletteTest();
      else if (cfg.mode == "ramp_test")    drawRampTest();
      else if (g_net.apMode)               drawSetup();
      else if (showInfo)                   drawJoined();
      else if (!q.valid)                   drawWaiting(cfg.feeds[feedIdx]);
      else                                 drawSingleChart(cfg, cfg.feeds[feedIdx], q, stale);

      flip();
      xSemaphoreGive(s_panelMutex);
    }

    s_stackFree = uxTaskGetStackHighWaterMark(nullptr);
    vTaskDelay(pdMS_TO_TICKS(33));
  }
}
