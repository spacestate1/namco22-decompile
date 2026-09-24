/*
 * rd_b4.c -- Phase B batch 4 (functions 0x20AA4..0x318BE): readable C
 * replacing lifted 68K functions, each proven against its lifted twin by
 * RR_RD=check (include/rd.h). Written from the 68K instructions
 * (tools/rd_show.py); Ghidra's C is a naming aid only.
 *
 * Conventions (as in rd_funcs.c / rd_calls.c):
 *   - A6 is always WRAM + 0x8000 (0x10008000): (d16,A6) is a fixed address.
 *   - A5 holds the sine table: sin(a) is the word at A5 + (a & ~1) (a signed
 *     byte offset), cos(a) the word 0x4000 further on.
 *   - Registers are left exactly as the 68K leaves them, including the upper
 *     halves a move.w does not touch (leaf functions keep them in a regs_t).
 *   - charge() counts the original instructions in order; poll() stands where
 *     the lifted code polls (a taken backward branch, a computed jmp); `cost`
 *     in the table is what runs after the last poll point.
 *   - A function whose CALLERS branch on its condition codes sets them
 *     (set_flags_*): the 68K returns results in Z at a few places.
 */
#include "rd.h"
#include "rr_lifted.h"

#define A6W(off)   (0x10008000u + (uint32_t)(int32_t)(off))   /* (d16,A6) */

/* ---- register helpers ---- */
typedef struct { uint32_t d[8], a[8]; } regs_t;
static void regs_load(regs_t *r)
{
    for (int i = 0; i < 8; i++) { r->d[i] = d_reg(i); r->a[i] = a_reg(i); }
}
static void regs_store(const regs_t *r)                 /* D0-D7, A0-A5 */
{
    for (int i = 0; i < 8; i++) set_d(i, r->d[i]);
    for (int i = 0; i < 6; i++) set_a(i, r->a[i]);
}
static inline uint32_t setw(uint32_t r, uint32_t v) { return (r & 0xFFFF0000u) | (v & 0xFFFFu); }
static inline uint32_t setb(uint32_t r, uint32_t v) { return (r & 0xFFFFFF00u) | (v & 0xFFu); }
static inline int32_t  sxw(uint32_t v) { return (int16_t)v; }
static inline int32_t  sxb(uint32_t v) { return (int8_t)v; }
static inline uint32_t muls_w(uint32_t a, uint32_t b) { return (uint32_t)(sxw(a) * sxw(b)); }

/* condition codes live in R[] bytes 0x43..0x47 (X,N,Z,V,C) */
static inline int  flag_z(void) { return RG1(0x45) != 0; }
static inline void set_nzvc(int n, int z, int v, int c) { RS1(0x44, n); RS1(0x45, z); RS1(0x46, v); RS1(0x47, c); }
/* cmp.w src,dst (dst - src) */
static inline void flags_cmp_w(uint32_t dst, uint32_t src)
{
    uint16_t d = (uint16_t)dst, s = (uint16_t)src, r = (uint16_t)(d - s);
    set_nzvc((r >> 15) & 1, r == 0, (((d ^ s) & (d ^ r)) >> 15) & 1, s > d);
}

/* sine table lookup: the word at A5 + angle (angle a signed word, bit 0 already clear) */
static inline uint16_t trig(uint32_t a5, uint32_t angle) { return (uint16_t)vrd16(a5 + (uint32_t)sxw(angle)); }

/* ---- WRAM globals (A6 = 0x10008000) ---- */
#define G_FRAME          A6W(0x0C4C)   /* word: frame counter (blink phases are taken from it) */
#define G_DL_CURSOR      A6W(0x0C46)   /* long: display-list write pointer */
#define G_DL_PRIO        A6W(0x0C40)   /* word: current display-list priority */
#define G_COURSE         A6W(0x2046)   /* word: course number */
#define G_COURSE_VAR     A6W(0x2048)   /* word: course variant (0..3) */
#define G_CAM_X          A6W(0x20F8)   /* word: camera / viewpoint X (world units >> ?) */
#define G_CAM_Y          A6W(0x2108)
#define G_CAM_Z          A6W(0x2118)
#define G_VIEW_OFS_X     A6W(0x20BC)   /* word: view offset subtracted with the camera */
#define G_VIEW_OFS_Y     A6W(0x20BE)
#define G_VIEW_OFS_Z     A6W(0x20C0)

/* The distance cull shared by the object emitters: dx/dz relative to the
 * camera go to D6w/D7w, |dx| / |dz| to D2w/D3w, D3 = |dx| + |dz|; returns 1
 * when D3 < limit (the object is drawn). The caller has cleared D2/D3 (moveq)
 * and charged the load of x. Charges 14..16 instructions. */
static int near_camera(regs_t *r, uint16_t x, uint16_t z, uint32_t limit)
{
    uint16_t dx = (uint16_t)(x - vrd16(G_CAM_X) - vrd16(G_VIEW_OFS_X));
    r->d[0] = setw(r->d[0], dx); r->d[6] = setw(r->d[6], dx); r->d[2] = setw(r->d[2], dx);
    charge(5);                                           /* sub, sub, move, move, bpl (the load is the caller's) */
    if (dx & 0x8000) { charge(1); r->d[2] = setw(r->d[2], (uint16_t)-dx); }
    uint16_t dz = (uint16_t)(z - vrd16(G_CAM_Z) - vrd16(G_VIEW_OFS_Z));
    r->d[0] = setw(r->d[0], dz); r->d[7] = setw(r->d[7], dz); r->d[3] = setw(r->d[3], dz);
    charge(6);
    if (dz & 0x8000) { charge(1); r->d[3] = setw(r->d[3], (uint16_t)-dz); }
    r->d[3] += r->d[2];
    charge(3);                                           /* add.l, cmpi.l, bcc */
    return r->d[3] < limit;
}

/* dy relative to the camera into D0w and D5w (4 instructions) */
static void rel_y(regs_t *r, uint16_t y)
{
    uint16_t dy = (uint16_t)(y - vrd16(G_CAM_Y) - vrd16(G_VIEW_OFS_Y));
    r->d[0] = setw(r->d[0], dy); r->d[5] = setw(r->d[5], dy);
    charge(4);
}

/* D1w = -sin(angle), D2w = cos(angle), D0w = the cos index, from the angle in
 * D0w (andi, move, addi, move, neg: 5 instructions) */
static void sin_cos(regs_t *r)
{
    uint16_t a = (uint16_t)(r->d[0] & 0xFFFE);
    r->d[1] = setw(r->d[1], (uint16_t)-trig(r->a[5], a));
    a = (uint16_t)(a + 0x4000);
    r->d[0] = setw(r->d[0], a);
    r->d[2] = setw(r->d[2], trig(r->a[5], a));
    charge(5);
}

/* write one long to the display list at A3 */
static inline void put_a3(regs_t *r, uint32_t v) { vwr32(r->a[3], v); r->a[3] += 4; }

/* dx*25, dy*25, dz*25 (D6, D5, D7 multiplied in place) to the list: move #0x19,
 * then three muls + move.l (7 instructions) */
static void put_scaled_xyz(regs_t *r)
{
    r->d[0] = setw(r->d[0], 0x19);
    r->d[6] = muls_w(r->d[6], 0x19); put_a3(r, r->d[6]);
    r->d[5] = muls_w(r->d[5], 0x19); put_a3(r, r->d[5]);
    r->d[7] = muls_w(r->d[7], 0x19); put_a3(r, r->d[7]);
    charge(7);
}

/* The object record the emitters write: model, dx*25, dy*25, dz*25, 0, 0x7FFF.
 * D0w must hold the model; its move.l is the caller's. Charges 9. */
static void put_scaled_position(regs_t *r)
{
    put_scaled_xyz(r);
    put_a3(r, 0); put_a3(r, 0x7FFF);
    charge(2);
}

/* ======================= trackside objects ======================= */

/* FUN_00020aa4: emit the course's animated trackside objects. First sets the
 * three state words 0x4A4A (1 or 2 from bit 4 of the frame counter), 0x4A4C (1,
 * or 2/3 blinking while the countdown 0x48E4 runs down) and 0x4A4E (2 when
 * 0x4846 == 0x48EA, else 1). The course variant selects, from the table at
 * 0x2089C, an object list (x, y, z, angle; 8 bytes; x = -1 ends) and a parallel
 * 8-byte model table (word 0 = which state word, words 1..3 = the model for
 * state 1..3). Every object within 35000 of the camera whose state is nonzero
 * is appended to the display list; the list ends with -1. */
static uint32_t rd_trackside_objects(void)
{
    regs_t r; regs_load(&r);
    uint16_t t = (uint16_t)(((vrd16(G_FRAME) >> 4) & 1) + 1);
    r.d[0] = setw(r.d[0], t); vwr16(A6W(0x4A4A), t);
    t = (uint16_t)(vrd16(A6W(0x48E4)) - 1);
    r.d[0] = setw(r.d[0], t);
    charge(8);
    if (!(t & 0x8000)) { charge(1); vwr16(A6W(0x48E4), t); }
    vwr16(A6W(0x4A4C), 1);
    t = (uint16_t)vrd16(A6W(0x48E4));
    r.d[0] = setw(r.d[0], t);
    charge(3);
    if (t) {
        t = (uint16_t)(((t >> 3) & 1) + 2);
        r.d[0] = setw(r.d[0], t); vwr16(A6W(0x4A4C), t);
        charge(4);
    }
    r.d[1] = setw(r.d[1], 1);
    r.d[0] = vrd32(A6W(0x4846));
    charge(4);
    if (r.d[0] == vrd32(A6W(0x48EA))) { charge(1); r.d[1] = setw(r.d[1], 2); }
    vwr16(A6W(0x4A4E), (uint16_t)r.d[1]);
    r.a[2] = 0x2089C;                                     /* table of (model table, object table) */
    r.d[0] = setw(r.d[0], vrd16(G_COURSE_VAR));
    r.a[1] = vrd32(r.a[2] + (uint32_t)(sxw(r.d[0]) * 8));
    r.a[0] = vrd32(r.a[2] + (uint32_t)(sxw(r.d[0]) * 8) + 4);
    r.d[4] = 0;
    r.a[3] = vrd32(G_DL_CURSOR);
    charge(7);
    for (;;) {
        r.d[2] = r.d[3] = r.d[5] = r.d[6] = r.d[7] = 0;
        uint32_t ent = r.a[0] + (uint32_t)(sxw(r.d[4]) * 8);
        uint16_t x = (uint16_t)vrd16(ent);
        r.d[0] = setw(r.d[0], x);
        charge(8);                                        /* 5 moveq, move, cmpi, beq */
        if (x == 0xFFFF) break;
        if (near_camera(&r, x, (uint16_t)vrd16(ent + 4), 0x88B8)) {
            rel_y(&r, (uint16_t)vrd16(ent + 2));
            uint16_t type_ofs = (uint16_t)(r.d[4] << 3);
            r.d[0] = setw(r.d[0], type_ofs);
            uint16_t state = (uint16_t)vrd16(r.a[1] + (uint32_t)sxw(type_ofs));
            state = (uint16_t)vrd16(A6W(0x4A4A) + (uint32_t)(sxw(state) * 2));
            r.d[1] = setw(r.d[1], state);
            charge(5);                                    /* move, lsl, move, move, beq */
            if (state) {
                uint16_t sel = (uint16_t)(state * 2 + type_ofs);
                r.d[1] = setw(r.d[1], sel);
                r.d[0] = (uint16_t)vrd16(r.a[1] + (uint32_t)sxw(sel));
                put_a3(&r, r.d[0]);
                charge(5);                                /* add, add, move, andi.l, move.l */
                put_scaled_position(&r);
                r.d[0] = setw(r.d[0], vrd16(ent + 6));
                charge(1);
                sin_cos(&r);
                put_a3(&r, r.d[1]); put_a3(&r, r.d[2]);
                put_a3(&r, 0); put_a3(&r, 0x7FFF); put_a3(&r, 4);
                charge(5);
            }
        }
        r.d[4] = setw(r.d[4], r.d[4] + 1);
        charge(2);                                        /* addq, bra */
        poll();
    }
    vwr32(r.a[3], 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, r.a[3]);
    charge(2);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_00020c36: on course 0 only (else continue at 0x20898), emit the six
 * digit boards: six fixed models 0x61C..0x621 and six digits 0x612 + d of the
 * decimal value of 0x4930 (FUN_0000a8c8), at the twelve positions of the table
 * at 0x20BD4, each within 20000 of the camera. */
static uint32_t rd_digit_boards(void)
{
    uint32_t r0;
    charge(2);                                            /* tst, bne */
    if (vrd16(G_COURSE)) return RD_JMP(0x020898);
    uint32_t a0 = A6W(0x4A32);
    static const uint16_t fixed[6] = { 0x61E, 0x61F, 0x620, 0x621, 0x61D, 0x61C };
    for (int i = 0; i < 6; i++) vwr16(a0 + 2u * (uint32_t)i, fixed[i]);
    set_a(0, a0 + 12);
    set_d(1, vrd32(A6W(0x4930)));
    charge(9);                                            /* lea, 6 moves, move.l, jsr */
    if ((r0 = rd_call(L_A8C8, 0x020C64))) return r0;
    regs_t r; regs_load(&r);
    r.d[0] = (r.d[0] << 4) | (r.d[0] >> 28);
    r.d[7] = setw(r.d[7], 5);
    charge(2);
    for (;;) {
        r.d[0] = (r.d[0] << 4) | (r.d[0] >> 28);
        uint16_t dig = (uint16_t)((r.d[0] & 0xF) + 0x612);
        r.d[1] = setw(r.d[1], dig);
        vwr16(r.a[0], dig); r.a[0] += 2;
        charge(6);                                        /* rol, move, andi, addi, move, dbf */
        uint16_t c = (uint16_t)(r.d[7] - 1);
        r.d[7] = setw(r.d[7], c);
        if (c == 0xFFFF) break;
        poll();
    }
    r.a[0] = 0x20BD4;                                     /* 12 (x, y, z, angle) positions */
    r.a[1] = A6W(0x4A32);                                 /* their models */
    r.a[3] = vrd32(G_DL_CURSOR);
    r.d[4] = 0;
    charge(4);
    for (;;) {
        r.d[2] = r.d[3] = r.d[5] = r.d[6] = r.d[7] = 0;
        uint32_t ent = r.a[0] + (uint32_t)(sxw(r.d[4]) * 8);
        charge(6);                                        /* 5 moveq, move */
        if (near_camera(&r, (uint16_t)vrd16(ent), (uint16_t)vrd16(ent + 4), 0x4E20)) {
            rel_y(&r, (uint16_t)vrd16(ent + 2));
            r.d[0] = setw(r.d[0], vrd16(r.a[1])); r.a[1] += 2;
            put_a3(&r, r.d[0]);
            charge(2);
            put_scaled_position(&r);
            r.d[0] = setw(r.d[0], vrd16(ent + 6));
            charge(1);
            sin_cos(&r);
            put_a3(&r, r.d[1]); put_a3(&r, r.d[2]);
            put_a3(&r, 0); put_a3(&r, 0x7FFF); put_a3(&r, 4);
            charge(5);
        }
        r.d[4] = setw(r.d[4], r.d[4] + 1);
        charge(3);                                        /* addq, cmpi, bne */
        if ((uint16_t)r.d[4] == 12) break;
        poll();
    }
    vwr32(r.a[3], 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, r.a[3]);
    charge(2);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_00020e36: emit the course's banners: for each (segment, angle ofs,
 * model, spare) record of the course's list (the table at 0x20F4E, segment
 * < 0 ends it) whose segment is within 0x50 of the car's (0x204C), place
 * the model beside the track at that segment's point (ECDC course tables
 * +0x0C xy, +0x10 y, +0x14 heading, +0x20 offset), within 10000 of the
 * camera. Each placed banner rewrites the list end marker. */
static uint32_t rd_track_banners(void)
{
    regs_t r; regs_load(&r);
    r.d[0] = setw(r.d[0], vrd16(G_COURSE));
    r.a[0] = 0x20F4E;
    r.a[1] = vrd32(r.a[0] + (uint32_t)(sxw(r.d[0]) * 4));
    charge(3);
    for (;;) {
        uint16_t seg = (uint16_t)vrd16(r.a[1]);
        r.d[6] = setw(r.d[6], seg);
        charge(2);                                        /* move, bmi */
        if (seg & 0x8000) break;
        uint16_t d = (uint16_t)(vrd16(A6W(0x204C)) - seg);
        r.d[0] = setw(r.d[0], d);
        charge(3);                                        /* move, sub, bpl */
        if (d & 0x8000) { charge(1); d = (uint16_t)-d; r.d[0] = setw(r.d[0], d); }
        charge(2);                                        /* cmpi, bcc */
        if (d < 0x50) {
            uint32_t crs = (uint32_t)sxw((uint16_t)(vrd16(G_COURSE) << 6));
            r.a[0] = 0xECDC;
            r.d[0] = setw(r.d[0], (uint16_t)(vrd16(G_COURSE) << 6));
            r.a[2] = vrd32(0xECDC + crs + 0x0C);
            r.a[3] = vrd32(0xECDC + crs + 0x14);
            r.a[4] = vrd32(0xECDC + crs + 0x20);
            r.a[0] = vrd32(0xECDC + crs + 0x10);
            charge(7);
            int32_t i6 = sxw(r.d[6]);
            uint16_t ang = (uint16_t)(-(int32_t)vrd16(r.a[3] + (uint32_t)(i6 * 2)) - 0x4000 + vrd16(r.a[1] + 2));
            r.d[5] = setw(r.d[5], ang);
            r.d[0] = setw(r.d[0], ang);
            charge(5);                                    /* move, neg, subi, add, move */
            sin_cos(&r);
            uint16_t ofs = (uint16_t)vrd16(r.a[4] + (uint32_t)(i6 * 8));
            r.d[0] = setw(r.d[0], ofs);
            r.d[1] = muls_w(r.d[1], ofs); r.d[1] += r.d[1]; r.d[1] = (r.d[1] << 16) | (r.d[1] >> 16);
            r.d[2] = muls_w(r.d[2], ofs); r.d[2] += r.d[2]; r.d[2] = (r.d[2] << 16) | (r.d[2] >> 16);
            r.d[1] = setw(r.d[1], r.d[1] + vrd16(r.a[2] + (uint32_t)(i6 * 4)));
            r.d[2] = setw(r.d[2], r.d[2] + vrd16(r.a[2] + (uint32_t)(i6 * 4) + 2));
            r.a[3] = vrd32(G_DL_CURSOR);
            r.d[3] = 0; r.d[4] = 0;
            charge(12);                                   /* move, 2x(muls,add,swap), 2 add, movea, 2 moveq */
            uint16_t dx = (uint16_t)(r.d[1] - vrd16(G_CAM_X) - vrd16(G_VIEW_OFS_X));
            r.d[1] = setw(r.d[1], dx); r.d[3] = setw(r.d[3], dx);
            charge(4);
            if (dx & 0x8000) { charge(1); r.d[3] = setw(r.d[3], (uint16_t)-dx); }
            uint16_t dz = (uint16_t)(r.d[2] - vrd16(G_CAM_Z) - vrd16(G_VIEW_OFS_Z));
            r.d[2] = setw(r.d[2], dz); r.d[4] = setw(r.d[4], dz);
            charge(4);
            if (dz & 0x8000) { charge(1); r.d[4] = setw(r.d[4], (uint16_t)-dz); }
            r.d[4] += r.d[3];
            charge(3);                                    /* add.l, cmpi.l, bcc */
            if (r.d[4] < 0x2710) {
                r.d[0] = setw(r.d[0], vrd16(r.a[1] + 4));
                put_a3(&r, r.d[0]);
                r.d[1] = muls_w(r.d[1], 0x19); put_a3(&r, r.d[1]);
                uint16_t dy = (uint16_t)(vrd16(r.a[0] + (uint32_t)(i6 * 2)) - vrd16(G_CAM_Y) - vrd16(G_VIEW_OFS_Y));
                r.d[0] = muls_w(dy, 0x19); put_a3(&r, r.d[0]);
                r.d[2] = muls_w(r.d[2], 0x19); put_a3(&r, r.d[2]);
                put_a3(&r, 0); put_a3(&r, 0x7FFF);
                r.d[0] = setw(r.d[0], r.d[5]);
                charge(14);
                sin_cos(&r);
                put_a3(&r, r.d[1]); put_a3(&r, r.d[2]);
                put_a3(&r, 0); put_a3(&r, 0x7FFF); put_a3(&r, 0);
                charge(5);
            }
            vwr32(r.a[3], 0xFFFFFFFFu);
            vwr32(G_DL_CURSOR, r.a[3]);
            charge(2);
        }
        r.a[1] += 8;
        charge(2);                                        /* adda, bra */
        poll();
    }
    regs_store(&r);
    return RD_RTS;
}

/* FUN_000211e0 (not in the replay: 0x1100 set): the car-carrier object.
 * Bit 1 of 0x11AA says whether 0x40B4 is set and 0x47D4 clear (cleared) or
 * not (set). When no object is active (0x44D8 == 0), look for another car
 * slot (0..7, not our own 0x11A0) on our course segment 0x11A2 whose flag bit
 * 1 is set and copy its 12-byte position block (0x16B0 + slot*0x40) to 0x44DA.
 * Then emit model 0x735 + frame (0x44D8 counts 0..11) at that position,
 * within 20000 of the camera. */
static uint32_t rd_car_carrier(void)
{
    regs_t r; regs_load(&r);
    charge(2);
    if (vrd16(A6W(0x1100))) { charge(1); regs_store(&r); return RD_RTS; }
    vwr8(A6W(0x11AA), vrd8(A6W(0x11AA)) & ~2u);
    charge(3);                                            /* bclr, tst, beq */
    int set = 1;
    if (vrd16(A6W(0x40B4))) { charge(2); if (!vrd16(A6W(0x47D4))) set = 0; }
    if (set) { charge(1); vwr8(A6W(0x11AA), vrd8(A6W(0x11AA)) | 2u); }
    charge(2);                                            /* tst, bne */
    if (vrd16(A6W(0x44D8)) == 0) {
        r.d[7] = 7; r.d[6] = 0;
        r.d[1] = setw(r.d[1], vrd16(A6W(0x11A2)));
        r.d[2] = 0x10;
        charge(5);                                        /* moveq, moveq, move, moveq, bset */
        int found = 0;
        for (;;) {
            charge(2);                                    /* cmp, beq */
            if ((uint16_t)r.d[7] != (uint16_t)vrd16(A6W(0x11A0))) {
                r.d[6] = setw(r.d[6], (uint16_t)(r.d[7] << 4));
                charge(4);                                /* move, lsl, cmp, bne */
                if ((uint16_t)r.d[1] == (uint16_t)vrd16(A6W(0x11AE) + (uint32_t)sxw(r.d[6]))) {
                    charge(2);                            /* btst, beq */
                    if (vrd8(A6W(0x11B6) + (uint32_t)sxw(r.d[6])) & 2) {
                        r.d[6] = setw(r.d[6], (uint16_t)(r.d[6] << 2));
                        charge(2);                        /* lsl, bra */
                        found = 1;
                        break;
                    }
                }
            }
            charge(1);                                    /* dbf */
            uint16_t c = (uint16_t)(r.d[7] - 1);
            r.d[7] = setw(r.d[7], c);
            if (c == 0xFFFF) break;
            poll();
        }
        if (!found) { charge(1); regs_store(&r); return RD_RTS; }
        vwr16(A6W(0x44D8), 0);
        r.a[0] = A6W(0x16B0) + (uint32_t)sxw(r.d[6]);
        r.a[1] = A6W(0x44DA);
        for (int i = 0; i < 6; i++) vwr16(r.a[1] + 2u * (uint32_t)i, vrd16(r.a[0] + 2u * (uint32_t)i));
        r.a[0] += 12; r.a[1] += 12;
        charge(9);
    }
    r.d[4] = 0;
    r.a[3] = vrd32(G_DL_CURSOR);
    r.d[2] = r.d[3] = r.d[5] = r.d[6] = r.d[7] = 0;
    r.d[0] = setw(r.d[0], vrd16(A6W(0x44DA)));
    charge(8);
    if (near_camera(&r, (uint16_t)vrd16(A6W(0x44DA)), (uint16_t)vrd16(A6W(0x44DE)), 0x4E20)) {
        rel_y(&r, (uint16_t)vrd16(A6W(0x44DC)));
        uint16_t f = (uint16_t)vrd16(A6W(0x44D8));
        r.d[0] = setw(r.d[0], f);
        vwr16(A6W(0x44D8), (uint16_t)(vrd16(A6W(0x44D8)) + 1));
        charge(4);                                        /* move, addq, cmpi, bne */
        if ((uint16_t)vrd16(A6W(0x44D8)) == 12) { charge(1); vwr16(A6W(0x44D8), 0); }
        r.d[0] = setw(r.d[0], (uint16_t)(f + 0x735));
        put_a3(&r, r.d[0]);
        charge(2);
        put_scaled_position(&r);
        r.d[0] = setw(r.d[0], vrd16(A6W(0x44E2)));
        charge(1);
        sin_cos(&r);
        put_a3(&r, r.d[1]); put_a3(&r, r.d[2]);
        put_a3(&r, 0); put_a3(&r, 0x7FFF); put_a3(&r, 4);
        charge(5);
    }
    vwr32(r.a[3], 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, r.a[3]);
    charge(3);                                            /* 2 moves + rts */
    regs_store(&r);
    return RD_RTS;
}

/* FUN_0002131a: on course 0 run FUN_000214c8 (the course-0 overlay), else
 * continue at 0x217D6 */
static uint32_t rd_course0_overlay(void)
{
    uint32_t r;
    charge(2);
    if (vrd16(G_COURSE) != 0) return 0x0217D6;
    charge(1);
    if ((r = rd_call(L_214C8, 0x021328))) return r;
    return RD_RTS;
}

/* FUN_0002132a: on course 4 run FUN_000213ee and continue at 0x21386, else
 * at 0x217D6 */
static uint32_t rd_course4_overlay(void)
{
    uint32_t r;
    charge(2);
    if (vrd16(G_COURSE) != 4) return 0x0217D6;
    charge(1);
    if ((r = rd_call(L_213EE, 0x021338))) return r;
    charge(1);
    return 0x021386;
}

/* FUN_0002133e: course 4 -> FUN_000218de then continue at 0x217D8;
 * course 8 -> FUN_0002184a; else nothing */
static uint32_t rd_course4_8_overlay(void)
{
    uint32_t r;
    uint16_t c = (uint16_t)vrd16(G_COURSE);
    charge(2);
    if (c == 4) {
        charge(1);
        if ((r = rd_call(L_218DE, 0x021354))) return r;
        charge(1);
        return 0x0217D8;
    }
    charge(2);
    if (c == 8) {
        charge(1);
        if ((r = rd_call(L_2184A, 0x02135E))) return r;
    }
    charge(1);                                            /* rts */
    return RD_RTS;
}

/* FUN_00021360: courses 5 and 6: FUN_00021446 then FUN_000215b8 (5) or
 * FUN_000215de (6) */
static uint32_t rd_course5_6_overlay(void)
{
    uint32_t r;
    uint16_t c = (uint16_t)vrd16(G_COURSE);
    charge(2);
    if (c != 5) {
        charge(2);
        if (c != 6) { charge(1); return RD_RTS; }
        charge(1);
        if ((r = rd_call(L_21446, 0x021380))) return r;
        charge(1);
        if ((r = rd_call(L_215DE, 0x021384))) return r;
        charge(1);
        return RD_RTS;
    }
    charge(1);
    if ((r = rd_call(L_21446, 0x021376))) return r;
    charge(1);
    if ((r = rd_call(L_215B8, 0x02137A))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_00021494: while the car (0x4846) is more than 0x1FF00 short of 0x204A,
 * raise 0x4346 by 0x90 (capped at 0x80000) and 0x434A by 0x10 */
static uint32_t rd_raise_4346(void)
{
    uint32_t d0 = vrd32(A6W(0x4846)) - 0x1FF00u;
    set_d(0, d0);
    charge(4);
    if ((int32_t)d0 >= (int32_t)vrd32(A6W(0x204A))) { charge(1); return RD_RTS; }
    uint32_t d1 = vrd32(A6W(0x4346)) + 0x90u;
    charge(4);
    if ((int32_t)d1 > 0x80000) { charge(1); d1 = 0x80000; }
    set_d(1, d1);
    vwr32(A6W(0x4346), d1);
    vwr16(A6W(0x434A), (uint16_t)(vrd16(A6W(0x434A)) + 0x10));
    charge(3);
    return RD_RTS;
}

/* the 8 words of an overlay object at 0x44C8: model, x, y, z, 3 angles, flags */
static void set_overlay_object(uint16_t model, uint16_t x, uint16_t y, uint16_t z,
                               uint16_t a0, uint16_t a1, uint16_t a2, uint16_t fl)
{
    vwr16(A6W(0x44C8), model); vwr16(A6W(0x44CA), x); vwr16(A6W(0x44CC), y); vwr16(A6W(0x44CE), z);
    vwr16(A6W(0x44D0), a0); vwr16(A6W(0x44D2), a1); vwr16(A6W(0x44D4), a2); vwr16(A6W(0x44D6), fl);
}

/* FUN_000214c8: course 0's two overlay objects. FUN_00021494, then a
 * blinking board (model 0x622.. or 0x632.. once the car is within 0xFF00
 * of 0x204A; 8 frames from bit 3..6 of the frame counter) at (-0x363B,
 * 0x168, 0x27AF), emitted by FUN_00021702; then a second one, model 0x5DE
 * or 0x5FC (0x7028 set) + a frame from the counter (frame/6 mod 15), at
 * (0x20D0, 0x230, -0x3250), emitted by continuing into FUN_00021702. */
static uint32_t rd_course0_boards(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_21494, 0x0214CA))) return r;
    uint16_t base = 0x622;
    uint32_t d0 = vrd32(A6W(0x4846)) - 0xFF00u;
    set_d(0, d0);
    charge(5);
    if ((int32_t)d0 < (int32_t)vrd32(A6W(0x204A))) { charge(1); base = 0x632; }
    set_d16(1, base);
    uint16_t m = (uint16_t)(((vrd16(G_FRAME) & 0x78) >> 3) + base);
    set_d16(0, m);
    set_overlay_object(m, (uint16_t)-0x363B, 0x168, 0x27AF, 0, 0x727E, 0, 4);
    charge(5 + 7 + 1);                                    /* 5, seven moves, bsr */
    if ((r = rd_call(L_21702, 0x021520))) return r;
    set_d16(1, 0x5DE);
    charge(3);                                            /* move, tst, bne */
    uint16_t d1 = 0x5DE;
    if (vrd16(A6W(0x7028)) == 0) {
        uint16_t d7 = (uint16_t)(vrd16(A6W(0x4846)) & 0xF);
        set_a(1, 0x21598);
        set_d16(7, d7);
        charge(5);                                        /* lea, move, andi, cmp, bne */
        if ((uint16_t)d_reg(6) == (uint16_t)vrd16(0x21598u + (uint32_t)sxw(d7) * 2u)) charge(2);
    } else { charge(1); d1 = 0x5FC; }
    set_d16(1, d1);
    uint32_t c = (uint16_t)vrd16(G_FRAME);
    c = (c / 6) & 0xFFFF;
    uint32_t q = c / 15, rem = c % 15;
    d0 = (q << 16) | rem;                                  /* after the swap */
    d0 = (d0 & 0xFFFF0000u) | (((rem & 0xF) + d1) & 0xFFFF);
    set_d(0, d0);
    set_overlay_object((uint16_t)d0, 0x20D0, 0x230, (uint16_t)-0x3250, 0, (uint16_t)-0xE48, 0, 4);
    charge(9 + 7 + 1);                                    /* 9, seven moves, bra */
    return 0x021702;
}

/* FUN_00021702: emit the overlay object at 0x44C8 (model, x, y, z, three
 * angles, flags & 7) as a three-angle record, when within 60000 of the camera.
 * The first and third angles are flipped by 0x8000. */
static uint32_t rd_overlay_object(void)
{
    regs_t r; regs_load(&r);
    r.d[4] = 0;
    r.a[3] = vrd32(G_DL_CURSOR);
    r.d[2] = r.d[3] = r.d[5] = r.d[6] = r.d[7] = 0;
    r.d[0] = setw(r.d[0], vrd16(A6W(0x44CA)));
    charge(8);
    if (near_camera(&r, (uint16_t)vrd16(A6W(0x44CA)), (uint16_t)vrd16(A6W(0x44CE)), 0xEA60)) {
        rel_y(&r, (uint16_t)vrd16(A6W(0x44CC)));
        r.d[0] = setw(r.d[0], vrd16(A6W(0x44C8)));
        put_a3(&r, r.d[0]);
        charge(2);
        put_scaled_xyz(&r);
        r.d[0] = setw(r.d[0], (uint16_t)(vrd16(A6W(0x44D0)) + 0x8000));
        charge(2);
        sin_cos(&r); put_a3(&r, r.d[1]); put_a3(&r, r.d[2]);
        r.d[0] = setw(r.d[0], vrd16(A6W(0x44D2)));
        charge(3);
        sin_cos(&r); put_a3(&r, r.d[1]); put_a3(&r, r.d[2]);
        r.d[0] = setw(r.d[0], (uint16_t)(vrd16(A6W(0x44D4)) + 0x8000));
        charge(4);
        sin_cos(&r); put_a3(&r, r.d[1]); put_a3(&r, r.d[2]);
        r.d[0] = (uint32_t)vrd16(A6W(0x44D6)) & 7;
        r.d[0] = (r.d[0]);
        put_a3(&r, r.d[0]);
        charge(5);                                        /* 2 move.l, move, andi.l, move.l */
    }
    vwr32(r.a[3], 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, r.a[3]);
    charge(3);
    regs_store(&r);
    return RD_RTS;
}

/* ======================= engine sound (0x220AA / 0x222E0) ======================= */

/* the table of level -> sound value at 0x225CE, indexed by |D0w| and capped
 * at the word 0x48C8 (lea, move, cmp, bcs [, move]) */
static uint16_t engine_level_lookup(regs_t *r, uint16_t idx)
{
    r->a[0] = 0x225CE;
    uint16_t v = (uint16_t)vrd16(0x225CEu + (uint32_t)sxw(idx) * 2u);
    charge(4);
    uint16_t cap = (uint16_t)vrd16(A6W(0x48C8));
    if (v >= cap) { charge(1); v = cap; }
    return v;
}

/* D1w = the sign of D0w as bit 1 (rol #2, andi #2), then D0w = |D0w|:
 * move, rol, andi, tst, bpl [, neg] */
static void sign_to_bit1(regs_t *r)
{
    uint16_t v = (uint16_t)r->d[0];
    r->d[1] = setw(r->d[1], (uint16_t)(((v << 2) | (v >> 14)) & 2));
    charge(5);
    if (v & 0x8000) { charge(1); r->d[0] = setw(r->d[0], (uint16_t)-v); }
}

/* FUN_000220aa: compute this frame's engine-sound value (D0w, a level
 * 0..0x3F through the table at 0x225CE) and pan bit (D1w bit 1), then continue
 * in FUN_000222e0, which sends them. 0x48C2 follows 0x0DD2 (a quarter of the
 * difference a frame, rounding up); while the 0x48C0 countdown runs the sound
 * follows the drift/skid (0x41D6, 0x4200) or boost (0x405E) states' own
 * values, else one of four formulas picked by 0x41E2 / 0x47BC / 0x4022.
 * (Names are provisional: the states are not yet identified.) */
static uint32_t rd_engine_sound(void)
{
    regs_t r; regs_load(&r);
    uint16_t d0 = (uint16_t)((vrd16(A6W(0x0DD2)) - vrd16(A6W(0x48C2))));
    d0 = (uint16_t)((int16_t)d0 >> 2);
    charge(4);                                            /* move, sub, asr, bmi */
    if (!(d0 & 0x8000)) { charge(1); d0++; }
    r.d[0] = setw(r.d[0], d0);
    vwr16(A6W(0x48C2), (uint16_t)(vrd16(A6W(0x48C2)) + d0));
    uint16_t t = (uint16_t)(vrd16(A6W(0x48C0)) - 1);
    vwr16(A6W(0x48C0), t);
    charge(3);                                            /* add, subq, bpl */
    int timed = 0;                                        /* 1: go to 0x2217C, 2: go to 0x2219E */
    if (t & 0x8000) {
        vwr16(A6W(0x48C0), 0);
        charge(3);                                        /* clr, tst, bne */
        if (vrd16(A6W(0x41D6))) {                         /* 0x22124 */
            uint16_t h = (uint16_t)(vrd16(A6W(0x41AA)) >> 1);
            r.d[0] = setw(r.d[0], h);
            vwr16(A6W(0x48BE), h);
            uint16_t a = (uint16_t)vrd16(A6W(0x41D8)), b = (uint16_t)vrd16(A6W(0x41DA));
            charge(5);                                    /* move, lsr, move, move, bpl */
            if (a & 0x8000) { charge(1); a = (uint16_t)-a; }
            charge(2);
            if (b & 0x8000) { charge(1); b = (uint16_t)-b; }
            r.d[1] = setw(r.d[1], (uint16_t)(b >> 8));
            r.d[0] = setw(r.d[0], (uint16_t)((a >> 8) + (b >> 8)));
            charge(3);
            goto skid_level;
        }
        charge(2);
        if (vrd16(A6W(0x4200))) {                         /* 0x22106 */
            vwr16(A6W(0x48BE), 4);
            uint16_t a = (uint16_t)vrd16(A6W(0x41D8)), b = (uint16_t)vrd16(A6W(0x41DA));
            charge(3);
            if (a & 0x8000) { charge(1); a = (uint16_t)-a; }
            charge(2);
            if (b & 0x8000) { charge(1); b = (uint16_t)-b; }
            r.d[1] = setw(r.d[1], (uint16_t)(b >> 7));
            r.d[0] = setw(r.d[0], (uint16_t)((a >> 7) + (b >> 7)));
            charge(4);                                    /* lsr, lsr, add, bra */
            goto skid_level;
        }
        charge(2);
        if (vrd16(A6W(0x405E))) {                         /* 0x2215C */
            vwr16(A6W(0x41AC), vrd16(A6W(0x40F2)));
            vwr16(A6W(0x48BC), 0x30);
            vwr16(A6W(0x48BE), 1);
            uint16_t f = (uint16_t)(vrd16(G_FRAME) & 3);
            r.d[0] = setw(r.d[0], f);
            charge(6);
            if (f) timed = 2;
            else { charge(1); timed = 1; }
        } else {
            charge(2);
            if (vrd16(A6W(0x47D4))) {                     /* 0x220E6 */
                uint16_t v = (uint16_t)(vrd16(A6W(0x47BC)) << 4);
                charge(4);
                if (v >= 0x3F) { charge(1); v = 0x3F; }
                r.d[0] = setw(r.d[0], v);
                vwr16(A6W(0x48BC), 0x30);
                vwr16(A6W(0x48BE), 5);
                charge(3);
            } else charge(1);                             /* bra */
            timed = 1;
        }
        if (0) {
skid_level:
            charge(2);                                    /* cmpi, bcs */
            if ((uint16_t)r.d[0] >= 0x3F) { charge(1); r.d[0] = setw(r.d[0], 0x3F); }
            vwr16(A6W(0x48BC), (uint16_t)r.d[0]);
            vwr16(A6W(0x48C0), 0x1E);
            charge(3);
            timed = 1;
        }
    } else timed = 2;

    if (timed == 1) {                                     /* 0x2217C: a timed sound */
        uint16_t n = (uint16_t)vrd16(A6W(0x48BE));
        r.d[0] = setw(r.d[0], n);
        charge(2);
        if (n) {
            vwr16(A6W(0x48BE), (uint16_t)(n - 1));
            uint16_t d1 = (uint16_t)(vrd16(A6W(0x41AC)) - vrd16(A6W(0x20A0)));
            r.d[0] = setw(r.d[0], vrd16(A6W(0x48BC)));
            r.d[1] = setw(r.d[1], (uint16_t)(((d1 << 2) | (d1 >> 14)) & 2));
            charge(7);
            regs_store(&r);
            return 0x0222E0;
        }
    }
    /* 0x2219E: the continuous engine sound */
    charge(2);
    if ((uint16_t)vrd16(A6W(0x41E2)) >= 0x3000) {        /* 0x221B6 */
        r.d[0] = setw(r.d[0], (uint16_t)((int16_t)(uint16_t)(0 - vrd16(A6W(0x0DD2))) >> 5));
        charge(3);                                        /* move, sub, asr */
        sign_to_bit1(&r);
        r.d[0] = setw(r.d[0], engine_level_lookup(&r, (uint16_t)r.d[0]));
        charge(1);
        regs_store(&r);
        return 0x0222E0;
    }
    charge(2);
    if (vrd16(A6W(0x47BC))) {                             /* 0x221E4 */
        r.d[0] = setw(r.d[0], (uint16_t)((int16_t)(uint16_t)(vrd16(A6W(0x48C2)) - vrd16(A6W(0x0DD2))) >> 4));
        charge(3);
        sign_to_bit1(&r);
        r.d[0] = setw(r.d[0], engine_level_lookup(&r, (uint16_t)r.d[0]));
        charge(1);
        regs_store(&r);
        return 0x0222E0;
    }
    charge(2);
    if (vrd16(A6W(0x4022)) == 0) {                        /* 0x22212 */
        r.d[0] = setw(r.d[0], (uint16_t)((int16_t)(uint16_t)(vrd16(A6W(0x48C2)) - vrd16(A6W(0x0DD2))) >> 1));
        charge(3);
        sign_to_bit1(&r);
        r.d[0] = setw(r.d[0], engine_level_lookup(&r, (uint16_t)r.d[0]));
        charge(1);
        regs_store(&r);
        return 0x0222E0;
    }
    charge(1);                                            /* bra 0x22240 */
    uint16_t v = (uint16_t)vrd16(A6W(0x408A));
    for (int i = 0; i < 6; i++) v = (uint16_t)(v - vrd16(A6W(0x0DD2)));
    v = (uint16_t)(v - vrd16(A6W(0x4088)));
    r.d[1] = setw(r.d[1], 5);
    r.d[2] = setw(r.d[2], vrd16(A6W(0x48CC)));
    charge(12);                                           /* move, 7 sub, move, move, cmpi, bcc */
    if ((uint16_t)vrd16(A6W(0x40B4)) < 0x8000) charge(1); /* bra */
    v = (uint16_t)((int16_t)v >> 5);
    v = (uint16_t)(v - (uint16_t)r.d[2]);
    r.d[0] = setw(r.d[0], v);
    charge(2);                                            /* asr, sub */
    sign_to_bit1(&r);
    uint16_t lvl = (uint16_t)(r.d[0] - 4);
    charge(2);
    if (lvl & 0x8000) { charge(1); lvl = 0; }
    r.d[0] = setw(r.d[0], lvl);
    uint16_t sp = (uint16_t)vrd16(A6W(0x4086));
    charge(2);
    if (sp & 0x8000) { charge(1); sp = (uint16_t)-sp; }
    uint16_t d3 = (uint16_t)(vrd16(A6W(0x4022)) >> 12);
    r.d[3] = setw(r.d[3], d3);
    charge(7);                                            /* lsr, move, lsr, lsr, add, btst, beq */
    if (vrd8(A6W(0x0C4D)) & 2) charge(1);                 /* move #0 */
    charge(2);
    if ((uint16_t)vrd16(A6W(0x4022)) < 0xE00) charge(1);  /* clr */
    r.d[2] = setw(r.d[2], 0);                             /* ...and cleared again: the speed term is unused */
    r.d[0] = setw(r.d[0], (uint16_t)(lvl & 0xFF));
    charge(3);                                            /* clr, add, andi */
    r.d[0] = setw(r.d[0], engine_level_lookup(&r, (uint16_t)r.d[0]));
    charge(1);
    regs_store(&r);
    return 0x0222E0;
}

/* FUN_000222e0: send the engine sound: D0w = level, D1w bit 1 = pan. The pan
 * flips on course 3 of the mirror mode (0x2394, 0xDA0 == 3) and in the rear
 * view (0xDDE); the 0x48BA countdown halves the level while it runs. The byte
 * 0x4345 gets the pan bits and, in bits 2..7, the complement of the level
 * with its bits reversed (D2w: bit 7-n of the level -> bit n). */
static uint32_t rd_engine_sound_send(void)
{
    regs_t r; regs_load(&r);
    charge(2);
    if (vrd16(A6W(0x2394))) {
        charge(2);
        if (vrd16(A6W(0x0DA0)) == 3) { charge(1); r.d[1] ^= 2; }
    }
    uint16_t cnt = (uint16_t)vrd16(A6W(0x48BA));
    r.d[2] = setw(r.d[2], cnt);
    charge(2);
    if (cnt) {
        cnt--;
        r.d[2] = setw(r.d[2], cnt);
        vwr16(A6W(0x48BA), cnt);
        r.d[0] = setw(r.d[0], (uint16_t)((uint16_t)r.d[0] >> 1));
        charge(3);
    }
    charge(2);
    if (vrd16(A6W(0x0DDE))) { charge(1); r.d[1] ^= 2; }
    r.d[2] = setw(r.d[2], 0);
    r.d[6] = setw(r.d[6], 7);
    r.d[5] = setw(r.d[5], 0);
    charge(3);
    for (;;) {
        charge(2);                                        /* btst, beq */
        if (r.d[0] & (1u << (r.d[6] & 31))) { charge(1); r.d[2] |= 1u << (r.d[5] & 31); }
        r.d[5] = setw(r.d[5], r.d[5] + 1);
        charge(2);                                        /* addq, dbf */
        uint16_t c = (uint16_t)(r.d[6] - 1);
        r.d[6] = setw(r.d[6], c);
        if (c == 0xFFFF) break;
        poll();
    }
    uint16_t m = (uint16_t)((~r.d[2]) & 0xFFFC);
    r.d[2] = setw(r.d[2], m);
    r.d[1] = setw(r.d[1], (uint16_t)(r.d[1] | m));
    vwr8(A6W(0x4345), (uint8_t)r.d[1]);
    charge(4);
    regs_store(&r);
    return RD_RTS;
}

/* ======================= attract course preview (0x227D2..0x22C9E) ======================= */

#define CALL(fn, ret) do { uint32_t r_ = rd_call(fn, ret); if (r_) return r_; } while (0)

/* FUN_000227d2: set the screen-window block at A6 (mode 0xD, 7, 0, 0, size
 * 0x140 x 0xF0), display-list priority 0, and run FUN_0000b56e */
static uint32_t rd_window_full(void)
{
    vwr16(A6W(0x00), 0xD);
    vwr16(A6W(0x28), 7);
    vwr16(A6W(0x2A), 0);
    vwr16(A6W(0x2C), 0);
    vwr16(A6W(0x1E), 0x140);
    vwr16(A6W(0x20), 0xF0);
    vwr16(G_DL_PRIO, 0);
    charge(8);
    CALL(L_B56E, 0x022800);
    return RD_RTS;
}

/* one step of "approach target by 1/16 (rounding the positive side up)" on
 * the word at a: move #t, move, sub, beq [, asr, bmi [, addq], add] */
static void approach_16(uint32_t a, uint16_t target, regs_t *r)
{
    uint16_t cur = (uint16_t)vrd16(a);
    uint16_t d = (uint16_t)(target - cur);
    r->d[1] = setw(r->d[1], cur);
    r->d[0] = setw(r->d[0], d);
    charge(4);
    if (!d) return;
    d = (uint16_t)((int16_t)d >> 4);
    charge(2);
    if (!(d & 0x8000)) { charge(1); d++; }
    r->d[0] = setw(r->d[0], d);
    vwr16(a, (uint16_t)(cur + d));
    charge(1);
}

/* FUN_00022802: ease the screen window at A6 toward (0x96, 0x6E, 0xA0 x 0x78)
 * by 1/16 a frame (the attract picture-in-picture shrinking), mode 0xD,
 * priority 0, then FUN_0000b56e */
static uint32_t rd_window_ease(void)
{
    regs_t r; regs_load(&r);
    r.a[2] = A6W(0);
    vwr16(A6W(0x28), 0);
    r.d[4] = setw(r.d[4], 0);
    charge(4);
    approach_16(A6W(0x2A), 0x96, &r);
    approach_16(A6W(0x2C), 0x6E, &r);
    approach_16(A6W(0x1E), 0xA0, &r);
    approach_16(A6W(0x20), 0x78, &r);
    vwr16(A6W(0x00), 0xD);
    vwr16(G_DL_PRIO, 0);
    charge(3);
    regs_store(&r);
    CALL(L_B56E, 0x02287A);
    return RD_RTS;
}

/* FUN_0002287c: start the attract course preview: course 0 variant 0 at
 * 0x70000, flags 0x46DE = 0, 0x1100 = -1, 0x2342 = 1, 0x478A = 0x7000;
 * FUN_0001025a; then continue in FUN_00022a2c */
static uint32_t rd_preview_start(void)
{
    vwr16(A6W(0x46DE), 0);
    vwr16(A6W(0x1100), 0xFFFF);
    vwr16(A6W(0x2342), 1);
    vwr16(G_COURSE, 0);
    vwr16(G_COURSE_VAR, 0);
    vwr32(A6W(0x204A), 0x70000);
    vwr16(A6W(0x478A), 0x7000);
    charge(8);
    CALL(L_1025A, 0x0228AE);
    charge(1);
    return 0x022A2C;
}

/* FUN_00022a2c: set up the course preview: clear the text layer, screen
 * mode 1 (FUN_0000c69a once), the preview's subsystems, a 0x708-frame
 * timer at 0x2330, then the first page (FUN_00022c3a), and step 0x232E */
static uint32_t rd_preview_setup(void)
{
    charge(1); CALL(L_575A, 0x022A32);
    charge(2);
    if (vrd16(A6W(0x0DE2)) != 1) {
        vwr16(A6W(0x0DE2), 1);
        charge(2); CALL(L_C69A, 0x022A46);
    }
    charge(1); CALL(L_28BBE, 0x022A4C);
    charge(1); CALL(L_1DFC6, 0x022A52);
    charge(1); CALL(L_1F6D4, 0x022A58);
    charge(1); CALL(L_22A98, 0x022A5E);
    charge(1); CALL(L_5650, 0x022A64);
    charge(1); CALL(L_5700, 0x022A6A);
    vwr16(A6W(0x2330), 0x708);
    vwr16(A6W(0x46F6), 0);
    vwr16(A6W(0x46E8), 0);
    charge(4); CALL(L_F9CA, 0x022A82);
    charge(1); CALL(L_B8B0, 0x022A88);
    charge(1); CALL(L_E6E0, 0x022A8E);
    charge(1); CALL(L_22C3A, 0x022A92);
    vwr16(A6W(0x232E), (uint16_t)(vrd16(A6W(0x232E)) + 1));
    charge(2);
    return RD_RTS;
}

/* FUN_00022a98: FUN_000186c6; the preview's camera script = the table at
 * 0x23712 [0x46DE]; 0x4A94 = 0x40, 0x4A96 = 0xE, clear 0x1162; FUN_00018702 */
static uint32_t rd_preview_camera(void)
{
    charge(1); CALL(L_186C6, 0x022A9E);
    uint16_t i = (uint16_t)vrd16(A6W(0x46DE));
    set_d16(0, i);
    uint32_t a0 = vrd32(0x23712u + (uint32_t)sxw(i) * 4u);
    set_a(0, a0);
    vwr32(A6W(0x4742), a0);
    vwr16(A6W(0x4A94), 0x40);
    vwr16(A6W(0x4A96), 0xE);
    vwr16(A6W(0x1162), 0);
    charge(8); CALL(L_18702, 0x022AC4);
    return RD_RTS;
}

/* copy the 16-byte camera key at a0 + off to 0x46E0.. (8 moves) */
static void preview_key(uint32_t a)
{
    static const uint16_t dst[8] = { 0x46E0, 0x46E4, 0x46E6, 0x46EA, 0x46EC, 0x46EE, 0x46F0, 0x46F2 };
    for (int i = 0; i < 8; i++) vwr16(A6W(dst[i]), vrd16(a + 2u * (uint32_t)i));
}

/* FUN_00022ac6: one frame of the attract course preview. Counts down the
 * 0x2330 timer (0x700: drop bit 0 of 0x11A4; every step 2..3 of 0x232E ends
 * early while 0x202A is set) and steps 0x232E when it ends. Otherwise: the
 * page's frame work (FUN_000103c4, FUN_00028bc8, FUN_00022c9e), the camera
 * key for this 128-frame slot of the variant's key table (0x22EF2), replaced
 * by key 15 when the followed car (0x46E4) is off the road; its speed and
 * followed-car block; then the frame's renderers; and the music cue
 * 0x46E0 (FUN_0000f9ca / FUN_0000f9ee) when it changes. */
static uint32_t rd_preview_frame(void)
{
    charge(2);
    if (vrd16(A6W(0x2330)) == 0x700) { charge(1); vwr8(A6W(0x11A4), vrd8(A6W(0x11A4)) & ~1u); }
    charge(2);
    if (vrd16(A6W(0x202A))) {
        uint16_t ph = (uint16_t)((vrd16(A6W(0x232E)) >> 2) & 3);
        set_d16(0, ph);
        charge(5);
        if ((int16_t)ph >= 2) {
            vwr8(A6W(0x11A4), vrd8(A6W(0x11A4)) & ~1u);
            vwr16(A6W(0x232E), (uint16_t)(vrd16(A6W(0x232E)) + 1));
            charge(3);
            return RD_RTS;
        }
    }
    uint16_t t = (uint16_t)(vrd16(A6W(0x2330)) - 1);
    vwr16(A6W(0x2330), t);
    charge(2);
    if (t == 0) {
        vwr16(A6W(0x232E), (uint16_t)(vrd16(A6W(0x232E)) + 1));
        charge(2);
        return RD_RTS;
    }
    charge(1); CALL(L_103C4, 0x022B08);
    charge(1); CALL(L_28BC8, 0x022B0E);
    charge(1); CALL(L_22C9E, 0x022B12);
    uint16_t slot = (uint16_t)vrd16(A6W(0x46F6));
    vwr16(A6W(0x46F6), (uint16_t)(slot + 1));
    uint16_t d1 = (uint16_t)(((slot >> 7) & 0xF) << 4);
    uint16_t d0 = (uint16_t)vrd16(A6W(0x46DE));
    uint32_t a0 = vrd32(0x22EF2u + (uint32_t)sxw(d0) * 4u);
    preview_key(a0 + (uint32_t)sxw(d1));
    charge(16);                                           /* move, addq, lsr, andi, lsl, lea, move, movea, 8 moves */
    d0 = (uint16_t)(vrd16(A6W(0x46E4)) << 6);
    uint16_t v = (uint16_t)vrd16(A6W(0x16BA) + (uint32_t)sxw(d0));
    charge(4);                                            /* move, lsl, move, bpl */
    if (v & 0x8000) { charge(1); v = (uint16_t)-v; }
    charge(2);                                            /* cmpi, bcc */
    int off_road = v >= 0x800;
    if (!off_road) {
        v = (uint16_t)vrd16(A6W(0x16B6) + (uint32_t)sxw(d0));
        charge(3);                                        /* move, cmpi, bge */
        off_road = (int16_t)v < -0x400;
    }
    if (off_road) {
        v = 0xF0;
        preview_key(a0 + 0xF0);
        charge(9);
    }
    d1 = v;
    uint16_t sp = (uint16_t)vrd16(A6W(0x46F2));
    charge(2);
    if (!sp) { charge(1); vwr16(A6W(0x46E8), 0); }
    vwr16(A6W(0x46E8), (uint16_t)(vrd16(A6W(0x46E8)) + sp));
    d1 = 0;
    charge(4);                                            /* add, move #0, tst, beq */
    if (vrd16(A6W(0x46F0))) { charge(2); d1 = (uint16_t)(vrd16(A6W(0x46E4)) << 6); }
    vwr16(A6W(0x46C8), d1);
    vwr16(G_DL_PRIO, 0);
    set_d16(0, sp); set_d16(1, d1); set_a(0, a0);
    charge(3); CALL(L_1800E, 0x022BE4);
    charge(1); CALL(L_19DAC, 0x022BEA);
    charge(1); CALL(L_17B64, 0x022BF0);
    charge(1); CALL(L_180DA, 0x022BF6);
    charge(1); CALL(L_1F1DA, 0x022BFC);
    charge(1); CALL(L_1FE38, 0x022C02);
    charge(1); CALL(L_2077C, 0x022C08);
    charge(1); CALL(L_146AE, 0x022C0E);
    charge(1); CALL(L_B56E, 0x022C14);
    charge(1); CALL(L_14EB6, 0x022C1A);
    uint16_t cue = (uint16_t)vrd16(A6W(0x46E0));
    set_d16(0, cue);
    charge(3);                                            /* move, cmp, beq */
    if (cue == (uint16_t)vrd16(A6W(0x46E2))) { charge(1); return RD_RTS; }
    vwr16(A6W(0x46E2), cue);
    charge(2);                                            /* move, bmi */
    if (cue & 0x8000) { charge(1); CALL(L_F9EE, 0x022C38); }
    else { charge(1); CALL(L_F9CA, 0x022C30); charge(1); }
    charge(1);
    return RD_RTS;
}

/* FUN_00022c3a: the preview's page overlay, by step (0x232E >> 2) & 3 through
 * the jump table at 0x22C50: steps 0/1 nothing; 2 and 3 draw the text block
 * at 0x22CE2 (priority 2: FUN_0000b4e4, FUN_0000b56e, FUN_0000b6d0); step 3
 * also FUN_00028116. */
static uint32_t rd_preview_page(void)
{
    uint16_t i = (uint16_t)((vrd16(A6W(0x232E)) >> 2) & 3);
    uint16_t o = (uint16_t)vrd16(0x22C50u + (uint32_t)sxw(i) * 2u);
    set_d16(0, o);
    charge(7);                                            /* move, lsr, andi, move, nop, nop, jmp */
    poll();
    if (i < 2) { charge(1); return RD_RTS; }
    vwr16(G_DL_PRIO, 2);
    set_a(0, 0x22CE2);
    charge(3); CALL(L_B4E4, i == 2 ? 0x022C6C : 0x022C8A);
    charge(1); CALL(L_B56E, i == 2 ? 0x022C72 : 0x022C90);
    charge(1); CALL(L_B6D0, i == 2 ? 0x022C78 : 0x022C96);
    if (i == 3) { charge(1); CALL(L_28116, 0x022C9C); }
    charge(1);
    return RD_RTS;
}

/* FUN_00022c9e: the preview's per-frame page work, by step (0x232E >> 2) & 3
 * through the table at 0x22CB4: 0/1 nothing; 2: FUN_0000e774, priority 2,
 * FUN_000306cc, FUN_0000b56e; 3: FUN_00028136. */
static uint32_t rd_preview_page_work(void)
{
    uint16_t i = (uint16_t)((vrd16(A6W(0x232E)) >> 2) & 3);
    uint16_t o = (uint16_t)vrd16(0x22CB4u + (uint32_t)sxw(i) * 2u);
    set_d16(0, o);
    charge(7);
    poll();
    if (i < 2) { charge(1); return RD_RTS; }
    if (i == 3) { charge(1); CALL(L_28136, 0x022CE0); charge(1); return RD_RTS; }
    charge(1); CALL(L_E774, 0x022CC6);
    vwr16(G_DL_PRIO, 2);
    charge(2); CALL(L_306CC, 0x022CD2);
    charge(1); CALL(L_B56E, 0x022CD8);
    charge(1);
    return RD_RTS;
}

/* ======================= attract preview camera / car markers (0x24818..0x24D68) ======================= */

/* FUN_00024818: aim the preview camera. With a key (0x46E0 >= 0): copy the
 * followed car's (0x46E4) position block (0x16AE + car*0x40: segment, x, y+0x46EA,
 * z, then pitch/heading/roll negated, heading +0x8000 unless the key's type is 2
 * -- then the car's 0x4AAC word) into the camera words 0x20D8..0x21A8, mark
 * the current viewport block (A6 + priority*0x80) active, and offset it by
 * FUN_000248b2. Without a key the camera follows the course path (FUN_00024912). */
static uint32_t rd_preview_aim(void)
{
    charge(2);
    if (vrd16(A6W(0x46E0)) & 0x8000) {
        charge(1); CALL(L_24912, 0x024902);
        uint16_t d0 = (uint16_t)(vrd16(G_DL_PRIO) << 7);
        set_d16(0, d0);
        set_a(1, A6W(0) + (uint32_t)sxw(d0));
        vwr16(A6W(0) + (uint32_t)sxw(d0), 1);
        charge(4);
        return RD_RTS;
    }
    uint16_t d4 = (uint16_t)(vrd16(A6W(0x46E4)) << 6);
    uint16_t d0 = (uint16_t)(vrd16(G_DL_PRIO) << 7);
    uint32_t car = (uint32_t)sxw(d4);
    set_d16(4, d4);
    set_a(1, A6W(0) + (uint32_t)sxw(d0));
    vwr16(A6W(0x20E8), vrd16(A6W(0x4AAA) + car));
    vwr16(A6W(0x20D8), vrd16(A6W(0x16AE) + car));
    vwr16(G_CAM_X, vrd16(A6W(0x16B0) + car));
    vwr16(G_CAM_Y, (uint16_t)(vrd16(A6W(0x16B2) + car) + vrd16(A6W(0x46EA))));
    vwr16(G_CAM_Z, vrd16(A6W(0x16B4) + car));
    vwr16(A6W(0x2188), (uint16_t)-(uint16_t)(vrd16(A6W(0x16B6) + car) + vrd16(A6W(0x46EE))));
    uint16_t hd = (uint16_t)(0x8000 - vrd16(A6W(0x4AAC) + car));
    uint16_t d1 = (uint16_t)(vrd16(A6W(0x46E0)) & 0xFFF);
    set_d16(1, d1);
    charge(5 + 2 + 2 + 3 + 2 + 4 + 3 + 4);
    if (d1 != 2) { charge(3); hd = (uint16_t)(0x8000 - vrd16(A6W(0x16B8) + car)); }
    vwr16(A6W(0x2198), hd);
    d0 = (uint16_t)-(uint16_t)vrd16(A6W(0x16BA) + car);
    set_d16(0, d0);
    vwr16(A6W(0x21A8), d0);
    vwr16(a_reg(1), 1);
    charge(1 + 3 + 1 + 1);                                /* move, 3, move, bsr */
    CALL(L_248B2, 0x0248B0);
    return RD_RTS;
}

/* FUN_000248b2: turn the camera by 0x46E6 + 0x46E8 about Y and move it
 * 0x46EC along the new heading (x -= .., z += ..), 16 up, with no roll */
static uint32_t rd_preview_orbit(void)
{
    uint16_t d1 = (uint16_t)(vrd16(A6W(0x46E6)) + vrd16(A6W(0x46E8)));
    uint16_t hd = (uint16_t)(vrd16(A6W(0x2198)) + d1);
    vwr16(A6W(0x2198), hd);
    uint16_t a = (uint16_t)((uint16_t)-hd & 0xFFFE);
    uint32_t a5 = a_reg(5);
    uint16_t s = (uint16_t)-trig(a5, a);
    a = (uint16_t)(a + 0x4000);
    uint16_t c = trig(a5, a);
    uint16_t dist = (uint16_t)vrd16(A6W(0x46EC));
    uint32_t d2 = muls_w(c, dist); d2 += d2; d2 = (d2 << 16) | (d2 >> 16);
    vwr16(G_CAM_Z, (uint16_t)(vrd16(G_CAM_Z) + d2));
    uint32_t d1l = muls_w(s, dist); d1l += d1l; d1l = (d1l << 16) | (d1l >> 16);
    vwr16(G_CAM_X, (uint16_t)(vrd16(G_CAM_X) + d1l));
    vwr16(G_CAM_Y, (uint16_t)(vrd16(G_CAM_Y) + 0x10));
    vwr16(A6W(0x21A8), 0);
    set_d16(0, a);
    set_d(1, d1l);
    set_d(2, d2);
    set_d16(3, dist);
    charge(22);
    return RD_RTS;
}

/* FUN_00024d68: animate the three course-path markers: marker n (2..0) is
 * the base point (table 0x24DE2 [variant]) plus the direction vector (0x24DF2)
 * times (cos(phase_n) + 1) / 2, stored at 0x470E/0x471E/0x472E + n*2; each
 * phase 0x46FE + n*2 advances by 0x20 a frame. */
static uint32_t rd_path_markers(void)
{
    regs_t r; regs_load(&r);
    r.a[0] = 0x24DE2; r.a[1] = 0x24DF2;
    r.d[0] = setw(r.d[0], vrd16(G_COURSE_VAR));
    r.a[0] = vrd32(0x24DE2u + (uint32_t)sxw(r.d[0]) * 4u);
    r.a[1] = vrd32(0x24DF2u + (uint32_t)sxw(r.d[0]) * 4u);
    r.d[7] = setw(r.d[7], 2);
    charge(6);
    for (;;) {
        int32_t n = sxw(r.d[7]);
        uint16_t ph = (uint16_t)vrd16(A6W(0x46FE) + (uint32_t)(n * 2));
        uint16_t k = trig(r.a[5], (uint16_t)((ph & 0xFFFE) + 0x4000));
        k = (uint16_t)((uint16_t)(k + 0x8000) >> 1);
        r.d[0] = setw(r.d[0], k);
        uint32_t v = r.a[1] + (uint32_t)(n * 8), b = r.a[0] + (uint32_t)(n * 8);
        r.d[1] = setw(r.d[1], vrd16(v));
        r.d[2] = setw(r.d[2], vrd16(v + 2));
        r.d[3] = setw(r.d[3], vrd16(v + 4));
        for (int j = 1; j <= 3; j++) {
            uint32_t p = muls_w(r.d[j], k); p += p; r.d[j] = (p << 16) | (p >> 16);
        }
        r.d[1] = setw(r.d[1], r.d[1] + vrd16(b));
        r.d[2] = setw(r.d[2], r.d[2] + vrd16(b + 2));
        r.d[3] = setw(r.d[3], r.d[3] + vrd16(b + 4));
        vwr16(A6W(0x470E) + (uint32_t)(n * 2), (uint16_t)r.d[1]);
        vwr16(A6W(0x471E) + (uint32_t)(n * 2), (uint16_t)r.d[2]);
        vwr16(A6W(0x472E) + (uint32_t)(n * 2), (uint16_t)r.d[3]);
        vwr16(A6W(0x46FE) + (uint32_t)(n * 2), (uint16_t)(vrd16(A6W(0x46FE) + (uint32_t)(n * 2)) + 0x20));
        charge(26);
        uint16_t c = (uint16_t)(r.d[7] - 1);
        r.d[7] = setw(r.d[7], c);
        if (c == 0xFFFF) break;
        poll();
    }
    regs_store(&r);
    return RD_RTS;
}

/* FUN_00024912: the preview camera on the course path. FUN_00024d68 first;
 * then, from the variant's list (0x24AC0: segment, height, flip, marker; 8
 * bytes, segment < 0 ends), the first entry within 100 segments of the
 * followed car's. Its segment, height, flip and marker go to 0x46D6..0x46DC
 * (a new segment resets the four marker phases at 0x46FE and 0x46FC = -1).
 * With a marker the camera sits on that path marker (position resolved by
 * FUN_000143b6), else on the track edge at that segment. Then it turns toward
 * the car (FUN_0000a98c on the difference; eased by 1/8 unless just reset). */
static uint32_t rd_preview_path(void)
{
    charge(1); CALL(L_24D68, 0x024916);
    regs_t r; regs_load(&r);
    r.d[0] = setw(r.d[0], vrd16(G_COURSE_VAR));
    r.a[1] = vrd32(0x24AC0u + (uint32_t)sxw(r.d[0]) * 4u);
    charge(3);
    for (;;) {
        uint16_t seg = (uint16_t)vrd16(r.a[1]);
        r.d[0] = setw(r.d[0], seg);
        charge(2);
        if (seg & 0x8000) { charge(1); regs_store(&r); return RD_RTS; }
        r.d[1] = setw(r.d[1], seg);
        uint16_t cs = (uint16_t)vrd16(A6W(0x16AE) + (uint32_t)sxw((uint16_t)((vrd16(A6W(0x46E4)) & 0xF) << 6)));
        r.d[2] = setw(r.d[2], cs);
        uint16_t d = (uint16_t)(seg - cs);
        r.d[0] = setw(r.d[0], d);
        charge(7);
        if (d & 0x8000) { charge(1); d = (uint16_t)-d; r.d[0] = setw(r.d[0], d); }
        charge(2);
        if (d < 0x64) break;
        r.a[1] += 8;
        charge(2);
        poll();
    }
    uint16_t car = (uint16_t)((vrd16(A6W(0x46E4)) & 0xF) << 6);
    uint16_t crs = (uint16_t)vrd16(A6W(0x4AAA) + (uint32_t)sxw(car));
    vwr16(A6W(0x20E8), crs);
    uint32_t tbl = 0xECDCu + (uint32_t)sxw((uint16_t)(crs << 6));
    r.d[0] = setw(r.d[0], (uint16_t)(crs << 6));
    r.a[2] = vrd32(tbl + 0x0C); r.a[3] = vrd32(tbl + 0x14);
    r.a[4] = vrd32(tbl + 0x20); r.a[0] = vrd32(tbl + 0x10);
    for (int i = 0; i < 4; i++) vwr16(A6W(0x46D6) + 2u * (uint32_t)i, vrd16(r.a[1] + 2u * (uint32_t)i));
    uint16_t d6 = (uint16_t)vrd16(A6W(0x46D6));
    r.d[6] = setw(r.d[6], d6);
    vwr16(A6W(0x20D8), d6);
    vwr16(A6W(0x46FC), 0);
    charge(11 + 4 + 5);
    if (d6 != (uint16_t)vrd16(A6W(0x46FA))) {
        vwr32(A6W(0x46FE), 0x28002800u); vwr32(A6W(0x4702), 0x28002800u);
        vwr32(A6W(0x4706), 0x28002800u); vwr32(A6W(0x470A), 0x28002800u);
        vwr16(A6W(0x46FC), 0xFFFF);
        charge(5);
    }
    vwr16(A6W(0x46FA), vrd16(A6W(0x20D8)));
    uint16_t mk = (uint16_t)vrd16(A6W(0x46DC));
    r.d[0] = setw(r.d[0], mk);
    charge(3);
    if (mk) {
        r.d[0] -= 1;
        int32_t m = sxw(r.d[0]);
        r.d[4] = setw(r.d[4], vrd16(A6W(0x470E) + (uint32_t)(m * 2)));
        r.d[3] = setw(r.d[3], vrd16(A6W(0x471E) + (uint32_t)(m * 2)));
        r.d[5] = setw(r.d[5], vrd16(A6W(0x472E) + (uint32_t)(m * 2)));
        vwr16(A6W(0x208C), (uint16_t)r.d[4]);
        vwr16(A6W(0x208E), (uint16_t)r.d[3]);
        vwr16(A6W(0x2090), (uint16_t)r.d[5]);
        vwr16(A6W(0x204C), vrd16(A6W(0x20D8)));
        charge(9);
        regs_store(&r);
        CALL(L_143B6, 0x0249FC);
        regs_load(&r);
        vwr16(A6W(0x20D8), vrd16(A6W(0x204C)));
        r.d[4] = setw(r.d[4], vrd16(A6W(0x208C)));
        r.d[3] = setw(r.d[3], vrd16(A6W(0x208E)));
        r.d[5] = setw(r.d[5], vrd16(A6W(0x2090)));
        vwr16(G_CAM_X, (uint16_t)r.d[4]);
        vwr16(G_CAM_Y, (uint16_t)r.d[3]);
        vwr16(G_CAM_Z, (uint16_t)r.d[5]);
        charge(8);
    } else {
        int32_t i6 = sxw(r.d[6]);
        uint16_t a = (uint16_t)(((uint16_t)(0x4000 - vrd16(r.a[3] + (uint32_t)(i6 * 2)))) & 0xFFFE);
        uint16_t s = (uint16_t)-trig(r.a[5], a);
        a = (uint16_t)(a + 0x4000);
        uint16_t c = trig(r.a[5], a);
        r.d[0] = setw(r.d[0], a);
        uint16_t ofs = (uint16_t)vrd16(r.a[4] + (uint32_t)(i6 * 4));
        r.d[4] = muls_w(s, ofs);
        r.d[5] = muls_w(c, ofs);
        charge(12);
        if (vrd16(A6W(0x46DA))) { charge(2); r.d[4] = 0u - r.d[4]; r.d[5] = 0u - r.d[5]; }
        r.d[4] = (r.d[4] << 16) | (r.d[4] >> 16);
        r.d[5] = (r.d[5] << 16) | (r.d[5] >> 16);
        r.d[4] = setw(r.d[4], r.d[4] + vrd16(r.a[2] + (uint32_t)(i6 * 4)));
        r.d[5] = setw(r.d[5], r.d[5] + vrd16(r.a[2] + (uint32_t)(i6 * 4) + 2));
        vwr16(G_CAM_X, (uint16_t)r.d[4]);
        vwr16(G_CAM_Z, (uint16_t)r.d[5]);
        uint16_t y = (uint16_t)(vrd16(r.a[0] + (uint32_t)(i6 * 2)) + 0x40);
        r.d[1] = setw(r.d[1], y);
        vwr16(G_CAM_Y, y);
        charge(9);
    }
    car = (uint16_t)((vrd16(A6W(0x46E4)) & 0xF) << 6);
    r.d[0] = setw(r.d[0], car);
    uint16_t dx = (uint16_t)(vrd16(A6W(0x16B0) + (uint32_t)sxw(car)) - r.d[4]);
    uint16_t dz = (uint16_t)(vrd16(A6W(0x16B4) + (uint32_t)sxw(car)) - r.d[5]);
    r.d[1] = setw(r.d[1], dx);
    r.d[2] = setw(r.d[2], dz);
    charge(9);
    if (dx == 0) {
        charge(2);
        if (dz == 0) { charge(2); r.d[0] = 0; goto have_angle; }
    }
    charge(1);
    regs_store(&r);
    CALL(L_A98C, 0x024A98);
    regs_load(&r);
    r.d[0] = setw(r.d[0], (uint16_t)(0x4000 - (uint16_t)r.d[0]));
    charge(2);
have_angle:;
    uint16_t h = (uint16_t)vrd16(A6W(0x2198));
    r.d[1] = setw(r.d[1], h);
    uint16_t turn = (uint16_t)(r.d[0] - h);
    charge(4);
    if (!vrd16(A6W(0x46FC))) { charge(1); turn = (uint16_t)((int16_t)turn >> 3); }
    r.d[0] = setw(r.d[0], turn);
    vwr16(A6W(0x2198), (uint16_t)(h + turn));
    uint16_t pt = (uint16_t)vrd16(A6W(0x46D8));
    r.d[0] = setw(r.d[0], pt);
    vwr16(A6W(0x2188), pt);
    vwr16(A6W(0x21A8), 0);
    charge(5);
    regs_store(&r);
    return RD_RTS;
}

/* ======================= race intro / link (0x25BC4..0x27680) ======================= */

/* FUN_00025bc4: start the race-intro fly-by: copy the eight start values
 * (0x25DC6) to the 8 camera longs at 0x4990, clear 0x49B0, set up the
 * mixer (FUN_00025e16), clear 0x49D0, step 0x49D2 and clear the text layer */
static uint32_t rd_intro_start(void)
{
    uint32_t a1 = A6W(0x4990), a2 = 0x25DC6;
    charge(3);
    for (int n = 7; n >= 0; n--) {
        vwr32(a1, vrd32(a2)); a1 += 4; a2 += 4;
        charge(2);
        if (n) poll();
    }
    set_a(1, a1); set_a(2, a2); set_d16(6, 0xFFFF);
    vwr32(A6W(0x49B0), 0);
    charge(2); CALL(L_25E16, 0x025BE0);
    vwr16(A6W(0x49D0), 0);
    vwr16(A6W(0x49D2), (uint16_t)(vrd16(A6W(0x49D2)) + 1));
    charge(3); CALL(L_575A, 0x025BF0);
    return RD_RTS;
}

/* FUN_00025bf2: one frame of the race-intro fly-by. Each of the 8 intro
 * objects' long at 0x4990 counts up by its step (0x25E06) to its target
 * (0x25DE6), then eases the rest of the way in quarters -- the first frame it
 * arrives, bit n of 0x49D0 is set and FUN_0001ea1c plays its sound. Once
 * none moves, 0x49B0 grows by 0x100 to 0x2000. Then the 8 x 9 grid of models
 * (0x25D36, 0 = none) is emitted as 0x8001 records, each turned by its
 * object's angle + the column's 0x2000 steps. */
static uint32_t rd_intro_frame(void)
{
    vwr16(G_DL_PRIO, 0);
    set_a(1, A6W(0x4990)); set_a(2, 0x25E06); set_a(3, 0x25DE6);
    set_d(0, 0); set_d(5, 0);
    set_d16(6, 7);
    set_d16(2, vrd16(A6W(0x49D0)));
    charge(8);
    for (;;) {
        uint32_t a1 = a_reg(1), a2 = a_reg(2), a3 = a_reg(3);
        set_d16(0, vrd16(a2)); set_a(2, a2 + 2);
        uint32_t d1 = vrd32(a1);
        set_d(1, d1);
        charge(4);                                        /* move, move.l, cmp.l, bge */
        if ((int32_t)d1 < (int32_t)vrd32(a3)) {
            vwr32(a1, vrd32(a1) + d_reg(0));
            charge(2);                                    /* add.l, bra */
        } else {
            charge(2);                                    /* btst, bne */
            if (!(d_reg(2) & (1u << (d_reg(6) & 31)))) {
                charge(1); CALL(L_1EA1C, 0x025C26);
            }
            a1 = a_reg(1); a3 = a_reg(3);
            set_d(2, d_reg(2) | (1u << (d_reg(6) & 31)));
            d1 = vrd32(a3) - vrd32(a1);
            d1 = (uint32_t)((int32_t)d1 >> 2);
            set_d(1, d1);
            vwr32(a1, vrd32(a1) + d1);
            charge(5);                                    /* bset, move.l, sub.l, asr.l, add.l */
        }
        set_d16(5, (uint16_t)(d_reg(5) + d_reg(1)));
        set_a(1, a_reg(1) + 4); set_a(3, a_reg(3) + 4);
        charge(4);
        uint16_t c = (uint16_t)(d_reg(6) - 1);
        set_d16(6, c);
        if (c == 0xFFFF) break;
        poll();
    }
    regs_t r; regs_load(&r);
    vwr16(A6W(0x49D0), (uint16_t)r.d[2]);
    charge(3);                                            /* move, tst, bne */
    if ((uint16_t)r.d[5] == 0) {
        uint32_t v = vrd32(A6W(0x49B0)) + 0x100;
        charge(4);
        if (v >= 0x2000) { charge(1); v = 0x2000; }
        r.d[0] = v;
        vwr32(A6W(0x49B0), v);
        charge(1);
    }
    r.a[1] = 0x25D36;
    r.a[3] = vrd32(G_DL_CURSOR);
    put_a3(&r, 0x8001);
    r.d[0] = (uint32_t)vrd16(G_DL_PRIO) & 7;
    put_a3(&r, r.d[0]);
    r.d[7] = setw(r.d[7], 7);
    r.d[4] = setw(r.d[4], 0);
    charge(8);
    for (;;) {
        r.a[2] = A6W(0x4990);
        r.d[5] = setw(r.d[5], (uint16_t)-0x9F5);
        r.d[6] = setw(r.d[6], 8);
        charge(3);
        for (;;) {
            r.d[0] = (uint16_t)vrd16(r.a[1]); r.a[1] += 2;
            charge(3);                                    /* moveq, move, beq */
            if (r.d[0]) {
                put_a3(&r, r.d[0]);
                r.d[5] = (uint32_t)sxw(r.d[5]);
                put_a3(&r, r.d[5]);
                r.d[0] = vrd32(r.a[2]);
                r.d[0] = setw(r.d[0], (uint16_t)-(uint16_t)(r.d[0] + r.d[4]));
                charge(6);                                /* move.l, ext, move.l, move.l, add, neg */
                uint16_t a = (uint16_t)(r.d[0] & 0xFFFE);
                r.d[1] = setw(r.d[1], (uint16_t)-trig(r.a[5], a));
                a = (uint16_t)(a + 0x4000);
                r.d[0] = setw(r.d[0], a);
                r.d[2] = setw(r.d[2], trig(r.a[5], a));
                charge(5);
                r.d[0] = setw(r.d[0], 0xE);
                int32_t s = sxw(r.d[1]), c = sxw(r.d[2]);
                r.d[1] = ((uint32_t)(s % 14) << 16) | ((uint32_t)(s / 14) & 0xFFFF);
                r.d[2] = ((uint32_t)(c % 14) << 16) | ((uint32_t)(c / 14) & 0xFFFF);
                r.d[2] = setw(r.d[2], r.d[2] + 0x1E34);
                r.d[0] = 0u - (vrd32(r.a[2]) - 0x48000u);
                r.d[0] >>= 3;
                r.d[2] = setw(r.d[2], r.d[2] + r.d[0]);
                r.d[1] = (uint32_t)sxw(r.d[1]);
                r.d[2] = (uint32_t)sxw(r.d[2]);
                put_a3(&r, r.d[1]); put_a3(&r, r.d[2]);
                charge(15);                               /* move, 2 ext, 2 divs, addi, move.l, subi, neg, lsr, add, 2 ext, 2 move.l */
                r.d[0] = vrd32(r.a[2]); r.a[2] += 4;
                a = (uint16_t)((uint16_t)(r.d[0] + r.d[4] + 0x8000) & 0xFFFE);
                r.d[0] = setw(r.d[0], (uint16_t)(r.d[0] + r.d[4] + 0x8000));
                charge(3);                                /* move.l, add, addi */
                r.d[1] = setw(r.d[1], (uint16_t)-trig(r.a[5], a));
                a = (uint16_t)(a + 0x4000);
                r.d[0] = setw(r.d[0], a);
                r.d[2] = setw(r.d[2], trig(r.a[5], a));
                charge(5);
                put_a3(&r, r.d[1]); put_a3(&r, r.d[2]);
                put_a3(&r, 0); put_a3(&r, 0x7FFF); put_a3(&r, 0); put_a3(&r, 0x7FFF); put_a3(&r, 0);
                charge(7);
            }
            r.d[5] = setw(r.d[5], r.d[5] + 0x285);
            charge(2);                                    /* addi, dbf */
            uint16_t c6 = (uint16_t)(r.d[6] - 1);
            r.d[6] = setw(r.d[6], c6);
            if (c6 == 0xFFFF) break;
            poll();
        }
        r.d[4] = setw(r.d[4], r.d[4] + 0x2000);
        charge(2);
        uint16_t c7 = (uint16_t)(r.d[7] - 1);
        r.d[7] = setw(r.d[7], c7);
        if (c7 == 0xFFFF) break;
        poll();
    }
    vwr32(r.a[3], 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, r.a[3]);
    charge(3);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_00025e16: race-intro mixer setup: window block at A6 (0x5A/5C/5E = 0,
 * mode 1) through FUN_0000b56e, three mixer bytes 0x90020100/180/200 = 0,
 * and 256 bytes of the table at 0x25E64 into czram 0x90014000 */
static uint32_t rd_intro_mixer(void)
{
    set_a(0, A6W(0));
    vwr16(A6W(0x5A), 0); vwr16(A6W(0x5C), 0); vwr16(A6W(0x5E), 0);
    vwr16(A6W(0x00), 1);
    charge(6); CALL(L_B56E, 0x025E34);
    vwr8(0x90020100u, 0); vwr8(0x90020180u, 0); vwr8(0x90020200u, 0);
    uint32_t a0 = 0x90014000u, a1 = 0x25E64;
    charge(6);
    for (int n = 0xFF; n >= 0; n--) {
        vwr8(a0++, vrd8(a1++));
        charge(2);
        if (n) poll();
    }
    set_a(0, a0); set_a(1, a1); set_d16(6, 0xFFFF);
    charge(1);
    return RD_RTS;
}

/* FUN_00025f50: the race-intro state machine, by 0x5884 through the table at
 * 0x25F64: 0 = wait for the link data (0x51F4 matching this cabinet's slot,
 * or not linked: -> state 3) then set up the intro (timer 0x708, screen,
 * sound flag bit 3 of 0x60004020 unless 0x10001071); 1 = run the intro's
 * frame (its eleven parts, camera at the origin); 2 = the countdown page
 * (FUN_00022c9e with step 0xF) until 0x4A70 runs out; 3 = done. */
static uint32_t rd_intro_state(void)
{
    uint16_t st = (uint16_t)(vrd16(A6W(0x5884)) & 3);
    set_d16(0, (uint16_t)vrd16(0x25F64u + (uint32_t)st * 2u));
    charge(6);
    poll();
    switch (st) {
    case 0: {
        charge(2);
        int ok = 0;
        if (vrd16(A6W(0x5890))) {
            uint32_t d0 = vrd32(A6W(0x51F4));
            set_d(0, d0);
            charge(2);
            if (d0) {
                uint16_t d1 = (uint16_t)(vrd16(A6W(0x51F8)) & 7);
                set_d16(1, d1);
                charge(4);
                ok = d0 == vrd32(A6W(0x5126) + (uint32_t)sxw(d1) * 4u);
            }
        }
        if (!ok) { vwr16(A6W(0x5884), 3); charge(2); return RD_RTS; }
        charge(1); CALL(L_28AFC, 0x025F96);
        charge(1); CALL(L_2652A, 0x025F9C);
        vwr16(A6W(0x4A72), 0x333);
        vwr16(A6W(0x4A7A), 0);
        vwr16(A6W(0x4A74), 0); vwr16(A6W(0x4A76), 0); vwr16(A6W(0x4A78), 0);
        set_a(0, A6W(0x4A7A));
        vwr16(A6W(0x4A70), 0x708);
        vwr16(A6W(0x4A7A), 0);
        charge(9); CALL(L_1F6D4, 0x025FCA);
        charge(1); CALL(L_575A, 0x025FD0);
        vwr16(A6W(0x0DE2), 1);
        charge(2); CALL(L_C69A, 0x025FDC);
        charge(1); CALL(L_B8B0, 0x025FE2);
        charge(2);
        if (vrd8(0x10001071u) == 0) {
            uint32_t sp = a_reg(7) - 4, d7 = d_reg(7);
            vwr32(sp, d7);
            uint16_t f = (uint16_t)(vrd16(0x60004020u) | 8);
            vwr16(0x60004020u, f);
            vwr16(A6W(0x48BA), 0x3C);
            set_d(7, vrd32(sp));
            charge(6);
        }
        vwr16(A6W(0x5884), (uint16_t)(vrd16(A6W(0x5884)) + 1));
        charge(2);
        return RD_RTS;
    }
    case 1:
        charge(1); CALL(L_22088, 0x026010);
        charge(1); CALL(L_260FE, 0x026014);
        charge(1); CALL(L_26142, 0x026018);
        charge(1); CALL(L_2628E, 0x02601C);
        charge(1); CALL(L_26482, 0x026020);
        charge(1); CALL(L_2632C, 0x026024);
        charge(1); CALL(L_269D8, 0x026028);
        charge(1); CALL(L_26A40, 0x02602C);
        charge(1); CALL(L_260E6, 0x026030);
        charge(1); CALL(L_260D6, 0x026034);
        charge(1); CALL(L_26218, 0x026038);
        vwr16(G_CAM_X, 0); vwr16(G_CAM_Y, 0); vwr16(G_CAM_Z, 0);
        vwr16(A6W(0x2188), 0); vwr16(A6W(0x2198), 0); vwr16(A6W(0x21A8), 0);
        charge(7); CALL(L_146AE, 0x026062);
        charge(1); CALL(L_B56E, 0x026068);
        charge(1);
        return RD_RTS;
    case 2: {
        vwr16(A6W(0x232E), 0xF);
        charge(2); CALL(L_22C9E, 0x0260C8);
        uint16_t t = (uint16_t)(vrd16(A6W(0x4A70)) - 1);
        vwr16(A6W(0x4A70), t);
        charge(2);
        if (!(t & 0x8000)) { charge(1); return RD_RTS; }
        vwr16(A6W(0x5884), (uint16_t)(vrd16(A6W(0x5884)) + 1));
        charge(2);
        return RD_RTS;
    }
    default:
        charge(1);
        return RD_RTS;
    }
}

/* FUN_00026b20: reset the cabinet-link (SCI) board: run bit 0x40000019,
 * load its 8 control words (0x26AF2) into 0x20020000, clear its 8 KB RAM
 * 0x20010000, load the next 8 control words, clear the 4 KB link buffer
 * 0x10002000, and reset the link state (0x1038 = 0x26, counters and the
 * 0x50-word table at 0x105E to -1) */
static uint32_t rd_link_reset(void)
{
    vwr8(0x40000019u, 1);
    uint32_t a1 = 0x26AF2, a2 = 0x20020000u;
    charge(4);
    for (int n = 7; n >= 0; n--) { vwr16(a2, vrd16(a1)); a1 += 2; a2 += 2; charge(3); if (n) poll(); }
    a2 = 0x20010000u;
    charge(3);
    for (int n = 0x1FFF; n >= 0; n--) { vwr16(a2, 0); a2 += 2; charge(3); if (n) poll(); }
    a2 = 0x20020000u;
    charge(2);
    for (int n = 7; n >= 0; n--) { vwr16(a2, vrd16(a1)); a1 += 2; a2 += 2; charge(3); if (n) poll(); }
    a2 = 0x10002000u;
    charge(2);
    for (int n = 0x3FF; n >= 0; n--) { vwr32(a2, 0); a2 += 4; charge(2); if (n) poll(); }
    vwr16(0x20020006u, 0);
    vwr16(A6W(0x1038), 0x26);
    static const uint16_t clr[] = { 0x1002, 0x100A, 0x100C, 0x1008, 0x1004, 0x1006, 0x1034, 0x1036, 0x103A, 0x1104, 0x105C };
    for (unsigned i = 0; i < sizeof clr / sizeof clr[0]; i++) vwr16(A6W(clr[i]), 0);
    uint32_t a0 = A6W(0x105E);
    charge(2 + 11 + 3);
    for (int n = 0x4F; n >= 0; n--) { vwr16(a0, 0xFFFF); a0 += 2; charge(2); if (n) poll(); }
    set_a(0, a0); set_a(1, a1); set_a(2, a2);
    set_d(0, 0xFFFF);                                     /* moveq #0 then move.w #-1 */
    set_d(7, 0x0000FFFFu);                                /* moveq #7 ... dbf: 0xFFFF */
    set_d16(6, 0xFFFF);
    charge(1);
    return RD_RTS;
}

/* FUN_00026e24: this cabinet's link number = byte 0x10001060 & 7 -> 0x11A0,
 * with its byte offsets *2 (0x1158), *16 (0x115A) and *64 (0x115C); and number
 * the 16 link slots 0..15 (the word at 0x129C - n*0x10) */
static uint32_t rd_link_number(void)
{
    uint16_t n = (uint16_t)(vrd8(0x10001060u) & 7);
    vwr16(A6W(0x11A0), n);
    vwr16(A6W(0x1158), (uint16_t)(n * 2));
    vwr16(A6W(0x115A), (uint16_t)(n * 16));
    vwr16(A6W(0x115C), (uint16_t)(n * 64));
    set_d16(0, (uint16_t)(n * 64));
    uint32_t a1 = A6W(0x129C);
    charge(12);
    for (int s = 15; s >= 0; s--) {
        vwr16(a1, (uint16_t)s);
        a1 -= 0x10;
        charge(3);
        if (s) poll();
    }
    set_a(1, a1);
    set_d(6, 0xFFFF);
    charge(1);
    return RD_RTS;
}

/* ======================= cabinet link (0x26EAE..0x27680) ======================= */

#define SCI_CTRL2   0x20020002u   /* link board control word (bits 2/3 toggled here) */
#define SCI_CTRL4   0x20020004u
#define SCI_RAM     0x20010000u   /* link board RAM: 0x1000 16-bit words, a ring */
#define LINK_TX     0x10002800u   /* outgoing packet staging (WRAM) */
#define LINK_RX     0x10002400u   /* incoming packet staging (WRAM) */
#define G_LINK_ME   A6W(0x11A0)   /* word: this cabinet's link number 0..7 */

/* FUN_00026eae: the per-frame cabinet-link exchange. Gathers the slots'
 * display state (FUN_00026e58) and sends this cabinet's packet (FUN_00026f86);
 * marks our bit in 0x1034; notes the frame counter byte; for each of the 16
 * slots resets its age counter (0x110C) when its sequence byte changed and ages
 * the live ones (bit n of 0x1104), dropping any 8 frames old; after 8 frames
 * of 0x1108 clears our own bit in 0x1105; counts the live cabinets ahead of us
 * (0x1102) and finds the highest live one (0x115E, *16 at 0x1160); then
 * FUN_00027008, FUN_0002708a, and continues in FUN_000270f2. */
static uint32_t rd_link_frame(void)
{
    vwr16(SCI_CTRL2, (uint16_t)(vrd16(SCI_CTRL2) & 9));
    charge(2); CALL(L_26E58, 0x026EB8);
    charge(1); CALL(L_26F86, 0x026EBC);
    regs_t r; regs_load(&r);
    r.d[0] = setw(r.d[0], vrd16(G_LINK_ME));
    r.d[7] = setw(r.d[7], vrd16(A6W(0x1034)));
    r.d[7] |= 1u << (r.d[0] & 31);
    vwr16(A6W(0x1034), (uint16_t)r.d[7]);
    charge(5);
    if ((uint16_t)r.d[7]) { charge(1); vwr16(SCI_CTRL2, (uint16_t)(vrd16(SCI_CTRL2) & 0xB)); }
    vwr8(A6W(0x11A9), vrd8(A6W(0x0C4D)));
    r.d[6] = 0xF;
    r.d[0] = setw(r.d[0], vrd16(A6W(0x1104)));
    charge(3);
    for (;;) {
        int32_t n = sxw(r.d[6]);
        r.d[2] = setw(r.d[2], (uint16_t)(r.d[6] << 4));
        uint32_t seq = A6W(0x11B5) + (uint32_t)sxw(r.d[2]);
        uint8_t old = vrd8(A6W(0x112D) + (uint32_t)(n * 2));
        r.d[1] = setb(r.d[1], old);
        charge(5);                                        /* move, lsl, move.b, cmp.b, beq */
        if (old != vrd8(seq)) { charge(1); vwr16(A6W(0x110C) + (uint32_t)(n * 2), 0); }
        vwr8(A6W(0x112D) + (uint32_t)(n * 2), vrd8(seq));
        charge(3);                                        /* move.b, btst, beq */
        if (r.d[0] & (1u << (r.d[6] & 31))) {
            uint16_t age = (uint16_t)(vrd16(A6W(0x110C) + (uint32_t)(n * 2)) + 1);
            r.d[1] = setw(r.d[1], age);
            charge(4);                                    /* move, addq, cmpi, bcs */
            if (age >= 8) { charge(2); r.d[0] &= ~(1u << (r.d[6] & 31)); r.d[1] = setw(r.d[1], 0); }
            vwr16(A6W(0x110C) + (uint32_t)(n * 2), (uint16_t)r.d[1]);
            charge(1);
        }
        charge(1);                                        /* dbf */
        uint16_t c = (uint16_t)(r.d[6] - 1);
        r.d[6] = setw(r.d[6], c);
        if (c == 0xFFFF) break;
        poll();
    }
    vwr16(A6W(0x1104), (uint16_t)r.d[0]);
    charge(2);
    regs_store(&r);
    CALL(L_2708A, 0x026F2C);
    regs_load(&r);
    uint16_t fc = (uint16_t)(vrd16(A6W(0x1108)) + 1);
    r.d[0] = setw(r.d[0], fc);
    vwr16(A6W(0x1108), fc);
    charge(5);
    if ((int16_t)fc >= 8) {
        r.d[0] = setw(r.d[0], vrd16(G_LINK_ME));
        vwr8(A6W(0x1105), vrd8(A6W(0x1105)) & (uint8_t)~(1u << (r.d[0] & 7)));
        charge(2);
    }
    r.d[0] = setw(r.d[0], vrd16(A6W(0x1104)));
    r.d[6] = 7; r.d[1] = 0;
    r.d[2] = setw(r.d[2], vrd16(G_LINK_ME));
    charge(6);
    if (r.d[0] & (1u << (r.d[2] & 31))) {
        for (;;) {
            uint16_t w = (uint16_t)r.d[0];
            w = (uint16_t)((w >> 1) | (w << 15));
            r.d[0] = setw(r.d[0], w);
            charge(2);                                    /* ror, bpl */
            if (w & 0x8000) { charge(1); r.d[1] = setw(r.d[1], r.d[1] + 1); }
            charge(1);
            uint16_t c = (uint16_t)(r.d[6] - 1);
            r.d[6] = setw(r.d[6], c);
            if (c == 0xFFFF) break;
            poll();
        }
    }
    vwr16(A6W(0x1102), (uint16_t)r.d[1]);
    r.d[1] = 7;
    r.d[0] = setw(r.d[0], vrd16(A6W(0x1104)));
    charge(3);                                            /* move, moveq, move */
    for (;;) {
        charge(2);                                        /* btst, bne */
        if (r.d[0] & (1u << (r.d[1] & 31))) break;
        charge(1);
        uint16_t c = (uint16_t)(r.d[1] - 1);
        r.d[1] = setw(r.d[1], c);
        if (c == 0xFFFF) break;
        poll();
    }
    vwr16(A6W(0x115E), (uint16_t)r.d[1]);
    r.d[1] = setw(r.d[1], (uint16_t)(r.d[1] << 4));
    vwr16(A6W(0x1160), (uint16_t)r.d[1]);
    charge(4);
    regs_store(&r);
    CALL(L_27008, 0x026F7E);
    charge(1); CALL(L_2708A, 0x026F82);
    charge(1);
    return 0x0270F2;
}

/* FUN_00026f86: send this cabinet's link packet: stage our 6 slot words
 * (0x11AC + me*16) and 12 position words (0x12AC + me*64) at 0x10002800,
 * -1 terminated; then write the packet bytes one per word into the link RAM
 * ring after the length word (0x1038 + 2), then the length and the byte sum,
 * with the board's control word 0x20020004 = 1 during and 3 after */
static uint32_t rd_link_send(void)
{
    regs_t r; regs_load(&r);
    r.a[1] = LINK_TX; r.a[2] = A6W(0x11AC); r.a[3] = A6W(0x12AC);
    r.d[2] = setw(r.d[2], vrd16(A6W(0x115A)));
    r.d[3] = setw(r.d[3], vrd16(A6W(0x115C)));
    r.d[5] = setw(r.d[5], 5);
    charge(6);
    for (;;) {
        vwr16(r.a[1], vrd16(r.a[2] + (uint32_t)sxw(r.d[2]))); r.a[1] += 2;
        r.d[2] = setw(r.d[2], r.d[2] + 2);
        charge(3);
        uint16_t c = (uint16_t)(r.d[5] - 1); r.d[5] = setw(r.d[5], c);
        if (c == 0xFFFF) break;
        poll();
    }
    r.d[5] = setw(r.d[5], 0xB);
    charge(1);
    for (;;) {
        vwr16(r.a[1], vrd16(r.a[3] + (uint32_t)sxw(r.d[3]))); r.a[1] += 2;
        r.d[3] = setw(r.d[3], r.d[3] + 2);
        charge(3);
        uint16_t c = (uint16_t)(r.d[5] - 1); r.d[5] = setw(r.d[5], c);
        if (c == 0xFFFF) break;
        poll();
    }
    vwr16(r.a[1], 0xFFFF);
    r.d[2] = setw(r.d[2], (uint16_t)(vrd16(A6W(0x1038)) + 2));
    r.a[1] = LINK_TX; r.a[2] = SCI_RAM; r.d[4] = 0;
    vwr16(SCI_CTRL4, 1);
    r.d[0] = setw(r.d[0], (uint16_t)(r.d[2] - 3));
    r.d[3] = 0;
    r.d[5] = setw(r.d[5], 0xFFF);
    charge(11);
    for (;;) {
        r.d[1] = setb(r.d[1], vrd8(r.a[1])); r.a[1] += 1;
        vwr16(r.a[2] + (uint32_t)sxw(r.d[4]), (uint16_t)r.d[1]);
        r.d[4] = setw(r.d[4], (uint16_t)((r.d[4] + 2) & r.d[5]));
        r.d[3] = setw(r.d[3], r.d[3] + r.d[1]);
        charge(6);
        uint16_t c = (uint16_t)(r.d[0] - 1); r.d[0] = setw(r.d[0], c);
        if (c == 0xFFFF) break;
        poll();
    }
    vwr16(r.a[2] + (uint32_t)sxw(r.d[4]), (uint16_t)r.d[2]);
    r.d[4] = setw(r.d[4], (uint16_t)((r.d[4] + 2) & r.d[5]));
    vwr16(SCI_CTRL4, 3);
    vwr16(r.a[2] + (uint32_t)sxw(r.d[4]), (uint16_t)r.d[3]);
    charge(6);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_00027008: merge the slot display state of every live cabinet on our
 * segment (FUN_00027062, twice): the words at +8 of each slot into
 * 0x1190..0x1196 (or / or-if-same-segment / and / and-if-same-segment),
 * and the words at +10 into 0x1198..0x119E */
static uint32_t rd_link_merge(void)
{
    for (int pass = 0; pass < 2; pass++) {
        set_d16(0, vrd16(A6W(0x11A2)));
        set_d(1, 0); set_d(2, 0); set_d(3, 0xFFFFFFFFu); set_d(4, 0xFFFFFFFFu);
        set_d(5, 0); set_d(6, 0); set_d(7, 7);
        set_a(0, A6W(pass ? 0x11B6 : 0x11B4));
        set_a(1, A6W(0x11AE));
        charge(11);
        CALL(L_27062, pass ? 0x027050 : 0x027024);
        uint32_t o = pass ? 0x1198 : 0x1190;
        vwr16(A6W(o + 0), (uint16_t)d_reg(1));
        vwr16(A6W(o + 2), (uint16_t)d_reg(2));
        vwr16(A6W(o + 4), (uint16_t)d_reg(3));
        vwr16(A6W(o + 6), (uint16_t)d_reg(4));
        charge(4);
    }
    charge(1);
    return RD_RTS;
}

/* FUN_00027126: receive link packets. Takes the 16 pending packet pointers
 * (the 16-word window at 0x105E + 0x105C, which advances by 0x20 through 4
 * windows) into 0x10DE, marking the window empty (-1). For each pointer, reads
 * the packet back out of the link RAM ring (0x20012000): its sum and length;
 * a length other than 0x28 counts a bad packet (0x1150, 0x1152); else the 39
 * data bytes are unpacked to 0x10002400 and, unless it is our own or a masked
 * cabinet (0x1162) or an unknown id (counted in 0x114C/0x114E), its 6 slot and
 * 12 position words are copied to that cabinet's 0x11AC / 0x12AC records. */
static uint32_t rd_link_receive(void)
{
    regs_t r; regs_load(&r);
    r.a[0] = A6W(0x10DE); r.a[1] = A6W(0x105E);
    r.d[1] = setw(r.d[1], vrd16(A6W(0x105C)));
    r.a[1] += (uint32_t)sxw(r.d[1]);
    r.d[0] = setw(r.d[0], 0xFFFF);
    r.d[6] = 0xF;
    charge(6);
    for (;;) {
        vwr16(r.a[0], vrd16(r.a[1])); r.a[0] += 2;
        vwr16(r.a[1], (uint16_t)r.d[0]); r.a[1] += 2;
        charge(3);
        uint16_t c = (uint16_t)(r.d[6] - 1); r.d[6] = setw(r.d[6], c);
        if (c == 0xFFFF) break;
        poll();
    }
    r.d[1] = setw(r.d[1], (uint16_t)((r.d[1] + 0x20) & 0x60));
    vwr16(A6W(0x105C), (uint16_t)r.d[1]);
    r.d[7] = 0xF;
    charge(4);
    for (;;) {
        r.a[1] = LINK_RX; r.a[2] = 0x20012000u; r.a[4] = A6W(0x10DE);
        uint16_t p = (uint16_t)vrd16(r.a[4] + (uint32_t)(sxw(r.d[7]) * 2));
        r.d[2] = setw(r.d[2], p);
        charge(5);
        if (!(p & 0x8000)) {
            uint16_t i = (uint16_t)(((p & 0xFFF) - 1) & 0xFFF);
            r.d[5] = setw(r.d[5], vrd16(r.a[2] + (uint32_t)(sxw(i) * 2)));
            vwr8(r.a[1], (uint8_t)r.d[5]); r.a[1] += 1;
            i = (uint16_t)((i - 1) & 0xFFF);
            r.d[4] = setw(r.d[4], vrd16(r.a[2] + (uint32_t)(sxw(i) * 2)));
            vwr8(r.a[1], (uint8_t)r.d[4]); r.a[1] += 1;
            r.d[6] = setw(r.d[6], 0x26);
            i = (uint16_t)((i - 0x26) & 0xFFF);
            r.d[2] = setw(r.d[2], i);
            charge(14);
            if ((uint8_t)r.d[4] != 0x28) {
                vwr16(A6W(0x1150), (uint16_t)(vrd16(A6W(0x1150)) + 1));
                vwr16(A6W(0x1152), (uint16_t)r.d[4]);
                charge(3);
            } else {
                for (;;) {
                    r.d[0] = setw(r.d[0], vrd16(r.a[2] + (uint32_t)(sxw(r.d[2]) * 2)));
                    vwr8(r.a[1], (uint8_t)r.d[0]); r.a[1] += 1;
                    r.d[5] = setb(r.d[5], (uint8_t)(r.d[5] - r.d[0]));
                    r.d[2] = setw(r.d[2], (uint16_t)((r.d[2] + 1) & 0xFFF));
                    charge(6);
                    uint16_t c = (uint16_t)(r.d[6] - 1); r.d[6] = setw(r.d[6], c);
                    if (c == 0xFFFF) break;
                    poll();
                }
                r.a[1] = LINK_RX + 2;
                r.a[2] = A6W(0x11AC); r.a[3] = A6W(0x12AC);
                uint16_t id = (uint16_t)vrd16(r.a[1]);
                r.d[4] = setw(r.d[4], id);
                charge(7);                                /* lea, addq, lea, lea, move, cmpi, beq */
                if (id != 0xFFFF) {
                    charge(2);
                    if (id != (uint16_t)vrd16(G_LINK_ME)) {
                        r.d[0] = setw(r.d[0], (uint16_t)(id & 0xFFF0));
                        charge(3);
                        if (id & 0xFFF0) {
                            vwr16(A6W(0x114C), (uint16_t)(vrd16(A6W(0x114C)) + 1));
                            vwr16(A6W(0x114E), id);
                            charge(3);
                        } else {
                            uint16_t mask = (uint16_t)vrd16(A6W(0x1162));
                            r.d[0] = setw(r.d[0], mask);
                            charge(2);
                            int skip = 0;
                            if (mask) { charge(2); skip = (r.d[0] >> (r.d[4] & 31)) & 1; }
                            if (!skip) {
                                id &= 0xF;
                                r.d[4] = setw(r.d[4], id);
                                r.d[2] = setw(r.d[2], (uint16_t)(id << 4));
                                r.d[3] = setw(r.d[3], (uint16_t)(id << 6));
                                r.d[5] = setw(r.d[5], 5);
                                charge(6);
                                for (;;) {
                                    vwr16(r.a[2] + (uint32_t)sxw(r.d[2]), vrd16(r.a[1])); r.a[1] += 2;
                                    r.d[2] = setw(r.d[2], r.d[2] + 2);
                                    charge(3);
                                    uint16_t c = (uint16_t)(r.d[5] - 1); r.d[5] = setw(r.d[5], c);
                                    if (c == 0xFFFF) break;
                                    poll();
                                }
                                r.d[5] = setw(r.d[5], 0xB);
                                charge(1);
                                for (;;) {
                                    vwr16(r.a[3] + (uint32_t)sxw(r.d[3]), vrd16(r.a[1])); r.a[1] += 2;
                                    r.d[3] = setw(r.d[3], r.d[3] + 2);
                                    charge(3);
                                    uint16_t c = (uint16_t)(r.d[5] - 1); r.d[5] = setw(r.d[5], c);
                                    if (c == 0xFFFF) break;
                                    poll();
                                }
                            }
                        }
                    }
                }
            }
        }
        charge(1);                                        /* dbf */
        uint16_t c = (uint16_t)(r.d[7] - 1); r.d[7] = setw(r.d[7], c);
        if (c == 0xFFFF) break;
        poll();
    }
    charge(1);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_0002721a: (not in the replay: 0x1100) copy each other cabinet's
 * received position record (0x166C - n*0x40, 12 words) into the car table
 * (0x16AC + n*0x40), skipping our own and masked cabinets (0x1162); the
 * word at +0x16 is also halved into two accumulators (+0x2A, +0x2C) */
static uint32_t rd_link_cars(void)
{
    regs_t r; regs_load(&r);
    charge(2);
    if (vrd16(A6W(0x1100))) { charge(1); return RD_RTS; }
    r.d[6] = 0xF;
    r.a[3] = A6W(0x166C);
    r.d[3] = setw(r.d[3], 0x3C0);
    r.d[4] = setw(r.d[4], (uint16_t)(vrd16(G_LINK_ME) << 6));
    charge(5);
    for (;;) {
        charge(2);
        int skip = (uint16_t)r.d[3] == (uint16_t)r.d[4];
        if (!skip) {
            uint16_t m = (uint16_t)vrd16(A6W(0x1162));
            r.d[5] = setw(r.d[5], m);
            charge(2);
            if (m) { charge(2); skip = (r.d[5] >> (r.d[6] & 31)) & 1; }
        }
        if (!skip) {
            uint32_t d = A6W(0) + (uint32_t)sxw(r.d[3]), s = r.a[3];
            static const uint16_t dst[10] = { 0x16AC, 0x16AE, 0x16B0, 0x16B2, 0x16B4, 0x16B6, 0x16C6, 0x16B8, 0x16BA, 0x16BC };
            static const uint8_t  src[10] = { 0, 2, 4, 6, 8, 0xA, 0xA, 0xC, 0xE, 0x10 };
            for (int i = 0; i < 10; i++) vwr16(d + dst[i], vrd16(s + src[i]));
            uint16_t w = (uint16_t)vrd16(s + 0x12);
            vwr16(d + 0x16BE, w); vwr16(d + 0x16DC, w);
            vwr16(d + 0x16C0, vrd16(s + 0x14));
            w = (uint16_t)vrd16(s + 0x16);
            vwr16(d + 0x16C2, w);
            w = (uint16_t)((int16_t)w >> 1);
            r.d[0] = setw(r.d[0], w);
            vwr16(d + 0x16D8, (uint16_t)(vrd16(d + 0x16D8) + w));
            vwr16(d + 0x16D6, (uint16_t)(vrd16(d + 0x16D6) + w));
            charge(20);
        }
        r.d[3] = setw(r.d[3], r.d[3] - 0x40);
        r.a[3] -= 0x40;
        charge(3);
        uint16_t c = (uint16_t)(r.d[6] - 1); r.d[6] = setw(r.d[6], c);
        if (c == 0xFFFF) break;
        poll();
    }
    charge(1);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_000272d4: (not in the replay) for each live cabinet on our segment whose
 * slot flag bit 3 is set, set bits 7 and 1 of its car flag byte (0x16C4 + n*0x40);
 * clear them for every other slot */
static uint32_t rd_link_car_flags(void)
{
    regs_t r; regs_load(&r);
    charge(2);
    if (vrd16(A6W(0x1100))) { charge(1); return RD_RTS; }
    r.d[1] = 0; r.d[2] = 0; r.d[3] = 0;
    r.d[4] = setw(r.d[4], vrd16(A6W(0x1104)));
    r.d[6] = 0xF;
    charge(5);
    for (;;) {
        uint32_t f = A6W(0x16C4) + (uint32_t)sxw(r.d[3]);
        charge(2);
        int on = 0;
        if (r.d[4] & (1u << (r.d[1] & 31))) {
            uint16_t seg = (uint16_t)vrd16(A6W(0x11AE) + (uint32_t)sxw(r.d[2]));
            r.d[0] = setw(r.d[0], seg);
            charge(3);
            if (seg == (uint16_t)vrd16(A6W(0x11A2))) {
                charge(2);
                on = (vrd8(A6W(0x11B6) + (uint32_t)sxw(r.d[2])) >> 3) & 1;
            }
        }
        if (on) { vwr8(f, vrd8(f) | 0x82); charge(3); }
        else { vwr8(f, vrd8(f) & 0x7D); charge(2); }
        r.d[1] = setw(r.d[1], r.d[1] + 1);
        r.d[2] = setw(r.d[2], r.d[2] + 0x10);
        r.d[3] = setw(r.d[3], r.d[3] + 0x40);
        charge(4);
        uint16_t c = (uint16_t)(r.d[6] - 1); r.d[6] = setw(r.d[6], c);
        if (c == 0xFFFF) break;
        poll();
    }
    charge(1);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_00027334: (not in the replay) store our car into our own link position
 * record (0x12AC + me*64): position block from the car table, y less 0x41E8,
 * heading plus 0x4AD8, flags with the low nibble from byte 0x10001061, and the
 * words 0x4022 / 0x4014 */
static uint32_t rd_link_store_me(void)
{
    charge(2);
    if (vrd16(A6W(0x1100))) { charge(1); return RD_RTS; }
    uint32_t a1 = A6W(0x12AC);
    uint16_t d4 = (uint16_t)vrd16(A6W(0x115C));
    uint32_t o = (uint32_t)sxw(d4), dst = a1 + o, car = A6W(0) + o;
    set_a(1, a1); set_d16(4, d4);
    vwr16(dst + 0, vrd16(car + 0x16AC));
    vwr16(dst + 2, vrd16(car + 0x16AE));
    vwr16(dst + 4, vrd16(car + 0x16B0));
    vwr16(dst + 6, vrd16(car + 0x16B2));
    vwr16(dst + 8, vrd16(car + 0x16B4));
    vwr16(dst + 0xA, (uint16_t)(vrd16(car + 0x16B6) - vrd16(A6W(0x41E8))));
    vwr16(dst + 0xC, vrd16(car + 0x16B8));
    vwr16(dst + 0xE, (uint16_t)(vrd16(car + 0x16BA) + vrd16(car + 0x4AD8)));
    vwr16(dst + 0x10, vrd16(car + 0x16BC));
    uint8_t b = vrd8(0x10001061u);
    uint16_t d1 = (uint16_t)(((d_reg(1) & 0xFF00) | b) & 0xF);
    uint16_t d0 = (uint16_t)((vrd16(car + 0x16DC) & 0xFFF0) | d1);
    set_d16(1, d1); set_d16(0, d0);
    vwr16(dst + 0x12, d0);
    vwr16(car + 0x16BE, d0);
    vwr16(dst + 0x16, vrd16(A6W(0x4022)));
    vwr16(dst + 0x14, vrd16(A6W(0x4014)));
    charge(2 + 2 + 5 + 3 + 1 + 3 + 1 + 7 + 2 + 1);
    return RD_RTS;
}

/* FUN_000273ee: link-join window. While 0x1162 is set: FUN_000274ca until
 * 0x116A is set; then for each of the 0x4A96+1 cabinets from 0x1168-8, count
 * frames it is seen (0x1170 + i*2; -1 when its live bit is set); each reaching
 * 8 counts toward 0x116C, and when that reaches 0x116E the window closes
 * (0x116A, 0x1164 cleared). */
static uint32_t rd_link_join(void)
{
    charge(2);
    if (!vrd16(A6W(0x1162))) { charge(1); return RD_RTS; }
    charge(2);
    if (!vrd16(A6W(0x116A))) {
        charge(1); CALL(L_274CA, 0x0273FE);
        charge(1);
        return RD_RTS;
    }
    regs_t r; regs_load(&r);
    r.d[0] = setw(r.d[0], (uint16_t)(vrd16(A6W(0x1168)) - 8));
    r.d[7] = setw(r.d[7], vrd16(A6W(0x4A96)));
    charge(3);
    for (;;) {
        uint32_t cnt = A6W(0x1170) + (uint32_t)(sxw(r.d[0]) * 2);
        charge(2);
        if (vrd8(A6W(0x1104)) & (1u << (r.d[0] & 7))) { charge(1); vwr16(cnt, 0xFFFF); }
        vwr16(cnt, (uint16_t)(vrd16(cnt) + 1));
        charge(3);
        if ((uint16_t)vrd16(cnt) == 8) { charge(1); vwr16(A6W(0x116C), (uint16_t)(vrd16(A6W(0x116C)) + 1)); }
        uint16_t k = (uint16_t)vrd16(A6W(0x116C));
        r.d[1] = setw(r.d[1], k);
        charge(3);
        if ((int16_t)k >= (int16_t)vrd16(A6W(0x116E))) {
            vwr16(A6W(0x116A), 0); vwr16(A6W(0x1164), 0);
            charge(2);
        }
        r.d[0] = setw(r.d[0], r.d[0] + 1);
        charge(2);
        uint16_t c = (uint16_t)(r.d[7] - 1); r.d[7] = setw(r.d[7], c);
        if (c == 0xFFFF) break;
        poll();
    }
    charge(1);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_00027680: reset the race-intro state machine (0x5884, 0x5886) */
static uint32_t rd_intro_reset(void)
{
    vwr16(A6W(0x5884), 0);
    vwr16(A6W(0x5886), 0);
    return RD_RTS;
}

/* ======================= race sound cues (0x28632..0x2881C) ======================= */

#define SND_CMD     0x60005000u   /* word: sound CPU command */
#define SND_VOICE   0x60005012u   /* word: voice/announcer command */
#define SND_CUE     0x6000501Eu   /* word: music/cue command */
#define SND_LEVELS  0x60005100u   /* 256 words: channel levels */

/* FUN_00028632: silence everything: clear the sound command (FUN_0002863e),
 * the cue (FUN_00028646), then the command block (FUN_0002864e) */
static uint32_t rd_snd_silence(void)
{
    charge(1); CALL(L_2863E, 0x028636);
    charge(1); CALL(L_28646, 0x02863A);
    charge(1);
    return 0x02864E;
}

/* FUN_0002863e: clear the sound command word */
static uint32_t rd_snd_cmd_clear(void)
{
    vwr16(SND_CMD, 0);
    return RD_RTS;
}

/* FUN_0002864e: clear the command words 0x6000500C..0x6000501C and
 * 0x60005020..0x6000503E, and set the 256 channel levels to 0xFF */
static uint32_t rd_snd_block_reset(void)
{
    uint32_t a0 = 0x6000500Cu;
    charge(2);
    for (int n = 8; n >= 0; n--) { vwr16(a0, 0); a0 += 2; charge(2); if (n) poll(); }
    a0 = 0x60005020u;
    charge(2);
    for (int n = 15; n >= 0; n--) { vwr16(a0, 0); a0 += 2; charge(2); if (n) poll(); }
    a0 = SND_LEVELS;
    charge(2);
    for (int n = 0xFF; n >= 0; n--) { vwr16(a0, 0xFF); a0 += 2; charge(2); if (n) poll(); }
    set_a(0, a0);
    set_d(7, 0x0000FFFFu);
    charge(1);
    return RD_RTS;
}

/* FUN_00028680: set fourteen channel levels to 0xFF */
static uint32_t rd_snd_levels_ff(void)
{
    static const uint16_t ch[] = { 0x104, 0x108, 0x110, 0x116, 0x11C, 0x122, 0x140, 0x14C, 0x160, 0x170, 0x174, 0x176, 0x17A, 0x17C };
    for (unsigned i = 0; i < sizeof ch / sizeof ch[0]; i++) vwr16(0x60005000u + ch[i], 0xFF);
    return RD_RTS;
}

/* FUN_0002872e: unless the sound command already is 0x405D/0x805D/0x405E/
 * 0x805E, clear the cue (FUN_00028646) and send 0x405E */
static uint32_t rd_snd_cmd_405e(void)
{
    static const uint16_t ok[4] = { 0x405D, 0x805D, 0x405E, 0x805E };
    for (int i = 0; i < 4; i++) {
        charge(2);
        if ((uint16_t)vrd16(SND_CMD) == ok[i]) { charge(1); return RD_RTS; }
    }
    charge(1); CALL(L_28646, 0x02875A);
    vwr16(SND_CMD, 0x405E);
    charge(2);
    return RD_RTS;
}

/* FUN_000287a6: reset the race sound-cue state (0x57C4 = 0xF0 frames to the
 * start music, the rest cleared), pick the lap-count cue 0x57CC from the
 * EEPROM setting byte 0x10001065 + (linked ? 4 : 0) + (0x2342 & 3) via the
 * table at 0x2880A, clear the per-race cue flags (FUN_0002881c) and continue
 * in FUN_00028c82 */
static uint32_t rd_snd_race_reset(void)
{
    vwr16(A6W(0x57C4), 0xF0);
    static const uint16_t clr[] = { 0x57CA, 0x57C6, 0x57CE, 0x57D4, 0x57D6, 0x5800, 0x5802, 0x5804, 0x5806 };
    for (unsigned i = 0; i < sizeof clr / sizeof clr[0]; i++) vwr16(A6W(clr[i]), 0);
    set_a(0, 0x10001065u);
    uint16_t d0 = 0;
    charge(10 + 4);
    if (vrd16(A6W(0x1100))) { charge(1); d0 = 4; }
    uint16_t d1 = (uint16_t)(vrd16(A6W(0x2342)) & 3);
    vwr16(A6W(0x57D0), d1);
    d0 = (uint16_t)(d0 + d1);
    uint32_t v = (uint32_t)(vrd8(0x10001065u + (uint32_t)sxw(d0)) & 7);
    set_d(1, v);
    d0 = (uint16_t)vrd16(0x2880Au + v * 2u);
    set_d(0, d0);
    vwr16(A6W(0x57CC), d0);
    charge(10 + 1);
    CALL(L_2881C, 0x028806);
    charge(1);
    return 0x028C82;
}

/* FUN_0002881c: clear the race sound-cue flags and counters (0x57BE..0x5806,
 * 0x57FA = -1) and the two cue tables 0x580C..0x5834 and 0x5834..0x586C */
static uint32_t rd_snd_cue_clear(void)
{
    static const uint16_t clr[] = { 0x5880, 0x5882, 0x57C2, 0x57BE, 0x57E0, 0x57E2, 0x57E4, 0x57E6, 0x57E8,
                                    0x57EA, 0x57EC, 0x57EE, 0x57DA, 0x57DC, 0x57DE, 0x57C8, 0x57F0, 0x57F2,
                                    0x57F4, 0x57F6, 0x57F8 };
    for (unsigned i = 0; i < sizeof clr / sizeof clr[0]; i++) vwr16(A6W(clr[i]), 0);
    vwr16(A6W(0x57FA), 0xFFFF);
    vwr16(A6W(0x57FC), 0); vwr16(A6W(0x57FE), 0); vwr16(A6W(0x586C), 0);
    uint32_t a0 = A6W(0x586E);
    charge(25 + 2);
    for (int n = 8; n >= 0; n--) { vwr16(a0, 0); a0 += 2; charge(2); if (n) poll(); }
    a0 = A6W(0x580C);
    charge(2);
    for (;;) { vwr16(a0, 0); a0 += 2; charge(3); if (a0 >= A6W(0x5834)) break; poll(); }
    a0 = A6W(0x5834);
    charge(2);
    for (;;) { vwr16(a0, 0); a0 += 2; charge(3); if (a0 >= A6W(0x586C)) break; poll(); }
    set_a(0, a0); set_a(1, A6W(0x586C));
    set_d(7, 0x0000FFFFu);
    charge(1);
    return RD_RTS;
}

/* ======================= race sound cues (0x289CA..0x29854) ======================= */

#define G_CUE_FLAGS   A6W(0x5880)   /* byte: cue request bits 0..7 (served by FUN_000294c8) */
#define G_CUE_FLAGS2  A6W(0x5882)   /* byte: second cue request byte */
#define G_SEGMENT     A6W(0x204C)   /* word: the car's track segment */
#define G_LAP_POS     A6W(0x204A)   /* word: race progress (lap) */

/* FUN_000289ca: the start countdown's sounds: while 0x57C4 >= 0, each whole
 * second (0x3C frames) sends the countdown cue (0x28A00 [seconds]) and at 0
 * notes the segment in 0x57C0; the counter runs down. Once it is negative,
 * sends the course's music (0x28A36 [0x2224 & 15]) once, unless the sound CPU
 * is still busy (bit 15 of its command word) */
static uint32_t rd_snd_countdown(void)
{
    charge(2);
    int16_t c = (int16_t)vrd16(A6W(0x57C4));
    if (c >= 0) {
        uint32_t d0 = (uint16_t)c;
        d0 = ((d0 % 0x3C) << 16) | (d0 / 0x3C);
        uint32_t d1 = (d0 << 16) | (d0 >> 16);
        set_d(0, d0); set_d(1, d1);
        charge(7);
        if (!(uint16_t)d1) {
            vwr16(SND_CUE, vrd16(0x28A00u + (uint32_t)sxw(d0) * 2u));
            charge(4);
            if (!(uint16_t)d0) { charge(1); vwr16(A6W(0x57C0), vrd16(G_SEGMENT)); }
        }
        vwr16(A6W(0x57C4), (uint16_t)(c - 1));
        charge(2);
        return RD_RTS;
    }
    charge(3);                                            /* bra, tst, bne */
    if (vrd16(A6W(0x57C6))) { charge(1); return RD_RTS; }
    uint16_t cmd = (uint16_t)vrd16(SND_CMD);
    cmd = (uint16_t)((cmd << 8) | (cmd >> 8));
    set_d16(0, cmd);
    charge(4);
    if (cmd & 0x80) { charge(1); return RD_RTS; }
    uint16_t m = (uint16_t)(vrd16(A6W(0x2224)) & 0xF);
    set_d16(0, m);
    vwr16(SND_CMD, vrd16(0x28A36u + (uint32_t)m * 2u));
    vwr16(A6W(0x57C6), 1);
    charge(6);
    return RD_RTS;
}

/* FUN_00028a56: once (0x57D6), send the lap cue 0x28A84 [(0x57D2 + (0x204E == 1)) & 3] */
static uint32_t rd_snd_lap_cue(void)
{
    charge(2);
    if (vrd16(A6W(0x57D6))) { charge(1); return RD_RTS; }
    uint16_t d0 = 0;
    charge(3);
    if (vrd16(A6W(0x204E)) == 1) { charge(1); d0 = 1; }
    uint16_t d1 = (uint16_t)vrd16(A6W(0x57D2));
    d0 = (uint16_t)((d0 + d1) & 3);
    set_d(0, d0); set_d16(1, d1);
    vwr16(SND_CUE, vrd16(0x28A84u + (uint32_t)d0 * 2u));
    vwr16(A6W(0x57D6), 1);
    charge(7);
    return RD_RTS;
}

/* FUN_00028a8c: once (0x57D4), send sound command 0x405C and set 0x5802/0x5804 */
static uint32_t rd_snd_405c(void)
{
    charge(2);
    if (vrd16(A6W(0x57D4))) { charge(1); return RD_RTS; }
    vwr16(SND_CMD, 0x405C);
    vwr16(A6W(0x57D4), 1);
    vwr16(A6W(0x5802), 1);
    vwr16(A6W(0x5804), 1);
    charge(5);
    return RD_RTS;
}

/* FUN_00028b12: once (0x5806), send sound command 0x405F unless it (or its
 * running form 0x805F) is already there */
static uint32_t rd_snd_405f(void)
{
    charge(2);
    if (vrd16(A6W(0x5806))) { charge(1); return RD_RTS; }
    uint16_t c = (uint16_t)vrd16(SND_CMD);
    charge(2);
    if (c == 0x805F) { charge(1); return RD_RTS; }
    charge(2);
    if (c == 0x405F) { charge(1); return RD_RTS; }
    vwr16(SND_CMD, 0x405F);
    vwr16(A6W(0x5806), 1);
    charge(3);
    return RD_RTS;
}

/* FUN_00028b98: unless sound is off (0x1000106E), send the preview page's cue
 * 0x28BB6 [(0x232E >> 2) & 3] */
static uint32_t rd_snd_preview_cue(void)
{
    charge(2);
    if (vrd8(0x1000106Eu)) { charge(1); return RD_RTS; }
    uint16_t i = (uint16_t)((vrd16(A6W(0x232E)) >> 2) & 3);
    set_d16(0, i);
    vwr16(SND_CUE, vrd16(0x28BB6u + (uint32_t)i * 2u));
    charge(6);
    return RD_RTS;
}

/* FUN_00028bbe: reset the preview's sound state (0x5808, 0x580A) */
static uint32_t rd_snd_preview_reset(void)
{
    vwr16(A6W(0x5808), 0);
    vwr16(A6W(0x580A), 0);
    return RD_RTS;
}

/* FUN_00028bc8: the preview's sounds: its page music once (0x28C22 [(0x232E >> 2)
 * & 7]), cue 0x40A8 at frame 300 and 0x40A9 at frame 900 (not when 0x7028 == 1),
 * counting frames in 0x580A; nothing while sound is off (0x1000106E) */
static uint32_t rd_snd_preview(void)
{
    charge(2);
    if (!vrd8(0x1000106Eu)) {
        charge(2);
        if (!vrd16(A6W(0x5808))) {
            uint16_t i = (uint16_t)((vrd16(A6W(0x232E)) >> 2) & 7);
            set_d16(0, i);
            vwr16(SND_CMD, vrd16(0x28C22u + (uint32_t)i * 2u));
            vwr16(A6W(0x5808), 1);
            charge(6);
        }
        charge(2);
        if (vrd16(A6W(0x580A)) == 0x12C) { charge(1); vwr16(SND_CUE, 0x40A8); }
        charge(2);
        if (vrd16(A6W(0x580A)) == 0x384) {
            charge(2);
            if (vrd16(A6W(0x7028)) != 1) { charge(1); vwr16(SND_CUE, 0x40A9); }
        }
    }
    vwr16(A6W(0x580A), (uint16_t)(vrd16(A6W(0x580A)) + 1));
    charge(2);
    return RD_RTS;
}

/* FUN_00028c32: the race sound cues each frame: update the cue state
 * (FUN_00028c76) and send pending cues (FUN_000294c8); then, when the cue
 * channel is free (bits 15/14 of 0x6000501E clear), FUN_000297e0 while
 * 0x586C is set, else the windowed voice (FUN_00029854 / 0002981c / 00029838
 * say whether the car is in a voice window -- they answer in Z -- and
 * FUN_0002988e plays it); finally clear 0x5882 */
static uint32_t rd_snd_race_frame(void)
{
    charge(1); CALL(L_28C76, 0x028C36);
    charge(1); CALL(L_294C8, 0x028C3A);
    uint16_t c = (uint16_t)vrd16(SND_CUE);
    c = (uint16_t)((c << 8) | (c >> 8));
    set_d16(0, c);
    charge(4);
    if (!(d_reg(0) & 0x80)) {
        charge(2);
        if (!(d_reg(0) & 0x40)) {
            charge(2);
            if (vrd16(A6W(0x586C))) {
                charge(1); CALL(L_297E0, 0x028C58);
                charge(1);
            } else {
                charge(1); CALL(L_29854, 0x028C5E);
                charge(1);
                if (!flag_z()) {
                    charge(1); CALL(L_2981C, 0x028C64);
                    charge(1);
                    if (!flag_z()) {
                        charge(1); CALL(L_29838, 0x028C6A);
                        charge(1);
                        if (!flag_z()) { charge(1); CALL(L_2988E, 0x028C70); }
                    }
                }
            }
        }
    }
    vwr16(G_CUE_FLAGS2, 0);
    charge(2);
    return RD_RTS;
}

/* FUN_00028c9c: the cue-state updates (FUN_00028cb8, 28d48, 28d8a, 28db2,
 * 28dd4, 28e94), continuing in FUN_000290ba */
static uint32_t rd_snd_cue_update(void)
{
    charge(1); CALL(L_28CB8, 0x028CA0);
    charge(1); CALL(L_28D48, 0x028CA4);
    charge(1); CALL(L_28D8A, 0x028CA8);
    charge(1); CALL(L_28DB2, 0x028CAC);
    charge(1); CALL(L_28DD4, 0x028CB0);
    charge(1); CALL(L_28E94, 0x028CB4);
    charge(1);
    return 0x0290BA;
}

/* FUN_00028cb8: the course's one "good time" cue: once (0x57C2), count frames
 * (0x57BE); when the car passes the course's segment (0x28D08 [course],
 * moved back by the start delay 0x1B4AA - 0x57C0) within the course's frame
 * limit (0x28D28 [course]) request cue bit 7 */
static uint32_t rd_cue_good_time(void)
{
    charge(2);
    if (vrd16(A6W(0x57C2))) { charge(1); return RD_RTS; }
    vwr16(A6W(0x57BE), (uint16_t)(vrd16(A6W(0x57BE)) + 1));
    uint32_t d0 = (uint32_t)(vrd16(G_COURSE) & 0xF) << 1;
    uint32_t a0 = 0x28D08 + d0, a1 = 0x28D28 + d0;
    uint16_t seg = (uint16_t)vrd16(a0);
    set_a(0, a0); set_a(1, a1); set_d(0, d0); set_d16(1, seg);
    charge(11);
    if (!seg) { charge(1); return RD_RTS; }
    uint16_t delay = (uint16_t)(vrd16(0x1B4AAu) - vrd16(A6W(0x57C0)));
    seg = (uint16_t)(seg - delay);
    uint16_t cur = (uint16_t)vrd16(G_SEGMENT);
    set_d16(1, seg); set_d16(0, cur);
    charge(6);
    if ((int16_t)cur < (int16_t)seg) { charge(1); return RD_RTS; }
    uint16_t t = (uint16_t)vrd16(A6W(0x57BE));
    set_d16(2, t);
    charge(3);
    if ((int16_t)t <= (int16_t)vrd16(a1)) { charge(1); vwr8(G_CUE_FLAGS, vrd8(G_CUE_FLAGS) | 0x80); }
    vwr16(A6W(0x57C2), 1);
    charge(2);
    return RD_RTS;
}

/* FUN_00028d48: a new lap (0x204A above 0x57CE and below the lap count
 * 0x57CC, not in attract 0x57D8) past the variant's segment (0x28D82 [var])
 * requests cue bit 2 and notes the lap */
static uint32_t rd_cue_new_lap(void)
{
    uint16_t lap = (uint16_t)vrd16(G_LAP_POS);
    set_d16(0, lap);
    charge(3);
    if ((int16_t)lap <= (int16_t)vrd16(A6W(0x57CE))) { charge(1); return RD_RTS; }
    charge(2);
    if ((int16_t)lap >= (int16_t)vrd16(A6W(0x57CC))) { charge(1); return RD_RTS; }
    charge(2);
    if (vrd16(A6W(0x57D8))) { charge(1); return RD_RTS; }
    uint16_t v = (uint16_t)(vrd16(G_COURSE_VAR) & 3);
    uint32_t a0 = 0x28D82u + (uint32_t)v * 2u;
    uint16_t seg = (uint16_t)vrd16(G_SEGMENT);
    set_a(0, a0); set_d16(0, seg);
    charge(7);
    if (seg < (uint16_t)vrd16(a0)) { charge(1); return RD_RTS; }
    vwr8(G_CUE_FLAGS, vrd8(G_CUE_FLAGS) | 4);
    vwr16(A6W(0x57CE), vrd16(G_LAP_POS));
    charge(3);
    return RD_RTS;
}

/* FUN_00028dd4: the course's corner-warning points (list at 0x28E2C [course]:
 * count, then segments): entering one (segment .. segment+16) the first time
 * requests cue bit 4; its flag (0x580C + the list's offset) clears once the
 * car is outside it */
static uint32_t rd_cue_corners(void)
{
    regs_t r; regs_load(&r);
    r.d[0] = (uint32_t)((vrd16(G_COURSE) & 0xF) << 2);
    r.a[0] = vrd32(0x28E2Cu + r.d[0]);
    r.a[2] = r.a[0] - 0x28E6C;
    r.a[1] = A6W(0x580C) + r.a[2];
    uint16_t n = (uint16_t)vrd16(r.a[0]); r.a[0] += 2;
    r.d[7] = setw(r.d[7], n);
    charge(14);
    if (n) {
        r.a[1] += 2;
        r.d[1] = setw(r.d[1], vrd16(G_SEGMENT));
        r.d[7] = setw(r.d[7], (uint16_t)(n - 1));
        charge(3);
        for (;;) {
            uint16_t p = (uint16_t)vrd16(r.a[0]);
            r.d[0] = setw(r.d[0], p);
            charge(3);
            int in = 0;
            if ((int16_t)r.d[1] >= (int16_t)p) {
                p = (uint16_t)(p + 0x10);
                r.d[0] = setw(r.d[0], p);
                charge(3);
                in = (int16_t)r.d[1] <= (int16_t)p;
            }
            if (in) {
                charge(2);
                if (!vrd16(r.a[1])) {
                    vwr16(r.a[1], 1);
                    vwr8(G_CUE_FLAGS, vrd8(G_CUE_FLAGS) | 0x10);
                    charge(2);
                }
                charge(1);
                regs_store(&r);
                return RD_RTS;
            }
            vwr16(r.a[1], 0);
            r.a[0] += 2; r.a[1] += 2;
            charge(4);
            uint16_t c = (uint16_t)(r.d[7] - 1); r.d[7] = setw(r.d[7], c);
            if (c == 0xFFFF) break;
            poll();
        }
    }
    charge(1);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_00028e94: the course's speed-trap points (lists at 0x28F12 [course]:
 * segments, minimum speeds, cues): entering one (segment .. segment+16) the
 * first time at a speed (0x4022, capped at 0x1600) of at least its minimum
 * requests cue bit 5 with that cue in 0x57DA; flags at 0x5834 + offset.
 * Outside all of them 0x57DA is cleared. */
static uint32_t rd_cue_speed_traps(void)
{
    regs_t r; regs_load(&r);
    r.d[0] = (uint32_t)((vrd16(G_COURSE) & 0xF) << 4);
    r.a[0] = 0x28F12u + r.d[0];
    r.a[1] = vrd32(r.a[0]); r.a[2] = vrd32(r.a[0] + 4); r.a[3] = vrd32(r.a[0] + 8);
    r.a[0] = r.a[0] + 8;
    r.a[4] = 0x29012;
    r.a[0] = r.a[1] - r.a[4];
    r.a[4] = A6W(0x5834);
    r.a[0] += r.a[4];
    uint16_t n = (uint16_t)vrd16(r.a[1]); r.a[1] += 2;
    r.d[7] = setw(r.d[7], n);
    charge(16);
    if (n) {
        r.a[0] += 2; r.a[2] += 2; r.a[3] += 2;
        r.d[1] = setw(r.d[1], vrd16(G_SEGMENT));
        uint16_t sp = (uint16_t)vrd16(A6W(0x4022));
        charge(7);
        if (sp > 0x1600) { charge(1); sp = 0x1600; }
        r.d[2] = setw(r.d[2], sp);
        r.d[7] = setw(r.d[7], (uint16_t)(n - 1));
        charge(1);
        for (;;) {
            uint16_t p = (uint16_t)vrd16(r.a[1]);
            r.d[0] = setw(r.d[0], p);
            charge(3);
            int in = 0;
            if ((int16_t)r.d[1] >= (int16_t)p) {
                p = (uint16_t)(p + 0x10);
                r.d[0] = setw(r.d[0], p);
                charge(3);
                in = (int16_t)r.d[1] <= (int16_t)p;
            }
            if (in) {
                charge(2);
                if (vrd16(r.a[0])) { charge(1); regs_store(&r); return RD_RTS; }
                charge(2);
                if ((int16_t)r.d[2] >= (int16_t)vrd16(r.a[2])) {
                    vwr16(r.a[0], 1);
                    vwr8(G_CUE_FLAGS, vrd8(G_CUE_FLAGS) | 0x20);
                    vwr16(A6W(0x57DA), vrd16(r.a[3]));
                    charge(4);
                    regs_store(&r);
                    return RD_RTS;
                }
            }
            vwr16(r.a[0], 0);
            r.a[0] += 2; r.a[1] += 2; r.a[2] += 2; r.a[3] += 2;
            charge(6);
            uint16_t c = (uint16_t)(r.d[7] - 1); r.d[7] = setw(r.d[7], c);
            if (c == 0xFFFF) break;
            regs_store(&r); poll();
        }
    }
    vwr16(A6W(0x57DA), 0);
    charge(2);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_0002914c: FUN_00029158, FUN_000291fe, continuing in FUN_0002948e */
static uint32_t rd_cue_rivals(void)
{
    charge(1); CALL(L_29158, 0x029150);
    charge(1); CALL(L_291FE, 0x029154);
    charge(1);
    return 0x02948E;
}

/* FUN_00029158: the "overtaken" voice (not in attract 0x57D8 or while 0x4866):
 * 0x57F4 holds it off for 0x1A4 frames. Linked: when bit 1 of 0x4201 is set;
 * single: when one of the other cars (0..7, not ours) on our segment (flag bit
 * 6 of its slot clear) has its bit in 0x4201. Picks the voice 0x291FA [random
 * & 1] into 0x57F0 and requests bit 0 of 0x5882. */
static uint32_t rd_cue_overtaken(void)
{
    regs_t r; regs_load(&r);
    charge(2);
    if (vrd16(A6W(0x57F4))) {
        vwr16(A6W(0x57F2), (uint16_t)(vrd16(A6W(0x57F2)) + 1));
        charge(3);
        if ((int16_t)vrd16(A6W(0x57F2)) >= 0x1A4) { charge(2); vwr16(A6W(0x57F2), 0); vwr16(A6W(0x57F4), 0); }
    }
    charge(2);
    if (vrd16(A6W(0x4866))) { charge(1); return RD_RTS; }
    charge(2);
    if (vrd16(A6W(0x57D8))) { charge(1); return RD_RTS; }
    charge(2);
    if (vrd16(A6W(0x1100))) {
        r.d[7] = 1;
        charge(3);
        if (vrd8(A6W(0x4201)) & 2) {
            r.d[5] = vrd32(A6W(0x5222)) & 1;
            vwr16(A6W(0x57F0), vrd16(0x291FAu + r.d[5] * 2u));
            vwr8(G_CUE_FLAGS2, vrd8(G_CUE_FLAGS2) | 1);
            charge(5);
        }
        charge(1);
        regs_store(&r);
        return RD_RTS;
    }
    r.a[0] = A6W(0x16AE); r.a[1] = A6W(0x11AC); r.d[7] = 0;
    charge(3);
    for (;;) {
        charge(2);
        if ((uint16_t)r.d[7] != (uint16_t)vrd16(G_LINK_ME)) {
            r.d[4] = setw(r.d[4], vrd16(r.a[1] + 2));
            charge(3);
            if ((uint16_t)r.d[4] == (uint16_t)vrd16(A6W(0x11A2))) {
                charge(2);
                if (!(vrd8(r.a[1] + 0x11B6) & 0x40)) {
                    charge(2);
                    if (vrd8(A6W(0x4201)) & (1u << (r.d[7] & 7))) {
                        r.d[5] = vrd32(A6W(0x5222)) & 1;
                        vwr16(A6W(0x57F0), vrd16(0x291FAu + r.d[5] * 2u));
                        vwr8(G_CUE_FLAGS2, vrd8(G_CUE_FLAGS2) | 1);
                        charge(5);
                    }
                }
            }
        }
        r.a[0] += 0x40; r.a[1] += 0x10;
        r.d[7] = setw(r.d[7], r.d[7] + 1);
        charge(5);
        if ((int16_t)r.d[7] >= 8) break;
        poll();
    }
    charge(1);
    regs_store(&r);
    return RD_RTS;
}

/* result of the rival voice window tests in Z (Z set: "no voice now") */
static uint32_t rival_window_loop(regs_t *r, void (*test)(void), uint32_t ret1, uint32_t ret2)
{
    charge(2);
    if (vrd16(A6W(0x1100))) {
        r->d[7] = 1;
        r->a[0] = A6W(0x16AE) + 0x40;
        r->d[3] = setw(r->d[3], vrd16(r->a[0]));
        charge(5);
        regs_store(r);
        CALL(test, ret1);
        charge(1);
        if (!flag_z()) { vwr8(A6W(0x57FA), vrd8(A6W(0x57FA)) & (uint8_t)~(1u << (d_reg(7) & 7))); }
        else vwr8(A6W(0x57FA), vrd8(A6W(0x57FA)) | (uint8_t)(1u << (d_reg(7) & 7)));
        charge(2);
        return RD_RTS;
    }
    r->a[0] = A6W(0x16AE); r->a[1] = A6W(0x11AC); r->d[7] = 0;
    charge(3);
    for (;;) {
        charge(2);
        if ((uint16_t)r->d[7] != (uint16_t)vrd16(G_LINK_ME)) {
            r->d[4] = setw(r->d[4], vrd16(r->a[1] + 2));
            charge(3);
            if ((uint16_t)r->d[4] == (uint16_t)vrd16(A6W(0x11A2))) {
                charge(2);
                if (!(vrd8(r->a[1] + 0x11B6) & 0x40)) {
                    r->d[3] = setw(r->d[3], vrd16(r->a[0]));
                    charge(2);
                    regs_store(r);
                    CALL(test, ret2);
                    regs_load(r);
                    charge(1);
                    if (!flag_z()) { charge(1); vwr8(A6W(0x57FA), vrd8(A6W(0x57FA)) & (uint8_t)~(1u << (r->d[7] & 7))); }
                    else { charge(2); vwr8(A6W(0x57FA), vrd8(A6W(0x57FA)) | (uint8_t)(1u << (r->d[7] & 7))); }
                }
            }
        }
        r->a[0] += 0x40; r->a[1] += 0x10;
        r->d[7] = setw(r->d[7], r->d[7] + 1);
        charge(5);
        if ((int16_t)r->d[7] >= 8) break;
        regs_store(r); poll();
    }
    charge(1);
    regs_store(r);
    return RD_RTS;
}

/* FUN_000291fe: the rival voices by where the car is: 0x57FC holds them off
 * for 300 frames after 0x57FE; none in attract (0x57D8) or while 0x4866
 * (0x57FA = -1). Segment < 30 selects the start voices (FUN_000292da),
 * beyond the variant's line (0x2925C) the "default" ones (0x293D2), else the
 * mid-race ones (FUN_0002938e), each tested for the linked rival or for
 * every other car on our segment, with the answer kept in bit n of 0x57FA. */
static uint32_t rd_cue_rival_voices(void)
{
    regs_t r; regs_load(&r);
    charge(2);
    if (vrd16(A6W(0x57FE))) {
        vwr16(A6W(0x57FC), (uint16_t)(vrd16(A6W(0x57FC)) + 1));
        charge(3);
        if ((int16_t)vrd16(A6W(0x57FC)) >= 0x12C) { charge(2); vwr16(A6W(0x57FC), 0); vwr16(A6W(0x57FE), 0); }
    }
    charge(2);
    int off = vrd16(A6W(0x4866)) != 0;
    if (!off) { charge(2); off = vrd16(A6W(0x57D8)) != 0; }
    if (off) { vwr16(A6W(0x57FA), 0xFFFF); charge(2); return RD_RTS; }
    r.d[1] = 0;
    uint16_t seg = (uint16_t)vrd16(G_SEGMENT);
    r.d[0] = setw(r.d[0], seg);
    charge(4);
    if ((int16_t)seg < 0x1E) { charge(1); r.d[1] = 1; }
    uint16_t v = (uint16_t)(vrd16(G_COURSE_VAR) & 3);
    r.d[2] = setw(r.d[2], v);
    charge(5);
    if ((int16_t)seg > (int16_t)vrd16(0x2925Cu + (uint32_t)v * 2u)) { charge(1); r.d[1] = 2; }
    uint16_t sel = (uint16_t)r.d[1];
    r.d[1] = setw(r.d[1], (uint16_t)vrd16(0x29256u + (uint32_t)sel * 2u));
    charge(3);
    regs_store(&r); poll();
    if (sel == 2) { regs_store(&r); return 0x0293D2; }          /* case table entry into FUN_000293d2 */
    if (sel == 1) r.d[6] = setw(r.d[6], r.d[2]);
    r.d[1] = setw(r.d[1], (uint16_t)(seg + 0x1E));
    r.d[2] = setw(r.d[2], (uint16_t)(seg - 0x1E));
    charge(sel == 1 ? 5 : 4);
    if (sel == 1) return rival_window_loop(&r, L_2938E, 0x02933A, 0x029370);
    return rival_window_loop(&r, L_292DA, 0x029286, 0x0292BC);
}

/* FUN_000293d2: the rival voices past the variant's line (case 2 of
 * FUN_000291fe's table): as the others, tested by FUN_0002944a */
static uint32_t rd_cue_rival_late(void)
{
    regs_t r; regs_load(&r);
    r.d[6] = setw(r.d[6], r.d[2]);
    r.d[1] = setw(r.d[1], (uint16_t)(r.d[0] + 0x1E));
    r.d[2] = setw(r.d[2], (uint16_t)(r.d[0] - 0x1E));
    charge(5);
    return rival_window_loop(&r, L_2944A, 0x0293F6, 0x02942C);
}

/* pick the rival voice 0x2930E [d4 + random & 1] into 0x57F8 and request bit 1
 * of 0x5882 (move.l, andi.l, add, move, bset) */
static void rival_voice(uint16_t d4)
{
    uint32_t d5 = vrd32(A6W(0x5222)) & 1;
    set_d(5, d5);
    d4 = (uint16_t)(d4 + d5);
    set_d16(4, d4);
    vwr16(A6W(0x57F8), vrd16(0x2930Eu + (uint32_t)sxw(d4) * 2u));
    vwr8(G_CUE_FLAGS2, vrd8(G_CUE_FLAGS2) | 2);
    charge(5);
}

/* FUN_000292da: start voice test for the rival at D3w: in D2w..D1w and its
 * bit (D7) of 0x57FA clear -> voice (0/1 behind, 2/3 ahead of D0w) and Z set;
 * already announced -> Z set; out of range -> Z clear */
static uint32_t rd_rival_start_test(void)
{
    uint16_t d0 = (uint16_t)d_reg(0), d1 = (uint16_t)d_reg(1), d2 = (uint16_t)d_reg(2), d3 = (uint16_t)d_reg(3);
    charge(2);
    if ((int16_t)d3 > (int16_t)d1) { flags_cmp_w(d3, d1); charge(1); return RD_RTS; }
    charge(2);
    if ((int16_t)d3 < (int16_t)d2) { flags_cmp_w(d3, d2); charge(1); return RD_RTS; }
    charge(2);
    if (!(vrd8(A6W(0x57FA)) & (1u << (d_reg(7) & 7)))) {
        uint16_t d4 = 0;
        set_d(4, 0);
        charge(3);
        if ((int16_t)d3 <= (int16_t)d0) { charge(1); d4 = 2; set_d(4, 2); }
        charge(1);
        rival_voice(d4);
    }
    flags_cmp_w(d0, d0);
    charge(2);
    return RD_RTS;
}

/* FUN_0002938e: mid-race voice test: D2w is moved on by the course's lead
 * (0x293CA [D6]); inside the window -> voice 0/1 (ahead) or 2/3 (behind) and
 * Z set; otherwise Z clear (the bra path keeps the last compare) */
static uint32_t rd_rival_mid_test(void)
{
    charge(2);
    if (!(vrd8(A6W(0x57FA)) & (1u << (d_reg(7) & 7)))) {
        uint16_t d0 = (uint16_t)d_reg(0), d1 = (uint16_t)d_reg(1), d3 = (uint16_t)d_reg(3);
        set_d(4, 2);
        uint16_t d2 = (uint16_t)(d_reg(2) + vrd16(0x293CAu + (uint32_t)sxw(d_reg(6)) * 2u));
        set_d16(2, d2);
        charge(5);
        uint16_t d4 = 2;
        if ((int16_t)d3 < (int16_t)d2) {
            charge(2);
            if ((int16_t)d3 > (int16_t)d0) {
                set_d(4, 0); d4 = 0;
                charge(3);
                if ((int16_t)d3 > (int16_t)d1) { flags_cmp_w(d3, d1); charge(2); return RD_RTS; }
            }
        }
        rival_voice(d4);
    }
    flags_cmp_w(d_reg(0), d_reg(0));
    charge(2);
    return RD_RTS;
}

/* FUN_0002944a: late voice test: D1w is moved back by 0x29486 [D6]; outside
 * the window -> voice 0/1 or 2/3 and Z set; inside -> Z clear */
static uint32_t rd_rival_late_test(void)
{
    charge(2);
    if (!(vrd8(A6W(0x57FA)) & (1u << (d_reg(7) & 7)))) {
        uint16_t d0 = (uint16_t)d_reg(0), d2 = (uint16_t)d_reg(2), d3 = (uint16_t)d_reg(3);
        set_d(4, 0);
        uint16_t d1 = (uint16_t)(d_reg(1) - vrd16(0x29486u + (uint32_t)sxw(d_reg(6)) * 2u));
        set_d16(1, d1);
        charge(5);
        uint16_t d4 = 0;
        if ((int16_t)d3 > (int16_t)d1) {
            charge(2);
            if ((int16_t)d3 <= (int16_t)d0) {
                set_d(4, 2); d4 = 2;
                charge(3);
                if ((int16_t)d3 < (int16_t)d2) { flags_cmp_w(d3, d2); charge(2); return RD_RTS; }
            }
        }
        rival_voice(d4);
    }
    flags_cmp_w(d_reg(0), d_reg(0));
    charge(2);
    return RD_RTS;
}

/* FUN_0002948e: the "final lap" timer 0x57C8: cleared on courses 2 and 3 and
 * whenever 0x4866 is clear; else counts up, requesting bit 2 of 0x5882 when it
 * starts and again every 600 frames */
static uint32_t rd_cue_final_lap(void)
{
    uint16_t c = (uint16_t)vrd16(G_COURSE);
    charge(2);
    int clr = c == 2;
    if (!clr) { charge(2); clr = c == 3; }
    if (!clr) { charge(2); clr = vrd16(A6W(0x4866)) == 0; }
    if (clr) { vwr16(A6W(0x57C8), 0); charge(2); return RD_RTS; }
    uint16_t t = (uint16_t)vrd16(A6W(0x57C8));
    charge(2);
    int req = 1;
    if (t) {
        charge(2);
        if (t == 0x258) { charge(1); vwr16(A6W(0x57C8), 0); }
        else req = 0;
    }
    if (req) { charge(1); vwr8(G_CUE_FLAGS2, vrd8(G_CUE_FLAGS2) | 4); }
    vwr16(A6W(0x57C8), (uint16_t)(vrd16(A6W(0x57C8)) + 1));
    charge(2);
    return RD_RTS;
}

/* FUN_000294c8: serve the cue requests of 0x5880 (bits 7, 0, 1, 2, 4, 5 in
 * that order), continuing with bit 6 in FUN_0002978a */
static uint32_t rd_cue_serve(void)
{
    charge(1); CALL(L_294E4, 0x0294CC);
    charge(1); CALL(L_29550, 0x0294D0);
    charge(1); CALL(L_29568, 0x0294D4);
    charge(1); CALL(L_294FC, 0x0294D8);
    charge(1); CALL(L_29580, 0x0294DC);
    charge(1); CALL(L_29770, 0x0294E0);
    charge(1);
    return 0x02978A;
}

/* one fixed cue: if bit b of 0x5880, send cue v (FUN_000297b6) and clear the bit */
static uint32_t cue_bit(int b, uint16_t v, uint32_t ret)
{
    charge(2);
    if (!(vrd8(G_CUE_FLAGS) & (1u << b))) { charge(1); return RD_RTS; }
    set_d16(1, v);
    charge(2); CALL(L_297B6, ret);
    vwr8(G_CUE_FLAGS, vrd8(G_CUE_FLAGS) & (uint8_t)~(1u << b));
    charge(2);
    return RD_RTS;
}
/* FUN_000294e4: bit 7 -> cue 0x4094 ("good time") */
static uint32_t rd_cue_bit7(void) { return cue_bit(7, 0x4094, 0x0294F4); }
/* FUN_00029550: bit 0 -> cue 0x4092 */
static uint32_t rd_cue_bit0(void) { return cue_bit(0, 0x4092, 0x029560); }
/* FUN_00029568: bit 1 -> cue 0x409F */
static uint32_t rd_cue_bit1(void) { return cue_bit(1, 0x409F, 0x029578); }

/* FUN_000294fc: bit 2 (new lap): the laps-to-go cue 0x29530 [(0x57CC -
 * 0x204A - 1) & 15] when there are laps left and it is not 0 */
static uint32_t rd_cue_laps_left(void)
{
    charge(2);
    if (!(vrd8(G_CUE_FLAGS) & 4)) { charge(1); return RD_RTS; }
    uint16_t lap = (uint16_t)vrd16(G_LAP_POS);
    set_d16(1, lap);
    charge(2);
    if (lap & 0x8000) { charge(1); return RD_RTS; }
    charge(1);
    if (!lap) { charge(1); return RD_RTS; }
    uint16_t d0 = (uint16_t)(vrd16(A6W(0x57CC)) - lap - 1);
    set_d(0, d0);
    charge(5);
    if (d0 & 0x8000) { charge(1); return RD_RTS; }
    d0 &= 0xF;
    set_d(0, d0);
    uint16_t cue = (uint16_t)vrd16(0x29530u + (uint32_t)d0 * 2u);
    set_d16(1, cue);
    charge(4);
    if (cue) { charge(1); CALL(L_297B6, 0x029528); }
    vwr8(G_CUE_FLAGS, vrd8(G_CUE_FLAGS) & (uint8_t)~4u);
    charge(2);
    return RD_RTS;
}

/* FUN_00029580: bit 4 (corner warning): a random cue from the list 0x295C0
 * [((0x204E - 1) & 15) + (linked ? 0 : 16)] (count, then cues) */
static uint32_t rd_cue_corner_voice(void)
{
    charge(2);
    if (!(vrd8(G_CUE_FLAGS) & 0x10)) { charge(1); return RD_RTS; }
    uint16_t d0 = (uint16_t)vrd16(A6W(0x204E));
    set_d16(0, d0);
    charge(2);
    if (!d0) { charge(1); return RD_RTS; }
    d0 = (uint16_t)((d0 - 1) & 0xF);
    charge(4);
    if (!vrd16(A6W(0x1100))) { charge(1); d0 = (uint16_t)(d0 + 0x10); }
    uint32_t a0 = vrd32(0x295C0u + (uint32_t)sxw(d0) * 4u);
    uint16_t n = (uint16_t)vrd16(a0); a0 += 2;
    set_d16(0, n); set_a(0, a0);
    charge(4);
    if (n) {
        uint32_t d1 = vrd32(A6W(0x5222));
        uint32_t dd0 = (d_reg(0) & 0xFFFF0000u) | (uint16_t)(n - 1);
        set_d(0, dd0);
        d1 &= dd0;
        d1 = (d1 & 0xFFFF0000u) | (uint16_t)vrd16(a0 + (uint32_t)sxw(d1) * 2u);
        set_d(1, d1);
        charge(5); CALL(L_297B6, 0x0295B8);
    }
    vwr8(G_CUE_FLAGS, vrd8(G_CUE_FLAGS) & (uint8_t)~0x10u);
    charge(2);
    return RD_RTS;
}

/* FUN_00029770: bit 5 (speed trap): its cue 0x57DA, if any */
static uint32_t rd_cue_speed_voice(void)
{
    charge(2);
    if (!(vrd8(G_CUE_FLAGS) & 0x20)) { charge(1); return RD_RTS; }
    uint16_t v = (uint16_t)vrd16(A6W(0x57DA));
    set_d16(1, v);
    charge(2);
    if (v) { charge(1); CALL(L_297B6, 0x029782); }
    vwr8(G_CUE_FLAGS, vrd8(G_CUE_FLAGS) & (uint8_t)~0x20u);
    charge(2);
    return RD_RTS;
}

/* FUN_0002978a: bit 6: cue 0x57DE once the cue channel is free (bits 15/14
 * of 0x6000501E clear); the bit is dropped either way */
static uint32_t rd_cue_bit6(void)
{
    charge(2);
    if (!(vrd8(G_CUE_FLAGS) & 0x40)) { charge(1); return RD_RTS; }
    uint16_t c = (uint16_t)vrd16(SND_CUE);
    c = (uint16_t)((c << 8) | (c >> 8));
    set_d16(0, c);
    charge(4);
    if (!(d_reg(0) & 0x80)) {
        charge(2);
        if (!(d_reg(0) & 0x40)) {
            set_d16(1, vrd16(A6W(0x57DE)));
            charge(2); CALL(L_297B6, 0x0297AE);
        }
    }
    vwr8(G_CUE_FLAGS, vrd8(G_CUE_FLAGS) & (uint8_t)~0x40u);
    charge(2);
    return RD_RTS;
}

/* FUN_00029854: is the car in the variant's voice window (0x2987E [var]:
 * low, high; exclusive)? D0 = 1 and Z clear if so, D0 = 0 and Z set if not;
 * in attract (0x57D8) D0 is left and Z clear */
static uint32_t rd_voice_window(void)
{
    uint16_t a = (uint16_t)vrd16(A6W(0x57D8));
    charge(2);
    if (a) { set_nzvc(a >> 15, 0, 0, 0); charge(1); return RD_RTS; }
    uint16_t seg = (uint16_t)vrd16(G_SEGMENT);
    uint16_t v = (uint16_t)(vrd16(G_COURSE_VAR) & 3);
    set_d16(1, seg); set_d16(0, v);
    charge(6);
    int in = 0;
    if ((int16_t)seg > (int16_t)vrd16(0x2987Eu + (uint32_t)v * 4u)) {
        charge(3);
        in = (int16_t)seg < (int16_t)vrd16(0x29880u + (uint32_t)v * 4u);
    }
    if (in) { set_d(0, 1); set_nzvc(0, 0, 0, 0); charge(3); }
    else { set_d(0, 0); set_nzvc(0, 1, 0, 0); charge(2); }
    return RD_RTS;
}

/* ======================= EEPROM / records (0x2AD26..0x2D9C6) ======================= */

#define EEPROM      0x58000000u   /* 64-byte records */
#define EE_COPY     0x10001000u   /* WRAM copy of the EEPROM settings / records */
#define EE_MAGIC    0x56313534u   /* 'V154' -- a valid record */

/* FUN_0002ad26: read the 62-byte EEPROM header record D0 (at EEPROM + D0*64)
 * into 0x500E, summing its bytes; Z set when the complement of the sum
 * equals the checksum word after it (callers branch on it) */
static uint32_t rd_ee_read_header(void)
{
    regs_t r; regs_load(&r);
    r.a[0] = EEPROM;
    r.d[1] = setw(r.d[0], (uint16_t)(r.d[0] << 6));
    r.a[0] += (uint32_t)sxw(r.d[1]);
    r.d[1] = 0; r.d[2] = 0;
    r.a[1] = A6W(0x500E);
    r.d[6] = 0x3D;
    charge(8);
    for (;;) {
        r.d[1] = setb(r.d[1], vrd8(r.a[0]));
        vwr8(r.a[1], (uint8_t)r.d[1]);
        r.d[2] = setw(r.d[2], r.d[2] + r.d[1]);
        r.a[0]++; r.a[1]++;
        charge(6);
        uint16_t c = (uint16_t)(r.d[6] - 1); r.d[6] = setw(r.d[6], c);
        if (c == 0xFFFF) break;
        regs_store(&r); poll();
    }
    r.d[2] = setw(r.d[2], (uint16_t)~r.d[2]);
    flags_cmp_w(r.d[2], vrd16(r.a[0]));
    charge(2);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_0002ae0a: check the 4 record slots whose bits (8..11) are set in 0x5060:
 * each (bank by the nibble of 0x5012 rotated in turn) must start 'V154' or
 * 'BREK', else restart the EEPROM set-up at 0x2ABF0 */
static uint32_t rd_ee_check_slots(void)
{
    regs_t r; regs_load(&r);
    r.a[0] = A6W(0x500E);
    r.d[1] = setw(r.d[1], vrd16(r.a[0] + 4));
    r.a[1] = 0x5800021Au;
    r.d[0] = vrd32(A6W(0x5060));
    r.d[7] = 8;
    charge(5);
    for (;;) {
        uint16_t w = (uint16_t)r.d[1];
        r.d[1] = setw(r.d[1], (uint16_t)((w << 4) | (w >> 12)));
        charge(3);
        if (r.d[0] & (1u << (r.d[7] & 31))) {
            r.d[2] = setw(r.d[2], (uint16_t)((r.d[1] & 1) << 5));
            r.d[6] = r.d[7];
            r.d[6] = setw(r.d[6], (uint16_t)((r.d[6] << 6) + r.d[2]));
            uint32_t v = vrd32(r.a[1] + (uint32_t)sxw(r.d[6]));
            charge(8);
            if (v != EE_MAGIC) {
                charge(2);
                if (v != 0x4252454Bu) { charge(1); regs_store(&r); return RD_JMP(0x02ABF0); }
            }
        }
        r.d[7] = setw(r.d[7], r.d[7] + 1);
        charge(3);
        if ((int16_t)r.d[7] >= 0xC) break;
        regs_store(&r); poll();
    }
    charge(1);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_0002ae7e: read the 30-byte settings group D2 (bank D3) from the EEPROM
 * (0x58000200 + D2*64 + D3*32) into 0x10001000 + D2*32, summing its bytes; Z
 * set when the complement of the sum equals the checksum word after it */
static uint32_t rd_ee_read_group(void)
{
    regs_t r; regs_load(&r);
    r.a[1] = 0x58000200u; r.a[2] = EE_COPY;
    r.d[4] = r.d[2];
    r.d[4] = setw(r.d[4], (uint16_t)(r.d[4] << 5)); r.a[2] += (uint32_t)sxw(r.d[4]);
    r.d[4] = setw(r.d[4], (uint16_t)(r.d[4] << 1)); r.a[1] += (uint32_t)sxw(r.d[4]);
    r.d[4] = r.d[3];
    r.d[4] = setw(r.d[4], (uint16_t)(r.d[4] << 5)); r.a[1] += (uint32_t)sxw(r.d[4]);
    r.d[4] = 0; r.d[5] = 0;
    r.d[6] = 0x1D;
    charge(13);
    for (;;) {
        r.d[4] = setb(r.d[4], vrd8(r.a[1]));
        vwr8(r.a[2], (uint8_t)r.d[4]);
        r.d[5] = setw(r.d[5], r.d[5] + r.d[4]);
        r.a[1]++; r.a[2]++;
        charge(6);
        uint16_t c = (uint16_t)(r.d[6] - 1); r.d[6] = setw(r.d[6], c);
        if (c == 0xFFFF) break;
        regs_store(&r); poll();
    }
    r.d[5] = setw(r.d[5], (uint16_t)~r.d[5]);
    flags_cmp_w(r.d[5], vrd16(r.a[1]));
    charge(2);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_0002b172: blank the 32-byte record buffer at 0x50A0: 13 zero words,
 * the 'V154' mark, one more zero word */
static uint32_t rd_ee_blank_record(void)
{
    /* registers live at every poll (the vblank handler saves them) */
    uint32_t a0 = A6W(0x50A0);
    set_a(0, a0); set_d(7, 12);
    charge(2);                                          /* lea, moveq */
    for (int n = 12; n >= 0; n--) {
        vwr16(a0, 0); a0 += 2;
        set_a(0, a0); set_d16(7, (uint16_t)(n - 1));
        charge(2);                                      /* clr.w, dbf */
        if (n) poll();
    }
    vwr32(a0, EE_MAGIC); a0 += 4;
    vwr16(a0, 0); a0 += 2;
    set_a(0, a0);
    charge(2);                                          /* move.l, clr.w (the rts is the table cost) */
    return RD_RTS;
}

/* FUN_0002b29e: the EEPROM error messages: while the 0x50C2 timer runs (not
 * on screens 2/3 of 0xDA0) print the message (table 0x2B2EE) of each error
 * bit 1..4 of 0x50C1; when it reaches 1 (or on those screens) clear it and
 * print the clearing text 0x2B32F */
static uint32_t rd_ee_messages(void)
{
    charge(2);
    uint16_t t = (uint16_t)vrd16(A6W(0x50C2));
    if (!t) { charge(1); return RD_RTS; }
    charge(2);
    int clear = t == 1;
    if (!clear) {
        vwr16(A6W(0x50C2), (uint16_t)(t - 1));
        charge(3);
        uint16_t s = (uint16_t)vrd16(A6W(0x0DA0));
        clear = s == 2;
        if (!clear) { charge(2); clear = s == 3; }
    }
    if (clear) {
        vwr16(A6W(0x50C2), 0);
        set_a(1, 0x2B32F);
        charge(3);
        return RD_JMP(0x005A0A);
    }
    set_d(2, 1); set_d(6, 3);
    charge(2);
    for (;;) {
        charge(2);
        if (vrd8(A6W(0x50C1)) & (1u << (d_reg(2) & 7))) {
            set_a(1, vrd32(0x2B2EEu + (uint32_t)sxw(d_reg(2)) * 4u));
            charge(3); CALL(L_5A0A, 0x02B2D8);
        }
        set_d16(2, (uint16_t)(d_reg(2) + 1));
        charge(2);
        uint16_t c = (uint16_t)(d_reg(6) - 1); set_d16(6, c);
        if (c == 0xFFFF) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* FUN_0002b998: print the text 0x2B9BA and copy the three setting words at
 * 0x10001000 to 0x1000081A */
static uint32_t rd_ee_show_settings(void)
{
    set_a(1, 0x2B9BA);
    charge(2); CALL(L_5A0A, 0x02B9A2);
    set_a(0, EE_COPY);
    vwr16(0x1000081Au, vrd16(EE_COPY));
    vwr16(0x1000081Cu, vrd16(EE_COPY + 2));
    vwr16(0x1000081Eu, vrd16(EE_COPY + 4));
    charge(4);
    return RD_RTS;
}

/* FUN_0002b9d4: build the 32-byte best-time record at 0x50A0 from the four
 * (time, name) pairs of the current table (0x50C6 + (0x5200 & 1)*32: 24-bit
 * time << 8 | name byte, then the name's low word), copy it to the settings
 * copy's record 0x51E6 (0x10001000 + n*32), and unless bit n of 0x5060 is set
 * continue in FUN_0002b188 (write it) */
static uint32_t rd_ee_build_record(void)
{
    charge(1); CALL(L_2B172, 0x02B9D8);
    regs_t r; regs_load(&r);
    r.a[0] = A6W(0x50A0); r.a[1] = A6W(0x50C6);
    r.d[7] = setw(r.d[7], (uint16_t)((vrd16(A6W(0x5200)) & 1) << 5));
    r.a[1] += (uint32_t)sxw(r.d[7]);
    r.d[6] = 3;
    charge(7);
    for (;;) {
        r.d[0] = vrd32(r.a[1]); r.d[1] = vrd32(r.a[1] + 4); r.a[1] += 8;
        r.d[2] = r.d[1];
        r.d[0] <<= 8;
        r.d[1] = ((r.d[1] << 16) | (r.d[1] >> 16)) & 0xFF;
        r.d[0] |= r.d[1];
        vwr32(r.a[0], r.d[0]); vwr16(r.a[0] + 4, (uint16_t)r.d[2]); r.a[0] += 6;
        charge(10);
        uint16_t c = (uint16_t)(r.d[6] - 1); r.d[6] = setw(r.d[6], c);
        if (c == 0xFFFF) break;
        regs_store(&r); poll();
    }
    r.a[0] = A6W(0x50A0); r.a[1] = EE_COPY;
    r.d[7] = vrd32(A6W(0x51E6));
    r.d[7] = setw(r.d[7], (uint16_t)(r.d[7] << 5));
    r.a[1] += (uint32_t)sxw(r.d[7]);
    r.d[7] = 0xC;
    charge(6);
    for (;;) {
        vwr16(r.a[1], vrd16(r.a[0])); r.a[0] += 2; r.a[1] += 2;
        charge(2);
        uint16_t c = (uint16_t)(r.d[7] - 1); r.d[7] = setw(r.d[7], c);
        if (c == 0xFFFF) break;
        regs_store(&r); poll();
    }
    r.d[0] = vrd32(A6W(0x5060));
    r.d[7] = vrd32(A6W(0x51E6));
    charge(4);
    regs_store(&r);
    if (r.d[0] & (1u << (r.d[7] & 31))) { charge(1); return RD_RTS; }
    charge(1);
    return RD_JMP(0x02B188);
}

/* FUN_0002ba34: unpack the two best-time tables from the settings copy
 * (0x10001020 and 0x100010C0, four 6-byte entries each: 24-bit time, then
 * name) into 0x5106/0x5126 (+16 per table) and the working copy at 0x50C6 */
static uint32_t rd_ee_unpack_times(void)
{
    regs_t r; regs_load(&r);
    r.a[0] = 0x10001020u; r.a[1] = A6W(0x50C6); r.d[7] = 0;
    charge(3);
    for (;;) {
        for (int t = 0; t < 2; t++) {
            uint32_t base = t ? 0x5126 : 0x5106;
            r.d[5] = r.d[7];
            r.d[5] = setw(r.d[5], (uint16_t)(r.d[5] << 2));
            r.d[6] = 1;
            charge(3);
            for (;;) {
                r.d[0] = vrd32(r.a[0]); r.a[0] += 4;
                r.d[2] = (uint16_t)vrd16(r.a[0]); r.a[0] += 2;
                r.d[1] = r.d[0];
                r.d[0] >>= 8;
                r.d[1] = (((r.d[1] << 16) | (r.d[1] >> 16)) & 0xFF0000u) | r.d[2];
                vwr32(A6W(base) + (uint32_t)(sxw(r.d[5]) * 4), r.d[0]);
                vwr32(r.a[1], r.d[0]); r.a[1] += 4;
                r.d[5] += 1;
                vwr32(A6W(base) + (uint32_t)(sxw(r.d[5]) * 4), r.d[1]);
                vwr32(r.a[1], r.d[1]); r.a[1] += 4;
                r.d[5] += 1;
                charge(15);
                uint16_t c = (uint16_t)(r.d[6] - 1); r.d[6] = setw(r.d[6], c);
                if (c == 0xFFFF) break;
                regs_store(&r); poll();
            }
        }
        r.a[0] = 0x100010C0u;
        r.d[7] = setw(r.d[7], r.d[7] + 1);
        charge(4);
        if ((int16_t)r.d[7] >= 2) break;
        regs_store(&r); poll();
    }
    charge(1);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_0002bbb6: unpack the four 6-byte records at 0x100010E0 (24-bit value,
 * then three bytes) into 0x593E (longs) and 0x594E (three words each, 8 apart) */
static uint32_t rd_ee_unpack_4(void)
{
    regs_t r; regs_load(&r);
    r.a[0] = 0x100010E0u; r.a[3] = A6W(0x593E); r.a[2] = A6W(0x594E); r.d[7] = 3;
    charge(4);
    for (;;) {
        r.d[0] = vrd32(r.a[0]) >> 8;
        vwr32(r.a[3], r.d[0]); r.a[3] += 4;
        for (int k = 0; k < 3; k++) {
            r.d[0] = setw(r.d[0], (uint16_t)(vrd16(r.a[0] + 2 + (uint32_t)k) & 0xFF));
            vwr16(r.a[2] + 2u * (uint32_t)k, (uint16_t)r.d[0]);
        }
        r.a[0] += 6; r.a[2] += 8;
        charge(15);
        uint16_t c = (uint16_t)(r.d[7] - 1); r.d[7] = setw(r.d[7], c);
        if (c == 0xFFFF) break;
        regs_store(&r); poll();
    }
    charge(1);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_0002c1cc: the play statistics: count this game in the counter for its
 * kind (0x100011E0 + k*4; k = 2 single / 0 linked / 4 linked with bit 2 of
 * 0x2343, +1 with 0x2340), adding the play time 0x5206 to 0x520A + k*4; both
 * reset when the count passes 999999 or the time 0x66FF2E2 */
static uint32_t rd_play_stats(void)
{
    uint32_t d0;
    charge(2);
    if (!vrd16(A6W(0x1100))) { d0 = 2; charge(2); }
    else {
        d0 = 0;
        charge(3);
        if (vrd8(A6W(0x2343)) & 4) { charge(1); d0 = 4; }
    }
    charge(2);
    if (vrd16(A6W(0x2340))) { charge(1); d0 = (d0 & 0xFFFF0000u) | (uint16_t)(d0 + 1); }
    set_d(0, d0);
    uint32_t a0 = 0x100011E0u, cnt = a0 + (uint32_t)sxw(d0) * 4u, tm = A6W(0x520A) + (uint32_t)sxw(d0) * 4u;
    set_a(0, a0);
    vwr32(cnt, vrd32(cnt) + 1);
    charge(4);
    if (vrd32(cnt) > 0xF423F) { vwr32(cnt, 0); vwr32(tm, 0); charge(3); return RD_RTS; }
    uint32_t d1 = vrd32(A6W(0x5206));
    set_d(1, d1);
    vwr32(tm, vrd32(tm) + d1);
    charge(4);
    if (vrd32(tm) > 0x66FF2E2u) { charge(2); vwr32(tm, 0); vwr32(cnt, 0); }
    charge(1);
    return RD_RTS;
}

/* FUN_0002c248: after a race with a new best time (0x4988), pick this
 * table's record number (0x2C292 [(0x2342 >> 2) & 1]) into 0x51E6, gather its
 * four times and names (0x5106/0x5126) into 0x50C6, and continue in
 * FUN_0002b9d4 to save it */
static uint32_t rd_ee_save_best(void)
{
    charge(2);
    if (!vrd16(A6W(0x4988))) { charge(1); return RD_RTS; }
    regs_t r; regs_load(&r);
    r.d[0] = setw(r.d[0], (uint16_t)(vrd16(A6W(0x2342)) >> 2));
    r.d[0] &= 1;
    vwr16(A6W(0x5200), (uint16_t)r.d[0]);
    vwr32(A6W(0x51E6), vrd32(0x2C292u + r.d[0] * 4u));
    r.d[1] = setw(r.d[1], (uint16_t)r.d[0]);
    r.d[0] = setw(r.d[0], (uint16_t)(r.d[0] << 5));
    r.d[1] = setw(r.d[1], (uint16_t)(r.d[1] << 4));
    r.a[1] = A6W(0x50C6) + (uint32_t)sxw(r.d[0]);
    r.a[0] = A6W(0x5106) + (uint32_t)sxw(r.d[1]);
    r.a[2] = A6W(0x5126) + (uint32_t)sxw(r.d[1]);
    r.d[7] = 3;
    charge(16);
    for (;;) {
        vwr32(r.a[1], vrd32(r.a[0])); r.a[0] += 4;
        vwr32(r.a[1] + 0x10, vrd32(r.a[2])); r.a[2] += 4;
        r.a[1] += 4;
        charge(4);
        uint16_t c = (uint16_t)(r.d[7] - 1); r.d[7] = setw(r.d[7], c);
        if (c == 0xFFFF) break;
        poll();
    }
    charge(1);
    regs_store(&r);
    return RD_JMP(0x02B9D4);
}

/* FUN_0002d9c6: unless 0x10001072 is set, reset the settings bytes (9 x3 at
 * 0x10001062, 4 x8 after: FUN_0002d9da) and continue in FUN_0002bb10 */
static uint32_t rd_ee_default_settings(void)
{
    set_a(1, 0x10001072u);
    charge(3);
    if (vrd8(0x10001072u)) { charge(1); return RD_RTS; }
    charge(1); CALL(L_2D9DA, 0x02D9D4);
    charge(1);
    return RD_JMP(0x02BB10);
}

/* ======================= boot RAM test (0x2FA68..0x2FF62) ======================= */

#define WATCHDOG    0x40000016u   /* syscon byte: written 0 to keep the board alive */

/* FUN_0002faf6: after a region's test of the power-on memory test (FUN_0002fa68,
 * whose region tests 0x2FE16 / 0x2FEE8 continue here through A3): (clear the text layer after the first),
 * print the region's name (popped from the stack) and "OK" (0x2FB9A) or "NG"
 * (0x2FBA1) by bit D2 of D7; next region at 0x2FAA4, and after the last save
 * D7*2 (the failed regions) in 0x50C4 */
static uint32_t rd_ramtest_next(void)
{
    charge(2);
    if ((uint16_t)d_reg(2) == 7) { charge(1); CALL(L_575A, 0x02FB02); }
    uint32_t sp = a_reg(7);
    set_a(1, vrd32(sp)); set_a(7, sp + 4);
    vwr16(A6W(0x0C60), 0);
    charge(3); CALL(L_5A0A, 0x02FB10);
    set_a(1, 0x2FB9A);
    charge(3);
    if (d_reg(7) & (1u << (d_reg(2) & 31))) { charge(1); set_a(1, 0x2FBA1); }
    vwr16(A6W(0x0C60), 0);
    charge(2); CALL(L_5A0A, 0x02FB28);
    charge(1);
    uint16_t c = (uint16_t)(d_reg(2) - 1);
    set_d16(2, c);
    if (c != 0xFFFF) return RD_JMP(0x02FAA4);
    uint16_t d7 = (uint16_t)(d_reg(7) * 2);
    set_d16(7, d7);
    vwr16(A6W(0x50C4), d7);
    charge(3);
    return RD_RTS;
}

/* ======================= camera matrix / course objects (0x2FF64..0x318BE) ======================= */

/* divs.w as the lifted code does it: on overflow the register is unchanged */
static uint32_t divs_w(uint32_t dividend, uint32_t divisor)
{
    int32_t q = (int32_t)SDIVREM((int32_t)dividend, sxw(divisor), '/');
    int32_t rm = (int32_t)SDIVREM((int32_t)dividend, sxw(divisor), '%');
    if ((uint32_t)(q + 0x8000) > 0xFFFFu) return dividend;
    return ((uint32_t)rm << 16) | ((uint32_t)q & 0xFFFF);
}
/* the fixed-point multiply the matrix code uses: muls.w, add.l, swap */
static inline uint32_t fxmul(uint32_t a, uint32_t b) { uint32_t m = muls_w(a, b); m += m; return (m << 16) | (m >> 16); }

/* FUN_0002ff64: 0x301A = -(cos(D0/2) * D1/2) / -sin(D0/2), 0 when the sine
 * is 0 (a tangent-scaled distance: the projection scale for a view angle) */
static uint32_t rd_view_scale(void)
{
    uint32_t d1 = setw(d_reg(1), (uint16_t)((int16_t)d_reg(1) >> 1));
    uint32_t d0 = setw(d_reg(0), (uint16_t)(((int16_t)d_reg(0) >> 1) & 0xFFFE));
    uint32_t a5 = a_reg(5);
    uint32_t d2 = setw(d_reg(2), (uint16_t)-trig(a5, d0));
    d0 = setw(d0, (uint16_t)(d0 + 0x4000));
    uint32_t d3 = setw(d_reg(3), trig(a5, d0));
    d3 = muls_w(d3, d1);
    charge(10);
    if ((uint16_t)d2) {
        d3 = divs_w(d3, d2);
        d3 = setw(d3, (uint16_t)-d3);
        charge(3);
    } else { d3 = setw(d3, 0); charge(2); }
    vwr16(A6W(0x301A), (uint16_t)d3);
    set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3);
    return RD_RTS;
}

/* FUN_0002ff92: build the 3x3 rotation matrix 0x3040..0x3050 from the angles
 * 0x301C (x), 0x301E (y), 0x3020 (z): Q15 products of their sines/cosines */
static uint32_t rd_rotation_matrix(void)
{
    uint32_t a5 = a_reg(5), d[8];
    for (int k = 0; k < 8; k++) d[k] = d_reg(k);
    static const uint16_t ang[3] = { 0x301C, 0x301E, 0x3020 };
    for (int k = 0; k < 3; k++) {                          /* D2/D3, D4/D5, D6/D7 = -sin, cos */
        uint16_t a = (uint16_t)(vrd16(A6W(ang[k])) & 0xFFFE);
        d[2 + 2 * k] = setw(d[2 + 2 * k], trig(a5, a));
        d[0] = setw(d[0], (uint16_t)(a + 0x4000));
        d[3 + 2 * k] = setw(d[3 + 2 * k], trig(a5, (uint16_t)(a + 0x4000)));
        d[2 + 2 * k] = setw(d[2 + 2 * k], (uint16_t)-d[2 + 2 * k]);
    }
    const uint32_t one = 0x7FFF;
    uint32_t t0, t1;
    t0 = fxmul(fxmul(one, d[5]), d[7]); t1 = fxmul(fxmul(d[2], d[4]), d[6]);
    vwr16(A6W(0x3040), (uint16_t)(t0 - t1));
    t0 = fxmul(fxmul(one, d[3]), d[6]);
    vwr16(A6W(0x3042), (uint16_t)-t0);
    t0 = fxmul(fxmul(one, d[4]), d[7]); t1 = fxmul(fxmul(d[2], d[5]), d[6]);
    vwr16(A6W(0x3044), (uint16_t)(t0 + t1));
    t0 = fxmul(fxmul(one, d[5]), d[6]); t1 = fxmul(fxmul(d[2], d[4]), d[7]);
    vwr16(A6W(0x3046), (uint16_t)(t0 + t1));
    t0 = fxmul(fxmul(one, d[3]), d[7]);
    vwr16(A6W(0x3048), (uint16_t)t0);
    t0 = fxmul(fxmul(one, d[4]), d[6]); t1 = fxmul(fxmul(d[2], d[5]), d[7]);
    vwr16(A6W(0x304A), (uint16_t)(t0 - t1));
    t0 = fxmul(fxmul(one, d[3]), d[4]);
    vwr16(A6W(0x304C), (uint16_t)-t0);
    t0 = fxmul(fxmul(one, one), d[2]);
    vwr16(A6W(0x304E), (uint16_t)t0);
    t0 = fxmul(fxmul(one, d[3]), d[5]);
    vwr16(A6W(0x3050), (uint16_t)t0);
    d[0] = t0;
    d[1] = (t1 & 0xFFFF0000u) | 0x7FFF;
    for (int k = 0; k < 8; k++) set_d(k, d[k]);
    set_a(4, 0x7FFF);
    return RD_RTS;
}

/* FUN_000300c4: rotate (D1w, D2w, D3w) by the matrix 0x3040 and add the
 * translation 0x303A..0x303E: result in D4w, D5w, D6w */
static uint32_t rd_rotate_point(void)
{
    uint32_t d1 = d_reg(1), d2 = d_reg(2), d3 = d_reg(3), d0 = 0, out[3];
    for (int row = 0; row < 3; row++) {
        uint32_t m = A6W(0x3040) + (uint32_t)row * 6u;
        uint32_t v = fxmul(vrd16(m), d1);
        d0 = fxmul(vrd16(m + 2), d2);
        v = setw(v, v + d0);
        d0 = fxmul(vrd16(m + 4), d3);
        v = setw(v, v + d0);
        v = setw(v, v + vrd16(A6W(0x303A) + (uint32_t)row * 2u));
        out[row] = v;
    }
    set_d(4, out[0]); set_d(5, out[1]); set_d(6, out[2]); set_d(0, d0);
    return RD_RTS;
}

/* FUN_0003013e: a 4-vertex 0x8004 billboard quad: rotate the four corners of
 * the record at A0 (at +0x0C/+0x18/+0x24/+0x30) with FUN_000300c4 into
 * 0x3022..0x3038; if all are in front (z > 0) emit the quad (model and
 * flags words from the record, depth = the mean z, each corner projected by
 * the scale 0x301A / z) and step A0 past the record (0x38 bytes); a corner
 * behind the camera skips it (lea 0x38(A0), rts at 0x30138) */
static uint32_t rd_billboard_quad(void)
{
    uint32_t a0 = a_reg(0);
    static const uint8_t ofs[4] = { 0x0C, 0x18, 0x24, 0x30 };
    static const uint16_t dst[4] = { 0x3022, 0x3028, 0x302E, 0x3034 };
    for (int k = 0; k < 4; k++) {
        set_d16(1, vrd16(a0 + ofs[k])); set_d16(2, vrd16(a0 + ofs[k] + 2)); set_d16(3, vrd16(a0 + ofs[k] + 4));
        charge(k ? 7 : 4);
        CALL(L_300C4, 0x03014E + 0x1C * (uint32_t)k);
        vwr16(A6W(dst[k]), (uint16_t)d_reg(4));
        vwr16(A6W(dst[k] + 2), (uint16_t)d_reg(5));
        vwr16(A6W(dst[k] + 4), (uint16_t)d_reg(6));
    }
    charge(3);
    regs_t r; regs_load(&r);
    r.a[1] = vrd32(G_DL_CURSOR);
    r.d[7] = setw(r.d[7], vrd16(A6W(0x3026)));
    charge(3);
    int behind = (int16_t)r.d[7] <= 0;
    for (int k = 1; k < 4 && !behind; k++) {
        uint16_t z = (uint16_t)vrd16(A6W(dst[k] + 4));
        r.d[0] = setw(r.d[0], z);
        charge(2);
        if ((int16_t)z <= 0) { behind = 1; break; }
        r.d[7] = setw(r.d[7], r.d[7] + z);
        charge(1);
    }
    if (behind) {
        poll();
        r.a[0] += 0x38;
        charge(2);
        regs_store(&r);
        return RD_RTS;
    }
    r.d[7] = (uint32_t)((int32_t)sxw(r.d[7]) >> 2);
    vwr32(r.a[1], 0x8004); r.a[1] += 4;
    r.d[0] = (uint32_t)sxw(vrd16(r.a[0])) + r.d[7]; r.a[0] += 2;
    vwr32(r.a[1], r.d[0]); r.a[1] += 4;
    r.d[0] = (uint16_t)vrd16(r.a[0]); r.a[0] += 2; vwr32(r.a[1], r.d[0]); r.a[1] += 4;
    vwr32(r.a[1], r.d[7]); r.a[1] += 4;
    r.d[0] = (uint16_t)vrd16(r.a[0]); r.a[0] += 2; vwr32(r.a[1], r.d[0]); r.a[1] += 4;
    r.d[0] = (uint16_t)vrd16(r.a[0]); r.a[0] += 2; vwr32(r.a[1], r.d[0]); r.a[1] += 4;
    r.d[5] = setw(r.d[5], vrd16(A6W(0x301A)));
    r.d[0] = (uint16_t)vrd16(r.a[0]); r.a[0] += 2; vwr32(r.a[1], r.d[0]); r.a[1] += 4;
    r.d[0] = (uint16_t)vrd16(r.a[0]); r.a[0] += 2; vwr32(r.a[1], r.d[0]); r.a[1] += 4;
    r.a[0] += 6;
    charge(2 + 23);
    for (int k = 0; k < 4; k++) {
        r.d[1] = setw(r.d[1], vrd16(A6W(dst[k])));
        r.d[2] = setw(r.d[2], vrd16(A6W(dst[k] + 2)));
        r.d[4] = setw(r.d[4], vrd16(A6W(dst[k] + 4)));
        r.d[0] = setw(r.d[0], (uint16_t)r.d[4]);
        r.d[1] = divs_w(muls_w(r.d[1], r.d[5]), r.d[4]);
        r.d[1] = (uint32_t)sxw(r.d[1]);
        vwr32(r.a[1], r.d[1]); r.a[1] += 4;
        r.d[2] = divs_w(muls_w(r.d[2], r.d[5]), r.d[4]);
        r.d[2] = (uint32_t)sxw(r.d[2]);
        vwr32(r.a[1], r.d[2]); r.a[1] += 4;
        r.d[0] = (uint32_t)sxw(r.d[0]);
        vwr32(r.a[1], r.d[0]); r.a[1] += 4;
        charge(14);
        int n = k < 3 ? 3 : 1;
        for (int j = 0; j < n; j++) {
            r.d[0] = (uint16_t)vrd16(r.a[0]); r.a[0] += 2;
            vwr32(r.a[1], r.d[0]); r.a[1] += 4;
            charge(3);
        }
        if (k < 3) { r.a[0] += 6; charge(1); }
    }
    vwr32(r.a[1], 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, r.a[1]);
    charge(3);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_00030d24 / FUN_00030d80: start course-object animation n (0 / 3): clear
 * the object's offsets and counters, place it at (0, 0, -6000), take the
 * object count and length from its script (0x3131E [n]) into 0x10008009 and
 * 0x305E, then FUN_00030bc2 */
static uint32_t course_anim_start(uint16_t n, uint32_t ret)
{
    vwr32(A6W(0x3062), 0); vwr32(A6W(0x3066), 0); vwr32(A6W(0x306A), 0);
    vwr16(A6W(0x308E), 0); vwr16(A6W(0x308C), 0); vwr16(A6W(0x305C), 0);
    uint32_t a0 = A6W(0x307A);
    vwr32(a0, 0); vwr32(a0 + 4, 0); vwr32(a0 + 8, (uint32_t)-0x1770);
    vwr16(a0 + 12, 0); vwr16(a0 + 14, 0); vwr16(a0 + 16, 0);
    set_d16(0, n);
    vwr16(A6W(0x3060), n);
    a0 = vrd32(0x3131Eu + (uint32_t)n * 4u);
    vwr16(A6W(0x0009), vrd16(a0));
    vwr16(A6W(0x305E), vrd16(a0 + 2));
    set_a(0, a0 + 4);
    charge(19);
    CALL(L_30BC2, ret);
    return RD_RTS;
}
static uint32_t rd_course_anim0(void) { return course_anim_start(0, 0x030D76); }
static uint32_t rd_course_anim3(void) { return course_anim_start(3, 0x030DD2); }

/* the two cosine interpolators: frac = D4/D3 as an angle 0..0x4000 (+0x4000
 * for FUN_000312de), c = cos(frac); result = D2 + (D2-D1)*c (312a4) or
 * D1 + (D2-D1)*c (312de), with D1/D2 doubled first */
static uint32_t cos_interp(int from_start)
{
    int32_t d1 = sxw(d_reg(1)) << 1, d2 = sxw(d_reg(2)) << 1;
    int32_t d3 = sxw(d_reg(3)), d4 = sxw(d_reg(4));
    uint32_t d0 = (uint32_t)(0x4000 * d4);
    d0 = (uint32_t)SDIVREM((int32_t)d0, d3, '/');
    if (from_start) d0 += 0x4000;
    d0 = setw(d0, (uint16_t)((d0 & 0xFFFE) + 0x4000));
    uint32_t d5 = (uint32_t)sxw(trig(a_reg(5), d0));
    uint32_t p = (uint32_t)(d2 - d1) * d5;
    p <<= 1;
    p = (p << 16) | (p >> 16);
    d0 = (uint32_t)sxw(p) + (uint32_t)(from_start ? d1 : d2);
    set_d(0, d0); set_d(1, (uint32_t)d1); set_d(2, (uint32_t)d2);
    set_d(3, (uint32_t)d3); set_d(4, (uint32_t)d4); set_d(5, d5);
    return RD_RTS;
}
/* FUN_000312a4 */
static uint32_t rd_cos_interp_end(void) { return cos_interp(0); }
/* FUN_000312de */
static uint32_t rd_cos_interp_start(void) { return cos_interp(1); }

/* sample channel `ch` of the course object at A3 (FUN_000311c6) with A0 = A3 */
#define SAMPLE(ch, ret) do { r.a[0] = r.a[3]; r.d[0] = setw(r.d[0], ch); charge(3); regs_store(&r); \
                             CALL(L_311C6, ret); regs_load(&r); } while (0)

/* the angle in D0w as a (-sin, cos) pair to the list (andi, move, addi, move,
 * neg, move.l, move.l) */
static void put_angle_pair(regs_t *r, uint32_t *a4)
{
    uint16_t a = (uint16_t)(r->d[0] & 0xFFFE);
    r->d[1] = setw(r->d[1], (uint16_t)-trig(r->a[5], a));
    a = (uint16_t)(a + 0x4000);
    r->d[0] = setw(r->d[0], a);
    r->d[2] = setw(r->d[2], trig(r->a[5], a));
    vwr32(*a4, r->d[1]); vwr32(*a4 + 4, r->d[2]); *a4 += 8;
    charge(7);
}

/* FUN_00030e8c: draw the animated course object 0x3060 (script 0x3131E [n]:
 * part count and length into 0x10008009 / 0x305E, then 0x1E-byte part
 * records after a 0x24-byte header) at frame 0x305C (wrapping at the length):
 * an identity root transform, then per part a transform slot (its parent +
 * the base slot 0x3056) with the three rotation and three position channels
 * sampled from its keys (FUN_000311c6), the parent link (0x8009), and the
 * model (0x800A: its code + a key-driven or blinking (0x31010 [0x308C]) frame
 * offset) at the object's position (0x307A, negated); parts with flag bit 0
 * add a second model on their own slot (0x8030) at 0x3062 offset, its code
 * from FUN_000310d0. Model 0x85E is a marker and draws nothing. */
static uint32_t rd_course_anim_draw(void)
{
    charge(1); CALL(L_30BE6, 0x030E90);
    regs_t r; regs_load(&r);
    r.a[4] = vrd32(G_DL_CURSOR);
    vwr8(A6W(0x3056), 0);
    vwr32(r.a[4], 0x8002); r.a[4] += 4;
    r.d[0] = setw(r.d[0], vrd16(G_DL_PRIO));
    vwr32(r.a[4], r.d[0]); r.a[4] += 4;
    vwr32(r.a[4], 0x8008); vwr32(r.a[4] + 4, 0); vwr32(r.a[4] + 8, 1); r.a[4] += 12;
    r.d[0] = setw(r.d[0], 0);
    charge(8 + 1);
    {
        uint16_t a = (uint16_t)(r.d[0] & 0xFFFE);
        r.d[1] = setw(r.d[1], (uint16_t)-trig(r.a[5], a));
        a = (uint16_t)(a + 0x4000);
        r.d[0] = setw(r.d[0], a);
        r.d[2] = setw(r.d[2], trig(r.a[5], a));
        for (int k = 0; k < 3; k++) { vwr32(r.a[4], r.d[1]); vwr32(r.a[4] + 4, r.d[2]); r.a[4] += 8; }
        vwr32(r.a[4], 1); vwr32(r.a[4] + 4, 0xFFFFFFFFu); r.a[4] += 8;
        charge(5 + 8);
    }
    r.d[0] = setw(r.d[0], vrd16(A6W(0x3060)));
    r.a[3] = vrd32(0x3131Eu + (uint32_t)sxw(r.d[0]) * 4u);
    r.d[7] = setw(r.d[7], vrd16(r.a[3])); r.a[3] += 2;
    vwr16(A6W(0x0009), (uint16_t)r.d[7]);
    r.d[0] = setw(r.d[0], vrd16(r.a[3])); r.a[3] += 2;
    vwr16(A6W(0x305E), (uint16_t)r.d[0]);
    r.a[2] = r.a[3];
    r.a[3] += 0x24;
    r.d[6] = setb(r.d[6], vrd8(A6W(0x3056)));
    vwr8(A6W(0x3056), (uint8_t)(vrd8(A6W(0x3056)) + r.d[7]));
    r.d[7] = setw(r.d[7], r.d[7] - 1);
    charge(14);
    if ((int16_t)r.d[0] <= (int16_t)vrd16(A6W(0x305C))) { charge(1); vwr16(A6W(0x305C), 0); }
    for (;;) {
        vwr32(r.a[4], 0x8008); r.a[4] += 4;
        r.d[0] = 0;
        r.d[0] = setb(r.d[0], (uint8_t)(vrd8(r.a[3] + 2) + r.d[6]));
        vwr32(r.a[4], r.d[0]); vwr32(r.a[4] + 4, 1); r.a[4] += 8;
        charge(6);
        SAMPLE(0x12, 0x030F32); put_angle_pair(&r, &r.a[4]);
        SAMPLE(0x16, 0x030F52); put_angle_pair(&r, &r.a[4]);
        SAMPLE(0x1A, 0x030F72); put_angle_pair(&r, &r.a[4]);
        vwr32(r.a[4], 1); vwr32(r.a[4] + 4, 0); r.a[4] += 8;
        charge(2);
        SAMPLE(0x06, 0x030F9E);
        r.d[0] = (uint32_t)((int32_t)r.d[0] >> 3); vwr32(r.a[4], r.d[0]); r.a[4] += 4;
        charge(2);
        SAMPLE(0x0A, 0x030FAC);
        r.d[0] = (uint32_t)((int32_t)r.d[0] >> 3); vwr32(r.a[4], r.d[0]); r.a[4] += 4;
        charge(2);
        SAMPLE(0x0E, 0x030FBA);
        r.d[0] = 0u - (uint32_t)((int32_t)r.d[0] >> 3); vwr32(r.a[4], r.d[0]); r.a[4] += 4;
        charge(3);
        vwr32(r.a[4], 0xFFFFFFFFu); vwr32(r.a[4] + 4, 0x8009); r.a[4] += 8;
        r.d[0] = 0; r.d[1] = 0;
        r.d[1] = setb(r.d[1], (uint8_t)(vrd8(r.a[3] + 2) + r.d[6]));
        vwr32(r.a[4], r.d[1]); r.a[4] += 4;
        r.d[0] = setb(r.d[0], vrd8(r.a[3] + 3));
        charge(9);
        if ((uint8_t)r.d[0]) { charge(1); r.d[0] = setb(r.d[0], (uint8_t)(r.d[0] + r.d[6])); }
        vwr32(r.a[4], r.d[0]); vwr32(r.a[4] + 4, r.d[1]); r.a[4] += 8;
        r.d[2] = setw(r.d[2], vrd16(r.a[3]));
        charge(5);
        if ((uint16_t)r.d[2] != 0x85E) {
            r.d[0] = setw(r.d[0], 0);
            r.d[0] = setb(r.d[0], vrd8(r.a[3] + 5));
            charge(3);
            if ((uint8_t)r.d[0]) {
                r.a[0] = r.a[2];
                r.d[7] = (r.d[7] << 16) | (r.d[7] >> 16);
                r.d[7] = setw(r.d[7], r.d[1]);
                r.d[6] = (r.d[6] << 16) | (r.d[6] >> 16);
                r.d[6] = setw(r.d[6], r.d[2]);
                charge(6);
                regs_store(&r);
                CALL(L_31168, 0x031004);
                regs_load(&r);
                r.d[2] = setw(r.d[2], r.d[6]);
                r.d[6] = (r.d[6] << 16) | (r.d[6] >> 16);
                r.d[1] = setw(r.d[1], r.d[7]);
                r.d[7] = (r.d[7] << 16) | (r.d[7] >> 16);
                r.d[2] = setw(r.d[2], r.d[2] + r.d[0]);
                charge(6);
            } else {
                r.d[0] = setb(r.d[0], (uint8_t)(vrd8(r.a[3] + 4) & 2));
                charge(3);
                if ((uint8_t)r.d[0]) {
                    r.d[0] = setw(r.d[0], (uint16_t)(vrd16(A6W(0x308C)) & 0xF));
                    r.d[2] = setw(r.d[2], r.d[2] + vrd16(0x31010u + (uint32_t)sxw(r.d[0]) * 2u));
                    charge(3);
                }
            }
            vwr32(r.a[4], 0x800A); vwr32(r.a[4] + 4, r.d[2]); vwr32(r.a[4] + 8, r.d[1]); r.a[4] += 12;
            r.a[0] = A6W(0x307A);
            for (int k = 0; k < 3; k++) { r.d[0] = 0u - vrd32(r.a[0]); r.a[0] += 4; vwr32(r.a[4], r.d[0]); r.a[4] += 4; }
            r.d[0] = setb(r.d[0], (uint8_t)(vrd8(r.a[3] + 4) & 1));
            charge(3 + 10 + 3);
            if ((uint8_t)r.d[0]) {
                vwr32(r.a[4], 0x8030); vwr32(r.a[4] + 4, r.d[1]); r.a[4] += 8;
                vwr8(A6W(0x3056), (uint8_t)(vrd8(A6W(0x3056)) + 1));
                r.d[1] = setb(r.d[1], vrd8(A6W(0x3056)));
                vwr32(r.a[4], r.d[1]); vwr32(r.a[4] + 4, 0); vwr32(r.a[4] + 8, 0x800A); r.a[4] += 12;
                charge(8);
                regs_store(&r);
                CALL(L_310D0, 0x031090);
                regs_load(&r);
                vwr32(r.a[4], r.d[2]); vwr32(r.a[4] + 4, r.d[1]); r.a[4] += 8;
                static const uint16_t pos[3] = { 0x307A, 0x307E, 0x3082 }, ofs[3] = { 0x3062, 0x3066, 0x306A };
                for (int k = 0; k < 3; k++) {
                    r.d[0] = (0u - vrd32(A6W(pos[k]))) + vrd32(A6W(ofs[k]));
                    vwr32(r.a[4], r.d[0]); r.a[4] += 4;
                }
                charge(2 + 12);
            }
        }
        r.a[3] += 0x1E;
        charge(2);
        uint16_t c = (uint16_t)(r.d[7] - 1); r.d[7] = setw(r.d[7], c);
        if (c == 0xFFFF) break;
        regs_store(&r); poll();
    }
    vwr16(A6W(0x305C), (uint16_t)(vrd16(A6W(0x305C)) + 1));
    vwr32(r.a[4], 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, r.a[4]);
        charge(4);
    regs_store(&r);
    return RD_RTS;
}

/* FUN_000316a2: the course's direction arrows: a 16-frame animation
 * (0x3090; frames 0,4,8,12 pick the arrow set 0x3181E), the variant's arrow
 * model (0x317DE) and the positions table 0x3189E. Linked: the two arrows of
 * FUN_00031724 (on the other table) and FUN_00031742; single: FUN_00031724
 * for our own slot and FUN_0003175e for each other car on our segment.
 * The list ends with -1. */
static uint32_t rd_course_arrows(void)
{
    uint32_t r0;
    uint16_t d3 = (uint16_t)((vrd16(A6W(0x3090)) + 1) & 0xF);
    vwr16(A6W(0x3090), d3);
    d3 = (uint16_t)((d3 & 0xC) << 2);
    set_a(0, 0x3181Eu + (uint32_t)sxw(d3) * 2u);
    set_a(1, vrd32(G_DL_CURSOR));
    d3 = (uint16_t)(vrd16(G_COURSE_VAR) & 3);
    set_d16(3, d3);
    set_d16(2, vrd16(0x317DEu + (uint32_t)d3 * 2u));
    set_a(3, 0x3189E);
    charge(7 + 8);
#define EXG_A0_A3() do { uint32_t t_ = a_reg(0); set_a(0, a_reg(3)); set_a(3, t_); } while (0)
    if (vrd16(A6W(0x2344))) {
        set_d16(7, 0);
        EXG_A0_A3();
        charge(3);
        if ((r0 = rd_call(L_31724, 0x0316E0))) return r0;
        EXG_A0_A3();
        set_d16(7, 1);
        charge(3);
        if ((r0 = rd_call(L_31742, 0x0316E8))) return r0;
        charge(1);
    } else {
        set_d16(7, 7);
        set_d16(6, vrd16(G_LINK_ME));
        set_d16(4, vrd16(A6W(0x11A2)));
        charge(3);
        for (;;) {
            charge(2);
            if ((uint16_t)d_reg(6) == (uint16_t)d_reg(7)) {
                EXG_A0_A3();
                charge(2);
                if ((r0 = rd_call(L_31724, 0x031700))) return r0;
                EXG_A0_A3();
                charge(2);
            } else {
                uint16_t o = (uint16_t)(d_reg(7) << 4);
                set_d16(3, o);
                charge(4);
                if ((uint16_t)d_reg(4) == (uint16_t)vrd16(A6W(0x11AE) + (uint32_t)sxw(o))) {
                    charge(1);
                    if ((r0 = rd_call(L_3175E, 0x031714))) return r0;
                }
            }
            charge(1);
            uint16_t c = (uint16_t)(d_reg(7) - 1); set_d16(7, c);
            if (c == 0xFFFF) break;
            poll();
        }
    }
#undef EXG_A0_A3
    vwr32(a_reg(1), 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, a_reg(1));
    charge(3);
    return RD_RTS;
}

static const rd_entry rd_table_b4[] = {
    /*  ep        fn                         kill    scratch cost  name */
//X    { 0x020AA4, rd_trackside_objects,      0x0FFF, 0,      1, "FUN_00020aa4", 1 },
//X    { 0x020C36, rd_digit_boards,           0x0BFF, 0,      1, "FUN_00020c36" },
//X    { 0x020E36, rd_track_banners,          0x1FFF, 0,      1, "FUN_00020e36", 1 },
//X    { 0x0211E0, rd_car_carrier,            0x0BFF, 0,      0, "FUN_000211e0" },
//X    { 0x02131A, rd_course0_overlay,        0x0BFF, 0,      1, "FUN_0002131a" },
//X    { 0x02132A, rd_course4_overlay,        0x3FFF, 0,      0, "FUN_0002132a" },
//X    { 0x02133E, rd_course4_8_overlay,      0x0BFF, 0,      0, "FUN_0002133e" },
//X    { 0x021360, rd_course5_6_overlay,      0x0103, 0,      0, "FUN_00021360" },
//X    { 0x021494, rd_raise_4346,             0x0003, 0,      0, "FUN_00021494" },
//X    { 0x0214C8, rd_course0_boards,         0x0BFF, 0,      0, "FUN_000214c8" },
//X    { 0x021702, rd_overlay_object,         0x08FF, 0,      0, "FUN_00021702" },
//X    { 0x0220AA, rd_engine_sound,           0x030F, 0,      0, "FUN_000220aa" },
//X    { 0x0222E0, rd_engine_sound_send,      0x08FF, 0,      1, "FUN_000222e0" },
//X    { 0x0227D2, rd_window_full,            0x03FF, 0,      1, "FUN_000227d2" },
//X    { 0x022802, rd_window_ease,            0x07FF, 0,      1, "FUN_00022802" },
//X    { 0x02287C, rd_preview_start,          0x3FFF, 0,      0, "FUN_0002287c" },
//X    { 0x022A2C, rd_preview_setup,          0x3FFF, 0,      0, "FUN_00022a2c" },
//X    { 0x022A98, rd_preview_camera,         0x1FF7, 0,      1, "FUN_00022a98" },
//X    { 0x022AC6, rd_preview_frame,          0x1FFF, 0,      0, "FUN_00022ac6" },
//X    { 0x022C3A, rd_preview_page,           0x0FFF, 0,      0, "FUN_00022c3a" },
//X    { 0x022C9E, rd_preview_page_work,      0x1FFF, 0,      0, "FUN_00022c9e" },
//X    { 0x024818, rd_preview_aim,            0x1FFF, 0,      1, "FUN_00024818" },
//X    { 0x0248B2, rd_preview_orbit,          0x000F, 0,      1, "FUN_000248b2" },
//X    { 0x024912, rd_preview_path,           0x1FFF, 0,      0, "FUN_00024912", 1 },
//X    { 0x024D68, rd_path_markers,           0x038F, 0,      1, "FUN_00024d68" },
//X    { 0x025BC4, rd_intro_start,            0x07FF, 0,      1, "FUN_00025bc4" },
//X    { 0x025BF2, rd_intro_frame,            0x0FFF, 0,      0, "FUN_00025bf2" },
//X    { 0x025E16, rd_intro_mixer,            0x03FF, 0,      0, "FUN_00025e16" },
//X    { 0x025F50, rd_intro_state,            0x3FFF, 0,      0, "FUN_00025f50" },
//X    { 0x026B20, rd_link_reset,             0x37C1, 0,      0, "FUN_00026b20" },
//X    { 0x026E24, rd_link_number,            0x034F, 0,      0, "FUN_00026e24" },
//X    { 0x026EAE, rd_link_frame,             0x0FFF, 0,      0, "FUN_00026eae" },
//X    { 0x026F86, rd_link_send,              0x0FFF, 0,      0, "FUN_00026f86" },
//X    { 0x027008, rd_link_merge,             0x03FF, 0,      0, "FUN_00027008" },
//X    { 0x027126, rd_link_receive,           0x1FFF, 0,      0, "FUN_00027126" },
//X    { 0x02721A, rd_link_cars,              0x08FF, 0,      0, "FUN_0002721a" },
//X    { 0x0272D4, rd_link_car_flags,         0x08FF, 0,      0, "FUN_000272d4" },
//X    { 0x027334, rd_link_store_me,          0x021F, 0,      0, "FUN_00027334" },
//X    { 0x0273EE, rd_link_join,              0x0FFF, 0,      0, "FUN_000273ee", 1 },
//X    { 0x027680, rd_intro_reset,            0x0000, 0,      3, "FUN_00027680" },
//X    { 0x028632, rd_snd_silence,            0x0180, 0,      0, "FUN_00028632" },
//X    { 0x02863E, rd_snd_cmd_clear,          0x0000, 0,      2, "FUN_0002863e" },
//X    { 0x02864E, rd_snd_block_reset,        0x0180, 0,      0, "FUN_0002864e" },
//X    { 0x028680, rd_snd_levels_ff,          0x0000, 0,      15, "FUN_00028680" },
//X    { 0x02872E, rd_snd_cmd_405e,           0x0000, 0,      0, "FUN_0002872e" },
//X    { 0x0287A6, rd_snd_race_reset,         0x1F87, 0,      0, "FUN_000287a6" },
//X    { 0x02881C, rd_snd_cue_clear,          0x0381, 0,      0, "FUN_0002881c" },
//X    { 0x0289CA, rd_snd_countdown,          0x0003, 0,      0, "FUN_000289ca" },
//X    { 0x028A56, rd_snd_lap_cue,            0x0003, 0,      0, "FUN_00028a56" },
//X    { 0x028A8C, rd_snd_405c,               0x0000, 0,      0, "FUN_00028a8c" },
    { 0x028B12, rd_snd_405f,               0x0000, 0,      0, "FUN_00028b12" },
    { 0x028B98, rd_snd_preview_cue,        0x0001, 0,      0, "FUN_00028b98" },
    { 0x028BBE, rd_snd_preview_reset,      0x0000, 0,      3, "FUN_00028bbe" },
    { 0x028BC8, rd_snd_preview,            0x0001, 0,      0, "FUN_00028bc8" },
    { 0x028C32, rd_snd_race_frame,         0x1F87, 0,      0, "FUN_00028c32" },
    { 0x028C9C, rd_snd_cue_update,         0x1F87, 0,      0, "FUN_00028c9c" },
    { 0x028CB8, rd_cue_good_time,          0x0307, 0,      0, "FUN_00028cb8" },
    { 0x028D48, rd_cue_new_lap,            0x0101, 0,      0, "FUN_00028d48" },
    { 0x028DD4, rd_cue_corners,            0x1F87, 0,      0, "FUN_00028dd4" },
    { 0x028E94, rd_cue_speed_traps,        0x1F87, 0,      0, "FUN_00028e94" },
    { 0x02914C, rd_cue_rivals,             0x03FF, 0,      0, "FUN_0002914c" },
    { 0x029158, rd_cue_overtaken,          0x03FF, 0,      0, "FUN_00029158" },
    { 0x0291FE, rd_cue_rival_voices,       0x03FF, 0,      0, "FUN_000291fe" },
    { 0x0292DA, rd_rival_start_test,       0x0030, 0,      0, "FUN_000292da" },
    { 0x02938E, rd_rival_mid_test,         0x003F, 0,      0, "FUN_0002938e" },
    { 0x0293D2, rd_cue_rival_late,         0x03FF, 0,      0, "default @293D2" },
    { 0x02944A, rd_rival_late_test,        0x003F, 0,      0, "FUN_0002944a" },
    { 0x02948E, rd_cue_final_lap,          0x0000, 0,      0, "FUN_0002948e" },
    { 0x0294C8, rd_cue_serve,              0x0103, 0,      0, "FUN_000294c8" },
    { 0x0294E4, rd_cue_bit7,               0x0103, 0,      0, "FUN_000294e4" },
    { 0x0294FC, rd_cue_laps_left,          0x0103, 0,      0, "FUN_000294fc" },
    { 0x029550, rd_cue_bit0,               0x0103, 0,      0, "FUN_00029550" },
    { 0x029568, rd_cue_bit1,               0x0103, 0,      0, "FUN_00029568" },
    { 0x029580, rd_cue_corner_voice,       0x0103, 0,      0, "FUN_00029580" },
    { 0x029770, rd_cue_speed_voice,        0x0103, 0,      0, "FUN_00029770" },
    { 0x02978A, rd_cue_bit6,               0x0103, 0,      0, "FUN_0002978a" },
    { 0x029854, rd_voice_window,           0x0003, 0,      0, "FUN_00029854" },
    { 0x02AD26, rd_ee_read_header,         0x034F, 0,      1, "FUN_0002ad26" },
    { 0x02AE0A, rd_ee_check_slots,         0x03C7, 0,      0, "FUN_0002ae0a" },
    { 0x02AE7E, rd_ee_read_group,          0x07FF, 0,      1, "FUN_0002ae7e" },
    { 0x02B172, rd_ee_blank_record,        0x0180, 0,      1, "FUN_0002b172" },
    { 0x02B29E, rd_ee_messages,            0x0FDF, 0,      0, "FUN_0002b29e" },
    { 0x02B998, rd_ee_show_settings,       0x0FDF, 0,      1, "FUN_0002b998" },
    { 0x02B9D4, rd_ee_build_record,        0x03C7, 0,      0, "FUN_0002b9d4" },
    { 0x02BA34, rd_ee_unpack_times,        0x03FF, 0,      0, "FUN_0002ba34" },
    { 0x02BBB6, rd_ee_unpack_4,            0x1F87, 0,      0, "FUN_0002bbb6" },
    { 0x02C1CC, rd_play_stats,             0x0103, 0,      0, "FUN_0002c1cc" },
    { 0x02C248, rd_ee_save_best,           0x1F87, 0,      0, "FUN_0002c248" },
    { 0x02D9C6, rd_ee_default_settings,    0x0201, 0,      0, "FUN_0002d9c6" },
    { 0x02FAF6, rd_ramtest_next,           0x0FDF, 0,      0, "FUN_0002faf6" },
    { 0x02FF64, rd_view_scale,             0x000F, 0,      1, "FUN_0002ff64" },
    { 0x02FF92, rd_rotation_matrix,        0x1BFF, 0,      127, "FUN_0002ff92" },
    { 0x0300C4, rd_rotate_point,           0x00F9, 0,      46, "FUN_000300c4" },
    { 0x03013E, rd_billboard_quad,         0x03FF, 0,      0, "FUN_0003013e" },
    { 0x030D24, rd_course_anim0,           0x0101, 0,      1, "FUN_00030d24" },
    { 0x030D80, rd_course_anim3,           0x0101, 0,      1, "FUN_00030d80" },
    { 0x030E8C, rd_course_anim_draw,       0x3FFF, 0,      0, "FUN_00030e8c", 1 },
    { 0x0312A4, rd_cos_interp_end,         0x003F, 0,      21, "FUN_000312a4" },
    { 0x0312DE, rd_cos_interp_start,       0x003F, 0,      22, "FUN_000312de" },
    { 0x0316A2, rd_course_arrows,          0x1BFF, 0,      0, "FUN_000316a2" },
};
RD_REGISTER(rd_table_b4)
