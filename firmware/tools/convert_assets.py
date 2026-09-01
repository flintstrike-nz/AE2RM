#!/usr/bin/env python3
"""Convert AE2RM J2ME assets into the raw formats the ESP32 firmware reads
from the SD card for the minimal playable slice (map m0 only).

Usage:
    pip install Pillow
    python3 tools/convert_assets.py

Reads from ../src/java/res/res (run `make` in src/ first to extract it),
writes into ./assets/sdcard, which you then copy onto the microSD card
used by the board (FAT32, files at the card root as laid out below).

Output layout on the SD card:
    /tiles0/tile_NN.bin   -- 24x24 RGB565 raw pixels, native uint16_t, one
                             file per tiles0.sprite frame (NN = 00..47)
    /maps/m0.aem          -- copied as-is; the firmware's own map loader
                             reads the same binary format the original
                             MIDlet used (see aeii/MainDisplayable.java,
                             loadMap()): int32 width, int32 height, then
                             width*height tile-index bytes, then building/
                             unit data the firmware does not parse yet.
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
OUT_DIR = os.path.join(HERE, "..", "assets", "sdcard")

TILE_COUNT = 48  # tiles0.sprite: FrameCount 48, FrameWidth/Height 24


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert_tile(src_png, dst_bin):
    im = Image.open(src_png).convert("RGB")
    w, h = im.size
    px = im.load()
    with open(dst_bin, "wb") as f:
        for y in range(h):
            for x in range(w):
                r, g, b = px[x, y]
                f.write(struct.pack("<H", rgb565(r, g, b)))


def main():
    if not os.path.isdir(RES_DIR):
        sys.exit(
            f"Resource dir not found: {RES_DIR}\n"
            "Run `make` in src/ first to extract AEIIRM_src.zip."
        )

    tiles_out = os.path.join(OUT_DIR, "tiles0")
    maps_out = os.path.join(OUT_DIR, "maps")
    os.makedirs(tiles_out, exist_ok=True)
    os.makedirs(maps_out, exist_ok=True)

    for i in range(TILE_COUNT):
        src = os.path.join(RES_DIR, f"tiles0_{i:02d}.png")
        dst = os.path.join(tiles_out, f"tile_{i:02d}.bin")
        convert_tile(src, dst)
        print(f"converted {os.path.basename(src)} -> {os.path.relpath(dst, OUT_DIR)}")

    shutil.copyfile(os.path.join(RES_DIR, "m0.aem"), os.path.join(maps_out, "m0.aem"))
    print("copied m0.aem")

    print(f"\nDone. Copy the contents of {OUT_DIR} onto the microSD card root.")


if __name__ == "__main__":
    main()
