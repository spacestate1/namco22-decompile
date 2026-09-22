/*
 * DSP & 3D Rendering Pipeline
 * Auto-split from game_deps.c / game_ported.c
 */
#include "propcycl.h"
#include "ending_rd.h"
#include "ea68k.h"
#include <math.h>
intptr_t anim_w_off(const void *p);   /* game_misc.c */

/* PAIR ORDER, A/B-able (register row 95 / FAILED_APPROACHES 1.27).
 *
 * The ROM at 0x0261C8 writes each rotation pair with two move.l's off
 * trigbase-2, whose LOW halves are sin then cos -- the same order
 * dsp_cmd_object_transform already uses and the same order MAME's own
 * command buffer carries (tools/overnight/rig_wire_gate.py). scene_node_render AND
 * camera_dsp_terrain_render (the rider child walker) have shipped cos-first, with the renderer's g_rot8008=2 swapping it back.
 * That is right by compensation for the FLYOVER (one producer) and wrong for
 * the DEMO, where the bike root comes through dsp_cmd_object_transform
 * sin-first and the rider through here cos-first: one global swap cannot
 * serve both, and the rider ends up ~130 deg off the bike.
 *
 * Switching the producer alone broke the picture last time, so it is now an
 * A/B-able: PROPCYCL_SNR_SINFIRST=0 (and the N key in rig_viewer) restores
 * the old cos-first wire. Validated with rot8008=1 and root15=1 together:
 * flyover 1 unit / 0.4 deg, demo 2-13 units / 1.4-1.9 deg against MAME
 * (tools/overnight/rig_split_gate.py), where the old wire scored 130 deg. */
int g_snr_sinfirst = 1;   /* DEFAULT ON since 2026-09-11 -- see register row 95 */
#define SNR_PAIR(dst, i, a) do { \
    if (g_snr_sinfirst) { (dst)[(i)] = vrd16s(0x20B004 + (a)); (dst)[(i)+1] = vrd16s(0x20B006 + (a)); } \
    else                { (dst)[(i)] = vrd16s(0x20B002 + (a)); (dst)[(i)+1] = vrd16s(0x20B004 + (a)); } \
} while (0)


/* Work RAM as intptr_t array */
extern intptr_t _W[];
#define W _W

/* ROM as byte array */
#define R g_sys.rom

/* Ghidra type aliases */
typedef int32_t undefined4;
typedef int16_t undefined2;
typedef int8_t undefined1;
typedef uint8_t byte;
typedef void* code;

/* Ghidra helpers */
#define CONCAT11(a,b) (((uint16_t)(a)<<8)|(uint8_t)(b))
#define CONCAT22(a,b) (((uint32_t)(a)<<16)|(uint16_t)(b))
#define CONCAT31(a,b) (((uint32_t)(a)<<8)|(uint32_t)(uint8_t)(b))
#define CONCAT44(a,b) (((uint64_t)(a)<<32)|(uint32_t)(b))
#define SBORROW4(a,b) ((int32_t)(a) < (int32_t)(b))
#define CARRY4(a,b) ((uint32_t)(a) > (uint32_t)(~(uint32_t)(b)))

/* Stub for unresolvable hardware accesses */
static int32_t _mmio_dummy;
#define MMIO_R(a) (_mmio_dummy)
#define MMIO_W(a,v) do { mem_write32((a),(v)); } while(0)
static int _safety_ctr = 0;

/* Cross-file function declarations (pointer-returning functions only,
 * to prevent 64-bit pointer truncation from implicit int return) */
short * sound_play_p();

/* ============================ THE SOUND COMMAND PATH ========================
 *
 * Everything named `scene_*` in this cluster is the SOUND SYSTEM, not a scene
 * loader -- the Ghidra names are wrong. ROM 0x00F4E0..0x00F9C4.
 *
 * The 68K sees the sound MCU's shared RAM at 0xA04000, which is
 * `g_sys.commsram` byte 0. Two regions:
 *
 *   0xA04000 + slot*2   32 sixteen-bit COMMAND words. Bit 14 (0x4000) is
 *                       "a new command is here"; bit 15 is "this voice is
 *                       running", which the MCU sets and the CPU clears to
 *                       stop a sound. Slot 0 is music, 24-31 are effects.
 *   0xA04100 + n*2      sixteen-bit PARAMETER words (volume, pitch, pan).
 *
 * Every access in the ROM is `move.w ... (a0, d0.w*2)` -- SIXTEEN BITS at a
 * WORD index. The transpilation had `g_sys.commsram[slot]`: a BYTE store at a
 * BYTE index, so the index was half right and the value lost its high byte --
 * and the high byte is where 0x4000 lives. The trigger bit was discarded by
 * every sound the game ever tried to play, which is why nothing was ever
 * audible. Both errors had to be fixed together; either alone still writes
 * the wrong place.
 *
 * The per-sound table is at ROM 0x355A4, stride 12 (`lsl.l #$2` on id*3):
 *     +0  command slot      (negative = this sound does not exist)
 *     +2  command word
 *     +4  parameter slot for the CALLER's argument
 *     +8  parameter slot for the table's own constant (negative = none)
 *     +10 that constant
 * All five are big-endian 16-bit. The transpile read +0 and +2 natively
 * through a `short *` into `R` (a uint8_t[]) and the rest through `rom_nr16`,
 * which is native order -- so on a little-endian host every one came back
 * byte-swapped.
 *
 * ARGUMENTS. The ROM takes TWO 16-bit stack arguments. Measured at a real
 * call site, ROM 0x0096D8:
 *     move.w #$6a,-(a7) ; move.w #$26,-(a7) ; jsr $f698
 * The LAST push lands at $8(a7), and $8(a7) is what the routine uses as the
 * table index -- so the SOUND ID is 0x26 and the PARAMETER is 0x6A. Read as
 * one 32-bit big-endian word, which is what Ghidra did, that is
 * (id << 16) | param. Ghidra then used the whole thing as the index.       */

/* PROPCYCL_SNDLOG=1: one line per AUDIBLE sound trigger -- the function that
 * asked for it, the id, the mailbox slot it lands in, the command word and
 * the parameter. This is the log to read when a sound is wrong, doubled or
 * missing: the game's own trigger is at the top of the chain and everything
 * below it (the MCU, the C352) only carries out what this line says. */
static void snd_report(const char *who, int id, int slot, unsigned cmd, int par)
{
    extern int g_sndlog;
    if (!g_sndlog) return;
    fprintf(stderr, "[SOUND] f%-6u sub=%-3ld %-22s id=0x%02X  slot %-2d cmd=%04X",
            (unsigned)g_sys.frame_count, (long)W[0x0CC0], who, id, slot, cmd);
    if (par >= 0) fprintf(stderr, "  param=0x%02X", par);
    fputc('\n', stderr);
}

/* The sound table at ROM 0x355A4. */
static inline int snd_slot (int id) { return vrd16s(0x355A4 + id * 12); }
static inline int snd_cmd  (int id) { return vrd16 (0x355A6 + id * 12); }
static inline int snd_pslot(int id) { return vrd16s(0x355A8 + id * 12); }
static inline int snd_pslot2(int id){ return vrd16s(0x355AC + id * 12); }
static inline int snd_pval2(int id) { return vrd16 (0x355AE + id * 12); }

/* A command word lives at commsram byte slot*2; a parameter at 0x100 + n*2.
 * Both indices are SIGNED -- the table really does carry -1 parameter slots,
 * and `(a0, d0.w*2)` sign-extends, so the hardware writes just below the
 * parameter region. Reproduced, with a range check because we do not have
 * the wrap-around a 16-bit index gives on the real bus. */
extern int g_sndlog;
static inline void snd_cmd_w(int slot, unsigned v) {

    int off = slot * 2;
    if (off >= 0 && off + 1 < (int)COMMSRAM_SIZE) comms_w16(g_sys.commsram, off, v);
}
static inline unsigned snd_cmd_r(int slot) {
    int off = slot * 2;
    return (off >= 0 && off + 1 < (int)COMMSRAM_SIZE) ? comms_r16(g_sys.commsram, off) : 0;
}
static inline void snd_par_w(int n, unsigned v) {
    int off = 0x100 + n * 2;
    if (off >= 0 && off + 1 < (int)COMMSRAM_SIZE) comms_w16(g_sys.commsram, off, v);
}

/* The slot list at ROM 0x359E4: BE16, stride 2, terminated by a negative. */
static void snd_params_reset(void) {
    int i;
    for (i = 0; ; i++) {
        int off = 0x359E4 + i * 2;
        if (off + 1 >= (int)ROM_SIZE) break;
        int slot = vrd16s(off);
        if (slot < 0) break;
        snd_par_w(slot, 0xFF);
    }
}

short * sound_play_or_defer();
short * sound_play_p2();
undefined4 * dsp_cmd_emit_arrow_indicator();
undefined4 * player_vehicle_dsp_render();
undefined4 * render_town_with_rotation();
char * eeprom_write_verify_block();

/* Forward declarations (called before definition in this file) */
uint64_t dsp_ram_clear_32();

/* Named work RAM variables */
#define g_fog_mode W[0xEB18]
#define g_fog_r W[0xEB1A]
#define g_fog_g W[0xEB1C]
#define g_fog_b W[0xEB1E]
#define g_stage_mode W16(0xE10)
#define g_targets_hit W16(0xE66)
#define g_combo_count W16(0xE64)
#define g_balloon_type W16(0xE68)
#define g_bonus_active W16(0xE6A)
#define g_terrain_flags W[0x12C0]
#define g_terrain_mask W[0x12C4]
#define g_terrain_needs_update W[0x12F8]
#define g_visible_chunk_count W[0x122C]
#define g_camera_offset_y W[0x1230]
#define g_chunk_lookup_table W[0x1234]
#define g_terrain_lod_level W[0x1304]
#define g_terrain_base_addr W[0x12B4]
#define g_terrain_bitmask_ptr W[0x12B8]
#define g_grid_cell_x W[0x0D28]
#define g_grid_cell_z W[0x0D2C]
/* The per-object (balloon) array. The ROM addresses it as a3/a2 = 0xE0490C
 * with a 0x3C-byte record -- `d0 = d2<<4; d0 -= d2; d0 <<= 2` at
 * objects_move_update @0x011DE2 and balloon_render_and_hit_check @0x0116EA --
 * and walks 8 records (`subq.l #8,d0; blt` @0x011E74).
 *
 * These macros used to name WRAM 0xE00F00, uniformly 0x3A10 too low, and
 * every call site indexed them `[n * 0xf]`: a 0xf-SLOT stride where the
 * _W[] model needs the 0x3C-BYTE one. Both errors cancelled for n=0, so the
 * array looked to work. It did not: at n=0 the active flag landed on
 * W[0x0F2C], and 0x0F2C == 0x0E6C + 12*0x10, i.e. entry 12 of the
 * visible-chunk list. Every balloon hit wrote `0x10 - W[0x0C8C]` over a
 * chunk id, and the chunk consumer then read a bogus spawn-list pair
 * (measured: chunk=-536 -> first=142507774) and indexed _W[] with it.
 * Latent only while the chunk list was walked at the wrong stride 4.   */
#define g_obj_base_x W[0x4910]
#define g_obj_base_y W[0x4914]
#define g_obj_base_z W[0x4918]
#define g_obj_speed W[0x491C]
#define g_obj_angle_delta W[0x4920]
#define g_obj_pos_x W[0x492C]
#define g_obj_pos_y W[0x4930]
#define g_obj_pos_z W[0x4934]
#define g_obj_grid_cell W[0x4938]
#define g_obj_active_flag W[0x493C]
#define g_obj_angle W[0x4940]
#define _g_system_halt W[0x0E1C]
#define g_scene_fade_level W[0x0E20]
#define g_scene_max_priority W[0x15F46]
#define _g_scene_transition_active W[0x15F44]
#define _g_game_timer_active W[0x16950]
#define g_scene_deferred_object W[0x15F50]
#define g_replay_data W[0x0EB60]
#define g_input_buttons_raw W[0x2B5E]
#define _g_halt_flag W[0x0CA8]
#ifndef g_time_limit
#define g_time_limit W[0x3FF0]
#endif
#ifndef g_stage_timer
#define g_eeprom_status W[0x4170]
#define g_stage_timer W[0x0CCC]
#endif
#ifndef g_credit_countdown
#define g_credit_countdown W[0x2C14]
#endif
#ifndef g_countdown_sub_frame
#define g_countdown_sub_frame W[0x17388]
#define g_countdown_display W[0x1737C]
#define _g_vblank_flag W[0x0C9C]
#define _g_dsp_render_result_0 W[0x0CB4]
#define _g_dsp_render_result_1 W[0x0CB8]
#define _g_sprite_dirty W[0x2C20]
#define _g_task_list_head W[0x2E40]
#define _g_task_list_tail W[0x2E44]
#define _g_task_pool_size W[0x2E48]
#endif
#ifndef g_countdown_sub_frame
#define g_countdown_sub_frame W[0x17388]
#define g_countdown_display W[0x1737C]
#define _g_vblank_flag W[0x0C9C]
#define _g_dsp_render_result_0 W[0x0CB4]
#define _g_dsp_render_result_1 W[0x0CB8]
#define _g_sprite_dirty W[0x2C20]
#define _g_task_list_head W[0x2E40]
#define _g_task_list_tail W[0x2E44]
#define _g_task_pool_size W[0x2E48]
#endif
#define g_audio_env_params W[0x3FE0]
#define g_grid_sub_x W[0x0D30]
#define g_grid_sub_z W[0x0D34]
#define g_grid_tile_x W[0x0D38]
#define g_grid_tile_z W[0x0D3C]
#define g_grid_frac_x W[0x0D40]
#define g_grid_frac_z W[0x0D44]
#define g_countdown_timer W[0x17378]
#define g_countdown_display W[0x1737C]
#define g_countdown_done W[0x17380]
#define g_countdown_phase W[0x17384]
#define g_countdown_sub_frame W[0x17388]
#define g_scene_transition_active W[0x15F44]
#define g_scene_deferred_timer W[0x15F56]
#define g_replay_frame_idx W[0x16D84]
#define g_replay_active W[0x17268]
#define g_transition_frame W[0x16648]
#define g_sub_state_attract W[0x0CC4]
#define g_sub_state_max W[0x0CC8]
#define g_time_display W[0x0E48]
#define g_input_boundary_flags W[0x2BD8]
#define g_coin_inserted W[0x2C12]
#define g_credit_countdown W[0x2C14]
#define g_pause_speed W[0x0E34]
#define g_stage_timer W[0x0CCC]
#define g_time_limit W[0x3FF0]
#define g_eeprom_status W[0x4170]
#define ROM16(addr) ((int16_t)((g_sys.rom[(addr)] << 8) | g_sys.rom[(addr)+1]))

/* Forward declarations (from original preambles) */
uint32_t FUN_000268ce(intptr_t node, intptr_t list);
char * eeprom_write_verify_block();
short * sound_play_or_defer();
short * sound_play_p();
uint8_t ** animated_objects_render_all();
uint32_t * render_gate_or_ring();
int * render_player_bike_model();
undefined4 * dsp_cmd_emit_arrow_indicator();
undefined4 * player_vehicle_dsp_render();
undefined4 * render_town_with_rotation();
uint8_t * gameplay_timer_update();
int * render_flag_banner();
short * sound_play_p2();
short * FUN_0000f5ca();

/* ---- camera_dsp_sky_render ---- */

void camera_dsp_sky_render(void)

{
  undefined4 *puVar1;
  int iVar2;
  
  *(int32_t*)W[0x0CA4] = 0x8008;
  ((int32_t*)W[0x0CA4])[1] = 3;
  ((int32_t*)W[0x0CA4])[2] = 1;
  ((int32_t*)W[0x0CA4])[3] = 0;
  ((int32_t*)W[0x0CA4])[4] = 0x7fff;
  /* Trig-table read — see comment at scene_node_render. */
  { uint32_t _a = (uint32_t)((W[0x0C8C] & 0x3f) * 0x400);
    ((int32_t*)W[0x0CA4])[5] = vrd16s(0x20B004 + _a);
    ((int32_t*)W[0x0CA4])[6] = vrd16s(0x20B006 + _a); }
  (void)puVar1;
  ((int32_t*)W[0x0CA4])[7] = 0;
  ((int32_t*)W[0x0CA4])[8] = 0x7fff;
  ((int32_t*)W[0x0CA4])[9] = 0;
  ((int32_t*)W[0x0CA4])[10] = 0;
  ((int32_t*)W[0x0CA4])[0xb] = 0;
  ((int32_t*)W[0x0CA4])[0xc] = 0xfffffec0;
  ((int32_t*)W[0x0CA4])[0xd] = 0;
  ((int32_t*)W[0x0CA4])[0xe] = 0xffffffff;
  ((int32_t*)W[0x0CA4])[0xf] = 0x8009;
  ((int32_t*)W[0x0CA4])[0x10] = 3;
  ((int32_t*)W[0x0CA4])[0x11] = W[0xB0FC];
  ((int32_t*)W[0x0CA4])[0x12] = 3;
  ((int32_t*)W[0x0CA4])[0x13] = 0x800a;
  if (W[0x12BC] == 0) {
    iVar2 = 0x30b;
  }
  else {
    iVar2 = 0x30d;
  }
  ((int32_t*)W[0x0CA4])[0x14] = iVar2 + ((int)W[0x0C8C] >> 2 & 1U);
  ((int32_t*)W[0x0CA4])[0x15] = 3;
  ((int32_t*)W[0x0CA4])[0x16] = W[0xEB08];
  ((int32_t*)W[0x0CA4])[0x17] = W[0xEB0C];
  ((int32_t*)W[0x0CA4])[0x18] = W[0xEB10];
  W[0x0CA4] = W[0x0CA4] + 0x19 * 4;
  return;
}


/* ---- camera_grid_calc_position ---- */

void camera_grid_calc_position(void)

{
  /* THE CAMERA'S GRID CELL IS `grid_x + grid_z * 8`, NOT AN ARRAY LOOKUP.
   * This is register row 90's bug in the CAMERA twin of the function row 90
   * fixed. ROM 0x006EB4:
   *     006EDC  lea.l ([$1c,a1], d0.l*8), a0     a1 = 0xE00CDC, d0 = grid_z
   *     006EE2  move.l a0, $18(a1)               -> W[0x0CF4]
   * a 68020 MEMORY-INDIRECT POST-INDEXED mode: it loads the long at
   * [a1+0x1c] (= W[0x0CF8], grid_x) and ADDS d0*8. It is an address
   * computation, not a load -- the identical instruction to
   * world_grid_calc_position's `lea ([$28,a1], d0.l*8)` at 0x006F10.
   * Ghidra rendered it as `(&W[0x0CF8])[grid_z*2]`, the (&W[base])[N] stride
   * class, which is right only at grid_z == 0 -- and every headless test in
   * this repo sits at z-cell 0, which is why it survived them all. At
   * grid_z == 1 it read slot 0x0CFA, a 2-mod-4 offset that sync_wram_to_W
   * rebuilds from neighbouring bytes.
   *
   * What it cost: W[0x0CF4] is the PROPS GATE. terrain_props_course0 hangs
   * four animated groups off `if (52 > W[0x0CF4])` (ROM 0x00500E `moveq #$34
   * / cmp.l $18(a3),d0 / bgt`) -- models 340-355, 334-339, 356-363, 364-367
   * and 368-373, record codes 409-442. Those are the RIVER AND WATERFALL, and
   * with the gate reading garbage they blinked in and out as the camera
   * crossed z-cells. A user reported exactly that for a whole session
   * ("a waterfall in the first level disappears now and then ... not just the
   * waterfall but the whole water above the cave") and named codes 422, 432,
   * 433 and 441 with the object picker -- one in each of those groups.
   *
   * divu.l, not divs.l: the camera form of this function uses an UNSIGNED
   * divide where the player form at 0x006EF0 uses a signed one. Kept as the
   * machine has it. */
  W[0x0CF8] = (int32_t)((uint32_t)(int32_t)W[0x0CDC] / 0x18000);
  W[0x0CFC] = (int32_t)((uint32_t)(int32_t)W[0x0CE4] / 0x18000);
  W[0x0CF4] = (int32_t)W[0x0CF8] + (int32_t)W[0x0CFC] * 8;
  return;
}


int g_contactlog = 0;

/* ---- camera_mode_select ---- */

/* THREE _W[] MODEL FAULTS IN ONE FUNCTION, all read straight off ROM 0x028302.
 *
 * 1. THE TERRAIN LAYER STRIDE IS 0x140, NOT 0x50 -- register row 91's bug, in
 *    the one file row 91's 93-site sweep did not cover. The ROM builds the
 *    index as
 *        02831C  move.l d2,d0 ; lsl.l #$2,d0 ; add.l d2,d0 ; lsl.l #$6,d0
 *        028324  tst.l  $e01324(d0.l)
 *    i.e. layer*4 + layer = layer*5, <<6 = layer*0x140, and a 32-bit test.
 *    At 0x50 the five per-layer CONTACT FLAGS were read from slots 0x1374,
 *    0x13C4, 0x1414, 0x1464 -- inside record 0 -- so the contact bitmask was
 *    built from the wrong memory and reported a hit almost every frame.
 *
 * 2/3. 0x16A28, 0x16A2A and 0x16A2C are 16-BIT (`cmpi.w #$7,(a3)` @0x02834A,
 *    `move.w $e16a2a.l` @0x028312 and 0x02833C, `tst.w`/`subq.w`/`move.w`
 *    on (a1) @0x028358..0x02838A). 0x16A2A is 2-mod-4, i.e. the LOW half of
 *    the slot at 0x16A28 -- not a slot of its own, which is how the C read it.
 *
 * What it cost: camera_mode_select's return value arms W[0x169EC]'s low half
 * for 120 frames, and that gates camera_dsp_sky_render -- record codes
 * 847-851, the flash a user sees as "stars around the rider's head", which
 * should appear on a hit and fade. MAME draws none of 847-851 in 720 frames
 * of gameplay capture. It also gates the row 114 fallback, so a false contact
 * costs the pose dispatch as well. */
short camera_mode_select(void)

{
  uint32_t uVar1;
  short sVar2;
  undefined2 uVar3;
  
  sVar2 = W_LO16(0x16A28);
  W_SET_LO16(0x16A28, 0);
  uVar1 = 0;
  do {
    if (W[0x1324 + (uVar1 * 0x140)] != 0) {
      W_SET_LO16(0x16A28, (uint16_t)(1 << (uVar1 & 0x3f)) | (uint16_t)W_LO16(0x16A28));
    }
    uVar1 = uVar1 + 1;
  } while ((int)uVar1 < 5);
  /* PROPCYCL_CONTACTLOG=1: the per-layer contact flags, the bitmask they
   * build and the mode that comes out. The mode arms the hit animation for
   * 120 frames, so "why is the hit flash always on" is a question about these
   * five flags. */
  { extern int g_contactlog; static int n;
    if (g_contactlog && (n++ % 60) == 0)
      fprintf(stderr, "[CONTACT] f%u flags %ld %ld %ld %ld %ld -> mask 0x%X"
              "  EB06=%ld 12D4=%ld 12DC=%ld prev=%d\n",
              g_sys.frame_count,
              (long)W[0x1324], (long)W[0x1324+0x140], (long)W[0x1324+0x280],
              (long)W[0x1324+0x3C0], (long)W[0x1324+0x500],
              (unsigned)(uint16_t)W_LO16(0x16A28), (long)W[0xEB06],
              (long)W[0x12D4], (long)W[0x12DC], (int)sVar2); }
  /* The PARTICLE gate's two fields, per layer: +0x118 (must be non-zero) and
   * +0x11C (must be 0xE0, 0xE1 or 0xE5 -- the SURFACE TYPE). Those are what
   * decide whether the spray billboards near ground and water appear. */
  { extern int g_contactlog; static int n3;
    if (g_contactlog && (n3++ % 60) == 0) {
      int L; char b[256]; int o = 0;
      o += snprintf(b+o, sizeof b-o, "[PGATE] f%u", g_sys.frame_count);
      for (L = 0; L < 5; L++)
        o += snprintf(b+o, sizeof b-o, "  L%d:%ld/%lX nf%ld", L,
                      (long)W[0x1424 + L * 0x140], (long)W[0x1428 + L * 0x140],
                      (long)W[0x13EC + L * 0x140]);
      o += snprintf(b+o, sizeof b-o, "   nlayers %ld  types L0:", (long)W[0x1308]);
      { int k; for (k = 0; k < 4 && o < (int)sizeof b - 8; k++)
          o += snprintf(b+o, sizeof b-o, "%02lX ", (long)(uint8_t)W[0x13AC + k]); }
      o += snprintf(b+o, sizeof b-o, "  facelist L0..L4:");
      { int k; for (k = 0; k < 5 && o < (int)sizeof b - 8; k++)
          o += snprintf(b+o, sizeof b-o, " %ld", (long)W[0x1330 + k * 0x140]); }
      fprintf(stderr, "%s\n", b); } }
  { extern int g_contactlog; static int n2;
    if (g_contactlog && (n2++ % 60) == 0)
      fprintf(stderr, "[CONTACT]   hit-anim counter W_LO16(0x169EC) = %d"
              "  (raw slot 0x%08lX)\n",
              (int)W_LO16(0x169EC), (unsigned long)(uint32_t)W[0x169EC]); }
  if ((W[0xEB06] != 0) || (((int)W_HI16(0x16A28) == 7 && (W[0x12D4] != 0)))) {
    if (-1 < (int)W_HI16(0x16A2C)) {
      W_SET_HI16(0x16A2C, (int)W_HI16(0x16A2C) - 1);
    }
    if (W[0x12D4] == 0) {
      if (W[0x12DC] == 0) {
        if (sVar2 == 0) {
          if ((int)W_LO16(0x16A28) == 2) {
            sVar2 = 2;
            uVar3 = 0x96;
          }
          else if ((int)W_LO16(0x16A28) == 4) {
            sVar2 = 3;
            uVar3 = 0x96;
          }
          else {
            if ((int)W_LO16(0x16A28) != 0x10) {
              return 0;
            }
            sVar2 = 1;
            uVar3 = 0x168;
          }
          if ((int)W_HI16(0x16A2C) < 0) {
            W_SET_HI16(0x16A2C, uVar3);
            return sVar2;
          }
          if (sVar2 != (int)W_HI16(0x16A28)) {
            W_SET_HI16(0x16A2C, uVar3);
            return sVar2;
          }
        }
      }
      else if (((int)W_HI16(0x16A2C) < 0) || ((int)W_HI16(0x16A28) != 7)) {
        W_SET_HI16(0x16A2C, 0xb4);
        return 7;
      }
    }
    else if (((int)W_HI16(0x16A2C) < 0) || ((int)W_HI16(0x16A28) != 4)) {
      W_SET_HI16(0x16A2C, 0x168);
      return 4;
    }
  }
  return 0;
}


/* ---- camera_setup_simple ---- */

void camera_setup_simple(undefined4 param_1)

{
  player_model_set_wheel_angle(param_1);
  player_model_smoothed_inputs();
  bicycle_front_rear_ik();
  bicycle_ik_solve();
  camera_dsp_terrain_render();
  return;
}


/* ---- camera_state_reset ---- */

void camera_state_reset(void)

{
  W[0x0CDC] = 0;
  W[0x0CE0] = 0;
  W[0x0CE4] = 0;
  W[0x0CE8] = 0;
  W[0x0CEC] = 0;
  W[0x0CF0] = 0;
  return;
}


/* ---- dsp_cmd_end_frame ---- */

void dsp_cmd_end_frame(void)

{
  *(int32_t*)W[0x0CA4] = 0x8010;
  ((int32_t*)W[0x0CA4])[1] = 0xffffffff;
  W[0x0CA4] = W[0x0CA4] + 2 * 4;
  return;
}


/* ---- dsp_cmd_end_list ---- */

void dsp_cmd_end_list(undefined4 param_1)

{
  *(int32_t*)W[0x0CA4] = 0x8010;
  ((int32_t*)W[0x0CA4])[1] = 3;
  ((int32_t*)W[0x0CA4])[2] = param_1;
  ((int32_t*)W[0x0CA4])[3] = 0xffffffff;
  W[0x0CA4] = W[0x0CA4] + 4 * 4;
  return;
}


/* ---- dsp_cmd_place_object ---- */

void dsp_cmd_place_object(undefined4 param_1,undefined4 param_2,int param_3,int param_4,int param_5)

{
  *(int32_t*)W[0x0CA4] = 0x8000;
  ((int32_t*)W[0x0CA4])[1] = param_1;
  ((int32_t*)W[0x0CA4])[2] = param_2;
  ((int32_t*)W[0x0CA4])[3] = param_3 - W[0x0CDC];
  ((int32_t*)W[0x0CA4])[4] = param_4 - W[0x0CE0];
  ((int32_t*)W[0x0CA4])[5] = param_5 - W[0x0CE4];
  W[0x0CA4] = W[0x0CA4] + 6 * 4;
  return;
}


/* ---- dsp_viewport_setup ---- */

void dsp_viewport_setup(int param_1,undefined4 param_2)

{
  undefined4 *puVar1;
  int iVar2;
  int iVar3;
  
  iVar3 = W[0x0CA0];
  iVar2 = W[0x0CA0] * 0x2000;
  /* Trig-table reads. The original Ghidra idiom dereferenced a 1-byte ROM
   * load (g_sys.rom indexed without &) added to the angle as a host pointer
   * (CLAUDE.md fix #14). Replace with the same direct rom_nr32 form used
   * in scene_node_render: 0x20B002/0x20B004 are the actual sin/cos table
   * locations in ROM. The "+2" of the original maps to base 0x20B004. */
  { uint32_t _a = (uint32_t)(W[0x0CE8] & 0xfffc);
    dsp_w32(0x10004 + 4 * (W[0x0CA0] * 0x2000 + param_1 * 0x20), (int32_t)(vrd16s(0x20B002 + _a)));
    dsp_w32(0x10008 + 4 * (iVar2 + param_1 * 0x20), (int32_t)(vrd16s(0x20B004 + _a))); }
  { uint32_t _a = (uint32_t)(W[0x0CEC] & 0xfffc);
    dsp_w32(0x1000C + 4 * (iVar3 * 0x2000 + param_1 * 0x20), (int32_t)(vrd16s(0x20B002 + _a)));
    dsp_w32(0x10010 + 4 * (iVar3 * 0x2000 + param_1 * 0x20), (int32_t)(vrd16s(0x20B004 + _a))); }
  { uint32_t _a = (uint32_t)(W[0x0CF0] & 0xfffc);
    dsp_w32(0x10014 + 4 * (iVar3 * 0x2000 + param_1 * 0x20), (int32_t)(vrd16s(0x20B002 + _a)));
    dsp_w32(0x10018 + 4 * (iVar3 * 0x2000 + param_1 * 0x20), (int32_t)(vrd16s(0x20B004 + _a))); }
  (void)puVar1;
  dsp_w32(0x1001C + 4 * (iVar3 * 0x2000 + param_1 * 0x20), (int32_t)(param_2));
  dsp_w32(0x10038 + 4 * (W[0x0CA0] * 0x2000 + param_1 * 0x20), (int32_t)(0x780));
  return;
}


/* ---- matrix_rotate_yaw_only ---- */

void matrix_rotate_yaw_only(undefined4 *param_1,int *param_2)

{
  short sVar1;
  short sVar2;
  int iVar3;
  int iVar4;
  
  sVar1 = vrd16s(0x20B004 + ((uint32_t)((*(int16_t*)param_1 + 0xe) & 0x3fff) * 2) * 2);
  sVar2 = vrd16s(0x20B006 + ((uint32_t)((*(int16_t*)param_1 + 0xe) & 0x3fff) * 2) * 2);
  iVar3 = fixed_point_mul_32x32(((int32_t*)(intptr_t)param_1)[2],(int)sVar2);
  iVar4 = fixed_point_mul_32x32(*param_1,(int)sVar1);
  ((int32_t*)(intptr_t)param_2)[2] = iVar3 - iVar4;
  iVar3 = fixed_point_mul_32x32(((int32_t*)(intptr_t)param_1)[2],(int)sVar1);
  iVar4 = fixed_point_mul_32x32(*param_1,(int)sVar2);
  *param_2 = iVar3 + iVar4;
  ((int32_t*)(intptr_t)param_2)[1] = 0;
  return;
}


/* ---- scene_clear_transition ---- */

void scene_clear_transition(void)

{
  W16_SET(0x15F44, 0);     /* ROM 0x00FA6A `clr.w $e15f44.l` */
  return;
}


/* ---- sound_play_or_defer ---- */

/* ROM 0x00F4E0 -- play a sound now, or park its id for sound_deferred_tick if the timer is running.
 * (Ghidra called this `scene_deferred_load_object`; it is the sound system, not a scene.) */
short * sound_play_or_defer(undefined4 param_1)
{
  /* ROM 0x00F4E0. The DEFERRED trigger: same table, but it takes only the
   * table's own constant parameter -- there is no caller parameter here. */
  /* ONE argument, at $4(a7) with no register saved at entry -- so Ghidra's
   * param_1 IS the id, not the high half of a packed pair. The two- and
   * three-argument entry points below are the ones that pack. */
  int id = (int)(int16_t)(uint32_t)param_1;
  snd_trig("sound_play_or_defer", id, -1);
  if ((((W[0x0CBC] & 0xfffffffe) == 2) || ((W[0x0CBC] & 0xfffffffe) == 4)) ||
      (W[0x3FFC] != 0)) {
    if (W_LO16(0x15F54) < 0) {
      int slot = snd_slot(id);
      if (slot >= 0) {
        int p2 = snd_pslot2(id);
        if (p2 >= 0) snd_par_w(p2, (unsigned)snd_pval2(id));
        snd_report("sound_play_or_defer", id, slot, (unsigned)snd_cmd(id), -1);
        snd_cmd_w(slot, (unsigned)snd_cmd(id) | 0x4000u);
      }
    } else {
      W_SET_HI16(0x15F58, id);   /* ROM 0x00F508 */
    }
  }
  return (short *)0;
}


/* ---- scene_init_and_setup ---- */

void scene_init_and_setup(void)

{
  sound_env_fill(0);
  if ((W[0x0CBC] != 1) || (W[0x0CC4] != 0x14)) {
    sound_reset_all();
  }
  if ((W[0x0CBC] == 3) ||
     (((W[0x0CBC] == 10 ||
       ((W[0x0CBC] == 1 && ((vrd16s(0x35B44 + (W[0x0CC4] & 0xfffffffe))) != 0))
       )) && (W[0x3FFC] != 0)))) {
    sound_play(0x02);
    comms_w16(g_sys.commsram, 0x0102, 0x2000);
    comms_w16(g_sys.commsram, 0x0104, 0x20);
    comms_w16(g_sys.commsram, 0x010A, 0x2000);
    comms_w16(g_sys.commsram, 0x010C, 0xff);
    comms_w16(g_sys.commsram, 0x010E, 0x2000);
    comms_w16(g_sys.commsram, 0x0110, 0xff);
    if (W[0x0CBC] == 3) {
      /* ROM 0x0100F0: the id is chosen by the COURSE -- `subq.l #3,d0 ;
       * bne ; moveq #$3e / moveq #$42` with d0 = W[0x0E0C] -- not read out
       * of the deferred-object variable, which is a different thing. */
      sound_play_or_defer(W[0x0E0C] == 3 ? 0x3E : 0x42);
      if (W[0x0E0C] == 3) {
        /* ROM 0x01010C: three `move.w #$40dN, $a0400X.l` -- SIXTEEN-BIT
         * command words, and the 0x4000 bit is the trigger. Stored a byte at
         * a time these kept only 0xd1/0xd2/0xd4 and never triggered. */
        comms_w16(g_sys.commsram, 0x0002, 0x40d1);
        comms_w16(g_sys.commsram, 0x0004, 0x40d2);
        comms_w16(g_sys.commsram, 0x0008, 0x40d4);
      }
      FUN_0000fd86(1);
    }
  }
  /* ROM 0x01012E..0x0101C2. THE SOUND-STATE BLOCK 0xE15F30..0xE15F62 IS
   * SIXTEEN-BIT, field by field -- every store here is `move.w` except the
   * long at 0xE15F30 and the one at 0xE15F2C (`move.l`). Written as whole
   * _W[] slots, a 4-aligned store landed in the LOW half, i.e. on the
   * NEIGHBOURING field (0xE15F54 = 0 cleared the deferred-sound timer at
   * 0xE15F56), and a 2-mod-4 store was never written back to work RAM and
   * was rebuilt from its neighbours' bytes on the next frame. */
  W16_SET(0x15F54, 0);                    /* course-1 stem: "restart theme" flag */
  W16_SET(0x15F50, 0);                    /* course-1 stem: timer */
  W16_SET(0x15F52, 0);                    /* course-1 stem: state */
  W16_SET(0x15F34, 0);                    /* countdown-voice stage */
  W[0x15F30] = (int32_t)W[0x4010];        /* a LONG: `move.l $e0(a3),$e15f30.l` */
  W16_SET(0x15F38, 0x7fff);               /* last timer seconds (0xE00E44) */
  W16_SET(0x15F3A, 0x7fff);               /* last bonus timer (0xE00E48) */
  W16_SET(0x15F36, (short)W[0x0CC0]);
  W16_SET(0x15F5E, (short)W[0x0D10]);     /* last heading */
  W16_SET(0x15F5A, 0);                    /* stem fade-in level */
  W16_SET(0x15F4E, 0);                    /* splash cooldown */
  W16_SET(0x15F4C, 0);                    /* dive counter */
  W16_SET(0x15F48, 0);                    /* sound_play_cooldown timer */
  W16_SET(0x15F4A, 0);                    /* cooldown-sound latch bits */
  W16_SET(0x15F44, 0);                    /* transition flag */
  W16_SET(0x15F5C, 0);                    /* proximity-sound timer */
  W16_SET(0x15F3C, 0);                    /* per-layer contact latch bits */
  W[0x15F2C] = 0;                         /* a LONG: `move.l d0,$e15f2c.l` */
  W16_SET(0x15F46, -1);                   /* sound busy countdown */
  W16_SET(0x15F42, -1);                   /* ambience id now playing */
  W16_SET(0x15F40, -1);                   /* last camera cell */
  return;
}


/* ---- sound_reset_upper ---- */

/* ROM 0x00F8EC -- parameters, then clear only the voices above the 0x356A0 boundary.
 * (Ghidra called this `scene_invalidate_partial`; it is the sound system, not a scene.) */
void sound_reset_upper(void)
{
  /* ROM 0x00F8EC -- parameters, then only the voices ABOVE the 0x356A0
   * boundary, so whatever is playing below it keeps going. */
  int first = vrd16s(0x356A0) + 1;
  if (first < 0) first = 0;
  snd_params_reset();
  { int i; for (i = first; i < 0x20; i++) snd_cmd_w(i, snd_cmd_r(i) & 0x7FFFu); }
}


/* ---- sound_play_p ---- */

/* ROM 0x00F698 -- play a sound with a caller parameter (volume, pitch or variant).
 * (Ghidra called this `scene_load_object`; it is the sound system, not a scene.) */
short * sound_play_p(undefined4 param_1)
{
  /* ROM 0x00F698. PLAY A SOUND: id in the high half, parameter in the low. */
  int id  = (int)(int16_t)((uint32_t)param_1 >> 16);
  int par = (int)(int16_t)((uint32_t)param_1 & 0xFFFF);
  snd_trig("sound_play_p", id, par & 0xFFFF);
  if (((((W[0x0CBC] & 0xfffffffe) == 2) || ((W[0x0CBC] & 0xfffffffe) == 4)) ||
       (W[0x3FFC] != 0)) &&
      (((W[0x0CBC] != 3 || (W[0x0CC0] != 3)) ||
        ((0x37 < W[0x0E44]) && (W[0x0E18] == 0))))) {
    int slot = snd_slot(id);
    if (slot >= 0) {
      snd_par_w(snd_pslot(id), (unsigned)par);
      int p2 = snd_pslot2(id);
      if (p2 >= 0) snd_par_w(p2, (unsigned)snd_pval2(id));
      snd_report("sound_play_p", id, slot, (unsigned)snd_cmd(id), par);
      snd_cmd_w(slot, (unsigned)snd_cmd(id) | 0x4000u);
    }
  }
  return (short *)0;
}


/* ---- scene_node_array_animate ---- */

void scene_node_array_animate(int param_1)

{
  /* THE HIT-REACTION ANIMATION, converted from the M68K at 0x026CDE..0x026E5A.
   *
   * This was the one guard that still fired in a normal run ("[GUARD]
   * scene_node_array_animate(4): body unconverted (anim set @ROM 0x128F20)"),
   * and it is the root cause of register row 114: `camera_update_main` calls it
   * in the `else` of `camera_mode_select()`, i.e. INSTEAD OF the pose/emit
   * dispatch, so on every terrain-contact frame the whole bike+rider chain was
   * absent from the display list. Row 114 mitigated that with a fallback pose;
   * this replaces the mitigation with the real function.
   *
   * `param_1` is an ANIMATION-SET ID, not a pointer -- 0x026D28 is
   * `move.l $12abb0(d0.l*4),d3`, the same table player_model_load_animation
   * uses -- and 0x026D38 then does `movea.l (a0),a1`, a SECOND big-endian
   * indirection. Ghidra collapsed both into `piVar5 = (int *)param_1`, so the
   * first dereference was of ADDRESS 4. Verified: 0x12ABB0[4] = 0x128F20,
   * whose entries are ROM pointers 0x127418, 0x127460, ...; MEM32[0x3765C] =
   * 0x20B002 (the trig base cell); the DOF mask table at 0x37664 is
   * {1,2,4,8,0x10,0x20}.
   *
   * Node layout, 0x80 bytes apart from 0xE0AD84 (so the first node processed
   * is 0xAE04, the rider root -- register row 64):
   *   +0x00 model   +0x04 parent link (i16)   +0x08 DOF mask (i16)
   *   +0x0a i16     +0x0c..0x20 per-DOF stream cursors
   *   +0x24..0x38 the six DOF values   +0x78 transform slot id
   * The first three longs of the keyframe overwrite +0x00/+0x04/+0x08, i.e.
   * the animation set defines the node as well as posing it. */
  uint32_t set, trig, nb, ent, kf;
  int slot, k, guard;
  int32_t *cur;
  intptr_t _entry_cur;
  const uint8_t *lo, *hi;

  W_SET_HI16(0x16A28, (uint16_t)param_1);       /* move.w d0,$e16a28 */
  W[0xEB08] = (int32_t)W[0x0D00] - (int32_t)W[0x0CDC];
  W[0xEB0C] = (int32_t)W[0x0D04] - (int32_t)W[0x0CE0];
  W[0xEB10] = (int32_t)W[0x0D08] - (int32_t)W[0x0CE4];
  W[0xAB7C] = 0;

  /* The table at 0x12ABB0 holds EIGHT valid sets (0..7), each a 17-entry
   * NULL-terminated list; [8] is 0 and [9] is 0x45, not a ROM address. The ROM
   * does not bound the index -- camera_mode_select only ever returns a small
   * one -- but reading [8] or beyond would walk garbage as keyframe pointers. */
  if (param_1 < 0 || param_1 > 7) return;
  set  = (uint32_t)vrd32(0x12ABB0 + (uint32_t)param_1 * 4);
  trig = (uint32_t)vrd32(0x3765C);
  if (set == 0 || set + 4 > (uint32_t)ROM_SIZE) return;

  cur = (int32_t *)W[0x0CA4];
  _entry_cur = (intptr_t)cur;
  lo  = g_sys.dspram; hi = g_sys.dspram + DSPRAM_SIZE;
  if ((const uint8_t *)cur < lo || (const uint8_t *)(cur + 0x40) > hi) return;

  nb   = 0xAD84;
  slot = 10;                                     /* moveq #$a,d6 */
  for (guard = 0; guard < 64; guard++) {
    ent = (uint32_t)vrd32(set);                  /* movea.l (a0),a1 */
    if (ent == 0 || ent + 0x2c > (uint32_t)ROM_SIZE) break;
    nb += 0x80;                                  /* lea $80(a4),a4 -- BEFORE use */
    if (nb + 0x80 >= (uint32_t)WORK_RAM_SIZE) break;

    /* 0x026D3A: three longs define the node (model, parent link, DOF mask) */
    W[nb + 0x00] = (int32_t)vrd32(ent);
    W[nb + 0x04] = (int32_t)vrd32(ent + 4);
    W[nb + 0x08] = (int32_t)vrd32(ent + 8);

    kf = ent + 0x0c;                             /* moveq #$c,d0 ; add.l (a0),d0 */
    if ((short)W_HI16(nb + 0x08) == 0) {
      /* 0x026D52: no animated DOF -- six inline longs */
      for (k = 0; k < 6; k++)
        W[nb + 0x24 + k * 4] = (int32_t)vrd32(kf + (uint32_t)k * 4);
    } else {
      /* 0x026D6A: per DOF, the mask bit picks inline value or STREAM POINTER.
       * For a streamed DOF the keyframe holds a ROM pointer P: the node caches
       * P+4 at +0x0c+4k (the next frame's cursor) and takes MEM32[P] as the
       * value now. */
      int mask = (int)(short)W_HI16(nb + 0x08);
      for (k = 0; k < 6; k++) {
        uint32_t dofbit = (uint32_t)vrd32(0x37664 + (uint32_t)k * 4);
        if ((mask & (int)dofbit) == 0) {
          W[nb + 0x24 + k * 4] = (int32_t)vrd32(kf);
          kf += 4;
        } else {
          uint32_t pp = (uint32_t)vrd32(kf);
          W[nb + 0x0c + k * 4] = (int32_t)(pp + 4);
          W[nb + 0x24 + k * 4] = (pp + 4 <= (uint32_t)ROM_SIZE)
                                 ? (int32_t)vrd32(pp) : 0;
          kf += 4;
        }
      }
    }

    /* ---- the wire format: 0x8008 / 0x8009 / 0x800a, as scene_node_render ---- */
    if ((const uint8_t *)(cur + 0x20) > hi) break;
    *cur++ = 0x8008;
    *cur++ = 3;
    *cur++ = 1;
    for (k = 0; k < 3; k++) {
      /* 0x026DA4: `d1 = (a3)+ & 0xfffc ; d1 += MEM32[0x3765c] ; move.l (a0),(a1)+ ;
       * move.l $2(a0),(a1)+` -- two 32-BIT reads off trigbase-2, register row 87,
       * so the LOW halves are sin@0x20B004+a and cos@0x20B006+a. */
      uint32_t ang = (uint32_t)((int32_t)W[nb + 0x30 + k * 4]) & 0xfffc;
      *cur++ = vrd16s(trig + 2 + ang);
      *cur++ = vrd16s(trig + 4 + ang);
    }
    *cur++ = (int)(short)W_LO16(nb + 0x08);      /* movea.w $a(a4),a0 */
    *cur++ = 0;
    *cur++ = (int32_t)W[nb + 0x24];
    *cur++ = (int32_t)W[nb + 0x28];
    *cur++ = (int32_t)W[nb + 0x2c];
    *cur++ = -1;
    *cur++ = 0x8009;
    *cur++ = 3;
    { /* 0x026E00: parent's slot -- `move.w $4(a4),d0 ; ext.l ; lsl.l #7 ;
       * move.l $78(a4,d0.l),(a1)+`, i.e. the parent node's +0x78 field. */
      int rel = (int)(short)W_HI16(nb + 0x04);
      int32_t pnb = (int32_t)nb + rel * 0x80;
      *cur++ = (pnb >= 0 && (uint32_t)pnb + 0x78 < (uint32_t)WORK_RAM_SIZE)
               ? (int32_t)W[(uint32_t)pnb + 0x78] : 0;
    }
    W[nb + 0x78] = slot;
    *cur++ = slot;
    slot++;
    *cur++ = 0x800A;
    *cur++ = (int32_t)W[nb + 0x00];
    *cur++ = (int32_t)W[nb + 0x78];
    *cur++ = (int32_t)W[0xEB08];
    *cur++ = (int32_t)W[0xEB0C];
    *cur++ = (int32_t)W[0xEB10];
    W[0x0CA4] = (intptr_t)cur;

    set += 4;                                    /* addq.l #4,d3 */
    if (set + 4 > (uint32_t)ROM_SIZE) break;
    if (vrd32(set) == 0) break;                  /* tst.l (a0) ; bne */
  }

  /* PROPCYCL_RIGDUMP=1 reports the emission: measured 16 nodes / 425 words
   * every call, i.e. the full rider chain. */
  { extern int g_rig_dump; static int n;
    if (g_rig_dump && n++ < 8)
      fprintf(stderr, "[SNA] set=%d nodes=%d words=%ld\n", param_1, guard,
              (long)(((intptr_t)cur - _entry_cur) / 4)); }
  nb += 0x80;                                    /* 0x026E40 */
  if (nb < (uint32_t)WORK_RAM_SIZE) W[nb] = -1;
  W_SET_HI16(0xEB04, 0);
  W_SET_HI16(0xEB06, 0);
}



/* ---------------------------------------------------------------------------
 * Node-field access for the two transform_point_*_by_node functions.
 *
 * Callers pass &W[node] and W is intptr_t[] (8 bytes per slot), but the
 * transpiled bodies indexed the parameters as int* (4 bytes). Every field
 * read/write was therefore misaligned by half a slot -- the SAME
 * (&W[base])[N] stride bug documented for scene_node_render. In the _W[]
 * model the slot index IS the byte offset, so field N is W[base + N*4].
 *
 * Measured symptom: rider nodes 9/13/15/17/19/20/22 came out with their X
 * (field 9, +0x24) or Y (field 10, +0x28) off by exactly +-0x800000, i.e.
 * bit 23 of a 24-bit coordinate, because the reads straddled slot
 * boundaries and picked up a neighbouring field's sign bits. Seven of the
 * flyover's 26 objects landed at |t| ~ 8.38 million instead of ~7500.
 *
 * param_1 is the exception: one caller (bicycle_ik_solve) passes a real
 * int32_t[3] of ROM constants, not a W pointer, so it is read through
 * node_in() which handles both. param_2/3/4 are always W pointers.
 * ------------------------------------------------------------------------ */
static inline int32_t node_in(const void *p, int n)
{
    const intptr_t *w = (const intptr_t *)p;
    if (w >= _W && w < _W + WORK_RAM_SIZE)
        return (int32_t)_W[(w - _W) + n * 4];    /* slot index == byte offset */
    return ((const int32_t *)p)[n];              /* a genuine int32 buffer */
}
static inline int node_is_w(const void *p)
{
    const intptr_t *w = (const intptr_t *)p;
    return w >= _W && w < _W + WORK_RAM_SIZE;
}

/* ---- transform_point_by_node ---- */

void transform_point_by_node(undefined4 *param_1,int *param_2,int *param_3)

{
  undefined4 uVar1;
  undefined4 uVar2;
  short sVar3;
  int32_t psVar4;
  short sVar5;
  short sVar6;
  int32_t psVar7;
  int32_t psVar8;
  if (!node_is_w(param_2) || !node_is_w(param_3)) return;
  const intptr_t n2 = (intptr_t *)param_2 - _W;
  const intptr_t n3 = (intptr_t *)param_3 - _W;

  psVar4 = vrd16s(0x20B006 + (W[n2 + 12] & 0xfffcU));
  uVar1 = node_in(param_1, 1);
  sVar5 = psVar4;
  uVar2 = node_in(param_1, 2);
  psVar7 = vrd16s(0x20B004 + (W[n2 + 12] & 0xfffcU));
  sVar3 = psVar7;
  psVar8 = vrd16s(0x20B006 + (W[n2 + 16] & 0xfffcU));
  sVar6 = (short)((int)(short)node_in(param_1, 2) * (int)psVar4 + (int)(short)node_in(param_1, 1) * (int)psVar7 >>
                 0xf);
  psVar7 = vrd16s(0x20B004 + (W[n2 + 16] & 0xfffcU));
  W[n3 + 8] = (int32_t)W[n2 + 8] + ((int)sVar6 * (int)psVar8 - (int)(short)node_in(param_1, 0) * (int)psVar7 >> 0xf)
  ;
  psVar4 = vrd16s(0x20B006 + (W[n2 + 20] & 0xfffcU));
  sVar6 = (short)((int)(short)node_in(param_1, 0) * (int)psVar8 + (int)sVar6 * (int)psVar7 >> 0xf);
  psVar8 = vrd16s(0x20B004 + (W[n2 + 20] & 0xfffcU));
  sVar5 = (short)((int)(short)uVar1 * (int)sVar5 - (int)(short)uVar2 * (int)sVar3 >> 0xf);
  W[n3 + 0] = (int32_t)W[n2 + 0] + ((int)sVar6 * (int)psVar4 - (int)sVar5 * (int)psVar8 >> 0xf);
  W[n3 + 4] = (int32_t)W[n2 + 4] + ((int)sVar5 * (int)psVar4 + (int)sVar6 * (int)psVar8 >> 0xf);
  return;
}


/* ---- transform_point_pair_by_node ---- */

void transform_point_pair_by_node(undefined4 *param_1,int *param_2,int *param_3,int *param_4)

{
  undefined4 uVar1;
  undefined4 uVar2;
  int32_t psVar3;
  int iVar4;
  uint32_t uVar5;
  int iVar6;
  short sVar7;
  short sVar8;
  int iVar9;
  short sVar10;
  int32_t psVar11;
  int32_t psVar12;
  if (!node_is_w(param_2) || !node_is_w(param_3) || !node_is_w(param_4)) return;
  const intptr_t n2 = (intptr_t *)param_2 - _W;
  const intptr_t n3 = (intptr_t *)param_3 - _W;
  const intptr_t n4 = (intptr_t *)param_4 - _W;

  psVar12 = vrd16s(0x20B006 + (W[n2 + 12] & 0xfffcU));
  uVar1 = node_in(param_1, 1);
  sVar7 = psVar12;
  uVar2 = node_in(param_1, 2);
  psVar11 = vrd16s(0x20B004 + (W[n2 + 12] & 0xfffcU));
  sVar8 = psVar11;
  psVar3 = vrd16s(0x20B006 + (W[n2 + 16] & 0xfffcU));
  sVar10 = (short)((int)(short)node_in(param_1, 2) * (int)psVar12 + (int)(short)node_in(param_1, 1) * (int)psVar11
                  >> 0xf);
  iVar4 = (int32_t)W[n2 + 8] + ((int)sVar10 * (int)psVar3 >> 0xf);
  psVar12 = vrd16s(0x20B004 + (W[n2 + 16] & 0xfffcU));
  iVar6 = (int)(short)node_in(param_1, 0) * (int)psVar12 >> 0xf;
  W[n3 + 8] = iVar4;
  W[n3 + 8] = (int32_t)W[n3 + 8] - iVar6;
  W[n4 + 8] = iVar6 + iVar4;
  iVar9 = (int)sVar10 * (int)psVar12;
  iVar6 = (int)(short)node_in(param_1, 0) * (int)psVar3;
  uVar5 = (int32_t)W[n2 + 20] & 0xfffc;
  psVar12 = vrd16s(0x20B006 + uVar5);
  sVar10 = (short)((int)(short)uVar1 * (int)sVar7 - (int)(short)uVar2 * (int)sVar8 >> 0xf);
  iVar4 = ((int)sVar10 * (int)(vrd16s(0x20B004 + uVar5)) >> 0xf) - (int32_t)W[n2 + 0];
  sVar7 = (short)(iVar6 + iVar9 >> 0xf);
  W[n3 + 0] = ((int)sVar7 * (int)psVar12 >> 0xf) - iVar4;
  sVar8 = (short)(iVar9 - iVar6 >> 0xf);
  W[n4 + 0] = ((int)sVar8 * (int)psVar12 >> 0xf) - iVar4;
  iVar4 = (int32_t)W[n2 + 4] + ((int)sVar10 * (int)psVar12 >> 0xf);
  W[n3 + 4] = iVar4 + ((int)sVar7 * (int)(vrd16s(0x20B004 + uVar5)) >> 0xf);
  W[n4 + 4] = iVar4 + ((int)sVar8 * (int)(vrd16s(0x20B004 + uVar5)) >> 0xf);
  return;
}


/* ---- dsp_cmd_place_object_rotated ---- */

void dsp_cmd_place_object_rotated
               (undefined4 param_1,undefined4 param_2,int param_3,int param_4,int param_5,
               uint32_t param_6,uint32_t param_7,uint32_t param_8,undefined4 param_9)

{
  *(int32_t*)W[0x0CA4] = 0x8002;
  ((int32_t*)W[0x0CA4])[1] = param_1;
  ((int32_t*)W[0x0CA4])[2] = param_2;
  ((int32_t*)W[0x0CA4])[3] = param_3 - W[0x0CDC];
  ((int32_t*)W[0x0CA4])[4] = param_4 - W[0x0CE0];
  ((int32_t*)W[0x0CA4])[5] = param_5 - W[0x0CE4];
  ((int32_t*)W[0x0CA4])[6] = (int)(int16_t)vrd16s(0x20B004 + (param_6 >> 1 & 0x7ffe) * 2);
  ((int32_t*)W[0x0CA4])[7] = (int)(int16_t)vrd16s(0x20B006 + (param_6 >> 1 & 0x7ffe) * 2);
  ((int32_t*)W[0x0CA4])[8] = (int)(int16_t)vrd16s(0x20B004 + (param_7 >> 1 & 0x7ffe) * 2);
  ((int32_t*)W[0x0CA4])[9] = (int)(int16_t)vrd16s(0x20B006 + (param_7 >> 1 & 0x7ffe) * 2);
  ((int32_t*)W[0x0CA4])[10] = (int)(int16_t)vrd16s(0x20B004 + (param_8 >> 1 & 0x7ffe) * 2);
  ((int32_t*)W[0x0CA4])[0xb] = (int)(int16_t)vrd16s(0x20B006 + (param_8 >> 1 & 0x7ffe) * 2);
  ((int32_t*)W[0x0CA4])[0xc] = param_9;
  W[0x0CA4] = W[0x0CA4] + 0xd * 4;
  return;
}


/* ---- rotate_euler_yxz_optimized ---- */

void rotate_euler_yxz_optimized
               (int param_1,int param_2,int param_3,uint32_t param_4,uint32_t param_5,uint32_t param_6,
               int *param_7,int *param_8,int *param_9)

{
  short sVar1;
  short sVar2;
  short sVar3;
  short sVar4;
  int iVar5;
  int iVar6;
  int iVar7;
  
  iVar5 = param_1 * (int16_t)vrd16s(0x20B006 + (param_5 >> 1 & 0x7ffe) * 2) -
          param_3 * (int16_t)vrd16s(0x20B004 + (param_5 >> 1 & 0x7ffe) * 2) >> 0xf;
  iVar6 = param_3 * (int16_t)vrd16s(0x20B006 + (param_5 >> 1 & 0x7ffe) * 2) +
          param_1 * (int16_t)vrd16s(0x20B004 + (param_5 >> 1 & 0x7ffe) * 2) >> 0xf;
  iVar7 = param_2 * (int16_t)vrd16s(0x20B006 + (param_6 >> 1 & 0x7ffe) * 2) -
          iVar5 * (int16_t)vrd16s(0x20B004 + (param_6 >> 1 & 0x7ffe) * 2) >> 0xf;
  sVar1 = vrd16s(0x20B006 + (param_4 >> 1 & 0x7ffe) * 2);
  sVar2 = vrd16s(0x20B004 + (param_4 >> 1 & 0x7ffe) * 2);
  sVar3 = vrd16s(0x20B004 + (param_4 >> 1 & 0x7ffe) * 2);
  sVar4 = vrd16s(0x20B006 + (param_4 >> 1 & 0x7ffe) * 2);
  *param_7 = iVar5 * (int16_t)vrd16s(0x20B006 + (param_6 >> 1 & 0x7ffe) * 2) +
             param_2 * (int16_t)vrd16s(0x20B004 + (param_6 >> 1 & 0x7ffe) * 2) >> 0xf;
  *param_8 = iVar7 * sVar4 + iVar6 * sVar3 >> 0xf;
  *param_9 = iVar6 * sVar1 - iVar7 * sVar2 >> 0xf;
  return;
}


/* ---- rotate_euler_zxy_optimized ---- */

void rotate_euler_zxy_optimized
               (int param_1,int param_2,int param_3,uint32_t param_4,uint32_t param_5,uint32_t param_6,
               int *param_7,int *param_8,int *param_9)

{
  /* Trig table: 16-bit big-endian Q15, entries every 4 bytes.
   *   cos(angle) = vrd16s(0x20B002 + (angle & 0xfffc))
   *   sin(angle) = vrd16s(0x20B004 + (angle & 0xfffc))
   *
   * This used index (angle>>1)&0x7ffe with bases 0x20B004/0x20B006 -- HALF
   * the stride, and the sin/cos roles swapped. Measured against real
   * sin/cos for heading 53172 (true sin,cos = -30363, 12318):
   *     old form -> (-27175, 18298)   wrong
   *     this form -> (-30363, 12318)  correct
   * The magnitude of the rotated vector was already right (|off| = 9399,
   * matching the recording's constant camera-to-bike distance of 9400),
   * but the DIRECTION was wrong -- a near-zero pitch read as -90 degrees,
   * putting the camera directly overhead instead of behind the bike.
   * Same convention dsp_viewport_setup uses (0x20B002 then 0x20B004). */
  /* COS IS 0x20B006, NOT THE LAGGING 0x20B002 (2026-09-22). ROM 0x021744:
   * `movea.l #$20b004,a1`, and every cos read in the function is
   * `move.w $2(a1,dN.l)` (0x021758, 0x0217C6, 0x021814, 0x021832, ...), the
   * sin reads `move.w (a1,dN.l)`. The lagging lane put the bad ending's
   * chase camera 2..21 units off MAME's (0xE00CDC at local 199: ours
   * 254465, MAME 254444); with 0x20B006 they agree. */
  int cos6 = vrd16s(0x20B006 + (param_6 & 0xfffc));
  int sin6 = vrd16s(0x20B004 + (param_6 & 0xfffc));
  int cos4 = vrd16s(0x20B006 + (param_4 & 0xfffc));
  int sin4 = vrd16s(0x20B004 + (param_4 & 0xfffc));
  int cos5 = vrd16s(0x20B006 + (param_5 & 0xfffc));
  int sin5 = vrd16s(0x20B004 + (param_5 & 0xfffc));
  int iVar5 = (param_2 * cos6 - param_1 * sin6) >> 0xf;
  int iVar6 = (param_1 * cos6 + param_2 * sin6) >> 0xf;
  int iVar7 = (param_3 * cos4 - iVar5 * sin4) >> 0xf;
  *param_7 = (iVar6 * cos5 - iVar7 * sin5) >> 0xf;
  *param_8 = (iVar5 * cos4 + param_3 * sin4) >> 0xf;
  *param_9 = (iVar7 * cos5 + iVar6 * sin5) >> 0xf;
  return;
}


/* ---- rotate_vector_euler_xyz ---- */

void rotate_vector_euler_xyz
               (int param_1,int param_2,int param_3,uint32_t param_4,uint32_t param_5,uint32_t param_6,
               int *param_7,int *param_8,int *param_9)

{
  uint32_t uVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  
  iVar2 = param_3;
  if ((short)param_4 != 0) {
    uVar1 = (param_4 >> 1 & 0x7ffe) * 2;
    iVar2 = param_3 * (int16_t)vrd16s(0x20B006 + (uVar1)) - param_2 * (int16_t)vrd16s(0x20B004 + (uVar1)) >> 0xf
    ;
    param_2 = param_2 * (int16_t)vrd16s(0x20B006 + (param_4 >> 1 & 0x7ffe) * 2) +
              param_3 * (int16_t)vrd16s(0x20B004 + (uVar1)) >> 0xf;
  }
  iVar4 = param_1;
  if ((short)param_5 != 0) {
    uVar1 = (param_5 >> 1 & 0x7ffe) * 2;
    iVar4 = param_1 * (int16_t)vrd16s(0x20B006 + (uVar1)) - iVar2 * (int16_t)vrd16s(0x20B004 + (uVar1)) >> 0xf;
    iVar2 = iVar2 * (int16_t)vrd16s(0x20B006 + (param_5 >> 1 & 0x7ffe) * 2) +
            param_1 * (int16_t)vrd16s(0x20B004 + (uVar1)) >> 0xf;
  }
  iVar3 = param_2;
  if ((short)param_6 != 0) {
    uVar1 = (param_6 >> 1 & 0x7ffe) * 2;
    iVar3 = param_2 * (int16_t)vrd16s(0x20B006 + (uVar1)) - iVar4 * (int16_t)vrd16s(0x20B004 + (uVar1)) >> 0xf;
    iVar4 = iVar4 * (int16_t)vrd16s(0x20B006 + (param_6 >> 1 & 0x7ffe) * 2) +
            param_2 * (int16_t)vrd16s(0x20B004 + (uVar1)) >> 0xf;
  }
  *param_7 = iVar4;
  *param_8 = iVar3;
  *param_9 = iVar2;
  return;
}


/* ---- camera_update_wrapper ---- */

void camera_update_wrapper(void)

{
  camera_update();
  return;
}

/* ---- dsp_cmd_emit_object_mode_8000 ---- */

void dsp_cmd_emit_object_mode_8000(void)

{
  undefined4 *puVar1;
  undefined4 uVar2;
  
  if (W[0x169E8] != 0x8000) {
    W[0x169E8] = 0x8000;
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = 0x8000;
    W[0x0CA4] = puVar1;
    if (W[0x0CC0] == 0xf) {
      uVar2 = 3;
    }
    else {
      uVar2 = 0;
    }
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = uVar2;
    W[0x0CA4] = puVar1;
  }
  return;
}

/* ---- dsp_cmd_object_param ---- */

void dsp_cmd_object_param(undefined4 param_1,undefined4 param_2,undefined4 param_3)

{
  *(int32_t*)W[0x0CA4] = 0x8009;
  ((int32_t*)W[0x0CA4])[1] = param_1;
  ((int32_t*)W[0x0CA4])[2] = param_2;
  ((int32_t*)W[0x0CA4])[3] = param_3;
  W[0x0CA4] = W[0x0CA4] + 4 * 4;
  return;
}

/* ---- dsp_cmd_object_transform ---- */

void dsp_cmd_object_transform
               (undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
               uint32_t param_5,uint32_t param_6,uint32_t param_7,undefined4 param_8)

{
  /* Trig table: interleaved [sin, cos] on a 4-byte stride, big-endian Q15,
   * sin at 0x20B004 and cos at 0x20B006, indexed by a 4-ALIGNED BYTE OFFSET.
   *
   * This used to index at HALF stride (mismatch-register row 39). The M68K at
   * 0x0220E8 settles it -- the doubling is right there and the transpilation
   * dropped it:
   *
   *   0220EA: movea.l #$20b004,A3
   *   022104: move.l  ($18,A7),D0     ; the angle
   *   022108: lsr.l   #1,D0           ; a >> 1
   *   02210A: andi.l  #$7ffe,D0       ; & 0x7ffe
   *   022110: add.l   D0,D0           ; * 2      <-- was missing here
   *   022112: movea.w (A3,D0.l),A0    ; sin
   *   022126: movea.w ($2,A3,D0.l),A0 ; cos
   *
   * and ((a >> 1) & 0x7ffe) * 2 == a & 0xfffc, which is the canonical form in
   * CLAUDE.md's "Trig table -- the definitive layout". `movea.w` sign-extends,
   * so the stored word is signed -- vrd16s. */
  *(int32_t*)W[0x0CA4] = 0x8008;
  ((int32_t*)W[0x0CA4])[1] = param_1;
  ((int32_t*)W[0x0CA4])[2] = 1;
  ((int32_t*)W[0x0CA4])[3] = (int)vrd16s(0x20B004 + (param_5 & 0xfffc));
  ((int32_t*)W[0x0CA4])[4] = (int)vrd16s(0x20B006 + (param_5 & 0xfffc));
  ((int32_t*)W[0x0CA4])[5] = (int)vrd16s(0x20B004 + (param_6 & 0xfffc));
  ((int32_t*)W[0x0CA4])[6] = (int)vrd16s(0x20B006 + (param_6 & 0xfffc));
  ((int32_t*)W[0x0CA4])[7] = (int)vrd16s(0x20B004 + (param_7 & 0xfffc));
  ((int32_t*)W[0x0CA4])[8] = (int)vrd16s(0x20B006 + (param_7 & 0xfffc));
  ((int32_t*)W[0x0CA4])[9] = param_8;
  ((int32_t*)W[0x0CA4])[10] = 0;
  ((int32_t*)W[0x0CA4])[0xb] = param_2;
  ((int32_t*)W[0x0CA4])[0xc] = param_3;
  ((int32_t*)W[0x0CA4])[0xd] = param_4;
  ((int32_t*)W[0x0CA4])[0xe] = 0xffffffff;
  W[0x0CA4] = W[0x0CA4] + 0xf * 4;
  return;
}

/* ---- dsp_cmd_set_matrix ---- */

void dsp_cmd_set_matrix(undefined4 param_1)

{
  *(int32_t*)W[0x0CA4] = 0x8002;
  ((int32_t*)W[0x0CA4])[1] = param_1;
  W[0x0CA4] = W[0x0CA4] + 2 * 4;
  return;
}

/* ---- dsp_cmd_set_velocity ---- */

int g_rig_split = 0;      /* PROPCYCL_RIG_SPLIT=1 -> row 64's arrangement */

void dsp_cmd_set_velocity
               (undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
               undefined4 param_5)

{
  /* With the whole chain walked in gameplay the six bike parts come from the
   * rig, so player_render's own placements of the same models would draw a
   * second bike (register row 64). Suppress those, not the chain: the models
   * are 0x45..0x4a and their -0x19 variants 0x2c..0x31. */
  if (!g_rig_split && W[0x0CBC] == 3 &&
      (((int32_t)param_1 >= 0x45 && (int32_t)param_1 <= 0x4a) ||
       ((int32_t)param_1 >= 0x2c && (int32_t)param_1 <= 0x31)))
      return;

  *(int32_t*)W[0x0CA4] = 0x800a;
  ((int32_t*)W[0x0CA4])[1] = param_1;
  ((int32_t*)W[0x0CA4])[2] = param_2;
  ((int32_t*)W[0x0CA4])[3] = param_3;
  ((int32_t*)W[0x0CA4])[4] = param_4;
  ((int32_t*)W[0x0CA4])[5] = param_5;
  W[0x0CA4] = W[0x0CA4] + 6 * 4;
  return;
}

/* ---- dsp_emit_articulated_model ---- */

void dsp_emit_articulated_model(intptr_t param_1,intptr_t param_2)

{
  /* ROM 0x016CA8 -- attach node param_1 to its PARENT's transform slot,
   * scaled: a 15-word 0x8008 (slot 4, mode 1) carrying the node's three
   * angles (X NEGATED, Y + 0x8000) as sin-first pairs -- `move.l (a3) ;
   * move.l $2(a3)` off 0x20B002, row 87 -- its +0x0A word and its position
   * * scale >> 15, then `0x8009, 4, <parent slot>, 4`, the parent being
   * node + (sext W16(node+4))*0x80. The node is a _W[] byte offset (or a
   * pointer into _W / a 68K address, see anim_w_off); the transpile read it
   * as a host-packed struct, cos-first. */
  intptr_t n = anim_w_off((const void *)param_1);
  int32_t sc = (int32_t)param_2;
  int32_t *cp = (int32_t *)W[0x0CA4];
  uint32_t a;
  if (n < 0 || n + 0x80 > WORK_RAM_SIZE) return;
  *cp++ = 0x8008; *cp++ = 4; *cp++ = 1;
  a = (uint32_t)(-(int32_t)W[n + 0x30]) & 0xfffc;
  *cp++ = vrd16s(0x20B004 + a); *cp++ = vrd16s(0x20B006 + a);
  a = (uint32_t)((int32_t)W[n + 0x34] + 0x8000) & 0xfffc;
  *cp++ = vrd16s(0x20B004 + a); *cp++ = vrd16s(0x20B006 + a);
  a = (uint32_t)W[n + 0x38] & 0xfffc;
  *cp++ = vrd16s(0x20B004 + a); *cp++ = vrd16s(0x20B006 + a);
  *cp++ = W_LO16(n + 8);                              /* movea.w $a(a0) */
  *cp++ = 0;
  *cp++ = (int32_t)(((int64_t)(int32_t)W[n + 0x24] * sc) >> 15);
  *cp++ = (int32_t)(((int64_t)(int32_t)W[n + 0x28] * sc) >> 15);
  *cp++ = (int32_t)(((int64_t)(int32_t)W[n + 0x2C] * sc) >> 15);
  *cp++ = -1;
  *cp++ = 0x8009; *cp++ = 4;
  { intptr_t par = n + (intptr_t)W_HI16(n + 4) * 0x80;
    *cp++ = (par >= 0 && par + 0x80 <= WORK_RAM_SIZE) ? (int32_t)W[par + 0x78] : 0; }
  *cp++ = 4;
  W[0x0CA4] = (intptr_t)cp;
  return;
}

/* ---- scene_calc_3d_distance ---- */

int scene_calc_3d_distance(int param_1,int param_2,int param_3)

{
  int iVar1;
  uint32_t uVar2;
  
  uVar2 = math_atan2(param_1,param_3);
  iVar1 = (int)(vrd16s(0x20B006 + (uVar2 & 0xfffc))) >> 5;
  if (iVar1 != 0) {
    param_1 = (param_3 << 10) / iVar1;
  }
  if (param_1 < 0) {
    param_1 = -param_1;
  }
  uVar2 = math_atan2(param_2,param_1);
  iVar1 = (int)(vrd16s(0x20B006 + (uVar2 & 0xfffc))) >> 5;
  if (iVar1 != 0) {
    param_2 = (param_1 << 10) / iVar1;
  }
  if (param_2 < 0) {
    param_2 = -param_2;
  }
  return param_2;
}

/* ---- sound_stop ---- */

/* ROM 0x00F7EC -- clear bit 15 of one command word -- stop that voice.
 * (Ghidra called this `scene_deactivate_object`; it is the sound system, not a scene.) */
void sound_stop(undefined4 param_1)
{
  /* ROM 0x00F7EC -- STOP A SOUND: `andi.w #$7fff` on its command word. */
  /* ONE argument, at $4(a7) with no register saved at entry -- so Ghidra's
   * param_1 IS the id, not the high half of a packed pair. The two- and
   * three-argument entry points below are the ones that pack. */
  int id = (int)(int16_t)(uint32_t)param_1;
  snd_trig("sound_stop", id, -1);
  int slot = snd_slot(id);
  snd_report("sound_stop", id, slot, snd_cmd_r(slot), -1);
  snd_cmd_w(slot, snd_cmd_r(slot) & 0x7FFFu);
}

/* ---- sound_play_ungated ---- */

/* ROM 0x00F486 -- as sound_play but without the game-state gates.
 * (Ghidra called this `scene_force_load_object`; it is the sound system, not a scene.) */
void sound_play_ungated(undefined4 param_1)

{
  /* ROM 0x00F486..0x00F4DE, the ungated twin of sound_play: no state gate,
   * otherwise the same table walk -- `tst.w (a1) ; bmi` on the slot, the
   * second parameter from +8/+10, `ori.w #$4000` on the command, and the
   * priority at +6 raised into the 16-bit busy word 0xE15F46.
   *
   * This read the table ONE BYTE at a time (`(&R[0x355A4])[id*6]`, a byte
   * index where the ROM entry is 12 bytes of BE16 fields), wrote the MCU
   * mailbox a byte at a time at a byte index (register row 141's defect,
   * which the rest of the cluster had already shed), and read the priority
   * native-endian. Every caller had also lost its argument: the three calls
   * in stage_camera_path_update are the level-countdown voices 0x0C, 0x12
   * and 0x13 (ROM 0x010268, 0x010334, 0x010306). */
  int id = (int)(int16_t)(uint32_t)param_1;
  snd_trig("sound_play_ungated", id, -1);
  if (id < 0 || id > 0x58) return;        /* the table's -1 row is 0x59 */
  {
    int slot = snd_slot(id);
    if (slot < 0) return;
    int p2 = snd_pslot2(id);
    if (p2 >= 0) snd_par_w(p2, (unsigned)snd_pval2(id));
    snd_report("sound_play_ungated", id, slot, (unsigned)snd_cmd(id), -1);
    snd_cmd_w(slot, (unsigned)snd_cmd(id) | 0x4000u);
    {
      int pri = vrd16s(0x355AA + id * 12);
      if (pri > W16(0x15F46)) W16_SET(0x15F46, pri);
    }
  }
}

/* ---- sound_reset_all ---- */

/* ROM 0x00F894 -- parameters to 0xFF, then clear bit 15 on all 32 command words.
 * (Ghidra called this `scene_invalidate_and_deactivate`; it is the sound system, not a scene.) */
void sound_reset_all(void)
{
  /* ROM 0x00F894. `cmpa.w $356a0.l, a0` -- a BE16 read, not the (ADDR)*4
   * artifact the old code had. It splits the 32 voices at that boundary and
   * clears both halves, which is one loop here. */
  snd_params_reset();
  { int i; for (i = 0; i < 0x20; i++) snd_cmd_w(i, snd_cmd_r(i) & 0x7FFFu); }
}

/* ---- sound_play_p_ungated ---- */

/* ROM 0x00F72C -- as sound_play_p but without the game-state gates.
 * (Ghidra called this `scene_load_object_vis`; it is the sound system, not a scene.) */
void sound_play_p_ungated(undefined4 param_1)
{
  /* ROM 0x00F72C. As sound_play_p but with no state gate. */
  int id  = (int)(int16_t)((uint32_t)param_1 >> 16);
  int par = (int)(int16_t)((uint32_t)param_1 & 0xFFFF);
  snd_trig("sound_play_p_ungated", id, par & 0xFFFF);
  int slot = snd_slot(id);
  if (slot >= 0) {
    snd_par_w(snd_pslot(id), (unsigned)par);
    int p2 = snd_pslot2(id);
    if (p2 >= 0) snd_par_w(p2, (unsigned)snd_pval2(id));
    snd_report("sound_play_p_ungated", id, slot, (unsigned)snd_cmd(id), par);
    snd_cmd_w(slot, (unsigned)snd_cmd(id) | 0x4000u);
  }
}

/* ---- scene_node_render_scaled ---- */

void scene_node_render_scaled(undefined4 *param_1,int param_2)

{
  /* ROM 0x02638A -- draw one node with a uniform scale, three ways by its
   * parent link (the HIGH word of +0x04):
   *   < 0  scale in slot 4 (a 13-word 0x8008 mode 6 diagonal), then the
   *        node's own rotation/position in slot 3, chained 3 -> 4 and to
   *        the PARENT's slot, and the model drawn in the node's slot;
   *   > 0  a root-less 0x800? data entry: model, camera-relative position,
   *        two scaled sin/cos pairs (`move.w $2/$4(a3)` * scale >> 14);
   *   = 0  scale in slot 3, own transform in the node's slot, drawn there
   *        at the camera-relative position (also stored to 0xE0EB08..).
   * Pairs are sin-first off the base held at 0x3765C (= 0x20B002). The node
   * is a _W[] byte offset / pointer into _W; the transpile read it
   * host-packed and cos-first. */
  intptr_t n = anim_w_off((const void *)param_1);
  int32_t sc = param_2;
  int32_t *cp = (int32_t *)W[0x0CA4];
  uint32_t a;
  int16_t link;
  int k;
  if (n < 0 || n + 0x80 > WORK_RAM_SIZE) return;
  link = W_HI16(n + 4);
  if (link < 0) {                                              /* 0x0263AC */
    *cp++ = 0x8008; *cp++ = 4; *cp++ = 6;
    *cp++ = sc; *cp++ = 0; *cp++ = 0; *cp++ = 0; *cp++ = sc; *cp++ = 0; *cp++ = 0; *cp++ = 0; *cp++ = sc;
    *cp++ = -1;
    *cp++ = 0x8008; *cp++ = 3; *cp++ = 1;
    for (k = 0; k < 3; k++) {
      a = (uint32_t)W[n + 0x30 + k * 4] & 0xfffc;
      *cp++ = vrd16s(0x20B004 + a); *cp++ = vrd16s(0x20B006 + a);
    }
    *cp++ = W_LO16(n + 8);
    *cp++ = 0;
    *cp++ = (int32_t)W[n + 0x24]; *cp++ = (int32_t)W[n + 0x28]; *cp++ = (int32_t)W[n + 0x2C];
    *cp++ = -1;
    *cp++ = 0x8009; *cp++ = 3; *cp++ = 4; *cp++ = 3;
    *cp++ = 0x8009; *cp++ = 3;
    { intptr_t par = n + (intptr_t)link * 0x80;
      *cp++ = (par >= 0 && par + 0x80 <= WORK_RAM_SIZE) ? (int32_t)W[par + 0x78] : 0; }
    *cp++ = (int32_t)W[n + 0x78];
    *cp++ = 0x800a; *cp++ = (int32_t)W[n]; *cp++ = (int32_t)W[n + 0x78];
    *cp++ = (int32_t)W[0xEB08]; *cp++ = (int32_t)W[0xEB0C]; *cp++ = (int32_t)W[0xEB10];
  } else if (link > 0) {                                       /* 0x026488 */
    *cp++ = (int32_t)W[n];
    *cp++ = (int32_t)W[n + 0x24] - (int32_t)W[0x0CDC];
    *cp++ = (int32_t)W[n + 0x28] - (int32_t)W[0x0CE0];
    *cp++ = (int32_t)W[n + 0x2C] - (int32_t)W[0x0CE4];
    a = (uint32_t)W[n + 0x30] & 0xfffc;
    *cp++ = (int32_t)(((int64_t)vrd16s(0x20B004 + a) * sc) >> 14);
    *cp++ = (int32_t)(((int64_t)vrd16s(0x20B006 + a) * sc) >> 14);
    /* 0x0264DA: the +0x34 angle is read and then overwritten by +0x38's */
    a = (uint32_t)W[n + 0x38] & 0xfffc;
    *cp++ = (int32_t)(((int64_t)vrd16s(0x20B004 + a) * sc) >> 14);
    *cp++ = (int32_t)(((int64_t)vrd16s(0x20B006 + a) * sc) >> 14);
    *cp++ = W_LO16(n + 8);
  } else {                                                     /* 0x026518 */
    *cp++ = 0x8008; *cp++ = 3; *cp++ = 6;
    *cp++ = sc; *cp++ = 0; *cp++ = 0; *cp++ = 0; *cp++ = sc; *cp++ = 0; *cp++ = 0; *cp++ = 0; *cp++ = sc;
    *cp++ = -1;
    *cp++ = 0x8008; *cp++ = (int32_t)W[n + 0x78]; *cp++ = 1;
    for (k = 0; k < 3; k++) {
      a = (uint32_t)W[n + 0x30 + k * 4] & 0xfffc;
      *cp++ = vrd16s(0x20B004 + a); *cp++ = vrd16s(0x20B006 + a);
    }
    *cp++ = W_LO16(n + 8);
    *cp++ = -1;
    *cp++ = 0x8009; *cp++ = (int32_t)W[n + 0x78]; *cp++ = 3; *cp++ = (int32_t)W[n + 0x78];
    *cp++ = 0x800a; *cp++ = (int32_t)W[n]; *cp++ = (int32_t)W[n + 0x78];
    W[0xEB08] = (int32_t)W[n + 0x24] - (int32_t)W[0x0CDC]; *cp++ = (int32_t)W[0xEB08];
    W[0xEB0C] = (int32_t)W[n + 0x28] - (int32_t)W[0x0CE0]; *cp++ = (int32_t)W[0xEB0C];
    W[0xEB10] = (int32_t)W[n + 0x2C] - (int32_t)W[0x0CE4]; *cp++ = (int32_t)W[0xEB10];
  }
  W[0x0CA4] = (intptr_t)cp;
  return;
}

/* ---- sound_env_fill ---- */

/* ROM 0x00F932 -- fill the four environment words at 0xA0BD58 with one value.
 * (Ghidra called this `scene_set_audio_params`; it is the sound system, not a scene.) */
int sound_env_fill(undefined4 param_1)
{
  /* ROM 0x00F932 -- four 16-bit words at 0xA0BD58, all set to the argument. */
  { int i; for (i = 0; i < 4; i++)
      comms_w16(g_sys.commsram, 0xBD58 + i * 2, (unsigned)(uint16_t)(uint32_t)param_1); }
  return 0;
}

/* ---- sound_set_volume ---- */

/* ROM 0x00F98A -- set one parameter slot to a level, biased by the global fade and clamped.
 * (Ghidra called this `scene_set_object_visibility`; it is the sound system, not a scene.) */
void sound_set_volume(uint32_t param_1)
{
  /* ROM 0x00F98A -- SET A VOLUME. The parameter slot is the high half, the
   * level the low; the level is biased by the global fade and clamped to
   * 0..0xFF. (`cmpi.w #$ff / ble` then `tst.w / bpl`, in that order.) */
  int slot = (int)(int16_t)(param_1 >> 16);
  int v = (int)(int16_t)(uint16_t)param_1 + (int)(int16_t)(uint32_t)g_scene_fade_level;
  if (v > 0xff) v = 0xff;
  if (v < 0)    v = 0;
  snd_par_w(slot, (unsigned)v);
}

/* ---- camera_lookat_interpolate ---- */

/* ROM 0x0128DE. `pos` is a 3-long world position (the ROM passes its
 * address: 0xE00D00, or a stack vector). The transpile read it as `int *`
 * straight into _W[] and let a zero `sin >> 4` divisor raise SIGFPE where the
 * 68K's zero-divide vector is a bare rte (register row 185). */
void camera_lookat_interpolate(int param_1,const int32_t *pos)

{
  int32_t dx = pos[0] - (int32_t)W[0x0CDC];
  int32_t dy = pos[1] - (int32_t)W[0x0CE0];
  int32_t dz = pos[2] - (int32_t)W[0x0CE4];
  int32_t h = (int32_t)math_atan2(dx >> 4, dz >> 4);
  int32_t s = (int32_t)vrd16s(0x20B004 + ((uint32_t)(uint16_t)h & 0xfffc)) >> 4;
  int32_t p = (int32_t)math_atan2(dy >> 4, m68k_divs((int32_t)((uint32_t)dx << 7), s));
  /* rate 0 is reachable (frame 263 of the fly-in): the 68K keeps the
   * dividend, which m68k_divs reproduces. */
  W[0x0CE8] = m68k_divs(p - (int32_t)W[0x0CE8] - 0x8000, param_1) + (int32_t)W[0x0CE8];
  if (h > (int32_t)W[0x0CEC] + 0x8000) h -= 0x10000;
  else if ((int32_t)W[0x0CEC] - 0x8000 > h) h += 0x10000;
  W[0x0CEC] = m68k_divs(h - (int32_t)W[0x0CEC], param_1) + (int32_t)W[0x0CEC];
  W[0x0CF0] = 0;
  return;
}

/* ---- camera_offset_apply ---- */

void camera_offset_apply(uint32_t param_1,undefined4 *param_2)

{
  int iVar1;
  int local_10;
  int local_c;
  int local_8;
  
  W[0x0CEC] = ((int32_t*)(intptr_t)param_2)[3] + W[0x0D10] + -0x4000;
  iVar1 = ((int32_t*)(intptr_t)param_2)[4] * W[0x0D0C];
  if (iVar1 < 0) {
    iVar1 = iVar1 + 0xff;
  }
  W[0x0CE8] = iVar1 >> 8;
  iVar1 = ((int32_t*)(intptr_t)param_2)[5] * W[0x0D14];
  if (iVar1 < 0) {
    iVar1 = iVar1 + 0xff;
  }
  W[0x0CF0] = iVar1 >> 8;
  rotate_euler_zxy_optimized
            (*param_2,((int32_t*)(intptr_t)param_2)[1],((int32_t*)(intptr_t)param_2)[2],W[0x0CE8],W[0x0CEC],W[0x0CF0],&local_10,
             &local_c,&local_8);
  W[0x0CDC] =
       ((local_10 + W[0x0D00]) - W[0x0CDC] >> (param_1 & 0x3f)) + W[0x0CDC];
  W[0x0CE0] =
       ((local_c + W[0x0D04]) - W[0x0CE0] >> (param_1 & 0x3f)) + W[0x0CE0];
  W[0x0CE4] =
       ((local_8 + W[0x0D08]) - W[0x0CE4] >> (param_1 & 0x3f)) + W[0x0CE4];
  return;
}

/* ---- camera_setup_extended ---- */

void camera_setup_extended
               (undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
               undefined4 param_5)

{
  player_model_set_wheel_angle(param_1);
  player_model_set_direct_inputs(param_2,param_3,param_4,param_5);
  bicycle_front_rear_ik();
  bicycle_ik_solve();
  camera_dsp_terrain_render();
  return;
}

/* ---- camera_update ---- */

void camera_update(void)

{
  short sVar1;
  undefined4 uVar2;
  undefined4 uVar3;
  int local_10;
  int local_c;
  int local_8;
  
  if (W[0x0E30] == 0) {
    sVar1 = (short)((uint32_t)W[0x12C8] >> 0x10);
    W[0x0CE8] = (int)(short)((short)W[0x0DA8] - (short)W[0x0CE8]) / (sVar1 + 0x10) +
                   W[0x0CE8];
    W[0x0CEC] = (int)(short)((short)W[0x0DAC] - (short)W[0x0CEC]) / (sVar1 + 0x10) +
                   W[0x0CEC];
    W[0x0CF0] = (int)(short)((short)W[0x0DB0] - (short)W[0x0CF0]) / (sVar1 + 0x10) +
                   W[0x0CF0];
    uVar3 = 0xffffdb80;
    uVar2 = 0x400;
  }
  else {
    uVar3 = 0xffffb000;
    uVar2 = 0x600;
    W[0x0CE8] = W[0x0D9C];
    W[0x0CEC] = W[0x0DA0];
    W[0x0CF0] = W[0x0DA4];
  }
  rotate_euler_zxy_optimized
            (0,uVar2,uVar3,W[0x0CE8],W[0x0CEC],W[0x0CF0],&local_10,&local_c,&local_8);
  W[0x0CDC] = local_10 + W[0x0D00];
  W[0x0CE0] = local_c + W[0x0D04];
  W[0x0CE4] = local_8 + W[0x0D08];
  return;
}

/* ---- dsp_cmd_emit_arrow_indicator ---- */
/* ROM 0x0123D4. Both arguments are ADDRESSES of 3-long world positions (the
 * arrow's tip and its base); the transpile's callers passed their VALUES,
 * which segfaulted the FINAL STAGE's transition (stage_camera_interpolate,
 * register row 185). This form takes the two vectors already read; the
 * callers read them from wherever the ROM's pointers point. The
 * calc_direction_and_distance output goes to 0xE17294/98/9C, three longs,
 * as `pea $e17294.l` at 0x01240A has it. */
int32_t *dsp_arrow_emit(const int32_t *a, const int32_t *b, int32_t *out)
{
  int32_t d[3], ang[3];
  uint32_t q;
  int32_t dist;

  d[0] = a[0] - b[0];
  d[1] = a[1] - b[1];
  d[2] = a[2] - b[2];
  dist = calc_direction_and_distance((undefined4 *)d, (uint32_t *)ang);
  W[0x17294] = ang[0];
  W[0x17298] = ang[1];
  W[0x1729C] = ang[2];
  /* 0x01241A `lsl.l #3,d0 ; divu.l d0,d2`: the ROM's zero-divide vector is a
   * bare rte, so a zero divisor leaves the dividend (0x80000000). */
  {
    uint32_t den = (uint32_t)dist << 3;
    q = den ? 0x80000000u / den : 0x80000000u;
  }
  if (q >= 0x8000) q = 0x7fff;
  out[0x00] = 0x8008;
  out[0x01] = 3;
  out[0x02] = 6;
  out[0x03] = (int32_t)q;
  out[0x04] = (int32_t)((uint32_t)d[0] << 11) / 0x8000;
  out[0x05] = 0;
  out[0x06] = 0;
  out[0x07] = (int32_t)((uint32_t)d[1] << 11) / 0x8000;
  out[0x08] = -(int32_t)q;
  out[0x09] = -(int32_t)q;
  out[0x0a] = (int32_t)((uint32_t)d[2] << 11) / 0x8000;
  out[0x0b] = 0;
  out[0x0c] = -1;
  out[0x0d] = 0x800a;
  out[0x0e] = 0x24e;
  out[0x0f] = 3;
  out[0x10] = b[0] - (int32_t)W[0x0CDC];
  out[0x11] = b[1] - (int32_t)W[0x0CE0];
  out[0x12] = b[2] - (int32_t)W[0x0CE4];
  return out + 0x13;
}

/* Host int32_t[3] vectors (the stack locals of render_floating_bonus_object). */
undefined4 * dsp_cmd_emit_arrow_indicator(int *param_1,int *param_2,undefined4 *param_3)
{
  return (undefined4 *)dsp_arrow_emit((const int32_t *)param_1,
                                      (const int32_t *)param_2, (int32_t *)param_3);
}

/* ---- scene_objects_draw_list @ 0x02B088 --------------------------------
 * A 0xFFFF-terminated list of 16-bit CELL numbers (8 wide); each becomes a
 * 0x8000-mode placement of model (cell + base) at the cell's grid centre
 * plus the block's (x, y, z). `list` is a ROM address; `pos` a 68K address
 * or a C array of four longs (x, y, z, model base) -- include/ea68k.h. The
 * transpile read the list as native host shorts (byte-swapped). */

void scene_objects_draw_list(ea_t list, ea_t pos)

{
  int32_t *dl;
  uint16_t c;

  list = ea_norm(list);
  pos = ea_norm(pos);
  dsp_cmd_emit_object_mode_8000();
  dl = (int32_t *)W[0x0CA4];
  for (; (c = (uint16_t)ea_rd16(list)) != 0xFFFF; list += 2) {
    dl = dl_put(dl, (int32_t)c + ea_rd32(pos + 12));
    dl = dl_put(dl, (int32_t)(c & 7) * 0x18000 + 0xC000 + ea_rd32(pos) - (int32_t)W[0x0CDC]);
    dl = dl_put(dl, ea_rd32(pos + 4) - (int32_t)W[0x0CE0]);
    dl = dl_put(dl, (int32_t)(c >> 3) * 0x18000 + 0xC000 + ea_rd32(pos + 8) - (int32_t)W[0x0CE4]);
  }
  W[0x0CA4] = (intptr_t)dl;
  return;
}

/* ---- dsp_param_init ---- */

int dsp_param_init(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  
  iVar2 = 0;
  do {
    iVar3 = 0;
    do {
      dsp_w32(0x10000 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0xf));
      dsp_w32(0x10004 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x10008 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0x7fff));
      dsp_w32(0x1000C + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x10010 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0x7fff));
      dsp_w32(0x10014 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x10018 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0x7fff));
      dsp_w32(0x1001C + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(2));
      dsp_w32(0x10020 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(200));
      dsp_w32(0x10024 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0x14));
      dsp_w32(0x10028 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x1002C + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0x5A82));
      dsp_w32(0x10030 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0xffffa57e));
      dsp_w32(0x10034 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(2));
      dsp_w32(0x10038 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0x780));
      dsp_w32(0x1003C + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0x140));
      dsp_w32(0x10040 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0xf0));
      dsp_w32(0x10044 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x10048 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x1004C + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x10050 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(7 - iVar3));
      dsp_w32(0x10054 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x10058 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x1005C + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x10060 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0x7fff));
      dsp_w32(0x10064 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x10068 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0x7fff));
      dsp_w32(0x1006C + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x10070 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0x7fff));
      dsp_w32(0x10074 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0x10000));
      dsp_w32(0x10078 + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      dsp_w32(0x1007C + 4 * (iVar2 * 0x2000 + iVar3 * 0x20), (int32_t)(0));
      iVar3 = iVar3 + 1;
    } while (iVar3 < 8);
    iVar2 = iVar2 + 1;
  } while (iVar2 < 2);
  iVar2 = 0;
  do {
    dsp_w32(0x100B4 + 4 * (iVar2 * 0x2000), (int32_t)(2));
    dsp_w32(0x100B8 + 4 * (iVar2 * 0x2000), (int32_t)(0x980));
    dsp_w32(0x100BC + 4 * (iVar2 * 0x2000), (int32_t)(0x140));
    dsp_w32(0x100C0 + 4 * (iVar2 * 0x2000), (int32_t)(0xf0));
    dsp_w32(0x100C4 + 4 * (iVar2 * 0x2000), (int32_t)(0));
    dsp_w32(0x100C8 + 4 * (iVar2 * 0x2000), (int32_t)(0));
    dsp_w32(0x100CC + 4 * (iVar2 * 0x2000), (int32_t)(0));
    dsp_w32(0x100F4 + 4 * (iVar2 * 0x2000), (int32_t)(0x10000));
    dsp_w32(0x100F8 + 4 * (iVar2 * 0x2000), (int32_t)(0));
    dsp_w32(0x100FC + 4 * (iVar2 * 0x2000), (int32_t)(0));
    iVar3 = iVar2 + 1;
    iVar1 = iVar2 + -1;
    iVar2 = iVar3;
  } while (iVar3 < 2);
  /* THE REAL LAYOUT, for viewports 2..7. ROM 0x022D3A writes a 32-LONG record
   * per viewport at 0xC10000 + buffer*0x8000 + viewport*0x80 (`lsl.l #$f` and
   * `lsl.l #$7`); the byte stores above are the transpile's reading of it --
   * Ghidra's int-pointer strides (0x2000, 0x20) on a uint8_t array, so every
   * value is one truncated byte at a quarter of its address. Other code in
   * this tree (dsp_viewport_setup, sprite_3d_project_and_draw) reads and
   * writes those same bytes for viewports 0/1, so they are left as they are;
   * their footprint ends below 0xC10100 for the viewport indices the game
   * uses. Records 2..7 are written properly here because the name-entry
   * magnifier aims viewport 2's (name_entry_render) and the renderer reads it
   * (lens_viewport): without a reset here the lens window would outlive the
   * screen. Host-order int32, like every other DSP word in g_sys.dspram. */
  { static const int32_t rec[32] = {
        0xf, 0, 0x7fff, 0, 0x7fff, 0, 0x7fff, 2, 0xc8, 0x14, 0, 0x5a82,
        (int32_t)0xffffa57e, 2, 0x780, 0x140, 0xf0, 0, 0, 0, 0 /* 7-vp */, 0, 0, 0,
        0x7fff, 0, 0x7fff, 0, 0x7fff, 0x10000, 0, 0 };
    int b, vp, k;
    for (b = 0; b < 2; b++)
      for (vp = 2; vp < 8; vp++)
        for (k = 0; k < 32; k++) {
          int32_t v = (k == 20) ? 7 - vp : rec[k];
          memcpy(&g_sys.dspram[0x10000 + b * 0x8000 + vp * 0x80 + k * 4], &v, 4);
        } }
  return iVar1;
}

/* ---- sound_stop_all ---- */

/* ROM 0x00F810 -- parameters to 0xFF, then clear bit 15 on all 32 voices.
 * (Ghidra called this `scene_load`; it is the sound system, not a scene.) */
void sound_stop_all(void)
{
  /* ROM 0x00F810 -- SILENCE EVERYTHING: every parameter to 0xFF, then clear
   * bit 15 on all 32 command words. */
  snd_params_reset();
  { int i; for (i = 0; i < 0x20; i++) snd_cmd_w(i, snd_cmd_r(i) & 0x7FFFu); }
}

/* ---- dsp_double_buffer_toggle ---- */

undefined4 dsp_double_buffer_toggle(void)

{
  /* void */;
  
  g_sys.dspram[0x0008] = g_sys.dspram[0x0008] ^ 1;
  return 0;
}

/* ---- dsp_address_validate ---- */

void dsp_address_validate(void)

{
  /* Ghidra *0 decl removed */
  /* void */;
  
  if (0 == NULL) {
    if ((0 >> 0x10 & 0x80) == 0) {
      g_sys.dspram[0x0010] = NULL;
      if (NULL < &g_sys.dspram[0x10400]) {
        g_sys.dspram[0x0010] = NULL;
      }
    }
    else {
      W[0xC841] = (uint8_t)(0 >> 0x10) & 0xf;
      dsp_error_check();
    }
  }
  else {
    // 0 = 0;
  }
  return;
}

/* ---- dsp_cmd_set_matrix_and_render ---- */

void dsp_cmd_set_matrix_and_render(undefined4 param_1)

{
  undefined4 *puVar1;
  
  puVar1 = (int32_t*)W[0x0CA4] + 1;
  *(int32_t*)W[0x0CA4] = 0x8002;
  W[0x0CA4] = puVar1;
  puVar1 = (int32_t*)W[0x0CA4] + 1;
  *(int32_t*)W[0x0CA4] = param_1;
  W[0x0CA4] = puVar1;
  dsp_viewport_setup(param_1,(int)W[0x169F2]);
  return;
}

/* ---- dsp_code_upload ---- */

uint64_t dsp_code_upload(void)

{
  /* void */;
  uint32_t uVar1;
  uint16_t uVar2;
  /* void */;
  undefined4 uVar3;
  short sVar4;
  undefined4 *puVar5;
  undefined2 *puVar6;
  
  g_sys.syscon[0x14] = 0;
  g_sys.syscon[0x1C] = 0;
  g_sys.dspram[0x0030] = 0xffffffff;
  g_sys.dspram[0x0034] = 1;
  g_sys.dspram[0x0038] = 0;
  uVar1 = CONCAT22((short)((uint32_t)0 >> 0x10),R[0x43748]);
  uVar3 = 0;
  puVar5 = &g_sys.dspram[0x0C04];
  puVar6 = &R[0x4374A];
  g_sys.dspram[0x0C00] = uVar1;
  do {
    uVar3 = CONCAT22((short)((uint32_t)uVar3 >> 0x10),*puVar6);
    *puVar5 = uVar3;
    uVar2 = (short)uVar1 - 1;
    uVar1 = (uint32_t)uVar2;
    puVar5 = puVar5 + 1;
    puVar6 = puVar6 + 1;
  } while (uVar2 != 0xffff);
  g_sys.syscon[0x1C] = 0xff;
  sVar4 = -0x10;
  while (g_sys.syscon[0x14] = 0, (g_sys.dspram[0x0030] & 0xffff) != 0) {
    sVar4 = sVar4 + -1;
    if (sVar4 == -1) goto LAB_0004b170;
  }
  uVar1 = (uint32_t)R[0x4B17E];
  puVar5 = &g_sys.dspram[0x0C04];
  g_sys.dspram[0x0C04] = &R[0x09B00];
  puVar6 = &R[0x4B180];
  g_sys.dspram[0x0C00] = uVar1;
  do {
    puVar5 = puVar5 + 1;
    uVar3 = CONCAT22((short)((uint32_t)uVar3 >> 0x10),*puVar6);
    *puVar5 = uVar3;
    uVar2 = (short)uVar1 - 1;
    uVar1 = (uint32_t)uVar2;
    puVar6 = puVar6 + 1;
  } while (uVar2 != 0xffff);
  g_sys.dspram[0x0030] = 0xffffffff;
  sVar4 = -0x10;
  do {
    g_sys.syscon[0x14] = 0;
    sVar4 = sVar4 + -1;
  } while (sVar4 != -1);
LAB_0004b170:
  g_sys.syscon[0x14] = 0;
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- dsp_code_upload_and_init ---- */

void dsp_code_upload_and_init(void)

{
  undefined2 uVar1;
  /* Ghidra null decl removed */;
  int extraout_A0_00;
  int extraout_A0_01;
  /* Ghidra null decl removed */;
  
  g_sys.syscon[0x1C] = 0;
  dsp_slot_table_clear();
  *(undefined2 *)(0) = 0xffff;
  uVar1 = dsp_code_verify();
  *(undefined2 *)(extraout_A0_00 + 2) = uVar1;
  *(undefined4 *)(0) = 0xffffffff;
  dsp_wait_result();
  (*(int16_t*)(extraout_A0_01 + 0x18)) = (short)*(undefined4 *)(0);
  return;
}

/* ---- dsp_code_upload_exec ---- */

void dsp_code_upload_exec(void)

{
  short sVar1;
  undefined2 uVar2;
  /* Ghidra null decl removed */;
  uint32_t *puVar3;
  uint32_t *puVar4;
  uint16_t *puVar5;
  
  g_sys.syscon[0x1C] = 0;
  // 0 = 0xffff;
  dsp_ram_clear_32();
  /* Ghidra null subscript removed: NULL[0xe] = 0xffffffff; */
  g_sys.syscon[0x1C] = 1;
  /* Ghidra null subscript removed: NULL[1] = 1; */
  /* Ghidra null subscript removed: NULL[2] = 1; */
  /* Ghidra null subscript removed: NULL[0xf] = 0x38; */
  sVar1 = 0x37;
  puVar3 = NULL + 0x4000;
  puVar5 = &R[0x3DCCC];
  do {
    puVar4 = puVar3 + 1;
    *puVar3 = (uint32_t)*puVar5;
    sVar1 = sVar1 + -1;
    puVar3 = puVar4;
    puVar5 = puVar5 + 1;
  } while (sVar1 != -1);
  *puVar4 = 0xffffffff;
  // 0 = 0xffffffff;
  uVar2 = dsp_wait_result();
  /* Ghidra null deref removed: *NULL = uVar2; */
  /* Ghidra null subscript removed: NULL[0x75] = 3; */
  /* DSP sync loop removed (was null pointer access) */
  return;
}

/* ---- dsp_code_verify ---- */

void dsp_code_verify(void)

{
  undefined2 extraout_D0u;
  uint32_t uVar1;
  uint16_t uVar2;
  /* Ghidra null decl removed */;
  uint32_t uVar3;
  /* Ghidra null decl removed */;
  undefined2 *puVar4;
  uint32_t *puVar5;
  /* Ghidra null decl removed */;
  
  dsp_ram_clear_32();
  /* DSP RAM pointer access via NULL - Ghidra failed to resolve DSP base address */
  mem_write32(0xC00030, 0xffffffff);
  uVar1 = 0;  /* CONCAT22 of DSP result - simplified */
  uVar3 = NULL;
  puVar4 = NULL;
  do {
    puVar5 = puVar5 + 1;
    puVar4 = puVar4 + 1;
    uVar3 = CONCAT22((short)(uVar3 >> 0x10),*puVar4);
    *puVar5 = uVar3;
    uVar2 = (short)uVar1 - 1;
    uVar1 = (uint32_t)uVar2;
  } while (uVar2 != 0xffff);
  g_sys.syscon[0x1C] = 1;
  dsp_wait_result();
  dsp_ram_clear_32();
  g_sys.syscon[0x1C] = 0;
  *(undefined4 *)(0) = 0;
  g_sys.syscon[0x1C] = 1;
  return;
}

/* ---- dsp_command_dispatch ---- */

void dsp_command_dispatch(void)

{
  /* Ghidra *0 decl removed */
  /* void */;
  uint16_t uVar1;
  /* void */;
  uint8_t unaff_D7b;
  
  uVar1 = (uint16_t)0;
  if (0 == NULL) {
    g_sys.dspram[0x0010] = 0;
    if (uVar1 == 0x8000) {
      W[0xC842] = 7;
      unaff_D7b = 0;
    }
    else if (uVar1 == 0x8001) {
      W[0xC842] = 7;
      unaff_D7b = 0;
    }
    else if (uVar1 == 0x8002) {
      W[0xC842] = 7;
      unaff_D7b = 0;
    }
    else if (uVar1 == 0xffff) {
      g_sys.dspram[0x0010] = (undefined4 *)0xffffffff;
      unaff_D7b = 0;
    }
    else {
      unaff_D7b = W[0xC842];
      if (0x7fff < uVar1) {
        dsp_command_error_handler();
        return;
      }
    }
  }
  if (((char)unaff_D7b < '\x01') || (unaff_D7b == 7)) {
    // 0 = 0;
  }
  else if ((unaff_D7b & 1) != 0) {
    // 0 = CONCAT22((short)((uint32_t)0 >> 0x10),uVar1);
  }
  return;
}

/* ---- dsp_command_error_handler ---- */

void dsp_command_error_handler(void)

{
  W[0xC800] = 0x8310000;
  error_report_and_return();
  return;
}

/* ---- dsp_command_filter ---- */

void dsp_command_filter(void)

{
  uint16_t uVar1;
  /* Ghidra *0 decl removed */
  
  if ((((0 == NULL) && (uVar1 = (uint16_t)0, g_sys.dspram[0x0010] = 0, uVar1 != 0x8000))
      && (uVar1 != 0x8001)) && (uVar1 != 0x8002)) {
    if (uVar1 == 0xffff) {
      g_sys.dspram[0x0010] = (undefined4 *)0xffffffff;
    }
    else if (0x7fff < uVar1) {
      dsp_command_error_handler();
      return;
    }
  }
  return;
}

/* ---- dsp_emit_dual_arrows ---- */

/* ROM 0x0124B8: two arrows from one tip to the two ROM bases at 0x35D2C and
 * 0x35D38 (3 BE32 longs each). The ROM passes the table ADDRESSES; they were
 * handed over as host pointers into the big-endian ROM and read natively. */
void dsp_emit_dual_arrows(int *param_1)

{
  int32_t b[3];
  int k;
  for (k = 0; k < 3; k++) b[k] = vrd32s(0x35D2C + k * 4);
  W[0x0CA4] = (intptr_t)dsp_arrow_emit((const int32_t *)param_1, b, (int32_t *)W[0x0CA4]);
  for (k = 0; k < 3; k++) b[k] = vrd32s(0x35D38 + k * 4);
  W[0x0CA4] = (intptr_t)dsp_arrow_emit((const int32_t *)param_1, b, (int32_t *)W[0x0CA4]);
  return;
}

/* ---- dsp_error_check ---- */

void dsp_error_check(void)

{
  if ((7 < W[0xC841]) && (W[0xC841] != 0)) {
    W[0xC800] = 0x8320000;
    error_report_and_return();
    return;
  }
  return;
}

/* ---- dsp_init ---- */

void dsp_init(void)

{
  dsp_code_upload();
  g_sys.syscon[0x1C] = 0;
  dsp_polygon_ram_init();
  dsp_param_init();
  sync_reset();
  g_sys.dspram[0] = 1;
  g_sys.syscon[0x1C] = 1;
  return;
}

/* ---- dsp_kickstart_simple ---- */

void dsp_kickstart_simple(void)

{
  if (W[0xC843] == '\0') {
    if ((short)g_sys.dspram[0x0004] != 0) {
      watchdog_spin_forever();
    }
    dsp_command_filter();
    g_sys.dspram[0x0004] = 1;
  }
  return;
}

/* ---- dsp_kickstart_with_offset ---- */

void dsp_kickstart_with_offset(void)

{
  if (W[0xC843] == '\0') {
    if ((short)g_sys.dspram[0x0004] != 0) {
      watchdog_spin_forever();
    }
    g_sys.dspram[0x0010] = &g_sys.dspram[0x10400];
    g_sys.dspram[0x0004] = 1;
  }
  return;
}

/* ---- dsp_kickstart_with_validation ---- */

void dsp_kickstart_with_validation(void)

{
  if (W[0xC843] == '\0') {
    if ((short)g_sys.dspram[0x0004] != 0) {
      watchdog_spin_forever();
    }
    dsp_address_validate();
    g_sys.dspram[0x0010] = &g_sys.dspram[0x10400];
    g_sys.dspram[0x0004] = 1;
  }
  return;
}

/* ---- dsp_param_restore_full ---- */

void dsp_param_restore_full(void)

{
  undefined4 *puVar1;
  undefined4 *puVar3;
  undefined4 *puVar2;
  
  puVar1 = &g_sys.dspram[0x10000];
  puVar3 = &W[0xD000];
  do {
    puVar2 = puVar1 + 1;
    *puVar1 = *puVar3;
    puVar1 = puVar2;
    puVar3 = puVar3 + 1;
  } while (puVar2 != (undefined4 *)&g_sys.dspram[0x10c00]);
  return;
}

/* ---- dsp_param_restore_page0 ---- */

void dsp_param_restore_page0(void)

{
  undefined4 *puVar1;
  undefined4 *puVar3;
  undefined4 *puVar2;
  
  puVar1 = &g_sys.dspram[0x10000];
  puVar3 = &W[0xCC00];
  do {
    puVar2 = puVar1 + 1;
    *puVar1 = *puVar3;
    puVar1 = puVar2;
    puVar3 = puVar3 + 1;
  } while (puVar2 != (undefined4 *)&g_sys.dspram[0x10400]);
  return;
}

/* ---- dsp_param_save_full ---- */

void dsp_param_save_full(void)

{
  undefined4 *puVar1;
  undefined4 *puVar3;
  undefined4 *puVar2;
  
  puVar1 = &g_sys.dspram[0x10000];
  puVar3 = &W[0xD000];
  do {
    puVar2 = puVar1 + 1;
    *puVar3 = *puVar1;
    puVar1 = puVar2;
    puVar3 = puVar3 + 1;
  } while (puVar2 != (undefined4 *)&g_sys.dspram[0x10c00]);
  return;
}

/* ---- dsp_param_save_page0 ---- */

void dsp_param_save_page0(void)

{
  undefined4 *puVar1;
  undefined4 *puVar3;
  undefined4 *puVar2;
  
  puVar1 = &g_sys.dspram[0x10000];
  puVar3 = &W[0xCC00];
  do {
    puVar2 = puVar1 + 1;
    *puVar3 = *puVar1;
    puVar1 = puVar2;
    puVar3 = puVar3 + 1;
  } while (puVar2 != (undefined4 *)&g_sys.dspram[0x10400]);
  return;
}

/* ---- dsp_place_distant_scenery ---- */

void dsp_place_distant_scenery(void)

{
  int *piVar1;
  
  if (0x11000 < W[0x0CE0]) {
    /* SCRATCH DSP CURSOR, 32-BIT ENTRIES. W[0x09B4] is the prop-placement
   cursor (A2 in terrain_props_dispatch's prologue) and is used as a host
   pointer: `*(int32_t*)W[0x09B4] = ...`. `W[0x09B4] + 1` is intptr_t
   arithmetic and advances ONE BYTE, so every entry after the first
   overlapped the previous one by three bytes. Same class as CLAUDE.md
   key-fix #1 (the W[0x0CA4] cursor), one slot over. Latent until the
   props dispatch was recovered -- nothing called these helpers before. */
  piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x5ce;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x24000 - W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xb4000 - W[0x0CE4];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x5cf;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x3c000 - W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xb4000 - W[0x0CE4];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x5d0;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = (intptr_t)&R[0x54000] - W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xb4000 - W[0x0CE4];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x5d6;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x24000 - W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xcc000 - W[0x0CE4];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x5d7;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x3c000 - W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xcc000 - W[0x0CE4];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x5d8;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = (intptr_t)&R[0x54000] - W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xcc000 - W[0x0CE4];
    W[0x09B4] = piVar1;
  }
  return;
}

/* ---- dsp_place_prop_a ---- */

void dsp_place_prop_a(void)

{
  int *piVar1;
  
  if (W[0x1258] == -1) {
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x1d0;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CE4];
    W[0x09B4] = piVar1;
  }
  return;
}

/* ---- dsp_place_prop_b ---- */

void dsp_place_prop_b(void)

{
  int *piVar1;
  
  if (W[0x1259] == -1) {
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x1d1;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CE4];
    W[0x09B4] = piVar1;
  }
  return;
}

/* ---- dsp_place_prop_c ---- */

void dsp_place_prop_c(void)

{
  int *piVar1;
  
  if (W[0x125A] == -1) {
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x1d2;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CE4];
    W[0x09B4] = piVar1;
  }
  return;
}

/* ---- dsp_place_structure_a ---- */

void dsp_place_structure_a(void)

{
  int *piVar1;
  
  if (W[0x125D] == -1) {
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x1ca;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CE4];
    W[0x09B4] = piVar1;
  }
  return;
}

/* ---- dsp_place_structure_b ---- */

void dsp_place_structure_b(void)

{
  int *piVar1;
  
  if (W[0x1262] == -1) {
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x1cc;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CE4];
    W[0x09B4] = piVar1;
  }
  return;
}

/* ---- dsp_place_structure_c ---- */

void dsp_place_structure_c(void)

{
  int *piVar1;
  
  if (W[0x126A] == -1) {
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x1cb;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CE4];
    W[0x09B4] = piVar1;
  }
  return;
}

/* ---- dsp_place_structure_d ---- */

void dsp_place_structure_d(void)

{
  int *piVar1;
  
  if (W[0x1272] == -1) {
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x1cd;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CE4];
    W[0x09B4] = piVar1;
  }
  return;
}

/* ---- dsp_place_structure_e ---- */

void dsp_place_structure_e(void)

{
  int *piVar1;
  
  if (W[0x1280] == -1) {
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x1ce;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CE4];
    W[0x09B4] = piVar1;
  }
  return;
}

/* ---- dsp_place_windmill_a ---- */

void dsp_place_windmill_a(void)

{
  int *piVar1;
  
  if (W[0x1258] == -1) {
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x1c8;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CE4];
    W[0x09B4] = piVar1;
  }
  return;
}

/* ---- dsp_place_windmill_b ---- */

void dsp_place_windmill_b(void)

{
  int *piVar1;
  
  if (W[0x1259] == -1) {
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0x1c9;
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CDC];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = 0xa000 - W[0x0CE0];
    W[0x09B4] = piVar1;
    piVar1 = (int *)((int32_t *)W[0x09B4] + 1);
    *(int32_t*)W[0x09B4] = -W[0x0CE4];
    W[0x09B4] = piVar1;
  }
  return;
}

/* ---- dsp_polygon_ram_init ---- */

void dsp_polygon_ram_init(void)

{
  g_sys.dspram[0] = 1;
  g_sys.dspram[0x0004] = 0;
  g_sys.dspram[0x0008] = 4;
  g_sys.dspram[0x000C] = 0;
  g_sys.dspram[0x0010] = 0;
  g_sys.dspram[0x0014] = 1;
  g_sys.dspram[0x0018] = 0x32;
  g_sys.dspram[0x001C] = 0x280;
  g_sys.dspram[0x0028] = 0;
  g_sys.dspram[0x002C] = 0;
  g_sys.dspram[0x0030] = 0;
  g_sys.dspram[0x0034] = 0;
  g_sys.dspram[0x0038] = 0;
  g_sys.dspram[0x003C] = 0;
  g_sys.dspram[0x0040] = 0;
  g_sys.dspram[0x0044] = 0;
  g_sys.dspram[0x0048] = 0;
  g_sys.dspram[0x004C] = 0;
  g_sys.dspram[0x0050] = 0;
  g_sys.dspram[0x0064] = 0;
  g_sys.dspram[0x0068] = 0;
  return;
}

/* ---- dsp_ram_clear_32 ---- */

uint64_t dsp_ram_clear_32(void)

{
  /* void */;
  /* void */;
  short sVar1;
  
  g_sys.syscon[0x1C] = 0;
  sVar1 = 0x1f;
  do {
    // 0 = 0;
    sVar1 = sVar1 + -1;
  } while (sVar1 != -1);
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- dsp_render_terminate ---- */

void dsp_render_terminate(void)

{
  if ((W[0xC843] == '\0') && ((short)g_sys.dspram[0x0004] == 0)) {
    g_sys.dspram[0x0010] = 0xffffffff;
    g_sys.dspram[0x0004] = 1;
  }
  return;
}

/* ---- dsp_reset_and_sync ---- */

void dsp_reset_and_sync(void)

{
  g_sys.syscon[0x1C] = 0;
  g_sys.dspram[0] = 0;
  g_sys.dspram[0x0028] = 0;
  g_sys.dspram[0x004C] = 3;
  g_sys.syscon[0x1C] = 1;
  g_sys.dspram[0x002C] = 0xffffffff;
  return;
}

/* ---- dsp_slot_table_clear ---- */

void dsp_slot_table_clear(void)

{
  short in_D0w;
  /* Ghidra null decl removed */;
  
  do {
    *(undefined4 *)(NULL + in_D0w * 4 + 0x84) = 0xffffffff;
    in_D0w = in_D0w + -1;
  } while (in_D0w != -1);
  return;
}

/* ---- dsp_slot_table_read ---- */

void dsp_slot_table_read(void)

{
  short in_D1w;
  undefined4 *puVar1;
  /* Ghidra null decl removed */;
  
  puVar1 = (undefined4 *)(0);
  do {
    // 0 = (short)*puVar1;
    in_D1w = in_D1w + -1;
    puVar1 = puVar1 + 1;
  } while (in_D1w != -1);
  return;
}

/* ---- dsp_upload_and_execute ---- */

uint64_t dsp_upload_and_execute(void)

{
  /* void */;
  uint32_t uVar1;
  uint16_t uVar3;
  undefined4 uVar2;
  /* void */;
  int iVar4;
  undefined4 *puVar5;
  undefined4 *puVar6;
  
  g_sys.syscon[0x1C] = 0;
  g_sys.dspram[0x0038] = 0;
  g_sys.dspram[0x0028] = 0;
  g_sys.dspram[0x0030] = 0xffffffff;
  g_sys.dspram[0] = 0xffffffff;
  uVar1 = CONCAT22((short)((uint32_t)0 >> 0x10),0);
  g_sys.dspram[0x0C00] = uVar1 + 1;
  uVar2 = 0;
  puVar5 = &g_sys.dspram[0x0C04];
  do {
    uVar2 = CONCAT22((short)((uint32_t)uVar2 >> 0x10),0);
    puVar6 = puVar5 + 1;
    *puVar5 = uVar2;
    uVar3 = (short)uVar1 - 1;
    uVar1 = (uint32_t)uVar3;
    puVar5 = puVar6;
  } while (uVar3 != 0xffff);
  *puVar6 = 0;
  g_sys.syscon[0x1C] = 1;
  iVar4 = 0x7fff8;
  do {
    if ((g_sys.dspram[0x0030] & 0xffff) == 0) {
      g_sys.dspram[0] = 0;
      g_sys.syscon[0x1C] = 0;
      g_sys.dspram[0x0030] = 0;
      g_sys.syscon[0x1C] = 1;
      uVar2 = 0;
      goto LAB_0004845e;
    }
    iVar4 = iVar4 + -1;
  } while (iVar4 != 0);
  uVar2 = 1;
LAB_0004845e:
  return (((uint64_t)(uVar2) << 32) | (uint32_t)(0));
}

/* ---- dsp_wait_result ---- */

undefined4 dsp_wait_result(void)

{
  undefined2 in_D0w;
  /* Ghidra null decl removed */;
  
  *(undefined2 *)(0) = in_D0w;
  do {
    if ((short)0 != -1) {
    }
  } while ((*(int16_t*)(0)) != 0);
  return 0xffffffff;
}

/* ---- rotate_vector_2d ---- */

void rotate_vector_2d(int param_1,int param_2,undefined4 param_3,uint32_t param_4,int *param_5,
                     int *param_6,undefined4 *param_7)

{
  uint32_t uVar1;
  
  uVar1 = (param_4 >> 1 & 0x7ffe) * 2;
  *param_5 = param_1 * (int16_t)vrd16s(0x20B006 + (uVar1)) - param_2 * (int16_t)vrd16s(0x20B004 + (uVar1)) >>
             0xf;
  *param_6 = param_2 * (int16_t)vrd16s(0x20B006 + (param_4 >> 1 & 0x7ffe) * 2) +
             param_1 * (int16_t)vrd16s(0x20B004 + (uVar1)) >> 0xf;
  *param_7 = param_3;
  return;
}

/* ---- rotate_euler_xzy ---- */

void rotate_euler_xzy(undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
                     undefined4 param_5,undefined4 param_6,undefined4 *param_7,undefined4 *param_8,
                     undefined4 *param_9)

{
  rotate_vector_2d(param_2,param_1,param_3,param_6,&param_2,&param_1,&param_3);
  rotate_vector_2d(param_1,param_3,param_2,param_5,&param_1,&param_3,&param_2);
  rotate_vector_2d(param_3,param_2,param_1,param_4,&param_3,&param_2,&param_1);
  *param_7 = param_1;
  *param_8 = param_2;
  *param_9 = param_3;
  return;
}

/* ---- rotate_euler_yxz ---- */

void rotate_euler_yxz(undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
                     undefined4 param_5,undefined4 param_6,undefined4 *param_7,undefined4 *param_8,
                     undefined4 *param_9)

{
  rotate_vector_2d(param_1,param_3,param_2,param_5,&param_1,&param_3,&param_2);
  rotate_vector_2d(param_3,param_2,param_1,param_4,&param_3,&param_2,&param_1);
  rotate_vector_2d(param_2,param_1,param_3,param_6,&param_2,&param_1,&param_3);
  *param_7 = param_1;
  *param_8 = param_2;
  *param_9 = param_3;
  return;
}

/* ---- rotate_euler_zxy ---- */

void rotate_euler_zxy(undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
                     undefined4 param_5,undefined4 param_6,undefined4 *param_7,undefined4 *param_8,
                     undefined4 *param_9)

{
  rotate_vector_2d(param_3,param_2,param_1,param_4,&param_3,&param_2,&param_1);
  rotate_vector_2d(param_2,param_1,param_3,param_6,&param_2,&param_1,&param_3);
  rotate_vector_2d(param_1,param_3,param_2,param_5,&param_1,&param_3,&param_2);
  *param_7 = param_1;
  *param_8 = param_2;
  *param_9 = param_3;
  return;
}

/* ---- rotate_euler_zyx ---- */

void rotate_euler_zyx(undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
                     undefined4 param_5,undefined4 param_6,undefined4 *param_7,undefined4 *param_8,
                     undefined4 *param_9)

{
  rotate_vector_2d(param_3,param_2,param_1,param_4,&param_3,&param_2,&param_1);
  rotate_vector_2d(param_1,param_3,param_2,param_5,&param_1,&param_3,&param_2);
  rotate_vector_2d(param_2,param_1,param_3,param_6,&param_2,&param_1,&param_3);
  *param_7 = param_1;
  *param_8 = param_2;
  *param_9 = param_3;
  return;
}

/* ---- camera_chase_player ---- */

void camera_chase_player(uint32_t param_1,undefined4 *param_2)

{
  int local_10;
  int local_c;
  int local_8;
  
  W[0x0CF0] = 0;
  W[0x0CEC] = ((int)(short)(((short)W[0xB738] + 0x4000) - (short)W[0x0CEC]) >>
                 (param_1 & 0x3f)) + W[0x0CEC];
  W[0x0CE8] = ((int32_t*)(intptr_t)param_2)[3] + W[0xB73C];
  rotate_euler_zxy_optimized
            (*param_2,((int32_t*)(intptr_t)param_2)[1],((int32_t*)(intptr_t)param_2)[2],W[0x0CE8],W[0x0CEC],0,&local_10,&local_c,&local_8)
  ;
  W[0x0CDC] = ((local_10 + W[0xB728]) - W[0x0CDC] >> (param_1 & 0x3f)) + W[0x0CDC]
  ;
  W[0x0CE0] = ((local_c + W[0xB72C]) - W[0x0CE0] >> (param_1 & 0x3f)) + W[0x0CE0];
  W[0x0CE4] = ((local_8 + W[0xB730]) - W[0x0CE4] >> (param_1 & 0x3f)) + W[0x0CE4];
  W16_SET(0x172F2, 0x780);
  return;
}

/* ---- camera_compute_position ---- */

void camera_compute_position(void)

{
  int local_10;
  int local_c;
  int local_8;
  
  if (W[0x0C24] == 0) {
    rotate_euler_zxy_optimized
              (0,0,W[0x0C34],W[0x0C28],W[0x0D10] + W[0x0C2C],0,&local_10,&local_c,
               &local_8);
    W[0x0CDC] = local_10 + W[0x0D00];
    W[0x0CE0] = local_c + W[0x0D04];
    W[0x0CE4] = local_8 + W[0x0D08];
    W[0x0CE8] = W[0x0C28];
    W[0x0CEC] = W[0x0D10] + W[0x0C2C];
    W[0x0CF0] = 0;
  }
  else {
    rotate_euler_zxy_optimized
              (0,0x400,0xffffdb80,W[0x0D0C],W[0x0D10],W[0x0D14],&local_10,&local_c,
               &local_8);
    W[0x0CDC] = local_10 + W[0x0D00];
    W[0x0CE0] = local_c + W[0x0D04];
    W[0x0CE4] = local_8 + W[0x0D08];
    W[0x0CE8] = W[0x0D0C];
    W[0x0CEC] = W[0x0D10];
    W[0x0CF0] = W[0x0D14];
  }
  return;
}

/* ---- camera_interpolate_6dof ---- */

/* ROM 0x02AC24: the camera block := a + (b - a) * t / n per long, six of
 * them (32-bit `muls.l`, then `divs.l`); with `zoom` set the seventh pair
 * also, into the 16-bit ending zoom 0xE172F2. `a`, `b` are 68K addresses. */
void camera_interpolate_6dof(uint32_t a, uint32_t b, int t, int n, int zoom)
{
  int i;
  if (n == 0) return;
  for (i = 0; i < 6; i++) {
    int32_t va = e_rd32(a + i * 4), vb = e_rd32(b + i * 4);
    W[0x0CDC + i * 4] = (int32_t)((uint32_t)(vb - va) * (uint32_t)t) / n + va;
  }
  if (zoom != 0) {
    int32_t va = e_rd32(a + 24), vb = e_rd32(b + 24);
    W16_SET(0x172F2, (int32_t)((uint32_t)(vb - va) * (uint32_t)t) / n + va);
  }
}

/* ---- camera_orbit_player ---- */

/* ROM 0x02ACA6, ported from the machine code. Ease the camera toward an
 * orbit about the player node (0xE0B728..): heading = node heading + 0x4000,
 * pitch = node pitch, roll = 0; the offset `rec` (a 68K address of [x, y, z,
 * pitch]) is turned by -(node pitch + rec pitch) about X, then by -heading
 * about Y, and the position moves 1/2^shift of the way there each frame.
 * Products are the ROM's 0x0146B2 (>> 15). The transpile dereferenced `rec`
 * as a host pointer and used 0x0146B2's >> 16 stand-in. */
void camera_orbit_player(int shift, uint32_t rec)
{
  int32_t s, c, ly, lz, r1, r2, r0, px, pz;
  uint32_t a;
  W[0x0CF0] = 0;
  W[0x0CEC] = (int32_t)W[0xB738] + 0x4000;
  W[0x0CE8] = (int32_t)W[0xB73C];
  a = (uint16_t)(-((int32_t)W[0xB73C] + e_rd32(rec + 12)));
  s = e_sin(a); c = e_cos(a);
  r0 = e_rd32(rec); r1 = e_rd32(rec + 4); r2 = e_rd32(rec + 8);
  ly = e_mul15(r1, c) - e_mul15(r2, s);
  lz = e_mul15(r1, s) + e_mul15(r2, c);
  a = (uint16_t)(-(int32_t)W[0x0CEC]);
  s = e_sin(a); c = e_cos(a);
  pz = e_mul15(lz, c) - e_mul15(r0, s);
  px = e_mul15(r0, c) + e_mul15(lz, s);
  W[0x0CDC] = (int32_t)W[0x0CDC] + ((px + (int32_t)W[0xB728] - (int32_t)W[0x0CDC]) >> (shift & 63));
  W[0x0CE0] = (int32_t)W[0x0CE0] + (((int32_t)W[0xB72C] + ly - (int32_t)W[0x0CE0]) >> (shift & 63));
  W[0x0CE4] = (int32_t)W[0x0CE4] + (((int32_t)W[0xB730] + pz - (int32_t)W[0x0CE4]) >> (shift & 63));
  W16_SET(0x172F2, 0x780);
}

/* ---- camera_set_from_array @ 0x02AC78 ----------------------------------
 * Six longs -> the camera block 0xE00CDC (x, y, z, pitch, heading, roll);
 * with `flag` the low word of a seventh -> the ending's focal 0xE172F2.
 * `src` is a 68K address (a ROM table, or 0xE17294 in work RAM) or a C
 * array standing in for a 68K stack local -- see include/ea68k.h. The
 * transpile walked `&W[0x0CDC]` as an `undefined4 *` (4 host bytes a step
 * over 8-byte slots) and read ROM tables natively. */

void camera_set_from_array(ea_t src, int flag)

{
  int i;
  src = ea_norm(src);
  for (i = 0; i < 6; i++) W[0x0CDC + i * 4] = ea_rd32(src + i * 4);
  if (flag != 0) W16_SET(0x172F2, (int16_t)ea_rd32(src + 24));
  return;
}

/* ---- sound_play ---- */

/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */

/* Animation-chain helpers shared with game_misc.c (anim_load_list) and
 * game_gameplay.c / game_player.c. Nodes are addressed by _W[] byte offset. */
int spline_interpolate_channel(intptr_t key, intptr_t acc, int t);
void scene_node_render(int *param_1);

/* ---- scene_animation_update ---- */

/* ROM 0x0265EC -- advance every animation node of a chain to frame `t`.
 * Node layout: see anim_load_list (game_misc.c). For each channel whose DOF
 * bit is set in the node's flags, the stream at +0x0C holds segments of
 *     [count byte][op byte][payload]
 * and the channel value at +0x24 is produced by op (table at 0x026662):
 *   0 per-frame longs  1 cubic (spline_interpolate_channel)  2 byte deltas
 *   3 word deltas  4 linear ramp through the accumulator
 * When the frame reaches the segment's end (`dt >= count`) the start frame
 * advances by count+1 and the stream pointer by the segment's size (table at
 * 0x0266F8: 6+4n, 0x12, n+3, 2(n+2), 4, 2). A node flagged 0x40 cycles its
 * MODEL through the [model, until-frame] word list at +0x7C, and the 0x7FFF
 * node is the camera.
 *
 * INDEX SCALES, decoded from the extension words (capstone drops them):
 * op 0 reads `$2(a4,a0.l*4)` (0x8C02), op 2 `$2(a4,a0.l)` (0x8802), op 3
 * `$2(a4,a0.l*2)` (0x8A02).
 *
 * The transpile indexed the node as `int *` into the 8-byte _W[] slots and
 * read the ROM streams through host pointers; both jump tables were lost. */
void anim_update(intptr_t a3, int t)
{
  int16_t d5 = (int16_t)t;
  int guard = 0;
  if (a3 < 0) return;
  while (a3 + 0x80 <= WORK_RAM_SIZE && (int32_t)W[a3] >= 0 && guard++ < 64) {
    int32_t d6 = W_HI16(a3 + 8);
    uint32_t seq = (uint32_t)W[a3 + 0x7C];
    int k;
    if (seq != 0 && d5 >= vrd16s(seq + 2)) {                  /* 0x026618 */
      seq += 4;
      W[a3 + 0x7C] = (int32_t)seq;
      W[a3] = vrd16s(seq);
    }
    for (k = 0; k < 6; k++) {
      intptr_t a2 = a3 + 0x24 + k * 4;
      uint32_t a4;
      int op, cnt;
      int32_t dt;
      if ((d6 & (int32_t)vrd32(0x37664 + k * 4)) == 0) continue;
      a4 = (uint32_t)W[a3 + 0x0C + k * 4];
      if (a4 + 2 > ROM_SIZE) continue;
      op  = (int8_t)vrd8(a4 + 1);
      cnt = vrd8(a4);
      dt  = (int32_t)d5 - (int32_t)W_A16(a3 + 0x6C, k);
      switch (op) {
      case 0: W[a2] = (int32_t)vrd32(a4 + 2 + dt * 4); break;             /* 0x02666C */
      case 1: {                                                             /* 0x026678 */
        int acc = (int)(int32_t)W[a2 + 0x18];
        W[a2] = spline_interpolate_channel((int)a4, &acc, (uint16_t)dt);
        W[a2 + 0x18] = (int32_t)acc;
        break; }
      case 2: W[a2] = (int32_t)W[a2] + vrd8s(a4 + 2 + dt); break;         /* 0x026692 */
      case 3: W[a2] = (int32_t)W[a2] + vrd16s(a4 + 2 + dt * 2); break;    /* 0x0266A2 */
      case 4: {                                                             /* 0x0266B2 */
        int32_t d0 = vrd16s(a4 + 2) + (int32_t)W[a2 + 0x18];
        W[a2 + 0x18] = d0;
        W[a2] = (d0 >> 8) + (int32_t)W[a2 + 0x30];
        break; }
      default: break;
      }
      if (dt >= cnt) {                                                      /* 0x0266C8 */
        uint32_t adv = 0;
        W_A16_SET(a3 + 0x6C, k, W_A16(a3 + 0x6C, k) + cnt + 1);
        switch (op) {
        case 0: adv = 6 + (uint32_t)cnt * 4; break;
        case 1: adv = 0x12; break;
        case 2: adv = (uint32_t)cnt + 3; break;
        case 3: adv = ((uint32_t)cnt + 2) * 2; break;
        case 4: adv = 4; break;
        case 5: adv = 2; break;
        default: break;
        }
        W[a3 + 0x0C + k * 4] = (int32_t)(a4 + adv);
        W[a2 + 0x18] = 0;
        W[a2 + 0x30] = W[a2];
      }
    }
    if ((int32_t)W[a3] == 0x7fff) {                                        /* 0x02675C */
      W[0x0CDC] = (int32_t)W[a3 + 0x24];
      W[0x0CE0] = (int32_t)W[a3 + 0x28];
      W[0x0CE4] = (int32_t)W[a3 + 0x2C];
      W[0x0CE8] = -(int32_t)W[a3 + 0x30];
      W[0x0CEC] = -(int32_t)W[a3 + 0x34];
      W[0x0CF0] = -(int32_t)W[a3 + 0x38];
    }
    a3 += 0x80;
  }
}

void scene_animation_update(int *param_1,undefined4 param_2)
{
  anim_update(anim_w_off(param_1), (int)(int16_t)(uint32_t)param_2);
}

/* ---- scene_get_max_priority ---- */

int scene_get_max_priority(void)

{
  int iVar1;
  undefined2 *puVar2;
  
  iVar1 = 0;
  for (puVar2 = &R[0x355A4]; -1 < (short)puVar2[1]; puVar2 = puVar2 + 6) {
    if (iVar1 < (short)puVar2[1]) {
      iVar1 = (int)(short)puVar2[1];
    }
  }
  return iVar1;
}

/* ---- scene_init ---- */

void scene_init(void)

{
  sound_stop_all();
  sound_env_apply();
  W16_SET(0x15F44, 0);                    /* ROM 0x00F2EC `clr.w $e15f44.l` */
  /* ROM 0x00F2F2 `tst.w $e04176.l` (the LOW half of slot 0x4174) and then a
   * 16-bit mailbox write of table entry 0's command, `move.w d1,(a0,d0.w*2)`.
   * It was a byte store at a byte index into the MCU mailbox. */
  if (W16(0x4176) == 0) {
    snd_cmd_w(snd_slot(0), (unsigned)snd_cmd(0) | 0x4000u);
    scene_load_loop();
  }
  /* ROM 0x00F31A/0x00F322: `move.w #$ffff` into the deferred-sound TIMER at
   * 0xE15F56 and the parked id at 0xE15F58 -- the pair sound_play_or_defer
   * and sound_deferred_tick use. This wrote the timer as a whole 2-mod-4
   * slot (never written back) and the id to 0xE15F50, the course-1 stem
   * timer. NOTE: nothing in the C calls this function; the ROM calls it from
   * entry_reset @0x00BD04. */
  W16_SET(0x15F56, -1);
  W16_SET(0x15F58, -1);
  return;
}

/* ---- scene_interpolation_evaluate ---- */

/* ROM 0x0267AC -- set every animated channel of a chain to its CUBIC segment
 * evaluated at frame `t` (a 16-bit argument, `move.w $1c(a7)`), without
 * advancing anything. Same node layout as anim_update. */
void scene_interpolation_evaluate(int *param_1, int param_2)

{
  intptr_t a2 = anim_w_off(param_1);
  int guard = 0;
  if (a2 < 0) return;
  while (a2 + 0x80 <= WORK_RAM_SIZE && (int32_t)W[a2] >= 0 && guard++ < 64) {
    int32_t d2 = W_HI16(a2 + 8);
    int k;
    for (k = 0; k < 6; k++) {
      if ((d2 & (int32_t)vrd32(0x37664 + k * 4)) != 0) {
        int acc = (int)(int32_t)W[a2 + 0x3C + k * 4];
        W[a2 + 0x24 + k * 4] = spline_interpolate_channel((int)(int32_t)W[a2 + 0x0C + k * 4],
                                                          &acc, (uint16_t)param_2);
        W[a2 + 0x3C + k * 4] = (int32_t)acc;
      }
    }
    a2 += 0x80;
  }
}

/* ---- sound_params_reset ---- */

/* ROM 0x00F84C -- every parameter slot in the 0x359E4 list to 0xFF.
 * (Ghidra called this `scene_invalidate_objects`; it is the sound system, not a scene.) */
void sound_params_reset(void)
{
  snd_params_reset();     /* ROM 0x00F84C -- parameters only */
}

/* ---- scene_load_loop ---- */

void scene_load_loop(void)

{
  short sVar1;
  short sVar2;
  short sVar3;
  
  sVar3 = 0;
  do {
    sVar2 = 0;
    do {
      sVar1 = 0;
      do {
        sync_wait_3();
        sVar1 = sVar1 + 1;
      } while (sVar1 < 10);
      keycus_write_2();
      sVar2 = sVar2 + 1;
    } while (sVar2 < 1);
    keycus_write_1();
    g_sys.syscon[0x14] = 0;
    sVar3 = sVar3 + 1;
  } while (sVar3 < 1);
  return;
}

/* ---- sound_play_p2 ---- */

/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */
/* dup removed */

/* ---- sound_env_apply ---- */

/* ROM 0x00F94A -- copy the four environment words from W[0x3FE0] to 0xA0BD58.
 * (Ghidra called this `scene_setup`; it is the sound system, not a scene.) */
int sound_env_apply(void)
{
  /* ROM 0x00F94A -- the same four words, from W[0x3FE0..] (BE16 halves). */
  { int i; for (i = 0; i < 4; i++)
      comms_w16(g_sys.commsram, 0xBD58 + i * 2,
                (unsigned)(uint16_t)(uint32_t)W[0x3FE0 + i * 2]); }
  return 0;
}

/* ---- sound_hold_and_defer ---- */

/* ROM 0x00F558 -- set bit 13 on a running voice and arm the deferred-sound timer.
 * (Ghidra called this `scene_start_deferred_load`; it is the sound system, not a scene.) */
void sound_hold_and_defer(undefined4 param_1)
{
  /* ROM 0x00F558 region -- set bit 13 on a running voice and arm the timer. */
  /* ONE argument, at $4(a7) with no register saved at entry -- so Ghidra's
   * param_1 IS the id, not the high half of a packed pair. The two- and
   * three-argument entry points below are the ones that pack. */
  int id = (int)(int16_t)(uint32_t)param_1;
  snd_trig("sound_hold_and_defer", id, -1);
  int slot = snd_slot(id);
  snd_report("sound_hold_and_defer", id, slot, snd_cmd_r(slot), -1);
  snd_cmd_w(slot, snd_cmd_r(slot) | 0x2000u);
  /* ROM 0x00F57A `move.w #$11d,$e15f56.l`: the 16-bit deferred-sound timer,
   * the LOW half of slot 0x15F54 -- the half sound_deferred_tick and
   * sound_play_or_defer read. The whole-slot store never reached them. */
  W16_SET(0x15F56, 0x11d);
}

/* ---- sound_deferred_tick ---- */

/* ROM 0x00F59A -- count the deferred-sound timer down and play the parked id when it expires.
 * (Ghidra called this `scene_update`; it is the sound system, not a scene.) */
void sound_deferred_tick(void)

{
  /* ROM 0x00F59A. The deferred sound: when its timer runs out, play the id
   * that was parked for it and clear the park.
   *
   * Two things were wrong and they compounded. The id is at 0xE15F58
   * (`tst.w (a2)` / `move.w (a2),-(a7)` with a2 = 0xE15F58), a SIXTEEN-BIT
   * field -- `sound_play_or_defer` parks it there itself with
   * `move.w $4(a7), $e15f58.l` at 0x00F508 -- not at W[0x15F50], which is
   * what g_scene_deferred_object names. And the call passed a literal 0,
   * which is sound id 0: SLOT 0, the announcer. So every time a deferred
   * sound came due the game played the announcer instead, on a path that
   * runs every frame -- the "prop cycle over and over" a player hears.
   * The timer at 0xE15F56 is 16-bit and 2-mod-4, i.e. the LOW half of its
   * slot; 0xE15F58 is 4-aligned, i.e. the HIGH half of its own. */
  if (W_LO16(0x15F54) >= 0) {
      W_SET_LO16(0x15F54, W_LO16(0x15F54) - 1);
      if (W_LO16(0x15F54) < 0 && W_HI16(0x15F58) >= 0) {
          sound_play_or_defer(W_HI16(0x15F58));
          W_SET_HI16(0x15F58, -1);
      }
  }
  return;
}

/* ---- scene_update_and_render ---- */

/* ROM 0x026808 -- advance the chain to frame `t`, then draw every node. The
 * frame is a pushed WORD (`move.w $c(a7)`); every caller in the transpile had
 * lost it, and the node was passed as the bare offset 0xab04 and then
 * dereferenced as a host address. */
void anim_update_and_render(intptr_t nb, int t)
{
  int guard = 0;
  if (nb < 0) return;
  anim_update(nb, t);
  while (nb + 0x80 <= WORK_RAM_SIZE && (int32_t)W[nb] >= 0 && guard++ < 64) {
    scene_node_render((int *)&_W[nb]);
    nb += 0x80;
  }
}

void scene_update_and_render(int *param_1, int param_2)

{
  anim_update_and_render(anim_w_off(param_1), param_2);
  return;
}

/* ---- sound_play_p2 ---- */

/* ROM 0x00F77C -- play a sound with TWO caller parameters.
 * (Ghidra called this `scene_load_object_pair`; it is the sound system, not a scene.) */
short * sound_play_p2(undefined4 param_1,undefined4 param_2)
{
  /* ROM 0x00F77C. Three arguments: id, and TWO parameters -- the second
   * overrides the table's own constant rather than adding to it. */
  int id  = (int)(int16_t)((uint32_t)param_1 >> 16);
  int p1v = (int)(int16_t)((uint32_t)param_1 & 0xFFFF);
  int p2v = (int)(int16_t)(uint32_t)param_2;
  snd_trig("sound_play_p2", id, p1v & 0xFFFF);
  if ((((W[0x0CBC] & 0xfffffffe) == 2) || ((W[0x0CBC] & 0xfffffffe) == 4)) ||
      (W[0x3FFC] != 0)) {
    int slot = snd_slot(id);
    if (slot >= 0) {
      snd_par_w(snd_pslot(id), (unsigned)p1v);
      int p2 = snd_pslot2(id);
      if (p2 >= 0) snd_par_w(p2, (unsigned)p2v);
      snd_report("sound_play_p2", id, slot, (unsigned)snd_cmd(id), p1v);
      snd_cmd_w(slot, (unsigned)snd_cmd(id) | 0x4000u);
    }
  }
  return (short *)0;
}

/* ---- matrix_rotate_xyz ---- */

void matrix_rotate_xyz(undefined4 *param_1,int *param_2)

{
  short sVar1;
  short sVar2;
  int iVar3;
  int iVar4;
  int iVar5;
  int iVar6;
  int iVar7;
  int iVar8;
  
  sVar1 = vrd16s(0x20B004 + ((uint32_t)((*(int16_t*)param_1 + 0xe) & 0x3fff) * 2) * 2);
  sVar2 = vrd16s(0x20B006 + ((uint32_t)((*(int16_t*)param_1 + 0xe) & 0x3fff) * 2) * 2);
  iVar3 = fixed_point_mul_32x32(((int32_t*)(intptr_t)param_1)[2],(int)sVar2);
  iVar4 = fixed_point_mul_32x32(*param_1,(int)sVar1,iVar3);
  iVar5 = fixed_point_mul_32x32(((int32_t*)(intptr_t)param_1)[2],(int)sVar1);
  iVar6 = fixed_point_mul_32x32(*param_1,(int)sVar2,iVar5);
  sVar1 = vrd16s(0x20B004 + ((uint32_t)((*(int16_t*)(param_1 + 4)) & 0x3fff) * 2) * 2);
  sVar2 = vrd16s(0x20B006 + ((uint32_t)((*(int16_t*)(param_1 + 4)) & 0x3fff) * 2) * 2);
  iVar7 = fixed_point_mul_32x32(((int32_t*)(intptr_t)param_1)[1],(int)sVar1);
  iVar8 = fixed_point_mul_32x32(iVar5 + iVar6,(int)sVar2);
  *param_2 = iVar8 - iVar7;
  iVar7 = fixed_point_mul_32x32(((int32_t*)(intptr_t)param_1)[1],(int)sVar2);
  iVar5 = fixed_point_mul_32x32(iVar5 + iVar6,(int)sVar1,iVar7);
  sVar1 = vrd16s(0x20B004 + ((uint32_t)((*(int16_t*)(param_1 + 3)) & 0x3fff) * 2) * 2);
  sVar2 = vrd16s(0x20B006 + ((uint32_t)((*(int16_t*)(param_1 + 3)) & 0x3fff) * 2) * 2);
  iVar6 = fixed_point_mul_32x32(iVar7 + iVar5,(int)sVar2);
  iVar8 = fixed_point_mul_32x32(iVar3 - iVar4,(int)sVar1);
  ((int32_t*)(intptr_t)param_2)[1] = iVar6 - iVar8;
  iVar5 = fixed_point_mul_32x32(iVar7 + iVar5,(int)sVar1);
  iVar3 = fixed_point_mul_32x32(iVar3 - iVar4,(int)sVar2);
  ((int32_t*)(intptr_t)param_2)[2] = iVar5 + iVar3;
  return;
}

/* ---- camera_dsp_terrain_render @ 0x02814C ---- */



void camera_dsp_terrain_render(void)

{
  extern int g_rig_ik_gameplay;
  undefined4 *puVar1;
  int iVar2;
  intptr_t nb3;          /* rider/bike node base, as a _W[] SLOT index */
  intptr_t piVar4;
  
  if (W[0x12BC] == 0) {
    iVar2 = 0;
  }
  else {
    iVar2 = -0x19;
  }
  W[0xEB08] = W[0x0D00] - W[0x0CDC];
  W[0xEB0C] = W[0x0D04] - W[0x0CE0];
  W[0xEB10] = W[0x0D08] - W[0x0CE4];
  /* NODE CHAIN WALK -- THIS IS WHAT DRAWS THE RIDER.
   *
   * nb3 is a _W[] SLOT index, not an int* into it. This walked with
   * `int W[nb3]` over an intptr_t[] array, so every field read was
   * misaligned by the documented (&W[base])[N] stride bug, and the
   * index at [0x11] below (computed from a half-word in the same
   * struct, scaled by 0x20) went so far out of range that it had to be
   * bounds-guarded to stop it segfaulting -- that is the
   * "[GUARD] camera_dsp_terrain_render ... stride is still unconverted"
   * line every launch printed.
   *
   * The chain is the one player_model_load_animation builds:
   *   0xAB04..0xAD84  6 bike nodes   (models 69-74, codes 138-143)
   *   0xAE04..        16 rider nodes (models 77-92,  codes 146-161)
   * stride 0x20 ints = 0x80 slots, terminated by a negative model id.
   * Half-words use W_HI16/W_LO16: on the big-endian M68K
   * `*(short *)(p + 1)` is the TOP half of the word (see vaddr.h). */
  nb3 = ((int)W_HI16(0x169EC) == 0) ? 0xAE04 : 0xAB84;
  /* GAMEPLAY: start at the RIDER, not the bike.
   *
   * `player_render` already emits the six bike parts itself, as models
   * 0x45..0x4a (69..74 -> codes 138..143) through its own
   * dsp_cmd_set_velocity calls. Walking the chain from 0xAB84 (node 1)
   * emits nodes 1..22, i.e. the bike AND the rider, so every bike part was
   * placed TWICE -- the raw display list read
   *   models 69,70,71,72,73,74, 69,70,71,72,73,74, 77,83,...
   * against MAME's gameplay capture, which has each of 138..143 exactly
   * once. On screen that is a doubled bike.
   *
   * This whole path is our IK stand-in: on real hardware camera_update_main
   * never runs in gameplay at all (register row 48 -- the CPU writes 66
   * words there and the master expands the scene). So the stand-in must
   * supply only what player_render does not, which is the rider. */
  /* PROPCYCL_RIG_SPLIT=1 restores the split above (register row 64's fix).
   *
   * By default gameplay now walks the WHOLE chain from 0xAB84 -- bike AND
   * rider -- which is exactly what the flyover does, and the flyover is the
   * arrangement that measures 2.2 deg against MAME where the split gameplay
   * measures 26.8 deg with the rider anchored on the rig root and 126.6 deg
   * anchored on the drawn body. The split put the six bike parts under
   * player_render's own matrix, ~100 deg from where the 16 rider parts
   * assemble, so the rider could never sit on the bike.
   *
   * The double-bike row 64 describes is avoided by suppressing
   * player_render's own emission instead (see dsp_cmd_set_velocity below),
   * rather than by shortening the chain. */
  { extern int g_rig_split;
    if (g_rig_split && g_rig_ik_gameplay && W[0x0CBC] == 3) nb3 = 0xAE04; }
  { static int fr; int dbg = getenv("PROPCYCL_NODEW") && fr++ < 400;
    if (dbg) { intptr_t t = nb3; int k = 0; (void)k;
      printf("    [NODEW] f=%d start=0x%lX flag=%ld\n", (int)g_sys.frame_count, (long)nb3, (long)W_HI16(0x169EC));
      (void)t; } }
  do {
    *(int32_t*)W[0x0CA4] = 0x8008;
    ((int32_t*)W[0x0CA4])[1] = 3;
    ((int32_t*)W[0x0CA4])[2] = 1;
    /* Trig-table reads, same pattern as scene_node_render @ 3361.
     * (intptr_t)puVar1 + 2 truncates the host pointer to 32 bits — replaced
     * with direct rom_nr32 reads at 0x20B002 + a (sin) / 0x20B004 + a (cos). */
    { uint32_t _a = (uint32_t)(W[nb3 + 0xc * 4] & 0xfffc);
      SNR_PAIR(((int32_t*)W[0x0CA4]), 3, _a); }
    { uint32_t _a = (uint32_t)(W[nb3 + 0xd * 4] & 0xfffc);
      SNR_PAIR(((int32_t*)W[0x0CA4]), 5, _a); }
    { uint32_t _a = (uint32_t)(W[nb3 + 0xe * 4] & 0xfffc);
      SNR_PAIR(((int32_t*)W[0x0CA4]), 7, _a); }
    (void)puVar1;
    ((int32_t*)W[0x0CA4])[9] = (int)W_LO16(nb3 + 8);
    ((int32_t*)W[0x0CA4])[10] = 0;
    ((int32_t*)W[0x0CA4])[0xb] = W[nb3 + 9 * 4];
    ((int32_t*)W[0x0CA4])[0xc] = W[nb3 + 10 * 4];
    ((int32_t*)W[0x0CA4])[0xd] = W[nb3 + 0xb * 4];
    ((int32_t*)W[0x0CA4])[0xe] = 0xffffffff;
    ((int32_t*)W[0x0CA4])[0xf] = 0x8009;
    ((int32_t*)W[0x0CA4])[0x10] = 3;
    /* Parent-node link: the half-word at +4 is the RELATIVE node index
     * (0, -1, -2, ... measured out of the ROM tables), scaled by the
     * 0x20-int node stride. This used to be an out-of-range int* read
     * that had to be bounds-guarded to avoid a segfault; with slot
     * indexing it lands inside the chain, so the guard is gone. It is
     * still range-checked because a corrupt chain must not scribble. */
    {
        intptr_t _idx = nb3 + (W_HI16(nb3 + 4) * 0x20 + 0x1e) * 4;
        ((int32_t*)W[0x0CA4])[0x11] =
            (_idx >= 0 && _idx < WORK_RAM_SIZE) ? (int32_t)W[_idx] : 0;
    }
    ((int32_t*)W[0x0CA4])[0x12] = W[nb3 + 0x1e * 4];
    ((int32_t*)W[0x0CA4])[0x13] = 0x800a;
    ((int32_t*)W[0x0CA4])[0x14] = iVar2 + W[nb3];
    ((int32_t*)W[0x0CA4])[0x15] = W[nb3 + 0x1e * 4];
    ((int32_t*)W[0x0CA4])[0x16] = W[0xEB08];
    piVar4 = (intptr_t)((int32_t*)W[0x0CA4] + 0x18);
    ((int32_t*)W[0x0CA4])[0x17] = W[0xEB0C];
    W[0x0CA4] = W[0x0CA4] + 0x19 * 4;
    *(int32_t*)piVar4 = W[0xEB10];
    nb3 += 0x20 * 4;
  } while (-1 < (int32_t)W[nb3]);
  return;
}





/* ---- camera_update_main @ 0x0283DE ---- */



/* PROPCYCL_RIG_IK_GAMEPLAY=0 restores the ROM's own branch. Default 1:
 * gameplay drives the rider with the LIVE IK (camera_setup_simple) that the
 * flyover uses and that is validated to 2.5 deg median against the
 * recording, instead of player_animation_keyframe_update -- which is
 * unconverted, emits 0 words, and leaves gameplay with no rider at all.
 * This is a STAND-IN, not the game's animation: the real path indexes a
 * keyframe table through a 68020 scaled-index addressing mode that Ghidra
 * rendered as `W[0x16A08 + iVar4*2]`, a slot that actually holds the
 * PREVIOUS iVar4 (the same function writes it at the end). The table has
 * not been reversed -- a ROM scan for its 12-pointer record signature
 * returns 1147 candidates, i.e. nothing conclusive. See RIDER_RIG.md. */
int g_rig_ik_gameplay = 1;
/* set only around the gameplay stand-in's root emission -- see below */
int g_root_xform_only = 0;
/* PROPCYCL_NODES_FULL=1: print all 32 fields of every rig node, for a
 * field-for-field diff against tools/overnight/probe_nodes.lua. */
int g_nodes_full = 0;

void camera_update_main(void)

{
  int iVar1;
    { extern int g_nodes_at; static int done = 0;
      if (g_nodes_at >= 0 && !done && (int)g_sys.frame_count >= g_nodes_at) { done = 1;
        extern intptr_t _W[]; int k;
        printf("    [NODE] idx  base     model  pos(x,y,z)                 ang(x,y,z)            hi4\n");
        for (k = 0; k < 26; k++) {
          intptr_t nb = 0xAB04 + k * 0x80;
          long model = (long)(int32_t)_W[nb];
          long x = (long)(int32_t)_W[nb + 9*4], y = (long)(int32_t)_W[nb + 10*4], z = (long)(int32_t)_W[nb + 0xb*4];
          long ax = (long)(int32_t)_W[nb + 0xc*4], ay = (long)(int32_t)_W[nb + 0xd*4], az = (long)(int32_t)_W[nb + 0xe*4];
          long hi4 = (long)(int16_t)((int32_t)_W[nb + 4] >> 16);
          long f1e = (long)(int32_t)_W[nb + 0x1e*4];
          long pf1e = (hi4 < 0 && k + hi4 >= 0) ? (long)(int32_t)_W[nb + hi4*0x80 + 0x1e*4] : -1;
          printf("    [NODE] %3d  0x%05lX  %5ld  %8ld %8ld %8ld   %6ld %6ld %6ld  %3ld  f1e=%ld parent_f1e=%ld\n",
                 k, (long)nb, model, x, y, z, ax, ay, az, hi4, f1e, pf1e);
        } } }
  /* PROPCYCL_NODES_FULL=N prints every one of the 32 fields of every node for
   * N consecutive frames starting at PROPCYCL_NODES, in the same
   * "frame node field value" form that tools/overnight/probe_nodes.lua dumps
   * out of MAME.  It must run over a RANGE inside ONE process: stitching
   * separate one-shot runs gave a prop rate of -4096/frame in one set and
   * -8192/frame in another, i.e. the runs were not comparable at all. */
  { extern int g_nodes_at, g_nodes_full; extern intptr_t _W[];
    int f = (int)g_sys.frame_count;
    if (g_nodes_full > 0 && g_nodes_at >= 0 && f >= g_nodes_at && f < g_nodes_at + g_nodes_full) {
      int k, fl;
      for (k = 0; k < 26; k++)
        for (fl = 0; fl < 0x20; fl++)
          printf("[NODEF] %d %d %d %ld\n", f, k, fl,
                 (long)(uint32_t)(int32_t)_W[0xAB04 + k*0x80 + fl*4]); } }

  { extern int g_rig_dump; extern intptr_t _W[];
    if (g_rig_dump) { static int n;
      if (n++ % 60 == 0) printf("[RIG] f=%d state=%ld sub=%ld 12BC=%ld EB06=%ld mode=%d AB04=%ld\n",
        (int)g_sys.frame_count, (long)W[0x0CBC], (long)g_sub_state_attract, (long)W[0x12BC],
        (long)W[0xEB06], camera_mode_select(), (long)W[0xAB04]); } }
  
  /* The gameplay IK stand-in needs the ROOT record too. player_model_set_pose
   * is what emits node 0 (the 11-word 0x8008 + 0x800a that defines transform
   * slot 0 and the camera-relative base every child hangs off). Without it
   * gameplay emitted the 22 children with slot 0 never validated: measured,
   * every part's matrix came back all-zero and the bike drew unrotated, while
   * the offsets were already right to a median of 14 units against the new
   * gameplay capture. */
  if (((W[0x0CBC] == 1) && (g_sub_state_attract == 3)) ||
     ((W[0x0CBC] == 5 && (g_stage_timer == 0x21))) ||
     /* NOT gated on g_rig_ik_gameplay. That flag selects which driver poses
      * the JOINTS (the live IK stand-in vs the ROM's own
      * player_animation_keyframe_update); the ROOT record is what validates
      * transform slot 0, and every child hangs off it whichever driver ran.
      * Measured here before the flag existed: with the root suppressed,
      * every rider matrix comes back all-zero and the gate reads exactly
      * acos(-0.5) = 120.0 deg on all 16 parts -- register row 49's symptom. */
     (W[0x0CBC] == 3)) {
    { extern int g_rig_dump; extern int g_root_xform_only; intptr_t _b=W[0x0CA4];
      /* In GAMEPLAY the root is wanted only to validate transform slot 0 --
       * player_render emits the bike body itself. In the attract/flyover
       * states the root IS the drawn body, so the sentinel is not set. */
      /* The root is the DRAWN body now, in gameplay too -- the flyover has
       * always worked that way and it is what makes the rider's parent
       * transform and the body on screen the same matrix by construction. */
      { extern int g_rig_split; g_root_xform_only = (g_rig_split && W[0x0CBC] == 3); }
      { extern int g_rig_dump; if (g_rig_dump) { static int n;
          if (n++ % 60 == 0) printf("[ROOT] node0 ang=%ld,%ld,%ld   -W[0D0C/10/14]=%ld,%ld,%ld\n",
             (long)(int32_t)W[0xAB34], (long)(int32_t)W[0xAB38], (long)(int32_t)W[0xAB3C],
             (long)-(int32_t)W[0x0D0C], (long)-(int32_t)W[0x0D10], (long)-(int32_t)W[0x0D14]); } }
      player_model_set_pose();
      g_root_xform_only = 0;
      if (g_rig_dump) { static int _n; if (_n++ % 60 == 0) printf("[RIG]   player_model_set_pose wrote %ld words\n", (long)((W[0x0CA4]-_b)/4)); } }
  }
  else {
    W_SET_HI16(0x169EC, 0);
  }
  iVar1 = camera_mode_select();
  if (getenv("PROPCYCL_CAMPATH")) { static int n;
      if (n++ % 120 == 0)
          printf("    [CAMPATH] f=%d mode=%d EB06=%ld 16A08=%ld 16A0A=%ld 169EC=%ld\n",
                 (int)g_sys.frame_count, iVar1, (long)W[0xEB06],
                 (long)W[0x16A08], (long)W[0x16A0A], (long)W_HI16(0x169EC)); }
  if (iVar1 == 0) {
    if (W[0xEB06] == 0) {
      { extern int g_rig_dump; intptr_t _b=W[0x0CA4]; player_animation_state_update();
      if (g_rig_dump) { static int _n; if (_n++ % 60 == 0) printf("[RIG]   player_animation_state_update wrote %ld words\n", (long)((W[0x0CA4]-_b)/4)); } }
      if (W[0xEB06] != 0) {
        W[0x0D94] = 0;
      }
    }
    else if (W[0x0CBC] == 3 && !g_rig_ik_gameplay) {
      { extern int g_rig_dump; intptr_t _b=W[0x0CA4]; player_animation_keyframe_update(W[0x0D98]);
      if (g_rig_dump) { static int _n; if (_n++ % 60 == 0) printf("[RIG]   player_animation_keyframe_update wrote %ld words\n", (long)((W[0x0CA4]-_b)/4)); } }
    }
    else {
      { extern int g_rig_dump; intptr_t _b=W[0x0CA4]; camera_setup_simple((uint32_t)(W[0x0D98] << 0x10) >> 6);
      if (g_rig_dump) { static int _n; if (_n++ % 60 == 0) printf("[RIG]   camera_setup_simple wrote %ld words\n", (long)((W[0x0CA4]-_b)/4)); } }
    }
  }
  else {
    intptr_t _b = W[0x0CA4];
    { extern int g_rig_dump; scene_node_array_animate(iVar1);
      if (g_rig_dump) { static int _n; if (_n++ % 60 == 0) printf("[RIG]   scene_node_array_animate wrote %ld words\n", (long)((W[0x0CA4]-_b)/4)); } }
    /* SAFETY NET, not a second draw.
     *
     * This block exists because of register row 114: the `else` runs INSTEAD
     * OF the pose/emit dispatch above, so while scene_node_array_animate was
     * a guarded stub the whole bike+rider chain was absent on every terrain
     * contact frame (68 objects -> 46, 22 missing, codes 138-161).
     *
     * The function is converted now, and the obvious tidy-up -- pose only when
     * the animation path wrote nothing -- was TRIED AND MEASURED WORSE: blink
     * events over a full level went 37 -> 134 and rider parts 21 -> 91. The
     * hit animation emits its own chain but does not by itself keep the model
     * on screen, so both run. Measured, this pairing is the best of the three:
     *   stub + unconditional pose   84 events, 41 rider
     *   converted + pose too        37 events, 21 rider   <- shipped
     *   converted, pose only as net 134 events, 91 rider
     * Checked for the row 64 double-draw before shipping (below). */
    if (W[0xEB06] == 0) {
      player_animation_state_update();
      if (W[0xEB06] != 0) W[0x0D94] = 0;
    } else if (W[0x0CBC] == 3 && !g_rig_ik_gameplay) {
      player_animation_keyframe_update(W[0x0D98]);
    } else {
      camera_setup_simple((uint32_t)(W[0x0D98] << 0x10) >> 6);
    }
    if (iVar1 == 1) {
      W_SET_LO16(0x169EC, 0x78);
    }
    else if (iVar1 == 4) {
      W_SET_LO16(0x169EC, 100);
    }
    else {
      W_SET_LO16(0x169EC, 0);
    }
  }
/* THE HIT-ANIMATION COUNTER IS A 16-BIT WORD AT A 2-MOD-4 OFFSET, i.e. the
 * LOW half of the slot at 0x169EC -- written full-width it is rebuilt from
 * neighbouring bytes by sync_wram_to_W every frame, so it never counted down
 * and the effect it gates was on permanently.
 *
 * ROM, every access 16-bit, a4 = 0xE169EE:
 *     028444  move.w d0,(a4)      arm (0x78 / 100 / 0)
 *     028486  tst.w  (a4)         `ble` -> skip
 *     02848E  subq.w #$1,(a4)     count down
 *     028490  tst.w  (a4)         `bne` -> skip the expiry call
 *
 * What it gates: camera_dsp_sky_render, which emits models 0x30b..0x30e
 * (record codes 847-851) -- the flash a user sees as "stars around the
 * rider's head". camera_mode_select() arms it to 120 or 100 frames on a
 * terrain hit, so on the machine it appears on contact and fades. MAME draws
 * NONE of codes 847-851 across 720 frames of dumps/gameplay and
 * gameplay_steer, and register row 45 had code 850 filed as emitted-by-us-
 * never-by-MAME since long before this. Same class as rows 46/66/71/99/106/
 * 108/125. */
  if (0 < (int)W_LO16(0x169EC)) {
    { extern int g_rig_dump; intptr_t _b=W[0x0CA4]; camera_dsp_sky_render();
      if (g_rig_dump) { static int _n; if (_n++ % 60 == 0) printf("[RIG]   camera_dsp_sky_render wrote %ld words\n", (long)((W[0x0CA4]-_b)/4)); } }
    W_SET_LO16(0x169EC, (int)W_LO16(0x169EC) - 1);
    if ((int)W_LO16(0x169EC) == 0) {
      FUN_0000fa72();
    }
  }
  { extern int g_rig_dump; intptr_t _b=W[0x0CA4]; hud_draw_wings();
      if (g_rig_dump) { static int _n; if (_n++ % 60 == 0) printf("[RIG]   hud_draw_wings wrote %ld words\n", (long)((W[0x0CA4]-_b)/4)); } }
  return;
}





/* ---- dsp_cmd_emit_object_mode_8002 @ 0x0260B4 ---- */

void dsp_cmd_emit_object_mode_8002(void)

{
  undefined4 *puVar1;
  undefined4 uVar2;
  
  if (W[0x169E8] != 0x8002) {
    W[0x169E8] = 0x8002;
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = 0x8002;
    W[0x0CA4] = puVar1;
    if (W[0x0CC0] == 0xf) {
      uVar2 = 3;
    }
    else {
      uVar2 = 0;
    }
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = uVar2;
    W[0x0CA4] = puVar1;
  }
  return;
}





/* ---- dsp_cmd_place_object_abs @ 0x021E46 ---- */

void dsp_cmd_place_object_abs
               (undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
               undefined4 param_5)

{
  *(int32_t*)W[0x0CA4] = 0x8000;
  ((int32_t*)W[0x0CA4])[1] = param_1;
  ((int32_t*)W[0x0CA4])[2] = param_2;
  ((int32_t*)W[0x0CA4])[3] = param_3;
  ((int32_t*)W[0x0CA4])[4] = param_4;
  ((int32_t*)W[0x0CA4])[5] = param_5;
  W[0x0CA4] = W[0x0CA4] + 6 * 4;
  return;
}







/* ---- dsp_frame_counter_decrement @ 0x03B770 ---- */

void dsp_frame_counter_decrement(void)

{
  /* NOT A STUB -- a LATENT NULL DEREFERENCE, disabled deliberately.
   *
   * The ROM at 0x03B770 is two instructions:
   *     subq.w #$1, $ea(a0)
   *     rts
   * i.e. it decrements the 16-bit word at **a0 + 0xEA**, where a0 is an
   * IMPLICIT REGISTER PARAMETER set by the caller. Ghidra lost that
   * parameter and substituted NULL, so the transcription read
   * `*(short *)(NULL + 0xea)` -- a write to absolute address 0xEA, which
   * would segfault the instant anything called this.
   *
   * It is currently unreachable (0 hits over 1200 frames of attract and of
   * all four courses), which is the only reason the tree has not crashed on
   * it. Fixing it properly means recovering a0 from the call sites and
   * giving the function a parameter; until then it reports and returns
   * rather than faulting. */
  STUB_HIT(0x03B770, "needs its a0 parameter -- see the note above; "
                     "the transcription dereferenced NULL");
  return;
}





/* ---- matrix_rotate_ypr @ 0x0147F8 ---- */

void matrix_rotate_ypr(undefined4 *param_1,int *param_2)

{
  short sVar1;
  short sVar2;
  int iVar3;
  int iVar4;
  int iVar5;
  int iVar6;
  
  sVar1 = vrd16s(0x20B004 + ((uint32_t)(*(uint16_t *)(param_1 + 3) & 0x3fff) * 2) * 2);
  sVar2 = vrd16s(0x20B006 + ((uint32_t)(*(uint16_t *)(param_1 + 3) & 0x3fff) * 2) * 2);
  iVar3 = fixed_point_mul_32x32(param_1[1],(int)sVar2);
  iVar4 = fixed_point_mul_32x32(param_1[2],(int)sVar1);
  param_2[1] = iVar3 - iVar4;
  iVar3 = fixed_point_mul_32x32(param_1[1],(int)sVar1);
  iVar4 = fixed_point_mul_32x32(param_1[2],(int)sVar2,iVar3);
  sVar1 = vrd16s(0x20B004 + ((uint32_t)(*(uint16_t *)((intptr_t)param_1 + 0xe) & 0x3fff) * 2) * 2);
  sVar2 = vrd16s(0x20B006 + ((uint32_t)(*(uint16_t *)((intptr_t)param_1 + 0xe) & 0x3fff) * 2) * 2);
  iVar5 = fixed_point_mul_32x32(*param_1,(int)sVar1);
  iVar6 = fixed_point_mul_32x32(iVar3 + iVar4,(int)sVar2);
  param_2[2] = iVar6 - iVar5;
  iVar5 = fixed_point_mul_32x32(*param_1,(int)sVar2);
  iVar3 = fixed_point_mul_32x32(iVar3 + iVar4,(int)sVar1);
  *param_2 = iVar5 + iVar3;
  return;
}





/* ---- scene_enter_transition @ 0x00FA46 ---- */



void scene_enter_transition(void)

{
  /* ROM 0x00FA46: `tst.w $e15f44.l ; bne` ... `move.w #$1,$e15f44.l`. */
  if (W16(0x15F44) == 0) {
    sound_reset_upper();
    sound_play_p(0x260076);
    W16_SET(0x15F44, 1);
  }
  return;
}





/* g_snr_sinfirst / SNR_PAIR are defined near the top of this file: they are
 * shared with camera_dsp_terrain_render, which is the rider CHILD emitter
 * (gdb: scene_node_render is hit twice a frame -- the root -- while
 * camera_dsp_terrain_render's walk writes the 24 child records). */

/* ---- scene_node_render @ 0x0260F6 ---- */

void scene_node_render(int *param_1)

{
  int iVar1;
  intptr_t piVar2;
  { extern int g_rig_dump, g_rig_calls; g_rig_calls++; }

  /* Guard: catch NULL and short-truncated pointers (< 0x10000).
   * Valid W[] pointers are above this on any 64-bit platform. */
  if ((uintptr_t)param_1 < 0x10000) return;

  /* SLOT INDEXING, not int* indexing.
   *
   * Callers pass &W[node], and W is intptr_t[] (8 bytes per slot) while
   * this body indexed param_1 as int* (4 bytes). Every field read was
   * therefore misaligned: W[nb + 9 * 4] landed 36 bytes in, i.e. slot 4.5,
   * not the node's field at BYTE offset 36. The node walk produced
   * garbage -- 48 copies of one wrong model at a degenerate position
   * (0,-192049,-1) -- and the RIDER (record codes 146-161, 17 body parts
   * the recording shows at |t| ~9000-9900, right on the bike) never
   * rendered at all.
   *
   * In the _W[] model the slot index IS the byte offset, so this is the
   * documented (&W[base])[N] -> W[base + N*4] conversion
   * (tools/fix_w_index.py). Deriving the base from the pointer keeps
   * every existing call site working unchanged. */
  if ((intptr_t *)param_1 < _W || (intptr_t *)param_1 >= _W + WORK_RAM_SIZE)
      return;
  const intptr_t nb = (intptr_t *)param_1 - _W;

  if (0x7000 < W[nb]) {
    if (W[nb] == 0x7fff) {
      dsp_viewport_setup((int)W[0x169F0],(int)W_LO16(nb + 8));
      return;
    }
    if (W[nb] == 0x7ffe) {
      /* ROM 0x02613C..0x026158: `move.l $24(a4)` (the node's animated channel 0)
       * into the zoom word of the current viewport's block -- a LONG at
       * 0xC10038 + buf<<15 + (sext W16 0x169F0)<<7. */
      dsp_w32(0x10038 + (uint32_t)W[0x0CA0] * 0x8000 + (uint32_t)(int16_t)W[0x169F0] * 0x80, (int32_t)W[nb + 0x24]);
      { extern int g_stagedbg;
        if (g_stagedbg && g_sys.frame_count % 15 == 0)
          printf("[CAMZ] f%u buf=%ld vp=%d zoomword=%04X\n", g_sys.frame_count, (long)W[0x0CA0],
                 (int)(int16_t)W[0x169F0], (unsigned)(int32_t)W[nb + 0x24]); }
      return;
    }
    if (W[nb] == 0x7ffd) {
      piVar2 = (intptr_t)((int32_t*)W[0x0CA4] + 1);
      *(int32_t*)W[0x0CA4] = 0x8010;
      if (W[nb + 9 * 4] == 0) {
        W[0x4704] = 0;
      }
      else {
        *(int32_t*)piVar2 = 3;
        W[0x4704] = W[nb + 9 * 4];
        piVar2 = (intptr_t)((int32_t*)W[0x0CA4] + 3);
        ((int32_t*)W[0x0CA4])[2] = W[0x4704];
      }
      iVar1 = -1;
      goto LAB_0002637e;
    }
  }
  if (W_HI16(nb + 4) < 0) {
    *(int32_t*)W[0x0CA4] = 0x8008;
    ((int32_t*)W[0x0CA4])[1] = 3;
    ((int32_t*)W[0x0CA4])[2] = 1;
    { uint32_t _a = (uint32_t)(W[nb + 0xc * 4] & 0xfffc);
      SNR_PAIR(((int32_t*)W[0x0CA4]), 3, _a); }
    { uint32_t _a = (uint32_t)(W[nb + 0xd * 4] & 0xfffc);
      SNR_PAIR(((int32_t*)W[0x0CA4]), 5, _a); }
    { uint32_t _a = (uint32_t)(W[nb + 0xe * 4] & 0xfffc);
      SNR_PAIR(((int32_t*)W[0x0CA4]), 7, _a); }
    ((int32_t*)W[0x0CA4])[9] = (int)W_LO16(nb + 8);
    ((int32_t*)W[0x0CA4])[10] = 0;
    ((int32_t*)W[0x0CA4])[0xb] = W[nb + 9 * 4];
    ((int32_t*)W[0x0CA4])[0xc] = W[nb + 10 * 4];
    ((int32_t*)W[0x0CA4])[0xd] = W[nb + 0xb * 4];
    ((int32_t*)W[0x0CA4])[0xe] = -1;
    ((int32_t*)W[0x0CA4])[0xf] = 0x8009;
    ((int32_t*)W[0x0CA4])[0x10] = 3;
    ((int32_t*)W[0x0CA4])[0x11] = W[nb + ((W_HI16(nb + 4)) * 0x20 + 0x1e) * 4];
    ((int32_t*)W[0x0CA4])[0x12] = W[nb + 0x1e * 4];
    ((int32_t*)W[0x0CA4])[0x13] = 0x800a;
    ((int32_t*)W[0x0CA4])[0x14] = W[nb];
    ((int32_t*)W[0x0CA4])[0x15] = W[nb + 0x1e * 4];
    ((int32_t*)W[0x0CA4])[0x16] = W[0xEB08];
    ((int32_t*)W[0x0CA4])[0x17] = W[0xEB0C];
    ((int32_t*)W[0x0CA4])[0x18] = W[0xEB10];
    W[0x0CA4] = W[0x0CA4] + 0x19 * 4;
    return;
  }
  if (0 < W_HI16(nb + 4)) {
    *(int32_t*)W[0x0CA4] = W[nb];
    ((int32_t*)W[0x0CA4])[1] = W[nb + 9 * 4] - W[0x0CDC];
    ((int32_t*)W[0x0CA4])[2] = W[nb + 10 * 4] - W[0x0CE0];
    ((int32_t*)W[0x0CA4])[3] = W[nb + 0xb * 4] - W[0x0CE4];
    { uint32_t _a = (uint32_t)(W[nb + 0xc * 4] & 0xfffc);
      SNR_PAIR(((int32_t*)W[0x0CA4]), 4, _a); }
    { uint32_t _a = (uint32_t)(W[nb + 0xd * 4] & 0xfffc);
      SNR_PAIR(((int32_t*)W[0x0CA4]), 6, _a); }
    { uint32_t _a = (uint32_t)(W[nb + 0xe * 4] & 0xfffc);
      SNR_PAIR(((int32_t*)W[0x0CA4]), 8, _a); }
    ((int32_t*)W[0x0CA4])[10] = (int)W_LO16(nb + 8);
    W[0x0CA4] = W[0x0CA4] + 0xb * 4;
    return;
  }
  *(int32_t*)W[0x0CA4] = 0x8008;
  ((int32_t*)W[0x0CA4])[1] = W[nb + 0x1e * 4];
  ((int32_t*)W[0x0CA4])[2] = 1;
  { uint32_t _a = (uint32_t)(W[nb + 0xc * 4] & 0xfffc);
    SNR_PAIR(((int32_t*)W[0x0CA4]), 3, _a); }
  { uint32_t _a = (uint32_t)(W[nb + 0xd * 4] & 0xfffc);
    SNR_PAIR(((int32_t*)W[0x0CA4]), 5, _a); }
  { uint32_t _a = (uint32_t)(W[nb + 0xe * 4] & 0xfffc);
    SNR_PAIR(((int32_t*)W[0x0CA4]), 7, _a); }
  ((int32_t*)W[0x0CA4])[9] = (int)W_LO16(nb + 8);
  ((int32_t*)W[0x0CA4])[10] = -1;
  ((int32_t*)W[0x0CA4])[0xb] = 0x800a;
  /* g_root_xform_only: emit this root for its TRANSFORM only. In gameplay
   * player_render already places the six bike parts (models 0x45..0x4a) with
   * its own dsp_cmd_set_velocity calls, so drawing the root here too put a
   * second bike body on screen. The renderer skips RIG_XFORM_ONLY after it
   * has taken the slot transform from the record. */
  { extern int g_root_xform_only;
    ((int32_t*)W[0x0CA4])[0xc] = g_root_xform_only ? -2 : W[nb]; }
  ((int32_t*)W[0x0CA4])[0xd] = W[nb + 0x1e * 4];
  W[0xEB08] = W[nb + 9 * 4] - W[0x0CDC];
  ((int32_t*)W[0x0CA4])[0xe] = W[0xEB08];
  W[0xEB0C] = W[nb + 10 * 4] - W[0x0CE0];
  piVar2 = (intptr_t)((int32_t*)W[0x0CA4] + 0x10);
  ((int32_t*)W[0x0CA4])[0xf] = W[0xEB0C];
  iVar1 = W[nb + 0xb * 4] - W[0x0CE4];
  W[0xEB10] = iVar1;
LAB_0002637e:
  *(int32_t*)piVar2 = iVar1;
  W[0x0CA4] = piVar2 + 4;
  return;
}





