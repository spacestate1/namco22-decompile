/*
 * tc2_c435.c -- the C435, System 23's 3D command processor, our own code (a fixed-function chip: behaviour as MAME's namcos23.cpp describes
 * it, c435_pio_w and what it dispatches). The main CPU feeds it 16-bit words -- one at a time through 0x02000000 (the PIO port), or by DMA from
 * its RAM (0x0100001C address, 0x01000020 size in bytes, 0x01000024 bit 0 = go) -- and it keeps:
 *   256 matrices (3x3, 2.14) and 256 vectors (s24); id 0x100 and up = a scratch zero one, vector 0x8000 = the light vector
 *   commands  (word 0 = header; the length is header & 0xF, or & 0xFF with bit 14)
 *     0x0xx_   matrix x matrix (.0), matrix x vector (.1), the same with an immediate operand (.2, .3), matrix set (.4), vector set (.5)
 *     0x4x__   state: 4.1 absolute priority, 4.2 / 4.3 scroll x / y, 4.4 scaling, 4.5 model blend factor, 4.7 / 4.8 light power / ambient,
 *              4.F a STATE SET: 0x0001 the C435 interrupt (Cause IP4, acknowledged at 0x0200000E), 0x0046 the viewport offset, 0x00C8 one
 *              line of the clip data (whose third line gives the viewport size), 0x0000 / 0x0002 / 0x000A an immediate polygon
 *     0x8___   RENDER a model: point-ROM model id, its matrix and vector -> one entry of the render list (a header of 0x8000-family
 *              with model 0 raises the C435 interrupt instead)
 *     0xC___   flush
 * DIRECT quads (already in screen space: x, y about the centre, u, v, a 1/z mantissa / exponent, shade per corner) -- 28 words, through the CTL
 * (0x0D00000C, one at a time; a header with bit 15 set is dropped) or the direct buffer (0x0CC00002 opens it, 0x0CC00000 takes a dummy word
 * then the 28) -- MAME's ctl_direct_poly_w / direct_buf_w
 * The render list is double-buffered: the frame's renderer (tc2_render.c) draws the one filled last frame and swaps, as MAME's render_run.
 * Also here: 0x01000028 R busy = 1 (never busy), 0x01000060 W / 0x0C800010 W restart the word buffer, 0x0C800016 W a state word (MAME
 * c435_state_pio_w: STATE SET without the 4.F header).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "tc2_c435.h"

uint32_t rr_read(uint32_t a, int size);
void tc2_irq_line(uint32_t bit, int on);
#define IP_C435 0x1000u

static struct {
    uint16_t buffer[256]; int pos;
    uint32_t address, size;
    uint16_t state_buf[256]; int state_pos;
} c435;
static int16_t matrices[256][9];
static int32_t vectors[256][3], light_vector[3], spv[3];
static int16_t spm[9];
static uint16_t absolute_priority, model_blend_factor, light_power, light_ambient, scaling;
static int16_t tx, ty;
static int16_t vp_offset_x, vp_offset_y, vp_size_x, vp_size_y;
static float clip_data[24]; static int clip_line;
static uint16_t direct_buf[28]; static int direct_pos, direct_nonempty, direct_open;
tc2_render_list tc2_render[2];
int tc2_render_cur;
unsigned long tc2_c435_unknown;

static int32_t s24(uint32_t v) { return (v & 0x800000) ? (int32_t)(v | 0xFF000000u) : (int32_t)(v & 0xFFFFFF); }
/* the C435's float: 8 bits of exponent, a 16-bit two's-complement mantissa; m * 2^(e - 46) */
float tc2_f24(uint32_t v)
{
    if (!(v & 0xFFFF)) return 0;
    uint32_t r = (v & 0x8000) ? 0x80000000u : 0;
    uint16_t m = (uint16_t)(r ? -v : v);
    uint8_t e = (uint8_t)((v >> 16) + 0x60);
    while (!(m & 0x8000)) { m <<= 1; e--; }
    r |= (uint32_t)e << 23 | (uint32_t)(m & 0x7FFF) << 8;
    float f; memcpy(&f, &r, 4); return f;
}
static int32_t *getv(uint16_t id) { if (id == 0x8000) return light_vector; if (id >= 0x100) { memset(spv, 0, sizeof spv); return spv; } return vectors[id]; }
static int16_t *getm(uint16_t id) { if (id >= 0x100) { memset(spm, 0, sizeof spm); return spm; } return matrices[id]; }
static void transpose(int16_t *m) { int16_t t[9]; memcpy(t, m, sizeof t); for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r * 3 + c] = t[c * 3 + r]; }

static tc2_render_entry *new_entry(int type)
{
    tc2_render_list *l = &tc2_render[tc2_render_cur];
    if (l->count >= TC2_RENDER_MAX) return NULL;
    tc2_render_entry *e = &l->e[l->count++];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->absolute_priority = absolute_priority;
    e->light_power = light_power; e->light_ambient = light_ambient;
    e->vp_size_x = vp_size_x; e->vp_size_y = vp_size_y; e->vp_offset_x = vp_offset_x; e->vp_offset_y = vp_offset_y;
    e->vp_fov = clip_data[23];
    return e;
}

static void mat_mat(void)                         /* 0.0: t = m1 * m2 (m1 identity with bit 8, m2 transposed with bit 10) */
{
    const uint16_t *b = c435.buffer;
    if ((b[0] & 0xF) != 4) return;
    int16_t m1[9], m2[9];
    memcpy(m2, getm(b[2]), sizeof m2);
    if (b[0] & 0x400) transpose(m2);
    if (b[0] & 0x100) { memset(m1, 0, sizeof m1); m1[0] = m1[4] = m1[8] = 0x4000; } else memcpy(m1, getm(b[4]), sizeof m1);
    int16_t *t = getm(b[1]);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
        t[r * 3 + c] = (int16_t)((m1[r * 3] * m2[c * 3] + m1[r * 3 + 1] * m2[c * 3 + 1] + m1[r * 3 + 2] * m2[c * 3 + 2]) >> 14);
}
static void mv(int32_t *t, const int16_t *m, const int32_t *v, const uint16_t *b)
{
    int32_t r[3];
    for (int k = 0; k < 3; k++) r[k] = (int32_t)((m[k * 3] * (int64_t)v[0] + m[k * 3 + 1] * (int64_t)v[1] + m[k * 3 + 2] * (int64_t)v[2]) >> 14);
    if (b[3] != 0xFFFF) {
        const int32_t *vt = getv(b[3]);
        for (int k = 0; k < 3; k++) r[k] = (b[0] & 0x1000) ? r[k] - vt[k] : r[k] + vt[k];
    }
    memcpy(t, r, sizeof r);
}
static void mat_vec(void)                         /* 0.1: t = m * v (+/- vt) */
{
    const uint16_t *b = c435.buffer;
    if ((b[0] & 0xF) != 4) return;
    int16_t m[9]; int32_t v[3];
    memcpy(m, getm(b[2]), sizeof m); memcpy(v, getv(b[4]), sizeof v);
    if (b[0] & 0x400) transpose(m);
    mv(getv(b[1]), m, v, b);
}
static void mat_mat_immed(void)                   /* 0.2: t = immediate * m2 */
{
    const uint16_t *b = c435.buffer;
    if ((b[0] & 0xF) != 12) return;
    int16_t m1[9], m2[9];
    for (int i = 0; i < 9; i++) m1[i] = (int16_t)b[4 + i];
    memcpy(m2, getm(b[2]), sizeof m2);
    if (b[0] & 0x400) transpose(m2);
    int16_t *t = getm(b[1]);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
        t[r * 3 + c] = (int16_t)((m1[r * 3] * m2[c] + m1[r * 3 + 1] * m2[3 + c] + m1[r * 3 + 2] * m2[6 + c]) >> 14);
}
static void mat_vec_immed(void)                   /* 0.3: t = m * immediate (+/- vt) */
{
    const uint16_t *b = c435.buffer;
    if ((b[0] & 0xF) != 9) return;
    int16_t m[9]; int32_t v[3];
    memcpy(m, getm(b[2]), sizeof m);
    for (int k = 0; k < 3; k++) v[k] = (int32_t)((uint32_t)b[4 + 2 * k] << 16 | b[5 + 2 * k]);
    if (b[0] & 0x400) transpose(m);
    mv(getv(b[1]), m, v, b);
}
static void mat_set(void) { const uint16_t *b = c435.buffer; if ((b[0] & 0xF) != 10) return; int16_t *t = getm(b[1]); for (int i = 0; i < 9; i++) t[i] = (int16_t)b[i + 2]; }
static void vec_set(void) { const uint16_t *b = c435.buffer; if ((b[0] & 0xF) != 7) return; int32_t *t = getv(b[1]); for (int i = 0; i < 3; i++) t[i] = s24((uint32_t)b[2 * i + 2] << 16 | b[2 * i + 3]); }

static int state_size(uint16_t type)
{
    switch (type & 0xFF) {
    case 0x00: return 43; case 0x01: return 1; case 0x02: return 53; case 0x09: return 19; case 0x0A: return 47;
    case 0x42: return 41; case 0x46: return 13; case 0xC0: return 33; case 0xC6: return 13; case 0xC8: return 17;
    default: return -1;
    }
}
static uint32_t L(const uint16_t *p, int i) { return (uint32_t)p[i] << 16 | p[i + 1]; }
/* an IMMEDIATE polygon (state 0x0000 / 0x0002 / 0x000A): the words' layouts per MAME's c435_state_set */
static void immediate(uint16_t type, const uint16_t *p, uint16_t header)
{
    tc2_render_entry *e = new_entry(TC2_RE_IMMEDIATE);
    if (!e) return;
    tc2_immediate *q = &e->imm;
    q->type = p[0]; q->h = L(p, 1); q->pal = L(p, 3);
    int k = 5, n = 4, with_i = 1;
    if (type == 0x0000) { q->zbias = L(p, 5); k = 7; n = header == 0x4F38 ? 4 : 3; }
    else if (type == 0x0002) { q->zbias = 0; k = 5; }
    else { q->zbias = L(p, 5); k = 7; with_i = 0; }
    if (with_i) for (int i = 0; i < n; i++, k += 2) q->i[i] = L(p, k);
    for (int i = 0; i < n; i++) { q->u[i] = L(p, k); q->v[i] = L(p, k + 2); k += 4; }
    for (int i = 0; i < n; i++) { q->x[i] = L(p, k); q->y[i] = L(p, k + 2); q->z[i] = L(p, k + 4); k += 6; }
    q->n = n;
}
static void state_set(uint16_t type, const uint16_t *p, uint16_t header)
{
    switch (type) {
    case 0x0000: case 0x0002: case 0x000A: immediate(type, p, header); break;
    case 0x0001: tc2_irq_line(IP_C435, p[0] & 1); break;
    case 0x0046: {
        const uint32_t d = L(p, 7);
        vp_offset_x = (int16_t)((int16_t)((d & 0xFFF) << 4) >> 4);
        vp_offset_y = (int16_t)-((int16_t)(((d >> 12) & 0xFFF) << 4) >> 4);
        break; }
    case 0x00C8:
        for (int i = 0; i < 8; i++) clip_data[clip_line * 8 + i] = tc2_f24(L(p, 2 * i + 1));
        clip_line = (clip_line + 1) % 3;
        if (clip_line == 0 && clip_data[10] != 0.f) {
            const float a10 = clip_data[10] < 0 ? -clip_data[10] : clip_data[10], a23 = clip_data[23] < 0 ? -clip_data[23] : clip_data[23];
            const float a2 = clip_data[2] < 0 ? -clip_data[2] : clip_data[2];
            vp_size_y = (int16_t)(a10 * a23 + 0.5f);
            vp_size_x = (int16_t)((float)vp_size_y * a2 / a10 + 0.5f);
        }
        break;
    default: break;
    }
}
static void render_model(void)                    /* 8: a model of the point ROM */
{
    const uint16_t *b = c435.buffer;
    const int size = b[0] & 0xF;
    if (b[1] == 0) { tc2_irq_line(IP_C435, 1); return; }
    tc2_render_entry *e = new_entry(TC2_RE_MODEL);
    if (!e) return;
    e->model = b[1];
    e->model2 = size == 4 ? b[2] : 0;
    e->scaling = (b[0] & 0x80) ? scaling / 16384.0f : 1.0f;
    e->transpose = (b[0] >> 6) & 1;
    e->mirror_x = (b[0] >> 13) & 1;
    e->model_blend_factor = model_blend_factor;
    e->tx = (b[0] & 0x200) ? tx : 0; e->ty = (b[0] & 0x200) ? ty : 0;
    memcpy(e->m, getm(b[size - 1]), sizeof e->m);
    memcpy(e->v, getv(b[size]), sizeof e->v);
    memcpy(e->light_vector, light_vector, sizeof e->light_vector);
}

void tc2_c435_pio(uint16_t w)
{
    c435.buffer[c435.pos++ & 255] = w;
    const uint16_t h = c435.buffer[0];
    const int psize = (h & 0x4000) ? (h & 0xFF) : (h & 0xF);
    if (c435.pos < psize + 1) return;
    int known = 1;
    switch (h & 0xC000) {
    case 0x0000:
        switch (h & 0xF0) {
        case 0x00: mat_mat(); break; case 0x10: mat_vec(); break; case 0x20: mat_mat_immed(); break; case 0x30: mat_vec_immed(); break;
        case 0x40: mat_set(); break; case 0x50: vec_set(); break; default: known = 0;
        }
        break;
    case 0x4000:
        switch (h & 0x3F00) {
        case 0x0000: case 0x0600: break;
        case 0x0100: absolute_priority = c435.buffer[1]; break;
        case 0x0200: tx = (int16_t)c435.buffer[1]; break;
        case 0x0300: ty = (int16_t)c435.buffer[1]; break;
        case 0x0400: if ((h & 0xFF) == 1) scaling = c435.buffer[1]; break;
        case 0x0500: model_blend_factor = c435.buffer[1]; break;
        case 0x0700: light_power = c435.buffer[1]; break;
        case 0x0800: light_ambient = c435.buffer[1]; break;
        case 0x0F00:
            if ((h & 0xFF) == 0) break;
            if (state_size(c435.buffer[1]) != (h & 0xFF) - 1 && h != 0x4F38) break;
            state_set(c435.buffer[1], c435.buffer + 2, h);
            break;
        default: known = 0;
        }
        break;
    case 0x8000: render_model(); break;
    case 0xC000: break;                           /* flush: the C451 interrupt, not wired on System 23 */
    }
    if (!known) tc2_c435_unknown++;
    c435.pos = 0;
}
/* MAME c435_state_pio_w: a state entry written word by word through 0x0C800016 (its type first) */
static void state_pio(uint16_t w)
{
    c435.state_buf[c435.state_pos++ & 255] = w;
    const int n = state_size(c435.state_buf[0]);
    if (n < 0 || c435.state_pos < n + 1) { if (n < 0) c435.state_pos = 0; return; }
    state_set(c435.state_buf[0], c435.state_buf + 1, 0);
    c435.state_pos = 0;
}

static void direct_entry(void)
{
    tc2_render_entry *e = new_entry(TC2_RE_DIRECT);
    if (!e) return;
    memcpy(e->d, direct_buf, sizeof e->d);
    e->vp_size_x = 320; e->vp_size_y = 240; e->vp_offset_x = 0; e->vp_offset_y = 0; e->vp_fov = 320.f;
}
int tc2_c435_write(uint32_t p, int size, uint32_t v)
{
    switch (p) {
    case 0x0D00000Cu:                                    /* MAME ctl_direct_poly_w */
        direct_buf[direct_pos++] = (uint16_t)v;
        if (v & 0xFFFF) direct_nonempty = 1;
        if (direct_pos >= 28) {
            if (direct_nonempty && !(direct_buf[0] & 0x8000)) direct_entry();
            direct_nonempty = 0; direct_pos = 0;
        }
        return 1;
    case 0x0CC00002u: direct_open = (v & 0xFFFF) != 0; direct_pos = 0; return 1;
    case 0x0CC00000u:
        if (!direct_open) return 1;
        if (direct_pos == 0) { direct_pos++; return 1; }
        direct_buf[direct_pos - 1] = (uint16_t)v; direct_pos++; direct_nonempty = 1;
        if (direct_pos >= 29) { direct_entry(); direct_nonempty = 0; direct_pos = 0; }
        return 1;
    case 0x0100001Cu: c435.address = v; return 1;
    case 0x01000020u: c435.size = v; return 1;
    case 0x01000024u:
        /* the length is the game's; a garbage one must not hang the frame -- main RAM is 16 MB */
        if (v & 1) for (uint32_t pos = 0; pos < c435.size && pos < 0x1000000u; pos += 2) tc2_c435_pio((uint16_t)rr_read((c435.address & 0x1FFFFFFFu) + pos, 2));
        return 1;
    case 0x01000060u: c435.pos = 0; return 1;
    case 0x02000000u: if (size == 2) { tc2_c435_pio((uint16_t)v); return 1; } return 0;
    case 0x0C800010u: c435.state_pos = 0; return 1;
    case 0x0C800016u: state_pio((uint16_t)v); return 1;
    }
    return 0;
}
int tc2_c435_owns(uint32_t p) { return (p >= 0x0100001Cu && p <= 0x0100002Bu) || p == 0x01000060u || p == 0x0C800010u || p == 0x0C800016u; }
