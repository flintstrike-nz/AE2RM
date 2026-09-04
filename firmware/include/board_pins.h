#pragma once

// ES3C28P (ESP32-S3 2.8" "Xiaozhi" board) pinout.
//
// Display + touch below are user-confirmed working on real hardware.
// SD, audio, and misc pins are transcribed from the vendor doc / lcdwiki
// and NOT yet confirmed on-device -- treat them the same way the whole
// file used to be treated: verify before relying on them, since a wrong
// assignment could contend with another peripheral on the same line and
// damage the board.

// --- Display (ILI9341, 4-wire SPI, SPI2 host) — verified working ---
#define PIN_LCD_SCLK   12
#define PIN_LCD_MOSI   11 // silkscreen "SDA"
#define PIN_LCD_MISO   13
#define PIN_LCD_CS     10
#define PIN_LCD_DC     46 // "RS" on some diagrams
#define PIN_LCD_RST    -1 // tied to chip EN
#define PIN_LCD_BL     45 // PWM

// --- Touch (FT6336, I2C, addr 0x38) — verified working ---
#define PIN_TOUCH_SDA  16
#define PIN_TOUCH_SCL  15
#define PIN_TOUCH_INT  17
#define PIN_TOUCH_RST  18

// --- Audio codec (ES8311, I2C addr 0x18 — shares the touch I2C bus) + I2S ---
// Used by src/audio.cpp (chiptune synth). Still not confirmed against
// real hardware -- see that file's header and the README.
#define PIN_AUDIO_PA_EN 1  // FM8002E amp SHUTDOWN input
// GPIO level that ENABLES the amp. The ES3C28P wires GPIO1 to the
// FM8002E's active-low SHUTDOWN pin, so LOW enables and HIGH shuts down
// (per the board reference cited in the audio PR review). NOT verified
// on hardware here -- if there's no sound, try flipping this to 1.
#define AUDIO_PA_EN_ON  0
#define AUDIO_PA_EN_OFF (!AUDIO_PA_EN_ON)
#define PIN_I2S_MCLK    4
#define PIN_I2S_BCLK    5
#define PIN_I2S_WS      7  // LRCK
#define PIN_I2S_DOUT    8  // to speaker
#define PIN_I2S_DIN     6  // from mic

// --- MicroSD card — SD/MMC 4-bit (NOT SPI), used for game assets ---
#define PIN_SD_CLK     38
#define PIN_SD_CMD     40
#define PIN_SD_D0      39
#define PIN_SD_D1      41
#define PIN_SD_D2      48
#define PIN_SD_D3      47

// --- Misc ---
#define PIN_RGB_LED     42 // WS2812, single
#define PIN_BOOT_BTN     0
#define PIN_BATTERY_ADC  9
#define PIN_UART_TX     43
#define PIN_UART_RX     44
// Free expansion pins: 2, 3, 14, 21
// Reserved, do not use: 19/20 (native USB), 26-37 (octal PSRAM)

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 320
