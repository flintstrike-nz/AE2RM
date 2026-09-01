# AE2RM ESP32 port (firmware)

Porting AE2RM (a ~19,500-line J2ME/MIDP game) to an ESP32 is a full rewrite,
not an automatic conversion — there's no JVM here. This is **milestone 1**
of that rewrite, scoped deliberately small: prove the asset pipeline and
rendering path work on real hardware before porting any game rules.

## Target hardware

ESP32-S3 2.8" ILI9341 board (diymore-style ES3C28P/ES3N28P):
- ILI9341V, 240x320, 4-wire SPI
- FT6336G capacitive touch, I2C (ES3C28P variant only)
- MicroSD card slot (used here for game assets)
- 16MB flash, 512KB SRAM + PSRAM

**GPIO pins are placeholders — see the big warning at the top of
`include/board_pins.h`.** The spec sheet for this board documents the
chips and interfaces but not exact pin numbers. Confirm them against your
actual unit (seller wiki/example code, or continuity testing) before
trusting them. Getting a pin wrong just means "nothing draws" — it won't
damage the board.

## What's implemented

- PlatformIO project targeting ESP32-S3, LovyanGFX display+touch driver
  configured for this exact display/touch combo (`include/LGFX_Config.h`)
- Asset converter (`tools/convert_assets.py`) that turns the original
  game's `tiles0_NN.png` tileset into raw RGB565 files and copies the
  `m0.aem` map file, both onto a layout you copy to the microSD card
- Firmware that mounts the SD card, parses the original `.aem` map format
  (same binary layout `aeii/MainDisplayable.java`'s `loadMap()` reads:
  big-endian width/height, then a column-major grid of tile-index bytes),
  loads tile bitmaps on demand, and renders a scrollable viewport that you
  pan by dragging on the touchscreen

## What's not implemented yet

Everything that makes it a game: units, movement, combat, buildings,
turns, the HUD, menus, MIDI music (needs its own synth — see the “Music”
question this was scoped against), and every map past `m0`. The original
`MainDisplayable.java` is ~11,000 lines covering all of that; this
milestone only reads its map-loading format and terrain layer.

## Build

```bash
pip install Pillow          # for the asset converter
cd src && make               # extracts AEIIRM_src.zip -> src/java
cd ../firmware
python3 tools/convert_assets.py
# copy the contents of firmware/assets/sdcard/ onto a FAT32 microSD card
pio run -t upload            # requires PlatformIO (pip install platformio)
pio device monitor
```

## Verification status

This was built and the asset converter was run and its output checked
(header parses to the expected 12x12 map, tile files are the expected
size). **It has not been flashed to or run on physical hardware** — this
session has no access to your board. Confirm the pins in `board_pins.h`
first, then flash and report back what you see; the touch-pan loop and
tile rendering are the two things most likely to need a follow-up fix
once you can see real output.

## Suggested next milestones

1. Get milestone 1 actually rendering on your hardware, fix pins/driver
   quirks that only show up on real silicon
2. Port `Unit`/`Sprite` rendering (unit icons over the map, no logic yet)
3. Port turn/movement rules from `MainDisplayable.java` for a single map
4. Menus, HUD, remaining maps, combat, music
