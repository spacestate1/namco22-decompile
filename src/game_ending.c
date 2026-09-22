/*
 * Results, Ending & Bonus
 * Auto-split from game_deps.c / game_ported.c
 */
#include "propcycl.h"
#include "ending_rd.h"
#include "ea68k.h"
#include <stdlib.h>   /* getenv/atoi for PROPCYCL_RESDUMP: undeclared, gnu89 truncates the pointer */

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
short * sound_play_or_defer();
short * sound_play_p2();
undefined4 * dsp_cmd_emit_arrow_indicator();
undefined4 * player_vehicle_dsp_render();
undefined4 * render_town_with_rotation();
char * eeprom_write_verify_block();

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


/* THE COUNT-DOWN / BONUS SCENE NODE CHAIN. W[0x166E4] holds the chain's base
 * as a _W[] BYTE OFFSET (0xB784 for the count-down cast, as the ROM's
 * 0xE0B784), and node field +N is W[base + N] -- the layout anim_load_list,
 * anim_update and scene_node_render use. The transpile addressed it as a
 * HOST-PACKED struct, `((int32_t*)W[0x166E4])[k]` / `*(int *)(W[0x166E4]+N)`,
 * 4-byte fields over 8-byte slots, which nothing else in the tree agreed
 * with; every such access is now BNODE(byte offset). */
static inline intptr_t bnb_base(void) {
  /* the ROM stores the 68K address (0xE0B784); every writer in this tree
   * stores the offset, and the unported ending path still stores a host
   * pointer -- anim_w_off accepts all three */
  intptr_t o = anim_w_off((const void *)W[0x166E4]);
  return o < 0 ? 0 : o;
}
#define BNB        bnb_base()
#define BNODE(off) W[BNB + (intptr_t)(off)]

/* One display-list word at the cursor, bounds-checked (include/ea68k.h). */
int32_t *dl_put(int32_t *p, int32_t v)
{
  const uint8_t *b = (const uint8_t *)p;
  if (b < g_sys.dspram || b + 4 > g_sys.dspram + DSPRAM_SIZE) {
    static int warned;
    if (warned++ == 0)
      fprintf(stderr, "[GUARD] dl_put: cursor %p outside dspram -- word dropped\n", (void *)p);
    return p;
  }
  *p = v;
  return p + 1;
}


/* PROPCYCL_ENDING_PHASE support (gameplay_sub14_init): the state phases
 * 0..4 leave behind that phases 5..12 read, as MAME holds it at counter 3400
 * (tools/overnight/snap_ending.lua, WRAM dump): the screen fade off (the
 * ending opens faded in), and the credits window -- half width 0xE172F8 =
 * 215, half height 0xE172F6 = 161, slide 0xE172F4 = 0 -- which phases 0..4
 * set up -- and the 16-bit gate mask 0xE04708 = 15 (phases 0..4 `ori.w`). The sub14_init entry sets the fade to 0xFF; phase 0 fades it in. */
void ending_phase_seed(int n)
{
  if (n < 5) return;
  W[0xEB16] = 0;
  W16_SET(0x172F4, 0);
  W16_SET(0x172F6, 0xA1);
  W16_SET(0x172F8, 0xD7);
  W16_SET(0x4708, 0xF);                     /* the gate/ring mask, MAME: 15 */
}


/* ---- the intro-orbit scene block: WHY THIS FUNCTION IS STILL WRONG ------
 *
 * MEASURED, not guessed. Three separate defects, and the third blocks the
 * other two:
 *
 * 1. Every `BNODE(N)` below is a HOST-POINTER read.
 *    W[0x166E4] is set to `(intptr_t)&W[0xB784]`, and _W is intptr_t[], so
 *    `+ 0x64` walks 0x64 BYTES = twelve and a half SLOTS and returns a
 *    straddled pair of unrelated words. The M68K holds a WRAM ADDRESS there
 *    (`movea.l (a3),a0; move.l $64(a0),d0` at ROM 0x016B00) and those are
 *    byte offsets into work RAM, i.e. W[0xB784 + N].
 *
 * 2. CORRECTED 2026-09-16 -- half of this entry was WRONG, and the register
 *    mapping is now measured from the prologue at ROM 0x016A1E:
 *        a2 = 0xE00CDC (camera)   a3 = 0xE166E4 (node base)
 *        a4 = 0xE00D00 (player)   d5 = 0xE16720
 *    - "reads $10(a2) = W[0x0CEC] where the C reads W[0x0D10]" is FALSE.
 *      ROM 0x016B0A is `move.l $10(a4),d0`, and a4 = 0xE00D00, so it really
 *      is W[0x0D10]. The C was right; do not "fix" it.
 *    - The coefficient IS wrong: 0x016B12 / 0x016B22 / 0x016B9C are each
 *      `moveq #$c4,dN ; add.l d2,dN`, and the prologue sets
 *      d2 = 0x12C - W[0x16728], which is this function's iVar3. Since
 *      iVar3 = iVar4 + 300, the ROM value is iVar4 + 0x1F0 -- the transpile
 *      lost BIT 8 and rebased it on iVar4, two errors that cancel into one
 *      plausible-looking expression. The `/ 0xf0` DIVISOR at 0x016BAE is
 *      genuinely 0xf0 and must stay.
 *    - ROM 0x016BF2/0x016C04 are `lea ([$10,a2], d0.l, $ffff3333)` and the
 *      same off a4 -- memory indirect, i.e. W[0x0CEC] / W[0x0D10] PLUS
 *      node[0x64] minus 0xCCCD, a VALUE, not an index off those addresses.
 *
 * 3. THE BLOCKER: W[0xB784 + 0x34/0x54/0x58/0x5C/0x64] are ALL ZERO,
 *    because the scene block is never loaded. The M68K initialiser at ROM
 *    0x017026 does
 *        move.l $123c4c.l,-(a7)      ; = 0x123BEC, a ROM table of scene ptrs
 *        movea.l #$e0b784,a0 ; move.l a0,(a2) ; move.l a0,-(a7)
 *        jsr $268ce                  ; FUN_000268ce(0xE0B784, 0x123BEC)
 *        move.w #1,$e0eb06
 *    and OUR call sites pass NULL for that second argument (see :291 and
 *    :3912 below, and CLAUDE.md transpilation fix #16).
 *
 * Fixing 1 and 2 without 3 makes the orbit MUCH worse, and that is measured:
 * with the field reads corrected against an all-zero block the camera runs
 * to |cam-player| = 2.4e8 and the heading jumps +-150 deg per 25 frames,
 * where the uncorrected garbage reads at least stay bounded near the player.
 * So they are deliberately left as they are until the scene block is loaded.
 * Do not "fix" 1 or 2 on their own -- do 3 first.                          */

/* PROPCYCL_ORBITFIX=1 -- the intro-orbit camera corrections, ROM-VERIFIED
 * INDIVIDUALLY but DEFAULT OFF because together they measurably make the
 * orbit WORSE. Register row 140. Same shape as row 127: a correct fix that
 * has to wait for a second one. */
int g_orbitfix = 0;
int g_orbit_ea = 1;    /* PROPCYCL_ORBIT_EA=0 restores the byte-indexed read, main.c */

/* ---- bonus_camera_update ---- */

void bonus_camera_update(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  int local_10;
  int local_c;
  int local_8;
  
  /* ROM 0x016A36..0x016A4C: `subq.w #1 ; tst.w ; bpl ; clr.w` on 0xE16728,
   * then `suba.w` -- a 16-bit counter in the HIGH half of its slot, sharing
   * it with the wind pitch word at 0xE1672A (bonus_sequence_animate). Run
   * full-width, the decrement borrowed through the pitch every frame. */
  W16_SET(0x16728, (int16_t)(W16(0x16728) - 1));
  if ((int16_t)W16(0x16728) < 0) {
    W16_SET(0x16728, 0);
  }
  iVar4 = -(int)(int16_t)W16(0x16728);
  iVar3 = iVar4 + 300;
  if (iVar3 < 0x3c) {
    W[0x16724] = 0;
    if (W16(0xE6A) == 0) {
      W[0x16720] = (iVar3 * 0x3333) / 0x3c;
      iVar4 = -0x592;
      iVar3 = 0x3e3;
    }
    else {
      W[0x16720] = 0;
      iVar4 = (iVar3 * iVar3 * -0x500) / 0xe10 + -0x30e;
      iVar3 = (iVar3 * 0x200) / 0x3c + 0x1e3;
    }
    W[0x0CEC] = -(W[0x16720] + BNODE(0x34));
  }
  else {
    if (W16(0xE6A) == 0) {
      local_c = 0xcccd;
    }
    else {
      local_c = 0x10000;
    }
    local_c = local_c - BNODE(100);
    /* `iVar3 + 0xc4`, NOT `iVar4 + 0xf0`. ROM 0x016B12 / 0x016B22 / 0x016B9C
     * are each `moveq #$c4,dN ; add.l d2,dN`, and the prologue (0x016A48)
     * sets d2 = 0x12C - W[0x16728], which is this function's `iVar3`
     * (`iVar4 + 300`), not `iVar4`. Since iVar3 = iVar4 + 300, the ROM's
     * value is `iVar4 + 0x1F0` -- so the transpile lost BIT 8 of the
     * constant AND rebased it on the wrong variable, which happen to look
     * like one plausible expression. The `/ 0xf0` divisor at 0x016BAE IS
     * genuinely 0xf0 and is left alone.
     * Register mapping, from the prologue at 0x016A1E:
     *   a2 = 0xE00CDC (camera)  a3 = 0xE166E4 (node base)  a4 = 0xE00D00 (player) */
    iVar2 = (g_orbitfix ? (iVar3 + 0xc4) : (iVar4 + 0xf0)) * (W[0x0D10] - local_c);
    if (iVar2 < 0) {
      iVar2 = iVar2 + 7;
    }
    W[0x0CEC] = local_c + ((g_orbitfix ? (iVar3 + 0xc4) : (iVar4 + 0xf0)) * (iVar2 >> 3)) / 0x1c20;
    W[0x16720] = 0x10000 - (BNODE(100) + W[0x0CEC]);
    if (W16(0xE6A) == 0) {
      local_8 = -0x592;
    }
    else {
      local_8 = -0x80e;
    }
    iVar4 = (((uint32_t)(int)(vrd16s(0x20B006 + ((iVar3 * 0x10000 + -0x3c0000) / 0xf0 & 0xfffcU))) >> 4) * 0x5000
            >> 0xc) + local_8 + ((g_orbitfix ? (iVar3 + 0xc4) : (iVar4 + 0xf0)) * (-0x2480 - local_8)) / 0xf0 + -0x2800;
    iVar3 = (iVar3 * 0x1d + -0x6cc) / 0xf0 + 0x3e3;
  }
  W[0x0CF0] = 0;
  W[0x0CE8] = 0;
  if (W[0x16720] < 0x3333) {
    iVar2 = 0;
    iVar1 = 1;
  }
  else {
    /* TWO 68020 MEMORY-INDIRECT EAs RENDERED AS DEREFERENCES -- rows 30/90/109's
     * class. ROM 0x016BF2 is `lea ([$10,a2], d0.l, $ffff3333), a0` and
     * 0x016C04 is the same off a4, with d0 = node[0x64] (loaded at 0x016BEE /
     * 0x016C00). The mode LOADS the long at [reg+0x10] and ADDS the index and
     * the displacement, so the result is the VALUE
     *     W[0x0CEC] + node[0x64] - 0xCCCD      (a2 = 0xE00CDC -> +0x10 = 0x0CEC)
     *     W[0x0D10] + node[0x64] - 0xCCCD      (a4 = 0xE00D00 -> +0x10 = 0x0D10)
     * not a dereference of `&W[...] + node[0x64]`, which is what the
     * transpile wrote and which indexes the _W[] array by a world-space
     * angle. */
    { int _n64 = BNODE(100);
      if (g_orbitfix || g_orbit_ea) {
        iVar2 = (int32_t)W[0x0CEC] + _n64 - 0xcccd;
        iVar1 = (int32_t)W[0x0D10] + _n64 - 0xcccd;
      } else {
        iVar2 = *(int *)((intptr_t)&W[0x0CEC] + _n64) + -0xcccd;
        iVar1 = *(int *)((intptr_t)&W[0x0D10] + _n64) + -0xcccd;
      } }
    /* iVar1 is the DIVISOR of the three divs.l below (0x016C4A/0x016C68/
     * 0x016C88). The 68K would trap on zero; the machine relies on it never
     * being zero here, but our node data is not yet loaded from the ROM scene
     * table, so guard rather than raise SIGFPE. */
    if (iVar1 == 0) iVar1 = 1;
  }
  rotate_euler_zxy_optimized(0,iVar3,iVar4,0,W[0x0CEC],0,&local_10,&local_c,&local_8);
  W[0x0CDC] =
       local_10 +
       BNODE(0x54) +
       (iVar2 * (W[0x0D00] - BNODE(0x54))) / iVar1;
  W[0x0CE0] =
       local_c + BNODE(0x58) +
                 (iVar2 * (W[0x0D04] - BNODE(0x58))) / iVar1;
  W[0x0CE4] =
       local_8 + BNODE(0x5c) +
                 (iVar2 * (W[0x0D08] - BNODE(0x5c))) / iVar1;
  camera_grid_calc_position();
  return;
}


/* ---- bonus_model_init_simple ---- */

void bonus_model_init_simple(void)

{
  /* ROM 0x016EF6: base 0xE0B784, and the chain is the ROM list whose
   * pointer is held at 0x123C4C (= 0x123BEC: 18 descriptors, the marshal's
   * 17 parts and node 17, model 0x250, which the draw walk stops on). */
  W[0x166E4] = 0xB784;
  anim_load_list(0xB784, vrd32(0x123C4C));
  BNODE((0xec) * 4) = 0x800;
  BNODE((0x22a) * 4) = (BNODE((0x22a) * 4) * 100) / 0x41;
  BNODE((0x22b) * 4) = (BNODE((0x22b) * 4) * 100) / 0x41;
  BNODE((0xc) * 4) = W[0x172A0];
  BNODE((0xec) * 4) = 0x400;
  BNODE((0x14c) * 4) = BNODE((0x14c) * 4) - BNODE((0xec) * 4);
  BNODE((0x1ac) * 4) = BNODE((0x1ac) * 4) - BNODE((0xec) * 4);
  BNODE((0x22c) * 4) = BNODE((0xec) * 4) + BNODE((0x22c) * 4);
  BNODE((0x22b) * 4) = BNODE((0x22b) * 4) + 0x32;
  return;
}


/* ---- bonus_sequence_animate ---- */

void bonus_sequence_animate(int param_1)

{
  /* ROM 0x01791A -- the COUNT-DOWN / INTRO-ORBIT scene, once per sub-5
   * frame with param_1 = the remaining count (W[0x16968]). Everything here
   * was checked against the disassembly line by line (2026-09-22); it had
   * been stubbed with a bare `return;`, so the start-line MARSHAL -- the
   * 17-part figure in the chain at 0xE0B784, records 1097-1113 -- and the
   * orbit's wind sound never ran at all. The chain is addressed as _W[]
   * byte offsets through BNODE (see the note above the macro). */
  short sVar1;
  undefined4 *puVar2;
  /* SIGNED 32-bit, like every 68K data register. Ghidra typed these
   * uint32_t, and C then widens a uint32_t added to a 64-bit _W[] slot as a
   * POSITIVE number: the flight controller's step `W[0x17298] += local_14`
   * with local_14 = -3120173 added 4,291,847,123 instead, and the next line's
   * `W[0x17298] >> 9` came out 2^32 >> 9 = 2^23 too large -- the marshal's
   * climb speed 0xE166EC read 8,413,352 where MAME holds 24,744 (orbit frame
   * 135), and he was launched hundreds of thousands of units off instead of
   * flying away (register row 180). */
  int32_t uVar3;
  int iVar4;
  int iVar5;
  int iVar6;
  int *local_24;
  int32_t local_20;
  int local_1c;
  undefined4 local_18;
  int32_t local_14;
  int32_t local_10;
  int local_c;
  uint8_t *local_8;
  
  iVar5 = -param_1 + 0x14a;
  { extern int g_stagedbg;   /* [MARSH2]: node 0 every 10 orbit frames, the line tools' marsh2.lua prints on MAME */
    if (g_stagedbg && ((iVar5 + 1) % 10 == 0 || iVar5 >= 319))
      printf("[MARSH2] orb=%d f%d cnt=%d | n0 mdl=%d pos=(%d,%d,%d)\n", iVar5 + 1, (int)g_sys.frame_count,
             param_1, (int)W[BNB], (int)W[BNB + 0x24], (int)W[BNB + 0x28], (int)W[BNB + 0x2C]); }
  { extern int g_stagedbg;
    if (g_stagedbg && (iVar5 == 0 || iVar5 == 30 || iVar5 == 90 || iVar5 == 150)) {
      intptr_t nb = BNB; int k;
      printf("[MARSH] f%d d2=%d base=0x%lX cam=(%d,%d,%d) hdg=%d ply=(%d,%d,%d) E6A=%d\n", (int)g_sys.frame_count, iVar5, (long)nb,
             (int)W[0x0CDC], (int)W[0x0CE0], (int)W[0x0CE4], (int)W[0x0CEC],
             (int)W[0x0D00], (int)W[0x0D04], (int)W[0x0D08], (int)W16(0xE6A));
      for (k = 0; k < 20; k++) {
        intptr_t n = nb + k * 0x80;
        printf("  n%-2d m=%-5d lnk=%-3d len=%-4d dof=%04x pos=(%d,%d,%d) ang=(%d,%d,%d) slot=%d\n", k,
               (int)W[n], W_HI16(n + 4), W_LO16(n + 4), (uint16_t)W_HI16(n + 8),
               (int)W[n + 0x24], (int)W[n + 0x28], (int)W[n + 0x2C],
               (int)W[n + 0x30], (int)W[n + 0x34], (int)W[n + 0x38], (int)W[n + 0x78]);
      }
    } }
  if (iVar5 == 0) {
    W16_SET(0x1672A, 0xb000);             /* move.w -- the LOW half of 0x16728 */
  }
  local_14 = W[0x17298] / -0x40000 + 0x42;
  if ((int)local_14 < 0x32) {
    local_14 = 0x32;
  }
  if (iVar5 < 0x3c) {
    local_14 = (int)(iVar5 * (local_14 - 0xff)) / 0x3c + 0xff;
  }
  if (0xdc < iVar5) {
    local_14 = (int)((-param_1 + 0x6e) * (0xff - local_14)) / 0x6e + local_14;
  }
  if (0xff < (int)local_14) {
    local_14 = 0xff;
  }
  iVar6 = W[0x17298];
  if (W[0x17298] < 0) {
    iVar6 = W[0x17298] + 0xff;
  }
  local_10 = ((iVar6 >> 8) << 10) / 0x4000 + 0xb000;
  if (0xcc00 < (int)local_10) {
    local_10 = 0xcc00;
  }
  /* 0x017A14: `moveq #0,d1 ; move.w $e1672a,d1` -- ZERO-extended -- then
   * `asr.l #3` and a WORD add back */
  W16_SET(0x1672A, (uint16_t)((int)(local_10 - (uint32_t)(uint16_t)W16(0x1672A)) >> 3)
                   + (uint16_t)W16(0x1672A));
  FUN_0000fc9a((short)local_14,(uint16_t)W16(0x1672A));
  { extern int g_stagedbg; int orb = iVar5 + 1;
    if (g_stagedbg && (orb % 30 == 1 || orb >= 326))
      printf("[WIND] orb=%d vol=%04X pitch=%04X 1672A=%04X 17298=%d\n", orb,
             comms_r16(g_sys.commsram, 0x118), comms_r16(g_sys.commsram, 0x11A),
             (uint16_t)W16(0x1672A), (int)W[0x17298]); }
  if (iVar5 == 0) {
    sound_play(0x2B);
  }
  if (iVar5 == 0x149) {
    sound_stop(0x2B);
    FUN_0000fc9a(0xff,0xb000);
  }
  if (iVar5 == 0xdc) {
    debug_flight_dispatch();
  }
  if (iVar5 < 0xdc) {
    if ((W16(0xE6A) != 0) && (iVar5 < 0x3c)) {
      if (W16(0xE6A) == 7) {
        iVar6 = -0x1400;
      }
      else {
        iVar6 = -0xe00;
      }
      /* the outputs are node 0's +0x24/+0x28/+0x2C (the transpile passed a
       * short-truncated pointer plus 0x24 as the addresses) */
      { int32_t _ox, _oy, _oz;
        rotate_euler_zxy_optimized
                  (0x1c00,0,W[0x16724] + iVar6,W[0x0D0C],W[0x0D10] + 0x4000,
                   W[0x0D14],&_ox,&_oy,&_oz);     /* 0x017AAE move.l -- no truncation */
        BNODE(0x24) = _ox; BNODE(0x28) = _oy; BNODE(0x2c) = _oz; }
      iVar6 = BNODE((9) * 4) + W[0x0D00];
      BNODE((9) * 4) = iVar6;
      BNODE((0x15) * 4) = iVar6;
      iVar6 = BNODE((10) * 4) + W[0x0D04] + 0x3dc;
      BNODE((10) * 4) = iVar6;
      BNODE((0x16) * 4) = iVar6;
      iVar6 = BNODE((0xb) * 4) + W[0x0D08];
      BNODE((0xb) * 4) = iVar6;
      BNODE((0x17) * 4) = iVar6;
    }
    if (0x3b < iVar5) {
      if (iVar5 < 0x5a) {
        BNODE((10) * 4) =
             BNODE((10) * 4) -
             (int)(vrd16s(0x20B004 + ((iVar5 * 0x8000 + -0x1e0000) / 0x1e & 0xfffcU))) /
             0x800;
      }
      else if ((W[0x17294] < BNODE((10) * 4)) || (0x9f < iVar5)) {
        if (iVar5 < 0x82) {
          W[0x166EC] = W[0x166EC] + -0x59e;
        }
        else {
          local_14 = (W[0x1729C] - W[0x166EC]) * 0x100 - (W[0x17298] >> 4);
          if (0x4000000 < (int)local_14) {
            local_14 = 0x4000000;
          }
          W[0x17298] = W[0x17298] + local_14;
          if (W[0x17298] < 0) {
            W[0x17298] = 0;
          }
          W[0x166EC] = (W[0x17298] >> 9) + -0x146e + W[0x166EC];
          if (W16(0xE6A) == 6) {
            iVar6 = 0x6000;
          }
          else if (W16(0xE6A) == 7) {
            iVar6 = -0x3000;
          }
          else {
            iVar6 = -0x2000;
          }
          W[0x166E8] = (((iVar6 - W[0x0D10]) - BNODE((0xd) * 4)) * 4 - (W[0x166E8] >> 1))
                         + W[0x166E8];
          BNODE((0xd) * 4) = (W[0x166E8] >> 9) + BNODE((0xd) * 4);
        }
      }
      else {
        W[0x166EC] = ((W[0x17294] - BNODE((10) * 4)) * 0x20 - (W[0x166EC] >> 5)) +
                       W[0x166EC];
      }
    }
    if (0x3b < iVar5) {
      if (iVar5 < 0xa0) {
        iVar6 = (0x2000 - W[0x1729C]) / 0x18;
      }
      else {
        if ((iVar5 < 0xb4) || (W16(0xE6A) != 7)) {
          iVar6 = 0x39ded;
        }
        else {
          iVar6 = 0x22b8e;
        }
        iVar6 = iVar6 - W[0x1729C] >> 6;
      }
      W[0x1729C] = iVar6 + W[0x1729C];
    }
    if (0x9f < iVar5) {
      if (iVar5 < 0xbe) {
        if (W16(0xE6A) == 6) {
          iVar6 = 0x880;
        }
        else {
          iVar6 = -0x880;
        }
        W[0x166F0] = ((iVar6 - W[0x172A0]) * 4 - (W[0x166F0] >> 3)) + W[0x166F0];
        W[0x172A0] = (W[0x166F0] >> 9) + W[0x172A0];
        if (W16(0xE6A) == 6) {
          iVar6 = 0x1000;
        }
        else {
          iVar6 = -0x3000;
        }
        W[0x172AC] = ((iVar6 - BNODE((0xe) * 4)) * 8 - (W[0x172AC] >> 1)) + W[0x172AC];
      }
      else {
        if (W16(0xE6A) == 0) {
          iVar6 = -0x1000;
        }
        else if (W16(0xE6A) == 6) {
          iVar6 = 0x1000;
        }
        else {
          iVar6 = -0xd00;
        }
        W[0x166F0] = ((iVar6 - W[0x172A0]) * 4 - (W[0x166F0] >> 3)) + W[0x166F0];
        W[0x172A0] = (W[0x166F0] >> 9) + W[0x172A0];
        W[0x172AC] = W[0x172AC] >> 2;
      }
    }
    if (W[0x17294] < BNODE((10) * 4)) {
      local_8 = &R[0x20B002] + (W[0x172A0] & 0xfffc);
      iVar6 = math_slope_angle(-(vrd16s(0x20B004 + (W[0x172A0] & 0xfffc))),0x8000);
      local_14 = (iVar6 + W[0x172A0]) - 0x4000;
      local_10 = W[0x172A0] - local_14;
    }
    else {
      local_14 = W[0x172A0];
      local_10 = 0x400;
    }
    W[0x172A4] = ((local_14 - BNODE((0xc) * 4)) * 0x80 - (W[0x172A4] >> 3)) + W[0x172A4];
    BNODE((0xc) * 4) = (W[0x172A4] >> 10) + BNODE((0xc) * 4);
    W[0x172A8] = ((local_10 - BNODE((0xec) * 4)) * 0x80 - (W[0x172A8] >> 3)) + W[0x172A8];
    BNODE((0xec) * 4) = (W[0x172A8] >> 10) + BNODE((0xec) * 4);
    BNODE((0xe) * 4) = (W[0x172AC] >> 9) + BNODE((0xe) * 4);
    if ((iVar5 < 0xa0) || (W16(0xE6A) == 6)) {
      local_14 = ((BNODE((10) * 4) - W[0x17294]) * 0x80) / -0x100 + 0x20;
      if ((int)local_14 < -0x60) {
        local_14 = 0xffffffa0;
      }
      if (0x20 < (int)local_14) {
        local_14 = 0x20;
      }
      local_20 = local_14;
      local_1c = (BNODE((10) * 4) - W[0x17294]) + 0x33d;
      local_18 = 0;
      /* ROM 0x017E84 `pea $80(a0)`: NODE 1 of the chain, and the target is
       * the three consecutive stack longs local_20/1c/18 -- an array. The two
       * lengths are `move.l #$5591a` / `#$5241d` (0x017E72/78), LONG
       * immediates; Ghidra kept only their low 16 bits. */
      { int32_t _tgt[3] = { (int32_t)local_20, (int32_t)local_1c, (int32_t)local_18 };
        ending_character_pose_calc((int)(BNB + 0x80),(undefined4 *)_tgt,0x5591a,0x5241d,0x1d8,0x164); }
    }
    else {
      W[0x16700] = ((W[0x172B4] - BNODE((0x2c) * 4)) * 0x10 - (W[0x16700] >> 1)) +
                     W[0x16700];
      BNODE((0x2c) * 4) = (W[0x16700] >> 9) + BNODE((0x2c) * 4);
      W[0x16704] = ((W[0x172B8] - BNODE((0x4c) * 4)) * 0x10 - (W[0x16704] >> 1)) +
                     W[0x16704];
      BNODE((0x4c) * 4) = (W[0x16704] >> 9) + BNODE((0x4c) * 4);
      W[0x16708] = ((W[0x172BC] - BNODE((0x4d) * 4)) * 0x10 - (W[0x16708] >> 1)) +
                     W[0x16708];
      BNODE((0x4d) * 4) = (W[0x16708] >> 9) + BNODE((0x4d) * 4);
      W[0x1670C] = ((W[0x172C0] - BNODE((0x6c) * 4)) * 0x10 - (W[0x1670C] >> 1)) +
                     W[0x1670C];
      BNODE((0x6c) * 4) = (W[0x1670C] >> 9) + BNODE((0x6c) * 4);
      W[0x16710] = ((W[0x172C4] - BNODE((0x8c) * 4)) * 0x10 - (W[0x16710] >> 1)) +
                     W[0x16710];
      BNODE((0x8c) * 4) = (W[0x16710] >> 9) + BNODE((0x8c) * 4);
      W[0x16714] = ((W[0x172C8] - BNODE((0xac) * 4)) * 0x10 - (W[0x16714] >> 1)) +
                     W[0x16714];
      BNODE((0xac) * 4) = (W[0x16714] >> 9) + BNODE((0xac) * 4);
      W[0x16718] = ((W[0x172CC] - BNODE((0xad) * 4)) * 0x10 - (W[0x16718] >> 1)) +
                     W[0x16718];
      BNODE((0xad) * 4) = (W[0x16718] >> 9) + BNODE((0xad) * 4);
      W[0x1671C] = ((W[0x172D0] - BNODE((0xcc) * 4)) * 0x10 - (W[0x1671C] >> 1)) +
                     W[0x1671C];
      BNODE((0xcc) * 4) = (W[0x1671C] >> 9) + BNODE((0xcc) * 4);
    }
    rotate_euler_zxy_optimized
              (0,W[0x166EC] >> 9,0,BNODE((0xec) * 4) - BNODE((0xc) * 4),-BNODE((0xd) * 4),
               -BNODE((0xe) * 4),&local_14,&local_10,&local_c);
    BNODE((9) * 4) = local_14 + BNODE((9) * 4);
    uVar3 = local_10;
    if ((0xb3 < iVar5) && (W16(0xE6A) == 7)) {
      iVar6 = iVar5 * 0x2b9 + -0x1ea14;
      if (iVar6 < 0) {
        iVar6 = iVar5 * 0x2b9 + -0x1e915;
      }
      uVar3 = local_10 + (iVar6 >> 8) * -6;
    }
    BNODE((10) * 4) = uVar3 + BNODE((10) * 4);
    BNODE((0xb) * 4) = local_c + BNODE((0xb) * 4);
    iVar6 = W[0x17298];
    if (W[0x17298] < 0) {
      iVar6 = W[0x17298] + 0x7f;
    }
    iVar6 = BNODE((0x10d) * 4) + (iVar6 >> 7) + 0x1000;
    BNODE((0x10d) * 4) = iVar6;
    BNODE((0x12d) * 4) = -iVar6;
    if (iVar5 < 0x82) {
      /* 0x0180B6/0x0180FC: both arguments are pushed as LONGS */
      local_10 = math_atan2((int32_t)(W[0x0CDC] - BNODE((9) * 4)),
                            (int32_t)(W[0x0CE4] - BNODE((0xb) * 4)));
      { int _cd = (int)(vrd16s(0x20B006 + (local_10 & 0xfffc))) >> 3;
        iVar6 = math_atan2((int32_t)((W[0x0CE0] - BNODE((10) * 4)) + -0x27b),
                           (int32_t)(((int32_t)(W[0x0CE4] - BNODE((0xb) * 4)) * 0x1000) /
                                     (_cd ? _cd : 1))); }
      local_14 = -iVar6;
      if ((int)local_14 < -0x8000) {
        local_14 = local_14 + 0x10000;
      }
      if (0x8000 < (int)local_14) {
        local_14 = local_14 - 0x10000;
      }
      if ((int)local_14 < -0x3000) {
        local_14 = 0xffffd000;
      }
      if (0x3000 < (int)local_14) {
        local_14 = 0x3000;
      }
      iVar6 = BNODE((0xd) * 4) + local_10;
      local_10 = -iVar6 + 0x8000;
      if ((int)local_10 < -0x8000) {
        local_10 = -iVar6 + 0x18000;
      }
      if (0x8000 < (int)local_10) {
        local_10 = local_10 - 0x10000;
      }
      if ((int)local_10 < -0x3800) {
        local_10 = 0xffffc800;
      }
      if (0x3800 < (int)local_10) {
        local_10 = 0x3800;
      }
      W[0x166F4] = (local_14 - BNODE((0x20c) * 4)) * 0x40 - (W[0x166F4] >> 3);
      iVar4 = (local_10 - BNODE((0x20d) * 4)) * 0x40;
      iVar6 = W[0x166F8] >> 3;
    }
    else {
      W[0x166F4] = BNODE((0x20c) * 4) * -0x10 - (W[0x166F4] >> 2);
      iVar4 = BNODE((0x20d) * 4) * -0x10;
      iVar6 = W[0x166F8] >> 2;
    }
    W[0x166F8] = iVar4 - iVar6;
    BNODE((0x20c) * 4) = (W[0x166F4] >> 9) + BNODE((0x20c) * 4);
    BNODE((0x20d) * 4) = (W[0x166F8] >> 9) + BNODE((0x20d) * 4);
    *(int32_t*)W[0x0CA4] = 0x8002;
    ((int32_t*)W[0x0CA4])[1] = 0;
    ((int32_t*)W[0x0CA4])[2] = 0x8010;
    ((int32_t*)W[0x0CA4])[3] = 3;
    puVar2 = (int32_t*)W[0x0CA4] + 5;
    ((int32_t*)W[0x0CA4])[4] = 0xffffe000;
    W[0x0CA4] = W[0x0CA4] + 6 * 4;
    *puVar2 = 0xffffffff;
    if (W16(0xE6A) != 8) {
      bonus_model_dsp_setup();
      /* 0x018272..0x01829C: walk the chain from node 1 until the model word
       * is 0x250 -- node 17 of the ROM list at 0x123BEC, which is what makes
       * it a sentinel -- drawing each node. local_24 then holds that node,
       * which is drawn scaled and attached to its parent below. */
      { intptr_t _nb = BNB; int _k;
        for (_k = 0; _k < 64; _k++) {
          _nb += 0x80;
          if (_nb < 0 || _nb + 0x80 > WORK_RAM_SIZE) break;
          if ((int32_t)W[_nb] == 0x250) break;
          scene_node_render((int *)&_W[_nb]);
        }
        local_24 = (int *)&_W[_nb]; }
      if ((0x9f < iVar5) && (W16(0xE6A) != 0)) {
        *(int32_t*)W[0x0CA4] = 0x8010;
        ((int32_t*)W[0x0CA4])[1] = 3;
        W[0x4704] = 0x1000;
        puVar2 = (int32_t*)W[0x0CA4] + 3;
        ((int32_t*)W[0x0CA4])[2] = 0x1000;
        W[0x0CA4] = W[0x0CA4] + 4 * 4;
        *puVar2 = 0xffffffff;
      }
      scene_node_render_scaled((intptr_t)local_24,0x5333);
    }
    if (W16(0xE6A) == 0) {
      if (iVar5 < 0xa0) {
        dsp_emit_articulated_model((intptr_t)local_24,0x5333);
        result_screen_render_score();
      }
    }
    else if (iVar5 < 0x3c) {
      iVar5 = BNODE((0x270) * 4);
      BNODE((0x270) * 4) = iVar5 + 2;
      if ((0x30 < iVar5 + 2) && (W16(0xE6A) != 8)) {
        BNODE((0x270) * 4) = 0x30;
      }
      BNODE((0x26a) * 4) = BNODE((0x26a) * 4) - BNODE((0x270) * 4);
      result_screen_render_bonus();
    }
    else if (W16(0xE6A) != 8) {
      dsp_emit_articulated_model((intptr_t)local_24,0x5333);
      result_screen_render_targets(0x9f < iVar5);
    }
    puVar2 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = 0x8010;
    W[0x0CA4] = puVar2;
    puVar2 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = (int32_t)0xffffffff;
    W[0x0CA4] = puVar2;
  }
  { extern int g_stagedbg; int orb = 332 - param_1;   /* [MF]: the per-frame line marsh3.lua prints on MAME */
    if (g_stagedbg && orb >= 88 && orb <= 135)
      printf("[MF] orb=%d cnt=%d pos=(%d,%d,%d) ang=(%d,%d,%d) EC=%d E8=%d F0=%d 94=%d 98=%d 9C=%d A0=%d A4=%d A8=%d AC=%d n7ax=%d\n",
             orb, param_1 - 1, (int)BNODE(0x24), (int)BNODE(0x28), (int)BNODE(0x2C),
             (int)BNODE(0x30), (int)BNODE(0x34), (int)BNODE(0x38),
             (int)W[0x166EC], (int)W[0x166E8], (int)W[0x166F0], (int)W[0x17294], (int)W[0x17298],
             (int)W[0x1729C], (int)W[0x172A0], (int)W[0x172A4], (int)W[0x172A8], (int)W[0x172AC],
             (int)BNODE(0x3B0)); }
  return;
}


/* ---- ending_town_overview_draw ---- */

/* ROM 0x02C6FC, ported from the machine code: the ferris wheel at rest, the
 * 0x1605C structure and the castle gate about the stack local (0, -0x5000, 0)
 * -- EFRAME(4) here, since W[0x46F8] is pointed at it for 0x1605C. */
void ending_town_overview_draw(void)
{
  uint32_t lf = EFRAME(4);
  int32_t *p;
  W[0x4704] = 0;
  W[0x169E8] = 0;
  W16_SET(0x4708, 0xffff);   /* ROM 0x02C71C: move.w #$ffff,$e04708 */
  e_wr32(lf + 8, 0);
  e_wr32(lf, 0);
  e_wr32(lf + 4, -0x5000);
  ending_ferris_wheel_draw(-0x5000, -0x5000, 0);
  W[0x46F8] = (int32_t)lf;
  e_setcur((int32_t *)render_player_bike_model((int *)e_cur()));
  e_wr32(lf,     e_rd32(lf)     - (int32_t)W[0x0CDC]);
  e_wr32(lf + 4, e_rd32(lf + 4) - (int32_t)W[0x0CE0]);
  e_wr32(lf + 8, e_rd32(lf + 8) - (int32_t)W[0x0CE4]);
  dsp_cmd_emit_object_mode_8000();
  p = e_cur();
  *p++ = 0x8010; *p++ = 3;
  W[0x4704] = -0x2dd;                       /* lea $fd23.w: sign-extended */
  *p++ = -0x2dd; *p++ = -1;
  e_setcur((int32_t *)render_gate_or_ring(0x36424, lf, (uint32_t *)p));
}


/* ---- results_bonus_dispatch @ 0x025B80 ---- */
/* The NOVICE page's rank ICON and its numbers, drawn for ranks 3..5 once the
 * rank reveal has landed. Ported from the ROM (0x025B80..0x025E48).
 *
 * 0x025BAC..0x025BC8: d0 = W[0x169C4] - 3 ; subq.l #2 ; bhi out ;
 * `move.w $25bd0(pc,d0.w*2)` (ext 0x020C: scale 2, and d0 is already -2..0,
 * so the reads land at 0x25BCC/CE/D0) ; `jmp $25bcc(pc,d0.w)`. Table
 * [0x0006, 0x00BA, 0x0152] -> 0x025BD2 (rank 3), 0x025C86 (rank 4),
 * 0x025D1E (rank 5). Each case places one icon under VIEWPORT 6 at the drop
 * height W[0x169C8] (case 9 of results_screen_update_normal lowers it to
 * 0x350), and only once it has landed draws the digits as sprites
 * 0x1CF + digit (a2 = sprite_draw_2d, first argument 0).
 *
 *   rank 3: icon 0x37A if W16(0xE68) == 1 else 0x379 at (0x90, -208); the
 *           E68 count as one or two digits at y 0x14F.
 *   rank 4: icon 0x377 at (-272, -32); the points still needed for rank 5,
 *           (threshold5 - score) / 50 + 1, as up to two digits at y 0xB5
 *           (threshold5 = 0x15C488 + course*32, the table FUN_000256be reads).
 *   rank 5: icon 0x35F if FUN_00031d40 found no ranking place, else 0x35D
 *           when the score beats the course's best (W[0x4088 + course*8],
 *           `$e04088(d0.l*8)`, ext 0x0FB0) or 0x35E; an animated sprite
 *           0x1D9 + ((fc >> 2) & 7); and for 0x35E the ranking place.
 *
 * The old stub's comment already located the one trap here: 0x15C488 is a
 * ROM table read with vrd32, never a W[] slot. */
void results_bonus_dispatch(void)
{
  int32_t r = W[0x169C4] - 3;
  if ((uint32_t)r > 2) return;
  if (r == 0) {                                          /* 0x025BD2 */
    int32_t model = (W16(0xE68) == 1) ? 0x37a : 0x379;
    dsp_cmd_place_object_rotated_abs(6, model, 0x90, -0xd0, W[0x169C8], 0, 0, 0, 0);
    if (W[0x169C8] != 0x350) return;
    int32_t v = W16(0xE68);                              /* move.w (a4) ; ext.l */
    if (v / 10 != 0) {
      sprite_draw_2d(0, 0x1cf + v / 10, 0x160, 0x14f, 0, 0x18, 0x20, 0, 0);
      sprite_draw_2d(0, 0x1cf + v % 10, 0x178, 0x14f, 0, 0x18, 0x20, 0, 0);
    } else {
      sprite_draw_2d(0, 0x1cf + v, 0x170, 0x14f, 0, 0x20, 0x20, 0, 0);
    }
  } else if (r == 1) {                                   /* 0x025C86 */
    int32_t d2 = (vrd32s(0x15C488 + W[0x0E0C] * 0x20) - W[0x169AC]) / 0x32 + 1;
    dsp_cmd_place_object_rotated_abs(6, 0x377, -0x110, -0x20, W[0x169C8], 0, 0, 0, 0);
    if (W[0x169C8] != 0x350) return;
    sprite_draw_2d(0, 0x1cf + d2 % 10, 0x6f, 0xb5, 0, 0x20, 0x20, 0, 0);
    d2 /= 10;
    if (d2 != 0)
      sprite_draw_2d(0, 0x1cf + d2 % 10, 0x4f, 0xb5, 0, 0x20, 0x20, 0, 0);
  } else {                                               /* 0x025D1E */
    int32_t d4 = FUN_00031d40(W[0x0E0C], W[0x169AC]);
    int32_t model;
    if (d4 == 0) model = 0x35f;
    else if ((int32_t)(W[0x4088 + W[0x0E0C] * 8] & 0xffffff) < W[0x169AC]) model = 0x35d;
    else model = 0x35e;
    dsp_cmd_place_object_rotated_abs(6, model, -0x110, -0x24, W[0x169C8], 0, 0, 0, 0);
    if (W[0x169C8] != 0x350) return;
    sprite_draw_2d(0, 0x1d9 + (int32_t)((W[0x0C98] >> 2) & 7), 0x5e, 0xd0, 0, 0x20, 0x20, 0, 0);
    if (model != 0x35e) return;
    int32_t d2 = d4;
    if (d2 / 10 != 0) {
      sprite_draw_2d(0, 0x1cf + d2 % 10, 0xad, 0x14c, 0, 0x20, 0x20, 0, 0);
      d2 /= 10;
      sprite_draw_2d(0, 0x1cf + d2 % 10, 0x8d, 0x14c, 0, 0x20, 0x20, 0, 0);
    } else {
      sprite_draw_2d(0, 0x1cf + d2 % 10, 0x9d, 0x14c, 0, 0x20, 0x20, 0, 0);
    }
  }
}


/* ---- results_rank_blink ---- */

void results_rank_blink(void)

{
  int iVar1;
  
  /* ROM 0x025F2A: a3 = 0xE169E0. Both flags are 16-bit -- 0x169DE is the LOW
   * half of slot 0x169DC (`move.w #1` / `tst.w $e169de.l`), 0x169E0 the HIGH
   * half of its own slot (`clr.w (a3)`, `move.w (a3)`, `addq.w #1,(a3)`). */
  if (W[0x16978] == 9) {
    W16_SET(0x169DE, 1);
    W16_SET(0x169E0, 0);
  }
  if (W16(0x169DE) != 0) {
    iVar1 = W[0x169C4] * 0x10;
    if ((W16(0x169E0) & 0x10) == 0) {
      text_draw_rect_solid
                ((short)vrd32(0x375FC + iVar1),
                 (short)vrd32(0x37600 + iVar1),
                 vrd32(0x37604 + iVar1));
    }
    else {
      /* 0x025F66..0x025F7A, right to left: `pea $7.w` (palette), `move.l
       * $c(a0),d0 ; move.w d0,-(a7)` (base tile, a WORD), then the longs at
       * +8, +4, +0 -- the same 0x375FC record the solid branch reads. The
       * decompile had lost every argument (register row 62's class). */
      text_draw_rect_blink((short)vrd32(0x375FC + iVar1),
                           (short)vrd32(0x37600 + iVar1),
                           vrd32(0x37604 + iVar1),
                           (uint16_t)vrd32(0x37608 + iVar1), 7);
    }
    W16_SET(0x169E0, W16(0x169E0) + 1);
  }
  return;
}


/* ---- results_tilemap_scroll_setup ---- */

void results_tilemap_scroll_setup(void)

{
  /* ROM 0x025FA0: `move.w #$ff70,$8a0000.l ; clr.w $8a0002.l` -- two 16-bit
   * registers. mem_write32(0x8A0000, 0xff70) wrote 0x0000 to 0x8A0000 and
   * 0xFF70 to 0x8A0002, which the next line then cleared. */
  mem_write16(0x8A0000, 0xff70);
  mem_write16(0x8A0002, 0);
  results_graphics_load();
  return;
}


/* ---- bonus_model_dsp_setup ---- */

void bonus_model_dsp_setup(void)

{
  /* ROM 0x016E26 -- the chain's ROOT (node 0 at W[0x166E4]) as a 15-word
   * 0x8008 into its own slot, offset (0, 0x249, 0), then an 0x800A drawing
   * its model there at the camera-relative position less 0x249 in Y (so
   * the offset cancels on the root and lifts every child). The pairs are
   * `move.l (a3) ; move.l $2(a3)` off 0x20B002 + angle -- sin-first, the
   * low halves (register row 87). The transpile read every node field as
   * a SLOT of its own (W[0x1675C], W[0x16708] ...: 0x166E4 + the field
   * offset) instead of through the base held there, and wrote cos first. */
  intptr_t n = BNB;
  int32_t *cp = (int32_t *)W[0x0CA4];
  uint32_t a;
  int k;
  if (n < 0 || n + 0x80 > WORK_RAM_SIZE) return;
  *cp++ = 0x8008; *cp++ = (int32_t)W[n + 0x78];
  *cp++ = 0; *cp++ = 0; *cp++ = 0x249; *cp++ = 0;
  *cp++ = 1;
  for (k = 0; k < 3; k++) {
    a = (uint32_t)W[n + 0x30 + k * 4] & 0xfffc;
    *cp++ = vrd16s(0x20B004 + a); *cp++ = vrd16s(0x20B006 + a);
  }
  *cp++ = W_LO16(n + 8);
  *cp++ = -1;
  *cp++ = 0x800a; *cp++ = (int32_t)W[n]; *cp++ = (int32_t)W[n + 0x78];
  W[0xEB08] = (int32_t)W[n + 0x24] - (int32_t)W[0x0CDC];          *cp++ = (int32_t)W[0xEB08];
  W[0xEB0C] = (int32_t)W[n + 0x28] - (int32_t)W[0x0CE0] - 0x249;  *cp++ = (int32_t)W[0xEB0C];
  W[0xEB10] = (int32_t)W[n + 0x2C] - (int32_t)W[0x0CE4];          *cp++ = (int32_t)W[0xEB10];
  W[0x0CA4] = (intptr_t)cp;
  return;
}

/* ---- bonus_render_vehicle ---- */

void bonus_render_vehicle(void)

{
  uint32_t uVar1;
  undefined4 *puVar2;
  
  if (W16(0xE6A) == 8) {
    dsp_cmd_emit_object_mode_8002();
    if ((W[0x166D0] - W[0x0CE0]) + -0x3cc < 0) {
      *(int32_t*)W[0x0CA4] = 0x8010;
      ((int32_t*)W[0x0CA4])[1] = 3;
      W[0x4704] = W[0x3EF4] + -0x5000;
      puVar2 = (int32_t*)W[0x0CA4] + 3;
      ((int32_t*)W[0x0CA4])[2] = W[0x4704];
      W[0x0CA4] = W[0x0CA4] + 4 * 4;
      *puVar2 = 0xffffffff;
    }
    *(int32_t*)W[0x0CA4] = 0x250;
    ((int32_t*)W[0x0CA4])[1] = W[0x166CC] - W[0x0CDC];
    ((int32_t*)W[0x0CA4])[2] = (W[0x166D0] - W[0x0CE0]) + -0x3cc;
    ((int32_t*)W[0x0CA4])[3] = W[0x166D4] - W[0x0CE4];
    uVar1 = W[0x166D8] - 0x4000U & 0xfffc;
    ((int32_t*)W[0x0CA4])[4] = (vrd16s(0x20B004 + uVar1)) * 0x5333 >> 0xf;
    ((int32_t*)W[0x0CA4])[5] = (vrd16s(0x20B006 + uVar1)) * 0x5333 >> 0xf;
    uVar1 = W[0x166DC] & 0xfffc;
    ((int32_t*)W[0x0CA4])[6] = (vrd16s(0x20B004 + uVar1)) * 0x5333 >> 0xf;
    ((int32_t*)W[0x0CA4])[7] = (vrd16s(0x20B006 + uVar1)) * 0x5333 >> 0xf;
    uVar1 = W[0x166E0] & 0xfffc;
    ((int32_t*)W[0x0CA4])[8] = (vrd16s(0x20B004 + uVar1)) * 0x5333 >> 0xf;
    puVar2 = (int32_t*)W[0x0CA4] + 10;
    ((int32_t*)W[0x0CA4])[9] = (vrd16s(0x20B006 + uVar1)) * 0x5333 >> 0xf;
    W[0x0CA4] = W[0x0CA4] + 0xb * 4;
    *puVar2 = 4;
  }
  return;
}

/* ---- ending_character_pose_calc ---- */

void ending_character_pose_calc
               (int param_1,undefined4 *param_2,int param_3,int param_4,int param_5,short param_6)

{
  /* ROM 0x02D98C -- two-bone IK for a waving arm / bending legs: aims the
   * node chain starting at param_1 at the target vector param_2[0..2].
   * param_1 is the chain's first node as a _W[] byte offset (or a 68K WRAM
   * address) and node field +N is W[n + N]; param_2 is a real int32[3].
   * The transpile wrote the node as a HOST-PACKED struct and read
   * param_2[1]/[2] from locals that were only adjacent on the 68K stack. */
  uint32_t uVar1;
  int iVar2;
  int iVar3;
  int32_t *v = (int32_t *)param_2;
  intptr_t n = param_1;
  if (n >= 0xE00000 && n < 0xE00000 + WORK_RAM_SIZE) n -= 0xE00000;
  if (n < 0 || n + 0x2c0 > WORK_RAM_SIZE) return;
#define PN(off) W[n + (off)]

  iVar2 = math_atan2(v[0], v[1]);
  PN(0x38) = iVar2;
  PN(0x1b8) = -iVar2;
  iVar3 = m68k_divs((int32_t)(v[1] << 0xf),
                    (int)(vrd16s(0x20B006 + ((uint32_t)PN(0x38) & 0xfffc))));
  iVar2 = math_atan2(-v[2], iVar3);
  iVar3 = m68k_divs(iVar3 << 0xf, (int)(vrd16s(0x20B006 + ((uint32_t)((uint16_t)iVar2 & 0xfffc)))));
  param_3 = iVar3 * iVar3 - param_3;
  if (param_3 < param_4) {
    iVar3 = math_slope_angle(param_3,param_4);
    uVar1 = -iVar3;
    PN(0xb0) = (int32_t)uVar1;
    PN(0x230) = (int32_t)uVar1;
    iVar3 = math_atan2((int)param_6 * (int)(vrd16s(0x20B004 + (uVar1 & 0xfffc))) >> 0xf,
                       param_5 + ((int)param_6 *
                                  (int)(vrd16s(0x20B006 + (uVar1 & 0xfffc))) >> 0xf));
    PN(0x30) = iVar3 - iVar2;
    PN(0x1b0) = iVar3 - iVar2;
    param_4 = -((int32_t)PN(0x30) + (int32_t)PN(0xb0));
  }
  else {
    PN(0xb0) = 0;
    PN(0x230) = 0;
    PN(0x30) = -iVar2;
    PN(0x1b0) = -iVar2;
    param_4 = param_4 - param_3;
    if (param_4 < -0x1800) {
      param_4 = -0x1800;
    }
  }
  PN(0x130) = param_4;
  PN(0x2b0) = param_4;
#undef PN
  return;
}

/* ---- ending_ferris_wheel_draw @ 0x02C518 --------------------------------
 *
 * The eight gondolas: each a 0x8008 transform + 0x800a placement under a
 * 0x8002 header. PORTED FROM THE ROM (0x02C518..0x02C6FA). The transpile read
 * the per-gondola tables at 0x3808C / 0x3809C / 0x36414 as host shorts (the
 * first two through `*(int16_t*)(i*2 + 0x3808c)`, a dereference of a ROM
 * OFFSET) and emitted the trig pairs as the HIGH halves. The ROM moves two
 * LONGS off the table (`lea -$2(a4,d0.l),a3 ; move.l (a3),(a2)+ ;
 * move.l $2(a3),(a2)+`), whose low halves are sin and cos (register row 87);
 * they are written here exactly as the ROM writes them. Returns the index
 * (1..8) of the gondola that reaches t == 6 this frame, else 0. */

int ending_ferris_wheel_draw(int y0, int y1, int t)

{
  int32_t *dl, d3, d7, a, d4 = 0;
  int d2;

  dsp_cmd_emit_object_mode_8002();
  dl = (int32_t *)W[0x0CA4];
  dl = dl_put(dl, 0x8010);
  dl = dl_put(dl, 3);
  W[0x4704] = ((int32_t)W[0x0CBC] == 1) ? (int32_t)W[0x3EF0] + (int32_t)0xFFFF2A00
                                        : (int32_t)0xFFFF0000;
  dl = dl_put(dl, (int32_t)W[0x4704]);
  dl = dl_put(dl, -1);
  for (d2 = 0; d2 < 8; d2++) {
    int16_t tw = vrd16s(0x3808C + d2 * 2);
    uint32_t xa;
    d3 = d2 * -9 + t;
    if (d3 == 6) d4 = (d2 == 7) ? 1 : d2 + 1;
    d7 = (d3 < 6) ? (y1 - y0) * d3 / 6 + y0 : y1;
    if (d3 < 0x24)       a = 0x4000;
    else if (d3 >= 0x60) a = 0x4000 + tw * 2;
    else {
      int32_t l = math_lerp_int(0, 0x8000, 0x24, 0x60, d3);
      int32_t c = vrd16s(0x20B006 + (l & 0xFFFC));
      a = (int32_t)(int16_t)c * tw / (int32_t)0xFFFF8000 + tw + 0x4000;
    }
    dl = dl_put(dl, 0x8008);
    dl = dl_put(dl, 3);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, (int32_t)0xFFFFA600);
    dl = dl_put(dl, 1);
    dl = dl_put(dl, vrd32s(0x20B002 + (a & 0xFFFC)));
    dl = dl_put(dl, vrd32s(0x20B004 + (a & 0xFFFC)));
    xa = vrd16(0x3809C + d2 * 2) & 0xFFFC;
    dl = dl_put(dl, vrd32s(0x20B002 + xa));
    dl = dl_put(dl, vrd32s(0x20B004 + xa));
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x7FFF);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, -1);
    dl = dl_put(dl, 0x800A);
    dl = dl_put(dl, vrd16s(0x36414 + d2 * 2));
    dl = dl_put(dl, 3);
    xa = (uint32_t)((d2 << 13) + 0x8000) & 0xFFFC;
    dl = dl_put(dl, ((int32_t)vrd16s(0x20B004 + xa) << 12 >> 11) - (int32_t)W[0x0CDC] + 0x3C000);
    dl = dl_put(dl, d7 - (int32_t)W[0x0CE0] + 0x18600);
    dl = dl_put(dl, ((int32_t)vrd16s(0x20B006 + xa) << 12 >> 11) - (int32_t)W[0x0CE4] + 0x114000);
  }
  dl = dl_put(dl, 0x8010);
  dl = dl_put(dl, -1);
  W[0x4704] = 0;
  W[0x0CA4] = (intptr_t)dl;
  return d4;
}

/* ---- ending_object_strip_draw @ 0x02B378 ----------------------------------
 *
 * PORTED FROM THE ROM (0x02B378..0x02B500). A list of 5-long records
 * (model base, x, y, z, angle) ended by a negative model, emitted as 0x8002
 * entries under viewport 3 on sub 15 (else 0), after a zsort-bias marker
 * when `bias` differs from 0xE04704. The model cycles through 8 frames on
 * `frame` (+8 a record). The angle's two special values come from the
 * two-entry jump table at 0x02B43A that capstone shows as code (index
 * angle+1, scale 2): -1 (0x02B43C) turns the record to face the PLAYER --
 * 0x8000 - atan2(player - record), pulled to the facing word 0xE16670 when
 * more than 0x1800 away from it -- and -2 (0x02B4BC) faces the camera with
 * the (sin, cos) the caller left at 0xE16668/0xE1666C. Otherwise the pair is
 * (sin, cos) of the angle -- `move.l (a0) ; move.l $2(a0)` off 0x20B002
 * (register row 87). The same logic as game_gameplay.c's sci_strip. `list`
 * is a 68K address (a transpiled caller's `&R[...]` is accepted). The
 * transpile walked the list natively (little-endian) and left the -1/-2
 * cases as an `unresolved dispatch`. */
static int32_t *e_strip_head(int32_t bias)
{
  int32_t *dl = (int32_t *)W[0x0CA4];
  W[0x169E8] = 0x8002;
  dl = dl_put(dl, 0x8002);
  dl = dl_put(dl, (W[0x0CC0] == 0xf) ? 3 : 0);
  if (bias != (int32_t)W[0x4704]) {
    dl = dl_put(dl, 0x8010);
    dl = dl_put(dl, 3);
    W[0x4704] = bias;
    dl = dl_put(dl, bias);
    dl = dl_put(dl, -1);
  }
  return dl;
}

void ending_object_strip_draw(ea_t list, int32_t frame, int32_t bias)
{
  int32_t *dl = e_strip_head(bias);
  ea_t rec = ea_norm(list);
  int32_t H = (int16_t)W[0x16670];
  int n = 0;
  while (ea_rd32(rec) >= 0 && n++ < 256) {
    int32_t ang = ea_rd32(rec + 16);
    frame += 8;
    dl = dl_put(dl, ((frame >> 3) & 7) + ea_rd32(rec));
    dl = dl_put(dl, ea_rd32(rec + 4)  - (int32_t)W[0x0CDC] + (int32_t)W[0x3EF0]);
    dl = dl_put(dl, ea_rd32(rec + 8)  - (int32_t)W[0x0CE0] + (int32_t)W[0x3EF4]);
    dl = dl_put(dl, ea_rd32(rec + 12) - (int32_t)W[0x0CE4] + (int32_t)W[0x3EF8]);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x7fff);
    if (ang == -2) {                                     /* 0x02B4BC */
      dl = dl_put(dl, (int32_t)W[0x16668]);
      dl = dl_put(dl, (int32_t)W[0x1666C]);
    } else {
      uint32_t idx;
      if (ang == -1) {                                   /* 0x02B43C */
        int32_t d2 = (uint16_t)(0x8000 - (int32_t)math_atan2((int32_t)W[0x0D00] - ea_rd32(rec + 4),
                                                             (int32_t)W[0x0D08] - ea_rd32(rec + 12)));
        int use_h = 0;
        if (d2 > H) {
          if (d2 - H > 0x1800 && H - d2 + 0x10000 > 0x1800) use_h = 1;
        } else if (d2 < H) {
          if (H - d2 - 0x10000 < -0x1800 && d2 - H < -0x1800) use_h = 1;
        }
        if (use_h) d2 = H;                               /* 0x02B4AE: movea.w (a4) */
        idx = (uint16_t)d2 & 0xfffc;
      } else {
        idx = (uint32_t)ang & 0xfffc;                    /* 0x02B4CA */
      }
      dl = dl_put(dl, e_sin(idx));
      dl = dl_put(dl, e_cos(idx));
    }
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x7fff);
    dl = dl_put(dl, 4);
    rec += 20;
  }
  W[0x0CA4] = (intptr_t)dl;
}

/* ---- ending_object_strip_draw_anim @ 0x02B502 -----------------------------
 *
 * PORTED FROM THE ROM (0x02B502..0x02B5C6). As above, but no 0xE03EF0 offset,
 * every record faces the camera (0xE16668/0xE1666C), and the model is base +
 * the BE16 at 0x37B98 + ((frame >> 3) mod count)*2: `divsl.l $10(a3),d0:d1`
 * leaves the REMAINDER in d0 and `movea.w $37b98(d0.l*2)` (ext 0x0BB0, scale
 * 2) indexes with it (register row 172's table). A zero count traps to a bare
 * `rte` on the machine, leaving d0 -- the previous record's `moveq #4`. */
void ending_object_strip_draw_anim(ea_t list, int32_t frame, int32_t bias)
{
  /* d0 entering the loop: `moveq #$ff` if the bias marker went out, else
   * the bias itself (0x02B536..0x02B55C) -- only ever used if the first
   * record's count is 0. */
  int32_t rem = (bias != (int32_t)W[0x4704]) ? -1 : bias;
  int32_t *dl = e_strip_head(bias);
  ea_t rec = ea_norm(list);
  int n = 0;
  while (ea_rd32(rec) >= 0 && n++ < 256) {
    int32_t cnt = ea_rd32(rec + 16);
    frame += 8;
    if (cnt != 0) rem = (frame >> 3) % cnt;
    dl = dl_put(dl, ea_rd32(rec) + vrd16s(0x37B98 + rem * 2));
    dl = dl_put(dl, ea_rd32(rec + 4)  - (int32_t)W[0x0CDC]);
    dl = dl_put(dl, ea_rd32(rec + 8)  - (int32_t)W[0x0CE0]);
    dl = dl_put(dl, ea_rd32(rec + 12) - (int32_t)W[0x0CE4]);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x7fff);
    dl = dl_put(dl, (int32_t)W[0x16668]);
    dl = dl_put(dl, (int32_t)W[0x1666C]);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x7fff);
    dl = dl_put(dl, 4);
    rem = 4;                                             /* moveq #4,d0 */
    rec += 20;
  }
  W[0x0CA4] = (intptr_t)dl;
}

/* ---- state_bonus_init ---- */

void state_bonus_init(void)

{
  sync_post();
  camera_state_reset();
  dsp_param_init();
  if (W16(0xE6A) == 0) {
    W[0x15F60] = 0xffff;
  }
  if (W16(0xE10) != 0) {
    if ((W16(0xE2C) & 7) == 0) {
      FUN_0000971a();
      goto LAB_00008b5a;
    }
    if (W16(0xE6A) != 0) {
      FUN_000098a4();
      goto LAB_00008b5a;
    }
  }
  FUN_00008b6a();
LAB_00008b5a:
  W[0x0CC0] = 1;
  state_bonus_run();
  return;
}

/* ---- state_bonus_run ---- */

void state_bonus_run(void)

{
  if (W16(0xE10) != 0) {
    if ((W16(0xE2C) & 7) == 0) {
      FUN_00009792();
      return;
    }
    if (W16(0xE6A) != 0) {
      FUN_000098dc();
      return;
    }
  }
  FUN_00008c82();
  return;
}

/* ---- state_ending_init ---- */

void state_ending_init(void)

{
  sync_post();
  camera_state_reset();
  ranking_sound_init();
  g_fog_mode = 1;
  W[0xEB16] = 0xc0;
  g_fog_r = 0;
  g_fog_g = 0;
  g_fog_b = 0;
  W_SET_HI16(0xEB20, 0);   /* clr.w $e0eb20: 16-bit, the HIGH half */
  W[0x15F5C] = 0;
  FUN_0000f584(0x0E);   /* ROM 0x02A1C8 `move.w #$e` */
  sound_reset_upper();
  W[0x0CBC] = 0xd;
  state_ending_run();
  return;
}

/* ---- state_ending_run ---- */

void state_ending_run(void)

{
  int iVar1;
  uint32_t uVar2;
  short sVar3;
  undefined2 uVar4;
  undefined2 uVar5;
  undefined2 uVar6;
  undefined2 uVar7;
  undefined2 uVar8;
  
  uVar2 = W[0x0C98] >> 6;
  dsp_cmd_set_camera(0,0x27,0x8000,0x8000,0xa00,0,(short)(W[0x0C98] << 5),0,2);
  dsp_w32(0x10038 + 4 * (W[0x0CA0] * 0x2000), (int32_t)(0x780));
  iVar1 = 0;
  _safety_ctr = 0;
  do {
    sprite_draw_2d(5,iVar1 + 0x14a,iVar1 << 8,0,1,0x20,0x20,0,0);
    iVar1 = iVar1 + 1;
  } while (iVar1 < 3);
  iVar1 = 0;
  do {
    sprite_draw_2d(5,iVar1 + 0x14d,iVar1 << 8,0xa0,1,0x20,0x20,0,0);
    iVar1 = iVar1 + 1;
  } while (iVar1 < 3);
  sprite_draw_2d(5,0x150,0x90,0x120,1,0x20,0x20,0,0);
  sprite_draw_2d(5,0x151,400,0x120,1,0x20,0x20,0,0);
  if ((W[0x0C98] & 0x20) == 0) {
    if ((W[0x2C0E] < g_time_limit) && (W16(0x3FF4) == 0)) {
      /* ROM 0x02A356..0x02A392: "INSERT n COIN(S)". The base tile and
       * palette of both banners were lost in decompilation (register row 62);
       * the digit's base is 0x38c + (coins per credit - coins) * 4, the rule
       * register row 60 found for the title screen. */
      text_draw_rect_blink(0x1a,6,0x16,0x330,3);
      text_draw_rect_blink(0x20,0xe,0x16,
                           (int16_t)(0x38c + (int16_t)(W16(0x3FF0) - (int16_t)W[0x2C0E]) * 4), 3);
    }
    else {
      /* ROM 0x02A320..0x02A34E: the credit line, then the FREE PLAY /
       * CREDIT banner at base 0x2F0, palette 3. */
      sVar3 = ((uint16_t)uVar2 & 7) << 1;
      text_print_string(6, 0x16, 0x3acd4, sVar3);
      text_draw_rect_blink(0x1b,6,0x16,0x2f0,3);
    }
  }
  else {
    if ((W[0x2C0E] < g_time_limit) && (W16(0x3FF4) == 0)) {
      uVar4 = 0x1a;
    }
    else {
      uVar4 = 0x1b;
    }
    text_draw_rect_solid(uVar4,6,0x16);
  }
  if (((g_time_limit <= W[0x2C0E]) || (W16(0x3FF4) != 0)) &&
     (W[0x15F5C] = W[0x15F5C] + 1, W[0x15F5C] == 0x1e)) {
    sound_play_p(0x0026006D);
  }
  return;
}

/* ---- state_ranking_init ---- */
void replay_cam_copy9(uint32_t dst, uint32_t src);   /* game_replay.c */

undefined4 state_ranking_init(void)

{
  undefined4 uVar1;
  short sVar2;
  int iVar3;
  undefined4 *puVar4;
  undefined4 *puVar5;
  
  if (W[0x16D84] < 0x259) {
    replay_load_builtin(9);
  }
  else {
    W[0x16D7C] = &g_replay_data;
    W[0x0E0C] = (int)W[0x15BE8];
    W16_SET(0xE64, W[0x15BEA]);
    W[0x15F60] = W[0x15BEC];
    W[0x15BE0] = W[0x16D84];
    /* ROM 0x0294C0 `move.l #$e17064,$e16d80.l`: the 68K ADDRESS of the
     * recorded camera path. It was a host pointer (&W[0x17064]), which
     * replay_camera_update then read at the wrong stride; that function now
     * resolves a path address itself (ROM or work RAM, row 161). */
    W[0x16D80] = 0xE17064;
  }
  W[0x17038] = 0;
  W[0x15BE4] = 0;
  W[0xEB16] = 0;
  g_fog_mode = 3;
  W[0x1703C] = 0;
  sync_post();
  ranking_sound_init();
  cgram_load_tile_block(0xf6,0x270);
  cz_load_color_ramp(0xf6, 5);   /* ROM 0x029504 `move.b #$5,-(a7)` -- palette byte lost (row 118) */
  /* ROM 0x029516 `movea.l $e16d7c,a0 ; move.l $c(a0),$e00c8c`. The replay pointer is
     a ROM ADDRESS on the builtin branch (replay_load_builtin) and a host pointer on the
     recorded one, exactly as replay_frame_tick handles it -- the host dereference of a
     ROM address here would fault the moment this branch ran. */
  { uint32_t rp = (uint32_t)W[0x16D7C];
    if (rp && rp < ROM_SIZE) W[0x0C8C] = (int32_t)vrd32(rp + 0xc);
    else                     W[0x0C8C] = ((int32_t *)W[0x16D7C])[3]; }
  gameplay_init_player_and_world();
  gameplay_init_state_vars();
  if (W[0x2C0D] == 0) {
    W[0x16D88] = W[0x0C98] & 3;
  }
  else {
    W[0x16D88] = W[0x2C0D] - 1;
  }
  /* ROM 0x02955E..0x0295BA: 64 longs 0xE17124 -> 0xE00D00 (the player
   * state saved by replay_start_recording), 3 longs 0xE17224 -> 0xE00E00,
   * 14 longs 0xE17230 -> 0xE012C0, then the camera block mirrored to
   * 0xE17040 (nine longs). All four were `undefined4 *` walks over the
   * 8-byte `_W[]` slots, which move half as many slots as the ROM moves
   * longs and split each one in two (register row 161). */
  if (600 < W[0x16D84]) {
    for (iVar3 = 0; iVar3 < 0x40; iVar3++)
      W[0x0D00 + iVar3 * 4] = (int32_t)W[0x17124 + iVar3 * 4];
    W[0x0E00] = W[0x17224];
    W[0x0E04] = W[0x17228];
    W[0x0E08] = W[0x1722C];
    for (iVar3 = 0; iVar3 < 0xe; iVar3++)
      W[0x12C0 + iVar3 * 4] = (int32_t)W[0x17230 + iVar3 * 4];
  }
  W[0x0CDC] = W[0x0D00];
  W[0x0CE0] = W[0x0D04];
  W[0x0CE4] = W[0x0D08];
  replay_cam_copy9(0x17040, 0x0CDC);
  W[0x15F5E] = (undefined2)W[0x0D10];
  uVar1 = 0;
  if (W[0x0CBC] != 3) {
    g_fog_r = 0;
    g_fog_g = 0;
    g_fog_b = 0;
    W[0x0CBC] = 0xb;
    uVar1 = state_ranking_run();
  }
  return uVar1;
}

/* ---- state_ranking_run ---- */

void state_ranking_run(void)

{
  short sVar1;
  undefined4 *puVar2;
  undefined4 *puVar3;
  
  if ((W[0x2BDA] & 1) != 0) {
    W[0x16D88] = (W[0x16D88] + 1) % 4;
  }
  if (-1 < (int)(W[0x17038] - 3U)) {
    if ((W[0x17038] - 3U & 0x28) == 0) {
      text_draw_rect_solid(0xf6,0x1e,2);
    }
    else {
      text_draw_rect_blink(0xf6, 0x1e, 2, 0x270, 5);
    }
  }
  /* REPLAY INPUT RECORD -- this is the demo flight's steering.
   *
   * W[0x16D7C] is a ROM ADDRESS (replay_load_builtin: mem_read32(0x15C508 +
   * idx*4), read back with mem_read32(+0xc)). This derefed it as a HOST
   * pointer in native byte order, so pitch/bank were garbage every frame:
   * the bike's heading wandered +-8 deg where the recording holds 290.0
   * dead steady, then flew off the map into every terrain edge case.
   *
   * Verified from the ROM stream at 0x15C76C: 16-byte big-endian records
   * [pitch_tgt, bank_tgt, ?, frame_ctr], frame_ctr = 2218,2219,2220... at
   * a 16-byte stride, steering values smooth (4964..8468 / 4234..1752).
   * The +4 advance below was therefore 4 bytes into a 16-byte record. A
   * host-pointer form (&g_replay_data, RAM-recorded replays) still exists
   * upstream, so both are honoured by range. */
  { uint32_t rp = (uint32_t)W[0x16D7C];
    if (rp && rp < ROM_SIZE) {
      W[0x0D50] = (int32_t)vrd32(rp + 0x0);
      W[0x0D54] = (int32_t)vrd32(rp + 0x4);
      W[0x2C04] = (int32_t)vrd32(rp + 0x8);
      W[0x0C8C] = (int32_t)vrd32(rp + 0xc);
    } else {
      W[0x0D50] = *(int32_t*)W[0x16D7C];
      W[0x0D54] = ((int32_t*)W[0x16D7C])[1];
      W[0x2C04] = ((int32_t*)W[0x16D7C])[2];
      W[0x0C8C] = ((int32_t*)W[0x16D7C])[3];
    } }
  W[0x12BC] = (uint32_t)(W[0x16D88] != 1);
  player_bounds_check();
  objects_move_update();
  world_grid_calc_position();
  world_render_all();
  replay_camera_update();
  replay_cam_copy9(0x0CDC, 0x17040);    /* ROM 0x0296CA: nine longs (row 161) */
  camera_grid_calc_position();
  terrain_chunk_visibility
            (W[0x0CDC],W[0x0CE0],W[0x0CE4],W[0x0CEC]);
  terrain_props_dispatch();
  player_render();
  objects_render_master();
  balloon_render_and_hit_check();
  stage_camera_path_update();
  environment_zone_tick();
  particle_system_update();
  /* next 16-byte record (was +4: one field, not one record) */
  W[0x16D7C] = W[0x16D7C] + (((uint32_t)W[0x16D7C] < ROM_SIZE) ? 0x10 : 16);
  W[0x15BE4] = W[0x15BE4] + 1;
  if (((W[0x2B3A] & 0x100) != 0) || ((W[0x2BA6] & 1) != 0)) {
    W[0x1703C] = 1;
  }
  W[0x17038] = W[0x17038] + 1;
  if (0x4b0 - W[0x17038] < 0x81) {
    W[0x1703C] = 1;
  }
  if (W[0x15BE0] - W[0x15BE4] < 0x81) {
    W[0x1703C] = 1;
  }
  if ((W[0x1703C] != 0) && (W[0xEB16] = W[0xEB16] + 2, 0xff < W[0xEB16])) {
    W[0xEB16] = 0xff;
    W[0x0CBC] = 0;
  }
  return;
}

/* ---- name_entry_fill_blanks ---- */

/* ROM 0x02ABD6 -- END pressed: every letter not yet entered becomes the '_'
 * filler (grid index 0x31) with the 0xFC00 drop. Name bytes are one _W[] slot
 * each; the drop words are a 16-bit array at stride 2 (`$e17288(d0.w*2)`). */
void name_entry_fill_blanks(void)
{
  int i;
  for (i = 0; i < 3; i++) {
    if (i >= W16(0x1727C)) {
      W[0x17278 + i] = 0x31;
      W_A16_SET(0x17288, i, 0xfc00);
    }
  }
}

/* ---- name_entry_render ---- */

/* ROM 0x02A852, ported from the disassembly. The screen is the RESULTS board
 * (0x376 frame, 0x37D, 0x395 letter panel at z 0x350), a MAGNIFIER over the
 * panel -- viewport 2, whose record at DSP RAM 0xC10100 + buffer*0x8000 this
 * function aims at the cursor (words 13/15/16/18/19/21/22 below) and in which
 * 0x395/0x37D are drawn again ten times closer (z 0x35) -- the lens ring as a
 * 2D sprite (0x1CC), the three name letters dropping into their slots (models
 * from the BE32 table 0x37990, indexed by the grid index), the five-digit
 * score odometer and the five name plaques (0x351).
 * The transpile had the per-letter drop words at stride 1 (0xE17288 is
 * `(a2,d2.l*2)`), the cursor index read as a whole slot, and the lens ease as
 * `uint32_t` compares where the ROM's are signed. */
int name_entry_render(void)
{
  int idx = W16(0x17282);
  int32_t tx = (idx % 13) * 42 - 250;          /* divs.l: remainder, quotient */
  int32_t ty = (idx / -13) * 42 - 43;
  int32_t cx = (int32_t)W[0x1726C], cy = (int32_t)W[0x17270], d;

  /* ease the lens a quarter of the way, at least one unit */
  if (tx != cx) {
    d = tx - cx; d = (d < 0 ? d + 3 : d) >> 2;
    if (d == 0) d = (tx > cx) ? 1 : -1;
    cx += d; W[0x1726C] = cx;
  }
  if (ty != cy) {
    d = ty - cy; d = (d < 0 ? d + 3 : d) >> 2;
    if (d == 0) d = (ty > cy) ? 1 : -1;
    cy += d; W[0x17270] = cy;
  }
  /* viewport 2's record: 0xC10134 = word 13, 0xC1013C/40 = 15/16,
   * 0xC10148/4C = 18/19, 0xC10154/58 = 21/22 (DSP words are host-order
   * int32 in g_sys.dspram, like every other writer in this tree) */
  { uint8_t *vp = &g_sys.dspram[0x10100 + (int32_t)W[0x0CA0] * 0x8000];
    *(int32_t *)(vp + 0x34) = 0;
    *(int32_t *)(vp + 0x3c) = 0x19;
    *(int32_t *)(vp + 0x40) = 0x19;
    *(int32_t *)(vp + 0x48) = -cx;
    *(int32_t *)(vp + 0x4c) = -cy;
    *(int32_t *)(vp + 0x54) = cx & 0xffff;
    *(int32_t *)(vp + 0x58) = cy & 0xffff; }
  dsp_cmd_place_object_abs(0,0x376,0,0,0x350);
  dsp_cmd_place_object_abs(0,0x37d,0,0,0x350);
  dsp_cmd_place_object_abs(0,0x395,0,0,0x350);
  dsp_cmd_place_object_abs(2,0x395,(cx * 20) / -0x40,(cy * 20) / -0x40,0x35);
  dsp_cmd_place_object_abs(2,0x37d,(cx * 20) / -0x40,(cy * 20) / -0x40,0x35);
  sprite_draw_2d(4,0x1cc,cx + 0x10f,200 - cy,0,0x20,0x20,0,0);
  { int x = -0x53, i;
    for (i = 0; i < 3; i++) {
      int a = W_A16(0x17288, i);
      int z = 0x350 - (((int)(int16_t)vrd16s(0x20B004 + (((a >> 1) & 0x7ffe) * 2)) << 10) >> 15);
      int y = (a < 0) ? 0xa4 : 0xa4 - ((a * a) >> 14);
      int mode = (a > 0) ? 3 : 0;
      dsp_cmd_set_camera(mode, vrd32(0x37990 + (int)(int8_t)W[0x17278 + i] * 4),
                         x, y, z, 0, 0, 0, 0);
      x += 0x53;
      if (i < W16(0x1727C)) {                     /* entered: drop it in */
        if (a < 0) {
          W_A16_SET(0x17288, i, a + 0x200);
          if (W_A16(0x17288, i) >= 0) sound_play(0x28);
        }
        else W_A16_SET(0x17288, i, 0);
      }
      else {                                      /* not entered: float up */
        if (a < 0 && a >= -0x40) sound_stop(0x28);
        if (a < 0x4000) W_A16_SET(0x17288, i, a + 0x40);
      }
    }
  }
  score_odometer_animate(0, (int32_t)W[0x0E54] << 3, (int32_t)W[0x0E54], -0x77, 0x14, 0x1c0);
  { int i;
    for (i = 0; i < 5; i++)
      dsp_cmd_place_object_rotated_abs(0,0x351,i * 0x28 - 0x4b,0x10,0x1c0,0,0,0,0); }
  return 0;
}

/* ---- ending_credits_render ---- */

/* ROM 0x02FD72, ported from the machine code; runs every frame of sub 15.
 * The staff roll: a table of 20-byte records at 0x38560 (start frame,
 * duration, column, palette, string pointer, model, animation), ending at a
 * negative start. On a record's first frame its string is written into the
 * text layer (0x89E000, BIG-ENDIAN tile words, char + 0x39078[palette]) on
 * the row the scroll will bring up next; while it is live its portrait model
 * scrolls up the screen as an 0x8001 entry (0x8001, 0 header: viewport 0)
 * -- with animation 1 through the ping-pong frames 0x37B98[(t >> 4) % 6].
 * While any record is live the text layer scrolls (0x8A0002 = 2t & 0x3FF)
 * and every other frame the row leaving the top is blanked with 0x20.
 * 0x8A0000 = 0x35C either way.
 *
 * The transpile read the table host-native through &R[], dropped the string
 * copy ("broken expression removed"), and left the animation's one-entry
 * jump table at 0x02FE42 as `unresolved dispatch; return` -- a return with the
 * command list unterminated. Its only case (anim == 1) is 0x02FE4E. */
undefined4 ending_credits_render(void)
{
  int32_t *p = e_cur();
  int16_t cnt = W16(0x172EA);
  int16_t d5 = (int16_t)(cnt << 1);
  int live = 0, n = 0;
  uint32_t r;
  W[0x169E8] = 0x8001;
  *p++ = 0x8001; *p++ = 0;
  *p++ = 0x8010; *p++ = 0; *p++ = -1;
  for (r = 0x38560; e_rd32(r) >= 0 && n++ < 200; r += 0x14) {
    int32_t st = e_rd32(r), dur = e_rd32(r + 4);
    int16_t col = vrd16s(r + 8), pal = vrd16s(r + 10), mdl = vrd16s(r + 16), anim = vrd16s(r + 18);
    if (st - 1 <= cnt && st + dur >= cnt) live = 1;
    if (!(st <= cnt && st + dur >= cnt)) continue;
    if (st == cnt) {
      uint32_t dst = (uint32_t)((((0x2a0 + d5) & 0x3ff) >> 4) << 7) + (uint32_t)(col * 2) + 0x89E000;
      uint32_t s = (uint32_t)e_rd32(r + 12);
      int k;
      for (k = 0; k < 64 && vrd8(s); k++, s++, dst += 2)
        vwr16(dst, (uint16_t)(vrd8(s) + vrd16(0x39078 + pal * 2)));
    }
    if (mdl >= 0) {
      int32_t m = (anim == 1) ? vrd16s(0x37B98 + (((int32_t)cnt >> 4) % 6) * 2) + mdl : mdl;
      *p++ = m;
      *p++ = (col - 20) * (0x104 + (int32_t)W[0x3EF0]);
      *p++ = (int32_t)((0x2a0 - 2 * (cnt - st)) * (0xec4 + (int32_t)W[0x3EF4])) / -0x100
             + (int32_t)W[0x3EF8] + 0xed0;
      *p++ = 0x2000;
      *p++ = 0; *p++ = 0x5fff; *p++ = 0; *p++ = 0x5fff; *p++ = 0; *p++ = 0x5fff;
      *p++ = 0;
    }
  }
  *p++ = 0x8010; *p++ = -1;
  e_setcur(p);
  if (live) {
    vwr16(0x8A0002, (uint16_t)(d5 & 0x3ff));
    if ((cnt & 1) == 0) {
      uint32_t dst = (uint32_t)((((d5 - 0x20) & 0x3ff) >> 4) << 7) + 0x89E000;
      int k;
      for (k = 0; k < 0x40; k++, dst += 2) vwr16(dst, 0x20);
    }
  } else {
    vwr16(0x8A0002, 0);
  }
  vwr16(0x8A0000, 0x35c);
  return 0;
}

/* ---- ending_frame_render @ 0x02B5C8 ---------------------------------------
 *
 * PORTED FROM THE ROM (0x02B5C8..0x02B656): run the rider's motion model
 * (0x02B226), copy the ending's player node 0xE0B728..0xE0B73C into the
 * player block 0xE00D00..0xE00D14, clear 0xD58/0xD5C/0xD60, pose the rig
 * (0x026A48), set the camera (0x02811C with 0xD98, 0x172DC, 0x172D8,
 * 0x172E0, 0x172E4) and, in phase 7 only, draw the player's shadow
 * (0x015C50). The transpile had it right; returns 0 (the ROM returns d0,
 * which no caller reads). */
int ending_frame_render(void)
{
  ending_player_physics_sim();
  W[0x0D00] = (int32_t)W[0xB728];
  W[0x0D04] = (int32_t)W[0xB72C];
  W[0x0D08] = (int32_t)W[0xB730];
  W[0x0D0C] = (int32_t)W[0xB734];
  W[0x0D10] = (int32_t)W[0xB738];
  W[0x0D14] = (int32_t)W[0xB73C];
  W[0x0D5C] = 0;
  W[0x0D60] = 0;
  W[0x0D58] = 0;
  player_model_set_pose();
  camera_setup_extended(W[0x0D98], W[0x172DC], W[0x172D8], W[0x172E0], W[0x172E4]);
  if ((int32_t)W[0x0CBC] == 3 && W16(0x172E8) == 7)
    hud_draw_wings();
  return 0;
}

/* ---- ending_player_reset @ 0x02B1B2 ---------------------------------------
 *
 * PORTED FROM THE ROM (0x02B1B2..0x02B224): speed 0xD48 = 0x1400 (an
 * immediate -- the transpile had `&R[0x1400]`), the player placed 0x1400>>5
 * behind the ending's node on x and z, 0xE00 = 0x1400>>2, attitude copied,
 * 0xD50/0xD54/0xD94/0xD98 cleared, animation set 0 loaded (0x026B46). */
void ending_player_reset(void)
{
  W[0x0D48] = 0x1400;
  W[0x0D00] = (int32_t)W[0xB728] - (0x1400 >> 5);
  W[0x0D04] = (int32_t)W[0xB72C];
  W[0x0D08] = (int32_t)W[0xB730] - (0x1400 >> 5);
  W[0x0E00] = 0x1400 >> 2;
  W[0x0D0C] = (int32_t)W[0xB734];
  W[0x0D10] = (int32_t)W[0xB738];
  W[0x0D14] = (int32_t)W[0xB73C];
  W[0x0D54] = 0;
  W[0x0D50] = 0;
  W[0x0D98] = 0;
  W[0x0D94] = 0;
  player_model_load_animation(0);
}

/* ---- ending_sky_draw_alt ---- */

void ending_sky_draw_alt(void)

{
  dsp_cmd_emit_object_mode_8000();
  W[0x4704] = 0;
  *(int32_t*)W[0x0CA4] = 0x8010;
  ((int32_t*)W[0x0CA4])[1] = 0xffffffff;
  ((int32_t*)W[0x0CA4])[2] = 0x29;
  ((int32_t*)W[0x0CA4])[3] = 0;
  ((int32_t*)W[0x0CA4])[4] = 0xfffea000;
  ((int32_t*)W[0x0CA4])[5] = 0;
  ((int32_t*)W[0x0CA4])[6] = 0x27;
  ((int32_t*)W[0x0CA4])[7] = 0;
  ((int32_t*)W[0x0CA4])[8] = 0xfffea000;
  ((int32_t*)W[0x0CA4])[9] = 0;
  W[0x0CA4] = W[0x0CA4] + 10 * 4;
  return;
}

/* ---- bonus_render_frame ---- */

void bonus_render_frame(void)

{
  /* ROM 0x016FA0. No caller in the ROM (the count-down draws through
   * bonus_sequence_animate); kept faithful to the disassembly. */
  intptr_t nb;
  int k;
  W16_SET(0x16728, 300);
  bonus_camera_update();
  *(int32_t *)W[0x0CA4] = 0x8002; W[0x0CA4] += 4;
  *(int32_t *)W[0x0CA4] = 0;      W[0x0CA4] += 4;
  bonus_model_dsp_setup();
  nb = BNB;
  for (k = 0; k < 64; k++) {
    nb += 0x80;
    if (nb < 0 || nb + 0x80 > WORK_RAM_SIZE) return;
    if ((int32_t)W[nb] == 0x250) break;
    scene_node_render((int *)&_W[nb]);
  }
  scene_node_render_scaled((undefined4 *)&_W[nb],0x5333);
  dsp_emit_articulated_model((intptr_t)&_W[nb],0x5333);
  { int32_t *cp = (int32_t *)W[0x0CA4];
    *cp++ = 0x8010; *cp++ = 3; *cp++ = (int32_t)0xffff4000; *cp++ = -1;
    W[0x0CA4] = (intptr_t)cp; }
  return;
}

/* ---- ending_camera_shake_init @ 0x02AF36 ----------------------------------
 * Arms the camera shake: `table` (a 68K address or C array, ea68k.h) of six
 * longs, `n` frames. 0xE172EE/0xE172F0 are 16-bit (`move.w d0`). */

void ending_camera_shake_init(ea_t table, int n)

{
  W[0x172D4] = (intptr_t)ea_norm(table);
  W16_SET(0x172EE, (int16_t)n);
  W16_SET(0x172F0, (int16_t)n);
  return;
}

/* ---- ending_camera_shake_update @ 0x02AE86 --------------------------------
 * While the 16-bit countdown 0xE172EE is non-negative, adds up to three
 * cosine wobbles to the camera angles starting at pitch (0xE00CE8) -- the
 * destination advances only past a term that is present (`add.l d0,(a3)+`
 * inside each branch). Table (six longs): [2]/[3] -> first term, [0]/[1] ->
 * second, [4]/[5] -> third, each cos(t*freq) / div. Then viewport 3's
 * camera words (dsp_viewport_setup(3, 2)). PORTED FROM THE ROM; the
 * transpile walked `&W[0x0CE8]` with an `int *` (4 host bytes a step). */

/* ROM 0x02AE86, ported from the machine code. While the 16-bit countdown
 * 0xE172EE is still >= 0 after its decrement, add a cosine wobble to the
 * camera angles: k = total - left; for the pairs (+8,+12), (+0,+4),
 * (+16,+20) of the record at W[0x172D4], a nonzero divisor adds
 * cos((freq * k) & 0xfffc) / divisor to the NEXT angle of 0xE00CE8.. -- the
 * destination only advances when a term is applied (`add.l d0,(a3)+`).
 * Then viewport 3 is set up from the camera (0x025FE2 with 3, 2). The
 * transpile walked &W[] as an int array and dereferenced the record's 68K
 * address as a host pointer. */
void ending_camera_shake_update(void)
{
  W16_SET(0x172EE, (int16_t)(W16(0x172EE) - 1));
  if (W16(0x172EE) >= 0) {
    ea_t t = ea_norm((ea_t)W[0x172D4]);
    uint32_t dst = 0x0CE8;
    int32_t d1 = (int16_t)(W16(0x172F0) - W16(0x172EE));
    if (ea_rd32(t + 12) != 0) {
      W[dst] = (int32_t)W[dst] + vrd16s(0x20B006 + ((d1 * ea_rd32(t + 8)) & 0xFFFC)) / ea_rd32(t + 12);
      dst += 4;
    }
    if (ea_rd32(t + 4) != 0) {
      W[dst] = (int32_t)W[dst] + vrd16s(0x20B006 + ((d1 * ea_rd32(t)) & 0xFFFC)) / ea_rd32(t + 4);
      dst += 4;
    }
    if (ea_rd32(t + 20) != 0)
      W[dst] = (int32_t)W[dst] + vrd16s(0x20B006 + ((d1 * ea_rd32(t + 16)) & 0xFFFC)) / ea_rd32(t + 20);
  }
  dsp_viewport_setup(3, 2);
  return;
}

/* ---- ending_character_ik_solve @ 0x02D8AE -------------------------------
 *
 * One damped two-angle aim (phase 8's head/arm/eye trackers). PORTED FROM
 * THE ROM, 0x02D8AE..0x02D98A. Every pointer argument is a 68K address --
 * a node field in work RAM (node+0x24 position, node+0x730 angle pair), a
 * velocity pair (0xE17324 ...), and a target: a node, the camera 0xE00CDC,
 * or the caller's stack local (a host int32[3], see ea68k.h). The transpile
 * took them as host int pointers, so stage9's `(short)W[0x1730C] + 0x730`
 * truncated a 68K address to 16 bits and every access faulted.
 *   yaw   = atan2(t.x - p.x, t.z - p.z)
 *   pitch = atan2(t.y - p.y - lift, ((t.z - p.z) << 12) / (cos(yaw) >> 3))
 * a[0] chases roll - pitch (wrapped at -0x8000), a[1] chases
 * 0x8000 - (yaw + bias); both through v[] with the ROM's `<<4`, `>>1`, `>>9`
 * spring. The a[1] impulse is also left in 0xE17308 for the caller. */

void ending_character_ik_solve(ea_t pos, ea_t ang, ea_t vel, ea_t tgt,
                               int lift, int bias, int roll)
{
  int32_t d0, d1, d2, d3;
  pos = ea_norm(pos); ang = ea_norm(ang); vel = ea_norm(vel); tgt = ea_norm(tgt);
  d2 = math_atan2(ea_rd32(tgt) - ea_rd32(pos), ea_rd32(tgt + 8) - ea_rd32(pos + 8));
  d0 = (int32_t)((uint32_t)(ea_rd32(tgt + 8) - ea_rd32(pos + 8)) << 12);
  d1 = vrd16s(0x20B006 + ((uint32_t)(uint16_t)d2 & 0xfffc)) >> 3;   /* move.w d2,d1 zero-ext */
  d0 = m68k_divs(d0, d1);
  d0 = math_atan2(ea_rd32(tgt + 4) - ea_rd32(pos + 4) - lift, d0);
  d3 = roll - d0;
  if (d3 < -0x8000) d3 += 0x10000;
  d3 -= ea_rd32(ang);
  d3 = (int32_t)((uint32_t)d3 << 4) - (ea_rd32(vel) >> 1);
  ea_wr32(vel, ea_rd32(vel) + d3);
  ea_wr32(ang, ea_rd32(ang) + (ea_rd32(vel) >> 9));
  d2 = 0x8000 - (d2 + bias);
  if (d2 < -0x8000) d2 += 0x10000;
  d2 -= ea_rd32(ang + 4);
  d2 = (int32_t)((uint32_t)d2 << 4) - (ea_rd32(vel + 4) >> 1);
  W[0x17308] = d2;
  ea_wr32(vel + 4, ea_rd32(vel + 4) + d2);
  ea_wr32(ang + 4, ea_rd32(ang + 4) + (ea_rd32(vel + 4) >> 9));
}

/* ---- ending_character_physics_sim @ 0x02DAA8 -----------------------------
 *
 * The portrait's three-link pendulum (phase 8, ending_sequence_stage9 is the
 * only caller). PORTED FROM THE ROM, 0x02DAA8..0x02E1B0, as a mechanical
 * instruction-by-instruction transliteration (one C statement per 68K
 * instruction, the address and mnemonic beside it): the body is ~560
 * instructions of straight-line fixed-point arithmetic and a hand rewrite
 * is where a lost `asr` or a `lea $c000.w` (= -0x4000, sign-extended) goes
 * missing. param_1 is the 68K address of the chain's node block (node +
 * 0x800 of the 0xE17310 list), param_2 a torque. State lives at 0xE17348
 * (angles) and 0xE17354 (velocities), three longs each. The transpile read
 * the node as a host-packed struct at W[0x17310] + N (a 68K address used as
 * a host pointer) and divided by trig reads with no zero guard; every
 * division here is m68k_divs, which does what the 68020 does on a zero
 * divisor in this ROM (the vector is a bare `rte`: quotient unchanged). */

#define PS_CC(v)      do { ps_mode = 0; ps_r = (uint32_t)(v); } while (0)
#define PS_CMP(a, b)  do { ps_mode = 1; ps_a = (uint32_t)(a); ps_b = (uint32_t)(b); } while (0)
#define PS_LE  (ps_mode ? (int32_t)ps_a <= (int32_t)ps_b : (int32_t)ps_r <= 0)
#define PS_PL  ((int32_t)ps_r >= 0)
#define PS_SHL(x, n)  ((uint32_t)(x) << (n))
#define PS_ASR(x, n)  ((uint32_t)((int32_t)(x) >> (n)))

void ending_character_physics_sim(int param_1, int param_2)
{
  uint32_t d0 = 0, d1 = 0, d2 = 0, d3 = 0, d4 = 0, d5 = 0, d6 = 0, d7 = 0;
  uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0;
  uint32_t P0 = (uint32_t)param_1, P1 = (uint32_t)param_2, L4 = 0, L8 = 0, Lc = 0;
  uint32_t ps_r = 0, ps_a = 0, ps_b = 0, stk[4]; int ps_mode = 0, sp = 0;

  (void)L4;
#define CC PS_CC
#define CMP PS_CMP
#define CC_LE PS_LE
#define CC_PL PS_PL
#define SHL PS_SHL
#define ASR PS_ASR
#define PUSH(v) (stk[sp++] = (uint32_t)(v))
#define POP(n)  (sp -= (n))
#define STK(i)  stk[sp - 1 - (i)]
  /* 02DAB0 movea.l  $8(a6), a2               */ a2 = P0;
  /* 02DAB4 movea.l  #$e17354, a3             */ a3 = 0xe17354U;
  /* 02DABA movea.l  #$e17348, a4             */ a4 = 0xe17348U;
  /* 02DAC0 move.l   #$20b004, d4             */ { uint32_t t_ = 0x20b004U; d4 = t_; CC(t_); }
  /* 02DAC6 movea.l  $e17310.l, a0            */ a0 = (uint32_t)ea_rd32(0xe17310U);
  /* 02DACC move.l   $6b0(a0), d0             */ { uint32_t t_ = (uint32_t)ea_rd32((a0 + (uint32_t)1712)); d0 = t_; CC(t_); }
  /* 02DAD0 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DAD6 movea.l  d4, a0                   */ a0 = d4;
  /* 02DAD8 movea.w  $2(a0, d0.l), a0         */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)2));
  /* 02DADC move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
  /* 02DADE lsl.l    #$4, d0                  */ d0 = SHL(d0, 4); CC(d0);
  /* 02DAE0 sub.l    a0, d0                   */ d0 = d0 - a0; CC(d0);
  /* 02DAE2 lsl.l    #$3, d0                  */ d0 = SHL(d0, 3); CC(d0);
  /* 02DAE4 add.l    a0, d0                   */ d0 = d0 + a0; CC(d0);
  /* 02DAE6 moveq    #$f, d1                  */ d1 = 0xfU; CC(d1);
  /* 02DAE8 asr.l    d1, d0                   */ d0 = ASR(d0, d1 & 63); CC(d0);
  /* 02DAEA move.l   d0, -$c(a6)              */ { uint32_t t_ = d0; Lc = t_; CC(t_); }
  /* 02DAEE movea.l  $e17310.l, a0            */ a0 = (uint32_t)ea_rd32(0xe17310U);
  /* 02DAF4 lea.l    $e8f6.w, a1              */ a1 = 0xffffe8f6U;
  /* 02DAF8 adda.l   $6b0(a0), a1             */ a1 = a1 + (uint32_t)ea_rd32((a0 + (uint32_t)1712));
  /* 02DAFC move.l   a1, $30(a2)              */ { uint32_t t_ = a1; ea_wr32((a2 + (uint32_t)48), (int32_t)(t_)); CC(t_); }
  /* 02DB00 move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DB04 add.l    $b8(a2), d0              */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d0);
  /* 02DB08 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DB0E movea.l  d4, a0                   */ a0 = d4;
  /* 02DB10 move.w   (a0, d0.l), d1           */ d1 = (d1 & 0xffff0000U) | ((uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0)) & 0xffffU);
  /* 02DB14 ext.l    d1                       */ d1 = (uint32_t)(int32_t)(int16_t)d1; CC(d1);
  /* 02DB16 move.l   (a3), d0                 */ { uint32_t t_ = (uint32_t)ea_rd32(a3); d0 = t_; CC(t_); }
  /* 02DB18 add.l    $4(a3), d0               */ d0 = d0 + (uint32_t)ea_rd32((a3 + (uint32_t)4)); CC(d0);
  /* 02DB1C muls.l   d0, d1                   */ d1 = (uint32_t)((int32_t)d1 * (int32_t)d0); CC(d1);
  /* 02DB20 move.l   d1, d0                   */ { uint32_t t_ = d1; d0 = t_; CC(t_); }
  /* 02DB22 lsl.l    #$2, d0                  */ d0 = SHL(d0, 2); CC(d0);
  /* 02DB24 add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DB26 lsl.l    #$3, d0                  */ d0 = SHL(d0, 3); CC(d0);
  /* 02DB28 sub.l    d1, d0                   */ d0 = d0 - d1; CC(d0);
  /* 02DB2A add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DB2C divs.l   #$28be, d0               */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)0x28beU); CC(d0);
  /* 02DB34 moveq    #$f, d1                  */ d1 = 0xfU; CC(d1);
  /* 02DB36 asr.l    d1, d0                   */ d0 = ASR(d0, d1 & 63); CC(d0);
  /* 02DB38 movea.l  d0, a1                   */ a1 = d0;
  /* 02DB3A move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DB3E andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DB44 move.w   (a0, d0.l), d1           */ d1 = (d1 & 0xffff0000U) | ((uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0)) & 0xffffU);
  /* 02DB48 ext.l    d1                       */ d1 = (uint32_t)(int32_t)(int16_t)d1; CC(d1);
  /* 02DB4A muls.l   (a3), d1                 */ d1 = (uint32_t)((int32_t)d1 * (int32_t)(uint32_t)ea_rd32(a3)); CC(d1);
  /* 02DB4E move.l   d1, d0                   */ { uint32_t t_ = d1; d0 = t_; CC(t_); }
  /* 02DB50 lsl.l    #$4, d0                  */ d0 = SHL(d0, 4); CC(d0);
  /* 02DB52 add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DB54 add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DB56 add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DB58 add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DB5A divs.l   #$28be, d0               */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)0x28beU); CC(d0);
  /* 02DB62 moveq    #$f, d1                  */ d1 = 0xfU; CC(d1);
  /* 02DB64 asr.l    d1, d0                   */ d0 = ASR(d0, d1 & 63); CC(d0);
  /* 02DB66 add.l    a1, d0                   */ d0 = d0 + a1; CC(d0);
  /* 02DB68 moveq    #$2b, d1                 */ d1 = 0x2bU; CC(d1);
  /* 02DB6A add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DB6C move.l   d0, -$8(a6)              */ { uint32_t t_ = d0; L8 = t_; CC(t_); }
  /* 02DB70 move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DB74 add.l    $b8(a2), d0              */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d0);
  /* 02DB78 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DB7E move.w   $2(a0, d0.l), d1         */ d1 = (d1 & 0xffff0000U) | ((uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)2)) & 0xffffU);
  /* 02DB82 ext.l    d1                       */ d1 = (uint32_t)(int32_t)(int16_t)d1; CC(d1);
  /* 02DB84 move.l   (a3), d0                 */ { uint32_t t_ = (uint32_t)ea_rd32(a3); d0 = t_; CC(t_); }
  /* 02DB86 add.l    $4(a3), d0               */ d0 = d0 + (uint32_t)ea_rd32((a3 + (uint32_t)4)); CC(d0);
  /* 02DB8A muls.l   d0, d1                   */ d1 = (uint32_t)((int32_t)d1 * (int32_t)d0); CC(d1);
  /* 02DB8E move.l   d1, d0                   */ { uint32_t t_ = d1; d0 = t_; CC(t_); }
  /* 02DB90 lsl.l    #$2, d0                  */ d0 = SHL(d0, 2); CC(d0);
  /* 02DB92 add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DB94 lsl.l    #$3, d0                  */ d0 = SHL(d0, 3); CC(d0);
  /* 02DB96 sub.l    d1, d0                   */ d0 = d0 - d1; CC(d0);
  /* 02DB98 add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DB9A divs.l   #$28be, d0               */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)0x28beU); CC(d0);
  /* 02DBA2 moveq    #$f, d1                  */ d1 = 0xfU; CC(d1);
  /* 02DBA4 asr.l    d1, d0                   */ d0 = ASR(d0, d1 & 63); CC(d0);
  /* 02DBA6 movea.l  d0, a1                   */ a1 = d0;
  /* 02DBA8 move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DBAC andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DBB2 move.w   $2(a0, d0.l), d1         */ d1 = (d1 & 0xffff0000U) | ((uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)2)) & 0xffffU);
  /* 02DBB6 ext.l    d1                       */ d1 = (uint32_t)(int32_t)(int16_t)d1; CC(d1);
  /* 02DBB8 muls.l   (a3), d1                 */ d1 = (uint32_t)((int32_t)d1 * (int32_t)(uint32_t)ea_rd32(a3)); CC(d1);
  /* 02DBBC move.l   d1, d0                   */ { uint32_t t_ = d1; d0 = t_; CC(t_); }
  /* 02DBBE lsl.l    #$4, d0                  */ d0 = SHL(d0, 4); CC(d0);
  /* 02DBC0 add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DBC2 add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DBC4 add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DBC6 add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DBC8 divs.l   #$28be, d0               */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)0x28beU); CC(d0);
  /* 02DBD0 moveq    #$f, d1                  */ d1 = 0xfU; CC(d1);
  /* 02DBD2 asr.l    d1, d0                   */ d0 = ASR(d0, d1 & 63); CC(d0);
  /* 02DBD4 add.l    $c(a6), d0               */ d0 = d0 + P1; CC(d0);
  /* 02DBD8 add.l    a1, d0                   */ d0 = d0 + a1; CC(d0);
  /* 02DBDA move.l   d0, -$4(a6)              */ { uint32_t t_ = d0; L4 = t_; CC(t_); }
  /* 02DBDE move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DBE2 add.l    $b8(a2), d0              */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d0);
  /* 02DBE6 add.l    $138(a2), d0             */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)312)); CC(d0);
  /* 02DBEA andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DBF0 movea.w  $2(a0, d0.l), a0         */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)2));
  /* 02DBF4 move.l   a0, d3                   */ { uint32_t t_ = a0; d3 = t_; CC(t_); }
  /* 02DBF6 move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DBFA add.l    $b8(a2), d0              */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d0);
  /* 02DBFE add.l    $138(a2), d0             */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)312)); CC(d0);
  /* 02DC02 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DC08 movea.l  d4, a0                   */ a0 = d4;
  /* 02DC0A movea.w  (a0, d0.l), a0           */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0));
  /* 02DC0E move.l   a0, d2                   */ { uint32_t t_ = a0; d2 = t_; CC(t_); }
  /* 02DC10 move.l   d2, d0                   */ { uint32_t t_ = d2; d0 = t_; CC(t_); }
  /* 02DC12 muls.l   d2, d0                   */ d0 = (uint32_t)((int32_t)d0 * (int32_t)d2); CC(d0);
  /* 02DC16 divs.l   d3, d0                   */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)d3); CC(d0);
  /* 02DC1A add.l    d3, d0                   */ d0 = d0 + d3; CC(d0);
  /* 02DC1C move.l   d0, d1                   */ { uint32_t t_ = d0; d1 = t_; CC(t_); }
  /* 02DC1E lsl.l    #$2, d1                  */ d1 = SHL(d1, 2); CC(d1);
  /* 02DC20 add.l    d0, d1                   */ d1 = d1 + d0; CC(d1);
  /* 02DC22 movea.l  d1, a1                   */ a1 = d1;
  /* 02DC24 lsl.l    #$4, d1                  */ d1 = SHL(d1, 4); CC(d1);
  /* 02DC26 sub.l    a1, d1                   */ d1 = d1 - a1; CC(d1);
  /* 02DC28 moveq    #$f, d0                  */ d0 = 0xfU; CC(d0);
  /* 02DC2A asr.l    d0, d1                   */ d1 = ASR(d1, d0 & 63); CC(d1);
  /* 02DC2C move.l   -$8(a6), d0              */ { uint32_t t_ = L8; d0 = t_; CC(t_); }
  /* 02DC30 neg.l    d0                       */ d0 = 0U - d0; CC(d0);
  /* 02DC32 muls.l   d2, d0                   */ d0 = (uint32_t)((int32_t)d0 * (int32_t)d2); CC(d0);
  /* 02DC36 divs.l   d3, d0                   */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)d3); CC(d0);
  /* 02DC3A sub.l    -$4(a6), d0              */ d0 = d0 - L4; CC(d0);
  /* 02DC3E muls.l   #$28be, d0               */ d0 = (uint32_t)((int32_t)d0 * (int32_t)0x28beU); CC(d0);
  /* 02DC46 divs.l   d1, d0                   */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)d1); CC(d0);
  /* 02DC4A movea.l  d0, a0                   */ a0 = d0;
  /* 02DC4C suba.l   (a3), a0                 */ a0 = a0 - (uint32_t)ea_rd32(a3);
  /* 02DC4E suba.l   $4(a3), a0               */ a0 = a0 - (uint32_t)ea_rd32((a3 + (uint32_t)4));
  /* 02DC52 move.l   (a4), d1                 */ { uint32_t t_ = (uint32_t)ea_rd32(a4); d1 = t_; CC(t_); }
  /* 02DC54 add.l    $4(a4), d1               */ d1 = d1 + (uint32_t)ea_rd32((a4 + (uint32_t)4)); CC(d1);
  /* 02DC58 add.l    $8(a4), d1               */ d1 = d1 + (uint32_t)ea_rd32((a4 + (uint32_t)8)); CC(d1);
  /* 02DC5C moveq    #$ee, d0                 */ d0 = 0xffffffeeU; CC(d0);
  /* 02DC5E divs.l   d0, d1                   */ d1 = (uint32_t)m68k_divs((int32_t)d1, (int32_t)d0); CC(d1);
  /* 02DC62 adda.l   d1, a0                   */ a0 = a0 + d1;
  /* 02DC64 move.l   a0, $8(a3)               */ { uint32_t t_ = a0; ea_wr32((a3 + (uint32_t)8), (int32_t)(t_)); CC(t_); }
  /* 02DC68 move.l   (a3), d1                 */ { uint32_t t_ = (uint32_t)ea_rd32(a3); d1 = t_; CC(t_); }
  /* 02DC6A add.l    $4(a3), d1               */ d1 = d1 + (uint32_t)ea_rd32((a3 + (uint32_t)4)); CC(d1);
  /* 02DC6E add.l    $8(a3), d1               */ d1 = d1 + (uint32_t)ea_rd32((a3 + (uint32_t)8)); CC(d1);
  /* 02DC72 muls.l   d2, d1                   */ d1 = (uint32_t)((int32_t)d1 * (int32_t)d2); CC(d1);
  /* 02DC76 move.l   d1, d0                   */ { uint32_t t_ = d1; d0 = t_; CC(t_); }
  /* 02DC78 lsl.l    #$2, d0                  */ d0 = SHL(d0, 2); CC(d0);
  /* 02DC7A add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DC7C movea.l  d0, a1                   */ a1 = d0;
  /* 02DC7E lsl.l    #$4, d0                  */ d0 = SHL(d0, 4); CC(d0);
  /* 02DC80 sub.l    a1, d0                   */ d0 = d0 - a1; CC(d0);
  /* 02DC82 divs.l   #$28be, d0               */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)0x28beU); CC(d0);
  /* 02DC8A tst.l    d0                       */ CC(d0);
  /* 02DC8C bpl.b    $2dc94                   */ if (CC_PL) goto L_02DC94;
  /* 02DC8E addi.l   #$f, d0                  */ d0 = d0 + 0xfU; CC(d0);
L_02DC94:
  /* 02DC94 asr.l    #$4, d0                  */ d0 = ASR(d0, 4); CC(d0);
  /* 02DC96 movea.l  d0, a0                   */ a0 = d0;
  /* 02DC98 move.l   -$8(a6), d1              */ { uint32_t t_ = L8; d1 = t_; CC(t_); }
  /* 02DC9C moveq    #$f, d0                  */ d0 = 0xfU; CC(d0);
  /* 02DC9E lsl.l    d0, d1                   */ d1 = SHL(d1, d0 & 63); CC(d1);
  /* 02DCA0 tst.l    d1                       */ CC(d1);
  /* 02DCA2 bpl.b    $2dcaa                   */ if (CC_PL) goto L_02DCAA;
  /* 02DCA4 addi.l   #$f, d1                  */ d1 = d1 + 0xfU; CC(d1);
L_02DCAA:
  /* 02DCAA asr.l    #$4, d1                  */ d1 = ASR(d1, 4); CC(d1);
  /* 02DCAC add.l    a0, d1                   */ d1 = d1 + a0; CC(d1);
  /* 02DCAE move.l   d1, d6                   */ { uint32_t t_ = d1; d6 = t_; CC(t_); }
  /* 02DCB0 lsl.l    #$2, d6                  */ d6 = SHL(d6, 2); CC(d6);
  /* 02DCB2 add.l    d1, d6                   */ d6 = d6 + d1; CC(d6);
  /* 02DCB4 divs.l   d3, d6                   */ d6 = (uint32_t)m68k_divs((int32_t)d6, (int32_t)d3); CC(d6);
  /* 02DCB8 divs.l   #$200, d6                */ d6 = (uint32_t)m68k_divs((int32_t)d6, (int32_t)0x200U); CC(d6);
  /* 02DCC0 move.l   d3, d5                   */ { uint32_t t_ = d3; d5 = t_; CC(t_); }
  /* 02DCC2 move.l   d2, d7                   */ { uint32_t t_ = d2; d7 = t_; CC(t_); }
  /* 02DCC4 move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DCC8 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DCCE movea.l  d4, a0                   */ a0 = d4;
  /* 02DCD0 move.w   (a0, d0.l), d1           */ d1 = (d1 & 0xffff0000U) | ((uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0)) & 0xffffU);
  /* 02DCD4 ext.l    d1                       */ d1 = (uint32_t)(int32_t)(int16_t)d1; CC(d1);
  /* 02DCD6 muls.l   (a3), d1                 */ d1 = (uint32_t)((int32_t)d1 * (int32_t)(uint32_t)ea_rd32(a3)); CC(d1);
  /* 02DCDA move.l   d1, d0                   */ { uint32_t t_ = d1; d0 = t_; CC(t_); }
  /* 02DCDC lsl.l    #$4, d0                  */ d0 = SHL(d0, 4); CC(d0);
  /* 02DCDE add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DCE0 add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DCE2 add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DCE4 add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DCE6 divs.l   #$28be, d0               */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)0x28beU); CC(d0);
  /* 02DCEE moveq    #$f, d1                  */ d1 = 0xfU; CC(d1);
  /* 02DCF0 asr.l    d1, d0                   */ d0 = ASR(d0, d1 & 63); CC(d0);
  /* 02DCF2 moveq    #$2b, d1                 */ d1 = 0x2bU; CC(d1);
  /* 02DCF4 add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DCF6 move.l   d0, -$8(a6)              */ { uint32_t t_ = d0; L8 = t_; CC(t_); }
  /* 02DCFA move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DCFE andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DD04 move.w   $2(a0, d0.l), d1         */ d1 = (d1 & 0xffff0000U) | ((uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)2)) & 0xffffU);
  /* 02DD08 ext.l    d1                       */ d1 = (uint32_t)(int32_t)(int16_t)d1; CC(d1);
  /* 02DD0A muls.l   (a3), d1                 */ d1 = (uint32_t)((int32_t)d1 * (int32_t)(uint32_t)ea_rd32(a3)); CC(d1);
  /* 02DD0E move.l   d1, d0                   */ { uint32_t t_ = d1; d0 = t_; CC(t_); }
  /* 02DD10 lsl.l    #$4, d0                  */ d0 = SHL(d0, 4); CC(d0);
  /* 02DD12 add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DD14 add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DD16 add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DD18 add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DD1A divs.l   #$28be, d0               */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)0x28beU); CC(d0);
  /* 02DD22 moveq    #$f, d1                  */ d1 = 0xfU; CC(d1);
  /* 02DD24 asr.l    d1, d0                   */ d0 = ASR(d0, d1 & 63); CC(d0);
  /* 02DD26 add.l    $c(a6), d0               */ d0 = d0 + P1; CC(d0);
  /* 02DD2A move.l   d0, -$4(a6)              */ { uint32_t t_ = d0; L4 = t_; CC(t_); }
  /* 02DD2E move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DD32 add.l    $b8(a2), d0              */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d0);
  /* 02DD36 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DD3C movea.w  $2(a0, d0.l), a0         */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)2));
  /* 02DD40 move.l   a0, d3                   */ { uint32_t t_ = a0; d3 = t_; CC(t_); }
  /* 02DD42 move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DD46 add.l    $b8(a2), d0              */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d0);
  /* 02DD4A andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DD50 movea.l  d4, a0                   */ a0 = d4;
  /* 02DD52 movea.w  (a0, d0.l), a0           */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0));
  /* 02DD56 move.l   a0, d2                   */ { uint32_t t_ = a0; d2 = t_; CC(t_); }
  /* 02DD58 move.l   d2, d1                   */ { uint32_t t_ = d2; d1 = t_; CC(t_); }
  /* 02DD5A muls.l   d5, d1                   */ d1 = (uint32_t)((int32_t)d1 * (int32_t)d5); CC(d1);
  /* 02DD5E divs.l   d3, d1                   */ d1 = (uint32_t)m68k_divs((int32_t)d1, (int32_t)d3); CC(d1);
  /* 02DD62 sub.l    d1, d7                   */ d7 = d7 - d1; CC(d7);
  /* 02DD64 muls.l   d6, d7                   */ d7 = (uint32_t)((int32_t)d7 * (int32_t)d6); CC(d7);
  /* 02DD68 move.l   d7, d0                   */ { uint32_t t_ = d7; d0 = t_; CC(t_); }
  /* 02DD6A bpl.b    $2dd6e                   */ if (CC_PL) goto L_02DD6E;
  /* 02DD6C addq.l   #$3, d0                  */ d0 = d0 + 0x3U; CC(d0);
L_02DD6E:
  /* 02DD6E asr.l    #$2, d0                  */ d0 = ASR(d0, 2); CC(d0);
  /* 02DD70 moveq    #$d, d1                  */ d1 = 0xdU; CC(d1);
  /* 02DD72 lsl.l    d1, d0                   */ d0 = SHL(d0, d1 & 63); CC(d0);
  /* 02DD74 divs.l   #$8000, d0               */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)0x8000U); CC(d0);
  /* 02DD7C move.l   -$8(a6), d1              */ { uint32_t t_ = L8; d1 = t_; CC(t_); }
  /* 02DD80 neg.l    d1                       */ d1 = 0U - d1; CC(d1);
  /* 02DD82 muls.l   d2, d1                   */ d1 = (uint32_t)((int32_t)d1 * (int32_t)d2); CC(d1);
  /* 02DD86 divs.l   d3, d1                   */ d1 = (uint32_t)m68k_divs((int32_t)d1, (int32_t)d3); CC(d1);
  /* 02DD8A sub.l    -$4(a6), d1              */ d1 = d1 - L4; CC(d1);
  /* 02DD8E muls.l   #$28be, d1               */ d1 = (uint32_t)((int32_t)d1 * (int32_t)0x28beU); CC(d1);
  /* 02DD96 movea.l  d1, a0                   */ a0 = d1;
  /* 02DD98 adda.l   d0, a0                   */ a0 = a0 + d0;
  /* 02DD9A move.l   d2, d0                   */ { uint32_t t_ = d2; d0 = t_; CC(t_); }
  /* 02DD9C muls.l   d2, d0                   */ d0 = (uint32_t)((int32_t)d0 * (int32_t)d2); CC(d0);
  /* 02DDA0 divs.l   d3, d0                   */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)d3); CC(d0);
  /* 02DDA4 add.l    d3, d0                   */ d0 = d0 + d3; CC(d0);
  /* 02DDA6 move.l   d0, d1                   */ { uint32_t t_ = d0; d1 = t_; CC(t_); }
  /* 02DDA8 lsl.l    #$2, d1                  */ d1 = SHL(d1, 2); CC(d1);
  /* 02DDAA add.l    d0, d1                   */ d1 = d1 + d0; CC(d1);
  /* 02DDAC lsl.l    #$3, d1                  */ d1 = SHL(d1, 3); CC(d1);
  /* 02DDAE sub.l    d0, d1                   */ d1 = d1 - d0; CC(d1);
  /* 02DDB0 add.l    d1, d1                   */ d1 = d1 + d1; CC(d1);
  /* 02DDB2 moveq    #$f, d0                  */ d0 = 0xfU; CC(d0);
  /* 02DDB4 asr.l    d0, d1                   */ d1 = ASR(d1, d0 & 63); CC(d1);
  /* 02DDB6 move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
  /* 02DDB8 divs.l   d1, d0                   */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)d1); CC(d0);
  /* 02DDBC movea.l  d0, a0                   */ a0 = d0;
  /* 02DDBE suba.l   (a3), a0                 */ a0 = a0 - (uint32_t)ea_rd32(a3);
  /* 02DDC0 move.l   (a4), d1                 */ { uint32_t t_ = (uint32_t)ea_rd32(a4); d1 = t_; CC(t_); }
  /* 02DDC2 add.l    $4(a4), d1               */ d1 = d1 + (uint32_t)ea_rd32((a4 + (uint32_t)4)); CC(d1);
  /* 02DDC6 moveq    #$ee, d0                 */ d0 = 0xffffffeeU; CC(d0);
  /* 02DDC8 divs.l   d0, d1                   */ d1 = (uint32_t)m68k_divs((int32_t)d1, (int32_t)d0); CC(d1);
  /* 02DDCC adda.l   d1, a0                   */ a0 = a0 + d1;
  /* 02DDCE move.l   a0, $4(a3)               */ { uint32_t t_ = a0; ea_wr32((a3 + (uint32_t)4), (int32_t)(t_)); CC(t_); }
  /* 02DDD2 move.l   (a3), d1                 */ { uint32_t t_ = (uint32_t)ea_rd32(a3); d1 = t_; CC(t_); }
  /* 02DDD4 add.l    $4(a3), d1               */ d1 = d1 + (uint32_t)ea_rd32((a3 + (uint32_t)4)); CC(d1);
  /* 02DDD8 muls.l   d2, d1                   */ d1 = (uint32_t)((int32_t)d1 * (int32_t)d2); CC(d1);
  /* 02DDDC move.l   d1, d0                   */ { uint32_t t_ = d1; d0 = t_; CC(t_); }
  /* 02DDDE lsl.l    #$2, d0                  */ d0 = SHL(d0, 2); CC(d0);
  /* 02DDE0 add.l    d1, d0                   */ d0 = d0 + d1; CC(d0);
  /* 02DDE2 lsl.l    #$3, d0                  */ d0 = SHL(d0, 3); CC(d0);
  /* 02DDE4 sub.l    d1, d0                   */ d0 = d0 - d1; CC(d0);
  /* 02DDE6 add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DDE8 divs.l   #$28be, d0               */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)0x28beU); CC(d0);
  /* 02DDF0 tst.l    d0                       */ CC(d0);
  /* 02DDF2 bpl.b    $2ddfa                   */ if (CC_PL) goto L_02DDFA;
  /* 02DDF4 addi.l   #$f, d0                  */ d0 = d0 + 0xfU; CC(d0);
L_02DDFA:
  /* 02DDFA asr.l    #$4, d0                  */ d0 = ASR(d0, 4); CC(d0);
  /* 02DDFC movea.l  d0, a0                   */ a0 = d0;
  /* 02DDFE move.l   -$8(a6), d0              */ { uint32_t t_ = L8; d0 = t_; CC(t_); }
  /* 02DE02 moveq    #$f, d1                  */ d1 = 0xfU; CC(d1);
  /* 02DE04 lsl.l    d1, d0                   */ d0 = SHL(d0, d1 & 63); CC(d0);
  /* 02DE06 tst.l    d0                       */ CC(d0);
  /* 02DE08 bpl.b    $2de10                   */ if (CC_PL) goto L_02DE10;
  /* 02DE0A addi.l   #$f, d0                  */ d0 = d0 + 0xfU; CC(d0);
L_02DE10:
  /* 02DE10 asr.l    #$4, d0                  */ d0 = ASR(d0, 4); CC(d0);
  /* 02DE12 add.l    a0, d0                   */ d0 = d0 + a0; CC(d0);
  /* 02DE14 lsl.l    #$2, d0                  */ d0 = SHL(d0, 2); CC(d0);
  /* 02DE16 divs.l   #$200, d0                */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)0x200U); CC(d0);
  /* 02DE1E muls.l   d5, d6                   */ d6 = (uint32_t)((int32_t)d6 * (int32_t)d5); CC(d6);
  /* 02DE22 add.l    d6, d0                   */ d0 = d0 + d6; CC(d0);
  /* 02DE24 divs.l   d3, d0                   */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)d3); CC(d0);
  /* 02DE28 move.l   d0, d6                   */ { uint32_t t_ = d0; d6 = t_; CC(t_); }
  /* 02DE2A move.l   d3, d1                   */ { uint32_t t_ = d3; d1 = t_; CC(t_); }
  /* 02DE2C move.l   d2, d7                   */ { uint32_t t_ = d2; d7 = t_; CC(t_); }
  /* 02DE2E move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DE32 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DE38 movea.l  d4, a0                   */ a0 = d4;
  /* 02DE3A movea.w  $2(a0, d0.l), a0         */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)2));
  /* 02DE3E move.l   a0, d3                   */ { uint32_t t_ = a0; d3 = t_; CC(t_); }
  /* 02DE40 move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DE44 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DE4A movea.l  d4, a0                   */ a0 = d4;
  /* 02DE4C movea.w  (a0, d0.l), a0           */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0));
  /* 02DE50 move.l   a0, d2                   */ { uint32_t t_ = a0; d2 = t_; CC(t_); }
  /* 02DE52 muls.l   d2, d1                   */ d1 = (uint32_t)((int32_t)d1 * (int32_t)d2); CC(d1);
  /* 02DE56 divs.l   d3, d1                   */ d1 = (uint32_t)m68k_divs((int32_t)d1, (int32_t)d3); CC(d1);
  /* 02DE5A sub.l    d1, d7                   */ d7 = d7 - d1; CC(d7);
  /* 02DE5C muls.l   d6, d7                   */ d7 = (uint32_t)((int32_t)d7 * (int32_t)d6); CC(d7);
  /* 02DE60 move.l   d7, d0                   */ { uint32_t t_ = d7; d0 = t_; CC(t_); }
  /* 02DE62 bpl.b    $2de66                   */ if (CC_PL) goto L_02DE66;
  /* 02DE64 addq.l   #$3, d0                  */ d0 = d0 + 0x3U; CC(d0);
L_02DE66:
  /* 02DE66 asr.l    #$2, d0                  */ d0 = ASR(d0, 2); CC(d0);
  /* 02DE68 moveq    #$d, d1                  */ d1 = 0xdU; CC(d1);
  /* 02DE6A lsl.l    d1, d0                   */ d0 = SHL(d0, d1 & 63); CC(d0);
  /* 02DE6C divs.l   #$8000, d0               */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)0x8000U); CC(d0);
  /* 02DE74 moveq    #$d5, d1                 */ d1 = 0xffffffd5U; CC(d1);
  /* 02DE76 muls.l   d2, d1                   */ d1 = (uint32_t)((int32_t)d1 * (int32_t)d2); CC(d1);
  /* 02DE7A divs.l   d3, d1                   */ d1 = (uint32_t)m68k_divs((int32_t)d1, (int32_t)d3); CC(d1);
  /* 02DE7E sub.l    $c(a6), d1               */ d1 = d1 - P1; CC(d1);
  /* 02DE82 muls.l   #$28be, d1               */ d1 = (uint32_t)((int32_t)d1 * (int32_t)0x28beU); CC(d1);
  /* 02DE8A movea.l  d1, a0                   */ a0 = d1;
  /* 02DE8C adda.l   d0, a0                   */ a0 = a0 + d0;
  /* 02DE8E muls.l   d2, d2                   */ d2 = (uint32_t)((int32_t)d2 * (int32_t)d2); CC(d2);
  /* 02DE92 divs.l   d3, d2                   */ d2 = (uint32_t)m68k_divs((int32_t)d2, (int32_t)d3); CC(d2);
  /* 02DE96 add.l    d3, d2                   */ d2 = d2 + d3; CC(d2);
  /* 02DE98 move.l   d2, d1                   */ { uint32_t t_ = d2; d1 = t_; CC(t_); }
  /* 02DE9A lsl.l    #$4, d1                  */ d1 = SHL(d1, 4); CC(d1);
  /* 02DE9C add.l    d2, d1                   */ d1 = d1 + d2; CC(d1);
  /* 02DE9E add.l    d1, d1                   */ d1 = d1 + d1; CC(d1);
  /* 02DEA0 add.l    d2, d1                   */ d1 = d1 + d2; CC(d1);
  /* 02DEA2 add.l    d1, d1                   */ d1 = d1 + d1; CC(d1);
  /* 02DEA4 moveq    #$f, d0                  */ d0 = 0xfU; CC(d0);
  /* 02DEA6 asr.l    d0, d1                   */ d1 = ASR(d1, d0 & 63); CC(d1);
  /* 02DEA8 move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
  /* 02DEAA divs.l   d1, d0                   */ d0 = (uint32_t)m68k_divs((int32_t)d0, (int32_t)d1); CC(d0);
  /* 02DEAE movea.l  d0, a0                   */ a0 = d0;
  /* 02DEB0 move.l   (a4), d1                 */ { uint32_t t_ = (uint32_t)ea_rd32(a4); d1 = t_; CC(t_); }
  /* 02DEB2 moveq    #$ee, d0                 */ d0 = 0xffffffeeU; CC(d0);
  /* 02DEB4 divs.l   d0, d1                   */ d1 = (uint32_t)m68k_divs((int32_t)d1, (int32_t)d0); CC(d1);
  /* 02DEB8 adda.l   d1, a0                   */ a0 = a0 + d1;
  /* 02DEBA move.l   a0, (a3)                 */ { uint32_t t_ = a0; ea_wr32(a3, (int32_t)(t_)); CC(t_); }
  /* 02DEBC move.l   $8(a3), d0               */ { uint32_t t_ = (uint32_t)ea_rd32((a3 + (uint32_t)8)); d0 = t_; CC(t_); }
  /* 02DEC0 add.l    d0, $8(a4)               */ { uint32_t t_ = (uint32_t)ea_rd32((a4 + (uint32_t)8)) + d0; ea_wr32((a4 + (uint32_t)8), (int32_t)(t_)); CC(t_); }
  /* 02DEC4 move.l   $4(a3), d0               */ { uint32_t t_ = (uint32_t)ea_rd32((a3 + (uint32_t)4)); d0 = t_; CC(t_); }
  /* 02DEC8 add.l    d0, $4(a4)               */ { uint32_t t_ = (uint32_t)ea_rd32((a4 + (uint32_t)4)) + d0; ea_wr32((a4 + (uint32_t)4), (int32_t)(t_)); CC(t_); }
  /* 02DECC move.l   (a3), d0                 */ { uint32_t t_ = (uint32_t)ea_rd32(a3); d0 = t_; CC(t_); }
  /* 02DECE add.l    d0, (a4)                 */ { uint32_t t_ = (uint32_t)ea_rd32(a4) + d0; ea_wr32(a4, (int32_t)(t_)); CC(t_); }
  /* 02DED0 move.l   (a4), d0                 */ { uint32_t t_ = (uint32_t)ea_rd32(a4); d0 = t_; CC(t_); }
  /* 02DED2 bpl.b    $2deda                   */ if (CC_PL) goto L_02DEDA;
  /* 02DED4 addi.l   #$f, d0                  */ d0 = d0 + 0xfU; CC(d0);
L_02DEDA:
  /* 02DEDA asr.l    #$4, d0                  */ d0 = ASR(d0, 4); CC(d0);
  /* 02DEDC add.l    d0, $38(a2)              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)) + d0; ea_wr32((a2 + (uint32_t)56), (int32_t)(t_)); CC(t_); }
  /* 02DEE0 move.l   $4(a4), d0               */ { uint32_t t_ = (uint32_t)ea_rd32((a4 + (uint32_t)4)); d0 = t_; CC(t_); }
  /* 02DEE4 bpl.b    $2deec                   */ if (CC_PL) goto L_02DEEC;
  /* 02DEE6 addi.l   #$f, d0                  */ d0 = d0 + 0xfU; CC(d0);
L_02DEEC:
  /* 02DEEC asr.l    #$4, d0                  */ d0 = ASR(d0, 4); CC(d0);
  /* 02DEEE add.l    d0, $b8(a2)              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)184)) + d0; ea_wr32((a2 + (uint32_t)184), (int32_t)(t_)); CC(t_); }
  /* 02DEF2 move.l   $8(a4), d0               */ { uint32_t t_ = (uint32_t)ea_rd32((a4 + (uint32_t)8)); d0 = t_; CC(t_); }
  /* 02DEF6 bpl.b    $2defe                   */ if (CC_PL) goto L_02DEFE;
  /* 02DEF8 addi.l   #$f, d0                  */ d0 = d0 + 0xfU; CC(d0);
L_02DEFE:
  /* 02DEFE asr.l    #$4, d0                  */ d0 = ASR(d0, 4); CC(d0);
  /* 02DF00 add.l    d0, $138(a2)             */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)312)) + d0; ea_wr32((a2 + (uint32_t)312), (int32_t)(t_)); CC(t_); }
  /* 02DF04 movea.l  $e17310.l, a0            */ a0 = (uint32_t)ea_rd32(0xe17310U);
  /* 02DF0A move.l   $6b4(a0), d0             */ { uint32_t t_ = (uint32_t)ea_rd32((a0 + (uint32_t)1716)); d0 = t_; CC(t_); }
  /* 02DF0E andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DF14 movea.l  d4, a0                   */ a0 = d4;
  /* 02DF16 movea.w  (a0, d0.l), a0           */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0));
  /* 02DF1A move.l   a0, d2                   */ { uint32_t t_ = a0; d2 = t_; CC(t_); }
  /* 02DF1C movea.l  $e17310.l, a0            */ a0 = (uint32_t)ea_rd32(0xe17310U);
  /* 02DF22 move.l   $6b4(a0), d0             */ { uint32_t t_ = (uint32_t)ea_rd32((a0 + (uint32_t)1716)); d0 = t_; CC(t_); }
  /* 02DF26 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DF2C movea.l  d4, a0                   */ a0 = d4;
  /* 02DF2E movea.w  $2(a0, d0.l), a0         */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)2));
  /* 02DF32 move.l   a0, d3                   */ { uint32_t t_ = a0; d3 = t_; CC(t_); }
  /* 02DF34 move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DF38 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DF3E movea.l  d4, a0                   */ a0 = d4;
  /* 02DF40 movea.w  (a0, d0.l), a0           */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0));
  /* 02DF44 move.l   a0, d7                   */ { uint32_t t_ = a0; d7 = t_; CC(t_); }
  /* 02DF46 move.l   d7, d1                   */ { uint32_t t_ = d7; d1 = t_; CC(t_); }
  /* 02DF48 lsl.l    #$4, d1                  */ d1 = SHL(d1, 4); CC(d1);
  /* 02DF4A add.l    d7, d1                   */ d1 = d1 + d7; CC(d1);
  /* 02DF4C add.l    d1, d1                   */ d1 = d1 + d1; CC(d1);
  /* 02DF4E add.l    d7, d1                   */ d1 = d1 + d7; CC(d1);
  /* 02DF50 add.l    d1, d1                   */ d1 = d1 + d1; CC(d1);
  /* 02DF52 moveq    #$f, d0                  */ d0 = 0xfU; CC(d0);
  /* 02DF54 asr.l    d0, d1                   */ d1 = ASR(d1, d0 & 63); CC(d1);
  /* 02DF56 muls.l   d2, d1                   */ d1 = (uint32_t)((int32_t)d1 * (int32_t)d2); CC(d1);
  /* 02DF5A move.l   -$c(a6), d6              */ { uint32_t t_ = Lc; d6 = t_; CC(t_); }
  /* 02DF5E muls.l   d3, d6                   */ d6 = (uint32_t)((int32_t)d6 * (int32_t)d3); CC(d6);
  /* 02DF62 sub.l    d1, d6                   */ d6 = d6 - d1; CC(d6);
  /* 02DF64 moveq    #$f, d1                  */ d1 = 0xfU; CC(d1);
  /* 02DF66 asr.l    d1, d6                   */ d6 = ASR(d6, d1 & 63); CC(d6);
  /* 02DF68 moveq    #$16, d0                 */ d0 = 0x16U; CC(d0);
  /* 02DF6A cmp.l    d6, d0                   */ CMP(d0, d6);
  /* 02DF6C ble.b    $2dfdc                   */ if (CC_LE) goto L_02DFDC;
  /* 02DF6E move.l   (a4), d6                 */ { uint32_t t_ = (uint32_t)ea_rd32(a4); d6 = t_; CC(t_); }
  /* 02DF70 moveq    #$f0, d1                 */ d1 = 0xfffffff0U; CC(d1);
  /* 02DF72 divs.l   d1, d6                   */ d6 = (uint32_t)m68k_divs((int32_t)d6, (int32_t)d1); CC(d6);
  /* 02DF76 add.l    $38(a2), d6              */ d6 = d6 + (uint32_t)ea_rd32((a2 + (uint32_t)56)); CC(d6);
  /* 02DF7A move.l   -$c(a6), d5              */ { uint32_t t_ = Lc; d5 = t_; CC(t_); }
  /* 02DF7E muls.l   d3, d5                   */ d5 = (uint32_t)((int32_t)d5 * (int32_t)d3); CC(d5);
  /* 02DF82 addi.l   #$fff50000, d5           */ d5 = d5 + 0xfff50000U; CC(d5);
  /* 02DF88 divs.l   d2, d5                   */ d5 = (uint32_t)m68k_divs((int32_t)d5, (int32_t)d2); CC(d5);
  /* 02DF8C moveq    #$46, d0                 */ d0 = 0x46U; CC(d0);
  /* 02DF8E cmp.l    d5, d0                   */ CMP(d0, d5);
  /* 02DF90 ble.b    $2dfac                   */ if (CC_LE) goto L_02DFAC;
  /* 02DF92 pea.l    $46.w                    */ PUSH(0x46U);
  /* 02DF96 move.l   d5, -(a7)                */ PUSH(d5);
  /* 02DF98 jsr      $a99e.l                  */ d0 = (uint32_t)math_slope_angle((int)STK(0), (int)STK(1));
  /* 02DF9E addq.l   #$8, a7                  */ POP(2);
  /* 02DFA0 move.l   d0, d1                   */ { uint32_t t_ = d0; d1 = t_; CC(t_); }
  /* 02DFA2 lea.l    $4000.w, a0              */ a0 = 0x4000U;
  /* 02DFA6 move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
  /* 02DFA8 sub.l    d1, d0                   */ d0 = d0 - d1; CC(d0);
  /* 02DFAA bra.b    $2dfb2                   */ goto L_02DFB2;
L_02DFAC:
  /* 02DFAC lea.l    $c000.w, a0              */ a0 = 0xffffc000U;
  /* 02DFB0 move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
L_02DFB2:
  /* 02DFB2 move.l   d0, $38(a2)              */ { uint32_t t_ = d0; ea_wr32((a2 + (uint32_t)56), (int32_t)(t_)); CC(t_); }
  /* 02DFB6 move.l   (a4), d5                 */ { uint32_t t_ = (uint32_t)ea_rd32(a4); d5 = t_; CC(t_); }
  /* 02DFB8 sub.l    (a3), d5                 */ d5 = d5 - (uint32_t)ea_rd32(a3); CC(d5);
  /* 02DFBA move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DFBE sub.l    d6, d0                   */ d0 = d0 - d6; CC(d0);
  /* 02DFC0 lsl.l    #$4, d0                  */ d0 = SHL(d0, 4); CC(d0);
  /* 02DFC2 move.l   d0, (a4)                 */ { uint32_t t_ = d0; ea_wr32(a4, (int32_t)(t_)); CC(t_); }
  /* 02DFC4 move.l   (a4), d0                 */ { uint32_t t_ = (uint32_t)ea_rd32(a4); d0 = t_; CC(t_); }
  /* 02DFC6 sub.l    d5, d0                   */ d0 = d0 - d5; CC(d0);
  /* 02DFC8 move.l   d0, (a3)                 */ { uint32_t t_ = d0; ea_wr32(a3, (int32_t)(t_)); CC(t_); }
  /* 02DFCA move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DFCE andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DFD4 movea.l  d4, a0                   */ a0 = d4;
  /* 02DFD6 movea.w  (a0, d0.l), a0           */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0));
  /* 02DFDA move.l   a0, d7                   */ { uint32_t t_ = a0; d7 = t_; CC(t_); }
L_02DFDC:
  /* 02DFDC move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02DFE0 add.l    $b8(a2), d0              */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d0);
  /* 02DFE4 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02DFEA movea.l  d4, a0                   */ a0 = d4;
  /* 02DFEC movea.w  (a0, d0.l), a0           */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0));
  /* 02DFF0 move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
  /* 02DFF2 lsl.l    #$2, d0                  */ d0 = SHL(d0, 2); CC(d0);
  /* 02DFF4 add.l    a0, d0                   */ d0 = d0 + a0; CC(d0);
  /* 02DFF6 lsl.l    #$3, d0                  */ d0 = SHL(d0, 3); CC(d0);
  /* 02DFF8 sub.l    a0, d0                   */ d0 = d0 - a0; CC(d0);
  /* 02DFFA add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02DFFC move.l   d7, d1                   */ { uint32_t t_ = d7; d1 = t_; CC(t_); }
  /* 02DFFE lsl.l    #$4, d1                  */ d1 = SHL(d1, 4); CC(d1);
  /* 02E000 add.l    d7, d1                   */ d1 = d1 + d7; CC(d1);
  /* 02E002 add.l    d1, d1                   */ d1 = d1 + d1; CC(d1);
  /* 02E004 add.l    d7, d1                   */ d1 = d1 + d7; CC(d1);
  /* 02E006 add.l    d1, d1                   */ d1 = d1 + d1; CC(d1);
  /* 02E008 movea.l  d1, a0                   */ a0 = d1;
  /* 02E00A add.l    d0, d1                   */ d1 = d1 + d0; CC(d1);
  /* 02E00C moveq    #$f, d0                  */ d0 = 0xfU; CC(d0);
  /* 02E00E asr.l    d0, d1                   */ d1 = ASR(d1, d0 & 63); CC(d1);
  /* 02E010 muls.l   d2, d1                   */ d1 = (uint32_t)((int32_t)d1 * (int32_t)d2); CC(d1);
  /* 02E014 move.l   -$c(a6), d6              */ { uint32_t t_ = Lc; d6 = t_; CC(t_); }
  /* 02E018 muls.l   d3, d6                   */ d6 = (uint32_t)((int32_t)d6 * (int32_t)d3); CC(d6);
  /* 02E01C sub.l    d1, d6                   */ d6 = d6 - d1; CC(d6);
  /* 02E01E moveq    #$f, d1                  */ d1 = 0xfU; CC(d1);
  /* 02E020 asr.l    d1, d6                   */ d6 = ASR(d6, d1 & 63); CC(d6);
  /* 02E022 moveq    #$23, d0                 */ d0 = 0x23U; CC(d0);
  /* 02E024 cmp.l    d6, d0                   */ CMP(d0, d6);
  /* 02E026 ble.b    $2e09c                   */ if (CC_LE) goto L_02E09C;
  /* 02E028 move.l   $4(a4), d6               */ { uint32_t t_ = (uint32_t)ea_rd32((a4 + (uint32_t)4)); d6 = t_; CC(t_); }
  /* 02E02C moveq    #$f0, d1                 */ d1 = 0xfffffff0U; CC(d1);
  /* 02E02E divs.l   d1, d6                   */ d6 = (uint32_t)m68k_divs((int32_t)d6, (int32_t)d1); CC(d6);
  /* 02E032 add.l    $b8(a2), d6              */ d6 = d6 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d6);
  /* 02E036 move.l   -$c(a6), d5              */ { uint32_t t_ = Lc; d5 = t_; CC(t_); }
  /* 02E03A muls.l   d3, d5                   */ d5 = (uint32_t)((int32_t)d5 * (int32_t)d3); CC(d5);
  /* 02E03E addi.l   #$ffee8000, d5           */ d5 = d5 + 0xffee8000U; CC(d5);
  /* 02E044 divs.l   d2, d5                   */ d5 = (uint32_t)m68k_divs((int32_t)d5, (int32_t)d2); CC(d5);
  /* 02E048 move.l   a0, d1                   */ { uint32_t t_ = a0; d1 = t_; CC(t_); }
  /* 02E04A moveq    #$f, d0                  */ d0 = 0xfU; CC(d0);
  /* 02E04C asr.l    d0, d1                   */ d1 = ASR(d1, d0 & 63); CC(d1);
  /* 02E04E sub.l    d1, d5                   */ d5 = d5 - d1; CC(d5);
  /* 02E050 moveq    #$4e, d0                 */ d0 = 0x4eU; CC(d0);
  /* 02E052 cmp.l    d5, d0                   */ CMP(d0, d5);
  /* 02E054 ble.b    $2e070                   */ if (CC_LE) goto L_02E070;
  /* 02E056 pea.l    $4e.w                    */ PUSH(0x4eU);
  /* 02E05A move.l   d5, -(a7)                */ PUSH(d5);
  /* 02E05C jsr      $a99e.l                  */ d0 = (uint32_t)math_slope_angle((int)STK(0), (int)STK(1));
  /* 02E062 addq.l   #$8, a7                  */ POP(2);
  /* 02E064 move.l   d0, d1                   */ { uint32_t t_ = d0; d1 = t_; CC(t_); }
  /* 02E066 lea.l    $4000.w, a0              */ a0 = 0x4000U;
  /* 02E06A move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
  /* 02E06C sub.l    d1, d0                   */ d0 = d0 - d1; CC(d0);
  /* 02E06E bra.b    $2e076                   */ goto L_02E076;
L_02E070:
  /* 02E070 lea.l    $c000.w, a0              */ a0 = 0xffffc000U;
  /* 02E074 move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
L_02E076:
  /* 02E076 sub.l    $38(a2), d0              */ d0 = d0 - (uint32_t)ea_rd32((a2 + (uint32_t)56)); CC(d0);
  /* 02E07A move.l   d0, $b8(a2)              */ { uint32_t t_ = d0; ea_wr32((a2 + (uint32_t)184), (int32_t)(t_)); CC(t_); }
  /* 02E07E move.l   $4(a4), d5               */ { uint32_t t_ = (uint32_t)ea_rd32((a4 + (uint32_t)4)); d5 = t_; CC(t_); }
  /* 02E082 sub.l    $4(a3), d5               */ d5 = d5 - (uint32_t)ea_rd32((a3 + (uint32_t)4)); CC(d5);
  /* 02E086 move.l   $b8(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)184)); d0 = t_; CC(t_); }
  /* 02E08A sub.l    d6, d0                   */ d0 = d0 - d6; CC(d0);
  /* 02E08C lsl.l    #$4, d0                  */ d0 = SHL(d0, 4); CC(d0);
  /* 02E08E move.l   d0, $4(a4)               */ { uint32_t t_ = d0; ea_wr32((a4 + (uint32_t)4), (int32_t)(t_)); CC(t_); }
  /* 02E092 move.l   $4(a4), d0               */ { uint32_t t_ = (uint32_t)ea_rd32((a4 + (uint32_t)4)); d0 = t_; CC(t_); }
  /* 02E096 sub.l    d5, d0                   */ d0 = d0 - d5; CC(d0);
  /* 02E098 move.l   d0, $4(a3)               */ { uint32_t t_ = d0; ea_wr32((a3 + (uint32_t)4), (int32_t)(t_)); CC(t_); }
L_02E09C:
  /* 02E09C move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02E0A0 add.l    $b8(a2), d0              */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d0);
  /* 02E0A4 add.l    $138(a2), d0             */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)312)); CC(d0);
  /* 02E0A8 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02E0AE movea.l  d4, a0                   */ a0 = d4;
  /* 02E0B0 movea.w  (a0, d0.l), a0           */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0));
  /* 02E0B4 move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
  /* 02E0B6 lsl.l    #$2, d0                  */ d0 = SHL(d0, 2); CC(d0);
  /* 02E0B8 add.l    a0, d0                   */ d0 = d0 + a0; CC(d0);
  /* 02E0BA movea.l  d0, a1                   */ a1 = d0;
  /* 02E0BC lsl.l    #$4, d0                  */ d0 = SHL(d0, 4); CC(d0);
  /* 02E0BE sub.l    a1, d0                   */ d0 = d0 - a1; CC(d0);
  /* 02E0C0 move.l   $38(a2), d1              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d1 = t_; CC(t_); }
  /* 02E0C4 add.l    $b8(a2), d1              */ d1 = d1 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d1);
  /* 02E0C8 andi.l   #$fffc, d1               */ d1 = d1 & 0xfffcU; CC(d1);
  /* 02E0CE movea.l  d4, a0                   */ a0 = d4;
  /* 02E0D0 movea.w  (a0, d1.l), a0           */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d1*1 + (uint32_t)0));
  /* 02E0D4 move.l   a0, d1                   */ { uint32_t t_ = a0; d1 = t_; CC(t_); }
  /* 02E0D6 lsl.l    #$2, d1                  */ d1 = SHL(d1, 2); CC(d1);
  /* 02E0D8 add.l    a0, d1                   */ d1 = d1 + a0; CC(d1);
  /* 02E0DA lsl.l    #$3, d1                  */ d1 = SHL(d1, 3); CC(d1);
  /* 02E0DC sub.l    a0, d1                   */ d1 = d1 - a0; CC(d1);
  /* 02E0DE add.l    d1, d1                   */ d1 = d1 + d1; CC(d1);
  /* 02E0E0 movea.l  d1, a0                   */ a0 = d1;
  /* 02E0E2 move.l   d7, d1                   */ { uint32_t t_ = d7; d1 = t_; CC(t_); }
  /* 02E0E4 lsl.l    #$4, d1                  */ d1 = SHL(d1, 4); CC(d1);
  /* 02E0E6 add.l    d7, d1                   */ d1 = d1 + d7; CC(d1);
  /* 02E0E8 add.l    d1, d1                   */ d1 = d1 + d1; CC(d1);
  /* 02E0EA add.l    d7, d1                   */ d1 = d1 + d7; CC(d1);
  /* 02E0EC add.l    d1, d1                   */ d1 = d1 + d1; CC(d1);
  /* 02E0EE movea.l  d1, a1                   */ a1 = d1;
  /* 02E0F0 add.l    a0, d1                   */ d1 = d1 + a0; CC(d1);
  /* 02E0F2 add.l    d0, d1                   */ d1 = d1 + d0; CC(d1);
  /* 02E0F4 moveq    #$f, d0                  */ d0 = 0xfU; CC(d0);
  /* 02E0F6 asr.l    d0, d1                   */ d1 = ASR(d1, d0 & 63); CC(d1);
  /* 02E0F8 muls.l   d2, d1                   */ d1 = (uint32_t)((int32_t)d1 * (int32_t)d2); CC(d1);
  /* 02E0FC move.l   -$c(a6), d6              */ { uint32_t t_ = Lc; d6 = t_; CC(t_); }
  /* 02E100 muls.l   d3, d6                   */ d6 = (uint32_t)((int32_t)d6 * (int32_t)d3); CC(d6);
  /* 02E104 sub.l    d1, d6                   */ d6 = d6 - d1; CC(d6);
  /* 02E106 moveq    #$f, d1                  */ d1 = 0xfU; CC(d1);
  /* 02E108 asr.l    d1, d6                   */ d6 = ASR(d6, d1 & 63); CC(d6);
  /* 02E10A moveq    #$2a, d0                 */ d0 = 0x2aU; CC(d0);
  /* 02E10C cmp.l    d6, d0                   */ CMP(d0, d6);
  /* 02E10E ble.w    $2e1aa                   */ if (CC_LE) goto L_02E1AA;
  /* 02E112 move.l   $4(a4), d6               */ { uint32_t t_ = (uint32_t)ea_rd32((a4 + (uint32_t)4)); d6 = t_; CC(t_); }
  /* 02E116 moveq    #$f0, d1                 */ d1 = 0xfffffff0U; CC(d1);
  /* 02E118 divs.l   d1, d6                   */ d6 = (uint32_t)m68k_divs((int32_t)d6, (int32_t)d1); CC(d6);
  /* 02E11C add.l    $b8(a2), d6              */ d6 = d6 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d6);
  /* 02E120 move.l   $38(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)56)); d0 = t_; CC(t_); }
  /* 02E124 add.l    $b8(a2), d0              */ d0 = d0 + (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d0);
  /* 02E128 andi.l   #$fffc, d0               */ d0 = d0 & 0xfffcU; CC(d0);
  /* 02E12E movea.l  d4, a0                   */ a0 = d4;
  /* 02E130 movea.w  (a0, d0.l), a0           */ a0 = (uint32_t)(int32_t)(int16_t)(uint32_t)(int32_t)ea_rd16((a0 + d0*1 + (uint32_t)0));
  /* 02E134 move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
  /* 02E136 lsl.l    #$2, d0                  */ d0 = SHL(d0, 2); CC(d0);
  /* 02E138 add.l    a0, d0                   */ d0 = d0 + a0; CC(d0);
  /* 02E13A lsl.l    #$3, d0                  */ d0 = SHL(d0, 3); CC(d0);
  /* 02E13C sub.l    a0, d0                   */ d0 = d0 - a0; CC(d0);
  /* 02E13E add.l    d0, d0                   */ d0 = d0 + d0; CC(d0);
  /* 02E140 move.l   a1, d1                   */ { uint32_t t_ = a1; d1 = t_; CC(t_); }
  /* 02E142 add.l    d0, d1                   */ d1 = d1 + d0; CC(d1);
  /* 02E144 moveq    #$f, d0                  */ d0 = 0xfU; CC(d0);
  /* 02E146 asr.l    d0, d1                   */ d1 = ASR(d1, d0 & 63); CC(d1);
  /* 02E148 muls.l   -$c(a6), d3              */ d3 = (uint32_t)((int32_t)d3 * (int32_t)Lc); CC(d3);
  /* 02E14E addi.l   #$ffeb0000, d3           */ d3 = d3 + 0xffeb0000U; CC(d3);
  /* 02E154 divs.l   d2, d3                   */ d3 = (uint32_t)m68k_divs((int32_t)d3, (int32_t)d2); CC(d3);
  /* 02E158 sub.l    d1, d3                   */ d3 = d3 - d1; CC(d3);
  /* 02E15A moveq    #$4b, d0                 */ d0 = 0x4bU; CC(d0);
  /* 02E15C cmp.l    d3, d0                   */ CMP(d0, d3);
  /* 02E15E ble.b    $2e17a                   */ if (CC_LE) goto L_02E17A;
  /* 02E160 pea.l    $4b.w                    */ PUSH(0x4bU);
  /* 02E164 move.l   d3, -(a7)                */ PUSH(d3);
  /* 02E166 jsr      $a99e.l                  */ d0 = (uint32_t)math_slope_angle((int)STK(0), (int)STK(1));
  /* 02E16C addq.l   #$8, a7                  */ POP(2);
  /* 02E16E move.l   d0, d1                   */ { uint32_t t_ = d0; d1 = t_; CC(t_); }
  /* 02E170 lea.l    $4000.w, a0              */ a0 = 0x4000U;
  /* 02E174 move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
  /* 02E176 sub.l    d1, d0                   */ d0 = d0 - d1; CC(d0);
  /* 02E178 bra.b    $2e180                   */ goto L_02E180;
L_02E17A:
  /* 02E17A lea.l    $c000.w, a0              */ a0 = 0xffffc000U;
  /* 02E17E move.l   a0, d0                   */ { uint32_t t_ = a0; d0 = t_; CC(t_); }
L_02E180:
  /* 02E180 sub.l    $38(a2), d0              */ d0 = d0 - (uint32_t)ea_rd32((a2 + (uint32_t)56)); CC(d0);
  /* 02E184 sub.l    $b8(a2), d0              */ d0 = d0 - (uint32_t)ea_rd32((a2 + (uint32_t)184)); CC(d0);
  /* 02E188 move.l   d0, $138(a2)             */ { uint32_t t_ = d0; ea_wr32((a2 + (uint32_t)312), (int32_t)(t_)); CC(t_); }
  /* 02E18C move.l   $8(a4), d5               */ { uint32_t t_ = (uint32_t)ea_rd32((a4 + (uint32_t)8)); d5 = t_; CC(t_); }
  /* 02E190 sub.l    $8(a3), d5               */ d5 = d5 - (uint32_t)ea_rd32((a3 + (uint32_t)8)); CC(d5);
  /* 02E194 move.l   $b8(a2), d0              */ { uint32_t t_ = (uint32_t)ea_rd32((a2 + (uint32_t)184)); d0 = t_; CC(t_); }
  /* 02E198 sub.l    d6, d0                   */ d0 = d0 - d6; CC(d0);
  /* 02E19A lsl.l    #$4, d0                  */ d0 = SHL(d0, 4); CC(d0);
  /* 02E19C move.l   d0, $8(a4)               */ { uint32_t t_ = d0; ea_wr32((a4 + (uint32_t)8), (int32_t)(t_)); CC(t_); }
  /* 02E1A0 move.l   $8(a4), d0               */ { uint32_t t_ = (uint32_t)ea_rd32((a4 + (uint32_t)8)); d0 = t_; CC(t_); }
  /* 02E1A4 sub.l    d5, d0                   */ d0 = d0 - d5; CC(d0);
  /* 02E1A6 move.l   d0, $8(a3)               */ { uint32_t t_ = d0; ea_wr32((a3 + (uint32_t)8), (int32_t)(t_)); CC(t_); }
L_02E1AA:
  /* 02E1B0 rts                               */ return;
#undef CC
#undef CMP
#undef CC_LE
#undef CC_PL
#undef SHL
#undef ASR
#undef PUSH
#undef POP
#undef STK
}

/* ---- ending_player_physics_sim @ 0x02B226 ---------------------------------
 *
 * PORTED FROM THE ROM (0x02B226..0x02B376), called only from
 * ending_frame_render: the scripted ending moves the node 0xE0B728.. and this
 * derives what the player's own flight model would have produced from the
 * step -- speed 0xD48 (Manhattan distance moved, x16), the pedal rate
 * 0xE02C04 (clamped 0..16), crank angle 0xD94 (+ rate*473), its smoothed copy
 * 0xD98 (`lsr.l #3`, logical), the pedal-sound level 0xE00/0xE04/0xE08, and
 * the pitch/roll lags 0xD50/0xD54 (`adda.w`/`ext.l` of the node's own low
 * words). The two `cmpi.w #$1185, $e172ea` gates stop the crank smoothing
 * and the level decay once phase 7 begins (counter 4485). All 32-bit, as
 * the transpile's intptr_t arithmetic was not. */
void ending_player_physics_sim(void)
{
  int32_t d2, d4, d0, d1, rate;
  d2 = (int32_t)W[0xB728] - (int32_t)W[0x0D00]; if (d2 < 0) d2 = -d2;
  d4 = (int32_t)W[0xB730] - (int32_t)W[0x0D08]; if (d4 < 0) d4 = -d4;
  d2 += d4;
  W[0x0D88] = d2 - ((int32_t)W[0x0D48] >> 4);
  W[0x0D48] = d2;
  W[0x0D8C] = (int32_t)W[0xB72C] - (int32_t)W[0x0D04];
  d1 = (int32_t)((uint32_t)d2 * (uint32_t)d2) >> 13;              /* muls.l */
  rate = ((((int32_t)W[0x0D8C] >> 4) + (int32_t)W[0x0D88] + d1) >> 4);
  if (rate < 0) rate = 0;
  W[0x0D48] = (int32_t)((uint32_t)d2 << 4);
  if (rate > 16) rate = 16;
  W[0x2C04] = rate;
  W[0x0D94] = (int32_t)((uint32_t)(int32_t)W[0x0D94] + (uint32_t)(rate * 0x1d9));
  if (W16(0x172EA) < 0x1185)
    W[0x0D98] = (int32_t)((uint32_t)(int32_t)W[0x0D98]
                          + ((uint32_t)((int32_t)W[0x0D94] - (int32_t)W[0x0D98]) >> 3));
  W[0x0E04] = (int32_t)W[0x0E00];
  W[0x0E00] = rate * 0x4a0;
  d0 = (int32_t)W[0x0D48] >> 2;
  if (d0 > (int32_t)W[0x0E00]) W[0x0E00] = d0;
  if (W16(0x172EA) < 0x1185 && (int32_t)W[0x0E04] > (int32_t)W[0x0E00])
    W[0x0E00] = (int32_t)W[0x0E04] - 30;                          /* moveq #$e2 */
  W[0x0E08] = (int32_t)W[0x0E08] - (int32_t)W[0x0E00];
  d0 = (int32_t)W[0xB734] - (int32_t)W[0x0D0C] + (int16_t)W[0xB734];   /* adda.w */
  W[0x0D50] = (int32_t)W[0x0D50] + ((d0 - (int32_t)W[0x0D50]) >> 2);
  d0 = (((int32_t)W[0xB73C] - (int32_t)W[0x0D14]) >> 1) - (int16_t)W[0xB73C];
  W[0x0D54] = (int32_t)W[0x0D54] + ((d0 - (int32_t)W[0x0D54]) >> 1);
}

/* ---- ending_sequence_stage1 ---- */

/* ROM 0x02B6A4, ported from the machine code -- ENDING PHASE 0 (frames
 * 0..539): the Solitar tower on its island, the lightning strikes, the white
 * flashes and the tower breaking apart. The transpile's version dereferenced
 * the scene-origin ROM address 0x37AA4 as the 68K vector table ("_vec_reset_sp
 * + 0x76b70"), read the event tables one BYTE at a time, dropped the sound ids
 * and left the three-case jump table at 0x02BA26 as a bare `dispatch`; it
 * segfaulted on its first frame.
 *
 * The work record is 0xE17294.. (a3): +0x20 the lightning-event index,
 * +0x24..+0x30 two fall velocities/positions, +0x34 the bolt's angle, +0x38
 * the bolt's remaining frames, +0x3C the cgram load cursor. */
static void end_fade_step(void)
{
  /* 0x02B6C6..0x02B708 (the same prologue opens every ending phase) */
  if ((int16_t)W[0xEB16] > 0) {
    if (W16(0x172FC) != 0 && (int16_t)W[0xEB16] != 0xff) {
      fog_set_from_table(0);
      W16_SET(0x172FA, 0x40);
    } else {
      int16_t v = (int16_t)((int16_t)W[0xEB16] - W16(0x172FA));
      W[0xEB16] = v;
      if (v < 0) W[0xEB16] = 0;
    }
  }
}

/* The Solitar island: sky, the three terrain-chunk lists about `origin`
 * (course 3's chunks, base 0x615), then the castle, the tower and the bridge
 * about the same origin -- 0x02BD24..0x02BD7A and its copies in the other
 * phases. */
static void end_draw_island(uint32_t origin, int clear_mode)
{
  ending_sky_draw();
  scene_objects_draw_list(0x37A7A, origin);
  scene_objects_draw_list(0x37A86, origin);
  scene_objects_draw_list(0x37A94, origin);
  if (clear_mode) W[0x169E8] = 0;
  W[0x4700] = (int32_t)origin;
  W[0x46FC] = (int32_t)origin;
  W[0x46F8] = (int32_t)origin;
  W[0x291C] = 0;
  render_castle_complex();
  render_tower_complex();
  render_bridge_structure();
}

void ending_sequence_stage1(void)
{
  int32_t *p;
  int16_t cnt, d4, q;
  int d2;
  end_fade_step();
  W16_SET(0x172EC, W16(0x172EA));
  cnt = W16(0x172EC);
  if (cnt == 0) sound_play(0x40);
  for (d2 = 0; vrd16s(0x37D4C + d2 * 2) > 0; d2 += 2) {
    if (W16(0x172EC) == vrd16s(0x37D4C + d2 * 2)) {
      int16_t f = vrd16s(0x37D4E + d2 * 2);
      fog_set_from_table(f);
      W16_SET(0x172FA, f != 0 ? 1 : 0x40);
    }
  }
  if (W16(0x172EC) == 0) {
    W16_SET(0x172F2, 0x780);
    W16_SET(0x172F6, 0xf0);
    W16_SET(0x172F8, 0x140);
    W16_SET(0x172F4, 0);
    W[0x172B4] = 0;
    W[0x172CC] = 0;
    W16_SET(0x4708, 0);
    W[0x172D0] = 0x28;
  }
  if (W16(0x172EC) < 0x108)
    camera_interpolate_6dof(0x37CFC, 0x37D18, W16(0x172EC), 0x108, 1);
  else
    camera_set_from_array(0x37D18, 1);
  if (W16(0x172EC) == 0x150) {
    for (d2 = 0; d2 < 6; d2++) W[0x17294 + d2 * 4] = vrd32s(0x37D34 + d2 * 4);
    ending_camera_shake_init(0xE17294, 0xcc);
  }
  W[0x17298] = (int32_t)W[0x17298] + ((int32_t)W[0x17298] >> 5);
  W[0x172A0] = (int32_t)W[0x172A0] + ((int32_t)W[0x172A0] >> 5);
  ending_camera_shake_update();
  if (W16(0x172EC) == 0x15a) sound_play(0x52);
  /* the lightning bolts: (frame, angle, frames) triples at 0x37D66 */
  for (d2 = 0; vrd16s(0x37D66 + d2 * 2) > 0; d2 += 3) {
    if (((int32_t)W16(0x172EC) & ~1) == ((int32_t)vrd16s(0x37D66 + d2 * 2) & ~1)) {
      sound_play(0x2e);
      W[0x172C8] = vrd16s(0x37D68 + d2 * 2);
      W[0x172CC] = vrd16s(0x37D6A + d2 * 2);
      fog_set_from_table(7);
      W16_SET(0x172FA, 1);
    }
  }
  if ((int32_t)W[0x172CC] > 0) {
    uint32_t ang = (uint32_t)W[0x172C8];
    int32_t z;
    dsp_cmd_emit_object_mode_8002();
    p = e_cur();
    *p++ = (int32_t)((uint16_t)W16(0x172EC) & 0xf) + 0x22e;
    if (ang != 0) {
      *p++ = e_rd32(0x37AA4) + ((e_sin(ang) * 0x1e00) >> 15) + 0x3f0cc - (int32_t)W[0x0CDC];
      *p++ = e_rd32(0x37AA8) + 0x1d028 - (int32_t)W[0x0CE0];
      z = e_rd32(0x37AAC) + ((e_cos(ang) * 0x1e00) >> 15) + 0x11c90b;
    } else {
      *p++ = e_rd32(0x37AA4) + 0x3f0cc - (int32_t)W[0x0CDC];
      *p++ = e_rd32(0x37AA8) + 0x1e028 - (int32_t)W[0x0CE0];
      z = e_rd32(0x37AAC) + 0x11c90b;
    }
    *p++ = z - (int32_t)W[0x0CE4];
    *p++ = 0x1090; *p++ = 0x3dd1;
    /* 0x02B95C: `move.l (a4),d0 ; asr.l #1,d0` -- the (sin, cos) pair of
     * -heading at half magnitude, matching the 0.5 of the other two pairs
     * (|(0x1090, 0x3DD1)| = 0x3FFF). The long is the packed [lagged-cos,
     * sin] word; its low half after the shift is sin/2 (see row 95). */
    { uint32_t h = (uint32_t)(-(int32_t)W[0x0CEC]);
      *p++ = e_sin(h) >> 1; *p++ = e_cos(h) >> 1; }
    *p++ = 0; *p++ = 0x3fff; *p++ = 4;
    e_setcur(p);
    W[0x172CC] = (int32_t)W[0x172CC] - 1;
  }
  if (W16(0x4708) & 0x40) {
    if ((W[0x0D00] & 0x1a) == 0) W16_SET(0x4708, W16(0x4708) & 0xffbf);
  } else if ((W[0x0D00] & 0xca) == 0) {
    W16_SET(0x4708, W16(0x4708) | 0x40);
  }
  if (vrd16s(0x37D5E) - 10 < W16(0x172EC)) W16_SET(0x4708, W16(0x4708) & 0xffbf);
  for (d2 = 0; vrd16s(0x37D5E + d2 * 2) > 0; d2++)
    if (W16(0x172EC) == vrd16s(0x37D5E + d2 * 2)) W[0x172B4] = d2 + 1;
  W16_SET(0x4708, W16(0x4708) & 0xffdf);
  d4 = (int16_t)(W16(0x172EC) - vrd16s(0x37D5C + (int32_t)W[0x172B4] * 2));
  /* the jump table at 0x02BA26: event 1 the tower cracking, 2 the pieces
   * start to fall, 3 the pieces falling */
  switch ((int32_t)W[0x172B4]) {
  case 1: {                                                   /* 0x02BA30 */
    int16_t r;
    q = (int16_t)(d4 / 10);
    if (q > 4) { W[0x172B4] = 0; break; }
    r = (int16_t)(((int32_t)d4 >> 1) % 5);
    d4 = r;
    if (r == 0) sound_play(0x2c);
    dsp_cmd_emit_object_mode_8002();
    p = e_cur();
    *p++ = 0x219 + d4;
    *p++ = e_rd32(0x37AA4) + 0x3f0cc - (int32_t)W[0x0CDC];
    *p++ = e_rd32(0x37AA8) + 0x1de28 - (int32_t)W[0x0CE0];
    *p++ = e_rd32(0x37AAC) + 0x11c90b - (int32_t)W[0x0CE4];
    *p++ = 0; *p++ = 0x7fff;
    { uint32_t a = (uint32_t)(((int32_t)q << 16) / 6 + (int32_t)0xffff1293);
      *p++ = e_sin(a); *p++ = e_cos(a); }
    *p++ = 0; *p++ = 0x7fff; *p++ = 4;
    e_setcur(p);
    if (q != 1 && d4 < 2) { fog_set_from_table(7); W16_SET(0x172FA, 0x100); }
    if (d4 == 4) W16_SET(0x4708, W16(0x4708) | 0x20);
    break; }
  case 2:                                                     /* 0x02BB20 */
    W16_SET(0x4708, W16(0x4708) | 0x20);
    W[0x172B8] = 0x18000;
    W[0x172BC] = 0;
    W[0x172C0] = 0x100000;
    W[0x172C4] = 0;
    sound_play(0x2e);
    break;
  case 3: {                                                   /* 0x02BB4E */
    uint32_t a, s5 = 0x5290;
    int k;
    if (W16(0x172EC) == 0x150 || W16(0x172EC) == 0x158) sound_play(0x2e);
    W16_SET(0x4708, W16(0x4708) | 1);
    dsp_cmd_emit_object_mode_8002();
    p = e_cur();
    for (k = 0; k < 6; k++) {
      int32_t t;
      a = (uint32_t)(((int32_t)k << 16) / 6 + (int32_t)0xffff1293);
      *p++ = 0x1f7;
      t = e_sin(a) * d4; if (t < 0) t += 15;
      *p++ = e_rd32(0x37AA4) - (t >> 4) + 0x3f0cc - (int32_t)W[0x0CDC];
      *p++ = e_rd32(0x37AA8) + ((int32_t)W[0x172BC] >> 8) + 0x19028 - (int32_t)W[0x0CE0];
      t = e_cos(a) * d4; if (t < 0) t += 15;
      *p++ = e_rd32(0x37AAC) - (t >> 4) + 0x11c90b - (int32_t)W[0x0CE4];
      { uint32_t b = (uint32_t)(-(int32_t)d4 << 7); *p++ = e_sin(b); *p++ = e_cos(b); }
      *p++ = e_sin(a); *p++ = e_cos(a);
      *p++ = 0; *p++ = 0x7fff; *p++ = 4;
    }
    {
      int32_t t;
      *p++ = 0x1f9;
      t = e_sin(s5) * d4; if (t < 0) t += 7;
      *p++ = e_rd32(0x37AA4) + (t >> 3) + 0x3f0cc - (int32_t)W[0x0CDC];
      *p++ = e_rd32(0x37AA8) + ((int32_t)W[0x172C4] >> 8) + 0x19028 - (int32_t)W[0x0CE0];
      t = e_cos(s5) * d4; if (t < 0) t += 7;
      *p++ = e_rd32(0x37AAC) + (t >> 3) + 0x11c90b - (int32_t)W[0x0CE4];
      { uint32_t b = (uint32_t)(-(int32_t)d4 << 8); *p++ = e_sin(b); *p++ = e_cos(b); }
      *p++ = e_sin(s5); *p++ = e_cos(s5);
      *p++ = 0; *p++ = 0x7fff; *p++ = 4;
    }
    e_setcur(p);
    W[0x172BC] = (int32_t)W[0x172BC] + (int32_t)W[0x172B8];
    W[0x172B8] = (int32_t)W[0x172B8] - 0x28d;
    W[0x172C4] = (int32_t)W[0x172C4] + (int32_t)W[0x172C0];
    W[0x172C0] = (int32_t)W[0x172C0] - 0xa371;
    break; }
  default:
    break;
  }
  if ((int32_t)W[0x172B4] - 2 < 0) FUN_0000fb20(0x2d);
  end_draw_island(0x37AA4, 1);
  W[0x12BC] = 1;
  if (W16(0x172EC) == 0) {
    FUN_000268ce(0xE0B704, 0x37C4C);
    ending_player_reset();
  } else {
    scene_interpolation_evaluate(0xE0B704, W16(0x172EC));
  }
  ending_frame_render();
  /* 0x02BDBE: stream the staff-roll font into cgram, one block a frame */
  { int32_t d1 = (int32_t)W[0x172D0];
    W[0x172D0] = d1 + 1;
    if (d1 < 0x5d)
      cgram_load_tile_block((int32_t)W[0x172D0], vrd8s(0x37ADB + (int32_t)W[0x172D0]) + 0x100); }
  if ((int32_t)W[0x172D0] == 0x5d) {
    int k;
    for (k = 0; k < 0x40; k++) vwr16(0x889000 + k * 2, 0xffff);
  }
  if ((int32_t)W[0x172D0] == 0x5e) cz_load_color_ramp(0x29, 7);
  if ((int32_t)W[0x172D0] == 0x5f) cz_load_color_ramp(0x12b, 0xb);
  if ((int32_t)W[0x172D0] == 0x60) cgram_load_tile_block(0x109, 0x160);
  if ((int32_t)W[0x172D0] == 0x61) cz_load_color_ramp(0x109, 9);
}


/* PROPCYCL_BADDUMP=<dir>:<k1,k2,...> -- write the whole DSP RAM after the
 * bad ending (phase 9) has built its list for LOCAL counter k, as
 * <dir>/ours_k<k>.bin, with the buffer index in the log line. MAME's
 * counterpart is snap_story_badend.lua PCS_DUMPK, whose label is the counter
 * AFTER the frame's increment, i.e. MAME poly_k<k+1>.bin. */
static void badend_dump_probe(int k)
{
  static int init = 0, nt = 0; static int ts[32]; static char dir[256];
  int i; const char *p;
  if (!init) {
    const char *e = getenv("PROPCYCL_BADDUMP"); init = 1;
    if (e) {
      const char *c = strchr(e, ':');
      if (c && (size_t)(c - e) < sizeof dir) {
        memcpy(dir, e, (size_t)(c - e)); dir[c - e] = 0;
        for (p = c + 1; *p && nt < 32; ) {
          ts[nt++] = atoi(p);
          while (*p && *p != ',') p++;
          if (*p == ',') p++;
        }
      }
    }
  }
  for (i = 0; i < nt; i++) {
    if (k == ts[i]) {
      char path[320]; snprintf(path, sizeof path, "%s/ours_k%d.bin", dir, k);
      FILE *f = fopen(path, "wb");
      if (f) { fwrite(g_sys.dspram, 1, DSPRAM_SIZE, f); fclose(f); }
      fprintf(stderr, "[BADDUMP] k=%d buf=%ld -> %s\n", k, (long)W[0x0CA0], path);
      fprintf(stderr, "[BADCAM] k=%d cam %d %d %d %d %d %d ply %d %d %d %d %d %d B728 %d %d %d %d %d %d\n", k,
              (int)W[0x0CDC], (int)W[0x0CE0], (int)W[0x0CE4], (int)W[0x0CE8], (int)W[0x0CEC], (int)W[0x0CF0],
              (int)W[0x0D00], (int)W[0x0D04], (int)W[0x0D08], (int)W[0x0D0C], (int)W[0x0D10], (int)W[0x0D14],
              (int)W[0xB728], (int)W[0xB72C], (int)W[0xB730], (int)W[0xB734], (int)W[0xB738], (int)W[0xB73C]);
      fprintf(stderr, "[BADCAM] k=%d D50 %d %d %d %d %d AB34 %d %d %d\n", k,
              (int)W[0x0D50], (int)W[0x0D54], (int)W[0x0D58], (int)W[0x0D5C], (int)W[0x0D60],
              (int)W[0xAB34], (int)W[0xAB38], (int)W[0xAB3C]);
    }
  }
}

/* ---- ending_sequence_stage10 @ 0x02EE3A -- ending PHASE 9, the BAD ending -
 *
 * Counter 6075..6795 (0x17BB..0x1A8A), entered straight from
 * gameplay_sub14_init when the final stage timed out (E18 != 1): the bike
 * circles the tower, climbs to the sun on a column of lightning, is struck,
 * and plunges; the screen fades out (EB16 = 1, step -2 from local 0x250) and
 * at counter 0x1A8A, in state 3, jumps to phase 11 / counter 0x1F08, which
 * the dispatcher's own increment turns into phase 12 (the END card) on the
 * same frame. PORTED FROM THE ROM (0x02EE3A..0x02F79C), instruction by
 * instruction. The transpile read the whole parameter record 0x3847C..0x3848F
 * ([0x30000, 0x2000, 0x40000, 0x1C000, 0x20], BE longs) one BYTE at a time,
 * so the `divs.l $10(a0)` of the counter-0 init divided by R[0x3848C] = 0 and
 * the first frame of the bad ending died with SIGFPE; it also read the lerp
 * immediates `pea $5000.w` / `lea $3a00.w` / `lea $1600.w` as host pointers
 * (&R[...]), stored `move.l #$30000` as NULL, the two strike tables
 * (0x38490, stride 5 words; 0x384B0, stride 2 words) one byte apart, passed a
 * truncated `(short)&local` as the camera record, and wrote the 0x20B902 /
 * 0x215102 trig longs as CONCAT22 of single bytes.
 *
 * Registers: a3 = 0xE172EC (local counter), a4 = 0xE17294 (work block),
 * d2 = 0xE0AB04 (rider node base; +0xC24.. = 0xE0B728.. the ending player),
 * d5 = 0xE00D00 (player), d6 = 0x3847C (ROM params), d7 = 0xE00CDC (camera). */

/* the packed trig long the ROM copies with `move.l (a0),(a2)+ / move.l
 * $2(a0),(a2)+` off 0x20B002 + a: the master uses the LOW halves, i.e.
 * sin(a) and cos(a) (register row 87). */
#define E10_SIN(a) vrd16s(0x20B004u + ((uint32_t)(a) & 0xfffcu))
#define E10_COS(a) vrd16s(0x20B006u + ((uint32_t)(a) & 0xfffcu))

int ending_sequence_stage10(void)
{
  /* -$14(a6)..-$8(a6): camera_chase_player's offset record (x, y, z, pitch) */
  static int32_t rec[4];
  const uint32_t P = 0x3847C;                 /* d6 */
  int32_t *dl;
  int16_t k, d3;
  int32_t d0, d4;

  W16_SET(0x172EC, (int16_t)(W16(0x172EA) + (int16_t)0xE845));   /* - 0x17BB */
  k = W16(0x172EC);

  /* 02EE72: the fade. tst.w/cmpi.w on the 16-bit level. */
  if ((int16_t)W[0xEB16] > 0) {
    if (W16(0x172FC) != 0 && (int16_t)W[0xEB16] < 0xff) {
      fog_set_from_table(0);
      W16_SET(0x172FA, 0x20);
    } else {
      int16_t v = (int16_t)((int16_t)W[0xEB16] - W16(0x172FA));
      W[0xEB16] = v;
      if (v < 0) W[0xEB16] = 0;
    }
    if ((int16_t)W[0xEB16] > 0xff) W[0xEB16] = 0xff;
  }
  W[0x12BC] = 1;

  /* 02EED0: the ending player's path (0xE0B728.. = x, y, z, pitch, heading, roll) */
  if (k == 0) {
    sound_play(0x56);
    W16_SET(0x172F2, 0x780);
    W16_SET(0x172F6, 0xf0);
    W16_SET(0x172F8, 0x140);
    W16_SET(0x172F4, 0);
    W16_SET(0x4708, 0);
    W[0x17294] = 0; W[0x17298] = 0; W[0x172A0] = 0; W[0x172A8] = 0; W[0x172AC] = 0;
    W[0xB728] = vrd32s(P);
    W[0xB72C] = vrd32s(P + 4);
    W[0xB730] = vrd32s(P + 8) + vrd32s(P + 0xc);
    W[0xB734] = 0;
    W[0xB738] = 0;
    d0 = m68k_divs(m68k_divs(0xb2864d, vrd32s(P + 0x10)), vrd32s(P + 0x10));
    W[0xB73C] = -(int32_t)math_atan2(vrd32s(P + 0xc) * 0x27, d0);
    ending_player_reset();
  }
  else if (k < 0xf0) {                        /* 02EF94: circle the tower */
    d4 = (int32_t)k * vrd32s(P + 0x10);
    W[0xB728] = vrd32s(P) + ((E10_SIN(d4) * (vrd32s(P + 0xc) >> 3)) >> 12);
    W[0xB72C] = vrd32s(P + 4);
    W[0xB730] = vrd32s(P + 8) + ((E10_COS(d4) * (vrd32s(P + 0xc) >> 3)) >> 12);
    W[0xB738] = (int32_t)k * vrd32s(P + 0x10);   /* $c34(d2): the HEADING */
  }
  else if (k < 0x1a4) {                       /* 02F01C: the climb, struck */
    d0 = (int32_t)W[0x172AC];
    if (d0 < 0) d0 += 0xff;
    W[0xB72C] = (int32_t)W[0xB72C] + (d0 >> 8);  W[0x0D04] = W[0xB72C];
    W[0xB734] = (int32_t)W[0xB734] + (int32_t)W[0x17294];  W[0x0D0C] = W[0xB734];
    W[0xB738] = (int32_t)W[0xB738] + (int32_t)W[0x17294] * 2;  W[0x0D10] = W[0xB738];
    W[0xB73C] = (int32_t)W[0xB73C] + (int32_t)W[0x17294];  W[0x0D14] = W[0xB73C];
    if ((int32_t)W[0x17294] > 0) {            /* 02F076: damp the spin to 0 */
      W[0x17294] = (int32_t)W[0x17294] - 0x21;
      if ((int32_t)W[0x17294] < 0) W[0x17294] = 0;
    } else if ((int32_t)W[0x17294] < 0) {
      W[0x17294] = (int32_t)W[0x17294] + 0x1f;
      if ((int32_t)W[0x17294] > 0) W[0x17294] = 0;
    }
    W[0x172AC] = (int32_t)W[0x172AC] + (int32_t)0xffffed40;
  }
  else if (k == 0x1a4) {                      /* 02F0A4: the plunge begins */
    W[0xB034] = (int32_t)W[0xB034] + (int32_t)W[0x3EF8] - 0x2000;   /* lea $e000.w */
    W[0xB434] = (int32_t)W[0xB434] + (int32_t)W[0x3EF0] + 0x1600;
    W[0xB5B4] = (int32_t)W[0xB5B4] + (int32_t)W[0x3EF0] + 0x1c00;
    W[0xB72C] = 0x30000;  W[0x0D04] = 0x30000;
    W[0xB738] = (int32_t)W[0xB738] + 0x40;  W[0x0D10] = W[0xB738];
    W[0xB73C] = -0x3000;  W[0x0D14] = -0x3000;                      /* lea $d000.w */
  }
  else {                                      /* 02F110 */
    W[0xB738] = (int32_t)W[0xB738] + 0x40;   W[0x0D10] = W[0xB738];
    W[0xB73C] = (int32_t)W[0xB73C] + 0x100;  W[0x0D14] = W[0xB73C];
  }

  if (k == 0x1a4) FUN_0000f5ca(0x2f, 0x1e);   /* 02F13C: id 0x2F, value 0x1E */

  /* 02F14C: the camera */
  if (k < 0xb4) {
    camera_set_from_array((ea_t)0x38460, 1);
  } else if (k < 0xf0) {
    rec[0] = math_lerp_int(0x6410, -0x200, 0xb4, 0xf0, k);
    rec[1] = -math_lerp_int(0x5000, -0x100, 0xb4, 0xf0, k);
    rec[2] = -math_lerp_int(0x10000, 0x1800, 0xb4, 0xf0, k);
    rec[3] = (int32_t)0xfffff1c8;
    camera_chase_player(k == 0xb4 ? 0 : 1, (void *)rec);
  } else if (k < 0x1a4) {
    /* 02F1F6: the record is filled and not used -- the camera holds */
    rec[0] = -0x200; rec[1] = -0x100; rec[2] = 0x1800; rec[3] = (int32_t)0xfffff1c8;
  } else {
    W[0x0CDC] = W[0x0D00];
    W[0x0CE4] = W[0x0D08];
    W[0x0CE0] = math_lerp_int(0x500, 0x3000, 0x1a4, 0x2d0, k) + (int32_t)W[0x0D04];
    W[0x0CE8] = -0x4000;
    W[0x0CF0] = 0;
    W[0x0CEC] = -(int32_t)W[0x0D10] * 2;
  }
  ending_camera_shake_update();

  if (k >= 0x1d0) {                           /* 02F26C: the fall's whistle */
    d0 = (0x100 + (int32_t)k) * 0xe1 + (int32_t)0xfffd8730;
    if (d0 < 0) d0 += 0xff;
    FUN_0000f66a(0x2f, (int16_t)((int16_t)(d0 >> 8) + 0x1e));
  }
  if (k == 0x250) {                           /* 02F2A4: fade out */
    W[0xEB16] = 1;
    W16_SET(0x172FA, -2);
  }
  for (d3 = 0; vrd16s(0x384B0 + d3 * 2) > 0; d3 += 2) {   /* 02F2BA: flashes */
    if (k == vrd16s(0x384B0 + d3 * 2)) {
      fog_set_from_table(vrd16s(0x384B2 + d3 * 2));
      W16_SET(0x172FA, 1);
    }
  }
  W16_SET(0x4708, W16(0x4708) & 0xfffd);
  W[0x16670] = (int16_t)(-(int32_t)W[0x0CEC]);           /* move.w */
  W[0x16668] = E10_SIN((uint16_t)W[0x16670]);
  W[0x1666C] = E10_COS((uint16_t)W[0x16670]);
  if (k == 0xa0) sound_play(0x2c);
  if (k < 0xa0) FUN_0000fb20(0x2d);

  if (k >= 0xa0 && k < 0xae) {                /* 02F36C: the bolt on the tower */
    dl = (int32_t *)W[0x0CA4];
    W[0x169E8] = 0x8001;
    dl = dl_put(dl, 0x8001);
    dl = dl_put(dl, (int32_t)W[0x0CC0] == 0xf ? 3 : 0);
    dl = dl_put(dl, 0x8010);
    dl = dl_put(dl, 3);
    dl = dl_put(dl, (int32_t)W[0x3EFC] + (int32_t)0xfffe8600);
    dl = dl_put(dl, -1);
    dl = dl_put(dl, (k & 0xf) + 0x22e);
    dl = dl_put(dl, (int32_t)W[0x3EF0] - 0x800);          /* lea $f800.w */
    dl = dl_put(dl, (int32_t)W[0x3EF4] + 0x3a00);
    dl = dl_put(dl, (int32_t)W[0x3EF8] + 0x26000);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x7fff);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x7fff);
    dl = dl_put(dl, E10_SIN(0x900));                      /* 0x20B902 */
    dl = dl_put(dl, E10_COS(0x900));
    dl = dl_put(dl, 4);
    dl = dl_put(dl, 0x8010);
    dl = dl_put(dl, -1);
    W[0x0CA4] = (intptr_t)dl;
    W[0x4704] = 0;
    W[0x1666C] = 0;
    W[0x16668] = 0;
  }

  /* 02F42A: the strikes, table 0x38490 = 5 words per entry, -1 ends:
   * (counter, spin, bolt angle, burst heading, climb) */
  for (d3 = 0; vrd16s(0x38490 + d3 * 2) > 0; d3 += 5) {
    if (k == vrd16s(0x38490 + d3 * 2)) {
      sound_play(0x2e);
      fog_set_from_table(7);
      W16_SET(0x172FA, 4);
      player_model_load_animation(1);
      W[0x17294] = vrd16s(0x38492 + d3 * 2);
      W[0x17298] = 0;
      if (d3 != 0) {
        W[0x1729C] = vrd16s(0x38494 + d3 * 2);
        W[0x172A0] = 0x10;
      }
      W[0x172A4] = vrd16s(0x38496 + d3 * 2);
      W[0x172A8] = 0x17;
      W[0x172AC] = (int32_t)vrd16s(0x38498 + d3 * 2) << 8;
    }
  }

  if ((int32_t)W[0x172A8] > 0) {              /* 02F4B6: the spark burst */
    W[0x172A8] = (int32_t)W[0x172A8] - 1;
    dsp_cmd_emit_object_mode_8002();
    dl = (int32_t *)W[0x0CA4];
    for (d3 = 0; d3 < 6; d3++) {
      int32_t m = (int32_t)d3 * 3 - (int32_t)W[0x172A8] + 0x21d;
      uint32_t a;
      if (m > 0x21d || m < 0x219) continue;
      dl = dl_put(dl, m);
      dl = dl_put(dl, (int32_t)W[0x0D00] - (int32_t)W[0x0CDC]);
      dl = dl_put(dl, (int32_t)W[0x0D04] - (int32_t)W[0x0CE0]);
      dl = dl_put(dl, (int32_t)W[0x0D08] - (int32_t)W[0x0CE4]);
      /* `move.l (a0),d0 ; asr.l #2` of the PACKED trig long off
       * 0x20B002 + a, i.e. (cos_lag << 16 | sin) >> 2: the master takes the
       * low 16 bits of the word, so the value is sin/4 with cos_lag's two low
       * bits in bits 15:14 -- not sin/4. Measured: MAME's CPU entry at local
       * 244 carries 204300, 832768, ..., and the master's own matrix for the
       * spark has singular values 0.3024/0.0824/0.0625, which the low-16-bit
       * reading reproduces to 4 digits and plain sin/4 does not. */
      a = (uint32_t)(0x8000 - (int32_t)W[0x0CE8]) & 0xfffc;
      dl = dl_put(dl, (int16_t)(vrd32s(0x20B002 + a) >> 2));
      dl = dl_put(dl, (int16_t)(vrd32s(0x20B004 + a) >> 2));
      a = (uint32_t)((int32_t)W[0x172A4] - (int32_t)W[0x0CEC] + (int32_t)d3 * 0x6000) & 0xfffc;
      dl = dl_put(dl, (int16_t)(vrd32s(0x20B002 + a) >> 2));
      dl = dl_put(dl, (int16_t)(vrd32s(0x20B004 + a) >> 2));
      dl = dl_put(dl, 0);
      dl = dl_put(dl, 0x1fff);
      dl = dl_put(dl, 4);
    }
    W[0x0CA4] = (intptr_t)dl;
  }

  if (k < 0xf8 && k >= 0xb4) {                /* 02F596: the column of light */
    int32_t v, q;
    dl = (int32_t *)W[0x0CA4];
    W[0x169E8] = 0x8001;
    dl = dl_put(dl, 0x8001);
    dl = dl_put(dl, (int32_t)W[0x0CC0] == 0xf ? 3 : 0);
    dl = dl_put(dl, (k & 0xf) + 0x22e);
    v = -(int32_t)E10_COS((uint32_t)math_lerp_int(0xc000, 0, 0xb4, 0xf0, k));
    q = (((int32_t)(int16_t)0xff4c + (int32_t)k) >> 4) + 1;   /* lea $ff4c.w: k - 0xb4 */
    v = m68k_divs(v, q);
    dl = dl_put(dl, v);
    dl = dl_put(dl, -v);
    dl = dl_put(dl, 0x18000);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x7fff);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x7fff);
    dl = dl_put(dl, E10_SIN(0xa100));                     /* 0x215102 */
    dl = dl_put(dl, E10_COS(0xa100));
    dl = dl_put(dl, 4);
    W[0x0CA4] = (intptr_t)dl;
  }

  if ((int32_t)W[0x172A0] > 0) {              /* 02F64C: the bolt */
    dl = (int32_t *)W[0x0CA4];
    W[0x169E8] = 0x8001;
    dl = dl_put(dl, 0x8001);
    dl = dl_put(dl, (int32_t)W[0x0CC0] == 0xf ? 3 : 0);
    dl = dl_put(dl, (k & 0xf) + 0x22e);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x18000);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x7fff);
    dl = dl_put(dl, 0);
    dl = dl_put(dl, 0x7fff);
    dl = dl_put(dl, E10_SIN((uint32_t)W[0x1729C]));
    dl = dl_put(dl, E10_COS((uint32_t)W[0x1729C]));
    dl = dl_put(dl, 4);
    W[0x0CA4] = (intptr_t)dl;
    W[0x172A0] = (int32_t)W[0x172A0] - 1;
  }

  ending_sky_draw();                          /* 02F6C8 */
  scene_objects_draw_list((ea_t)0x37A7A, (ea_t)0x37AA4);
  scene_objects_draw_list((ea_t)0x37A86, (ea_t)0x37AA4);
  scene_objects_draw_list((ea_t)0x37A94, (ea_t)0x37AA4);
  W[0x169E8] = 0;
  W[0x4700] = 0x37AA4;                        /* ROM addresses (e_rd32 readers) */
  W[0x46FC] = 0x37AA4;
  W[0x46F8] = 0x37AA4;
  W[0x291C] = 0;
  render_castle_complex();
  render_tower_complex();
  render_bridge_structure();

  if (k < 0xf0) {
    ending_frame_render();
  } else {                                    /* 02F742 */
    player_model_set_pose();
    W[0x17298] = (int32_t)W[0x17298] + 1;
    if ((int32_t)W[0x17298] < 0x20) player_animation_state_update();
    else camera_dsp_terrain_render();
    if (k >= 0x1a4) camera_dsp_sky_render();
  }

  /* 02F772: the END card, via phase 11 / counter 0x1F08 */
  if (W16(0x172EA) == 0x1a8a && (int32_t)W[0x0CBC] == 3) {
    W16_SET(0x172E8, 0xb);
    W16_SET(0x172EA, 0x1f08);
  }
  badend_dump_probe(k);
  return 0;
}
#undef E10_SIN
#undef E10_COS
/* ---- ending_sequence_stage3 ---- */

/* ROM 0x02C146, ported from the machine code -- ENDING PHASE 2 (frames
 * 1560..2144): the fixed camera 0x37F88 while the island rises back through
 * y = 0x2000 .. -0x11000 over 0x249 frames (copy of 0x37AA4 at 0xE17294), the
 * lights on (flags |= 0xE), a +0x10000 zsort bias after the island, and the
 * hero loaded at local frame 0x69 (0x37F80) and animated from there. */
void ending_sequence_stage3(void)
{
  int d2;
  int32_t *p;
  W16_SET(0x172EC, W16(0x172EA) - 1560);
  camera_set_from_array(0x37F88, 1);
  if (W16(0x172EC) == 0) {
    W16_SET(0x4708, W16(0x4708) | 0xe);
    for (d2 = 0; d2 < 7; d2++) W[0x17294 + d2 * 4] = vrd32s(0x37AA4 + d2 * 4);
  }
  W[0x17298] = math_lerp_int(0x2000, -0x11000, 0, 0x249, W16(0x172EC)) + vrd32s(0x37AA8);
  ending_camera_shake_update();
  end_draw_island(0xE17294, 1);
  if ((int32_t)W[0x4704] != 0x10000) {
    p = e_cur();
    *p++ = 0x8010; *p++ = 3;
    W[0x4704] = 0x10000;
    *p++ = 0x10000; *p++ = -1;
    e_setcur(p);
  }
  W[0x12BC] = 1;
  if (W16(0x172EC) == 0x69) {
    FUN_000268ce(0xE0B704, 0x37F80);
    ending_player_reset();
    ending_frame_render();
  }
  W16_SET(0x172EC, W16(0x172EC) - 0x69);
  if (W16(0x172EC) > 0) {
    scene_interpolation_evaluate(0xE0B704, W16(0x172EC));
    ending_frame_render();
  }
}

/* ---- ending_sequence_stage4 ---- */

/* ROM 0x02C29C, ported from the machine code -- ENDING PHASE 3 (frames
 * 2145..2774): the hero on the bike, the camera orbiting him (0x2ACA6 with
 * the offset 0x3805C, eased toward 0x3806C from local frame 0x1C2), while the
 * 3D window shrinks to the upper left for the staff roll (half-size
 * 0x140 x 0xF0 -> 0xD5 x 0xA0 over frames 0x78..0xD2). The hero's animation
 * is 0x3804C, then 0x38054 from frame 0xD2; the island origin is 0x3807C
 * (its y creeps down 0x40 a frame). At counter 0xAD5 the CZ fog registers
 * 0x810000..6 are set to -0x80. */
void ending_sequence_stage4(void)
{
  int d2;
  int32_t loc[4];
  W16_SET(0x172EC, W16(0x172EA) - 2145);
  if (W16(0x172EC) == 0)
    for (d2 = 0; d2 < 4; d2++) W[0x17294 + d2 * 4] = vrd32s(0x3807C + d2 * 4);
  W[0x12BC] = 1;
  if (W16(0x172EC) == 0) {
    FUN_000268ce(0xE0B704, 0x3804C);
    W[0x2A9C] = 0; W[0x2A90] = 0; W[0x2AA8] = 0;
    W[0x2A84] = 0x10000; W[0x2A78] = 0x10000;
    ending_player_reset();
  } else if (W16(0x172EC) == 0xd2) {
    scene_interpolation_evaluate(0xE0B704, W16(0x172EC));
    FUN_000268ce(0xE0B704, 0x38054);
  } else {
    scene_interpolation_evaluate(0xE0B704,
        W16(0x172EC) < 0xd2 ? W16(0x172EC) : (int16_t)(W16(0x172EC) - 0xd2));
  }
  if (W16(0x172EC) < 0x78) {
    W16_SET(0x172F8, 0x140);
    W16_SET(0x172F6, 0xf0);
  } else if (W16(0x172EC) < 0xd2) {
    W16_SET(0x172F8, math_lerp_int(0x140, 0xd5, 0x78, 0xd2, W16(0x172EC)));
    W16_SET(0x172F6, math_lerp_int(0xf0, 0xa0, 0x78, 0xd2, W16(0x172EC)));
  }
  if (W16(0x172EC) == 0) {
    camera_orbit_player(0, 0x3805C);
    for (d2 = 0; d2 < 4; d2++) W[0x172A4 + d2 * 4] = vrd32s(0x3805C + d2 * 4);
  } else if (W16(0x172EC) < 0x1c2) {
    camera_orbit_player(1, 0x3805C);
  } else {
    for (d2 = 0; d2 < 4; d2++)
      loc[d2] = math_lerp_int(vrd32s(0x3805C + d2 * 4), vrd32s(0x3806C + d2 * 4),
                              0x1c2, 0x276, W16(0x172EC));
    W[0x172A4] = (int32_t)W[0x172A4] - (((int32_t)W[0x172A4] - loc[0]) >> 3);
    W[0x172A8] = (int32_t)W[0x172A8] - (((int32_t)W[0x172A8] - loc[1]) >> 3);
    W[0x172AC] = (int32_t)W[0x172AC] - (((int32_t)W[0x172AC] - loc[2]) >> 3);
    W[0x172B0] = (int32_t)W[0x172B0] - (((int32_t)W[0x172B0] - loc[3]) >> 4);
    camera_orbit_player(1, 0xE172A4);
  }
  ending_camera_shake_update();
  end_draw_island(0xE17294, 0);
  W[0x17298] = (int32_t)W[0x17298] - 0x40;
  ending_frame_render();
  if (W16(0x172EA) == 0xad5) {
    vwr16(0x810000, 0xff80); vwr16(0x810002, 0xff80);
    vwr16(0x810004, 0xff80); vwr16(0x810006, 0xff80);
  }
}

/* ---- ending_sequence_stage5 ---- */

/* ROM 0x02C7BE, ported from the machine code -- ENDING PHASE 4 (frames
 * 2775..3374): the hero circling the festival town on the bike (his node
 * 0xE0B728.. driven directly round (0x380D4) at 192 units of angle a frame),
 * the camera 0xE17294 tracking him (atan2 to 0x380D4), the tower, the bridge,
 * the castle, the eight flags/banners rising and spreading from local frame
 * 0x1A4, the CZ fog 0x810000..6 easing -0x80 -> 0 over 0x12C..0x258. The
 * island origin is a stack local on the machine (-$18(a6)): EFRAME(3), since
 * W[0x46F8..0x4700] point at it. */
void ending_sequence_stage5(void)
{
  uint32_t lf = EFRAME(3);
  int32_t *p;
  int32_t d7, sc, a;
  int16_t cnt;
  int d2;
  W16_SET(0x172EC, W16(0x172EA) - 2775);
  cnt = W16(0x172EC);
  e_wr32(lf, 0);
  e_wr32(lf + 8, 0);
  e_wr32(lf + 12, 0x615);
  e_wr32(lf + 4, (int32_t)((uint32_t)(vrd32s(0x380E8) - vrd32s(0x380E4)) * (uint32_t)(int32_t)cnt) / 0x258
                 + vrd32s(0x380E4));
  if (cnt >= 0x12c) {
    int16_t v = (int16_t)math_lerp_int(-0x80, 0, 0x12c, 0x258, cnt);
    vwr16(0x810000, v); vwr16(0x810002, v); vwr16(0x810004, v); vwr16(0x810006, v);
  }
  if (cnt == 0)
    for (d2 = 0; d2 < 7; d2++) W[0x17294 + d2 * 4] = vrd32s(0x380B0 + d2 * 4);
  W[0x17294] = vrd32s(0x380B0);
  W[0x1729C] = vrd32s(0x380B8);
  W[0x172A4] = vrd32s(0x380C0);
  W[0x17298] = math_lerp_int(vrd32s(0x380CC), vrd32s(0x380D0), 0, 0x258, cnt) + vrd32s(0x380B4);
  d7 = (int32_t)math_atan2(vrd32s(0x380D4) - (int32_t)W[0x17294], vrd32s(0x380DC) - (int32_t)W[0x1729C]);
  {
    int32_t dz = (vrd32s(0x380DC) - (int32_t)W[0x1729C]) << 8;
    int32_t c = e_cos((uint16_t)d7) >> 5;
    int32_t h = c ? dz / c : 0;
    int32_t y = (e_rd32(lf + 4) - (int32_t)W[0x17298] + vrd32s(0x380D8)) >> 2;
    W[0x172A0] = -(int32_t)math_atan2(y, h);
  }
  camera_set_from_array(0xE17294, 1);
  ending_camera_shake_update();
  ending_sky_draw();
  scene_objects_draw_list(0x37A7A, lf);
  scene_objects_draw_list(0x37A86, lf);
  scene_objects_draw_list(0x37A94, lf);
  W[0x4700] = (int32_t)lf; W[0x46FC] = (int32_t)lf; W[0x46F8] = (int32_t)lf;
  W[0x291C] = 0;
  render_tower_complex();
  render_bridge_structure();
  dsp_cmd_emit_object_mode_8002();
  e_setcur((int32_t *)render_player_bike_model((int *)e_cur()));
  e_wr32(lf,     e_rd32(lf)     - (int32_t)W[0x0CDC]);
  e_wr32(lf + 4, e_rd32(lf + 4) - (int32_t)W[0x0CE0]);
  e_wr32(lf + 8, e_rd32(lf + 8) - (int32_t)W[0x0CE4]);
  dsp_cmd_emit_object_mode_8000();
  e_setcur((int32_t *)render_gate_or_ring(0x36424, lf, (uint32_t *)e_cur()));
  dsp_cmd_emit_object_mode_8000();
  p = e_cur();
  if (e_rd32(lf + 4) > -0x19200 && (int32_t)W[0x4704] != -0x7000) {
    *p++ = 0x8010; *p++ = 3; W[0x4704] = -0x7000; *p++ = -0x7000; *p++ = -1;
  }
  *p++ = 0x1f4; *p++ = e_rd32(lf); *p++ = e_rd32(lf + 4); *p++ = e_rd32(lf + 8);
  if ((int32_t)W[0x4704] != -0x7000) {
    *p++ = 0x8010; *p++ = 3; W[0x4704] = -0x7000; *p++ = -0x7000; *p++ = -1;
  }
  W[0x169E8] = 0x8002;
  *p++ = 0x8002; *p++ = 3;
  if (cnt >= 0x12c) {
    sc = math_lerp_int(vrd32s(0x380EC), vrd32s(0x380F8), 0x12c, 0x258, cnt);
    d7 = math_lerp_int(vrd32s(0x380F0), vrd32s(0x380FC), 0x12c, 0x258, cnt);
    a  = math_lerp_int(vrd32s(0x380F4), vrd32s(0x38100), 0x12c, 0x258, cnt);
  } else {
    sc = vrd32s(0x380EC); d7 = vrd32s(0x380F0); a = vrd32s(0x380F4);
  }
  e_wr32(lf + 4, e_rd32(lf + 4) + a);
  if (cnt < 0x1a4) {
    for (d2 = 0; d2 < 8; d2++)
      p = (int32_t *)render_flag_banner(d2, lf, sc, (uint32_t)d7, 0, (int *)p);
  } else {
    for (d2 = 0; d2 < 8; d2++) {
      int32_t add, ang;
      e_wr32(lf + 4, e_rd32(lf + 4) + math_lerp_int(0, vrd16s(0x38124 + d2 * 2), 0x1a4, 0x258, cnt));
      add = math_lerp_int(0, vrd16s(0x38114 + d2 * 2), 0x1a4, 0x258, cnt);
      ang = math_lerp_int(0, vrd16s(0x38104 + d2 * 2), 0x1a4, 0x258, cnt) + d7;
      p = (int32_t *)render_flag_banner(d2, lf, sc, (uint32_t)ang, add, (int *)p);
    }
  }
  *p++ = 0x8010; *p++ = -1;
  W[0x4704] = 0;
  e_setcur(p);
  W[0x12BC] = 1;
  {
    int32_t th = (int32_t)cnt * 192 + 0x3800;
    W[0xB738] = th;
    W[0xB728] = vrd32s(0x380D4) + (e_cos((uint32_t)th) * 2) + 0x1a000;
    W[0xB730] = vrd32s(0x380DC) + (e_sin((uint32_t)th) * 2) - 0x15800;
    W[0xB72C] = vrd32s(0x380D8);
    W[0xB73C] = (int32_t)math_atan2(0x1000000, 0x286e390) + (int32_t)0xffff0000;
    W[0xB734] = -0x200;
  }
  if (cnt == 0) ending_player_reset();
  ending_frame_render();
}


/* ---- ending_sequence_stage6 @ 0x02CC5A -- ending PHASE 5 ------------------
 *
 * Counter 3375..3915: the credits' first flyover -- the camera block at ROM
 * 0x38134, the bridge and the floating island, the waterfall's animated
 * models. PORTED FROM THE ROM instruction by instruction (0x02CC5A..0x02CEE8);
 * the transpile read every ROM table as ONE BYTE (`R[0x38174]`), passed host
 * pointers where the ROM passes table addresses, wrote the four CZ attribute
 * WORDS at 0x810000 as overlapping longs, and dropped the memory-indirect
 * `lea ([$37ac4, d0.l*4], $146)` -- a MODEL id read out of a ROM table of
 * longs, not the table's own address. */

void ending_sequence_stage6(void)

{
  /* -$20(a6): (x, y, z, model base) for 0x37A7A;  -$10(a6): the same for
   * 0x37A94. Static, because render_bridge_structure reads the second one
   * through W[0x4700] -- a pointer into this frame on the 68K too. */
  static int32_t pa[4], pb[4];
  int32_t *dl, d0;
  int16_t k;

  W16_SET(0x172EC, (int16_t)(W16(0x172EA) + (int16_t)0xF2D1));   /* - 3375 */
  k = W16(0x172EC);
  if (k == 2) {                             /* 02CC98: four CZ attribute words */
    mem_write16(0x810000, 0xFF80);
    mem_write16(0x810002, 0xFF80);
    mem_write16(0x810004, 0xFF80);
    mem_write16(0x810006, 0xFF80);
  }
  pa[1] = math_lerp_int(vrd32s(0x38174), vrd32s(0x38178), 0, 0x21c, (int)k);
  pb[1] = math_lerp_int(vrd32s(0x38180), vrd32s(0x38184), 0, 0x21c, (int)k);
  camera_set_from_array((ea_t)0x38134, 1);
  ending_camera_shake_update();
  pa[0] = 0; pa[2] = 0; pa[3] = 0x615;
  pb[3] = 0x615; pb[0] = (int32_t)0xFFFE4000; pb[2] = 0xDB000;
  ending_sky_draw_alt();
  scene_objects_draw_list((ea_t)0x38150, (ea_t)0x37AB4);
  scene_objects_draw_list((ea_t)0x37A94, (ea_t)pb);
  W[0x4700] = (intptr_t)pb;
  render_bridge_structure();
  scene_objects_draw_list((ea_t)0x37A7A, (ea_t)pa);
  dsp_cmd_emit_object_mode_8000();
  dl = (int32_t *)W[0x0CA4];
  /* 02CD72: the waterfall group, all at ROM 0x37AB4 - camera */
  W[0x17294] = vrd32s(0x37AB4) - (int32_t)W[0x0CDC];
  W[0x17298] = vrd32s(0x37AB8) - (int32_t)W[0x0CE0];
  W[0x1729C] = vrd32s(0x37ABC) - (int32_t)W[0x0CE4];
#define E6_AT(model) do { dl = dl_put(dl, (model)); dl = dl_put(dl, (int32_t)W[0x17294]); \
    dl = dl_put(dl, (int32_t)W[0x17298]); dl = dl_put(dl, (int32_t)W[0x1729C]); } while (0)
  E6_AT(vrd32s(0x37AC4 + ((k >> 1) & 0xF) * 4) + 0x146);
  E6_AT((k >> 2) % 16 + 0x154);
  E6_AT((k >> 2) % 6 + 0x14E);
  E6_AT((k >> 2) % 4 + 0x16C);
  E6_AT(vrd32s(0x37AC4 + ((k >> 1) & 0xF) * 4) + 0x164);
  E6_AT((k >> 1) % 6 + 0x170);
#undef E6_AT
  d0 = pa[1] - (int32_t)W[0x0CE0];
  if (d0 > (int32_t)0xFFFE6E00 && (int32_t)W[0x4704] != (int32_t)0xFFFF9000) {
    dl = dl_put(dl, 0x8010);
    dl = dl_put(dl, 3);
    W[0x4704] = (int32_t)0xFFFF9000;         /* lea $9000.w: sign-extended */
    dl = dl_put(dl, (int32_t)0xFFFF9000);
    dl = dl_put(dl, -1);
  }
  dl = dl_put(dl, 500);
  dl = dl_put(dl, pa[0] - (int32_t)W[0x0CDC]);
  dl = dl_put(dl, pa[1] - (int32_t)W[0x0CE0]);
  dl = dl_put(dl, pa[2] - (int32_t)W[0x0CE4]);
  if (W[0x4704] != 0) {
    dl = dl_put(dl, 0x8010);
    dl = dl_put(dl, -1);
    W[0x4704] = 0;
  }
  dl = dl_put(dl, 0x8010);
  dl = dl_put(dl, -1);
  W[0x4704] = 0;
  W[0x0CA4] = (intptr_t)dl;
  sync_wait_3();
  return;
}

/* ---- ending_sequence_stage7 @ 0x02CEEA -- ending PHASE 6 ------------------
 *
 * Counter 3915..4485: the town below the cliff, the gondola wheel
 * (ending_ferris_wheel_draw) and the gate. PORTED FROM THE ROM
 * (0x02CEEA..0x02D16C). The transpile copied the shake table one BYTE at a
 * time (`(&R[0x381DC])[i]`) into consecutive SLOTS, read every ROM long as a
 * byte, compared the objshift against a host pointer (`&R[0x01200]`, the
 * immediate 0x1200), and substituted NULL for `lea $df10.w` (-0x20F0). */

void ending_sequence_stage7(void)

{
  static int32_t pos[4];                    /* -$10(a6): x, y, z, model base */
  int32_t *dl, d7, y, top;
  int16_t k, d2;
  int i;

  W16_SET(0x172EC, (int16_t)(W16(0x172EA) + (int16_t)0xF0B5));   /* - 3915 */
  k = W16(0x172EC);
  if (k == 0x11A) {
    for (i = 0; i < 6; i++) W[0x17294 + i * 4] = vrd32s(0x381DC + i * 4);
    ending_camera_shake_init(EA_W(0x17294), 0x11F);
  }
  if (k < 0x11A)
    y = math_lerp_int(vrd32s(0x381D4), vrd32s(0x381D8) + (int32_t)W[0x3EF8], 0, 0x11A, (int)k);
  else {
    W[0x172A0] = (int32_t)W[0x172A0] + ((int32_t)W[0x172A0] >> 4);
    y = vrd32s(0x381D8) + (int32_t)W[0x3EF8];
  }
  pos[1] = y;
  top = vrd32s(0x381D8) + (int32_t)W[0x3EF8] + 0x1200;
  if (top > y) d7 = y - vrd32s(0x381D8) - 0x1200;
  else         d7 = 0xA000;
  camera_set_from_array((ea_t)0x38188, 1);
  ending_camera_shake_update();
  pos[0] = 0; pos[2] = 0; pos[3] = 0x615;
  if (k >= 0x192) {
    W[0x172A8] = 0;
    W[0x17298] = 0;
    d2 = (int16_t)ending_ferris_wheel_draw(vrd32s(0x381D0), y, (int)k - 0x192);
    if (d2 != 0) W[0x172A0] = (int32_t)d2 * vrd32s(0x381E8);
  }
  ending_sky_draw_alt();
  scene_objects_draw_list((ea_t)0x381C0, (ea_t)0x37AB4);
  dl = (int32_t *)W[0x0CA4];
  dl = dl_put(dl, 0x8010);
  dl = dl_put(dl, 3);
  W[0x4704] = ((uint32_t)(y - (int32_t)W[0x0CE0]) <= 0xFFFE6E00u ? (int32_t)0xFFFFDF10 : 0x4FB0)
              + (int32_t)W[0x3EF4];
  dl = dl_put(dl, (int32_t)W[0x4704]);
  dl = dl_put(dl, -1);
  W[0x0CA4] = (intptr_t)dl;
  scene_objects_draw_list((ea_t)0x37A7A, (ea_t)pos);
  pos[0] -= (int32_t)W[0x0CDC];
  pos[1] -= (int32_t)W[0x0CE0];
  pos[2] -= (int32_t)W[0x0CE4];
  dsp_cmd_emit_object_mode_8000();
  W[0x0CA4] = (intptr_t)render_gate_or_ring((ea_t)0x36424, (ea_t)pos, W[0x0CA4]);
  dsp_cmd_emit_object_mode_8000();
  dl = (int32_t *)W[0x0CA4];
  dl = dl_put(dl, 0x8010);
  dl = dl_put(dl, 3);
  W[0x4704] = (pos[1] > (int32_t)0xFFFE6E00 ? (int32_t)0xFFFFE000 : 0x1000) + (int32_t)W[0x3EF4];
  dl = dl_put(dl, (int32_t)W[0x4704]);
  dl = dl_put(dl, -1);
  dl = dl_put(dl, 500);
  dl = dl_put(dl, pos[0]);
  dl = dl_put(dl, pos[1]);
  dl = dl_put(dl, pos[2]);
  dl = dl_put(dl, 0x8010);
  dl = dl_put(dl, 3);
  dl = dl_put(dl, 0x4B00);
  dl = dl_put(dl, -1);
  dl = dl_put(dl, vrd32s(0x37AC4 + ((k >> 1) & 0xF) * 4) + 0x164);
  dl = dl_put(dl, -(int32_t)W[0x0CDC]);
  dl = dl_put(dl, d7 - (int32_t)W[0x0CE0]);
  dl = dl_put(dl, -(int32_t)W[0x0CE4]);
  dl = dl_put(dl, 0x8010);
  dl = dl_put(dl, -1);
  W[0x4704] = 0;
  W[0x0CA4] = (intptr_t)dl;
  return;
}

/* ---- ending_sequence_stage8 @ 0x02D16E -- ending PHASE 7 ------------------
 *
 * Counter 4485..5355: the hero flies in over the town, drops to the ground
 * ahead of the gate and coasts to a stop, the camera swinging round to him.
 * PORTED FROM THE ROM instruction by instruction (0x02D16E..0x02D8AC).
 * Registers: a2 = 0xE17294 (camera block, $1c..$3c = 0x172B0..0x172D0 are
 * the phase's flight state), a3 = 0xE0AB04 (the player node: $c24..$c38 =
 * position/attitude 0xE0B728..0xE0B73C, $5b0 = 0xE0B0B4), a4 = ROM 0x38228,
 * d6 = 0xE17304 (yaw rate), d7 = 0xE172DC. What the transpile had wrong:
 *   - the six-long copy at 0x02D1C6 stepped an `undefined4 *` over _W[]
 *     (four host bytes, i.e. straddling slots) -- the phase-7 SIGSEGV;
 *   - every ROM table read as ONE BYTE (`R[0x38238]`, `R[0x3822C]`);
 *   - the camera-block copy at 0x02D710 (`(a2, d4.w*4)`) at stride one;
 *   - `lerp(0x6000, 0x1000, 0, 0x257, k)` (d3) dropped -- it is the radius
 *     of the approach arc, added to x and fed to atan2 (0x02D26C..0x02D2B8)
 *     and, still live in d3, to the camera heading at 0x02D732;
 *   - `lsr.l #3` (0x02D53E) is LOGICAL, and `W[0x46F8]`/the list arguments
 *     are ROM addresses, not host pointers.
 * Every `>> 6` with an `addi.l #$3f` before it (bpl skips it) is a
 * truncating divide by 64. */
static int32_t e8_div64(int32_t v) { return v / 64; }

void ending_sequence_stage8(void)
{
  int32_t d3 = 0, d0, d1, loc;
  int16_t k, d4;
  uint32_t a;

  W16_SET(0x172EC, (int16_t)(W16(0x172EA) + (int16_t)0xEE7B));   /* - 4485 */
  k = W16(0x172EC);
  if (k == 0) {                                          /* 0x02D1AE */
    W[0x0E0C] = 0;
    for (d4 = 0; d4 < 6; d4++) W[0xB728 + d4 * 4] = vrd32s(0x38228 + d4 * 4);
    W[0x2A84] = 0x13dbb;
    W[0x2A78] = 0x13dbb;
    W[0x2A9C] = 0x13dbb;
    W[0x2A90] = 0x13dbb;
    W[0x2AA8] = 0x13dbb;
    ending_player_reset();
    W[0x172CC] = m68k_divs((int32_t)((uint32_t)(0x2a000 - vrd32s(0x3822C)) << 6), 0x258);
    W[0xB72C] = 0x2a000;
  } else if (k < 0x258) {                               /* 0x02D228 (k < 0x1de) / 0x02D2F0 */
    int32_t prev_yaw = 0;
    W[0xB738] = math_lerp_int((int32_t)0xfffd8000, vrd32s(0x38238), 0, 0x257, k);
    a = (uint32_t)W[0xB738] & 0xfffc;
    d3 = math_lerp_int(0x6000, 0x1000, 0, 0x257, k);
    W[0xB728] = vrd32s(0x38228) + d3 + ((e_cos(a) * d3) >> 15);
    W[0xB730] = ((e_sin(a) * d3) >> 15) + vrd32s(0x38230);
    if (k >= 0x1de) prev_yaw = (int32_t)W[0xB73C];      /* 0x02D366: (d6) = $c38 */
    W[0xB73C] = (int32_t)((uint32_t)math_atan2((int32_t)((uint32_t)d3 << 8), 0xac80f) + 0xffff0000u) / 3;
    if (k < 0x1de) {
      W[0xB72C] = math_lerp_int(0x2a000, vrd32s(0x3822C), 0, 0x257, k);
    } else {
      W[0x17304] = (int32_t)((uint32_t)((int32_t)W[0xB73C] - prev_yaw) << 9);
      W[0xB734] = (int32_t)W[0xB734] + ((0x400 - (int32_t)W[0xB734]) >> 6);
      W[0x172CC] = (int32_t)W[0x172CC] - m68k_divs((int32_t)((uint32_t)(int32_t)W[0xB734] << 6), 0x800);
      if ((int32_t)W[0x172CC] < 0) W[0x172CC] = 0;
      W[0xB72C] = (int32_t)W[0xB72C] - e8_div64((int32_t)W[0x172CC]);
    }
  } else {                                               /* 0x02D3E2 */
    if (k == 0x258) {
      W[0x172B0] = m68k_divs((int32_t)((uint32_t)vrd32s(0x38238) << 12) + 0x28000000, 0x17df8);
      W[0x172B4] = 0x50;
      W[0x172B8] = (int32_t)((uint32_t)(int32_t)W[0xB734] << 6);
      W[0x172BC] = (int32_t)((uint32_t)(int32_t)W[0x172E0] << 6);
      W[0x172C0] = (int32_t)((uint32_t)(int32_t)W[0x172E4] << 6);
      W[0x172C8] = (int32_t)W[0xB0B4];
      W[0x172C4] = (int32_t)W[0xB0B4];
    }
    if ((int32_t)W[0x172B0] > 0x100) {                   /* 0x02D444: still climbing out */
      W[0x172B8] = (int32_t)W[0x172B8] + ((0x80000 - (int32_t)W[0x172B8]) >> 6);
      loc = m68k_divs(vrd32s(0x38238) * 34 + 0x550000, 0x17df8);
      d1 = m68k_divs((int32_t)W[0x172B8], 0x1600);
      if (loc > d1) loc = d1;
      W[0x172B0] = (int32_t)W[0x172B0] - loc;
      W[0x172CC] = (int32_t)W[0x172CC] - m68k_divs((int32_t)W[0x172B8], 0x800);
      if ((int32_t)W[0x172CC] < 0) W[0x172CC] = 0;
      W[0xB72C] = (int32_t)W[0xB72C] - e8_div64((int32_t)W[0x172CC]);
      W[0xB730] = (int32_t)W[0xB730] - e8_div64((int32_t)W[0x172B0]);
      d0 = -(int32_t)W[0xB73C] * 4 - ((int32_t)W[0x17304] >> 4);
      W[0x17304] = (int32_t)W[0x17304] + d0;
      W[0xB73C] = (int32_t)W[0xB73C] + ((int32_t)W[0x17304] >> 9);
      W[0x172BC] = (int32_t)W[0x172BC] + ((0xf80 - (int32_t)W[0x172BC]) >> 3);
      W[0x172C0] = (int32_t)W[0x172C0] + ((0xf00 - (int32_t)W[0x172C0]) >> 3);
      W[0x172C4] = (int32_t)W[0x172C4] + ((-0x1400 - (int32_t)W[0x172C4]) >> 4);  /* lea $ec00.w */
      W[0x172DC] = (int32_t)W[0x172DC] + ((0xd00 - (int32_t)W[0x172DC]) >> 4);
      W[0x0D98] = (int32_t)((uint32_t)(int32_t)W[0x0D98]
                            + ((uint32_t)(0xaa00 - (int32_t)W[0x0D98]) >> 3));    /* lsr.l */
    } else {                                             /* 0x02D54A: landed, coasting */
      W[0x172B4] = (int32_t)W[0x172B4] + 0xa3;
      if ((int32_t)W[0xB72C] < 0x13ebb)
        W[0x172B8] = (int32_t)W[0x172B8] - ((int32_t)W[0x172B8] >> 4);
      W[0xB72C] = (int32_t)W[0xB72C] - e8_div64((int32_t)W[0x172B4]);
      if ((int32_t)W[0xB72C] < 0x13dbb) {
        W[0xB72C] = 0x13dbb;
        if ((int32_t)W[0x172CC] == 0) W[0x172CC] = (int32_t)0xfffffe7f;
        W[0x172B0] = 0;
      }
      W[0x172BC] = (int32_t)W[0x172BC] - ((int32_t)W[0x172BC] >> 4);
      W[0x172C0] = (int32_t)W[0x172C0] - ((int32_t)W[0x172C0] >> 4);
      W[0x172C4] = (int32_t)W[0x172C4] + (((int32_t)W[0x172C8] - (int32_t)W[0x172C4]) >> 4);
      if ((int32_t)W[0x172CC] != 0) {
        d0 = -(int32_t)W[0x172DC] * 128 - ((int32_t)W[0x172CC] >> 1);
        W[0x172CC] = (int32_t)W[0x172CC] + d0;
        W[0x172DC] = (int32_t)W[0x172DC] + ((int32_t)W[0x172CC] >> 9);
      } else {
        W[0x172DC] = (int32_t)W[0x172DC] - ((int32_t)W[0x172DC] >> 4);
      }
    }
    /* 0x02D5EE */
    W[0x172E0] = e8_div64((int32_t)W[0x172BC]);
    W[0x172E4] = e8_div64((int32_t)W[0x172C0]);
    W[0xB730] = (int32_t)W[0xB730] - e8_div64((int32_t)W[0x172B0]);
    W[0xB734] = e8_div64((int32_t)W[0x172B8]);
    W[0xB0B4] = (int32_t)W[0x172C4];
  }
  /* 0x02D640: the player shadow probes (register rows 106/137) */
  W[0x2AA4] = 0;
  W[0x2AA0] = 0;
  { int32_t rx, ry, rz;
    uint32_t p = (uint32_t)W[0xB734], y = (uint32_t)W[0xB738], r = (uint32_t)W[0xB73C];
    rotate_euler_zxy_optimized(0x8c0, 0x100, 0x200, p, y, r, &rx, &ry, &rz);
    W[0x2A88] = rx; W[0x2A8C] = rz;
    rotate_euler_zxy_optimized(-0x8c0, 0x100, 0x200, p, y, r, &rx, &ry, &rz);  /* pea $f740.w */
    W[0x2A94] = rx; W[0x2A98] = rz;
    rotate_euler_zxy_optimized(0, 0, 0x2b0, p, y, r, &rx, &ry, &rz);
    W[0x2A7C] = rx; W[0x2A80] = rz;
    rotate_euler_zxy_optimized(0, 0x4e0, 0x200, p, y, r, &rx, &ry, &rz);
    W[0x2A70] = rx; W[0x2A74] = rz; }
  if (k < 0x258) {                                       /* 0x02D70E */
    int32_t dz, c;
    for (d4 = 0; d4 < 7; d4++) W[0x17294 + d4 * 4] = vrd32s(0x381F4 + d4 * 4);
    W[0x17294] = vrd32s(0x381F4);
    /* 0x02D732: `add.l (a4),d3 ; sub.l (a2),d3` -- d3 is still the arc
     * radius computed above; at k == 0 the ROM never set it (the caller's
     * register), taken as 0 here. */
    W[0x172A4] = (int32_t)math_atan2(d3 + vrd32s(0x38228) - (int32_t)W[0x17294],
                                     vrd32s(0x38230) - (int32_t)W[0x1729C]);
    c = e_cos((uint32_t)W[0x172A4]) >> 5;
    dz = m68k_divs((int32_t)((uint32_t)(vrd32s(0x38230) - (int32_t)W[0x1729C]) << 8), c);
    W[0x172A0] = -(int32_t)math_atan2(((int32_t)W[0xB72C] - vrd32s(0x38214)) >> 2, dz);
    camera_set_from_array((ea_t)0xE17294, 1);
    W[0x172D0] = 0xb400;
  } else {                                               /* 0x02D79A */
    if (k == 0x258)
      while ((int32_t)W[0x172A0] < 0) W[0x172A0] = (int32_t)W[0x172A0] + 0x10000;
    W[0x172A0] = (int32_t)W[0x172A0] + ((vrd32s(0x38200) - (int32_t)W[0x172A0]) >> 6);
    W[0x172A4] = (int32_t)W[0x172A4]
               + m68k_divs((int32_t)((uint32_t)(vrd32s(0x38204) - (int32_t)W[0x172A4]) << 8),
                           (int32_t)W[0x172D0]);
    W[0x172D0] = (int32_t)W[0x172D0] - 0x160;
    if ((int32_t)W[0x172D0] < 0x800) W[0x172D0] = 0x800;
    camera_set_from_array((ea_t)0xE17294, 0);
  }
  ending_camera_shake_update();                          /* 0x02D7FE */
  ending_sky_draw_alt();
  scene_objects_draw_list((ea_t)0x38250, (ea_t)0x37AB4);
  W[0x46F8] = 0x38240;                                   /* movea.l #$38240 -- a ROM address */
  scene_objects_draw_list((ea_t)0x37A7A, (ea_t)0x38240);
  ending_ferris_wheel_draw(vrd32s(0x38244), vrd32s(0x38244), 0);
  W[0x12BC] = 0;
  ending_frame_render();
  /* 0x02D84A: the facing word 0xE16670 (kept as a whole slot, like every
   * other user of it in the tree) and its (sin, cos) */
  W[0x16670] = (int16_t)(-(int32_t)W[0x0CEC]);
  W[0x16668] = e_sin((uint16_t)W[0x16670]);
  W[0x1666C] = e_cos((uint16_t)W[0x16670]);
  ending_object_strip_draw((ea_t)0x3826C, (int32_t)W16(0x172EA), (int32_t)0xffff8000);
  { extern int g_stagedbg;   /* [PH7]: the fields snap_ending.lua's wram dumps carry */
    if (g_stagedbg && (k % 50 == 0 || k % 50 == 14 || k == 868))
      fprintf(stderr, "[PH7] k=%d node=%ld,%ld,%ld,%ld,%ld,%ld cam=%ld,%ld,%ld,%ld,%ld,%ld "
              "st=%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld D48=%ld D98=%ld 72DC=%ld 7304=%ld B0B4=%ld "
              "2C04=%ld E00=%ld,%ld,%ld D94=%ld D50=%ld,%ld 6668=%ld,%ld\n", k,
              (long)W[0xB728], (long)W[0xB72C], (long)W[0xB730], (long)W[0xB734], (long)W[0xB738], (long)W[0xB73C],
              (long)W[0x17294], (long)W[0x17298], (long)W[0x1729C], (long)W[0x172A0], (long)W[0x172A4], (long)W[0x172A8],
              (long)W[0x172B0], (long)W[0x172B4], (long)W[0x172B8], (long)W[0x172BC], (long)W[0x172C0], (long)W[0x172C4],
              (long)W[0x172CC], (long)W[0x172D0],
              (long)W[0x0D48], (long)W[0x0D98], (long)W[0x172DC], (long)W[0x17304], (long)W[0xB0B4],
              (long)W[0x2C04], (long)W[0x0E00], (long)W[0x0E04], (long)W[0x0E08], (long)W[0x0D94], (long)W[0x0D50], (long)W[0x0D54],
              (long)W[0x16668], (long)W[0x1666C]); }
}

/* ---- ending_sequence_stage9 @ 0x02E1B2 -----------------------------------
 *
 * Ending phase 8: the character PORTRAIT (counters 0x14EB..0x17BA). PORTED
 * FROM THE ROM, 0x02E1B2..0x02EE38, instruction by instruction. Prologue:
 * a2 = 0xE172EC (local frame c = counter - 0x14EB, a WORD), a3 = 0xE1730C,
 * a4 = 0xE17310, d7 = 0xE17314, d5 = 0xE166E4 -- four node-chain pointers
 * (68K addresses into the 0xE0AB04 animation nodes), d2 = 0xE17318 (three
 * spring velocities), d3 = 0xE17294 (the camera record handed to
 * camera_set_from_array; +0x20..+0x30 are this phase's scalar springs).
 *
 * What the transpile had wrong (why it faulted on the first frame): the four
 * character lists were loaded from NULL -- ROM 0x02E2E2..0x02E332 pushes the
 * longs at 0x123C44 / 0x123C3C / 0x123C38 / 0x123C40 first and each loader
 * call consumes one -- and every node field was a host-packed dereference of
 * a 68K address (`*(undefined4 *)(W[0x17310] + 0x3b0)`), including the
 * memory-indirect `([,d7.l],$430)` / `([,d5.l],$28)` fields which Ghidra
 * turned into fixed _W[] slots (W[0x17744], W[0x16B18] ...). The ik/pose
 * calls passed 68K addresses truncated to `short`, lost the pose lengths'
 * top bits (0x591a for 0x5591a) and the phase-3 tracker's roll argument
 * (a char of two unrelated slots where the ROM passes d4, the eye swing
 * plus node +0x30 - node +0x3B0). */

static inline int32_t s9_rd(uint32_t a)            { return ea_rd32((ea_t)a); }
static inline void    s9_wr(uint32_t a, int32_t v) { ea_wr32((ea_t)a, v); }
static inline void    s9_add(uint32_t a, int32_t v){ ea_wr32((ea_t)a, ea_rd32((ea_t)a) + v); }

uint32_t ending_sequence_stage9(void)
{
  int16_t c;
  int32_t d0, d1, d4;
  uint32_t n3, n4, n7, n5;          /* (a3) (a4) (d7) (d5): node-chain bases */
  int32_t loc[3];                   /* -$c(a6) -$8(a6) -$4(a6) */
  int i;

  c = (int16_t)(W16(0x172EA) + 0xeb15);
  W16_SET(0x172EC, c);
  if (c >= 0x2d && c <= 0x5f)
    W16_SET(0x172F4, math_lerp_int(0, 0x80, 0x2d, 0x5f, c));
  if (c == 0x24) cgram_load_tile_block(0x61, 0x1a0);
  if (c == 0x25) cgram_load_tile_block(0x6a, 0x1a4);
  if (c == 0x26) cgram_load_tile_block(0x60, 0x1a8);
  if (c == 0x27) cz_load_color_ramp(0x61, 5);           /* `move.b #$5` (row 118) */
  if (c == 300) {
    text_draw_rect_blink(0x61, 0x11, 0x19, 0x1a0, 5);
    text_draw_rect_blink(0x6a, 0x13, 0x19, 0x1a4, 5);
    text_draw_rect_blink(0x60, 0x15, 0x19, 0x1a8, 5);
  }

  if (c == 0) {                                          /* 0x02E2D2 */
    uint32_t a;
    W[0x169F0] = 3;                  /* move.w #3 / #2 (the tree keeps both as slots) */
    W[0x169F2] = 2;
    W[0x1730C] = (int32_t)0xE0AB04;
    W[0x17310] = (int32_t)FUN_000268ce(0xE0AB04, vrd32(0x123C44));
    W[0x17314] = (int32_t)FUN_000268ce((uint32_t)W[0x17310], vrd32(0x123C3C));
    W[0x166E4] = (int32_t)FUN_000268ce((uint32_t)W[0x17314], vrd32(0x123C38));
    FUN_000268ce((uint32_t)W[0x166E4], vrd32(0x123C40));
    n4 = (uint32_t)W[0x17310];
    s9_wr(n4 + 0x3b0, 0x1000);
    s9_wr(n4 + 0x530, 0x1000);
    s9_wr(n4 + 0x3b8, 0xe00);
    s9_wr(n4 + 0x538, -0xe00);
    s9_wr(n4 + 0x430, 0xd00);
    s9_wr(n4 + 0x5b0, 0xd00);
    s9_wr(n4 + 0x434, -0x500);
    s9_wr(n4 + 0x5b4, 0x500);
    s9_wr(n4 + 0x4b4, 0xb00);
    s9_wr(n4 + 0x634, -0xb00);
    s9_wr(n4 + 0x6b4, 0x1100);
    s9_wr((uint32_t)W[0x1730C] + 0x734, -0xb00);
    n7 = (uint32_t)W[0x17314];
    s9_wr(n7 + 0x430, -0x800);                           /* lea $f800.w */
    s9_wr(n7 + 0x5b0, -0x800);
    s9_wr(n7 + 0x434, 0xf600);
    s9_wr(n7 + 0x5b4, -0xf600);
    s9_wr(n7 + 0x438, -0x200);                           /* lea $fe00.w */
    s9_wr(n7 + 0x5b8, 0x200);
    s9_wr(n7 + 0x4b0, 0x3700);
    s9_wr(n7 + 0x630, 0x3700);
    s9_wr(n7 + 0x4b4, 0xd200);
    s9_wr(n7 + 0x634, -0xd200);
    a = (uint32_t)W[0x166E4] + 0x24;                     /* 6 longs from ROM 0x38420 */
    for (i = 0; i < 6; i++) s9_wr(a + i * 4, (int32_t)vrd32(0x38420 + i * 4));
    for (i = 0; i < 7; i++) W[0x17294 + i * 4] = (int32_t)vrd32(0x383F8 + i * 4);
    for (i = 0; i < 3; i++) {
      W[0x17318 + i * 4] = 0; W[0x17324 + i * 4] = 0; W[0x17330 + i * 4] = 0;
      W[0x1733C + i * 4] = 0; W[0x166F4 + i * 4] = 0; W[0x17354 + i * 4] = 0;
    }
    W[0x17348] = 0;
    W[0x1734C] = 0x800;
    W[0x17350] = 0x1000;
    W[0x172C0] = 0;                                      /* d3+$2c/$28/$20 */
    W[0x172BC] = 0;
    W[0x172B4] = 0;
    W[0x172B8] = 0x1e0000;                               /* d3+$24 */
    W[0x172C4] = 0x20000;                                /* d3+$30 */
    g_fog_b = 0xff;
    g_fog_g = 0xff;
    g_fog_r = 0xff;
    W[0xEB16] = 0;
  }
  n3 = (uint32_t)W[0x1730C];
  n4 = (uint32_t)W[0x17310];
  n7 = (uint32_t)W[0x17314];
  n5 = (uint32_t)W[0x166E4];

  /* 0x02E568: two model ids of the first figure (the blink / expression) */
  if (c < 0x78) {
    s9_wr(n3 + 0x780, ((c & 0x3c) == 4) ? 0x3e0 : 0x3e2);
    s9_wr(n3 + 0x700, ((c & 0x38) == 8) ? 0x3e5 : 0x3d9);
  } else if (c >= 0x1c2) {
    s9_wr(n3 + 0x780, 0x3e2);
    s9_wr(n3 + 0x700, 0x3df);
  } else {
    s9_wr(n3 + 0x780, 0x3da);
    s9_wr(n3 + 0x700, 0x3d9);
  }

  /* 0x02E5DC: the first figure's head tracker */
  {
    ea_t tgt = 0;
    int call = 1;
    if (c >= 0x78 && c < 0xff) {                         /* look at the third figure */
      loc[0] = s9_rd(n5 + 0x24);
      loc[1] = s9_rd(n5 + 0x28) + 0x27b;
      loc[2] = s9_rd(n5 + 0x2c);
      tgt = (ea_t)loc;
    } else if (c < 0x12c) {                              /* look at the second */
      loc[0] = s9_rd(n4 + 0x24);
      loc[1] = s9_rd(n4 + 0x28) + 0x236;
      loc[2] = s9_rd(n4 + 0x2c);
      tgt = (ea_t)loc;
    } else if (c < 0x1e0) {
      tgt = (ea_t)0xE00CDC;                              /* at the camera */
    } else {
      call = 0;
    }
    if (call)
      ending_character_ik_solve((ea_t)(n3 + 0x24), (ea_t)(n3 + 0x730), (ea_t)0xE17324,
                                tgt, 0x24e, s9_rd(n3 + 0x34), 0);
  }

  /* 0x02E6B4: the first figure's body springs */
  if (c < 0x78) {
    d4 = (s9_rd(n3 + 0x630) - s9_rd(n3 + 0x4b0)) * 2;
    d4 -= W[0x17318] >> 1;
    W[0x17318] = (int32_t)W[0x17318] + d4;
    s9_add(n3 + 0x4b0, (int32_t)W[0x17318] >> 12);
    d0 = (s9_rd(n3 + 0x5b0) - s9_rd(n3 + 0x430)) * 2;
    d0 -= (int32_t)W[0x17320] >> 1;
    d0 += d4 >> 1;
    W[0x17320] = (int32_t)W[0x17320] + d0;
    s9_add(n3 + 0x430, (int32_t)W[0x17320] >> 12);
  } else if (c < 0x12c) {
    d4 = (int32_t)((uint32_t)(s9_rd(n3 + 0x630) - s9_rd(n3 + 0x4b0)) << 4);
    d4 -= (int32_t)W[0x17318] >> 1;
    W[0x17318] = (int32_t)W[0x17318] + d4;
    s9_add(n3 + 0x4b0, (int32_t)W[0x17318] >> 9);
    d0 = (int32_t)((uint32_t)(-(s9_rd(n3 + 0x5b8) + s9_rd(n3 + 0x438))) << 4);
    d0 -= (int32_t)W[0x1731C] >> 1;
    W[0x1731C] = (int32_t)W[0x1731C] + d0;
    s9_add(n3 + 0x438, (int32_t)W[0x1731C] >> 9);
    d0 = (int32_t)((uint32_t)(s9_rd(n3 + 0x5b0) - s9_rd(n3 + 0x430)) << 4);
    d0 -= (int32_t)W[0x17320] >> 1;
    d0 += d4 >> 1;
    W[0x17320] = (int32_t)W[0x17320] + d0;
    s9_add(n3 + 0x430, (int32_t)W[0x17320] >> 9);
  } else if (c < 0x1e0) {
    d0 = (c < 0x136) ? (int32_t)vrd32(0x38438) + 0x80 : (int32_t)vrd32(0x38438);
    d0 = (int32_t)((uint32_t)(d0 - s9_rd(n3 + 0x24)) << 3);
    d0 -= (int32_t)W[0x17318] >> 2;
    W[0x17318] = (int32_t)W[0x17318] + d0;
    s9_add(n3 + 0x24, (int32_t)W[0x17318] >> 9);
    if (c >= 0x131) {
      d1 = (int32_t)vrd32(0x38448) - s9_rd(n3 + 0x34);
      d0 = d1 * 25;                                      /* 2d1+d1, <<3, +d1 */
      d1 = m68k_divs((int32_t)W[0x1731C], -3);
      W[0x1731C] = (int32_t)W[0x1731C] + d0 + d1;
    }
    s9_add(n3 + 0x34, (int32_t)W[0x1731C] >> 9);
    d0 = (c < 0x136) ? (int32_t)vrd32(0x38440) - 0x20 : (int32_t)vrd32(0x38440);
    d0 = (int32_t)((uint32_t)(d0 - s9_rd(n3 + 0x2c)) << 4);
    d0 -= (int32_t)W[0x17320] >> 2;
    W[0x17320] = (int32_t)W[0x17320] + d0;
    s9_add(n3 + 0x2c, (int32_t)W[0x17320] >> 9);
    s9_wr(n3 + 0x430, m68k_divs(-3 * (int32_t)W[0x1731C], 0x400));
    s9_wr(n3 + 0x5b0, m68k_divs(3 * (int32_t)W[0x1731C], 0x400));
  }

  /* 0x02E890: the second figure's two model ids */
  if (c < 0x78) {
    s9_wr(n4 + 0x700, ((c & 0x1c) == 0x18) ? 0x3ff : 0x3f9);
    s9_wr(n4 + 0x680, vrd16s(0x38450 + ((c >> 3) & 7) * 2));   /* ext 0x0BB0: scale 2 */
  } else if (c < 0x82) {
    s9_wr(n4 + 0x700, 0x3fb);
    s9_wr(n4 + 0x680, 0x3fc);
  } else if (c < 0xa0) {
    s9_wr(n4 + 0x700, 0x3fb);
    s9_wr(n4 + 0x680, 0x3fa);
  } else {
    s9_wr(n4 + 0x700, ((c & 0x3c) == 0x18) ? 0x3ff : 0x3f9);
    s9_wr(n4 + 0x680, 0x3fe);
  }

  /* 0x02E932: the second figure's head, tracker and pendulum */
  if (c < 0x8c || c >= 0xe6) {
    d4 = (int32_t)((uint32_t)(-s9_rd(n4 + 0x334)) << 4);
    d4 -= (int32_t)W[0x172B4] >> 1;
    W[0x172B4] = (int32_t)W[0x172B4] + d4;
    s9_add(n4 + 0x334, (int32_t)W[0x172B4] >> 9);
    loc[0] = s9_rd(n3 + 0x24);
    loc[1] = s9_rd(n3 + 0x28) + 0x24e;
    loc[2] = s9_rd(n3 + 0x2c);
  } else {
    d4 = (int32_t)((uint32_t)(0x1800 - s9_rd(n4 + 0x334)) << 4);
    d4 -= (int32_t)W[0x172B4] >> 1;
    W[0x172B4] = (int32_t)W[0x172B4] + d4;
    s9_add(n4 + 0x334, (int32_t)W[0x172B4] >> 9);
    loc[0] = s9_rd(n5 + 0x24);                           /* ([,d5.l],$24) */
    loc[1] = s9_rd(n5 + 0x28) + 0x27b;
    loc[2] = s9_rd(n5 + 0x2c);
  }
  ending_character_ik_solve((ea_t)(n4 + 0x24), (ea_t)(n4 + 0x6b0), (ea_t)0xE17330, (ea_t)loc,
                            0x236, s9_rd(n4 + 0x34) + s9_rd(n4 + 0x334), 0);
  d4 += (int32_t)W[0x17308];
  d0 = vrd16s(0x20B006 + ((uint32_t)s9_rd(n4 + 0x6b0) & 0xfffc));   /* movea.w */
  d0 = (d0 * 0x79) >> 15;                                /* (a<<4 - a)<<3 + a */
  d0 = d0 * d4;
  d0 = m68k_divs(d0, 0x28be) >> 5;
  ending_character_physics_sim((int)(n4 + 0x800), d0);

  /* 0x02EA40: the third figure's model id */
  if (c < 0x1e)
    s9_wr(n7 + 0x780, 0x430);
  else if (c < 0x1c2 && c >= 0x186)
    s9_wr(n7 + 0x780, 0x432);
  else
    s9_wr(n7 + 0x780, (((c & 0x3c) == 0xc) && c > 0xb4) ? 0x432 : 0x42a);

  /* 0x02EA8E: its eye swing */
  if (c >= 0x1e && c <= 0xb4) {
    d0 = math_lerp_int(0, 0x20000, 0x1e, 0xb4, c);
    d4 = vrd16s(0x20B006 + ((uint32_t)d0 & 0xfffc));
    d4 = m68k_divs((int32_t)((uint32_t)d4 << 12), 0x8000);
    s9_wr(n7 + 0x780, (d4 < 0) ? 0x432 : 0x430);
    d4 += s9_rd(n7 + 0x30) - s9_rd(n7 + 0x3b0) - 0x1000;
  } else {
    d4 = s9_rd(n7 + 0x30) - s9_rd(n7 + 0x3b0);
  }
  if (c < 0x1c2 && c >= 0x168) {
    loc[0] = 0x3b000;
    loc[1] = 0x15000;
    loc[2] = 0xdf000;
  } else {
    loc[0] = s9_rd(n3 + 0x24);
    loc[1] = s9_rd(n3 + 0x28) + 0x28a;
    loc[2] = s9_rd(n3 + 0x2c);
  }
  ending_character_ik_solve((ea_t)(n7 + 0x24), (ea_t)(n7 + 0x730), (ea_t)0xE1733C, (ea_t)loc,
                            0x239, s9_rd(n7 + 0x34), d4);

  /* 0x02EB74: the fourth figure's tracker */
  if (c >= 0x87) {
    ea_t tgt;
    if (c < 0x14a) {
      loc[0] = s9_rd(n3 + 0x24);
      loc[1] = s9_rd(n3 + 0x28) + 0x24e;
      loc[2] = s9_rd(n3 + 0x2c);
      tgt = (ea_t)loc;
    } else {
      tgt = (ea_t)0xE00CDC;
    }
    ending_character_ik_solve((ea_t)(n5 + 0x24), (ea_t)(n5 + 0x830), (ea_t)0xE166F4, tgt,
                              0x27b, s9_rd(n5 + 0x34), 0);
  }

  /* 0x02EBDC: the fourth figure rises and waves */
  if (c < 0x1e) {
    s9_wr(n5 + 0x28, (int32_t)vrd32(0x38424) + 0x1000);
    loc[0] = 0;
    loc[1] = 0x33d;
  } else if (c < 0x96) {
    s9_wr(n5 + 0x28, math_lerp_int((int32_t)vrd32(0x38424) + 0x1000, (int32_t)vrd32(0x38424),
                                   0x1e, 0x96, c));
    W[0x172C0] = -0x4400;
    if (c >= 0x78) {
      loc[0] = math_lerp_int(-0x40, 0x20, 0x78, 0x96, c);
      d0 = math_lerp_int(-0x1800, 0, 0x78, 0x96, c);
    } else {
      loc[0] = -0x40;
      d0 = -0x1800;
    }
    s9_wr(n5 + 0x330, d0);
    s9_wr(n5 + 0x1b0, d0);
    loc[1] = 0x33d;
  } else {
    s9_add(n5 + 0x28, (int32_t)W[0x172C0] >> 9);
    d0 = (int32_t)((uint32_t)((int32_t)vrd32(0x38424) - s9_rd(n5 + 0x28)) << 4);
    d0 -= (int32_t)W[0x172C0] >> 2;
    W[0x172C0] = (int32_t)W[0x172C0] + d0;
    loc[0] = 0;
    loc[1] = s9_rd(n5 + 0x28) - (int32_t)vrd32(0x38424) + 0x33d;
  }
  loc[2] = 0;
  ending_character_pose_calc((int)(n5 + 0x80), (undefined4 *)loc, 0x5591a, 0x5241d, 0x1d8, 0x164);

  /* 0x02ED0A: its spin, a spring toward the nearer of +-0x4000 */
  W[0x172C4] = (int32_t)W[0x172C4] - 0xc0;
  if ((int32_t)W[0x172C4] < 0) W[0x172C4] = 0;
  d1 = -0x4000 + s9_rd(n5 + 0x434);                      /* lea $c000.w */
  d4 = (uint16_t)d1;                                     /* moveq #0 ; move.w d1,d4 */
  d0 = (d4 < 0x8000) ? 0x4000 : 0xc000;
  d0 -= d4;
  d1 = (int32_t)W[0x172B8] >> 4;
  d0 = (int32_t)((uint32_t)d0 << 4) - d1 + (int32_t)W[0x172C4];
  W[0x172B8] = (int32_t)W[0x172B8] + d0;
  d0 = ((int32_t)W[0x172B8] >> 9) + s9_rd(n5 + 0x434);
  s9_wr(n5 + 0x434, d0);
  s9_wr(n5 + 0x4b4, -d0);

  /* 0x02ED78: the camera pulls back */
  if (c >= 0x1e0) {
    W[0x172A0] = math_lerp_int((int32_t)vrd32(0x38404), 0x4000, 0x1e0, 0x2d0, c);
    W[0x17298] = math_lerp_int((int32_t)vrd32(0x383FC), (int32_t)vrd32(0x383FC) + 0x400,
                               0x1e0, 0x2d0, c);
  }
  camera_set_from_array((ea_t)0xE17294, 1);
  player_model_render_only();
  ending_sky_draw_alt();
  scene_objects_draw_list((ea_t)0x38414, (ea_t)0x37AB4);
  if (c > 0x2b0) {                                       /* fade to white */
    int16_t v = (int16_t)((int16_t)W[0xEB16] + 8);
    W[0xEB16] = v;
    if (v > 0xff) W[0xEB16] = 0xff;
  }
  if (W16(0x172EA) == 0x17ba && (int32_t)W[0x0CBC] == 3) {  /* 0x02EE0E */
    W16_SET(0x172E8, 0xb);
    W16_SET(0x172EA, 0x1f08);
  }
  { extern int g_stagedbg;   /* [END9] every 60 frames and at the exit */
    if (g_stagedbg && (c % 60 == 0 || W16(0x172E8) != 8))
      printf("[END9] c=%d ctr=%d phase=%d fade=%d n3=%06X n4=%06X n7=%06X n5=%06X cursor+%ld\n",
             c, (int)(uint16_t)W16(0x172EA), (int)W16(0x172E8), (int)(int16_t)W[0xEB16],
             (unsigned)n3, (unsigned)n4, (unsigned)n7, (unsigned)n5,
             (long)((int32_t *)W[0x0CA4] - (int32_t *)g_sys.dspram));
    if (g_stagedbg && (c == 44 || c == 344 || c == 644 || c == 714))
      printf("[END9S] c=%d pend=%d,%d,%d,%d,%d,%d spr=%d,%d,%d,%d,%d v=%d,%d,%d\n", c,
             (int)W[0x17348], (int)W[0x1734C], (int)W[0x17350], (int)W[0x17354], (int)W[0x17358],
             (int)W[0x1735C], (int)W[0x172B4], (int)W[0x172B8], (int)W[0x172BC], (int)W[0x172C0],
             (int)W[0x172C4], (int)W[0x17318], (int)W[0x1731C], (int)W[0x17320]); }
  return 0;
}

/* ---- ending_sequence_stage2 @ 0x02BE6E ---- */
/* ROM 0x02BE6E, ported from the machine code -- ENDING PHASE 1 (frames
 * 540..1559): three fixed cameras on the island (0x37E30, +0x1C, +0x38),
 * the castle/tower/bridge lights toggling (bits 2, 4, 8 of the 16-bit flags
 * 0xE04708, sounds 0x2B/0x2D) on the frame lists 0x37E84/0x37E98/0x37EB8,
 * then the island sinking (origin record copied to 0xE17294, its y falling
 * by W[0x172B0] >> 4 twice a frame from local frame 0x26C). The hero is
 * loaded at local frame 0x21C (0x37E28) and animated from there. This
 * function was absent from the live C altogether. */
void ending_sequence_stage2(void)
{
  int16_t d2 = 0;
  uint32_t lst = 0;
  int bit = 0, d4;
  W16_SET(0x172EC, W16(0x172EA) - 540);
  if (W16(0x172EC) == 0) {
    W16_SET(0x4708, W16(0x4708) & 0xfff1);
    for (d4 = 0; d4 < 7; d4++) W[0x17294 + d4 * 4] = vrd32s(0x37AA4 + d4 * 4);
    comms_w16(g_sys.commsram, 0x11a, 0xff);          /* move.w #$ff,$a0411a */
    W[0x172B0] = 0;
  }
  if (W16(0x172EC) == 0 || W16(0x172EC) == 0xb4 || W16(0x172EC) == 0x168) sound_play(0x2d);
  if (W16(0x172EC) < 0xb4) {
    camera_set_from_array(0x37E30, 1);
    d2 = (int16_t)(0xb4 - W16(0x172EC)); lst = 0x37E84; bit = 2;
  } else if (W16(0x172EC) < 0x168) {
    camera_set_from_array(0x37E30 + 0x1c, 0);
    d2 = (int16_t)(0x168 - W16(0x172EC)); lst = 0x37E98; bit = 4;
  } else {
    camera_set_from_array(0x37E30 + 0x38, 0);
    d2 = (int16_t)(0x21c - W16(0x172EC)); lst = 0x37EB8; bit = 8;
  }
  for (; vrd16s(lst) > 0; lst += 2) {
    if (d2 == vrd16s(lst)) {
      int16_t v = (int16_t)(W16(0x4708) ^ bit);
      W16_SET(0x4708, v);
      sound_play((v & bit) ? 0x2b : 0x2d);
    }
  }
  if (W16(0x172EC) >= 0x26c) {
    W[0x172B0] = (int32_t)W[0x172B0] + 5;
    if ((int32_t)W[0x172B0] > 0x4e0) W[0x172B0] = 0x4e0;
    W[0x17298] = (int32_t)W[0x17298] - (int32_t)((uint32_t)W[0x172B0] >> 4);
  } else if (W16(0x172EC) >= 0x258) {
    uint32_t a = (uint32_t)math_lerp_int(0, 0x18000, 0x258, 0x26c, W16(0x172EC));
    W[0x172B0] = (int32_t)(-(int32_t)e_sin(a)) / (int32_t)(0x80 + (int32_t)W[0x3EF0]);
  }
  W[0x17298] = (int32_t)W[0x17298] - (int32_t)((uint32_t)W[0x172B0] >> 4);
  ending_camera_shake_update();
  W[0x16670] = (int16_t)(-(int32_t)W[0x0CEC]);
  W[0x16668] = e_sin((uint16_t)W[0x16670]);
  W[0x1666C] = e_cos((uint16_t)W[0x16670]);
  end_draw_island(0xE17294, 1);
  W[0x12BC] = 1;
  if (W16(0x172EC) == 0x21c) {
    FUN_000268ce(0xE0B704, 0x37E28);
    ending_player_reset();
    ending_frame_render();
  }
  W16_SET(0x172EC, W16(0x172EC) - 540);
  if (W16(0x172EC) > 0) {
    scene_interpolation_evaluate(0xE0B704, W16(0x172EC));
    ending_frame_render();
  }
}

/* ---- ending entry 11 @ 0x02FCAA -- re-enter the final stage ------------
 * Reached only when the ending runs outside state 3 (stage9 / stage10 jump
 * straight to counter 0x1F08 in state 3, and the dispatcher's own increment
 * then makes it phase 12). On the first frame of the phase (counter 0x1B21,
 * the end of phase 10) it sets course 3, the story flag E10 (a WORD) and the
 * replay frame counter 0xE00C8C = -0x10F, reloads the player and the world
 * and starts the stage transition; afterwards it runs the transition, and
 * holds the counter at 0x1B20 while the transition reports sub 0.
 * PORTED FROM THE ROM (0x02FCAA..0x02FCF8). */
void ending_sequence_phase11(void)
{
  if (W16(0x172EA) == 0x1b21) {
    W[0x0E0C] = 3;
    W16_SET(0x0E10, 1);                       /* move.w #1,$e00e10 */
    W[0x0C8C] = (int32_t)0xfffffef1;
    gameplay_init_player_and_world();
    gameplay_init_state_vars();
    stage_transition_init();
  } else {
    stage_transition_run();
  }
  if (W[0x0CC0] == 0) W16_SET(0x172EA, 0x1b20);
}

/* ---- ending entry 12 @ 0x02FCFA -- the END card ---------------------------
 * In state 3: course 3; the fade colour (0xE0EB1A/1C/1E, the g_fog_* words)
 * WHITE (0xFF, `lea $ff.w`) when the final stage was cleared (E18 == 1) and
 * BLACK otherwise; the fade level to 0xFF (fully faded); gameplay sub 6 (the
 * final results page) next frame; the text layer back on; and the two
 * ending sounds 0x40 and 0x2D stopped. Outside state 3 it parks the ending
 * at phase/counter 0xFFFF. PORTED FROM THE ROM (0x02FCFA..0x02FD70). */
void ending_sequence_phase12(void)
{
  if ((int32_t)W[0x0CBC] == 3) {
    int16_t c = ((int32_t)W[0x0E18] == 1) ? 0xff : 0;
    W[0x0E0C] = 3;
    g_fog_b = c;                              /* 0xE0EB1E */
    g_fog_g = c;                              /* 0xE0EB1C */
    g_fog_r = c;                              /* 0xE0EB1A */
    W[0xEB16] = 0xff;
    W[0x0CC0] = 6;
    tilemap_enable_set();
    sound_stop(0x40);
    sound_stop(0x2d);
  } else {
    W16_SET(0x172EA, -1);
    W16_SET(0x172E8, -1);
  }
}


/* ---- ending_sequence_stage11 ---- */

int ending_sequence_stage11(void)

{
  undefined4 uVar1;
  int iVar2;
  int iVar3;
  int *piVar4;
  int *piVar5;
  int local_10;
  int local_c;
  int local_8;

  W16_SET(0x172EC, W16(0x172EA) + -0x1a8b);
  W[0x1729C] = 0;
  W[0x17298] = 0;
  W[0x17294] = 0;
  if (W[0x2B38] != 0) {
    W[0x172A4] = -1;
  }
  if ((W[0x2B3A] & 0x200) != 0) {
    W[0x0CDC] = R[0x384E0];
    W[0x0CE0] = R[0x384E4];
    W[0x0CE4] = R[0x384E8];
    W[0x0CE8] = R[0x384EC];
    W[0x0CEC] = R[0x384F0];
    W[0x0CF0] = R[0x384F4];
  }
  if ((W[0x2B38] & 4) != 0) {
    W[0x1729C] = 0x80;
  }
  if ((W[0x2B38] & 0x400) != 0) {
    W[0x1729C] = (int32_t)0xffffff80;
  }
  if ((W[0x2B38] & 0x10) != 0) {
    W[0x0CEC] = W[0x0CEC] + -0x100;
  }
  if ((W[0x2B38] & 0x20) != 0) {
    W[0x0CEC] = W[0x0CEC] + 0x100;
  }
  if ((W[0x2B38] & 0x80) != 0) {
    W[0x0CE8] = W[0x0CE8] + -0x100;
  }
  if ((W[0x2B38] & 0x40) != 0) {
    W[0x0CE8] = W[0x0CE8] + 0x100;
  }
  rotate_euler_zxy_optimized
            (0,0,W[0x1729C],W[0x0CE8],W[0x0CEC],W[0x0CF0],&local_10,&local_c,&local_8);
  W[0x0CDC] = local_10 + W[0x0CDC];
  W[0x0CE0] = local_c + W[0x0CE0];
  W[0x0CE4] = local_8 + W[0x0CE4];
  ending_camera_shake_update();
  W[0xB204] = (int)(vrd16s(0x383E4 + ((W16(0x172EC) >> 4) % 4) * 2));
  W[0xB284] = (int)(vrd16s(0x383EC + ((W16(0x172EC) >> 4) % 5) * 2));
  W[0xBA84] = (int)(vrd16s(0x383CE + ((W16(0x172EC) >> 4) % 7) * 2));
  W[0xBB04] = (int)(vrd16s(0x383DC + ((W16(0x172EC) >> 4) % 4) * 2));
  W[0xCF84] = (int)(vrd16s(0x383B0 + ((W16(0x172EC) >> 4) % 9) * 2));
  W[0xD004] = (int)(vrd16s(0x383C2 + ((W16(0x172EC) >> 4) % 6) * 2));
  if ((int)W16(0x172EC) % 0x96 == 0) {
    W[0x169F0] = 3;
    uVar1 = FUN_000268ce(&W[0xAB04], NULL);

    uVar1 = FUN_000268ce(uVar1, NULL);
    uVar1 = FUN_000268ce(uVar1, NULL);
    uVar1 = FUN_000268ce(uVar1, NULL);
    FUN_000268ce(uVar1, NULL);
    player_model_render_only();
  }
  else {
    player_model_update();
  }
  dsp_cmd_emit_object_mode_8000();
  *(int32_t*)W[0x0CA4] = 0x8010;
  ((int32_t*)W[0x0CA4])[1] = 3;
  ((int32_t*)W[0x0CA4])[2] = W[0x3EF4];
  piVar5 = (intptr_t)((int32_t*)W[0x0CA4] + 4);
  ((int32_t*)W[0x0CA4])[3] = 0xffffffff;
  if ((W[0x2C0C] & 0x10) != 0) {
    *piVar5 = 0x8008;
    ((int32_t*)W[0x0CA4])[5] = 3;
    ((int32_t*)W[0x0CA4])[6] = 1;
    ((int32_t*)W[0x0CA4])[7] = 0;
    ((int32_t*)W[0x0CA4])[8] = 0x7fff;
    ((int32_t*)W[0x0CA4])[9] = 0;
    ((int32_t*)W[0x0CA4])[10] = 0x7fff;
    ((int32_t*)W[0x0CA4])[0xb] = 0;
    ((int32_t*)W[0x0CA4])[0xc] = 0x7fff;
    ((int32_t*)W[0x0CA4])[0xd] = 0;
    ((int32_t*)W[0x0CA4])[0xe] = 0;
    ((int32_t*)W[0x0CA4])[0xf] = 0;
    ((int32_t*)W[0x0CA4])[0x10] = 0xffffff00;
    ((int32_t*)W[0x0CA4])[0x11] = 0;
    ((int32_t*)W[0x0CA4])[0x12] = 0xffffffff;
    ((int32_t*)W[0x0CA4])[0x13] = 0x8009;
    ((int32_t*)W[0x0CA4])[0x14] = 3;
    ((int32_t*)W[0x0CA4])[0x15] = W[0xB07C];
    ((int32_t*)W[0x0CA4])[0x16] = 3;
    ((int32_t*)W[0x0CA4])[0x17] = 0x800a;
    ((int32_t*)W[0x0CA4])[0x18] = ((int)W16(0x172EC) >> 4) % 0x11 + 0x433;
    ((int32_t*)W[0x0CA4])[0x19] = 3;
    ((int32_t*)W[0x0CA4])[0x1a] = W[0xEB08];
    ((int32_t*)W[0x0CA4])[0x1b] = W[0xEB0C];
    ((int32_t*)W[0x0CA4])[0x1c] = W[0xEB10];
    ((int32_t*)W[0x0CA4])[0x1d] = 0x8008;
    ((int32_t*)W[0x0CA4])[0x1e] = 3;
    ((int32_t*)W[0x0CA4])[0x1f] = 1;
    ((int32_t*)W[0x0CA4])[0x20] = 0x7fff;
    ((int32_t*)W[0x0CA4])[0x21] = 0;
    ((int32_t*)W[0x0CA4])[0x22] = 0;
    ((int32_t*)W[0x0CA4])[0x23] = 0x7fff;
    ((int32_t*)W[0x0CA4])[0x24] = 0;
    ((int32_t*)W[0x0CA4])[0x25] = 0x7fff;
    ((int32_t*)W[0x0CA4])[0x26] = 0;
    ((int32_t*)W[0x0CA4])[0x27] = 0;
    ((int32_t*)W[0x0CA4])[0x28] = 0;
    ((int32_t*)W[0x0CA4])[0x29] = 0xffffff00;
    ((int32_t*)W[0x0CA4])[0x2a] = 0;
    ((int32_t*)W[0x0CA4])[0x2b] = 0xffffffff;
    ((int32_t*)W[0x0CA4])[0x2c] = 0x8009;
    ((int32_t*)W[0x0CA4])[0x2d] = 3;
    ((int32_t*)W[0x0CA4])[0x2e] = W[0xB1FC];
    ((int32_t*)W[0x0CA4])[0x2f] = 3;
    ((int32_t*)W[0x0CA4])[0x30] = 0x800a;
    ((int32_t*)W[0x0CA4])[0x31] = 0x250;
    ((int32_t*)W[0x0CA4])[0x32] = 3;
    ((int32_t*)W[0x0CA4])[0x33] = W[0xEB08];
    ((int32_t*)W[0x0CA4])[0x34] = W[0xEB0C];
    piVar5 = (intptr_t)((int32_t*)W[0x0CA4] + 0x36);
    ((int32_t*)W[0x0CA4])[0x35] = W[0xEB10];
  }
  if (W[0x3EFC] == 0) {
    iVar3 = 0x444;
    do {
      piVar4 = piVar5;
      switch(iVar3) {
      case 0x448:
        *piVar4 = iVar3;
        piVar4[1] = 0xcb2 - W[0x0CDC];
        piVar4[2] = 0xa000 - W[0x0CE0];
        iVar2 = -0x1c2 - W[0x0CE4];
        break;
      default:
        *piVar4 = iVar3;
        piVar4[1] = -W[0x0CDC];
        piVar4[2] = 0xa000 - W[0x0CE0];
        iVar2 = -W[0x0CE4];
        break;
      case 0x451:
      case 0x452:
        *piVar4 = iVar3;
        piVar4[1] = -0x820 - W[0x0CDC];
        piVar4[2] = 0xa000 - W[0x0CE0];
        iVar2 = 0xa5 - W[0x0CE4];
        break;
      case 0x453:
      case 0x454:
        *piVar4 = iVar3;
        piVar4[1] = -0x820 - W[0x0CDC];
        piVar4[2] = 0xa000 - W[0x0CE0];
        iVar2 = -0x57d - W[0x0CE4];
        break;
      case 0x455:
      case 0x456:
        *piVar4 = iVar3;
        piVar4[1] = -0x820 - W[0x0CDC];
        piVar4[2] = 0xa000 - W[0x0CE0];
        iVar2 = -0x28a - W[0x0CE4];
      }
      piVar4[3] = iVar2;
      iVar3 = iVar3 + 1;
      piVar5 = piVar4 + 4;
    } while (iVar3 < 0x460);
    piVar4[4] = 0x29;
    piVar4[5] = 0;
    piVar4[6] = -0x20000;
    piVar4[7] = 0;
    piVar4[8] = 0x27;
    piVar4[9] = 0;
    piVar4[10] = -0x20000;
    piVar5 = piVar4 + 0xc;
    piVar4[0xb] = 0;
  }
  *piVar5 = 0x8010;
  W[0x0CA4] = piVar5 + 2;
  piVar5[1] = -1;
  W[0x4704] = 0;
  W[0x17294] = W[0x3EF0] + -0x58900;
  W[0x17298] = W[0x3EF4] + -0xaae0;
  W[0x1729C] = W[0x3EF8] + -0x8bb00;
  W[0x172A0] = 0x495;
  scene_objects_draw_list((int32_t*)&g_sys.rom[0x384FC], &W[0x17294]);
  ending_sky_draw_alt(); iVar3 = 0;
  if ((W16(0x172EA) == 0x1b20) && (iVar3 = W[0x172A4] + 1, iVar3 == 0)) {
    W[0x172A4] = 0;
    W16_SET(0x172EA, 0x1a8a);
  }
  return iVar3;
}

/* ---- ending_sky_draw ---- */

void ending_sky_draw(void)

{
  undefined4 *puVar1;

  dsp_cmd_emit_object_mode_8000();
  if (W[0x4704] != 0) {
    W[0x4704] = 0;
    puVar1 = (intptr_t)((int32_t*)W[0x0CA4] + 1);
    *(int32_t*)W[0x0CA4] = 0x8010;
    W[0x0CA4] = W[0x0CA4] + 2 * 4;
    *puVar1 = 0xffffffff;
  }
  *(int32_t*)W[0x0CA4] = 0x29;
  ((int32_t*)W[0x0CA4])[1] = 0;
  ((int32_t*)W[0x0CA4])[2] = 0xfff9f000;
  ((int32_t*)W[0x0CA4])[3] = 0;
  ((int32_t*)W[0x0CA4])[4] = 0x27;
  ((int32_t*)W[0x0CA4])[5] = 0;
  ((int32_t*)W[0x0CA4])[6] = 0xfff9f000;
  ((int32_t*)W[0x0CA4])[7] = 0;
  ((int32_t*)W[0x0CA4])[8] = 0x2b;
  ((int32_t*)W[0x0CA4])[9] = 0;
  ((int32_t*)W[0x0CA4])[10] = 0xfffb2000;
  ((int32_t*)W[0x0CA4])[0xb] = 0;
  W[0x0CA4] = W[0x0CA4] + 0xc * 4;
  return;
}


/* ---- bonus_model_init_full @ 0x017026 ---- */



void bonus_model_init_full(void)

{
  int iVar1;
  
  /* ROM 0x017048..0x017058: the same list as bonus_model_init_simple --
   * `move.l $123c4c.l,-(a7)` -- loaded at 0xE0B784. The transpile passed
   * NULL, so the chain came out empty and a fake 0x250 had to be planted
   * at node 1 to stop the walk (transpilation fix #16); that is what kept
   * the start-line marshal off the screen. */
  W[0x166E4] = 0xB784;
  anim_load_list(0xB784, vrd32(0x123C4C));
  W16_SET(0xEB06, 1);                     /* 0x01705E move.w */
  /* THE ORBIT OFFSET WAS WRITTEN 27 BYTES SHORT OF WHERE IT IS READ BACK.
   * ROM 0x017068/6E/74 push `$2c(a0)`, `$28(a0)`, `$24(a0)` as this call's
   * three output pointers (a0 = (a2) = W[0x166E4] = 0xE0B784), i.e. node 0's
   * x/y/z DOF cells. The transpile passed `W[0x166E4] + 9 / + 10 / + 0xb` --
   * HOST BYTE offsets 9, 10 and 11, which are not only wrong but mutually
   * OVERLAPPING (three 4-byte writes at +9, +10, +11). Everything else in
   * this function addresses the same cells as `BNODE((9) * 4)`,
   * `[10]`, `[0xb]` = host bytes 0x24/0x28/0x2C, which is what the ROM
   * displacements say and what `bonus_camera_update` reads. So the readback
   * on the very next line read a cell the rotation had never written.
   *
   * `rotate_euler_zxy_optimized` has NO PROTOTYPE anywhere in the tree, so
   * the intptr_t -> int* mismatch was silent. Register row 137's class.
   *
   * Measured over the 330-frame intro orbit (sub 5), player stationary:
   * |cam-player| swept 1705 -> 26346 -> 9399 where an orbit radius should be
   * roughly constant -- the camera started inside the deck (a screen full of
   * cliff face) and ballooned to ~3x. Only the ENDPOINT was right, which is
   * why this survived: the register recorded "the orbit finishes at heading
   * 52795 with |cam-player| = 9404" and looked no further. */
  { int32_t _ox, _oy, _oz;
    rotate_euler_zxy_optimized
              (0x1c00,0,(int32_t)0xfffff200,W[0x0D0C],W[0x0D10] + 0x4000,W[0x0D14],
               &_ox,&_oy,&_oz);
    BNODE(0x24) = _ox;    /* ROM $24(a0) */
    BNODE(0x28) = _oy;    /* ROM $28(a0) */
    BNODE(0x2c) = _oz; }  /* ROM $2c(a0) */
  W[0x166CC] = BNODE((9) * 4) + W[0x0D00];
  BNODE((9) * 4) = W[0x166CC];
  BNODE((0x15) * 4) = W[0x166CC];
  W[0x166D0] = BNODE((10) * 4) + W[0x0D04] + 0x3dc;
  BNODE((10) * 4) = W[0x166D0];
  BNODE((0x16) * 4) = W[0x166D0];
  W[0x166D4] = BNODE((0xb) * 4) + W[0x0D08];
  BNODE((0xb) * 4) = W[0x166D4];
  BNODE((0x17) * 4) = W[0x166D4];
  W[0x166D8] = -W[0x0D0C];
  BNODE((0xc) * 4) = W[0x166D8];
  if (W16(0xE6A) < 4) {
    iVar1 = (int)W16(0xE6A) << 0xc;
  }
  else {
    iVar1 = 0x3000;
  }
  W[0x166DC] = (iVar1 - W[0x0D10]) + 0x4000;
  BNODE((0xd) * 4) = W[0x166DC];
  BNODE((0x19) * 4) = W[0x166DC];
  W[0x166E0] = -W[0x0D14];
  BNODE((0xe) * 4) = W[0x166E0];
  BNODE((0xec) * 4) = 0x800;
  BNODE((0x22a) * 4) = (BNODE((0x22a) * 4) * 100) / 0x41;
  BNODE((0x22b) * 4) = (BNODE((0x22b) * 4) * 100) / 0x41;
  W[0x166E8] = 0;
  W[0x166EC] = 0;
  W[0x166F0] = 0;
  W[0x166F4] = 0;
  W[0x166F8] = 0;
  W[0x166FC] = 0;
  W[0x16700] = 0;
  W[0x16704] = 0;
  W[0x16708] = 0;
  W[0x1670C] = 0;
  W[0x16710] = 0;
  W[0x16714] = 0;
  W[0x16718] = 0;
  W[0x1671C] = 0;
  W[0x17294] = BNODE((10) * 4);
  W[0x17298] = 0xf52;
  W[0x1729C] = 0;
  W[0x172A0] = 0;
  W[0x172A4] = 0;
  W[0x172A8] = 0;
  W[0x172AC] = 0;
  W[0x172B4] = BNODE((0x2c) * 4);
  W[0x172B8] = BNODE((0x4c) * 4);
  W[0x172BC] = BNODE((0x4d) * 4);
  W[0x172C0] = BNODE((0x6c) * 4);
  W[0x172C4] = BNODE((0x8c) * 4);
  W[0x172C8] = BNODE((0xac) * 4);
  W[0x172CC] = BNODE((0xad) * 4);
  W[0x172D0] = BNODE((0xcc) * 4);
  BNODE((0xc) * 4) = W[0x172A0];
  /* 0x01722E..0x017260: node 19 = a copy of node 17 (32 longs, +0x980 <-
   * +0x880), its X angle +0x400, Z +0x32, +0x5C = +0x28, and its parent
   * link -- the HIGH word of +0x04, `subq.w #2,$984(a0)` -- two further back */
  { int _k; for (_k = 0; _k < 0x20; _k++)
    BNODE(0x980 + _k * 4) = BNODE(0x880 + _k * 4); }
  BNODE((0x26c) * 4) = BNODE((0x26c) * 4) + 0x400;
  BNODE((0x26b) * 4) = BNODE((0x26b) * 4) + 0x32;
  BNODE((0x277) * 4) = BNODE((0x26a) * 4);
  W_SET_HI16(BNB + 0x984, W_HI16(BNB + 0x984) - 2);
  if (W16(0xE6A) == 0) {
    BNODE((0xec) * 4) = 0x400;
    BNODE((0x14c) * 4) = BNODE((0x14c) * 4) - BNODE((0xec) * 4);
    BNODE((0x1ac) * 4) = BNODE((0x1ac) * 4) - BNODE((0xec) * 4);
    BNODE((0x22c) * 4) = BNODE((0xec) * 4) + BNODE((0x22c) * 4);
    BNODE((0x22b) * 4) = BNODE((0x22b) * 4) + 0x32;
  }
  else {
    BNODE((0xec) * 4) = 0x400;
    BNODE((0x14c) * 4) = BNODE((0x14c) * 4) + BNODE((0xec) * 4) * -4;
    BNODE((0x1ac) * 4) = BNODE((0x1ac) * 4) + BNODE((0xec) * 4) * -4;
    BNODE((0x14e) * 4) = BNODE((0x14e) * 4) - BNODE((0xec) * 4);
    BNODE((0x1ae) * 4) = BNODE((0xec) * 4) + BNODE((0x1ae) * 4);
    BNODE((0x1ec) * 4) = BNODE((0xec) * 4) * 2 + BNODE((0x1ec) * 4);
    BNODE((0x22a) * 4) = BNODE((0x22a) * 4) + 0x1c0;
    BNODE((0x22b) * 4) = BNODE((0x22b) * 4) + 0x72;
    BNODE((0x22c) * 4) = BNODE((0xec) * 4) + 0x8000 + BNODE((0x22c) * 4);
  }
  W16_SET(0x16728, 300);                  /* 0x017312 move.w */
  return;
}





/* ---- ranking_sound_init @ 0x028E5E ---- */

void ranking_sound_init(void)

{
  int iVar1;
  undefined2 uVar2;
  
  cgram_load_tile_block(0x19, 0x360);
  cgram_load_tile_block(0x1a,0x330);
  cgram_load_tile_block(0x1b,0x2f0);
  iVar1 = 0;
  do {
    cgram_load_tile_block((short)iVar1 + 0x20, iVar1 * 4 + 0x390);
    iVar1 = iVar1 + 1;
  } while (iVar1 < 9);
  /* ROM 0x028EA6..0x028EBC: `move.b #$3` / `move.b #$5` palette bytes, which the
     decompiler lost -- one call had no palette argument at all (register row 118). */
  (void)uVar2;
  cz_load_color_ramp(0x19, 3);
  cz_load_color_ramp(0xf7, 5);
  return;
}





/* ---- results_graphics_load @ 0x025E4A ---- */

/* ROM 0x025E4A. Every cz_load_color_ramp call pushes its palette index as a
 * BYTE (`move.b #$1,-(a7)` 0x025E6A, `#$5` 0x025EC8, `#$7` 0x025EEA /
 * 0x025F16) -- register row 118's class; the C passed 0, which loaded all
 * three ramps onto palette 0. */
void results_graphics_load(void)
{
  int i;
  sync_post();
  cgram_load_tile_block(0xb7, 0x120);
  cz_load_color_ramp(0xb7, 1);
  cgram_load_tile_block(0xd4, 0x1a0);
  cgram_load_tile_block(0xe3, 0x1c0);
  for (i = 0; i < 10; i++) {
    cgram_load_tile_block(i + 0xe6, i * 4 + 0x160);
    cgram_load_tile_block(i + 0xfb, i * 6 + 0x1e0);
  }
  cgram_load_tile_block(0x105, 0x21c);
  cz_load_color_ramp(0xfb, 5);
  if (W16(0xE10) == 0) {
    cgram_load_tile_block(0x106, 0x240);
    cgram_load_tile_block(0x107, 0x280);
    cgram_load_tile_block(0x108, 0x2c0);
    cz_load_color_ramp(0x106, 7);
  }
  else {
    cgram_load_tile_block(0x12a, 0x240);
    cz_load_color_ramp(0x12a, 7);
  }
  return;
}





/* ==========================================================================
 * THE RESULTS PAGE (gameplay sub 6/7) -- both variants, PORTED FROM THE ROM.
 *
 *   results_screen_init_normal    @ 0x023B1A   (NOVICE)
 *   results_screen_update_normal  @ 0x023C90
 *   results_screen_init_boss      @ 0x02462E   (ADVANCED / story)
 *   results_screen_update_boss    @ 0x02475C
 *
 * Rewritten instruction by instruction from the disassembly rather than
 * patched Ghidra C; what was wrong in the old C, all of it measured:
 *
 *  - 16-BIT FIELDS. The ROM keeps the stamp angles 0xE169A4/A6/A8, the stamp
 *    sound latch 0xE169AA, the rank pointer 0xE169BA and its speed/flag
 *    0xE169BC/BE, the saved scroll words 0xE169E2/E4 and the sound-stage
 *    counter 0xE15F5C as .w fields, several of them in the same 32-bit slot.
 *    Whole-slot, the stamp's Y angle 0xE169A6 (a 2-mod-4 offset: the one that
 *    turns the stamp round to show FAILED) was rebuilt from its neighbours'
 *    bytes every frame, and 0xE169A4 = 0xC000 read as +49152 instead of
 *    -16384, so the stamp never started its slam (`tst.w $e169a4 ; bpl`).
 *    0xE16978 (the phase) is .w too but stays a whole slot: it is 4-aligned
 *    with no live +2 neighbour and gameplay_sub10/11 share it whole-slot.
 *  - THE STAMP'S POSITION was held in `undefined2` locals: 0xFC30 (y = -976)
 *    and 0xFD80 (-640) arrived zero-extended as 64560/64896. ROM 0x025198..
 *    0x0251BA stores full longs 0xFFFFFC30 / 0xFFFFFD80.
 *  - `pea ([, d7.l], $1b0)` (0x024ED8 / 0x0242A0) is a 68020 memory-indirect
 *    EA: the VALUE W[0x16998] + 0x1B0 (register row 109's class). The C
 *    passed the literal address 0xE16B48 to FUN_0000e528.
 *  - dsp_cmd_set_camera(2, 0x359, &g_sys.rom[0x1940], ...) and
 *    sprite_draw_2d(..., &g_sys.rom[0x5000], 0): host pointers where the ROM
 *    pushes the immediates 0x1940 / 0x5000 (`pea $1940.w`, `pea $5000.w`).
 *  - Lost arguments: FUN_0000f584(0x43) (0x024018 / 0x024B76 / 0x025284),
 *    FUN_0000f66a(0x32, level) (0x023FA6), sound_play_p's (0x26, 0x72..74).
 *  - The case-9 high-score read `$e04088(d0.l*8)` (0x024056, ext 0x0FB0,
 *    scale 8) is W[0x4088 + course*8], not (&W[0x4088])[course*2].
 *  - The text-layer scroll words at 0x8A0000/0x8A0002 are 16-bit BIG-ENDIAN
 *    registers; `g_sys.tilemapattr[0] = 0xff58` stored one byte of them.
 *
 * Measured against MAME (tools/overnight/snap_results_story.lua), the CPU
 * command list this now emits matches the machine's own list entry for
 * entry on both variants -- see the register row for the numbers.
 * ======================================================================== */

/* the ROM's input edge words, whole-slot as their writer
 * (input_read_service_buttons) stores them */
#define RS_START_EDGE()  ((W[0x2B3A] & 0x100) != 0)   /* move.w $e02b3a ; andi #$100 */
#define RS_A_EDGE()      ((W[0x2BA6] & 1) != 0)       /* move.w $e02ba6 ; andi #1 */
#define RS_PH()          ((int)(int16_t)W[0x16978])  /* .w, kept whole-slot, see above */
#define RS_PH_SET(v)     (W[0x16978] = (int16_t)(v))

/* PROPCYCL_RESDUMP=<dir>:<t1,t2,...> -- write the whole DSP RAM (both CPU
 * command buffers) at the entry of the results update on the frames whose
 * W[0x1696C] (frames since sub 6) equals one of the t values, as
 * <dir>/ours_t<t>.bin, for a word-for-word diff against the polygon RAM
 * that snap_results_story.lua dumps out of MAME (tools/overnight/
 * results_cpulist_diff.py). */
static void results_dump_probe(void)
{
  int i; const char *p;
  static int init = 0, nt = 0; static int ts[32]; static char dir[256];
  if (!init) {
    const char *e = getenv("PROPCYCL_RESDUMP"); init = 1;
    if (e) {
      const char *c = strchr(e, ':');
      if (c && (size_t)(c - e) < sizeof dir) {
        memcpy(dir, e, (size_t)(c - e)); dir[c - e] = 0;
        for (p = c + 1; *p && nt < 32; ) {
          ts[nt++] = atoi(p);
          while (*p && *p != ',') p++;
          if (*p == ',') p++;
        }
      }
    }
  }
  for (i = 0; i < nt; i++) {
    if ((int)W[0x1696C] == ts[i]) {
      char path[320]; snprintf(path, sizeof path, "%s/ours_t%d.bin", dir, ts[i]);
      FILE *f = fopen(path, "wb");
      if (f) { fwrite(g_sys.dspram, 1, DSPRAM_SIZE, f); fclose(f); }
      printf("[RESDUMP] t=%d buf=%ld cursor=%lx -> %s\n", ts[i], (long)W[0x0CA0],
             (unsigned long)(uintptr_t)W[0x0CA4], path);
    }
  }
}

/* The body both init functions share: ROM 0x023B3A..0x023C8A (normal) and
 * 0x02464E..0x02475A (boss) write the same fields in a different order. */
static void results_init_common(void)
{
  int i;
  sync_post();                          /* jsr $2106a */
  camera_state_reset();                 /* jsr $21d84 */
  g_fog_mode = 3;                       /* move.w #$3,$e0eb18 */
  W16_SET(0x15F5C, 0);                  /* clr.w $e15f5c */
  W16_SET(0x169A4, (int16_t)0xc000);    /* move.w #$c000,$e169a4 */
  W16_SET(0x169A6, 0);                  /* clr.w  $e169a6 */
  W16_SET(0x169A8, 0);                  /* clr.w  $e169a8 */
  W[0x16998] = 0x100;
  W[0x1699C] = 0x100;
  W[0x169A0] = 0x4000;
  RS_PH_SET(0);                         /* clr.w $e16978 */
  W[0x1697C] = 0x1f;
  W[0x0E50] = (W[0x0E4C] / 10) << 3;    /* divs.l #10 ; lsl.l #3 */
  W[0x16984] = 0;
  W[0x16970] = 0;
  W[0x1696C] = 0;
  W[0x169CC] = (W[0x0E44] / 6) * 5;     /* divs.l #6 ; *5 */
  W[0x0E44] = (W[0x0E44] / 0x3c) * 0x3c;
  W[0x0E48] = W[0x0E44];                /* move.l (a2),$e00e48 */
  W[0x16994] = 0;
  W16_SET(0x169AA, 0);                  /* clr.w $e169aa */
  W[0x169AC] = 0;
  W[0x169B0] = 0;
  W[0x169B4] = 0x4af;
  W16_SET(0x169B8, 1);   /* ROM 0x02471E `move.w` */
  W[0x0CC0] = 7;
  /* THE DIGIT ARRAY IS 16-BIT, STRIDE 2, AND 10 IS THE BLANK FACE (register
   * row 105): clr.w $e169d0 then move.w #$a,$e169d0(d2.l*2) for d2 = 1..4. */
  W_A16_SET(0x169D0, 0, 0);
  for (i = 1; i < 5; i++) W_A16_SET(0x169D0, i, 10);
  W16_SET(0x169DA, (int16_t)0xffff);   /* move.w #$ffff,$e169da */
  W16_SET(0x169DC, 0);                  /* clr.w $e169dc */
}

/* ---- results_screen_init_boss @ 0x02462E ---- */

int results_screen_init_boss(void)
{
  set_background_color(0x40, 0x80, 0x40);   /* 0x024638 */
  results_init_common();
  return 0;
}

/* ---- results_screen_init_normal @ 0x023B1A ---- */

int results_screen_init_normal(void)
{
  set_background_color(0x40, 0x80, 0x40);   /* 0x023B24 */
  results_init_common();
  /* the NOVICE page's rank reveal state, 0x023C1A..0x023C42 */
  W16_SET(0x169BA, vrd16s(0x37508));    /* move.w $37508.l,$e169ba */
  W[0x169C0] = -1;                      /* moveq #$ff ; move.l */
  W[0x169C4] = 0;
  W16_SET(0x169BC, 0x10);
  W16_SET(0x169BE, 1);
  W[0x169C8] = 0;
  W16_SET(0x169DE, 0);                  /* 0x023C70 clr.w $e169de */
  /* 0x023C76 move.w $8a0000.l,$e169e2 / 0x023C80 move.w $8a0002.l,$e169e4 */
  W16_SET(0x169E2, (int16_t)mem_read16(0x8A0000));
  W16_SET(0x169E4, (int16_t)mem_read16(0x8A0002));
  return 0;
}

/* The draw helpers both update functions share. */

/* the POINT gauge that swings in from the top: ROM 0x024226..0x02432A
 * (normal, radius 0xB0) / 0x024E60..0x024F54 (boss, radius 0xC0) */
static void results_draw_point_gauge(int radius)
{
  int32_t v = W[0x16998], a, s, c, idx;
  if (W[0x16984] == 1) { if (v > 0) W[0x16998] = v - 8; }
  else if (v < 0x100) W[0x16998] = v + 8;
  v = W[0x16998] * 0x13;                 /* d1*8 + d1 ; *2 ; + d1 = 19*d1 */
  if (v < 0) v += 0xf;
  v >>= 4;
  FUN_0000e37e(1, W[0x0E50], v + 0x13a, v + 0xf3, 0x220);
  /* 0x0242A0 `pea ([, d4.l], $1b0)`: memory-indirect -- the VALUE
   * W[0x16998] + 0x1B0, not the address 0xE16B48. */
  FUN_0000e528(5, W[0x0E50], W[0x16998] + 0x1b0, -W[0x16998], 0x220);
  a = W[0x16998] << 7;
  idx = (a >> 1) & 0x7ffe;
  s = vrd16s(0x20B004 + idx * 2);
  c = vrd16s(0x20B006 + idx * 2);
  dsp_cmd_place_object_rotated_abs(1, 0x346, 0x410,
      0x400 - ((c * radius) >> 15), ((s * radius) >> 15) + 0x6b0, -a, 0, 0, 2);
}

/* the TIME gauge from the other side: ROM 0x02432E..0x024426 (normal) /
 * 0x024FBE..0x0250C4 (boss) */
static void results_draw_time_gauge(int radius)
{
  int32_t v = W[0x1699C], a, s, c, idx, t;
  if (W[0x16984] == 3) { if (v > 0) W[0x1699C] = v - 8; }
  else if (v < 0x100) W[0x1699C] = v + 8;
  v = W[0x1699C];
  if (W[0x0E18] != 2) {
    t = v * 0x1c;                        /* d1*8 - d1 ; *4 = 28*d1 */
    int32_t y = t; if (y < 0) y += 0xf; y >>= 4;
    FUN_0000e0e8(W[0x0E48], t / -0x10 - 0x188, y + 0x130, 0x2d0, -0x10 - v, -v);
  }
  a = v << 7;
  idx = (a >> 1) & 0x7ffe;
  s = vrd16s(0x20B004 + idx * 2);
  c = vrd16s(0x20B006 + idx * 2);
  dsp_cmd_place_object_rotated_abs(1, 0x346, -400,
      0x400 - ((c * radius) >> 15), ((s * radius) >> 15) + 0x6b0, -a, 0x8000, 0, 2);
}

/* the rank pointer (0x344 / 0x345) under viewport 2: y and z are built from
 * the SAME sine term in the ROM too (0x02448A / 0x0244AE both read
 * $20b004), so that is not a transcription artifact. */
static void results_draw_rank_pointer(int32_t limit, int shift, int32_t y0, int32_t z0)
{
  int32_t v = W[0x169A0], s, off;
  if (W[0x16984] == 6) { if (v > 0) W[0x169A0] = v - 0x200; }
  else if (v < limit) W[0x169A0] = v + 0x200;
  v = W[0x169A0];
  s = vrd16s(0x20B004 + ((v >> 1) & 0x7ffe) * 2);
  off = (s << shift) >> 15;
  dsp_cmd_place_object_rotated_abs(2, (W[0x0E18] == 1) ? 0x344 : 0x345, 0x70,
                                   off + y0, off + z0, -v, 0, 0, 0);
}

/* the minimap summary, both variants: ROM 0x02456E..0x0245CA (normal) /
 * 0x0251FA..0x025256 (boss) */
static void results_draw_minimap(void)
{
  if (RS_PH() == 0) return;              /* tst.w (a3) ; beq */
  int32_t n = (W[0x17034] * W[0x1696C]) / 0xf0;
  minimap_draw_targets(n);
  if (W[0x1696C] >= 0xf0) n = W[0x17034];
  minimap_draw_trail(n);
}

/* the sound-stage tail both update functions end with: 0x0245DE / 0x02525E */
static void results_sound_tail(void)
{
  W16_SET(0x15F5C, W16(0x15F5C) + 1);    /* addq.w #1,(a0) */
  if (W16(0x15F5C) < 2) FUN_0000fcc6();
  if (W16(0x15F5C) == 2) { FUN_0000fd30(); FUN_0000f584(0x43); }
  if (W16(0x15F5C) == 0xc) sound_play_or_defer(0x43);
}

/* ---- results_screen_update_boss @ 0x02475C ---- */

int results_screen_update_boss(void)
{
  int32_t t3; int i;
  results_dump_probe();
  if ((int)W16(0xE6A) + (int)W16(0xE66) != 0)       /* movea.w ; adda.w ; tst.l */
    tilemap_wind_flash_update(5);
  t3 = W[0x1696C] - 3;                               /* -$10(a6) */
  W[0x16980] = 0;
  /* 0x0247C2: movea.w (a3),a0 ; cmpa.l #9 ; bhi default ; the table at
   * 0x0247D8 (index a0.w*2, ext 0x8208): 0x247EC 0x24840 0x248C8 0x249AE
   * default 0x24A26 0x24AC4 0x24AF4 0x24BC6 default. */
  switch ((uint32_t)RS_PH() > 9 ? -1 : RS_PH()) {
  case 0:                                            /* 0x0247EC */
    W[0xEB16] = (int16_t)(W[0x1697C] << 3);
    W[0x1697C] -= 1;
    if (W[0x1697C] < 0) {
      RS_PH_SET(2); W[0x1697C] = 0x40; W[0x16984] = 1; W[0x16988] = 0x708;
      if (W[0x0E18] == 2 && W[0x0E0C] != 3)
        sound_play_p((int32_t)((0x26u << 16) | 0x72u));  /* f698(0x26, 0x72) */
    }
    break;
  case 1:                                            /* 0x024840 */
    if ((W[0x169B0] >> 3) == W[0x169AC]) W[0x1697C] = 0;
    if (RS_START_EDGE()) { W16_SET(0x169A6, score_check_quota()); W[0x1697C] = 0; }
    if (RS_A_EDGE())     { W16_SET(0x169A6, score_check_quota()); W[0x1697C] = 0; }
    W[0x1697C] -= 1;
    if (W[0x1697C] >= 0) break;
    W16_SET(0x169A6, score_check_quota());
    W[0x169B0] = W[0x169AC] << 3;
    if (W[0x0E18] == 2) goto stamp_wait;             /* beq.w $24adc */
    RS_PH_SET(2); W[0x1697C] = 0x3c; W[0x16984] = 3; W[0x16988] = W[0x0E48] / 3;
    break;
  case 2:                                            /* 0x0248C8 */
    if (W[0x16984] == 7) {
      if ((int32_t)score_check_quota() == 0x8000) {  /* FAILED: tilt the stamp */
        if (W16(0x169A8) == 0x200) sound_play_p((int32_t)((0x26u << 16) | 0x74u));
        if (W16(0x169A8) == 0) sound_play(0x14);
        if (W16(0x169A8) < 0x400) W16_SET(0x169A8, W16(0x169A8) + 0x40);
      }
      else if (W[0x0E0C] < 3 && W[0x1697C] == 300)
        sound_play_p((int32_t)((0x26u << 16) | 0x73u));
    }
    if (RS_START_EDGE()) W[0x1697C] = 0;
    if (RS_A_EDGE())     W[0x1697C] = 0;
    W[0x1697C] -= 1;
    if (W[0x1697C] < 0) { RS_PH_SET(W[0x16984]); W[0x1697C] = W[0x16988]; }
    /* 0x02496A: the adds and the quota test run EVERY frame here (the bpl
     * lands on them), unlike the NOVICE page where they sit inside the
     * branch -- ROM 0x023E4C `bpl.w $24112`. */
    if (RS_PH() == 1) W[0x169AC] += W[0x0E4C];
    if (RS_PH() == 3) W[0x169AC] += W[0x169CC];
    if (RS_PH() == 6 && W[0x0E18] == 1) W[0x169AC] += 1000;
    W16_SET(0x169A6, score_check_quota());
    break;
  case 3:                                            /* 0x0249AE */
    if ((W[0x169B0] >> 3) == W[0x169AC]) W[0x1697C] = 0;
    if (RS_START_EDGE()) { W16_SET(0x169A6, score_check_quota()); W[0x1697C] = 0; }
    if (RS_A_EDGE())     { W16_SET(0x169A6, score_check_quota()); W[0x1697C] = 0; }
    W[0x1697C] -= 1;
    if (W[0x1697C] >= 0) break;
    W[0x169B0] = W[0x169AC] << 3;
    W16_SET(0x169A6, score_check_quota());
    W[0x0E48] = 0;
    RS_PH_SET(2); W[0x1697C] = 0x3c; W[0x16984] = 6; W[0x16988] = 0x1e;
    break;
  case 5:                                            /* 0x024A26 -- THE STAMP */
    if (W16(0x169DC) == 0) break;                    /* the map trail finished */
    if (W16(0x169A4) < 0) {                          /* slam down, spinning */
      W16_SET(0x169A4, W16(0x169A4) + 0x200);
      W16_SET(0x169A6, W16(0x169A6) + 0x2000);
    }
    W[0x16994] += 1;
    if (RS_START_EDGE()) { W16_SET(0x169A6, score_check_quota()); W16_SET(0x169A4, 0); W[0x1697C] = 0; }
    if (RS_A_EDGE())     { W16_SET(0x169A6, score_check_quota()); W16_SET(0x169A4, 0); W[0x1697C] = 0; }
    if (W16(0x169A4) == 0 && W16(0x169AA) == 0) { sound_play(0x05); W16_SET(0x169AA, 1); }
    W[0x1697C] -= 1;
    if (W[0x1697C] < 0) { RS_PH_SET(2); W[0x1697C] = 300; W[0x16984] = 7; }
    break;
  case 6:                                            /* 0x024AC4 */
    if (RS_A_EDGE()) W[0x1697C] = 0;
    W[0x1697C] -= 1;
    if (W[0x1697C] >= 0) break;
  stamp_wait:                                        /* 0x024ADC */
    RS_PH_SET(2); W[0x1697C] = 0x1e; W[0x16984] = 5; W[0x16988] = 0x5a;
    break;
  case 7:                                            /* 0x024AF4 */
    tilemap_enable_set();                            /* jsr $225f8 */
    sync_post();
    mem_write16(0x8A0000, 0x35c);
    mem_write16(0x8A0002, 0);
    game_stats_record_end();                         /* jsr $20df2 */
    if (W[0x169AC] >= W[0x0E60]) {                   /* COMPLETED */
      W[0x0E54] += W[0x169AC];
      W16_SET(0xE2C, W16(0xE2C) ^ (int16_t)(1 << (W[0x0E0C] & 0x3f)));
      if (W16(0xE2C) == 0) {                         /* every stage cleared */
        W16_SET(0xE12, 1); RS_PH_SET(8); W[0x1697C] = 0x3f;
      } else {
        W16_SET(0xE64, W16(0xE64) + 1);
        W16_SET(0xE66, W16(0xE66) + 1);
        W16_SET(0xE6A, 0);
        W[0x0CC0] = 0x1a;
        if (W16(0x15F5C) >= 2) FUN_0000f584(0x43);   /* 0x024B76 */
        else W16_SET(0x15F5C, 9999);                 /* 0x024B86 */
      }
    } else {                                         /* FAILED */
      g_fog_r = 0; g_fog_g = 0; g_fog_b = 0;         /* clr.w $e0eb1a/1c/1e */
      if (W[0x0E0C] == 3) {
        W[0x0E54] += W[0x169AC];
        sync_post();
        RS_PH_SET(8);
      } else {
        W[0x0CC0] = 0x1c;
      }
    }
    break;
  case 8:                                            /* 0x024BC6 */
    W[0xEB16] = (int16_t)(W[0xEB16] + 4);
    if ((int16_t)W[0xEB16] > 0xff) {
      W[0xEB16] = 0xff;
      W[0x0CC8] = 0xb;
      W[0x0CC0] = (FUN_00031d40(3, W[0x0E54]) != 0) ? 0x10 : 0x1e;
    }
    break;
  default:                                           /* 0x024C04 */
    W[0x16980] = -0x300;
    W[0x0CC0] = 8;
    break;
  }

  /* 0x024C16 -- the draw */
  if (t3 == 0) results_tilemap_scroll_setup();
  if (t3 > 0 && RS_PH() < 7) {
    if (W[0x0E0C] == 2) text_draw_rect_blink(0x12a, 6, 0xb, 0x240, 7);
  } else {
    text_draw_rect_solid(0x12a, 6, 0xb);
  }
  dsp_cmd_place_object_abs(0, 0x37d, 0, 0, 0x350);
  dsp_cmd_place_object_abs(0, 0x365, 0, 0, 0x350);
  sprite_draw_2d(6, W[0x0E0C] * 2 + 0x1e1, 0x2e, 0x67, 0x350, 0x20, 0x20, 0, 0);
  sprite_draw_2d(6, W[0x0E0C] * 2 + 0x1e2, 0x2e, 0x167, 0x350, 0x20, 0x20, 0, 0);
  if (W[0x0E0C] != 3) {
    sprite_draw_2d(6, 0x1ee, 0xfa, 0xf5, 0xc000, 0x20, 0x20, 0x5000, 0);
    sprite_draw_2d(6, 0x1ef, 0x211, 0x157, 0xc000, 0x20, 0x20, 0x5000, 0);
  }
  sprite_draw_2d(6, 0x1ef, 0x211, 0xce, 0xc000, 0x20, 0x20, 0x5000, 0);
  dsp_cmd_place_object_abs(0, W16(0xE66) + 0x36e, 0, 0, 0x350);  /* movea.w $e00e66 */
  if (W[0x0E0C] != 3) {                              /* POINTS REQUIRED, 0x024DA6 */
    int32_t q = W[0x0E60], x = 0x97;
    for (i = 0; i < 5; i++) {
      int32_t y = W[0x16980] * 0x22;
      if (y < 0) y += 0x3f;
      y >>= 6;
      dsp_cmd_set_camera(2, q % 10 + 0x347, x, y - 0x48, 0x1c0, 0, 0, 0, 0);
      dsp_cmd_set_camera(2, 0x351, x + 4, y - 0x4d, 0x1c1, 0, 0, 0, 0);
      q /= 10;
      x -= 0x2c;
    }
  }
  results_draw_point_gauge(0xc0);
  W[0x169B0] = score_odometer_animate(0, W[0x169B0], W[0x169AC], -0x40, 0x26, 0x19e);
  for (i = 0; i < 5; i++)
    dsp_cmd_place_object_rotated_abs(0, 0x351, i * 0x28 - 0x14, 0x22, 0x19e, 0, 0, 0, 0);
  results_draw_time_gauge(0xc0);
  results_draw_rank_pointer(0x5000, 9, 0x300, 0x900);
  if (W[0x16994] != 0) {                             /* THE STAMP, 0x02517E */
    int32_t sx, sy, sz;
    if (W[0x0E0C] == 3) { sx = 0xf0; sy = -0x280; sz = 0x520; }
    else                { sx = 0xd0; sy = -0x3d0; sz = 0x5c0; }
    /* one model, 0x341: COMPLETED on its face, FAILED on its back -- the Y
     * angle W16(0x169A6) is score_check_quota(), 0 or 0x8000. The FAILED
     * stamp is also tilted by 0x169A8 (ramped to 0x400 in case 2). */
    dsp_cmd_place_object_rotated_abs(5, 0x341, sx, sy, sz,
        (int32_t)W16(0x169A4), (int32_t)W16(0x169A6), (int32_t)W16(0x169A8), 3);
  }
  results_draw_minimap();
  W[0x1696C] += 1;
  results_sound_tail();
  return 0;
}

/* ---- results_screen_update_normal @ 0x023C90 ---- */

int results_screen_update_normal(void)
{
  int32_t t3; int i;
  results_dump_probe();
  t3 = W[0x1696C] - 3;                               /* -$8(a6) */
  if (t3 == 0) {
    results_graphics_load();
    mem_write16(0x8A0000, 0xff58);   /* ROM 0x023CE0, 16-bit */
    mem_write16(0x8A0002, 5);
  }
  W[0x16980] = 0;
  /* 0x023CF6: movea.w (a3),a0 ; cmpa.l #$a ; bhi default ; the table at
   * 0x023D0C (index a0.w*2): 0x23D22 0x23D50 0x23E06 0x23E9E default default
   * 0x23F2E default 0x23F72 0x24036 0x240DC. */
  switch ((uint32_t)RS_PH() > 10 ? -1 : RS_PH()) {
  case 0:                                            /* 0x023D22 */
    W[0xEB16] = (int16_t)(W[0x1697C] << 3);
    W[0x1697C] -= 1;
    if (W[0x1697C] < 0) { RS_PH_SET(2); W[0x1697C] = 0x40; W[0x16984] = 1; W[0x16988] = 0x708; }
    break;
  case 1:                                            /* 0x023D50 */
    if ((W[0x169AC] << 3) == W[0x169B0]) W[0x1697C] = 0;
    if (RS_START_EDGE()) { W16_SET(0x169A6, score_check_quota()); W[0x1697C] = 0; }
    if (RS_A_EDGE())     { W16_SET(0x169A6, score_check_quota()); W[0x1697C] = 0; }
    W[0x1697C] -= 1;
    if (W[0x1697C] >= 0) break;
    if (FUN_000256be() == 0) break;                  /* the rank reveal */
    W16_SET(0x169A6, score_check_quota());
    W[0x169B0] = W[0x169AC] << 3;
    if (W[0x0E18] == 2) { RS_PH_SET(2); W[0x1697C] = 0x1e; W[0x16984] = 9; W[0x16988] = 0x1e; }
    else { RS_PH_SET(2); W[0x1697C] = 0x3c; W[0x16984] = 3; W[0x16988] = W[0x0E48] / 3; }
    break;
  case 2:                                            /* 0x023E06 */
    if (RS_START_EDGE()) W[0x1697C] = 0;
    if (RS_A_EDGE())     W[0x1697C] = 0;
    if (W[0x1697C] == 0x96 && W[0x16984] == 8 && W[0x169C4] >= 4) sound_play(0x32);
    W[0x1697C] -= 1;
    if (W[0x1697C] >= 0) break;
    RS_PH_SET(W[0x16984]);
    W[0x1697C] = W[0x16988];
    if (RS_PH() == 1) W[0x169AC] += W[0x0E4C];
    if (RS_PH() == 3) W[0x169AC] += W[0x169CC];
    if (RS_PH() == 6 && W[0x0E18] == 1) W[0x169AC] += 1000;
    W16_SET(0x169A6, score_check_quota());
    break;
  case 3:                                            /* 0x023E9E */
    if ((W[0x169AC] << 3) == W[0x169B0]) W[0x1697C] = 0;
    if (RS_START_EDGE()) { W16_SET(0x169A6, score_check_quota()); W[0x1697C] = 0; }
    if (RS_A_EDGE())     { W16_SET(0x169A6, score_check_quota()); W[0x1697C] = 0; }
    W[0x1697C] -= 1;
    if (W[0x1697C] >= 0) break;
    if (FUN_000256be() == 0) break;
    W[0x169B0] = W[0x169AC] << 3;
    W16_SET(0x169A6, score_check_quota());
    W[0x0E48] = 0;
    RS_PH_SET(2); W[0x1697C] = 0x3c; W[0x16984] = 6; W[0x16988] = 0x708;
    break;
  case 6:                                            /* 0x023F2E */
    if (RS_A_EDGE()) W[0x1697C] = 0;
    if ((W[0x169AC] << 3) != W[0x169B0]) break;
    if (FUN_000256be() == 0) break;
    RS_PH_SET(2); W[0x1697C] = 0xb4; W[0x16984] = 10; W[0x16988] = 0x1e;
    break;
  case 8: {                                          /* 0x023F72 -- fade out */
    int16_t lv = (int16_t)W[0xEB16];
    if (lv == 0 || lv == 0x3c) sound_play(0x17);
    if (W[0x169C4] >= 4) FUN_0000f66a(0x32, lv > 0xff ? 0xff : lv);
    W[0xEB16] = (int16_t)(lv + 4);
    if ((int16_t)W[0xEB16] > 0xff) {
      mem_write16(0x8A0000, 0x35c);    /* ROM 0x023FC0, 16-bit */
      mem_write16(0x8A0002, 0);
      W[0x0CC8] = 6;
      W[0x0E54] = W[0x169AC];
      W[0xEB16] = 0xff;
      if (FUN_00031d40(W[0x0E0C], W[0x169AC]) != 0 && W[0x169C4] == 5) {
        W[0x0CC0] = 0x10;                            /* the top-rank sequence */
      } else {
        W[0x0CC0] = 10;
        if (W16(0x15F5C) >= 2) FUN_0000f584(0x43);   /* 0x024018 */
        W16_SET(0x15F5C, 9999);
      }
      game_stats_record_end();
    }
    break; }
  case 9:                                            /* 0x024036 -- rank icon drop */
    if ((W[0x1697C] == 0x3c && W[0x169C4] >= 4) || (W[0x1697C] == 0 && W[0x169C4] < 4)) {
      int32_t n = W[0x169C4];
      /* 0x024056 `move.l $e04088(d0.l*8),d0` (ext 0x0FB0, scale 8) */
      if ((int32_t)(W[0x4088 + W[0x0E0C] * 8] & 0xffffff) < W[0x169AC] && W[0x169C4] == 5)
        n = 6;
      sound_play_event(n);
    }
    if (RS_START_EDGE()) W[0x1697C] = 0;
    if (RS_A_EDGE())     W[0x1697C] = 0;
    W[0x169C8] = W[0x1697C] * -0x18 + 0x350;
    W[0x1697C] -= 1;
    if (W[0x1697C] < 0) {
      W[0x169C8] = 0x350;
      RS_PH_SET(2); W[0x1697C] = 0xf0; W[0x16984] = 8; W[0x16988] = 0x1e;
    }
    break;
  case 10:                                           /* 0x0240DC */
    if (W16(0x169DC) == 0) break;                    /* the map trail finished */
    if (W[0x169C4] > 2) { RS_PH_SET(9); W[0x1697C] = 0x40; }
    else                { RS_PH_SET(8); W[0x1697C] = 0x1e; }
    break;
  default:                                           /* 0x024100 */
    W[0x16980] = -0x300;
    W[0x0CC0] = 8;
    break;
  }

  /* 0x024112 -- the draw */
  dsp_cmd_place_object_abs(0, 0x37d, 0, 0, 0x350);
  dsp_cmd_place_object_abs(0, 0x373, 0, 0, 0x350);
  dsp_cmd_place_object_abs(0, 0x368, 0, 0, 0x350);
  sprite_draw_2d(6, W[0x0E0C] * 2 + 0x1e1, 0x2e, 0x67, 0x350, 0x20, 0x20, 0, 0);
  sprite_draw_2d(6, W[0x0E0C] * 2 + 0x1e2, 0x2e, 0x167, 0x350, 0x20, 0x20, 0, 0);
  sprite_draw_2d(6, 0x1f0, 0x215, 0x9e, 0xc000, 0x20, 0x20, 0, 0);
  sprite_draw_2d(6, 0x1eb, 0xf5, 0xd3, 0x34f, 0x20, 0x20, 0, 0);
  sprite_draw_2d(6, 0x1ec, 0x1f5, 0xd3, 0x34f, 0x20, 0x20, 0, 0);
  results_draw_point_gauge(0xb0);
  results_draw_time_gauge(0xb0);
  results_draw_rank_pointer(0x6000, 6, 0x3d0, 0x900);
  W[0x169B0] = score_odometer_animate(0, W[0x169B0], W[0x169AC], -0x3c, 0x48, 0x19e);
  for (i = 0; i < 5; i++)
    dsp_cmd_place_object_rotated_abs(0, 0x351, i * 0x28 - 0x10, 0x44, 0x19e, 0, 0, 0, 0);
  /* the rank-list arrow, 0x024540: `movea.w $e169ba,a0` and `pea $1940.w`
   * -- an immediate x, not &g_sys.rom[0x1940] */
  dsp_cmd_set_camera(2, 0x359, 0x1940, (int32_t)W16(0x169BA), 0x2000, 0, 0, 0x8000, 0);
  results_draw_minimap();
  results_rank_blink();
  W[0x1696C] += 1;
  results_bonus_dispatch();
  results_sound_tail();
  return 0;
}





