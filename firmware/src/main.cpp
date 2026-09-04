// AE2RM ESP32 port -- milestone 17: single-player vs. a basic AI, plus
// m0's mission-script intro cutscene, now including its scripted sprite
// effects (CreateSpriteAtUnit).
//
// Boots to a title screen (the original's own splash/logo art, see
// showTitleScreen()), tap to continue, then a mission menu with two tabs:
// the 8 story missions (m0.aem-m7.aem) and the 12 skirmish maps
// (s0.aem-s11.aem), showing each map's real title when /strings.dat
// loaded (see loadStrings()) else a generic label. Tap one to play: a
// full-screen briefing card shows first, tap to start. You are always
// blue on a story map (the computer is red); on a skirmish map the turn
// queue and side count come from castle ownership -- you are colour 0
// (the first castle found), every other side is AI (see
// buildSkirmishTurnQueue()). m0 additionally runs its
// mission-script's intro cutscene once at mission start, after the
// briefing (see runIntroScript()) -- real dialog text, a couple of
// scripted unit moves/removals, and camera pans; the other 7 maps start
// playable immediately after their briefing. The camera opens centered on
// your king. Tap a unit belonging to the current turn's side to select it
// -- its movement range highlights cyan (terrain-cost-limited flood fill)
// and every enemy it could attack this turn (from where it stands or
// after moving within that range) highlights red. Tap a cyan tile to move
// there (capturing a village/castle you move onto, if this unit type
// can), tap a red-highlighted enemy to attack it, stepping into range
// first if needed (one action per unit per turn: a move, or a
// move-then-attack, matching the original). A unit that has used its
// action is greyed out until next turn. Every hit -- and the
// counterattack, if it happens -- plays the original's own spark effect
// and a damage number over the target (see playHitEffect()), paced so
// both are actually visible. Tap
// "END TURN" to pass to the AI, which immediately plays its whole turn
// (attack a guaranteed kill if one's in range, else the weakest in-range
// enemy, else move-and-attack, else retreat if critically low on health,
// else close the distance -- see aiActUnit()'s comment; not a port of
// the original's scoring-heuristic AI) and hands back control.
//
// A fixed header/footer frame the map (see drawHud()): the header shows
// whose turn it is and your gold; the footer shows the selected/inspected
// unit's stats or the living-unit tally, plus MENU (hamburger), SHOP
// (cart) and END TURN (return-arrow) icon buttons. Each side earns gold
// at its turn start from the villages (+30) and castles (+50) it owns;
// the SHOP button (on your turn, if you hold a castle) opens a recruit
// list -- pick an affordable unit, then tap a green tile next to one of
// your castles to deploy it. The AI recruits too. A unit that starts its
// side's turn on a neutral town or a building that side owns heals up to
// 20 HP. MENU opens an in-mission pause menu -- return / save / load (an
// NVS snapshot) / exit to title. The turn model runs a queue of up to four
// sides (story maps m0-m7 are always 2: color 0 blue = human, color 1 red
// = AI); endTurn() resolves every AI side in the queue before control
// returns to the human. A mission ends when only one side still has units
// -- a side's king dying routs its whole army (checkEndConditions()), a
// simplification of the original's castle-capture-tied defeat, see
// README. With every side wiped out at once the banner reads DRAW. The win banner has a RETRY button that reloads the same mission
// directly; tapping anywhere else on the banner returns to the mission
// menu. Drag pans the camera during a mission. See firmware/README.md for
// what's implemented and what's next.

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include "LGFX_Config.h"
#include "board_pins.h"
#include "embedded_assets.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
// No secrets.h -- see include/secrets.h.example. Without it, WiFi/OTA are
// skipped entirely and the firmware just runs standalone off the SD card.
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define OTA_PASSWORD ""
#endif

static LGFX gfx;
static bool otaInProgress = false;

// sdReady: the microSD card mounted. assetsReady: the game has a usable
// asset source -- the SD card, or a set of assets baked into the firmware
// by tools/convert_assets.py (see openAsset() / embedded_assets.h). When
// neither is available we still finish setup() and run loop() with the
// game logic gated off, so WiFi/OTA stay alive and a fixed build can be
// pushed over the air instead of forcing a USB trip for every SD problem.
static bool sdReady = false;
static bool assetsReady = false;

constexpr int TILE_SIZE = 24;
constexpr int TILE_COUNT = 48;

// On-screen layout: a header band (whose turn it is + gold) and a footer
// band (MENU / SHOP / END TURN buttons, and the selected-unit stats or
// living-unit tally) frame the map so the HUD never sits on top of
// terrain or units. The scrollable map viewport is only the strip between
// them -- every map<->screen conversion offsets the y axis by MAP_VIEW_Y
// and clamps/clips to MAP_VIEW_H.
constexpr int HEADER_H = 16;
constexpr int FOOTER_H = 46;
constexpr int MAP_VIEW_Y = HEADER_H;
constexpr int MAP_VIEW_H = DISPLAY_HEIGHT - HEADER_H - FOOTER_H;
constexpr int FOOTER_Y = DISPLAY_HEIGHT - FOOTER_H;

// On-device touch calibration aid: when true, boot shows a full-screen
// crosshair + coordinate readout harness (touchTest()) so a mis-mapped
// panel is obvious. Left in, off by default, now that offset_rotation is
// set in LGFX_Config.h.
constexpr bool TOUCH_DEBUG = false;

// Asset frame caches live in PSRAM, not internal SRAM: two full caches
// (tiles + unit icons) already used ~110KB of the ~320KB of internal RAM
// as plain static arrays, and more asset types are coming in later
// milestones. Allocated once in setup() by allocAssetCaches().
static uint16_t *tileCachePixels = nullptr; // [TILE_COUNT][TILE_SIZE*TILE_SIZE], flattened
inline uint16_t *tileFrame(int index) { return tileCachePixels + (size_t)index * TILE_SIZE * TILE_SIZE; }
static bool tileLoaded[TILE_COUNT] = {false};

static uint8_t *mapTiles = nullptr; // [x * mapHeight + y], matches the
                                     // column-major layout MainDisplayable
                                     // reads from the .aem file
static int mapWidth = 0;
static int mapHeight = 0;

// Unit map-icons: Unit.UNIT_NAMES has 12 types; unit_icons.png comes in 4
// team colors (MainDisplayable.FRACTION_COLOR_PREFIXES order). tools/
// convert_assets.py writes transparent source pixels as this sentinel so
// pushImage() can skip them -- these icons aren't square.
constexpr int UNIT_ICON_SIZE = 24;
constexpr int UNIT_TYPE_COUNT = 12;
constexpr int UNIT_COLOR_COUNT = 4;
constexpr uint16_t TRANSPARENT_565 = 0xF81F;
static const char *UNIT_COLOR_NAMES[UNIT_COLOR_COUNT] = {"blue", "red", "green", "black"};

static uint16_t *unitIconCachePixels = nullptr; // [UNIT_COLOR_COUNT*UNIT_TYPE_COUNT][UNIT_ICON_SIZE*UNIT_ICON_SIZE], flattened
inline uint16_t *unitIconFrame(int color, int type)
{
    return unitIconCachePixels + ((size_t)color * UNIT_TYPE_COUNT + type) * UNIT_ICON_SIZE * UNIT_ICON_SIZE;
}
static bool unitIconLoaded[UNIT_COLOR_COUNT][UNIT_TYPE_COUNT] = {};

// A unit placement read from the map file's trailing unit-record list.
// `color` is the map file's raw color slot (0-3, indexing UNIT_COLOR_NAMES
// directly), used directly as this unit's team/turn. This is exact for
// story maps (m0-m7): MainDisplayable.java's loadMap(), for skirmishMode
// == 0 (which every "m*.aem" map uses), always hardcodes a 2-side turn
// queue where raw color 0 maps to fraction 1 (drawn blue) and raw color 1
// to fraction 2 (drawn red) -- see getBuildingFraction()/setBuildingFraction()
// and the turn-queue setup in loadMap(). It is NOT exact for skirmish maps
// (s0-s11, not converted/loaded by this firmware), which build a real
// building-derived turn queue supporting up to 4 sides; that logic isn't
// ported.
struct UnitPlacement
{
    uint8_t type;
    uint8_t color;
    int16_t tileX;
    int16_t tileY;
    bool hasMoved;
    bool alive;
    uint8_t health; // 0-100, matches Unit.java's percentage scale
};
constexpr int MAX_UNITS = 64;
static UnitPlacement units[MAX_UNITS];
static int unitCount = 0;

// Terrain movement cost per type (tiles0.prop's TypeDef line 2: "TypeDef
// index moveCost defenceBonus name name") and the tile-index -> terrain-type
// mapping (TypeDef's TileDef lines, first of the two type fields -- the
// second is only used for the tileset's visual blending, not gameplay).
// Values are this specific tileset's (tiles0), not general engine
// constants, but every current map uses tiles0.
constexpr int TERRAIN_TYPE_COUNT = 11;
static const uint8_t TERRAIN_MOVE_COST[TERRAIN_TYPE_COUNT] = {1, 1, 2, 2, 3, 3, 1, 1, 1, 1, 3};
static const int8_t TERRAIN_DEFENCE_BONUS[TERRAIN_TYPE_COUNT] = {0, 5, 10, 10, 15, 0, 5, 15, 15, 15, -15};
static const uint8_t TILE_TERRAIN_TYPE[TILE_COUNT] = {
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, // 00-14: water
    2, 2,                                        // 15-16: woods
    4,                                           // 17: mountain
    1,                                           // 18: grass
    3,                                           // 19: hill
    0, 0, 0, 0, 0, 0, 0,                         // 20-26: road
    8,                                           // 27: broken building
    0,                                           // 28: road
    6, 6,                                        // 29-30: bridge
    7, 7,                                        // 31-32: town (non-fraction)
    3,                                           // 33: hill
    7,                                           // 34: town (non-fraction)
    3, 3,                                        // 35-36: hill
    8, 9, 8, 9, 8, 9, 8, 9, 8, 9,                 // 37-46: fraction buildings (village/castle pairs)
    10,                                          // 47: lava
};

// Unit.UNIT_NAMES order: soldier, archer, lizard, wizard, wisp, spider,
// golem, catapult, wyvern, king, skeleton, crystall (from each unit's
// "MoveRange" line in its .unit file).
constexpr uint8_t UNIT_TYPE_KING = 9;
static const uint8_t UNIT_MOVE_RANGE[UNIT_TYPE_COUNT] = {5, 5, 5, 5, 5, 6, 5, 4, 7, 5, 5, 4};

// Combat stats, from each unit's "Attack min max" / "Defence" /
// "AttackRange max min" lines. crystall (index 11) has 0/0/0/0 in its
// .unit file -- it's the non-combat "crystal escort" objective piece, not
// a fighting unit, and MAX_ATTACK_RANGE 0 naturally keeps it from ever
// being a valid attacker here.
static const uint8_t UNIT_OFFENCE_MIN[UNIT_TYPE_COUNT] = {50, 50, 50, 40, 35, 60, 60, 50, 70, 55, 40, 0};
static const uint8_t UNIT_OFFENCE_MAX[UNIT_TYPE_COUNT] = {55, 55, 55, 45, 40, 65, 70, 70, 80, 65, 50, 0};
static const uint8_t UNIT_DEFENCE[UNIT_TYPE_COUNT] = {5, 5, 10, 5, 10, 15, 30, 10, 25, 20, 2, 15};
static const uint8_t UNIT_ATTACK_RANGE_MAX[UNIT_TYPE_COUNT] = {1, 2, 1, 1, 1, 1, 1, 4, 1, 1, 1, 0};
static const uint8_t UNIT_ATTACK_RANGE_MIN[UNIT_TYPE_COUNT] = {1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 0};

// Recruitment cost, verbatim from each unit's ".unit" file "Cost" line.
// -1 means the source has no price at all (skeleton/crystall -- both
// scripted-only). The king (index 9) does carry a price in the data (200)
// but is NOT recruitable here regardless: the shop only ever offers unit
// types [0, SHOP_BUYABLE_COUNT) -- soldier..wyvern -- so index 9+ is out
// of range by construction, not by cost. Don't assume "cost > 0" means
// "buyable". The original also gates on a per-mission `allowedUnits` cap
// this port doesn't read yet.
static const int16_t UNIT_COST[UNIT_TYPE_COUNT] = {150, 250, 300, 400, 500, 600, 600, 700, 1000, 200, -1, -1};
constexpr int SHOP_BUYABLE_COUNT = 9; // recruitable types are exactly [0, 9): soldier..wyvern

// Per-turn income: the original credits +30 for each owned village and
// +50 for each owned castle at that side's turn start (MainDisplayable
// ~6412). Terrain type 8 = village, 9 = castle.
constexpr int VILLAGE_INCOME = 30;
constexpr int CASTLE_INCOME = 50;

// Same UNIT_TYPE_COUNT order as above, for the tap-to-inspect stat panel.
static const char *const UNIT_TYPE_NAMES[UNIT_TYPE_COUNT] = {
    "SOLDIER", "ARCHER", "LIZARD", "WIZARD", "WISP", "SPIDER",
    "GOLEM", "CATAPULT", "WYVERN", "KING", "SKELETON", "CRYSTAL",
};

// Per-side identity (UnitPlacement::color 0..3). The HUD colour for
// "black" is a light grey -- true black is invisible on the dark bands.
static const char *const PLAYER_NAME[4] = {"BLUE", "RED", "GREEN", "BLACK"};
static const uint16_t PLAYER_HUD_COLOR[4] = {TFT_CYAN, TFT_RED, TFT_GREEN, 0xBDF7};

// Bit flags from each unit's "HasProperty N" lines (UNIT_PROPERTIES[i] =
// OR of 1<<N for each line). Bits read here:
//   bit 3 (0x08) "can capture a village" -- soldier and king
//   bit 4 (0x10) "can capture a castle"  -- king only (no other unit in
//                this tileset has it)
//   bit 0 (0x01) "flyer"  -- wyvern; the archer's anti-air bonus targets it
//   bit 1 (0x02) "amphibious" -- lizard; its water offence/defence bonus
//   bit 6 (0x40) "anti-air"   -- archer; +15 vs. a flyer
// (see matchupOffenceBonus()/matchupDefenceBonus()). The remaining bits
// (other matchup flags) exist in the source data but aren't interpreted.
constexpr uint8_t UNIT_PROPERTY_CAPTURE_VILLAGE = 0x08;
constexpr uint8_t UNIT_PROPERTY_CAPTURE_CASTLE = 0x10;
static const uint16_t UNIT_PROPERTIES[UNIT_TYPE_COUNT] = {
    0x08, 0x40, 0x02, 0x20, 0x100, 0x80, 0x00, 0x200, 0x01, 0x1C, 0x00, 0x00,
};

// Fraction-building tiles (villages and castles) per MainDisplayable.java:
// FRACTION_BUILDINGS=37 is the first of 10 consecutive tile indices
// (37-46), 5 owner slots (0=neutral, 1-4=team) x 2 sub-types interleaved
// -- even offset from 37 is a village (terrain type 8), odd is a castle
// (terrain type 9). getBuildingFraction()/setBuildingFraction() convert
// between a tile index and its owning fraction with this exact formula.
constexpr uint8_t FRACTION_BUILDINGS = 37;
constexpr uint8_t CUSTOM_TILES = FRACTION_BUILDINGS + 10; // 47, exclusive upper bound

inline bool isFractionBuilding(uint8_t tile)
{
    return tile >= FRACTION_BUILDINGS && tile < CUSTOM_TILES;
}

inline int buildingFraction(uint8_t tile)
{
    return (tile - FRACTION_BUILDINGS) / 2; // 0-4
}

inline uint8_t setBuildingFraction(uint8_t tile, int fraction)
{
    return FRACTION_BUILDINGS + fraction * 2 + (tile - FRACTION_BUILDINGS) % 2;
}

// Up to 4 sides (`UnitPlacement::color` 0..3): story maps use exactly two
// (0 blue = human, 1 red = AI), but the turn model is general so skirmish
// maps can later add green/black. The human is always color 0; every
// other color in the turn queue is AI.
constexpr int MAX_PLAYERS = 4;
constexpr int HUMAN_COLOR = 0;

// The order sides take turns in. For story maps it's just {0, 1}; a
// skirmish map derives it from which fractions own a castle at start.
// currentTurn holds a color, not an index -- switchTurn() walks the queue.
static uint8_t turnQueue[MAX_PLAYERS] = {0, 1};
static int turnQueueLen = 2;

static bool gameOver = false;
static int winnerColor = -1; // colour of the last side standing, or -1 for a draw

// Living-unit count each side had once the mission was fully set up
// (after any m0 intro-script removals). A side is defeated when it drops
// to zero -- but only if it started with some: m4/m6 place no red units
// at all (see README), and a side that was never in the fight isn't a
// loser. Set at the end of startGame(); carried in a save.
static int startingUnits[MAX_PLAYERS] = {0, 0, 0, 0};

// Latched true once a side that started with units has none left (wiped
// out, or routed by its king's death). A defeated side stays defeated:
// switchTurn() skips its slot in the queue and the shop refuses it, so a
// later recruit can't bring it back into a 3-4 way fight. Cleared in
// startGame(); carried in a save.
static bool eliminated[MAX_PLAYERS] = {false, false, false, false};

// The mission startGame() most recently loaded (m<currentMapIndex>.aem),
// or -1 before any mission has loaded. Only used by the win/loss banner's
// RETRY button to reload the same mission without a trip through the
// mission menu.
static int currentMapIndex = -1;

// Movement points remaining when the currently selected unit could reach
// [x*mapHeight+y], or -1 if unreachable. Lazily allocated/resized to match
// the current map's dimensions inside computeReachable() (not in loadMap()
// -- there's no selected unit yet when a map loads), and recomputed there
// each time a unit is selected.
static int8_t *reachableCost = nullptr;
static int selectedUnit = -1; // index into units[], or -1 if none selected

// A unit whose stat panel is showing, or -1 if none. Distinct from
// selectedUnit: tapping *any* living unit -- an enemy, or one that's
// already moved this turn -- shows its stats without starting a move,
// while selectedUnit only ever holds a unit the current player can still
// act with. Cleared by tapping empty ground, the MENU/END TURN buttons,
// or selecting a movable unit.
static int infoUnit = -1;

// The original's localized string table (PaintableObject.getLocaleString()),
// copied as-is to /strings.dat by convert_assets.py. Loaded once (see
// loadStrings()) into a single PSRAM buffer of NUL-terminated strings, with
// scriptStringOffsets[i] giving string i's byte offset into it -- used for
// the mission menu's real titles (indices 121-128) and m0's mission-script
// dialog text. A missing/failed-to-load table degrades gracefully: the menu
// falls back to generic "Mission N" labels and the m0 intro cutscene is
// skipped entirely (see runIntroScript()) rather than showing blank dialogs.
static char *scriptStrings = nullptr;
static uint32_t *scriptStringOffsets = nullptr;
static int scriptStringCount = 0;

// Colour of the side whose turn it is (0..3). endTurn() resolves every
// non-human side in the queue through the AI synchronously before handing
// control back, so the player only ever sees currentTurn == HUMAN_COLOR.
static int currentTurn = 0;

// Treasury per side. Story maps start every side at 0 in the original;
// income accrues at each side's turn start from the villages/castles it
// owns (see computeIncome()). The header shows the human's gold; a save
// carries all four.
static int32_t gold[MAX_PLAYERS] = {0, 0, 0, 0};

// The in-mission pause menu (hamburger) and the shop are both modal over
// the game: while either is open drawViewport() paints it on top and
// loop()/handleTap() route touches to it, not the map. shopBuyType >= 0
// means the shop list picked a unit and we're now waiting for the player
// to tap a deploy tile.
static bool pauseMenuOpen = false;
static bool shopOpen = false;
static int shopBuyType = -1;

static int viewX = 0; // top-left of the viewport, in pixels, into the map
static int viewY = 0;

// Screen x/y of the top-left of map tile (mx, my), given the current
// scroll. The y axis is offset by MAP_VIEW_Y so the map draws in the
// strip between the header and footer, not the whole screen.
static inline int tileScreenX(int mx) { return mx * TILE_SIZE - viewX; }
static inline int tileScreenY(int my) { return my * TILE_SIZE - viewY + MAP_VIEW_Y; }

// m0.aem .. m7.aem -- the story maps, always a hardcoded 2-side fight
// (see UnitPlacement's comment). s0.aem .. s11.aem -- skirmish maps,
// whose turn queue and side count startGame() derives from castle
// ownership (buildSkirmishTurnQueue()).
constexpr int STORY_MAP_COUNT = 8;
constexpr int SKIRMISH_MAP_COUNT = 12;

// Every side starts a skirmish with this much gold (the original offers a
// menu of 500..200000; this port picks one fixed value -- there's no
// pre-match setup screen). buildSkirmishTurnQueue()'s comment covers the
// other simplifications.
constexpr int SKIRMISH_START_GOLD = 2000;

// True while the active game is a skirmish map (s<currentMapIndex>.aem);
// false for a story map. Set by startGame(), carried in the save, and
// read by RETRY and the win banner.
static bool skirmishMode = false;

enum AppState
{
    STATE_MENU,
    STATE_PLAYING,
};
static AppState appState = STATE_MENU;

// Opens a game asset by its card-relative path ("/tiles0/tile_00.bin",
// "/maps/m0.aem", ...). Prefers the SD card when it's mounted so a card
// can still override/extend the build; falls back to the copy baked into
// the firmware by tools/convert_assets.py. Returns an invalid File (bool
// == false) if neither source has it -- callers already handle that.
static File openAsset(const char *path)
{
    if (sdReady)
    {
        File f = SD_MMC.open(path, FILE_READ);
        if (f)
            return f;
    }
    return openEmbeddedAsset(path);
}

bool loadTile(int index)
{
    if (index < 0 || index >= TILE_COUNT)
        return false;
    if (tileLoaded[index])
        return true;

    char path[48];
    snprintf(path, sizeof(path), "/tiles0/tile_%02d.bin", index);
    File f = openAsset(path);
    if (!f)
    {
        Serial.printf("tile load failed: %s\n", path);
        return false;
    }
    constexpr size_t FRAME_BYTES = TILE_SIZE * TILE_SIZE * sizeof(uint16_t);
    size_t got = f.read(reinterpret_cast<uint8_t *>(tileFrame(index)), FRAME_BYTES);
    f.close();
    if (got != FRAME_BYTES)
    {
        Serial.printf("tile %d short read (%u/%u bytes)\n", index, (unsigned)got, (unsigned)FRAME_BYTES);
        return false;
    }
    tileLoaded[index] = true;
    return true;
}

bool loadUnitIcon(int color, int type)
{
    if (color < 0 || color >= UNIT_COLOR_COUNT || type < 0 || type >= UNIT_TYPE_COUNT)
        return false;
    if (unitIconLoaded[color][type])
        return true;

    char path[48];
    snprintf(path, sizeof(path), "/units/%s_%02d.bin", UNIT_COLOR_NAMES[color], type);
    File f = openAsset(path);
    if (!f)
    {
        Serial.printf("unit icon load failed: %s\n", path);
        return false;
    }
    constexpr size_t FRAME_BYTES = UNIT_ICON_SIZE * UNIT_ICON_SIZE * sizeof(uint16_t);
    size_t got = f.read(reinterpret_cast<uint8_t *>(unitIconFrame(color, type)), FRAME_BYTES);
    f.close();
    if (got != FRAME_BYTES)
    {
        Serial.printf("unit icon %s short read (%u/%u bytes)\n", path, (unsigned)got, (unsigned)FRAME_BYTES);
        return false;
    }
    unitIconLoaded[color][type] = true;
    return true;
}

bool readBE32(File &f, uint32_t &out)
{
    uint8_t b[4];
    if (f.read(b, 4) != 4)
        return false;
    out = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
    return true;
}

// Parses the unit-placement records that follow the tile grid in a .aem
// file: a building-color table (skipped -- not used by this rendering-only
// milestone) then a count-prefixed list of {encoded type+color, x, y}
// records (see aeii/MainDisplayable.java's loadMap(), the loop reading
// `fractionKings`/units after the building data). Populates the global
// `units`/`unitCount`. This is best-effort and must not fail the map load,
// since the terrain already loaded successfully: a failure before any
// per-unit record is read (missing/implausible header fields, a bad seek)
// leaves `unitCount` at 0; a failure partway through the record list (the
// file is truncated) logs a warning and keeps whatever complete records
// were already parsed rather than discarding them -- they're valid data,
// just fewer than the file's stated count.
void loadUnitPlacements(File &f, const char *path)
{
    unitCount = 0;

    uint32_t buildingColorCount;
    if (!readBE32(f, buildingColorCount) || buildingColorCount > 4096)
    {
        Serial.printf("map %s: no/implausible unit data, skipping\n", path);
        return;
    }
    if (!f.seek((uint32_t)buildingColorCount * 4, SeekCur))
    {
        Serial.printf("map %s: couldn't seek past building-color table\n", path);
        return;
    }

    uint32_t rawUnitCount;
    if (!readBE32(f, rawUnitCount))
    {
        Serial.printf("map %s: no unit count, skipping unit data\n", path);
        return;
    }

    int toLoad = (int)min(rawUnitCount, (uint32_t)MAX_UNITS);
    for (int i = 0; i < toLoad; ++i)
    {
        uint8_t rec[5];
        if (f.read(rec, 5) != 5)
        {
            Serial.printf("map %s: truncated unit records, loaded %d\n", path, unitCount);
            return;
        }
        uint8_t encoded = rec[0];
        int16_t xPx = (int16_t)(uint16_t((rec[1] << 8) | rec[2]));
        int16_t yPx = (int16_t)(uint16_t((rec[3] << 8) | rec[4]));
        uint8_t type = encoded % UNIT_TYPE_COUNT;
        uint8_t color = encoded / UNIT_TYPE_COUNT;
        if (color >= UNIT_COLOR_COUNT)
            continue; // outside the 4 team colors this milestone can render

        units[unitCount].type = type;
        units[unitCount].color = color;
        units[unitCount].tileX = xPx / TILE_SIZE;
        units[unitCount].tileY = yPx / TILE_SIZE;
        units[unitCount].hasMoved = false;
        units[unitCount].alive = true;
        units[unitCount].health = 100;
        ++unitCount;
    }

    Serial.printf("map %s: %d unit placements (of %lu in file)\n", path, unitCount, (unsigned long)rawUnitCount);
}

bool loadMap(const char *path)
{
    File f = openAsset(path);
    if (!f)
    {
        Serial.printf("map load failed: %s\n", path);
        return false;
    }

    // Matches aeii/MainDisplayable.java loadMap(): two big-endian int32
    // dimensions, then mapWidth*mapHeight tile-index bytes in column-major
    // (x outer, y inner) order. Building/unit data follows but this
    // milestone only renders terrain.
    uint8_t hdr[8];
    if (f.read(hdr, 8) != 8)
    {
        Serial.printf("map %s: truncated header\n", path);
        f.close();
        return false;
    }
    // Assemble as uint32_t first: for a malformed header, hdr[0]/hdr[4] can
    // be >= 0x80, and shifting that into bit 31 of a (promoted-to-)signed
    // int is undefined behavior before the sanity check below ever runs.
    uint32_t rawWidth = (uint32_t(hdr[0]) << 24) | (uint32_t(hdr[1]) << 16) | (uint32_t(hdr[2]) << 8) | hdr[3];
    uint32_t rawHeight = (uint32_t(hdr[4]) << 24) | (uint32_t(hdr[5]) << 16) | (uint32_t(hdr[6]) << 8) | hdr[7];

    // Sanity bounds: real AE2RM maps are well under 256x256 tiles; this also
    // keeps the width*height multiplication below from overflowing int, and
    // only narrows to int after rawWidth/rawHeight are known to fit.
    constexpr uint32_t MAX_MAP_DIM = 256;
    if (rawWidth == 0 || rawHeight == 0 || rawWidth > MAX_MAP_DIM || rawHeight > MAX_MAP_DIM)
    {
        Serial.printf("map %s: implausible dimensions %lux%lu\n", path, (unsigned long)rawWidth, (unsigned long)rawHeight);
        f.close();
        return false;
    }
    int width = int(rawWidth);
    int height = int(rawHeight);

    size_t tileDataSize = (size_t)width * (size_t)height;
    uint8_t *newTiles = static_cast<uint8_t *>(malloc(tileDataSize));
    if (!newTiles)
    {
        Serial.printf("map %s: out of memory allocating %u bytes\n", path, (unsigned)tileDataSize);
        f.close();
        return false;
    }

    if (f.read(newTiles, tileDataSize) != tileDataSize)
    {
        Serial.printf("map %s: truncated tile data\n", path);
        free(newTiles);
        f.close();
        return false;
    }

    free(mapTiles);
    mapTiles = newTiles;
    mapWidth = width;
    mapHeight = height;

    loadUnitPlacements(f, path); // best-effort; see its own comment
    f.close();

    Serial.printf("loaded map %s: %dx%d tiles\n", path, mapWidth, mapHeight);
    return true;
}

inline bool inMapBounds(int mx, int my)
{
    return mx >= 0 && my >= 0 && mx < mapWidth && my < mapHeight;
}

inline uint8_t tileAt(int mx, int my)
{
    return mapTiles[mx * mapHeight + my];
}

int unitIndexAt(int mx, int my)
{
    for (int i = 0; i < unitCount; ++i)
    {
        if (units[i].alive && units[i].tileX == mx && units[i].tileY == my)
            return i;
    }
    return -1;
}

inline int manhattanDist(int x1, int y1, int x2, int y2)
{
    return abs(x1 - x2) + abs(y1 - y2);
}

// Generalized over an explicit origin (not just attacker.tileX/tileY) so
// the AI can ask "could this unit attack from a hypothetical tile" while
// planning a move, not just from where it's currently standing.
bool inAttackRangeFrom(uint8_t type, int fromX, int fromY, int tx, int ty)
{
    int d = manhattanDist(fromX, fromY, tx, ty);
    return d >= UNIT_ATTACK_RANGE_MIN[type] && d <= UNIT_ATTACK_RANGE_MAX[type] && UNIT_ATTACK_RANGE_MAX[type] > 0;
}

bool inAttackRange(const UnitPlacement &attacker, int tx, int ty)
{
    return inAttackRangeFrom(attacker.type, attacker.tileX, attacker.tileY, tx, ty);
}

void drawViewport(); // defined below; playHitEffect() redraws between animation frames
void clampView();    // defined below; playHitEffect() re-centers the camera on off-screen combat
int ownedCastleCount(int color);              // defined below; used by the AI's recruit step
bool isShopDeployTile(int x, int y, int color); // defined below; likewise

// Scrolls so the *center* of map tile (tx, ty) sits at the center of the
// map viewport (the strip between the header and footer), then clamps.
inline void centerViewOnTile(int tx, int ty)
{
    viewX = tx * TILE_SIZE + TILE_SIZE / 2 - DISPLAY_WIDTH / 2;
    viewY = ty * TILE_SIZE + TILE_SIZE / 2 - MAP_VIEW_H / 2;
    clampView();
}

// Runs the last-side-standing check. A side is out once it has no living
// units -- either wiped out in combat, or routed the instant its king
// dies (resolveHit() clears every unit of that colour). That defeat is
// latched into eliminated[] here so it can't be undone by a later
// recruit. The mission ends once at most one side that started with
// units still has any; winnerColor is that side (or -1 if the last two
// fell in the same exchange -- a draw). A simplification of the
// original's castle-capture-tied defeat, see README. A no-op once
// gameOver is set; call after any combat exchange and after each turn
// resolves.
void checkEndConditions()
{
    if (gameOver)
        return;

    int alive[MAX_PLAYERS] = {0};
    for (int i = 0; i < unitCount; ++i)
        if (units[i].alive && units[i].color < MAX_PLAYERS)
            alive[units[i].color]++;

    int sidesStarted = 0, sidesLeft = 0, lastLeft = -1;
    for (int c = 0; c < MAX_PLAYERS; ++c)
    {
        if (startingUnits[c] <= 0)
            continue; // never in the fight -- e.g. m4/m6 have no red units
        ++sidesStarted;
        if (alive[c] > 0)
        {
            ++sidesLeft;
            lastLeft = c;
        }
        else
            eliminated[c] = true; // latched -- no coming back via recruitment
    }

    // The human losing its last unit is a terminal defeat -- the mission
    // ends here even with two or more AI sides still fighting. This is
    // deliberate, not the last-side-standing rule below: every remaining
    // side is AI, so there is nothing left for the player to do (they
    // can't act, and there's no spectator mode). winnerColor is the sole
    // survivor when exactly one AI side is left (banner: "<SIDE> WINS"),
    // otherwise -1 -- which drawViewport() renders as DEFEAT if any AI
    // side is still standing, or DRAW if none is.
    if (startingUnits[HUMAN_COLOR] > 0 && alive[HUMAN_COLOR] == 0)
    {
        gameOver = true;
        winnerColor = (sidesLeft == 1) ? lastLeft : -1;
        Serial.println("game over: human side eliminated -- defeat");
        return;
    }

    // Nothing to decide on a one-side sandbox map; otherwise the mission
    // ends once at most one side still has units.
    if (sidesStarted < 2 || sidesLeft > 1)
        return;

    gameOver = true;
    winnerColor = lastLeft; // -1 if every side was wiped out at once
    Serial.printf("game over: color %d wins (%d of %d sides eliminated)\n",
                  winnerColor, sidesStarted - sidesLeft, sidesStarted);
}

constexpr uint8_t TEMPLE_TILE = 34; // the one non-fraction "town" graphic that grants combat bonuses

// Ported from Unit.getOffenceBonusAgainstUnitEx(): flat offence added when
// this attacker/victim/attacker-tile combination matches a matchup rule.
//   - archer (property bit 6) vs a flyer (property bit 0, the wyvern): +15
//   - wisp (type 4) vs skeleton (type 10): +15
//   - lizard (property bit 1) attacking from a water tile: +10
//   - attacking from the temple tile (index 34): +25
int matchupOffenceBonus(uint8_t atkType, uint8_t vicType, int atkX, int atkY)
{
    int b = 0;
    if ((UNIT_PROPERTIES[atkType] & (1 << 6)) && (UNIT_PROPERTIES[vicType] & (1 << 0)))
        b += 15;
    if (atkType == 4 && vicType == 10)
        b += 15;
    uint8_t t = tileAt(atkX, atkY);
    if ((UNIT_PROPERTIES[atkType] & (1 << 1)) && t < TILE_COUNT && TILE_TERRAIN_TYPE[t] == 5)
        b += 10;
    if (t == TEMPLE_TILE)
        b += 25;
    return b;
}

// Ported from Unit.getDefenceBonusAgainstUnitEx(), MINUS the base
// TERRAIN_DEFENCE_BONUS[tile] term which resolveHit()/wouldGuaranteeKill()
// already apply. What's left:
//   - lizard (property bit 1) defending on a water tile: +15
//   - defending on the temple tile (index 34): +15
int matchupDefenceBonus(uint8_t vicType, int vicX, int vicY)
{
    int b = 0;
    uint8_t t = tileAt(vicX, vicY);
    if ((UNIT_PROPERTIES[vicType] & (1 << 1)) && t < TILE_COUNT && TILE_TERRAIN_TYPE[t] == 5)
        b += 15;
    if (t == TEMPLE_TILE)
        b += 15;
    return b;
}

// One hit: attackerIdx's roll in [offenceMin, offenceMax) plus any matchup
// offence bonus, against victimIdx's defence (base + terrain + matchup
// bonus), scaled by the attacker's current health%. Shared by
// attackUnit()'s direct hit and its counterattack. Returns the damage
// dealt, for attackUnit()'s hit-effect animation.
int resolveHit(int attackerIdx, int victimIdx)
{
    UnitPlacement &attacker = units[attackerIdx];
    UnitPlacement &victim = units[victimIdx];

    int offence = random(UNIT_OFFENCE_MIN[attacker.type], UNIT_OFFENCE_MAX[attacker.type]);
    offence += matchupOffenceBonus(attacker.type, victim.type, attacker.tileX, attacker.tileY);
    uint8_t victimTile = tileAt(victim.tileX, victim.tileY);
    // Same guard as computeReachable()'s: a corrupt/out-of-range tile index
    // shouldn't read past TILE_TERRAIN_TYPE/TERRAIN_DEFENCE_BONUS -- treat
    // it as no terrain bonus rather than crashing mid-combat.
    int terrainBonus = victimTile < TILE_COUNT ? TERRAIN_DEFENCE_BONUS[TILE_TERRAIN_TYPE[victimTile]] : 0;
    int defence = UNIT_DEFENCE[victim.type] + terrainBonus +
                  matchupDefenceBonus(victim.type, victim.tileX, victim.tileY);

    int hit = (offence - defence) * attacker.health / 100;
    hit = constrain(hit, 0, (int)victim.health);

    victim.health -= hit;
    Serial.printf("attack: unit %d (type %d) hits unit %d (type %d) for %d (hp now %d)\n",
                  attackerIdx, attacker.type, victimIdx, victim.type, hit, victim.health);

    if (victim.health == 0)
    {
        victim.alive = false;
        // A side whose king dies is routed -- the rest of its army leaves
        // the field. checkEndConditions() (run right after every combat
        // exchange) then declares the winner once one side is left. This
        // is a simplification of the original's castle-capture-tied
        // defeat, but generalises cleanly to 3-4 sides.
        if (victim.type == UNIT_TYPE_KING)
        {
            int routed = victim.color;
            for (int i = 0; i < unitCount; ++i)
                if (units[i].color == routed)
                    units[i].alive = false;
            Serial.printf("king of color %d killed -- side routed\n", routed);
        }
    }
    return hit;
}

// Plays the original's hit-flash effect (createSimpleSparkSprite() with
// sprRedSpark in MainDisplayable.java) over unitIdx's tile: a 6-frame
// spark animation (/effects/redspark_NN.bin, one file per frame -- see
// convert_assets.py's comment on why the sheet is split into per-frame
// files rather than converted as one image) with a "-N" damage label.
// Not ported: the original's damage label rises and fades over ~800ms;
// this just holds it static for the spark's duration, then lets the next
// drawViewport() clear it -- simpler, and this display has no alpha
// blending to fade it with anyway. Does nothing if a frame failed to
// load (e.g. missing from the SD card) -- this is cosmetic, not worth
// failing an attack over.
void playHitEffect(int unitIdx, int hit)
{
    const UnitPlacement &u = units[unitIdx];
    int px = tileScreenX(u.tileX);
    int py = tileScreenY(u.tileY);
    if (px <= -TILE_SIZE || px >= DISPLAY_WIDTH ||
        py <= MAP_VIEW_Y - TILE_SIZE || py >= MAP_VIEW_Y + MAP_VIEW_H)
    {
        // AI combat happens anywhere on the map -- runAITurn() never
        // moves the camera -- so this isn't a rare edge case; recenter
        // the viewport (the strip between header and footer) on the target
        // instead of silently skipping the effect, so every hit actually
        // gets one, not just combat that happened to already be in view.
        centerViewOnTile(u.tileX, u.tileY);
        px = tileScreenX(u.tileX);
        py = tileScreenY(u.tileY);
    }

    constexpr int SPARK_FRAME_W = 20, SPARK_FRAME_H = 20, SPARK_FRAME_COUNT = 6;
    constexpr size_t FRAME_PIXELS = (size_t)SPARK_FRAME_W * SPARK_FRAME_H;
    constexpr unsigned long FRAME_DELAY_MS = 60;

    // Loaded once and cached (in .bss, not PSRAM -- 2400 pixels total is
    // trivial) since combat calls this repeatedly; sheetLoaded latches
    // even on failure so a missing asset isn't re-read from SD every hit.
    static uint16_t sheet[SPARK_FRAME_COUNT * FRAME_PIXELS];
    static bool sheetLoaded = false, sheetOk = false;
    if (!sheetLoaded)
    {
        sheetLoaded = true;
        sheetOk = true;
        for (int frame = 0; frame < SPARK_FRAME_COUNT && sheetOk; ++frame)
        {
            char path[32];
            snprintf(path, sizeof(path), "/effects/redspark_%02d.bin", frame);
            File f = openAsset(path);
            sheetOk = f && f.read(reinterpret_cast<uint8_t *>(sheet + (size_t)frame * FRAME_PIXELS),
                                   FRAME_PIXELS * sizeof(uint16_t)) == FRAME_PIXELS * sizeof(uint16_t);
            if (f)
                f.close();
        }
    }

    char label[8];
    snprintf(label, sizeof(label), "-%d", hit);
    int sparkX = px + (TILE_SIZE - SPARK_FRAME_W) / 2;
    int sparkY = py + (TILE_SIZE - SPARK_FRAME_H) / 2;
    // Keep the damage label inside the map strip (a unit on the top row
    // would otherwise put it up in the header).
    int labelY = py >= MAP_VIEW_Y + 10 ? py - 10 : py + TILE_SIZE;

    for (int frame = 0; frame < SPARK_FRAME_COUNT; ++frame)
    {
        // A full redraw each frame, not just erasing the spark rect: pushImage()
        // only skips transparent source pixels, so without resetting the
        // background first, the previous frame's opaque pixels would smear
        // into the next.
        drawViewport();
        gfx.startWrite();
        gfx.setClipRect(0, MAP_VIEW_Y, DISPLAY_WIDTH, MAP_VIEW_H); // don't let the spark/label spill into the HUD
        if (sheetOk)
            gfx.pushImage(sparkX, sparkY, SPARK_FRAME_W, SPARK_FRAME_H,
                           sheet + (size_t)frame * FRAME_PIXELS, TRANSPARENT_565);
        gfx.setTextSize(1);
        gfx.fillRect(px, labelY, 26, 10, TFT_BLACK);
        gfx.setTextColor(TFT_WHITE, TFT_BLACK);
        gfx.setCursor(px + 1, labelY + 1);
        gfx.print(label);
        gfx.clearClipRect();
        gfx.endWrite();
        delay(FRAME_DELAY_MS);
    }
}

// Resolves attackerIdx attacking victimIdx, then victimIdx's counterattack
// if it's still alive and eligible -- matching Unit.java's
// canPerformCloseAttack(): adjacent (distance == 1, regardless of the
// attacker's own attack range) AND the victim's own MIN_ATTACK_RANGE is 1
// (a ranged-only unit like the catapult, MIN_ATTACK_RANGE 2, can never
// counter). Not ported: canPerformCloseAttack() also checks the victim's
// unitState != 4, a status-effect flag this milestone doesn't model.
// Each hit plays playHitEffect() where it lands, with a real pause before
// the counter -- so a human watching (or an AI turn resolving several
// attacks) sees the exchange happen rather than the whole thing landing
// in one silent redraw, matching the original's own paced attack/counter
// (createSimpleSparkSprite() + an ~800ms wait between them).
constexpr unsigned long COUNTERATTACK_PAUSE_MS = 300;
void attackUnit(int attackerIdx, int victimIdx)
{
    int hit = resolveHit(attackerIdx, victimIdx);
    playHitEffect(victimIdx, hit);

    UnitPlacement &attacker = units[attackerIdx];
    UnitPlacement &victim = units[victimIdx];
    if (victim.alive &&
        manhattanDist(victim.tileX, victim.tileY, attacker.tileX, attacker.tileY) == 1 &&
        UNIT_ATTACK_RANGE_MIN[victim.type] == 1)
    {
        delay(COUNTERATTACK_PAUSE_MS);
        int counterHit = resolveHit(victimIdx, attackerIdx);
        playHitEffect(attackerIdx, counterHit);
    }
    checkEndConditions();
}

// Flood-fills how far `u` could move this turn, terrain-cost-limited by
// UNIT_MOVE_RANGE, into reachableCost (allocated/resized here to match the
// current map). A tile occupied by any other unit is impassable -- this
// doesn't distinguish ally/enemy (the original lets a unit pass through
// allies), which is a simplification, not a port of its exact pathing.
// Uses relaxation-until-stable rather than a real priority queue: costs
// are tiny (1-3) and story maps are small, so this is cheap in practice
// and bounded (at most UNIT_MOVE_RANGE passes) even on a larger map.
void computeReachable(const UnitPlacement &u)
{
    size_t cells = (size_t)mapWidth * mapHeight;
    static size_t reachableCapacity = 0;
    if (reachableCapacity != cells)
    {
        free(reachableCost);
        reachableCost = static_cast<int8_t *>(malloc(cells));
        reachableCapacity = reachableCost ? cells : 0;
    }
    if (!reachableCost)
        return;

    memset(reachableCost, -1, cells);
    reachableCost[u.tileX * mapHeight + u.tileY] = UNIT_MOVE_RANGE[u.type];

    static const int DX[4] = {1, -1, 0, 0};
    static const int DY[4] = {0, 0, 1, -1};

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (int x = 0; x < mapWidth; ++x)
        {
            for (int y = 0; y < mapHeight; ++y)
            {
                int budget = reachableCost[x * mapHeight + y];
                if (budget < 0)
                    continue;
                for (int d = 0; d < 4; ++d)
                {
                    int nx = x + DX[d], ny = y + DY[d];
                    if (!inMapBounds(nx, ny))
                        continue;
                    int occupant = unitIndexAt(nx, ny);
                    if (occupant >= 0 && !(nx == u.tileX && ny == u.tileY))
                        continue;
                    uint8_t tile = tileAt(nx, ny);
                    if (tile >= TILE_COUNT)
                        continue; // treat a corrupt/out-of-range tile index as impassable
                    int cost = TERRAIN_MOVE_COST[TILE_TERRAIN_TYPE[tile]];
                    int remain = budget - cost;
                    if (remain < 0)
                        continue;
                    int8_t &slot = reachableCost[nx * mapHeight + ny];
                    if (remain > slot)
                    {
                        slot = (int8_t)remain;
                        changed = true;
                    }
                }
            }
        }
    }
}

void drawViewport();
void clampView(); // defined below, with the touch-drag handling it's shared with
bool saveGame();
bool loadGame();
bool hasSavedGame();
void toast(const char *msg);

// Loads /strings.dat (see convert_assets.py) into scriptStrings/
// scriptStringOffsets. Safe to call more than once -- a no-op if already
// loaded. Mission titles start at locale string index 121 (one per
// m0.aem..m7.aem, in order -- MainDisplayable.java's
// `getLocaleString(121 + mission)`), so this runs once at boot rather than
// only when m0's script needs it.
bool loadStrings()
{
    if (scriptStrings)
        return true;

    File f = openAsset("/strings.dat");
    if (!f)
    {
        Serial.println("strings.dat not found -- mission titles/dialog text unavailable");
        return false;
    }

    uint8_t hdr[4];
    if (f.read(hdr, 4) != 4)
    {
        Serial.println("strings.dat: truncated header");
        f.close();
        return false;
    }
    uint32_t count = (uint32_t(hdr[0]) << 24) | (uint32_t(hdr[1]) << 16) | (uint32_t(hdr[2]) << 8) | hdr[3];

    // The header count comes straight off the SD card -- untrusted. Every
    // record costs at least 2 bytes (a zero-length string's length
    // prefix), so a count that couldn't possibly fit in the file's
    // remaining bytes is corrupt; reject it before it's used to size the
    // offsets allocation (count+1 on a bogus multi-billion count would
    // overflow size_t on this 32-bit target and under-allocate, then the
    // read loop below would walk off the end of that buffer).
    size_t remaining = f.size() - 4;
    if (count > remaining / 2)
    {
        Serial.printf("strings.dat: implausible string count %u for a %u-byte file\n",
                       (unsigned)count, (unsigned)f.size());
        f.close();
        return false;
    }

    // Concatenated-with-NUL-terminators bytes can't exceed the file's
    // remaining size (each string costs len+1 bytes in the buffer vs.
    // len+2 in the file, i.e. one *less* byte per string) -- f.size() is a
    // safe, simple upper bound for the buffer, not a tight one.
    size_t bufCap = f.size();
    char *buf = static_cast<char *>(ps_malloc(bufCap));
    uint32_t *offsets = static_cast<uint32_t *>(malloc(sizeof(uint32_t) * (count + 1)));
    if (!buf || !offsets)
    {
        Serial.println("strings.dat: allocation failed");
        free(buf);
        free(offsets);
        f.close();
        return false;
    }

    size_t pos = 0;
    uint32_t i = 0;
    for (; i < count; ++i)
    {
        uint8_t lenBytes[2];
        if (f.read(lenBytes, 2) != 2)
            break;
        uint16_t len = (uint16_t(lenBytes[0]) << 8) | lenBytes[1];
        offsets[i] = (uint32_t)pos;
        if (len > 0 && f.read(reinterpret_cast<uint8_t *>(buf + pos), len) != len)
            break;
        pos += len;
        buf[pos++] = '\0';
    }
    f.close();

    if (i < count)
    {
        // A partial table would make getScriptString() return "" for any
        // index past the truncation point -- ShowDialog would render an
        // empty box instead of the "cutscene skipped" fallback this
        // function's callers actually expect on a load failure. Reject
        // the whole table rather than serving a silently incomplete one.
        Serial.printf("strings.dat: truncated after %u/%u strings, rejecting table\n", (unsigned)i, (unsigned)count);
        free(buf);
        free(offsets);
        return false;
    }
    offsets[count] = (uint32_t)pos; // one-past-the-last string's end

    scriptStrings = buf;
    scriptStringOffsets = offsets;
    scriptStringCount = (int)count;
    Serial.printf("loaded %d locale strings (%u bytes)\n", scriptStringCount, (unsigned)pos);
    return true;
}

// "" for an out-of-range index or an unloaded table, never nullptr --
// callers (mission menu labels, dialog rendering) can gfx.print() the
// result unconditionally.
const char *getScriptString(int id)
{
    if (!scriptStrings || id < 0 || id >= scriptStringCount)
        return "";
    return scriptStrings + scriptStringOffsets[id];
}

// Locale string 121+i is story mission i's real title (used by both the
// mission menu and showMissionBriefing()); 129+i is its objective text
// (see showMissionBriefing()) -- MainDisplayable.java's
// getLocaleString(121 + mission)/getSaveInfoString(). 101+i is skirmish
// map i's name (getLocaleString(101 + var5)).
constexpr int MISSION_TITLE_STRING_BASE = 121;
constexpr int SKIRMISH_TITLE_STRING_BASE = 101;

// The mission menu has two tabs -- the 8 story missions and the 12
// skirmish maps -- because 20 rows don't fit one screen. menuTab
// persists across returns to the menu.
enum { MENU_TAB_STORY, MENU_TAB_SKIRMISH };
static int menuTab = MENU_TAB_STORY;

constexpr int MENU_TAB_Y = 28;
constexpr int MENU_TAB_H = 16;
constexpr int MENU_ROW_TOP = 48;

// Skirmish tab packs 12 rows, so its rows are shorter and single-size;
// the story tab keeps its roomier two-size rows.
inline int menuRowCount() { return menuTab == MENU_TAB_SKIRMISH ? SKIRMISH_MAP_COUNT : STORY_MAP_COUNT; }
inline int menuRowH() { return menuTab == MENU_TAB_SKIRMISH ? 22 : 28; }

void drawMenu()
{
    gfx.startWrite();
    gfx.fillScreen(TFT_BLACK);
    gfx.setTextSize(2);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setCursor(20, 6);
    gfx.print("AE2RM");

    // Tabs.
    const char *tabLabel[2] = {"STORY", "SKIRMISH"};
    int tabW = (DISPLAY_WIDTH - 40) / 2;
    for (int t = 0; t < 2; ++t)
    {
        int tx = 20 + t * tabW;
        bool active = (t == menuTab);
        gfx.fillRect(tx, MENU_TAB_Y, tabW - 4, MENU_TAB_H, active ? TFT_WHITE : TFT_DARKGREY);
        gfx.setTextSize(1);
        gfx.setTextColor(active ? TFT_BLACK : TFT_WHITE, active ? TFT_WHITE : TFT_DARKGREY);
        gfx.setCursor(tx + 8, MENU_TAB_Y + 4);
        gfx.print(tabLabel[t]);
    }

    // loadStrings() is called once from setup(), so this is just a lookup,
    // not a fresh SD read per row. A missing/failed-to-load strings.dat
    // falls back to a generic label instead of an empty row.
    bool skirmish = (menuTab == MENU_TAB_SKIRMISH);
    int rowH = menuRowH();
    int textSize = skirmish ? 1 : 2;
    int base = skirmish ? SKIRMISH_TITLE_STRING_BASE : MISSION_TITLE_STRING_BASE;
    for (int i = 0; i < menuRowCount(); ++i)
    {
        int rowY = MENU_ROW_TOP + i * rowH;
        gfx.fillRect(20, rowY, DISPLAY_WIDTH - 40, rowH - 6, TFT_DARKGREY);
        gfx.drawRect(20, rowY, DISPLAY_WIDTH - 40, rowH - 6, TFT_WHITE);
        gfx.setTextSize(textSize);
        gfx.setTextColor(TFT_WHITE, TFT_DARKGREY);
        gfx.setCursor(30, rowY + (skirmish ? 4 : 6));
        const char *title = getScriptString(base + i);
        if (title[0])
            gfx.print(title);
        else
            gfx.printf(skirmish ? "Skirmish %d" : "Mission %d", i + 1);
    }
    gfx.endWrite();
}

// Reads one '\n'-terminated line from f into buf (stripping a trailing
// '\r' and the newline itself), NUL-terminated. Returns false only at EOF
// with nothing left to read -- a line longer than bufSize is silently
// truncated (every m0.script line is well under 96 bytes).
bool readScriptLine(File &f, char *buf, size_t bufSize)
{
    if (!f.available())
        return false;
    size_t n = 0;
    while (f.available() && n + 1 < bufSize)
    {
        int c = f.read();
        if (c < 0 || c == '\n')
            break;
        if (c != '\r')
            buf[n++] = (char)c;
    }
    buf[n] = '\0';
    return true;
}

// Greedy word-wrap of `text` into gfx.print() lines starting at (x,y),
// gfx already at setTextSize(1) -- good enough for this game's short
// English dialog/objective strings, not general typesetting. A single
// word longer than maxChars (doesn't happen in this game's text) just
// gets hard-split. Stops after maxRows lines even if text remains.
// Shared by showScriptDialog() and showMissionBriefing() so the wrap
// logic exists once.
void drawWrappedText(int x, int y, int maxChars, int maxRows, int lineH, const char *text)
{
    int row = 0;
    const char *p = text;
    char lineBuf[64];
    while (*p && row < maxRows)
    {
        int lineLen = 0;
        const char *lastSpace = nullptr;
        const char *scan = p;
        int cap = maxChars < (int)sizeof(lineBuf) - 1 ? maxChars : (int)sizeof(lineBuf) - 1;
        while (*scan && *scan != '\n' && lineLen < cap)
        {
            if (*scan == ' ')
                lastSpace = scan;
            lineBuf[lineLen++] = *scan;
            ++scan;
        }
        if (*scan && *scan != '\n' && lastSpace)
        {
            lineLen = (int)(lastSpace - p);
            scan = lastSpace + 1;
        }
        lineBuf[lineLen] = '\0';
        gfx.setCursor(x, y + row * lineH);
        gfx.print(lineBuf);
        ++row;
        p = (*scan == '\n') ? scan + 1 : scan;
        while (*p == ' ')
            ++p;
    }
}

// Blocks until the player taps and releases the screen. ArduinoOTA.handle()
// is normally only serviced from loop() -- this blocking wait has to call
// it itself, or leaving a modal screen open would make the board
// unreachable over OTA for as long as the player doesn't tap. Bailing out
// on otaInProgress (rather than continuing to poll touch) matches loop()'s
// own rule of leaving the display/SD alone once a flash write has started.
// Shared by showScriptDialog() and showMissionBriefing().
void waitForTapRelease()
{
    bool wasDown = false;
    for (;;)
    {
        ArduinoOTA.handle();
        if (otaInProgress)
            break;
        int32_t x, y;
        bool down = gfx.getTouch(&x, &y);
        if (!down && wasDown)
            break;
        wasDown = down;
        delay(16);
    }
}

// Renders a bottom dialog box with word-wrapped `text` and blocks until the
// player taps (and releases) the screen. Called synchronously from
// runIntroScript(), which runs before startGame() hands control back to
// loop() -- this is the only way to pace the cutscene on input without a
// per-frame script state machine (out of scope for this milestone; see
// runIntroScript()'s comment).
void showScriptDialog(const char *text)
{
    constexpr int BOX_H = 70;
    constexpr int BOX_Y = DISPLAY_HEIGHT - BOX_H;
    constexpr int PAD = 6;
    constexpr int CHAR_W = 6; // gfx default font at setTextSize(1)
    constexpr int LINE_H = 10;
    constexpr int MAX_ROWS = (BOX_H - PAD - LINE_H - 4) / LINE_H; // leaves room for the prompt line
    const int maxChars = (DISPLAY_WIDTH - 2 * PAD) / CHAR_W;

    gfx.startWrite();
    gfx.fillRect(0, BOX_Y, DISPLAY_WIDTH, BOX_H, TFT_BLACK);
    gfx.drawRect(0, BOX_Y, DISPLAY_WIDTH, BOX_H, TFT_WHITE);
    gfx.setTextSize(1);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    drawWrappedText(PAD, BOX_Y + PAD, maxChars, MAX_ROWS, LINE_H, text);

    gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
    gfx.setCursor(PAD, BOX_Y + BOX_H - LINE_H - 2);
    gfx.print("[tap to continue]");
    gfx.endWrite();

    waitForTapRelease();
}

// A one-time, full-screen "mission briefing" shown right after a map loads
// (all 8 story maps, not just m0 -- see startGame()): the mission's real
// title (locale string 121+mapIndex, same as the mission menu row) and its
// objective text (locale string 129+mapIndex -- getSaveInfoString()'s
// sibling table in the original, one entry per story map, discovered
// while wiring up mission titles in an earlier milestone but not read
// until now). Blocks on tap-to-start, same as a script dialog. Skipped
// entirely if strings.dat isn't loaded -- there's nothing useful to show,
// and jumping straight into gameplay is a safe, already-established
// fallback (see loadStrings()'s comment).
constexpr int MISSION_OBJECTIVE_STRING_BASE = 129;
void showMissionBriefing(int mapIndex)
{
    if (!scriptStrings)
        return;

    const char *title = getScriptString(MISSION_TITLE_STRING_BASE + mapIndex);
    const char *objective = getScriptString(MISSION_OBJECTIVE_STRING_BASE + mapIndex);
    if (!title[0] && !objective[0])
        return; // table loaded but neither string present -- nothing to show

    constexpr int PAD = 10;
    constexpr int CHAR_W = 6;  // gfx default font at setTextSize(1)
    constexpr int LINE_H = 10;
    constexpr int TITLE_Y = 40;
    constexpr int OBJECTIVE_Y = TITLE_Y + 30;
    constexpr int MAX_ROWS = 6;
    const int maxChars = (DISPLAY_WIDTH - 2 * PAD) / CHAR_W;

    gfx.startWrite();
    gfx.fillScreen(TFT_BLACK);
    gfx.setTextSize(2);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setCursor(PAD, TITLE_Y);
    gfx.print(title);

    gfx.setTextSize(1);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    drawWrappedText(PAD, OBJECTIVE_Y, maxChars, MAX_ROWS, LINE_H, objective);

    gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
    gfx.setCursor(PAD, DISPLAY_HEIGHT - LINE_H - PAD);
    gfx.print("[tap to start]");
    gfx.endWrite();

    waitForTapRelease();
}

// The skirmish equivalent of showMissionBriefing(): skirmish maps have no
// per-map objective text (the original shows a generic "defeat all
// enemies" card, locale strings 71/137), so this just names the map and
// the side count. Blocks on tap-to-start.
void showSkirmishBriefing(int mapIndex)
{
    const char *name = scriptStrings ? getScriptString(SKIRMISH_TITLE_STRING_BASE + mapIndex) : "";
    gfx.startWrite();
    gfx.fillScreen(TFT_BLACK);
    gfx.setTextSize(2);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setCursor(10, 40);
    gfx.printf("SKIRMISH %d", mapIndex + 1);
    gfx.setTextSize(1);
    if (name[0])
    {
        gfx.setCursor(10, 66);
        gfx.print(name);
    }
    gfx.setCursor(10, 90);
    gfx.printf("%d sides -- you are %s", turnQueueLen, PLAYER_NAME[HUMAN_COLOR]);
    gfx.setCursor(10, 104);
    gfx.print("last side standing wins");
    gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
    gfx.setCursor(10, DISPLAY_HEIGHT - 20);
    gfx.print("[tap to start]");
    gfx.endWrite();
    waitForTapRelease();
}

// A one-time title screen shown at boot, before the mission menu: the
// original's splash.png background with logo.png composited over it,
// tap to continue. Not the original's actual title screen: that's a
// multi-stage alpha-fade transition (a small studio logo fades in and
// out, then the game logo fades in over black, then the splash
// background fades in behind it with its own glow effect --
// updateIntroTransition() in MainDisplayable.java) driven by a
// combatDrapValue counter this port has no equivalent alpha-blending
// pipeline for. This shows the two images statically instead of
// animating the transition between them -- a real simplification, not
// silently dropped. ms_logo.png (the studio splash) isn't shown at all.
// Skipped entirely (straight into the menu) if either asset is missing,
// same graceful-degradation pattern as strings.dat/m0.script.
void showTitleScreen()
{
    constexpr size_t SPLASH_PIXELS = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT;
    constexpr int LOGO_H = 85;
    constexpr size_t LOGO_PIXELS = (size_t)DISPLAY_WIDTH * LOGO_H;
    constexpr int LOGO_Y = 20; // approximate placement -- see this function's comment

    uint16_t *splash = static_cast<uint16_t *>(ps_malloc(SPLASH_PIXELS * sizeof(uint16_t)));
    uint16_t *logo = static_cast<uint16_t *>(ps_malloc(LOGO_PIXELS * sizeof(uint16_t)));
    bool ok = splash && logo;

    if (ok)
    {
        File f = openAsset("/title/splash.bin");
        ok = f && f.read(reinterpret_cast<uint8_t *>(splash), SPLASH_PIXELS * sizeof(uint16_t)) == SPLASH_PIXELS * sizeof(uint16_t);
        if (f)
            f.close();
    }
    if (ok)
    {
        File f = openAsset("/title/logo.bin");
        ok = f && f.read(reinterpret_cast<uint8_t *>(logo), LOGO_PIXELS * sizeof(uint16_t)) == LOGO_PIXELS * sizeof(uint16_t);
        if (f)
            f.close();
    }

    if (ok)
    {
        gfx.startWrite();
        gfx.fillScreen(TFT_BLACK);
        gfx.pushImage(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, splash, TRANSPARENT_565);
        gfx.pushImage(0, LOGO_Y, DISPLAY_WIDTH, LOGO_H, logo, TRANSPARENT_565);
        gfx.setTextSize(1);
        gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
        gfx.fillRect(0, DISPLAY_HEIGHT - 12, DISPLAY_WIDTH, 12, TFT_BLACK);
        gfx.setCursor(4, DISPLAY_HEIGHT - 10);
        gfx.print("[tap to continue]");
        gfx.endWrite();
        waitForTapRelease();
    }
    else
    {
        Serial.println("title screen assets missing/failed to load, skipping");
    }

    free(splash);
    free(logo);
}

// m0's scripted sprite effects (CreateSpriteAtUnit) are non-blocking in the
// original: showSpriteOnMap()/createSimpleSparkSprite() append the sprite to
// an active-effects vector and return immediately -- the effect then ticks
// forward on its own every ~50ms alongside whatever the script does next,
// so several effects (e.g. m0's Spark + Smoke on the same casualty) run
// concurrently, and the following Wait is what actually paces the script,
// not the effect's own duration (verified against Sprite.update()'s default
// case in Sprite.java: setPosition(currentX + shiftX, currentY + shiftY)
// every tick, and nextFrame()/bounceMode decrementing only once per full
// frame-sequence wrap). A small fixed slot table plus tickCutsceneEffects(),
// called from runIntroScript()'s Wait handling below, reproduces that
// instead of blocking synchronously inside CreateSpriteAtUnit itself.
constexpr int MAX_CUTSCENE_EFFECTS = 4;
constexpr unsigned long CUTSCENE_EFFECT_TICK_MS = 50; // matches Sprite.update()'s own cadence
struct CutsceneEffect
{
    bool active = false;
    const char *prefix = nullptr;
    int frameW = 0, frameH = 0, frameCount = 0;
    int currentFrame = 0;
    int px = 0, py = 0;         // current top-left pixel position
    int sxPerTick = 0, syPerTick = 0; // added to px/py every tick -- a motion delta, not a one-time offset
    unsigned long frameDelayMs = 0;
    unsigned long delayAccumulator = 0;
    int repeatsRemaining = 0; // bounceMode from the script: number of full frame-sequence loops before stopping
};
CutsceneEffect cutsceneEffects[MAX_CUTSCENE_EFFECTS];

// Queues a numbered sequence of RGB565 frame files (/effects/<prefix>_NN.bin,
// frameCount files of frameW x frameH each -- see convert_assets.py) to play
// starting at (centerPx, centerPy), stepping by (sxPerTick, syPerTick) each
// tick, for repeatCount full loops through its frameCount frames. Returns
// immediately; tickCutsceneEffects() below drives it forward. Silently
// dropped if every slot is already in use -- m0 never has more than two
// effects active at once (Spark + Smoke on the same casualty), so this
// should never actually happen.
void spawnCutsceneSpriteEffect(int centerPx, int centerPy, const char *prefix, int frameW, int frameH, int frameCount,
                                int sxPerTick, int syPerTick, int repeatCount, unsigned long frameDelayMs)
{
    for (CutsceneEffect &e : cutsceneEffects)
    {
        if (e.active)
            continue;
        e.active = true;
        e.prefix = prefix;
        e.frameW = frameW;
        e.frameH = frameH;
        e.frameCount = frameCount;
        e.currentFrame = 0;
        e.px = centerPx - frameW / 2;
        e.py = centerPy - frameH / 2;
        e.sxPerTick = sxPerTick;
        e.syPerTick = syPerTick;
        e.frameDelayMs = frameDelayMs;
        e.delayAccumulator = 0;
        e.repeatsRemaining = repeatCount;
        return;
    }
}

bool anyCutsceneEffectActive()
{
    for (const CutsceneEffect &e : cutsceneEffects)
        if (e.active)
            return true;
    return false;
}

// Advances every active effect by one tick (elapsedMs, expected to be
// CUTSCENE_EFFECT_TICK_MS) and redraws the viewport plus whatever's still
// active on top of it. Called only from runIntroScript()'s Wait handling,
// which is the only place m0's script leaves idle time for effects to run
// in -- so, unlike the original's real-time loop, an effect started right
// before a ShowDialog/GetUnit/etc. with no following Wait won't animate
// until the next Wait tick. Always redraws when called (even once every
// effect has just deactivated) so the last frame doesn't linger on screen
// through the rest of the wait -- pushImage()'s transparent-skip only
// omits source pixels, not the previous frame's opaque ones.
void tickCutsceneEffects(unsigned long elapsedMs)
{
    for (CutsceneEffect &e : cutsceneEffects)
    {
        if (!e.active)
            continue;
        e.px += e.sxPerTick;
        e.py += e.syPerTick;
        e.delayAccumulator += elapsedMs;
        if (e.delayAccumulator >= e.frameDelayMs)
        {
            e.delayAccumulator -= e.frameDelayMs;
            e.currentFrame = (e.currentFrame + 1) % e.frameCount;
            if (e.currentFrame == 0 && --e.repeatsRemaining <= 0)
                e.active = false;
        }
    }

    drawViewport();
    uint16_t *frame = nullptr;
    size_t frameCap = 0;
    for (const CutsceneEffect &e : cutsceneEffects)
    {
        if (!e.active)
            continue;
        size_t frameBytes = (size_t)e.frameW * e.frameH * sizeof(uint16_t);
        if (frameBytes > frameCap)
        {
            free(frame);
            frame = static_cast<uint16_t *>(malloc(frameBytes));
            frameCap = frame ? frameBytes : 0;
        }
        if (!frame)
            continue;
        char path[40];
        snprintf(path, sizeof(path), "/effects/%s_%02d.bin", e.prefix, e.currentFrame);
        File f = openAsset(path); // SD or the embedded copy -- works card-less
        bool ok = f && f.read(reinterpret_cast<uint8_t *>(frame), frameBytes) == frameBytes;
        if (f)
            f.close();
        if (ok)
        {
            gfx.startWrite();
            gfx.setClipRect(0, MAP_VIEW_Y, DISPLAY_WIDTH, MAP_VIEW_H); // stay out of the HUD bands
            gfx.pushImage(e.px, e.py, e.frameW, e.frameH, frame, TRANSPARENT_565);
            gfx.clearClipRect();
            gfx.endWrite();
        }
    }
    free(frame);
}

// Interprets a hand-picked subset of m0.script -- the intro cutscene,
// @Case 0 through @Case 13 (ending at "StartPlay") -- and stops there.
// @Case 14 onward drives the rest of the mission's tutorial hints
// (Test-conditioned ShowHelp overlays checked against live game state:
// CurrentTurn, UnitFinishedMove, CountUnits, etc.) and the ending dialog
// (Test-gated on CountUnits/GameState, ending in CompleteMission) -- both
// need a real per-frame condition-evaluating state machine hooked into
// ongoing play, not the one-shot linear pass this function does. That's a
// materially bigger feature, left for a future milestone; only m0's story
// intro ships here.
//
// Runs synchronously (blocking) right after startGame() first draws the
// map -- not the original's real-time script VM, which runs the *whole*
// mission this way, this port doesn't attempt. Commands with no equivalent
// in this port (no fade/cursor-sprite system, no per-tile movement
// animation to pace) are silently skipped, not simulated: ShowMapName,
// NextState, SetFadeEnabled, SetFadeValue, SetCursorVisible,
// SetMapStepMax, SetUnitSpeed, Vibrate, ScheduleUnitAnimationStop,
// StartPlay.
void runIntroScript()
{
    // 224 is the highest ShowDialog string index this case range uses
    // (see @Case 5-12 below) -- a structurally valid but short/empty
    // table (e.g. a 4-byte strings.dat with count == 0) would otherwise
    // pass loadStrings() and still leave getScriptString() returning ""
    // for these IDs, running the cutscene's unit moves/removal but
    // showing blank dialog boxes instead of the documented skip.
    constexpr int LAST_INTRO_DIALOG_STRING = 224;
    if (!loadStrings() || scriptStringCount <= LAST_INTRO_DIALOG_STRING)
        return; // dialog text unavailable/incomplete -- skip the cutscene, not show blank boxes

    File f = openAsset("/scripts/m0.script");
    if (!f)
    {
        Serial.println("m0.script not found, skipping intro cutscene");
        return;
    }

    int scriptUnit = -1; // last unit named by GetUnit, for a following RemoveUnit
    char line[96];
    while (readScriptLine(f, line, sizeof(line)))
    {
        // showScriptDialog() already bails out of its own wait on
        // otaInProgress; check again here so a Wait/camera-pan/etc.
        // between dialogs doesn't keep touching the display/SD once a
        // flash write has actually started.
        if (otaInProgress)
            break;
        if (line[0] == '\0')
            continue;
        if (!strncmp(line, "@Case", 5))
        {
            // "@Case 14" is where the Test-driven tutorial/epilogue this
            // milestone doesn't port begins -- see the comment above.
            if (!strcmp(line, "@Case 14"))
                break;
            continue;
        }

        char *saveptr = nullptr;
        char *tok[8] = {};
        int n = 0;
        for (char *t = strtok_r(line, " ", &saveptr); t && n < 8; t = strtok_r(nullptr, " ", &saveptr))
            tok[n++] = t;
        if (n == 0)
            continue;

        if (!strcmp(tok[0], "GetUnitPlotRoute") && n >= 6)
        {
            // Args are (sx, sy, color, ex, ey, animate) -- matches
            // getUnit(x, y, color).plotRoute(ex, ey, animate) in the
            // original, NOT (color, sx, sy, ex, ey); getting this order
            // wrong makes every lookup below miss (verified against
            // m0.aem's actual starting positions while implementing this).
            // The original animates the route (the trailing bool selects
            // animated vs. instant); this port always teleports.
            int sx = atoi(tok[1]), sy = atoi(tok[2]);
            int color = atoi(tok[3]);
            int ex = atoi(tok[4]), ey = atoi(tok[5]);
            int idx = unitIndexAt(sx, sy);
            if (idx >= 0 && units[idx].color == color)
            {
                units[idx].tileX = (int16_t)ex;
                units[idx].tileY = (int16_t)ey;
            }
        }
        else if (!strcmp(tok[0], "MoveMapAndCursor") && n >= 2)
        {
            int tx, ty;
            if (!strcmp(tok[1], "king") && n >= 3)
            {
                int color = atoi(tok[2]);
                int kingIdx = -1;
                for (int i = 0; i < unitCount; ++i)
                {
                    if (units[i].alive && units[i].type == UNIT_TYPE_KING && units[i].color == color)
                    {
                        kingIdx = i;
                        break;
                    }
                }
                if (kingIdx < 0)
                    continue;
                tx = units[kingIdx].tileX;
                ty = units[kingIdx].tileY;
            }
            else if (n >= 3)
            {
                tx = atoi(tok[1]);
                ty = atoi(tok[2]);
            }
            else
            {
                continue;
            }
            // Center the viewport on the target tile -- a jump cut, not
            // the original's smooth pan (no per-frame animation loop to
            // pace it against here).
            centerViewOnTile(tx, ty);
            drawViewport();
        }
        else if (!strcmp(tok[0], "GetUnit") && n >= 3)
        {
            scriptUnit = unitIndexAt(atoi(tok[1]), atoi(tok[2]));
        }
        else if (!strcmp(tok[0], "RemoveUnit"))
        {
            if (scriptUnit >= 0)
                units[scriptUnit].alive = false;
        }
        else if (!strcmp(tok[0], "CreateSpriteAtUnit") && n >= 6 && scriptUnit >= 0)
        {
            // Args are (name, sx, sy, bounceMode, delay) --
            // createSimpleSparkSprite(sprite, unit.currentX, unit.currentY,
            // sx, sy, bounceMode, delay) in the original. sx/sy are a
            // per-tick motion delta (Sprite.update()'s default case adds
            // shiftX/shiftY to the sprite's position every ~50ms, not just
            // once -- e.g. m0's Smoke uses (0, -3) and rises throughout).
            // bounceMode is a repeat count, not an animation variant: it's
            // decremented each time the frame sequence wraps back to 0 and
            // the effect stops once it hits zero (m0's RedSpark passes 2,
            // so it loops twice). Spawned non-blocking -- see
            // spawnCutsceneSpriteEffect()/tickCutsceneEffects() above --
            // and actually animated by the following Wait command below.
            const char *prefix = nullptr;
            int frameW = 0, frameH = 0, frameCount = 0;
            if (!strcmp(tok[1], "RedSpark"))
            {
                prefix = "redspark";
                frameW = frameH = 20;
                frameCount = 6;
            }
            else if (!strcmp(tok[1], "Spark"))
            {
                prefix = "spark";
                frameW = frameH = 24;
                frameCount = 6;
            }
            else if (!strcmp(tok[1], "Smoke"))
            {
                prefix = "smoke";
                frameW = 24;
                frameH = 20;
                frameCount = 4;
            }
            if (prefix)
            {
                int sxPerTick = atoi(tok[2]), syPerTick = atoi(tok[3]);
                int repeatCount = atoi(tok[4]);
                unsigned long frameDelay = (unsigned long)atoi(tok[5]);
                int centerPx = tileScreenX(units[scriptUnit].tileX) + TILE_SIZE / 2;
                int centerPy = tileScreenY(units[scriptUnit].tileY) + TILE_SIZE / 2;
                spawnCutsceneSpriteEffect(centerPx, centerPy, prefix, frameW, frameH, frameCount,
                                           sxPerTick, syPerTick, repeatCount, frameDelay);
            }
        }
        else if (!strcmp(tok[0], "ShowDialog") && n >= 2)
        {
            drawViewport(); // make sure what's on screen is current before covering part of it
            showScriptDialog(getScriptString(atoi(tok[1])));
        }
        else if (!strcmp(tok[0], "Wait") && n >= 2)
        {
            // The original paces this against its own tick rate; there's
            // no equivalent tick here, so this is a plain approximate
            // delay, not a faithful conversion of "N ticks" -- except that
            // any CreateSpriteAtUnit effects queued above are ticked and
            // redrawn during it, so a wait long enough to cover an
            // effect's duration is what actually plays it, same as the
            // original's real-time loop advancing the sprite regardless of
            // the script.
            constexpr unsigned long MS_PER_WAIT_TICK = 80;
            unsigned long totalMs = (unsigned long)atoi(tok[1]) * MS_PER_WAIT_TICK;
            unsigned long elapsed = 0;
            while (elapsed < totalMs)
            {
                unsigned long step = min(CUTSCENE_EFFECT_TICK_MS, totalMs - elapsed);
                delay(step);
                elapsed += step;
                if (anyCutsceneEffectActive())
                    tickCutsceneEffects(step);
            }
        }
        // Every other command in this case range (ShowMapName, NextState,
        // SetFadeEnabled, SetFadeValue, SetCursorVisible, SetMapStepMax,
        // SetUnitSpeed, Vibrate, ScheduleUnitAnimationStop, StartPlay) has
        // no equivalent here -- see this function's doc comment -- and is
        // silently skipped.
    }
    f.close();
    if (!otaInProgress)
        drawViewport();
}

// (Re)counts each side's living units into startingUnits[] -- the roster
// checkEndConditions() measures defeat against, and the source of the
// footer's per-side tally. Called once before the first mission render
// and again after m0's intro script (which repositions/removes units).
void recountStartingUnits()
{
    for (int c = 0; c < MAX_PLAYERS; ++c)
        startingUnits[c] = 0;
    for (int i = 0; i < unitCount; ++i)
        if (units[i].alive && units[i].color < MAX_PLAYERS)
            startingUnits[units[i].color]++;
}

// Turns a freshly-loaded skirmish map into a playable N-side game, the
// way MainDisplayable.loadLevel() does for skirmishMode == 1:
//   - Walk the tiles in column-major order; each time a castle belonging
//     to a fraction (1-4) not seen yet appends that fraction to the turn
//     queue. Queue position becomes that side's colour, so turnQueue[]
//     ends up the identity {0,1,..} and the colour<->fraction mapping is
//     what actually varies per map.
//   - Rewrite every fraction building's encoded owner from its raw
//     fraction to that fraction's queue position (+1), or to neutral if
//     the fraction holds no castle (so it never took a queue slot).
//   - Remap each unit's colour the same way; drop units whose fraction
//     isn't in the queue.
// Simplifications vs. the original (which has a full pre-match setup
// screen): colour 0 -- the first castle found in the scan -- is always
// the human, the rest are AI; there's no team grouping, unit-type cap,
// or configurable start gold. Returns false if fewer than two fractions
// hold a castle (not a valid skirmish map).
bool buildSkirmishTurnQueue()
{
    int posOfFraction[5]; // fraction 1..4 -> queue position, -1 if absent
    for (int i = 0; i < 5; ++i)
        posOfFraction[i] = -1;
    turnQueueLen = 0;

    for (int x = 0; x < mapWidth; ++x)
        for (int y = 0; y < mapHeight; ++y)
        {
            uint8_t t = mapTiles[x * mapHeight + y];
            if (!isFractionBuilding(t) || TILE_TERRAIN_TYPE[t] != 9) // castle only
                continue;
            int frac = buildingFraction(t); // 0 neutral, 1-4 team
            if (frac <= 0 || frac > 4 || posOfFraction[frac] >= 0)
                continue;
            if (turnQueueLen >= MAX_PLAYERS)
                continue; // >4 castle-holding sides: unsupported, ignore the rest
            posOfFraction[frac] = turnQueueLen;
            turnQueue[turnQueueLen] = (uint8_t)turnQueueLen;
            ++turnQueueLen;
        }

    if (turnQueueLen < 2)
    {
        Serial.printf("skirmish: only %d castle-holding side(s) -- not playable\n", turnQueueLen);
        return false;
    }

    // Remap building ownership: raw fraction -> queue position (+1), or
    // neutral for a fraction with no castle.
    for (int i = 0; i < mapWidth * mapHeight; ++i)
    {
        uint8_t t = mapTiles[i];
        if (!isFractionBuilding(t))
            continue;
        int frac = buildingFraction(t);
        if (frac <= 0)
            continue; // already neutral
        int pos = (frac <= 4) ? posOfFraction[frac] : -1;
        mapTiles[i] = setBuildingFraction(t, pos < 0 ? 0 : pos + 1);
    }

    // Remap unit colours the same way; drop units of a fraction that
    // never took a queue slot.
    int w = 0;
    for (int r = 0; r < unitCount; ++r)
    {
        int frac = units[r].color + 1; // raw colour slot 0-3 -> fraction 1-4
        int pos = (frac >= 1 && frac <= 4) ? posOfFraction[frac] : -1;
        if (pos < 0)
            continue;
        units[w] = units[r];
        units[w].color = (uint8_t)pos;
        ++w;
    }
    unitCount = w;

    Serial.printf("skirmish: %d sides, %d units\n", turnQueueLen, unitCount);
    return true;
}

// Loads m<mapIndex>.aem (or s<mapIndex>.aem when `skirmish`) and resets
// all per-game state, then switches to STATE_PLAYING. Asset caches
// (tiles/unit icons) are content-independent across maps and are
// deliberately NOT reset here. Returns false (and bounces to the mission
// menu) if the map can't be loaded, or a skirmish map has fewer than two
// castle-holding sides.
//
// interactive: normal mission start -- show the full-screen briefing and,
// on m0, play the intro cutscene. loadGame() passes false: it only needs
// the terrain reloaded before it overwrites the live state, and both
// mission-start screens already ran when the save was first started.
bool startGame(int mapIndex, bool interactive = true, bool skirmish = false)
{
    char path[24];
    snprintf(path, sizeof(path), skirmish ? "/maps/s%d.aem" : "/maps/m%d.aem", mapIndex);

    gfx.fillScreen(TFT_BLACK);
    gfx.setTextSize(2);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setCursor(10, 10);
    gfx.printf(skirmish ? "loading skirmish %d..." : "loading mission %d...", mapIndex + 1);

    if (!loadMap(path))
    {
        gfx.setCursor(10, 40);
        gfx.print("map load failed!");
        delay(2000);
        // Must set appState here, not just draw the menu -- RETRY (added
        // this milestone) calls startGame() from STATE_PLAYING, and a
        // failure that left appState untouched would keep loop() routing
        // touches to handleTap() (the gameplay handler) against a screen
        // that's showing the menu, swallowing the first mission tap.
        appState = STATE_MENU;
        drawMenu();
        return false;
    }

    currentMapIndex = mapIndex;
    skirmishMode = skirmish;
    currentTurn = HUMAN_COLOR;
    selectedUnit = -1;
    infoUnit = -1;
    gameOver = false;
    winnerColor = -1;
    pauseMenuOpen = false;
    shopOpen = false;
    shopBuyType = -1;
    for (int c = 0; c < MAX_PLAYERS; ++c)
    {
        gold[c] = 0;
        eliminated[c] = false;
    }

    if (skirmish)
    {
        // Derive the turn queue (and remap building/unit ownership) from
        // castle control -- see buildSkirmishTurnQueue().
        if (!buildSkirmishTurnQueue())
        {
            gfx.fillScreen(TFT_BLACK);
            gfx.setTextSize(2);
            gfx.setTextColor(TFT_WHITE, TFT_BLACK);
            gfx.setCursor(10, 10);
            gfx.print("bad skirmish map");
            delay(2000);
            appState = STATE_MENU;
            drawMenu();
            return false;
        }
        for (int c = 0; c < turnQueueLen; ++c)
            gold[c] = SKIRMISH_START_GOLD;
    }
    else
    {
        // Story maps m0-m7 are always a 2-side blue-vs-red fight (see the
        // team-colour note in the README), starting at 0 gold.
        turnQueue[0] = 0;
        turnQueue[1] = 1;
        turnQueueLen = 2;
    }

    // Start the camera on the human player's king (color 0), like the
    // original does -- several maps place your side well down/right of the
    // origin, so a fixed 0,0 view left your units off-screen at mission
    // start. Fall back to the first color-0 unit, then to 0,0.
    int focusX = 0, focusY = 0;
    for (int i = 0; i < unitCount; ++i)
    {
        if (units[i].alive && units[i].color == 0)
        {
            focusX = units[i].tileX;
            focusY = units[i].tileY;
            if (units[i].type == UNIT_TYPE_KING)
                break;
        }
    }
    centerViewOnTile(focusX, focusY);

    if (interactive)
    {
        if (skirmish)
            showSkirmishBriefing(mapIndex);
        else
            showMissionBriefing(mapIndex);
    }

    recountStartingUnits(); // so the first render's footer tally isn't blank

    appState = STATE_PLAYING;
    gfx.fillScreen(TFT_BLACK);
    drawViewport();

    if (mapIndex == 0 && interactive && !skirmish)
    {
        runIntroScript(); // only m0 has a mission-script file -- see its comment
        recountStartingUnits(); // the intro repositions/removes units
        drawViewport();
    }
    return true;
}

void handleMenuTap(int screenX, int screenY)
{
    // Tab bar -- hit-test each drawn button rect exactly (drawMenu() lays
    // them out as [20 + t*tabW, +tabW-4), so the 4px gap and the margins
    // outside are dead).
    if (screenY >= MENU_TAB_Y && screenY < MENU_TAB_Y + MENU_TAB_H)
    {
        int tabW = (DISPLAY_WIDTH - 40) / 2;
        for (int t = 0; t < 2; ++t)
        {
            int tx = 20 + t * tabW;
            if (screenX >= tx && screenX < tx + tabW - 4)
            {
                if (t != menuTab)
                {
                    menuTab = t;
                    drawMenu();
                }
                return;
            }
        }
        return;
    }

    int rowH = menuRowH();
    for (int i = 0; i < menuRowCount(); ++i)
    {
        int rowY = MENU_ROW_TOP + i * rowH;
        if (screenX >= 20 && screenX < DISPLAY_WIDTH - 20 && screenY >= rowY && screenY < rowY + rowH - 6)
        {
            startGame(i, true, menuTab == MENU_TAB_SKIRMISH);
            return;
        }
    }
}

void tryCaptureBuilding(const UnitPlacement &u); // defined below; used by the AI first

// Color 0 (blue) is always the human; every other color in the turn queue
// is AI. endTurn() below auto-resolves each AI side's turn synchronously,
// so control only ever returns to the player on color HUMAN_COLOR.
inline bool isAiColor(int color) { return color != HUMAN_COLOR; }

// True if attackerType/attackerHealth attacking victimIdx is guaranteed to
// kill it no matter how resolveHit()'s random() roll comes out -- using
// UNIT_OFFENCE_MIN (the floor of that roll) against the same
// terrain-adjusted defence resolveHit() itself computes, scaled by
// attacker health the same way. A worst-case-roll kill is a genuinely
// certain one, not a probability estimate.
bool wouldGuaranteeKill(uint8_t attackerType, uint8_t attackerHealth, int atkX, int atkY, int victimIdx)
{
    const UnitPlacement &victim = units[victimIdx];
    uint8_t tile = tileAt(victim.tileX, victim.tileY);
    int terrainBonus = tile < TILE_COUNT ? TERRAIN_DEFENCE_BONUS[TILE_TERRAIN_TYPE[tile]] : 0;
    int defence = UNIT_DEFENCE[victim.type] + terrainBonus +
                  matchupDefenceBonus(victim.type, victim.tileX, victim.tileY);
    int minOffence = UNIT_OFFENCE_MIN[attackerType] +
                     matchupOffenceBonus(attackerType, victim.type, atkX, atkY);
    int minHit = (minOffence - defence) * attackerHealth / 100;
    return minHit >= victim.health;
}

// Returns the index of the best living enemy of `color` that a unit of
// `type`/`health` standing at (fromX,fromY) could attack, or -1 if none
// are in range. "Best" ranks a guaranteed kill (see wouldGuaranteeKill())
// above anything that's merely damage -- securing a kill is strictly
// better than a bigger wound on a different target, whatever their
// relative health -- and falls back to the lowest-health target as
// before when neither or both candidates are guaranteed kills. Still not
// the original's scoring heuristic (sub_10cb() and friends in
// MainDisplayable.java, which weighs many more factors and isn't
// ported), just a cheap, obviously-better-than-arbitrary pair of rules.
int findAttackTarget(uint8_t type, uint8_t health, int color, int fromX, int fromY)
{
    int best = -1;
    bool bestIsKill = false;
    for (int i = 0; i < unitCount; ++i)
    {
        if (!units[i].alive || units[i].color == color)
            continue;
        if (!inAttackRangeFrom(type, fromX, fromY, units[i].tileX, units[i].tileY))
            continue;

        bool isKill = wouldGuaranteeKill(type, health, fromX, fromY, i);
        bool better = best < 0 ||
                      (isKill && !bestIsKill) ||
                      (isKill == bestIsKill && units[i].health < units[best].health);
        if (better)
        {
            best = i;
            bestIsKill = isKill;
        }
    }
    return best;
}

// For the human's move-then-attack in one turn: among the selected unit's
// reachable, unoccupied tiles, find the one from which `targetIdx` sits in
// attack range, favouring the tile that spends the least movement (highest
// reachableCost left) and, as a tiebreak, the best terrain defence bonus.
// Returns false if the target can't be both reached and hit this turn.
bool findAttackApproachTile(const UnitPlacement &attacker, int targetIdx, int &outX, int &outY)
{
    outX = outY = -1;
    if (!reachableCost || targetIdx < 0)
        return false;
    const UnitPlacement &target = units[targetIdx];
    int bestBudget = -1, bestDef = INT32_MIN;
    for (int x = 0; x < mapWidth; ++x)
    {
        for (int y = 0; y < mapHeight; ++y)
        {
            int budget = reachableCost[x * mapHeight + y];
            if (budget < 0)
                continue;
            if (unitIndexAt(x, y) >= 0 && !(x == attacker.tileX && y == attacker.tileY))
                continue;
            if (!inAttackRangeFrom(attacker.type, x, y, target.tileX, target.tileY))
                continue;
            uint8_t tile = tileAt(x, y);
            int def = tile < TILE_COUNT ? TERRAIN_DEFENCE_BONUS[TILE_TERRAIN_TYPE[tile]] : 0;
            if (budget > bestBudget || (budget == bestBudget && def > bestDef))
            {
                bestBudget = budget;
                bestDef = def;
                outX = x;
                outY = y;
            }
        }
    }
    return outX >= 0;
}

// True if the selected unit could attack `targetIdx` this turn, whether
// from where it stands or after a move -- drives the red target highlight.
bool canAttackThisTurn(const UnitPlacement &attacker, int targetIdx)
{
    if (targetIdx < 0 || !units[targetIdx].alive)
        return false;
    if (inAttackRange(attacker, units[targetIdx].tileX, units[targetIdx].tileY))
        return true;
    int ax, ay;
    return findAttackApproachTile(attacker, targetIdx, ax, ay);
}

// A deliberately simple AI move for one unit, evaluated in this priority:
//   1. Attack an enemy already in range from the current tile.
//   2. Otherwise, if some reachable tile puts an enemy in range, move
//      there and attack (move-then-attack in one turn, as the original
//      and the human both do -- see handleTap()). Every reachable
//      attack-capable tile is checked, not just
//      the first found: a tile reaching a target this unit is guaranteed
//      to kill (wouldGuaranteeKill() -- true if even the worst-case damage
//      roll finishes it) always wins over one that only wounds; among
//      ties on that, the target's lowest health wins as before
//      (findAttackTarget()'s rule); among ties on THAT, the tile with the
//      best terrain defence bonus for THIS unit wins -- a free tiebreak,
//      not a real lookahead at whether a counterattack will actually
//      land.
//   3. Otherwise, if this unit's health is critically low (<=25), move to
//      whichever reachable tile MAXIMIZES its distance to the CLOSEST
//      living enemy (checked against every enemy, not just the one
//      nearest before moving -- with enemies on multiple sides, only that
//      minimum says how exposed a tile actually leaves the unit) --
//      crude self-preservation, not real defensive positioning (terrain
//      only factors into step 2's tiebreak above, not into retreat tile
//      choice, and there's no regard for whether retreating abandons an
//      objective).
//   4. Otherwise, move toward the nearest living enemy (by post-move
//      distance) among reachable tiles, to close the gap for a future
//      turn.
//   5. If no enemies remain, do nothing.
// No pathfinding beyond computeReachable()'s flood fill, no coordination
// between units, no target prioritization beyond findAttackTarget()'s
// guaranteed-kill-then-weakest rule. This is enough to make
// single-player winnable and losable, not a port of the original's AI.
void aiActUnit(int idx)
{
    UnitPlacement &u = units[idx];

    int target = findAttackTarget(u.type, u.health, u.color, u.tileX, u.tileY);
    if (target >= 0)
    {
        attackUnit(idx, target);
        u.hasMoved = true;
        return;
    }

    computeReachable(u);
    if (!reachableCost)
    {
        u.hasMoved = true; // can't plan a move this turn; don't get stuck retrying
        return;
    }

    int nearestEnemy = -1, nearestDist = INT32_MAX;
    for (int i = 0; i < unitCount; ++i)
    {
        if (!units[i].alive || units[i].color == u.color)
            continue;
        int d = manhattanDist(u.tileX, u.tileY, units[i].tileX, units[i].tileY);
        if (d < nearestDist)
        {
            nearestDist = d;
            nearestEnemy = i;
        }
    }
    if (nearestEnemy < 0)
    {
        u.hasMoved = true; // nothing left to fight
        return;
    }

    constexpr int RETREAT_HEALTH_THRESHOLD = 25;
    bool retreating = u.health <= RETREAT_HEALTH_THRESHOLD;

    // A full scan, not a first-match: an attack-capable tile found early in
    // x/y order must not win over a later one that reaches a weaker target,
    // and (for retreat) distance-to-the-single-nearest-enemy isn't a safe
    // score once there's more than one enemy on the board -- maximizing it
    // can walk the unit straight at a different enemy. So every reachable
    // tile is scored fully before moveToX/Y is committed below.
    int moveToX = -1, moveToY = -1;
    int bestApproachDist = nearestDist;  // used only when !retreating
    int bestRetreatMinDist = -1;         // used only when retreating
    int attackTileX = -1, attackTileY = -1, attackTargetHealth = INT32_MAX, attackTileDefBonus = INT32_MIN;
    bool attackTileIsKill = false;
    for (int x = 0; x < mapWidth; ++x)
    {
        for (int y = 0; y < mapHeight; ++y)
        {
            if (reachableCost[x * mapHeight + y] < 0)
                continue;
            if (unitIndexAt(x, y) >= 0 && !(x == u.tileX && y == u.tileY))
                continue;

            int tileTarget = findAttackTarget(u.type, u.health, u.color, x, y);
            if (tileTarget >= 0)
            {
                // A tile reaching a guaranteed kill (see wouldGuaranteeKill())
                // beats one that only wounds, whatever the health numbers;
                // among tiles tying on that, the weakest target wins as
                // before, and among ties on THAT, prefer the tile with the
                // best terrain defence bonus for THIS unit -- the tile a
                // counterattack (if the target survives and is
                // close-range-eligible) would land on, per resolveHit()'s
                // own terrain lookup on the defender's tile. Not a full
                // lookahead (doesn't know if a counter will actually
                // happen), just a free tiebreak among otherwise equal
                // attack options.
                bool isKill = wouldGuaranteeKill(u.type, u.health, x, y, tileTarget);
                uint8_t tile = tileAt(x, y);
                int defBonus = tile < TILE_COUNT ? TERRAIN_DEFENCE_BONUS[TILE_TERRAIN_TYPE[tile]] : 0;
                bool better = attackTileX < 0 ||
                              (isKill && !attackTileIsKill) ||
                              (isKill == attackTileIsKill && units[tileTarget].health < attackTargetHealth) ||
                              (isKill == attackTileIsKill && units[tileTarget].health == attackTargetHealth && defBonus > attackTileDefBonus);
                if (better)
                {
                    attackTileX = x;
                    attackTileY = y;
                    attackTargetHealth = units[tileTarget].health;
                    attackTileDefBonus = defBonus;
                    attackTileIsKill = isKill;
                }
                continue; // an attack-capable tile never competes with approach/retreat
            }

            if (retreating)
            {
                // Distance to the CLOSEST living enemy from this tile, not
                // just to whichever enemy was nearest before moving -- with
                // enemies on multiple sides, only the minimum across all of
                // them tells you how exposed this tile actually leaves the
                // unit.
                int minDist = INT32_MAX;
                for (int i = 0; i < unitCount; ++i)
                {
                    if (!units[i].alive || units[i].color == u.color)
                        continue;
                    int d = manhattanDist(x, y, units[i].tileX, units[i].tileY);
                    if (d < minDist)
                        minDist = d;
                }
                if (minDist > bestRetreatMinDist)
                {
                    bestRetreatMinDist = minDist;
                    moveToX = x;
                    moveToY = y;
                }
            }
            else
            {
                int d = manhattanDist(x, y, units[nearestEnemy].tileX, units[nearestEnemy].tileY);
                if (d < bestApproachDist)
                {
                    bestApproachDist = d;
                    moveToX = x;
                    moveToY = y;
                }
            }
        }
    }

    if (attackTileX >= 0)
    {
        moveToX = attackTileX;
        moveToY = attackTileY;
    }

    if (moveToX >= 0)
    {
        u.tileX = (int16_t)moveToX;
        u.tileY = (int16_t)moveToY;
        tryCaptureBuilding(u);
    }
    u.hasMoved = true;

    int postMoveTarget = findAttackTarget(u.type, u.health, u.color, u.tileX, u.tileY);
    if (postMoveTarget >= 0)
        attackUnit(idx, postMoveTarget);
}

// A deliberately blunt AI purchase: once its units have moved, if it owns
// a castle and can afford something, buy the strongest unit it can pay for
// (up to a soft army-size cap so it doesn't just spam) and drop it on a
// tile next to that castle. No unit-mix planning -- see aiActUnit()'s note
// on why the AI here isn't a port of the original's.
void aiTryRecruit(int color)
{
    if (gameOver || eliminated[color] || ownedCastleCount(color) == 0 || unitCount >= MAX_UNITS)
        return;
    int myUnits = 0;
    for (int i = 0; i < unitCount; ++i)
        if (units[i].alive && units[i].color == color)
            ++myUnits;
    if (myUnits >= 10)
        return;

    int buyType = -1;
    for (int t = SHOP_BUYABLE_COUNT - 1; t >= 0; --t) // priciest first
        if (UNIT_COST[t] > 0 && gold[color] >= UNIT_COST[t])
        {
            buyType = t;
            break;
        }
    if (buyType < 0)
        return;

    for (int x = 0; x < mapWidth && buyType >= 0; ++x)
        for (int y = 0; y < mapHeight; ++y)
        {
            if (!isShopDeployTile(x, y, color))
                continue;
            gold[color] -= UNIT_COST[buyType];
            UnitPlacement &nu = units[unitCount++];
            nu.type = (uint8_t)buyType;
            nu.color = (uint8_t)color;
            nu.tileX = (int16_t)x;
            nu.tileY = (int16_t)y;
            nu.hasMoved = true; // it moves next turn
            nu.alive = true;
            nu.health = 100;
            if (color < MAX_PLAYERS)
                startingUnits[color]++;
            Serial.printf("AI (color %d) recruited type %d at (%d,%d), gold now %ld\n",
                          color, buyType, x, y, (long)gold[color]);
            buyType = -1; // done -- one unit per turn
            break;
        }
}

void runAITurn()
{
    for (int i = 0; i < unitCount; ++i)
    {
        if (units[i].alive && units[i].color == currentTurn && !units[i].hasMoved)
            aiActUnit(i);
        if (gameOver) // e.g. this AI unit just killed the human king
            return;
    }
    aiTryRecruit(currentTurn);
}

// Villages/castles color `c` owns, as gold: the sum credited at its turn
// start. buildingFraction() is 1-based (0 = neutral), so ownership is
// fraction == c + 1.
int computeIncome(int c)
{
    int income = 0;
    for (int i = 0; i < mapWidth * mapHeight; ++i)
    {
        uint8_t t = mapTiles[i];
        if (!isFractionBuilding(t) || buildingFraction(t) != c + 1)
            continue;
        income += (TILE_TERRAIN_TYPE[t] == 9) ? CASTLE_INCOME : VILLAGE_INCOME;
    }
    return income;
}

// At each side's turn start the original heals up to 20 HP (capped at
// 100) for each of that side's units standing on a neutral town (terrain
// type 7) or a building that side owns -- MainDisplayable ~4102.
constexpr int TURN_HEAL = 20;
void applyTurnHealing(int color)
{
    for (int i = 0; i < unitCount; ++i)
    {
        UnitPlacement &u = units[i];
        if (!u.alive || u.color != color || u.health >= 100)
            continue;
        uint8_t t = tileAt(u.tileX, u.tileY);
        if (t >= TILE_COUNT)
            continue;
        bool onTown = TILE_TERRAIN_TYPE[t] == 7;
        bool onOwned = isFractionBuilding(t) && buildingFraction(t) == color + 1;
        if (!onTown && !onOwned)
            continue;
        int heal = min(TURN_HEAL, 100 - (int)u.health);
        u.health += heal;
        Serial.printf("heal: color %d unit %d +%d (hp %d)\n", color, i, heal, u.health);
    }
}

// Advance currentTurn to the next colour in the turn queue, then grant
// that side its turn-start income and healing and un-move its units.
void switchTurn()
{
    int pos = 0;
    for (int i = 0; i < turnQueueLen; ++i)
        if (turnQueue[i] == currentTurn)
        {
            pos = i;
            break;
        }
    // Advance to the next side still in the fight -- skip any that have
    // been eliminated so a defeated colour never gets another turn.
    for (int step = 1; step <= turnQueueLen; ++step)
    {
        int cand = turnQueue[(pos + step) % turnQueueLen];
        if (!eliminated[cand])
        {
            currentTurn = cand;
            break;
        }
    }

    for (int i = 0; i < unitCount; ++i)
        if (units[i].color == currentTurn)
            units[i].hasMoved = false;
    selectedUnit = -1;

    int income = computeIncome(currentTurn);
    if (income > 0)
    {
        gold[currentTurn] += income;
        Serial.printf("turn income: color %d +%d (now %ld)\n", currentTurn, income, (long)gold[currentTurn]);
    }
    applyTurnHealing(currentTurn);
}

void endTurn()
{
    checkEndConditions(); // the player may have cleared the board on this turn
    if (gameOver)
    {
        drawViewport();
        return;
    }

    // Hand off around the queue, running each AI side's whole turn as we
    // pass it, until control is back on the human (or the game ends). At
    // most one full lap -- guards a degenerate queue with no human slot.
    for (int steps = 0; steps < turnQueueLen; ++steps)
    {
        switchTurn();
        Serial.printf("turn: color %d\n", currentTurn);
        if (gameOver)
            break;
        if (!isAiColor(currentTurn))
            break;
        runAITurn();
        checkEndConditions();
        if (gameOver)
            break;
    }

    // The human is always an un-eliminated member of the queue when
    // endTurn() runs (the shop/END-TURN paths and checkEndConditions()
    // guarantee it), so the lap above lands back on HUMAN_COLOR. Belt and
    // braces: never leave the player holding an AI side if it somehow
    // didn't.
    if (!gameOver && isAiColor(currentTurn))
        currentTurn = HUMAN_COLOR;

    drawViewport();
}

// MENU (hamburger), SHOP (cart) and END TURN (return-arrow) are square
// icon buttons in a row at the footer band's right edge; the
// selected-unit stat panel fills the space to their left. MENU is always
// available -- a mission whose win condition can't trigger (m4/m6 place
// no red units, see README) would otherwise trap the player.
constexpr int ICON_BTN = 30;
constexpr int ICON_GAP = 5;
constexpr int ICON_BTN_Y = FOOTER_Y + (FOOTER_H - ICON_BTN) / 2;

constexpr int HUD_BTN_W = ICON_BTN; // END TURN, rightmost
constexpr int HUD_BTN_H = ICON_BTN;
constexpr int HUD_BTN_X = DISPLAY_WIDTH - ICON_BTN - 4;
constexpr int HUD_BTN_Y = ICON_BTN_Y;

constexpr int SHOP_BTN_X = HUD_BTN_X - ICON_BTN - ICON_GAP;
constexpr int SHOP_BTN_Y = ICON_BTN_Y;

constexpr int MENU_BTN_W = ICON_BTN;
constexpr int MENU_BTN_H = ICON_BTN;
constexpr int MENU_BTN_X = SHOP_BTN_X - ICON_BTN - ICON_GAP;
constexpr int MENU_BTN_Y = ICON_BTN_Y;

// Win/loss banner geometry, and its RETRY button -- shared between
// drawViewport() (drawing it) and handleTap() (hit-testing it), so both
// use the same constants rather than each computing the layout itself.
constexpr int BANNER_H = 20;
constexpr int BANNER_Y = (DISPLAY_HEIGHT - BANNER_H) / 2;
constexpr int RETRY_BTN_W = 70;
constexpr int RETRY_BTN_H = 18;
constexpr int RETRY_BTN_X = (DISPLAY_WIDTH - RETRY_BTN_W) / 2;
constexpr int RETRY_BTN_Y = BANNER_Y + BANNER_H + 8;

// Pause menu (hamburger) -- a centred list, shared between drawPauseMenu()
// and handleTap(). Rows: Return to game / Save game / Load game / Exit to
// title.
enum { PM_RETURN, PM_SAVE, PM_LOAD, PM_EXIT, PM_ROWS };
constexpr int PM_W = 176;
constexpr int PM_ROW_H = 30;
constexpr int PM_PAD = 6;
constexpr int PM_H = PM_ROWS * PM_ROW_H + PM_PAD * 2;
constexpr int PM_X = (DISPLAY_WIDTH - PM_W) / 2;
constexpr int PM_Y = (DISPLAY_HEIGHT - PM_H) / 2;

// Shop unit list (buyable types 0..8, SHOP_BUYABLE_COUNT rows). Shared
// between drawShop() and handleTap().
constexpr int SHOP_W = 200;
constexpr int SHOP_ROW_H = 20;
constexpr int SHOP_HEAD_H = 22;
constexpr int SHOP_H = SHOP_HEAD_H + SHOP_BUYABLE_COUNT * SHOP_ROW_H + 6;
constexpr int SHOP_X = (DISPLAY_WIDTH - SHOP_W) / 2;
constexpr int SHOP_Y = (DISPLAY_HEIGHT - SHOP_H) / 2;

inline bool isCastleTile(uint8_t tile)
{
    return isFractionBuilding(tile) && TILE_TERRAIN_TYPE[tile] == 9;
}

int ownedCastleCount(int color)
{
    int n = 0;
    for (int i = 0; i < mapWidth * mapHeight; ++i)
        if (isCastleTile(mapTiles[i]) && buildingFraction(mapTiles[i]) == color + 1)
            ++n;
    return n;
}

// Ground a shop-bought unit can be deployed onto: on the map, unoccupied,
// passable terrain (no mountain/water/lava), and 4-adjacent to a castle
// `color` owns.
bool isShopDeployTile(int x, int y, int color)
{
    if (!inMapBounds(x, y) || unitIndexAt(x, y) >= 0)
        return false;
    uint8_t here = tileAt(x, y);
    if (here >= TILE_COUNT)
        return false;
    uint8_t tt = TILE_TERRAIN_TYPE[here];
    if (tt == 4 || tt == 5 || tt == 10) // mountain, water, lava
        return false;
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int d = 0; d < 4; ++d)
    {
        int nx = x + dx[d], ny = y + dy[d];
        if (inMapBounds(nx, ny) && isCastleTile(tileAt(nx, ny)) &&
            buildingFraction(tileAt(nx, ny)) == color + 1)
            return true;
    }
    return false;
}

// If u just moved onto an enemy/neutral fraction building it's equipped to
// capture (Unit.java's UNIT_PROPERTY_CAPTURE_VILLAGE/CASTLE bits -- soldier
// and king for villages, king only for castles), flips its ownership to u's
// color. Mutates mapTiles directly; the tile redraws next frame like any
// other terrain change since drawViewport() reloads whatever tile index is
// actually at each visible cell.
void tryCaptureBuilding(const UnitPlacement &u)
{
    uint8_t tile = tileAt(u.tileX, u.tileY);
    if (!isFractionBuilding(tile))
        return;

    int ownerFraction = buildingFraction(tile);
    int myFraction = u.color + 1; // building fractions are 1-based; see UnitPlacement's comment
    if (ownerFraction == myFraction)
        return;

    bool isCastle = TILE_TERRAIN_TYPE[tile] == 9;
    bool canCapture = isCastle ? (UNIT_PROPERTIES[u.type] & UNIT_PROPERTY_CAPTURE_CASTLE)
                                : (UNIT_PROPERTIES[u.type] & UNIT_PROPERTY_CAPTURE_VILLAGE);
    if (!canCapture)
        return;

    mapTiles[u.tileX * mapHeight + u.tileY] = setBuildingFraction(tile, myFraction);
    Serial.printf("unit captured %s at (%d,%d) for color %d\n", isCastle ? "castle" : "village", u.tileX, u.tileY, u.color);
}

// Handles a tap (as opposed to a drag-to-pan) at the given screen
// coordinates: the MENU button, the END TURN button, selecting a movable
// unit belonging to the current turn (or moving/attacking with it once
// selected), or -- for any other living unit, friend or foe -- showing
// its stat panel (infoUnit) without starting a move.
void handleTap(int screenX, int screenY)
{
    auto inRect = [&](int rx, int ry, int rw, int rh)
    { return screenX >= rx && screenX < rx + rw && screenY >= ry && screenY < ry + rh; };

    // Modal layers consume the tap before anything game-side sees it.
    if (shopOpen)
    {
        if (shopBuyType < 0)
        {
            // List: tap a row you can afford to pick that unit for deploy.
            for (int i = 0; i < SHOP_BUYABLE_COUNT; ++i)
            {
                int ry = SHOP_Y + SHOP_HEAD_H + i * SHOP_ROW_H;
                if (!inRect(SHOP_X + 4, ry, SHOP_W - 8, SHOP_ROW_H))
                    continue;
                if (gold[HUMAN_COLOR] >= UNIT_COST[i] && unitCount < MAX_UNITS)
                    shopBuyType = i;
                drawViewport();
                return;
            }
            shopOpen = false; // tap off the list closes it
            drawViewport();
            return;
        }

        // Deploy: a green tile spawns the unit; anything else cancels back
        // to the list.
        if (screenY >= MAP_VIEW_Y && screenY < MAP_VIEW_Y + MAP_VIEW_H)
        {
            int mx = (screenX + viewX) / TILE_SIZE;
            int my = (screenY - MAP_VIEW_Y + viewY) / TILE_SIZE;
            if (isShopDeployTile(mx, my, 0) && unitCount < MAX_UNITS &&
                gold[HUMAN_COLOR] >= UNIT_COST[shopBuyType])
            {
                gold[HUMAN_COLOR] -= UNIT_COST[shopBuyType];
                UnitPlacement &nu = units[unitCount++];
                nu.type = (uint8_t)shopBuyType;
                nu.color = 0;
                nu.tileX = (int16_t)mx;
                nu.tileY = (int16_t)my;
                nu.hasMoved = false; // usable this turn, like the original
                nu.alive = true;
                nu.health = 100;
                Serial.printf("recruited type %d at (%d,%d) for %d, gold now %ld\n",
                              shopBuyType, mx, my, UNIT_COST[shopBuyType], (long)gold[HUMAN_COLOR]);
                startingUnits[0]++; // it's now part of blue's roster
                shopOpen = false;
                shopBuyType = -1;
                drawViewport();
                return;
            }
        }
        shopBuyType = -1; // back to the list
        drawViewport();
        return;
    }
    if (pauseMenuOpen)
    {
        for (int i = 0; i < PM_ROWS; ++i)
        {
            int ry = PM_Y + PM_PAD + i * PM_ROW_H;
            if (!inRect(PM_X + 6, ry, PM_W - 12, PM_ROW_H - 4))
                continue;
            if (i == PM_RETURN)
            {
                pauseMenuOpen = false;
            }
            else if (i == PM_SAVE)
            {
                bool ok = saveGame();
                drawViewport();
                toast(ok ? "Game saved" : "Save failed");
                delay(700);
            }
            else if (i == PM_LOAD)
            {
                if (!hasSavedGame())
                    return; // disabled row -- ignore
                pauseMenuOpen = false;
                if (loadGame())
                {
                    drawViewport();
                }
                else
                {
                    // loadGame() may have failed before touching game
                    // state (bad magic) or after bouncing to the menu
                    // (bad map) -- force the menu either way so taps route
                    // to handleMenuTap(), not handleTap().
                    appState = STATE_MENU;
                    selectedUnit = infoUnit = -1;
                    drawMenu();
                    toast("Load failed");
                    delay(900);
                    drawMenu();
                }
                return;
            }
            else // PM_EXIT
            {
                pauseMenuOpen = false;
                selectedUnit = infoUnit = -1;
                appState = STATE_MENU;
                showTitleScreen(); // splash + tap, same as boot
                drawMenu();
                return;
            }
            drawViewport();
            return;
        }
        pauseMenuOpen = false; // tap outside the list closes it
        drawViewport();
        return;
    }

    if (inRect(MENU_BTN_X, MENU_BTN_Y, ICON_BTN, ICON_BTN))
    {
        selectedUnit = infoUnit = -1;
        pauseMenuOpen = true;
        drawViewport();
        return;
    }
    if (inRect(SHOP_BTN_X, SHOP_BTN_Y, ICON_BTN, ICON_BTN))
    {
        selectedUnit = infoUnit = -1;
        // Recruiting needs your turn and a castle to deploy next to.
        if (currentTurn == HUMAN_COLOR && !gameOver && !eliminated[HUMAN_COLOR] && ownedCastleCount(HUMAN_COLOR) > 0)
        {
            shopOpen = true;
            shopBuyType = -1;
        }
        else
        {
            drawViewport();
            toast(gameOver ? "Mission over" : currentTurn != 0 ? "Not your turn" : "Need a castle to recruit");
            delay(800);
        }
        drawViewport();
        return;
    }

    if (gameOver)
    {
        if (inRect(RETRY_BTN_X, RETRY_BTN_Y, RETRY_BTN_W, RETRY_BTN_H))
        {
            startGame(currentMapIndex, true, skirmishMode);
            return;
        }
        // Any other tap while the win banner is up returns to the menu.
        appState = STATE_MENU;
        drawMenu();
        return;
    }

    if (inRect(HUD_BTN_X, HUD_BTN_Y, HUD_BTN_W, HUD_BTN_H))
    {
        infoUnit = -1;
        endTurn();
        return;
    }

    // Taps in the header/footer bands that missed a button do nothing to
    // the map (and never map to a bogus edge tile).
    if (screenY < MAP_VIEW_Y || screenY >= MAP_VIEW_Y + MAP_VIEW_H)
        return;

    int mx = (screenX + viewX) / TILE_SIZE;
    int my = (screenY - MAP_VIEW_Y + viewY) / TILE_SIZE;
    if (!inMapBounds(mx, my))
    {
        if (infoUnit >= 0)
        {
            infoUnit = -1;
            drawViewport();
        }
        return;
    }

    if (selectedUnit >= 0)
    {
        int targetIdx = unitIndexAt(mx, my);
        UnitPlacement &sel = units[selectedUnit];
        bool tappedEnemy = targetIdx >= 0 && units[targetIdx].color != currentTurn;

        if (tappedEnemy && inAttackRange(sel, mx, my))
        {
            // Already in range -- attack from where it stands.
            attackUnit(selectedUnit, targetIdx);
            sel.hasMoved = true;
        }
        else if (tappedEnemy)
        {
            // Move into range first, then attack (one turn) -- same as the
            // AI's move-then-attack in aiActUnit().
            int ax, ay;
            if (findAttackApproachTile(sel, targetIdx, ax, ay))
            {
                sel.tileX = (int16_t)ax;
                sel.tileY = (int16_t)ay;
                tryCaptureBuilding(sel);
                attackUnit(selectedUnit, targetIdx);
                sel.hasMoved = true;
            }
            else
            {
                infoUnit = targetIdx; // can't be reached-and-hit this turn -- show its panel
            }
        }
        else if (reachableCost && reachableCost[mx * mapHeight + my] >= 0 && targetIdx < 0)
        {
            sel.tileX = (int16_t)mx;
            sel.tileY = (int16_t)my;
            sel.hasMoved = true;
            tryCaptureBuilding(sel);
        }
        else if (targetIdx >= 0)
        {
            // Tapped a friendly/own unit while one was selected -- deselecting
            // silently would make tap-to-inspect need a second tap. Show its
            // panel instead, same as tapping it with nothing selected would.
            infoUnit = targetIdx;
        }
        selectedUnit = -1;
        drawViewport();
        return;
    }

    int idx = unitIndexAt(mx, my);
    // Only ever selectable as a movable unit if it's the human's and it's
    // the human's turn -- taps never drive an AI side.
    if (idx >= 0 && units[idx].color == HUMAN_COLOR && currentTurn == HUMAN_COLOR && !units[idx].hasMoved)
    {
        infoUnit = -1;
        selectedUnit = idx;
        computeReachable(units[idx]);
        drawViewport();
    }
    else if (idx != infoUnit)
    {
        // Any other living unit -- an enemy, or a friendly unit that's
        // already moved this turn -- can't be selected to act with, but
        // tapping it (or empty ground, idx == -1, to dismiss) shows/hides
        // its stat panel instead of doing nothing.
        infoUnit = idx;
        drawViewport();
    }
}

// Writes a fully-desaturated, slightly-dimmed copy of an RGB565 unit-icon
// frame into `out` (same UNIT_ICON_SIZE^2 layout). TRANSPARENT_565 pixels
// pass through untouched so the sprite's cut-out edges stay transparent.
static void desaturateIcon(const uint16_t *src, uint16_t *out)
{
    for (int i = 0; i < UNIT_ICON_SIZE * UNIT_ICON_SIZE; ++i)
    {
        uint16_t p = src[i];
        if (p == TRANSPARENT_565)
        {
            out[i] = p;
            continue;
        }
        int r = ((p >> 11) & 0x1F) * 255 / 31;
        int g = ((p >> 5) & 0x3F) * 255 / 63;
        int b = (p & 0x1F) * 255 / 31;
        int y = (r * 77 + g * 150 + b * 29) >> 8; // Rec.601 luma
        y = y * 5 / 8;                            // dim so it reads as "disabled"
        out[i] = (uint16_t)(((y * 31 / 255) << 11) | ((y * 63 / 255) << 5) | (y * 31 / 255));
    }
}

constexpr uint16_t BAND_BG = 0x18E3;   // near-black, distinct from the map
constexpr uint16_t BAND_EDGE = 0x4208; // thin separator line
constexpr uint16_t GOLD_COLOR = 0xFEA0; // warm coin yellow

// A little coin glyph with its centre at (cx, cy), ~9px across.
static void drawCoin(int cx, int cy)
{
    gfx.fillCircle(cx, cy, 4, GOLD_COLOR);
    gfx.drawCircle(cx, cy, 4, 0x9C40);   // darker rim
    gfx.drawFastVLine(cx, cy - 2, 5, 0x9C40); // a stamped mark
}

// The header and footer bands. Opaque, drawn every frame over a fixed
// screen region the map never touches (drawViewport() clips the map to
// the strip between them), so HUD and terrain never overlap.
//   header: whose turn it is (left), gold (right)
//   footer: selected-unit stats or the living-unit tally (left);
//           MENU / SHOP / END TURN icon buttons (right)
void drawHud()
{
    // ---- header ----
    gfx.fillRect(0, 0, DISPLAY_WIDTH, HEADER_H, BAND_BG);
    gfx.drawFastHLine(0, HEADER_H - 1, DISPLAY_WIDTH, BAND_EDGE);
    gfx.setTextSize(1);
    int tc = currentTurn >= 0 && currentTurn < 4 ? currentTurn : 0;
    gfx.setTextColor(PLAYER_HUD_COLOR[tc], BAND_BG);
    gfx.setCursor(4, 4);
    if (currentTurn == HUMAN_COLOR)
        gfx.print("YOUR TURN");
    else if (turnQueueLen > 2)
        gfx.printf("%s TURN", PLAYER_NAME[tc]);
    else
        gfx.print("ENEMY TURN");

    char goldStr[12];
    snprintf(goldStr, sizeof(goldStr), "%ld", (long)gold[HUMAN_COLOR]);
    int goldTextX = DISPLAY_WIDTH - 4 - (int)strlen(goldStr) * 6;
    drawCoin(goldTextX - 8, 7);
    gfx.setTextColor(GOLD_COLOR, BAND_BG);
    gfx.setCursor(goldTextX, 4);
    gfx.print(goldStr);

    // ---- footer ----
    gfx.fillRect(0, FOOTER_Y, DISPLAY_WIDTH, FOOTER_H, BAND_BG);
    gfx.drawFastHLine(0, FOOTER_Y, DISPLAY_WIDTH, BAND_EDGE);

    // MENU (hamburger), SHOP (cart), END TURN (return arrow) -- left to
    // right at the footer's right edge.
    gfx.fillRoundRect(MENU_BTN_X, MENU_BTN_Y, ICON_BTN, ICON_BTN, 4, TFT_DARKGREY);
    gfx.drawRoundRect(MENU_BTN_X, MENU_BTN_Y, ICON_BTN, ICON_BTN, 4, TFT_WHITE);
    {
        int bx = MENU_BTN_X + 8, bw = ICON_BTN - 16;
        for (int k = 0; k < 3; ++k)
            gfx.fillRect(bx, MENU_BTN_Y + 8 + k * 6, bw, 3, TFT_WHITE);
    }

    bool shopReady = currentTurn == HUMAN_COLOR && !gameOver && !eliminated[HUMAN_COLOR] && ownedCastleCount(HUMAN_COLOR) > 0;
    uint16_t cartColor = shopReady ? GOLD_COLOR : 0x7BEF;
    gfx.fillRoundRect(SHOP_BTN_X, SHOP_BTN_Y, ICON_BTN, ICON_BTN, 4, shopReady ? TFT_DARKGREY : 0x2104);
    gfx.drawRoundRect(SHOP_BTN_X, SHOP_BTN_Y, ICON_BTN, ICON_BTN, 4, shopReady ? TFT_WHITE : 0x7BEF);
    {
        // Shopping cart: basket outline, angled handle, two wheels.
        int cx = SHOP_BTN_X + ICON_BTN / 2, cy = SHOP_BTN_Y + ICON_BTN / 2;
        gfx.drawRect(cx - 6, cy - 4, 13, 9, cartColor);
        gfx.drawLine(cx - 6, cy - 4, cx - 10, cy - 8, cartColor);
        gfx.fillCircle(cx - 3, cy + 8, 2, cartColor);
        gfx.fillCircle(cx + 4, cy + 8, 2, cartColor);
    }

    gfx.fillRoundRect(HUD_BTN_X, HUD_BTN_Y, ICON_BTN, ICON_BTN, 4, TFT_DARKGREEN);
    gfx.drawRoundRect(HUD_BTN_X, HUD_BTN_Y, ICON_BTN, ICON_BTN, 4, TFT_WHITE);
    {
        // "Return" glyph: a riser on the right, a shaft running left, and
        // an arrowhead pointing left (end the turn / hand back control).
        int cx = HUD_BTN_X + ICON_BTN / 2, cy = HUD_BTN_Y + ICON_BTN / 2;
        gfx.fillRect(cx + 4, cy - 7, 3, 12, TFT_WHITE);            // riser
        gfx.fillRect(cx - 5, cy + 2, 12, 3, TFT_WHITE);            // shaft
        gfx.fillTriangle(cx - 9, cy + 3, cx - 3, cy - 2, cx - 3, cy + 8, TFT_WHITE); // head
    }

    // Footer-left: the tap-to-inspect unit's stats, else the unit
    // currently selected to act with, else the living-unit tally. A unit
    // that died since the tap (e.g. an AI turn ran) just stops showing.
    if (infoUnit >= 0 && (infoUnit >= unitCount || !units[infoUnit].alive))
        infoUnit = -1;
    int showUnit = infoUnit >= 0 ? infoUnit : selectedUnit;
    int panelX = 5, panelY = FOOTER_Y + 5;
    if (showUnit >= 0 && showUnit < unitCount && units[showUnit].alive)
    {
        const UnitPlacement &iu = units[showUnit];
        uint16_t ownerColor = PLAYER_HUD_COLOR[iu.color & 3];
        gfx.setTextColor(ownerColor, BAND_BG);
        gfx.setCursor(panelX, panelY);
        gfx.printf("%s  HP %d", UNIT_TYPE_NAMES[iu.type], iu.health);
        gfx.setTextColor(TFT_WHITE, BAND_BG);
        gfx.setCursor(panelX, panelY + 12);
        gfx.printf("ATK %d-%d  DEF %d", UNIT_OFFENCE_MIN[iu.type], UNIT_OFFENCE_MAX[iu.type], UNIT_DEFENCE[iu.type]);
        gfx.setCursor(panelX, panelY + 24);
        gfx.printf("RNG %d-%d  MOV %d", UNIT_ATTACK_RANGE_MIN[iu.type], UNIT_ATTACK_RANGE_MAX[iu.type], UNIT_MOVE_RANGE[iu.type]);
    }
    else
    {
        int aliveByColor[MAX_PLAYERS] = {0};
        for (int i = 0; i < unitCount; ++i)
            if (units[i].alive && units[i].color < MAX_PLAYERS)
                aliveByColor[units[i].color]++;
        // "N vs N [vs N ...]" for every side that started the mission.
        gfx.setCursor(panelX, panelY + 12);
        bool first = true;
        for (int c = 0; c < MAX_PLAYERS; ++c)
        {
            if (startingUnits[c] <= 0)
                continue;
            if (!first)
            {
                gfx.setTextColor(TFT_WHITE, BAND_BG);
                gfx.print(" vs ");
            }
            gfx.setTextColor(PLAYER_HUD_COLOR[c], BAND_BG);
            gfx.printf("%d", aliveByColor[c]);
            first = false;
        }
    }
}

// --- Save / load ------------------------------------------------------
// A single NVS slot ("aeii"/"save"): a flat snapshot of the mission in
// progress, enough to resume a story or skirmish map exactly -- units,
// turn, per-side gold, the turn queue, camera, AND the live tile grid
// (so captured villages/castles keep their new owner, and a skirmish
// map's queue-remapped building fractions survive). Mission-script
// progress isn't covered (m0's cutscene only runs at mission start, and
// loadGame() suppresses it).
constexpr int SAVE_MAX_TILES = 2048; // every story/skirmish map is well under this
struct SaveBlob
{
    uint32_t magic;
    int32_t mapIndex;
    int32_t turn;
    int32_t gold[MAX_PLAYERS];
    int32_t viewX, viewY;
    int32_t count;
    int32_t mapCells; // mapWidth*mapHeight, must match on load
    int32_t startUnits[MAX_PLAYERS]; // per-side starting roster, for checkEndConditions()
    int32_t queueLen;               // active sides this game
    uint8_t queue[MAX_PLAYERS];     // turn order
    uint8_t elim[MAX_PLAYERS];      // latched per-side defeat
    uint8_t over;
    int8_t winner;
    uint8_t skirmish;               // 0 = story map (m<idx>), 1 = skirmish (s<idx>)
    UnitPlacement units[MAX_UNITS];
    uint8_t tiles[SAVE_MAX_TILES];
};
constexpr uint32_t SAVE_MAGIC = 0x41453206; // "AE2", format 6 (skirmish flag)

bool hasSavedGame()
{
    Preferences p;
    if (!p.begin("aeii", true))
        return false;
    // Size alone isn't enough: an older-format blob can be byte-identical
    // in length (a new format field lands in what used to be struct
    // padding), so also check the magic -- otherwise LOAD looks available
    // and then fails in loadGame(). Preferences can't do a partial read,
    // so pull the whole blob (rare call -- menu / pause-menu render).
    bool ok = false;
    if (p.getBytesLength("save") == sizeof(SaveBlob))
    {
        static SaveBlob b;
        ok = p.getBytes("save", &b, sizeof(b)) == sizeof(b) && b.magic == SAVE_MAGIC;
    }
    p.end();
    return ok;
}

bool saveGame()
{
    if (currentMapIndex < 0 || !mapTiles)
        return false;
    size_t cells = (size_t)mapWidth * mapHeight;
    if (cells == 0 || cells > SAVE_MAX_TILES)
        return false;

    static SaveBlob b;
    b = SaveBlob{};
    b.magic = SAVE_MAGIC;
    b.mapIndex = currentMapIndex;
    b.turn = currentTurn;
    for (int c = 0; c < MAX_PLAYERS; ++c)
    {
        b.gold[c] = gold[c];
        b.startUnits[c] = startingUnits[c];
        b.queue[c] = turnQueue[c];
        b.elim[c] = eliminated[c] ? 1 : 0;
    }
    b.queueLen = turnQueueLen;
    b.viewX = viewX;
    b.viewY = viewY;
    b.count = unitCount;
    b.mapCells = (int32_t)cells;
    b.over = gameOver ? 1 : 0;
    b.winner = (int8_t)winnerColor;
    b.skirmish = skirmishMode ? 1 : 0;
    memcpy(b.units, units, sizeof(units));
    memcpy(b.tiles, mapTiles, cells);

    Preferences p;
    if (!p.begin("aeii", false))
        return false;
    size_t n = p.putBytes("save", &b, sizeof(b));
    p.end();
    return n == sizeof(b);
}

bool loadGame()
{
    Preferences p;
    if (!p.begin("aeii", true))
        return false;
    static SaveBlob b;
    b = SaveBlob{};
    size_t n = p.getBytes("save", &b, sizeof(b));
    p.end();
    bool loadSkirmish = b.skirmish != 0;
    int mapLimit = loadSkirmish ? SKIRMISH_MAP_COUNT : STORY_MAP_COUNT;
    if (n != sizeof(b) || b.magic != SAVE_MAGIC ||
        b.mapIndex < 0 || b.mapIndex >= mapLimit ||
        b.count < 0 || b.count > MAX_UNITS)
        return false;

    // Validate the serialized turn model before anything indexes gold[]
    // or walks the queue with it: length in range, every active colour
    // unique and in range, the human side present (endTurn() assumes it
    // can always hand control back to HUMAN_COLOR), and the saved current
    // turn present in the queue. A corrupt/tampered blob otherwise reads
    // gold[] out of bounds or spins switchTurn().
    if (b.queueLen < 2 || b.queueLen > MAX_PLAYERS)
        return false;
    bool seen[MAX_PLAYERS] = {false};
    bool turnInQueue = false;
    for (int i = 0; i < (int)b.queueLen; ++i)
    {
        uint8_t c = b.queue[i];
        if (c >= MAX_PLAYERS || seen[c])
            return false;
        seen[c] = true;
        if ((int32_t)c == b.turn)
            turnInQueue = true;
    }
    if (!turnInQueue || !seen[HUMAN_COLOR])
        return false;

    // Reload terrain (no briefing/cutscene -- they ran when this game was
    // first started). If the map won't load, or its size no longer
    // matches the snapshot (assets changed under an old save), bail
    // *before* touching live state -- startGame() has already bounced to
    // the mission menu. startGame() also rebuilds a skirmish queue from
    // the fresh map, but the memcpy of saved tiles + the queue/unit
    // restore below overwrite that with the snapshot's authoritative
    // state.
    if (!startGame((int)b.mapIndex, false, loadSkirmish)) // already bounced to the menu
        return false;
    if (!mapTiles || b.mapCells != (int32_t)((size_t)mapWidth * mapHeight))
    {
        appState = STATE_MENU; // caller redraws; don't leave a half-loaded game
        return false;
    }
    memcpy(mapTiles, b.tiles, (size_t)b.mapCells); // captured-building ownership

    currentTurn = (int)b.turn;
    for (int c = 0; c < MAX_PLAYERS; ++c)
    {
        gold[c] = b.gold[c];
        startingUnits[c] = b.startUnits[c];
        turnQueue[c] = b.queue[c];
        eliminated[c] = b.elim[c] != 0;
    }
    turnQueueLen = (int)b.queueLen;
    unitCount = (int)b.count;
    memcpy(units, b.units, sizeof(units));
    gameOver = b.over != 0;
    winnerColor = b.winner;
    selectedUnit = infoUnit = -1;
    shopOpen = false;
    shopBuyType = -1;
    viewX = b.viewX;
    viewY = b.viewY;
    clampView();
    return true;
}

// A brief centred toast (used for save confirmations / errors). Drawn now,
// wiped by the next drawViewport().
void toast(const char *msg)
{
    int w = (int)strlen(msg) * 6 + 16;
    int x = (DISPLAY_WIDTH - w) / 2, y = MAP_VIEW_Y + MAP_VIEW_H / 2 - 10;
    gfx.fillRoundRect(x, y, w, 20, 4, TFT_BLACK);
    gfx.drawRoundRect(x, y, w, 20, 4, TFT_WHITE);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setTextSize(1);
    gfx.setCursor(x + 8, y + 6);
    gfx.print(msg);
}

void drawPauseMenu()
{
    static const char *LABELS[PM_ROWS] = {"Return to game", "Save game", "Load game", "Exit to title"};
    gfx.fillRoundRect(PM_X, PM_Y, PM_W, PM_H, 6, 0x2945);
    gfx.drawRoundRect(PM_X, PM_Y, PM_W, PM_H, 6, TFT_WHITE);
    gfx.setTextSize(1);
    for (int i = 0; i < PM_ROWS; ++i)
    {
        int ry = PM_Y + PM_PAD + i * PM_ROW_H;
        bool disabled = (i == PM_LOAD && !hasSavedGame());
        gfx.fillRoundRect(PM_X + 6, ry, PM_W - 12, PM_ROW_H - 4, 4, disabled ? 0x2104 : TFT_DARKGREY);
        gfx.setTextColor(disabled ? 0x8410 : TFT_WHITE, disabled ? 0x2104 : TFT_DARKGREY);
        gfx.setCursor(PM_X + 16, ry + (PM_ROW_H - 4 - 8) / 2);
        gfx.print(LABELS[i]);
    }
}

// The shop overlay. Two modes:
//  - list (shopBuyType < 0): a panel of the buyable unit types + cost,
//    greyed if you can't afford it or the unit array is full.
//  - deploy (shopBuyType >= 0): the map (already drawn by drawViewport)
//    with every valid deploy tile outlined green and a hint in the footer.
constexpr uint16_t SHOP_BG = 0x2945;
void drawShop()
{
    gfx.setTextSize(1);

    if (shopBuyType >= 0)
    {
        gfx.setClipRect(0, MAP_VIEW_Y, DISPLAY_WIDTH, MAP_VIEW_H);
        int firstCol = viewX / TILE_SIZE, firstRow = viewY / TILE_SIZE;
        for (int c = 0; c <= DISPLAY_WIDTH / TILE_SIZE + 1; ++c)
            for (int r = 0; r <= MAP_VIEW_H / TILE_SIZE + 1; ++r)
            {
                int mx = firstCol + c, my = firstRow + r;
                if (!isShopDeployTile(mx, my, 0))
                    continue;
                int px = tileScreenX(mx), py = tileScreenY(my);
                gfx.drawRect(px, py, TILE_SIZE, TILE_SIZE, TFT_GREEN);
                gfx.drawRect(px + 1, py + 1, TILE_SIZE - 2, TILE_SIZE - 2, TFT_GREEN);
            }
        gfx.clearClipRect();

        gfx.fillRect(0, FOOTER_Y, DISPLAY_WIDTH, FOOTER_H, BAND_BG);
        gfx.drawFastHLine(0, FOOTER_Y, DISPLAY_WIDTH, BAND_EDGE);
        gfx.setTextColor(TFT_GREEN, BAND_BG);
        gfx.setCursor(5, FOOTER_Y + 8);
        gfx.printf("Deploy %s: tap a green tile", UNIT_TYPE_NAMES[shopBuyType]);
        gfx.setTextColor(0xAD55, BAND_BG);
        gfx.setCursor(5, FOOTER_Y + 24);
        gfx.print("tap anywhere else to cancel");
        return;
    }

    gfx.fillRoundRect(SHOP_X, SHOP_Y, SHOP_W, SHOP_H, 6, SHOP_BG);
    gfx.drawRoundRect(SHOP_X, SHOP_Y, SHOP_W, SHOP_H, 6, GOLD_COLOR);
    gfx.setTextColor(GOLD_COLOR, SHOP_BG);
    gfx.setCursor(SHOP_X + 8, SHOP_Y + 7);
    gfx.print("RECRUIT");
    char gs[12];
    snprintf(gs, sizeof(gs), "%ld", (long)gold[HUMAN_COLOR]);
    int gx = SHOP_X + SHOP_W - 8 - (int)strlen(gs) * 6;
    drawCoin(gx - 8, SHOP_Y + 11);
    gfx.setCursor(gx, SHOP_Y + 7);
    gfx.print(gs);

    for (int i = 0; i < SHOP_BUYABLE_COUNT; ++i)
    {
        int ry = SHOP_Y + SHOP_HEAD_H + i * SHOP_ROW_H;
        bool afford = gold[HUMAN_COLOR] >= UNIT_COST[i] && unitCount < MAX_UNITS;
        gfx.setTextColor(afford ? TFT_WHITE : 0x7BEF, SHOP_BG);
        gfx.setCursor(SHOP_X + 10, ry + 6);
        gfx.print(UNIT_TYPE_NAMES[i]);
        char cs[8];
        snprintf(cs, sizeof(cs), "%d", UNIT_COST[i]);
        gfx.setTextColor(afford ? GOLD_COLOR : 0x7BEF, SHOP_BG);
        gfx.setCursor(SHOP_X + SHOP_W - 12 - (int)strlen(cs) * 6, ry + 6);
        gfx.print(cs);
    }
}

void drawViewport()
{
    int firstCol = viewX / TILE_SIZE;
    int firstRow = viewY / TILE_SIZE;
    int cols = DISPLAY_WIDTH / TILE_SIZE + 2;
    int rows = MAP_VIEW_H / TILE_SIZE + 2;

    // Load every tile this frame needs from the SD card *before* opening the
    // display transaction below. SD/MMC and the display SPI bus are on
    // separate pins on this board, but keeping card I/O out of the
    // startWrite()/endWrite() block still avoids holding that transaction
    // open across slower, variable-latency card reads.
    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < cols; ++col)
        {
            int mx = firstCol + col;
            int my = firstRow + row;
            if (!inMapBounds(mx, my))
                continue;
            loadTile(tileAt(mx, my));
        }
    }
    for (int i = 0; i < unitCount; ++i)
    {
        if (units[i].alive &&
            units[i].tileX >= firstCol && units[i].tileX < firstCol + cols &&
            units[i].tileY >= firstRow && units[i].tileY < firstRow + rows)
        {
            loadUnitIcon(units[i].color, units[i].type);
        }
    }

    gfx.startWrite();

    // Everything map-related is confined to the viewport strip between the
    // header and footer -- clip so a tile or unit sprite at the edge can't
    // bleed into either band.
    gfx.setClipRect(0, MAP_VIEW_Y, DISPLAY_WIDTH, MAP_VIEW_H);

    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < cols; ++col)
        {
            int mx = firstCol + col;
            int my = firstRow + row;
            if (!inMapBounds(mx, my))
                continue; // leave the cleared background showing past the map edge

            uint8_t tile = tileAt(mx, my);
            int px = mx * TILE_SIZE - viewX;
            int py = tileScreenY(my);

            if (tile >= TILE_COUNT || !tileLoaded[tile])
            {
                // A failed/out-of-range tile would otherwise leave whatever
                // was drawn at this rectangle by a previous frame's scroll
                // position showing through.
                gfx.fillRect(px, py, TILE_SIZE, TILE_SIZE, TFT_BLACK);
                continue;
            }

            gfx.pushImage(px, py, TILE_SIZE, TILE_SIZE, tileFrame(tile));
        }
    }

    if (selectedUnit >= 0 && reachableCost)
    {
        for (int row = 0; row < rows; ++row)
        {
            for (int col = 0; col < cols; ++col)
            {
                int mx = firstCol + col;
                int my = firstRow + row;
                if (!inMapBounds(mx, my))
                    continue;
                if (reachableCost[mx * mapHeight + my] < 0)
                    continue;
                int px = mx * TILE_SIZE - viewX;
                int py = tileScreenY(my);
                gfx.drawRect(px, py, TILE_SIZE, TILE_SIZE, TFT_CYAN);
                gfx.drawRect(px + 1, py + 1, TILE_SIZE - 2, TILE_SIZE - 2, TFT_CYAN);
            }
        }
    }

    for (int i = 0; i < unitCount; ++i)
    {
        const UnitPlacement &u = units[i];
        if (!u.alive || !unitIconLoaded[u.color][u.type])
            continue; // dead, out of view this frame, or failed to load
        int px = u.tileX * TILE_SIZE - viewX;
        int py = tileScreenY(u.tileY);
        if (px <= -UNIT_ICON_SIZE || px >= DISPLAY_WIDTH ||
            py <= MAP_VIEW_Y - UNIT_ICON_SIZE || py >= MAP_VIEW_Y + MAP_VIEW_H)
            continue;
        // A unit that has used its action is done until its side's next
        // turn (switchTurn() clears hasMoved then) -- draw it desaturated +
        // dimmed so it reads as unavailable, keyed on hasMoved alone so
        // your spent units stay greyed through the AI's turn too. Per
        // pixel, so the sprite's transparent edges are untouched.
        const uint16_t *frame = unitIconFrame(u.color, u.type);
        if (u.hasMoved)
        {
            static uint16_t greyed[UNIT_ICON_SIZE * UNIT_ICON_SIZE];
            desaturateIcon(frame, greyed);
            frame = greyed;
        }
        gfx.pushImage(px, py, UNIT_ICON_SIZE, UNIT_ICON_SIZE, frame, TRANSPARENT_565);

        if (i == selectedUnit)
            gfx.drawRect(px, py, UNIT_ICON_SIZE, UNIT_ICON_SIZE, TFT_YELLOW);
        else if (selectedUnit >= 0 && u.color != currentTurn && canAttackThisTurn(units[selectedUnit], i))
            gfx.drawRect(px, py, UNIT_ICON_SIZE, UNIT_ICON_SIZE, TFT_RED); // attackable this turn (from here or after moving)

        if (u.health < 100)
        {
            int barY = py + UNIT_ICON_SIZE - 3;
            gfx.fillRect(px, barY, UNIT_ICON_SIZE, 3, TFT_BLACK);
            int filled = (UNIT_ICON_SIZE - 2) * u.health / 100;
            gfx.fillRect(px + 1, barY + 1, filled, 1, u.health > 33 ? TFT_GREEN : TFT_RED);
        }
    }

    gfx.clearClipRect();

    drawHud();

    if (gameOver)
    {
        bool noWinner = winnerColor < 0 || winnerColor >= MAX_PLAYERS;
        uint16_t winColor = noWinner ? TFT_WHITE : PLAYER_HUD_COLOR[winnerColor];
        bool humanAlive = false, otherAlive = false;
        for (int i = 0; i < unitCount; ++i)
        {
            if (!units[i].alive)
                continue;
            if (units[i].color == HUMAN_COLOR)
                humanAlive = true;
            else
                otherAlive = true;
        }
        char banner[20];
        if (!noWinner)
            snprintf(banner, sizeof(banner), "%s WINS", PLAYER_NAME[winnerColor]);
        else if (startingUnits[HUMAN_COLOR] > 0 && !humanAlive && otherAlive)
            strcpy(banner, "DEFEAT"); // human wiped out, 2+ other sides fight on
        else
            strcpy(banner, "DRAW"); // nobody left standing
        gfx.fillRect(0, BANNER_Y, DISPLAY_WIDTH, BANNER_H, TFT_BLACK);
        gfx.drawFastHLine(0, BANNER_Y, DISPLAY_WIDTH, winColor);
        gfx.drawFastHLine(0, BANNER_Y + BANNER_H - 1, DISPLAY_WIDTH, winColor);
        gfx.setTextSize(2);
        gfx.setTextColor(winColor, TFT_BLACK);
        gfx.setCursor((DISPLAY_WIDTH - (int)strlen(banner) * 12) / 2, BANNER_Y + 4);
        gfx.print(banner);
        gfx.setTextSize(1);

        // Retry the same mission without a trip through the menu -- tap
        // anywhere else on the banner still returns to the mission menu
        // (handleTap()'s existing gameOver behavior), this is just a
        // faster path back into the mission you just finished.
        gfx.fillRect(RETRY_BTN_X, RETRY_BTN_Y, RETRY_BTN_W, RETRY_BTN_H, TFT_DARKGREY);
        gfx.drawRect(RETRY_BTN_X, RETRY_BTN_Y, RETRY_BTN_W, RETRY_BTN_H, TFT_WHITE);
        gfx.setTextColor(TFT_WHITE, TFT_DARKGREY);
        gfx.setCursor(RETRY_BTN_X + 10, RETRY_BTN_Y + 5);
        gfx.print("RETRY");
    }

    if (pauseMenuOpen)
        drawPauseMenu();
    else if (shopOpen)
        drawShop();

    gfx.endWrite();
}

void clampView()
{
    int maxX = mapWidth * TILE_SIZE - DISPLAY_WIDTH;
    int maxY = mapHeight * TILE_SIZE - MAP_VIEW_H;
    viewX = constrain(viewX, 0, max(0, maxX));
    viewY = constrain(viewY, 0, max(0, maxY));
}

// Connects to WiFi (if secrets.h provides an SSID) and starts ArduinoOTA.
// Non-fatal on failure -- the game runs fine offline off the SD card, it
// just won't be updatable over the air until WiFi is reachable.
void setupOTA()
{
    if (strlen(WIFI_SSID) == 0)
    {
        Serial.println("no WIFI_SSID in secrets.h -- skipping WiFi/OTA");
        return;
    }

    gfx.println("connecting wifi...");
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    // The AP here intermittently answers association with AUTH_EXPIRE /
    // AUTH_FAIL on the first try even with correct credentials, so retry
    // the whole begin() a few times rather than waiting out one long
    // timeout. ~6s per attempt, 4 attempts.
    constexpr uint32_t WIFI_ATTEMPT_MS = 6000;
    constexpr int WIFI_ATTEMPTS = 4;
    for (int attempt = 1; attempt <= WIFI_ATTEMPTS && WiFi.status() != WL_CONNECTED; attempt++)
    {
        Serial.printf("wifi: attempt %d/%d\n", attempt, WIFI_ATTEMPTS);
        WiFi.disconnect(true);
        delay(100);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_ATTEMPT_MS)
        {
            delay(250);
        }
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi connect failed -- continuing offline");
        gfx.println("wifi failed, continuing offline");
        WiFi.mode(WIFI_OFF);
        return;
    }

    ArduinoOTA.setHostname("ae2rm");
    if (strlen(OTA_PASSWORD) > 0)
    {
        ArduinoOTA.setPassword(OTA_PASSWORD);
    }
    else
    {
        // Not fatal -- this is a hobby device, and requiring a password
        // would break the simplest "just try it on my LAN" path -- but
        // this must not be silent: anyone who can reach this device on
        // the network can flash it while OTA_PASSWORD is unset.
        Serial.println("WARNING: OTA_PASSWORD is empty -- OTA updates are unauthenticated");
        gfx.println("WARNING: OTA has no password!");
        delay(1500);
    }

    ArduinoOTA.onStart([]()
                        {
        otaInProgress = true;
        gfx.fillScreen(TFT_BLACK);
        gfx.setCursor(10, 10);
        gfx.println("OTA update...");
        Serial.println("OTA: start"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                           {
        int pct = total ? (progress * 100) / total : 0;
        gfx.fillRect(10, 40, 220, 20, TFT_BLACK);
        gfx.setCursor(10, 40);
        gfx.printf("%d%%", pct); });
    ArduinoOTA.onEnd([]()
                      { Serial.println("OTA: done, rebooting"); });
    ArduinoOTA.onError([](ota_error_t error)
                        {
        otaInProgress = false;
        Serial.printf("OTA error [%u]\n", error);
        gfx.println("OTA failed"); });

    ArduinoOTA.begin();

    Serial.printf("wifi connected: %s   OTA host: ae2rm.local\n", WiFi.localIP().toString().c_str());
    gfx.printf("wifi: %s\n", WiFi.localIP().toString().c_str());
    delay(500);
}

// Allocates tileCachePixels/unitIconCachePixels from PSRAM. Fatal if it
// fails -- board_pins.h/platformio.ini target a module with PSRAM, so a
// failure here means PSRAM didn't init, not that it's merely full.
bool allocAssetCaches()
{
    size_t tileBytes = (size_t)TILE_COUNT * TILE_SIZE * TILE_SIZE * sizeof(uint16_t);
    size_t unitBytes = (size_t)UNIT_COLOR_COUNT * UNIT_TYPE_COUNT * UNIT_ICON_SIZE * UNIT_ICON_SIZE * sizeof(uint16_t);

    tileCachePixels = static_cast<uint16_t *>(ps_malloc(tileBytes));
    unitIconCachePixels = static_cast<uint16_t *>(ps_malloc(unitBytes));

    if (!tileCachePixels || !unitIconCachePixels)
    {
        Serial.printf("PSRAM allocation failed (tiles %u bytes: %s, units %u bytes: %s)\n",
                       (unsigned)tileBytes, tileCachePixels ? "ok" : "FAILED",
                       (unsigned)unitBytes, unitIconCachePixels ? "ok" : "FAILED");
        return false;
    }
    return true;
}

// Full-screen touch calibration harness (TOUCH_DEBUG only). Blocks until
// the "DONE" box is tapped. Draws a reference frame with corner labels,
// then a live crosshair + coordinate readout at the reported touch point
// and a dot trail, so a mis-scaled / rotated / mirrored / dead touch
// panel is obvious just by pressing a known spot and reading the number.
void touchTest()
{
    gfx.fillScreen(TFT_BLACK);
    gfx.drawRect(0, 0, gfx.width(), gfx.height(), TFT_DARKGREY);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setTextSize(1);
    gfx.setCursor(4, 4);
    gfx.print("TL 0,0");
    gfx.setCursor(gfx.width() - 52, 4);
    gfx.printf("TR %d,0", gfx.width() - 1);
    gfx.setCursor(4, gfx.height() - 12);
    gfx.printf("BL 0,%d", gfx.height() - 1);
    gfx.setCursor(gfx.width() - 74, gfx.height() - 12);
    gfx.printf("BR %d,%d", gfx.width() - 1, gfx.height() - 1);

    // DONE box, top-centre.
    const int bw = 70, bh = 26, bx = (gfx.width() - bw) / 2, by = 2;
    auto drawDone = [&]() {
        gfx.fillRect(bx, by, bw, bh, TFT_RED);
        gfx.setTextColor(TFT_WHITE, TFT_RED);
        gfx.setTextSize(2);
        gfx.setCursor(bx + 10, by + 6);
        gfx.print("DONE");
    };
    drawDone();

    int32_t x, y;
    bool wasDown = false;
    while (true)
    {
        ArduinoOTA.handle();
        bool down = gfx.getTouch(&x, &y);
        if (down && !wasDown)
        {
            if (x >= bx && x < bx + bw && y >= by && y < by + bh)
                return;
            Serial.printf("[touchtest] %ld,%ld\n", (long)x, (long)y);
            gfx.fillRect(0, 40, gfx.width(), 40, TFT_BLACK);
            gfx.setTextColor(TFT_GREEN, TFT_BLACK);
            gfx.setTextSize(3);
            gfx.setCursor(10, 46);
            gfx.printf("%ld,%ld", (long)x, (long)y);
            gfx.drawFastHLine(x - 12, y, 25, TFT_YELLOW);
            gfx.drawFastVLine(x, y - 12, 25, TFT_YELLOW);
        }
        else if (down)
        {
            gfx.fillCircle(x, y, 2, TFT_CYAN);
        }
        wasDown = down;
        delay(16);
    }
}

void setup()
{
    Serial.begin(115200);
    delay(200);

    gfx.init();
    gfx.setRotation(0);
    gfx.setBrightness(200);
    // tools/convert_assets.py writes ordinary RGB565 values as a native
    // (little-endian) uint16_t -- e.g. red (0xF800) is stored as bytes
    // [0x00, 0xF8] -- matching tileCache's in-memory layout. The panel's
    // SPI protocol needs each pixel's MSB sent first, so pushImage() must
    // swap bytes for this data; set it explicitly rather than relying on
    // the library default. (An earlier version of this line had the
    // swap backwards -- see PR review history.)
    gfx.setSwapBytes(true);
    gfx.fillScreen(TFT_BLACK);
    gfx.setTextColor(TFT_WHITE);
    gfx.setTextSize(2);
    gfx.setCursor(10, 10);
    gfx.println("AE2RM ESP32");
    gfx.println("booting...");

    if (!allocAssetCaches())
    {
        gfx.println("PSRAM alloc failed!");
        while (true)
            delay(1000);
    }

    // WiFi/OTA first, and before the SD gate below: a missing or
    // miswired card must not cost us the wireless-recovery path.
    setupOTA(); // best-effort; game runs offline if this doesn't connect

    // SD card is wired as SD/MMC 4-bit, on its own dedicated pins (not
    // shared with the display's SPI bus).
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3);
    if (SD_MMC.begin())
    {
        sdReady = true;
    }
    else
    {
        Serial.println("SD init failed");
        gfx.println("SD init failed!");
    }

    // Playable as long as *some* asset source exists: the card, or assets
    // baked into this build by tools/convert_assets.py.
    assetsReady = sdReady || haveEmbeddedAssets();
    if (!assetsReady)
    {
        gfx.println("no game assets");
        gfx.println("(OTA still available)");
        Serial.println("no assets: SD failed and none embedded -- OTA only");
        return; // fall through to loop(); ArduinoOTA.handle() keeps working
    }
    if (!sdReady)
    {
        gfx.println("using built-in assets");
        Serial.println("SD unavailable -- running on embedded assets");
    }

    loadStrings(); // best-effort; see its comment -- missing strings.dat degrades, doesn't block boot

    if (TOUCH_DEBUG)
        touchTest();

    showTitleScreen(); // best-effort; see its comment -- missing assets skip straight to the menu

    drawMenu();
}

// A touch that never strays more than this many pixels from where it
// started is a tap (select/move/END TURN); anything that moves further is
// a drag (pan the camera). Chosen to comfortably exceed capacitive-touch
// jitter on a stationary finger without feeling laggy for an intentional
// drag -- unverified on hardware, may need tuning once you can feel it.
constexpr int32_t TAP_MOVE_THRESHOLD = 8;

void loop()
{
    static int32_t touchStartX = -1, touchStartY = -1;
    static int32_t lastX = -1, lastY = -1;
    static bool isDrag = false;
    int32_t x, y;

    ArduinoOTA.handle();
    if (otaInProgress)
    {
        // Don't touch the display/SD from the game loop while a flash
        // write is in progress.
        return;
    }


    // No usable asset source: keep servicing OTA (above) but run no game logic.
    if (!assetsReady)
    {
        delay(50);
        return;
    }

    if (gfx.getTouch(&x, &y))
    {
        if (lastX < 0)
        {
            touchStartX = x;
            touchStartY = y;
            isDrag = false;
        }
        // The menu has nothing to drag-pan, so only STATE_PLAYING
        // distinguishes a tap from a drag; every menu touch is a tap. A
        // gesture that begins in the header/footer band, or over the pause
        // menu / shop list, is never a pan. Shop *deploy* mode still pans
        // so you can reach an off-screen deploy tile.
        else if (appState == STATE_PLAYING && !pauseMenuOpen &&
                 !(shopOpen && shopBuyType < 0) &&
                 touchStartY >= MAP_VIEW_Y && touchStartY < MAP_VIEW_Y + MAP_VIEW_H)
        {
            if (!isDrag && (abs(x - touchStartX) > TAP_MOVE_THRESHOLD || abs(y - touchStartY) > TAP_MOVE_THRESHOLD))
                isDrag = true;
            if (isDrag)
            {
                // "Grab the map" panning: drag right and the map content
                // follows your finger (the viewport's left edge moves
                // left). The earlier inverted feel was the 180-deg touch
                // mismatch, now fixed in LGFX_Config.h.
                viewX -= (x - lastX);
                viewY -= (y - lastY);
                clampView();
                drawViewport();
            }
        }
        lastX = x;
        lastY = y;
    }
    else
    {
        if (lastX >= 0 && !isDrag)
        {
            if (appState == STATE_MENU)
                handleMenuTap(lastX, lastY);
            else
                handleTap(lastX, lastY);
        }
        lastX = lastY = -1;
    }

    delay(16);
}
