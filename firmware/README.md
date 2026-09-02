# AE2RM ESP32 port (firmware)

Porting AE2RM (a ~19,500-line J2ME/MIDP game) to an ESP32 is a full rewrite,
not an automatic conversion — there's no JVM here. This is **milestone 3**:
terrain (milestone 1) and units (milestone 2), plus local hotseat
movement — tap a unit, tap where to move it, tap END TURN to pass to the
other side. Still no combat, no AI, no menus.

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
  big-endian width/height, a column-major grid of tile-index bytes, a
  building-color table it skips over, then a count-prefixed list of unit
  placement records), loads tile bitmaps on demand, and renders a
  scrollable viewport that you pan by dragging on the touchscreen
- Starting units drawn as static 24x24 map icons (`unit_icons.png`, one
  of 12 types x 4 team colors) at their map-file position, with
  transparent-pixel blitting so they don't cover terrain with a square
  background. **Team-color assignment is exact for story maps** (`m0`-`m7`,
  the only ones converted so far): `MainDisplayable.java`'s `loadMap()`
  always hardcodes the same 2-side turn queue for these (raw color 0 =
  blue, 1 = red), so using each unit record's raw color slot directly is
  correct here — it would NOT be for skirmish maps (`s0`-`s11`, not
  converted/loaded), which build a real building-derived queue for up to
  4 sides; that logic isn't ported.
- Local hotseat movement (`firmware/src/main.cpp`): tap a unit belonging
  to the side whose turn it is to select it; its movement range lights up
  cyan, terrain-cost-limited by `UNIT_MOVE_RANGE` per unit type and
  `TERRAIN_MOVE_COST` per tile type (both taken directly from the
  original's `.unit` files and `tiles0.prop` — see the tables' comments
  in `main.cpp`). Tap a highlighted tile to move there; tap END TURN to
  pass to the other side and let its units move. **No combat, and no
  AI** — the two sides are both driven by whoever is holding the device;
  porting the original's AI (a large scoring heuristic spanning much of
  `MainDisplayable.java`) is out of scope here.
- Asset frame caches (tiles + unit icons) live in PSRAM via `ps_malloc()`,
  not internal SRAM -- two caches were already ~110KB as plain static
  arrays (34% of the ~320KB internal RAM budget), and more asset types
  are coming
- WiFi + OTA updates (`ArduinoOTA`): flash once over USB, then push
  subsequent builds wirelessly — see "Build & flash" below

## What's not implemented yet

Combat, buildings/capture, an AI opponent, the HUD beyond the turn
indicator and END TURN button, menus, MIDI music (needs its own synth —
see the "Music" question this was scoped against), and every map past
`m0`. The original `MainDisplayable.java` is ~11,000 lines covering all
of that; this milestone only reads its map-loading format, terrain
layer, unit starting positions, and enough movement rules for two people
to pass a device back and forth.

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
lands on the inactive slot instead of overwriting the one that's running.
That's not the same as automatic rollback, though: this firmware doesn't
configure ESP-IDF bootloader rollback or app-side image validation, so a
bad image that still boots (just broken) will stay selected. **USB
flashing is the recovery path** if an OTA update leaves the board in a
bad state.

## Verification status

This was built and the asset converter was run and its output checked
(header parses to the expected 12x12 map, m0's 6 unit records parse to
plausible type/color/tile values, tile and unit-icon files are the
expected size). The terrain-cost and unit-move-range tables in
`main.cpp` were cross-checked field-by-field against `tiles0.prop` and
each `*.unit` file. **It has not been flashed to or run on physical
hardware** — this session has no access to your board. Display/touch
pins are confirmed; SD/MMC pins and WiFi/OTA are not — flash over USB
first and report back what you see. The SD/MMC init, tile/unit
rendering, tap-vs-drag detection (the `TAP_MOVE_THRESHOLD` in
`main.cpp` is a guess), and OTA path are the things most likely to need
a follow-up fix once you can see real output.

## Suggested next milestones

1. Get milestone 3 actually rendering and responding to touch on your
   hardware (terrain + units + movement), fix pins/driver/touch-threshold
   quirks that only show up on real silicon
2. Combat and buildings/capture for a single map
3. Menus, HUD, remaining maps, an AI opponent, music
