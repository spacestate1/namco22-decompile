/*
 * L3 differential trace — reimpl side.
 *
 * Output directory layout (mirrored by tools/l3/mame_trace.lua):
 *   meta.json     side / sizes / dsp word order
 *   frames.jsonl  one line per captured frame: fc, host frame, state, sub
 *   f%06u.bin     work RAM (0x40000 bytes, big-endian byte-faithful)
 *                 ++ DSP RAM (0x20000 bytes)
 *
 * DSP RAM words here are host-little-endian (the transpiled code writes
 * int32 through host pointers); MAME's are bus-order with only d0-23
 * connected. The differ canonicalizes both to sign-extended 24-bit.
 */
#include "propcycl.h"
#include "trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static char  s_dir[512];
static bool  s_active = false;
static FILE* s_frames_jsonl = NULL;
static uint32_t s_max_fc = 0;
static uint32_t s_last_fc = 0;
static uint32_t s_dumped = 0;

/* WRAM big-endian u32 read (local copy to avoid game_logic.c macros) */
static uint32_t wram32(uint32_t off) {
    return ((uint32_t)g_sys.work_ram[off] << 24) |
           ((uint32_t)g_sys.work_ram[off + 1] << 16) |
           ((uint32_t)g_sys.work_ram[off + 2] << 8) |
            (uint32_t)g_sys.work_ram[off + 3];
}

bool trace_init(const char* dir, uint32_t max_fc) {
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    if (mkdir(s_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[TRACE] cannot create %s: %s\n", s_dir, strerror(errno));
        return false;
    }

    char path[600];
    snprintf(path, sizeof(path), "%s/meta.json", s_dir);
    FILE* mf = fopen(path, "w");
    if (!mf) {
        fprintf(stderr, "[TRACE] cannot write %s\n", path);
        return false;
    }
    fprintf(mf,
        "{\"side\": \"reimpl\", \"wram_size\": %u, \"dsp_size\": %u, "
        "\"dsp_word_order\": \"le\", \"anchor\": \"wram_0x0C98_increment\"}\n",
        (unsigned)WORK_RAM_SIZE, (unsigned)DSPRAM_SIZE);
    fclose(mf);

    snprintf(path, sizeof(path), "%s/frames.jsonl", s_dir);
    s_frames_jsonl = fopen(path, "w");
    if (!s_frames_jsonl) {
        fprintf(stderr, "[TRACE] cannot write %s\n", path);
        return false;
    }

    s_max_fc = max_fc;
    s_active = true;
    printf("[TRACE] L3 trace -> %s (max fc %u)\n", s_dir, max_fc);
    return true;
}

bool trace_active(void) { return s_active; }

void trace_on_frame(uint32_t fc) {
    if (!s_active) return;
    if (s_max_fc && fc > s_max_fc) return;
    if (fc == s_last_fc && s_dumped > 0) return;  /* defensive: once per fc */
    s_last_fc = fc;

    char path[600];
    snprintf(path, sizeof(path), "%s/f%06u.bin", s_dir, fc);
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[TRACE] cannot write %s — disabling trace\n", path);
        s_active = false;
        return;
    }
    fwrite(g_sys.work_ram, 1, WORK_RAM_SIZE, f);
    fwrite(g_sys.dspram, 1, DSPRAM_SIZE, f);
    fclose(f);

    uint32_t state = wram32(0x0CBC);
    uint32_t sub   = wram32(0x0CC0);
    fprintf(s_frames_jsonl,
        "{\"fc\": %u, \"host\": %u, \"state\": %u, \"sub\": %u, \"file\": \"f%06u.bin\"}\n",
        fc, g_sys.frame_count, state, sub, fc);
    fflush(s_frames_jsonl);
    s_dumped++;
}

void trace_finish(void) {
    if (s_frames_jsonl) { fclose(s_frames_jsonl); s_frames_jsonl = NULL; }
    if (s_active)
        printf("[TRACE] %u frames dumped to %s\n", s_dumped, s_dir);
    s_active = false;
}

/* ================= KEYCUS replay (D2) ================= */

typedef struct {
    uint32_t fc;
    uint32_t addr;
    uint32_t data;
    uint32_t mask;
} KeycusRec;

static KeycusRec* s_kc = NULL;
static size_t s_kc_count = 0, s_kc_pos = 0;
static bool s_kc_active = false;
static uint32_t s_kc_misses = 0;

bool trace_keycus_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[TRACE] keycus log %s: %s\n", path, strerror(errno));
        return false;
    }
    size_t cap = 1024;
    s_kc = malloc(cap * sizeof(KeycusRec));
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        KeycusRec r;
        if (sscanf(line, "%u %x %x %x", &r.fc, &r.addr, &r.data, &r.mask) == 4) {
            if (s_kc_count == cap) {
                cap *= 2;
                s_kc = realloc(s_kc, cap * sizeof(KeycusRec));
            }
            s_kc[s_kc_count++] = r;
        }
    }
    fclose(f);
    s_kc_active = s_kc_count > 0;
    printf("[TRACE] keycus replay: %zu records from %s\n", s_kc_count, path);
    return s_kc_active;
}

bool trace_keycus_active(void) { return s_kc_active; }

/* Consume the next recorded keycus value. The recording and the replay
 * must perform the same read sequence; if they drift, fail LOUDLY
 * (GUARDRAILS G3) — a silent fallback would turn a determinism break
 * into a heisenbug. */
uint32_t trace_keycus_read(uint32_t addr) {
    if (s_kc_pos < s_kc_count) {
        KeycusRec* r = &s_kc[s_kc_pos++];
        if (r->addr != addr && s_kc_misses++ < 16)
            fprintf(stderr,
                "[TRACE] keycus addr drift: replay #%zu recorded 0x%06x, "
                "game read 0x%06x (recorded fc %u)\n",
                s_kc_pos - 1, r->addr, addr, r->fc);
        return r->data;
    }
    if (s_kc_misses++ < 16)
        fprintf(stderr, "[TRACE] keycus replay EXHAUSTED at read #%zu "
                "(addr 0x%06x) — returning 0\n", s_kc_pos, addr);
    s_kc_pos++;
    return 0;
}
