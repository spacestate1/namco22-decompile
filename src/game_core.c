/*
 * Hand-ported core functions (top 5 most-called)
 * These replace the stubs/disabled versions.
 *
 * All functions use _W[] (intptr_t array) for work RAM access,
 * matching the transpiled code. This ensures mid-frame consistency.
 */
#include "propcycl.h"
#include <string.h>

/* ======= Helpers ======= */

/* Work RAM via _W[] — same memory as transpiled code */
extern intptr_t _W[];
#define W _W

/* Big-endian ROM 16-bit read */
static inline int16_t rom_read16(uint32_t addr) {
    return (int16_t)((g_sys.rom[addr] << 8) | g_sys.rom[addr + 1]);
}

/* Trig table: Q15 signed, 16-bit BIG-ENDIAN, entries every 4 bytes.
 *     cos(a) = 0x20B002 + (a & 0xfffc)
 *     sin(a) = 0x20B004 + (a & 0xfffc)
 * Verified against real sin/cos at heading 53172 (true sin,cos =
 * -30363, 12318): 0x20B002 -> 12307, 0x20B004 -> -30363.
 *
 * These read 0x20B004 for COS and 0x20B006 for SIN -- both one entry
 * high, so trig_cos returned sin and trig_sin returned a neighbouring
 * entry entirely. Everything placed through dsp_cmd_set_camera /
 * dsp_cmd_place_object_rotated_abs got a wrong rotation matrix; the sky
 * dome projected off-frame (black sky). The index itself was already
 * right: ((a>>1)&0x7FFE)*2 == a & 0xfffc. */
/* cos at 0x20B006, not 0x20B002: both callers are ROM 0x021E6E and 0x022008,
 * which read `movea.w $2(a3,d0.l)` off a3 = 0x20B004 -- the exact lane. The
 * 0x20B002 one lags an entry (13 where cos(90 deg) is 0): harmless to the
 * eye, but it made every 0x8001/0x8002 cos word differ from MAME's command
 * list (the count-down direction plate: (32767, 13) against (32767, 0)). */
static inline int16_t trig_cos(uint32_t angle) {
    return rom_read16(0x20B006 + (angle & 0xFFFC));
}
static inline int16_t trig_sin(uint32_t angle) {
    return rom_read16(0x20B004 + (angle & 0xFFFC));
}

/* DSP command buffer pointer — stored as real C pointer in W[0x0CA4].
 *
 * BOUNDS-CHECKED: this had none, so any producer that ran the cursor past
 * the end of dspram turned the next emit into a segfault (seen on the
 * attract path at ~frame 940 via dsp_cmd_place_object_rotated_abs).
 * Every caller already handles a NULL return by dropping the command,
 * which is the right failure mode: lose one object, not the process. */
static int32_t* dsp_cmd_ptr(void) {
    /* PROPCYCL_LISTWHO=<sub>: every display-list write on state-3 sub <sub>,
     * with the cursor offset and the CALLER's return address. Resolve it with
     *     addr2line -e build/propcycl -f -C $((ra - 0x555555554000))
     * under `setarch -R` (ASLR off, PIE base 0x555555554000).
     *
     * This is what identified the DAY1 stale-list bug: it showed all sixty
     * writes on that screen coming from `dsp_cmd_set_camera` and NONE from any
     * rig emitter, which is what proved the rider records on screen were stale
     * buffer contents rather than something being emitted. "Which function put
     * this object in the list" is otherwise a guessing game across ~15
     * candidate emitters. */
    { extern int g_listwho; static int n;
      if (g_listwho > 0 && (long)W[0x0CBC] == 3 && (long)W[0x0CC0] == g_listwho && n++ < 60)
        fprintf(stderr, "[LISTWHO] f%u sub=%ld off=%ld ra=%p ra1=%p\n",
                (unsigned)g_sys.frame_count, (long)W[0x0CC0],
                (long)((uint8_t*)W[0x0CA4] - g_sys.dspram),
                __builtin_return_address(0), __builtin_return_address(1)); }
    uint8_t *p = (uint8_t *)W[0x0CA4];
    uint8_t *lo = g_sys.dspram, *hi = g_sys.dspram + DSPRAM_SIZE;
    if (p < lo || p + 0x40 > hi) {
        static int warned;
        if (warned++ == 0 || propcycl_verbose())
            fprintf(stderr, "[GUARD] dsp_cmd_ptr: cursor %p outside dspram "
                    "[%p,%p) -- command dropped\n", (void*)p, (void*)lo, (void*)hi);
        return NULL;
    }
    return (int32_t*)p;
}
static void dsp_cmd_advance(int count) {
    W[0x0CA4] = (intptr_t)((int32_t*)W[0x0CA4] + count);
}

/* ======= 1. math_atan2 (107 calls) ======= */
/* V2-MIGRATED: math_atan2 @ 0x00A870 now lives in src/v2/game_math_v2.c,
 * translated from the M68K machine code and L2-verified against MAME
 * (8021/8021 calls match). The hand-port that lived here had the
 * base-path conditions swapped and different quadrant transforms
 * (systematically mirrored angles) — see tools/l3/l2_verify.c --v1-sim. */

/* ======= Billboard picker (game_ui debug tool) =======
 * `sprite_3d_project_and_draw` (game_hud.c) is the one caller of
 * sprite_draw_2d that draws a 2D sprite projected FROM a 3D-space
 * position -- the particle "billboard" effects (spray etc, register row
 * 134/137/138). It is not distinguishable from any other sprite draw
 * (HUD digits, menu tiles, banners) once the call has happened, so it
 * tags itself via g_spr3d_tag immediately around its own call; this file
 * records the tagged calls into a small per-frame list, keyed by the
 * same z (z_depth + layer*0x200000) the record ends up carrying in
 * spriteram -- src/renderer_3d.c cross-references that z against
 * sprite_collect()'s output to recover the exact on-screen bounding box
 * for highlighting, without needing to touch sprite_hw.c (a MiSTer-model
 * port kept byte-for-byte faithful to that project) at all. */
int g_spr3d_tag = 0;
/* THE Z ALONE IS NOT A KEY. `sprite_3d_project_and_draw` -- the only caller
 * that tags -- always passes z_depth = 0, so `z_depth + layer * 0x200000`
 * collapses to the LAYER, identical for every billboard on it; a spray
 * emitter putting several particles on one layer is the normal case, so
 * matching on z alone resolved them all to the same on-screen box (and could
 * pick an unrelated sprite sitting at that z). The screen position
 * disambiguates them, and the consumer claims each item at most once. */
typedef struct { int tile; uint32_t z; int sx, sy; } billboard_call;
#define BILLBOARD_MAX 64
static billboard_call g_billboard_calls[BILLBOARD_MAX];
static int g_billboard_n;
void billboard_calls_reset(void) { g_billboard_n = 0; }
int  billboard_calls_count(void) { return g_billboard_n; }
void billboard_calls_get(int i, int *tile, uint32_t *z, int *sx, int *sy) {
    if (i < 0 || i >= g_billboard_n) {
        if (tile) *tile = -1; if (z) *z = 0; if (sx) *sx = 0; if (sy) *sy = 0; return; }
    if (tile) *tile = g_billboard_calls[i].tile;
    if (z)    *z    = g_billboard_calls[i].z;
    if (sx)   *sx   = g_billboard_calls[i].sx;
    if (sy)   *sy   = g_billboard_calls[i].sy;
}

/* ======= 2. sprite_draw_2d (100 calls) ======= */
/* Now uses W[] directly — no mid-frame sync issue */
void sprite_draw_2d(int layer, int16_t tile_id, int16_t screen_x, int16_t screen_y,
                    int32_t z_depth, int16_t p6, int16_t p7, int16_t p8, int16_t p9) {
    if (getenv("PROPCYCL_SPR")) { static int n;
        if (n++ % 120 == 0)
            printf("    [SPR] f=%d tile=%d CA8=%ld CAC=%ld count=%d AAF0=%ld\n",
                   (int)g_sys.frame_count, (int)tile_id, (long)W[0x0CA8],
                   (long)W[0x0CAC], (int)(int16_t)W[0x4AEC], (long)W_HI16(0xAAF0)); }
    /* PROPCYCL_SPRCALL=1 logs the first 400 calls; PROPCYCL_SPRCALL=<frame>
     * logs every call on that one frame instead, which is what a comparison
     * against a MAME spriteram dump at a given screen actually needs. */
    { extern int g_sprcall; if (g_sprcall) { static int n;
        int want = (g_sprcall > 1) ? ((int)g_sys.frame_count == g_sprcall) : (n++ < 400);
        if (want) printf("[SPRCALL] f=%d tile=%d (0x%X) xy=(%d,%d) size=%dx%d p8=%d layer=%d ra=%p\n",
            (int)g_sys.frame_count, (int)tile_id, (unsigned)(uint16_t)tile_id,
            (int)screen_x, (int)screen_y, (int)p6, (int)p7, (int)p8, layer,
            __builtin_return_address(0)); } }
    if (W[0x0CA8] != 0 || W[0x0CAC] != 0) return;

    int16_t count = (int16_t)W[0x4AEC];
    if (count == 0xFF) return;

    count++;
    W[0x4AEC] = count;

    /* The buffer selector is the 16-bit word at byte 0xAAF0 -- the HIGH half
     * of the slot. The LOW half (byte 0xAAF2) is the environment system's
     * target fog R (game_terrain.c, environment_params_load), so a whole-slot
     * read picks up the fog value (255) as a garbage selector. */
    int16_t buf_sel = W_HI16(0xAAF0);
    uint32_t base = 0x4AF0 + buf_sel * 0x1800;
    uint32_t entry = base + count * 0x18;

    /* Recorded HERE, not at function entry: both early returns above
     * (W[0x0CA8]/W[0x0CAC] set, and the count hitting 0xFF) drop the sprite
     * without it ever reaching spriteram, and the picker must not list a
     * billboard that was never drawn. */
    if (g_spr3d_tag && g_billboard_n < BILLBOARD_MAX) {
        g_billboard_calls[g_billboard_n].tile = tile_id;
        g_billboard_calls[g_billboard_n].z = (uint32_t)(z_depth + layer * 0x200000) & 0xFFFFFF;
        g_billboard_calls[g_billboard_n].sx = screen_x;
        g_billboard_calls[g_billboard_n].sy = screen_y;
        g_billboard_n++;
    }

    W[entry + 0x00] = tile_id;
    W[entry + 0x02] = screen_x + 0x280;
    W[entry + 0x04] = screen_y + 0x32A;
    W[entry + 0x08] = z_depth + layer * 0x200000;
    W[entry + 0x0C] = p6;
    W[entry + 0x0E] = p7;
    W[entry + 0x10] = p8;
    W[entry + 0x12] = p9;

    uint32_t list_base = 0x4AF0 + buf_sel * 0x1800;
    int16_t cur = (int16_t)W[list_base + 0x16];

    int16_t prev = 0;
    while (cur != 0) {
        uint32_t cur_entry = list_base + cur * 0x18;
        int32_t cur_z = (int32_t)W[cur_entry + 0x08];
        if (cur_z >= z_depth + layer * 0x200000) break;
        prev = cur;
        cur = (int16_t)W[list_base + cur * 0x18 + 0x16];
    }

    W[entry + 0x16] = cur;
    W[entry + 0x14] = prev;

    if (prev == 0) {
        W[list_base + 0x16] = count;
    } else {
        W[list_base + prev * 0x18 + 0x16] = count;
    }
    if (cur != 0) {
        W[list_base + cur * 0x18 + 0x14] = count;
    }
}

/* The sound MCU's shared RAM, as the 68K addresses it: command words at
 * 0xA04000 + slot*2, parameters at 0xA04100 + n*2, both SIXTEEN BITS
 * big-endian. `g_sys.commsram` byte 0 is 0xA04000. */
int g_sndlog = 0;   /* PROPCYCL_SNDLOG: every sound trigger, with its table entry */

/* PROPCYCL_SNDTRIG=1: every sound REQUEST, logged at the ENTRY of each sound
 * routine -- before any of the state gates -- in exactly the line format
 * tools/overnight/probe_sound_triggers.lua prints from MAME's own 68K, so the
 * two streams diff directly. PROPCYCL_SNDLOG says what reached the MCU; this
 * says what the game logic ASKED for, which is where a wrong or missing id is
 * decided. `par` < 0 means the routine takes no parameter. */
int g_sndtrig = 0;
void snd_trig(const char *fn, int id, int par)
{
    if (!g_sndtrig) return;
    fprintf(stderr, "T fc=%d st=%d sub=%d fn=%s id=%x ",
            (int)(int32_t)_W[0x0C98], (int)(int32_t)_W[0x0CBC], (int)(int32_t)_W[0x0CC0],
            fn, (unsigned)(uint16_t)id);
    if (par < 0) fprintf(stderr, "par=-1\n");
    else         fprintf(stderr, "par=%x\n", (unsigned)(uint16_t)par);
}
/* The terrain-contact picker takes three LONGS (kind, fallback cooldown,
 * speed); the MAME probe prints them the same way. */
void snd_trig_contact(int kind, int p2, int spd)
{
    if (!g_sndtrig) return;
    fprintf(stderr, "T fc=%d st=%d sub=%d fn=contact_pick id=%x par=%x spd=%d\n",
            (int)(int32_t)_W[0x0C98], (int)(int32_t)_W[0x0CBC], (int)(int32_t)_W[0x0CC0],
            (unsigned)kind, (unsigned)p2, spd);
}
static inline void snd_cmd_write(int slot, uint16_t v) {

    uint32_t off = (uint32_t)slot * 2;
    if (slot >= 0 && off + 1 < COMMSRAM_SIZE) {
        g_sys.commsram[off]     = (uint8_t)(v >> 8);
        g_sys.commsram[off + 1] = (uint8_t)(v & 0xFF);
    }
}
static inline void snd_param_write(int n, uint16_t v) {
    uint32_t off = 0x100u + (uint32_t)n * 2;
    if (n >= 0 && off + 1 < COMMSRAM_SIZE) {
        g_sys.commsram[off]     = (uint8_t)(v >> 8);
        g_sys.commsram[off + 1] = (uint8_t)(v & 0xFF);
    }
}

/* ======= 3. sound_play -- THE GAME'S SOUND TRIGGER ======= */
/* ROM 0x00F394. One 16-bit argument: an index into the sound table at
 * 0x355A4 (stride 12, all fields big-endian 16-bit):
 *     +0 command slot   +2 command word   +4 parameter slot
 *     +6 PRIORITY       +8 second parameter slot   +10 its value
 *
 * It writes `cmd | 0x4000` -- bit 14 is "a new command is here" -- into the
 * MCU's shared RAM at 0xA04000 + slot*2, i.e. `g_sys.commsram` byte slot*2.
 *
 * THE BALLOON POP IS ID 5 (slot 28, command 0x0005): ROM 0x011688 and
 * 0x01177E, the two pop sites inside balloon_render_and_hit_check, both push
 * `#$5` before calling here. Every call site in the transpiled tree was
 * `sound_play()` with NO ARGUMENT -- the declaration is unprototyped, so C
 * accepted it and the id was whatever was in the register. That was every
 * sound in the game; see tools/overnight/fix_sound_args.py.
 *
 * Three further defects were in this function itself:
 *   - `id > 0x20` rejected everything above 32, but the table runs to 0x58
 *     (0x59 is its -1 terminator) -- so the ending, results and name-entry
 *     sounds were dropped even once their ids were restored;
 *   - the parameter write was at `(0x100 + slot) * 2`, double the offset the
 *     ROM uses (`movea.l #$a04100` then `(a0, d0.w*2)` = 0x100 + slot*2);
 *   - the priority came from +10 (the second parameter's VALUE) where
 *     ROM 0x00F46C reads `$6(a1)`. */
int32_t sound_play(int32_t scene_id) {
    int16_t id = (int16_t)scene_id;
    snd_trig("sound_play", id, -1);
    /* SND_ID_UNRECOVERED means the decompiler lost this call's argument and
     * the ROM has not been read for it yet. PLAY NOTHING. An unprototyped
     * `sound_play()` passes whatever is in the register, and a random id
     * indexes a real row of the sound table -- the symptom is the announcer
     * sample retriggering over and over during play. A missing sound is the
     * honest failure; a wrong one is not.
     *
     * Three of these are not lost arguments at all but spurious calls:
     * `stage_transition_run`'s trio (game_gameplay.c) have no counterpart in
     * the ROM, which calls FUN_00026b46 and friends at those points
     * (0x0137B8..). Its ONE real sound call is sound_play_p(0x0A, ...)
     * at 0x0137B0, and that one is present and correct. */
    if (id < 0 || id > 0x58) return 0;

    uint32_t tbl = 0x355A4 + (uint32_t)id * 12;
    int16_t comms_slot = rom_read16(tbl + 0);
    int16_t comms_data = rom_read16(tbl + 2);
    int16_t priority   = rom_read16(tbl + 6);
    int16_t vis_slot   = rom_read16(tbl + 8);
    int16_t vis_data   = rom_read16(tbl + 10);

    if (id == 0x15) {
        /* The coin sound. With a screen transition running the ROM plays
         * (0x26, 0x77) through the two-argument entry instead; otherwise it
         * writes this entry's command with no slot-validity check at all. */
        /* 0xE15F44 is a 16-bit flag (`tst.w $e15f44.l` @0x00F3A8), the HIGH half
         * of its slot; the whole-slot read also saw the priority word at 0xE15F46. */
        if (W16(0x15F44) != 0) { sound_play_p(0x00260077); return 0; }
        snd_cmd_write(comms_slot, (uint16_t)comms_data | 0x4000);
        return 0;
    }

    {
        int32_t state = (int32_t)W[0x0CBC];
        if ((state & ~1) != 2 && (state & ~1) != 4 && W[0x3FFC] == 0) return 0;
        if (state == 3 && W[0x0CC0] == 3) {
            if (W[0x0E44] <= 0x37 || W[0x0E18] != 0) return 0x38;
        }
    }

    if (g_sndlog)
        fprintf(stderr, "[SOUND] f%-6u sub=%-3ld %-22s id=0x%02X  slot %-2d cmd=%04X\n",
                (unsigned)g_sys.frame_count, (long)W[0x0CC0], "sound_play",
                id, comms_slot, (uint16_t)comms_data | 0x4000);
    if (comms_slot < 0) return 0;
    if (vis_slot >= 0) snd_param_write(vis_slot, (uint16_t)vis_data);
    snd_cmd_write(comms_slot, (uint16_t)comms_data | 0x4000);
    /* ROM 0x00F46C: `cmp.w $e15f46.l,d0 ; ble ; move.w $6(a1),$e15f46.l`. 0xE15F46
     * is 2-mod-4 -- the LOW half of slot 0x15F44 -- and a whole-slot store to it
     * was never written back to work RAM, so the sync rebuilt it from its
     * neighbours every frame and the ambience gate in stage_camera_path_update
     * (`tst.w $e15f46`) saw garbage. */
    if (priority > W16(0x15F46)) W16_SET(0x15F46, priority);
    return 0;
}

/* ======= 4. debug_draw_value (69 calls) ======= */
/* Writes to textram directly — no work_ram sync issue */
void debug_draw_value(int col, int row, int hex_digits, const char* label,
                      int32_t value, int16_t suffix) {
    int label_len = 0;
    if (label) {
        const char* p = label;
        while (*p) {
            int off = (row * 64 + col + label_len) * 2;
            if (off >= 0 && off + 1 < TEXTRAM_SIZE) {
                g_sys.textram[off]     = 0;
                g_sys.textram[off + 1] = (uint8_t)*p;
            }
            label_len++;
            p++;
        }
    }

    int hex_col = col + label_len;
    for (int i = hex_digits - 1; i >= 0; i--) {
        int digit = (value >> (i * 4)) & 0xF;
        int off = (row * 64 + hex_col + (hex_digits - 1 - i)) * 2;
        if (off >= 0 && off + 1 < TEXTRAM_SIZE) {
            uint8_t ch = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
            g_sys.textram[off]     = 0;
            g_sys.textram[off + 1] = ch;
        }
    }
    (void)suffix;
}

/* ======= 5. dsp_cmd_set_camera (66 calls) ======= */
/*
 * Emits a 13-word 0x8002 inline DSP command: [0x8002, p1, p2, p3, p4, p5, cos_x, sin_x, ...]
 *
 * Despite the name, this does NOT set camera state. It places a single object
 * at a position that is already in camera-frame space. The caller supplies
 * absolute camera-relative coordinates — typically camera at origin (after
 * camera_state_reset()) so p3/p4/p5 are object XYZ from the camera's viewpoint.
 *
 * Parameter layout (matching the 13-word 0x8002 command — see renderer_internal.h):
 *   p1=priority, p2=model_id, p3=x, p4=y, p5=z
 *   rot_x/y/z: 16-bit rotation angles (65536 = full circle)
 *
 * cos/sin ORDER: writes [sin_x, cos_x, sin_y, cos_y, sin_z, cos_z], matching
 * the transpiled dsp_cmd_place_object_rotated, the renderer's
 * placement_matrix(s,c,...) and pc_master_model.py's mode-2 grammar.
 * (It used to write cos first, which made every rotation wrong.)
 *
 * IMPORTANT — NO camera subtraction: this function does NOT subtract
 * W[0x0CDC/0x0CE0/0x0CE4] (game camera position). The transpiled
 * dsp_cmd_place_object_rotated DOES subtract camera. The renderer undoes
 * camera heading rotation for both (glRotatef(-heading)).
 */
void dsp_cmd_set_camera(int32_t p1, int32_t p2, int32_t p3, int32_t p4,
                        int32_t p5, uint32_t rot_x, uint32_t rot_y,
                        uint32_t rot_z, int32_t flags) {
    int32_t* cmd = dsp_cmd_ptr();
    if (!cmd) return;
    cmd[0]  = 0x8002;
    cmd[1]  = p1;
    cmd[2]  = p2;
    cmd[3]  = p3;
    cmd[4]  = p4;
    cmd[5]  = p5;
    /* SIN FIRST, COS SECOND -- [s_x,c_x, s_y,c_y, s_z,c_z].
     * This wrote cos first, which the file's own comment flagged as
     * "rotation is therefore currently incorrect". Ground truth:
     * pc_master_model.py documents a mode-2 entry as
     * {model, tx, ty, tz, s0,c0, s1,c1, s2,c2}; the transpiled
     * dsp_cmd_place_object_rotated writes sin first; and the renderer's
     * placement_matrix(s1,c1,s2,c2,s3,c3) decodes sin first. Every
     * rotated placement from these two encoders had its rotation built
     * from swapped sin/cos -- the sky dome projected to screen
     * x[-428..-222] (off-frame left, hence a black sky) where the
     * recording puts it at x[296..343]. */
    cmd[6]  = (int32_t)trig_sin(rot_x);
    cmd[7]  = (int32_t)trig_cos(rot_x);
    cmd[8]  = (int32_t)trig_sin(rot_y);
    cmd[9]  = (int32_t)trig_cos(rot_y);
    cmd[10] = (int32_t)trig_sin(rot_z);
    cmd[11] = (int32_t)trig_cos(rot_z);
    cmd[12] = flags;
    dsp_cmd_advance(13);
}

/* ======= Bonus: dsp_cmd_place_object_rotated_abs (49 calls) ======= */
/*
 * Emits a 13-word 0x8001 inline DSP command for a rotated object at an absolute
 * (non-camera-relative) position. Same parameter layout as dsp_cmd_set_camera
 * but writes opcode 0x8001 instead of 0x8002, and the renderer interprets it
 * as a mode-0x8001 data entry with rotation.
 *
 * Same sin-first ordering as dsp_cmd_set_camera.
 * Same no-camera-subtraction caveat applies here.
 */
void dsp_cmd_place_object_rotated_abs(int32_t p1, int32_t p2, int32_t p3, int32_t p4,
                                       int32_t p5, uint32_t rot_x, uint32_t rot_y,
                                       uint32_t rot_z, int32_t flags) {
    int32_t* cmd = dsp_cmd_ptr();
    if (!cmd) return;
    cmd[0]  = 0x8001;
    cmd[1]  = p1;
    cmd[2]  = p2;
    cmd[3]  = p3;
    cmd[4]  = p4;
    cmd[5]  = p5;
    /* SIN FIRST, COS SECOND -- [s_x,c_x, s_y,c_y, s_z,c_z].
     * This wrote cos first, which the file's own comment flagged as
     * "rotation is therefore currently incorrect". Ground truth:
     * pc_master_model.py documents a mode-2 entry as
     * {model, tx, ty, tz, s0,c0, s1,c1, s2,c2}; the transpiled
     * dsp_cmd_place_object_rotated writes sin first; and the renderer's
     * placement_matrix(s1,c1,s2,c2,s3,c3) decodes sin first. Every
     * rotated placement from these two encoders had its rotation built
     * from swapped sin/cos -- the sky dome projected to screen
     * x[-428..-222] (off-frame left, hence a black sky) where the
     * recording puts it at x[296..343]. */
    cmd[6]  = (int32_t)trig_sin(rot_x);
    cmd[7]  = (int32_t)trig_cos(rot_x);
    cmd[8]  = (int32_t)trig_sin(rot_y);
    cmd[9]  = (int32_t)trig_cos(rot_y);
    cmd[10] = (int32_t)trig_sin(rot_z);
    cmd[11] = (int32_t)trig_cos(rot_z);
    cmd[12] = flags;
    dsp_cmd_advance(13);
}

/* ======= Bonus: fixed_point_mul_32x32 (44 calls) ======= */
int32_t fixed_point_mul_32x32(int32_t a, int32_t b) {
    int64_t result = (int64_t)a * (int64_t)b;
    return (int32_t)(result >> 16);
}
