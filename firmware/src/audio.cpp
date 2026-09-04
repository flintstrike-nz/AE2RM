#include "audio.h"

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <driver/i2s.h> // Arduino-ESP32 2.x ships the legacy I2S driver
#include <math.h>
#include <atomic>

#include "board_pins.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
namespace
{
constexpr int SAMPLE_RATE = 16000;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr int BLOCK_SAMPLES = 240; // ~15 ms per fill
constexpr uint8_t ES8311_ADDR = 0x18;

constexpr int NUM_TONE_VOICES = 3; // 0 melody, 1 bass, 2 SFX (preempts)
constexpr int SFX_VOICE = 2;

bool g_ready = false;
std::atomic<int> g_vol{2}; // 0..AUDIO_VOL_MAX; written by the game task, read by the synth task
MusicId g_music = MUSIC_OFF; // only touched under g_mux

// Master gain per volume level (index 0..AUDIO_VOL_MAX). Level 0 is
// silence; the rest leave headroom for 3 square voices + noise.
constexpr int VOL_GAIN[AUDIO_VOL_MAX + 1] = {0, 14, 28, 46};

// ---------------------------------------------------------------------------
// ES8311 codec -- minimal DAC-only bring-up. Transcribed from Espressif's
// esp-adf es8311 driver for MCLK = 256*fs, 16-bit I2S, codec as slave.
// Unverified by ear (see audio.h).
// ---------------------------------------------------------------------------
bool es8311Write(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

int es8311Read(uint8_t reg)
{
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)
        return -1;
    if (Wire.requestFrom((int)ES8311_ADDR, 1) != 1)
        return -1;
    return Wire.read();
}

bool es8311Init()
{
    // The touch controller already brought Wire up on the shared bus
    // (see board_pins.h); begin() again is harmless if it didn't.
    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);

    if (es8311Read(0xFD) < 0) // chip ID reg -- also just a presence check
    {
        Serial.println("audio: ES8311 not responding on I2C");
        return false;
    }

    struct
    {
        uint8_t reg, val;
    } seq[] = {
        {0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10}, {0x16, 0x24},
        {0x04, 0x20}, {0x05, 0x00}, {0x0B, 0x00}, {0x0C, 0x00}, // 0x04: DAC OSR for 256*fs
        {0x10, 0x1F}, {0x11, 0x7F}, {0x00, 0x80}, // slave mode, power up
        {0x0D, 0x01}, {0x0E, 0x02}, {0x12, 0x00}, {0x13, 0x10},
        {0x09, 0x0C}, {0x0A, 0x0C},               // SDP in/out: 16-bit I2S (0b011 word length)
        {0x32, 0xBF},                             // DAC volume (~0 dB)
        {0x37, 0x08}, {0x44, 0x58},               // 0x44: internal DAC reference path
        {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF}, // MCLK divider set for 256*fs
        {0x01, 0x3F},                             // enable all clocks
    };
    bool ok = true;
    for (auto &s : seq)
        ok &= es8311Write(s.reg, s.val);
    if (!ok)
        Serial.println("audio: ES8311 register write failed");
    return ok;
}

// ---------------------------------------------------------------------------
// I2S TX
// ---------------------------------------------------------------------------
bool i2sInit()
{
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = 0;
    cfg.dma_buf_count = 6;
    cfg.dma_buf_len = BLOCK_SAMPLES;
    cfg.use_apll = true; // cleaner audio clock
    cfg.tx_desc_auto_clear = true;
    cfg.fixed_mclk = SAMPLE_RATE * 256;

    if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK)
    {
        Serial.println("audio: i2s_driver_install failed");
        return false;
    }
    i2s_pin_config_t pins = {};
    pins.mck_io_num = PIN_I2S_MCLK;
    pins.bck_io_num = PIN_I2S_BCLK;
    pins.ws_io_num = PIN_I2S_WS;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK)
    {
        Serial.println("audio: i2s_set_pin failed");
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }
    i2s_zero_dma_buffer(I2S_PORT);
    return true;
}

// ---------------------------------------------------------------------------
// Synth
// ---------------------------------------------------------------------------
struct Voice
{
    uint32_t phase = 0;  // 16.16 fixed
    uint32_t step = 0;   // phase increment per sample
    int32_t env = 0;     // 8.8 fixed envelope level; amplitude = env >> 8
    int32_t decay = 0;   // env units subtracted per sample
};
Voice g_voice[NUM_TONE_VOICES];
uint32_t g_noiseLfsr = 0xACE1u;
int32_t g_noiseVol = 0;

// ~180 ms linear decay to silence -- a plucky chiptune envelope. At
// 16 kHz that's ~2880 samples, so a note stays audible for most of a
// music step and every SFX segment.
constexpr int DECAY_SAMPLES = 2880;

// MIDI note -> 16.16 phase increment per sample, filled once at init so
// voiceNote() (called from inside the sequencer's critical section) is a
// plain lookup with no powf().
uint32_t g_phaseStep[128];
void buildPhaseTable()
{
    for (int n = 0; n < 128; ++n)
    {
        float hz = 440.0f * powf(2.0f, (n - 69) / 12.0f);
        g_phaseStep[n] = (uint32_t)(hz * 65536.0f / SAMPLE_RATE);
    }
}

void voiceNote(int v, uint8_t note, int vol)
{
    if (note == 0 || note >= 128)
    {
        g_voice[v].env = 0;
        return;
    }
    g_voice[v].step = g_phaseStep[note];
    g_voice[v].env = vol << 8;
    g_voice[v].decay = (vol << 8) / DECAY_SAMPLES;
    if (g_voice[v].decay < 1)
        g_voice[v].decay = 1;
}

// One music step: a note per melodic voice, held for durMs.
struct Step
{
    uint8_t mel, bass;
    uint16_t durMs;
};

// Short looping chiptune sketches. Note numbers are MIDI (60 = middle C);
// 0 = rest. These are approximations written for this port, not
// transcriptions of the original tracks.
const Step TITLE_TRACK[] = {
    {72, 48, 260}, {76, 0, 260}, {79, 55, 260}, {76, 0, 260},
    {74, 50, 260}, {77, 0, 260}, {81, 57, 260}, {77, 0, 260},
    {72, 48, 260}, {76, 0, 260}, {79, 55, 260}, {84, 0, 260},
    {83, 55, 260}, {79, 0, 260}, {76, 52, 260}, {72, 0, 260},
};
const Step BATTLE_TRACK[] = {
    {64, 40, 200}, {0, 40, 200}, {67, 40, 200}, {64, 40, 200},
    {60, 36, 200}, {0, 36, 200}, {63, 36, 200}, {60, 36, 200},
    {62, 38, 200}, {0, 38, 200}, {65, 38, 200}, {69, 38, 200},
    {67, 43, 200}, {64, 43, 200}, {62, 43, 200}, {59, 43, 200},
};

struct Track
{
    const Step *steps;
    int count;
};
const Track TRACKS[MUSIC_COUNT] = {
    {nullptr, 0},
    {TITLE_TRACK, (int)(sizeof(TITLE_TRACK) / sizeof(Step))},
    {BATTLE_TRACK, (int)(sizeof(BATTLE_TRACK) / sizeof(Step))},
};

// SFX: a burst of {note, noise, durMs} on the SFX voice.
struct SfxNote
{
    uint8_t note;
    uint8_t noise; // 0..255 noise level for this segment
    uint16_t durMs;
};
const SfxNote SFX_BLIP[] = {{84, 0, 40}};
const SfxNote SFX_SEL[] = {{72, 0, 30}, {79, 0, 40}};
const SfxNote SFX_MOV[] = {{60, 40, 30}, {64, 20, 30}};
const SfxNote SFX_HIT_[] = {{0, 200, 60}, {0, 90, 60}};
const SfxNote SFX_CAP[] = {{72, 0, 60}, {76, 0, 60}, {79, 0, 90}};
const SfxNote SFX_REC[] = {{67, 0, 50}, {74, 0, 50}, {79, 0, 80}};
const SfxNote SFX_WIN[] = {{72, 0, 120}, {76, 0, 120}, {79, 0, 120}, {84, 0, 240}};
const SfxNote SFX_LOSE[] = {{60, 0, 160}, {58, 0, 160}, {55, 0, 320}};

struct SfxDef
{
    const SfxNote *notes;
    int count;
};
const SfxDef SFX[SFX_COUNT] = {
    {SFX_BLIP, 1}, {SFX_SEL, 2}, {SFX_MOV, 2}, {SFX_HIT_, 2},
    {SFX_CAP, 3}, {SFX_REC, 3}, {SFX_WIN, 4}, {SFX_LOSE, 3},
};

// Sequencer state (touched by both the task and the API -- guarded by a
// mutex around the small critical sections).
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
int g_trackStep = 0;
uint32_t g_stepElapsedMs = 0;

const SfxNote *g_sfxNotes = nullptr;
int g_sfxCount = 0, g_sfxIdx = 0;
uint32_t g_sfxElapsedMs = 0;

void startSfx(SfxId id)
{
    portENTER_CRITICAL(&g_mux);
    g_sfxNotes = SFX[id].notes;
    g_sfxCount = SFX[id].count;
    g_sfxIdx = 0;
    g_sfxElapsedMs = 0;
    portEXIT_CRITICAL(&g_mux);
}

// Advance the music + SFX sequencers by `ms`, then render `n` samples.
void renderBlock(int16_t *out, int n)
{
    const uint32_t blockMs = (uint32_t)n * 1000 / SAMPLE_RATE;

    // --- sequencer tick ---
    portENTER_CRITICAL(&g_mux);
    MusicId music = g_music;
    if (music != MUSIC_OFF && TRACKS[music].count > 0)
    {
        const Track &t = TRACKS[music];
        if (g_trackStep >= t.count)
            g_trackStep = 0;
        if (g_stepElapsedMs == 0)
        {
            voiceNote(0, t.steps[g_trackStep].mel, 150);
            voiceNote(1, t.steps[g_trackStep].bass, 110);
        }
        g_stepElapsedMs += blockMs;
        if (g_stepElapsedMs >= t.steps[g_trackStep].durMs)
        {
            g_stepElapsedMs = 0;
            g_trackStep = (g_trackStep + 1) % t.count;
        }
    }
    else
    {
        g_voice[0].env = g_voice[1].env = 0;
    }

    if (g_sfxNotes && g_sfxIdx < g_sfxCount)
    {
        if (g_sfxElapsedMs == 0)
        {
            const SfxNote &s = g_sfxNotes[g_sfxIdx];
            voiceNote(SFX_VOICE, s.note, 200);
            g_noiseVol = s.noise;
        }
        g_sfxElapsedMs += blockMs;
        if (g_sfxElapsedMs >= g_sfxNotes[g_sfxIdx].durMs)
        {
            g_sfxElapsedMs = 0;
            if (++g_sfxIdx >= g_sfxCount)
            {
                g_sfxNotes = nullptr;
                g_noiseVol = 0;
            }
        }
    }
    portEXIT_CRITICAL(&g_mux);

    // --- render ---
    int lvl = g_vol.load();
    const int master = VOL_GAIN[lvl < 0 ? 0 : lvl > AUDIO_VOL_MAX ? AUDIO_VOL_MAX : lvl];
    for (int i = 0; i < n; ++i)
    {
        int32_t acc = 0;
        for (int v = 0; v < NUM_TONE_VOICES; ++v)
        {
            Voice &vc = g_voice[v];
            if (vc.env > 0)
            {
                vc.phase += vc.step;
                int32_t amp = vc.env >> 8;
                acc += ((vc.phase & 0x8000) ? amp : -amp); // 50% square
                vc.env -= vc.decay;
                if (vc.env < 0)
                    vc.env = 0;
            }
        }
        if (g_noiseVol > 0)
        {
            uint32_t b = ((g_noiseLfsr >> 0) ^ (g_noiseLfsr >> 1)) & 1u;
            g_noiseLfsr = (g_noiseLfsr >> 1) | (b << 30);
            int32_t s = (g_noiseLfsr & 1) ? 1 : -1;
            acc += s * (g_noiseVol >> 2);
        }
        acc = acc * master;
        if (acc > 32000)
            acc = 32000;
        else if (acc < -32000)
            acc = -32000;
        out[i] = (int16_t)acc;
    }
}

int16_t g_block[BLOCK_SAMPLES];

void synthTask(void *)
{
    for (;;)
    {
        renderBlock(g_block, BLOCK_SAMPLES);
        size_t wrote = 0;
        i2s_write(I2S_PORT, g_block, sizeof(g_block), &wrote, portMAX_DELAY);
    }
}
} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool audioInit()
{
    {
        Preferences p;
        if (p.begin("aeii", true))
        {
            int v = p.getInt("vol", 2);
            g_vol = (v < 0) ? 0 : (v > AUDIO_VOL_MAX) ? AUDIO_VOL_MAX : v;
            p.end();
        }
    }

    buildPhaseTable();

    pinMode(PIN_AUDIO_PA_EN, OUTPUT);
    digitalWrite(PIN_AUDIO_PA_EN, AUDIO_PA_EN_OFF); // amp muted until everything's up

    // I2S first: the ES8311 wants MCLK/BCLK/LRCK present before its
    // control registers are written (its power-up guidance, and the
    // order the working ES3C28P bring-up uses).
    if (!i2sInit())
    {
        Serial.println("audio: disabled (I2S init failed)");
        return false;
    }
    delay(10); // let the I2S clocks settle
    if (!es8311Init())
    {
        Serial.println("audio: disabled (ES8311 setup failed)");
        i2s_driver_uninstall(I2S_PORT);
        return false; // amp still off -- contract: audioAvailable() == false
    }

    digitalWrite(PIN_AUDIO_PA_EN, AUDIO_PA_EN_ON);

    BaseType_t ok = xTaskCreatePinnedToCore(synthTask, "synth", 3072, nullptr, 1, nullptr, 0);
    if (ok != pdPASS)
    {
        Serial.println("audio: synth task create failed");
        digitalWrite(PIN_AUDIO_PA_EN, AUDIO_PA_EN_OFF);
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    g_ready = true;
    Serial.printf("audio: ready (vol=%d)\n", g_vol.load());
    return true;
}

void audioSfx(SfxId id)
{
    if (!g_ready || g_vol.load() == 0 || id < 0 || id >= SFX_COUNT)
        return;
    startSfx(id);
}

void audioMusic(MusicId id)
{
    if (id < 0 || id >= MUSIC_COUNT)
        return;
    portENTER_CRITICAL(&g_mux);
    if (g_music != id)
    {
        g_music = id;
        g_trackStep = 0;
        g_stepElapsedMs = 0;
    }
    portEXIT_CRITICAL(&g_mux);
}

void audioCycleVolume()
{
    g_vol = (g_vol.load() + 1) % (AUDIO_VOL_MAX + 1);
    Preferences p;
    if (p.begin("aeii", false))
    {
        p.putInt("vol", g_vol.load());
        p.end();
    }
}

int audioVolume() { return g_vol.load(); }

const char *audioVolumeLabel()
{
    static const char *L[AUDIO_VOL_MAX + 1] = {"Off", "Low", "Med", "High"};
    int v = g_vol.load();
    return L[v < 0 ? 0 : v > AUDIO_VOL_MAX ? AUDIO_VOL_MAX : v];
}

bool audioAvailable() { return g_ready; }
