// AE2RM ESP32 port -- milestone 2: render map terrain + starting units.
//
// This does NOT yet include game rules, movement, combat, or menus -- units
// are drawn statically at their map-file starting positions, with no turn
// logic, selection, or animation. Loads m0.aem + tiles0 + unit_icons from
// the SD card (converted by tools/convert_assets.py) and lets the player
// pan around it by touch/drag. See firmware/README.md for what's
// implemented and what's next.

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
// This reflects where MainDisplayable.java's loadMap() places starting
// units, but NOT its actual team-color logic (that goes through the
// scripted turn queue, which isn't ported). `color` here is simply the
// map file's raw color slot (0-3, indexing UNIT_COLOR_NAMES directly) --
// close enough for a static rendering milestone, not authoritative.
struct UnitPlacement
{
    uint8_t type;
    uint8_t color;
    int16_t tileX;
    int16_t tileY;
};
constexpr int MAX_UNITS = 64;
static UnitPlacement units[MAX_UNITS];
static int unitCount = 0;

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
        if (units[i].tileX >= firstCol && units[i].tileX < firstCol + cols &&
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

    for (int i = 0; i < unitCount; ++i)
    {
        const UnitPlacement &u = units[i];
        if (!unitIconLoaded[u.color][u.type])
            continue; // out of view this frame, or failed to load
        int px = u.tileX * TILE_SIZE - viewX;
        int py = u.tileY * TILE_SIZE - viewY;
        if (px <= -UNIT_ICON_SIZE || py <= -UNIT_ICON_SIZE || px >= DISPLAY_WIDTH || py >= DISPLAY_HEIGHT)
            continue;
        gfx.pushImage(px, py, UNIT_ICON_SIZE, UNIT_ICON_SIZE, unitIconFrame(u.color, u.type), TRANSPARENT_565);
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

void loop()
{
    static int32_t lastX = -1, lastY = -1;
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
        if (lastX >= 0)
        {
            viewX -= (x - lastX);
            viewY -= (y - lastY);
            clampView();
            drawViewport();
        }
        lastX = x;
        lastY = y;
    }
    else
    {
        lastX = lastY = -1;
    }

    delay(16);
}
