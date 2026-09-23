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

// Colour depth per channel is the single biggest lever on refresh rate: frame
// time scales with (2^depth - 1), so every bit dropped roughly halves it.
//
// Measured on this panel: 6 bits -> 70 Hz, 5 bits -> ~110 Hz, 4 bits -> 221 Hz.
// 5 is the default: 32 levels per channel is enough for real tonal range, and
// ~110 Hz proved acceptable once flash reads stopped stalling the refresh.
//
// This is only the fallback. The live value is config.bitDepth, because it is
// a real trade between colour fidelity and flicker and belongs to whoever is
// looking at the panel. Result shows up at /api/status -> refreshHz.
static const uint8_t PANEL_BIT_DEPTH_DEFAULT = 5;
static const uint8_t PANEL_BRIGHTNESS_DEFAULT = 5;

// Poll interval default, in seconds. 60 measurably steadies the panel against
// 30 - every fetch is a TLS handshake, and a desk ticker does not need a price
// that is fresher than a minute.
static const uint16_t POLL_SEC_DEFAULT = 60;
static const uint8_t PANEL_BIT_DEPTH_MIN     = 3;
static const uint8_t PANEL_BIT_DEPTH_MAX     = 6;
