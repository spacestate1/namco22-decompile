/*
 * Math Utilities
 * Auto-split from game_deps.c / game_ported.c
 */
#include "propcycl.h"

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

/* ---- math_slope_angle ---- */
/* V2-MIGRATED: math_slope_angle @ 0x00A99E now lives in
 * src/v2/game_math_v2.c, translated from the M68K machine code and
 * L2-verified against MAME (3402/3402 calls match; this transpiled
 * version matched 0/3402 — native-LE read, unscaled index, table value
 * doubled). The fc~301 attract-demo div-by-zero guard moved with it. */


/* ---- score_odometer_animate ---- */

/* THE RESULTS-SCREEN SCORE ODOMETER (ROM @0x0252CC), three bugs deep
 * (register row 105). It steps a 5-digit reel one notch per call toward
 * `param_3`, draws the five faces, and returns the new packed
 * `(sub_step & 7) | (value << 3)` the caller stores back.
 *
 *  1. THE DIGIT ARRAY AT 0xE169D0 IS A 16-BIT ARRAY, STRIDE 2. Every ROM
 *     access is `move.w`/`movea.w (A2, D2.l*2)` with A2 = 0xE169D0
 *     (0x25306, 0x2534C, 0x25368, 0x25398, 0x253DA). The C indexed it
 *     `W[0x169D0 + i]` -- stride ONE in the `_W[]` byte-offset model -- so
 *     digits 1..4 landed on slots 0x169D1..0x169D4, i.e. 1-mod-4 and
 *     2-mod-4 offsets that `sync_wram_to_W` rebuilds from neighbouring
 *     bytes every frame. Same class as register row 71; `W_A16` exists for
 *     exactly this.
 *  2. BOTH ROM TABLES WERE READ HOST-NATIVE. `rom_nr32(0x37550 + d*4)` on a
 *     big-endian table of 32-bit model ids -- the ROM is
 *     `move.l ($37550,D0.w*4),D7` at 0x2539C and `($37524,...)` at 0x253DE.
 *     Read BE they are [637..646] and [627..636]: the POINT reel's face
 *     models (record codes 706..715 and 696..705, the very set register
 *     row 100 measured under the HUD viewport). Read natively the first
 *     entry is 0x7D020000 = 2,097,283,072, which every draw path rejects
 *     as an out-of-range model -- so the odometer drew NOTHING.
 *  3. Consequence, and the visible symptom: with the digits garbage the
 *     count-up never reached its target, so `results_screen_update_normal`
 *     phase 1 never took its early exit (`(W[0x169AC] << 3) == W[0x169B0]`)
 *     and sat out its full 1800-frame timeout. Measured against MAME
 *     (tools/overnight/snap_results.lua): the machine spends 57 frames in
 *     that phase, we spent 1830.
 */
int score_odometer_animate
              (undefined4 param_1,uint32_t param_2,int param_3,int param_4,undefined4 param_5,
              undefined4 param_6)

{
  short sVar1;
  int iVar2;
  int iVar3;
  uint32_t uVar4;
  int local_12;
  int local_a;
  
  uVar4 = param_2 & 7;
  if ((int)param_2 >> 3 < param_3) {
    local_a = -1;
    iVar3 = 1;
    iVar2 = 0;
    do {
      sVar1 = W_A16(0x169D0, iVar2);
      if (((int)sVar1 != (param_3 / iVar3) % 10) && (local_a < 0)) {
        uVar4 = uVar4 + 1 & 7;
        local_a = iVar2;
        if (uVar4 == 0) {
          sound_play(SND_ID_UNRECOVERED);   /* id lost by the decompiler */
          sVar1 = (short)((sVar1 + 1) % 10);
        }
      }
      W_A16_SET(0x169D0, iVar2, sVar1);
      iVar3 = iVar3 * 10;
      iVar2 = iVar2 + 1;
    } while (iVar2 < 5);
  }
  local_12 = 0;
  iVar3 = 1;
  iVar2 = 0;
  do {
    local_12 = iVar3 * ((int)(short)W_A16(0x169D0, iVar2) % 10) + local_12;
    iVar3 = iVar3 * 10;
    iVar2 = iVar2 + 1;
  } while (iVar2 < 5);
  iVar2 = param_4 + 200;
  iVar3 = 0;
  do {
    dsp_cmd_set_camera(param_1,(int32_t)vrd32(0x37550 + (int)(short)W_A16(0x169D0, iVar3) * 4),
                       iVar2,param_5,param_6,0,0,0,0);
    iVar2 = iVar2 + -0x28;
    iVar3 = iVar3 + 1;
  } while (iVar3 < 5);
  param_4 = param_4 + 200;
  iVar2 = 0;
  do {
    if (iVar2 == local_a) {
      sVar1 = (short)uVar4 * -0x1000;
    }
    else {
      sVar1 = 0;
    }
    dsp_cmd_set_camera(param_1,(int32_t)vrd32(0x37524 + (int)(short)W_A16(0x169D0, iVar2) * 4),
                       param_4,param_5,param_6,(int)sVar1,0,0,0);
    if (sVar1 != 0) {
      dsp_cmd_set_camera(param_1,(int32_t)vrd32(0x37524 + (((int)(short)W_A16(0x169D0, iVar2) + 1) % 10) * 4),
                         param_4,param_5,param_6,0,0,0,0);
    }
    param_4 = param_4 + -0x28;
    iVar2 = iVar2 + 1;
  } while (iVar2 < 5);
  return uVar4 + local_12 * 8;
}


/* ---- spherical_to_cartesian ---- */

void spherical_to_cartesian
               (uint32_t param_1,int param_2,int param_3,int *param_4,int *param_5,int *param_6)

{
  /* (pitch, heading, speed) -> (dx, dy, dz). THIS IS THE FLIGHT VELOCITY.
   *
   * It read cos(pitch) and cos(heading) as `vrd16s(0x20B006 + idx * 2)` -- a
   * ONE-BYTE read of a 16-bit Q15 table (max 127, the CLAUDE.md fix-#11
   * class) -- so `cos*cos >> 15` was always 0 and the X velocity was
   * exactly zero: the bike's X was frozen at 194142 for the entire demo
   * flight while the recording turns to 320 degrees. It also used the
   * half-stride, sin/cos-swapped index (angle>>1 & 0x7ffe at 0x20B004/6)
   * already corrected in rotate_euler_zxy_optimized. Table convention,
   * verified against real sin/cos:
   *     cos(a) = vrd16s(0x20B002 + (a & 0xfffc))
   *     sin(a) = vrd16s(0x20B004 + (a & 0xfffc))
   * The +0x4000 on the heading is the original's (heading + 90 deg), kept
   * as-is: sin(h+90) = cos h, cos(h+90) = -sin h. */
  int cos_p = vrd16s(0x20B002 + (param_1 & 0xfffc));
  int sin_p = vrd16s(0x20B004 + (param_1 & 0xfffc));
  uint32_t h  = (uint32_t)param_2 + 0x4000U;
  int cos_h = vrd16s(0x20B002 + (h & 0xfffc));
  int sin_h = vrd16s(0x20B004 + (h & 0xfffc));
  *param_5 = param_3 * sin_p >> 0xf;
  *param_6 = param_3 * (short)(sin_h * cos_p >> 0xf) >> 0xf;
  *param_4 = param_3 * (short)(cos_h * cos_p >> 0xf) >> 0xf;
  return;
}



/* ========== TIER 3 DEPENDENCIES ========== */
/* 62 functions extracted from game_all.c */

/* ---- math_lerp_int ---- */

int math_lerp_int(int param_1,int param_2,int param_3,int param_4,int param_5)

{
  return param_1 + ((param_5 - param_3) * (param_2 - param_1)) / (param_4 - param_3);
}

/* ---- math_atan2_coarse @ 0x00A91A ---- */
/* V2-MIGRATED: math_atan2_coarse now lives in src/v2/game_math_v2.c,
 * translated from the M68K machine code (quadrant jump table at 0xA966
 * decoded) and L2-verified against MAME (262/262 calls match; this
 * stubbed version returned 0 for every quadrant — matched only the 54
 * calls whose true answer happened to be 0). */





