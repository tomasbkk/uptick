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

Pin assignments live in `include/pins.h`, taken from the block Adafruit_Protomatter selects under `ARDUINO_ADAFRUIT_MATRIXPORTAL_ESP32S3`.

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

To start over, use Forget WiFi on the config page, or erase NVS with `pio run -t erase`.

## Configuration

Settings are a single JSON document at `/config.json` on LittleFS, with a `schema` field so future firmware can migrate rather than silently misread an old shape. WiFi credentials and the CoinGecko API key live separately in NVS and are not part of the config document, so exporting config cannot leak them.

```json
{
  "schema": 1,
  "brightness": 5,
  "bitDepth": 5,
  "rotate180": false,
  "wifiSleep": false,
  "mode": "single_chart",
  "dwellSec": 15,
  "refreshSec": 60,
  "tz": "Europe/Bratislava",
  "feeds": [
    { "source": "binance", "symbol": "BTCUSDT", "label": "BTC", "interval": "1h", "decimals": 0 }
  ]
}
```

Factory feeds are BTC, ETH, SOL and ZEC on Binance. `decimals: 0` means "pick from magnitude" - whole dollars above $1000, cents below, four places under $1.

The `source` field is the extension point: `binance`, `coinbase` and `coingecko` for crypto and `yahoo` for stocks and everything else Yahoo Finance lists, each behind its own fetch adapter. The renderer never learns where the numbers came from.

### Ordering feeds

The rotation follows the order of the `feeds` array, so reordering is an array move and nothing else. The web UI offers both a drag grip and up/down arrows; like every other feed edit, the new order is held until Save.

The device does not apply it mid-dwell. When the config arrives, the renderer looks up the feed that is currently on screen and follows it to its new index, so the panel keeps showing the same thing until its turn is up and the new order starts from the next rotation. A feed that was deleted outright falls through to the index clamp and the next swap picks up the new order regardless.

Quotes are indexed by feed position, so `configFromJson` carries each quote across to wherever its feed moved, matched on source and symbol; anything unmatched starts empty and refetches on its turn. Without that a reorder would print the previous occupant's price under the new feed's label until the next fetch. The same applies to adding and deleting, which had the bug before reordering existed. The poller's attempt timers are indexed by position too, and are simply reset - the worst case is one early refetch.

### Backing feeds up

**`pio run -t uploadfs` erases the saved config.** It rewrites the whole `spiffs` partition from `data/`, and `/config.json` lives in that partition, so every filesystem upload takes the feed list with it. Firmware uploads do not: `pio run -t upload` writes only `app0` at `0x10000` and leaves the filesystem alone. Only run `uploadfs` when something under `data/` actually changed.

**Export** and **Import** in the Feeds card exist for the times you do. Export downloads `uptick-feeds.json`; import reads it back, validates it and loads it into the form, and the feeds are stored when you press Save. The order that matters is export, then `uploadfs`, then import.

Both run entirely in the browser - the page already holds the config, so no endpoint is involved, and exporting captures what is on screen including edits that have not been saved yet. Import accepts three shapes, because all three turn up: an export file, a whole `/api/config` document, or a bare array of feeds. Anything unknown is normalised rather than trusted - an unrecognised `source` falls back to `binance`, an unrecognised `interval` to `1h`, `decimals` is clamped to 0-8 - and a file that fails validation leaves the feeds on screen untouched.

`interval` is the chart candle width, not the poll rate. 64 candles fill the panel's 64 columns, so it sets how far back the chart reaches. Yahoo has no 4h bar, so `4h` rounds to daily there - the same compromise Coinbase forces by rounding it up to 6h.

### REST API

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/status` | mode, IP, RSSI, heap, uptime, refresh rate and stability, core assignments |
| GET / POST | `/api/config` | the config document; a `bitDepth` change restarts the device |
| POST | `/api/brightness` | live brightness, not persisted |
| POST | `/api/mode` | live display mode, not persisted |
| GET / POST | `/api/palette` | live colour overrides by name, not persisted |
| POST | `/api/defaults` | restore factory settings, leaving WiFi alone |
| POST | `/api/apikey` | store the CoinGecko key (write-only) |
| POST | `/api/fetch` | pause or resume market polling |
| GET | `/api/check?symbol=` | validate a Yahoo ticker on the page's behalf (Yahoo sends no CORS header) |
| POST | `/api/wifisleep` | toggle WiFi power save |
| POST | `/api/stats` | reset the refresh stability counters |
| GET | `/api/files` | list LittleFS contents |
| GET | `/api/scan` | visible networks |
| POST | `/api/wifi` / `/api/forget` | store or erase credentials, then restart |
| POST | `/api/update` | OTA firmware upload |

Plain HTTP on the LAN. Do not port-forward it.

## Architecture

Core 0 runs the radio, the web server and the market fetch. Core 1 runs the display. Confirmed rather than assumed - `/api/status` reports `coreDisplay`, `corePanelISR` and `coreNet`.

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
- **It sends no CORS header**, unlike all three crypto APIs. The web page cannot check a ticker from the browser, so those checks go through `/api/check` on the device instead. One TLS handshake per click, and only when the button is pressed.

Not every listing is priced in dollars, so the quote currency is read from the same response and carried on the `Quote`. The panel prints `$` only when the currency really is USD - a Frankfurt or London line prints bare, since the 5x7 font has no glyph for those and a wrong symbol is worse than none. The crypto sources are all USD pairs and are unaffected.

Alternatives were measured rather than assumed. Stooq now gates its CSV behind a JavaScript proof-of-work challenge, which an ESP32 cannot solve. Alpha Vantage, Finnhub, Twelve Data, marketstack, FMP, Tiingo and Polygon all require a key. IEX Cloud is gone. MarketData.app is keyless only for AAPL.

### Parsing

Candle responses are parsed by a hand-rolled streaming scanner (`extractField`) rather than ArduinoJson. ArduinoJson filters cannot select by index inside an array, so filtering could not isolate the close, and buffering the whole document would mean holding hundreds of fields of JSON in RAM at the same moment a TLS session is open. The scanner pulls one field straight off the socket with no allocation, handles both quoted strings and bare numbers, and keeps the *last* 64 values rather than the first - CoinGecko returns far more points than asked for and the recent end is the interesting one.

The 24h endpoints are objects rather than arrays, so ArduinoJson key filtering works there and is used.

Yahoo needs a second scanner (`extractYahoo`). Its series sits three arrays deep and its quote fields come back in no fixed order - `volume`, `high`, `close`, `open` and `low` have each been observed in different positions - so index-based extraction does not work on it. That scanner walks the stream once matching two literal keys, `"regularMarketChangePercent":` from `meta` and `"close":[` from `indicators.quote[0]`, and gets both the day's move and the chart out of a single pass. Bars that come back `null` are dropped rather than interpolated, and an unterminated trailing number on a truncated response is discarded rather than committed, since `13` may be the first two digits of `137.5`.

### Polling schedule

Two rules, both driven by what is on screen:

**Only the visible feed is polled.** A feed nobody is looking at does not need fresh data, and fetching it disturbs the panel showing something else. This puts every fetch at a screen transition - the moment a glitch is least noticeable, because the content is changing anyway - and leaves the rest of each dwell completely quiet. The cost is that effective refresh becomes the rotation period rather than the poll interval.

**In layouts that show every feed**, fetches are spaced `interval / feeds` apart instead. Without that they clump: `marketPoll` runs off the network loop every couple of milliseconds, so the moment one fetch finishes the next due feed starts. Four feeds at 60s produced a six second burst of four TLS handshakes followed by 54 seconds of quiet.

## Display

### Layout

64 x 32 is tight. Row budget for `single_chart`:

```
rows  0-6    BTC            +4.37%     5x7 font, 6px advance
row   7      blank
rows  8-14   $214,460                  price
row   15     blank
rows 16-31   chart, 64 cols
```

`$214,460.00` is 66 px in the 5x7 font - one pixel too wide. The price line steps down a ladder: full precision in the 5x7 font, then TomThumb 3x5 at 4 px advance, then a `$214.4K` abbreviation. Thousands separators are inserted into the integer part only.

Text wrapping is turned off. Adafruit_GFX wraps using the glyph advance rather than its ink, so a right-aligned string ending flush at column 63 still trips the check, and that last character gets redrawn at x=0 on the following line in its own colour - which showed up as the percentage's green painted through the `$` of the price.

The chart scales to the min/max of the visible window, never to zero, or intraday moves would render as a flat line. The trace runs along the fill's top edge as a staircase, never as a line between column centres: `drawLine` spreads a steep move across both columns, so half of it lands above the previous column's fill as stray pixels floating over the chart.

### The percentage is always 24h

The chart span is per-feed and adjustable; the percentage is always the fixed 24 hour change, so it stays comparable between feeds whose charts cover different spans. The two disagree routinely - a coin can be flat over a day and up 8% over a 64 hour chart - so the percentage is tinted by the percentage and the chart by the chart. Colouring a visibly falling line green to match a positive 24h figure would be a lie.

At `1h` candles the 24h figure comes free from the candles already fetched, 24 bars back. Only `5m` and `15m` feeds trigger a second request, since 64 of those candles do not span a day.

For a Yahoo feed it is the move against the previous close, not a rolling 24 hours. A rolling window is meaningless for something that trades 6.5 hours a day, and the previous close is the number every quote page prints. Yahoo supplies it in the same response, so no second request is ever needed. Outside market hours the price holds at the last close and the figure stops moving - that is correct, not a stalled fetch, and the stale indicator stays off because it tracks fetch failures rather than data age.

### Palette

Eleven named colours, all overridable live through `/api/palette` and the web UI's colour pickers: `LABEL`, `PRICE`, `PCT_UP`, `PCT_DOWN`, `FILL_UP`, `TRACE_UP`, `FILL_DOWN`, `TRACE_DOWN`, `DIM`, `ACCENT`, `STALE`.

Overrides are RAM-only. The web page remembers picks in local storage and can export them as a `PAL_FACTORY` table to paste back into `display.cpp`.

### Brightness, and why the palette is awkward

Protomatter has no brightness API, so brightness is applied by scaling every colour component before drawing. That has consequences worth understanding before tuning colours:

- The 1-100 slider maps onto an 8-100 output range with a square curve. The floor exists because a linear scale reached pure black near the bottom, at which point the panel is too dark to see and the only way to recover is a reset.
- Scaling eats quantisation levels. Measured at 4-bit depth and 40% brightness: a fill with green 220 renders, green 95 and green 74 are both black. There is about one usable green level in that combination.
- Secondary channels drop in and out at different points, which is why a warm red looks red at low brightness and orange at high - the green component only survives once scaling stops crushing it.

The chart fill is therefore the bright colour and the trace the dim one, not the other way round: a colour dim enough to sit behind a bright trace crushes to zero and the whole fill disappears.

A driver with hardware brightness (output-enable duty rather than colour scaling) would remove all of this. See the notes at the end.

### Refresh rate and colour depth

`bitDepth` is a setting, 3 to 6, because frame time scales with `2^depth - 1` and it is a real trade between colour fidelity and flicker. Measured on this panel:

| Bit depth | Levels per channel | Refresh |
| --- | --- | --- |
| 6 | 64 | 70 Hz |
| 5 | 32 | 134 Hz |
| 4 | 16 | 221 Hz |

Protomatter takes colour depth in its constructor, so the matrix object is heap-allocated and built in `displayBegin()` from the stored value. Changing the setting restarts the device rather than tearing down a live DMA chain.

`/api/status` reports `refreshHz`, plus `refreshLow`, `refreshPeak` and `refreshDips` sampled at 200 ms. Use it rather than guessing: a steady figure during a visible flicker means the problem is electrical rather than a starved refresh. Sampling normalises by actual elapsed time - assuming a fixed window made a delayed sample look like a *higher* rate, which is exactly backwards and once reported 214 Hz on a panel running at 136.

## What causes flicker, and what fixed it

A HUB75 panel has no framebuffer and no PWM. Only one row-pair is lit at a time, selected by the address lines, and greyscale is made by showing each bit-plane for a weighted duration. At 134 Hz, 1/16 scan and 5-bit that is roughly 10,700 timed intervals per second with a tightest deadline around 15 microseconds. Miss one and a row stays lit too long.

Anything that delays that timing shows as flicker. Ranked by measured effect:

**Flash reads stall both cores.** `spi_flash_read` disables the instruction cache on *both* cores, so core pinning cannot help. Serving the web UI from LittleFS flickered the panel on every page load. Fixed by reading the page into internal RAM once at boot.

**Market fetches.** Each is a TLS handshake. Fetching four feeds at once produced a visible burst. Fixed by polling only the visible feed, which also moves every fetch onto a screen transition.

**WiFi power save.** The radio wakes on every DTIM beacon. Now off by default and a persisted setting; costs about 40 mA, irrelevant on mains power.

**Redrawing needlessly.** Nothing on the panel animates, so frames are gated on a `FrameKey` of everything that can change the image. 30 `show()` calls a second became roughly two a minute.

Config saves bracket the flash write with `displayPause()` / `displayResume()`, which hold a mutex so `show()` never runs mid-write. Every exit path must resume - pause takes the mutex, so leaking it freezes the render task permanently. These deliberately do **not** call `matrix->stop()` / `matrix->resume()`: on the S3 that pair kills the refresh and never restores it when called from a task other than the one driving the panel.

## Status

Working: provisioning, captive portal, web config, live palette tuning, OTA, Protomatter rendering at configurable depth, Binance / Coinbase / CoinGecko adapters, display-gated polling.

Flicker is confined to the moment the screen changes feed, where the content is changing anyway.

Ideas, roughly in order of value:

- **Swap Protomatter for a fully-DMA driver.** `ESP32-HUB75-MatrixPanel-DMA` is Adafruit_GFX compatible, so every draw function here is unchanged and only the object setup differs - maybe 60 lines. It refreshes from a circular DMA chain with no per-bitplane interrupt, and it has `setBrightness8()`, which would remove every palette problem described above in one go.
- Buttons for feed cycling, LIS3DH tap-to-advance and orientation
- ESP Web Tools + Improv for browser flashing and provisioning
- A second PlatformIO environment for plain ESP32 boards (needs a pin remap and a 4 MB partition table)
