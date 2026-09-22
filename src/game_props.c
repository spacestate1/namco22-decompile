/*
 * game_props.c -- per-course PROP PLACEMENT.  GENERATED, do not hand-edit:
 *   python3 tools/m68k/props_to_c.py > src/game_props.c
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * terrain_props_dispatch @0x004F22 ends in a computed jump:
 *
 *     move.l  $00E00E0C,D0        ; the course
 *     subq.l  #3,D0
 *     bhi.w   <return>
 *     move.w  (0x0E,PC,D0.w*2),D0 ; table at 0x4F78 + (course-3)*2
 *     jmp     (0x02,PC,D0.w)      ; -> 0x4F72 + entry
 *
 * Ghidra gave up on it -- "WARNING: Could not recover jumptable at
 * 0x00004f6e. Too many branches" -- and having failed to resolve the
 * targets it never disassembled them either.  The four routines the table
 * points at (0x4F7A / 0x522E / 0x5424 / 0x55CC, 1708 bytes) are absent
 * from decompiled/propcycl_annotated.c entirely: there is nothing between
 * terrain_props_dispatch @0x4F22 and dsp_place_windmill_a @0x5626.  The
 * transpiled stub called terrain_lod_update_wrapper() in place of the
 * indirect jump, so NO COURSE HAS EVER PLACED A SINGLE PROP.
 *
 * The routines are pure inline DSP-list emitters -- no calls, no loops,
 * 17 forward branches -- so the generator translates them symbolically.
 * Register roles, from the shared prologue at 0x4F22:
 *
 *     A2 -> &W[0x09B4]  scratch holding the DSP write cursor
 *     A3 -> &W[0x0CDC]  camera pos: (a3)=X 4(a3)=Y 8(a3)=Z 0x18(a3)=W[0x0CF4]
 *     A4 -> &W[0x0C98]  frame counter
 *     D2  = 0x00E00CA4  the real DSP cursor, written back at the end
 *     D7  = 0x00035000  ROM base of the prop animation tables
 *
 * Course 3 uses the ordinary stack calling convention instead of inlining,
 * so it comes out as three dsp_cmd_* calls.
 */
#include "propcycl.h"
#include "rom_decode.h"
#include "vaddr.h"
#include "game_funcs.h"

extern intptr_t _W[];
#define W _W

/* Neither is prototyped in game_funcs.h; course 3 calls both. */
void dsp_cmd_place_object_abs(int32_t priority, int32_t model,
                              int32_t x, int32_t y, int32_t z);
void dsp_cmd_set_camera(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e,
                        int32_t f, int32_t g, int32_t h, int32_t i);

static void props_overflow(void)
{
    static int warned;
    if (warned++ == 0)
        fprintf(stderr, "[GUARD] terrain props: DSP cursor overran dspram -- "
                        "remaining props dropped\n");
}

/* Bounds-checked, for the same reason dsp_cmd_ptr() is (register row 19):
 * a producer that runs off the end of dspram turns the next store into a
 * segfault. Dropping the rest of one course's props is the right failure. */
static int props_room(const int32_t *cur)
{
    const uint8_t *p = (const uint8_t *)cur;
    return p >= g_sys.dspram && p + 4 <= g_sys.dspram + DSPRAM_SIZE;
}

/* The cursor lives in W[0x09B4], NOT in a local: the dsp_place_* helpers
 * that course 2 calls advance that same slot, so caching it here would
 * desync across every one of those calls. This is what the M68K does --
 * A2 points at W[0x09B4] and each emit is movea.l (a2),a0 / addq.l #4,(a2). */
#define EMIT(v) do { int32_t *p_ = (int32_t *)W[0x09B4]; \
                     if (!props_room(p_)) { props_overflow(); return; } \
                     *p_ = (int32_t)(v); W[0x09B4] = (intptr_t)(p_ + 1); } while (0)

/* ---- course 0 props: ROM 0x004F7A..0x00522E ---- */
static void terrain_props_course0(void)
{

    W[0x09B4] = W[0x0CA4];
    EMIT(32768);
    EMIT(0);
    EMIT(39);
    EMIT(0);
    EMIT(-90112);
    EMIT(0);
    EMIT(40);
    EMIT(0);
    EMIT(-90112);
    EMIT(0);
    EMIT(((326) + (vrd32s(0x35000 + (((((((uint32_t)(W[0x0C98]) >> 1)) & (15))) << 2))))));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
    if ((int32_t)(52) > (int32_t)(W[0x0CF4])) goto L_5130;
    EMIT(((((((uint32_t)(W[0x0C98]) >> 2)) & (15))) + 340));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
    EMIT(((((uint32_t)(((uint32_t)(W[0x0C98]) >> 2)) % (uint32_t)(6))) + 334));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
    EMIT(((((((uint32_t)(W[0x0C98]) >> 2)) & (3))) + 364));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
    EMIT(((((uint32_t)(((uint32_t)(W[0x0C98]) >> 1)) % (uint32_t)(6))) + 368));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
    EMIT(((356) + (vrd32s(0x35000 + (((((((uint32_t)(W[0x0C98]) >> 1)) & (15))) << 2))))));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
  L_5130:
    if ((int32_t)(66) >= (int32_t)(W[0x0CF4])) goto L_5178;
    if ((int32_t)(95) <= (int32_t)(W[0x0CF4])) goto L_5178;
    if ((int32_t)(((/*?$1c(a3)*/0) - (2))) <= (int32_t)(0)) goto L_5178;
    EMIT(377);
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
  L_5178:
    if ((int32_t)(vrd8(0xE012A1)) != (int32_t)(255)) goto L_51B2;
    EMIT(374);
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
  L_51B2:
    if ((int32_t)(vrd8(0xE01267)) != (int32_t)(255)) goto L_51EC;
    EMIT(375);
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
  L_51EC:
    if ((int32_t)(vrd8(0xE01266)) != (int32_t)(255)) goto L_5226;
    EMIT(376);
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
  L_5226:
    W[0x0CA4] = W[0x09B4];
    return;
}

/* ---- course 1 props: ROM 0x00522E..0x005424 ---- */
static void terrain_props_course1(void)
{

    W[0x09B4] = W[0x0CA4];
    EMIT(32768);
    EMIT(0);
    if ((int32_t)(32) <= (int32_t)(W[0x0CF4])) goto L_5286;
    EMIT(39);
    EMIT(0);
    EMIT(-90112);
    EMIT(0);
    EMIT(40);
    EMIT(0);
    EMIT(-90112);
    EMIT(0);
  L_5286:
    if ((int32_t)(68) > (int32_t)(W[0x0CF4])) goto L_52AC;
    EMIT(42);
    EMIT(0);
    EMIT(-90112);
    EMIT(0);
  L_52AC:
    if ((int32_t)(68) <= (int32_t)(W[0x0CF4])) goto L_5336;
    EMIT(((379) + (vrd32s(0x35000 + (((((((uint32_t)(W[0x0C98]) >> 1)) & (15))) << 2))))));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
    EMIT(((387) + (vrd32s(0x35000 + (((((((uint32_t)(W[0x0C98]) >> 1)) & (15))) << 2))))));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
  L_5336:
    EMIT(((395) + (vrd32s(0x35000 + (((((((uint32_t)(W[0x0C98]) >> 1)) & (15))) << 2))))));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
    EMIT(((((((uint32_t)(W[0x0C98]) >> 2)) & (15))) + 409));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
    EMIT(((((uint32_t)(((uint32_t)(W[0x0C98]) >> 2)) % (uint32_t)(6))) + 403));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
    if ((int32_t)(vrd8(0xE012AA)) != (int32_t)(255)) goto L_541C;
    EMIT(425);
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
  L_541C:
    W[0x0CA4] = W[0x09B4];
    return;
}

/* ---- course 2 props: ROM 0x005424..0x0055CC ---- */
static void terrain_props_course2(void)
{

    W[0x09B4] = W[0x0CA4];
    EMIT(32768);
    EMIT(0);
    EMIT(39);
    EMIT(0);
    EMIT(-90112);
    EMIT(0);
    EMIT(40);
    EMIT(0);
    EMIT(-90112);
    EMIT(0);
    EMIT(((448) + (vrd32s(0x35000 + (((((((uint32_t)(W[0x0C98]) >> 1)) & (15))) << 2))))));
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
    if ((int32_t)(40) <= (int32_t)(W[0x0CF4])) goto L_54C6;
    if ((int32_t)(28) < (int32_t)(W[0x0CF4])) goto L_54F6;
  L_54C6:
    EMIT(467);
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
  L_54F6:
    if ((int32_t)(36) != (int32_t)(W[0x0CF4])) goto L_552E;
    EMIT(463);
    EMIT(-(W[0x0CDC]));
    EMIT(((40960) - (W[0x0CE0])));
    EMIT(-(W[0x0CE4]));
  L_552E:
    if ((int32_t)(((W[0x0CF4]) - 42)) > (int32_t)(10)) goto L_55AA;
    switch (0x554C + (vrd16s(0x554C + 2 * (((W[0x0CF4]) - 42))))) {
        case 0x5562: goto L_5562;
        case 0x5568: goto L_5568;
        case 0x556A: goto L_556A;
        case 0x5594: goto L_5594;
        case 0x559C: goto L_559C;
        case 0x55AA: goto L_55AA;
    }
  L_5562:
    dsp_place_prop_a();
  L_5568:
    goto L_55B0;
  L_556A:
    dsp_place_prop_b();
    dsp_place_prop_c();
    dsp_place_windmill_a();
    dsp_place_structure_a();
    dsp_place_structure_c();
    dsp_place_structure_d();
    dsp_place_structure_e();
    dsp_place_distant_scenery();
    goto L_55C6;
  L_5594:
    dsp_place_prop_a();
    goto L_55B0;
  L_559C:
    dsp_place_prop_a();
    dsp_place_structure_b();
    goto L_55B0;
  L_55AA:
    dsp_place_windmill_a();
  L_55B0:
    dsp_place_windmill_b();
    dsp_place_structure_a();
    dsp_place_structure_c();
    dsp_place_structure_d();
    dsp_place_structure_e();
  L_55C6:
    W[0x0CA4] = W[0x09B4];
    return;
}

/* ---- course 3 props: ROM 0x0055CC..0x005626 ---- */
static void terrain_props_course3(void)
{

    dsp_cmd_place_object_abs(0, 39, 0, -356352, 0);
    dsp_cmd_place_object_abs(0, 41, 0, -356352, 0);
    dsp_cmd_set_camera(0, 43, 0, -278528, 0, 0, 32768, 0, 4);
}

/* ---- terrain_props_dispatch @ 0x004F22 ---- */
/* Replaces the transpiled stub, which called terrain_lod_update_wrapper()
 * where the M68K performs the computed jump above.  gameplay_tick already
 * calls terrain_lod_update_wrapper() directly, so that stub was both a
 * duplicate of an existing call AND the reason props never appeared. */
void terrain_props_dispatch(void)
{
    extern int g_no_props;                /* PROPCYCL_NO_PROPS=1, for A/B */
    uint32_t course = (uint32_t)(int32_t)W[0x0E0C];
    if (g_no_props) return;
    if (course > 3) return;               /* the bhi.w after subq.l #3 */
    switch (course) {
    case 0: terrain_props_course0(); break;
    case 1: terrain_props_course1(); break;
    case 2: terrain_props_course2(); break;
    case 3: terrain_props_course3(); break;
    }
}

int g_no_props = 0;
