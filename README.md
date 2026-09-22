# uptick

A small LED ticker: crypto and stock prices with a sparkline, on a 64x32 HUB75 panel driven by an ESP32-S3.

## Hardware

- Adafruit Matrix Portal S3 (ESP32-S3, 8 MB flash, 2 MB PSRAM, HUB75 socket)
- P4 indoor 64x32 RGB panel, 1/16 scan, 256 x 128 mm
- 5 V supply into the Matrix Portal screw terminals

### Power

**The screw terminals are outputs, not inputs.** Adafruit's pinouts page: *"they were designed as outputs ONLY - power from the USB port connects directly to these pads, so you should power from USB and then connect the matrix power inputs to these terminals."* Feeding those terminals from an external supply while USB is also plugged in can damage the board.

The HUB75 ribbon carries no power either - only signals and ground. The panel is always powered through its own spade pigtail.

That leaves two valid arrangements:

- **USB-C powers everything.** Supply into the board, panel pigtail onto the screw terminals. One cable, and what the panel can draw is capped by the USB source: 500-900 mA from a laptop port, 1.5-3 A from a decent charger.
- **Panel on its own supply.** 5 V wired directly to the panel's power input, USB kept on the board for flashing and serial. Grounds tie together through the ribbon. This is the arrangement for full brightness or a second panel.

Never wire an external supply to the screw terminals.

The panel is 2048 LEDs. Full white is roughly 3-4 A; a ticker screen is mostly dark and at 40% brightness sits in the hundreds of mA, which is why the USB-only arrangement works. The failure mode when it does not is a brown-out - the board reboot-loops or the serial port drops when the screen fills - not gradual dimming.

Two mitigations, both cheap and both already in the firmware: the brightness scale, and the 200 cap that keeps anything from being drawn as full white. 1000 uF across the 5 V rail near the panel connector helps with power-on inrush.

### Panel wiring

1/16 scan uses address lines A-D only, so the board's address-E jumper stays alone. The 2x10 socket accepts the 2x8 IDC cable that ships with the panel - no off-by-one to get wrong.

Pin assignments live in `include/pins.h`, taken from the block Adafruit_Protomatter selects under `ARDUINO_ADAFRUIT_MATRIXPORTAL_ESP32S3`.

## Build

PlatformIO:

```bash
pio run -t upload
```

```bash
pio run -t uploadfs
```

The second command is required - the web UI lives in `data/www/` on LittleFS, and the device serves a diagnostic page instead without it.

`GET /api/files` reports what is actually on the filesystem. `totalBytes: 0` there means LittleFS found no partition: the Arduino wrapper looks the partition up by *label* and defaults to `spiffs`, so the entry in `partitions_8mb_ota.csv` has to be named `spiffs` even though it holds a LittleFS image. Renaming it to something descriptive silently breaks mounting, because PlatformIO still finds it by subtype and uploads the image to a partition the firmware never looks at.

Serial monitor:

```bash
pio device monitor
```

## First run

1. Flash firmware and filesystem, then power up.
2. With no stored credentials the device starts a setup AP called `Uptick-XXXX` and runs a captive portal. Join it; the config page should pop automatically, or browse to `192.168.4.1`.
3. Pick your network, enter the password, save. The device restarts and joins.
4. Reach it at `http://uptick.local/` or at the IP printed on serial. Android's mDNS is unreliable, so keep the IP handy.

To start over, use Forget WiFi on the config page, or erase NVS with `pio run -t erase`.

## Configuration

Settings are a single JSON document at `/config.json` on LittleFS, with a `schema` field so future firmware can migrate rather than silently misread an old shape. WiFi credentials live separately in NVS and are not part of the config document, so exporting config never leaks them.

```json
{
  "schema": 1,
  "brightness": 40,
  "rotate180": false,
  "mode": "single_chart",
  "changeWindow": "chart",
  "dwellSec": 15,
  "refreshSec": 30,
  "tz": "Europe/Bratislava",
  "feeds": [
    { "source": "binance", "symbol": "BTCUSDT", "label": "BTC", "interval": "1h", "decimals": 0 }
  ]
}
```

The `source` field is the extension point: `binance` today, another exchange or a stock API later, each behind its own fetch adapter. The renderer never learns where the numbers came from.

### REST API

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/status` | mode, IP, RSSI, heap, uptime, version |
| GET | `/api/config` | current config document |
| POST | `/api/config` | validate, apply and persist a config document |
| POST | `/api/brightness` | live brightness, not persisted |
| GET | `/api/scan` | visible networks |
| POST | `/api/wifi` | store credentials and restart |
| POST | `/api/forget` | erase credentials and restart into setup |
| POST | `/api/update` | OTA firmware upload |

Plain HTTP on the LAN. Do not port-forward it.

## Architecture

Core 0 runs the radio, the web server and the market data fetch. Core 1 runs the display. The two share a `Config` struct behind a mutex, so settings apply live without a reboot - only credential changes restart the device.

The split matters because the S3 drives HUB75 through the LCD_CAM peripheral with chained DMA descriptors: once a frame is pushed the panel keeps refreshing with no CPU involvement. That is what makes the plain blocking `WebServer` the right choice here instead of an async one - a handler can block for a whole TLS handshake and not cost a single frame.

## Data source

Binance klines, which takes an exact count so the chart needs no resampling:

```
https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1h&limit=64
```

64 candles for 64 columns, and the percentage comes out of the same response - one request, one TLS handshake, no second endpoint.

Coinbase is supported too (`api.exchange.coinbase.com/products/BTC-USD/candles?granularity=3600`), which matters if the device ever sits behind a US IP, since Binance blocks those. Coinbase returns newest-first and Binance oldest-first; the adapter normalises to oldest-first so the renderer never has to know.

Responses are parsed by a hand-rolled streaming scanner (`extractField`) rather than ArduinoJson. ArduinoJson filters cannot select by index inside an array, so filtering could not isolate the close, and buffering the whole document would mean holding 64 x 12 fields of JSON in RAM at the same moment a TLS session is open. The scanner pulls field 4 straight off the socket with no allocation and handles both quoted strings and bare numbers.

## Display layout

64 x 32 is tight. Row budget for `single_chart`:

```
rows  0-6    BTC            +4.37%     5x7 font, 6px advance
rows  7-14   $214460                   price
row   15     separator
rows 16-31   sparkline, 64 cols
```

`$214,460.00` is 66 px in the 5x7 font - one pixel too wide for the panel. The price line therefore steps down a ladder: full precision in the 5x7 font, then TomThumb 3x5 at 4 px advance if that overflows, then a `$214.4K` abbreviation. That ordering is why a six figure price still renders rather than getting clipped.

The chart scales to the min/max of the visible window, never to zero, or intraday moves would render as a flat line along the bottom.

The area under the trace is a 50% checkerboard of the line colour, not a dimmer solid. A dim solid does not survive the panel's colour depth: at 4 bits per channel there are 16 levels, and the original fill colour quantised to zero and vanished. Dithering is independent of bit depth and brightness, always exactly half the line's luminance, and reads as solid at this pixel pitch.

### Refresh rate and colour depth

`PANEL_BIT_DEPTH` in `include/pins.h` is the single biggest lever on refresh rate, because frame time scales with `2^depth - 1`. Measured on this panel:

| Bit depth | Levels per channel | Refresh |
| --- | --- | --- |
| 6 | 64 | 69 Hz - visibly flickers |
| 4 | 16 | 216 Hz - occasional flicker |
| 3 | 8 | ~430 Hz expected |

It does not scale perfectly, because there is fixed per-frame overhead.

`GET /api/status` reports `refreshHz`, sampled once a second from Protomatter's frame counter. Use it rather than guessing: a steady figure during a flicker means the problem is electrical (ribbon seating, cable length) rather than a starved refresh.

Low depth is cheap here because nothing on screen needs a gradient - flat fills, a two-colour trace and white text. The dithered fill is what makes it cheap; a colour-based fill puts a floor on how far the depth can drop.

### Which percentage is shown

`changeWindow` picks it, globally:

- `chart` (default) - change across the charted window, so the number and the line always agree
- `24h` - the conventional fixed 24 hour figure, which at a short interval will not match the graph

The 24 h figure is derived from the candles already fetched whenever the charted window is longer than a day. At the default `1h` x 64 bars the reference close is 24 bars back, so the common case costs no extra request; only `5m` and `15m` fall through to a second call (`/api/v3/ticker/24hr`, or Coinbase `/stats`).

When the two disagree - up over a day, down over the last hour - the percentage text is tinted by the percentage and the chart is tinted by the chart. Colouring a visibly falling line green to match a positive 24 h number would be worse than letting them differ.

Flipping the setting takes effect on the next refresh; until then the panel falls back to the chart-window figure rather than blanking.

### Brightness

Protomatter has no brightness API. Brightness is applied by scaling every colour component before drawing, in `col()`. The same helper caps any component at 200, so nothing on screen is ever full white - indistinguishable at this pixel pitch and it keeps worst-case current clear of what a 3 A supply will give up.

The 1-100 slider does not map linearly onto that scale. It maps onto 8-100 with a square curve: the curve makes the slider feel proportional, and the floor exists because a linear scale reached pure black near the bottom, at which point the panel is too dark to see and the only way to recover is a reset.

Brightness from `/api/brightness` is a live preview and is not persisted. The config POST is what saves it.

### Flash writes and the panel

Writing to LittleFS on the S3 can stall the bus long enough to starve the matrix DMA and glitch a frame. Every config save brackets the write with `displayPause()` / `displayResume()`, which hold a mutex so that `show()` - which rebuilds bitplanes and rewrites DMA descriptors - never runs mid-write. If you add another flash write, bracket it the same way.

Those helpers deliberately do **not** call `matrix.stop()` / `matrix.resume()`. On the S3, calling that pair from a task other than the one driving the panel kills the refresh and never restores it: the display goes black and stays black until a reset. Holding the mutex is sufficient, and anything that does slip through is a single frame that the next redraw repairs.

## Architecture notes

`Config` holds `String` members, so copying it deep-copies around forty strings. Both the render loop and the poll loop run far too often to do that per iteration, so `g_cfgRev` is bumped on every change and each reader caches its own copy until the revision moves.

The two mutexes (`CfgLock`, `QuoteLock`) are never held at the same time. Both readers copy config, release, then take the quote lock, so there is no lock ordering to get wrong.

## Status

Working end to end: provisioning, captive portal, web config, live settings, OTA, Protomatter rendering, and the Binance and Coinbase adapters. Nothing has been compiled or run on hardware yet.

Next:

- bring-up on the real panel: confirm refresh, tune brightness and the chart fill
- buttons for feed cycling, LIS3DH tap-to-advance and orientation
- ESP Web Tools + Improv for browser flashing and provisioning
- a `/api/preview` endpoint rendering the current frame into the web UI
