/* pedal_enc_core.c -- see include/pedal_enc_core.h. No SDL, no OS. */
#include "pedal_enc_core.h"
#include <stdlib.h>
#include <string.h>

/* ---- quadrature ---------------------------------------------------------
 * index = (previous << 2) | current, state = (A << 1) | B. One Gray-code step
 * forward is 00 -> 01 -> 11 -> 10 -> 00 (+1); the other way is -1. Same-state
 * and two-bit jumps are 0. */
static const int8_t qtab[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0,
};

void penc_qdec_init(penc_qdec_t *q, int a, int b)
{
    q->s = (uint8_t)(((a != 0) << 1) | (b != 0));
}

int penc_qdec_step(penc_qdec_t *q, int a, int b)
{
    uint8_t cur = (uint8_t)(((a != 0) << 1) | (b != 0));
    int d = qtab[(q->s << 2) | cur];
    q->s = cur;
    return d;
}

/* ---- cadence ------------------------------------------------------------ */
void penc_cad_reset(penc_cad_t *c)
{
    double cpr = c->cpr;
    memset(c, 0, sizeof *c);
    c->cpr = cpr;
}

void penc_cad_init(penc_cad_t *c, double counts_per_rev)
{
    memset(c, 0, sizeof *c);
    c->cpr = counts_per_rev;
}

void penc_cad_ticks(penc_cad_t *c, uint32_t t, int counts)
{
    if (!counts) return;
    c->cum += counts;
    /* Events within one bucket of the bucket's FIRST event share a snapshot,
     * so a high-resolution encoder (thousands of counts a second) cannot fill
     * the ring with a few milliseconds of history and starve the window. The
     * snapshot is (latest time, cumulative count), so it stays exact. */
    if (c->n > 0 && (uint32_t)(t - c->bstart) < PENC_BUCKET_MS) {
        int last = (c->head + PENC_RING - 1) % PENC_RING;
        c->t[last] = t;
        c->c[last] = c->cum;
        return;
    }
    c->t[c->head] = t;
    c->c[c->head] = c->cum;
    c->head = (c->head + 1) % PENC_RING;
    if (c->n < PENC_RING) c->n++;
    c->bstart = t;
}

void penc_cad_set_rpm(penc_cad_t *c, uint32_t t, double rpm)
{
    c->have_rpm = 1;
    c->rpm_t = t;
    c->rpm_v = rpm > 0.0 ? rpm : 0.0;
}

double penc_cad_rpm(const penc_cad_t *c, uint32_t now)
{
    if (c->have_rpm) {
        uint32_t age = now - c->rpm_t;
        if ((int32_t)age < 0) age = 0;
        return age > PENC_RPM_HOLD_MS ? 0.0 : c->rpm_v;
    }
    if (c->n == 0 || c->cpr <= 0.0) return 0.0;

    int newest = (c->head + PENC_RING - 1) % PENC_RING;
    uint32_t tn = c->t[newest];
    uint32_t age = now - tn;
    if ((int32_t)age < 0) age = 0;
    if (age > PENC_TIMEOUT_MS) return 0.0;

    /* Walk back from the newest snapshot until the window is spanned, but stop
     * at a gap: a pause followed by a restart is two runs, and a slope drawn
     * across the pause would read the restart as slow. */
    int oldest = newest, prev = newest;
    for (int k = 1; k < c->n; k++) {
        int i = (newest - k + PENC_RING) % PENC_RING;
        if (c->t[prev] - c->t[i] > PENC_GAP_MS) break;
        oldest = prev = i;
        if (tn - c->t[i] >= PENC_WINDOW_MS) break;
    }
    /* One lone event after a gap: all there is to go on is that gap. */
    if (oldest == newest && c->n > 1)
        oldest = (newest - 1 + PENC_RING) % PENC_RING;
    if (oldest == newest) return 0.0;

    uint32_t dt = tn - c->t[oldest];
    if (dt < PENC_MIN_DT_MS) return 0.0;        /* not enough span to be a rate */
    int64_t dc = c->c[newest] - c->c[oldest];
    if (dc <= 0) return 0.0;                     /* backwards / no net motion */

    double rpm = (double)dc / c->cpr * 60000.0 / (double)dt;

    /* No count for `age` ms means the true rate is at most one count per
     * `age`. Past the mean interval that bound is tighter than the window's
     * slope, so a stop reads as a smooth 1/t fall-off instead of holding the
     * old cadence until the hard timeout. */
    if (age > 0) {
        double cap = 60000.0 / (c->cpr * (double)age);
        if (rpm > cap) rpm = cap;
    }
    return rpm;
}

int penc_level_from_rpm(double rpm, double full_rpm)
{
    if (rpm < 1.0 || full_rpm <= 0.0) return 0;
    int l = (int)(rpm / full_rpm * 127.0 + 0.5);
    if (l < 1) l = 1;
    if (l > 127) l = 127;
    return l;
}

/* ---- Peloton serial ------------------------------------------------------ */
int penc_pel_request(int type, uint8_t out[4])
{
    out[0] = 0xF5;
    out[1] = (uint8_t)type;
    out[2] = (uint8_t)(0xF5 + type);       /* low byte of the sum before it */
    out[3] = 0xF6;
    return 4;
}

int penc_pel_feed(penc_pel_t *p, uint8_t c, penc_pel_msg_t *m)
{
    if (p->n == 0) {                        /* idle: wait for a bike frame */
        if (c == 0xF1) p->b[p->n++] = c;
        return 0;
    }
    p->b[p->n++] = c;
    if (p->n == 3 && (p->b[2] < 1 || p->b[2] > 16)) {   /* absurd length */
        p->n = 0;
        return 0;
    }
    if (p->n >= 3 && p->n == p->b[2] + 5) {             /* F1 ty len pay.. cs F6 */
        int len = p->b[2], n = p->n;
        p->n = 0;
        uint8_t sum = 0;
        for (int i = 0; i < n - 2; i++) sum = (uint8_t)(sum + p->b[i]);
        if (p->b[n - 1] != 0xF6 || sum != p->b[n - 2]) return 0;
        long v = 0;                          /* digits, least significant first */
        for (int i = 2 + len; i >= 3; i--) {
            int d = p->b[i] - 0x30;
            if (d < 0 || d > 9) return 0;
            v = v * 10 + d;
        }
        m->type = p->b[1];
        m->value = v;
        return 1;
    }
    if (p->n >= (int)sizeof p->b) p->n = 0;
    return 0;
}

/* ---- text lines ---------------------------------------------------------- */
int penc_line_feed(penc_line_t *l, uint8_t c, double *val)
{
    if (c == '\n' || c == '\r') {
        int ok = 0;
        if (!l->bad && l->n > 0) {
            l->b[l->n] = 0;
            char *end;
            double d = strtod(l->b, &end);
            while (*end == ' ' || *end == '\t') end++;
            if (end != l->b && *end == 0) { *val = d; ok = 1; }
        }
        l->n = 0;
        l->bad = 0;
        return ok;
    }
    if (l->n < (int)sizeof l->b - 1) l->b[l->n++] = (char)c;
    else l->bad = 1;                         /* too long: drop to end of line */
    return 0;
}
