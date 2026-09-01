# AE2RM ESP32 port (firmware)

Porting AE2RM (a ~19,500-line J2ME/MIDP game) to an ESP32 is a full rewrite,
not an automatic conversion — there's no JVM here. This is **milestone 1**
of that rewrite, scoped deliberately small: prove the asset pipeline and
rendering path work on real hardware before porting any game rules.

## Target hardware

ESP32-S3 2.8" board, ES3C28P ("Xiaozhi" variant):
- ILI9341, 240x320, 4-wire SPI — **display + touch pins user-confirmed
  working on real hardware**
- FT6336 capacitive touch, I2C
- ES8311 audio codec + I2S (not used by the firmware yet)
- MicroSD via SD/MMC 4-bit (not SPI — its own dedicated pins)
- 16MB flash, 512KB SRAM + PSRAM

See `include/board_pins.h` for the full pinout. Display/touch are
verified; SD, audio, and misc pins are transcribed from vendor docs and
not yet confirmed on-device.

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
- WiFi + OTA updates (`ArduinoOTA`): flash once over USB, then push
  subsequent builds wirelessly — see "Build & flash" below

## What's not implemented yet

Everything that makes it a game: units, movement, combat, buildings,
turns, the HUD, menus, MIDI music (needs its own synth — see the “Music”
question this was scoped against), and every map past `m0`. The original
`MainDisplayable.java` is ~11,000 lines covering all of that; this
milestone only reads its map-loading format and terrain layer.

## Build & flash

```bash
pip install Pillow          # for the asset converter
cd src && make               # extracts AEIIRM_src.zip -> src/java
cd ../firmware
python3 tools/convert_assets.py
# copy the contents of firmware/assets/sdcard/ onto a FAT32 microSD card
```

### First flash: over USB

WiFi/OTA needs credentials compiled in, so set those up before or after
your first flash -- they only matter once you want to update wirelessly:

```bash
cp include/secrets.h.example include/secrets.h
$EDITOR include/secrets.h    # set WIFI_SSID / WIFI_PASSWORD; OTA_PASSWORD optional
```

Then, with PlatformIO installed (`pip install platformio`):

```bash
pio run -t upload
pio device monitor
```

The serial log prints the board's IP and OTA hostname once WiFi connects:
```
wifi connected: 192.168.1.42   OTA host: ae2rm.local
```
If `secrets.h` is missing or `WIFI_SSID` is empty, the firmware just skips
WiFi/OTA and runs the game standalone off the SD card -- WiFi is optional.

### Later updates: over the air

```bash
pio run -e esp32-s3-devkitc-1-ota -t upload
```

This uses the `esp32-s3-devkitc-1-ota` environment in `platformio.ini`
(`upload_protocol = espota`, targeting `ae2rm.local`). If mDNS doesn't
resolve on your network, pass the IP instead:

```bash
pio run -e esp32-s3-devkitc-1-ota -t upload --upload-port 192.168.1.42
```

If you set `OTA_PASSWORD` in `secrets.h`, add a matching
`upload_flags = --auth=yourpassword` line under `[env:esp32-s3-devkitc-1-ota]`
in your local `platformio.ini` (don't commit the password).

The board shows an "OTA update..." screen with a progress percentage while
flashing, then reboots into the new firmware. The partition table
(`default_16MB.csv`) has two app slots (`app0`/`app1`), so an OTA write
goes to the inactive slot and won't brick a working board; USB flashing
is still there as a fallback if WiFi is ever unreachable.

## Verification status

This was built and the asset converter was run and its output checked
(header parses to the expected 12x12 map, tile files are the expected
size). **It has not been flashed to or run on physical hardware** — this
session has no access to your board. Display/touch pins are confirmed;
SD/MMC pins and WiFi/OTA are not — flash over USB first and report back
what you see. The SD/MMC init, tile rendering, touch-pan loop, and OTA
path are the things most likely to need a follow-up fix once you can see
real output.

## Suggested next milestones

1. Get milestone 1 actually rendering on your hardware, fix pins/driver
   quirks that only show up on real silicon
2. Port `Unit`/`Sprite` rendering (unit icons over the map, no logic yet)
3. Port turn/movement rules from `MainDisplayable.java` for a single map
4. Menus, HUD, remaining maps, combat, music
