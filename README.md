# AE2RM

**Ancient Empires 2 Revolution Mod (AE2RM)** — a turn-based tactics game
originally released for J2ME (feature) phones. This repository hosts the
game's original download and source code, plus an in-progress ESP32-S3
port that rewrites it from scratch for real hardware.

* [Project main page (archived)](https://web.archive.org/web/20201110233517/http://projectd8.org/Ancient_Empires_II_RM)
* [Download the original game (`jar/`)](jar/)
* [Original J2ME source code (`src/`)](src/)
* [ESP32 port (`firmware/`)](firmware/)

## The ESP32 port

`firmware/` is a ground-up rewrite of the game in C++/Arduino for an
ESP32-S3 board, not an automatic conversion — the original is J2ME/MIDP
Java (~19,500 lines), and there's no JVM on a microcontroller. Every
subsystem below was ported by reading the decompiled original
(`src/java/src/aeii/*.java`) and its data files (`.unit`, `.prop`, `.aem`
map format) and re-implementing the relevant behavior in C++, verifying
each new table against its source file field-by-field where practical.

**Target hardware:** an ESP32-S3 2.8" board, ES3C28P ("Xiaozhi" variant)
— ILI9341 240x320 SPI TFT, FT6336 capacitive touch, SD/MMC storage,
16MB flash, PSRAM. See [`firmware/README.md`](firmware/README.md) for
the full pinout and hardware details.

### What works today

- **Rendering**: the original's tile/sprite assets are converted to raw
  RGB565 by `firmware/tools/convert_assets.py` and loaded from a microSD
  card; the game renders a scrollable, touch-pannable view of the map
- **All 8 story maps** (`m0`-`m7`), loaded from the original `.aem`
  binary format, with a mission-select menu
- **Units**: all 12 unit types, rendered as team-colored map icons, with
  real movement ranges and terrain movement costs taken directly from
  the original's `.unit` and tileset data files
- **Combat**: damage, counterattacks, and unit death, following the core
  of the original's damage formula (without its per-unit matchup
  bonuses — see the firmware README for exactly what's and isn't ported)
- **Capture**: villages and castles change ownership when the right unit
  type moves onto them, matching the original's capture-eligibility
  rules exactly
- **An AI opponent**: you play blue, the computer plays red — this makes
  the game genuinely single-player. It targets the weakest in-range
  enemy and retreats when critically low on health, but it's still a
  simple heuristic, not a port of the original's scoring-based AI
- **A unit stat panel**: tap any unit you can't currently act with — an
  enemy, or one that's already moved — to see its type, HP, attack,
  defence, range, and movement
- **Real mission titles**, and **`m0`'s intro cutscene**: the original's
  localized string table (`lang.dat`) is now read for mission-menu
  labels and for actual English dialog text, and a small interpreter
  runs the opening scene of `m0.script` — the only mission-script file
  in the source archive — repositioning units, removing one scripted
  casualty, panning the camera, and showing real story dialog before
  handing control to the player. Only the intro (`@Case 0`-`13`); the
  rest of the script (tutorial hints and an ending dialog, both gated on
  live game state) isn't ported yet
- **OTA firmware updates** over WiFi, after an initial USB flash

### What's not there yet

An AI on par with the original; two of the eight story maps (`m4`, `m6`)
start with zero enemy units in their map data and so can't reach the
win condition at all in single-player yet — the reason isn't fully
known (only `m0` has a mission-script file at all, and its rest — the
part that could conceivably explain this — isn't ported yet either; it
applies equally to `m1`, `m2`, `m3`, `m5`, `m7`, which do work with no
script). Also missing: in-game menus beyond mission select, and MIDI
music (the original's music format needs its own synthesizer — there's
no audio subsystem yet). See [`firmware/README.md`](firmware/README.md)
for the full list and the reasoning behind each scoping decision.

### Roadmap

Each milestone below was scoped, built, and reviewed as its own pull
request — see each one for the full implementation detail and review
history.

**Done:**

1. [Terrain rendering](https://github.com/hayden-flintoft/AE2RM/pull/1) —
   asset pipeline, `.aem` map loading, touch-pannable tile viewport
2. [Starting units](https://github.com/hayden-flintoft/AE2RM/pull/2) —
   unit icons rendered at their map-file positions
3. [Movement](https://github.com/hayden-flintoft/AE2RM/pull/3) —
   tap-to-select, terrain-cost movement range, tap-to-move, turn passing
4. [Combat](https://github.com/hayden-flintoft/AE2RM/pull/4) — attack
   range, damage, counterattacks, unit death
5. [Capture & win condition](https://github.com/hayden-flintoft/AE2RM/pull/5)
   — village/castle capture, a side loses when its king dies
6. [Mission menu](https://github.com/hayden-flintoft/AE2RM/pull/6) — all
   8 story maps selectable, an unconditional way back to the menu
7. [Basic AI opponent](https://github.com/hayden-flintoft/AE2RM/pull/7)
   — single-player vs. a simple heuristic AI
8. [Smarter AI](https://github.com/hayden-flintoft/AE2RM/pull/8) —
   weakest-target prioritization and low-health retreat
9. [Unit stat panel](https://github.com/hayden-flintoft/AE2RM/pull/9) —
   tap any unit you can't act with to see its stats
10. [Mission-script interpreter, part 1](https://github.com/hayden-flintoft/AE2RM/pull/10)
    — real mission titles, `m0`'s intro cutscene with actual dialog text

**Planned, not yet started (no fixed order):**

- Get milestone 7 actually running on the physical board and fix
  whatever real hardware reveals — pins, touch feel, timing, AI behavior
  you can actually watch
- The rest of `m0`'s mission-script (`@Case 14` onward — tutorial hints
  and an ending dialog, both gated on live game state, needing a real
  per-frame condition-evaluating state machine the milestone 10 intro
  didn't need) and investigate whether `m4`/`m6`'s missing red units are
  recoverable some other way, since neither is explained by the intro
  cutscene alone
- MIDI music via a small on-device synthesizer
- Further AI work (defensive positioning, unit-coordination, closer to
  the original's scoring heuristic)
- More in-game HUD beyond mission select and the unit stat panel
  (settings, etc.)
- Skirmish maps (`s0`-`s11`), which need a real building-derived,
  4-side turn queue instead of the hardcoded 2-side one story maps use

### Verification status

**This firmware has not yet been flashed to or run on physical
hardware.** Development has happened without direct access to the
target board: every piece of game data (movement ranges, combat stats,
terrain costs, capture rules) has been cross-checked against the
original's source files, and the code builds cleanly, but nothing has
been confirmed against real touch input, display output, or timing.
Display and touch GPIO pins have been confirmed working by the hardware
owner; several other pin assignments and behaviors are still flagged as
unverified in `firmware/README.md`. If you're picking this repository
up, flashing it and reporting back what you see is the most valuable
next step.

## Building the firmware

```bash
cd src && make               # extracts AEIIRM_src.zip -> src/java
cd ../firmware
pip install Pillow
python3 tools/convert_assets.py
# copy firmware/assets/sdcard/ onto a FAT32 microSD card
pio run -t upload            # requires PlatformIO: pip install platformio
```

Full build, flash, and OTA-update instructions are in
[`firmware/README.md`](firmware/README.md).
