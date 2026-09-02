# AE2RM ESP32 port (firmware)

Porting AE2RM (a ~19,500-line J2ME/MIDP game) to an ESP32 is a full rewrite,
not an automatic conversion — there's no JVM here. This is **milestone 7**:
terrain, units, movement, combat, capture, and the mission menu
(milestones 1-6), plus a basic AI opponent — you're always blue, the
computer is always red, so this is now playable single-player. No other
menus, no scripted mission events.

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
  game's `tiles0_NN.png` tileset into raw RGB565 files and copies all 8
  story map files (`m0.aem`-`m7.aem`), both onto a layout you copy to the
  microSD card
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
  all converted and playable): `MainDisplayable.java`'s `loadMap()` always
  hardcodes the same 2-side turn queue for these (raw color 0 = blue, 1 =
  red), so using each unit record's raw color slot directly is correct
  here — it would NOT be for skirmish maps (`s0`-`s11`, not
  converted/loaded), which build a real building-derived queue for up to
  4 sides; that logic isn't ported. `m4` and `m6` place no color-1 (red)
  units at all in their starting layout -- their extra encounters are
  driven by the original's scripted mission events (`m*.script` files),
  which aren't ported, so those two missions currently have less to
  fight than intended.
- Local hotseat movement (`firmware/src/main.cpp`): tap a unit belonging
  to the side whose turn it is to select it; its movement range lights up
  cyan, terrain-cost-limited by `UNIT_MOVE_RANGE` per unit type and
  `TERRAIN_MOVE_COST` per tile type (both taken directly from the
  original's `.unit` files and `tiles0.prop` — see the tables' comments
  in `main.cpp`). Tap a highlighted tile to move there; tap END TURN to
  pass to the AI (see below).
- Combat: any enemy unit already within the selected unit's attack range
  (`UNIT_ATTACK_RANGE_MIN`/`MAX` per type — e.g. archers can hit at 1-2
  tiles, the catapult only at 2-4) highlights red and is a valid tap
  target instead of a move. Damage follows the core of the original's
  `Unit.attackUnit()`: a random roll in `[offenceMin, offenceMax)` against
  the defender's `UNIT_DEFENCE` plus `TERRAIN_DEFENCE_BONUS` for the tile
  it's standing on, scaled by the attacker's current health% — but
  **without** the original's per-property matchup bonuses (mounted vs.
  ground, golem vs. skeleton, water/swamp bonuses, etc.). These depend on
  the same `UNIT_PROPERTIES` bit flags milestone 5 reads for capture
  eligibility (below) -- the bits combat would need are just not
  interpreted here; only the capture-related ones are. A surviving
  defender counterattacks if adjacent (`Unit.canPerformCloseAttack()`:
  melee-only, and only if the defender's own `MIN_ATTACK_RANGE` is 1 --
  the catapult, for example, can never counterattack). A unit at 0 health
  is removed. Damaged units show a small health bar. This is "move OR
  attack" per turn, not the original's "move then attack" -- combining
  the two would need tracking attack range from every tile in the move
  range, not just the unit's current one, which is out of scope here.
- Basic AI (`aiActUnit()` in `main.cpp`): color 1 (red) is always the
  computer, color 0 (blue) is always you -- there's no way to flip this,
  and the two-human hotseat mode from earlier milestones no longer
  applies (`endTurn()` always auto-resolves red's turn through the AI).
  Each AI unit, in order: attacks an enemy already in range from where it
  stands; else, if some reachable tile puts an enemy in range, moves
  there and attacks (the AI *is* allowed "move then attack" -- see the
  note on `handleTap()` for why human play doesn't get that); else moves
  toward the nearest living enemy to close the distance for a later turn;
  else (no enemies left) does nothing. No pathfinding beyond
  `computeReachable()`'s flood fill, no retreat or defensive
  positioning, no target prioritization (it attacks the first eligible
  enemy in array order, not the weakest or most valuable), and no
  coordination between units. This is **not** a port of the original's
  AI, which is a large scoring heuristic spanning much of
  `MainDisplayable.java` (`sub_10cb()` and friends) -- it's enough to
  make single-player winnable and losable, nothing more.
- Village/castle capture: moving a unit onto an enemy or neutral
  fraction-building tile flips its ownership if that unit type is
  equipped to capture it, per each `.unit` file's `HasProperty` bits
  (`UNIT_PROPERTIES` in `main.cpp`) -- soldier and king can capture
  villages, but **only the king can capture a castle**, matching the
  source data (no other unit has that property bit set).
- Win condition: a side loses when its king dies in combat. This is a
  simplification of the original, which ties defeat more to castle
  capture/`fractionKings` bookkeeping this milestone doesn't track --
  king death is the clearest single condition to key off without it. The
  screen shows a "BLUE/RED WINS" banner; tapping it returns to the
  mission menu (see below) rather than restarting in place.
- Mission menu: boots to a list of "Mission 1"-"Mission 8" (`m0.aem`
  through `m7.aem`) instead of loading `m0` directly. Labels are generic
  -- the original's mission titles come from a localized string table
  this firmware doesn't read. Tapping a row loads that map and resets all
  per-game state (turn, selection, win/loss, camera position); the tile
  and unit-icon asset caches are content-independent across maps and are
  deliberately not reset. The win banner's tap-to-continue returns here --
  and so does a dedicated **MENU** button, always available during a
  mission regardless of game state. That button isn't optional polish:
  `m4`/`m6` place no red units and their scripted spawns aren't ported,
  so those two missions can never trigger the king-death win condition --
  without an unconditional way out, playing one would permanently strand
  you in STATE_PLAYING.
- Asset frame caches (tiles + unit icons) live in PSRAM via `ps_malloc()`,
  not internal SRAM -- two caches were already ~110KB as plain static
  arrays (34% of the ~320KB internal RAM budget), and more asset types
  are coming
- WiFi + OTA updates (`ArduinoOTA`): flash once over USB, then push
  subsequent builds wirelessly — see "Build & flash" below

## What's not implemented yet

Scripted mission events (`m*.script` files -- the extra encounters/
triggers that make `m4` and `m6` more than "one starting skirmish"), the
original's actual AI (this milestone's is a simple heuristic -- see
above), the combat property bonuses noted above, any HUD beyond the turn
indicator/END TURN/MENU buttons/mission menu, MIDI music (needs its own
synth — see the "Music" question this was scoped against), and skirmish
maps (`s0`-`s11`, which would also need a 4-side, non-hardcoded turn
queue -- see the team-color note above). The original
`MainDisplayable.java` is ~11,000 lines covering all of that; this
milestone reads its map-loading format, terrain layer, unit starting
positions, and enough movement/combat/capture rules for a single-player
skirmish against a basic AI, playable to a conclusion on the 6 story
maps that start with red units to fight (`m4` and `m6` don't -- see
above -- so those two can't yet reach the win condition).

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
(all 8 maps' headers parse to plausible dimensions, every map's unit
records parse to plausible type/color/tile values with zero leftover
bytes, tile and unit-icon files are the expected size). The
terrain-cost, unit-move-range, combat-stat (offence/defence/
attack-range), and unit-property (`UNIT_PROPERTIES`) tables in
`main.cpp` were cross-checked field-by-field against `tiles0.prop` and
each `*.unit` file. **It has not been flashed to or run on physical
hardware** — this session has no access to your board. Display/touch
pins are confirmed; SD/MMC pins and WiFi/OTA are not — flash over USB
first and report back what you see. The AI has no recursion and no
`while(true)`; its one loop with a runtime-dependent trip count
(`computeReachable()`'s `while (changed)` relaxation) terminates because
each cell's movement budget only increases and is capped by
`UNIT_MOVE_RANGE`, so a hang isn't the structural risk. `endTurn()`
resolves every AI unit's move/attack and mutates game state before a
single `drawViewport()` call at the end -- there's no per-unit animation
to watch, only the final board state, and whether that final state looks
like a coherent turn (not e.g. every unit just sitting still) is
unverified. So is the zero-reachable-tiles edge case (handled by falling
through to `hasMoved = true` without a move). The SD/MMC init, tile/unit
rendering, tap-vs-drag detection (the `TAP_MOVE_THRESHOLD` in `main.cpp`
is a guess), menu tap hit-testing, and OTA path are the other things
most likely to need a follow-up fix once you can see real output.

## Suggested next milestones

1. Get milestone 7 actually rendering and responding to touch on your
   hardware (mission menu + terrain + units + movement + combat +
   capture + AI), fix pins/driver/touch-threshold/AI-behavior quirks
   that only show up on real silicon
2. Scripted mission events, music, a smarter AI
