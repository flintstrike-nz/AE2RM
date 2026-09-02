// AE2RM ESP32 port -- milestone 4: local hotseat movement + combat, no AI.
//
// Tap a unit belonging to the current turn's side to select it: its
// movement range highlights cyan (terrain-cost-limited flood fill) and any
// enemy already in its attack range highlights red. Tap a cyan tile to
// move there, tap a red-highlighted enemy to attack it (one action per
// unit per turn -- this is "move OR attack", not the original's "move
// then attack"), tap "END TURN" to pass to the other side. Still no AI
// (two-human hotseat only -- see README for why), no buildings/capture,
// no menus, and only map m0. Drag still pans the camera.
// See firmware/README.md for what's implemented and what's next.

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

// Movement points remaining when the currently selected unit could reach
// [x*mapHeight+y], or -1 if unreachable. Lazily allocated/resized to match
// the current map's dimensions inside computeReachable() (not in loadMap()
// -- there's no selected unit yet when a map loads), and recomputed there
// each time a unit is selected.
static int8_t *reachableCost = nullptr;
static int selectedUnit = -1; // index into units[], or -1 if none selected

// Two-side hotseat only: 0 or 1, matching UnitPlacement::color directly
// (see the comment on that struct). There is no AI opponent -- the "other"
// side is just the second human player's turn. Porting the original's AI
// (a large scoring heuristic across much of MainDisplayable.java) is out
// of scope for this milestone.
static int currentTurn = 0;

static int viewX = 0; // top-left of the viewport, in pixels, into the map
static int viewY = 0;

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

bool inAttackRange(const UnitPlacement &attacker, int tx, int ty)
{
    int d = manhattanDist(attacker.tileX, attacker.tileY, tx, ty);
    return d >= UNIT_ATTACK_RANGE_MIN[attacker.type] && d <= UNIT_ATTACK_RANGE_MAX[attacker.type] && UNIT_ATTACK_RANGE_MAX[attacker.type] > 0;
}

// Resolves units[attackerIdx] attacking units[victimIdx], matching the
// core of Unit.java's attackUnit(): a random roll in [offenceMin,
// offenceMax) against the victim's defence (base + terrain bonus), scaled
// by the attacker's current health%. Deliberately does NOT port the
// original's per-property matchup bonuses (mounted-vs-ground,
// golem-vs-skeleton, water/swamp bonuses, etc. -- see Unit.java's
// getOffenceBonusAgainstUnit()); those depend on per-unit HasProperty
// flags this milestone doesn't read.
// One hit: attackerIdx's roll in [offenceMin, offenceMax) against
// victimIdx's defence (base + terrain bonus), scaled by the attacker's
// current health%. Shared by attackUnit()'s direct hit and its
// counterattack.
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
        victim.alive = false;
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

void endTurn()
{
    currentTurn = 1 - currentTurn;
    for (int i = 0; i < unitCount; ++i)
    {
        if (units[i].color == currentTurn)
            units[i].hasMoved = false;
    }
    selectedUnit = -1;
    Serial.printf("turn: %s\n", currentTurn == 0 ? "blue" : "red");
    drawViewport();
}

constexpr int HUD_BTN_W = 76;
constexpr int HUD_BTN_H = 22;
constexpr int HUD_BTN_X = DISPLAY_WIDTH - HUD_BTN_W - 4;
constexpr int HUD_BTN_Y = DISPLAY_HEIGHT - HUD_BTN_H - 4;

// Handles a tap (as opposed to a drag-to-pan) at the given screen
// coordinates: the END TURN button, selecting a movable unit belonging to
// the current turn, or moving the selected unit to a reachable tile.
void handleTap(int screenX, int screenY)
{
    if (screenX >= HUD_BTN_X && screenX < HUD_BTN_X + HUD_BTN_W &&
        screenY >= HUD_BTN_Y && screenY < HUD_BTN_Y + HUD_BTN_H)
    {
        endTurn();
        return;
    }

    int mx = (screenX + viewX) / TILE_SIZE;
    int my = (screenY + viewY) / TILE_SIZE;
    if (!inMapBounds(mx, my))
        return;

    if (selectedUnit >= 0)
    {
        int targetIdx = unitIndexAt(mx, my);
        // Attacking is a standalone action from the unit's current tile --
        // this is "move OR attack" per turn, not the original's "move then
        // attack"; combining the two would mean tracking reachable-attack
        // range from every tile in the move range, not just the unit's
        // current one, which is out of scope here.
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
        }
        selectedUnit = -1;
        drawViewport();
        return;
    }

    int idx = unitIndexAt(mx, my);
    if (idx >= 0 && units[idx].color == currentTurn && !units[idx].hasMoved)
    {
        selectedUnit = idx;
        computeReachable(units[idx]);
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

    gfx.fillRect(HUD_BTN_X, HUD_BTN_Y, HUD_BTN_W, HUD_BTN_H, TFT_DARKGREY);
    gfx.drawRect(HUD_BTN_X, HUD_BTN_Y, HUD_BTN_W, HUD_BTN_H, TFT_WHITE);
    gfx.setTextColor(TFT_WHITE, TFT_DARKGREY);
    gfx.setCursor(HUD_BTN_X + 6, HUD_BTN_Y + 7);
    gfx.print("END TURN");

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

    if (!loadMap("/maps/m0.aem"))
    {
        gfx.println("map load failed!");
        while (true)
            delay(1000);
    }

    setupOTA(); // best-effort; game runs offline if this doesn't connect

    gfx.fillScreen(TFT_BLACK);
    drawViewport();
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
        else
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
            handleTap(lastX, lastY);
        lastX = lastY = -1;
    }

    delay(16);
}
