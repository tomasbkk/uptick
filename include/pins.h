#pragma once
#include <Arduino.h>

// HUB75 wiring for the Adafruit Matrix Portal S3.
//
// These are the board's own HUB75 pins, as Adafruit's examples for the Matrix
// Portal S3 define them - they belong to the board, not to any one library.

static const int8_t HUB75_R1 = 42, HUB75_G1 = 41, HUB75_B1 = 40;
static const int8_t HUB75_R2 = 38, HUB75_G2 = 39, HUB75_B2 = 37;

// Our P4 panel is 1/16 scan, so A-D address 16 row pairs and E is unused.
// The board does route E to GPIO 21 for 64-row panels, behind a jumper.
static const int8_t HUB75_A = 45, HUB75_B = 36, HUB75_C = 48, HUB75_D = 35;
static const int8_t HUB75_E = -1;

static const int8_t HUB75_CLK = 2;
static const int8_t HUB75_LAT = 47;
static const int8_t HUB75_OE  = 14;

static const uint16_t PANEL_W = 64;
static const uint16_t PANEL_H = 32;

// Colour depth comes from the build, not from config. The DMA library chooses
// its gamma table from PIXEL_COLOR_DEPTH_BITS at compile time and uses that
// table's output without rescaling it to the runtime depth, so the two must be
// equal or bright colours lose their top bit. Deriving one from the other makes
// that impossible to get wrong. Set it in platformio.ini.
#ifndef PIXEL_COLOR_DEPTH_BITS
#error "PIXEL_COLOR_DEPTH_BITS must be set in platformio.ini build_flags"
#endif
static const uint8_t PANEL_BIT_DEPTH = PIXEL_COLOR_DEPTH_BITS;

// Shift clock. The S3 divides a 160 MHz source, so the library rounds a request
// to 10, 16 or ~23 MHz. 16 is the one where the requested and real clocks
// match exactly, which also keeps the library's computed refresh rate honest -
// asking for 8 actually runs at 10 and under-reports by a fifth.
//
// At 7-bit that computes to ~223 Hz. If the panel ever shows ghosting or
// smeared columns, drop to 8 MHz (~111 Hz at 7-bit) before anything else.
static const uint32_t HUB75_CLOCK_HZ = 16000000;

// GPIO drive strength for every HUB75 line, 0 (~5 mA) to 3 (~40 mA).
//
// The DMA driver sets all of them to 3, the maximum, and that wrecked WiFi:
// measured on this board at 16 MHz, ping had a 955 ms median with 30% loss at
// drive 3, against 2 ms and no loss at drive 0. The antenna sits a few
// centimetres from these lines, and the sharpest possible edges put the most
// energy into the 2.4 GHz band - and the driver clocks continuously, repeating
// each row up to 32 times a frame, so the lines are never quiet. Softer edges
// keep the harmonics down without touching the clock.
//
// Lower is quieter; too low and the edges get slow enough to shift the wrong
// bit, which shows as speckled or smeared pixels. Drive 0 is clean on this
// board with the panel plugged straight in; a ribbon cable may need 1.
static const uint8_t HUB75_DRIVE = 0;

// 5% of full OE duty. The library floors every bit-plane at one pixel-clock
// of output, so even the bottom of the slider never goes fully dark.
static const uint8_t PANEL_BRIGHTNESS_DEFAULT = 5;

// Poll interval default, in seconds. Every fetch is a TLS handshake, and a
// desk ticker does not need a price fresher than a minute.
static const uint16_t POLL_SEC_DEFAULT = 60;

// Ticker scroll speed, pixels per second. The tape moves one pixel a frame,
// so this is also its frame rate. 30 read as a little fast on the real panel
// with two sections moving; 20 is about three characters a second. Much below
// 15 the motion starts to look stepped, since each step is a whole pixel.
static const uint8_t SCROLL_PPS_DEFAULT = 20;
static const uint8_t SCROLL_PPS_MIN     = 10;
static const uint8_t SCROLL_PPS_MAX     = 90;

// Vertical ticker pause, seconds. 0 crawls continuously; anything more holds
// the list each time an entry reaches the top, the way a departure board does.
static const uint8_t PAUSE_SEC_DEFAULT = 0;
static const uint8_t PAUSE_SEC_MAX     = 10;
