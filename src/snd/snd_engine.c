/* snd_engine.c -- Prop Cycle's sound driver (the M37710 program in
 * pr1data.8k) rewritten as readable C.
 *
 * HOW THE REWRITE WORKS. The driver still runs as the translated program
 * (gen/snd_driver.c, one C case per original instruction). A routine written
 * here is marked with SND_ENTRY(address, function); tools/gen/snd_translate.py
 * then calls the function at that address instead of translating the
 * instruction there. The function does the routine's whole job on the same
 * memory and returns the way the original returns, so the translated code
 * around it cannot tell the difference -- including the CPU registers and flags
 * it leaves behind (the "exit state" at the end of each routine: glue for the
 * hybrid phase, deleted once the routine's callers are rewritten too).
 *
 * TWO KINDS OF READABLE ROUTINE. A TIMED one (SND_ENTRY's third argument =
 * its maximum cycle cost) charges the original's cycles and bus accesses and is
 * held to the CYCLE-EXACT gate (tools/gen/snd_gate.sh). An UNTIMED one (third
 * argument 0) is plain C: it runs whole whenever reached, may call translated
 * routines with snd_call(), and is held to the EVENT-EXACT gate
 * (tools/gen/snd_event_gate.sh) -- the notes it plays must equal the oracle's
 * within what the original meets against itself. The tick and everything
 * under it are untimed; the cycle gate stops applying once they are.
 *
 * GATES (history: both were required for the timed routines):
 *   - the chip trace (PROPCYCL_CHIPTRACE): every frame's register image and
 *     every key-on must equal the oracle's (propcycl_sndoracle) on every
 *     scenario. A readable routine runs atomically, so interrupts land at
 *     slightly different cycles and the audio bytes are no longer compared.
 *   - MAME, note for note: tools/overnight/c352_stream_gate.py against
 *     tools/overnight/c352_gameplay.lua's capture.
 *
 * MEMORY MAP, as the driver sees it (direct page = 0, data bank usually 0):
 *   0x0000-0x007F   on-chip peripherals (timers, ports, A-D)
 *   0x0080-0x03FF   the driver's own variables (named below as they are found)
 *   0x2000-0x2FFF   the C352: 32 voices x 8 registers (vol_f, vol_r, freq,
 *                   flags, bank, start, end, loop), 0x400 control, 0x404 exec
 *   0x4000-0xBFFF   RAM shared with the 68K: command words at 0x4000 + slot*2,
 *                   parameters at 0x4100 + n*2, the CHANNEL RECORDS (0xC0
 *                   bytes each, from 0x7000) and the dispatch vectors at 0xBF00
 *   0xC000-0xFFFF   the program and its tables (pr1data.8k 0xC000-0xFFFF)
 */
#include "snd_engine.h"
#include "m377_sem.h"
#include <stdio.h>
#include <stdlib.h>

#define SND_ENTRY(addr, fn, maxcyc)   /* marker read by tools/gen/snd_translate.py */

/* May a readable routine costing at most `maxcyc` cycles run as one block now?
 * Only if no timer, A-D conversion, board pin or frame end falls inside it --
 * then the original could not have been interrupted between its instructions
 * either. Otherwise the translated instructions run. */
uint64_t mcu_sound_next_pin_event(void);
static uint64_t g_atomic_denied;
int snd_atomic_ok(m37710_t *c, unsigned maxcyc)
{
    static int off = -1; if (off < 0) off = getenv("PROPCYCL_SNDNOTIMED") != NULL;   /* TEST: every timed routine declines */
    if (off) return 0;
    const uint64_t end = c->cycles + maxcyc;
    if (end < m37710_next_event(c) && end < mcu_sound_next_pin_event()) return 1;
    g_atomic_denied++;
    return 0;
}

/* A readable routine that costs MORE than the maximum it declared could have
 * run straight across an interrupt or the frame end -- the window check above
 * would have been wrong. That showed up as a few frames of wrong volume per
 * run, so it is checked on every call and stops the sound loudly. */
void snd_cost_check(m37710_t *c, uint32_t pc, uint64_t t0, unsigned maxcyc)
{
    if (c->cycles - t0 <= maxcyc) return;
    fprintf(stderr, "[SND] readable routine at %06X cost %llu cycles, declared at most %u -- raise SND_ENTRY's maxcyc\n",
            pc, (unsigned long long)(c->cycles - t0), maxcyc);
    c->unimpl_hit = true; c->unimpl_pc = pc; c->unimpl_op = 0xFFFE;
}

/* PROPCYCL_SNDSTATS=1: at exit, how often each readable routine RAN versus
 * DECLINED (a path it leaves to the original). A gate that passes proves
 * nothing about a routine that never ran -- this says whether it did. */
enum { R_VOL_CH, R_VOL_SCR, R_GLIDE, R_PENV, R_PAN, R_LEVEL, R_TICK, R_TICKBODY, R_MAILBOX, R_CHANNELS, R_CHUPDATE,
       R_HOOK_FX1, R_HOOK_FX4, R_HOOK_VOL, R_PAN_VOL, R_NOTE_EVENT, R_KEY_ON, R_SEQUENCER, R_SEQCMD, R_IDLE, R_NEWCMD,
       R_STOP_TRACK, R_STOP_SLOT, R_ALLOC, R_ENV, R_SAMPLE, R_STREAM, R_FIXPAN, R_PANSCRIPT, R_N };
static const char *const R_NAME[R_N] = { "voice_volume_from_channel 0xE97E", "voice_volume_from_scratch 0xE987",
                                         "pitch_glide 0xEBB8", "pitch_envelope 0xEBFD",
                                         "pan_to_speakers 0xE87E", "slot_level 0xD745",
                                         "tick 0xC258 (untimed)", "tick_body 0xD684 (untimed)",
                                         "mailbox 0xD6E3 (untimed)", "channels 0xE46E (untimed)",
                                         "channel_update 0xE8DB (untimed)",
                                         "hook fx1 +0x5C 0xEF45", "hook fx4 +0x5A 0xE9D6", "hook volume +0x60 0xECAA",
                                         "pan_then_volume 0xECC4", "note_event 0xE4E7 (untimed)",
                                         "key_on 0xE5A7 (untimed)", "sequencer 0xD9D0 (untimed)",
                                         "sequence commands (readable ones)",
                                         "idle wait 0xC12B (skipped to next event)",
                                         "new command 0xD788 (untimed)",
                                         "stop track 0xE18D (untimed)", "stop slot + children 0xD982 (untimed)",
                                         "voice allocator (6 routines, untimed)",
                                         "volume envelope (untimed)", "sample change 0xF013/0xF064 (untimed)",
                                         "sample streaming 0xEB29 (untimed)", "fixed pan 0xE7FE (untimed)",
                                         "pan script 0xECD4/0xED7B/0xED8B (untimed)" };
static uint64_t g_ran[R_N], g_declined[R_N];
static uint64_t g_seq_cmd[64];      /* sequencer commands run, by number */
/* PROPCYCL_SNDTRACE=1: every sequence command (byte, pointer, bank, handler)
 * and every bail, on stderr -- the first thing to diff against the oracle's
 * PROPCYCL_MCUPC=D9F3 when a command's operand length is wrong. */
static int trace_on(void) { static int t = -1; if (t < 0) { const char *e = getenv("PROPCYCL_SNDTRACE"); t = e && *e && *e != '0'; } return t; }
static void stats_report(void)
{
    fprintf(stderr, "[SND] readable routines (ran / declined), atomic window denied %llu times:\n",
            (unsigned long long)g_atomic_denied);
    for (int i = 0; i < R_N; i++)
        fprintf(stderr, "[SND]   %-36s %10llu / %llu\n", R_NAME[i],
                (unsigned long long)g_ran[i], (unsigned long long)g_declined[i]);
    fprintf(stderr, "[SND] sequencer commands run:");
    for (int i = 0; i < 64; i++) if (g_seq_cmd[i]) fprintf(stderr, " %02X:%llu", i, (unsigned long long)g_seq_cmd[i]);
    fprintf(stderr, "\n");
}
static inline int ran(int r)      { static int armed = -1;
                                    if (armed < 0) { const char *e = getenv("PROPCYCL_SNDSTATS"); armed = e && *e && *e != '0';
                                                     if (armed) atexit(stats_report); }
                                    g_ran[r]++; return 1; }
static inline int declined(int r) { g_declined[r]++; return 0; }

/* ---- the driver's variables (IRAM, direct page 0) ------------------------ */
enum {
    V_SCRATCH       = 0xC0,  /* 8 bytes of scratch shared by the voice routines */
    V_SPEAKER_ATTEN = 0xC0,  /* the same 4 scratch bytes as the pan law's output:
                                front L, front R, rear L, rear R (0xFF = off) */
    V_PAN_TMP       = 0xC4,  /* pan position scratch */
    V_PAN_FOLD      = 0xC5,  /* the pan law's first value, before it is placed */
    V_MASTER_ATTEN  = 0xCC,
    V_PAN_MASK      = 0xF4,  /* pan position mask */  /* attenuation added to every voice's volume */
    V_VOICE_REGS    = 0xEA,  /* address of the C352 voice being updated (0x2000 + v*16) */
};
/* ---- channel record fields (offsets from the record's address) ----------- */
enum {
    CH_TARGET_PITCH = 0x04,  /* 16-bit: the pitch the channel is heading for */
    CH_PENV_ON      = 0x07,  /* byte: pitch envelope running (script in bank 0x21) */
    CH_GLIDE        = 0x0D,  /* byte: glide rate; 0 = jump straight to the target */
    CH_PITCH        = 0x42,  /* 16-bit */  /* 16-bit: the channel's current pitch */
    CH_ATTEN        = 0x70,  /* 4 bytes: attenuation for the chip's four volume
                                bytes, in register order: vol_f low (front
                                right), vol_f high (front left), vol_r low (rear
                                right), vol_r high (rear left) */
};
/* ---- ROM tables ----------------------------------------------------------- */
enum {
    T_ATTEN_TO_VOLUME = 0xF106,
    T_PAN_LAW         = 0xF306,  /* 64 bytes: attenuation across a quarter turn of pan */  /* 256 bytes: attenuation (0 = loudest) -> C352 volume byte */
};

/* direct-page address (the driver keeps the direct page at 0, but say so) */
static inline uint32_t dp(const m37710_t *c, uint32_t a) { return (c->dpr + a) & 0xFFFF; }
/* 16-bit data-bank address, as the original's absolute and indexed modes form it */
static inline uint32_t dbank(const m37710_t *c, uint32_t a) { return (((uint32_t)c->dt << 16) + a) & 0xFFFFFF; }

/* TIMING. The driver shares one clock with its interrupts and the C352, so a
 * readable routine must cost exactly the cycles the original does and make its
 * bus accesses in the same order -- otherwise the timer interrupts land at
 * other moments and the chip trace drifts (measured: 187 of 4000 frames of the
 * coin run differed, first at frame 304, until this was done). The cycle model
 * (src/snd/m377_sem.h) charges one cycle per bus access, which rd8/wr8 already
 * do, plus, per instruction, one for dispatch and one per byte fetched:
 * op(c, len) charges that. A taken branch costs 2 more. */
/* Look at a byte without a bus access (no cycle): only for deciding, on entry,
 * whether a readable routine handles this case at all. */
static inline uint8_t peek8(m37710_t *c, uint32_t a) { return c->read8(c->user, a & 0xFFFFFF); }

static inline void op(m37710_t *c, unsigned len) { c->cycles += 1u + len; c->fetches += len; }

/* =========================================================================
 * VOICE VOLUME -> CHIP  (0xE97E, second entry 0xE987)
 *
 * The four attenuation bytes plus the master attenuation give each of the
 * voice's four chip volume bytes through the ROM curve at 0xF106; a sum that
 * passes 255 is silence. The result goes to the voice's vol_f and vol_r.
 * 0xE97E takes the attenuations from the channel record (X); 0xE987 is entered
 * with them already in the scratch bytes 0xC0-0xC3 (the caller at 0xECD1).
 * ========================================================================= */
static void voice_volume_write(m37710_t *c)
{
    /* the four output bytes start at 0 (silence): LDM #0 x2 */
    op(c, 4); wr16(c, dp(c, V_SCRATCH + 4), 0);
    op(c, 4); wr16(c, dp(c, V_SCRATCH + 6), 0);
    op(c, 2); op(c, 1);                        /* SEP #X, SEM: 8-bit A and X */

    bool carry = false, overflow = false;
    for (int i = 0; i < 4; i++) {
        op(c, 2); const uint8_t att = rd8(c, dp(c, V_SCRATCH + i));
        op(c, 1);                              /* CLC */
        op(c, 2); const uint8_t master = rd8(c, dp(c, V_MASTER_ATTEN));
        const unsigned sum = (unsigned)att + master;
        carry = sum > 0xFF;
        overflow = (~(att ^ master) & (att ^ sum) & 0x80) != 0;
        op(c, 2);                              /* BCS: past 255 -> leave it silent */
        if (carry) { c->cycles += 2; continue; }
        op(c, 1); c->x = (uint16_t)sum;        /* TAX (8-bit X: the exit state keeps it) */
        op(c, 3); const uint8_t vol = rd8(c, dbank(c, T_ATTEN_TO_VOLUME + sum));
        op(c, 2); wr8(c, dp(c, V_SCRATCH + 4 + i), vol);
    }
    op(c, 2); op(c, 1);                        /* CLP #X, CLM: back to 16-bit */

    op(c, 2); const uint16_t voice = rd16(c, dp(c, V_VOICE_REGS));
    op(c, 2); const uint16_t vol_f = rd16(c, dp(c, V_SCRATCH + 4));
    op(c, 3); wr16(c, dbank(c, voice + 0), vol_f);
    op(c, 2); const uint16_t vol_r = rd16(c, dp(c, V_SCRATCH + 6));
    op(c, 3); wr16(c, dbank(c, voice + 2), vol_r);

    /* exit state, as the original leaves it */
    c->y = voice;
    c->a = vol_r;
    c->ps &= (uint16_t)~(M377_M | M377_X | M377_N | M377_Z | M377_C | M377_V);
    if (vol_r == 0)      c->ps |= M377_Z;
    if (vol_r & 0x8000)  c->ps |= M377_N;
    if (carry)           c->ps |= M377_C;
    if (overflow)        c->ps |= M377_V;
    op(c, 1); c->pc = pop16(c);              /* RTS */
}

SND_ENTRY(0xE97E, snd_voice_volume_from_channel, 180)
int snd_voice_volume_from_channel(m37710_t *c)
{
    const uint32_t ch = c->x;
    op(c, 1);                                                            /* CLM */
    op(c, 2); const uint16_t lo = rd16(c, dp(c, CH_ATTEN + 0 + ch));
    op(c, 2); wr16(c, dp(c, V_SCRATCH + 0), lo);
    op(c, 2); const uint16_t hi = rd16(c, dp(c, CH_ATTEN + 2 + ch));
    op(c, 2); wr16(c, dp(c, V_SCRATCH + 2), hi);
    voice_volume_write(c);
    return ran(R_VOL_CH);
}

SND_ENTRY(0xE987, snd_voice_volume_from_scratch, 160)
int snd_voice_volume_from_scratch(m37710_t *c)
{
    voice_volume_write(c);
    return ran(R_VOL_SCR);
}

/* =========================================================================
 * PITCH GLIDE  (0xEBB8, once per channel per tick)
 *
 * With the channel's glide rate at 0 the current pitch simply takes the
 * target. A non-zero rate steps toward it (a signed step scaled by MPY through
 * 0xECA2); no scenario gated so far (attract, all stages, both modes, the
 * endings) ever sets one, so that path is left to the original instructions
 * until one does and it can be checked against the oracle.
 * ========================================================================= */
SND_ENTRY(0xEBB8, snd_pitch_glide, 40)
int snd_pitch_glide(m37710_t *c)
{
    const uint32_t ch = c->x;
    if (peek8(c, dp(c, CH_GLIDE + ch)) != 0) return declined(R_GLIDE);      /* gliding: original code */

    op(c, 1); c->ps |= M377_M;                              /* SEM */
    op(c, 3); c->dt = 0;                                    /* LDT #0 (89 C2 00) */
    op(c, 2); (void)rd8(c, dp(c, CH_GLIDE + ch));           /* LDA $0D,X -> 0 */
    op(c, 2); c->cycles += 2;                               /* BEQ, taken */
    op(c, 1); c->ps &= (uint16_t)~M377_M;                   /* CLM */
    op(c, 2); const uint16_t target = rd16(c, dp(c, CH_TARGET_PITCH + ch));
    op(c, 2); wr16(c, dp(c, CH_PITCH + ch), target);

    /* exit state: 16-bit A = the pitch, N/Z from it */
    c->a = target;
    c->ps &= (uint16_t)~(M377_N | M377_Z);
    if (target == 0)     c->ps |= M377_Z;
    if (target & 0x8000) c->ps |= M377_N;
    op(c, 1); c->pc = pop16(c);                             /* RTS */
    return ran(R_GLIDE);
}

/* =========================================================================
 * PITCH ENVELOPE  (0xEBFD, once per channel per tick)
 *
 * A running envelope walks a script in bank 0x21 (bytes < 0xF0 are levels
 * interpolated with MPY; 0xFD jumps, 0xFE loops, anything else ends it) into
 * the pitch offset at +0x2A. With it off the routine only selects bank 0x21
 * and returns. The running path is never reached in the gated scenarios, so
 * it stays the original's.
 * ========================================================================= */
SND_ENTRY(0xEBFD, snd_pitch_envelope, 20)
int snd_pitch_envelope(m37710_t *c)
{
    const uint32_t ch = c->x;
    if (peek8(c, dp(c, CH_PENV_ON + ch)) != 0) return declined(R_PENV);    /* running: original code */

    op(c, 1); c->ps |= M377_M;                              /* SEM */
    op(c, 3); c->dt = 0x21;                                 /* LDT #$21 (89 C2 21) */
    op(c, 2); (void)rd8(c, dp(c, CH_PENV_ON + ch));         /* LDA $07,X -> 0 */
    op(c, 2); c->cycles += 2;                               /* BEQ, taken */

    /* exit state: 8-bit A = 0 (its high byte kept), Z set */
    c->a &= 0xFF00;
    c->ps = (uint16_t)((c->ps & ~M377_N) | M377_Z);
    op(c, 1); c->pc = pop16(c);                             /* RTS */
    return ran(R_PENV);
}

/* =========================================================================
 * PAN -> FOUR SPEAKERS  (0xE87E)
 *
 * The pan position (A, masked by 0xF4, offset a quarter turn) picks a pair of
 * adjacent speakers and splits the sound between them through the pan law at
 * 0xF306: one side gets law[p], the other law[63 - p], and the two speakers
 * not in the pair are silenced (0xFF). Output in the scratch bytes 0xC0-0xC3,
 * the order voice_volume_write() reads them in.
 *
 * Two of the four quadrants are ever used (the pair front L/R, and front L
 * with rear L); the other two -- the rear pair -- are left to the original
 * until a scenario exercises them. Entered with 8-bit A and 16-bit X only.
 * ========================================================================= */
SND_ENTRY(0xE87E, snd_pan_to_speakers, 110)
int snd_pan_to_speakers(m37710_t *c)
{
    if (((c->ps >> 4) & 3u) != 2 || (c->ps & M377_D)) return declined(R_PAN);
    const uint8_t a_in = (uint8_t)c->a;
    const uint8_t pos  = (uint8_t)((a_in & peek8(c, dp(c, V_PAN_MASK))) + 0x20);
    if (pos & 0x80) return declined(R_PAN);                   /* rear pair: original code */

    op(c, 1); c->ps &= (uint16_t)~M377_M;                     /* CLM */
    op(c, 3); c->dt = 0;                                      /* LDT #0 */
    op(c, 4); wr16(c, dp(c, V_SPEAKER_ATTEN + 0), 0xFFFF);    /* all four off */
    op(c, 4); wr16(c, dp(c, V_SPEAKER_ATTEN + 2), 0xFFFF);
    op(c, 1); c->ps |= M377_M;                                /* SEM */
    op(c, 1); push16(c, c->x);                                /* PHX */
    op(c, 2); const uint8_t mask = rd8(c, dp(c, V_PAN_MASK));
    op(c, 1);                                                 /* CLC */
    op(c, 2);                                                 /* ADC #$20 */
    const uint8_t masked = (uint8_t)(a_in & mask);
    const bool overflow = (~(masked ^ 0x20) & (masked ^ pos) & 0x80) != 0;
    op(c, 2); wr8(c, dp(c, V_PAN_TMP), pos);
    const uint8_t p = pos & 0x3F;                             /* AND #$3F */
    op(c, 2); op(c, 2); op(c, 1);                             /* AND, SEP #X, TAX */
    op(c, 4); const uint8_t near = rd8(c, dbank(c, T_PAN_LAW + p));          /* LDB */
    op(c, 3); wr8(c, dp(c, V_PAN_FOLD), near);                               /* STB */
    op(c, 2); op(c, 1);                                       /* EOR #$3F, TAX */
    op(c, 3); const uint8_t far = rd8(c, dbank(c, T_PAN_LAW + (p ^ 0x3F)));
    op(c, 2); c->ps &= (uint16_t)~M377_X;                     /* CLP #X */
    op(c, 1); c->x = pop16(c);                                /* PLX */
    op(c, 2); (void)rd8(c, dp(c, V_PAN_TMP));                 /* ASL $C4 */
    wr8(c, dp(c, V_PAN_TMP), (uint8_t)(pos << 1));
    op(c, 2);                                                 /* BCS, not taken */
    const bool second_half = (pos & 0x40) != 0;               /* N after the ASL */
    op(c, 2); if (second_half) c->cycles += 2;                /* BMI */
    uint8_t shown;
    if (!second_half) {                                       /* front L/R pair */
        op(c, 2); wr8(c, dp(c, V_SPEAKER_ATTEN + 0), far);
        op(c, 2); shown = rd8(c, dp(c, V_PAN_FOLD));
        op(c, 2); wr8(c, dp(c, V_SPEAKER_ATTEN + 1), shown);
    } else {                                                  /* front L + rear L */
        op(c, 2); wr8(c, dp(c, V_SPEAKER_ATTEN + 2), far);
        op(c, 2); shown = rd8(c, dp(c, V_PAN_FOLD));
        op(c, 2); wr8(c, dp(c, V_SPEAKER_ATTEN + 0), shown);
    }

    /* exit state: 8-bit A and B hold the law values (high bytes kept),
     * DT 0, 16-bit X restored; C clear, V from the ADC, N/Z from the last LDA */
    c->a = (uint16_t)((c->a & 0xFF00) | shown);
    c->b = (uint16_t)((c->b & 0xFF00) | near);
    c->ps &= (uint16_t)~(M377_C | M377_V | M377_N | M377_Z);
    if (overflow)     c->ps |= M377_V;
    if (shown == 0)   c->ps |= M377_Z;
    if (shown & 0x80) c->ps |= M377_N;
    op(c, 1); c->pc = pop16(c);                               /* RTS */
    return ran(R_PAN);
}

/* =========================================================================
 * SLOT LEVEL  (0xD745, per mailbox slot while it is sounding)
 *
 * Copies the slot's level -- a byte reached through the slot's pointer at
 * +0x1E -- to +0x20. Mode bits 0x3000 in the slot's flags word (+0x0A) select
 * a level that is ADDED to a mailbox parameter or ramped over time; neither
 * is ever set in a gated scenario, so those stay the original's.
 * ========================================================================= */
enum { SLOT_FLAGS = 0x0A, SLOT_LEVEL_PTR = 0x1E, SLOT_LEVEL = 0x20 };
SND_ENTRY(0xD745, snd_slot_level, 40)
int snd_slot_level(m37710_t *c)
{
    const uint32_t slot = c->x;
    if (((c->ps >> 4) & 3u) != 0) return declined(R_LEVEL);
    const uint16_t fl = (uint16_t)(peek8(c, dp(c, SLOT_FLAGS + slot)) | peek8(c, dp(c, SLOT_FLAGS + 1 + slot)) << 8);
    if (fl & 0x3000) return declined(R_LEVEL);                /* added or ramped: original code */

    op(c, 1);                                                 /* CLM */
    op(c, 2); (void)rd16(c, dp(c, SLOT_FLAGS + slot));        /* LDA $0A,X */
    op(c, 3);                                                 /* AND #$3000 */
    op(c, 2); c->cycles += 2;                                 /* BEQ, taken */
    op(c, 1); c->ps |= M377_M;                                /* SEM */
    op(c, 2); const uint16_t ptr = rd16(c, dp(c, SLOT_LEVEL_PTR + slot));  /* LDA ($1E,X) */
    const uint8_t level = rd8(c, ((uint32_t)c->dt << 16) | ptr);
    op(c, 2); wr8(c, dp(c, SLOT_LEVEL + slot), level);        /* STA $20,X */

    /* exit state: A = 0x00:level (the AND left the high byte 0), 8-bit, N/Z */
    c->a = level;
    c->ps &= (uint16_t)~(M377_N | M377_Z);
    if (level == 0)   c->ps |= M377_Z;
    if (level & 0x80) c->ps |= M377_N;
    op(c, 1); c->pc = pop16(c);                               /* RTS */
    return ran(R_LEVEL);
}

/* =========================================================================
 * THE 120 Hz TICK  (0xC258, Timer A0 through the vector at 0x01EA)
 *
 * Saves the interrupted code's registers, gives the tick direct page 0, data
 * bank 0, 16-bit registers and interrupt level 1, runs the tick body through
 * the vector at 0xBFBA (0xD684: the mailbox and every channel), then restores
 * and returns from the interrupt. The original re-enables interrupts (CLI) so
 * the pins and Timer B0 can preempt the body; here the body is one event.
 * ========================================================================= */
enum { VEC_TICK_BODY = 0xBFBA };
SND_ENTRY(0xC258, snd_tick, 0)
int snd_tick(m37710_t *c)
{
    c->ps &= (uint16_t)~0x3B;                        /* CLP: 16-bit A and X, binary, C/Z clear */
    push16(c, c->a); push16(c, c->b);                /* PSH #$3F: A B X Y DPR DT */
    push16(c, c->x); push16(c, c->y);
    push16(c, c->dpr); push8(c, c->dt);

    c->dt = 0;
    c->dpr = 0;
    c->a = 0x0100;
    c->ps = 0x0100;                                  /* interrupt level 1, mask open */

    if (snd_call(c, rd16(c, VEC_TICK_BODY))) return 1;             /* the tick body (0xD684) */

    c->ps &= (uint16_t)~0x3B;                        /* CLP #$3B, then PUL #$3F */
    c->dt = pop8(c); c->dpr = pop16(c);
    c->y = pop16(c); c->x = pop16(c);
    c->b = pop16(c); c->a = pop16(c);
    c->ps = pop16(c); c->pc = pop16(c); c->pg = pop8(c);   /* RTI */
    return ran(R_TICK);
}

/* =========================================================================
 * THE TICK BODY  (0xD684, through the vector at 0xBFBA)
 *
 * Advances the tick clock (0xDA, +0x40 a tick; sequences compare against it),
 * and unless a tick is already in progress (0xD3 -- the original can be
 * re-entered by a nested Timer A0), takes the pan mask the game set (0x448A),
 * runs the mailbox and every channel through the vector at 0x0104 (0xD6E3),
 * and services the game's request counter (0x43FC vs 0x43FE, via 0xD879).
 *
 * Declined, never run in a gated scenario: the game's 0x5A mode at 0x4480
 * (a jump to 0xD7F7), a patch in the ROM's data bank (0x99AA at 0x200002
 * calls 0x200100) and the stack-alignment push (the stack is always odd here).
 * ========================================================================= */
enum { V_TICK_CLOCK = 0xDA, V_TICK_BUSY = 0xD3, V_SAVED_SP = 0xEE,
       SH_MODE = 0x4480, SH_PAN_MASK = 0x448A, SH_REQ_68K = 0x43FC, SH_REQ_DONE = 0x43FE,
       VEC_MAILBOX = 0x0104, REQ_SERVICE = 0xD879 };
SND_ENTRY(0xD684, snd_tick_body, 0)
int snd_tick_body(m37710_t *c)
{
    const bool busy = peek8(c, dp(c, V_TICK_BUSY)) != 0;
    if (!busy && (peek8(c, SH_MODE) == 0x5A
                  || (peek8(c, 0x200002) | peek8(c, 0x200003) << 8) == 0x99AA
                  || !(c->s & 1)))
        return declined(R_TICKBODY);

    c->ps &= (uint16_t)~(M377_X | M377_M);             /* 16-bit */
    const uint16_t clk = rd16(c, dp(c, V_TICK_CLOCK));
    const uint32_t sum = (uint32_t)clk + 0x40;
    wr16(c, dp(c, V_TICK_CLOCK), (uint16_t)sum);
    c->ps &= (uint16_t)~(M377_C | M377_V | M377_N | M377_Z);
    if (sum > 0xFFFF) c->ps |= M377_C;
    if (~(clk ^ 0x40) & (clk ^ sum) & 0x8000) c->ps |= M377_V;
    c->a = (uint16_t)sum;

    if (busy) {                                        /* nested tick: only the clock moves */
        const uint8_t b = rd8(c, dp(c, V_TICK_BUSY));
        c->a = (uint16_t)((c->a & 0xFF00) | b);
        c->ps |= M377_M;                               /* left 8-bit: no CLM on this path */
        c->ps &= (uint16_t)~(M377_N | M377_Z);
        if (b & 0x80) c->ps |= M377_N;
        c->pc = pop16(c);                              /* RTS */
        return ran(R_TICKBODY);
    }
    (void)rd8(c, dp(c, V_TICK_BUSY));
    wr8(c, dp(c, V_TICK_BUSY), 1);
    c->dpr = 0; c->dt = 0;
    (void)rd8(c, SH_MODE);
    wr16(c, V_SAVED_SP, c->s);
    wr8(c, V_PAN_MASK, rd8(c, SH_PAN_MASK));

    c->ps |= M377_M;                                   /* the mailbox runs with 8-bit A, as it was entered */
    c->a = (uint16_t)((c->s & 0xFF00) | rd8(c, SH_PAN_MASK));
    if (snd_call(c, rd16(c, VEC_MAILBOX))) return 1;                 /* 0xD6E3: every slot and channel */

    (void)rd16(c, 0x200002);                           /* the data-bank patch check */
    const uint8_t req = rd8(c, SH_REQ_68K);
    const uint8_t done = rd8(c, SH_REQ_DONE);
    if (req != done) {
        c->ps |= M377_M;
        if (snd_call(c, REQ_SERVICE)) return 1;
    } else {
        c->ps |= M377_C;                               /* CMP equal */
    }

    const uint16_t sp = rd16(c, V_SAVED_SP);
    c->s = sp;                                         /* TCS */
    wr8(c, V_TICK_BUSY, 0);
    c->ps &= (uint16_t)~(M377_M | M377_N | M377_Z);    /* CLM; N/Z from LDA $EE */
    c->a = sp;
    if (sp == 0) c->ps |= M377_Z;
    if (sp & 0x8000) c->ps |= M377_N;
    c->pc = pop16(c);                                  /* RTS */
    return ran(R_TICKBODY);
}

/* =========================================================================
 * THE MAILBOX AND THE TRACKS  (0xD6E3, through the vector at 0x0104)
 *
 * One pass over the game's command slots, 0 .. count-1 (count in 0x4484):
 *   - a slot whose command word (0x4000 + 2i) has bit 14 set holds a NEW
 *     command from the 68K: the command handler (vector 0x0106) takes it;
 *   - otherwise, if the slot's track (record 0x4500 + 0x50*i) is running --
 *     bit 15 of its flags at +0x0A -- the track's step count from the table
 *     at 0x4040 + 2i is stored into those flags, the sequencer (vector
 *     0x010A, 0xD9D0) advances it, and 0xD745 updates its level.
 * Then every channel is updated (vector 0x0126, 0xE46E).
 * The slot's pointer pair (0xD6: +0x100 a slot, 0xD8: +0x50 a slot) and the
 * command-word address (0xE0) are left in direct page for the callees.
 * ========================================================================= */
enum { V_SLOT_A = 0xD6, V_SLOT_TRACK = 0xD8, V_SLOT_INDEX = 0xD0, V_SLOT_CMD = 0xE0,
       SH_CMD_WORDS = 0x4000, SH_STEP_PTRS = 0x4040, SH_TRACK_RECS = 0x4500, SH_SLOT_COUNT = 0x4484,
       TRK_FLAGS = 0x0A, VEC_NEW_COMMAND = 0x0106, VEC_SEQUENCER = 0x010A, VEC_CHANNELS = 0x0126,
       SLOT_LEVEL_ROUTINE = 0xD745 };
SND_ENTRY(0xD6E3, snd_mailbox, 0)
int snd_mailbox(m37710_t *c)
{
    c->ps &= (uint16_t)~M377_M;
    wr16(c, dp(c, V_SLOT_A), 0x5000);
    wr16(c, dp(c, V_SLOT_TRACK), SH_TRACK_RECS);
    c->ps |= M377_M;
    wr8(c, dp(c, V_SLOT_INDEX), 0);
    uint16_t cmd_addr = SH_CMD_WORDS;                  /* LDX #$4000 */

    for (;;) {
        c->x = cmd_addr;
        wr16(c, dp(c, V_SLOT_CMD), cmd_addr);          /* STX $E0 */
        c->ps &= (uint16_t)~M377_M;
        const uint16_t cmd = rd16(c, dp(c, cmd_addr));
        const uint16_t shifted = (uint16_t)(cmd << 1);   /* ASL: N = bit 14 */
        c->a = shifted;
        c->ps &= (uint16_t)~(M377_C | M377_N | M377_Z);
        if (cmd & 0x8000)     c->ps |= M377_C;
        if (shifted & 0x8000) c->ps |= M377_N;
        if (shifted == 0)     c->ps |= M377_Z;

        if (cmd & 0x4000) {                            /* a new command from the game */
            if (snd_call(c, rd16(c, VEC_NEW_COMMAND))) return 1;
        } else {
            const uint16_t step_ptr = rd16(c, dp(c, cmd_addr + 0x40));
            c->x = step_ptr;
            c->b = rd16(c, dp(c, step_ptr));           /* LDB $00,X (16-bit) */
            const uint16_t trk = rd16(c, dp(c, V_SLOT_TRACK));
            c->x = trk;
            const uint16_t flags = rd16(c, dp(c, trk + TRK_FLAGS));
            c->a = flags;
            c->ps &= (uint16_t)~(M377_N | M377_Z);
            if (flags & 0x8000) c->ps |= M377_N;
            if (flags == 0)     c->ps |= M377_Z;
            if (flags & 0x8000) {                      /* track running */
                wr16(c, dp(c, trk + TRK_FLAGS), c->b);   /* STB $0A,X */
                if (snd_call(c, rd16(c, VEC_SEQUENCER))) return 1;
                if (snd_call(c, SLOT_LEVEL_ROUTINE)) return 1;
            }
        }

        c->ps |= M377_M;                               /* next slot */
        const uint8_t idx = (uint8_t)(rd8(c, dp(c, V_SLOT_INDEX)) + 1);
        wr8(c, dp(c, V_SLOT_INDEX), idx);
        const uint8_t count = rd8(c, SH_SLOT_COUNT);
        c->a = (uint16_t)((c->a & 0xFF00) | idx);
        c->ps &= (uint16_t)~(M377_M | M377_C | M377_N | M377_Z);   /* CMP, then CLM */
        if (idx >= count) c->ps |= M377_C;
        if (idx == count) c->ps |= M377_Z;
        if ((uint8_t)(idx - count) & 0x80) c->ps |= M377_N;
        if (idx == count) break;

        const uint32_t a2 = (uint32_t)rd16(c, dp(c, V_SLOT_A)) + 0x100;
        wr16(c, dp(c, V_SLOT_A), (uint16_t)a2);
        const uint32_t t2 = (uint32_t)rd16(c, dp(c, V_SLOT_TRACK)) + 0x50 + (a2 > 0xFFFF);
        wr16(c, dp(c, V_SLOT_TRACK), (uint16_t)t2);
        c->a = (uint16_t)t2;
        cmd_addr = (uint16_t)(rd16(c, dp(c, V_SLOT_CMD)) + 2);   /* LDX $E0 ; INX ; INX */
    }

    if (snd_call(c, rd16(c, VEC_CHANNELS))) return 1;                /* every channel (0xE46E) */
    c->pc = pop16(c);                                  /* RTS */
    return ran(R_MAILBOX);
}

/* =========================================================================
 * EVERY CHANNEL  (0xE46E, through the vector at 0x0126)
 *
 * For each channel record (0x7000 + 0xC0*i, count in 0x4482; its C352 voice
 * at 0x2000 + 0x10*i, kept in 0xEA):
 *   - the channel's note-event queue is a ring of 8 events of 8 bytes at
 *     +0x80, read index +0x35, write index +0x34, the next event's pointer
 *     at +0x36 and its first word its due time. Every event now due (time
 *     not after the tick clock 0xDA) is popped; the LAST one popped goes to
 *     the note-event handler (vector 0x0124, 0xE4E7) with Y = the event --
 *     a late tick drops the events it overran and plays the newest;
 *   - if the channel is sounding (+0x38) the per-channel update runs
 *     (vector 0x010C, 0xE8DB: pitch, volume, pan into the voice).
 * Then the channel count goes to the chip's control register 0x2404.
 * ========================================================================= */
enum { CH_RD = 0x35, CH_WR = 0x34, CH_EVENT_PTR = 0x36, CH_ACTIVE = 0x38, CH_EVENTS = 0x80,
       CH_RECORD_BASE = 0x7000, CH_RECORD_SIZE = 0xC0, VOICE_REG_SIZE = 0x10,
       V_CHAN_INDEX = 0xD1, V_CHAN_RECORD = 0xEC, SH_CHAN_COUNT = 0x4482, C352_CONTROL = 0x2404,
       VEC_NOTE_EVENT = 0x0124, VEC_CHANNEL_UPDATE = 0x010C };
static inline void set_nz_w(m37710_t *c, uint16_t v, bool wide)
{
    c->ps &= (uint16_t)~(M377_N | M377_Z);
    if ((wide ? v : (v & 0xFF)) == 0)     c->ps |= M377_Z;
    if (v & (wide ? 0x8000 : 0x80))       c->ps |= M377_N;
}
/* CMP of a 16-bit accumulator-width compare, flags as the chip sets them */
static inline void cmp_flags(m37710_t *c, uint16_t a, uint16_t m, bool wide)
{
    const uint32_t mask = wide ? 0xFFFF : 0xFF;
    const uint32_t r = ((uint32_t)a & mask) - ((uint32_t)m & mask);
    c->ps &= (uint16_t)~(M377_N | M377_Z | M377_C);
    if (((uint32_t)a & mask) >= ((uint32_t)m & mask)) c->ps |= M377_C;
    if ((r & mask) == 0) c->ps |= M377_Z;
    if (r & (wide ? 0x8000 : 0x80)) c->ps |= M377_N;
}
SND_ENTRY(0xE46E, snd_channels, 0)
int snd_channels(m37710_t *c)
{
    c->ps &= (uint16_t)~M377_M;
    wr16(c, dp(c, V_VOICE_REGS), 0x2000);
    c->ps |= M377_M;
    wr8(c, dp(c, V_CHAN_INDEX), 0);
    uint16_t rec = CH_RECORD_BASE;
    c->x = rec;
    c->dt = 0;

    for (;;) {
        wr16(c, dp(c, V_CHAN_RECORD), rec);            /* STX $EC; M = 1 here */
        const uint8_t rd = rd8(c, dp(c, rec + CH_RD));
        const uint8_t wr = rd8(c, dp(c, rec + CH_WR));
        cmp_flags(c, rd, wr, false);
        c->a = (uint16_t)((c->a & 0xFF00) | rd);

        bool due = false;
        if (rd != wr) {
            c->ps &= (uint16_t)~M377_M;
            const uint16_t clock = rd16(c, dp(c, V_TICK_CLOCK));
            uint16_t t = rd16(c, ((uint32_t)c->dt << 16) | rd16(c, dp(c, rec + CH_EVENT_PTR)));
            cmp_flags(c, t, clock, true);
            c->a = t;
            due = (t == clock) || ((uint16_t)(t - clock) & 0x8000);   /* BEQ, or not BPL */
            while (due) {
                c->ps |= M377_M;
                c->y = rd16(c, dp(c, rec + CH_EVENT_PTR));             /* this event */
                const uint8_t next = (uint8_t)(rd8(c, dp(c, rec + CH_RD)) + 1);
                wr8(c, dp(c, rec + CH_RD), next);
                c->ps &= (uint16_t)~M377_M;
                const uint16_t ptr = (uint16_t)(((next & 7) << 3) + rec + CH_EVENTS);
                wr16(c, dp(c, rec + CH_EVENT_PTR), ptr);
                c->ps |= M377_M;
                const uint8_t w2 = rd8(c, dp(c, rec + CH_WR));
                cmp_flags(c, next, w2, false);
                c->a = (uint16_t)((ptr & 0xFF00) | next);
                c->ps &= (uint16_t)~M377_M;
                if (next == w2) break;                                  /* queue empty */
                t = rd16(c, ((uint32_t)c->dt << 16) | ptr);
                cmp_flags(c, t, clock, true);
                c->a = t;
                if (!((uint16_t)(t - clock) & 0x8000)) break;           /* BMI: next one also due? */
            }
            if (due && snd_call(c, rd16(c, VEC_NOTE_EVENT))) return 1;  /* 0xE4E7, Y = the event */
        }

        const bool wide = !(c->ps & M377_M);            /* as the last code left it */
        const uint16_t active = wide ? rd16(c, dp(c, c->x + CH_ACTIVE)) : rd8(c, dp(c, c->x + CH_ACTIVE));
        c->a = wide ? active : (uint16_t)((c->a & 0xFF00) | active);
        set_nz_w(c, active, wide);
        if (active && snd_call(c, rd16(c, VEC_CHANNEL_UPDATE))) return 1;   /* 0xE8DB */

        c->ps |= M377_M;
        const uint8_t idx = (uint8_t)(rd8(c, dp(c, V_CHAN_INDEX)) + 1);
        wr8(c, dp(c, V_CHAN_INDEX), idx);
        const uint8_t count = rd8(c, dbank(c, SH_CHAN_COUNT));      /* absolute: the data bank a handler left */
        cmp_flags(c, idx, count, false);
        c->a = (uint16_t)((c->a & 0xFF00) | idx);
        if (idx == count) break;

        c->ps &= (uint16_t)~M377_M;
        const uint32_t v = (uint32_t)rd16(c, dp(c, V_VOICE_REGS)) + VOICE_REG_SIZE;
        wr16(c, dp(c, V_VOICE_REGS), (uint16_t)v);
        rec = (uint16_t)(rd16(c, dp(c, V_CHAN_RECORD)) + CH_RECORD_SIZE + (v > 0xFFFF));
        c->a = rec;
        c->x = rec;
        c->ps |= M377_M;
    }

    c->ps &= (uint16_t)~M377_M;
    wr16(c, dbank(c, C352_CONTROL), c->a);                       /* the channel count, and whatever A held above it */
    c->pc = pop16(c);                                  /* RTS */
    return ran(R_CHANNELS);
}

/* =========================================================================
 * ONE CHANNEL'S UPDATE  (0xE8DB, through the vector at 0x010C)
 *
 * X = the channel record, 0xEA = its C352 voice.
 *   - The voice's chip flags (+6) say whether the note is still there: not
 *     busy and no key-on pending means the chip finished it -- the channel is
 *     marked silent (+0x38) and nothing else happens.
 *   - A busy voice with flag 0x20 runs the handler at 0x012A first.
 *   - A channel whose pitch follows a GAME PARAMETER (+0x0F = parameter
 *     number) takes it from the mailbox (0x4100 + 2n) as its target pitch.
 *   - The four effect stages run (vectors 0x011A, 0x011E, 0x0120, 0x0122:
 *     glide, pitch envelope, and the rest).
 *   - PITCH: current pitch +0x42, plus +0x40, +0x44, +0x46 and the global
 *     transpose 0xDC, is a 16-bit semitone:fraction; the chip frequency is
 *     the ROM table at 0xF346 (one word a semitone) linearly interpolated by
 *     the fraction -- the one MPY here.
 *   - LEVEL: +0x1F plus the slot's level +0x02 plus the level byte behind the
 *     pointer at +0x48, saturating at 0xFF, becomes the master attenuation
 *     0xCC for the volume writer (vector 0x011C), which is JUMPED to: it
 *     returns straight to this routine's caller.
 * ========================================================================= */
enum { VF_FLAGS = 0x06, VF_FREQ = 0x04, CH_PITCH_PARAM = 0x0F, CH_PITCH_A = 0x40, CH_PITCH_C = 0x44,
       CH_PITCH_D = 0x46, CH_LEVEL = 0x1F, CH_SLOT_LEVEL = 0x02, CH_LEVEL_PTR = 0x48,
       V_TRANSPOSE = 0xDC, V_PARAM_PTR = 0xF0, SH_PARAMS = 0x4100, T_SEMITONE_FREQ = 0xF346,
       VEC_BUSY_FX = 0x012A, VEC_FX1 = 0x011A, VEC_FX2 = 0x011E, VEC_FX3 = 0x0120, VEC_FX4 = 0x0122,
       VEC_VOLUME_WRITER = 0x011C };
static inline uint32_t abs_y(const m37710_t *c, uint16_t a) { return (((uint32_t)c->dt << 16) + a + c->y) & 0xFFFFFF; }
static inline uint8_t adc8(m37710_t *c, uint8_t a, uint8_t m)
{
    const unsigned r = (unsigned)a + m + ((c->ps & M377_C) ? 1u : 0u);
    c->ps &= (uint16_t)~(M377_N | M377_Z | M377_C | M377_V);
    if (r > 0xFF) c->ps |= M377_C;
    if (~(a ^ m) & (a ^ r) & 0x80) c->ps |= M377_V;
    if ((r & 0xFF) == 0) c->ps |= M377_Z;
    if (r & 0x80) c->ps |= M377_N;
    return (uint8_t)r;
}
static inline uint16_t adc16(m37710_t *c, uint16_t a, uint16_t m)
{
    const uint32_t r = (uint32_t)a + m + ((c->ps & M377_C) ? 1u : 0u);
    c->ps &= (uint16_t)~(M377_N | M377_Z | M377_C | M377_V);
    if (r > 0xFFFF) c->ps |= M377_C;
    if (~(a ^ m) & (a ^ r) & 0x8000) c->ps |= M377_V;
    if ((r & 0xFFFF) == 0) c->ps |= M377_Z;
    if (r & 0x8000) c->ps |= M377_N;
    return (uint16_t)r;
}
SND_ENTRY(0xE8DB, snd_channel_update, 0)
int snd_channel_update(m37710_t *c)
{
    const uint16_t ch = c->x;
    c->ps &= (uint16_t)~M377_M;
    c->y = rd16(c, dp(c, V_VOICE_REGS));
    const uint16_t vflags = rd16(c, abs_y(c, VF_FLAGS));
    c->a = vflags;
    set_nz_w(c, vflags, true);

    if (!(vflags & 0x8000)) {                          /* not busy */
        cmp_flags(c, vflags, 0x4000, true);
        if (vflags < 0x4000) {                         /* and no key-on pending: the chip finished it */
            c->ps |= M377_M;
            wr8(c, dp(c, ch + CH_ACTIVE), 0);
            c->pc = pop16(c);
            return ran(R_CHUPDATE);
        }
        c->dt = 0x21;
    } else if (vflags & 0x0020) {
        c->a = 0x0020; set_nz_w(c, c->a, true);
        c->dt = 0x21;
        if (snd_call(c, rd16(c, VEC_BUSY_FX))) return 1;
    } else {
        c->a = 0; set_nz_w(c, 0, true);
    }

    c->ps |= M377_M;                                   /* E901 */
    const uint8_t param = rd8(c, dp(c, c->x + CH_PITCH_PARAM));
    c->a = (uint16_t)((c->a & 0xFF00) | param);
    set_nz_w(c, param, false);
    if (param) {
        c->dt = 0;
        c->ps &= (uint16_t)~M377_M;
        const uint16_t ptr = (uint16_t)(SH_PARAMS + param * 2);   /* AND #$FF ; ASL ; ADC #$4100 */
        wr16(c, dp(c, V_PARAM_PTR), ptr);
        const uint16_t target = rd16(c, ((uint32_t)c->dt << 16) | rd16(c, dp(c, V_PARAM_PTR)));
        wr16(c, dp(c, c->x + CH_TARGET_PITCH), target);
        c->a = target; set_nz_w(c, target, true);
        c->ps &= (uint16_t)~(M377_C | M377_V);
    }

    if (snd_call(c, rd16(c, VEC_FX1))) return 1;
    if (snd_call(c, rd16(c, VEC_FX2))) return 1;
    if (snd_call(c, rd16(c, VEC_FX3))) return 1;
    if (snd_call(c, rd16(c, VEC_FX4))) return 1;

    /* pitch */
    c->dt = 0;
    c->ps &= (uint16_t)~M377_M;
    const uint16_t x = c->x;
    uint16_t p = rd16(c, dp(c, x + CH_PITCH));
    c->ps &= (uint16_t)~M377_C; p = adc16(c, p, rd16(c, dp(c, x + CH_PITCH_A)));
    c->ps &= (uint16_t)~M377_C; p = adc16(c, p, rd16(c, dp(c, x + CH_PITCH_C)));
    c->ps &= (uint16_t)~M377_C; p = adc16(c, p, rd16(c, dp(c, V_TRANSPOSE)));
    c->ps &= (uint16_t)~M377_C; p = adc16(c, p, rd16(c, dp(c, x + CH_PITCH_D)));
    wr16(c, dp(c, V_SCRATCH + 0), p);
    c->ps |= M377_M;
    const uint8_t semitone = rd8(c, dp(c, V_SCRATCH + 1));
    wr8(c, dp(c, V_SCRATCH + 1), 0);                   /* 0xC0 now holds the fraction alone */
    push16(c, x);                                      /* PHX */
    const uint16_t tbl = (uint16_t)(((uint16_t)(semitone << 1) & 0xFF) + T_SEMITONE_FREQ);   /* 8-bit ASL: bit 7 is lost */
    c->ps &= (uint16_t)~M377_M;
    const uint16_t f0 = rd16(c, dp(c, tbl + 0));
    const uint16_t f1 = rd16(c, dp(c, tbl + 2));
    const uint16_t diff = (uint16_t)(f1 - f0);         /* SEC ; SBC */
    const uint16_t frac = rd16(c, dp(c, V_SCRATCH + 0));
    const uint32_t prod = (uint32_t)diff * frac;       /* MPY $C0 -> B:A */
    wr16(c, dp(c, V_SCRATCH + 0), (uint16_t)prod);
    wr16(c, dp(c, V_SCRATCH + 2), (uint16_t)(prod >> 16));
    c->b = (uint16_t)(prod >> 16);
    const uint16_t mid = rd16(c, dp(c, V_SCRATCH + 1));   /* the product >> 8 */
    c->ps &= (uint16_t)~M377_C;                        /* MPY clears carry */
    const uint16_t freq = adc16(c, mid, rd16(c, dp(c, tbl + 0)));
    c->x = pop16(c);                                   /* PLX */
    c->y = rd16(c, dp(c, V_VOICE_REGS));
    wr16(c, abs_y(c, VF_FREQ), freq);

    /* level -> master attenuation */
    c->ps |= M377_M;
    uint8_t lv = rd8(c, dp(c, c->x + CH_LEVEL));
    c->ps &= (uint16_t)~M377_C;
    lv = adc8(c, lv, rd8(c, dp(c, c->x + CH_SLOT_LEVEL)));
    if (!(c->ps & M377_C)) {
        const uint16_t lp = rd16(c, dp(c, c->x + CH_LEVEL_PTR));
        lv = adc8(c, lv, rd8(c, ((uint32_t)c->dt << 16) | lp));
    }
    if (c->ps & M377_C) { lv = 0xFF; c->ps = (uint16_t)((c->ps & ~M377_Z) | M377_N); }
    c->a = (uint16_t)((freq & 0xFF00) | lv);
    wr8(c, dp(c, V_MASTER_ATTEN), lv);

    c->pc = rd16(c, VEC_VOLUME_WRITER);                /* JMP ($011C): the writer returns to our caller */
    return ran(R_CHUPDATE);
}

/* =========================================================================
 * THE CHANNEL'S OWN HOOKS  (0xEF45, 0xE9D6, 0xECAA)
 *
 * A channel record carries the addresses of the routines that give it its
 * behaviour, set when its sound starts; the per-channel update reaches them
 * through these one-instruction dispatchers, JMP (hook,X):
 *   +0x5A  effect stage 4        (vector 0x0122)
 *   +0x5C  effect stage 1        (vector 0x011A; e.g. 0xEF6D / 0xEF5C)
 *   +0x60  how volume is formed  (vector 0x011C; e.g. 0xECC4 pan-then-volume)
 * 0xE9D5, a bare RTS, is the empty hook. The pointer is read from bank 0 (the
 * program bank), and the jump is a jump: the hook returns to our caller.
 * ========================================================================= */
enum { CH_HOOK_FX4 = 0x5A, CH_HOOK_FX1 = 0x5C, CH_HOOK_VOLUME = 0x60, CH_PAN_PTR = 0x5E, VEC_PAN_LAW = 0x012C,
       ROUTINE_VOLUME_FROM_SCRATCH = 0xE987 };
static int hook(m37710_t *c, unsigned field, int r)
{
    c->pc = rd16(c, (uint16_t)(c->x + field));
    return ran(r);
}
SND_ENTRY(0xEF45, snd_hook_fx1, 0)
int snd_hook_fx1(m37710_t *c) { return hook(c, CH_HOOK_FX1, R_HOOK_FX1); }
SND_ENTRY(0xE9D6, snd_hook_fx4, 0)
int snd_hook_fx4(m37710_t *c) { return hook(c, CH_HOOK_FX4, R_HOOK_FX4); }
SND_ENTRY(0xECAA, snd_hook_volume, 0)
int snd_hook_volume(m37710_t *c) { return hook(c, CH_HOOK_VOLUME, R_HOOK_VOL); }

/* =========================================================================
 * PAN, THEN VOLUME  (0xECC4 -- a channel's +0x60 hook)
 *
 * The pan position is the byte behind the channel's pointer at +0x5E; the
 * pan law (vector 0x012C, 0xE87E) turns it into the four speaker
 * attenuations, and the volume writer's second entry (0xE987) combines them
 * with the master attenuation into the chip -- jumped to, as the original.
 * ========================================================================= */
SND_ENTRY(0xECC4, snd_pan_then_volume, 0)
int snd_pan_then_volume(m37710_t *c)
{
    c->ps |= M377_M;
    c->dt = 0;
    const uint8_t pan = rd8(c, ((uint32_t)c->dt << 16) | rd16(c, dp(c, c->x + CH_PAN_PTR)));
    c->a = (uint16_t)((c->a & 0xFF00) | pan);
    set_nz_w(c, pan, false);
    if (snd_call(c, rd16(c, VEC_PAN_LAW))) return 1;
    c->ps &= (uint16_t)~M377_M;
    c->pc = ROUTINE_VOLUME_FROM_SCRATCH;
    return ran(R_PAN_VOL);
}

/* =========================================================================
 * A NOTE EVENT  (0xE4E7, through the vector at 0x0124)
 *
 * X = the channel, Y = the event (8 bytes in its ring): +2 the kind and its
 * parameter, +4 the SOUND DEFINITION's address, +6 the level pointer.
 * The sound definition (Y after the swap): +0 the sample number, +2 the
 * slot level, +4 the target pitch, +6/+8/+0xC/+0xE copied as they are, +0xA
 * the pitch offset (+0x0B added to +0x05).
 *   - kind 0: the parameter is the note (to +0x05); note 0x7F means STOP;
 *   - kind 1: the parameter is the slot level (to +0x02);
 *   - STOP: an idle channel (+0x06 = 0) is cleared by 0xEFAE (jumped to);
 *     otherwise its release flag (+0x2E) is set.
 * Starting a note: a different sample than the channel holds (the
 * definition's +0 bit 15 forces it, and is cleared) goes through 0xF013 with
 * the sample number in A; a repeat keeps the sample, and a channel with a
 * running modulation (+0x32) restarts it (0xF064). The channel is marked
 * sounding (+0x38) and, unless bit 7 of the kind says silent, keyed on
 * through the vector at 0x0118 (0xE5A7).
 * Declined, never in a gated scenario: kinds 2 and up, and a kind-0
 * parameter with bit 7 set.
 * ========================================================================= */
enum { EV_KIND = 2, EV_SOUND = 4, EV_LEVEL_PTR = 6,
       SD_SAMPLE = 0, SD_SLOT_LEVEL = 2, SD_TARGET = 4, SD_W6 = 6, SD_W8 = 8, SD_PITCH_OFS = 0x0A, SD_WC = 0x0C, SD_WE = 0x0E,
       CH_SAMPLE = 0x00, CH_NOTE = 0x05, CH_NOTE_OFS = 0x0B, CH_BUSY = 0x06, CH_RELEASE = 0x2E, CH_MOD = 0x32,
       CH_SOUND = 0x3C, V_EVENT_KIND = 0xE2,
       ROUTINE_SAMPLE_CHANGE = 0xF013, ROUTINE_MOD_RESTART = 0xF064, ROUTINE_CHANNEL_CLEAR = 0xEFAE, VEC_KEY_ON = 0x0118 };
SND_ENTRY(0xE4E7, snd_note_event, 0)
int snd_note_event(m37710_t *c)
{
    const uint16_t ch = c->x, ev = c->y;
    const uint32_t kw = abs_y(c, EV_KIND);
    const uint8_t kind_b = peek8(c, kw), param = peek8(c, kw + 1);
    const bool stop_masked = ((kind_b & 0x7F) == 0 && param == 0x7F);
    if (((c->ps >> 4) & 3u) != 0) return declined(R_NOTE_EVENT);
    if (!stop_masked && ((kind_b & 0x7F) >= 2 || ((kind_b & 0x7F) == 0 && (param & 0x80))))
        return declined(R_NOTE_EVENT);

    const uint16_t sound = rd16(c, abs_y(c, EV_SOUND));
    wr16(c, dp(c, ch + CH_SOUND), sound);
    wr16(c, dp(c, ch + CH_LEVEL_PTR), rd16(c, abs_y(c, EV_LEVEL_PTR)));
    const uint16_t kindw = rd16(c, abs_y(c, EV_KIND));
    wr16(c, dp(c, V_SCRATCH + 4), kindw);              /* 0xC4 kind, 0xC5 parameter */
    c->y = rd16(c, dp(c, ch + CH_SOUND));              /* Y = the sound definition */
    (void)ev;

    bool stop = ((kindw & 0xFF7F) == 0x7F00);
    if (!stop) {
        wr16(c, dp(c, ch + CH_SLOT_LEVEL), rd16(c, abs_y(c, SD_SLOT_LEVEL)));
        wr16(c, dp(c, ch + CH_TARGET_PITCH), rd16(c, abs_y(c, SD_TARGET)));
        c->ps |= M377_M;
        const uint8_t kind = rd8(c, dp(c, V_SCRATCH + 4));
        wr8(c, dp(c, V_EVENT_KIND), kind);
        if ((kind & 0x7F) == 1) {
            wr8(c, dp(c, ch + CH_SLOT_LEVEL), rd8(c, dp(c, V_SCRATCH + 5)));
        } else {                                       /* kind 0: the note */
            const uint8_t note = rd8(c, dp(c, V_SCRATCH + 5));
            if (note == 0x7F) stop = true;
            else wr8(c, dp(c, ch + CH_NOTE), note);
        }
    } else {
        c->ps |= M377_M;
    }

    if (stop) {                                        /* E549 */
        const uint8_t busy = rd8(c, dp(c, ch + CH_BUSY));
        c->a = (uint16_t)((c->a & 0xFF00) | busy);
        set_nz_w(c, busy, false);
        if (!busy) { c->pc = ROUTINE_CHANNEL_CLEAR; return ran(R_NOTE_EVENT); }
        wr8(c, dp(c, ch + CH_RELEASE), 1);
        c->pc = pop16(c);                              /* RTS */
        return ran(R_NOTE_EVENT);
    }

    /* E52B: start the note */
    c->ps &= (uint16_t)~M377_M;
    wr16(c, dp(c, ch + SD_PITCH_OFS), rd16(c, abs_y(c, SD_PITCH_OFS)));
    uint16_t sample = rd16(c, abs_y(c, SD_SAMPLE));
    bool change;
    if (sample & 0x8000) {                             /* forced: clear the flag, then change */
        sample &= 0x7FFF;
        wr16(c, abs_y(c, SD_SAMPLE), sample);
        change = true;
    } else {
        change = sample != rd16(c, dp(c, ch + CH_SAMPLE));
    }
    c->a = sample;
    set_nz_w(c, sample, true);
    if (change) {
        if (snd_call(c, ROUTINE_SAMPLE_CHANGE)) return 1;   /* A = the sample number */
    } else {
        c->ps |= M377_M;
        const uint8_t mod = rd8(c, dp(c, c->x + CH_MOD));
        c->a = (uint16_t)((c->a & 0xFF00) | mod);
        set_nz_w(c, mod, false);
        if (mod && snd_call(c, ROUTINE_MOD_RESTART)) return 1;
    }

    /* E578 */
    c->ps &= (uint16_t)~M377_M;
    const uint16_t x = c->x;
    wr16(c, dp(c, x + SD_W6), rd16(c, abs_y(c, SD_W6)));
    wr16(c, dp(c, x + SD_W8), rd16(c, abs_y(c, SD_W8)));
    wr16(c, dp(c, x + SD_WC), rd16(c, abs_y(c, SD_WC)));
    const uint16_t we = rd16(c, abs_y(c, SD_WE));
    wr16(c, dp(c, x + SD_WE), we);
    c->ps |= M377_M;
    c->ps &= (uint16_t)~M377_C;
    const uint8_t n2 = adc8(c, rd8(c, dp(c, x + CH_NOTE)), rd8(c, dp(c, x + CH_NOTE_OFS)));
    wr8(c, dp(c, x + CH_NOTE), n2);
    wr8(c, dp(c, x + CH_ACTIVE), 0xFF);
    const uint8_t k = rd8(c, dp(c, V_EVENT_KIND));
    c->a = (uint16_t)((we & 0xFF00) | k);
    set_nz_w(c, k, false);
    if (k & 0x80) {                                    /* silent: no key-on */
        c->dt = 0;
        c->pc = pop16(c);
        return ran(R_NOTE_EVENT);
    }
    if (snd_call(c, rd16(c, VEC_KEY_ON))) return 1;   /* 0xE5A7 */
    c->pc = pop16(c);                                  /* E5A2: RTS */
    return ran(R_NOTE_EVENT);
}

/* A BAIL. Readable code keeps registers, flags, memory and the stack exactly
 * as the original has them at the same point, so wherever the original takes
 * a path no gated scenario ever runs, the readable code hands the CPU to the
 * translated program AT THAT ADDRESS and stops: the original then runs that
 * path itself. A never-run path is thereby never guessed at. (Declining at
 * entry is the special case of bailing at the entry address.) */
static int bail(m37710_t *c, uint16_t pc, int r)
{
    if (trace_on()) fprintf(stderr, "[SEQ] bail to %04X Y=%04X\n", pc, c->y);
    c->pc = pc;
    g_declined[r]++;
    return 1;
}
static inline void lda8(m37710_t *c, uint8_t v) { c->a = (uint16_t)((c->a & 0xFF00) | v); set_nz_w(c, v, false); }
static inline void cmp8(m37710_t *c, uint8_t a, uint8_t m) { cmp_flags(c, a, m, false); }

/* =========================================================================
 * KEY-ON  (0xE5A7, through the vector at 0x0118)
 *
 * X = the channel, 0xEA = its voice. Chooses how the channel will behave
 * from its sound definition and installs it as the channel's hooks, then
 * keys the voice on.
 *   - Volume envelope: +0x06 = envelope number (0 = none). Envelope n's
 *     script address is the word n-1 in the table whose address is at
 *     0x210006 (kept at +0x1C); its first byte's sign decides whether +0x1E
 *     is reset; 0xEF9D starts it. With none, +0x1F = 0 and the fx1 hook
 *     (+0x5C) is the empty 0xE9D5.
 *   - PAN MODE, +0x0E, with +0x03 its argument:
 *       bit 7 set, or below 0x40  fixed pan through the pan law (0x012E):
 *                                 the four attenuations to +0x70..+0x73,
 *                                 volume hook +0x60 = 0xE97E;
 *       0x7D                      pan FOLLOWS a game parameter (0x4100 + 2n
 *                                 into +0x5E): volume hook 0xECC4;
 *       0x7F                      pan through 0x012C, volume hook 0xE97E;
 *       0x79/0x7A/0x7C/0x7E       a pan SCRIPT (table at 0x210008); script
 *                                 0 or 0xFF forms, volume hook 0xECD4 for
 *                                 0x7A/0x7E.
 *   - +0x0C (a second modulation) = 0: fx4 hook (+0x5A) empty.
 *   - The voice: bank = +0x14, flags = +0x2C | KEYON.
 * Paths no gated scenario runs are bailed to (see bail()).
 * ========================================================================= */
enum { CH_ENV = 0x06, CH_RELEASE_SRC = 0x09, CH_ENV_SCRIPT = 0x1C, CH_ENV_POS = 0x1E, CH_ENV_LEVEL = 0x1F,
       CH_PAN_MODE = 0x0E, CH_PAN_ARG = 0x03, CH_PAN_MODE_SAVE = 0x6E, CH_PAN_ARG_SAVE = 0x6F, CH_SPK = 0x70,
       CH_MOD2 = 0x0C, CH_BANK = 0x14, CH_VFLAGS = 0x2C, CH_PSCR_FLAG = 0x31, CH_PSCR_A = 0x67, CH_PSCR_B = 0x6C,
       CH_PSCR_POS = 0x3E, CH_PSCR_POS2 = 0x6A, CH_PITCH_B = 0x40,
       T_ENV_TABLE_PTR = 0x0006, T_PANSCR_TABLE_PTR = 0x0008,
       ROUTINE_ENV_START = 0xEF9D, VEC_PAN_FIXED = 0x012E, HOOK_EMPTY = 0xE9D5, HOOK_VOL_STD = 0xE97E,
       HOOK_VOL_PARAM_PAN = 0xECC4, HOOK_VOL_PAN_SCRIPT = 0xECD4 };
SND_ENTRY(0xE5A7, snd_key_on, 0)
int snd_key_on(m37710_t *c)
{
    c->ps &= (uint16_t)~M377_M;
    c->y = rd16(c, dp(c, V_VOICE_REGS));
    c->a = 0; set_nz_w(c, 0, true);
    wr16(c, abs_y(c, VF_FLAGS), 0);                    /* voice flags cleared */
    c->ps |= M377_M;
    c->dt = 0x21;
    const uint16_t ch = c->x;
    lda8(c, rd8(c, dp(c, ch + CH_PENV_ON)));
    if (c->a & 0xFF) return bail(c, 0xE5B8, R_KEY_ON);   /* pitch envelope at key-on */

    /* volume envelope */
    const uint8_t env = rd8(c, dp(c, ch + CH_ENV));
    lda8(c, env);
    if (env) {
        c->ps &= (uint16_t)~M377_M;
        const uint16_t idx = (uint16_t)((((uint16_t)((c->a & 0xFF00) | (uint8_t)(env - 1))) & 0xFF) << 1);
        c->ps &= (uint16_t)~M377_C;
        const uint16_t slot = adc16(c, idx, rd16(c, dbank(c, T_ENV_TABLE_PTR)));
        c->y = slot;
        const uint16_t script = rd16(c, abs_y(c, 0));
        wr16(c, dp(c, ch + CH_ENV_SCRIPT), slot);      /* STY $1C,X */
        c->y = script;
        c->ps |= M377_M;
        const uint8_t rel = rd8(c, dp(c, ch + CH_RELEASE_SRC));
        wr8(c, dp(c, ch + CH_RELEASE), rel);
        const uint8_t first = rd8(c, abs_y(c, 0));
        lda8(c, first);
        if (!(first & 0x80)) {
            c->ps &= (uint16_t)~M377_M;
            wr16(c, dp(c, ch + CH_ENV_POS), 0xFFFF);
            c->ps |= M377_M;
        }
        if (snd_call(c, ROUTINE_ENV_START)) return 1;
        c->ps &= (uint16_t)~M377_M;
    } else {
        wr8(c, dp(c, ch + CH_ENV_LEVEL), 0);
        c->ps &= (uint16_t)~M377_M;
        wr16(c, dp(c, ch + CH_HOOK_FX1), HOOK_EMPTY);
    }

    /* E60E */
    const uint16_t x = c->x;
    c->a = 0; set_nz_w(c, 0, true);
    wr16(c, dp(c, x + CH_PITCH_B), 0);
    wr16(c, dp(c, x + CH_PITCH_C), 0);
    c->ps |= M377_M;
    const uint8_t mode = rd8(c, dp(c, x + CH_PAN_MODE));
    lda8(c, mode);

    enum { VOL_STD, VOL_DONE } vol = VOL_STD;
    if (mode & 0x80) goto fixed_pan;
    if (mode == 0x7C || mode == 0x7E || mode == 0x7A || mode == 0x79) goto pan_script;
    wr8(c, dp(c, x + CH_PAN_MODE_SAVE), mode);
    {
        const uint8_t arg = rd8(c, dp(c, x + CH_PAN_ARG));
        c->b = (uint16_t)((c->b & 0xFF00) | arg);
        wr8(c, dp(c, x + CH_PAN_ARG_SAVE), arg);
    }
    cmp8(c, mode, 0x7D);
    if (mode == 0x7D) {                                /* pan follows a game parameter */
        const uint8_t n = rd8(c, dp(c, x + CH_PAN_ARG));
        c->ps &= (uint16_t)~M377_M;
        c->ps &= (uint16_t)~M377_C;
        const uint16_t pp = adc16(c, (uint16_t)(n << 1), 0x4100);
        c->a = pp;
        wr16(c, dp(c, x + CH_PAN_PTR), pp);
        wr16(c, dp(c, x + CH_HOOK_VOLUME), HOOK_VOL_PARAM_PAN);
        vol = VOL_DONE;
        goto second_mod;
    }
    if (mode > 0x7D) {                                 /* 0x7F */
        lda8(c, rd8(c, dp(c, x + CH_PAN_ARG)));
        if (snd_call(c, rd16(c, VEC_PAN_LAW))) return 1;
        goto store_speakers;
    }
    cmp8(c, mode, 0x40);
    if (mode >= 0x40) return bail(c, 0xE63C, R_KEY_ON);

fixed_pan:
    lda8(c, rd8(c, dp(c, x + CH_PAN_ARG)));
    c->b = (uint16_t)((c->b & 0xFF00) | rd8(c, dp(c, x + CH_PAN_MODE)));
    if (snd_call(c, rd16(c, VEC_PAN_FIXED))) return 1;
store_speakers:
    c->ps &= (uint16_t)~M377_M;
    {
        const uint16_t xx = c->x;
        const uint16_t a0 = rd16(c, dp(c, V_SCRATCH + 0));
        wr16(c, dp(c, xx + CH_SPK), a0);
        const uint16_t a2 = rd16(c, dp(c, V_SCRATCH + 2));
        wr16(c, dp(c, xx + CH_SPK + 2), a2);
        c->a = a2; set_nz_w(c, a2, true);
    }
    goto vol_std;

pan_script: {
        /* E68D: the pan script's table word for argument n */
        const uint8_t n = rd8(c, dp(c, x + CH_PAN_ARG));
        c->ps &= (uint16_t)~M377_M;
        const uint16_t idx = (uint16_t)(((uint8_t)(n - 1)) << 1);
        c->ps &= (uint16_t)~M377_C;
        c->y = adc16(c, idx, rd16(c, dbank(c, T_PANSCR_TABLE_PTR)));
        c->y = rd16(c, abs_y(c, 0));
        c->ps |= M377_M;
        const uint8_t v = rd8(c, abs_y(c, 0));
        lda8(c, v);
        c->y = (uint16_t)(c->y + 1);
        cmp8(c, v, 0x00);
        uint8_t flag;
        if (v == 0x00) {
            flag = 1;
        } else {
            cmp8(c, v, 0xFF);
            if (v != 0xFF) {
                flag = v;
            } else {
                const uint8_t m2 = rd8(c, dp(c, x + CH_PAN_MODE));
                lda8(c, m2);
                const uint8_t a3 = rd8(c, dp(c, x + CH_PAN_ARG));
                c->b = (uint16_t)((c->b & 0xFF00) | a3);
                if (m2 == 0x7E) goto to7a;
                if (m2 == 0x7C) return bail(c, 0xE6DD, R_KEY_ON);
                cmp_flags(c, a3, rd8(c, dp(c, x + CH_PAN_ARG_SAVE)), false);
                if (a3 == rd8(c, dp(c, x + CH_PAN_ARG_SAVE))) goto second_mod_vol_skip;
                return bail(c, 0xE6BC, R_KEY_ON);
            to7a:
                wr8(c, dp(c, x + CH_PAN_MODE_SAVE), 0x7A);
                wr8(c, dp(c, x + CH_PAN_MODE), 0x7A);
                /* PHY ; PHX ; LDY $3C,X ; TYX ; LDM $0E,X #$7A ; PLX ; PLY -- the sound
                 * definition itself is switched to 0x7A too (16-bit index registers) */
                push16(c, c->y); push16(c, c->x);
                {
                    const uint16_t sd = rd16(c, dp(c, c->x + CH_SOUND));
                    wr8(c, dp(c, sd + CH_PAN_MODE), 0x7A);
                }
                c->x = pop16(c); c->y = pop16(c);
                wr8(c, dp(c, c->x + CH_PAN_ARG_SAVE), (uint8_t)c->b);
                flag = 1;
            }
        }
        /* E6F0/E6F2 */
        lda8(c, flag);
        wr8(c, dp(c, c->x + CH_PSCR_FLAG), flag);
        const uint8_t w = rd8(c, abs_y(c, 0));
        lda8(c, w);
        c->y = (uint16_t)(c->y + 1);
        wr8(c, dp(c, c->x + CH_PSCR_A), w);
        wr8(c, dp(c, c->x + CH_PSCR_B), w);
        const uint8_t m3 = rd8(c, dp(c, c->x + CH_PAN_MODE));
        lda8(c, m3);
        cmp8(c, m3, 0x7A);
        if (m3 != 0x7A && m3 != 0x7E) return bail(c, 0xE706, R_KEY_ON);
        c->ps &= (uint16_t)~M377_M;
        wr16(c, dp(c, c->x + CH_HOOK_VOLUME), HOOK_VOL_PAN_SCRIPT);
        c->ps |= M377_M;
        wr16(c, dp(c, c->x + CH_PSCR_POS), c->y);      /* STY: 16-bit index */
        wr16(c, dp(c, c->x + CH_PSCR_POS2), c->y);
        vol = VOL_DONE;
        goto second_mod;
    }
second_mod_vol_skip:
    vol = VOL_DONE;
    goto second_mod;

vol_std:
    c->ps &= (uint16_t)~M377_M;
    wr16(c, dp(c, c->x + CH_HOOK_VOLUME), HOOK_VOL_STD);
second_mod:
    (void)vol;
    c->ps |= M377_M;
    {
        const uint8_t m2 = rd8(c, dp(c, c->x + CH_MOD2));
        lda8(c, m2);
        if (m2) return bail(c, 0xE72B, R_KEY_ON);
    }
    c->ps &= (uint16_t)~M377_M;
    wr16(c, dp(c, c->x + CH_HOOK_FX4), HOOK_EMPTY);

    /* E782: key the voice on */
    c->dt = 0;
    c->y = rd16(c, dp(c, V_VOICE_REGS));
    const uint16_t bank = rd16(c, dp(c, c->x + CH_BANK));
    wr16(c, abs_y(c, 8), bank);
    const uint16_t vf = (uint16_t)(rd16(c, dp(c, c->x + CH_VFLAGS)) | 0x4000);
    c->a = vf; set_nz_w(c, vf, true);
    wr16(c, abs_y(c, VF_FLAGS), vf);
    c->pc = pop16(c);                                  /* RTS */
    return ran(R_KEY_ON);
}

/* =========================================================================
 * THE SEQUENCER  (0xD9D0, through the vector at 0x010A)
 *
 * X = the track record (0x4500 + 0x50*slot), B = its step flags just stored.
 * A track whose step flags lost bit 15 is stopped (0xE18D, jumped to). When
 * its next event time (+0x00) is due against the tick clock, its sequence --
 * in the data bank at +0x12, from the pointer at +0x10 -- is read:
 *   byte < 0x80  a COMMAND: 0xE2 = the byte, and handler (byte & 0x3F) from
 *                the table at 0x0130 runs (it may read operands through Y);
 *   byte >= 0x80 a WAIT of (byte & 0x7F) units, to +0x1A;
 * a wait already running (+0x1A nonzero) just counts down. The next event
 * time then advances by the wait length (+0x09) times the tempo (+0x07) --
 * the MPY -- and the pointer is saved. A handler may end the track by
 * returning past the sequencer (snd_call reports it; the original code
 * carries on). Glide-length forms (+0x1B, +0x0D) no scenario uses are bailed.
 * ========================================================================= */
enum { TRK_TIME = 0x00, TRK_TEMPO = 0x07, TRK_UNIT = 0x09, TRK_SEQ_PTR = 0x10, TRK_SEQ_BANK = 0x12,
       TRK_WAIT = 0x1A, TRK_WAIT_HI = 0x1B, TRK_GLIDE = 0x0D, V_TRACK = 0xD8, SEQ_HANDLERS = 0x0130,
       ROUTINE_TRACK_STOPPED = 0xE18D };
SND_ENTRY(0xD9D0, snd_sequencer, 0)
int snd_sequencer(m37710_t *c)
{
    if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xD9D0, R_SEQUENCER);
    const bool running = (c->b & 0x8000) != 0;         /* ASL B */
    c->b = (uint16_t)(c->b << 1);
    c->ps &= (uint16_t)~(M377_C | M377_N | M377_Z);
    if (running) c->ps |= M377_C;
    if (c->b & 0x8000) c->ps |= M377_N;
    if (c->b == 0) c->ps |= M377_Z;
    if (!running) { c->pc = ROUTINE_TRACK_STOPPED; return ran(R_SEQUENCER); }

    const uint16_t trk = c->x;
    const uint16_t t = rd16(c, dp(c, trk + TRK_TIME));
    const uint16_t clock = rd16(c, dp(c, V_TICK_CLOCK));
    cmp_flags(c, t, clock, true);
    c->a = t;
    if (t != clock && !((uint16_t)(t - clock) & 0x8000)) {   /* not due yet */
        c->pc = pop16(c);
        return ran(R_SEQUENCER);
    }

    c->ps |= M377_M;
    const uint8_t bank = rd8(c, dp(c, trk + TRK_SEQ_BANK));
    push8(c, bank); c->dt = pop8(c);                   /* PHA ; PLB */
    set_nz_w(c, bank, false);
    const uint8_t waiting = rd8(c, dp(c, trk + TRK_WAIT));
    lda8(c, waiting);
    if (waiting) {
        c->y = rd16(c, dp(c, trk + TRK_SEQ_PTR));
        const uint8_t w = (uint8_t)(rd8(c, dp(c, trk + TRK_WAIT)) - 1);
        wr8(c, dp(c, trk + TRK_WAIT), w);
        set_nz_w(c, w, false);
    } else {
        c->y = rd16(c, dp(c, trk + TRK_SEQ_PTR));
        for (;;) {                                     /* D9EA */
            c->ps |= M377_M;
            c->x = rd16(c, dp(c, V_TRACK));
            const uint8_t b = rd8(c, abs_y(c, 0));
            lda8(c, b);
            if (b & 0x80) {                            /* a wait */
                const uint8_t w = b & 0x7F;
                lda8(c, w);
                wr8(c, dp(c, c->x + TRK_WAIT), w);
                c->y = (uint16_t)(c->y + 1);
                break;
            }
            c->y = (uint16_t)(c->y + 1);
            wr8(c, dp(c, V_EVENT_KIND), b);
            /* AND #$3F ; PEA D9EA ; CLM ; AND #$FF ; ASL ; ADC #$130 ; PHX ; TAX ; LDA $00,X ; PLX ; PHA ; RTS */
            c->ps &= (uint16_t)~M377_M;
            g_seq_cmd[b & 0x3F]++;
            if (trace_on()) fprintf(stderr, "[SEQ] cmd %02X at Y=%04X bank %02X handler %04X\n", b, c->y - 1, c->dt, rd16(c, dp(c, (uint16_t)(SEQ_HANDLERS + (b & 0x3F) * 2))));
            const uint16_t entry = (uint16_t)(SEQ_HANDLERS + (b & 0x3F) * 2);
            const uint16_t handler = rd16(c, dp(c, entry));
            c->a = handler;
            c->ps &= (uint16_t)~(M377_C | M377_V);
            set_nz_w(c, handler, true);
            if (snd_call(c, handler)) return 1;        /* returns to D9EA -- or ends the track */
        }
    }

    /* DA25 */
    c->ps |= M377_M;
    const uint16_t x = c->x;
    const uint8_t hi = rd8(c, dp(c, x + TRK_WAIT_HI));
    lda8(c, hi);
    if (hi) return bail(c, 0xDA92, R_SEQUENCER);
    const uint8_t gl = rd8(c, dp(c, x + TRK_GLIDE));
    lda8(c, gl);
    if (gl) return bail(c, 0xDA2E, R_SEQUENCER);

    /* DA81: time += unit * tempo */
    const uint8_t unit = rd8(c, dp(c, x + TRK_UNIT));
    const uint8_t tempo = rd8(c, dp(c, x + TRK_TEMPO));
    const uint16_t step = (uint16_t)(unit * tempo);    /* MPY $07,X (8-bit): B:A */
    c->b = (uint16_t)((c->b & 0xFF00) | (step >> 8));
    push8(c, (uint8_t)(step >> 8)); push8(c, (uint8_t)step);   /* PHB ; PHA */
    c->ps &= (uint16_t)~M377_M;
    c->a = pop16(c);                                   /* PLA (16-bit) */
    c->dt = 0;
    c->ps &= (uint16_t)~M377_C;
    const uint16_t nt = adc16(c, c->a, rd16(c, dp(c, x + TRK_TIME)));
    c->a = nt;
    wr16(c, dp(c, x + TRK_TIME), nt);
    wr16(c, dp(c, x + TRK_SEQ_PTR), c->y);
    c->pc = pop16(c);                                  /* RTS */
    return ran(R_SEQUENCER);
}

/* =========================================================================
 * SEQUENCE COMMANDS  (handlers in the table at 0x0130)
 *
 * Entered from the sequencer with X = the track record, Y = the byte after
 * the command (its operands), 0xE2 = the command byte, the data bank = the
 * sequence's, 16-bit A and X/Y. Each returns to the sequencer's read loop.
 * ========================================================================= */
static inline uint8_t  seq8 (m37710_t *c, int ofs) { return rd8(c, abs_y(c, (uint16_t)ofs)); }
static inline uint16_t seq16(m37710_t *c, int ofs) { return rd16(c, abs_y(c, (uint16_t)ofs)); }
/* a game parameter's low byte: the mailbox word 0x4100 + 2n (bank 0) */
static inline uint8_t param8(m37710_t *c, uint8_t n) { return rd8(c, dp(c, (uint16_t)(SH_PARAMS + n * 2))); }
static int seq_done(m37710_t *c) { c->pc = pop16(c); return ran(R_SEQCMD); }

/* 0x10 (0xE277) GOTO: pointer = word, bank = byte after it. How music loops. */
SND_ENTRY(0xE277, snd_seq_goto, 0)
int snd_seq_goto(m37710_t *c)
{
    if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xE277, R_SEQCMD);
    const uint16_t to = seq16(c, 0);
    const uint16_t bw = seq16(c, 2);
    c->b = to; c->a = bw;
    c->y = to;                                         /* TBY */
    c->ps |= M377_M;
    wr8(c, dp(c, c->x + TRK_SEQ_BANK), (uint8_t)bw);
    push8(c, (uint8_t)bw); c->dt = pop8(c);            /* PHA ; PLB */
    set_nz_w(c, c->dt, false);
    return seq_done(c);
}

/* 0x02 (0xDAAF) POKE16: word [address] = value. */
SND_ENTRY(0xDAAF, snd_seq_poke16, 0)
int snd_seq_poke16(m37710_t *c)
{
    if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xDAAF, R_SEQCMD);
    const uint16_t addr = seq16(c, 0);
    c->x = addr;
    const uint16_t v = seq16(c, 2);
    c->a = v; set_nz_w(c, v, true);
    wr16(c, dp(c, addr), v);
    c->y = (uint16_t)(c->y + 4);
    return seq_done(c);
}

/* SET A TRACK FIELD (0xDABD): byte [track + (i & 0x7F)] = the next byte, or,
 * with bit 7 of i set, game parameter n's low byte. */
static int seq_set_field(m37710_t *c)
{
    c->ps |= M377_M;
    const uint8_t i = seq8(c, 0);
    c->ps &= (uint16_t)~M377_M;
    const uint16_t trk = c->x;
    push16(c, trk);                                    /* PHX ; ADC $01,S */
    const uint16_t target = (uint16_t)((i & 0x7F) + trk);
    c->x = target;
    c->ps |= M377_M;
    uint8_t v;
    if (i & 0x80) v = param8(c, seq8(c, 1));
    else          v = seq8(c, 1);
    lda8(c, v);
    wr8(c, dp(c, target), v);
    c->x = pop16(c);                                   /* PLX */
    c->y = (uint16_t)(c->y + 2);
    return seq_done(c);
}
/* 0x06 (0xDB5C): the long-wait byte +0x1B = 0, then set a field. */
SND_ENTRY(0xDB5C, snd_seq_06, 0)
int snd_seq_06(m37710_t *c)
{
    c->ps |= M377_M;
    wr8(c, dp(c, c->x + TRK_WAIT_HI), 0);
    return seq_set_field(c);
}
/* 0x07 (0xDB63): +0x1E = the address of the track's own +0x06, then set a field. */
SND_ENTRY(0xDB63, snd_seq_07, 0)
int snd_seq_07(m37710_t *c)
{
    if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xDB63, R_SEQCMD);
    c->ps &= (uint16_t)~M377_C;
    const uint16_t v = adc16(c, c->x, 6);
    c->a = v;
    wr16(c, dp(c, c->x + 0x1E), v);
    return seq_set_field(c);
}
/* 0x2D (0xDB9D): the word at +0x0C = 0 (glide off), then set a field. */
SND_ENTRY(0xDB9D, snd_seq_2d, 0)
int snd_seq_2d(m37710_t *c)
{
    if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xDB9D, R_SEQCMD);
    wr16(c, dp(c, c->x + 0x0C), 0);
    return seq_set_field(c);
}

/* THE SUB-CHANNEL ITERATOR (0xDAF4). Operands: a MASK byte (one bit per
 * sub-channel of the track's slot, 0x20 bytes each from 0xD6), a FIELD byte
 * (bit 7: take the values from game parameters), and -- when the command
 * byte has bit 6 -- one value shared by all; otherwise one value per selected
 * sub-channel follows. Each selected sub-channel's field is set, then the
 * command's ACTION (address in 0xF0) runs with X = that field's address. */
/* sub-channel record fields (and the keymap scratch) */
enum { SC_VALUE = 0x1A, SC_MASK = 0x1B, SC_ENABLED = 0x13, SC_KEYMAP = 0x10, SC_SOUND = 0x18, SC_CHANNEL = 0x16,
       SC_SKIP = 0x12, SC_LEVEL_PARAM = 0x1C, SC_DELAY = 0x08, SC_FOLLOWS = 0x14, CH_RING_WR = 0x34,
       T_KEYMAPS_PTR = 0x000C, V_KIND = 0xC0 };
enum { V_ITER_MASK = 0xCE, V_ITER_SHARED = 0xD2, V_ITER_PARAM = 0xE3, V_ACTION = 0xF0, V_SLOT_BASE = 0xD6 };
enum seq_action { ACT_NONE, ACT_SET_0B, ACT_FLAG_SUBCH, ACT_SET_01_HI, ACT_SET_01_80, ACT_BAIL, ACT_VOICE_1D };
static int seq_iterate_body(m37710_t *c, uint16_t action_addr, enum seq_action act, uint8_t act_val, bool finish);
static int seq_iterate(m37710_t *c, uint16_t action_addr, enum seq_action act, uint8_t act_val)
{
    c->ps &= (uint16_t)~M377_M;
    wr16(c, dp(c, V_ACTION), action_addr);             /* LDM $F0 */
    return seq_iterate_body(c, action_addr, act, act_val, true);
}
/* from 0xDAF5 (after the F0 store); `finish` = return to the sequencer (RTS)
 * rather than to a readable caller that JSRed into the body. Returns 1 when
 * it bailed (the caller must stop), else 0 (finish) or seq_done's value. */
static int seq_iterate_body(m37710_t *c, uint16_t action_addr, enum seq_action act, uint8_t act_val, bool finish)
{
    c->ps |= M377_M;
    wr8(c, dp(c, V_ITER_SHARED), 0);
    uint8_t mask = seq8(c, 0);
    lda8(c, mask);
    wr8(c, dp(c, V_ITER_MASK), mask);
    c->y = (uint16_t)(c->y + 1);
    const uint8_t field = seq8(c, 0);
    c->b = (uint16_t)((c->b & 0xFF00) | field);
    if (rd8(c, dp(c, V_EVENT_KIND)) & 0x40) {          /* one shared value */
        const uint8_t sv = seq8(c, 1);
        wr8(c, dp(c, V_EVENT_KIND), sv);
        c->y = (uint16_t)(c->y + 1);
        wr8(c, dp(c, V_ITER_SHARED), 0xFF);
    }
    wr8(c, dp(c, V_ITER_PARAM), (field & 0x80) ? 0xFF : 0);
    c->y = (uint16_t)(c->y + 1);
    c->ps &= (uint16_t)~M377_M;
    c->ps &= (uint16_t)~M377_C;
    uint16_t target = adc16(c, (uint16_t)(field & 0x7F), rd16(c, dp(c, V_SLOT_BASE)));
    c->b = target;
    c->ps |= M377_M;

    for (;;) {                                         /* DB28 */
        uint8_t m = rd8(c, dp(c, V_ITER_MASK));
        const bool sel = m & 0x80;
        wr8(c, dp(c, V_ITER_MASK), (uint8_t)(m << 1));
        if (sel) {
            uint8_t sh = rd8(c, dp(c, V_ITER_SHARED));
            const bool shared = sh & 0x80;
            wr8(c, dp(c, V_ITER_SHARED), (uint8_t)(sh << 1));
            uint8_t v;
            if (shared) v = rd8(c, dp(c, V_EVENT_KIND));
            else { v = seq8(c, 0); c->y = (uint16_t)(c->y + 1); }
            uint8_t pm = rd8(c, dp(c, V_ITER_PARAM));
            wr8(c, dp(c, V_ITER_PARAM), (uint8_t)(pm << 1));
            if (pm & 0x80) v = param8(c, v);
            lda8(c, v);
            c->x = c->b;                               /* TBX */
            wr8(c, dp(c, c->x), v);
            switch (act) {                             /* JMP ($00F0) */
            case ACT_NONE: break;
            case ACT_SET_0B: wr8(c, dp(c, c->x + 0x0B), act_val); break;
            case ACT_SET_01_HI: {                      /* 0x1B: +0x01 = (own byte, or 0xC7) | 0x80 */
                uint8_t w;
                if (rd8(c, dp(c, 0xC6)) & 0x80) w = rd8(c, dp(c, 0xC7));
                else { w = seq8(c, 0); c->y = (uint16_t)(c->y + 1); }
                w |= 0x80;
                lda8(c, w);
                wr8(c, dp(c, c->x + 0x01), w);
                break; }
            case ACT_SET_01_80: wr8(c, dp(c, c->x + 0x01), 0x80); break;   /* 0x1C */
            case ACT_VOICE_1D: {                       /* 0xE349: give the sub-channel a voice */
                const uint8_t pri = rd8(c, dp(c, c->x));
                lda8(c, pri);
                wr8(c, dp(c, 0xC2), pri);
                if (snd_call(c, rd16(c, 0x0128))) return 1;          /* reset the sub-channel */
                const uint8_t v = rd8(c, dp(c, c->x + 0x11));
                c->ps &= (uint16_t)~(M377_M | M377_C);
                push16(c, c->b);                                     /* PHB */
                const uint32_t prod = (uint32_t)v * 0xC0;             /* MPY #$C0 */
                c->b = (uint16_t)(prod >> 16);
                const uint16_t chn = adc16(c, (uint16_t)prod, 0x7000);  /* its channel */
                c->a = chn;
                wr16(c, dp(c, c->x + SC_CHANNEL), chn);
                push16(c, c->x);                                     /* PHX */
                if (snd_call(c, rd16(c, 0x0110))) return 1;          /* request */
                if (snd_call(c, rd16(c, 0x010E))) return 1;          /* best request */
                c->x = pop16(c);                                     /* PLX */
                c->ps |= M377_M;
                const uint8_t best = (uint8_t)c->a;
                cmp8(c, best, rd8(c, dp(c, 0xC2)));
                if (best == rd8(c, dp(c, 0xC2))) {                   /* this one wins: claim */
                    wr8(c, dp(c, c->x + 0x6E), 0);
                    if (snd_call(c, rd16(c, 0x0108))) return 1;
                }
                c->ps &= (uint16_t)~M377_M;
                c->b = pop16(c);                                     /* PLAB */
                break; }
            case ACT_BAIL:                             /* an action left to the original */
                c->pc = action_addr;
                g_declined[R_SEQCMD]++;
                return 1;
            case ACT_FLAG_SUBCH: {                     /* word at (X & 0xFFF0) |= 0x8000 */
                c->ps &= (uint16_t)~M377_M;
                const uint16_t sa = (uint16_t)(c->x & 0xFFF0);
                const uint16_t w = (uint16_t)(rd16(c, dp(c, sa)) | 0x8000);
                c->a = w;
                wr16(c, dp(c, sa), w);
                c->ps |= M377_M;
                break; }
            }
        }
        /* DB50 */
        c->ps &= (uint16_t)~M377_M;
        c->ps &= (uint16_t)~M377_C;
        c->b = adc16(c, c->b, 0x20);
        c->ps |= M377_M;
        const uint8_t left = rd8(c, dp(c, V_ITER_MASK));
        lda8(c, left);
        if (!left) break;
    }
    return finish ? seq_done(c) : 0;
}
SND_ENTRY(0xDAF0, snd_seq_04, 0)
int snd_seq_04(m37710_t *c) { if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xDAF0, R_SEQCMD); return seq_iterate(c, 0xDB50, ACT_NONE, 0); }
SND_ENTRY(0xDB6D, snd_seq_08, 0)
int snd_seq_08(m37710_t *c) { if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xDB6D, R_SEQCMD); return seq_iterate(c, 0xDB74, ACT_SET_0B, 0x7F); }
SND_ENTRY(0xE38A, snd_seq_28, 0)
int snd_seq_28(m37710_t *c) { if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xE38A, R_SEQCMD); return seq_iterate(c, 0xE391, ACT_FLAG_SUBCH, 0); }
SND_ENTRY(0xE3A3, snd_seq_29, 0)
int snd_seq_29(m37710_t *c) { if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xE3A3, R_SEQCMD); return seq_iterate(c, 0xE3AA, ACT_SET_0B, 0x7E); }

#define SEQ_ENTRY_MODE(addr) do { if (((c->ps >> 4) & 3u) != 0) return bail(c, (addr), R_SEQCMD); } while (0)

/* 0x1A (0xDC83): iterator, action +0x0B = 0x7D. */
SND_ENTRY(0xDC83, snd_seq_1a, 0)
int snd_seq_1a(m37710_t *c) { SEQ_ENTRY_MODE(0xDC83); return seq_iterate(c, 0xDC8A, ACT_SET_0B, 0x7D); }
/* 0x1D (0xE342): iterator; its action (0xE349) gives each selected sub-channel a voice:
 * reset it, derive its channel (0x7000 + voice x 0xC0), request the voice at the
 * priority just set, and claim it if this request is now the best. */
SND_ENTRY(0xE342, snd_seq_1d, 0)
int snd_seq_1d(m37710_t *c) { SEQ_ENTRY_MODE(0xE342); return seq_iterate(c, 0xE349, ACT_VOICE_1D, 0); }

/* 0x1B (0xDD01): iterator whose action sets each sub-channel's +0x01 to a byte
 * (its own, or one shared) with bit 7 set. The field-from-parameter form
 * (bit 7 of the field byte) is the original's. */
SND_ENTRY(0xDD01, snd_seq_1b, 0)
int snd_seq_1b(m37710_t *c)
{
    SEQ_ENTRY_MODE(0xDD01);
    c->ps |= M377_M;
    const uint8_t field = seq8(c, 1);
    lda8(c, field);
    if (field & 0x80) return bail(c, 0xDD3A, R_SEQCMD);
    c->ps &= (uint16_t)~M377_M;
    wr16(c, dp(c, V_ACTION), 0xDD22);
    c->ps |= M377_M;
    const uint8_t k = rd8(c, dp(c, V_EVENT_KIND));
    const uint8_t k2 = (uint8_t)(k << 1);
    lda8(c, k2);
    wr8(c, dp(c, 0xC6), k2);
    if (k2 & 0x80) { const uint8_t v = seq8(c, 3); lda8(c, v); wr8(c, dp(c, 0xC7), v); }
    if (seq_iterate_body(c, 0xDD22, ACT_SET_01_HI, 0, false)) return 1;   /* JSR 0xDAF5 */
    c->ps |= M377_M;
    const uint8_t k3 = rd8(c, dp(c, 0xC6));
    lda8(c, k3);
    if (k3 & 0x80) c->y = (uint16_t)(c->y + 1);
    return seq_done(c);
}

/* 0x1C (0xDC90): with the field's bit 7 clear, the plain iterator with action
 * +0x01 = 0x80; with it set, each selected sub-channel's 16-bit field takes a
 * game parameter's whole word with bit 15 set (shared value only; one value
 * per sub-channel is the original's). */
SND_ENTRY(0xDC90, snd_seq_1c, 0)
int snd_seq_1c(m37710_t *c)
{
    SEQ_ENTRY_MODE(0xDC90);
    const uint8_t field0 = seq8(c, 1);
    if (!(field0 & 0x80)) return seq_iterate(c, 0xDCFB, ACT_SET_01_80, 0);
    c->ps &= (uint16_t)~M377_M;
    wr16(c, dp(c, V_ACTION), 0xDCFB);
    c->ps |= M377_M;
    wr8(c, dp(c, V_ITER_SHARED), 0);
    const uint8_t mask = seq8(c, 0);
    lda8(c, mask);
    wr8(c, dp(c, V_ITER_MASK), mask);
    c->y = (uint16_t)(c->y + 1);
    const uint8_t field = seq8(c, 0);
    c->b = (uint16_t)((c->b & 0xFF00) | field);
    if (rd8(c, dp(c, V_EVENT_KIND)) & 0x40) {
        const uint8_t sv = seq8(c, 1);
        wr8(c, dp(c, V_EVENT_KIND), sv);
        c->y = (uint16_t)(c->y + 1);
        wr8(c, dp(c, V_ITER_SHARED), 0xFF);
    }
    wr8(c, dp(c, V_ITER_PARAM), 0);
    c->y = (uint16_t)(c->y + 1);                        /* DCBB */
    c->ps &= (uint16_t)~(M377_M | M377_C);
    c->b = adc16(c, (uint16_t)(field & 0x7F), rd16(c, dp(c, V_SLOT_BASE)));
    c->ps |= M377_M;
    for (;;) {                                         /* DCC9 */
        const uint8_t m = rd8(c, dp(c, V_ITER_MASK));
        wr8(c, dp(c, V_ITER_MASK), (uint8_t)(m << 1));
        if (m & 0x80) {
            const uint8_t sh = rd8(c, dp(c, V_ITER_SHARED));
            wr8(c, dp(c, V_ITER_SHARED), (uint8_t)(sh << 1));
            if (!(sh & 0x80)) return bail(c, 0xDCD6, R_SEQCMD);
            const uint8_t n = rd8(c, dp(c, V_EVENT_KIND));
            c->ps &= (uint16_t)~M377_M;
            const uint16_t pw = rd16(c, dp(c, (uint16_t)(SH_PARAMS + n * 2)));
            c->x = c->b;
            const uint16_t v = (uint16_t)(pw | 0x8000);
            c->a = v; set_nz_w(c, v, true);
            wr16(c, dp(c, c->x), v);
        } else {
            c->ps &= (uint16_t)~M377_M;
        }
        c->ps &= (uint16_t)~M377_C;                    /* DCEC */
        c->b = adc16(c, c->b, 0x20);
        c->ps |= M377_M;
        const uint8_t left = rd8(c, dp(c, V_ITER_MASK));
        lda8(c, left);
        if (!left) break;
    }
    return seq_done(c);
}

/* 0x0A (0xDB8D): +0x1E = the mailbox address of game parameter n. */
SND_ENTRY(0xDB8D, snd_seq_0a, 0)
int snd_seq_0a(m37710_t *c)
{
    SEQ_ENTRY_MODE(0xDB8D);
    c->ps |= M377_M;
    const uint8_t n = seq8(c, 0);
    c->y = (uint16_t)(c->y + 1);
    c->ps &= (uint16_t)~(M377_M | M377_C);
    const uint16_t a = adc16(c, (uint16_t)(n << 1), SH_PARAMS);
    c->a = a;
    wr16(c, dp(c, c->x + 0x1E), a);
    return seq_done(c);
}

/* 0x0D (0xDBF0): START ANOTHER TRACK. Slot n's command word = the operand's
 * low 11 bits | 0x4800, and its step pointer (0x4040 + 2n) = this slot's
 * command-word address. */
SND_ENTRY(0xDBF0, snd_seq_0d, 0)
int snd_seq_0d(m37710_t *c)
{
    SEQ_ENTRY_MODE(0xDBF0);
    c->ps |= M377_M;
    const uint8_t n = seq8(c, 0);
    c->y = (uint16_t)(c->y + 1);
    c->ps &= (uint16_t)~M377_M;
    const uint16_t w = seq16(c, 0);
    c->y = (uint16_t)(c->y + 2);
    c->ps &= (uint16_t)~M377_C;
    const uint16_t slot = adc16(c, (uint16_t)(n << 1), SH_CMD_WORDS);
    const uint16_t trk = c->x;
    c->x = slot;
    const uint16_t cmd = (uint16_t)((w & 0x07FF) | 0x4800);
    c->b = cmd;
    wr16(c, dp(c, slot), cmd);
    const uint16_t me = rd16(c, dp(c, V_SLOT_CMD));
    c->a = me; set_nz_w(c, me, true);
    wr16(c, dp(c, slot + 0x40), me);
    c->x = trk;
    return seq_done(c);
}

/* GOSUB / RETURN: the track's call stack is +0x14 (depth) and three-byte
 * entries from +0x22: return pointer, return bank. */
enum { TRK_DEPTH = 0x14, TRK_STACK = 0x22 };
static uint16_t call_entry(m37710_t *c, uint8_t d)      /* ASL ; ADC $14,X ; CLM ; AND ; ADC $D8 ; ADC #$22 */
{
    c->ps &= (uint16_t)~M377_C;
    if (d & 0x80) c->ps |= M377_C;
    const uint8_t t = adc8(c, (uint8_t)(d << 1), rd8(c, dp(c, c->x + TRK_DEPTH)));
    c->ps &= (uint16_t)~M377_M;
    uint16_t e = adc16(c, t, rd16(c, dp(c, V_TRACK)));
    e = adc16(c, e, TRK_STACK);
    return e;
}
/* 0x11 (0xE157): GOSUB pointer, bank. */
SND_ENTRY(0xE157, snd_seq_gosub, 0)
int snd_seq_gosub(m37710_t *c)
{
    SEQ_ENTRY_MODE(0xE157);
    c->ps |= M377_M;
    const uint8_t d = rd8(c, dp(c, c->x + TRK_DEPTH));
    const uint16_t e = call_entry(c, d);
    c->x = e;
    const uint16_t to = seq16(c, 0);
    c->b = to;
    c->ps |= M377_M;
    wr8(c, dp(c, e + 2), c->dt);                       /* PHT ; PLA ; STA $02,X */
    const uint8_t bank = seq8(c, 2);
    c->ps &= (uint16_t)~M377_M;
    c->y = (uint16_t)(c->y + 3);
    wr16(c, dp(c, e), c->y);                           /* the return pointer */
    c->y = to;
    c->ps |= M377_M;
    lda8(c, bank);
    push8(c, bank); c->dt = pop8(c);
    c->x = rd16(c, dp(c, V_TRACK));
    wr8(c, dp(c, c->x + TRK_SEQ_BANK), bank);
    const uint8_t nd = (uint8_t)(rd8(c, dp(c, c->x + TRK_DEPTH)) + 1);
    wr8(c, dp(c, c->x + TRK_DEPTH), nd);
    set_nz_w(c, nd, false);
    return seq_done(c);
}
/* 0x14 (0xE185): RETURN; at depth 0 the track stops (0xE18D, jumped to). */
SND_ENTRY(0xE185, snd_seq_return, 0)
int snd_seq_return(m37710_t *c)
{
    SEQ_ENTRY_MODE(0xE185);
    c->ps |= M377_M;
    const uint8_t d = rd8(c, dp(c, c->x + TRK_DEPTH));
    lda8(c, d);
    if (!d) { c->pc = ROUTINE_TRACK_STOPPED; return ran(R_SEQCMD); }
    const uint8_t nd = (uint8_t)(d - 1);
    wr8(c, dp(c, c->x + TRK_DEPTH), nd);
    const uint16_t e = call_entry(c, nd);
    c->x = e;
    c->y = rd16(c, dp(c, e));
    c->ps |= M377_M;
    const uint8_t bank = rd8(c, dp(c, e + 2));
    lda8(c, bank);
    c->x = rd16(c, dp(c, V_TRACK));
    wr8(c, dp(c, c->x + TRK_SEQ_BANK), bank);
    push8(c, bank); c->dt = pop8(c);
    return seq_done(c);
}

/* 0x17 (0xE143): bytes to 0x4300.. until one whose low 7 bits are 0 (it is
 * written too), then the counter at 0x43FA counts up. */
SND_ENTRY(0xE143, snd_seq_17, 0)
int snd_seq_17(m37710_t *c)
{
    SEQ_ENTRY_MODE(0xE143);
    c->x = 0x4300;
    c->ps |= M377_M;
    for (;;) {
        const uint8_t b = seq8(c, 0);
        c->y = (uint16_t)(c->y + 1);
        wr8(c, dp(c, c->x), b);
        c->x = (uint16_t)(c->x + 1);
        lda8(c, (uint8_t)(b << 1));
        if (!(uint8_t)(b << 1)) break;
    }
    c->x = 0x43FA;
    const uint8_t n = (uint8_t)(rd8(c, dp(c, c->x)) + 1);
    wr8(c, dp(c, c->x), n);
    set_nz_w(c, n, false);
    return seq_done(c);
}

/* 0x19 (0xDC4A): a record at 0x9000 + 16n. A 16-bit MASK: its top bit
 * selects a WORD for the record's first two bytes, then each further bit a
 * BYTE for the next byte, until the mask runs out (the loop at 0xDC6F). */
SND_ENTRY(0xDC4A, snd_seq_19, 0)
int snd_seq_19(m37710_t *c)
{
    SEQ_ENTRY_MODE(0xDC4A);
    c->ps |= M377_M;
    const uint8_t n = seq8(c, 0);
    c->ps &= (uint16_t)~M377_M;
    c->x = (uint16_t)(0x9000 + (n << 4));
    c->y = (uint16_t)(c->y + 1);
    c->b = seq16(c, 0);
    c->y = (uint16_t)(c->y + 2);
    const bool w = c->b & 0x8000; c->b = (uint16_t)(c->b << 1);
    if (w) { const uint16_t v = seq16(c, 0); c->y = (uint16_t)(c->y + 2); c->a = v; wr16(c, dp(c, c->x), v); }
    c->x = (uint16_t)(c->x + 2);
    do {
        const bool by = c->b & 0x8000; c->b = (uint16_t)(c->b << 1);
        if (by) {
            c->ps |= M377_M;
            const uint8_t v = seq8(c, 0); c->y = (uint16_t)(c->y + 1);
            c->a = (uint16_t)((c->a & 0xFF00) | v);
            wr8(c, dp(c, c->x), v);
            c->ps &= (uint16_t)~M377_M;
        }
        c->x = (uint16_t)(c->x + 1);
    } while (c->b != 0);
    return seq_done(c);
}

/* =========================================================================
 * NOTE-ON FOR SUB-CHANNELS  (commands 0x20 / 0x22 -> 0xDFB8)
 *
 * The slot's eight sub-channels (0x20 bytes each from 0xD6) are the track's
 * "instruments". Operands: a MASK, then (command bit 6) one shared value or
 * one value per selected sub-channel. For each selected sub-channel the value
 * (a note, or for 0x22 a level) goes to +0x1A and an event is QUEUED to its
 * channel (0xE045); the mask is kept in each +0x1B. Then every UNselected
 * sub-channel that follows another (+0x14 = 1 + its leader's index) and whose
 * leader just played re-queues with the leader's value. The command then
 * leaves the sequence for this tick: it drops its own return address and jumps
 * to the sequencer's tail (0xDA11) -- all of which the stack layout carries.
 *
 * QUEUE AN EVENT (0xE045), X = the sub-channel, A = the value: if the
 * sub-channel is enabled (+0x13), its sound is the sub-channel record itself
 * or, with a keymap (+0x10 = instrument + 1), the sound the KEYMAP picks for
 * this value (0xE0EB: the table at 0x21000C, eight (upper bound, sound)
 * pairs, sound n = the record at 0x9000 + 16n). The event goes into its
 * channel's ring (+0x16 = the channel; ring index +0x34): kind 0xC0 (0 note /
 * 1 level) | value << 8, bit 7 if +0x12 says skip (counted down), the sound,
 * the level pointer (the track's +0x20, or game parameter +0x1C), and the
 * due time: the track clock plus +0x08 x 64.
 * ========================================================================= */
/* sub-channel fields: declared above the iterator, which uses SC_CHANNEL */
/* E0EB -- keymap lookup; entered by JSR (return E06F) with A = instrument, B = value */
static int keymap_pick(m37710_t *c)
{
    push16(c, 0xE06F);                                 /* the JSR */
    c->ps &= (uint16_t)~M377_M;
    c->a &= 0x00FF;
    c->y = rd16(c, dp(c, V_KIND));
    push16(c, c->y);                                   /* PHY */
    c->a = (uint16_t)(c->a << 4);                      /* RLA #4 */
    c->dt = 0x21;
    c->ps &= (uint16_t)~M377_C;
    c->a = adc16(c, c->a, rd16(c, dbank(c, T_KEYMAPS_PTR)));
    c->y = (uint16_t)(c->a + 1);
    c->ps |= M377_M;
    wr8(c, dp(c, V_KIND), 8);
    const uint8_t v = (uint8_t)c->b;
    cmp8(c, v, 0xFF);
    if (v == 0xFF) return bail(c, 0xE108, R_SEQCMD);
    for (;;) {                                         /* E10E */
        const uint8_t bound = rd8(c, abs_y(c, 1));
        cmp8(c, v, bound);
        if (v < bound) break;
        const uint8_t left = (uint8_t)(rd8(c, dp(c, V_KIND)) - 1);
        wr8(c, dp(c, V_KIND), left);
        set_nz_w(c, left, false);
        if (!left) break;                              /* the last entry takes the rest */
        c->y = (uint16_t)(c->y + 2);
    }
    const uint8_t id = rd8(c, abs_y(c, 0));            /* E114 */
    c->ps &= (uint16_t)~M377_M;
    c->a = id; set_nz_w(c, id, true);
    if (!id) return bail(c, 0xE12D, R_SEQCMD);
    c->ps &= (uint16_t)~M377_C;
    c->a = adc16(c, (uint16_t)(id << 4), 0x9000);
    wr16(c, dp(c, c->x + SC_SOUND), c->a);
    c->dt = 0;
    c->y = pop16(c);                                   /* E13F: PLY ; STY $C0 ; RTS */
    wr16(c, dp(c, V_KIND), c->y);
    (void)pop16(c);
    return 0;
}
/* E045 -- queue an event; entered by JSR (return `ret`), X = sub-channel, A = value */
static int queue_event(m37710_t *c, uint16_t ret)
{
    push16(c, ret);                                    /* the JSR */
    { const uint16_t t = c->a; c->a = c->b; c->b = t; }   /* XAB: B = the value */
    c->ps |= M377_M;
    const uint8_t en = rd8(c, dp(c, c->x + SC_ENABLED));
    lda8(c, en);
    if (!en) { (void)pop16(c); return 0; }
    push16(c, c->y);                                   /* PHY */
    push8(c, c->dt);                                   /* PHT */
    c->dt = 0;
    const uint8_t e3 = rd8(c, dp(c, V_ITER_PARAM));
    lda8(c, e3);
    if (e3) return bail(c, 0xE054, R_SEQCMD);
    const uint8_t km = rd8(c, dp(c, c->x + SC_KEYMAP));
    lda8(c, km);
    if (km) {
        lda8(c, (uint8_t)(km - 1));
        if (keymap_pick(c)) return 1;
    } else {
        c->ps &= (uint16_t)~M377_M;
        c->a = c->x;
        wr16(c, dp(c, c->x + SC_SOUND), c->x);         /* the sub-channel is its own sound */
    }
    c->ps |= M377_M;                                   /* E076 */
    const uint8_t kind = rd8(c, dp(c, V_KIND));
    cmp8(c, kind, 2);
    if (kind == 2) return bail(c, 0xE07D, R_SEQCMD);
    wr8(c, dp(c, V_KIND + 1), (uint8_t)c->b);          /* STB $C1: the value */
    const uint16_t chn = rd16(c, dp(c, c->x + SC_CHANNEL));
    c->y = chn;
    const uint8_t wi = rd8(c, abs_y(c, CH_RING_WR));
    wr8(c, abs_y(c, CH_RING_WR), (uint8_t)(wi + 1));
    c->ps &= (uint16_t)~(M377_M | M377_C);
    uint16_t slot = adc16(c, (uint16_t)((wi & 7) << 3), chn);
    slot = adc16(c, slot, CH_EVENTS);
    c->y = slot;
    uint16_t kw = rd16(c, dp(c, V_KIND));
    c->ps |= M377_M;
    const uint8_t skip = rd8(c, dp(c, c->x + SC_SKIP));
    c->b = (uint16_t)((c->b & 0xFF00) | skip);
    if (skip) { wr8(c, dp(c, c->x + SC_SKIP), (uint8_t)(skip - 1)); kw |= 0x80; }
    c->ps &= (uint16_t)~M377_M;
    wr16(c, abs_y(c, 2), kw);
    wr16(c, abs_y(c, 4), rd16(c, dp(c, c->x + SC_SOUND)));
    c->ps |= M377_M;
    const uint8_t lp = rd8(c, dp(c, c->x + SC_LEVEL_PARAM));
    c->ps &= (uint16_t)~(M377_M | M377_C);
    const uint16_t level_ptr = lp ? adc16(c, (uint16_t)(lp << 1), SH_PARAMS)
                                  : adc16(c, rd16(c, dp(c, V_TRACK)), 0x20);
    wr16(c, abs_y(c, 6), level_ptr);
    const uint16_t now = rd16(c, ((uint32_t)c->dt << 16) | rd16(c, dp(c, V_TRACK)));   /* LDB ($D8) */
    c->b = now;
    c->ps |= M377_M;
    const uint8_t delay = rd8(c, dp(c, c->x + SC_DELAY));
    c->ps &= (uint16_t)~M377_M;
    uint16_t due = now;
    if (delay) {
        c->ps &= (uint16_t)~M377_C;
        due = adc16(c, (uint16_t)(delay * 0x40), now);   /* MPY #$40 ; ADC $01,S ; PLAB ; XAB */
        c->a = now; c->b = due;
    }
    wr16(c, abs_y(c, 0), due);                          /* STB $0000,Y: the due time */
    c->dt = pop8(c);                                   /* PLB */
    c->y = pop16(c);                                   /* PLY */
    (void)pop16(c);                                    /* RTS */
    return 0;
}
static int seq_note_on(m37710_t *c, uint8_t kind)
{
    c->ps |= M377_M;
    wr8(c, dp(c, V_KIND), kind);
    wr8(c, dp(c, V_ITER_PARAM), 0);
    const uint8_t mask = seq8(c, 0);                   /* DFB8 */
    lda8(c, mask);
    wr8(c, dp(c, V_ITER_MASK), mask);
    push8(c, mask);                                    /* PHA */
    c->y = (uint16_t)(c->y + 1);
    wr8(c, dp(c, V_ITER_SHARED), 0);
    if (rd8(c, dp(c, V_EVENT_KIND)) & 0x40) {
        const uint8_t sv = seq8(c, 0);
        c->y = (uint16_t)(c->y + 1);
        wr8(c, dp(c, V_EVENT_KIND), sv);
        wr8(c, dp(c, V_ITER_SHARED), 0xFF);
    }
    const uint8_t mode = rd8(c, dp(c, c->x + 0x17));
    lda8(c, mode);
    if (mode & 0x80) return bail(c, 0xDFA9, R_SEQCMD);

    c->x = rd16(c, dp(c, V_SLOT_BASE));
    for (;;) {                                         /* DFD6 */
        c->ps |= M377_M;
        const uint8_t m = rd8(c, dp(c, V_ITER_MASK));
        wr8(c, dp(c, c->x + SC_MASK), m);
        wr8(c, dp(c, V_ITER_MASK), (uint8_t)(m << 1));
        if (m & 0x80) {
            const uint8_t sh = rd8(c, dp(c, V_ITER_SHARED));
            wr8(c, dp(c, V_ITER_SHARED), (uint8_t)(sh << 1));
            uint8_t v;
            if (sh & 0x80) v = rd8(c, dp(c, V_EVENT_KIND));
            else { v = seq8(c, 0); c->y = (uint16_t)(c->y + 1); }
            lda8(c, v);
            wr8(c, dp(c, c->x + SC_VALUE), v);
            if (queue_event(c, 0xDFF0)) return 1;
        }
        c->ps |= M377_M;
        const uint8_t left = rd8(c, dp(c, V_ITER_MASK));
        lda8(c, left);
        if (!left) break;
        c->ps &= (uint16_t)~(M377_M | M377_C);
        c->x = adc16(c, c->x, 0x20);
        c->a = c->x;
    }

    c->x = rd16(c, dp(c, V_SLOT_BASE));                /* E000: the followers */
    c->ps |= M377_M;
    const uint8_t orig = pop8(c);
    const uint8_t unsel = (uint8_t)~orig;
    lda8(c, unsel);
    push16(c, c->y);                                   /* PHY */
    wr8(c, dp(c, V_ITER_MASK), unsel);
    for (;;) {                                         /* E008 */
        c->ps |= M377_M;
        const uint8_t m = rd8(c, dp(c, V_ITER_MASK));
        wr8(c, dp(c, V_ITER_MASK), (uint8_t)(m << 1));
        if (m & 0x80) {
            const uint8_t f = rd8(c, dp(c, c->x + SC_FOLLOWS));
            lda8(c, f);
            if (f) {
                c->ps &= (uint16_t)~(M377_M | M377_C);
                const uint16_t leader = adc16(c, (uint16_t)((uint8_t)(f - 1) << 5), rd16(c, dp(c, V_SLOT_BASE)));
                c->ps |= M377_M;
                const uint8_t lm = rd8(c, dp(c, leader + SC_MASK));
                lda8(c, lm);
                if (lm & 0x80) {
                    lda8(c, rd8(c, dp(c, leader + SC_VALUE)));
                    if (queue_event(c, 0xE02D)) return 1;
                }
            }
        }
        c->ps |= M377_M;
        const uint8_t left = rd8(c, dp(c, V_ITER_MASK));
        lda8(c, left);
        if (!left) break;
        c->ps &= (uint16_t)~(M377_M | M377_C);
        c->x = adc16(c, c->x, 0x20);
        c->a = c->x;
    }
    c->y = pop16(c);                                   /* E03D: PLY */
    (void)pop16(c);                                    /* PLX: our own return address */
    c->x = rd16(c, dp(c, V_TRACK));
    c->pc = 0xDA11;                                    /* JMP $DA11: the sequencer's tail */
    return ran(R_SEQCMD);
}
SND_ENTRY(0xDF59, snd_seq_20, 0)
int snd_seq_20(m37710_t *c) { SEQ_ENTRY_MODE(0xDF59); return seq_note_on(c, 0); }
SND_ENTRY(0xDF6D, snd_seq_22, 0)
int snd_seq_22(m37710_t *c) { SEQ_ENTRY_MODE(0xDF6D); return seq_note_on(c, 1); }

/* =========================================================================
 * CONDITIONAL GOTO  (command 0x1F, 0xDE77)
 *
 * Operands: OP (low 4 bits: the comparison; bits 7/6 and 5/4: what the two
 * operands are), operand a, operand b, then two targets -- pointer and bank
 * at +3/+5 (taken when the comparison holds) and +6/+8 (otherwise). An
 * operand is an immediate byte (kind 10) or game parameter n's word (kind
 * 00). Comparisons used: 0 b == a, 2 b <= a, 3 b >= a (unsigned); the others
 * and the other operand kinds are the original's (bailed).
 * ========================================================================= */
/* the operand fetcher at 0xDF26: A holds the two kind bits at bits 7/6 */
static int fetch_operand(m37710_t *c, uint8_t kinds, uint16_t *out, uint16_t at)
{
    const bool k1 = kinds & 0x80, k0 = kinds & 0x40;
    c->a = (uint16_t)((c->a & 0xFF00) | (uint8_t)(kinds << 1));
    if (k1 && k0) return bail(c, 0xDF52, R_SEQCMD);
    if (!k1 && k0) return bail(c, 0xDF2B, R_SEQCMD);
    (void)at;
    const uint8_t n = seq8(c, 1);
    c->ps &= (uint16_t)~M377_M;
    if (k1) *out = n;                                  /* immediate byte */
    else    *out = rd16(c, dp(c, (uint16_t)(SH_PARAMS + n * 2)));   /* game parameter */
    c->b = *out;
    return 0;
}
SND_ENTRY(0xDE77, snd_seq_1f, 0)
int snd_seq_1f(m37710_t *c)
{
    SEQ_ENTRY_MODE(0xDE77);
    c->ps |= M377_M;
    const uint8_t op = seq8(c, 0);
    lda8(c, op);
    push8(c, op);                                      /* PHA */
    push16(c, 0xDE7F);                                 /* JSR DF1A */
    push16(c, 0xDF1D);                                 /* JSR DF26 */
    uint16_t a, b;
    if (fetch_operand(c, op, &a, 0)) return 1;
    (void)pop16(c);                                    /* RTS to DF1D */
    c->y = (uint16_t)(c->y + 1);
    wr16(c, dp(c, V_SCRATCH + 0), a);                  /* STB $C0 */
    c->ps |= M377_M;
    if (fetch_operand(c, (uint8_t)(op << 2), &b, 0)) return 1;
    (void)pop16(c);                                    /* RTS to DE7F */
    c->ps |= M377_M;
    (void)pop8(c);                                     /* PLA: the op */
    c->ps &= (uint16_t)~M377_M;
    const uint16_t handler = rd16(c, dp(c, (uint16_t)(0xDE90 + (op & 0x0F) * 2)));
    c->a = handler;
    const unsigned cmpop = op & 0x0F;
    if (cmpop != 0 && cmpop != 2 && cmpop != 3) return bail(c, handler, R_SEQCMD);
    const uint16_t a0 = rd16(c, dp(c, V_SCRATCH + 0));
    cmp_flags(c, b, a0, true);                         /* CMPB $C0 */
    bool taken;
    if (cmpop == 0)      taken = (b == a0);
    else if (cmpop == 2) taken = (b <= a0);
    else                 taken = !(b < a0);
    const uint16_t ptr = seq16(c, taken ? 2 : 5);
    c->a = ptr;
    c->x = rd16(c, dp(c, V_TRACK));
    wr16(c, dp(c, c->x + TRK_SEQ_PTR), ptr);
    c->ps |= M377_M;
    const uint8_t bank = seq8(c, taken ? 4 : 7);
    lda8(c, bank);
    wr8(c, dp(c, c->x + TRK_SEQ_BANK), bank);
    push8(c, bank); c->dt = pop8(c);
    c->y = rd16(c, dp(c, c->x + TRK_SEQ_PTR));
    return seq_done(c);
}

/* =========================================================================
 * GAME-PARAMETER ARITHMETIC  (command 0x1E, 0xDD85)
 *
 * param[d] = param[d] OP operand, the operand an 8-bit or 16-bit immediate
 * or another game parameter; the flags of the result go to 0xC4 (0x1F's
 * later comparisons can test them). OP (low 4 bits of the second byte): 0
 * assign, 1 add, 2 subtract (carry = no borrow), 5 modulo (DIV), 6 RANDOM
 * below the operand (the LFSR at 0xE8, then modulo); the rest -- multiply,
 * the logic operators -- and the other operand forms are the original's.
 * ========================================================================= */
SND_ENTRY(0xDD85, snd_seq_1e, 0)
int snd_seq_1e(m37710_t *c)
{
    SEQ_ENTRY_MODE(0xDD85);
    c->ps |= M377_M;
    const uint8_t d0 = seq8(c, 0);
    if (d0 & 0x40) return bail(c, 0xDD85, R_SEQCMD);   /* destination form 0xDD93 */
    const uint8_t dn = seq8(c, 1);
    const uint16_t dst = (uint16_t)(SH_PARAMS + dn * 2);
    c->y = (uint16_t)(c->y + 2);
    const uint8_t o = d0;                              /* read again at 0xDD9D, before the INYs */
    uint16_t operand;
    if (o & 0x20) {                                    /* immediate */
        if (o & 0x10) { operand = seq16(c, 0); c->y = (uint16_t)(c->y + 1); }
        else          { operand = seq8(c, 0); }
    } else {
        if (o & 0x10) return bail(c, 0xDD85, R_SEQCMD);   /* 0xDDC4 (bit 4, the fourth ASL) */
        operand = rd16(c, dp(c, (uint16_t)(SH_PARAMS + seq8(c, 0) * 2)));
    }
    wr16(c, dp(c, V_SCRATCH + 0), operand);            /* STA $C0 */
    c->y = (uint16_t)(c->y + 1);
    const unsigned f = o & 0x0F;
    c->ps &= (uint16_t)~M377_M;
    c->x = dst;
    uint16_t v = rd16(c, dp(c, dst));
    const uint16_t handler = rd16(c, dp(c, (uint16_t)(0xDDF7 + f * 2)));
    c->a = v; c->b = handler;                          /* as the handler is entered */
    c->ps &= (uint16_t)~M377_C;                        /* ADCB #$DDF7 leaves carry clear */
    switch (f) {
    case 0: v = operand; set_nz_w(c, v, true); break;
    case 1: c->ps &= (uint16_t)~M377_C; v = adc16(c, v, operand); break;
    case 2: { const uint32_t r = (uint32_t)v - operand;
              c->ps &= (uint16_t)~(M377_N | M377_Z | M377_C | M377_V);
              if (v >= operand) c->ps |= M377_C;
              if ((~(v ^ ~operand) & (v ^ r)) & 0x8000) c->ps |= M377_V;
              v = (uint16_t)r;
              if (!v) c->ps |= M377_Z;
              if (v & 0x8000) c->ps |= M377_N;
              /* BCC -> SEC ; else CLC: the saved carry means BORROW */
              if (c->ps & M377_C) c->ps &= (uint16_t)~M377_C; else c->ps |= M377_C;
              break; }
    case 6: {                                          /* random: step the LFSR, then modulo */
              c->ps |= M377_M;
              const uint8_t e8 = rd8(c, dp(c, 0xE8)), e9 = rd8(c, dp(c, 0xE9));
              uint8_t t = (uint8_t)(e8 ^ e9);
              bool cy = (c->ps & M377_C) != 0;
              { bool nc = t & 0x80; t = (uint8_t)((t << 1) | cy); cy = nc; }   /* ROL */
              { bool nc = t & 0x80; t = (uint8_t)((t << 1) | cy); cy = nc; }   /* ROL */
              c->ps &= (uint16_t)~M377_M;
              const uint16_t w = rd16(c, dp(c, 0xE8));
              const uint16_t w2 = (uint16_t)((w << 1) | (cy ? 1 : 0));
              wr16(c, dp(c, 0xE8), w2);
              if (w & 0x8000) c->ps |= M377_C; else c->ps &= (uint16_t)~M377_C;   /* ROL $E8; DIV keeps C */
              if (!w2) return bail(c, 0xDE61, R_SEQCMD);
              v = w2; }
              /* fall through: modulo */
    case 5: { c->b = 0;
              if (operand == 0) { const uint16_t t2 = c->a; (void)t2; c->ps |= M377_V; v = 0; }   /* DIV overflow: A kept, XAB -> B = 0 */
              else { v = (uint16_t)(v % operand); c->ps &= (uint16_t)~M377_V; }
              set_nz_w(c, v, true);
              break; }
    default: return bail(c, handler, R_SEQCMD);
    }
    c->a = v;
    wr16(c, dp(c, dst), v);                            /* DE0D: STA $00,X ; PHP ; PLA ; STA $C4 */
    wr16(c, dp(c, V_SCRATCH + 4), c->ps);
    c->a = c->ps;
    return seq_done(c);
}

/* =========================================================================
 * COUNTED REPEAT  (command 0x12, 0xE286)
 *
 * Operands: count, target pointer, target bank. The track's loop stack is
 * +0x34 (4-byte entries: count, bank, the pointer to this command's
 * operands), depth +0x15. The first time, an entry is pushed with the count;
 * each later arrival counts down and jumps back until it reaches 0, then
 * pops and continues past the operands.
 * ========================================================================= */
SND_ENTRY(0xE286, snd_seq_12, 0)
int snd_seq_12(m37710_t *c)
{
    SEQ_ENTRY_MODE(0xE286);
    c->ps |= M377_M;
    const uint16_t trk = c->x;
    const uint8_t d = rd8(c, dp(c, trk + 0x15));
    c->ps &= (uint16_t)~(M377_M | M377_C);
    uint16_t e = adc16(c, (uint16_t)(d << 2), rd16(c, dp(c, V_TRACK)));
    e = adc16(c, e, 0x34);
    c->b = (uint16_t)((c->b & 0xFF00) | d);
    if (d) {
        const uint16_t prev = (uint16_t)(e - 4);
        c->x = prev;
        if (c->y != rd16(c, dp(c, prev + 2)) || c->dt != rd8(c, dp(c, prev + 1))) {
            return bail(c, 0xE2BC, R_SEQCMD);          /* a different loop: the original's (X = the previous entry) */
        }
        const uint8_t n = (uint8_t)(rd8(c, dp(c, prev)) - 1);
        c->ps |= M377_M;
        wr8(c, dp(c, prev), n);
        if (n) {                                       /* again */
            c->x = rd16(c, dp(c, V_TRACK));
        } else {                                       /* done */
            c->x = rd16(c, dp(c, V_TRACK));
            wr8(c, dp(c, c->x + 0x15), (uint8_t)(rd8(c, dp(c, c->x + 0x15)) - 1));
            c->ps &= (uint16_t)~M377_M;
            c->y = (uint16_t)(c->y + 4);
            c->a = c->y;
            return seq_done(c);
        }
    } else {
        c->x = e;                                      /* E2C1: a new loop */
        c->ps &= (uint16_t)~M377_M;
        wr16(c, dp(c, e + 2), c->y);
        c->ps |= M377_M;
        wr8(c, dp(c, e), seq8(c, 0));
        wr8(c, dp(c, e + 1), c->dt);
        c->x = rd16(c, dp(c, V_TRACK));
        wr8(c, dp(c, c->x + 0x15), (uint8_t)(rd8(c, dp(c, c->x + 0x15)) + 1));
    }
    c->ps |= M377_M;                                   /* E2D6: go to the target */
    const uint8_t bank = seq8(c, 3);
    wr8(c, dp(c, c->x + TRK_SEQ_BANK), bank);
    c->ps &= (uint16_t)~M377_M;
    c->b = seq16(c, 1);
    c->y = c->b;
    c->dt = bank;
    return seq_done(c);
}

/* =========================================================================
 * THE IDLE WAIT  (0xC12B, the main loop: LDA $83 ; BEQ)
 *
 * The main loop waits for Timer B0's handler (0xC22A) to toggle 0x83, then
 * runs its jobs. The original spins, a few cycles an iteration -- 98% of every
 * instruction the program executes. Here the wait simply jumps the clock to
 * the next moment anything can happen (a timer, the A-D converter, a board
 * pin, the frame end); the interrupt is then taken there as usual. Returning
 * to the same address keeps the loop, so a flag set by that interrupt is seen
 * on the next pass. Held to the event gate (it changes when, not what).
 * ========================================================================= */
SND_ENTRY(0xC12B, snd_idle_wait, 0)
int snd_idle_wait(m37710_t *c)
{
    if (!(c->ps & M377_M) || peek8(c, dp(c, 0x83)) != 0) return declined(R_IDLE);   /* flag set: the original runs on */
    c->a &= 0xFF00;
    c->ps = (uint16_t)((c->ps & ~M377_N) | M377_Z);
    uint64_t next = m37710_next_event(c);
    const uint64_t pin = mcu_sound_next_pin_event();
    if (pin < next) next = pin;
    if (next > c->cycles && next != UINT64_MAX) c->cycles = next;
    else c->cycles += 9;                               /* an event is already due: one iteration's worth */
    return ran(R_IDLE);                                /* PC stays at 0xC12B */
}

/* =========================================================================
 * A NEW COMMAND FROM THE GAME  (0xD788, through the vector at 0x0106)
 *
 * X = the slot's command-word address (0x4000 + 2i), 0xD8 its track. A track
 * still running is stopped first (vector 0x0116). The command word is marked
 * RUNNING (bit 15 set, bit 14 -- "new" -- cleared); unless bit 11 says a
 * command 0x0D already set it, the slot's step pointer (+0x40) points back
 * at its own command word. The command's low 11 bits are the SOUND number:
 * its sequence is the 3-byte entry (pointer, bank) n in the table whose
 * address is at 0x210000. The track starts at that sequence, now, with its
 * working fields (+0x0E/+0x14/+0x16/+0x18/+0x1A) cleared, its flags = the
 * command word, and all eight of the slot's sub-channels disabled (+0x13).
 * ========================================================================= */
enum { VEC_STOP_TRACK = 0x0116, T_SOUND_TABLE_PTR = 0x0000 };
SND_ENTRY(0xD788, snd_new_command, 0)
int snd_new_command(m37710_t *c)
{
    if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xD788, R_NEWCMD);
    c->x = rd16(c, dp(c, V_TRACK));
    const uint16_t fl = rd16(c, dp(c, c->x + TRK_FLAGS));
    c->a = fl; set_nz_w(c, fl, true);
    if (fl & 0x8000) {                                 /* running: stop it first */
        if (snd_call(c, rd16(c, VEC_STOP_TRACK))) return 1;
    }
    c->ps &= (uint16_t)~M377_M;
    const uint16_t ca = rd16(c, dp(c, V_SLOT_CMD));
    c->x = ca;
    if (!(rd16(c, dp(c, ca)) & 0x0800)) wr16(c, dp(c, ca + 0x40), ca);
    const uint16_t cmd = (uint16_t)((rd16(c, dp(c, ca)) & 0x3FFF) | 0x8000);
    wr16(c, dp(c, ca), cmd);
    push16(c, cmd);                                    /* PHA */
    const uint16_t n = cmd & 0x07FF;
    wr16(c, dp(c, V_SCRATCH + 0), n);
    c->ps &= (uint16_t)~M377_C;
    uint16_t e = adc16(c, (uint16_t)(n << 1), n);      /* ASL ; ADC $C0: 3n */
    c->dt = 0x21;
    e = adc16(c, e, rd16(c, dbank(c, T_SOUND_TABLE_PTR)));
    c->x = e;
    const uint16_t ptr = rd16(c, dbank(c, e));
    c->b = rd16(c, dbank(c, (uint16_t)(e + 2)));
    const uint16_t trk = rd16(c, dp(c, V_TRACK));
    c->x = trk;
    wr16(c, dp(c, trk + TRK_SEQ_PTR), ptr);
    wr16(c, dp(c, trk + TRK_TIME), rd16(c, dp(c, V_TICK_CLOCK)));
    c->ps |= M377_M;
    wr8(c, dp(c, trk + TRK_SEQ_BANK), (uint8_t)c->b);
    c->dt = 0;
    c->ps &= (uint16_t)~M377_M;
    c->a = 0;
    static const uint8_t cleared[] = { 0x18, 0x0E, 0x1A, 0x14, 0x16 };
    for (unsigned i = 0; i < sizeof cleared; i++) wr16(c, dp(c, trk + cleared[i]), 0);
    c->a = pop16(c);                                   /* PLA */
    wr16(c, dp(c, trk + TRK_FLAGS), c->a);
    c->ps |= M377_M;
    c->x = rd16(c, dp(c, V_SLOT_A));
    lda8(c, 0);
    for (unsigned k = 0; k < 8; k++) wr8(c, dp(c, c->x + 0x13 + k * 0x20), 0);
    c->pc = pop16(c);                                  /* RTS */
    return ran(R_NEWCMD);
}

/* =========================================================================
 * STOP A TRACK  (0xE18D -- the sequencer's "stopped", command 0x14 at depth
 * 0, and 0xD982 through the command table's entry 0x15)
 *
 * For the slot 0xD0 (sub-channels from 0xD6): its column of the voice
 * matrix at 0x8800 (4 x 8 bytes a slot) is cleared; each ENABLED
 * sub-channel is disabled, its channel's event queue flushed, and its chip
 * voice (+0x11 = the voice number; 0x2000 + 16v) either released (+0x2E,
 * while the channel still sounds) or stopped -- the voice flags cleared
 * outright, or only their bit 1 for a channel already silent -- and the
 * voice handed back to the allocator (vectors 0x0112, 0x010E, 0x0114). The
 * command word is marked stopped, its step pointer and the track's flags
 * cleared. The original then leaves by PLY ; RTS -- it drops one frame --
 * which the readable entry below keeps.
 * ========================================================================= */
enum { V_STOP_COUNT = 0xC4, V_STOP_SUB = 0xC2, V_VOICE = 0xCC, SC_VOICE = 0x11, SC_ENV_FLAGS = 0x1D,
       VEC_VOICE_FREE = 0x0112, VEC_VOICE_COUNT = 0x010E, VEC_VOICE_DROP = 0x0114, SLOT_MATRIX = 0x8800 };
static int track_stop_core(m37710_t *c)
{
    c->ps |= M377_M;
    const uint8_t slot = rd8(c, dp(c, V_SLOT_INDEX));
    c->ps &= (uint16_t)~M377_M;
    uint16_t col = (uint16_t)(SLOT_MATRIX + slot);
    c->a = 0;
    c->ps |= M377_M;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) wr8(c, dp(c, col + j * 0x20), 0);
        col = (uint16_t)(col + 0x100);
    }
    wr8(c, dp(c, V_SCRATCH + 0), 0);
    c->b = col;

    wr8(c, dp(c, V_STOP_COUNT), 8);
    c->x = rd16(c, dp(c, V_SLOT_BASE));
    for (;;) {                                         /* E1C4 */
        c->ps |= M377_M;
        const uint16_t sub = c->x;
        const uint8_t en = rd8(c, dp(c, sub + SC_ENABLED));
        lda8(c, en);
        uint16_t a_next;
        if (!en) {
            a_next = sub;                              /* E256 */
        } else {
            wr8(c, dp(c, sub + SC_ENABLED), 0);
            const uint8_t envf = rd8(c, dp(c, sub + SC_ENV_FLAGS));
            c->b = (uint16_t)((c->b & 0xFF00) | envf);
            wr16(c, dp(c, V_STOP_SUB), sub);
            const uint8_t vn = rd8(c, dp(c, sub + SC_VOICE));
            c->ps &= (uint16_t)~M377_M;
            wr16(c, dp(c, V_VOICE), vn);
            const uint16_t chn = rd16(c, dp(c, sub + SC_CHANNEL));
            push16(c, sub);                            /* PHX */
            c->x = chn;
            c->ps |= M377_M;
            wr8(c, dp(c, chn + CH_WR), rd8(c, dp(c, chn + CH_RD)));   /* flush the event queue */
            const uint8_t busy = rd8(c, dp(c, chn + CH_BUSY));
            const uint8_t envs = (uint8_t)(envf << 1);  /* ASL B */
            c->b = (uint16_t)((c->b & 0xFF00) | envs);
            const uint16_t voice = (uint16_t)(0x2000 + (vn << 4));
            if (envs) {                                /* stop the voice outright */
                c->ps &= (uint16_t)~M377_M;
                c->x = voice;
                wr16(c, dp(c, voice + VF_FLAGS), 0);
            } else if (busy) {                         /* let it release */
                wr8(c, dp(c, chn + CH_RELEASE), 1);
            } else {                                   /* already silent: clear its bit 1 */
                c->ps &= (uint16_t)~M377_M;
                c->x = voice;
                const uint16_t f = (uint16_t)(rd16(c, dp(c, voice + VF_FLAGS)) & 0xFFFD);
                c->a = f;
                wr16(c, dp(c, voice + VF_FLAGS), f);
            }
            c->x = rd16(c, dp(c, V_STOP_SUB));         /* E218 */
            if (snd_call(c, rd16(c, VEC_VOICE_FREE))) return 1;
            if (snd_call(c, rd16(c, VEC_VOICE_COUNT))) return 1;
            c->ps |= M377_M;
            if ((uint8_t)c->a) {
                if (snd_call(c, rd16(c, VEC_VOICE_DROP))) return 1;
            }
            c->ps &= (uint16_t)~M377_M;
            a_next = pop16(c);                         /* PLA */
        }
        c->ps &= (uint16_t)~(M377_M | M377_C);         /* E233 */
        c->x = adc16(c, a_next, 0x20);
        c->a = c->x;
        c->ps |= M377_M;
        const uint8_t left = (uint8_t)(rd8(c, dp(c, V_STOP_COUNT)) - 1);
        wr8(c, dp(c, V_STOP_COUNT), left);
        set_nz_w(c, left, false);
        if (!left) break;
    }
    c->ps &= (uint16_t)~M377_M;                        /* E23D */
    const uint16_t ca = rd16(c, dp(c, V_SLOT_CMD));
    c->x = ca;
    const uint16_t w = (uint16_t)(rd16(c, dp(c, ca)) & 0x7FFF);
    c->a = w;
    wr16(c, dp(c, ca), w);
    wr16(c, dp(c, ca + 0x40), 0);
    c->x = rd16(c, dp(c, V_TRACK));
    wr16(c, dp(c, c->x + TRK_FLAGS), 0);
    return 0;
}
SND_ENTRY(0xE18D, snd_track_stop, 0)
int snd_track_stop(m37710_t *c)
{
    if (track_stop_core(c)) return 1;
    c->y = pop16(c);                                   /* PLY: the frame the original drops */
    c->dt = 0;
    c->pc = pop16(c);                                  /* RTS */
    return ran(R_STOP_TRACK);
}

/* =========================================================================
 * STOP A SLOT AND WHAT IT STARTED  (0xD982, through the vector at 0x0116)
 *
 * Every slot whose step pointer (+0x40 of its command word) is THIS slot's
 * command word -- the slot itself, and the tracks it started with command
 * 0x0D -- and which is running or newly commanded, is stopped (0xE18D, via
 * the command table's entry 0x15, with a pushed X that its PLY drops). The
 * scan walks all 32 slots with 0xD0/0xD6/0xE0 set as the stop expects, and
 * restores them after.
 * ========================================================================= */
SND_ENTRY(0xD982, snd_stop_slot, 0)
int snd_stop_slot(m37710_t *c)
{
    if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xD982, R_STOP_SLOT);
    const uint16_t me = rd16(c, dp(c, V_SLOT_CMD));
    wr16(c, dp(c, 0xC6), me);
    push16(c, rd16(c, dp(c, V_SLOT_A)));
    push16(c, rd16(c, dp(c, V_SLOT_INDEX)));
    wr16(c, dp(c, V_SLOT_INDEX), 0);
    wr16(c, dp(c, V_SLOT_A), 0x5000);
    wr16(c, dp(c, V_SLOT_CMD), SH_CMD_WORDS);
    for (;;) {                                         /* D999 */
        c->ps &= (uint16_t)~M377_M;
        const uint16_t ca = rd16(c, dp(c, V_SLOT_CMD));
        c->x = ca;
        const uint16_t w = rd16(c, dp(c, ca));
        c->a = w;
        if ((w & 0x8000) || (w & 0x4000)) {
            const uint16_t sp = rd16(c, dp(c, ca + 0x40));
            cmp_flags(c, sp, rd16(c, dp(c, 0xC6)), true);
            c->a = sp;
            if (sp == rd16(c, dp(c, 0xC6))) {
                if (track_stop_core(c)) return 1;      /* PEA ; PHX ; JMP (0x015A) -- its PLY drops the X */
                c->y = ca;
                c->dt = 0;
            }
        }
        c->ps |= M377_M;                               /* D9AF */
        wr8(c, dp(c, V_SLOT_CMD), (uint8_t)(rd8(c, dp(c, V_SLOT_CMD)) + 2));
        wr8(c, dp(c, V_SLOT_INDEX), (uint8_t)(rd8(c, dp(c, V_SLOT_INDEX)) + 1));
        c->ps &= (uint16_t)~(M377_M | M377_C);
        const uint16_t a2 = adc16(c, rd16(c, dp(c, V_SLOT_A)), 0x0100);
        wr16(c, dp(c, V_SLOT_A), a2);
        cmp_flags(c, a2, 0x7000, true);
        c->a = a2;
        if (a2 == 0x7000) break;
    }
    wr16(c, dp(c, V_SLOT_INDEX), pop16(c));
    wr16(c, dp(c, V_SLOT_A), pop16(c));
    const uint16_t me2 = rd16(c, dp(c, 0xC6));
    c->a = me2;
    wr16(c, dp(c, V_SLOT_CMD), me2);
    c->pc = pop16(c);
    return ran(R_STOP_SLOT);
}

/* =========================================================================
 * THE VOICE ALLOCATOR
 *
 * 32 chip voices, 32 slots. Each voice has ONE owner, a sub-channel record
 * (0x4F00 + 2v; 0 = free). A sub-channel that wants a voice it cannot have
 * leaves a REQUEST in the matrix at 0x8800 + 32v + slot (its priority,
 * 0xC2) and 0x8C00 + 32v + slot (which of the slot's sub-channels). When a
 * voice is freed, the highest request takes it and its sub-channel is
 * re-enabled. 0xCC = the voice in question, 0xCA its owner entry.
 * ========================================================================= */
enum { OWNERS = 0x4F00, V_OWNER_ENTRY = 0xCA, V_BEST = 0xDE, V_PRIORITY = 0xC2, V_SUBIDX = 0xC3 };
static int alloc_done(m37710_t *c) { c->pc = pop16(c); return ran(R_ALLOC); }

/* 0xD8F9 (vector 0x0112): free voice 0xCC; X = its matrix row. */
SND_ENTRY(0xD8F9, snd_voice_free, 0)
int snd_voice_free(m37710_t *c)
{
    c->ps &= (uint16_t)~(M377_M | M377_C);
    const uint16_t v = rd16(c, dp(c, V_VOICE));
    const uint16_t oe = adc16(c, (uint16_t)(v << 1), OWNERS);
    wr16(c, dp(c, V_OWNER_ENTRY), oe);
    wr16(c, dp(c, oe), 0);
    const uint16_t row = adc16(c, (uint16_t)(rd16(c, dp(c, V_VOICE)) << 5), SLOT_MATRIX);
    c->a = row; c->x = row;
    return alloc_done(c);
}
/* 0xD912 (vector 0x010E): the highest request in row X -> A (0: none),
 * B = its slot, 0xDE = its address. Ties go to the first. */
SND_ENTRY(0xD912, snd_voice_best, 0)
int snd_voice_best(m37710_t *c)
{
    c->ps |= M377_M;
    uint8_t n = rd8(c, 0x4482);
    wr8(c, dp(c, V_SCRATCH + 0), n);
    uint8_t best = 0, best_i = 0, i = 0;
    uint16_t x = c->x;
    do {
        const uint8_t r = rd8(c, dp(c, x));
        if (r > best) { best = r; best_i = i; wr16(c, dp(c, V_BEST), x); }
        x = (uint16_t)(x + 1); i++;
        n = (uint8_t)(n - 1);
        wr8(c, dp(c, V_SCRATCH + 0), n);
    } while (n);
    c->x = x;
    c->a = (uint16_t)((c->a & 0xFF00) | best);
    c->b = (uint16_t)((c->b & 0xFF00) | best_i);
    set_nz_w(c, best_i, false);
    return alloc_done(c);
}
/* 0xD936 (vector 0x0114): give voice 0xCA's entry to the request at 0xDE --
 * slot (low 5 bits) x 8 + its sub-channel index (0x8C00 side) names the
 * sub-channel 0x5000 + 32(...), which becomes the owner and is re-enabled. */
SND_ENTRY(0xD936, snd_voice_handoff, 0)
int snd_voice_handoff(m37710_t *c)
{
    c->ps &= (uint16_t)~(M377_M | M377_C);
    const uint16_t hi = adc16(c, rd16(c, dp(c, V_BEST)), 0x0400);
    c->x = hi;
    c->ps |= M377_M;
    const uint8_t k = adc8(c, (uint8_t)((hi & 0x1F) << 3), rd8(c, dp(c, hi)));
    c->ps &= (uint16_t)~M377_M;
    const uint16_t sub = adc16(c, (uint16_t)(k << 5), 0x5000);
    c->x = rd16(c, dp(c, V_OWNER_ENTRY));
    wr16(c, dp(c, c->x), sub);
    c->x = sub; c->a = sub;
    c->ps |= M377_M;
    wr8(c, dp(c, sub + SC_ENABLED), 0xFF);
    return alloc_done(c);
}
/* 0xD8A0 (vector 0x0110): X = a sub-channel wanting voice +0x11: leave its
 * request (priority 0xC2, index) in the matrix; X = that voice's row. */
SND_ENTRY(0xD8A0, snd_voice_request, 0)
int snd_voice_request(m37710_t *c)
{
    if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xD8A0, R_ALLOC);
    c->ps |= M377_M;
    const uint16_t sub = c->x;
    const uint8_t subidx = (uint8_t)(((sub & 0xE0) << 3 | (sub & 0xE0) >> 5) & 0xFF);   /* AND #$E0 ; RLA #3 (8-bit) */
    wr8(c, dp(c, V_SUBIDX), subidx);
    const uint8_t v = rd8(c, dp(c, sub + SC_VOICE));
    c->ps &= (uint16_t)~M377_M;
    const uint16_t row = adc16(c, (uint16_t)(v << 5), SLOT_MATRIX);   /* carry as the caller left it */
    push16(c, row);
    c->ps |= M377_M;
    const uint8_t lo = adc8(c, (uint8_t)row, rd8(c, dp(c, V_SLOT_INDEX)));
    const uint16_t cell = (uint16_t)((row & 0xFF00) | lo);
    c->b = (uint16_t)((c->b & 0xFF00) | rd8(c, dp(c, V_PRIORITY)));
    wr8(c, dp(c, cell), (uint8_t)c->b);
    c->ps &= (uint16_t)~M377_M;
    const uint16_t cell2 = adc16(c, cell, 0x0400);
    c->ps |= M377_M;
    lda8(c, subidx);
    wr8(c, dp(c, cell2), subidx);
    c->x = pop16(c);
    return alloc_done(c);
}
/* 0xD8CF (vector 0x0108): X = a sub-channel taking its voice: enable it and
 * make it the owner, disabling a different previous owner. */
SND_ENTRY(0xD8CF, snd_voice_claim, 0)
int snd_voice_claim(m37710_t *c)
{
    if (!(c->ps & M377_M)) return bail(c, 0xD8CF, R_ALLOC);
    const uint16_t sub = c->x;
    wr8(c, dp(c, sub + SC_ENABLED), 0xFF);
    const uint8_t v = rd8(c, dp(c, sub + SC_VOICE));
    c->ps &= (uint16_t)~(M377_M | M377_C);
    const uint16_t oe = adc16(c, (uint16_t)(v << 1), OWNERS);
    wr16(c, dp(c, V_OWNER_ENTRY), sub);
    const uint16_t owner = rd16(c, dp(c, oe));
    c->b = owner;
    if (owner && owner == sub) { c->x = sub; return alloc_done(c); }
    if (owner) {
        c->ps |= M377_M;
        wr8(c, dp(c, owner + SC_ENABLED), 0);
        c->ps &= (uint16_t)~M377_M;
    }
    c->a = sub;
    wr16(c, dp(c, oe), sub);
    c->x = sub;
    return alloc_done(c);
}
/* 0xD95B (vector 0x0128): reset sub-channel X (its own sound, keymap off). */
SND_ENTRY(0xD95B, snd_subch_reset, 0)
int snd_subch_reset(m37710_t *c)
{
    if (!(c->ps & M377_M)) return bail(c, 0xD95B, R_ALLOC);
    const uint16_t x = c->x;
    wr8(c, dp(c, x + SC_KEYMAP), 0);
    c->ps &= (uint16_t)~M377_M;
    static const uint8_t z[] = { 0x00, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x12, 0x14, 0x16, 0x18, 0x1C, 0x1E };
    for (unsigned i = 0; i < sizeof z; i++) wr16(c, dp(c, x + z[i]), 0);
    c->a = x;
    wr16(c, dp(c, x + SC_SOUND), x);
    return alloc_done(c);
}

/* =========================================================================
 * THE VOLUME ENVELOPE  (a channel's fx1 hook: 0xEF48 up, 0xEF5C down,
 * 0xEF6D sustain; 0xEF9D started at key-on)
 *
 * Level = +0x1E (16-bit; +0x1F, its high byte, is the level proper), moving
 * by the RATE +0x18 toward the segment's TARGET +0x1A. A segment ends when
 * the target is reached or passed (or the 16-bit sum wraps); the level is
 * then set to the target exactly, the fraction to 0x80, and the next is read
 * from the SCRIPT (bank 0x21, pointer +0x20):
 *   rate code, target   the next segment -- up or down by comparison; the
 *                       rate is the table at 0xF206 (a target equal to the
 *                       level is passed over at once)
 *   0xFE                SUSTAIN: wait for the release counter +0x2E, which
 *                       counts down once armed; at 0 the script is scanned
 *                       past its next 0xFE and continues (the release)
 *   0xFF                OFF: the chip voice's flags cleared, the channel
 *                       marked silent (0xEFAE)
 *   0x00                a script jump -- never used in a gated scenario: bailed
 * ========================================================================= */
enum { CH_ENV_RATE = 0x18, CH_ENV_TARGET = 0x1A, CH_ENV_PTR = 0x20, V_RATE_CODE = 0xC6, T_ENV_RATES = 0xF206,
       HOOK_ENV_UP = 0xEF48, HOOK_ENV_DOWN = 0xEF5C, HOOK_ENV_SUSTAIN = 0xEF6D };
static int env_done(m37710_t *c) { c->pc = pop16(c); return ran(R_ENV); }
/* 0xEFAE -- the channel's voice off, the channel silent */
static int env_off(m37710_t *c)
{
    c->ps &= (uint16_t)~M377_M;
    wr16(c, dp(c, rd16(c, dp(c, V_VOICE_REGS)) + VF_FLAGS), 0);
    c->ps |= M377_M;
    wr8(c, dp(c, c->x + CH_ACTIVE), 0);
    return env_done(c);
}
/* 0xEF9D -- read segments from the script at Y (bank 0x21) */
static int env_segments(m37710_t *c)
{
    for (;;) {
        c->ps |= M377_M;
        const uint8_t b = rd8(c, abs_y(c, 0));
        lda8(c, b);
        if (b == 0x00) return bail(c, 0xEFBC, R_ENV);
        cmp8(c, b, 0xFE);
        if (b == 0xFF) return env_off(c);
        if (b == 0xFE) {                               /* sustain */
            c->ps &= (uint16_t)~M377_M;
            wr16(c, dp(c, c->x + CH_HOOK_FX1), HOOK_ENV_SUSTAIN);
            return env_done(c);
        }
        c->y = (uint16_t)(c->y + 1);                   /* EFD2 */
        wr8(c, dp(c, V_RATE_CODE), b);
        const uint8_t target = rd8(c, abs_y(c, 0));
        c->y = (uint16_t)(c->y + 1);
        wr16(c, dp(c, c->x + CH_ENV_PTR), c->y);
        wr8(c, dp(c, c->x + CH_ENV_TARGET), target);
        const uint8_t level = rd8(c, dp(c, c->x + CH_ENV_LEVEL));
        cmp8(c, target, level);
        lda8(c, target);
        if (target == level) continue;                 /* passed over at once */
        c->ps &= (uint16_t)~M377_M;
        wr16(c, dp(c, c->x + CH_HOOK_FX1), target > level ? HOOK_ENV_UP : HOOK_ENV_DOWN);
        c->ps |= M377_M;
        const uint8_t code = rd8(c, dp(c, V_RATE_CODE));
        c->ps &= (uint16_t)~(M377_M | M377_C);
        const uint16_t rate = rd16(c, dp(c, (uint16_t)(T_ENV_RATES + code * 2)));   /* 0xEFF8 */
        c->a = rate;
        wr16(c, dp(c, c->x + CH_ENV_RATE), rate);
        return env_done(c);
    }
}
/* 0xEF91 / 0xEF96 -- a segment finished: level = target, next segment */
static int env_next(m37710_t *c, bool reload_ptr)
{
    c->ps |= M377_M;
    if (reload_ptr) { c->dt = 0x21; c->y = rd16(c, dp(c, c->x + CH_ENV_PTR)); }
    const uint8_t t = rd8(c, dp(c, c->x + CH_ENV_TARGET));
    wr8(c, dp(c, c->x + CH_ENV_LEVEL), t);
    wr8(c, dp(c, c->x + CH_ENV_POS), 0x80);
    return env_segments(c);
}
/* 0xEF6D -- sustain: wait for the armed release counter to run out */
static int env_sustain(m37710_t *c)
{
    c->ps |= M377_M;
    const uint8_t r = rd8(c, dp(c, c->x + CH_RELEASE));
    lda8(c, r);
    if (!r) return env_done(c);
    const uint8_t r2 = (uint8_t)(r - 1);
    wr8(c, dp(c, c->x + CH_RELEASE), r2);
    set_nz_w(c, r2, false);
    if (r2) return env_done(c);
    c->dt = 0x21;                                      /* the release: scan past the next 0xFE */
    c->y = rd16(c, dp(c, c->x + CH_ENV_PTR));
    for (;;) {
        const uint8_t b = rd8(c, abs_y(c, 0));
        lda8(c, b);
        if (b == 0x00) return bail(c, 0xEF8C, R_ENV);
        c->y = (uint16_t)(c->y + 1);
        if (b == 0xFF) return env_off(c);
        if (b == 0xFE) return env_next(c, false);
        c->y = (uint16_t)(c->y + 1);
    }
}
SND_ENTRY(0xEF48, snd_env_up, 0)
int snd_env_up(m37710_t *c)
{
    if (!(c->ps & M377_M)) return bail(c, 0xEF48, R_ENV);
    c->ps &= (uint16_t)~(M377_M | M377_C);
    const uint16_t pos = adc16(c, rd16(c, dp(c, c->x + CH_ENV_POS)), rd16(c, dp(c, c->x + CH_ENV_RATE)));
    wr16(c, dp(c, c->x + CH_ENV_POS), pos);
    c->ps |= M377_M;
    if (c->ps & M377_C) return env_next(c, true);      /* wrapped */
    const uint8_t lv = rd8(c, dp(c, c->x + CH_ENV_LEVEL)), tg = rd8(c, dp(c, c->x + CH_ENV_TARGET));
    cmp8(c, lv, tg);
    if (lv >= tg) return env_next(c, true);
    return env_sustain(c);
}
SND_ENTRY(0xEF5C, snd_env_down, 0)
int snd_env_down(m37710_t *c)
{
    if (!(c->ps & M377_M)) return bail(c, 0xEF5C, R_ENV);
    c->ps &= (uint16_t)~M377_M;
    const uint16_t a = rd16(c, dp(c, c->x + CH_ENV_POS)), r = rd16(c, dp(c, c->x + CH_ENV_RATE));
    const uint16_t pos = (uint16_t)(a - r);
    wr16(c, dp(c, c->x + CH_ENV_POS), pos);
    c->ps |= M377_M;
    if (a < r) return env_next(c, true);               /* borrowed */
    const uint8_t tg = rd8(c, dp(c, c->x + CH_ENV_TARGET)), lv = rd8(c, dp(c, c->x + CH_ENV_LEVEL));
    cmp8(c, tg, lv);
    if (tg >= lv) return env_next(c, true);
    return env_sustain(c);
}
SND_ENTRY(0xEF6D, snd_env_sustain, 0)
int snd_env_sustain(m37710_t *c) { return env_sustain(c); }
SND_ENTRY(0xEF9D, snd_env_start, 0)
int snd_env_start(m37710_t *c) { if (!(c->ps & M377_M)) return bail(c, 0xEF9D, R_ENV); return env_segments(c); }
SND_ENTRY(0xEFAE, snd_env_off, 0)
int snd_env_off(m37710_t *c) { return env_off(c); }

/* =========================================================================
 * SAMPLE CHANGE  (0xF013, A = the sample number; 0xF064 re-applies the
 * channel's current sample)
 *
 * Sample n's definition is the word n of the table whose address is at
 * 0x210002: +0 the pitch offset (to +0x46), +2 the chip bank (+0x14), +4 the
 * voice flags (+0x2C; bit 5 = a modulated sample, +0x32), +6 the start
 * (plus the channel's +0x0A offset), +8 the end, +0xA the loop -- written to
 * the voice's registers directly. +0x10 keeps the definition, +0x12 the
 * address after its first word.
 * ========================================================================= */
enum { CH_SAMPLE_DEF = 0x10, CH_SAMPLE_REST = 0x12, CH_PITCH_OFS = 0x46, T_SAMPLE_TABLE_PTR = 0x0002 };
static int sample_apply(m37710_t *c)                    /* 0xF035: Y = the definition, B = its flags */
{
    c->ps &= (uint16_t)~M377_M;
    const uint16_t x = c->x;
    const uint16_t ofs = (uint16_t)(rd16(c, dp(c, x + 0x0A)) & 0x00FF);
    push16(c, x);
    c->ps &= (uint16_t)~M377_C;
    const uint16_t voice = rd16(c, dp(c, V_VOICE_REGS));
    c->x = voice;
    const uint16_t start = adc16(c, ofs, rd16(c, abs_y(c, 6)));
    wr16(c, dp(c, voice + 0x0A), start);
    wr16(c, dp(c, voice + 0x0C), rd16(c, abs_y(c, 8)));
    const uint16_t loop = rd16(c, abs_y(c, 0x0A));
    c->a = loop;
    wr16(c, dp(c, voice + 0x0E), loop);
    c->x = pop16(c);
    c->y = (uint16_t)(c->y + 2);
    wr16(c, dp(c, c->x + CH_SAMPLE_REST), c->y);
    c->ps |= M377_M;
    c->dt = 0;
    const uint8_t mod = (c->b & 0x20) ? 0xFF : 0x00;
    c->b = (uint16_t)((c->b & 0xFF00) | mod);
    wr8(c, dp(c, c->x + CH_MOD), mod);
    c->y = rd16(c, dp(c, c->x + CH_SOUND));
    c->pc = pop16(c);
    return ran(R_SAMPLE);
}
SND_ENTRY(0xF013, snd_sample_change, 0)
int snd_sample_change(m37710_t *c)
{
    if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xF013, R_SAMPLE);
    c->dt = 0x21;
    const uint16_t n = c->a;
    wr16(c, dp(c, c->x + CH_SAMPLE), n);
    c->ps &= (uint16_t)~M377_C;
    const uint16_t e = adc16(c, (uint16_t)(n << 1), rd16(c, dbank(c, T_SAMPLE_TABLE_PTR)));
    c->y = e;
    const uint16_t def = rd16(c, abs_y(c, 0));
    c->y = def;
    wr16(c, dp(c, c->x + CH_SAMPLE_DEF), def);
    wr16(c, dp(c, c->x + CH_PITCH_OFS), rd16(c, abs_y(c, 0)));
    wr16(c, dp(c, c->x + CH_BANK), rd16(c, abs_y(c, 2)));
    c->b = rd16(c, abs_y(c, 4));
    wr16(c, dp(c, c->x + CH_VFLAGS), c->b);
    return sample_apply(c);
}
SND_ENTRY(0xF064, snd_sample_reapply, 0)
int snd_sample_reapply(m37710_t *c)
{
    c->ps &= (uint16_t)~M377_M;
    c->dt = 0x21;
    c->y = rd16(c, dp(c, c->x + CH_SAMPLE_DEF));
    c->b = rd16(c, dp(c, c->x + CH_VFLAGS));
    return sample_apply(c);
}

/* =========================================================================
 * SAMPLE STREAMING  (0xEB29, the busy-voice hook, vector 0x012A)
 *
 * A "modulated" sample (+0x32, from the definition's flag bit 5) is a chain
 * of SEGMENTS, 8 bytes each after the definition (+0x12 = the current one).
 * First call (+0x32 = 0xFF): the voice gets the first segment's end (+0xC),
 * start (+8) and loop (+0xA's word); +0x32 becomes 0x7F. Later calls wait
 * for the chip's loop-reached flag (bit 11 of the voice flags), then load the
 * next segment: with its flag bit 5 clear, the voice flags are its word +2
 * and it repeats end/loop in place; with it set, the chain moves on 8 bytes
 * and the voice flags take their low bit from the next segment's.
 * ========================================================================= */
SND_ENTRY(0xEB29, snd_sample_stream, 0)
int snd_sample_stream(m37710_t *c)
{
    if (((c->ps >> 4) & 3u) != 0) return bail(c, 0xEB29, R_STREAM);
    c->ps |= M377_M;
    const uint8_t mode = rd8(c, dp(c, c->x + CH_MOD));
    lda8(c, mode);
    const uint16_t ch = c->x;
    if (mode & 0x80) {                                 /* first call */
        wr8(c, dp(c, ch + CH_MOD), 0x7F);
        push16(c, ch);
        c->ps &= (uint16_t)~M377_M;
        c->y = rd16(c, dp(c, ch + CH_SAMPLE_REST));
        const uint16_t voice = rd16(c, dp(c, V_VOICE_REGS));
        c->x = voice;
        wr16(c, dp(c, voice + 0x0E), rd16(c, abs_y(c, 0x0C)));
        wr16(c, dp(c, voice + 0x0A), rd16(c, abs_y(c, 0x08)));
        const uint16_t w = rd16(c, abs_y(c, 0x0A));
        c->a = (uint16_t)(w >> 1);
        if (w & 1) return bail(c, 0xEB47, R_STREAM);
        c->dt = 0;
        c->ps &= (uint16_t)~M377_C;
        const uint16_t nxt = adc16(c, c->y, 8);
        c->a = nxt;
        c->x = pop16(c);
        wr16(c, dp(c, c->x + CH_SAMPLE_REST), nxt);
        c->pc = pop16(c);
        return ran(R_STREAM);
    }
    c->ps &= (uint16_t)~M377_M;                        /* EB55 */
    c->y = rd16(c, dp(c, ch + CH_SAMPLE_REST));
    push16(c, ch);
    const uint16_t voice = rd16(c, dp(c, V_VOICE_REGS));
    c->x = voice;
    const uint16_t vf = rd16(c, dp(c, voice + VF_FLAGS));
    c->a = (uint16_t)(vf & 0x0800);
    if (!(vf & 0x0800)) { c->x = pop16(c); c->dt = 0; c->pc = pop16(c); return ran(R_STREAM); }
    wr16(c, dp(c, voice + 0x0C), rd16(c, abs_y(c, 6)));
    const uint16_t f2 = rd16(c, abs_y(c, 2));
    c->a = f2;
    wr16(c, dp(c, V_SCRATCH + 0), f2);
    if (!(f2 & 0x20)) {
        wr16(c, dp(c, voice + VF_FLAGS), f2);
        wr16(c, dp(c, voice + 0x0C), rd16(c, abs_y(c, 6)));
        wr16(c, dp(c, voice + 0x0E), rd16(c, abs_y(c, 8)));
        c->x = pop16(c); c->dt = 0; c->pc = pop16(c);
        return ran(R_STREAM);
    }
    c->b = rd16(c, abs_y(c, 8));                       /* EB80 */
    if (c->b & 0x8000) return bail(c, 0xEB86, R_STREAM);
    c->y = (uint16_t)(c->y + 8);
    c->b = rd16(c, abs_y(c, 2));
    const uint16_t nf = (uint16_t)(((f2 >> 1) << 1) | (c->b & 1));   /* LSR ; LSRB ; ROL */
    c->b = (uint16_t)(c->b >> 1);
    c->a = nf;
    wr16(c, dp(c, voice + VF_FLAGS), nf);
    wr16(c, dp(c, voice + 0x0E), rd16(c, abs_y(c, 4)));
    wr16(c, dp(c, voice + 0x0A), rd16(c, abs_y(c, 0)));
    c->x = pop16(c);
    wr16(c, dp(c, c->x + CH_SAMPLE_REST), c->y);
    c->dt = 0;
    c->pc = pop16(c);
    return ran(R_STREAM);
}

/* =========================================================================
 * FIXED PAN  (0xE7FE, vector 0x012E): A = a signed left/right position, B =
 * a front/rear amount. The four speaker attenuations 0xC0..0xC3 (front L,
 * front R, rear L, rear R): the far side of left/right gets |A| x 4, then
 * the rear pair gets B x 4 on top, saturating at 0xFF. Negative B and the
 * saturation never happen in a gated scenario: bailed.
 * ========================================================================= */
SND_ENTRY(0xE7FE, snd_fixed_pan, 0)
int snd_fixed_pan(m37710_t *c)
{
    c->ps &= (uint16_t)~M377_M;
    c->dt = 0;
    wr16(c, dp(c, V_SCRATCH + 0), 0);
    wr16(c, dp(c, V_SCRATCH + 2), 0);
    c->ps |= M377_M;
    const uint8_t a = (uint8_t)c->a;
    if (a & 0x80) {
        const uint8_t v = (uint8_t)((uint8_t)~a << 2);
        wr8(c, dp(c, V_SCRATCH + 0), v); wr8(c, dp(c, V_SCRATCH + 2), v);
        c->a = (uint16_t)((c->a & 0xFF00) | v);
    } else {
        const uint8_t v = (uint8_t)(a << 2);
        wr8(c, dp(c, V_SCRATCH + 1), v); wr8(c, dp(c, V_SCRATCH + 3), v);
        c->a = (uint16_t)((c->a & 0xFF00) | v);
    }
    { const uint16_t t = c->a; c->a = c->b; c->b = t; }   /* XAB */
    const uint8_t r = (uint8_t)c->a;
    set_nz_w(c, r, false);
    if (r & 0x80) return bail(c, 0xE83A, R_FIXPAN);
    const uint8_t r4 = (uint8_t)(r << 2);
    c->ps &= (uint16_t)~M377_C;
    if (r & 0x40) c->ps |= M377_C;                     /* the second ASL's carry */
    push8(c, r4);
    const uint8_t s3 = adc8(c, r4, rd8(c, dp(c, V_SCRATCH + 3)));
    if (c->ps & M377_C) { c->a = (uint16_t)((c->a & 0xFF00) | s3); return bail(c, 0xE82B, R_FIXPAN); }
    wr8(c, dp(c, V_SCRATCH + 3), s3);
    (void)pop8(c);
    c->ps &= (uint16_t)~M377_C;
    const uint8_t s2 = adc8(c, r4, rd8(c, dp(c, V_SCRATCH + 2)));
    if (c->ps & M377_C) { c->a = (uint16_t)((c->a & 0xFF00) | s2); return bail(c, 0xE835, R_FIXPAN); }
    c->a = (uint16_t)((c->a & 0xFF00) | s2);
    wr8(c, dp(c, V_SCRATCH + 2), s2);
    c->pc = pop16(c);
    return ran(R_FIXPAN);
}

/* =========================================================================
 * THE PAN SCRIPT  (volume hooks for pan modes 0x7A/0x7E: 0xECD4 to start,
 * then 0xED7B / 0xED8B while a sweep runs)
 *
 * The channel's pan moves along a script (bank 0x21; +0x6A the pointer,
 * +0x3E the loop point): a byte below 0x80 or above 0x84 is a SWEEP -- its
 * magnitude a rate (the table at 0xF206), its sign the direction -- followed
 * by the target; 0x84 loops back to +0x3E. The position +0x66 (16-bit; +0x67
 * its high byte, the distance still to go) runs by the rate +0x62; the pan
 * the voice gets is target (+0x6C) - distance, through the pan law (vector
 * 0x012C) and the volume writer (0xE987, jumped to). The other markers never
 * run in a gated scenario and bail.
 * ========================================================================= */
enum { PS_HOLD = 0x31, PS_DIST = 0x67, PS_POS = 0x66, PS_RATE = 0x62, PS_TARGET = 0x6C, PS_PTR = 0x6A,
       PS_LOOP = 0x3E, HOOK_PS_DOWN = 0xED7B, HOOK_PS_UP = 0xED8B };
static int pan_to_voice(m37710_t *c, uint8_t pan)          /* ECCA */
{
    c->a = (uint16_t)((c->a & 0xFF00) | pan);
    if (snd_call(c, rd16(c, VEC_PAN_LAW))) return 1;
    c->ps &= (uint16_t)~M377_M;
    c->pc = ROUTINE_VOLUME_FROM_SCRATCH;
    return ran(R_PANSCRIPT);
}
static int pan_current(m37710_t *c)                        /* ED9A */
{
    c->ps |= M377_M;
    const uint8_t tg = rd8(c, dp(c, c->x + PS_TARGET)), dist = rd8(c, dp(c, c->x + PS_DIST));
    c->ps |= M377_C;
    const uint8_t pan = (uint8_t)(tg - dist);
    return pan_to_voice(c, pan);
}
static int pan_segment(m37710_t *c)                        /* ECEC */
{
    for (;;) {
        c->ps |= M377_M;
        c->y = rd16(c, dp(c, c->x + PS_PTR));
        c->dt = 0x21;
        const uint8_t b = rd8(c, abs_y(c, 0));
        c->y = (uint16_t)(c->y + 1);
        lda8(c, b);
        if (b == 0x80) return bail(c, 0xED3F, R_PANSCRIPT);
        if (b == 0x81) { cmp8(c, b, 0x82); return bail(c, 0xED0B, R_PANSCRIPT); }
        if (b == 0x82) return bail(c, 0xED31, R_PANSCRIPT);
        if (b == 0x83) { cmp8(c, b, 0x84); return bail(c, 0xED12, R_PANSCRIPT); }
        if (b == 0x84) {                               /* loop back */
            wr8(c, dp(c, c->x + PS_HOLD), 0);
            c->y = (uint16_t)(rd16(c, dp(c, c->x + PS_LOOP)) - 1);
            const uint8_t d = rd8(c, abs_y(c, 0));      /* ED1D */
            c->y = (uint16_t)(c->y + 1);
            wr8(c, dp(c, c->x + PS_DIST), d);
            wr8(c, dp(c, c->x + PS_TARGET), d);
            wr16(c, dp(c, c->x + PS_LOOP), c->y);
            wr16(c, dp(c, c->x + PS_PTR), c->y);
            c->ps &= (uint16_t)~M377_M;
            wr16(c, dp(c, c->x + 0x00), 0xEDA3);        /* as the original writes it */
            c->ps |= M377_M;                            /* EDA3 */
            const uint8_t hold = rd8(c, dp(c, c->x + PS_HOLD));
            lda8(c, hold);
            if (hold) return bail(c, 0xEDA8, R_PANSCRIPT);
            continue;
        }
        /* a sweep: ED48 */
        const bool neg = b & 0x80;
        const uint8_t code = neg ? (uint8_t)(~b + 1) : b;
        c->ps &= (uint16_t)~(M377_M | M377_C);
        const uint16_t rate = rd16(c, dp(c, (uint16_t)(T_ENV_RATES + code * 2)));   /* 0xEFF8 */
        wr16(c, dp(c, c->x + CH_HOOK_VOLUME), neg ? HOOK_PS_UP : HOOK_PS_DOWN);
        c->a = rate;
        wr16(c, dp(c, c->x + PS_RATE), rate);           /* ED60 */
        c->ps |= M377_M;
        const uint8_t target = rd8(c, abs_y(c, 0));
        c->y = (uint16_t)(c->y + 1);
        wr16(c, dp(c, c->x + PS_PTR), c->y);
        c->dt = 0;
        const uint8_t cur = rd8(c, dp(c, c->x + PS_TARGET));
        wr8(c, dp(c, c->x + PS_DIST), (uint8_t)(target - cur));
        wr8(c, dp(c, c->x + PS_POS), 0);
        wr8(c, dp(c, c->x + PS_TARGET), target);
        return pan_current(c);
    }
}
SND_ENTRY(0xECD4, snd_pan_script_start, 0)
int snd_pan_script_start(m37710_t *c)
{
    if (!(c->ps & M377_M)) return bail(c, 0xECD4, R_PANSCRIPT);
    const uint8_t h = (uint8_t)(rd8(c, dp(c, c->x + PS_HOLD)) - 1);
    wr8(c, dp(c, c->x + PS_HOLD), h);
    set_nz_w(c, h, false);
    if (h) return bail(c, 0xECD9, R_PANSCRIPT);
    c->y = rd16(c, dp(c, c->x + PS_PTR));
    c->dt = 0x21;
    const uint8_t d = rd8(c, abs_y(c, 0));
    c->y = (uint16_t)(c->y + 1);
    wr8(c, dp(c, c->x + PS_DIST), d);
    wr8(c, dp(c, c->x + PS_POS), 0x80);
    return pan_segment(c);
}
SND_ENTRY(0xED7B, snd_pan_sweep_down, 0)
int snd_pan_sweep_down(m37710_t *c)
{
    c->ps &= (uint16_t)~M377_M;
    const uint16_t p = rd16(c, dp(c, c->x + PS_POS)), r = rd16(c, dp(c, c->x + PS_RATE));
    if (p < r) return pan_segment(c);
    wr16(c, dp(c, c->x + PS_POS), (uint16_t)(p - r));
    return pan_current(c);
}
SND_ENTRY(0xED8B, snd_pan_sweep_up, 0)
int snd_pan_sweep_up(m37710_t *c)
{
    c->ps &= (uint16_t)~M377_M;
    const uint16_t p = rd16(c, dp(c, c->x + PS_POS));
    if (!p) return pan_segment(c);
    const uint32_t q = (uint32_t)p + rd16(c, dp(c, c->x + PS_RATE));
    if (q > 0xFFFF) return pan_segment(c);
    wr16(c, dp(c, c->x + PS_POS), (uint16_t)q);
    return pan_current(c);
}
