# uptick

A small LED ticker: crypto prices with a chart, on a 64x32 HUB75 panel driven by an ESP32-S3.

## Hardware

- Adafruit Matrix Portal S3 (ESP32-S3, 8 MB flash, 2 MB PSRAM, HUB75 socket)
- P4 indoor 64x32 RGB panel, 1/16 scan, 256 x 128 mm
- 5 V supply

### Power

**The screw terminals are outputs, not inputs.** From Adafruit's pinouts page: *"they were designed as outputs ONLY - power from the USB port connects directly to these pads, so you should power from USB and then connect the matrix power inputs to these terminals."* Feeding those terminals from an external supply while USB is also plugged in can damage the board.

The HUB75 ribbon carries no power either - only signals and ground. The panel is always powered through its own spade pigtail.

That leaves two valid arrangements:

- **USB-C powers everything.** Supply into the board, panel pigtail onto the screw terminals. What the panel can draw is capped by the USB source: 500-900 mA from a laptop port, 1.5-3 A from a decent charger.
- **Panel on its own supply.** 5 V wired directly to the panel's power input, USB kept on the board for flashing and serial. Grounds tie together through the ribbon.

Never wire an external supply to the screw terminals.

The panel is 2048 LEDs. Full white is roughly 3-4 A; a ticker screen is mostly dark and sits in the hundreds of mA. The failure mode when the supply is inadequate is a brown-out - the board reboot-loops or the serial port drops - not gradual dimming. 1000 uF across the 5 V rail near the panel connector helps with power-on inrush.

### Panel wiring

1/16 scan uses address lines A-D only, so the board's address-E jumper stays alone. The 2x10 socket accepts the 2x8 IDC cable that ships with the panel.

Pin assignments live in `include/pins.h`, taken from the block Adafruit_Protomatter selects under `ARDUINO_ADAFRUIT_MATRIXPORTAL_ESP32S3` - the pins are the board's, so they carried over unchanged to the DMA driver.

## Build

```bash
pio run -t upload
```

```bash
pio run -t uploadfs
```

The second command is required whenever anything in `data/` changes - the web UI lives on LittleFS, and without it the device serves a diagnostic page instead.

`GET /api/files` reports what is actually on the filesystem. `totalBytes: 0` there means LittleFS found no partition: the Arduino wrapper looks the partition up by *label* and defaults to `spiffs`, so the entry in `partitions_8mb_ota.csv` has to be named `spiffs` even though it holds a LittleFS image. Renaming it to something descriptive silently breaks mounting, because PlatformIO still finds it by subtype and uploads the image to a partition the firmware never looks at.

Serial monitor:

```bash
pio device monitor
```

Note the monitor holds the port - close it before uploading or the flash fails with a permission error.

## First run

1. Flash firmware and filesystem, then power up.
2. With no stored credentials the device starts a setup AP called `Uptick-XXXX` and runs a captive portal. Join it; the config page should pop automatically, or browse to `192.168.4.1`.
3. Pick your network, enter the password, save. The device restarts and joins.
4. The panel shows the SSID, IP and `uptick.local` for eight seconds after joining, so the address is always recoverable without a serial cable.

Every boot opens on a splash: the wordmark from the web page, "up" in white and "tick" in green, with the firmware version in the bottom-right corner. It stays up for at least 5 seconds, counted from when the panel starts, so a slow WiFi join leaves it up no longer than the join takes. Its colours are fixed rather than taken from the palette, so tuning the palette never changes the logo.

To start over, use Forget WiFi on the config page, or erase NVS with `pio run -t erase`.

## Configuration

Settings are a single JSON document at `/config.json` on LittleFS, with a `schema` field so future firmware can migrate rather than silently misread an old shape. WiFi credentials and the CoinGecko API key live separately in NVS and are not part of the config document, so exporting config cannot leak them.

```json
{
  "schema": 1,
  "brightness": 5,
  "rotate180": false,
  "wifiSleep": false,
  "mode": "single_chart",
  "dwellSec": 15,
  "scrollSpeed": 20,
  "pauseSec": 0,
  "palette": { "DIVIDER": "#202020" },
  "refreshSec": 60,
  "feeds": [
    { "source": "binance", "symbol": "BTCUSDT", "label": "BTC", "interval": "1h", "decimals": 0 }
  ]
}
```

`mode` is `single_chart`, `ticker` (horizontal) or `ticker_vertical`. `ticker_vertical` was called `multi_price` before it scrolled; a config or backup that still says so is translated on load rather than refused, since refusing the stored config at boot would throw the whole document away, feeds included. Keys the firmware no longer uses - `bitDepth`, and `tz`, which was stored but never applied to anything - are ignored for the same reason. Up to 32 feeds (`UPTICK_MAX_FEEDS`); the web page reads the limit from `/api/status` rather than repeating it.

A label left blank is made from the symbol. Binance and Coinbase symbols carry their quote currency, which only costs width on a 64 px panel when it is a dollar, so `USDT`, `USDC`, `FDUSD`, `BUSD`, `TUSD` and `USD` quotes are dropped - BTCUSDT and BTC-USD both become BTC. Any other quote stays, since "ETH" over a price in bitcoin would misrepresent it, so ETHBTC stays ETHBTC. Stock symbols and CoinGecko ids are used as they are. The web page does this before saving and previews it in the label box as the symbol is typed; `config.cpp` does the same for a config POSTed without labels.

Factory feeds are BTC, ETH, SOL and ZEC on Binance. `decimals: 0` means "pick from magnitude" - whole dollars above $1000, cents below, four places under $1.

The `source` field is the extension point: `binance`, `coinbase` and `coingecko` for crypto and `yahoo` for stocks and everything else Yahoo Finance lists, each behind its own fetch adapter. The renderer never learns where the numbers came from.

### Symbols are checked before they are saved

Save checks every feed whose source and symbol are not already stored, and refuses a symbol its source does not recognise, marking the offending line. Unchanged feeds are not re-checked - they were checked on the way in - so saving thirty feeds after changing one costs one check.

| Source | How it is checked |
| --- | --- |
| Binance | its full symbol list, once - about 26 KB compressed |
| Yahoo | through the device, which asks spark for up to 20 at a time |
| CoinGecko | one `simple/price` request for all the ids |
| Coinbase | one request per product, in parallel |

Binance cannot be checked per symbol from a browser. It leaves the CORS header off its error responses, so the 400 for an unknown symbol is unreadable to the page - exactly what an unreachable Binance looks like. The full list is a 200 with the header.

A source that cannot be reached at all does not block the save: that says nothing about whether the symbol exists. The page saves and says how many could not be checked. Only the web page validates; a config POSTed to `/api/config` directly is trusted as given.

### Ordering feeds

The rotation follows the order of the `feeds` array, so reordering is an array move and nothing else. The web UI offers both a drag grip and up/down arrows; like every other feed edit, the new order is held until Save.

The device does not apply it mid-dwell. When the config arrives, the renderer looks up the feed that is currently on screen and follows it to its new index, so the panel keeps showing the same thing until its turn is up and the new order starts from the next rotation. A feed that was deleted outright falls through to the index clamp and the next swap picks up the new order regardless.

Quotes are indexed by feed position, so `configFromJson` carries each quote across to wherever its feed moved, matched on source and symbol; anything unmatched starts empty and refetches on its turn. Without that a reorder would print the previous occupant's price under the new feed's label until the next fetch. The same applies to adding and deleting, which had the bug before reordering existed. The poller's attempt timers are indexed by position too, and are simply reset - the worst case is one early refetch.

### Backing feeds up

**`pio run -t uploadfs` erases the saved config.** It rewrites the whole `spiffs` partition from `data/`, and `/config.json` lives in that partition, so every filesystem upload takes the feed list with it. Firmware uploads do not: `pio run -t upload` writes only `app0` at `0x10000` and leaves the filesystem alone. Only run `uploadfs` when something under `data/` actually changed.

**Export** and **Import** in the Feeds card exist for the times you do. Export downloads `uptick-feeds.json`; import reads it back, validates it and loads it into the form, and the feeds are stored when you press Save. The order that matters is export, then `uploadfs`, then import.

Both run entirely in the browser - the page already holds the config, so no endpoint is involved, and exporting captures what is on screen including edits that have not been saved yet. Import accepts three shapes, because all three turn up: an export file, a whole `/api/config` document, or a bare array of feeds. Anything unknown is normalised rather than trusted - an unrecognised `source` falls back to `binance`, an unrecognised `interval` to `1h`, `decimals` is clamped to 0-8 - and a file that fails validation leaves the feeds on screen untouched.

`interval` is the chart candle width, not the poll rate. 64 candles fill the panel's 64 columns, so it sets how far back the chart reaches. Yahoo has no 4h bar, so `4h` rounds to daily there - the same compromise Coinbase forces by rounding it up to 6h.

### Saving

Every setting the web page changes is one config document, so there is one Save: a bar that appears at the foot of the page whenever anything is unsaved, on any tab, and saves all of it. Brightness, mode, scroll speed, pause, power save and colours apply to the panel as soon as they change, so the effect can be judged before saving; everything else waits for Save. Pausing fetching and the CoinGecko key have their own buttons and are not part of the config.

Power save used to be applied only when WiFi joined, so saving a change to it did nothing until a restart. A config POST now applies it straight away.

### REST API

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/status` | mode, IP, RSSI, heap, uptime, colour depth and refresh rate, core assignments, task stack headroom, ticker frame time, feed limit |
| GET / POST | `/api/config` | the config document |
| POST | `/api/brightness` | live brightness, not persisted |
| POST | `/api/speed` | live scroll speed for both tickers, not persisted |
| POST | `/api/pause` | live vertical-ticker pause, not persisted |
| POST | `/api/mode` | live display mode, not persisted; the web page's mode picker uses it, so a mode applies as soon as it is chosen |
| GET / POST | `/api/palette` | live colour overrides by name, not persisted - the config's `palette` is what persists |
| POST | `/api/defaults` | restore factory settings and colours, leaving WiFi alone |
| POST | `/api/apikey` | store the CoinGecko key (write-only) |
| POST | `/api/fetch` | pause or resume market polling |
| GET | `/api/check?symbol=` | one Yahoo ticker's price, name and currency, for the check button (Yahoo sends no CORS header) |
| GET | `/api/check?symbols=A,B` | which of several Yahoo tickers exist, for the check before saving |
| POST | `/api/wifisleep` | toggle WiFi power save |
| GET | `/api/files` | list LittleFS contents |
| GET | `/api/scan` | visible networks |
| POST | `/api/wifi` / `/api/forget` | store or erase credentials, then restart |
| POST | `/api/update` | OTA firmware upload |

Plain HTTP on the LAN. Do not port-forward it.

## Architecture

Core 0 runs the radio, the web server and the market fetch. Core 1 runs the display. Confirmed rather than assumed - `/api/status` reports `coreDisplay` and `coreNet`.

Task stacks are measured, not guessed: `/api/status` reports `stackNet` and `stackDisplay`, the least free stack each task has ever had. The net task has 12 KB because it runs TLS handshakes and, on a save, holds a scratch `Config` plus two `String` arrays sized by the feed limit; measured with 32 feeds it bottoms out with about 5.6 KB free. The display task has 6 KB; the ticker's number formatting costs about 1 KB, and it bottoms out with about 3.3 KB free - on the original 4 KB it would have had 1.3 KB.

`Config` holds `String` members, so copying it deep-copies around forty strings. Both the render loop and the poll loop run far too often to do that per iteration, so `g_cfgRev` is bumped on every change and each reader caches its own copy until the revision moves.

The two mutexes (`CfgLock`, `QuoteLock`) are never held at the same time. Both readers copy config, release, then take the quote lock, so there is no lock ordering to get wrong. Where both have to move together - the quote remap on a config change - `displayPause()` brackets the pair instead, so no frame can observe the moment they disagree. That bracket must be released before `configSave()`, which pauses again and would deadlock on a non-recursive mutex.

Both guards tolerate a null handle. `configBegin()` loads the stored config through the same path a POST takes, so it takes the quote lock during `setup()` - and it used to run before `marketBegin()` created that mutex, which meant `xSemaphoreTake(nullptr, portMAX_DELAY)` and a boot that hung with the panel dark and nothing on serial past the banner. `marketBegin()` now runs first, and the null check means the same mistake degrades into working code rather than a brick.

Recovering from a hang like that is worth knowing: the S3's native USB ignores the RTS reset, so `esptool` cannot auto-enter the bootloader once the app stops servicing USB, and reports `Write timeout`. Hold BOOT, tap RESET, release BOOT to enter it by hand; flash; then press RESET, because `--after hard_reset` does not work over native USB either.

The web UI is read off LittleFS once at boot and served from a buffer in internal RAM. This is a correctness fix, not an optimisation - see the flicker notes below.

## Data sources

Binance klines, which takes an exact count so the chart needs no resampling:

```
https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1h&limit=64
```

64 candles for 64 columns. Coinbase is supported too, which matters behind a US IP since Binance blocks those. Coinbase returns newest-first and Binance oldest-first; the adapter normalises to oldest-first so the renderer never has to know.

CoinGecko is supported for coins the exchanges do not list. Its symbol is the coin id (`bitcoin`, not `BTCUSDT`), it needs a demo API key, and it chooses its own granularity from the range requested rather than taking a candle size. Its free tier allows roughly 10k calls a month, so it suits slow polling only.

### Stocks, via Yahoo Finance

```
https://query1.finance.yahoo.com/v8/finance/chart/AAPL?range=1mo&interval=60m
```

Yahoo's chart endpoint was chosen because it is the only stock source still usable without a key or a subscription. One request returns the price, the change against the previous close and the candle series together, and a 404 is how it answers an unknown ticker, so validation needs nothing extra.

It is not only stocks. The same endpoint and the same three fields cover ETFs (`SPY`), indices (`^GSPC`), futures (`GC=F`), currency pairs (`EURUSD=X`) and non-US listings (`SAP.DE`, `VOD.L`). Tickers are percent-encoded, since `^` is not legal unencoded in a path.

Two things to know about it:

- **It is unofficial.** There is no contract and no support. If it changes shape, the adapter breaks. Yahoo also rejects a `curl/*` User-Agent with a 429 - `uptick/<version>` passes, but that is the sort of filter that can tighten without notice.
- **It sends no CORS header**, where the crypto APIs do on their normal responses. The web page cannot check a ticker from the browser, so those checks go through `/api/check` on the device instead.

Not every listing is priced in dollars, so the quote currency is read from the same response and carried on the `Quote`. The panel prints `$` only when the currency really is USD - a Frankfurt or London line prints bare, since the 5x7 font has no glyph for those and a wrong symbol is worse than none. The crypto sources are all USD pairs and are unaffected.

Alternatives were measured rather than assumed. Stooq now gates its CSV behind a JavaScript proof-of-work challenge, which an ESP32 cannot solve. Alpha Vantage, Finnhub, Twelve Data, marketstack, FMP, Tiingo and Polygon all require a key. IEX Cloud is gone. MarketData.app is keyless only for AAPL.

### Parsing

Candle responses are parsed by a hand-rolled streaming scanner (`extractField`) rather than ArduinoJson. ArduinoJson filters cannot select by index inside an array, so filtering could not isolate the close, and buffering the whole document would mean holding hundreds of fields of JSON in RAM at the same moment a TLS session is open. The scanner pulls one field straight off the socket with no allocation, handles both quoted strings and bare numbers, and keeps the *last* 64 values rather than the first - CoinGecko returns far more points than asked for and the recent end is the interesting one.

The 24h endpoints are objects rather than arrays, so ArduinoJson key filtering works there and is used.

Yahoo needs a second scanner (`extractYahoo`). Its series sits three arrays deep and its quote fields come back in no fixed order - `volume`, `high`, `close`, `open` and `low` have each been observed in different positions - so index-based extraction does not work on it. That scanner walks the stream once matching literal keys - the day's change, currency, last regular trade and trading sessions from `meta`, `"timestamp":[`, and `"close":[` from `indicators.quote[0]` - and gets the price, the day's move, the market hours and the chart out of a single pass. It reads to the document's closing brace rather than stopping after the close array: the top-level order moves too, and the extended-hours request has sent `indicators` before `timestamp` where the chart request sends it after. Stopping early lost the timestamps, so every pre-market price was discarded and the panel showed the market as closed. Bars that come back `null` are dropped rather than interpolated, and an unterminated trailing number on a truncated response is discarded rather than committed, since `13` may be the first two digits of `137.5`.

### Polling schedule

The layout decides how data is fetched.

**Single chart polls the feed on screen**, one candle request at a time, so an update lands while you are looking at it and a price moving is visible. The one exception is a feed that has no data yet - after boot, or just added - which is fetched during the previous feed's dwell, so its first turn opens on its chart rather than on "waiting for data". Once it has data it refreshes on its own turn again. The rest of the list is left alone, so effective refresh is the rotation period rather than the poll interval.

Fetching only on screen once also kept every fetch at a screen transition, where a refresh glitch showed least. The DMA driver made that moot: a fetch cannot disturb the panel now.

**Both tickers batch.** They show a price and a change per feed and no chart, which is what the batch endpoints return. One request per source replaces one per feed, every poll interval:

| Source | Batch request | Per request |
| --- | --- | --- |
| Binance | `/api/v3/ticker/24hr?type=MINI&symbols=[...]` | every symbol |
| Yahoo | `/v8/finance/spark?symbols=...&range=1d&interval=1d` | 20 at most - spark answers 400 above that |
| CoinGecko | `/api/v3/simple/price?ids=...&include_24hr_change=true` | every id |
| Coinbase | `/products/{id}/stats` | one - there is no batch endpoint |

Measured on the device with eight feeds across Binance and Yahoo: two requests a cycle, about half a second apart, and nothing else until the next one. At thirty feeds it is still two or three.

Each call to `marketPoll` makes at most one request, so the web server is serviced between them. Batch data updates price and change only; charts are left as they were and refilled when the chart layout is next on.

Details that each cost something to find:

- Binance `MINI` leaves out `priceChangePercent`. It is recomputed from `openPrice`, which opens the same rolling 24 hours, so the figure is identical.
- Symbols are deduplicated before they go out. Binance refuses a list that names one twice (`-1151`), and the same symbol can legitimately sit in two feeds with different chart spans; each result is applied to every feed that carries it.
- Binance also refuses the whole list over one symbol it does not recognise (`-1121`), which is why the web page checks symbols before saving. A coin delisted after it was added would still take every Binance feed stale with it - visibly, since they would all show it.
- Yahoo and CoinGecko simply leave unknown symbols out of a mixed answer. Spark answers 404 when it recognises none of them.
- Spark carries no currency. A Yahoo feed that has only ever been batch-fetched prints no currency symbol rather than a possibly wrong `$`; its first chart fetch teaches it the real one.

### HTTP/1.0, and chunked responses

Every request is made with `useHTTP10(true)`. The parsers read `getStream()` raw, and `getStream()` does not undo chunked transfer encoding: the chunk-size lines arrive in the byte stream mixed into the JSON. CoinGecko and Coinbase both chunk over HTTP/1.1, which silently broke their 24h lookups - the parse stopped at the first chunk header - and could splice hex digits into a chart price wherever a chunk boundary fell mid-number. HTTP/1.0 has no chunked encoding; all six endpoints were checked to answer it with a plain body.

## Display

### Layout

64 x 32 is tight. Row budget for `single_chart`:

```
rows  0-6    BTC            ▲4.37%     5x7 font, 6px advance
row   7      blank
rows  8-14   $214,460                  price
row   15     blank
rows 16-31   chart, 64 cols
```

`$214,460.00` is 66 px in the 5x7 font - one pixel too wide. The price line steps down a ladder: full precision in the 5x7 font, then TomThumb 3x5 at 4 px advance, then a `$214.4K` abbreviation. Thousands separators are inserted into the integer part only.

Direction is shown by a triangle, not a sign - the same as the ticker - with the figure itself unsigned. When the label and change are too wide for the 5x7 font the line drops to TomThumb, which covers printable ASCII only, so there the triangle is drawn as a shape, 5 wide by 3 tall, centred on the small digits.

Text wrapping is turned off. Adafruit_GFX wraps using the glyph advance rather than its ink, so a right-aligned string ending flush at column 63 still trips the check, and that last character gets redrawn at x=0 on the following line in its own colour - which showed up as the percentage's green painted through the `$` of the price.

The chart scales to the min/max of the visible window, never to zero, or intraday moves would render as a flat line. The trace runs along the fill's top edge as a staircase, never as a line between column centres: `drawLine` spreads a steep move across both columns, so half of it lands above the previous column's fill as stray pixels floating over the chart.

### Ticker

Two tapes crawling right to left, the way an exchange board or a business channel's crawl does. Each is a 16 px section holding two lines of the 5x7 font - 7 + 1 + 7 + 1 - so every entry is a small block, label and change above and price below:

```
rows  0-15   BTC ▲1.73%   ETH ▼2.52%   SOL ▼2.2      first half of the feeds
             84,434       2,676        114.78
rows 16-31   TSLA ▲0.44%  SPCX ▼1.58%  EOSE ▲        second half
             380.56       152.27       3.685
```

The first half of the feed list runs along the top and the second half along the bottom, with the odd one out on top, so reordering the feeds is how to choose which section a feed is in - crypto above and stocks below, say. A single feed gets one section, centred. The change sits behind a triangle coloured by direction; CP437 `0x1E` and `0x1F` in the built-in font are solid up and down triangles. There is no currency symbol: a tape is read as figures, and a prefix would only be right for the feeds priced in dollars.

Row 15, the blank row between the sections, carries a dark grey divider in the `DIVIDER` palette colour. Being in the blank row, it touches neither section's text.

A block is only as wide as its longer line, which makes it around 40% narrower than the same entry on one line, so more of them pass at the same speed.

An entry's text is frozen when it enters at the right edge. A price that changed mid-crawl would change the block's width while it moved and shunt everything behind it, so the next pass carries the new figure instead - which is how a real tape behaves. Each section holds only the entries in view, retiring them as they leave on the left and taking the next feed as soon as there is room behind the last.

The tape moves one pixel a frame, so `scrollSpeed` (pixels per second, 10-90, default 20) is also its frame rate, paced with `vTaskDelayUntil` so it holds steady. The web page sets it with a slider that previews live through `/api/speed`, like brightness, and Save keeps it. 30 read as a little fast on the real panel with both sections moving. Much below 15 the motion starts to look stepped, because each step is a whole pixel.

Measured on the device with 32 feeds: a frame draws in about 0.75 ms, a few percent of its budget at any speed the slider allows, reported as `tickerDrawUs` in `/api/status`. Frames are drawn to the back buffer and flipped with the two-frame guard described under double buffering. The ticker is the one screen that bypasses `FrameKey` gating; it stands aside for the setup and just-joined screens.

It uses batch fetching, so thirty feeds cost two or three requests a poll cycle.

### Vertical ticker

The whole feed list rising up the panel and looping, two entries in view. Each entry is the same block as the top of the chart screen - label and change on one line, price below - drawn by the same `drawQuoteHeader`, so the two screens read alike. The tickers print figures without a currency symbol, because a Yahoo feed that has only been batch-fetched does not know its currency, and a list where BTC has a `$` and TSLA does not reads as a mistake. The chart screen keeps it.

With `pauseSec` at 0 the list crawls continuously at `scrollSpeed`. With a pause it holds each time an entry reaches the top, then rises one entry and holds again, the way a departure board does. That is easier to read: vertical motion moves every character at once, where a horizontal crawl brings text in along the direction it is read.

Unlike the horizontal tape, entries are drawn live every frame rather than frozen - moving vertically, a price that changes width shifts nothing else. Rows above the panel are clipped by the driver: its pixel path takes unsigned coordinates, so -1 wraps past the height check, and its line paths test for negatives explicitly. A frame draws in about 1.5 ms.

A list of one or two fits the panel as it is and does not move.

### The percentage is always 24h

The chart span is per-feed and adjustable; the percentage is always the fixed 24 hour change, so it stays comparable between feeds whose charts cover different spans. The two disagree routinely - a coin can be flat over a day and up 8% over a 64 hour chart - so the percentage is tinted by the percentage and the chart by the chart. Colouring a visibly falling line green to match a positive 24h figure would be a lie.

At `1h` candles the 24h figure comes free from the candles already fetched, 24 bars back. Only `5m` and `15m` feeds trigger a second request, since 64 of those candles do not span a day.

For a Yahoo feed it is the move against the previous close, not a rolling 24 hours. A rolling window is meaningless for something that trades 6.5 hours a day, and the previous close is the number every quote page prints. Yahoo supplies it in the same response, so no second request is needed during the regular session. Pre-market and after hours, the chart screen shows the extended-hours price and its change instead - see market hours below. Once trading stops the price holds and the figure stops moving - that is correct, not a stalled fetch, and the stale indicator stays off because it tracks fetch failures rather than data age.

### Market hours

On the chart screen a stock's price carries an icon at the right edge, under the change figure, saying whether it is live:

| Icon | When | Price and change shown |
| --- | --- | --- |
| none | regular session | the live price, against the previous close |
| rising sun | pre-market, 4:00-9:30 in New York | the pre-market price, against the previous close |
| crescent moon | after hours, 16:00-20:00 in New York | the after-hours price, against today's close |
| padlock | closed - overnight, weekends, holidays | the last regular close and its day change |

The hours are not hardcoded. Every Yahoo chart response carries `currentTradingPeriod`, the exchange's pre, regular and post sessions for the trading day as Unix times, so this works for any exchange, including ones with no extended hours. The device keeps UTC from SNTP (`pool.ntp.org`), started once WiFi joins, and compares against those. Until the clock syncs, no icon is shown.

The extended-hours price is a second request, made only while an extended session is on: today at 5-minute bars with `includePrePost=true`, of which only the newest real close is kept. The chart request cannot supply it - Yahoo's keyless meta has no pre- or post-market price, and asking the chart request for extended hours would put those bars in the chart too, over twice as many per day, so 64 columns would cover under half the history.

A session only counts as live if the price actually traded in it. A quote whose last trade is from before the session opened means nothing has traded yet - which is what a holiday looks like, since Yahoo can still report a regular session for one - so it shows the padlock. So do the first minutes of pre-market, before anything trades.

The state is worked out at draw time, so the icon changes when the session does, not when the feed next comes round. Crypto never shows one. The tickers do not either: Yahoo's batch endpoint breaks when asked for extended hours, answering 400 or returning timestamps with no prices, so they stay on regular-session prices.

### Palette

Fifteen named colours, all overridable live through `/api/palette` and the web UI's colour pickers: `LABEL`, `PRICE`, `PCT_UP`, `PCT_DOWN`, `FILL_UP`, `TRACE_UP`, `FILL_DOWN`, `TRACE_DOWN`, `DIM`, `ACCENT`, `STALE`, `DIVIDER`, `SUN`, `MOON`, `CLOSED`. `DIVIDER` is the line between the horizontal ticker's sections; set to black it disappears. The last three are the market-hours icons.

A picker applies its colour at once through `/api/palette`, which is RAM only, and raises the page's save bar; Save stores the palette in the config with everything else. Only the colours that differ from factory are stored, by name, so a colour nobody changed follows the factory set if that is ever retuned - and a config with no `palette` at all means factory, so what is on flash always says exactly what the panel shows. Unknown names and malformed colours are skipped rather than refused. Reset to factory, and Restore default settings, both bring the factory set back.

The palette starts at factory before anything else runs, not in `displayBegin()`: the stored config is loaded, and its colours applied, before the panel is brought up.

The factory set itself was tuned on the panel with the pickers and exported from the page with "export as C++" - a `PAL_FACTORY` table to paste into `display.cpp`. The page used to keep picks in the browser's local storage and push them back over the device on every load; that is gone now the device keeps them, and the page clears the old entry.

### Brightness

Brightness is the panel's output-enable duty cycle, set with `setBrightness8()`. Every draw call clears only the six RGB bits of each DMA word and OE is bit 7, so brightness is written once and survives all redraws; it is re-applied only when the value changes, because it walks both buffers.

The slider is linear - 5% means 5% duty, so a stored value keeps its meaning. No floor is needed: the driver keeps at least one pixel-clock of output per bit-plane for any non-zero value, so 1% is dim but never dark, and visibly different from 5%.

This replaced colour scaling, which Protomatter forced because it had no brightness control. Scaling was the cause of every palette problem this project had: dark colours scaled below what the panel could show and rendered black (measured at 4-bit and 40%: green 220 rendered, 95 and 74 were both black), and the components did not scale in proportion, so a warm red turned orange as brightness rose. With duty-cycle brightness the palette is the palette at every level, and colours simply get brighter.

### Colour depth and refresh

Depth is fixed by the build: `-DPIXEL_COLOR_DEPTH_BITS=7` in `platformio.ini`. It cannot be a runtime setting. The driver picks its CIE 1931 gamma table from that macro at compile time and uses the table's output unshifted, so a runtime depth that differs drops the top bit of every bright colour. Its own fallback for mismatched depths is dead code - the fallback branch defines `LUT_NATIVE_BIT_DEPTH` as `0` and the code tests it with `#ifdef`, which is true either way. `include/pins.h` derives the runtime depth from the same macro, so the two cannot drift. Native tables exist for 4, 6, 7, 8, 10 and 12 bits.

The gamma table also sets how dark a colour can go before it becomes black. Every palette component survives at 6 bits and above; at 4 bits, `PCT_UP`'s red (41) and `STALE`'s blue (41) both vanish.

Refresh is computed by the driver from clock, depth and scan, and reported as `refreshHz` in `/api/status`. There is nothing to sample any more: with no CPU in the refresh loop, the rate cannot vary. Computed for this panel, against what Protomatter measured:

| Depth | 16 MHz clock | Protomatter |
| --- | --- | --- |
| 5 | 781 Hz | 134 Hz |
| 6 | 422 Hz | 69 Hz |
| **7** | **223 Hz** | - |
| 8 | 115 Hz | - |

7-bit at 223 Hz is the build default, and matches what the device reports. The DMA buffers, double-buffered, cost about 48 KB of internal RAM more than Protomatter did.

The S3 divides a 160 MHz source, so the driver rounds a requested clock to 10, 16 or about 23 MHz. 16 is the one where requested and real match; asking for 8 actually runs at 10, while the driver still computes refresh from 8 and under-reports by a fifth.

### Double buffering

Drawing goes to a back buffer and `flipDMABuffer()` swaps. A flip only rewires the last descriptor of each chain, so the DMA moves across when it finishes the frame it is on - up to one frame after the call returns. Until then the buffer that drawing now targets is still on screen, and writing to it tears. The driver's wait for this is commented out on the S3, so `waitForFlip()` enforces it: nothing is drawn until two frame periods after a flip. The render loop sleeps far longer than that today; it exists for scrolling.

Every screen starts with `fillScreen(0)`, which matters here: the back buffer holds the frame from two flips ago, not the current one.

## Flicker, and what it cost WiFi

A HUB75 panel has no framebuffer and no PWM. One row-pair is lit at a time and greyscale comes from showing each bit-plane for a weighted duration.

Protomatter needed a timer interrupt between every bit-plane - about 10,700 deadlines a second at 134 Hz and 5-bit, the tightest around 15 microseconds. Anything that delayed one showed as flicker, and a lot did: `spi_flash_read` disables the instruction cache on *both* cores, so serving the web page from LittleFS flickered the panel on every load; TLS handshakes; WiFi power save; even redrawing an unchanged frame. Each was found and worked around, and the fixes are still in place because they are good hygiene: the page is served from RAM, only the visible feed is polled, power save is off by default, and redraws are gated on a `FrameKey`.

The DMA driver removes the problem instead of working around it. Every bit-plane of every row, latch and OE timing included, is laid out as a ring of descriptors in internal RAM that the LCD peripheral walks forever. No interrupt fires and no CPU is involved, and internal RAM needs no cache, so the refresh runs straight through flash writes. `displayPause()` / `displayResume()` no longer protect the refresh; they remain only so `configFromJson` can swap the config and the quotes without a frame being drawn from one new and one old.

### Drive strength

The driver sets every HUB75 pin to the S3's maximum drive strength, level 3 (about 40 mA). On this board that wrecked WiFi. Measured by ping from the LAN, same position, RSSI -37 dBm throughout:

| Configuration | Replies | Median | Max |
| --- | --- | --- | --- |
| Protomatter | 30/30 | 3 ms | 6 ms |
| DMA, 16 MHz, drive 3 (driver default) | 21/30 | 955 ms | 1935 ms |
| DMA, 10 MHz, drive 3 | 30/30 | 16 ms | 157 ms |
| DMA, 16 MHz, drive 1 | 40/40 | 45 ms | 407 ms |
| **DMA, 16 MHz, drive 0** | **40/40** | **2 ms** | **6 ms** |

It is RF, not bus contention. Changing edge speed at a fixed clock changed the result, which bus load would not care about - and the DMA stream is at most 32 MB/s against roughly 960 MB/s of internal RAM bandwidth. The antenna sits a few centimetres from these lines, and this driver clocks continuously, repeating each row up to 32 times a frame, where Protomatter shifted each bit-plane once and then held.

`displayBegin()` therefore overrides every HUB75 pin to `HUB75_DRIVE` (0) after `begin()`. Too low a drive could in principle shift the wrong bit at 16 MHz, showing as speckled pixels; on this board, with the Matrix Portal plugged straight into the panel, drive 0 is clean. A board that reaches the panel over a ribbon cable may need 1, and possibly 10 MHz to get WiFi back.

## Status

Working: provisioning, captive portal, web config, live palette tuning, OTA, flicker-free DMA rendering with hardware brightness, two-section scrolling ticker, tabbed web UI, Binance / Coinbase / CoinGecko / Yahoo adapters with batch fetching, symbol checks on save, up to 32 feeds, feed reordering and export/import.

Ideas, roughly in order of value:

- Buttons for feed cycling, LIS3DH tap-to-advance and orientation
- ESP Web Tools + Improv for browser flashing and provisioning
- A second PlatformIO environment for plain ESP32 boards (needs a pin remap and a 4 MB partition table)
