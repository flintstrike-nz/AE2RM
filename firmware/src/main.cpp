// AE2RM ESP32 port -- milestone 1: render a real game map on real hardware.
//
// This does NOT yet include game rules, units, combat, or menus -- it is the
// first proof that the asset pipeline and rendering path work end to end:
// load the m0.aem map + tiles0 tileset from the SD card (converted by
// tools/convert_assets.py) and let the player pan around it by touch/drag.
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

static uint16_t tileCache[TILE_COUNT][TILE_SIZE * TILE_SIZE];
static bool tileLoaded[TILE_COUNT] = {false};

static uint8_t *mapTiles = nullptr; // [x * mapHeight + y], matches the
                                     // column-major layout MainDisplayable
                                     // reads from the .aem file
static int mapWidth = 0;
static int mapHeight = 0;

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
    size_t got = f.read(reinterpret_cast<uint8_t *>(tileCache[index]), sizeof(tileCache[index]));
    f.close();
    if (got != sizeof(tileCache[index]))
    {
        Serial.printf("tile %d short read (%u/%u bytes)\n", index, (unsigned)got, (unsigned)sizeof(tileCache[index]));
        return false;
    }
    tileLoaded[index] = true;
    return true;
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
    f.close();

    free(mapTiles);
    mapTiles = newTiles;
    mapWidth = width;
    mapHeight = height;

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

            gfx.pushImage(px, py, TILE_SIZE, TILE_SIZE, tileCache[tile]);
        }
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

void setup()
{
    Serial.begin(115200);
    delay(200);

    gfx.init();
    gfx.setRotation(0);
    gfx.setBrightness(200);
    // tools/convert_assets.py writes each RGB565 pixel as a native
    // (little-endian) uint16_t, matching tileCache's in-memory layout, so
    // pushImage() below must not byte-swap them; set this explicitly
    // rather than relying on the library default.
    gfx.setSwapBytes(false);
    gfx.fillScreen(TFT_BLACK);
    gfx.setTextColor(TFT_WHITE);
    gfx.setTextSize(2);
    gfx.setCursor(10, 10);
    gfx.println("AE2RM ESP32");
    gfx.println("booting...");

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
