#!/usr/bin/env python3
"""Convert AE2RM J2ME assets into the raw formats the ESP32 firmware reads
from the SD card: the tiles0 tileset, unit icons, all 8 story maps
(m0-m7 -- the "m*.aem" set loadMap() always builds the same 2-side team
queue for, see main.cpp's UnitPlacement comment; skirmish maps s0-s11
aren't converted), the localized string table, m0's mission script, the
title screen, and the combat hit-flash effect.

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
    /title/splash.bin           -- 240x320 RGB565, transparent -- the
                                original's title-screen background
                                (splash.png), shown full-screen at boot.
    /title/logo.bin              -- 240x85 RGB565, transparent -- the
                                original's game logo (logo.png), composited
                                over the splash background.
    /effects/redspark_NN.bin    -- 20x20 RGB565, transparent, one file per
                                redspark.sprite frame (NN = 00..05) -- the
                                original's combat hit-flash effect
                                (createSimpleSparkSprite() with sprRedSpark
                                in MainDisplayable.java), played by
                                main.cpp's playHitEffect() on every hit.
    /effects/spark_NN.bin       -- 24x24 RGB565, transparent, 6 frames
                                (NN = 00..05) -- m0's mission-script
                                CreateSpriteAtUnit Spark effect, played by
                                main.cpp's runIntroScript() via
                                playCutsceneSpriteEffect().
    /effects/smoke_NN.bin       -- 24x20 RGB565, transparent, 4 frames
                                (NN = 00..03) -- m0's mission-script
                                CreateSpriteAtUnit Smoke effect, same path
                                as spark_NN.bin above.
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

# Generated C header that bakes the SD-card asset tree into the firmware
# image, so the board runs without a card. Consumed by src/embedded_assets.cpp
# via __has_include; git-ignored, regenerated on every run.
GENERATED_HEADER = os.path.join(HERE, "..", "src", "generated_assets.h")

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


# Combat/cutscene effect sprites this port plays via playHitEffect()/
# playCutsceneSpriteEffect() in main.cpp -- name, (FrameWidth, FrameHeight)
# from each *.sprite file, FrameCount. All three are single-row sheets.
EFFECT_SPRITES = {
    "redspark": (20, 20, 6),
    "spark": (24, 24, 6),
    "smoke": (24, 20, 4),
}


def convert_effect_sprite(name, dst_dir):
    # Each *.png is a single-row sheet of FrameCount frames side by side.
    # Cropped into one file per frame (matching the tile/unit-icon
    # convention below) rather than converted as one flat image -- the
    # firmware loads each frame into a contiguous per-frame buffer slot,
    # which only works if each frame's bytes are contiguous in its own
    # file; a single raw conversion of the whole sheet would interleave
    # all frames' pixels row by row instead.
    frame_w, frame_h, frame_count = EFFECT_SPRITES[name]
    im = Image.open(os.path.join(RES_DIR, f"{name}.png")).convert("RGBA")
    for frame in range(frame_count):
        box = (frame * frame_w, 0, (frame + 1) * frame_w, frame_h)
        dst = os.path.join(dst_dir, f"{name}_{frame:02d}.bin")
        write_raw565(im.crop(box), dst, transparent=True)


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
    strings_out = os.path.join(OUT_DIR, "strings.dat")
    if os.path.isfile(LANG_PATH):
        shutil.copyfile(LANG_PATH, strings_out)
        print("copied lang.dat -> strings.dat")
    else:
        # OUT_DIR persists across runs (it's not wiped at the top of
        # main()) -- if a previous run wrote strings.dat and this one
        # can't find lang.dat, leaving the old file in place would ship a
        # stale table silently instead of the fallback this warning
        # describes.
        if os.path.isfile(strings_out):
            os.remove(strings_out)
        print(f"WARNING: {LANG_PATH} not found -- skipping strings.dat "
              "(mission menu falls back to generic titles, m0's intro "
              "cutscene won't have dialog text)")

    # Title screen: splash.png is exactly 240x320 -- a pixel-perfect match
    # for this display -- with logo.png (240x85) composited over it. Both
    # carry real alpha (confirmed against the source PNGs, not just
    # assumed), so converted the same transparent way as unit icons.
    title_out = os.path.join(OUT_DIR, "title")
    os.makedirs(title_out, exist_ok=True)
    write_raw565(Image.open(os.path.join(RES_DIR, "splash.png")).convert("RGBA"),
                 os.path.join(title_out, "splash.bin"), transparent=True)
    write_raw565(Image.open(os.path.join(RES_DIR, "logo.png")).convert("RGBA"),
                 os.path.join(title_out, "logo.bin"), transparent=True)
    print("converted splash.png, logo.png -> title/")

    # Combat hit-flash (redspark, createSimpleSparkSprite() with
    # sprRedSpark) and m0's mission-script CreateSpriteAtUnit effects
    # (spark, smoke -- see runIntroScript() in main.cpp).
    effects_out = os.path.join(OUT_DIR, "effects")
    os.makedirs(effects_out, exist_ok=True)
    for name in EFFECT_SPRITES:
        convert_effect_sprite(name, effects_out)
        frame_count = EFFECT_SPRITES[name][2]
        print(f"converted {name}.png -> effects/{name}_*.bin ({frame_count} frames)")

    scripts_out = os.path.join(OUT_DIR, "scripts")
    os.makedirs(scripts_out, exist_ok=True)
    # Only m0 has a mission-script file in the source archive (see the root
    # README's roadmap) -- the firmware only looks for m0.script.
    m0_script = os.path.join(RES_DIR, "m0.script")
    m0_script_out = os.path.join(scripts_out, "m0.script")
    if os.path.isfile(m0_script):
        shutil.copyfile(m0_script, m0_script_out)
        print("copied m0.script")
    elif os.path.isfile(m0_script_out):
        # Same stale-output concern as strings.dat above.
        os.remove(m0_script_out)

    write_generated_header()

    print(f"\nDone. Copy the contents of {OUT_DIR} onto the microSD card root,")
    print("or just build the firmware -- the same assets are now baked into")
    print(f"{os.path.relpath(GENERATED_HEADER)} for card-less operation.")


def write_generated_header():
    """Concatenate the whole assets/sdcard tree into one blob and emit a C
    header (blob array + path table) for src/embedded_assets.cpp to link in.

    Paths in the table are card-relative with a leading slash, exactly what
    the firmware passes to openAsset() ("/maps/m0.aem", "/tiles0/tile_00.bin").
    """
    entries = []  # (card_path, bytes)
    for root, _dirs, files in os.walk(OUT_DIR):
        for fn in sorted(files):
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, OUT_DIR)
            card_path = "/" + rel.replace(os.sep, "/")
            with open(full, "rb") as fh:
                entries.append((card_path, fh.read()))
    entries.sort(key=lambda e: e[0])

    blob = bytearray()
    toc = []  # (card_path, offset, length)
    for card_path, data in entries:
        toc.append((card_path, len(blob), len(data)))
        blob.extend(data)

    lines = [
        "// AUTO-GENERATED by tools/convert_assets.py -- do not edit, not committed.",
        "// The game's SD-card asset tree, linked into the firmware image so the",
        "// board can run without a card. See src/embedded_assets.h.",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"// {len(toc)} files, {len(blob)} bytes total.",
        "static const unsigned char GENERATED_ASSET_BLOB[] = {",
    ]
    row = []
    for i, b in enumerate(blob):
        row.append(f"{b}")
        if len(row) == 20:
            lines.append("    " + ",".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ",".join(row) + ",")
    lines.append("};")
    lines.append("")
    lines.append("struct GeneratedAssetEntry { const char *path; unsigned offset; unsigned length; };")
    lines.append("static const GeneratedAssetEntry GENERATED_ASSET_TOC[] = {")
    for card_path, off, length in toc:
        esc = card_path.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'    {{ "{esc}", {off}u, {length}u }},')
    lines.append("};")
    lines.append(f"static const int GENERATED_ASSET_COUNT = {len(toc)};")
    lines.append("")

    with open(GENERATED_HEADER, "w") as fh:
        fh.write("\n".join(lines))
    print(f"wrote {os.path.relpath(GENERATED_HEADER)} "
          f"({len(toc)} files, {len(blob)} bytes)")


if __name__ == "__main__":
    main()
