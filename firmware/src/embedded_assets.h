// Optional in-firmware copy of the game assets, so the board can run
// without a microSD card.
//
// tools/convert_assets.py writes src/generated_assets.h (git-ignored) --
// one concatenated blob plus a path table -- from the same files it lays
// out for the SD card. When that header is present at build time, the
// whole asset tree is linked into the firmware image (~335 KB of flash as
// of the title screen + cutscene effects -- the 240x320 splash alone is
// 150 KB) and openEmbeddedAsset() serves them; when it isn't, these
// functions report "nothing embedded" and the firmware needs the card.
#pragma once

#include <FS.h>

// A readable File backed by the in-flash copy of `path` (same
// card-relative paths the firmware uses, e.g. "/maps/m0.aem"), or an
// invalid File (operator bool == false) if no asset is embedded for it.
fs::File openEmbeddedAsset(const char *path);

// True when this build has assets linked in (generated_assets.h existed).
bool haveEmbeddedAssets();
