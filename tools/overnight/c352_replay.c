/* Replay a captured C352 register-write stream (tools/overnight/dump_c352.lua)
 * through our own ported chip (src/c352.c) and write the front L/R output
 * as a WAV file -- the "ours" side of the AUDIO_PLAN.md phase 2 gate.
 *
 * Usage:
 *   c352_replay <writes.txt> <rom_dir> <out.wav> [total_seconds]
 *
 * writes.txt lines: "<time_seconds> <offset_hex> <data_hex> <mask_hex>"
 * (time decimal machine seconds, the rest hex) -- dump_c352.lua's format.
 * Pacing is by REAL MACHINE TIME, not an assumed video frame rate: early
 * boot/POST does not vblank at a steady 59.9Hz, so a frame-count-based
 * replay desyncs badly in that window (measured: a capture that reached
 * only ~196 video-frame notifications while -seconds_to_run demanded far
 * more simulated time). Each write's own timestamp is exact regardless.
 *
 * total_seconds, if given, pads/truncates the output to that length (e.g.
 * to match a reference -wavwrite capture's duration exactly) by holding
 * whatever chip state the last write left behind. Defaults to the last
 * write's own timestamp.
 *
 * rom_dir must contain pr1wavea.2l and pr1waveb.1l (the extracted/ names).
 * Sample ROM layout matches MAME's ROM_REGION for "c352" in namcos22.cpp:
 * pr1wavea.2l at byte 0, pr1waveb.1l at byte 0x800000, in a 16MB space.
 */
#include "c352.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ROM_SIZE   0x1000000u
#define WAVE_SIZE  0x400000u
#define WAVEB_OFF  0x800000u

static double c352_hz(void) { return (double)C352_CLOCK / (double)C352_DIVIDER; }

static void wav_write_header(FILE *f, uint32_t data_bytes, uint32_t rate, int channels)
{
    uint32_t byte_rate = rate * channels * 2;
    uint16_t block_align = (uint16_t)(channels * 2);
    uint32_t riff_size = 36 + data_bytes;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    { uint32_t sz = 16; fwrite(&sz, 4, 1, f); }
    { uint16_t fmt = 1; fwrite(&fmt, 2, 1, f); }
    { uint16_t ch = (uint16_t)channels; fwrite(&ch, 2, 1, f); }
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    { uint16_t bits = 16; fwrite(&bits, 2, 1, f); }
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

static uint8_t *load_wave_rom(const char *dir)
{
    uint8_t *rom = calloc(1, ROM_SIZE);
    if (!rom) { fprintf(stderr, "out of memory\n"); exit(1); }

    char path[1024];
    snprintf(path, sizeof(path), "%s/pr1wavea.2l", dir);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    if (fread(rom, 1, WAVE_SIZE, f) != WAVE_SIZE) { fprintf(stderr, "short read %s\n", path); exit(1); }
    fclose(f);

    snprintf(path, sizeof(path), "%s/pr1waveb.1l", dir);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    if (fread(rom + WAVEB_OFF, 1, WAVE_SIZE, f) != WAVE_SIZE) { fprintf(stderr, "short read %s\n", path); exit(1); }
    fclose(f);

    return rom;
}

typedef struct { double t; uint32_t offset, data, mask; } write_t;

static write_t *load_writes(const char *path, int *count_out)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }

    int cap = 4096, n = 0;
    write_t *w = malloc((size_t)cap * sizeof(write_t));

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        double t; unsigned offset, data, mask;
        int got = sscanf(line, "%lf %x %x %x", &t, &offset, &data, &mask);
        if (got != 4) continue;
        if (n == cap) { cap *= 2; w = realloc(w, (size_t)cap * sizeof(write_t)); }
        w[n].t = t;
        w[n].offset = offset;
        w[n].data = data;
        w[n].mask = mask;
        n++;
    }
    fclose(f);
    *count_out = n;
    return w;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <writes.txt> <rom_dir> <out.wav> [total_seconds]\n", argv[0]);
        return 1;
    }
    const char *writes_path = argv[1];
    const char *rom_dir = argv[2];
    const char *out_path = argv[3];

    uint8_t *rom = load_wave_rom(rom_dir);

    c352_t chip;
    c352_init(&chip, rom, ROM_SIZE);

    int nwrites;
    write_t *writes = load_writes(writes_path, &nwrites);
    if (nwrites == 0) { fprintf(stderr, "no writes parsed from %s\n", writes_path); return 1; }

    double last_t = writes[nwrites - 1].t;
    double total_seconds = (argc >= 5) ? atof(argv[4]) : last_t;
    if (last_t > total_seconds) total_seconds = last_t;

    double rate = c352_hz();
    long cap_samples = (long)(total_seconds * rate) + 4096;
    int16_t *buf4 = malloc((size_t)cap_samples * 4 * sizeof(int16_t));
    int16_t *out2 = malloc((size_t)cap_samples * 2 * sizeof(int16_t));
    if (!buf4 || !out2) { fprintf(stderr, "out of memory\n"); return 1; }

    long total_samples = 0;

    for (int i = 0; i < nwrites; i++) {
        /* Generate up to (but not including) this write's timestamp with
         * the PREVIOUS register state -- the write takes effect starting
         * exactly at its own moment, matching MAME's lazy stream->update()
         * before every register access. */
        long target = (long)(writes[i].t * rate);
        long n = target - total_samples;
        if (n > 0) {
            c352_generate(&chip, buf4 + total_samples * 4, (int)n);
            total_samples += n;
        }

        /* dump_c352.lua taps mcu.spaces["program"], which is BYTE
         * addressed (M37710 program space: data_width 16, addr_shift 0).
         * c352_device::read/write take the WORD-granular register index
         * (reg_map is `offsetof(...)/sizeof(u16)`), so the captured byte
         * offset must be halved. Verified against the capture itself:
         * voice registers step by 0x10 bytes (8 words x 2), and the
         * keyon/keyoff exec register (documented word offset 0x202)
         * shows up at byte offset 0x404 = 0x202*2. */
        c352_write(&chip, writes[i].offset / 2, (uint16_t)writes[i].data, (uint16_t)writes[i].mask);
    }

    long final_target = (long)(total_seconds * rate);
    long tail = final_target - total_samples;
    if (tail > 0) {
        c352_generate(&chip, buf4 + total_samples * 4, (int)tail);
        total_samples += tail;
    }

    /* Front L/R only -- matches the "speaker" route
     * (m_c352->add_route(0/1, "speaker", ...)) that MAME's -wavwrite for
     * the standard cabinet actually records. */
    for (long i = 0; i < total_samples; i++) {
        out2[i * 2 + 0] = buf4[i * 4 + 0];
        out2[i * 2 + 1] = buf4[i * 4 + 1];
    }

    FILE *of = fopen(out_path, "wb");
    if (!of) { fprintf(stderr, "cannot write %s\n", out_path); return 1; }
    uint32_t data_bytes = (uint32_t)(total_samples * 2 * sizeof(int16_t));
    wav_write_header(of, data_bytes, (uint32_t)(rate + 0.5), 2);
    fwrite(out2, 1, data_bytes, of);
    fclose(of);

    fprintf(stderr, "[c352_replay] %d writes over %.3fs, %ld samples (%.3f Hz) -> %s\n",
            nwrites, total_seconds, total_samples, rate, out_path);
    return 0;
}
