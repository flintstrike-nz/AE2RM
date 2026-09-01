#pragma once

// !!! VERIFY BEFORE FLASHING !!!
//
// The AliExpress listing for this board (diymore-style ESP32-S3 2.8" ILI9341
// + FT6336G touch module) documents the display/touch ICs and the fact that
// they're wired via 4-line SPI (display) and I2C (touch), but does not
// publish an exact GPIO pinout in the spec sheet. These pin numbers are
// placeholders based on the most common wiring for this class of board and
// MUST be confirmed against your actual unit before you rely on them:
//   - check the seller's product page / wiki / included "CD" resources for
//     a pinout diagram or example sketch,
//   - or trace/continuity-test the module against the ESP32-S3 pins,
//   - or open an issue on the seller's github/wiki if they provide one.
// Wrong pins here just mean "nothing draws" / "touch doesn't respond" --
// it will not damage the board -- but don't trust these values as fact.

// --- Display (ILI9341V, 4-wire SPI) ---
#define PIN_LCD_SCLK   12
#define PIN_LCD_MOSI   11
#define PIN_LCD_MISO   13
#define PIN_LCD_DC      2
#define PIN_LCD_CS     10
#define PIN_LCD_RST     3   // set to -1 if RST is tied to EN/3V3 on your unit
#define PIN_LCD_BL     14

// --- Touch (FT6336G, I2C) — ES3C28P (touch) variant only ---
#define PIN_TOUCH_SDA   6
#define PIN_TOUCH_SCL   7
#define PIN_TOUCH_INT   5
#define PIN_TOUCH_RST   4   // set to -1 if not broken out separately

// --- MicroSD card (SPI, used for game assets) ---
#define PIN_SD_CS       9
// SD shares the display's SCLK/MOSI/MISO lines on most of these boards.

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 320
