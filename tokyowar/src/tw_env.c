/*
 * tw_env.c -- the TRACE ORACLE's environment (dev builds only; the shipped game has no such thing).
 *
 * What the 68K sees that is not the 68K -- the master DSP answering the program upload, the sound
 * MCU raising its ready bit, and the instruction an interrupt lands on -- is taken from MAME's own
 * register trace (tools/mame/env_from_trace.py) and replayed here, so the LIFTED 68K can be
 * compared with MAME instruction for instruction before the DSP (T4) and MCU (T5) exist as code.
 * TW_ENV=<file> in tw_trace; without RR_TRACE this file is empty stubs.
 */
#include <stdio.h>
#include <stdlib.h>
#include "tw_mem.h"
#include "tw_hw.h"
#include "tw_env.h"

#ifdef RR_TRACE
#include "lift_rt.h"
#include "lift_cpu.h"

typedef struct { char kind; int flow; uint64_t n; uint32_t addr; int size, bit, level; uint32_t value; } env_t;
static env_t *rec; static size_t nrec, pos;       /* main flow + interrupts, in trace order */
static env_t *erec; static size_t nerec, epos;     /* handler reads, by episode */
static uint64_t main_n, ep_n; static int ep_no = -1;
static uint32_t mism_addr[64]; static unsigned long mism_cnt[64]; static int n_mism; static unsigned long mism_total;
static int active;

/* every place ours held something other than what MAME's 68K saw: DSP / MCU answers, or a bug */
static void note_mismatch(uint32_t addr)
{
    mism_total++;
    for (int i = 0; i < n_mism; i++) if (mism_addr[i] == addr) { mism_cnt[i]++; return; }
    if (n_mism < 64) { mism_addr[n_mism] = addr; mism_cnt[n_mism++] = 1; }
}
void tw_env_report(void)
{
    if (!active) return;
    fprintf(stderr, "[TW] trace oracle: %lu environment reads differed from ours, at %d addresses:\n", mism_total, n_mism);
    for (int i = 0; i < n_mism; i++) fprintf(stderr, "       0x%06X x%lu\n", mism_addr[i], mism_cnt[i]);
}

static void apply(const env_t *e)
{
    switch (e->kind) {
    case 'E': {
        if (rr_read(e->addr, e->size) != e->value) note_mismatch(e->addr);
        rr_write(e->addr, e->size, e->value);
        break;
    }
    case 'B': {
        uint32_t v = rr_read(e->addr, 1);
        if (((v >> e->bit) & 1u) != e->value) note_mismatch(e->addr);
        v = (v & ~(1u << e->bit)) | ((uint32_t)e->value << e->bit);
        rr_write(e->addr, 1, v);
        break;
    }
    case 'I':
        for (int line = 0; line < 4; line++)                /* the syscon line that owns this level */
            if ((g_tw.syscon[line] & 7) == e->level && (g_hw.irq_enabled & (1u << line))) { g_hw.irq_state |= 1u << line; break; }
        rr_irq_enter(e->level);
        break;
    }
}

static void hook(uint32_t pc)
{
    (void)pc;
    if (rr_in_irq) {                                         /* a handler: its reads are keyed by episode */
        while (epos < nerec && (erec[epos].flow < ep_no || (erec[epos].flow == ep_no && erec[epos].n < ep_n))) epos++;   /* skip what was not reached */
        while (epos < nerec && erec[epos].flow == ep_no && erec[epos].n == ep_n) apply(&erec[epos++]);
        ep_n++;
        return;
    }
    while (pos < nrec && rec[pos].n == main_n) {
        const env_t *e = &rec[pos++];
        if (e->kind == 'I') { ep_no++; ep_n = 0; }
        apply(e);                                            /* an 'I' runs its handler here, then the rest at n */
    }
    main_n++;
}

int tw_env_init(const char *path)
{
    if (!path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[TW] cannot open TW_ENV %s\n", path); return 0; }
    char line[128]; size_t cap = 0, ecap = 0;
    while (fgets(line, sizeof line, f)) {
        env_t e = {0}; unsigned long long n; unsigned a, v; int b, sz, lv, fl;
        if (line[0] == 'I' && sscanf(line + 2, "%llu %d", &n, &lv) == 2) { e.kind = 'I'; e.n = n; e.level = lv; }
        else if (line[0] == 'E' && sscanf(line + 2, "%d %llu %x %d %x", &fl, &n, &a, &sz, &v) == 5) { e.kind = 'E'; e.flow = fl; e.n = n; e.addr = a; e.size = sz; e.value = v; }
        else if (line[0] == 'B' && sscanf(line + 2, "%d %llu %x %d %d", &fl, &n, &a, &b, &sz) == 5) { e.kind = 'B'; e.flow = fl; e.n = n; e.addr = a; e.bit = b; e.value = (uint32_t)sz; }
        else continue;
        if (e.kind != 'I' && e.flow >= 0) {                  /* a read inside an interrupt handler */
            if (nerec == ecap) { ecap = ecap ? ecap * 2 : 1024; erec = realloc(erec, ecap * sizeof *erec); }
            erec[nerec++] = e;
        } else {
            if (nrec == cap) { cap = cap ? cap * 2 : 1024; rec = realloc(rec, cap * sizeof *rec); }
            rec[nrec++] = e;
        }
    }
    fclose(f);
    fprintf(stderr, "[TW] trace oracle: %zu main-flow + %zu handler environment records from %s\n", nrec, nerec, path);
    rr_trace_hook = hook;
    active = 1;
    atexit(tw_env_report);                                   /* the trace build exits from inside the trace hook */
    return 1;
}
int tw_env_active(void) { return active; }
#else
void tw_env_report(void) {}
int tw_env_init(const char *path) { (void)path; return 0; }
int tw_env_active(void) { return 0; }
#endif
