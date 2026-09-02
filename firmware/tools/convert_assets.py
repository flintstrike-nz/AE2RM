#!/usr/bin/env python3
"""Convert AE2RM J2ME assets into the raw formats the ESP32 firmware reads
from the SD card: the tiles0 tileset, unit icons, and all 8 story maps
(m0-m7 -- the "m*.aem" set loadMap() always builds the same 2-side team
queue for, see main.cpp's UnitPlacement comment; skirmish maps s0-s11
aren't converted).

Usage:
    pip install Pillow
    python3 tools/convert_assets.py

Reads from ../src/java/res/res (run `make` in src/ first to extract it),
writes into ./assets/sdcard, which you then copy onto the microSD card
used by the board (FAT32, files at the card root as laid out below).

Output layout on the SD card:
    /tiles0/tile_NN.bin      -- 24x24 RGB565 raw pixels, native uint16_t,
                                one file per tiles0.sprite frame (NN = 00..47)
    /units/COLOR_TT.bin      -- 24x24 RGB565, one per (color, unit type)
                                combination, cropped from each color's
                                unit_icons.png (row 0 = the map-icon frame
                                for that type; see Unit.UNIT_NAMES for the
                                TT order). Transparent source pixels are
                                written as the sentinel color TRANSPARENT_565
                                (magenta) so the firmware can skip them when
                                blitting -- these sprites aren't square.
    /maps/mN.aem              -- copied as-is for N in 0..7; the firmware's
                                own map loader reads the same binary format
                                the original MIDlet used (see
                                aeii/MainDisplayable.java, loadMap()): int32
                                width, int32 height, then width*height
                                tile-index bytes, then a building-color
                                table and a list of unit placement records
                                the firmware also parses (see main.cpp's
                                loadMap()).
    /strings.dat               -- copied as-is from src/java/res/lang.dat
                                (int32 BE count, then that many uint16
                                BE-length-prefixed UTF-8 strings -- the
                                original's PaintableObject.getLocaleString()
                                table). Used for mission menu titles and
                                m0's mission-script dialog text.
    /scripts/m0.script          -- copied as-is; the only mission-script
                                file in the source archive (see the root
                                README's roadmap). A small subset of its
                                commands (the intro cutscene, @Case 0-13)
                                is interpreted by main.cpp's
                                runIntroScript(); the rest (Test-driven
                                tutorial hints and the ending dialog,
                                @Case 14 onward) isn't ported yet.
"""
import os
import shutil
import struct
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
RES_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "src", "java", "res", "res"))
LANG_PATH = os.path.normpath(os.path.join(RES_DIR, "..", "lang.dat"))
OUT_DIR = os.path.join(HERE, "..", "assets", "sdcard")

TILE_COUNT = 48  # tiles0.sprite: FrameCount 48, FrameWidth/Height 24
UNIT_ICON_SIZE = 24  # unit_icons.sprite: FrameWidth/Height 24
UNIT_TYPE_COUNT = 12  # Unit.UNIT_NAMES.length
UNIT_COLORS = ["blue", "red", "green", "black"]  # MainDisplayable.FRACTION_COLOR_PREFIXES order

# Must match TRANSPARENT_565 in firmware/src/main.cpp -- pure magenta, chosen
# because it doesn't occur in any of this game's sprite art.
TRANSPARENT_565 = 0xF81F
ALPHA_THRESHOLD = 128  # source pixels less opaque than this become TRANSPARENT_565


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def write_raw565(im, dst_bin, transparent=False):
    px = im.load()
    w, h = im.size
    with open(dst_bin, "wb") as f:
        for y in range(h):
            for x in range(w):
                if transparent and px[x, y][3] < ALPHA_THRESHOLD:
                    value = TRANSPARENT_565
                else:
                    r, g, b = px[x, y][:3]
                    value = rgb565(r, g, b)
                    if transparent and value == TRANSPARENT_565:
                        # Vanishingly unlikely (would need this exact
                        # magenta as real, opaque sprite art) but would
                        # silently become a transparent pixel -- avoid it.
                        value ^= 1
                f.write(struct.pack("<H", value))


def convert_tile(src_png, dst_bin):
    write_raw565(Image.open(src_png).convert("RGB"), dst_bin)


def convert_unit_icons(color, dst_dir):
    im = Image.open(os.path.join(RES_DIR, color, "unit_icons.png")).convert("RGBA")
    for unit_type in range(UNIT_TYPE_COUNT):
        # Row 0 of the sheet (see unit_icons.sprite's FrameDef list): frame
        # index == unit type for the first of its two map-icon variants.
        box = (
            unit_type * UNIT_ICON_SIZE, 0,
            (unit_type + 1) * UNIT_ICON_SIZE, UNIT_ICON_SIZE,
        )
        frame = im.crop(box)
        dst = os.path.join(dst_dir, f"{color}_{unit_type:02d}.bin")
        write_raw565(frame, dst, transparent=True)


def main():
    if not os.path.isdir(RES_DIR):
        sys.exit(
            f"Resource dir not found: {RES_DIR}\n"
            "Run `make` in src/ first to extract AEIIRM_src.zip."
        )

    tiles_out = os.path.join(OUT_DIR, "tiles0")
    units_out = os.path.join(OUT_DIR, "units")
    maps_out = os.path.join(OUT_DIR, "maps")
    os.makedirs(tiles_out, exist_ok=True)
    os.makedirs(units_out, exist_ok=True)
    os.makedirs(maps_out, exist_ok=True)

    for i in range(TILE_COUNT):
        src = os.path.join(RES_DIR, f"tiles0_{i:02d}.png")
        dst = os.path.join(tiles_out, f"tile_{i:02d}.bin")
        convert_tile(src, dst)
        print(f"converted {os.path.basename(src)} -> {os.path.relpath(dst, OUT_DIR)}")

    for color in UNIT_COLORS:
        convert_unit_icons(color, units_out)
        print(f"converted {color}/unit_icons.png -> units/{color}_*.bin ({UNIT_TYPE_COUNT} frames)")

    STORY_MAP_COUNT = 8  # m0.aem .. m7.aem
    for i in range(STORY_MAP_COUNT):
        name = f"m{i}.aem"
        shutil.copyfile(os.path.join(RES_DIR, name), os.path.join(maps_out, name))
        print(f"copied {name}")

    # lang.dat's on-disk format (int32 BE count, then that many
    # DataOutputStream.writeUTF-style entries: uint16 BE length + UTF-8
    # bytes) is exactly what the firmware's own reader expects -- copy it
    # unmodified rather than reprocessing it. Used for m0's mission-script
    # dialog text (PaintableObject.getLocaleString()) and the mission
    # menu's titles (indices 121-128, one per m0.aem..m7.aem -- see
    # MainDisplayable.java's `getLocaleString(121 + mission)`).
    if os.path.isfile(LANG_PATH):
        shutil.copyfile(LANG_PATH, os.path.join(OUT_DIR, "strings.dat"))
        print("copied lang.dat -> strings.dat")
    else:
        print(f"WARNING: {LANG_PATH} not found -- skipping strings.dat "
              "(mission menu falls back to generic titles, m0's intro "
              "cutscene won't have dialog text)")

    scripts_out = os.path.join(OUT_DIR, "scripts")
    os.makedirs(scripts_out, exist_ok=True)
    # Only m0 has a mission-script file in the source archive (see the root
    # README's roadmap) -- the firmware only looks for m0.script.
    m0_script = os.path.join(RES_DIR, "m0.script")
    if os.path.isfile(m0_script):
        shutil.copyfile(m0_script, os.path.join(scripts_out, "m0.script"))
        print("copied m0.script")

    print(f"\nDone. Copy the contents of {OUT_DIR} onto the microSD card root.")


if __name__ == "__main__":
    main()
