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

static void tickerReset()
{
  memset(s_rows, 0, sizeof(s_rows));
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
static void tickerRow(const Config& cfg, TickRow& r, uint8_t first, uint8_t n, int16_t yTop)
{
  for (uint8_t k = 0; k < r.count; k++) r.e[k].x--;

  while (r.count && r.e[0].x + r.e[0].w <= 0)
  {
    memmove(&r.e[0], &r.e[1], sizeof(TickEntry) * (r.count - 1));
    r.count--;
  }

  if (n)
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

  const uint8_t n = cfg.feedCount;
  if (n >= 2)
  {
    // Top takes the odd one out.
    const uint8_t top = (n + 1) / 2;
    tickerRow(cfg, s_rows[0], 0,   top,     0);
    tickerRow(cfg, s_rows[1], top, n - top, 16);

    // Row 15 is the blank row between the sections, so a line there touches
    // neither. It is a palette colour so it can be tuned live - or set to
    // black, which takes it away.
    matrix->drawFastHLine(0, 15, PANEL_W, C_DIVIDER());
  }
  else
  {
    tickerRow(cfg, s_rows[0], 0, n, 8);
    s_rows[1].count = 0;                  // so its old entries cannot return
  }
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

static void verticalReset(const Config& cfg)
{
  s_vTop    = 0;
  s_vOff    = 0;
  s_vHoldTo = millis() + (uint32_t)cfg.pauseSec * 1000UL;
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
    return;
  }

  if (s_vTop >= n) s_vTop = 0;           // the list shrank under us

  const bool holding = cfg.pauseSec && s_vOff == 0 &&
                       (int32_t)(millis() - s_vHoldTo) < 0;
  if (!holding && ++s_vOff >= V_BLOCK)
  {
    s_vOff    = 0;
    s_vTop    = (s_vTop + 1) % n;
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

enum : uint8_t { ANIM_NONE, ANIM_HORIZONTAL, ANIM_VERTICAL, ANIM_CHART };

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
      const uint16_t fps = (anim == ANIM_CHART) ? SNOW_FPS
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
