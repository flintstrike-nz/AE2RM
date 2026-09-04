// Chiptune-style audio for the AE2RM port: an ES8311 codec fed by a tiny
// square-wave + noise synth over I2S. "Chiptune approximation" was the
// agreed scope -- this does NOT parse the original MIDI tracks, it plays
// short hand-written loops and sound effects.
//
// Everything here is best-effort: audioInit() returns false if the ES8311
// doesn't ACK on I2C or the I2S driver won't install, and every other
// call then no-ops. Note this does actively drive the I2S + amp GPIOs
// (4/5/6/7/8, 1) before it knows they're right -- see board_pins.h's
// warning about a wrong pin contending with another peripheral; those
// lines are otherwise unused on this board.
//
// NOTE: unverified on hardware. This environment can flash the board and
// read its boot log but has no way to hear the output, so the ES8311
// register sequence, the I2S clocking, and the mix levels are all
// untested by ear -- see firmware/README.md's verification section.
#pragma once

#include <stdint.h>

enum SfxId
{
    SFX_UI_BLIP,     // menu / tab tap
    SFX_SELECT,      // pick up a unit
    SFX_MOVE,        // unit finished a move
    SFX_HIT,         // combat hit lands
    SFX_CAPTURE,     // building captured
    SFX_RECRUIT,     // shop deploy
    SFX_VICTORY,     // win banner
    SFX_DEFEAT,      // loss / draw banner
    SFX_COUNT
};

enum MusicId
{
    MUSIC_OFF,
    MUSIC_TITLE,     // title screen + mission menu
    MUSIC_BATTLE,    // in a mission
    MUSIC_COUNT
};

// Brings up the ES8311 (over the shared touch I2C bus) and the I2S TX
// peripheral, enables the speaker amp, and starts the synth task. Safe to
// call once from setup(); returns false (and leaves audio disabled) on
// any failure. Reads the persisted mute flag.
bool audioInit();

// Fire-and-forget a sound effect. No-op if audio is disabled or muted.
void audioSfx(SfxId id);

// Switch the looping background track (MUSIC_OFF stops it). No-op if
// audio is disabled; still tracked (and honoured on unmute) if muted.
void audioMusic(MusicId id);

// Toggle / query mute. The new state is persisted (Preferences
// "aeii"/"mute") so it survives a reboot.
void audioToggleMute();
bool audioMuted();

// True once audioInit() has succeeded -- for a HUD indicator / menu row.
bool audioAvailable();
