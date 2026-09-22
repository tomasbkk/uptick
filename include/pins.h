#pragma once
#include <Arduino.h>

// HUB75 wiring for the Adafruit Matrix Portal S3.
//
// Verified against Adafruit_Protomatter's own example, which selects these
// under ARDUINO_ADAFRUIT_MATRIXPORTAL_ESP32S3. Non-const because Protomatter
// takes uint8_t* rather than pointer-to-const.

static uint8_t HUB75_RGB[]  = {42, 41, 40, 38, 39, 37}; // R1 G1 B1 R2 G2 B2
static uint8_t HUB75_ADDR[] = {45, 36, 48, 35, 21};     // A  B  C  D  (E)

static const uint8_t HUB75_CLK = 2;
static const uint8_t HUB75_LAT = 47;
static const uint8_t HUB75_OE  = 14;

static const uint16_t PANEL_W = 64;
static const uint16_t PANEL_H = 32;

// Protomatter derives height from the address line count: 2 * 2^addrCount.
// Our P4 panel is 1/16 scan, so 4 lines -> 32 rows, and pin E stays unused.
// A 64x64 panel would use 5 here (and need the board's address-E jumper).
static const uint8_t HUB75_ADDR_COUNT = 4;

// Colour depth per channel, and the single biggest lever on refresh rate:
// frame time scales with (2^depth - 1), so every bit dropped roughly halves it.
//
// Measured on this panel: 6 bits gave 69 Hz, which flickers visibly. 4 bits
// costs 15 time units per frame instead of 63, so it should land near 290 Hz.
// 16 levels per channel is plenty for flat fills, a two-colour trace and text.
//
// Check the result at /api/status -> refreshHz. Anything comfortably above
// about 200 Hz reads as steady.
static const uint8_t PANEL_BIT_DEPTH = 4;
