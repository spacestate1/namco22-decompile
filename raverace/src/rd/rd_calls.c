/*
 * rd_calls.c -- readable replacements for functions that CALL others
 * (include/rd.h: rd_call). Same rules as rd_funcs.c; in addition:
 *   - charge your own instructions up to and including each bsr/jsr, in order,
 *     before rd_call (the callee charges its own); `cost` in the table is what
 *     runs after the last poll point (usually the rts)
 *   - `if ((r = rd_call(L_X, <address after the bsr>))) return r;` at every call
 *   - a trailing bra into another function is a tail jump: return its address
 */
#include "rd.h"
#include "rr_lifted.h"

/* FUN_0001502c: two vertex records (FUN_00015048) -- index D1w, then D1w+1 */
static uint32_t rd_two_vertex_records(void)
{
    uint32_t r;
    charge(1);                                          /* bsr */
    if ((r = rd_call(L_15048, 0x015030))) return r;
    charge(2);                                          /* addq.w, bsr */
    set_d16(1, (uint16_t)(d_reg(1) + 1));
    if ((r = rd_call(L_15048, 0x015036))) return r;
    return RD_RTS;
}

/* FUN_0001a8d0: FUN_0001a8dc, FUN_0001a950, then continue in FUN_0001aa10 */
static uint32_t rd_1a8d0(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_1A8DC, 0x01A8D4))) return r;
    charge(1);
    if ((r = rd_call(L_1A950, 0x01A8D8))) return r;
    charge(1);                                          /* bra.w */
    return 0x01AA10;
}

/* FUN_00028c76: FUN_00028c82, FUN_00028c9c, then continue in FUN_0002914c */
static uint32_t rd_28c76(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_28C82, 0x028C7A))) return r;
    charge(1);
    if ((r = rd_call(L_28C9C, 0x028C7E))) return r;
    charge(1);
    return 0x02914C;
}

static const rd_entry rd_table_calls[] = {
    { 0x01502C, rd_two_vertex_records, 0x0803, 0, 1, "FUN_0001502c" },
    { 0x01A8D0, rd_1a8d0,              0x03FF, 0, 0, "FUN_0001a8d0" },
    { 0x028C76, rd_28c76,              0x1F87, 0, 0, "FUN_00028c76" },
};
RD_REGISTER(rd_table_calls)
