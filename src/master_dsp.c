/*
 * master_dsp.c -- see include/master_dsp.h.
 *
 * The hardware protocol, from the ROM (main_loop @0x00BF2A, irq_vblank
 * @0x00C0BE, dsp_polygon_ram_init @0x022CAE):
 *   - the CPU sets polygon-RAM word 0 to 1 while it builds a frame and clears
 *     it when done;
 *   - at vblank, if word 0 == 0 (CPU done) and word 1 == 0 (master done), the
 *     CPU flips the list buffer (word 4), points its cursor at the other list
 *     and RINGS the master: word 1 = 1;
 *   - the master (INT0 -> BIOS 0x0002 -> the handler whose address the
 *     master's own init stores at data 0x23A) builds the scene from the list
 *     the CPU just finished, clears word 1, and IDLEs.
 * The master is far faster than a frame on the board, so running it to IDLE
 * inside the vblank call is exact in effect, and it is what the gate does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "propcycl.h"
#include "c25.h"              /* the shared engine's master DSP (engine/c25) */
#ifdef PROPCYCL_ORACLE
#include "c25_oracle.h"
#endif
#include "master_dsp.h"

_Static_assert(offsetof(SystemState, dspram) % 4 == 0, "dspram must be word aligned");
_Static_assert(DSPRAM_SIZE == C71_POLY_WORDS * 4, "dspram is the 0x8000-word polygon RAM");

#define PROG_COUNT_ADDR  0x43748u    /* upload word count - 1 */
#define PROG_BLOCK_ADDR  0x4374Au    /* the master program, BE16, to program 0x4000 */
#define IDLE_OP          0xCE1F
#define MAX_FRAME_STEPS  3000000L
#define DOORBELL_WAIT    0x404A      /* `lac 01h ; bz 404Ah` with dp 0x100: spin on word 1 */
#define BIOS_HANDOFF     0x01A8      /* BIOS: call header[1] x8, install slots, jump to header[4] */
#define BIOS_WORD0_WAIT  0x01C2      /* BIOS: `lac 00h ; bnz 01C2h` -- wait for the CPU's word 0 */

static c71_t   *g_m;
static bool     g_active;
static bool     g_have_out;
static bool     g_parked;       /* stopped at the doorbell wait, not at IDLE */
static uint32_t g_out[C71_POLY_WORDS];
static long     g_frames, g_fail;
static int      g_log;
#ifdef PROPCYCL_ORACLE
static void lockstep_start(void);
static void lockstep_report(void);
#endif          /* PROPCYCL_MASTERLOG, read once (register row 40) */

bool master_dsp_active(void) { return g_active; }
const uint32_t *master_dsp_output(void) { return g_have_out ? g_out : NULL; }

static uint32_t *poly(void) { return (uint32_t *)(void *)g_sys.dspram; }

/* Run until an IDLE retires, or until the master reaches the doorbell wait
 * with the doorbell down (it would spin there until the CPU rings: on the
 * board that is the program's first wait after its init). False on a fault
 * or a runaway. */
static bool run_to_idle(long *steps_out)
{
    long st = 0;
    g_parked = false;
    while (st < MAX_FRAME_STEPS) {
        uint16_t pc0 = g_m->pc;
        if ((pc0 == DOORBELL_WAIT && (g_m->poly[1] & 0xFFFFFF) == 0) ||
            (pc0 == BIOS_WORD0_WAIT && (g_m->poly[0] & 0xFFFFFF) != 0)) {
            g_parked = true; if (steps_out) *steps_out = st; return true;
        }
        if (g_log > 1 && (st < 600 || (g_log > 2 && st >= 54040 && st < 54400)))
            printf("  [MT] %4ld pc=%04X op=%04X acc=%08X dp=%X ar=%04X arp=%d imr=%X prd=%X tim=%X intm=%d ar7=%X\n", st, pc0, g_m->prog[pc0],
                   (unsigned)g_m->acc, g_m->dp, g_m->ar[g_m->arp], g_m->arp, g_m->imr, g_m->prd, g_m->tim, g_m->intm, g_m->ar[7]);
        uint16_t prevpc = pc0;
        if (!c71_step(g_m)) {
            fprintf(stderr, "[MASTER] stopped after %ld steps: %s\n", st, g_m->error);
            return false;
        }
        st++;
        if (g_log > 1 && prevpc >= 0x4000 && g_m->pc < 0x4000)
            printf("  [MJ] %ld: %04X (op %04X) -> %04X sp=%d top=%04X acc=%08X\n", st, prevpc, g_m->prog[prevpc], g_m->pc,
                   g_m->sp, g_m->sp ? g_m->stack[g_m->sp-1] : 0, (unsigned)g_m->acc);
        if (g_m->prog[pc0] == IDLE_OP) { g_m->pc = pc0; if (steps_out) *steps_out = st; return true; }
    }
    fprintf(stderr, "[MASTER] no IDLE after %ld steps (pc %04X)\n", st, g_m->pc);
    return false;
}

bool master_dsp_init(const char *rom_dir)
{
    const char *e = getenv("PROPCYCL_MASTER");
    if (!e || atoi(e) == 0) return false;
    g_log = getenv("PROPCYCL_MASTERLOG") ? atoi(getenv("PROPCYCL_MASTERLOG")) + 1 : 0;

    char bios[1024];
    snprintf(bios, sizeof bios, "%s/c71.bin", rom_dir ? rom_dir : "extracted");
    g_m = calloc(1, sizeof *g_m);
    if (!g_m || !c71_load(g_m, bios, NULL)) {
        fprintf(stderr, "[MASTER] no DSP BIOS at %s -- using the built-in scene expansion\n", bios);
        free(g_m); g_m = NULL;
        return false;
    }
    uint32_t cnt = (uint32_t)((g_sys.rom[PROG_COUNT_ADDR] << 8) | g_sys.rom[PROG_COUNT_ADDR + 1]) + 1;
    if (cnt > 0x4000) { fprintf(stderr, "[MASTER] bad program size %u\n", cnt); free(g_m); g_m = NULL; return false; }

    /* Boot the way the board does: the BIOS from its reset vector (self
     * tests, register and stack set-up -- AR7 = 0x7F, as MAME's own register
     * dump shows) up to its hand-off at 0x01A8, where the 68K's upload has
     * put the program at 0x4000. The BIOS then runs the program's header:
     * header[1] eight times, the INT0 slot 0x23A = header[3] (0x4008, the
     * frame handler), the timer slot 0x23D = header[0], waits for the CPU to
     * clear word 0, clears the doorbell and enters header[4]. The self tests
     * scribble over polygon RAM and extram, so the CPU's polygon RAM is saved
     * and put back at the hand-off, and the program loaded there. */
    static uint32_t save[C71_POLY_WORDS];
    memcpy(save, poly(), sizeof save);
    memset(poly(), 0, sizeof save);     /* power-on: the BIOS reads the upload-protocol words */
    /* Super System 22: a port-2 read runs the PDP command block; point RAM at
     * 0xF80000; this host parks the master at IDLE itself (run_to_idle), so
     * IDLE retires as a no-op; port 3 leaves the BIO pin alone. */
    g_m->ss22 = 1; g_m->ptram_base = C71_PTRAM_SS22; g_m->idle_halts = 0; g_m->port3_bioz = 0;
    /* THE PROGRAM: translated to C at build time (gen/pc_c25.c, from the ROM
     * files by tools/gen/c25_translate.py). The oracle build can run the
     * interpreter instead (PROPCYCL_C25=oracle) -- the gate. */
    { extern bool pc_c25_exec(c71_t *, int); g_m->xlat = pc_c25_exec; }
#ifdef PROPCYCL_ORACLE
    { const char *o = getenv("PROPCYCL_C25"); if (o && !strcmp(o, "oracle")) { c25_oracle_use(g_m); fprintf(stderr, "[MASTER] program: the interpreter ORACLE\n"); } }
#endif
    g_m->poly = poly();
    g_m->ptrom = (const uint32_t *)(const void *)g_pointrom;   /* signed24; the port sign-extends anyway */
    g_m->ptrom_words = g_pointrom_count;
    c71_reset(g_m);
    g_m->pc = 0x0000;
    long st = 0;
    while (g_m->pc != BIOS_HANDOFF && st < 2000000) {
        if (!c71_step(g_m)) { fprintf(stderr, "[MASTER] BIOS stopped: %s\n", g_m->error); free(g_m); g_m = NULL; return false; }
        st++;
    }
    if (g_m->pc != BIOS_HANDOFF) { fprintf(stderr, "[MASTER] BIOS never reached its hand-off\n"); free(g_m); g_m = NULL; return false; }
    memcpy(poly(), save, sizeof save);
    /* Both list buffers start EMPTY -- a lone -1, what the CPU's end-of-frame
     * terminator (ROM 0x02221C) leaves in a list it wrote nothing into, and
     * what MAME's attract shows at f301. On the board the first doorbell
     * always follows a CPU frame, so the master never sees a buffer without
     * one; here the first vblank can ring before the CPU has run a frame. */
    { uint32_t *pw = poly();
      if ((pw[0x4100] & 0xFFFFFF) == 0) pw[0x4100] = 0xFFFFFFFFu;
      if ((pw[0x6100] & 0xFFFFFF) == 0) pw[0x6100] = 0xFFFFFFFFu; }
    for (uint32_t i = 0; i < cnt; i++)
        g_m->prog[0x4000 + i] = (uint16_t)((g_sys.rom[PROG_BLOCK_ADDR + 2*i] << 8) | g_sys.rom[PROG_BLOCK_ADDR + 2*i + 1]);
    long st2 = 0;
    if (!run_to_idle(&st2)) { free(g_m); g_m = NULL; return false; }
    st += st2;
    printf("[MASTER] booted: %u-word program, init %ld steps, %s at %04X\n", cnt, st,
           g_parked ? "waiting for the doorbell" : "IDLE", g_m->pc);
    g_active = true;
#ifdef PROPCYCL_ORACLE
    { const char *o = getenv("PROPCYCL_C25");
      if (o && !strcmp(o, "lockstep")) { lockstep_start(); atexit(lockstep_report); } }
#endif
    return true;
}

/* One master frame on g_m (INT0 at the idle loop, run to IDLE). `primary`:
 * the machine whose output the renderer uses (the lockstep's shadow is not). */
static void frame_on(bool primary)
{
    uint32_t *pw = g_m->poly;

    memset(g_m->written, 0, sizeof g_m->written); g_m->n_written = 0;
    if (!g_parked && (pw[1] & 0xFFFFFF) == 0) return;   /* idle and not rung */
    if (primary && g_log > 1 && g_frames < 3) {
        printf("[MASTER] in: parked=%d word0=%X word1=%X word4=%X\n", g_parked, pw[0], pw[1], pw[4]);
        for (int b = 0; b < 2; b++) {
            int base = b ? 0x6100 : 0x4100, i;
            printf("  list %04X:", base);
            for (i = 0; i < 0x1E00; i++) {
                uint32_t w = pw[base + i] & 0xFFFFFF;
                if (i < 40) printf(" %06X", w);
                if (w == 0x8010 && (pw[base + i + 1] & 0xFFFFFF) == 0xFFFFFF) break;
            }
            printf("  ... terminator at +%X\n", i);
        }
    }
    if (!g_parked) {
        /* INT0 at the idle loop: return past the IDLE, vector 0x0002 */
        if (g_m->sp >= 64) { fprintf(stderr, "[MASTER] stack full\n"); g_active = false; return; }
        g_m->stack[g_m->sp++] = (uint16_t)(g_m->pc + (g_m->prog[g_m->pc] == IDLE_OP ? 1 : 0));
        g_m->pc = 0x0002; g_m->intm = 1;
    }                            /* else: resume at the doorbell wait, which now falls through */

    long st = 0;
    if (!run_to_idle(&st)) {
        if (primary && ++g_fail > 3) { fprintf(stderr, "[MASTER] disabled after repeated faults\n"); g_active = false; }
        return;
    }
    if (!primary) return;
    for (int i = 0; i < C71_POLY_WORDS; i++) g_out[i] = pw[i] & 0xFFFFFF;
    g_have_out = true;
    if (++g_frames <= 3 || g_log)
        printf("[MASTER] frame %ld: %ld steps, %u polygon-RAM writes\n", g_frames, st, g_m->n_written);
}

#ifdef PROPCYCL_ORACLE
/* ---- PROPCYCL_C25=lockstep: THE TRANSLATION GATE, in one process --------------
 * A SHADOW master runs the interpreter oracle beside the translated one. Each
 * frame it gets the same polygon RAM (the CPU's writes included), runs the same
 * host logic, and afterwards every register, data RAM, program RAM, point RAM,
 * the stack and polygon RAM must be equal. Independent of whether the game run
 * itself is deterministic (Prop Cycle's gameplay is not: register row 147). */
static c71_t   *g_sh;
static bool     g_sh_parked;
static uint32_t g_sh_poly[C71_POLY_WORDS];
static long     g_ls_frames, g_ls_bad;

static const char *ls_compare(const c71_t *a, const c71_t *b)
{
#define F(x) if (a->x != b->x) return #x
    F(pc); F(pfc); F(t); F(acc); F(p); F(arp); F(arb); F(dp); F(pm); F(sxm); F(ovm); F(intm);
    F(c); F(tc); F(cnf); F(imr); F(prd); F(tim); F(tint_pend); F(sp); F(rpt); F(bank); F(latch);
    F(pt_addr); F(pt_data); F(bioz); F(idle); F(ifr); F(steps); F(n_written);
#undef F
    if (memcmp(a->ar, b->ar, sizeof a->ar)) return "ar";
    if (memcmp(a->stack, b->stack, sizeof a->stack)) return "stack";
    if (memcmp(a->ram, b->ram, sizeof a->ram)) return "data RAM";
    if (memcmp(a->prog, b->prog, sizeof a->prog)) return "program RAM";
    if (memcmp(a->ptram, b->ptram, sizeof a->ptram)) return "point RAM";
    if (memcmp(a->poly, b->poly, sizeof g_sh_poly)) return "polygon RAM";
    return NULL;
}

static void lockstep_frame(void)
{
    memcpy(g_sh_poly, g_m->poly, sizeof g_sh_poly);      /* the CPU's writes since last frame */
    frame_on(true);
    c71_t *m = g_m; bool pk = g_parked;
    g_m = g_sh; g_parked = g_sh_parked;
    frame_on(false);
    g_sh_parked = g_parked;
    g_m = m; g_parked = pk;
    g_ls_frames++;
    /* C25_LS_INJECT=<frame>: NEGATIVE CONTROL -- flip one data-RAM bit of the
     * translated machine on that frame; the gate must report it */
    { static long inj = -2; if (inj == -2) { const char *e = getenv("C25_LS_INJECT"); inj = e ? atol(e) : -1; }
      if (inj == g_ls_frames) g_m->ram[0x300] ^= 1; }
    const char *diff = ls_compare(g_m, g_sh);
    if (!diff && g_parked != g_sh_parked) diff = "parked";
    if (diff) {
        if (++g_ls_bad <= 5)
            fprintf(stderr, "[C25-LOCKSTEP] frame %ld: translation != oracle (%s); pc %04X vs %04X\n",
                    g_ls_frames, diff, g_m->pc, g_sh->pc);
        /* resync so one fault is reported once */
        uint32_t *keep = g_sh->poly; bool (*x)(c71_t *, int) = g_sh->xlat;
        *g_sh = *g_m; g_sh->poly = keep; g_sh->xlat = x;
        memcpy(g_sh_poly, g_m->poly, sizeof g_sh_poly); g_sh_parked = g_parked;
    }
    if (g_ls_frames % 600 == 0)
        fprintf(stderr, "[C25-LOCKSTEP] %ld frames, %ld differing\n", g_ls_frames, g_ls_bad);
}

static void lockstep_start(void)
{
    g_sh = malloc(sizeof *g_sh);
    if (!g_sh) return;
    *g_sh = *g_m;
    memcpy(g_sh_poly, g_m->poly, sizeof g_sh_poly);
    g_sh->poly = g_sh_poly;
    g_sh_parked = g_parked;
    c25_oracle_use(g_sh);
    fprintf(stderr, "[C25-LOCKSTEP] shadow master: the interpreter oracle\n");
}
static void lockstep_report(void)
{
    if (g_sh) fprintf(stderr, "[C25-LOCKSTEP] END: %ld frames, %ld differing -> %s\n",
                      g_ls_frames, g_ls_bad, g_ls_bad ? "FAIL" : "PASS");
}
#endif

void master_dsp_vblank(void)
{
    if (!g_active) return;
#ifdef PROPCYCL_ORACLE
    if (g_sh) { lockstep_frame(); return; }
#endif
    frame_on(true);
}
