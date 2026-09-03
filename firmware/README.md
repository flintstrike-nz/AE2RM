# AE2RM ESP32 port (firmware)

Porting AE2RM (a ~19,500-line J2ME/MIDP game) to an ESP32 is a full rewrite,
not an automatic conversion — there's no JVM here. This is **milestone 17**:
terrain, units, movement, combat, capture, and the mission menu
(milestones 1-6), an AI opponent (milestones 7-8, 11, 13) — you're always
blue, the computer is always red, so this is playable single-player — a
tap-to-inspect unit stat panel, living-unit-count HUD readout, and RETRY
button on the win/loss banner (milestones 9, 12, 14), a full-screen mission
briefing before every mission (milestone 15), a title screen and a combat
hit-flash effect using the original's own art (milestone 16), `m0`'s
scripted sprite effects (`CreateSpriteAtUnit`, milestone 17), and `m0`'s
intro cutscene, the first piece of the original's scripted mission events
to be ported (milestone 10; see "Mission-script interpreter" below for
exactly how much of the format that covers).

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
  units at all in their starting layout. It's tempting to blame this on
  unported mission scripting, but that doesn't hold up: the source
  archive this port builds from contains a `.script` file (a real
  cutscene/dialog interpreter language) for `m0` only, not for any of
  `m1`-`m7` -- and the other five (`m1`,`m2`,`m3`,`m5`,`m7`) work fine
  without one. Why `m4`/`m6` specifically ship with no starting
  opposition in their `.aem` data isn't known.
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
- Hit-flash effect (`playHitEffect()` in `main.cpp`, new this milestone):
  every hit -- the direct attack and, if it happens, the counterattack --
  plays the original's own combat spark (`createSimpleSparkSprite()`
  with `sprRedSpark` in `MainDisplayable.java`, converted from
  `redspark.png`'s 6-frame sheet) over the target's tile, with a static
  "-N" damage label. A real pause (`COUNTERATTACK_PAUSE_MS`, 300ms)
  separates the attack from the counter so both are actually visible,
  not just one final redraw -- an approximation of the original's own
  ~800ms gap between them. Not ported: the original's damage label
  rises and fades over that time; this just holds it in place for the
  spark's duration then lets the next redraw clear it (this port has no
  alpha-blending pipeline to fade it with). Each of the 6 frames forces
  a full `drawViewport()` redraw first -- `pushImage()`'s transparent-skip
  only omits *source* pixels that are transparent, so without resetting
  the background before every frame, the previous frame's opaque pixels
  would smear into the next -- unverified on real hardware whether that
  redraw-per-frame cost makes the animation feel too slow.
- `m0`'s scripted sprite effects (`spawnCutsceneSpriteEffect()`/
  `tickCutsceneEffects()` in `main.cpp`, new this milestone):
  `runIntroScript()` now handles the intro's `CreateSpriteAtUnit` commands
  for real, instead of silently skipping them -- `RedSpark`/`Spark`/`Smoke`
  (`spark.png`/`smoke.png`, converted the same per-frame way as
  `redspark.png`), positioned from the scripted unit's tile. Non-blocking,
  matching the original's own `showSpriteOnMap()`: `CreateSpriteAtUnit`
  queues an effect into a small fixed slot table and returns immediately,
  and the following `Wait` command is what actually ticks and redraws it
  (in `CUTSCENE_EFFECT_TICK_MS` = 50ms steps, the same cadence
  `Sprite.update()` runs at) -- so consecutive effects (m0's Spark + Smoke
  on the same casualty) run concurrently instead of serializing, and
  `Wait` stays the script's real pacing control instead of each effect
  adding its own duration on top. Two more details only matter once it's
  actually ticking: the command's `sx`/`sy` are a per-tick motion delta,
  not a one-time offset (confirmed in `Sprite.update()`'s default case --
  `setPosition(currentX + shiftX, currentY + shiftY)` runs every tick, not
  once -- m0's Smoke uses `(0, -3)` and rises for its whole duration), and
  `bounceMode` is a repeat count, not an animation variant (decremented
  each time the frame sequence wraps back to frame 0; m0's `RedSpark`
  passes `2`, so it loops twice). Unlike `playHitEffect()`'s statically
  cached frame buffer (loaded once, reused on every combat hit all game),
  this loads its buffer fresh per tick and frees it once nothing's active
  -- the intro script only triggers a handful of these total, so there's
  nothing worth keeping cached.
- Basic AI (`aiActUnit()` in `main.cpp`): color 1 (red) is always the
  computer, color 0 (blue) is always you -- there's no way to flip this,
  and the two-human hotseat mode from earlier milestones no longer
  applies (`endTurn()` always auto-resolves red's turn through the AI).
  Each AI unit, in order: attacks an enemy already in range from where it
  stands; else, if some reachable tile puts an enemy in range, moves
  there and attacks (the AI *is* allowed "move then attack" -- for the
  human side that's a deliberate simplification, not a technical limit:
  the AI already shows this is doable by scanning every reachable tile
  for an attack opportunity, but doing that live as a human drags a
  selection around -- highlighting which of many reachable tiles also
  opens an attack -- is more UI than this milestone scoped; every
  reachable attack-capable tile is scored, not just the first one found
  in scan order. A tile reaching a target this unit is **guaranteed to
  kill** (`wouldGuaranteeKill()` -- true if even the worst-case damage
  roll, using `UNIT_OFFENCE_MIN` against the same terrain-adjusted
  defence `resolveHit()` computes, still finishes the target) always
  wins over one that only wounds, whatever the health numbers; among
  tiles tying on that, the one giving the lowest-health target wins;
  among tiles tying on *that*, the one with the best terrain defence
  bonus for *this* unit wins -- a free tiebreak using the same
  `TERRAIN_DEFENCE_BONUS` lookup `resolveHit()` already applies to a
  defender, not a real lookahead at whether a counterattack will
  actually land there);
  else, if this unit's own health is at or below 25, retreats -- moves
  to whichever reachable tile *maximizes* its distance to the *closest*
  living enemy (scored against every enemy on the board, not just
  whichever one was nearest before moving -- with enemies on multiple
  sides, only that minimum says how exposed a tile actually leaves the
  unit), instead of closing the distance, to avoid handing away a free
  kill; else moves toward the nearest living enemy to close the distance
  for a later turn; else (no enemies left) does nothing. When it does
  attack, `findAttackTarget()` applies that same guaranteed-kill-then-
  weakest rule, not just the first target found in array order --
  securing a kill beats a bigger wound elsewhere, and finishing off a
  damaged unit (when neither or both options are a kill) is a permanent
  gain over splitting damage across several full-health enemies. No
  pathfinding beyond `computeReachable()`'s flood fill, no defensive
  positioning beyond the retreat rule and the attack-tile terrain
  tiebreak, no value-based target prioritization beyond "is it a
  guaranteed kill" and "what's its health" (not by unit type or tactical
  importance), and no coordination between units. This is **not** a
  port of the original's
  AI, which is a large scoring heuristic spanning much of
  `MainDisplayable.java` (`sub_10cb()` and friends) -- it's enough to
  make single-player winnable and losable, nothing more.
- Village/castle capture: moving a unit onto an enemy or neutral
  fraction-building tile flips its ownership if that unit type is
  equipped to capture it, per each `.unit` file's `HasProperty` bits
  (`UNIT_PROPERTIES` in `main.cpp`) -- soldier and king can capture
  villages, but **only the king can capture a castle**, matching the
  source data (no other unit has that property bit set).
- Win condition: a side loses when its king dies in combat, **or** when it
  has no units left at all -- `checkEndConditions()`, run after every
  combat exchange and every turn, so clearing the last enemy (king or
  not) ends the mission instead of leaving you stuck on a won board. A
  side that started with zero units (`m4`/`m6` place no red units) is
  exempt -- its emptiness isn't a defeat. This is a simplification of the
  original, which ties defeat more to castle capture/`fractionKings`
  bookkeeping this milestone doesn't track. The
  screen shows a "BLUE/RED WINS" banner with a **RETRY** button that
  reloads the same mission (`currentMapIndex`, set each time
  `startGame()` runs) without a trip through the menu; tapping anywhere
  else on the banner returns to the mission menu (see below) instead.
- Unit stat panel: tapping any living unit that *isn't* selectable this
  turn (an enemy, or a friendly unit that's already moved) shows a small
  bottom-left panel with its type, current HP, attack/defence, attack
  range, and move range (`infoUnit` in `main.cpp`) instead of doing
  nothing. Tapping empty ground, or the MENU/END TURN buttons, dismisses
  it; tapping a unit that *is* selectable still selects it for movement
  as before -- the two interactions don't conflict since a unit is never
  both at once. No portrait, no per-unit ability text, no equivalent to
  the original's fuller unit-info screen.
- Living-unit counts: the top HUD bar, next to the BLUE/RED TURN
  indicator, shows how many of each side's units are still alive, as
  `blueCount:redCount` in each side's color, recomputed every redraw.
  Only colors 0/1 are counted -- consistent with the rest of this port,
  which only ever loads story maps that place those two -- so this
  isn't a general 4-color skirmish scoreboard.
- Title screen (`showTitleScreen()` in `main.cpp`, new this milestone):
  shown once at boot, before the mission menu -- the original's own
  `splash.png` (exactly 240x320, a pixel-perfect match for this
  display) with `logo.png` composited over it, tap to continue. Not the
  original's actual title screen: that's a multi-stage alpha-fade
  transition (a studio splash fades in and out, then the game logo
  fades in over black, then the background fades in behind it with its
  own glow effect -- `updateIntroTransition()` in
  `MainDisplayable.java`) driven by an alpha counter this port has no
  blending pipeline for. This shows the two images statically instead
  of animating the transition between them, and doesn't show the
  studio splash (`ms_logo.png`) at all -- a real simplification,
  documented in the code rather than silently dropped. Skipped entirely
  (straight to the menu) if either asset is missing.
- Mission menu: boots to a list of `m0.aem` through `m7.aem`'s real
  titles ("TEMPLE RAIDERS", "TO THE RESCUE", etc. -- locale string
  indices 121-128, `getSaveInfoString()`'s label in the original, close
  enough to a menu title to reuse -- see "Mission-script interpreter"
  below for where these strings come from) instead of the generic
  "Mission N" labels earlier milestones showed; a missing/failed-to-load
  `strings.dat` falls back to those generic labels rather than an empty
  row. Tapping a row loads that map and resets all per-game state (turn,
  selection, win/loss, camera position); the tile and unit-icon asset
  caches are content-independent across maps and are deliberately not
  reset. The win banner's tap-to-continue returns here -- and so does a
  dedicated **MENU** button, always available during a mission regardless
  of game state. That button isn't optional polish: `m4`/`m6` place no
  red units in their map data (see above), so those two missions can
  never trigger the king-death win condition -- without an unconditional
  way out, playing one would permanently strand you in STATE_PLAYING.
- Mission briefing (`showMissionBriefing()` in `main.cpp`, new this
  milestone): right after a map loads -- all 8 story maps, not just
  `m0` -- a full-screen title-and-objective card shows before handing
  control to the player (or, for `m0`, before its intro cutscene runs).
  The objective text is locale string 129+mapIndex, one entry per story
  map -- `getSaveInfoString()`'s sibling table in the original
  (`PaintableObject.getLocaleString()`), discovered while wiring up
  mission titles in an earlier milestone but not read until now. Skipped
  entirely (straight into gameplay) if `strings.dat` isn't loaded, same
  fallback as the mission menu's generic titles. Shares its word-wrap
  and tap-to-continue logic (`drawWrappedText()`/`waitForTapRelease()`)
  with `showScriptDialog()` below rather than duplicating it.
- Mission-script interpreter (`runIntroScript()` in `main.cpp`, new this
  milestone): `m0.script` is the only mission-script file in the source
  archive (a real cutscene/dialog language -- `MainDisplayable.java`
  interprets ~35 `@Case` blocks of it across the whole mission, gated on
  live game-state `Test`s like `CurrentTurn`/`CountUnits`/`GameState`,
  ending in tutorial `ShowHelp` overlays and a `CompleteMission` epilogue).
  This milestone ports only its intro cutscene (`@Case 0`-`13`, ending at
  `StartPlay`) as a one-shot linear pass run once at mission start, not a
  per-frame state machine -- the remaining cases need one (to evaluate
  `Test`s against ongoing play) and are explicitly not attempted here.
  What the intro does: repositions three of `m0`'s starting units to
  where the story leaves them post-cutscene (`GetUnitPlotRoute`, teleport
  instead of the original's animated walk), removes one scripted casualty
  (`GetUnit`+`RemoveUnit` -- a blue soldier dies before the player's
  forces arrive), pans the camera to specific tiles or a side's king
  (`MoveMapAndCursor`, a jump cut instead of a smooth pan), and shows four
  real dialog lines with the original's actual English text
  (`ShowDialog`, looked up from `/strings.dat` -- see below -- rendered
  in a bottom text box, blocking on a tap to continue), and plays its
  scripted sprite effects (`CreateSpriteAtUnit`, milestone 17 -- see
  `spawnCutsceneSpriteEffect()`/`tickCutsceneEffects()` above;
  `RedSpark`/`Spark`/`Smoke` over the named unit's tile, ticked and
  animated during the following `Wait`). Every other
  command in that case range (`ShowMapName`, `NextState`,
  `SetFadeEnabled`/`SetFadeValue`, `SetCursorVisible`, `SetMapStepMax`/
  `SetUnitSpeed`, `Vibrate`, `ScheduleUnitAnimationStop`, `StartPlay`) has
  no equivalent in this port (no fade/cursor-sprite system, no per-tile
  movement animation to pace) and is silently skipped, not simulated.
  Getting the
  `GetUnitPlotRoute` argument order right (`x y color destX destY
  animate`, not `color x y destX destY` -- easy to guess wrong from the
  script's own bare integers) was verified by checking it against
  `m0.aem`'s actual unit starting positions before shipping, not just
  against the Java source reading order.
- `/strings.dat`: the original's localized string table
  (`PaintableObject.getLocaleString()`), copied byte-for-byte from
  `src/java/res/lang.dat` by `convert_assets.py` -- same on-disk format
  (int32 BE count, then that many uint16-BE-length-prefixed UTF-8
  strings), loaded once at boot (`loadStrings()`) into a single PSRAM
  buffer. Currently used for mission titles and `m0`'s dialog text only;
  the table has ~300 more strings (menu labels, unit names, etc.) this
  milestone doesn't read.
- Asset frame caches (tiles + unit icons) live in PSRAM via `ps_malloc()`,
  not internal SRAM -- two caches were already ~110KB as plain static
  arrays (34% of the ~320KB internal RAM budget), and more asset types
  are coming
- WiFi + OTA updates (`ArduinoOTA`): flash once over USB, then push
  subsequent builds wirelessly — see "Build & flash" below

## What's not implemented yet

The rest of `m0`'s mission-script (`@Case 14` onward -- the Test-gated
tutorial `ShowHelp` overlays checked against live game state, and the
`CompleteMission` epilogue dialog; not what's causing `m4`/`m6`'s missing
red units, see above), the original's actual AI (this milestone's is a
simple heuristic -- see above), the combat property bonuses noted above,
any HUD beyond the turn indicator/living-unit counts/END TURN/MENU
buttons/mission menu/unit stat panel/dialog box, MIDI music (needs its own synth — see
the "Music" question this was scoped against), and skirmish maps
(`s0`-`s11`, which would also need a 4-side, non-hardcoded turn queue --
see the team-color note above). The original `MainDisplayable.java` is
~11,000 lines covering all of that; this milestone reads its map-loading
format, terrain layer, unit starting positions, enough movement/combat/
capture rules for a single-player skirmish against a basic AI, real
mission titles and `m0`'s intro cutscene, and is playable to a
conclusion on the 6 story maps that start with red units to fight (`m4`
and `m6` don't -- see above -- so those two can't yet reach the win
condition).

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
`m0`'s intro cutscene is additionally unverified in two ways specific to
it: `strings.dat`'s parse (header count, length-prefixed reads) was only
checked by inspecting the Python asset converter's copy of `lang.dat`
and reasoning through the format, not by running the firmware's own
reader; and the `GetUnitPlotRoute`/`MoveMapAndCursor`/`GetUnit` argument
order was checked against `m0.aem`'s actual starting unit positions with
a one-off script, not exercised on-device (see the "Mission-script
interpreter" bullet above for what that check found).

## Suggested next milestones

1. Get milestone 7 actually rendering and responding to touch on your
   hardware (mission menu + terrain + units + movement + combat +
   capture + AI), fix pins/driver/touch-threshold/AI-behavior quirks
   that only show up on real silicon
2. The rest of `m0`'s mission-script (tutorial `ShowHelp` overlays and
   the ending dialog -- needs a real per-frame `Test`-evaluating state
   machine, not the one-shot linear pass milestone 10 uses for the
   intro), music
