// AE2RM ESP32 port -- milestone 14: single-player vs. a basic AI, plus
// m0's mission-script intro cutscene.
//
// Boots to a mission menu (m0.aem-m7.aem, showing each map's real title
// when /strings.dat loaded -- see loadStrings() -- else a generic
// "Mission N" label). Tap one to play: you are always blue, the computer
// is always red. m0 additionally runs its mission-script's intro cutscene
// once at mission start (see runIntroScript()) before handing control to
// you -- real dialog text, a couple of scripted unit moves/removals, and
// camera pans; the other 7 maps start playable immediately. Tap a unit
// belonging to the current turn's side to select it -- its movement range
// highlights cyan (terrain-cost-limited flood fill) and any enemy already
// in its attack range highlights red. Tap a cyan tile to move there
// (capturing a village/castle you move onto, if this unit type can), tap
// a red-highlighted enemy to attack it (one action per unit per turn --
// this is "move OR attack", not the original's "move then attack"), tap
// "END TURN" to pass to the AI, which immediately plays its whole turn
// (attack a guaranteed kill if one's in range, else the weakest in-range
// enemy, else move-and-attack, else retreat if critically low on health,
// else close the distance -- see aiActUnit()'s comment; not a port of
// the original's scoring-heuristic AI) and hands back control. A
// living-unit-count readout next to the
// turn indicator (blue:red, colors 0/1 only) tracks how the battle
// stands. Tap any other living unit (an enemy, or one that's already
// moved) to see its stats instead. A side loses when
// its king dies -- a simplification of the original's
// castle-capture-tied defeat condition, see README -- and the win banner
// has a RETRY button that reloads the same mission directly; tapping
// anywhere else on the banner, or the always-available MENU button,
// returns to the mission menu. Drag still pans the camera during a
// mission. See firmware/README.md for what's implemented and what's
// next.

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "LGFX_Config.h"
#include "board_pins.h"

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

constexpr int TILE_SIZE = 24;
constexpr int TILE_COUNT = 48;

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

// Same UNIT_TYPE_COUNT order as above, for the tap-to-inspect stat panel.
static const char *const UNIT_TYPE_NAMES[UNIT_TYPE_COUNT] = {
    "SOLDIER", "ARCHER", "LIZARD", "WIZARD", "WISP", "SPIDER",
    "GOLEM", "CATAPULT", "WYVERN", "KING", "SKELETON", "CRYSTAL",
};

// Bit flags from each unit's "HasProperty N" lines (UNIT_PROPERTIES[i] =
// OR of 1<<N for each line). Only the two capture-related bits are used by
// this milestone: bit 3 (0x08, "can capture a village") is soldier and
// king; bit 4 (0x10, "can capture a castle") is king only -- no other unit
// in this tileset can take a castle. The other bits (mounted/flying
// matchup bonuses, water bonuses, etc.) exist in the source data but
// aren't read here since milestone 4 didn't port the combat bonuses that
// use them.
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

static bool gameOver = false;
static int winnerColor = -1; // 0 or 1, matches UnitPlacement::color

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

// 0 or 1, matching UnitPlacement::color directly (see the comment on that
// struct). Color 1 is always AI-controlled (see AI_COLOR below) -- there
// is no two-human hotseat mode anymore, endTurn() always resolves color
// 1's turn through the AI synchronously before returning control.
static int currentTurn = 0;

static int viewX = 0; // top-left of the viewport, in pixels, into the map
static int viewY = 0;

// m0.aem .. m7.aem -- the story maps loadMap()'s hardcoded 2-side turn
// queue is exact for (see UnitPlacement's comment). Skirmish maps
// (s0-s11) aren't converted or offered here.
constexpr int STORY_MAP_COUNT = 8;

enum AppState
{
    STATE_MENU,
    STATE_PLAYING,
};
static AppState appState = STATE_MENU;

bool loadTile(int index)
{
    if (index < 0 || index >= TILE_COUNT)
        return false;
    if (tileLoaded[index])
        return true;

    char path[48];
    snprintf(path, sizeof(path), "/tiles0/tile_%02d.bin", index);
    File f = SD_MMC.open(path, FILE_READ);
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
    File f = SD_MMC.open(path, FILE_READ);
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
    File f = SD_MMC.open(path, FILE_READ);
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

// One hit: attackerIdx's roll in [offenceMin, offenceMax) against
// victimIdx's defence (base + terrain bonus), scaled by the attacker's
// current health%. Shared by attackUnit()'s direct hit and its
// counterattack. Deliberately does NOT port the original's per-property
// matchup bonuses (mounted-vs-ground, golem-vs-skeleton, water/swamp
// bonuses, etc. -- see Unit.java's getOffenceBonusAgainstUnit()):
// UNIT_PROPERTIES is read elsewhere for capture eligibility, but the
// matchup-bonus bits it also carries aren't interpreted here.
void resolveHit(int attackerIdx, int victimIdx)
{
    UnitPlacement &attacker = units[attackerIdx];
    UnitPlacement &victim = units[victimIdx];

    int offence = random(UNIT_OFFENCE_MIN[attacker.type], UNIT_OFFENCE_MAX[attacker.type]);
    uint8_t victimTile = tileAt(victim.tileX, victim.tileY);
    // Same guard as computeReachable()'s: a corrupt/out-of-range tile index
    // shouldn't read past TILE_TERRAIN_TYPE/TERRAIN_DEFENCE_BONUS -- treat
    // it as no terrain bonus rather than crashing mid-combat.
    int terrainBonus = victimTile < TILE_COUNT ? TERRAIN_DEFENCE_BONUS[TILE_TERRAIN_TYPE[victimTile]] : 0;
    int defence = UNIT_DEFENCE[victim.type] + terrainBonus;

    int hit = (offence - defence) * attacker.health / 100;
    hit = constrain(hit, 0, (int)victim.health);

    victim.health -= hit;
    Serial.printf("attack: unit %d (type %d) hits unit %d (type %d) for %d (hp now %d)\n",
                  attackerIdx, attacker.type, victimIdx, victim.type, hit, victim.health);

    if (victim.health == 0)
    {
        victim.alive = false;
        // Win condition: a side loses when its king dies. This is a
        // simplification -- the original ties defeat to castle capture as
        // much as king death, tracked through fractionKings/turn-queue
        // bookkeeping this milestone doesn't port -- but king death is
        // the clearest single condition to key off without that machinery.
        if (victim.type == UNIT_TYPE_KING)
        {
            gameOver = true;
            winnerColor = attacker.color;
            Serial.printf("game over: color %d wins (king killed)\n", winnerColor);
        }
    }
}

// Resolves attackerIdx attacking victimIdx, then victimIdx's counterattack
// if it's still alive and eligible -- matching Unit.java's
// canPerformCloseAttack(): adjacent (distance == 1, regardless of the
// attacker's own attack range) AND the victim's own MIN_ATTACK_RANGE is 1
// (a ranged-only unit like the catapult, MIN_ATTACK_RANGE 2, can never
// counter). Not ported: canPerformCloseAttack() also checks the victim's
// unitState != 4, a status-effect flag this milestone doesn't model.
void attackUnit(int attackerIdx, int victimIdx)
{
    resolveHit(attackerIdx, victimIdx);

    UnitPlacement &attacker = units[attackerIdx];
    UnitPlacement &victim = units[victimIdx];
    if (victim.alive &&
        manhattanDist(victim.tileX, victim.tileY, attacker.tileX, attacker.tileY) == 1 &&
        UNIT_ATTACK_RANGE_MIN[victim.type] == 1)
    {
        resolveHit(victimIdx, attackerIdx);
    }
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

    File f = SD_MMC.open("/strings.dat", FILE_READ);
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

constexpr int MENU_ROW_H = 28;
constexpr int MENU_ROW_TOP = 50;

void drawMenu()
{
    gfx.startWrite();
    gfx.fillScreen(TFT_BLACK);
    gfx.setTextSize(2);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setCursor(20, 10);
    gfx.print("AE2RM");
    gfx.setTextSize(1);
    gfx.setCursor(20, 30);
    gfx.print("select a mission");

    // Locale string 121+i is mission i's real title (see loadStrings()'s
    // comment); loadStrings() is called once from setup(), so this is
    // just a lookup, not a fresh SD read per row. A missing/failed-to-load
    // strings.dat falls back to a generic "Mission N" label instead of an
    // empty row.
    constexpr int MISSION_TITLE_STRING_BASE = 121;
    for (int i = 0; i < STORY_MAP_COUNT; ++i)
    {
        int rowY = MENU_ROW_TOP + i * MENU_ROW_H;
        gfx.fillRect(20, rowY, DISPLAY_WIDTH - 40, MENU_ROW_H - 6, TFT_DARKGREY);
        gfx.drawRect(20, rowY, DISPLAY_WIDTH - 40, MENU_ROW_H - 6, TFT_WHITE);
        gfx.setTextSize(2);
        gfx.setTextColor(TFT_WHITE, TFT_DARKGREY);
        gfx.setCursor(30, rowY + 6);
        const char *title = getScriptString(MISSION_TITLE_STRING_BASE + i);
        if (title[0])
            gfx.print(title);
        else
            gfx.printf("Mission %d", i + 1);
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
    const int maxChars = (DISPLAY_WIDTH - 2 * PAD) / CHAR_W;

    gfx.startWrite();
    gfx.fillRect(0, BOX_Y, DISPLAY_WIDTH, BOX_H, TFT_BLACK);
    gfx.drawRect(0, BOX_Y, DISPLAY_WIDTH, BOX_H, TFT_WHITE);
    gfx.setTextSize(1);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);

    // Greedy word-wrap, good enough for these short dialog lines -- not
    // general typesetting. A single word longer than maxChars (doesn't
    // happen in this game's English dialog) would just get hard-split.
    int row = 0;
    const char *p = text;
    char lineBuf[64];
    constexpr int MAX_ROWS = (BOX_H - PAD - LINE_H - 4) / LINE_H; // leaves room for the prompt line
    while (*p && row < MAX_ROWS)
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
        gfx.setCursor(PAD, BOX_Y + PAD + row * LINE_H);
        gfx.print(lineBuf);
        ++row;
        p = (*scan == '\n') ? scan + 1 : scan;
        while (*p == ' ')
            ++p;
    }

    gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
    gfx.setCursor(PAD, BOX_Y + BOX_H - LINE_H - 2);
    gfx.print("[tap to continue]");
    gfx.endWrite();

    // ArduinoOTA.handle() is normally only serviced from loop() -- this
    // blocking wait has to call it itself, or leaving a dialog open would
    // make the board unreachable over OTA for as long as the player
    // doesn't tap. Bailing out on otaInProgress (rather than continuing to
    // poll touch/redraw) matches loop()'s own rule of leaving the
    // display/SD alone once a flash write has actually started.
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
// in this port (no fade/cursor-sprite/particle system, no per-tile
// movement animation to pace) are silently skipped, not simulated:
// ShowMapName, NextState, SetFadeEnabled, SetFadeValue, SetCursorVisible,
// SetMapStepMax, SetUnitSpeed, Vibrate, ScheduleUnitAnimationStop,
// CreateSpriteAtUnit, StartPlay.
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

    File f = SD_MMC.open("/scripts/m0.script", FILE_READ);
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
            viewX = tx * TILE_SIZE - DISPLAY_WIDTH / 2;
            viewY = ty * TILE_SIZE - DISPLAY_HEIGHT / 2;
            clampView();
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
        else if (!strcmp(tok[0], "ShowDialog") && n >= 2)
        {
            drawViewport(); // make sure what's on screen is current before covering part of it
            showScriptDialog(getScriptString(atoi(tok[1])));
        }
        else if (!strcmp(tok[0], "Wait") && n >= 2)
        {
            // The original paces this against its own tick rate; there's
            // no equivalent tick here, so this is a plain approximate
            // delay, not a faithful conversion of "N ticks".
            constexpr unsigned long MS_PER_WAIT_TICK = 80;
            delay((unsigned long)atoi(tok[1]) * MS_PER_WAIT_TICK);
        }
        // Every other command in this case range (ShowMapName, NextState,
        // SetFadeEnabled, SetFadeValue, SetCursorVisible, SetMapStepMax,
        // SetUnitSpeed, Vibrate, ScheduleUnitAnimationStop,
        // CreateSpriteAtUnit, StartPlay) has no equivalent here -- see this
        // function's doc comment -- and is silently skipped.
    }
    f.close();
    if (!otaInProgress)
        drawViewport();
}

// Loads m<mapIndex>.aem and resets all per-game state, then switches to
// STATE_PLAYING. Asset caches (tiles/unit icons) are content-independent
// across maps and are deliberately NOT reset here.
void startGame(int mapIndex)
{
    char path[24];
    snprintf(path, sizeof(path), "/maps/m%d.aem", mapIndex);

    gfx.fillScreen(TFT_BLACK);
    gfx.setTextSize(2);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setCursor(10, 10);
    gfx.printf("loading mission %d...", mapIndex + 1);

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
        return;
    }

    currentMapIndex = mapIndex;
    currentTurn = 0;
    selectedUnit = -1;
    infoUnit = -1;
    gameOver = false;
    winnerColor = -1;
    viewX = 0;
    viewY = 0;

    appState = STATE_PLAYING;
    gfx.fillScreen(TFT_BLACK);
    drawViewport();

    if (mapIndex == 0)
        runIntroScript(); // only m0 has a mission-script file -- see its comment
}

void handleMenuTap(int screenX, int screenY)
{
    for (int i = 0; i < STORY_MAP_COUNT; ++i)
    {
        int rowY = MENU_ROW_TOP + i * MENU_ROW_H;
        if (screenX >= 20 && screenX < DISPLAY_WIDTH - 20 && screenY >= rowY && screenY < rowY + MENU_ROW_H - 6)
        {
            startGame(i);
            return;
        }
    }
}

void tryCaptureBuilding(const UnitPlacement &u); // defined below; used by the AI first

// Color 1 (red) is always the computer side; color 0 (blue) is always the
// human. There's no way to flip this, and no way to disable it either --
// endTurn() below always auto-resolves color 1's turn through the AI, so
// the two-human hotseat mode earlier milestones had no longer applies.
constexpr int AI_COLOR = 1;

// True if attackerType/attackerHealth attacking victimIdx is guaranteed to
// kill it no matter how resolveHit()'s random() roll comes out -- using
// UNIT_OFFENCE_MIN (the floor of that roll) against the same
// terrain-adjusted defence resolveHit() itself computes, scaled by
// attacker health the same way. A worst-case-roll kill is a genuinely
// certain one, not a probability estimate.
bool wouldGuaranteeKill(uint8_t attackerType, uint8_t attackerHealth, int victimIdx)
{
    const UnitPlacement &victim = units[victimIdx];
    uint8_t tile = tileAt(victim.tileX, victim.tileY);
    int terrainBonus = tile < TILE_COUNT ? TERRAIN_DEFENCE_BONUS[TILE_TERRAIN_TYPE[tile]] : 0;
    int defence = UNIT_DEFENCE[victim.type] + terrainBonus;
    int minHit = (UNIT_OFFENCE_MIN[attackerType] - defence) * attackerHealth / 100;
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

        bool isKill = wouldGuaranteeKill(type, health, i);
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

// A deliberately simple AI move for one unit, evaluated in this priority:
//   1. Attack an enemy already in range from the current tile.
//   2. Otherwise, if some reachable tile puts an enemy in range, move
//      there and attack (this AI is allowed the original's "move then
//      attack" -- see the note on handleTap() for why human play doesn't
//      get that). Every reachable attack-capable tile is checked, not just
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
                bool isKill = wouldGuaranteeKill(u.type, u.health, tileTarget);
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

void runAITurn()
{
    for (int i = 0; i < unitCount; ++i)
    {
        if (units[i].alive && units[i].color == currentTurn && !units[i].hasMoved)
            aiActUnit(i);
        if (gameOver) // e.g. this AI unit just killed the human king
            return;
    }
}

void switchTurn()
{
    currentTurn = 1 - currentTurn;
    for (int i = 0; i < unitCount; ++i)
    {
        if (units[i].color == currentTurn)
            units[i].hasMoved = false;
    }
    selectedUnit = -1;
}

void endTurn()
{
    switchTurn();
    Serial.printf("turn: %s\n", currentTurn == 0 ? "blue" : "red");

    if (currentTurn == AI_COLOR && !gameOver)
    {
        runAITurn();
        if (!gameOver)
        {
            switchTurn();
            Serial.println("turn: blue (AI done)");
        }
    }

    drawViewport();
}

constexpr int HUD_BTN_W = 76;
constexpr int HUD_BTN_H = 22;
constexpr int HUD_BTN_X = DISPLAY_WIDTH - HUD_BTN_W - 4;
constexpr int HUD_BTN_Y = DISPLAY_HEIGHT - HUD_BTN_H - 4;

// Always-available way back to the mission menu, independent of gameOver.
// Without this, a mission whose win condition can never trigger (m4/m6
// place no red units in their map data -- see README) would trap the
// player in STATE_PLAYING with no way out.
constexpr int MENU_BTN_W = 50;
constexpr int MENU_BTN_H = 16;
constexpr int MENU_BTN_X = DISPLAY_WIDTH - MENU_BTN_W - 4;
constexpr int MENU_BTN_Y = 2;

// Win/loss banner geometry, and its RETRY button -- shared between
// drawViewport() (drawing it) and handleTap() (hit-testing it), so both
// use the same constants rather than each computing the layout itself.
constexpr int BANNER_H = 20;
constexpr int BANNER_Y = (DISPLAY_HEIGHT - BANNER_H) / 2;
constexpr int RETRY_BTN_W = 70;
constexpr int RETRY_BTN_H = 18;
constexpr int RETRY_BTN_X = (DISPLAY_WIDTH - RETRY_BTN_W) / 2;
constexpr int RETRY_BTN_Y = BANNER_Y + BANNER_H + 8;

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
    if (screenX >= MENU_BTN_X && screenX < MENU_BTN_X + MENU_BTN_W &&
        screenY >= MENU_BTN_Y && screenY < MENU_BTN_Y + MENU_BTN_H)
    {
        infoUnit = -1;
        appState = STATE_MENU;
        drawMenu();
        return;
    }

    if (gameOver)
    {
        if (screenX >= RETRY_BTN_X && screenX < RETRY_BTN_X + RETRY_BTN_W &&
            screenY >= RETRY_BTN_Y && screenY < RETRY_BTN_Y + RETRY_BTN_H)
        {
            startGame(currentMapIndex);
            return;
        }
        // Any other tap while the win banner is up returns to the menu.
        appState = STATE_MENU;
        drawMenu();
        return;
    }

    if (screenX >= HUD_BTN_X && screenX < HUD_BTN_X + HUD_BTN_W &&
        screenY >= HUD_BTN_Y && screenY < HUD_BTN_Y + HUD_BTN_H)
    {
        infoUnit = -1;
        endTurn();
        return;
    }

    int mx = (screenX + viewX) / TILE_SIZE;
    int my = (screenY + viewY) / TILE_SIZE;
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
        // Attacking is a standalone action from the unit's current tile --
        // this is "move OR attack" per turn, not the original's "move then
        // attack". aiActUnit() below shows the underlying tracking
        // (reachable-attack range from every tile in the move range,
        // not just the current one) is doable; this is a deliberate
        // simplification for human play, not a technical limit -- doing
        // it live as a human drags a selection around (highlighting
        // which reachable tiles also open an attack) is more UI than
        // this milestone scoped.
        if (targetIdx >= 0 && units[targetIdx].color != currentTurn && inAttackRange(units[selectedUnit], mx, my))
        {
            attackUnit(selectedUnit, targetIdx);
            units[selectedUnit].hasMoved = true;
        }
        else if (reachableCost && reachableCost[mx * mapHeight + my] >= 0 && targetIdx < 0)
        {
            units[selectedUnit].tileX = (int16_t)mx;
            units[selectedUnit].tileY = (int16_t)my;
            units[selectedUnit].hasMoved = true;
            tryCaptureBuilding(units[selectedUnit]);
        }
        else if (targetIdx >= 0)
        {
            // Tapped a unit that wasn't a valid attack target from here (out
            // of range, or on the current player's own side) -- deselecting
            // silently would make tap-to-inspect need a second tap on any
            // unit tapped while another was already selected. Show its
            // panel instead, same as tapping it with nothing selected would.
            infoUnit = targetIdx;
        }
        selectedUnit = -1;
        drawViewport();
        return;
    }

    int idx = unitIndexAt(mx, my);
    if (idx >= 0 && units[idx].color == currentTurn && !units[idx].hasMoved)
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

void drawViewport()
{
    int firstCol = viewX / TILE_SIZE;
    int firstRow = viewY / TILE_SIZE;
    int cols = DISPLAY_WIDTH / TILE_SIZE + 2;
    int rows = DISPLAY_HEIGHT / TILE_SIZE + 2;

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
            int py = my * TILE_SIZE - viewY;

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
                int py = my * TILE_SIZE - viewY;
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
        int py = u.tileY * TILE_SIZE - viewY;
        if (px <= -UNIT_ICON_SIZE || py <= -UNIT_ICON_SIZE || px >= DISPLAY_WIDTH || py >= DISPLAY_HEIGHT)
            continue;
        gfx.pushImage(px, py, UNIT_ICON_SIZE, UNIT_ICON_SIZE, unitIconFrame(u.color, u.type), TRANSPARENT_565);

        if (i == selectedUnit)
            gfx.drawRect(px, py, UNIT_ICON_SIZE, UNIT_ICON_SIZE, TFT_YELLOW);
        else if (selectedUnit >= 0 && u.color != currentTurn && inAttackRange(units[selectedUnit], u.tileX, u.tileY))
            gfx.drawRect(px, py, UNIT_ICON_SIZE, UNIT_ICON_SIZE, TFT_RED); // valid attack target this turn

        if (u.health < 100)
        {
            int barY = py + UNIT_ICON_SIZE - 3;
            gfx.fillRect(px, barY, UNIT_ICON_SIZE, 3, TFT_BLACK);
            int filled = (UNIT_ICON_SIZE - 2) * u.health / 100;
            gfx.fillRect(px + 1, barY + 1, filled, 1, u.health > 33 ? TFT_GREEN : TFT_RED);
        }
    }

    // HUD: fixed screen-space overlay, always on top, not affected by scroll.
    uint16_t turnColor = currentTurn == 0 ? TFT_BLUE : TFT_RED;
    gfx.fillRect(0, 0, 90, 16, TFT_BLACK);
    gfx.setTextColor(turnColor, TFT_BLACK);
    gfx.setTextSize(1);
    gfx.setCursor(2, 4);
    gfx.print(currentTurn == 0 ? "BLUE TURN" : "RED TURN");

    // Living-unit counts, both sides -- how the battle stands at a glance,
    // updated every redraw since a unit can die on any turn (yours or the
    // AI's). Only colors 0 (blue)/1 (red) are counted: story maps never
    // place units of the other two team colors this port's asset pipeline
    // converts (see the team-color note in firmware/README.md), so a
    // green/black count would always read 0 here.
    int blueAlive = 0, redAlive = 0;
    for (int i = 0; i < unitCount; ++i)
    {
        if (!units[i].alive)
            continue;
        if (units[i].color == 0)
            ++blueAlive;
        else if (units[i].color == 1)
            ++redAlive;
    }
    constexpr int UNIT_COUNT_X = 96;
    gfx.fillRect(UNIT_COUNT_X, 0, MENU_BTN_X - UNIT_COUNT_X, 16, TFT_BLACK);
    gfx.setCursor(UNIT_COUNT_X, 4);
    gfx.setTextColor(TFT_BLUE, TFT_BLACK);
    gfx.print(blueAlive);
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.print(":");
    gfx.setTextColor(TFT_RED, TFT_BLACK);
    gfx.print(redAlive);

    gfx.fillRect(MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H, TFT_DARKGREY);
    gfx.drawRect(MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H, TFT_WHITE);
    gfx.setTextColor(TFT_WHITE, TFT_DARKGREY);
    gfx.setCursor(MENU_BTN_X + 6, MENU_BTN_Y + 4);
    gfx.print("MENU");

    gfx.fillRect(HUD_BTN_X, HUD_BTN_Y, HUD_BTN_W, HUD_BTN_H, TFT_DARKGREY);
    gfx.drawRect(HUD_BTN_X, HUD_BTN_Y, HUD_BTN_W, HUD_BTN_H, TFT_WHITE);
    gfx.setTextColor(TFT_WHITE, TFT_DARKGREY);
    gfx.setCursor(HUD_BTN_X + 6, HUD_BTN_Y + 7);
    gfx.print("END TURN");

    // Tap-to-inspect stat panel (see infoUnit's comment) -- bottom-left,
    // clear of the END TURN button on the bottom-right. A unit that died
    // or otherwise vanished since the tap (e.g. an AI turn ran) just stops
    // showing rather than reading stale/invalid data.
    if (infoUnit >= 0 && (infoUnit >= unitCount || !units[infoUnit].alive))
        infoUnit = -1;
    if (infoUnit >= 0)
    {
        const UnitPlacement &iu = units[infoUnit];
        constexpr int PANEL_W = HUD_BTN_X - 8;
        constexpr int PANEL_H = 40;
        constexpr int PANEL_X = 4;
        constexpr int PANEL_Y = DISPLAY_HEIGHT - PANEL_H - 4;
        uint16_t ownerColor = iu.color == 0 ? TFT_BLUE : TFT_RED;
        gfx.fillRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, TFT_BLACK);
        gfx.drawRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, ownerColor);
        gfx.setTextColor(TFT_WHITE, TFT_BLACK);
        gfx.setCursor(PANEL_X + 4, PANEL_Y + 3);
        gfx.printf("%s HP %d", UNIT_TYPE_NAMES[iu.type], iu.health);
        gfx.setCursor(PANEL_X + 4, PANEL_Y + 13);
        gfx.printf("ATK %d-%d DEF %d", UNIT_OFFENCE_MIN[iu.type], UNIT_OFFENCE_MAX[iu.type], UNIT_DEFENCE[iu.type]);
        gfx.setCursor(PANEL_X + 4, PANEL_Y + 23);
        gfx.printf("RANGE %d-%d MOVE %d", UNIT_ATTACK_RANGE_MIN[iu.type], UNIT_ATTACK_RANGE_MAX[iu.type], UNIT_MOVE_RANGE[iu.type]);
    }

    if (gameOver)
    {
        uint16_t winColor = winnerColor == 0 ? TFT_BLUE : TFT_RED;
        gfx.fillRect(0, BANNER_Y, DISPLAY_WIDTH, BANNER_H, TFT_BLACK);
        gfx.drawFastHLine(0, BANNER_Y, DISPLAY_WIDTH, winColor);
        gfx.drawFastHLine(0, BANNER_Y + BANNER_H - 1, DISPLAY_WIDTH, winColor);
        gfx.setTextSize(2);
        gfx.setTextColor(winColor, TFT_BLACK);
        gfx.setCursor(30, BANNER_Y + 4);
        gfx.print(winnerColor == 0 ? "BLUE WINS" : "RED WINS");
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

    gfx.endWrite();
}

void clampView()
{
    int maxX = mapWidth * TILE_SIZE - DISPLAY_WIDTH;
    int maxY = mapHeight * TILE_SIZE - DISPLAY_HEIGHT;
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
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    constexpr uint32_t WIFI_TIMEOUT_MS = 10000;
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS)
    {
        delay(250);
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi connect timed out -- continuing offline");
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

    // SD card is wired as SD/MMC 4-bit, on its own dedicated pins (not
    // shared with the display's SPI bus).
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3);
    if (!SD_MMC.begin())
    {
        gfx.println("SD init failed!");
        Serial.println("SD init failed");
        while (true)
            delay(1000);
    }

    setupOTA(); // best-effort; game runs offline if this doesn't connect

    loadStrings(); // best-effort; see its comment -- missing strings.dat degrades, doesn't block boot

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

    if (gfx.getTouch(&x, &y))
    {
        if (lastX < 0)
        {
            touchStartX = x;
            touchStartY = y;
            isDrag = false;
        }
        // The menu has nothing to drag-pan, so only STATE_PLAYING
        // distinguishes a tap from a drag; every menu touch is a tap.
        else if (appState == STATE_PLAYING)
        {
            if (!isDrag && (abs(x - touchStartX) > TAP_MOVE_THRESHOLD || abs(y - touchStartY) > TAP_MOVE_THRESHOLD))
                isDrag = true;
            if (isDrag)
            {
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
