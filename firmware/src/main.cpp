// AE2RM ESP32 port -- milestone 1: render a real game map on real hardware.
//
// This does NOT yet include game rules, units, combat, or menus -- it is the
// first proof that the asset pipeline and rendering path work end to end:
// load the m0.aem map + tiles0 tileset from the SD card (converted by
// tools/convert_assets.py) and let the player pan around it by touch/drag.
// See firmware/README.md for what's implemented and what's next.

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include "LGFX_Config.h"
#include "board_pins.h"

static LGFX gfx;

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
    File f = SD.open(path, FILE_READ);
    if (!f)
    {
        Serial.printf("tile load failed: %s\n", path);
        return false;
    }
    f.read(reinterpret_cast<uint8_t *>(tileCache[index]), sizeof(tileCache[index]));
    f.close();
    tileLoaded[index] = true;
    return true;
}

bool loadMap(const char *path)
{
    File f = SD.open(path, FILE_READ);
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
    f.read(hdr, 8);
    mapWidth = (hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
    mapHeight = (hdr[4] << 24) | (hdr[5] << 16) | (hdr[6] << 8) | hdr[7];

    free(mapTiles);
    mapTiles = static_cast<uint8_t *>(malloc(mapWidth * mapHeight));
    f.read(mapTiles, mapWidth * mapHeight);
    f.close();

    Serial.printf("loaded map %s: %dx%d tiles\n", path, mapWidth, mapHeight);
    return true;
}

inline uint8_t tileAt(int mx, int my)
{
    if (mx < 0 || my < 0 || mx >= mapWidth || my >= mapHeight)
        return 0;
    return mapTiles[mx * mapHeight + my];
}

void drawViewport()
{
    int firstCol = viewX / TILE_SIZE;
    int firstRow = viewY / TILE_SIZE;
    int cols = DISPLAY_WIDTH / TILE_SIZE + 2;
    int rows = DISPLAY_HEIGHT / TILE_SIZE + 2;

    gfx.startWrite();
    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < cols; ++col)
        {
            int mx = firstCol + col;
            int my = firstRow + row;
            uint8_t tile = tileAt(mx, my);
            if (!loadTile(tile))
                continue;

            int px = mx * TILE_SIZE - viewX;
            int py = my * TILE_SIZE - viewY;
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

void setup()
{
    Serial.begin(115200);
    delay(200);

    gfx.init();
    gfx.setRotation(0);
    gfx.setBrightness(200);
    gfx.fillScreen(TFT_BLACK);
    gfx.setTextColor(TFT_WHITE);
    gfx.setTextSize(2);
    gfx.setCursor(10, 10);
    gfx.println("AE2RM ESP32");
    gfx.println("booting...");

    // SD card shares the display's SPI wiring on this board; give it its
    // own SPIClass instance on the same pins (LovyanGFX drives the panel
    // through the ESP-IDF SPI driver directly, not through this object).
    SPI.begin(PIN_LCD_SCLK, PIN_LCD_MISO, PIN_LCD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS, SPI))
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

    gfx.fillScreen(TFT_BLACK);
    drawViewport();
}

void loop()
{
    static int32_t lastX = -1, lastY = -1;
    int32_t x, y;

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
