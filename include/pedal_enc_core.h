/* pedal_enc_core.h -- the pure logic behind an EXTERNAL pedal (an exercise
 * bike's crank encoder, a Peloton's cadence, a DIY flywheel sensor).
 *
 * No SDL, no OS, no clock: every function takes the time it is given, so the
 * whole thing runs under tools/pedal_enc_test.c with synthetic streams.
 *
 * The game only ever sees "how hard is the rider pedalling" as the 0..127
 * level src/input.c already turns into MCU pulses (input_pedal_step). This
 * file's job is to turn a REAL cadence into that level's input, in RPM:
 *
 *   quadrature A/B edges -> penc_qdec  -> +-1 counts \
 *   a wrapping counter axis            -> +-N counts  >-> penc_cad -> RPM
 *   a serial line of counts            -> +-N counts /
 *   a serial line of RPM / a Peloton   -> penc_cad_set_rpm ----------> RPM
 *
 * The estimator is TIME-based (counts per elapsed millisecond), never counts
 * per frame. That is deliberate: the pause bug in register row 145 was a
 * counter that kept accumulating while nothing consumed it and then arrived
 * as one frame's worth. A rate measured over wall-clock time has no such
 * backlog to deliver.
 */
#ifndef PEDAL_ENC_CORE_H
#define PEDAL_ENC_CORE_H

#include <stdint.h>

/* ---- quadrature decoder ----------------------------------------------- */
/* Four decoded counts per full A/B cycle. A transition that skips a state
 * (a bounce, a missed edge) decodes as 0 rather than guessing a direction. */
typedef struct { uint8_t s; } penc_qdec_t;      /* s = (A << 1) | B */
void penc_qdec_init(penc_qdec_t *q, int a, int b);
int  penc_qdec_step(penc_qdec_t *q, int a, int b);   /* -1, 0 or +1 */

/* ---- cadence estimator ------------------------------------------------- */
#define PENC_RING        128     /* snapshots kept                           */
#define PENC_BUCKET_MS   10      /* events closer than this share a snapshot */
#define PENC_WINDOW_MS   400     /* the slope is measured over ~this span    */
#define PENC_GAP_MS      700     /* a longer pause between events breaks a run */
#define PENC_MIN_DT_MS   50      /* less span than this is not a rate        */
#define PENC_TIMEOUT_MS  2000    /* no count for this long = stopped         */
#define PENC_RPM_HOLD_MS 1500    /* a directly reported RPM is good this long */

typedef struct {
    uint32_t t[PENC_RING];       /* ms, snapshot times                       */
    int64_t  c[PENC_RING];       /* cumulative counts at t                   */
    int      n, head;            /* stored, next slot                        */
    int64_t  cum;                /* running count (signed)                   */
    uint32_t bstart;             /* first event time of the newest snapshot  */
    double   cpr;                /* decoded counts per crank revolution      */
    int      have_rpm;           /* set by penc_cad_set_rpm: direct source   */
    uint32_t rpm_t;
    double   rpm_v;
} penc_cad_t;

void   penc_cad_init(penc_cad_t *c, double counts_per_rev);
void   penc_cad_reset(penc_cad_t *c);
/* `counts` decoded encoder counts (signed) observed at `t_ms`. */
void   penc_cad_ticks(penc_cad_t *c, uint32_t t_ms, int counts);
/* A source that reports RPM itself (a Peloton, an "rpm" serial line). */
void   penc_cad_set_rpm(penc_cad_t *c, uint32_t t_ms, double rpm);
/* Cadence at `now_ms`, >= 0. Backwards pedalling reads as 0: the cabinet's
 * pedal is a one-way optical sensor. */
double penc_cad_rpm(const penc_cad_t *c, uint32_t now_ms);
/* RPM -> the 0..127 pedal level input.c feeds through MAME's pulse curve.
 * `full_rpm` is the cadence that means "flat out". 0 stays 0; anything
 * above ~1 RPM is at least 1 so a slow turn still moves the bike. */
int    penc_level_from_rpm(double rpm, double full_rpm);

/* ---- Peloton Gen-1 bike serial protocol -------------------------------- */
/* Sources: ihaque/pelomon (peloton.h, logic-analyser decode) and
 * ptx2/gymnasticon. 19200 8N1 RS-232 on the bike's 3.5mm jack. The head unit
 * sends  F5 <type> <sum> F6  and the bike answers  F1 <type> <len> <len ASCII
 * digits, LEAST significant first> <sum> F6, where <sum> is the low byte of
 * the sum of every byte before it. */
#define PENC_PEL_CADENCE    0x41   /* RPM                    */
#define PENC_PEL_OUTPUT     0x44   /* watts * 10             */
#define PENC_PEL_RESISTANCE 0x4A   /* raw, needs the bike's calibration table */

typedef struct { uint8_t b[24]; int n; } penc_pel_t;
typedef struct { int type; long value; } penc_pel_msg_t;

/* Build the 4-byte request for `type`; returns 4. */
int  penc_pel_request(int type, uint8_t out[4]);
/* Feed one received byte. Returns 1 and fills *m when a frame with a good
 * checksum and clean digits completes, else 0. Resynchronises on 0xF1. */
int  penc_pel_feed(penc_pel_t *p, uint8_t byte, penc_pel_msg_t *m);

/* ---- text lines ("123\n") ---------------------------------------------- */
typedef struct { char b[48]; int n; int bad; } penc_line_t;
/* Feed one byte; returns 1 when a complete line parsed as a number. */
int  penc_line_feed(penc_line_t *l, uint8_t byte, double *val);

#endif
