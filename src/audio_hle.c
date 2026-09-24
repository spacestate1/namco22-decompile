/* See include/audio_hle.h. */
#include "audio_hle.h"
#include "mcu_sound.h"
#include "audio.h"
#include "propcycl.h"
#include "c352.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Decompiled game state -- read-only here, same array every game_*.c file
 * uses. audio_hle_tick() polls W[0x0CBC]/W[0x0CC0] each frame to pick the
 * music track; it never writes state, only reads what the real game logic
 * already maintains. */
extern intptr_t _W[];
#define W _W

#define MAX_VOICES 8   /* concurrent one-shot SFX playback slots */

typedef struct {
    int16_t *pcm;      /* mono s16, owned */
    uint32_t frames;
    int      rate;
    bool     loaded;
} sfx_asset_t;

static sfx_asset_t g_assets[AUDIO_SFX_COUNT];
static sfx_asset_t g_music_assets[AUDIO_MUSIC_COUNT];

typedef struct {
    const sfx_asset_t *asset;
    double pos;    /* fractional source-frame read position */
    bool   active;
} voice_t;

static voice_t       g_voices[MAX_VOICES];
static SDL_SpinLock   g_voice_lock;

/* ---- THE REAL CHIP, OVER THE REAL WAVE ROM ------------------------------
 *
 * src/c352.c is a port of MAME's and is gated sample-exact against it, so the
 * chip itself is sound. What is missing is the thing that DRIVES it: the
 * M37710 sound MCU running pr1data.8k (include/m37710.h). Until that core
 * works nothing writes these registers and the chip renders silence -- which
 * is correct, and is what was asked for in place of a recording. */
static c352_t         g_chip;
static uint8_t       *g_wave;          /* 16MB sample space, or NULL */
static bool           g_chip_ready;
static bool           g_chip_on;
static SDL_SpinLock   g_chip_lock;

/* THE CHIP RUNS ON THE SOUND DRIVER'S CLOCK. On the board the C352 and the
 * M37710 share one timeline, and the driver READS the chip back: after a
 * key-on its next tick reads the voice flags (BUSY set, KEYON cleared by the
 * chip) and writes them back with its own bits. With the chip advanced lazily
 * on the audio thread the driver only ever saw what it had written, so it
 * re-wrote KEYON and every note it keyed played TWICE, 8 ms apart -- MAME
 * writes 0x8008 there, we wrote 0x4008 (the attract demo's balloon pop).
 * So, as MAME does with stream->update() before every register access, the
 * driver side brings the chip up to the current MCU cycle before each read
 * and write (audio_hle_c352_sync) and appends what it generated to this FIFO;
 * the audio device -- or the headless WAV dump -- only drains it.
 * One chip frame is exactly 192 MCU cycles: (16.384 MHz) / (24.576 MHz / 288). */
#define CHIP_FIFO 32768
#define MCU_CYC_PER_CHIP_FRAME 192u
static int16_t        g_cfifo[CHIP_FIFO * 4];  /* FL,FR,RL,RR per frame */
static int            g_cfifo_n;               /* frames held */
static double         g_cfifo_frac;            /* fractional read cursor */
static uint64_t       g_chip_done;             /* chip frames generated so far, in MCU time */
static bool           g_chip_synced;

static audio_music_t g_music_current = AUDIO_MUSIC_NONE;
static double         g_music_pos;
static SDL_SpinLock   g_music_lock;


static FILE *g_dump_f;      /* PROPCYCL_AUDIODUMP -- see audio_hle_init */
static long  g_dump_bytes;
static int   g_dump_rate;
static bool  g_music_disabled; /* PROPCYCL_NO_MUSIC -- see audio_hle_init */
/* Master output gain, 0.0 .. 1.0, set from the Escape menu and saved with
 * the control bindings. Applied once at the very end of the mix, after the
 * chip and the one-shot voices are summed and before the clamp, so it
 * attenuates everything the cabinet would emit and cannot change the
 * relative balance the sound driver decided on. */
static float g_volume = 1.0f;

static bool  g_gameplay_rec;   /* PROPCYCL_GAMEPLAY_REC -- A/B the old loop */

static const struct { audio_sfx_t id; const char *file; } SFX_DEFS[] = {
    { AUDIO_SFX_COIN,    "sfx_coin.wav" },
};


static bool load_wav_mono16(const char *path, sfx_asset_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t hdr[44];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return false; }
    if (memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return false; }

    uint16_t channels = (uint16_t)(hdr[22] | (hdr[23] << 8));
    uint32_t rate = (uint32_t)(hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24));
    uint16_t bits = (uint16_t)(hdr[34] | (hdr[35] << 8));
    uint32_t data_bytes = (uint32_t)(hdr[40] | (hdr[41] << 8) | (hdr[42] << 16) | (hdr[43] << 24));
    if (channels != 1 || bits != 16) { fclose(f); return false; }
    /* Guard what mix_frames() assumes downstream: the music path indexes
     * this asset with `% a->frames` every mix (UB at zero, and a crash a
     * corrupt/truncated capture -- e.g. extract_music_assets.py slicing
     * past the end of a short reference -- would otherwise hit on the
     * audio thread instead of at load time). A zero rate would not crash
     * but would freeze playback on a single repeated sample forever, which
     * is equally not a usable asset. */
    if (rate == 0 || data_bytes < 2) { fclose(f); return false; }

    int16_t *buf = malloc(data_bytes);
    if (!buf) { fclose(f); return false; }
    if (fread(buf, 1, data_bytes, f) != data_bytes) { free(buf); fclose(f); return false; }
    fclose(f);

    out->pcm = buf;
    out->frames = data_bytes / 2;
    out->rate = (int)rate;
    out->loaded = true;
    return true;
}

/* Bound on `frames` for the accumulator below. The live SDL callback's
 * buffer is AUDIO_REQ_SAMPLES (audio.c, currently 1024); the offline dump
 * path (audio_hle_tick) already caps itself at 8192. Both are far under
 * this; it exists so an unexpected caller can't blow the stack via the
 * fixed-size accumulator instead of silently corrupting it. */
#define MAX_MIX_FRAMES 8192

/* Mixes `frames` stereo frames at `rate` from every active one-shot voice
 * PLUS the current looping music track (if any) into dst (which this
 * clears first). Shared by the live SDL callback and the offline dump
 * tick below, so a dump verifies the actual mix path, not a shortcut.
 *
 * Every source accumulates into a 32-bit buffer and is clipped to int16
 * ONCE at the end, not after each source. Clipping per-source (the
 * original shape) is not just cosmetic: once one voice alone pins a
 * sample at +-32767, a later source's contribution -- even a negative one
 * that would pull the true sum back under the ceiling -- has no effect on
 * a value that is already clamped. That is a different, and wrong,
 * result from a real additive mixer whenever more than one source is
 * loud at once (an SFX on top of the ambient track, or >1 concurrent
 * SFX voice). */
/* Loads pr1wavea.2l + pr1waveb.1l into the 16MB space the chip addresses,
 * laid out as namcos22.cpp's ROM_REGION does it: A at 0, B at 0x800000. */
static bool chip_load_wave(const char *rom_dir)
{
    if (!rom_dir) return false;
    g_wave = (uint8_t *)calloc(1, 0x1000000u);
    if (!g_wave) return false;
    static const struct { const char *name; uint32_t off; } PARTS[2] = {
        { "pr1wavea.2l", 0x000000u }, { "pr1waveb.1l", 0x800000u }
    };
    for (int i = 0; i < 2; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", rom_dir, PARTS[i].name);
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "[AUDIO_HLE] no %s -- the chip path stays off, "
                    "gameplay will be silent\n", path);
            free(g_wave); g_wave = NULL; return false;
        }
        size_t got = fread(g_wave + PARTS[i].off, 1, 0x400000u, f);
        fclose(f);
        if (got == 0) { free(g_wave); g_wave = NULL; return false; }
    }
    c352_init(&g_chip, g_wave, 0x1000000u);
    return true;
}

/* Programs one voice's eight registers and leaves KEYON set; the caller
 * writes the exec register once afterwards to start them together, which is
 * what the driver does. */
static void chip_program_voice(int v, const uint16_t r[8], int keyon)
{
    for (int i = 0; i < 8; i++) {
        uint16_t val = r[i];
        if (i == 3) {            /* flags: BUSY is the chip's own status bit,
                                  * never something a writer sets */
            val = (uint16_t)(val & ~0x8000u);
            if (keyon) val |= 0x4000u;   /* KEYON */
        }
        c352_write(&g_chip, (uint32_t)(v * 8 + i), val, 0xFFFF);
    }
}

/* The sound MCU's own writes to the chip. src/mcu_sound.c calls this for
 * every completed 16-bit register write the driver makes; taking the same
 * lock the mixer does keeps the register file consistent with the samples
 * being generated on the audio thread. */
/* The chip's LIVE voice state, for comparing against MAME's own register
 * read-back. A write shadow cannot answer "is this voice playing": BUSY is
 * the chip's bit, set when key-on executes and cleared when the sample
 * ends, and it never appears in anything the driver wrote. */
void audio_hle_c352_dump(const char *path)
{
    FILE *f;
    int v, r;
    if (!g_chip_ready) return;
    f = fopen(path, "w");
    if (!f) return;
    SDL_AtomicLock(&g_chip_lock);
    for (v = 0; v < 32; v++) {
        const c352_voice_t *vc = &g_chip.voice[v];
        uint16_t reg[8] = { vc->vol_f, vc->vol_r, vc->freq, vc->flags,
                            vc->wave_bank, vc->wave_start, vc->wave_end, vc->wave_loop };
        fprintf(f, "v%02d", v);
        for (r = 0; r < 8; r++) fprintf(f, " %04X", reg[r]);
        fputc('\n', f);
    }
    SDL_AtomicUnlock(&g_chip_lock);
    fclose(f);
}

/* Bring the chip up to MCU cycle `cyc`: generate the frames it would have
 * produced by then and append them to the FIFO. Game thread only -- the chip
 * itself is never touched by the audio thread any more. */
void audio_hle_c352_sync(uint64_t cyc)
{
    if (!g_chip_ready) return;
    const uint64_t target = cyc / MCU_CYC_PER_CHIP_FRAME;
    if (!g_chip_synced) { g_chip_done = target; g_chip_synced = true; return; }
    while (g_chip_done < target) {
        int16_t tmp[512 * 4];
        uint64_t left = target - g_chip_done;
        int n = left > 512 ? 512 : (int)left;
        c352_generate(&g_chip, tmp, n);
        g_chip_done += (uint64_t)n;
        if (!g_chip_on) continue;                 /* keep time, emit nothing */
        SDL_AtomicLock(&g_chip_lock);
        if (g_cfifo_n + n > CHIP_FIFO) {          /* nobody draining: drop the oldest */
            int drop = g_cfifo_n + n - CHIP_FIFO;
            if (drop > g_cfifo_n) drop = g_cfifo_n;
            memmove(g_cfifo, g_cfifo + drop * 4, (size_t)(g_cfifo_n - drop) * 4 * sizeof(int16_t));
            g_cfifo_n -= drop; g_cfifo_frac -= drop; if (g_cfifo_frac < 0) g_cfifo_frac = 0;
        }
        memcpy(g_cfifo + g_cfifo_n * 4, tmp, (size_t)n * 4 * sizeof(int16_t));
        g_cfifo_n += n;
        SDL_AtomicUnlock(&g_chip_lock);
    }
}

void audio_hle_c352_write(unsigned word_off, unsigned val)
{
    if (!g_chip_ready) return;
    c352_write(&g_chip, (uint32_t)word_off, (uint16_t)val, 0xFFFF);
}

/* What the driver reads back: the chip's LIVE register, not an echo of the
 * last write. The caller syncs to the current cycle first. */
unsigned audio_hle_c352_read(unsigned word_off)
{
    if (!g_chip_ready) return 0;
    return c352_read(&g_chip, (uint32_t)word_off);
}

static void chip_set_gameplay(int on)
{
    if (!g_chip_ready) return;
    if (on && !g_chip_on) {
        /* Nothing to program: the MCU owns these registers. */
        SDL_AtomicLock(&g_chip_lock);
        g_cfifo_n = 0; g_cfifo_frac = 0.0;
        SDL_AtomicUnlock(&g_chip_lock);
        g_chip_on = true;
    } else if (!on && g_chip_on) {
        for (int v = 0; v < 32; v++)                 /* KEYOFF everything */
            c352_write(&g_chip, (uint32_t)(v * 8 + 3), 0x2000, 0xFFFF);
        c352_write(&g_chip, 0x202, 0x0000, 0xFFFF);
        g_chip_on = false;
    }
}


/* Pulls `frames` output-rate frames out of the chip, resampling from its own
 * 85333.33Hz. Keeps the fractional cursor and any unconsumed chip frames
 * across calls so the stream does not click at buffer boundaries. */
static void chip_mix(int32_t *acc, int frames, int rate, bool live)
{
    if (!g_chip_ready || !g_chip_on || rate <= 0) return;
    const double chip_hz = (double)C352_CLOCK / (double)C352_DIVIDER;
    double step = chip_hz / (double)rate;

    SDL_AtomicLock(&g_chip_lock);
    if (live) {
        /* The game (and so the driver) is paced by the display; the device by
         * its own crystal. Trim the drain rate by up to 2% to hold the FIFO
         * near CHIP_TARGET instead of letting it run dry or pile up. */
        const double CHIP_TARGET = 4096.0;
        double e = ((double)g_cfifo_n - CHIP_TARGET) / CHIP_TARGET;
        if (e > 1.0) e = 1.0; else if (e < -1.0) e = -1.0;
        step *= 1.0 + 0.02 * e;
    }
    for (int i = 0; i < frames; i++) {
        int i0 = (int)g_cfifo_frac;
        if (i0 + 1 >= g_cfifo_n) break;            /* underrun: silence */
        double fr = g_cfifo_frac - i0;
        /* front L/R only; the rear pair is the cabinet's other speaker pair */
        double l = g_cfifo[i0 * 4 + 0] + (g_cfifo[(i0 + 1) * 4 + 0] - g_cfifo[i0 * 4 + 0]) * fr;
        double r = g_cfifo[i0 * 4 + 1] + (g_cfifo[(i0 + 1) * 4 + 1] - g_cfifo[i0 * 4 + 1]) * fr;
        acc[i * 2 + 0] += (int32_t)l;
        acc[i * 2 + 1] += (int32_t)r;
        g_cfifo_frac += step;
    }
    int drop = (int)g_cfifo_frac;
    if (drop > g_cfifo_n) drop = g_cfifo_n;
    if (drop > 0) {
        memmove(g_cfifo, g_cfifo + drop * 4,
                (size_t)(g_cfifo_n - drop) * 4 * sizeof(int16_t));
        g_cfifo_n -= drop; g_cfifo_frac -= drop;
    }
    SDL_AtomicUnlock(&g_chip_lock);
}

/* Output frames the FIFO can supply right now at `rate` (headless dump). */
static int chip_avail(int rate)
{
    if (!g_chip_ready || !g_chip_on || rate <= 0) return 0;
    const double step = (double)C352_CLOCK / (double)C352_DIVIDER / (double)rate;
    SDL_AtomicLock(&g_chip_lock);
    double room = (double)(g_cfifo_n - 1) - g_cfifo_frac;
    SDL_AtomicUnlock(&g_chip_lock);
    return room > 0 ? (int)(room / step) + 1 : 0;
}

static void mix_frames(int16_t *dst, int frames, int rate, bool live)
{
    if (frames > 0) memset(dst, 0, (size_t)frames * 2 * sizeof(int16_t));
    if (rate <= 0 || frames <= 0) return;
    if (frames > MAX_MIX_FRAMES) {
        fprintf(stderr, "[AUDIO_HLE] mix_frames: %d frames > cap %d, truncating\n",
                frames, MAX_MIX_FRAMES);
        frames = MAX_MIX_FRAMES;
    }

    int32_t acc[MAX_MIX_FRAMES * 2];
    memset(acc, 0, (size_t)frames * 2 * sizeof(int32_t));

    SDL_AtomicLock(&g_voice_lock);
    for (int v = 0; v < MAX_VOICES; v++) {
        voice_t *vc = &g_voices[v];
        if (!vc->active) continue;
        const sfx_asset_t *a = vc->asset;
        double step = (double)a->rate / (double)rate;

        for (int i = 0; i < frames; i++) {
            if (vc->pos + 1.0 >= (double)a->frames) { vc->active = false; break; }
            uint32_t i0 = (uint32_t)vc->pos;
            double frac = vc->pos - (double)i0;
            double s = a->pcm[i0] + (a->pcm[i0 + 1] - a->pcm[i0]) * frac;

            acc[i * 2 + 0] += (int32_t)s;
            acc[i * 2 + 1] += (int32_t)s;

            vc->pos += step;
        }
    }
    SDL_AtomicUnlock(&g_voice_lock);

    SDL_AtomicLock(&g_music_lock);
    if (g_music_current != AUDIO_MUSIC_NONE && g_music_assets[g_music_current].loaded) {
        const sfx_asset_t *a = &g_music_assets[g_music_current];
        double step = (double)a->rate / (double)rate;
        double pos = g_music_pos;

        for (int i = 0; i < frames; i++) {
            uint32_t i0 = (uint32_t)pos;
            uint32_t i1 = (i0 + 1) % a->frames;
            double frac = pos - (double)i0;
            double s = a->pcm[i0] + (a->pcm[i1] - a->pcm[i0]) * frac;

            acc[i * 2 + 0] += (int32_t)s;
            acc[i * 2 + 1] += (int32_t)s;

            pos += step;
            if (pos >= (double)a->frames) pos -= (double)a->frames;
        }
        g_music_pos = pos;
    }
    SDL_AtomicUnlock(&g_music_lock);


    chip_mix(acc, frames, rate, live);

    {
        float g = g_volume;
        for (int i = 0; i < frames * 2; i++) {
            int32_t v = (int32_t)(acc[i] * g);
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            dst[i] = (int16_t)v;
        }
    }
}

static void audio_hle_fill(int16_t *dst, int frames, void *user)
{
    (void)user;
    mix_frames(dst, frames, audio_sample_rate(), true);
}

static void wav_patch_header_at_exit(void)
{
    if (!g_dump_f) return;
    uint32_t data_bytes = (uint32_t)g_dump_bytes;
    uint32_t riff_size = 36 + data_bytes;
    fseek(g_dump_f, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, g_dump_f);
    fseek(g_dump_f, 40, SEEK_SET);
    fwrite(&data_bytes, 4, 1, g_dump_f);
    fclose(g_dump_f);
    g_dump_f = NULL;
}

void audio_hle_init(const char *sounds_dir, const char *rom_dir)
{
    if (!sounds_dir) sounds_dir = "sounds";

    for (size_t i = 0; i < sizeof(SFX_DEFS) / sizeof(SFX_DEFS[0]); i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", sounds_dir, SFX_DEFS[i].file);
        if (!load_wav_mono16(path, &g_assets[SFX_DEFS[i].id]))
            fprintf(stderr,
                    "[AUDIO_HLE] cannot load %s (run tools/extract_sound_assets.py) "
                    "-- that SFX will be silent\n", path);
    }

    /* PROPCYCL_NO_MUSIC=1: skip the looping ambient/music track entirely
     * and mix one-shot SFX only -- the audio equivalent of PROPCYCL_NO_3D/
     * PROPCYCL_NO_SPRITES/PROPCYCL_NO_TEXT elsewhere in this project, for
     * isolating one layer to verify another. Read once here, not in the
     * frame loop (row 40: getenv() in the frame loop segfaults). */
    /* Read by VALUE, not by presence: register row 119 records the same
     * trap on PROPCYCL_ANIMOBJ, where `=0` turned the feature ON and two
     * "with and without" runs came back byte-identical. */
    { const char *e = getenv("PROPCYCL_NO_MUSIC");
      g_music_disabled = (e && *e && *e != '0'); }
    /* PROPCYCL_GAMEPLAY_REC=1 puts the old captured gameplay loop back, to
     * A/B it against the ROM-sample voices. Off by default: that loop is a
     * recording of MAME's whole output and carries its SFX round with it. */
    { const char *e = getenv("PROPCYCL_GAMEPLAY_REC");
      g_gameplay_rec = (e && *e && *e != '0'); }

    /* PROPCYCL_NO_CHIP=1 keeps the chip off and falls back to the decoded-WAV
     * bed/pedal, for A/B and for a run with no wave ROM to hand. */
    { const char *e = getenv("PROPCYCL_NO_CHIP");
      if (!(e && *e && *e != '0')) g_chip_ready = chip_load_wave(rom_dir); }
    /* The sound driver itself. Without the wave ROM there is nothing for it
     * to play, so it is only started when the chip is up. */
    if (g_chip_ready) mcu_sound_init(rom_dir);
    if (g_chip_ready)
        printf("[AUDIO_HLE] C352 running the wave ROM (%s)\n", rom_dir);

    /* PROPCYCL_AUDIODUMP=<path>: mirror the live mix into a WAV file, so a
     * trigger/track can be verified on a box with no speaker (or
     * headless). Read once here, not in the frame loop (row 40:
     * getenv() in the frame loop segfaults). */
    const char *dp = getenv("PROPCYCL_AUDIODUMP");
    if (dp) {
        g_dump_f = fopen(dp, "wb");
        if (g_dump_f) {
            g_dump_rate = audio_sample_rate();
            if (g_dump_rate <= 0) g_dump_rate = 44100; /* headless: no device open */
            uint32_t z = 0;
            fwrite("RIFF", 1, 4, g_dump_f); fwrite(&z, 4, 1, g_dump_f);
            fwrite("WAVE", 1, 4, g_dump_f);
            fwrite("fmt ", 1, 4, g_dump_f);
            { uint32_t sz = 16; fwrite(&sz, 4, 1, g_dump_f); }
            { uint16_t fmt = 1; fwrite(&fmt, 2, 1, g_dump_f); }
            { uint16_t ch = 2; fwrite(&ch, 2, 1, g_dump_f); }
            uint32_t rate = (uint32_t)g_dump_rate;
            fwrite(&rate, 4, 1, g_dump_f);
            uint32_t byte_rate = rate * 2 * 2;
            fwrite(&byte_rate, 4, 1, g_dump_f);
            uint16_t block_align = 4; fwrite(&block_align, 2, 1, g_dump_f);
            uint16_t bits = 16; fwrite(&bits, 2, 1, g_dump_f);
            fwrite("data", 1, 4, g_dump_f); fwrite(&z, 4, 1, g_dump_f);
            atexit(wav_patch_header_at_exit);
        } else {
            fprintf(stderr, "[AUDIO_HLE] cannot open %s for PROPCYCL_AUDIODUMP\n", dp);
        }
    }

    audio_set_source(audio_hle_fill, NULL);
}


void audio_hle_trigger(audio_sfx_t sfx)
{
    if (sfx < 0 || sfx >= AUDIO_SFX_COUNT || !g_assets[sfx].loaded) return;

    SDL_AtomicLock(&g_voice_lock);
    int slot = -1;
    for (int v = 0; v < MAX_VOICES; v++) if (!g_voices[v].active) { slot = v; break; }
    if (slot < 0) slot = 0; /* steal the oldest slot rather than drop the trigger */
    g_voices[slot].asset = &g_assets[sfx];
    g_voices[slot].pos = 0.0;
    g_voices[slot].active = true;
    SDL_AtomicUnlock(&g_voice_lock);
}

static void set_music(audio_music_t track)
{
    /* The comparison must happen under the same lock mix_frames() reads
     * g_music_current/g_music_pos under -- this used to compare outside
     * the lock, racing the audio thread's locked read of the same pair
     * (e.g. this could observe a stale g_music_current, and the audio
     * thread could observe the write to g_music_current split from the
     * write to g_music_pos). */
    SDL_AtomicLock(&g_music_lock);
    if (track != g_music_current) {
        if (propcycl_verbose())
            fprintf(stderr, "[AUDIO_HLE] music track -> %d (was %d) fc=%ld st=%ld sub=%ld course=%ld\n",
                    (int)track, (int)g_music_current, (long)W[0x0C98], (long)W[0x0CBC],
                    (long)W[0x0CC0], (long)W[0x0E0C]);
        g_music_current = track;
        /* Hard switch, no crossfade: a track change is an instant cut,
         * not a blend. Not measured against MAME either way -- the real
         * M37710 sequencer's own transition behaviour at a state change
         * is unknown (that firmware is undecompiled), so this is a
         * declared simplification, not a claim of matching hardware. See
         * AUDIO_PLAN.md's phase-3 "not covered yet" list. */
        g_music_pos = 0.0;
    }
    SDL_AtomicUnlock(&g_music_lock);
}

/* Picks the track for the CURRENT decompiled game state. Mapping measured
 * against MAME (tools/extract_music_assets.py's CAPTURE_RECIPES), not
 * guessed: continuous audio through the whole state-3 menu sequence, a
 * clear step up in level exactly when sub crosses into 2/5/3 (gameplay).
 * See include/audio_hle.h's audio_music_t for the state->track table. */
static audio_music_t music_for_current_state(void)
{
    /* THERE IS NO PRERECORDED MUSIC ANY MORE, DELIBERATELY.
     *
     * Every track here used to be a slice of MAME's own captured output for
     * one game state, looped. That is a recording of the machine, not the
     * machine: it carried MAME's balloon pops and terrain hits round with it
     * every 16.85s, and two runs with completely different player input came
     * out bit-identical, max sample difference 0. Silence is the honest
     * placeholder until the M37710 core drives the C352 for real. */
    return AUDIO_MUSIC_NONE;
}

/* audio_hle_tick(): call once per game frame (main.c, after game_frame()).
 * Always polls game state for the music track (needed whether or not a
 * live device is open). The dump-writing half only runs when there is no
 * device -- PROPCYCL_AUDIODUMP verification on a headless/deviceless run;
 * the live SDL callback drives mix_frames() on its own otherwise. Either
 * way it exercises the exact same mix_frames() path, not a shortcut. */
/* Starts and stops the chip with gameplay. It renders silence until the
 * M37710 core drives it; that is the point. PROPCYCL_NO_CHIP=1 skips it. */
/* The chip is mixed whenever the SOUND DRIVER is running, not only during
 * gameplay. The old gate here dated from when nothing programmed the C352
 * and it only had to be on for the state we had a recording for; now the
 * MCU owns every voice and decides for itself what is playing, so gating on
 * the game state would mute the boot chime, the menus and the coin.
 *
 * Measured: with the gate in place our output was silent for a whole
 * 14-second run while the driver was writing the chip the entire time --
 * chip_mix()'s `!g_chip_on` early return meant c352_generate was never
 * called at all. */
static void gameplay_voices_tick(void)
{
    if (g_chip_ready) chip_set_gameplay(mcu_sound_ready() && !g_music_disabled);
}

float audio_hle_volume(void) { return g_volume; }

void audio_hle_set_volume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_volume = v;
}

void audio_hle_tick(void)
{
    set_music(g_music_disabled ? AUDIO_MUSIC_NONE : music_for_current_state());
    gameplay_voices_tick();

    /* Run the real sound driver for one frame. It reads the commands the
     * game has just written into g_sys.commsram and programs the C352
     * itself, so this is where the game's own sounds come from -- there is
     * nothing for us to decide, only to run. */
    mcu_sound_run_frame();

    if (!g_dump_f || audio_sample_rate() > 0) return;

    /* Headless: write exactly what the driver's clock produced this frame. */
    int n = chip_avail(g_dump_rate);
    if (!g_chip_ready || !g_chip_on) {          /* no chip: keep the old pacing */
        static double acc = 0.0;
        acc += (double)g_dump_rate / (25600000.0 / (814.0 * 525.0));
        n = (int)acc; acc -= n;
    }
    while (n > 0) {
        int16_t buf[8192 * 2];
        int k = n > 8192 ? 8192 : n;
        mix_frames(buf, k, g_dump_rate, false);
        fwrite(buf, sizeof(int16_t), (size_t)k * 2, g_dump_f);
        g_dump_bytes += (long)k * 2 * (long)sizeof(int16_t);
        n -= k;
    }
}
