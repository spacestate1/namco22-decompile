/*
 * Gameplay State Machine
 * Auto-split from game_deps.c / game_ported.c
 */
#include <stdlib.h>   /* getenv: an implicit int declaration truncates the pointer */
#include "propcycl.h"
#include "ending_rd.h"
#include <stdlib.h>   /* getenv returns a POINTER: see name_entry_test_env */

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

/* Read 32-bit big-endian value from ROM (fixes M68K→x86 endianness).
 * Use this anywhere the transpiled code does *(int*)(&R[offset]). */
#define ROM_READ32(off) ((int32_t)((R[off]<<24)|(R[(off)+1]<<16)|(R[(off)+2]<<8)|R[(off)+3]))

/* Cross-file function declarations (pointer-returning functions only,
 * to prevent 64-bit pointer truncation from implicit int return) */
short * sound_play_p();
short * sound_play_or_defer();
short * sound_play_p2();
undefined4 * dsp_cmd_emit_arrow_indicator();
undefined4 * player_vehicle_dsp_render();
undefined4 * render_town_with_rotation();
char * eeprom_write_verify_block();

/* Forward declarations (called before definition in this file) */
uint16_t gameplay_sub17_run();
uint16_t gameplay_sub25_gameover_run();
uint32_t gameplay_sub27_run();
uint32_t gameplay_sub5_service_run();
void gameplay_sub29_run(void);
void gameplay_sub31_run(void);

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
#define g_scene_max_priority W16(0x15F46)
/* sound_set_volume (ROM 0x00F98A) takes TWO words, pushed `move.w level ;
 * move.w slot`: the PARAMETER SLOT in the high half, the level in the low.
 * Every call in stage_camera_path_update that the decompile reduced to a
 * single level wrote PARAMETER SLOT 0 instead of its own -- once or more
 * every gameplay frame. The slots below are read off the ROM's pushes. */
#define SNDVOL(slot, level) \
    sound_set_volume(((uint32_t)(uint16_t)(slot) << 16) | (uint16_t)(level))
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

/* ---- gameplay_init_player_and_world ---- */

void gameplay_init_player_and_world(void)

{
  int iVar1;
  int iVar2;
  undefined4 uVar3;
  undefined4 uVar4;
  undefined4 uVar5;
  
  /* Read player start position from ROM tables (big-endian 32-bit).
   * Tables at ROM 0xB85B8, stride 0x10 per course, 6 values: X,Y,Z,?,yaw,? */
  {
    int ci = (int)W[0x0E0C];  /* course index */
    int o0 = 0xB85B8 + ci * 4; W[0x0D00] = (R[o0]<<24)|(R[o0+1]<<16)|(R[o0+2]<<8)|R[o0+3];
    int o1 = 0xB85C8 + ci * 4; W[0x0D04] = (R[o1]<<24)|(R[o1+1]<<16)|(R[o1+2]<<8)|R[o1+3];
    int o2 = 0xB85D8 + ci * 4; W[0x0D08] = (R[o2]<<24)|(R[o2+1]<<16)|(R[o2+2]<<8)|R[o2+3];
    int o3 = 0xB85E8 + ci * 4; W[0x0D0C] = (R[o3]<<24)|(R[o3+1]<<16)|(R[o3+2]<<8)|R[o3+3];
    int o4 = 0xB85F8 + ci * 4; W[0x0D10] = (R[o4]<<24)|(R[o4+1]<<16)|(R[o4+2]<<8)|R[o4+3];
    int o5 = 0xB8608 + ci * 4; W[0x0D14] = (R[o5]<<24)|(R[o5+1]<<16)|(R[o5+2]<<8)|R[o5+3];
    if (propcycl_verbose()) printf("[INIT] course=%d player=(%d,%d,%d) heading=%d\n",
           ci, (int)W[0x0D00], (int)W[0x0D04], (int)W[0x0D08], (int)W[0x0D10]);
  }
  world_grid_calc_position();
  if (W16(0xE10) == 0) {
    iVar1 = ROM_READ32(0x15C448 + ((int)W[0x3FFA] + W[0x0E0C] * 3) * 4);
  }
  else {
    if (W16(0xE6A) < 4) {
      iVar1 = (int)W16(0xE6A);
    }
    else {
      iVar1 = 3;
    }
    iVar2 = (W16(0xE64) * 6 + W[0x0E0C] * 0x12 + W[0x3FFA] * 2) * 4;
    /* ROM 0x00AE4C: muls.l $15c4f8.l,d4 -- the 300-point quota discount per
     * continue taken is a BE32; R[0x15C4F8] was its high BYTE (0), so the
     * story quota never dropped after a continue (MAME: 1000 -> 700 -> 400
     * -> 100). */
    W[0x0E60] = ROM_READ32(0x15C328 + iVar2) - vrd32s(0x15C4F8) * iVar1;
    iVar1 = ROM_READ32(0x15C32C + iVar2);
  }
  W[0x0E44] = iVar1 * 0x3c;
  if (W[0x0E0C] == 0) {
    uVar5 = 0x7f;
    uVar4 = 0x40;
    uVar3 = 0x40;
  }
  else {
    uVar5 = 0;
    uVar4 = 0;
    uVar3 = 0;
  }
  set_background_color(uVar3,uVar4,uVar5);
  environment_zone_init();
  game_stats_record_start();
  return;
}


/* ---- stage_camera_path_update ---- */

uint32_t stage_camera_path_update(void)

{
  int bVar1;
  short sVar5;
  int iVar2;
  int iVar3;
  short sVar6;
  uint32_t uVar4;
  short sVar7;
  int iVar8;
  uint16_t uVar9;
  int *piVar10;
  short local_e;
  
  if ((W[0x0CBC] - 3U != 0) && (W[0x3FFC] == 0)) {
    return W[0x0CBC] - 3U;
  }
  if (W[0x0E0C] == 1) {
    FUN_0000fe42();
  }
  if (W[0x0E0C] == 3) {
    FUN_0000fd56();
  }
  if (W16(0x15F34) != 0) {
    FUN_0000fcc6();
  }
  if ((W[0x0E18] < 1) || (W[0x0CBC] != 3)) {
    if ((W[0x0E44] < 0x259) &&
       (((0x3b < W[0x0E44] && (W[0x0E44] < W16(0x15F38))) &&
        (W[0x0E44] % 0x3c == 0)))) {
      sound_play_ungated();
    }
    if ((W16(0x15F3A) < W[0x0E48]) && ((W[0x0E48] % 0x3c & 0xfcU) == 4))
    goto LAB_00010338;
  }
  else {
    if (W16(0x15F34) == 0) {
      W16_SET(0x15F34, 1);
    }
    if (W16(0x15F34) < 2) {
      W16_SET(0x15F34, 2);
LAB_00010338:
      sound_play_ungated();
    }
    else if (((g_scene_max_priority < 0x15) && (W[0x0E0C] != 3)) && (W16(0x15F34) == 2)) {
      W16_SET(0x15F34, 3);
      if (W[0x0E18] == 2) {
        if (W[0x0E0C] != 3) {
          sound_play_p_ungated(0x00260071);
        }
      }
      else {
        sound_play_p_ungated(0x00260075);
      }
    }
  }
  W16_SET(0x15F38, (short)W[0x0E44]);
  W16_SET(0x15F3A, (short)W[0x0E48]);
  if (W[0x0CAC] != 0) {
    SNDVOL(8, 0xff);                    /* ROM 0x01059C: slots 8, 6, 2 */
    SNDVOL(6, 0xff);
    SNDVOL(2, 0xff);
    sound_set_volume(CONCAT22(vrd16(0x35818),0xff));
    sound_set_volume(CONCAT22(vrd16(0x357E8),0xff));
    sound_set_volume(CONCAT22(vrd16(0x357F4),0xff));
    sound_set_volume((((uint32_t)(vrd16(0x35998)) << 16) | (uint16_t)(0xff)));
    uVar4 = sound_stop(W16(0x15F42));
    W16_SET(0x15F42, -1);
    return uVar4;
  }
  sVar6 = (short)W[0x0D48] + -0xe77;
  SNDVOL(8, (short)((sVar6 * -0x50) / 0x1d09) + 0x50);    /* ROM 0x01037C */
  if (sVar6 < 0) {
    sVar6 = 0;
  }
  if (0x2e4b < sVar6) {
    sVar6 = 0x2e4b;
  }
  comms_w16(g_sys.commsram, 0x010E, (short)((sVar6 * 0x1500) / 0x2e4b) + 0x1400);
  sVar6 = (short)(W[0x0E00] >> 3);
  if (sVar6 < 1) {
    sVar6 = 1;
  }
  if (0x1000 < sVar6) {
    sVar6 = 0x1000;
  }
  if (sVar6 < 0x401) {
    sVar7 = (short)((uint32_t)(sVar6 * 0x2c) >> 8);
    sVar5 = 0x100;
  }
  else {
    sVar7 = (short)((uint32_t)((int)sVar6 << 4) / 0xc00);
    sVar5 = 0x55;
  }
  SNDVOL(6, sVar5 - sVar7);                                /* ROM 0x0103FC */
  sVar6 = sVar6 * 3 + 0x2000;
  if (0x4b7f < sVar6) {
    sVar6 = 0x4b80;
  }
  comms_w16(g_sys.commsram, 0x010A, sVar6);
  iVar8 = 0;
  local_e = 0;
  do {
    if (W[0x1424 + (local_e * 0x140)] != 0) {
      iVar8 = W[0x1428 + (local_e * 0x140)];
      break;
    }
    local_e = local_e + 1;
  } while (local_e < 6);
  sVar6 = (short)((vrd16s(0x20B004 + (W[0x0D9C] & 0xfffc))) * -0x1b >> 0xe) + -2;
  if (sVar6 < 1) {
    W16_SET(0x15F4A, W16(0x15F4A) & 0xfffe);
    sVar6 = 0;
  }
  if (0x28 < sVar6) {
    sVar6 = 0x28;
  }
  if (((sVar6 == 0x28) || ((g_terrain_flags != 0 && (0x18 < sVar6)))) &&
     (0x8000 < (W[0x0D0C] & 0xffff))) {
    W16_SET(0x15F4C, W16(0x15F4C) + 1);
    if (W16(0x15F4C) == 2) {
      sound_play(0x03);
    }
  }
  else {
    W16_SET(0x15F4C, 0);
  }
  if (0xe1 < iVar8) {
    if ((g_scene_max_priority < 1) && (W[0x1424] != 0)) {
      sound_play(0x03);
    }
    sVar6 = (short)(iVar8 << 5) + -0x1c40 + sVar6;
    if (0x28 < sVar6) {
      sVar6 = 0x28;
    }
  }
  comms_w16(g_sys.commsram, 0x0102, (short)((sVar6 * 0xc00) / 0x28) + 0x2000);
  SNDVOL(2, (short)(((int)sVar6 << 6) / -0x28) + 0x40);    /* ROM 0x010538 */
  if (((((W16(0x15F4A) & 1) == 0) && (3 < W16(0x15F4C))) &&
      (0x8000 < W[0x0D04] - W[0x2AA8])) && (W[0x0D50] < 0x2800)) {
    sound_play_cooldown((W[0x0D50] & 2) ? 0x50 : 0x51);   /* ROM 0x01057C */
    W16_SET(0x15F4A, W16(0x15F4A) | 1);
  }
  uVar9 = g_sys.commsram[0];
  W[0x3ED0] = (uint32_t)uVar9;
  uVar9 = g_sys.commsram[0x0002];
  W[0x3ED4] = (uint32_t)uVar9;
  uVar9 = g_sys.commsram[0x02A0];
  W[0x3ED8] = (uint32_t)uVar9;
  uVar9 = g_sys.commsram[0x02A2];
  W[0x3EDC] = (uint32_t)uVar9;
  uVar9 = g_sys.commsram[0x02A4];
  W[0x3EE0] = (uint32_t)uVar9;
  uVar9 = g_sys.commsram[0x02C0];
  W[0x3EE4] = (uint32_t)uVar9;
  uVar9 = g_sys.commsram[0x02C2];
  W[0x3EE8] = (uint32_t)uVar9;
  uVar9 = g_sys.commsram[0x02C4];
  W[0x3EEC] = (uint32_t)uVar9;
  sVar6 = W16(0x15F5E) - (short)W[0x0D10];
  if ((((0x1000 < sVar6) && (sVar6 < 0xf000)) || (0x1000 < W[0x0D4C] - W[0x0D48])) &&
     (((W16(0x15F4A) & 2) == 0 && (W[0x0CBC] != 1)))) {
    sound_play_cooldown((W[0x0D54] & 2) ? 0x55 : 0x56);   /* ROM 0x0106C2 */
    W16_SET(0x15F4A, W16(0x15F4A) | 2);
  }
  W16_SET(0x15F5E, (short)W[0x0D10]);
  uVar9 = 0;
  bVar1 = true;
  if ((iVar8 == 0xe0) || (iVar8 == 0xe1)) {
    if ((W16(0x15F3C) & 0x80) == 0) {
      sound_play(0x39);
    }
    W16_SET(0x15F3C, W16(0x15F3C) | 0x80);
    W16_SET(0x15F3E, 0xf0);
    bVar1 = false;
  }
  else {
    W16_SET(0x15F3C, W16(0x15F3C) & 0xff7f);
  }
  if (W[0x16E4] == 0) {
    uVar9 = 1;
  }
  else {
    if ((bVar1) && ((W16(0x15F3C) & 8) == 0)) {
      FUN_0000fb38((short)W[0x16EC],0x3c,(short)W[0x0D4C]);
      bVar1 = false;
    }
    W16_SET(0x15F3C, W16(0x15F3C) | 8);
  }
  if (W[0x1824] == 0) {
    uVar9 = uVar9 + 1;
  }
  else {
    if ((bVar1) && ((W16(0x15F3C) & 0x10) == 0)) {
      FUN_0000fb38((short)W[0x182C],10,(short)W[0x0D4C] + 0x800);
      if (((W16(0x15F4A) & 2) == 0) && (0x800 < W[0x0D4C])) {
        sound_play_cooldown(0x54);
        W16_SET(0x15F4A, W16(0x15F4A) | 2);
      }
      bVar1 = false;
    }
    W16_SET(0x15F3C, W16(0x15F3C) | 0x10);
  }
  W16_SET(0x15F4E, W16(0x15F4E) - 1);
  if (W[0x1324] == 0) {
    uVar9 = uVar9 + 1;
  }
  else {
    if ((bVar1) && ((W16(0x15F3C) & 1) == 0)) {
      if ((W[0x132C] == 3) || ((W[0x132C] == 7 || (W[0x132C] == 8)))) {
        FUN_0000fb38((short)W[0x132C],5,(short)W[0x0D4C] + -0x800);
      }
      else {
        if ((((0x1800 < W[0x0D4C]) && (W16(0x15F4E) < 1)) && (W[0x1464] == 0)) &&
           ((W[0x15A4] == 0 && (W[0x16E4] == 0)))) {
          sound_play(0x38);
          W16_SET(0x15F3E, 0xf0);
          W16_SET(0x15F4E, 0x5a);
        }
        if (((W16(0x15F4A) & 2) == 0) && (0x4000 < W[0x0D4C])) {
          sound_play_cooldown(0x54);
          W16_SET(0x15F4A, W16(0x15F4A) | 2);
        }
      }
      bVar1 = false;
    }
    W16_SET(0x15F3C, W16(0x15F3C) | 1);
  }
  if (W[0x1464] == 0) {
    uVar9 = uVar9 + 1;
  }
  else {
    if ((bVar1) && ((W16(0x15F3C) & 2) == 0)) {
      iVar8 = FUN_0000fb38((short)W[0x146C],5,(short)W[0x0D4C] + -0xc00);
      if (((W16(0x15F4A) & 2) == 0) &&
         (((0x3800 < W[0x0D4C] && (iVar8 != 0)) && (W[0x0CBC] != 1)))) {
        sound_play_cooldown(0x54);
        W16_SET(0x15F4A, W16(0x15F4A) | 2);
      }
      bVar1 = false;
    }
    W16_SET(0x15F3C, W16(0x15F3C) | 2);
  }
  if (W[0x15A4] == 0) {
    uVar9 = uVar9 + 1;
  }
  else {
    if ((((bVar1) && ((W16(0x15F3C) & 4) == 0)) &&
        ((iVar8 = FUN_0000fb38((short)W[0x15AC],5,(short)W[0x0D4C] + -0xc00),
         (W16(0x15F4A) & 2) == 0 && ((0x3800 < W[0x0D4C] && (iVar8 != 0)))))) &&
       (W[0x0CBC] != 1)) {
      sound_play_cooldown(0x54);
      W16_SET(0x15F4A, W16(0x15F4A) | 2);
    }
    W16_SET(0x15F3C, W16(0x15F3C) | 4);
  }
  if (((W16(0x15F4A) & 2) == 0) && (W[0x12D4] != 0)) {
    sound_play_cooldown(0x54);
    W16_SET(0x15F4A, W16(0x15F4A) | 2);
  }
  W16_SET(0x15F3E, W16(0x15F3E) - 1);
  if (uVar9 < 5) {
    if (W16(0x15F3E) < 0) {
      W16_SET(0x15F3C, W16(0x15F3C) & 0x80);
    }
  }
  else {
    W16_SET(0x15F3C, W16(0x15F3C) & 0x80);
    W16_SET(0x15F4A, W16(0x15F4A) & 0xfffd);
  }
  if (g_scene_max_priority < 0) {
    /* ROM 0x010A1E..0x010A8C: while the hit-reaction counter 0xE169EE runs
     * (`tst.w ; ble`), keep sound 0x2F -- the sound-table row at 0x357D8 --
     * playing. Every field of that row is a BIG-ENDIAN WORD: `move.w (a0),d1`
     * is the command SLOT (27), `move.w $2(a0)` the command (0x0020),
     * `$8(a0)`/`$a(a0)` the parameter slot and level pushed to the helper held
     * in d4, 0x00F98A = sound_set_volume(slot<<16 | level). The mailbox access
     * is `move.w (a0, d1.w*2)` off 0xA04000, i.e. a 16-bit command word.
     *
     * The decompile read the slot as ONE BYTE, `R[0x357D8]` -- the zero high
     * byte of 0x001B -- and wrote a byte, so on every frame of every hit it
     * stored 0x00 into the high byte of SLOT 0, THE MUSIC. That cleared the
     * music's bit 15 ("keep looping") and the driver ended the song at its
     * next loop point: the theme stopped after the first collision on every
     * course and never came back. It also passed only the level to
     * sound_set_volume, which set parameter slot 0 instead of 0x5E, and the
     * hit sound itself never played. */
    if (0 < (int)W_LO16(0x169EC)) {
      int      hslot = vrd16s(0x357D8);
      uint32_t hcmd  = (uint32_t)(int32_t)vrd16s(0x357DA);
      if (hslot >= 0 &&
          (comms_r16(g_sys.commsram, (unsigned)hslot * 2) & 0x80ffu) != (hcmd | 0x8000u)) {
        sound_set_volume(((uint32_t)vrd16(0x357E0) << 16) | vrd16(0x357E2));
        comms_w16(g_sys.commsram, (unsigned)hslot * 2, (hcmd | 0xc000u) & 0xffffu);
      }
    }
  }
  else {
    W16_SET(0x15F46, g_scene_max_priority - 1);
    if (g_scene_max_priority < 0) {
      W16_SET(0x15F42, -1);
    }
  }
  sVar6 = (short)W[0x0CE0];
  if (W[0x0E0C] == 0) {
    if ((((int)W[0x0CF4] < 0x24) && ((W[0x0CF4] & 6) == 2)) && (0x12 < (int)W[0x0CF4])) {
      iVar8 = scene_calc_3d_distance
                        ((short)(0x4c297 - W[0x0CDC]),0x2cbc - sVar6,
                         (short)(0x5dbdf - W[0x0CE4]));
      iVar2 = scene_calc_3d_distance
                        (0x56341 - W[0x0CDC],0x10108 - W[0x0CE0],0x6632e - W[0x0CE4])
      ;
      iVar3 = scene_calc_3d_distance
                        ((short)(0x4bcfa - W[0x0CDC]),(short)(0x11e54 - W[0x0CE0]),
                         0x53280 - W[0x0CE4]);
      if (iVar2 < iVar8) {
        iVar8 = iVar2;
      }
      if (iVar3 < iVar8) {
        iVar8 = iVar3;
      }
      SNDVOL(vrd16(0x35818), (short)((iVar8 << 8) / 0x20000));   /* ROM 0x010B54 */
      if ((W16(0x15F42) != 0x34) && (g_scene_max_priority < 0)) {
        W16_SET(0x15F42, 0x34);
        sound_play(0x34);
      }
      goto LAB_00010e78;
    }
    if (((0x50 < (int)W[0x0CF4]) && ((W[0x0CF4] - 1 & 4) == 0)) && ((int)W[0x0CF4] < 0x65))
    {
      if (W[0x0CE0] < 0x13800) {
        sVar6 = (0x3800 - sVar6) * 4;
      }
      else {
        sVar6 = 0x3800 - sVar6;
      }
      iVar8 = scene_calc_3d_distance
                        (-0x3d17 - (short)W[0x0CDC],sVar6,(short)(0x114068 - W[0x0CE4]));
      sound_set_volume(CONCAT22(vrd16(0x357F4),(short)((iVar8 << 8) / 0x40000)));
      if (W[0x0CE0] < 0x13000) {
        sVar6 = 0;
      }
      else {
        sVar6 = 0x3000 - (short)W[0x0CE0];
      }
      iVar8 = scene_calc_3d_distance
                        ((short)(0x536f3 - W[0x0CDC]),sVar6,(short)(0x113700 - W[0x0CE4]))
      ;
      sound_set_volume(CONCAT22(vrd16(0x357E8),(short)((iVar8 << 8) / 0x80000)));
      if ((W16(0x15F42) != 0x31) && (g_scene_max_priority < 0)) {
        W16_SET(0x15F42, 0x31);
        sound_play(0x31);
      }
      goto LAB_00010e78;
    }
    if (W16(0x15F42) < 1) goto LAB_00010e78;
    SNDVOL(vrd16(0x35818), 0xff);          /* ROM 0x010C72 */
    SNDVOL(vrd16(0x357E8), 0xff);          /* ROM 0x010C80 */
  }
  else {
    if (W[0x0E0C] == 2) {
      if ((W[0x0CF4] + 1 | 9) == 0x3d) {
        iVar8 = scene_calc_3d_distance
                          ((short)(0x63555 - W[0x0CDC]),(short)(0xd600 - W[0x0CE0]),
                           (short)(0xa888d - W[0x0CE4]));
        sound_set_volume(CONCAT22(vrd16(0x35998),(short)((iVar8 << 8) / 0x100000)));
        if ((W16(0x15F42) != 0x54) && (g_scene_max_priority < 0)) {
          W16_SET(0x15F42, 0x54);
          sound_play(0x54);
        }
        goto LAB_00010e78;
      }
    }
    else if (W[0x0E0C] == 1) {
      if (((0x5c < (int)W[0x0CF4]) && ((W[0x0CF4] & 4) != 0)) && ((int)W[0x0CF4] < 0x7e)) {
        if (W[0x0CE0] < 0x1a000) {
          sVar6 = 0;
        }
        else {
          sVar6 = -0x6000 - sVar6;
        }
        iVar8 = scene_calc_3d_distance
                          ((short)(0x8f8db - W[0x0CDC]),sVar6,
                           (short)(0x15029d - W[0x0CE4]));
        sound_set_volume
                  (CONCAT22(vrd16(0x357E8),(short)((iVar8 << 8) / 0x80000)));   /* ROM 0x010D96: a WORD */
        if ((W16(0x15F42) != 0x30) && (g_scene_max_priority < 0)) {
          W16_SET(0x15F42, 0x30);
          sound_play(0x30);
        }
        goto LAB_00010e78;
      }
    }
    else {
      if (W[0x0E0C] != 3) goto LAB_00010e78;
      if (((0x60 < (int)W[0x0CF4]) && ((W[0x0CF4] & 4) == 0)) && ((int)W[0x0CF4] < 0x7c)) {
        iVar8 = scene_calc_3d_distance
                          ((short)(0x33d95 - W[0x0CDC]),(short)(0x33b25 - W[0x0CE0]),
                           (short)(0x1488c8 - W[0x0CE4]));
        sound_set_volume
                  (CONCAT22(vrd16(0x35818),(short)((iVar8 << 8) / 0x40000)));   /* ROM 0x010E30: a WORD */
        if ((W16(0x15F42) != 0x34) && (g_scene_max_priority < 0)) {
          W16_SET(0x15F42, 0x34);
          sound_play(0x34);
        }
        goto LAB_00010e78;
      }
    }
    if (W16(0x15F42) < 1) goto LAB_00010e78;
  }
  /* The shared zone-exit tail at ROM 0x010E68: each course pushes its OWN
   * ambience slot before branching here -- course 0 0x357F4 (0x010C90, after
   * its two above), course 1 0x357E8 (0x010DCC), course 2 0x35998 (0x010D22),
   * course 3 0x35818 (0x010E5E) -- and restores it to full before stopping
   * the ambience. */
  SNDVOL(W[0x0E0C] == 0 ? vrd16(0x357F4) :
         W[0x0E0C] == 1 ? vrd16(0x357E8) :
         W[0x0E0C] == 2 ? vrd16(0x35998) : vrd16(0x35818), 0xff);
  sound_stop(W16(0x15F42));
  W16_SET(0x15F42, -1);
LAB_00010e78:
  W16_SET(0x15F40, (short)W[0x0CF4]);
  if (0 < W16(0x15F48)) {
    W16_SET(0x15F48, W16(0x15F48) - 1);
  }
  if (0 < W16(0x15F5C)) {
    W16_SET(0x15F5C, W16(0x15F5C) - 1);
    return W[0x0CF4];
  }
  if (W[0x0E0C] == 0) {
    if (W[0x0D24] == 0x1b) {
      for (piVar10 = &R[0x35B68]; -1 < *piVar10; piVar10 = piVar10 + 3) {
        if ((W[0x0D04] - ((int32_t*)(intptr_t)piVar10)[1] < 0xa01) && (-0xa01 < W[0x0D04] - ((int32_t*)(intptr_t)piVar10)[1])) {
          iVar8 = W[0x0D00] - *piVar10;
          if (iVar8 < 0) {
            iVar8 = -iVar8;
          }
          iVar2 = W[0x0D08] - ((int32_t*)(intptr_t)piVar10)[2];
          if (iVar2 < 0) {
            iVar2 = -iVar2;
          }
          if (iVar2 + iVar8 < 0x1001) {
            W16_SET(0x15F5C, 200);
            sound_play(0x08);
          }
        }
      }
    }
    if (((W[0x0D24] == 0x1a) && (W[0x0D04] - R[0x35BA0] < 0xa00)) &&
       (-0xa00 < W[0x0D04] - R[0x35BA0])) {
      iVar8 = W[0x0D00] - (int)g_sys.rom[0x35B9C];
      if (iVar8 < 0) {
        iVar8 = -iVar8;
      }
      iVar2 = W[0x0D08] - (int)g_sys.rom[0x35BA4];
      if (iVar2 < 0) {
        iVar2 = -iVar2;
      }
      if (iVar2 + iVar8 < 0x1000) {
        W16_SET(0x15F5C, 200);
        sound_play(0x08);
      }
    }
    if (W[0x0D24] != 0x23) {
      return 0x23;
    }
    uVar4 = W[0x0D04] - R[0x35BB0];
    if (0x9ff < (int)uVar4) {
      return uVar4;
    }
    if ((int)uVar4 < -0x9ff) {
      return uVar4;
    }
    iVar8 = W[0x0D00] - (int)g_sys.rom[0x35BAC];
    if (iVar8 < 0) {
      iVar8 = -iVar8;
    }
    iVar2 = W[0x0D08] - (int)g_sys.rom[0x35BB4];
    if (iVar2 < 0) {
      iVar2 = -iVar2;
    }
    if (0xfff < iVar2 + iVar8) {
      return uVar4;
    }
  }
  else {
    if (W[0x0E0C] - 2U != 0) {
      return W[0x0E0C] - 2U;
    }
    if ((W[0x0D24] != 0x15) && (W[0x0D24] != 0xd)) {
      return 0xd;
    }
    uVar4 = W[0x0D04] - R[0x35BC0];
    if (0x9ff < (int)uVar4) {
      return uVar4;
    }
    if ((int)uVar4 < -0x9ff) {
      return uVar4;
    }
    iVar8 = W[0x0D00] - (int)g_sys.rom[0x35BBC];
    if (iVar8 < 0) {
      iVar8 = -iVar8;
    }
    iVar2 = W[0x0D08] - R[0x35BC4];
    if (iVar2 < 0) {
      iVar2 = -iVar2;
    }
    if (0x3fff < iVar2 + iVar8) {
      return uVar4;
    }
  }
  W16_SET(0x15F5C, 200);
  uVar4 = sound_play(0x1C);
  return uVar4;
}


/* ---- stage_transition_run ---- */

int stage_transition_run(void)

{
  int iVar1;
  int iVar2;
  undefined2 uVar3;
  
  W[0x4704] = 0;
  W[0x169E8] = 0;
  if (W[0x16648] < 0x21c) {
    iVar2 = W[0xEB16] + -8;
    if (iVar2 < 0) {
      iVar2 = 0;
    }
    W[0xEB16] = (short)iVar2;
    stage_camera_interpolate();
    if (0x1a7 < W[0x16648]) {
      stage_transition_fade((short)W[0x16648] + -0x1a8);
    }
  }
  else {
    if (W[0x16648] == 0x21c) {
      g_fog_b = 0xff;
      g_fog_g = 0xff;
      g_fog_r = 0xff;
      W[0xEB16] = 0;
      g_fog_mode = 1;
    }
    stage_transition_whiteout();
  }
  iVar2 = W[0x0D48];
  if (0x3800 < W[0x0D48]) {
    W[0x0D48] = 0x3800;
  }
  stage_camera_path_update();
  W[0x0D48] = iVar2;
  if (W[0x16648] == 0) {
    sound_play(0x32);                 /* ROM 0x01369E `move.w #$32` */
  }
  if (W[0x16648] == 0x21c) {
    sound_play(0x33);                 /* ROM 0x0136B0 `move.w #$33` */
  }
  if (W[0x16648] == 0xf5) {
    sound_play(0x3);                  /* ROM 0x0136C2 `move.w #$3` */
  }
  if (0x365 < W[0x16648]) {
    FUN_0000f66a(0x33, (short)((W[0x16648] * 0xff + -0x3629a) / 0x1e));   /* ROM 0x0136EE `move.w #$33` */
  }
  if (W[0x16648] < 0xdc) {
    uVar3 = 0;
  }
  else {
    iVar2 = (W[0x16648] * -5 + 0x44c) / 0x2a8 + 6;
    iVar1 = W[0x0D48] / 0x600;
    if ((iVar2 < iVar1) && (iVar2 = iVar1, 6 < iVar1)) {
      iVar2 = 6;
    }
    uVar3 = (undefined2)iVar2;
  }
  tilemap_wind_flash_update(uVar3);
  W[0x16648] = W[0x16648] + 1;
  W[0x16968] = 900 - W[0x16648];
  iVar2 = W[0x16968] / 0x3c + -3;
  if (W[0x16968] / 0x3c < 3) {
    iVar2 = sprite_draw_2d(0,W[0x16968] / 0x3c + 0x153,0xf0,0x90,0,0x40,0x40,0,0);
  }
  if ((W[0x16968] < 0xb5) && (iVar2 = W[0x16968] % 0x3c, iVar2 == 0)) {
    iVar2 = sound_play_p(CONCAT22(0x0A,(short)(W[0x16968] / -0x3c) + 0x86));
  }
  if (W[0x16968] < 1) {
    player_model_load_animation(0);
    W[0xEB06] = 1;
    W[0x0E18] = 0;
    iVar2 = W[0x0CBC] + -3;
    if (iVar2 == 0) {
      iVar2 = 3;
      W[0x0CC0] = 3;
    }
    else {
      W[0x0CC0] = 0;
    }
  }
  return iVar2;
}


/* ---- stage_camera_interpolate ---- */
/* ROM 0x01298C -- the FINAL STAGE's fly-in (course 3, gameplay sub 19: the
 * story's day 4 goes through stage_transition_init instead of the intro
 * orbit). Rewritten from the disassembly, register row 185. The transpile
 * could not survive a single frame of it:
 *   - both node lists were loaded from NULL (ROM: MEM32[0x123C38] and
 *     MEM32[0x123C3C], `move.l $123c38.l,-(a7)` at 0x0129D2);
 *   - the six-long node initialisers at 0x360A8/0x360C0, the -$20/-$1c/-$18
 *     vector off 0x35D2C/0x35D38 and the path table at a4 = 0x35D7C were read
 *     natively (rom_nr32) or one BYTE at a time (R[]) where every access is a
 *     `move.l` of a big-endian long;
 *   - the look-at target block was three HOST POINTERS (&R[0x1EBB0] ...)
 *     where 0x012F20 has the immediates #$2b0b0/#$1ebb0/#$8651b;
 *   - the spring force vector -$14/-$10/-$c was three separate C locals
 *     handed over by the address of the first;
 *   - dsp_cmd_emit_arrow_indicator got VALUES where the ROM passes the
 *     addresses of 3-long positions -- the SIGSEGV;
 *   - scene_objects_draw_list / the two strip draws read ROM tables through
 *     native host pointers.
 * 0x16650 and 0x1665C are 16-bit (`addq.w #1,$e16650`, `move.w d0,$e1665c`,
 * `muls.w`/`suba.w` reads). */

extern intptr_t anim_load_list(intptr_t a1, uint32_t d1);
extern int32_t *dsp_arrow_emit(const int32_t *a, const int32_t *b, int32_t *out);
int physics_spring_update_general(int p1, int p2, int p3, int p4, const int32_t *vec);
void camera_lookat_interpolate(int rate, const int32_t *pos);

static int32_t sci_mul(int32_t a, int32_t b) { return (int32_t)((int64_t)a * b); }

/* 0x012B12..0x012B82 and 0x012C20..0x012D02: a flight speed from a velocity
 * triple (a, b, c) -- the same shape twice, the divisor scaled (cos*60)>>7
 * for W[0x0D48] and cos>>3 for W[0x0D88]. */
static int32_t sci_speed(int32_t a, int32_t b, int32_t c, int mul60)
{
  uint32_t h = math_atan2(a, b);
  int32_t s = (int32_t)vrd16s(0x20B004 + (h & 0xfffc)) >> 4;
  int32_t d2 = m68k_divs((int32_t)((uint32_t)a << 11), s);
  uint32_t g = math_atan2(c, d2);
  int32_t cc = (int32_t)vrd16s(0x20B006 + ((g - 0x8000u) & 0xfffc));
  int32_t den = mul60 ? (sci_mul(cc, 60) >> 7) : (cc >> 3);
  int32_t q = d2 / 4;                                   /* bpl ; addq #3 ; asr #2 */
  return m68k_divs(-(int32_t)((uint32_t)q << 12), den);
}

/* scene_objects_draw_list (ROM 0x02B088) on ROM addresses: a 0xFFFF-terminated
 * BE16 cell list at `list`, the block at `pos` = (x, y, z, model base). */
static void sci_cell_list(uint32_t list, uint32_t pos)
{
  int32_t *dl;
  uint32_t c;
  int n = 0;
  dsp_cmd_emit_object_mode_8000();
  dl = (int32_t *)W[0x0CA4];
  for (; (c = vrd16(list)) != 0xffff && n < 1024; list += 2, n++) {
    *dl++ = (int32_t)c + vrd32s(pos + 12);
    *dl++ = (int32_t)(c & 7) * 0x18000 + 0xc000 + vrd32s(pos) - (int32_t)W[0x0CDC];
    *dl++ = vrd32s(pos + 4) - (int32_t)W[0x0CE0];
    *dl++ = (int32_t)(c >> 3) * 0x18000 + 0xc000 + vrd32s(pos + 8) - (int32_t)W[0x0CE4];
  }
  W[0x0CA4] = (intptr_t)dl;
}

/* The shared head of ROM 0x02B378 / 0x02B502: the 0x8002 header, viewport 3
 * on sub 15 else 0, and the zsort bias marker when it changes. */
static int32_t *sci_strip_head(int32_t shift)
{
  int32_t *dl = (int32_t *)W[0x0CA4];
  W[0x169E8] = 0x8002;
  *dl++ = 0x8002;
  *dl++ = (W[0x0CC0] == 0xf) ? 3 : 0;
  if (shift != (int32_t)W[0x4704]) {
    *dl++ = 0x8010;
    *dl++ = 3;
    W[0x4704] = shift;
    *dl++ = shift;
    *dl++ = -1;
  }
  return dl;
}

/* ending_object_strip_draw (ROM 0x02B378) on a ROM record list: 5 longs per
 * record (model, x, y, z, angle), ended by a negative model. The model cycles
 * through 8 frames on `frame`; the angle's two special values are recovered
 * from the jump table at 0x02B43A (capstone shows it as code): -1 turns the
 * record to face the PLAYER (atan2 of player - record, pulled to the heading
 * word 0xE16670 when more than 0x1800 away), -2 faces the CAMERA
 * (W[0x16668]/W[0x1666C]). The pair is (sin, cos) -- `move.l (a0) ;
 * move.l $2(a0)` off 0x20B002, register row 87. */
static void sci_strip(uint32_t rec, int32_t frame, int32_t shift)
{
  int32_t *dl = sci_strip_head(shift);
  int32_t H = (int16_t)W[0x16670];
  int n = 0;
  while (vrd32s(rec) >= 0 && n++ < 256) {
    int32_t a = vrd32s(rec + 16);
    frame += 8;
    *dl++ = ((frame >> 3) & 7) + vrd32s(rec);
    *dl++ = vrd32s(rec + 4)  - (int32_t)W[0x0CDC] + (int32_t)W[0x3EF0];
    *dl++ = vrd32s(rec + 8)  - (int32_t)W[0x0CE0] + (int32_t)W[0x3EF4];
    *dl++ = vrd32s(rec + 12) - (int32_t)W[0x0CE4] + (int32_t)W[0x3EF8];
    *dl++ = 0;
    *dl++ = 0x7fff;
    if (a == -2) {                                      /* 0x02B4BC */
      *dl++ = (int32_t)W[0x16668];
      *dl++ = (int32_t)W[0x1666C];
    } else {
      uint32_t idx;
      if (a == -1) {                                    /* 0x02B43C */
        int32_t d2 = (uint16_t)(0x8000 - math_atan2((int32_t)W[0x0D00] - vrd32s(rec + 4),
                                                    (int32_t)W[0x0D08] - vrd32s(rec + 12)));
        int use_h = 0;
        if (d2 > H) {
          if (d2 - H > 0x1800 && H - d2 + 0x10000 > 0x1800) use_h = 1;
        } else if (d2 < H) {
          if (H - d2 - 0x10000 < -0x1800 && d2 - H < -0x1800) use_h = 1;
        }
        if (use_h) d2 = H;
        idx = (uint16_t)d2 & 0xfffc;
      } else {
        idx = (uint32_t)a & 0xfffc;
      }
      *dl++ = vrd16s(0x20B004 + idx);
      *dl++ = vrd16s(0x20B006 + idx);
    }
    *dl++ = 0;
    *dl++ = 0x7fff;
    *dl++ = 4;
    rec += 20;
  }
  W[0x0CA4] = (intptr_t)dl;
}

/* ending_object_strip_draw_anim (ROM 0x02B502): the model is base + the BE16
 * at 0x37B98 + ((frame >> 3) mod count)*2 -- `divsl.l $10(a3),d0:d1` then
 * `movea.w $37b98(d0.l*2)` (register row 172's table) -- and every record
 * faces the camera. */
static void sci_strip_anim(uint32_t rec, int32_t frame, int32_t shift)
{
  int32_t *dl = sci_strip_head(shift);
  int32_t rem = 0;
  int n = 0;
  while (vrd32s(rec) >= 0 && n++ < 256) {
    int32_t cnt = vrd32s(rec + 16);
    frame += 8;
    if (cnt != 0) rem = (frame >> 3) % cnt;             /* a zero divisor: rte, d0 kept */
    *dl++ = vrd32s(rec) + vrd16s(0x37B98 + rem * 2);
    *dl++ = vrd32s(rec + 4)  - (int32_t)W[0x0CDC];
    *dl++ = vrd32s(rec + 8)  - (int32_t)W[0x0CE0];
    *dl++ = vrd32s(rec + 12) - (int32_t)W[0x0CE4];
    *dl++ = 0;
    *dl++ = 0x7fff;
    *dl++ = (int32_t)W[0x16668];
    *dl++ = (int32_t)W[0x1666C];
    *dl++ = 0;
    *dl++ = 0x7fff;
    *dl++ = 4;
    rec += 20;
  }
  W[0x0CA4] = (intptr_t)dl;
}

int stage_camera_interpolate(void)

{
  int32_t v[3];            /* -$20 / -$1c / -$18 */
  int32_t t[3];            /* -$14 / -$10 / -$c  */
  int32_t mode, k, i;

  if (W[0x16648] == 0) {                                /* 0x0129CC */
    intptr_t n2;
    W[0x17314] = (intptr_t)&W[0xB704];
    n2 = anim_load_list(0xB704, vrd32(0x123C38));
    W[0x17310] = (intptr_t)&W[n2];
    anim_load_list(n2, vrd32(0x123C3C));
    for (k = 0; k < 6; k++) W[0xB704 + 0x24 + k * 4] = vrd32s(0x360A8 + k * 4);
    for (k = 0; k < 6; k++) W[n2 + 0x24 + k * 4] = vrd32s(0x360C0 + k * 4);
  }
  if (W[0x16648] == 0xf0) W[0x1664C] = 1;
  v[0] = vrd32s(0x35D2C) + vrd32s(0x35D38) - 2 * (int32_t)W[0x1645C];
  v[1] = vrd32s(0x35D30) + vrd32s(0x35D3C) - 2 * (int32_t)W[0x16460];
  v[2] = vrd32s(0x35D34) + vrd32s(0x35D40) - 2 * (int32_t)W[0x16464];

  mode = (int32_t)W[0x1664C];
  if (mode == 0) {                                      /* 0x012E0A */
    W[0x0D00] = (int32_t)W[0x1645C];
    W[0x0D04] = (int32_t)W[0x16460];
    W[0x0D08] = (int32_t)W[0x16464];
    W[0x0D10] = (int32_t)math_atan2(v[0], v[2]);
    W[0x0D0C] = 0;
    W[0x0D14] = 0;
  } else {
    if (mode == 1) {                                    /* 0x012AA2 */
      int32_t a1 = sci_mul((int32_t)W[0x16450] - (int32_t)W[0x1645C], 0xd00) +
                   sci_mul((int32_t)W[0x16468] - (int32_t)W[0x1645C], 0xd00);
      int32_t d1 = sci_mul((int32_t)W[0x1660C], 168);
      a1 = a1 / 128 + (d1 / 4) / -128;
      if (a1 > 0) {
        W[0x1664C] = 2;
        W[0x12BC] = 1;
        W16_SET(0x1665C, (int16_t)W[0x16648]);
        W[0x0D48] = sci_speed((int32_t)W[0x1660C], (int32_t)W[0x16614], (int32_t)W[0x16610], 1);
        W[0x16654] = (int32_t)W[0x0D0C];
        W[0x16658] = 0;
      }
    }
    t[0] = 0;                                           /* 0x012B94 */
    t[1] = (W[0x1664C] == 1)
           ? (sci_mul((int32_t)W[0x0D48], 0x993a) - 0x993a000) / 0x1000 : 0;
    t[2] = 0;
    W[0x16498] = (int32_t)W[0x1642C];
    W[0x1649C] = (int32_t)W[0x16430];
    W[0x164A0] = (int32_t)W[0x16434];
    for (i = 1; i < 8; i++)
      physics_spring_update_general(i, (i == 4 && W[0x1664C] < 2) ? 0x80 : 4, 0xd00, 0xd00, t);

    if (W[0x1664C] == 1) {                              /* 0x012C20 */
      W[0x0D48] = sci_speed((int32_t)W[0x1660C], (int32_t)W[0x16614], (int32_t)W[0x16610], 1);
      W[0x0D88] = sci_speed((int32_t)W[0x16384], (int32_t)W[0x1638C], (int32_t)W[0x16388], 0);
      W[0x0D00] = (int32_t)W[0x1645C];
      W[0x0D04] = (int32_t)W[0x16460];
      W[0x0D08] = (int32_t)W[0x16464];
      W[0x0D10] = (int32_t)math_atan2(v[0], v[2]);
      {
        int32_t s = (int32_t)vrd16s(0x20B004 + ((uint32_t)W[0x0D10] & 0xfffc)) >> 4;
        int32_t p = (int32_t)math_atan2(v[1], m68k_divs((int32_t)((uint32_t)v[0] << 11), s));
        W[0x0D0C] = (int32_t)W[0x0D0C] + ((p - (int32_t)W[0x0D0C] - 0x8000) >> 4);
      }
      W[0x0D14] = 0;                                    /* 0x012E30 */
    } else {                                            /* 0x012D6C */
      int32_t d1, d0;
      W[0x0D88] = 0;
      d1 = m68k_divs((int16_t)W16(0x16650) * 0xd00, 0x21c - (int16_t)W16(0x1665C)) + 0x300;
      d0 = sci_mul(0x3000 - (int32_t)W[0x0D0C], d1) + sci_mul((int32_t)W[0x16658], -0xc0);
      W[0x16658] = (int32_t)W[0x16658] + d0 / 256;
      W[0x0D0C] = (int32_t)W[0x0D0C] + ((int32_t)W[0x16658] >> 10);
      spherical_to_cartesian((uint32_t)W[0x0D0C], (int)W[0x0D10], (int32_t)W[0x0D48] >> 4,
                             &v[0], &v[1], &v[2]);
      W[0x0D00] = (int32_t)W[0x0D00] + v[0];
      W[0x0D04] = (int32_t)W[0x0D04] + v[1];
      W[0x0D08] = (int32_t)W[0x0D08] + v[2];
    }
  }

  world_grid_calc_position();                           /* 0x012E34 */
  if ((int32_t)W[0x0D00] > 0x19ff0) {
    world_stage_update();
    world_render_objects();
    world_render_props();
  } else {
    W[0x0E38] = 1;
    W[0x2A90] = 0;
    W[0x2A9C] = 0;
    W[0x2A84] = 0;
    W[0x2A78] = 0;
    W[0x2AA8] = 0;
  }
  /* 0x012E7E: the look-at target -- the path table a4 = 0x35D7C, two 3-long
   * points, lerped along x past 0x2D312. */
  if ((int32_t)W[0x0D00] > 0x2d312) {
    int32_t f = ((int32_t)W[0x0D00] - 0x520f3) >> 4;
    for (k = 0; k < 3; k++) {
      int32_t p0 = vrd32s(0x35D7C + k * 4), p1 = vrd32s(0x35D88 + k * 4);
      t[k] = p0 + sci_mul(p1 - p0, f) / -0x24df;
    }
  } else {
    for (k = 0; k < 3; k++) t[k] = vrd32s(0x35D88 + k * 4);
  }
  if (W[0x16648] == 0) {                                /* 0x012F10 */
    W[0x0CDC] = vrd32s(0x35D7C);
    W[0x0CE0] = vrd32s(0x35D80);
    W[0x0CE4] = vrd32s(0x35D84);
    t[0] = 0x2b0b0;
    t[1] = 0x1ebb0;
    t[2] = 0x8651b;
    camera_lookat_interpolate(1, t);
  } else {
    W[0x0CDC] = (int32_t)W[0x0CDC] + ((t[0] - (int32_t)W[0x0CDC]) >> 3);
    W[0x0CE0] = (int32_t)W[0x0CE0] + ((t[1] - (int32_t)W[0x0CE0]) >> 3);
    W[0x0CE4] = (int32_t)W[0x0CE4] + ((t[2] - (int32_t)W[0x0CE4]) >> 3);
    if (W[0x16648] >= 0x3c) {
      int32_t rate, p[3];
      rate = (W[0x1664C] != 0) ? 4 : (sci_mul(-0x40, (int32_t)W[0x16648]) + 0xf00) / 0xb4 + 0x48;
      p[0] = (int32_t)W[0x0D00];
      p[1] = (int32_t)W[0x0D04];
      p[2] = (int32_t)W[0x0D08];
      camera_lookat_interpolate(rate, p);
    }
  }
  camera_grid_calc_position();                          /* 0x012FA4 */
  dsp_viewport_setup(0, 2);
  sci_cell_list(0x35D54, 0x35D44);
  terrain_props_dispatch();
  W[0x0E0C] = 0;
  player_model_set_pose();
  W[0x2C04] = (W[0x1664C] == 0) ? 0 : 8;
  camera_setup_extended(0, 0xffffff00, 0, 0, -((int32_t)W[0x0D88] >> 9));
  hud_draw_wings();
  /* ROM 0x013016..0x01306E (a3 = 0xE1642C): per i, an arrow from the long
   * triple at a3+i*12 to the one at a3+0xC+i*12, and another from
   * a3+0x54-i*12 to a3+0x60-i*12 -- ADDRESSES, `pea (a3,d0.l)`. */
  for (i = 0; i < 4; i++) {
    int32_t a[3], b[3];
    for (k = 0; k < 3; k++) {
      a[k] = (int32_t)W[0x1642C + i * 12 + k * 4];
      b[k] = (int32_t)W[0x16438 + i * 12 + k * 4];
    }
    W[0x0CA4] = (intptr_t)dsp_arrow_emit(a, b, (int32_t *)W[0x0CA4]);
    for (k = 0; k < 3; k++) {
      a[k] = (int32_t)W[0x16480 - i * 12 + k * 4];
      b[k] = (int32_t)W[0x1648C - i * 12 + k * 4];
    }
    W[0x0CA4] = (intptr_t)dsp_arrow_emit(a, b, (int32_t *)W[0x0CA4]);
  }
  W[0x16670] = (int16_t)(-(int32_t)W[0x0CEC]);          /* 0x013078 move.w */
  W[0x16668] = (int)(vrd16s(0x20B004 + ((uint32_t)(uint16_t)W[0x16670] & 0xfffc)));
  W[0x1666C] = (int)(vrd16s(0x20B006 + ((uint32_t)(uint16_t)W[0x16670] & 0xfffc)));
  W[0x1257] = 4;
  render_windmill();
  sci_strip(0x35D94, (int32_t)W[0x16648], (int32_t)0xffff0000);
  sci_strip_anim(0x35E4C, (int32_t)W[0x16648], (int32_t)0xfffe0000);
  W[0x169E8] = 0x8002;
  {
    int32_t *dl = (int32_t *)W[0x0CA4];
    *dl++ = 0x8002;
    *dl++ = 0;
    W[0x0CA4] = (intptr_t)dl;
  }
  {
    intptr_t nb;
    int guard = 0;
    for (nb = 0xB704; (int32_t)W[nb] >= 0 && guard++ < 64; nb += 0x80)
      scene_node_render((int *)&W[nb]);
  }
  if (W[0x1664C] == 2) W16_SET(0x16650, (int16_t)(W16(0x16650) + 1));
  { extern int g_stagedbg;
    if (g_stagedbg && (W[0x16648] % 20) == 0)
      fprintf(stderr, "[FLYIN] t=%ld mode=%ld ply=(%d,%d,%d) pitch=%d hdg=%d spd=%d cam=(%d,%d,%d) chdg=%d cpitch=%d p1=(%d,%d,%d)\n",
              (long)W[0x16648], (long)W[0x1664C], (int32_t)W[0x0D00], (int32_t)W[0x0D04], (int32_t)W[0x0D08],
              (int32_t)W[0x0D0C], (int32_t)W[0x0D10], (int32_t)W[0x0D48],
              (int32_t)W[0x0CDC], (int32_t)W[0x0CE0], (int32_t)W[0x0CE4], (int32_t)W[0x0CEC], (int32_t)W[0x0CE8],
              (int32_t)W[0x16438], (int32_t)W[0x1643C], (int32_t)W[0x16440]); }
  return (int32_t)W[0x1664C] - 2;
}

/* ---- stage_transition_fade ---- */

void stage_transition_fade(int param_1)

{
  int iVar1;
  int iVar2;
  
  if (param_1 < 0x21) {
    if (param_1 < 0x20) {
      if (param_1 % 4 == 0) {
        iVar2 = param_1;
        if (param_1 < 0) {
          iVar2 = param_1 + 3;
        }
        iVar1 = param_1;
        if (param_1 < 0) {
          iVar1 = param_1 + 3;
        }
        cgram_load_tile_block((short)(iVar1 >> 2) + 0x113, ((uint8_t)(iVar2 >> 2) & 1) * 4 + 0x1a0)
        ;
        if (param_1 < 0) {
          param_1 = param_1 + 3;
        }
        /* ROM 0x013564..0x013572: palette = ((param_1/4) & 1) + 7, a BYTE push the
           decompiler lost (register row 118) -- it alternates 7/8 with the tile bank */
        cz_load_color_ramp((short)(param_1 >> 2) + 0x113, (uint8_t)(((iVar2 >> 2) & 1) + 7));
      }
      else {
        iVar2 = param_1;
        if (param_1 < 0) {
          iVar2 = param_1 + 3;
        }
        if (param_1 < 0) {
          param_1 = param_1 + 3;
        }
        /* param_4 (base tile code) and param_5 (palette) recovered from the
         * M68K at 0x0135D8:
         *   01359C: move.l D1,-(A7)        D1 = 8 - ((D2>>2)&1)   -> palette
         *   0135B4: move.w D0,-(A7)        D0 = 0x1a0 + ((D2>>2)&1)*4
         *   ... pea ($6,A0) / ($12,A0) / ($113,A0) ; lea ($12,A7),A7
         * The 0x12 frame pop confirms the WORD push for param_4. Ghidra had
         * collapsed the two into `(short)(8 - (iVar2 >> 2 & 1U) >> 0x10)`,
         * which is the PALETTE expression shifted right 16 -- always 0 -- with
         * the palette itself replaced by a literal 0. */
        text_draw_rect_blink
                  ((short)(param_1 >> 2) + 0x113,(short)W[0x3EF0] + 0x12,(short)W[0x3EF4] + 6,
                   0x1a0 + (int)(iVar2 >> 2 & 1U) * 4, 8 - (int)(iVar2 >> 2 & 1U));
      }
    }
  }
  else {
    text_draw_rect_solid(0x11a,(short)W[0x3EF0] + 0x12,(char)W[0x3EF4] + '\x06');
  }
  return;
}

/* ---- stage_transition_whiteout ---- */

void stage_transition_whiteout(void)

{
  int iVar1;
  short sVar2;
  int local_1c [6];
  
  if (W[0x16648] == 0x21c) {
    W[0x0C8C] = (int32_t)0xfffffe98;
    W[0x16660] = 0;
    W[0x12BC] = 0;
    W[0x0E0C] = 3;
    /* ROM 0x0131A6..0x013200: the player's placement for the stage, from the
     * per-course BE32 tables at 0xB85B8 (+0x10/+0x20/+0x40/+0x50), indexed
     * course*4 (`(a0,d0.l*4)`: capstone drops the scale). The transpile read
     * two natively, three as ONE BYTE, and made 0x2E40/0x2800 host pointers
     * -- the final stage started ~4e8 units out of the world (row 185). */
    {
      uint32_t c4 = (uint32_t)W[0x0E0C] * 4;
      W[0x0D00] = vrd32s(0xB85B8 + c4);
      W[0x0D04] = vrd32s(0xB85C8 + c4) - 0x1529a;
      W[0x0D08] = vrd32s(0xB85D8 + c4) - 0x2b23b;
      W[0x0D0C] = 0x2e40;
      W[0x0D10] = vrd32s(0xB85F8 + c4);
      W[0x0D14] = vrd32s(0xB8608 + c4);
    }
    W[0x0D48] = 0x3800;
    W[0x16658] = 0x2800;
    world_props_init_dispatch(3);
    W[0x0E38] = 0;
    W[0x1F8C] = 0x99;
  }
  debug_profiler_mark();
  objects_move_update();
  W[0x0E04] = W[0x0E00];
  W[0x0E00] = ((900 - W[0x16648]) * 0x2000) / 0x168 + 0x800;
  if (W[0x0E00] < W[0x0D48] >> 2) {
    W[0x0E00] = W[0x0D48] >> 2;
  }
  W[0x0E08] = W[0x0E08] - W[0x0E00];
  iVar1 = (int)W[0x16658] * -0xc0 +
          ((W[0x16648] * 0xd00 + -0x1b6c00) / 0x168 + 0x300) *
          (vrd32s(0xB85E8 + (uint32_t)W[0x0E0C] * 4) - (int)W[0x0D0C]);   /* 0x013278 */
  if (iVar1 < 0) {
    iVar1 = iVar1 + 0xff;
  }
  W[0x16658] = W[0x16658] + (iVar1 >> 8);
  if ((0x2d0 < W[0x16648]) && (-0x81 < (int)W[0x16658])) {
    W[0x16658] = -0x80;
  }
  W[0x0D0C] = W[0x0D0C] + ((int)W[0x16658] >> 10);
  if ((int)W[0x0D0C] < 0) {
    W[0x0D0C] = 0;
  }
  W[0x0D84] = W[0x0D48] * W[0x0D48] >> 0x16;
  if (W[0x0D84] < 1) {
    W[0x0D84] = 1;
  }
  W[0x0D88] = -(W[0x0D8C] + W[0x0D84]);
  W[0x0D48] = W[0x0D48] - W[0x0D84];
  if (W[0x0D48] < 0xe9e) {
    W[0x0D48] = 0xe9e;
  }
  if (W[0x16648] < 0x348) {
    W[0x2C04] = ((900 - W[0x16648]) * 0x2000) / 0x168;
    W[0x16660] = W[0x2C04] + W[0x16660] & 0xffff;
  }
  else if (W[0x16660] != 0) {
    W[0x2C04] = ((900 - W[0x16648]) * 0x2000) / 0x168;
    W[0x16660] = W[0x2C04] + W[0x16660];
    if (0xffff < (int)W[0x16660]) {
      W[0x16660] = 0;
    }
  }
  W[0x2C04] = W[0x2C04] >> 10;
  W[0x0D98] = (int)(W[0x16660] << 6) / 0x10000;
  W[0x0D94] = W[0x0D98] * 10;
  spherical_to_cartesian
            (W[0x0D0C],W[0x0D10],W[0x0D48] >> 4,local_1c,local_1c + 1,local_1c + 2);
  W[0x0D00] = W[0x0D00] + local_1c[0];
  W[0x0D04] = local_1c[1] + W[0x0D04];
  W[0x0D08] = W[0x0D08] + local_1c[2];
  world_grid_calc_position();
  debug_profiler_mark();
  sVar2 = 0;
  do {
    /* ROM 0x013404: six BE32 longs at 0x360D8 -> 0x360F0 (`(d4.w*4)`). */
    local_1c[sVar2] =
         vrd32s(0x360D8 + sVar2 * 4) +
         (int32_t)((int64_t)((int32_t)W[0x16648] - 0x21c) *
                   (vrd32s(0x360F0 + sVar2 * 4) - vrd32s(0x360D8 + sVar2 * 4))) / 0x168;
    sVar2 = sVar2 + 1;
  } while (sVar2 < 6);
  camera_offset_apply(0,local_1c);
  camera_grid_calc_position();
  dsp_viewport_setup(0,2);
  debug_profiler_mark();
  terrain_chunk_visibility(W[0x0CDC],W[0x0CE0],W[0x0CE4],W[0x0CEC]);
  debug_profiler_mark();
  player_model_set_pose();
  camera_setup_extended(W[0x16660],0x100,0,0,0);
  debug_profiler_mark();
  terrain_props_dispatch();
  debug_profiler_mark();
  FUN_0000e016();
  debug_profiler_mark();
  objects_render_master();
  debug_profiler_mark();
  balloon_render_and_hit_check();
  debug_profiler_mark();
  return;
}

/* ---- state_gameplay_init ---- */

void state_gameplay_init(void)

{
  W[0x0CBC] = 3;
  /* THE ROM'S OWN VALUE. 0x00A9F2 is `moveq #$14,D0 ; move.l D0,$E00CC0` -- 20,
   * the CONTROLS tutorial, which chains 20->21->12->13 (MODE SELECT) ->0->1
   * (STAGE SELECT) ->2. This line used to read 2 with the comment "Skip
   * countdown (0x14), go straight to gameplay_sub_init", which bypassed the
   * entire pre-gameplay sequence -- verified on the machine with
   * tools/overnight/snap_menu.lua. */
  W[0x0CC0] = 0x14;
  W16_SET(0xE2C, 0xf);
  W16_SET(0xE64, 0);
  W16_SET(0xE66, 0);
  W[0x0E54] = 0;
  W[0x0E58] = 0;
  W[0x0E44] = 0;
  W[0x12BC] = 0;
  W16_SET(0xE6A, 0);
  W16_SET(0xE12, 0);
  W16_SET(0xE14, 0);
  gameplay_vars_init();
  /* Escape -> Levels / PROPCYCL_LEVEL (src/level_select.c): when a stage was
   * asked for directly, put the story variables where that day has them and
   * enter the DAY screen (ADVANCED) or the STAGE SELECT (NOVICE) instead of
   * the CONTROLS tutorial. A no-op otherwise. */
  { extern int level_select_apply(void); level_select_apply(); }
  if (propcycl_verbose()) printf("[GAMEPLAY] Init complete, entering gameplay_sub_init (sub=2)\n");
  state_gameplay_run();
  return;
}

/* ---- state_stage_start_init ---- */

void state_stage_start_init(void)

{
  uint32_t uVar1;
  uint32_t uVar2;
  uint32_t uVar3;
  uint32_t uVar4;
  
  sync_reset();
  sync_post();
  uVar2 = W[0x0C98] >> 2;
  uVar3 = W[0x0C98] >> 4;
  uVar4 = W[0x0C98] >> 1;
  uVar1 = W[0x0C98] & 0xf;
  W[0xEB16] = 0;
  tilemap_enable_set();
  sound_stop_all();
  W_SET_HI16(0xEB20, 0);   /* clr.w $e0eb20: 16-bit, the HIGH half */
  set_background_color
            ((uVar2 & 0xf) << 3,(((uint8_t)uVar4 & 3) + ((uint8_t)uVar3 & 0xd)) * '\b',uVar1 << 3);
  W[0x2C24] = 0;
  W[0x0CBC] = 5;
  g_stage_timer = 0;
  return;
}

/* ---- state_stage_start_run ---- */

void stage_start_debug_init(void);
uint16_t stage_start_debug_camera(void);

void state_stage_start_run(void)

{
  /* Stage start sub-state dispatch (ROM table at 0x3523C).
   * Debug mode only has cases 0 and 1. After init sets g_stage_timer=7,
   * advance to gameplay since scene loading is complete. */
  switch ((short)g_stage_timer) {
    case 0: stage_start_debug_init(); break;
    case 1: stage_start_debug_camera(); break;
    default:
      /* Scene is loaded, advance to gameplay */
      if (propcycl_verbose()) printf("[STAGE] Stage start complete (timer=%d), entering gameplay\n",
             (int)(short)g_stage_timer);
      W[0x0CBC] = 2;  /* gameplay_init */
      break;
  }
  return;
}

/* ---- stage_render_environment ---- */

void stage_render_environment(void)

{
  uint16_t uVar1;
  uint16_t uVar2;
  short sVar3;
  int *piVar4;
  
  W[0x169E8] = 0;
  /* 0xE04708 is 16-bit, the HIGH half of its slot (`move.w (a3)`, `ori.w`,
   * `andi.w` at 0x015FBC..0x015FE2); the low half is the windmill counter. */
  if ((W_HI16(0x4708) & 0xe) == 0) {
    if (((int32_t)W[0x0D00] & 0xba) == 0) {
      W_SET_HI16(0x4708, W_HI16(0x4708) | 0xe);
    }
  }
  else if (((int32_t)W[0x0D00] & 0x1a) == 0) {
    W_SET_HI16(0x4708, W_HI16(0x4708) & 0xfff1);
  }
  /* stride 0x10 -- ROM 0x016042 `lea $10(a2),a2`. Was `int *piVar4`
   * walked with `+ 4` into an intptr_t[] array, i.e. 2 slots. */
  uVar2 = 0xff;
  W[0x291C] = 0;
  for (sVar3 = 0; (uint32_t)(int)sVar3 < g_visible_chunk_count; sVar3 = sVar3 + 1) {
    uVar1 = uVar2 & (short)(int8_t)R[0x36394 + (uint32_t)W[0x0E6C + (uint32_t)sVar3 * 0x10]];
    if (uVar1 != 0) {
      if ((uVar1 & 1) != 0) {
        render_castle_complex();
        uVar2 = uVar2 & 0xfffe;
      }
      if ((uVar1 & 2) != 0) {
        render_tower_complex();
        uVar2 = uVar2 & 0xfffd;
      }
      if ((uVar1 & 4) != 0) {
        render_bridge_structure();
        uVar2 = uVar2 & 0xfffb;
      }
    }
  }
  dsp_cmd_emit_object_mode_8002();
  return;
}

/* ---- stage_sky_sphere_draw ---- */

void stage_sky_sphere_draw(undefined4 param_1)

{
  uint32_t uVar1;
  undefined4 *puVar2;
  
  W[0x169E8] = 0;
  dsp_cmd_emit_object_mode_8000();
  if (W[0x4704] != 0x700) {
    *(int32_t*)W[0x0CA4] = 0x8010;
    ((int32_t*)W[0x0CA4])[1] = 3;
    W[0x4704] = 0x700;
    puVar2 = (int32_t*)W[0x0CA4] + 3;
    ((int32_t*)W[0x0CA4])[2] = 0x700;
    W[0x0CA4] = W[0x0CA4] + 4 * 4;
    *puVar2 = 0xffffffff;
  }
  *(int32_t*)W[0x0CA4] = 0x8008;
  ((int32_t*)W[0x0CA4])[1] = 3;
  ((int32_t*)W[0x0CA4])[2] = 1;
  ((int32_t*)W[0x0CA4])[3] = 0;
  ((int32_t*)W[0x0CA4])[4] = 0x7fff;
  uVar1 = (uint32_t)W[0x17392];
  ((int32_t*)W[0x0CA4])[5] = vrd16s(0x20B002 + ((uVar1 & 0x3f) * 0x400));
  ((int32_t*)W[0x0CA4])[6] = vrd16s(0x20B004 + ((uVar1 & 0x3f) * 1024));
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
  ((int32_t*)W[0x0CA4])[0x11] = W[0xAB7C + (param_1 * 0x20)];
  ((int32_t*)W[0x0CA4])[0x12] = 3;
  ((int32_t*)W[0x0CA4])[0x13] = 0x800a;
  ((int32_t*)W[0x0CA4])[0x14] = ((int)W[0x17392] >> 2 & 1U) + 0x30d;
  ((int32_t*)W[0x0CA4])[0x15] = 3;
  ((int32_t*)W[0x0CA4])[0x16] = W[0xEB08];
  ((int32_t*)W[0x0CA4])[0x17] = W[0xEB0C];
  ((int32_t*)W[0x0CA4])[0x18] = W[0xEB10];
  W[0x0CA4] = W[0x0CA4] + 0x19 * 4;
  return;
}

/* ---- stage_static_objects_draw ---- */

void stage_static_objects_draw(void)

{
  int *piVar1;
  int iVar2;
  
  W[0x169E8] = 0;
  dsp_cmd_emit_object_mode_8000();
  if (W[0x4704] != 0) {
    piVar1 = (intptr_t)((int32_t*)W[0x0CA4] + 1);
    *(int32_t*)W[0x0CA4] = 0x8010;
    W[0x0CA4] = W[0x0CA4] + 2 * 4;
    *piVar1 = -1;
    W[0x4704] = 0;
  }
  iVar2 = 0x444;
  do {
    switch(iVar2) {
    case 0x448:
    case 0x451:
    case 0x452:
    case 0x453:
    case 0x454:
    case 0x455:
    case 0x456:
      break;
    default:
      *(int32_t*)W[0x0CA4] = iVar2;
      ((int32_t*)W[0x0CA4])[1] = -W[0x0CDC];
      piVar1 = (intptr_t)((int32_t*)W[0x0CA4] + 3);
      ((int32_t*)W[0x0CA4])[2] = -W[0x0CE0];
      W[0x0CA4] = W[0x0CA4] + 4 * 4;
      *piVar1 = -W[0x0CE4];
    }
    iVar2 = iVar2 + 1;
  } while (iVar2 < 0x460);
  return;
}

/* ---- stage_water_objects_draw ---- */

void stage_water_objects_draw(void)

{
  /* ROM 0x0330DC -- THE INTERMISSION'S WHOLE BACKDROP, not just water: the
   * sky dome (models 0x28/0x27 at y = -0x20000, camera-attached), the Cliff
   * Rock terrain around the house -- course-0 chunks (base 0x495) for the
   * cells in the BE16 list at 0x39C04, terminated by a negative word -- the
   * 8-frame water surface (model 0x146 + the ping-pong table at 0x39C30,
   * `lea ([$39c30, d0.l*4], $146)`: a MEMORY-INDIRECT EA, i.e. the table
   * value plus 0x146), and one spinning 0x8002 prop (model 0x17A).
   *
   * The transpile walked the cell list through a native `uint16_t *`, so
   * every chunk id came back byte-swapped (0x0012 -> 0x1200) and the terrain
   * was never drawn -- the cut-scene played over the sky alone; it read the
   * water table and the prop's rotation pairs ONE BYTE at a time. The pairs
   * are row 87's `move.l (a3),(a1)+ ; move.l $2(a3),(a1)+` off trigbase-2:
   * the LOW halves are sin and cos. */
  int32_t *cp;
  uint32_t a3;
  int32_t cx = (int32_t)W[0x0CDC], cy = (int32_t)W[0x0CE0], cz = (int32_t)W[0x0CE4];
  int guard = 0;
  uint32_t ang;

  W[0x169E8] = 0;
  dsp_cmd_emit_object_mode_8000();
  cp = (int32_t *)W[0x0CA4];
  if (W[0x4704] != 0) {
    *cp++ = 0x8010;
    *cp++ = -1;
    W[0x4704] = 0;
  }
  *cp++ = 0x28; *cp++ = 0; *cp++ = (int32_t)0xfffe0000; *cp++ = 0;
  *cp++ = 0x27; *cp++ = 0; *cp++ = (int32_t)0xfffe0000; *cp++ = 0;
  for (a3 = 0x39C04; vrd16s(a3) >= 0 && guard++ < 128; a3 += 2) {    /* 0x03313A */
    int32_t w = vrd16s(a3);
    *cp++ = 0x495 + w;
    *cp++ = (w & 7) * 0x18000 + 0xC000 - cx - 0x58900;
    *cp++ = -0xAAE0 - cy;
    *cp++ = (w / 8) * 0x18000 + 0xC000 - cz - 0x8AB00;               /* asr with the +7 round */
  }
  *cp++ = (int32_t)vrd32(0x39C30 + ((W16(0x17392) >> 1) & 0xF) * 4) + 0x146;   /* 0x0331AE */
  *cp++ = -0x58900 - cx;
  *cp++ = -0xAAE0 - cy;
  *cp++ = -0x8AB00 - cz;
  W[0x169E8] = 0x8002;                                                 /* 0x0331DC */
  *cp++ = 0x8002;
  *cp++ = (int32_t)(int16_t)W[0x169F0];
  *cp++ = 0x17A;
  *cp++ = -0x25BF - cx;                                                /* lea $da41.w */
  *cp++ = -0x49D8 - cy;                                                /* lea $b628.w */
  *cp++ = -0x247D2 - cz;
  *cp++ = vrd16s(0x20B004 + 0x23C); *cp++ = vrd16s(0x20B006 + 0x23C);   /* angle 0x023C */
  *cp++ = vrd16s(0x20B004 + 0xF2B4); *cp++ = vrd16s(0x20B006 + 0xF2B4); /* angle 0xF2B4 */
  ang = ((uint32_t)((int32_t)W16(0x17392) * (int32_t)0xffff0060)) & 0xfffc;  /* 0x033238 */
  *cp++ = vrd16s(0x20B004 + ang); *cp++ = vrd16s(0x20B006 + ang);
  *cp++ = 4;
  W[0x0CA4] = (intptr_t)cp;
  return;
}

/* ---- gameplay_countdown_render ---- */

void gameplay_countdown_render(void)

{
  /* THE CONTROLS-TUTORIAL PROPS. Ghidra could not recover the jump table at
   * ROM 0x032C64 and emitted the whole body as `/* countdown render dispatch *\/`,
   * so the handlebar, the pedal-and-shoe and the two arrows were never drawn --
   * on screen, the tutorial ran with an empty sky. Same class as rows 65/85/94/
   * 96/97/104.
   *
   * ROM 0x032C34:
   *     movea.l #$23326,a2            ; a2 = sprite_draw_2d
   *     movea.l #$e17388,a1           ; a1 = &W[0x17388]  (sub-frame)
   *     moveq #$10,d0 ; cmp.l (a1),d0 ; bgt -> return
   *     move.l $e17384.l,d0           ; d0 = W[0x17384]   (phase)
   *     subq.l #1,d0 ; subq.l #5,d0 ; bhi -> return       ; phase 1..6
   *     move.w $32c6e(pc,d0.w*2),d0 ; nop ; jmp $32c64(pc,d0.w)
   *
   * capstone prints that EA without its scale; extension word 0x0212 has
   * bits10-9 = 01, i.e. **d0.w*2** (the trap CLAUDE.md records), so the six
   * word entries at 0x32C64 are indexed by phase-6 = -5..0:
   *     000C 0038 0064 00A2 00CC 0106  ->  0x32C70 0x32C9C 0x32CC8
   *                                        0x32D06 0x32D30 0x32D6A
   * Arguments read off the pushes (M68K pushes right-to-left, so the LAST
   * push before the `jsr (a2)` is param_1); every case shares the tail at
   * 0x32DA6, which pushes the layer 0 and calls. The frame pop is
   * `lea $24(a7),a7` = 9 longs, matching sprite_draw_2d's 9 parameters.
   *
   * Sizes 0x40 are a 2x zoom of the 32x32 tile; the 0x20 pair in phase 6 is
   * native size. W[0x17388] & 0x20 is the blink bit -- phases 1-4 alternate
   * their own prop with 0x194, the plain handlebar. */
  if (W[0x17388] < 0x10) return;
  switch (W[0x17384]) {
  case 1: {                                   /* 0x032C70 */
    int tile = (W[0x17388] & 0x20) ? 0x196 : 0x194;
    sprite_draw_2d(0, tile, 0xE0, 0xC0, 0, 0x40, 0x40, 0, 0);
    break; }
  case 2: {                                   /* 0x032C9C */
    int tile = (W[0x17388] & 0x20) ? 0x195 : 0x194;
    sprite_draw_2d(0, tile, 0xE0, 0xC0, 0, 0x40, 0x40, 0, 0);
    break; }
  case 3: {                                   /* 0x032CC8 */
    int tile = (W[0x17388] & 0x20) ? 0x198 : 0x194;
    int x    = (tile == 0x194) ? 0xE0 : 0xC1;
    sprite_draw_2d(0, tile, x, 0xC0, 0, 0x40, 0x40, 0, 0);
    break; }
  case 4: {                                   /* 0x032D06 */
    int tile = (W[0x17388] & 0x20) ? 0x197 : 0x194;
    sprite_draw_2d(0, tile, 0xE0, 0xC0, 0, 0x40, 0x40, 0, 0);
    break; }
  case 5: {                                   /* 0x032D30 */
    int tile = 0x1A8 - (((int)W[0x17388] >> 2) & 7);   /* asr.l #2 then and #7 */
    int x    = (tile == 0x1A3) ? 0xA2 : 0xA0;
    sprite_draw_2d(0, tile, x, 0xA0, 0, 0x40, 0x40, 0, 0);
    break; }
  case 6:                                     /* 0x032D6A -- two sprites */
    sprite_draw_2d(0, 0x1F1, 0x0A0, 0x90, 0, 0x20, 0x20, 0, 0);
    sprite_draw_2d(0, 0x1F2, 0x1A0, 0x90, 0, 0x20, 0x20, 0, 0);
    break;
  default: break;
  }
}

/* ---- gameplay_countdown_setup ---- */

void gameplay_countdown_setup(void)

{
  /* ROM 0x032908..0x032950. Both cz_load_color_ramp calls push a BYTE
   * argument that Ghidra dropped, and it picks the colour ramp:
   *     032920: move.b #$1,-(a7) ; pea $b8.w ; jsr $21b48
   *     032942: move.b #$5,-(a7) ; pea $f4.w ; jsr $21b48
   * Passing 0 for both loaded the wrong ramps under the tutorial's tiles.
   * Same lost-argument class as register row 62. */
  sync_post();
  cgram_load_tile_block(0xb8,0x120);
  cz_load_color_ramp(0xb8, 1);
  cgram_load_tile_block(0xb3,0x1b0);
  cgram_load_tile_block(0xcd,0x1e0);
  cz_load_color_ramp(0xf4, 5);
  return;
}

/* ---- gameplay_countdown_tick ---- */

void gameplay_countdown_tick(void)

{
  uint32_t uVar1;
  /* void */;
  int iVar2;
  undefined2 uVar3;
  undefined2 uVar4;
  undefined4 uVar5;
  
  /* Find which countdown threshold we're at.
   * ROM table at 0x398F4: 8 byte threshold values.
   * ROM table at 0x3991C: 8 x 4-byte display parameters per threshold.
   * Ghidra lost the 'foundIdx' variable — reconstructed here. */
  int foundIdx = 0;
  iVar2 = 0;
  do {
    /* ROM 0x03297C: `cmp.l $398f4(d4.l*4),d0` -- 32-bit BE entries on a
     * stride of 4, not one byte at stride 1. The table is
     * [0,60,240,420,600,780,960,1140]; read byte-wise it came out
     * [0,0,0,0,0,0,0,60], so foundIdx was almost always wrong AND the last
     * threshold (1140) never applied -- which is why our CONTROLS tutorial
     * lasted 61 frames where the machine gives it 1141. */
    if (vrd32s(0x398F4 + iVar2 * 4) <= (int)g_countdown_display) {
      foundIdx = iVar2;
    }
    iVar2 = iVar2 + 1;
  } while (iVar2 < 8);
  uVar1 = g_countdown_display - vrd32s(0x398F4 + foundIdx * 4);
  if (uVar1 == 0) {
    sync_post();
  }
  /* Read display parameter from ROM at 0x3991C + foundIdx*4 (32-bit BE) */
  if (foundIdx < 7) {
    int rom_off = 0x3991C + foundIdx * 4;
    int32_t display_param = 0;
    if (rom_off + 3 < ROM_SIZE)
      display_param = (R[rom_off]<<24)|(R[rom_off+1]<<16)|(R[rom_off+2]<<8)|R[rom_off+3];
    if (display_param >= 0) {
      if ((int)uVar1 < 0x10) {
        text_draw_number((short)display_param, 0x160, uVar1);
        /* Additional displays for specific countdown phases */
        if (foundIdx == 1) {
          text_draw_number(0xc9,0x240,uVar1);
          text_draw_number(0xfa,0x200,(short)uVar1);
        }
        if (foundIdx == 2) {
          text_draw_number(199,0x240,uVar1);
          text_draw_number(0xf9,0x200,(short)uVar1);
        }
        if (foundIdx == 3) {
          text_draw_number(200,0x240,uVar1);
        }
        if (foundIdx == 4) {
          text_draw_number(0xcc,0x240,uVar1);
        }
        if (foundIdx == 5) {
          text_draw_number(0xf4,0x200,uVar1);
          text_draw_number(0xf3,0x210,uVar1);
        }
      }
      else {
        /* ROM 0x032A66..0x032A90 -- one of the six `text_draw_rect_blink`
         * sites register row 62 left open because the arguments are register
         * expressions, not immediates:
         *     pea $1.w                      param_5 = 1      (palette)
         *     move.w #$160,-(a7)            param_4 = 0x160  (base tile)
         *     move.l $3993c(a0.l*4),-(a7)   a0 = d2*2  =>  0x3993C + idx*8
         *     move.l $39938(a0.l*4),-(a7)                    0x39938 + idx*8
         *     move.l (a0,d2.l*4),-(a7)      a0 = d5 = 0x3991C + idx*4
         * The decomposition of display_param into three bytes was invented:
         * 0x3991C holds single tile codes (0xAF,0xB0,...), not packed
         * coordinates, so this drew base 0 / palette 0 -- the charset row
         * smeared across the top of every menu and gameplay frame. */
        text_draw_rect_blink(vrd32s(0x3991C + foundIdx * 4),
                             vrd32s(0x39938 + foundIdx * 8),
                             vrd32s(0x3993C + foundIdx * 8),
                             0x160, 1);
        /* THE PER-PHASE CAPTIONS AND ARROWS -- ROM 0x032A96..0x032C10, absent
         * from the decompilation entirely. These are the second banner on each
         * tutorial screen: the red arrow beside the handlebar, "PULL", and the
         * pedal caption. Every one is a `text_draw_rect_blink` whose base tile
         * and palette are immediates (0x200/5, 0x240/1, 0x210/5), with a
         * `text_draw_rect_solid` on the opposite half of the 0x20 blink bit --
         * i.e. the caption alternates between drawn and cleared.
         * Verified against MAME's own tilemap: at fc700 (phase 2) rows 14/15
         * cols 12..13 carry 5200..5203, which is exactly blk 0xF9 at col 0x0C
         * row 0x0E with base 0x200 palette 5; at fc963 (phase 3) rows 14/15
         * cols 7..11 carry 1240..1244 = blk 0xC7 col 7 row 0x0E base 0x240
         * palette 1; at fc1263 (phase 5) row 20 cols 21..24 carry 5214..5217 =
         * blk 0xF3 col 0x15 row 0x13 base 0x210 palette 5. */
        if (foundIdx == 1) {
          text_draw_rect_blink(0xFA, 0x19, 0x0E, 0x200, 5);
          if (uVar1 & 0x20) text_draw_rect_blink(0xC9, 0x1B, 0x0E, 0x240, 1);
          else              text_draw_rect_solid(0xC9, 0x1B, 0x0E);
        }
        if (foundIdx == 2) {
          text_draw_rect_blink(0xF9, 0x0C, 0x0E, 0x200, 5);
          if (uVar1 & 0x20) text_draw_rect_blink(0xC7, 0x07, 0x0E, 0x240, 1);
          else              text_draw_rect_solid(0xC7, 0x07, 0x0E);
        }
        if (foundIdx == 3) {
          if (uVar1 & 0x20) text_draw_rect_blink(0xC8, 0x07, 0x0E, 0x240, 1);
          else              text_draw_rect_solid(0xC8, 0x07, 0x0E);
        }
        if (foundIdx == 4) {
          if (uVar1 & 0x20) text_draw_rect_blink(0xCC, 0x07, 0x0E, 0x240, 1);
          else              text_draw_rect_solid(0xCC, 0x07, 0x0E);
        }
        if (foundIdx == 5) {
          /* moveq #$f8,d0 ; add.l d3,d0 ; moveq #$10,d1 ; and.l d1,d0 */
          if (((int)uVar1 - 8) & 0x10) {
            text_draw_rect_blink(0xF4, 0x10, 0x0E, 0x200, 5);
            text_draw_rect_solid(0xF3, 0x15, 0x13);
          } else {
            text_draw_rect_blink(0xF3, 0x15, 0x13, 0x210, 5);
            text_draw_rect_solid(0xF4, 0x10, 0x0E);
          }
        }
      }
    }
  }
  text_draw_rect_blink(0xcd, 0xf, 6, 0x1e0, 1);
  return;
}

/* ---- gameplay_input_to_force ---- */

void gameplay_input_to_force(void)

{
  int iVar1;
  int iVar2;
  
  iVar1 = (int)W[0x2BCA] - (int)W[0x3FD2];
  iVar2 = (int)W[0x2BC8] - (int)W[0x3FD0];
  if (iVar1 < 0) {
    W[0x0D50] = iVar1 + 0x50;
    if (0 < W[0x0D50]) goto LAB_0000aaac;
  }
  else {
    W[0x0D50] = iVar1 + -0x50;
    if (W[0x0D50] < 0) {
LAB_0000aaac:
      W[0x0D50] = 0;
    }
  }
  if (iVar2 < 0) {
    W[0x0D54] = iVar2 + 0x50;
    if (W[0x0D54] < 1) goto LAB_0000aad4;
  }
  else {
    W[0x0D54] = iVar2 + -0x50;
    if (-1 < W[0x0D54]) goto LAB_0000aad4;
  }
  W[0x0D54] = 0;
LAB_0000aad4:
  if (W[0x0D50] < -0xc0) {
    W[0x0D50] = -0xc0;
  }
  if (0xc0 < W[0x0D50]) {
    W[0x0D50] = 0xc0;
  }
  if (W[0x0D54] < -0xc0) {
    W[0x0D54] = -0xc0;
  }
  if (0xc0 < W[0x0D54]) {
    W[0x0D54] = 0xc0;
  }
  W[0x0D50] = W[0x0D50] * 0xaa;
  W[0x0D54] = W[0x0D54] * -0xaa;
  return;
}

/* ---- gameplay_render_frame ---- */

void gameplay_render_frame(void)

{
  int iVar1;
  undefined2 uVar2;
  undefined2 uVar3;
  undefined2 uVar4;
  undefined2 uVar5;
  undefined2 uVar6;
  
  iVar1 = 0;
  do {
    if (W[0x1324 + (iVar1 * 0x140)] == 0) {
      uVar5 = (undefined2)W[0x1320 + (iVar1 * 0x140)];
      uVar4 = (undefined2)W[0x131C + (iVar1 * 0x140)];
      uVar3 = (undefined2)W[0x1318 + (iVar1 * 0x140)];
      uVar2 = 2;
    }
    else {
      uVar5 = (undefined2)W[0x1320 + (iVar1 * 0x140)];
      uVar4 = (undefined2)W[0x131C + (iVar1 * 0x140)];
      uVar3 = (undefined2)W[0x1318 + (iVar1 * 0x140)];
      uVar2 = 3;
    }
    dsp_cmd_place_object(0,uVar2,uVar3,uVar4,uVar5);
    iVar1 = iVar1 + 1;
  } while (iVar1 < 5);
  uVar6 = 6;
  wait_dsp_sync();
  uVar5 = (undefined2)((uint32_t)W[0x0CEC] >> 0x10);
  uVar4 = (undefined2)((uint32_t)W[0x0CE4] >> 0x10);
  uVar3 = (undefined2)((uint32_t)W[0x0CE0] >> 0x10);
  uVar2 = (undefined2)((uint32_t)W[0x0CDC] >> 0x10);
  terrain_chunk_visibility
            (W[0x0CDC],W[0x0CE0],W[0x0CE4],W[0x0CEC]);
  wait_dsp_sync(uVar2,uVar3,uVar4,uVar5,uVar6);
  dsp_cmd_place_object(0,0x13,0,0,0);
  iVar1 = 0;
  do {
    dsp_cmd_place_object
              (0,4,(short)W[0x1318 + (iVar1 * 0x140)],(short)W[0x1328 + (iVar1 * 0x140)],
               (short)W[0x1320 + (iVar1 * 0x140)]);
    iVar1 = iVar1 + 1;
  } while (iVar1 < 5);
  dsp_cmd_end_list(0x1e00);
  dsp_cmd_place_object_rotated
            (0,0x45,W[0x0D00],W[0x0D04],(short)W[0x0D08],-(short)W[0x0D0C],
             (short)-W[0x0D10],(short)-W[0x0D14],4);
  dsp_cmd_end_frame();
  return;
}

/* ---- gameplay_sub10_results_run ---- */

void gameplay_sub10_results_run(void)

{
  tilemap_enable_set();
  sync_post();
  camera_state_reset();
  W[0x16978] = 0;
  W[0x1697C] = 0x3f;
  W[0x0E58] = 0;
  W[0x169AC] = 0;
  W[0x169B0] = 0;
  W[0x16974] = 0;
  sound_reset_upper();
  sound_play_or_defer(0x4A);
  W[0x0CC0] = 0xb;
  gameplay_sub11_run();
  return;
}

/* ---- gameplay_sub11_run ---- */

void gameplay_sub11_run(void)

{
  /* REWRITTEN FROM THE ROM @0x0254D2 (register row 104). Ghidra rendered the
   * jump table on W[0x16978] as `if (v < 2 || v == 2) return;`, i.e. it
   * dropped the three cases for v = 0, 1, 2 and kept only the v >= 3 tail.
   * Sub 10 enters here with v = 0, so the fade-in never ran, v never became
   * 2 and the results screen sat in sub 11 forever (7200 frames measured;
   * MAME leaves after 305). The dispatch is
   *     move.w (A4),D0 ; ext.l ; subq.l #2 ; bhi default
   *     move.w ($c,PC,D0.w*2),D0 ; jmp ($2,PC,D0.w)      table @0x025512:
   *     v=0 -> 0x025514  v=1 -> 0x025548  v=2 -> 0x025570  v>=3 -> 0x0255AE
   * and MAME's own sub-11 timeline (tools/overnight/snap_results.lua) runs
   * exactly v=0 for 62 frames (fade-in, EB16 = 1697C*4 from 252 to 0), v=2
   * for 180 (W[0x16974] counting), v=3 for 63 (fade-out) and then state 0. */
  wait_dsp_sync();
  switch ((int)(int16_t)W[0x16978]) {
  case 0:                                   /* 0x025514: fade in */
    debug_profiler_mark();
    W[0xEB16] = (W[0x1697C] << 2) & 0xffff;
    W[0x1697C] = W[0x1697C] + -1;
    if (W[0x1697C] < 0) {
      W[0x169AC] = W[0x0E54];
      W[0x16978] = 2;
      W[0x1697C] = 0xb4;
    }
    break;
  case 1:                                   /* 0x025548: wait for the count-up */
    debug_profiler_mark();
    if ((W[0x169B0] >> 3) == W[0x169AC]) {
      W[0x16978] = W[0x16978] + 1;
      W[0x1697C] = 0xb4;
    }
    break;
  case 2:                                   /* 0x025570: hold; START skips */
    debug_profiler_mark();
    if ((W_LO16(0x2BA4) & 1) != 0) {        /* move.w $e02ba6 -- the 16-bit at 0x2BA6 */
      W[0x16978] = W[0x16978] + 1;
      W[0x1697C] = 0x3f;
    }
    if ((W_LO16(0x2B80) & 0x100) != 0) {    /* move.w $e02b82 */
      W[0x16978] = W[0x16978] + 1;
      W[0x1697C] = 0x3f;
    }
    W[0x1697C] = W[0x1697C] + -1;
    if (W[0x1697C] < 0) {
      W[0x16978] = W[0x16978] + 1;
      W[0x1697C] = 0x3f;
    }
    break;
  default:                                  /* 0x0255AE: fade out, leave */
    debug_profiler_mark();
    g_fog_r = 0;
    g_fog_g = 0;
    g_fog_b = 0;
    W[0xEB16] = (0x3f - (short)W[0x1697C]) * 4;
    W[0x1697C] = W[0x1697C] + -1;
    if (W[0x1697C] < 0) {
      wait_dsp_sync();
      W[0xEB16] = 0xff;
      debug_profiler_mark();
      game_stats_accumulate();
      if (W[0x0E18] == 1) {
        W[0x2C12] = 1;
        W[0x0CBC] = 10;
      }
      else {
        W[0x0CBC] = 0;
      }
    }
    break;
  }
  sprite_draw_2d(7,0x15d,0x40,0x80,0,0x20,0x20,0,0);
  sprite_draw_2d(7,0x15e,0x140,0x80,0,0x20,0x20,0,0);
  debug_profiler_mark();
  dsp_cmd_set_camera(0,0x27,0x8000,0xfff78000,0xfff40a00,0,W[0x0C98] << 5,0,2);
  debug_profiler_mark();
  if (0 < W[0x16978]) {
    W[0x16974] = W[0x16974] + 1;
  }
  return;
}

/* ---- gameplay_sub12_bonus_check_init ---- */

void gameplay_sub12_bonus_check_init(void)

{
  undefined2 uVar1;
  undefined1 uVar2;
  
  sync_post();
  camera_state_reset();
  sound_reset_all();
  W16_SET(0xE10, 0);
  W[0x0C78] = 0;
  W[0x0C80] = 1;
  W[0x0C68] = 0;
  W[0x0C5C] = 0;
  W[0x0C60] = 0;
  W16_SET(0x0C56, 0);            /* 0x009AE6 clr.w */
  W16_SET(0x0C58, 0);            /* 0x009AEC clr.w */
  W16_SET(0x0C54, 0);
  W[0x0C82] = 0;
  W[0x0C84] = 599;
  W[0x0C88] = 0;
  W[0xEB16] = 0;
  /* THE GLOVE POINTER'S X. `R[0x35080]` is ONE BYTE of a big-endian LONG --
   * register row 52/115's class. The table at 0x35080 is the glove's target
   * position per menu option, BE32: -32 for NOVICE, 416 for ADVANCED (and the
   * very next line's own `ROM_READ32(0x35080 + W[0x0C82]*4)` already reads it
   * as a long, so producer and consumer disagreed). The byte read gives 255. */
  W[0x0C70] = vrd32s(0x35080);
  uVar2 = 0x2c;
  cgram_load_tile_block(0x1c,0x140);
  uVar1 = 0;
  /* ROM 0x009B2C `move.b #$5,-(a7) ; pea $1c.w ; jsr cz_load_color_ramp` --
   * the palette is a pushed BYTE the decompile lost (register row 118). */
  cz_load_color_ramp(0x1c, 5);
  W[0x0CC0] = 0xd;
  /* ROM 0x009B42 `move.w #$f,-(a7) ; jsr $f4e0`: the MODE SELECT music, id
   * 0x0F (command 0x00A1). The decompile passed a dead local holding 0, which
   * wrote 0x4000 -- command 0 -- to slot 0, so the driver keyed nothing and
   * the mode-select screen was silent where MAME's slot 0 reads 0x80A1. */
  sound_play_or_defer(0x0F);
  gameplay_sub13_bonus_check_run();
  return;
}

/* ---- gameplay_sub13_bonus_check_run ---- */

void gameplay_sub13_bonus_check_run(void)

{
  short sVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  int iVar5;
  int iVar6;
  
  if ((W[0x0C68] + -3 < 3) || (2 < W[0x0C78])) {
    text_draw_rect_solid(0x1c,0x13,0x1c);
  }
  else {
    text_draw_rect_blink(0x1c, 0x13, 0x1c, 0x140, 5);
  }
  iVar2 = W[0x0C88] * 0x1c;
  if (iVar2 < 0) {
    iVar2 = iVar2 + 0xf;
  }
  FUN_0000e0e8((short)W[0x0C84],(short)((W[0x0C88] * 0x1c) / -0x10) + -0x188,
               (short)((iVar2 >> 4) + 0x130),0x2d0,(short)(-0x10 - W[0x0C88]),
               -(short)W[0x0C88]);
  sVar1 = W[0x0C82];
  if (W16(0x0C54) == 0) {
    if ((W[0x2BDC] & 2) != 0) {
      W[0x0C82] = 0;
    }
    if ((W[0x2BDC] & 1) != 0) {
      W[0x0C82] = W[0x0C80];
    }
    if ((W[0x2B3C] & 0x20) != 0) {
      W[0x0C82] = 0;
    }
    if ((W[0x2B3C] & 0x10) != 0) {
      W[0x0C82] = W[0x0C80];
    }
    if (sVar1 != W[0x0C82]) {
      sound_play(0x10);
    }
    tilemap_scroll_set((uint16_t)W[0x0C98] & 0x48);
    W16_SET(0xE10, W[0x0C82]);
    if ((W[0x2B3A] & 0x100) != 0) {
      sound_play(0x11);
      W16_SET(0x0C54, 1);
      tilemap_scroll_set(0);
    }
    if ((W[0x2BA6] & 1) != 0) {
      sound_play(0x11);
      W16_SET(0x0C54, 1);
      tilemap_scroll_set(0);
    }
    W[0x0C84] = W[0x0C84] + -1;
    if (W[0x0C84] < 0) {
      if (W16(0x0C54) != 1) {
        sound_play(0x11);
      }
      W16_SET(0x0C54, 1);
      tilemap_scroll_set(0);
    }
  }
  else {
    W[0x0C88] = W[0x0C88] + 8;
    if (0x100 < W[0x0C88]) {
      W[0x0C88] = 0x100;
    }
    if ((W[0x2B3C] & 0x100) != 0) {
      W[0x0C78] = 0x2d;
    }
    if ((W[0x2BA6] & 1) != 0) {
      W[0x0C78] = 0x2d;
    }
    W[0x0C78] = W[0x0C78] + 1;
    if (0x2d < W[0x0C78]) {
      if (W16(0xE10) == 0) {
        W[0x0CC0] = 0;
      }
      else {
        W[0x0CC0] = 0x16;
      }
    }
  }
  if ((W[0x2C0C] & 0x10) != 0) {
    if ((W[0x2B3A] & 0x80) != 0) {
      W16_SET(0xE64, (short)(W16(0xE64) + 1) % 3);
      debug_draw_value(5,6,1,0xa19a,W16(0xE64) + 1,0xa199,2);
    }
    if ((W[0x2B3A] & 0x40) != 0) {
      W16_SET(0xE64, (short)((W16(0xE64) + 2) % 3));
      debug_draw_value(5,6,1,0xa19a,W16(0xE64) + 1,0xa199,2);
    }
  }
  iVar2 = ROM_READ32(0x35080 + W[0x0C82] * 4) - W[0x0C70];
  if (iVar2 < 0) {
    iVar2 = iVar2 + 0xf;
  }
  W[0x0C70] = W[0x0C78] * 2 + (iVar2 >> 4) + W[0x0C70];
  /* THE GLOVE POINTER'S Y, and why it sat at the top of every menu screen.
   * ROM 0x009E6C is `adda.l $35088.l, a0` -- a big-endian LONG, value **288**.
   * `R[0x35088]` reads its first BYTE, which is 0, so the glove drew at y = 0
   * plus the small sine wobble (measured: the sprite went out at y=3). That is
   * exactly the "glove pointer sits ~288 px high" this file has carried as an
   * open item -- the missing number IS the 288. Register row 52/115's class. */
  iVar6 = vrd32s(0x35088) + ((int)(short)vrd16s(0x20B004 + ((W[0x0C68] << 0xb) >> 1 & 0x7ffe) * 2) >> 0xb);
  iVar2 = W[0x0C78] * 8;
  iVar5 = 0;
  _safety_ctr = 0;
  do {
    if (iVar5 == W[0x0C82]) {
      if (W16(0x0C54) != 0) {
        W16_SET(0x0C56 + iVar5 * 2, (int16_t)W16(0x0C56 + iVar5 * 2) + (-1));
        sVar1 = (int16_t)W16(0x0C56 + iVar5 * 2);
        goto joined_r0x00009eb8;
      }
      W16_SET(0x0C56 + iVar5 * 2, (int16_t)W16(0x0C56 + iVar5 * 2) + (1));
      if (0x80 < (int16_t)W16(0x0C56 + iVar5 * 2)) {
        W16_SET(0x0C56 + iVar5 * 2, 0x80);
      }
    }
    else {
      W16_SET(0x0C56 + iVar5 * 2, (int16_t)W16(0x0C56 + iVar5 * 2) + (-1));
      sVar1 = (int16_t)W16(0x0C56 + iVar5 * 2);
joined_r0x00009eb8:
      if (sVar1 < 0) {
        W16_SET(0x0C56 + iVar5 * 2, 0);
      }
    }
    W[0x0C5C + iVar5 * 4] = W[0x0C5C + iVar5 * 4] + 0x200;   /* 0x009EC0 addi.l $e00c5c(d2.l*4) */
    iVar5 = iVar5 + 1;
    if (1 < iVar5) {
      dsp_cmd_set_camera(0,0x375,0,0,0x350,0,0,0,0);
      dsp_cmd_set_camera(0,0x37d,0,0,0x350,0,0,0,0);
      dsp_cmd_set_camera(0,0x368,0,0,0x350,0,0,0,0);
      iVar5 = 0;
      do {
        if ((iVar5 == W[0x0C82]) || (W16(0x0C54) == 0)) {
          iVar3 = 0;
        }
        else {
          iVar3 = W[0x0C78] * W[0x0C78];
        }
        iVar4 = (int)(int16_t)W16(0x0C56 + iVar5 * 2) *
                ((int)(short)vrd16s(0x20B004 + ((uint32_t)W[0x0C5C + iVar5 * 4] >> 1 & 0x7ffe) * 2) >> 10);
        if (iVar4 < 0) {
          iVar4 = iVar4 + 0xf;
        }
        sVar1 = vrd16s(0x20B004 + (iVar4 >> 5 & 0x7ffe) * 2);
        dsp_cmd_set_camera(0,iVar5 * 2 + 0x369,iVar5 * 0x1a0 + -0xd0,0xa8 - (short)iVar3,0x350,0,0,
                           sVar1,0);
        dsp_cmd_set_camera(0,0x36d,iVar5 * 0x1a0 + -0xc0,0x98 - iVar3,0x351,0,0,(int)sVar1,0);
        sprite_draw_2d(6,0x157,W[0x0C70],iVar6 + iVar2,0,0x20,0x20,0,0);
        iVar5 = iVar5 + 1;
      } while (iVar5 < 2);
      if (W[0x0C68] == 10) {
        sound_play_p(0x0026006B);
      }
      if (W16(0x0C54) == 0) {
        sprite_draw_2d(2,vrd16s(0x35070 + (W[0x0C68] >> 5 & 3U) * 2),0xc0,0x170,0,
                       0x20,0x20,0,0);
        sprite_draw_2d(2,0x1ab,0x98,0x1bc,0,0x20,0x20,0,0);
        sprite_draw_2d(2,vrd16s(0x35078 + (W[0x0C68] >> 5 & 3U) * 2),0x150,0x180,0
                       ,0x20,0x20,0,0);
      }
      W[0x0C68] = W[0x0C68] + 1;
      return;
    }
  if (++_safety_ctr > 10000) break; } while( true ); _safety_ctr = 0;
}

/* ---- gameplay_sub14_init ---- */

void gameplay_sub14_init(void)

{
  W16_SET(0x172EC, 0);
  W16_SET(0x172EE, 0);
  if (W[0x0E18] == 1) {
    W16_SET(0x172EA, 0);
    W16_SET(0x172E8, 0);
  }
  else {
    W16_SET(0x172E8, 9);
    W16_SET(0x172EA, 0x17bb);
  }
  /* PROPCYCL_ENDING_PHASE=<n>: start the ending at phase n (ending function
   * n of the 13-entry table at ROM 0x38514) with the phase counter 0x172EA at
   * that phase's start, i.e. the previous entry of the end-counter table at
   * 0x37A62 ([540, 1560, 2145, 2775, 3375, 3915, 4485, 5355, 6075, 6795,
   * 6945, 7945]). A test knob, so a late phase can be checked without
   * playing the earlier ones; state an earlier phase set up is NOT made. */
  /* getenv is declared here: this file is compiled gnu89 without <stdlib.h>,
   * so an undeclared getenv returns an implicit `int` -- a 64-bit pointer
   * cut to 32 bits, and the dereference below segfaulted on the first frame
   * of the ending whenever the knob was set (register row 40's class). */
  int seed_phase = -1;
  { extern char *getenv(const char *);
    extern int g_ending_phase_req;   /* Escape -> Levels -> ending phase (level_select.c) */
    const char *e = getenv("PROPCYCL_ENDING_PHASE");
    char req[8];
    if (g_ending_phase_req >= 0) { snprintf(req, sizeof req, "%d", g_ending_phase_req); e = req;
                                   g_ending_phase_req = -1; }
    if (e && *e) {
      int n = atoi(e);
      if (n >= 0 && n < 13) {
        W16_SET(0x172E8, n);
        W16_SET(0x172EA, n == 0 ? 0 : vrd16s(0x37A62 + (n - 1) * 2));
        seed_phase = n;
      }
    }
    /* PROPCYCL_ENDING_TOTAL=<points>: the story TOTAL 0xE00E54 on entering
     * the ending, so a shortcut run (PROPCYCL_LEVEL=adv:3, total 0) takes
     * the same results -> name-entry branch (FUN_00031d40 rank check) as a
     * full story; MAME's bad-ending capture holds 8995 there. */
    e = getenv("PROPCYCL_ENDING_TOTAL");
    if (e && *e) W[0x0E54] = (int32_t)atol(e); }
  sync_post();
  sound_reset_upper();
  set_background_color(0,0,0);
  W_SET_HI16(0xEB20, 1);   /* move.w #1,$e0eb20: 16-bit, the HIGH half */
  W[0xEB16] = 0xff;
  W16_SET(0x172FA, 0x10);
  g_fog_mode = 3;
  W[0x0E0C] = 3;
  W[0x12DC] = 0;
  W[0x12D4] = 0;
  W16_SET(0x4708, 0);   /* ROM 0x02FFDA: clr.w $e04708 -- 16-bit, the HIGH half */
  W[0x172E4] = 0;
  W[0x172E0] = 0;
  W[0x172DC] = 0;
  W[0x172D8] = 0;
  W[0x0CC0] = 0xf;
  tilemap_wind_flash_update(6);
  if (seed_phase >= 0) { extern void ending_phase_seed(int); ending_phase_seed(seed_phase); }
  gameplay_sub15_run();
  return;
}

/* ---- gameplay_sub15_run ---- */

void gameplay_sub15_run(void)

{
  undefined4 *puVar1;
  
  W[0x4704] = 0;
  W[0x169E8] = 0;
  ending_credits_render();
  /* ROM 0x030034: `move.w (a2),d0 ; movea.l $38514(d0.w*4),a0 ; jsr (a0)`
   * with a2 = 0xE172E8 -- the ending phase indexes a 13-entry table of BE32
   * code pointers. Ghidra left `/ * gameplay sub dispatch * /;`, so the ending
   * drew nothing but the credits layer. Table (read from ROM):
   *   0 0x2B6A4 stage1   1 0x2BE6E stage2   2 0x2C146 stage3   3 0x2C29C stage4
   *   4 0x2C7BE stage5   5 0x2CC5A stage6   6 0x2CEEA stage7   7 0x2D16E stage8
   *   8 0x2E1B2 stage9   9 0x2EE3A stage10 10 0x2F79E stage11
   *  11 0x2FCAA re-enter the final stage (stage_transition)
   *  12 0x2FCFA the END card: fade, then sub 6 (final results) */
  switch (W16(0x172E8)) {
    case 0:  ending_sequence_stage1();  break;
    case 1:  ending_sequence_stage2();  break;
    case 2:  ending_sequence_stage3();  break;
    case 3:  ending_sequence_stage4();  break;
    case 4:  ending_sequence_stage5();  break;
    case 5:  ending_sequence_stage6();  break;
    case 6:  ending_sequence_stage7();  break;
    case 7:  ending_sequence_stage8();  break;
    case 8:  ending_sequence_stage9();  break;
    case 9:  ending_sequence_stage10(); break;
    case 10: ending_sequence_stage11(); break;
    case 11: ending_sequence_phase11(); break;
    case 12: ending_sequence_phase12(); break;
    default: break;
  }
  if (W[0x4704] != 0) {
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = 0x8010;
    W[0x0CA4] = puVar1;
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = (int32_t)0xffffffff;
    W[0x0CA4] = puVar1;
    W[0x4704] = 0;
  }
  viewport_params_write();
  if ((W[0x2BA6] & 1) != 0) {
    W16_SET(0x172E8, 0xb);
    W16_SET(0x172EA, 0x1f08);
  }
  W16_SET(0x172EA, W16(0x172EA) + 1);
  if (W16(0x172EA) == (vrd16s(0x37A62 + (W16(0x172E8) * 2)))) {
    W16_SET(0x172E8, W16(0x172E8) + 1);
    W[0x0CC0] = 0xf;
  }
  /* PROPCYCL_STAGEDBG: every ending PHASE change, with the counter */
  { extern int g_stagedbg; static int last_ph = -2;
    if (g_stagedbg && (int)W16(0x172E8) != last_ph) {
      last_ph = W16(0x172E8);
      fprintf(stderr, "[ENDPH] f%u phase=%d ctr=%d sub=%d fade=%d\n", g_sys.frame_count,
              (int)W16(0x172E8), (int)(uint16_t)W16(0x172EA), (int)W[0x0CC0],
              (int)(int16_t)W[0xEB16]);
    } }
  stub_empty(&R[0x3B092]);
  return;
}

/* ---- gameplay_sub16_init ---- */

/* NAME ENTRY. Both functions are PORTED FROM THE ROM (0x02A400 / 0x02A58A)
 * instruction by instruction; the transpile had lost the input widths, used
 * Ghidra's element strides on the 16-bit arrays, and passed the 68K ADDRESS
 * of the record (0x7274, truncated) to highscore_table_insert, which
 * dereferenced it as a host pointer and crashed at the end of every entry.
 *
 * Work RAM, all from the ROM's own access widths:
 *   0xE17274  long   the record being entered: score (top byte = story
 *                    continues when cleared, else 0x7F) ...
 *   0xE17278  byte*3 ... and its three NAME bytes (grid indices 0..49)
 *   0xE1727C  word   letters entered (3 = done)
 *   0xE1727E  word   cursor column 0..12     0xE17280  word  row 0..3
 *   0xE17282  word   cursor index row*13+col (50 = back, 51 = END)
 *   0xE17284  word   the timer, 1800 frames  0xE17286  word  the rank
 *   0xE17288  word*3 per-letter drop animation (0x4000 = parked above,
 *                    0xC000 = just picked, 0 = landed, 0xFC00 = END filler)
 *   0xE1726C/0xE17270  long  the magnifier's eased x / y
 *   0xE17290  long   the closing slide (0..0x100, +8 a frame once done)
 * The name bytes are one `_W[]` slot each (pinned in game_init, like the
 * 0xE04030 table they end up in). */

#define NE_NAME(i)          ((int)(int8_t)W[0x17278 + (i)])
#define NE_NAME_SET(i, v)   (W[0x17278 + (i)] = (int8_t)(v))

uint32_t highscore_table_insert(int, intptr_t);
short FUN_00031d40(int, uint32_t);

void gameplay_sub16_init(void)

{
  int i, n;
  { static int ne_tbl_dump_done; void ne_tbl_dump_enter(void);
    if (!ne_tbl_dump_done) { ne_tbl_dump_done = 1; ne_tbl_dump_enter(); } }

  /* NOVICE: a score that ranks 4th or lower goes into the table with a
   * blank name and skips the entry screen (ROM `subq.l #4,d0 ; blt`). */
  if (W16(0xE10) == 0) {
    int rank = FUN_00031d40((int32_t)W[0x0E0C], (uint32_t)W[0x0E54]);
    if (rank - 4 >= 0) {
      W[0x17274] = (int32_t)W[0x0E54];
      NE_NAME_SET(0, 0); NE_NAME_SET(1, 0); NE_NAME_SET(2, 0);
      highscore_table_insert((int32_t)W[0x0E0C], 0xE17274);
      dsp_param_init();
      W[0x0CC0] = 10;
      gameplay_sub10_results_run();
      return;
    }
  }
  tilemap_wind_flash_update(6);
  sync_post();
  sound_reset_upper();
  sound_play_or_defer(0x41);
  dsp_param_init();
  camera_state_reset();
  g_fog_r = 0;                        /* clr.w $e0eb1a/1c/1e */
  g_fog_g = 0;
  g_fog_b = 0;
  set_background_color(0,0,0);
  g_sys.videomix[0x0005] = 0;
  g_sys.videomix[0x0006] = 0;
  g_sys.videomix[0x0007] = 0;
  g_fog_mode = 3;
  W[0xEB16] = 0;
  NE_NAME_SET(0, 0x31); NE_NAME_SET(1, 0x31); NE_NAME_SET(2, 0x31);
  /* The five odometer digits, least significant first, 10 = blank once the
   * remaining value is 0 -- a 16-bit array at stride 2 (row 105). */
  n = (int32_t)W[0x0E54];
  for (i = 0; i < 5; i++) {
    W_A16_SET(0x169D0, i, (n == 0) ? 10 : n % 10);
    n = n / 10;
  }
  W16_SET(0x17288, 0x4000);
  W16_SET(0x1728A, 0x4000);
  W16_SET(0x1728C, 0x4000);
  W16_SET(0x1727C, 0);
  W16_SET(0x17286, FUN_00031d40((int32_t)W[0x0E0C], (uint32_t)W[0x169AC]));
  W[0x1726C] = (int32_t)0xffffff06;
  W[0x17270] = (int32_t)0xffffffd5;
  W16_SET(0x1727E, 0);
  W16_SET(0x17280, 0);
  W16_SET(0x17282, 0);
  W[0x2C12] = 0;                      /* clr.w $e02c12 (a pinned 16-bit slot) */
  W16_SET(0x17284, 0x708);
  W[0x17290] = 0;
  W[0x0CC0] = 0x11;
  gameplay_sub17_run();
  return;
}

/* PROPCYCL_TEST_NAME="i,j,k": enter those grid indices (0..51; 50 = back,
 * 51 = END) headlessly through the game's OWN input path -- the handlebar
 * ADC (input_force_analog, which read_mcu_inputs_process turns into the
 * direction flags 0xE02BDC on the next frame) for the cursor, and a START
 * edge for the selection. Same schedule as tools/overnight/snap_nameentry.lua
 * drives into MAME: from 40 frames in, one action every 10 frames. */
/* The knobs are read once, at program start. NOTE THE PROTOTYPES: this file
 * does not include <stdlib.h>, and an IMPLICITLY declared getenv() returns
 * `int`, so the pointer comes back cut to 32 bits and sign-extended -- a
 * fault on first use. That is register row 40's "getenv() inside the frame
 * loop segfaults, the fault address is a sign-extended 32-bit truncation of
 * a stack pointer": not a corrupted environment, a missing declaration. */
/* (getenv/atoi/atol are declared at the top of this file.) */
static int  ne_test_n = -1, ne_test_tgt[8], ne_test_dbg;
static long ne_test_score = -1, ne_test_storyend = -1;
static int  ne_test_e18 = -1;
__attribute__((constructor)) static void name_entry_test_env(void)
{
  const char *e = getenv("PROPCYCL_TEST_NAME"), *p;
  if (e && *e) {
    ne_test_n = 0;
    for (p = e; *p && ne_test_n < 8; ) {
      ne_test_tgt[ne_test_n++] = atoi(p);
      while (*p && *p != ',') p++;
      if (*p == ',') p++;
    }
  }
  e = getenv("PROPCYCL_TEST_HISCORE"); ne_test_score = e ? atol(e) : -1;
  e = getenv("PROPCYCL_TEST_STORYEND"); ne_test_storyend = e ? atol(e) : -1;
  if (e && (e = strchr(e, ',')) != NULL) ne_test_e18 = atoi(e + 1);
  ne_test_dbg = getenv("PROPCYCL_STAGEDBG") != NULL;
}

static void name_entry_test_drive(void)
{
  static int k, phase;
  const int n = ne_test_n; const int *tgt = ne_test_tgt;
  int cnt, col, row, t, tc, tr;
  extern void input_force_analog(int x, int y);
  if (n <= 0) return;
  if (++k <= 40) return;
  cnt = W16(0x1727C);
  if (cnt >= n || cnt >= 3) return;
  col = W16(0x1727E); row = W16(0x17280);
  t = tgt[cnt]; tc = t % 13; tr = t / 13;
  phase = (phase + 1) % 10;
  if (phase >= 3) return;
  if (col != tc)      input_force_analog(tc > col ? 0x0C0 : 0x33E, 0x1FF);
  else if (row != tr) input_force_analog(0x1FF, tr > row ? 0x0C0 : 0x33E);
  else if (phase == 0) W[0x2BA6] |= 1;      /* a START edge this frame */
}

/* PROPCYCL_TEST_HISCORE=<score>: on the RESULTS screen (state 3 sub 7) hold
 * the stage score 0xE169AC at <score> -- the poke
 * tools/overnight/snap_nameentry.lua (PNE_SCORE) makes into MAME, so a NOVICE
 * run reaches results case 8 with a table-making score and goes to name entry
 * (sub 16) on both sides. Called from state_gameplay_run. */
void name_entry_test_score(void)
{
  static int storyend_done;
  if (ne_test_score >= 0 && (int32_t)W[0x0CC0] == 7) W[0x169AC] = (int32_t)ne_test_score;
  /* PROPCYCL_TEST_STORYEND=<total>[,<e18>]: on the first frame of STORY gameplay
   * (sub 3 with 0xE00E10 set) jump to name entry holding the end-of-story
   * state MAME's full run has at sub 16 (mame_pass/story.txt: all-cleared
   * 0xE00E12 = 1, continues 0xE00E14 = 0, total 0xE00E54) -- the shortcut
   * snap_nameentry.lua PNE_MODE=1 takes on the machine. sub16_init reads
   * nothing else, so the entry is the same one the ending leads to. */
  if (ne_test_storyend >= 0 && !storyend_done && (int32_t)W[0x0CC0] == 3 && W16(0xE10) != 0) {
    storyend_done = 1;
    W16_SET(0xE12, 1); W16_SET(0xE14, 0);
    W[0x0E54] = (int32_t)ne_test_storyend;
    if (ne_test_e18 >= 0) W[0x0E18] = ne_test_e18;   /* ",1": the normal ending -> the replay */
    W[0x0CC0] = 16;
    printf("[TEST] f%u: story end -> name entry (sub 16), total %ld\n", g_sys.frame_count, ne_test_storyend);
  }
}

/* The ranking tables as the MACHINE holds them, byte for byte (the
 * `TBL <tag> E04030 ...` / `E15BF0 ...` lines tools/overnight/
 * snap_nameentry.lua writes), rebuilt from this engine's representation: a
 * 4-aligned score long per 8-byte entry, every other byte its own slot. */
static int ne_tbl_byte(uint32_t a)
{
  uint32_t base, e;
  if (a >= 0x15BF0 && a < 0x15E70) base = 0x15BF0;
  else if (a >= 0x15E74 && a < 0x15E94) base = 0x15E74;
  else if (a >= 0x4030 && a < 0x4080) base = 0x4030;
  else if (a >= 0x4088 && a < 0x40A8) base = 0x4088;
  else return (int)W[a] & 0xff;
  e = base + ((a - base) & ~7u);
  if (a - e < 4) return (int)(((uint32_t)(int32_t)W[e] >> (24 - 8 * (a - e))) & 0xff);
  return (int)W[a] & 0xff;
}
static void ne_tbl_dump(const char *tag)
{
  uint32_t a;
  printf("TBL %s E04030 ", tag);
  for (a = 0x4030; a <= 0x40AB; a++) printf("%02X", a >= 0x4080 && a < 0x4088 ? 0 : ne_tbl_byte(a));
  printf("\nTBL %s E15BF0 ", tag);
  for (a = 0x15BF0; a <= 0x15E93; a++) printf("%02X", ne_tbl_byte(a));
  printf("\n");
}

void ne_tbl_dump_enter(void) { if (ne_test_dbg) ne_tbl_dump("enter"); }

/* ---- gameplay_sub17_run ---- */

uint16_t gameplay_sub17_run(void)

{
  /* Inputs, as the ROM reads them: 0xE02B3C/0xE02B3A (the button matrix
   * input_decode_buttons would fill -- not run in this engine, so 0) and
   * 0xE02BA6 (START edge) are kept whole-slot by the reader in this tree;
   * the stick direction word 0xE02BDC is the HIGH half of its slot, which is
   * where read_mcu_inputs_process puts the edge plus the auto-repeat. */
  int dir, b3c, b3a;

  name_entry_test_drive();
  dir = W16(0x2BDC);
  b3c = (int)W[0x2B3C]; b3a = (int)W[0x2B3A];
  if (W16(0x1727C) < 3) {
    int prev;
    if ((b3c & 0x80) || (dir & 4)) {
      W16_SET(0x17280, W16(0x17280) - 1);
      if (W16(0x17280) < 0) W16_SET(0x17280, 0);
    }
    if ((b3c & 0x40) || (dir & 8)) {
      W16_SET(0x17280, W16(0x17280) + 1);
      if (W16(0x17280) > 3) W16_SET(0x17280, 3);
    }
    if ((b3c & 0x20) || (dir & 2)) {
      W16_SET(0x1727E, W16(0x1727E) - 1);
      if (W16(0x1727E) < 0) W16_SET(0x1727E, 0);
    }
    if ((b3c & 0x10) || (dir & 1)) {
      W16_SET(0x1727E, W16(0x1727E) + 1);
      if (W16(0x1727E) > 0xc) W16_SET(0x1727E, 0xc);
    }
    prev = W16(0x17282);
    W16_SET(0x17282, W16(0x17280) * 13 + W16(0x1727E));
    if (W16(0x17282) > 0x33) {
      W16_SET(0x17280, 3);
      W16_SET(0x1727E, 0xc);
      W16_SET(0x17282, 0x33);
    }
    if (W16(0x17282) != prev) sound_play(0x58);
    if ((b3a & 8) || (W[0x2BA6] & 1)) {
      if (W16(0x17282) == 0x32) {                 /* back: no click */
        W16_SET(0x1727C, W16(0x1727C) - 1);
        if (W16(0x1727C) < 0) W16_SET(0x1727C, 0);
      }
      else {
        if (W16(0x17282) == 0x33) {                /* END */
          name_entry_fill_blanks();
          W16_SET(0x1727C, 3);
          W16_SET(0x17284, 0x78);
        }
        else {
          NE_NAME_SET(W16(0x1727C), W16(0x17282));
          W_A16_SET(0x17288, W16(0x1727C), 0xc000);
          W16_SET(0x1727C, W16(0x1727C) + 1);
          if (W16(0x1727C) > 2) W16_SET(0x17284, 0x96);
        }
        sound_play(0x29);
      }
    }
  }
  name_entry_render();
  if (W16(0x1727C) == 3 && (int32_t)W[0x17290] < 0x100)
    W[0x17290] = (int32_t)W[0x17290] + 8;
  { int32_t f = (int32_t)W[0x17290], m = f * 28;
    FUN_0000e0e8(W16(0x17284) - 0x96, m / -0x10 - 0x188, ((m < 0 ? m + 0xf : m) >> 4) + 0x130,
                 0x2d0, -0x10 - f, -f); }
  if (W16(0x17284) < 0x80)
    W[0xEB16] = (int16_t)((0x7f - W16(0x17284)) * 2);
  W16_SET(0x17284, W16(0x17284) - 1);
  if (W16(0x17284) < 0) {
    tilemap_enable_set();
    dsp_param_init();
    W[0x17274] = W16(0xE10) ? (int32_t)W[0x0E54] : (int32_t)W[0x169AC];
    if (W16(0xE10) != 0 && W16(0xE12) != 0) {
      int cont;
      cont = (W16(0xE14) >= 10) ? 9 : W16(0xE14);
      W[0x17274] = (int32_t)((uint32_t)(int32_t)W[0x17274] + ((uint32_t)cont << 24));
    }
    else {
      W[0x17274] = (int32_t)((uint32_t)(int32_t)W[0x17274] + 0x7f000000u);
    }
    highscore_table_insert((int32_t)W[0x0E0C], 0xE17274);
    eeprom_write_block(&W[0x4030],0x79);    /* stubbed: the EEPROM is not modelled */
    if (ne_test_dbg) {
      int e, j;
      printf("[NAME] f%u score=%08X name=%02X %02X %02X -> story head %d:",
             g_sys.frame_count, (uint32_t)(int32_t)W[0x17274], NE_NAME(0) & 0xff,
             NE_NAME(1) & 0xff, NE_NAME(2) & 0xff, (int)(int8_t)W[0x40A8]);
      for (e = (int8_t)W[0x40A8], j = 0; j < 10 && e >= 0 && e < 10; j++, e = (int8_t)W[0x4037 + e * 8])
        printf(" %d:%X/%02X%02X%02X", e, (uint32_t)(int32_t)W[0x4030 + e * 8] & 0xffffff,
               (int)W[0x4034 + e * 8] & 0xff, (int)W[0x4035 + e * 8] & 0xff, (int)W[0x4036 + e * 8] & 0xff);
      printf("\n");
      ne_tbl_dump("leave");
    }
    W[0x0CC0] = 10;
  }
  if (W16(0x17284) < 0x96) W16_SET(0x1727C, 3);
  if (b3a & 0x100) {
    dsp_param_init();
    W[0x0CC0] = 10;
  }
  return 0;
}

/* ---- gameplay_sub20_countdown_init ---- */

void gameplay_sub20_countdown_init(void)

{
  camera_state_reset();
  sound_reset_all();
  sound_play_or_defer(0x3F);
  W[0x17378] = 0;
  W[0x17380] = 0;
  W[0xEB16] = 0;
  g_fog_mode = 3;
  g_fog_r = 0;
  g_fog_g = 0;
  g_fog_b = 0;
  W[0x0CC0] = 0x15;
  return;
}

/* ---- gameplay_sub21_countdown_run ---- */

uint32_t gameplay_sub21_countdown_run(void)

{
  undefined4 uVar1;
  uint32_t uVar2;
  int iVar3;
  
  g_countdown_display = W[0x17378] + -3;
  dsp_w32(0x10038 + 4 * (W[0x0CA0] * 0x2000), (int32_t)(0x780));
  iVar3 = 0;
  do {
    /* Same 32-bit BE table as gameplay_countdown_tick, and the SAME one-byte
     * misread. Read byte-wise the table is [0,0,0,0,0,0,0,60], so as soon as
     * the counter passed 60 this loop ran to iVar3 == 8, W[0x17384] became 7,
     * and `if (6 < W[0x17384])` set the exit flag -- the CONTROLS tutorial
     * ended after ~61 frames where the machine runs it for 1140 (the table's
     * real last threshold). */
    if (W[0x17378] < vrd32s(0x398F4 + iVar3 * 4)) break;
    iVar3 = iVar3 + 1;
  } while (iVar3 < 8);
  W[0x17384] = iVar3 + -1;
  g_countdown_sub_frame = W[0x17378] - vrd32s(0x398F4 + (int)W[0x17384] * 4);
  if (g_countdown_display == 0) {
    gameplay_countdown_setup();
  }
  if (6 < W[0x17384]) {
    W[0x17380] = 1;
  }
  if (((W[0x2B3A] & 0x100) != 0) || ((W[0x2BA6] & 1) != 0)) {
    W[0x17380] = 1;
  }
  if (W[0x17380] != 0) {
    W[0x0CC0] = 0xc;
    sound_stop(0x3F);
  }
  dsp_cmd_set_camera(0,0x27,0x8000,0xfff78000,0xfff40a00,0,W[0x0C98] << 5,0,2);
  if (-1 < g_countdown_display) {
    gameplay_countdown_tick();
  }
  gameplay_countdown_render();
  tilemap_scroll_set((uint16_t)(W[0x17378] >> 5) & 1); uVar1 = 0;
  uVar2 = (((uint32_t)((int32_t)((uint32_t)uVar1 >> 8)) << 8) | (uint8_t)(W[0x2C0C])) & 0xffffff10;
  if ((W[0x2C0C] & 0x10) != 0) {
    debug_draw_value(2,2,4,(char*)(g_sys.rom + 0x3b096),W[0x17378],0xb0a0,4);
    debug_draw_value(2,3,4,(char*)(g_sys.rom + 0x3b0a1),W[0x17384],&R[0x3B0A0],&R[0x60004]);
    uVar2 = debug_draw_value(2,4,4,(char*)(g_sys.rom + 0x3b0ab),g_countdown_sub_frame,&R[0x3B0A0],
                             &R[0x60004]);
  }
  W[0x17378] = W[0x17378] + 1;
  return uVar2;
}

/* ---- gameplay_sub22_init ---- */

void gameplay_sub22_init(void)

{
  /* 0x1738C AND 0x1738E ARE TWO 16-BIT FIELDS SHARING ONE SLOT, and the
   * whole ADVANCED screen hung on it. ROM 0x032DB4 is `clr.w $e1738e` then
   * `clr.w $e1738c`, and every later access is 16-bit too: `move.w #$1,(a4)`
   * and `tst.w (a4)` on 0xE1738C (0x032EF4/0x032EF8), `addq.w #$1,(a0)` on
   * 0xE1738E (0x032F16). 0x1738C is 4-aligned so it is the HIGH half;
   * 0x1738E is 2-mod-4, the LOW half -- and written full-width it was
   * rebuilt from neighbouring bytes by `sync_wram_to_W` every frame, so the
   * frame counter NEVER ADVANCED PAST ZERO. `sVar1 = W[0x1738E] - 3` was
   * therefore always negative, the draw block was never entered even once,
   * and the screen sat on whatever the previous state had left. Rows
   * 46/108/130's class. */
  W_SET_LO16(0x1738C, 0);      /* 0x1738E -- frame counter */
  W_SET_HI16(0x1738C, 0);      /* 0x1738C -- "button pressed" latch */
  g_fog_r = 0;
  g_fog_g = 0;
  g_fog_b = 0;
  g_fog_mode = 3;
  W[0xEB16] = 0xff;
  W[0x0CC0] = 0x17;
  gameplay_sub23_run();
  return;
}

/* ---- gameplay_sub23_run ---- */

void gameplay_sub23_run(void)

{
  short sVar1;
  
  sVar1 = (short)(W_LO16(0x1738C) - 3);
  if (W_LO16(0x1738C) == 3) {
    set_background_color(0,0,0);
    sync_post();
    /* THE ADVANCED-MODE SCREEN, recovered from ROM 0x032E1C..0x032ED8.
     *
     * This whole path was unreachable until register row 149 made the stick
     * able to pick ADVANCED, so none of it had ever run and its damage had
     * never been seen: a user's first look at it reported "numbers and
     * things where they weren't supposed to be".
     *
     * Three faults here, all of the shapes this register already knows:
     *
     * (a) The first `text_draw_rect_blink` had been reduced to
     *     `(0, 0, 0, 0, 0)` with the comment "simplified broken multi-line
     *     call" -- register row 62's class, the base tile code and palette
     *     lost by the decompiler. Base 0 / palette 0 is precisely "tile 0
     *     everywhere", which is what the user saw. ROM 0x032E88..0x032EAE
     *     pushes right-to-left: `pea $1`, `move.w #$120`,
     *     `move.l $39988(a0.l*4)`, `move.l $39984(a0.l*4)`, `move.l d5`,
     *     with d5 = `$39970(d0.w*4)` and a0 = `lea (a0,a0.l)` = 2*W16(0xE66).
     *     So 0x39984/0x39988 is a table of PAIRS at stride EIGHT -- the same
     *     shape register row 112 recovered at 0x39938/0x3993C.
     *
     * (b) Both `cz_load_color_ramp` palette indices were 0. The ROM pushes
     *     them as BYTES -- `move.b #$1` and `move.b #$5` -- which is row
     *     118's class exactly: Ghidra modelled that byte as the top of a
     *     long, so every ramp landed on palette 0 and the screen drew grey.
     *
     * (c) The base codes were truncated with `(short)` where the ROM pushes
     *     a LONG. Harmless for these table values, wrong in principle. */
    cgram_load_tile_block(vrd32(0x39970 + (W16(0xE66) * 4)), 0x120);
    cz_load_color_ramp(vrd32(0x39970 + (W16(0xE66) * 4)), 1);

    if (W16(0xE66) == 3) {
      cgram_load_tile_block(0x11f, 0x1a0);
    }
    cz_load_color_ramp(0x11f, 5);
  }
  if (-1 < sVar1) {
    text_draw_rect_blink(vrd32(0x39970 + (W16(0xE66) * 4)),
                         vrd32(0x39984 + (W16(0xE66) * 8)),
                         vrd32(0x39988 + (W16(0xE66) * 8)),
                         0x120, 1);
    if (W16(0xE66) == 3) {
      text_draw_rect_blink(0x11f, 10, 0x10, 0x1a0, 5);
    }
  }
  if (((W[0x2BA6] & 1) != 0) || ((W[0x2B3A] & 0x100) != 0)) {
    W_SET_HI16(0x1738C, 1);
  }
  if (W_HI16(0x1738C) == 0) {
    W[0xEB16] = W[0xEB16] + -4;
    if (W[0xEB16] < 0) {
      W[0xEB16] = 0;
    }
  }
  else {
    W[0xEB16] = W[0xEB16] + 6;
    if (0xff < W[0xEB16]) {
      W[0x0CC0] = 0;
    }
  }
  W_SET_LO16(0x1738C, (int16_t)(W_LO16(0x1738C) + 1));
  if (0x96 - W_LO16(0x1738C) < 0x40) {
    W_SET_HI16(0x1738C, 1);
  }
  return;
}

/* ---- gameplay_sub24_gameover_init ---- */

void gameplay_sub24_gameover_init(void)

{
  state_ranking_init();
  W[0x1703C] = 0;
  g_fog_r = 0xff;
  g_fog_g = 0xff;
  g_fog_b = 0xff;
  g_fog_mode = 3;
  W[0x17038] = 0;
  cgram_load_tile_block(0xf6,0x270);
  cz_load_color_ramp(0xf6, 5);   /* ROM 0x029F4A `move.b #$5,-(a7)` -- palette byte lost (row 118) */
  W[0x0CC0] = 0x19;
  gameplay_sub25_gameover_run();
  return;
}

/* ---- gameplay_sub25_gameover_run ---- */

uint16_t gameplay_sub25_gameover_run(void)

{
  uint16_t uVar2;
  int iVar1;
  
  if (-1 < (int)(W[0x17038] - 3U)) {
    if ((W[0x17038] - 3U & 0x28) == 0) {
      text_draw_rect_solid(0xf6,0x1e,2);
    }
    else {
      text_draw_rect_blink(0xf6, 0x1e, 2, 0x270, 5);
    }
  }
  state_ranking_run();
  uVar2 = W[0x2B3A] & 0x100;
  if (((W[0x2B3A] & 0x100) != 0) ||
     (uVar2 = W[0x2BA6] & 1, (W[0x2BA6] & 1) != 0)) {
    W[0x1703C] = 1;
  }
  W[0x17038] = W[0x17038] + 1;
  if (0x4b0 - W[0x17038] < 0x80) {
    W[0x1703C] = 1;
  }
  if (W[0x1703C] == 0) {
    W[0xEB16] = 0;
  }
  else {
    W[0xEB16] = W[0xEB16] + 2;
    if (0xff < W[0xEB16]) {
      W[0xEB16] = 0xff;
      iVar1 = FUN_00031d40((short)W[0x0E0C],W[0x0E54]);
      if ((iVar1 == 0) || (W[0x169C4] != 5)) {
        W[0x0CC0] = 10;
        W[0x0CC8] = 0xf;
        uVar2 = 2;
        W[0x0CC4] = 2;
      }
      else {
        W[0x0CC0] = 0x10;
        uVar2 = 6;
        W[0x0CC8] = 6;
        W[0x0CC4] = 6;
      }
    }
  }
  return uVar2;
}

/* ---- gameplay_sub26_init ---- */

/* THE STORY-MODE INTERMISSION (subs 26/27) between the DAYs, rewritten from
 * ROM 0x0333B8 / 0x033476. On MAME it lasts 861 frames after day 1 and 680
 * after day 2, then goes to the next DAY screen (sub 22).
 *
 * Per-day parameters are four BIG-ENDIAN WORDS at 0x399A4 + day*8 (the
 * indexed EAs are `-$8(a3,d0.l)` with extension word 0x0AF8 -- SCALE 2 on
 * d0 = day*4, which capstone prints without its scale):
 *   +0 first animation set  +2 last animation set  +4 script index
 *   +6 the fade-out frame
 * The script index selects two ROM cursors: the SOUND script at
 * 0x39BF8[i] (6-byte [frame, id, param] records) and the CAPTION script at
 * 0x39AD0[i] (see stage_event_text_display).
 *
 * State, all 16-bit unless noted: 0xE17390 animation set, 0xE17392 frame,
 * 0xE17394 caption cursor (long, the working copy) / 0xE173A4 (long, kept),
 * 0xE17398 / 0xE173A2 caption toggle (working / kept), 0xE1739C sound
 * cursor (long), 0xE173A0 "done", 0xE173A8 the last sound row (long). Two of
 * them sit at 2-mod-4 offsets, which sync_wram_to_W rebuilds from their
 * neighbours if they are written as whole _W[] slots. */

void gameplay_sub26_init(void)

{
  int day = W16(0x0E66);          /* a 16-bit field (0x024B58 `addq.w`) */
  sync_post();
  camera_state_reset();
  sound_reset_upper();
  sound_play_or_defer(0x45);
  W[0x169F0] = 0;                      /* clr.w $e169f0 / $e169f2 */
  W[0x169F2] = 0;
  { extern int g_stagedbg;
    if (g_stagedbg)
      fprintf(stderr, "[INTER] f%u enter: day %d  sets %d..%d script %d end %d\n",
              (unsigned)g_sys.frame_count, day,
              vrd16s(0x399A4 + day * 8), vrd16s(0x399A6 + day * 8),
              vrd16s(0x399A8 + day * 8), vrd16s(0x399AA + day * 8)); }
  W16_SET(0x17390, vrd16s(0x399A4 + day * 8));
  W[0x1739C] = (int32_t)vrd32(0x39BF8 + vrd16s(0x399A8 + day * 8) * 4);
  W[0x173A4] = (int32_t)vrd32(0x39AD0 + vrd16s(0x399A8 + day * 8) * 4);
  W[0x4704] = 0;
  W16_SET(0x17392, 0);
  W16_SET(0x17398, 0);
  W16_SET(0x173A2, 0);
  W[0xEB06] = 1;
  W16_SET(0x173A0, 0);
  environment_params_reload();
  g_fog_mode = 3;
  W[0x0CC0] = 0x1b;
  gameplay_sub27_run();
  return;
}

/* ---- gameplay_sub27_run ---- */

uint32_t gameplay_sub27_run(void)

{
  extern void anim_update_and_render(intptr_t nb, int t);
  int day = W16(0x0E66);          /* a 16-bit field (0x024B58 `addq.w`) */
  int16_t fr;
  int guard = 0;

  W[0x4704] = 0;
  if (W16(0x17392) == 0) W[0x173A8] = 0;

  /* 0x033538: the SOUND script -- every record whose frame is now. */
  while ((uint32_t)W[0x1739C] >= 4 && (uint32_t)W[0x1739C] + 6 <= ROM_SIZE &&
         W16(0x17392) == vrd16s((uint32_t)W[0x1739C]) && guard++ < 32) {
    uint32_t c = (uint32_t)W[0x1739C];
    if (vrd16s(c + 4) >= 0) {
      sound_play_p(((uint32_t)vrd16(c + 2) << 16) | vrd16(c + 4));
    } else if (vrd16s(c + 2) >= 0) {
      /* a direct MCU row at 0x399BC + id*8: [slot, cmd, pslot, pval] */
      uint32_t row = 0x399BC + (uint32_t)vrd16s(c + 2) * 8;
      W[0x173A8] = (int32_t)row;
      comms_w16(g_sys.commsram, 0x100 + (unsigned)vrd16s(row + 4) * 2, vrd16(row + 6));
      comms_w16(g_sys.commsram, (unsigned)vrd16s(row) * 2, vrd16(row + 2) | 0x4000u);
    } else {
      /* stop the last row: `andi.w #$37ff` on its command word */
      unsigned sl = (unsigned)vrd16s((uint32_t)W[0x173A8]) * 2;
      if (sl + 1 < COMMSRAM_SIZE)
        comms_w16(g_sys.commsram, sl, comms_r16(g_sys.commsram, sl) & 0x37ffu);
    }
    W[0x1739C] = (int32_t)(c + 6);
  }

  fr = W16(0x17392);
  if (day == 1) {                                                   /* 0x033540 */
    if (fr == 0) {
      comms_w16(g_sys.commsram, 0x0102, 0x2400);
      comms_w16(g_sys.commsram, 0x0104, 0xff);
      comms_w16(g_sys.commsram, 0x010A, 0x2000);
      comms_w16(g_sys.commsram, 0x010C, 0xff);
      comms_w16(g_sys.commsram, 0x010E, 0x2000);
      comms_w16(g_sys.commsram, 0x0110, 0xff);
      sound_play(0x02);
    }
    if (fr < 0x78)       comms_w16(g_sys.commsram, 0x0104, (uint16_t)((fr * 0xff) / -0x78 + 0xff));
    else if (fr < 0xaa)  comms_w16(g_sys.commsram, 0x0104, 0);
    else if (fr < 0xc8)  comms_w16(g_sys.commsram, 0x0104, (uint16_t)((fr - 0xaa) / 0x1e));
    else if (fr == 0xdc) sound_stop(0x02);      /* `move.w #$2` -- the C had 0: the MUSIC */
  }

  /* 0x0335E4: the caption script, on its kept cursor/toggle */
  W[0x17394] = W[0x173A4];
  W16_SET(0x17398, W16(0x173A2));
  stage_event_text_display(fr, (0x19 << 16) | 7);
  W[0x173A4] = W[0x17394];
  W16_SET(0x173A2, W16(0x17398));

  W[0xEB04] = W[0xEB04] + 1;                                        /* 0x03361E */
  if (W[0xEB06] != 0) {
    stage_objects_load_list(W16(0x17390));
    player_model_render_only();
  } else {
    int32_t *cp = (int32_t *)W[0x0CA4];
    cp[0] = 0x8002;
    cp[1] = (int32_t)(int16_t)W[0x169F0];
    W[0x0CA4] = (intptr_t)(cp + 2);
    dsp_viewport_setup((int)(int16_t)W[0x169F0], (int)(int16_t)W[0x169F2]);
    anim_update_and_render(0xAB04, (int16_t)W[0xEB04]);
    W[0xEB06] = (W_LO16(0xAB08) <= (int16_t)W[0xEB04]) ? 1 : 0;   /* 0x033684 */
    if (W[0xEB06] != 0) {
      { extern int g_stagedbg;
        if (g_stagedbg) fprintf(stderr, "[INTER] f%u frame %d: animation set %d done (len %d)\n",
                                (unsigned)g_sys.frame_count, fr, W16(0x17390), W_LO16(0xAB08)); }
      W16_SET(0x17390, W16(0x17390) + 1);
      if (W16(0x17390) > vrd16s(0x399A6 + day * 8)) W16_SET(0x173A0, 1);
    }
  }
  if (day == 2 && fr >= 0x142 && fr < 0x1e0) stage_sky_sphere_draw(0x41);   /* `move.w #$41` */
  stage_static_objects_draw();
  if (day == 1) stage_water_objects_draw();

  /* 0x0336E4: the fades -- white in over the first 0x22 frames, then out */
  if (fr <= 0x21) {
    g_fog_b = 0xff; g_fog_g = 0xff; g_fog_r = 0xff;
    W[0xEB16] = 0xff - fr * 8;
  } else {
    g_fog_b = 0; g_fog_g = 0; g_fog_r = 0;
  }
  {
    int32_t end = vrd16s(0x399AA + day * 8);
    if (day == 1) {
      if (end - 0x101 <= fr) W[0xEB16] = (fr - end) + 0x101;
    } else {
      if (end - 0x21 <= fr) W[0xEB16] = 0x108 + (fr - end) * 8;
    }
  }
  if ((int32_t)W[0xEB16] < 0)    W[0xEB16] = 0;
  if ((int32_t)W[0xEB16] > 0xff) W[0xEB16] = 0xff;

  W16_SET(0x17392, fr + 1);
  { extern int g_stagedbg;
    int k = fr + 1;
    if (g_stagedbg && (k % 30 == 0 || k == 1)) {
      int i;
      fprintf(stderr, "[CAM] k=%d fr=%d set=%d eb04=%d cam=%d,%d,%d,%d,%d,%d ", k, fr, W16(0x17390),
              (int)(int16_t)W[0xEB04], (int)(int32_t)W[0x0CDC], (int)(int32_t)W[0x0CE0], (int)(int32_t)W[0x0CE4],
              (int)(int32_t)W[0x0CE8], (int)(int32_t)W[0x0CEC], (int)(int32_t)W[0x0CF0]);
      for (i = 0; i < 6; i++) {
        intptr_t nb = 0xAB04 + i * 0x80;
        fprintf(stderr, " [%d m=%d t=%d/%d/%d r=%d/%d/%d]", i, (int)(int32_t)W[nb],
                (int)(int32_t)W[nb+0x24], (int)(int32_t)W[nb+0x28], (int)(int32_t)W[nb+0x2C],
                (int)(int32_t)W[nb+0x30], (int)(int32_t)W[nb+0x34], (int)(int32_t)W[nb+0x38]);
      }
      fputc('\n', stderr);
    } }
  if (fr + 1 == vrd16s(0x399AA + day * 8) - 1) FUN_0000f584(0x45);  /* 0x0337B8 */

  if (W16(0x173A0) != 0) {                                          /* 0x0337C4 */
    { extern int g_stagedbg;
      if (g_stagedbg) fprintf(stderr, "[INTER] f%u frame %d: done -> sub 22\n",
                              (unsigned)g_sys.frame_count, fr); }
    W[0x0CC0] = 0x16;
    FUN_0000f66a(0x24, 0xff);
    FUN_0000f66a(0x25, 0xff);
    FUN_0000f66a(0x23, 0xff);
    sound_stop(0x02);
    if (W[0x173A8] != 0) {
      unsigned sl = (unsigned)vrd16s((uint32_t)W[0x173A8]) * 2;
      if (sl + 1 < COMMSRAM_SIZE)
        comms_w16(g_sys.commsram, sl, comms_r16(g_sys.commsram, sl) & 0x37ffu);
    }
    if (W[0x0CBC] == 3) sound_play_or_defer(0x0F);
  }
  /* 0x03382E: START (or the handlebar button) skips it */
  /* read as whole slots, like their writer (read_mcu_inputs_process) and
   * every other reader (arcade_menu.c) -- the ROM's are 16-bit fields */
  if ((W[0x2BA6] & 1) != 0 || (W[0x2B3A] & 0x100) != 0) {
    W16_SET(0x173A0, 1);
    FUN_0000f584(0x45);
  }
  return 0;
}

/* ==== THE STORY CONTINUE / GAME-OVER SCREENS: gameplay subs 28..31 ========
 * Ported instruction by instruction from the ROM (0x03451A..0x034FFE). The
 * Ghidra C these replace called different functions from the ROM's own jsr
 * targets, passed truncated or missing arguments, read the 16-bit story state
 * as whole _W[] slots, and segfaulted on its first frame (text_print_string
 * handed 0xffffb0d2, a sign-extended ROM address, as a host string).
 *
 * The screen's state, all 16-bit (`move.w`/`tst.w`/`addq.w` in the ROM):
 *   0x169B8  SEL    the YES/NO selection (1 = YES)            -- HIGH half
 *   0x173C0  COUNT  the YES/NO column-reveal countdown 15..-1 -- HIGH half
 *   0x173C2  BLINK  frame counter, bits 3-4 blink the choice  -- LOW  half
 *   0x173C4  PREV   SEL on the previous frame                 -- HIGH half
 *   0x173C6  ARMED  enough coins (or free play) this screen   -- LOW  half
 * 0x173C2 and 0x173C6 are 2-mod-4: as whole slots they were rebuilt from
 * their neighbours' bytes by sync_wram_to_W every frame.
 * 0x169B4 TIMER is 32-bit (`move.l`, `divs.l`): 900 frames = 15 s, shown as
 * TIMER/60 in two large digits. The coin count W[0x2C0E] is a 16-bit value
 * at a 2-mod-4 offset kept in a pinned whole slot (game_logic.c), so it is
 * read as (int16_t)W[0x2C0E] like every other user of it.
 * a2 = 0xE03F30 in the ROM: $c4(a2) = 0x3FF4 FREE PLAY, $c6(a2) = 0x3FF6
 * coins to continue, $c8(a2) = 0x3FF8 the continue-timer index. */

#define CONT_FREE   ((int)W16(0x3FF4))
#define CONT_NEED   ((int)W16(0x3FF6))
#define CONT_COINS  ((int)(int16_t)W[0x2C0E])
/* ROM 0x0345DE et seq.: `move.w $c6(a2),d0 ; cmp.w (a4),d0 ; ble ok ;
 * tst.w $c4(a2) ; beq no` -- enough coins, or free play. */
#define CONT_CAN_CONTINUE() (CONT_NEED <= CONT_COINS || CONT_FREE != 0)

/* PROPCYCL_CONTDBG=1: one line per frame of subs 28..31 (and the first frame
 * after), in the format of tools/overnight/probe_continue.lua's `C` lines, so
 * the two logs diff field for field against the machine. */
static void cont_dbg(void)
{
  static int on = -1;
  if (on < 0) { const char *e = getenv("PROPCYCL_CONTDBG"); on = (e && *e && *e != '0'); }
  if (!on) return;
  printf("C f=%u sub=%ld coins=%d t=%ld sel=%d cnt=%d blk=%d prev=%d en=%d "
         "ba6=%04X b3a=%04X b82=%04X s5c=%ld e6a=%d e14=%d fp=%d need=%d "
         "eb16=%ld f3c=%ld c8=%ld tot=%ld sc=%ld quota=%ld\n",
         (unsigned)g_sys.frame_count, (long)W[0x0CC0], CONT_COINS, (long)W[0x169B4],
         W16(0x169B8), W16(0x173C0), W16(0x173C2), W16(0x173C4), W16(0x173C6),
         (unsigned)(W[0x2BA6] & 0xffff), (unsigned)(W[0x2B3A] & 0xffff),
         (unsigned)(W[0x2B82] & 0xffff), (long)W[0x15F5C], W16(0x0E6A), W16(0x0E14),
         CONT_FREE, CONT_NEED, (long)W[0xEB16], (long)W[0x1703C], (long)W[0x173C8],
         (long)W[0x0E54], (long)W[0x169AC], (long)W[0x0E60]);
}

/* ---- gameplay_sub28_init @ 0x03451A ---- */

void gameplay_sub28_init(void)

{
  sync_post();
  ranking_sound_init();
  /* 034526: move.w $e03ff8.l,d0 ; move.l $15c4fc(d0.w*4),$e169b4.l --
   * [1200, 900, 600] frames by the continue-timer setting. */
  W[0x169B4] = vrd32s(0x15C4FC + (int)W16(0x3FF8) * 4);
  W16_SET(0x169B8, 1);
  W16_SET(0x173C4, 1);
  W16_SET(0x173C0, 0xF);
  W16_SET(0x173C2, 0);
  W16_SET(0x173C6, 0);
  scene_enter_transition();
  W[0x0CC0] = 0x1d;
  gameplay_sub29_run();
  return;
}

/* The continue itself: ROM 0x034676..0x0346B0 (Start) and 0x0346EE..0x034728
 * (the handlebar button), identical code. */
static void continue_take(void)
{
  if (CONT_FREE == 0) {
    W[0x2C0E] = (intptr_t)(int16_t)(CONT_COINS - CONT_NEED);   /* sub.w d0,(a4) */
  }
  game_timer_lap();
  W16_SET(0x0E6A, W16(0x0E6A) + 1);      /* continues taken this game: quota -300 each */
  W16_SET(0x0E14, W16(0x0E14) + 1);
  sync_post();
  W[0x0CC0] = 0;                          /* clr.l -> sub 0, the stage select again */
  scene_clear_transition();
  sound_play(0x0F);
}

/* ---- gameplay_sub29_run @ 0x034572 ---- */

void gameplay_sub29_run(void)

{
  static int s_stale_lc;   /* -$10(a6): see the dead branch at 0x034B70 */
  int old_sel, d3, lc, i;
  uint32_t pen, src;
  int pal = (int)vrd16(0x37958);          /* 0x000E: palette of the credit line */

  /* 0345A4: a coin edge restarts the timer; the coin that completes the
   * continue price also selects YES and restarts the choice blink. */
  if (((W[0x2B82] & 1) != 0) || ((W[0x2B82] & 4) != 0)) {
    W[0x169B4] = vrd32s(0x15C4FC + (int)W16(0x3FF8) * 4);
    if (CONT_COINS == CONT_NEED) {       /* move.w (a4),d0 ; cmp.w $c6(a2),d0 ; bne */
      W16_SET(0x173C2, 0);
      W16_SET(0x169B8, 1);
    }
  }
  /* 0345DE: while the continue can be taken, the stick / buttons move SEL. */
  if (CONT_CAN_CONTINUE()) {
    W16_SET(0x173C6, 1);
    old_sel = W16(0x169B8);               /* movea.w (a0),a0 : sign-extended */
    if ((W[0x2BDA] & 2) != 0) W16_SET(0x169B8, 1);
    if ((W[0x2BDA] & 1) != 0) W16_SET(0x169B8, 0);
    if ((W[0x2B3C] & 0x20) != 0) W16_SET(0x169B8, 1);
    if ((W[0x2B3C] & 0x10) != 0) W16_SET(0x169B8, 0);
    if (old_sel != W16(0x169B8)) {
      sound_play(0x10);
    }
  }
  /* 034656: Start. NO -> the timer runs out now; YES without the coins ->
   * the timer is rounded down to a whole second; YES with them -> continue. */
  if ((W[0x2BA6] & 1) != 0) {
    if (W16(0x169B8) == 0) {
      W[0x169B4] = 0;
    }
    else if (CONT_CAN_CONTINUE()) {
      continue_take();
    }
    else {
      W[0x169B4] = (W[0x169B4] / 0x3c) * 0x3c;   /* divs.l ; *15*4 */
    }
  }
  /* 0346CE: the handlebar button -- the same, minus the rounding. */
  if ((W[0x2B3A] & 0x100) != 0) {
    if (W16(0x169B8) == 0) {
      W[0x169B4] = 0;
    }
    else if (CONT_CAN_CONTINUE()) {
      continue_take();
    }
  }
  /* 034730 */
  if (W16(0x173C4) != W16(0x169B8)) {
    W16_SET(0x173C2, 0);
  }
  /* 034742: the scrolling sky. Pushes (right to left) 2, 0, fc<<5, 0,
   * #$fff40a00, #$fff78000, #$8000, $27, 0. The Ghidra call had the two
   * negative coordinates cut to 0x8000 / 0xa00. */
  dsp_cmd_set_camera(0, 0x27, 0x8000, (int32_t)0xfff78000, (int32_t)0xfff40a00, 0,
                     (uint32_t)W[0x0C98] << 5, 0, 2);
  /* 034772: "CONTINUE?" */
  text_draw_rect_blink(0xb7, 0xb, 8, 0x120, 1);
  /* 03478C: "POINTS REQUIRED: 300 POINTS LESS." -- only until the coins are
   * in, only in coin play, and only while a continue still lowers the quota
   * (three times: 1000 -> 700 -> 400 -> 100). */
  if ((W16(0x173C6) == 0) && (CONT_FREE == 0) && (W16(0x0E6A) < 3)) {
    text_draw_rect_blink(0xd4, 2, 0x18, 0x1a0, 1);
    text_draw_rect_blink(0xe3, 0x18, 0x18, 0x1c0, 1);
    /* 0347DA: move.l $15c4f8.l,d3 -- a BE32 (300); the Ghidra C read ONE
     * BYTE of it. Digit block 0xFB+d, base code 0x1E0 + d*6, palette 5. */
    d3 = vrd32s(0x15C4F8);
    for (i = 0; i < 3; i++) {
      text_draw_rect_blink(0xfb + d3 % 10, 0x16 - i * 2, 0x17, 0x1e0 + (d3 % 10) * 6, 5);
      d3 = d3 / 10;
    }
  }
  /* 034836: the countdown, TIMER/60 in two large digits (tens blank when 0) */
  d3 = W[0x169B4] / 0x3c;
  text_draw_rect_blink(0xe6 + d3 % 10, 0x13, 0xd, 0x160 + (d3 % 10) * 4, 1);
  d3 = d3 / 10;
  if (d3 != 0) {
    text_draw_rect_blink(0xe6 + d3 % 10, 0x11, 0xd, 0x160 + (d3 % 10) * 4, 1);
  }
  else {
    text_draw_rect_solid(0xe6, 0x11, 0xd);
  }
  /* 0348C6: the YES/NO choice, once the continue can be taken. */
  if (CONT_CAN_CONTINUE()) {
    if (W16(0x173C0) >= 0) {
      /* 0348DE: reveal the five choice blocks one 8-byte column per frame.
       * sync_post clears the whole text layer first, which is why the rest of
       * the screen blanks for these 16 frames on the machine too. */
      sync_post();
      text_draw_number(0x11b, 0x1e0, (int)W16(0x173C0));
      text_draw_number(0x11c, 0x240, (int)W16(0x173C0));
      text_draw_number(0x11d, 0x2a0, (int)W16(0x173C0));
      text_draw_number(0x11e, 0x2e0, (int)W16(0x173C0));
      text_draw_number(0x1c, 0x320, (int)W16(0x173C0));
      W16_SET(0x173C0, W16(0x173C0) - 1);
      if (W16(0x173C0) < 0) {
        /* 034956: palette bytes pushed with move.b */
        cz_load_color_ramp(0x11b, 5);
        cz_load_color_ramp(0x11c, 7);
        cz_load_color_ramp(0x11d, 9);
        cz_load_color_ramp(0x1c, 0xb);
      }
    }
    else {
      int blk, col, row, base, bpal;
      int on = ((W16(0x173C2) & 0x18) == 0x18);
      text_draw_rect_solid(0x11b, 5, 0xd);
      text_draw_rect_solid(0x11c, 0x17, 0xd);
      if (W16(0x169B8) != 0) {
        /* 03499C: YES selected */
        text_draw_rect_blink(0x11e, 0x18, 0xe, 0x2e0, 9);
        if (!on) { blk = 0x11b; col = 5;    row = 0xd; base = 0x1e0; bpal = 5; }
        else     { blk = 0x11d; col = 6;    row = 0xe; base = 0x2a0; bpal = 9; }
      }
      else {
        /* 034A16: NO selected */
        text_draw_rect_blink(0x11d, 6, 0xe, 0x2a0, 9);
        if (!on) { blk = 0x11c; col = 0x17; row = 0xd; base = 0x240; bpal = 7; }
        else     { blk = 0x11e; col = 0x18; row = 0xe; base = 0x2e0; bpal = 9; }
      }
      text_draw_rect_blink(blk, col, row, base, bpal);           /* 034A8C */
      text_draw_rect_blink(0x1c, 0x13, 0x1a, 0x320, 0xb);
    }
    /* 034AA8: the handlebar and gauge demo sprites */
    sprite_draw_2d(2, (int16_t)vrd16(0x35070 + ((W[0x0C98] >> 5) & 3) * 2), 0xc0, 0x150, 0,
                   0x20, 0x20, 0, 0);
    sprite_draw_2d(2, 0x1ab, 0x98, 0x19c, 0, 0x20, 0x20, 0, 0);
    sprite_draw_2d(2, (int16_t)vrd16(0x35078 + ((W[0x0C98] >> 5) & 3) * 2), 0x150, 0x160, 0,
                   0x20, 0x20, 0, 0);
  }
  /* 034B44: "INSERT n MORE COIN(S)", blinking on bit 5 of the frame count. */
  if (!CONT_CAN_CONTINUE()) {
    if ((W[0x0C98] & 0x20) == 0) {
      if (CONT_CAN_CONTINUE()) {
        /* 034B70: dead code in the ROM (the outer test excludes it). Kept
         * for fidelity; its palette word is an uninitialised local, the
         * previous frame's -$10(a6). */
        text_print_string(6, 0x16, (char *)&g_sys.rom[0x3B0B6], (s_stale_lc << 1) & 0xffff);
        text_draw_rect_solid(0xfb, 0x10, 0x11);
        text_draw_rect_blink(0x1b, 8, 0x12, 0x2f0, 3);
      }
      else {
        text_draw_rect_blink(0x1a, 8, 0x12, 0x330, 3);
        /* 034BD0: movea.w $c6(a2),a0 ; suba.w (a4),a0 -- the digit is the
         * number of coins still missing: block 0xFB, base 0x1E0 + n*6. */
        text_draw_rect_blink(0xfb, 0x10, 0x11, 0x1e0 + (CONT_NEED - CONT_COINS) * 6, 5);
      }
    }
    else {
      text_draw_rect_solid(CONT_CAN_CONTINUE() ? 0x1b : 0x1a, 8, 0x12);
      text_draw_rect_solid(0xfb, 0x10, 0x11);
    }
  }
  /* 034C3C: cycle palette 0x0E's pen 0 through the 8-colour table at
   * 0x3795A every 64 frames -- the colour-cycling credit line. The Ghidra C
   * read the palette number (a BE16, 0x000E) as one byte (0) and so cycled
   * palette 0 instead. */
  lc = (int)((W[0x0C98] >> 6) & 7);
  s_stale_lc = lc;
  pen = 0x7F00 + (uint32_t)pal * 0x10;
  src = 0x3795A + (uint32_t)lc * 4;
  g_sys.palette_ram[pen]               = R[src];
  g_sys.palette_ram[pen + 1]           = 0;
  g_sys.palette_ram[0x8000 + pen]      = R[src + 1];
  g_sys.palette_ram[0x8000 + pen + 1]  = 0;
  g_sys.palette_ram[0x10000 + pen]     = R[src + 2];
  g_sys.palette_ram[0x10000 + pen + 1] = 0;
  palette_mark_written((int)pen);
  palette_mark_written((int)pen + 1);
  /* 034CA8: the credit line -- "FREE PLAY", or "CREDIT(S) n/need" */
  if (CONT_FREE != 0) {
    text_print_string(0x1c, 0x1c, (char *)&g_sys.rom[0x3B0D2], pal);
  }
  else {
    d3 = (CONT_COINS < 10) ? CONT_COINS : 9;
    text_draw_signed_decimal(0x20, 0x1c, 4, d3, pal);
    text_draw_hex_digits(0x25, 0x1c, 1, CONT_NEED, pal);
    text_print_string(0x19, 0x1c, (char *)&g_sys.rom[0x3B0DC], pal);
    text_print_string(0x24, 0x1c, (char *)&g_sys.rom[0x3B0E6], pal);
  }
  /* 034D4E: time out -> bank the stage score into the total, then the name
   * entry (sub 16) if it made the story ranking, else the TOTAL POINTS
   * board (sub 30). FUN_00031d40 takes the FULL 32-bit total; the Ghidra
   * call cut it to a char. */
  W[0x169B4] = W[0x169B4] - 1;
  if (W[0x169B4] < 0) {
    W[0x0E54] = W[0x169AC] + W[0x0E54];
    g_sub_state_max = 0xb;
    scene_clear_transition();
    if (FUN_00031d40(3, (uint32_t)W[0x0E54]) != 0) {
      W[0x0CC0] = 0x10;
    }
    else {
      W[0x0CC0] = 0x1e;
    }
  }
  W16_SET(0x173C4, W16(0x169B8));
  W16_SET(0x173C2, W16(0x173C2) + 1);
  /* 034DA2: the results jingle's counter (0xE15F5C, addq.w). */
  W[0x15F5C] = W[0x15F5C] + 1;
  if (W[0x15F5C] == 0x5a) {
    sound_play(0x43);
  }
  cont_dbg();
  return;
}


/* ---- gameplay_sub30_init @ 0x034DC6 ---- */

void gameplay_sub30_init(void)

{
  int i, d3, v;

  tilemap_enable_set();
  camera_state_reset();
  W[0xEB16] = 0xff;          /* the fade level: pinned whole slot (game_logic.c) */
  g_fog_mode = 3;
  W[0x1703C] = 0;
  W[0x15ED0] = 8;
  W[0x173C8] = 0;            /* clr.l: the screen's frame counter */
  /* 034DFA: the total's five digits for the odometer, 10 = a blank reel.
   * `move.w d0,$e169d0(d2.l*2)` -- a 16-bit array at stride 2, which
   * score_odometer_animate reads with W_A16 (register row 105); the Ghidra
   * C stored whole slots at stride 1. */
  d3 = W[0x0E54];
  for (i = 0; i < 5; i++) {
    v = (d3 == 0 && i != 0) ? 10 : d3 % 10;
    W_A16_SET(0x169D0, i, v);
    d3 = d3 / 10;
  }
  W[0x0CC0] = 0x1f;
  gameplay_sub31_run();
  return;
}

/* ---- gameplay_sub31_run @ 0x034E40 ---- */

void gameplay_sub31_run(void)

{
  int i;

  /* the results board: frame, backdrop, "RESULTS" plate */
  dsp_cmd_place_object_abs(0, 0x365, 0, 0, 0x350);
  dsp_cmd_place_object_abs(0, 0x373, 0, 0, 0x350);
  dsp_cmd_place_object_abs(0, 0x37d, 0, 0, 0x350);
  gameover_stage_clear_text();                     /* "TOTAL POINTS" */
  /* 034E8C: pushes 0x130, $fff0.w, $ff8a.w (sign-extended), total, total<<3, 0 */
  score_odometer_animate(0, W[0x0E54] << 3, W[0x0E54], -0x76, -0x10, 0x130);
  for (i = 0; i < 5; i++) {
    dsp_cmd_place_object_rotated_abs(0, 0x351, i * 0x28 - 0x4a, -0x14, 0x130, 0, 0, 0, 0);
  }
  sprite_draw_2d(7, 0x1ef, 0x1e0, 0x11a, (int32_t)0xffe0bb40, 0x30, 0x30, 0x6b00, 0);
  /* 034F1C: hold 240 frames, then fade out and go to the game over (sub 10) */
  W[0x173C8] = W[0x173C8] + 1;
  if (0xf0 < W[0x173C8]) {
    W[0x1703C] = 1;
    W[0x15ED0] = 8;
  }
  if (W[0x1703C] == 0) {
    W[0xEB16] = (int16_t)(W[0xEB16] - W[0x15ED0]);
    if (W[0xEB16] < 0) {
      W[0xEB16] = 0;
    }
  }
  else {
    W[0xEB16] = (int16_t)(W[0x15ED0] + W[0xEB16]);
    if (0xff < W[0xEB16]) {
      W[0xEB16] = 0xff;
      FUN_0000f584(0x43);   /* ROM 0x034F54 / 0x034F84 `move.w #$43` */
      W[0x0CC0] = 10;
    }
  }
  W[0x15F5C] = W[0x15F5C] + 1;
  if (W[0x15F5C] == 2) {
    FUN_0000f584(0x43);   /* ROM 0x034F54 / 0x034F84 `move.w #$43` */
  }
  if (W[0x15F5C] == 3) {
    sound_play_or_defer(0x43);
  }
  cont_dbg();
  return;
}

/* ---- gameplay_sub4_service_init ---- */

void gameplay_sub4_service_init(void)

{
  if (W[0x0E0C] == 3) {
    stage_transition_init();
  }
  else {
    sync_post();
    set_background_color(0,0,0);
    W[0x1698C] = 0;
    W[0x16990] = 0;
    bonus_model_init_full();
    camera_update_wrapper();
    W[0x16968] = 0x14a;
    W[0x0CC0] = 5;
    gameplay_sub5_service_run();
  }
  return;
}

/* ---- gameplay_sub6_init ---- */

void gameplay_sub6_init(void)

{
  W_SET_HI16(0xEB20, 0);   /* clr.w $e0eb20: 16-bit, the HIGH half */
  tilemap_scroll_set(0);
  if (W16(0xE10) == 0) {
    results_screen_init_normal();
  }
  else {
    results_screen_init_boss();
  }
  gameplay_sub7_run();
  return;
}

/* ---- gameplay_sub7_run ---- */

void gameplay_sub7_run(void)

{
  if (W16(0xE10) == 0) {
    results_screen_update_normal();
  }
  else {
    results_screen_update_boss();
  }
  return;
}

/* ---- gameplay_sub8_init ---- */

void gameplay_sub8_init(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x0252AE is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. Sub-state 8 has no init work in the retail ROM.
   */
  return;
}

/* ---- gameplay_sub9_results_init ---- */

void gameplay_sub9_results_init(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x0252B0 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. Sub-state 9 has no init work in the retail ROM.
   */
  return;
}

/* ---- gameplay_sub_init ---- */

void gameplay_sub_init(void)

{
  W[0x0C8C] = (int32_t)0xfffffeb5;
  gameplay_init_player_and_world();
  gameplay_init_state_vars();
  W[0x0CC0] = 4;
  gameplay_sub4_service_init();
  return;
}

/* ---- gameplay_tick ---- */

void gameplay_tick(void)

{
  undefined2 uVar1;
  undefined2 uVar2;
  undefined2 uVar3;
  
  if (_g_system_halt == 0) {
    static int _gt_dbg = 0;
    /* Per-frame stage trace: only under PROPCYCL_VERBOSE. It printed 14
     * lines a frame for the first frames of every launch. */
    int _gt = (_gt_dbg < 3) && propcycl_verbose();
    if (_gt) printf("  [GT] terrain_lod...\n");
    terrain_lod_update_wrapper();
    gameplay_input_to_force();
    if (_gt) printf("  [GT] player_render_stub...\n");
    player_render_stub();
    if (W[0x0E18] == 0) {
      replay_record_frame();
    }
    uVar1 = 3;
    gameplay_timer_update();
    uVar3 = 0xb;
    wait_dsp_sync(uVar1);
    uVar1 = 3;
    if (_gt) printf("  [GT] player_bounds_check...\n");
    player_bounds_check();
    uVar2 = 0;
    wait_dsp_sync(uVar1,uVar3);
    uVar1 = 3;
    if (_gt) printf("  [GT] objects_move_update...\n");
    objects_move_update();
    wait_dsp_sync(uVar1,uVar2);
    uVar3 = 3;
    if (_gt) printf("  [GT] world_grid+render_all...\n");
    world_grid_calc_position();
    uVar1 = 3;
    world_render_all();
    /* Replay: the terrain step can take its hard-reset branch where our
     * collision disagrees with the recorded path; the recording is the
     * ground truth for what the camera and rider show, so re-pin. */
    { extern void flight_replay_pin_attitude(void); flight_replay_pin_attitude(); }
    uVar2 = 7;
    wait_dsp_sync(uVar1,uVar3);
    uVar1 = 3;
    if (_gt) printf("  [GT] camera_update...\n");
    camera_update_wrapper();
    camera_grid_calc_position();
    wait_dsp_sync(uVar1,uVar2);
    if (_gt) printf("  [GT] FUN_0000e016...\n");
    FUN_0000e016();
    if (_gt) printf("  [GT] terrain_chunk_visibility...\n");
    terrain_chunk_visibility(W[0x0CDC],W[0x0CE0],W[0x0CE4],W[0x0CEC]);
    uVar3 = 8;
    wait_dsp_sync();
    uVar1 = 3;
    if (_gt) printf("  [GT] terrain_props_dispatch...\n");
    terrain_props_dispatch();
    uVar2 = 0xc;
    wait_dsp_sync(uVar1,uVar3);
    uVar1 = 3;
    if (_gt) printf("  [GT] player_render...\n");
    player_render();
    wait_dsp_sync(uVar1,uVar2);
    if (_gt) printf("  [GT] objects_render_master...\n");
    objects_render_master();
    if (W[0x0E0C] == 3) {
      world_highscore_3d_draw();
    }
    uVar2 = 6;
    wait_dsp_sync();
    uVar1 = 3;
    if (_gt) printf("  [GT] balloon_render...\n");
    balloon_render_and_hit_check();
    wait_dsp_sync(uVar1,uVar2);
    if (_gt) printf("  [GT] stage_camera+env+wind+particle...\n");
    stage_camera_path_update();
    environment_zone_tick();
    wind_effect_level_calc();
    particle_system_update();
    FUN_0000a532();
    if (_gt) { printf("  [GT] gameplay_tick COMPLETE\n"); _gt_dbg++; }
  }
  debug_profiler_mark();
  return;
}

/* ---- stage_cutscene_dispatch ---- */

void stage_cutscene_dispatch(void)

{
  undefined4 *puVar1;
  
  W[0x4704] = 0;
  W[0x169E8] = 0;
  /* misc dispatch */;
  dsp_w32(0x10038 + 4 * (W[0x0CA0] * 0x2000), (int32_t)((int)W16(0x17360)));
  if (W[0x4704] != 0) {
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = 0x8010;
    W[0x0CA4] = puVar1;
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = (int32_t)0xffffffff;
    W[0x0CA4] = puVar1;
    W[0x4704] = 0;
  }
  stage_event_text_display((int)W16(0x172EA),0x190007);
  W[0x0C8C] = W[0x0C8C] + 1;
  W16_SET(0x172EA, W16(0x172EA) + 1);
  if (W16(0x172EA) == (vrd16s(0x39082 + (W16(0x172E8) * 2)))) {
    W16_SET(0x172E8, W16(0x172E8) + 1);
    W[0x0CC4] = 0xf;
  }
  return;
}

/* ---- stage_event_text_display ---- */

void stage_event_text_display(int param_1,undefined4 param_2)

{
  /* ROM 0x033264 -- the intermission's CAPTION SCRIPT. 0xE17394 holds a ROM
   * CURSOR (a long) into a list of 6-byte records [frame, block, column],
   * all BIG-ENDIAN WORDS; 0xE17398 is a 16-bit double-buffer toggle.
   *   - at a record's frame: block > 0 starts revealing that caption block
   *     (text_draw_rect_blink, alternating base tile 0x110/0x190 and palette
   *     d5/d5+2); block <= 0 blanks the PREVIOUS record's rectangle
   *     (text_draw_rect_solid on cur[-4], cur[-2]). Either way cur += 6.
   *   - for the next two records: load the colour ramp one frame before, and
   *     reveal the block column by column (text_draw_number) for the 16
   *     frames before the record's frame.
   * param_1 is the frame (long), param_2 packs the row in its HIGH word
   * (`movea.w $24(a7)`) and the palette in its LOW word (`move.w $26(a7)`).
   *
   * The transpile dereferenced the ROM cursor as a host pointer -- the
   * SIGSEGV on entering the intermission after the first story stage. */
  int32_t d2 = param_1;
  int16_t d5 = (int16_t)(uint32_t)param_2;
  int32_t row = (int16_t)((uint32_t)param_2 >> 16);
  int16_t d3 = (d5 == 7) ? 0x110 : 0x210;
  uint32_t cur = (uint32_t)W[0x17394];
  uint32_t a2;
  int16_t tog;
  if (cur < 4 || cur + 12 > ROM_SIZE) return;
  tog = W16(0x17398);
  if (d2 == vrd16s(cur)) {                                         /* 0x033290 */
    if (vrd16s(cur + 2) > 0) {
      text_draw_rect_blink(vrd16s(cur + 2), vrd16s(cur + 4), row,
                           (uint16_t)((tog << 7) + d3), (int32_t)d5 + tog * 2);
      W16_SET(0x17398, 1 - tog);
    } else {
      text_draw_rect_solid(vrd16s(cur - 4), vrd16s(cur - 2), row);
    }
    cur += 6;
    W[0x17394] = (int32_t)cur;
  }
  tog = W16(0x17398);
  for (a2 = cur; a2 != cur + 12; a2 += 6) {                        /* 0x033302 */
    int32_t fr = vrd16s(a2), blk = vrd16s(a2 + 2);
    if (blk <= 0) continue;
    if (d2 == fr - 1) cz_load_color_ramp(blk, (uint8_t)(d5 + tog * 2));
    if (fr - 2 < d2 || d2 < fr - 0x11) continue;
    if (d2 == 0 && fr - 0x11 < d2) {
      int16_t d4;
      for (d4 = 0xf; fr - d2 - 2 <= d4; d4--)
        text_draw_number(blk, (tog << 7) + d3, d4);
    } else {
      text_draw_number(blk, (tog << 7) + d3, fr - d2 - 2);
    }
  }
  return;
}

/* ---- stage_handler_course0 ---- */

void stage_handler_course0(int *param_1)

{
  if (*param_1 == 0x44) {
                    
                    
    /* course handler dispatch */;
    return;
  }
  return;
}

/* ---- stage_handler_course1 ---- */

void stage_handler_course1(int *param_1)

{
  if (*param_1 == 0x15) {
                    
                    
    /* course handler dispatch */;
    return;
  }
  return;
}

/* ---- stage_handler_course2 ---- */

void stage_handler_course2(int *param_1)

{
  if (*param_1 == 0x10) {
                    
                    
    /* course handler dispatch */;
    return;
  }
  return;
}

/* ---- stage_handler_course3 ---- */

void stage_handler_course3(int *param_1)

{
  if (*param_1 == 10) {
                    
                    
    /* course handler dispatch */;
    return;
  }
  return;
}

/* ---- stage_objects_load_list ---- */

/* ROM 0x032F30: load animation set `idx` into the player rig -- the BE32 list
 * of lists at 0x1532F8[idx], each loaded by FUN_000268ce starting at node
 * 0xE0AB04 and chaining through the returned node. The argument is a 16-bit
 * index (`move.w $8(a7),d0`); the transpile read it as a host pointer. */
void stage_objects_load_list(int idx)
{
  /* ROM 0x032F30: `move.w $8(a7),d0` (a WORD) indexes a table of BE32 ROM
   * pointers at 0x1532F8 (`$1532f8(d0.w*4)`); each entry is a NULL-terminated
   * list of node-list pointers, loaded one after another into the chain at
   * 0xE0AB04 by 0x0268CE. The transpile treated the index as a host pointer
   * and dereferenced it -- the intermission's SIGSEGV. */
  extern intptr_t anim_load_list(intptr_t a1, uint32_t d1);
  uint32_t a2 = vrd32(0x1532F8 + (uint32_t)(int16_t)(uint32_t)idx * 4);
  intptr_t node = 0xAB04;
  int guard = 0;
  while (a2 != 0 && a2 + 4 <= ROM_SIZE && vrd32(a2) != 0 && guard++ < 32) {
    node = anim_load_list(node, vrd32(a2));
    a2 += 4;
  }
}

/* ---- stage_start_debug_camera ---- */

uint16_t stage_start_debug_camera(void)

{
  uint16_t uVar1;
  uint16_t uVar2;
  int iVar3;
  undefined2 uVar4;
  
  if ((W[0x2B3C] & 8) != 0) {
    if ((W[0x2B40] & 8) == 0) {
      W[0x1672C] = W[0x1672C] + 1;
    }
    else {
      W[0x1672C] = W[0x1672C] + 8;
    }
  }
  if ((W[0x2B3C] & 0x800) != 0) {
    if ((W[0x2B40] & 0x800) == 0) {
      W[0x1672C] = W[0x1672C] + -1;
    }
    else {
      W[0x1672C] = W[0x1672C] + -8;
    }
  }
  if (W[0x1672C] < 0) {
    W[0x1672C] = 0;
  }
  if ((W[0x2B3A] & 0x200) != 0) {
    W[0x16750] = W[0x16750] ^ 1;
  }
  if ((W[0x2B3A] & 0x100) != 0) {
    W[0x16752] = W[0x16752] ^ 1;
  }
  if (W[0x16750] == 0) {
    if ((W[0x2B38] & 0x20) != 0) {
      W[0x16730] = W[0x16730] - W[0x1674C];
    }
    if ((W[0x2B38] & 0x10) != 0) {
      W[0x16730] = W[0x1674C] + W[0x16730];
    }
    if ((W[0x2B38] & 0x80) != 0) {
      W[0x16734] = W[0x1674C] + W[0x16734];
    }
    if ((W[0x2B38] & 0x40) != 0) {
      W[0x16734] = W[0x16734] - W[0x1674C];
    }
    if ((W[0x2B38] & 4) != 0) {
      W[0x16738] = W[0x1674C] + W[0x16738];
    }
    if ((W[0x2B38] & 0x400) != 0) {
      W[0x16738] = W[0x16738] - W[0x1674C];
    }
    if (((W[0x2B3A] & 1) != 0) && (W[0x1674C] = W[0x1674C] * 10, 9999 < W[0x1674C])) {
      W[0x1674C] = 1;
    }
    if ((W[0x2B3A] & 2) != 0) {
      W[0x16730] = 0;
      W[0x16734] = 0;
      W[0x16738] = 4000;
    }
  }
  else {
    if ((W[0x2B3C] & 0x20) != 0) {
      W[0x16740] = W[0x16740] + 0x100 & 0xffff;
    }
    if ((W[0x2B3C] & 0x10) != 0) {
      W[0x16740] = W[0x16740] - 0x100 & 0xffff;
    }
    if ((W[0x2B3C] & 0x80) != 0) {
      W[0x1673C] = W[0x1673C] + 0x100 & 0xffff;
    }
    if ((W[0x2B3C] & 0x40) != 0) {
      W[0x1673C] = W[0x1673C] - 0x100 & 0xffff;
    }
    if ((W[0x2B3C] & 0x400) != 0) {
      W[0x16744] = W[0x16744] + 0x100 & 0xffff;
    }
    if ((W[0x2B3C] & 4) != 0) {
      W[0x16744] = W[0x16744] - 0x100 & 0xffff;
    }
    if ((W[0x2B3A] & 1) != 0) {
      W[0x16748] = (W[0x16748] + 1) % 6;
    }
    if ((W[0x2B3A] & 2) != 0) {
      W[0x1673C] = 0;
      W[0x16740] = 0;
      W[0x16744] = 0;
    }
  }
  if (W[0x16752] == 0) {
    g_sys.videomix[0x0008] = 0;
    g_sys.videomix[0x0009] = 0;
    g_sys.videomix[0x000A] = 0;
  }
  else {
    g_sys.videomix[0x0008] = 0x30;
    g_sys.videomix[0x0009] = 0x30;
    g_sys.videomix[0x000A] = 0x80;
  }
  dsp_cmd_set_camera(0,W[0x1672C],W[0x16730],(short)W[0x16734],(short)W[0x16738],
                     (short)W[0x1673C],(short)W[0x16740],(short)W[0x16744],(short)W[0x16748]
                    );
  text_print_string(1,3,0x3a2e6, 0);
  text_draw_signed_decimal(8,3,6,(short)W[0x1672C], 0);
  text_print_string(1,4,0x3a2ee, 0);
  text_draw_hex_digits(8,4,6,(short)W[0x16730], 0);
  text_print_string(1,5,0x3a2f6, 0);
  text_draw_hex_digits(8,5,6,(short)W[0x16734], 0);
  text_print_string(1,6,0x3a2fe, 0);
  text_draw_hex_digits(8,6,6,(short)W[0x16738], 0);
  text_print_string(1,7,0x3a306, 0);
  text_draw_hex_digits(8,7,4,(short)W[0x1673C], 0);
  text_print_string(1,8,0x3a30e, 0);
  text_draw_hex_digits(8,8,4,(short)W[0x16740], 0);
  text_print_string(1,9,0x3a316, 0);
  text_draw_hex_digits(8,9,4,(short)W[0x16744], 0);
  text_print_string(1,10,0x3a31e, 0);
  text_print_string(8, 10, 0, 0); /* simplified broken ROM access */
  text_print_string(1,0xb,0x3a326, 0);
  text_draw_signed_decimal(8,0xb,4,(short)W[0x1674C], 0);
  if (W[0x16750] == 0) {
    uVar4 = 0xa33e;
  }
  else {
    uVar4 = 0xa32e;
  }
  text_print_string(1,0xc,uVar4, 0);
  uVar1 = W[0x1677E];
  uVar2 = W[0x2B3A] & 8;
  if ((W[0x2B3A] & 8) != 0) {
    W[0x16754 + ((short)W[0x1677E])] = W[0x1677C];
    W[0x1677E] = W[0x1677E] + 1;
    uVar2 = uVar1;
  }
  for (iVar3 = 0; iVar3 < (short)W[0x1677E]; iVar3 = iVar3 + 1) {
    text_draw_hex_digits(0x14,(short)iVar3 + 5,4,W[0x16754 + (iVar3)], 0); uVar2 = 0;
  }
  W[0x1677C] = W[0x1677C] + 1;
  return uVar2;
}

/* ---- stage_start_debug_init ---- */

void stage_start_debug_init(void)

{
  int iVar1;
  
  W[0x1677C] = 0;
  W[0x1677E] = 0;
  iVar1 = 0;
  do {
    W[0x16754 + (iVar1)] = 0xffff;
    iVar1 = iVar1 + 1;
  } while (iVar1 < 0x14);
  sync_post();
  camera_state_reset();
  W[0x16730] = R[0x36508];
  W[0x16734] = R[0x3650C];
  W[0x16738] = R[0x36510];
  W[0x1673C] = R[0x36514];
  W[0x16740] = R[0x36518];
  W[0x16744] = R[0x3651C];
  W[0x16748] = R[0x36520];
  W[0x1672C] = 0;
  W[0x1674C] = 10;
  W[0x16750] = 0;
  W[0x16752] = 0;
  text_print_string(9,1,0x3a2ac, 0);
  g_stage_timer = 7;
  return;
}

/* ---- gameplay_timer_update ---- */

int g_test_timehold;   /* PROPCYCL_TEST_TIMEHOLD, main.c */
int g_test_pass;       /* PROPCYCL_TEST_PASS=<frames>, main.c */

uint8_t * gameplay_timer_update(void)

{
  uint8_t *puVar1;
  
  if (g_test_timehold && (int)W[0x0E44] < 0x1000) W[0x0E44] = 0x1000;
  /* PROPCYCL_TEST_PASS=<frames>: CLEAR the stage headlessly -- cap the timer
   * to <frames> once, and hold the score over the stage quota 0xE00E60, the
   * same two pokes tools/overnight/snap_story.lua makes on MAME. */
  if (g_test_pass > 0) {
    static int capped, last_t;
    if ((int)W[0x0E44] > last_t + 60) capped = 0;   /* a new stage reloaded the timer */
    if (!capped && (int)W[0x0E44] > g_test_pass) { W[0x0E44] = g_test_pass; capped = 1; }
    last_t = (int)W[0x0E44];
    if ((int)W[0x0E4C] < (int)W[0x0E60] + 500) W[0x0E4C] = W[0x0E60] + 500;
    /* NOVICE point attack has no quota: a clear is a PERFECT, i.e. the score
     * reaching the course TARGET W[0x15FE2] (FUN_0000a532's stage-end test).
     * Reach it once the capped timer is half spent, and overshoot by 2000 so
     * the result lands in the TOP band of the ladder at 0x15C478 (rank 5, the
     * only one sub 7 sends on to the ranking entry). */
    if (W16(0xE10) == 0 && capped && (int)W[0x0E44] < g_test_pass / 2 &&
        (int)W[0x15FE2] > 0 && (int)W[0x0E4C] < (int)W[0x15FE2])
      W[0x0E4C] = W[0x15FE2] + 2000;
  }
  if ((((W[0x0E18] == 0) || ((int)W[0x0E44] % 0x3c == 0)) &&
      (W[0x0E5C] = W[0x0E5C] + 1, W[0x0E30] == 0)) &&
     ((W[0x15ED4] == '\0' && (W[0x0E44] = W[0x0E44] + -1, (int)W[0x0E44] < 0)
      ))) {
    W[0x0E44] = (uint8_t *)0x0;
  }
  if ((int)W[0x0E44] < (int)W[0x0E48]) {
    W[0x0E48] = W[0x0E48] + -1;
  }
  puVar1 = W[0x0E44];
  if ((int)W[0x0E48] < (int)W[0x0E44]) {
    puVar1 = (uint8_t *)(((int)W[0x0E44] - (int)W[0x0E48]) + -4);
    if ((int)W[0x0E44] - (int)W[0x0E48] < 4) {
      W[0x0E48] = W[0x0E44];
    }
    else {
      W[0x0E48] = W[0x0E48] + 4;
    }
    if (0x1734 < (int)W[0x0E48]) {
      W[0x0E48] = 0x1734;   /* the cap is an IMMEDIATE (99 s * 60), not a ROM address -- host-pointer class */
    }
  }
  if (((int)W[0x0E5C] < 0x80) &&
     (puVar1 = (uint8_t *)(W[0x0E5C] & 0x10), puVar1 == (uint8_t *)0x0)) {
    puVar1 = (uint8_t *)sprite_draw_2d(0,0x156,0xd0,0x90,0,0x40,0x40,W[0x0E5C] << 9,0);
  }
  return puVar1;
}

/* ---- gameplay_init_state_vars @ 0x00AEB8 ---- */



int gameplay_init_state_vars(void)

{
  int iVar1;
  
  sync_post();
  camera_state_reset();
  hud_init();
  terrain_chunk_render(W[0x0E0C]);
  world_props_init_dispatch(W[0x0E0C]);
  player_init_physics();
  terrain_lod_reset();
  scene_init_and_setup();
  animated_objects_reset_all();
  balloon_system_init();
  player_model_load_animation(0);
  W[0xEB06] = 1;
  particle_system_init();
  if (propcycl_verbose()) printf("[GAMEPLAY] gameplay_init_state_vars complete\n");
  _g_system_halt = 0;
  g_scene_fade_level = 0;
  W[0x0E24] = 0;
  W[0x0E28] = 0x3c;
  W[0x0E18] = (int32_t)0xffffffff;
  W[0x0D18] = W[0x0D00];
  W[0x0D1C] = W[0x0D04];
  W[0x0D20] = W[0x0D08];
  W[0x0D4C] = 0;
  W[0x0D50] = 0;
  W[0x0D54] = 0;
  W[0x0D58] = 0;
  W[0x0D5C] = 0;
  W[0x0D60] = 0;
  W[0x0D64] = 0;
  W[0x0D68] = 0;
  W[0x0D6C] = 0;
  W[0x0D80] = 0;
  W[0x0D84] = 0;
  W[0x0D88] = 0;
  W[0x0D8C] = 0;
  W[0x0D90] = 100;
  W[0x0D94] = 0;
  W[0x0D98] = 0;
  W[0x0E00] = 0;
  W[0x0E04] = 0;
  W[0x0E08] = 0;
  W[0x0CE8] = W[0x0D0C];
  W[0x0CEC] = W[0x0D10];
  W[0x0CF0] = W[0x0D14];
  W[0x0CF4] = W[0x0D00] / 0x18000 + (W[0x0D08] / 0x18000) * 8;
  g_fog_r = 0xff;
  g_fog_g = 0xff;
  g_fog_b = 0xff;
  W[0xEB16] = 0;
  g_fog_mode = 1;
  W[0x0E38] = 0;
  W[0x1F8C] = 0x99;
  iVar1 = 0;
  do {
    W[0x130C + (iVar1 * 0x50) * 4] = 0;
    W[0x1310 + (iVar1 * 0x50) * 4] = 0;
    W[0x1314 + (iVar1 * 0x50) * 4] = 0;
    W[0x1318 + (iVar1 * 0x50) * 4] = 0;
    W[0x131C + (iVar1 * 0x50) * 4] = 0;
    W[0x1320 + (iVar1 * 0x50) * 4] = 0;
    W[0x1324 + (iVar1 * 0x50) * 4] = 0;
    W[0x1328 + (iVar1 * 0x50) * 4] = 0;
    W[0x132C + (iVar1 * 0x50) * 4] = 0;
    W[0x1330 + (iVar1 * 0x50) * 4] = 0;
    W[0x13EC + (iVar1 * 0x50) * 4] = 0;
    W[0x1424 + (iVar1 * 0x50) * 4] = 0;
    W[0x1428 + (iVar1 * 0x50) * 4] = 0;
    W[0x142C + (iVar1 * 0x50) * 4] = 0;
    W[0x1430 + (iVar1 * 0x50) * 4] = 0;
    iVar1 = iVar1 + 1;
  } while (iVar1 < 10);
  W[0x2AA8] = 0;
  W[0x2A90] = 0;
  W[0x2A9C] = 0;
  W[0x2A84] = 0;
  W[0x2A78] = 0;
  W[0x0E30] = 0;
  g_pause_speed = 0x28a;
  replay_start_recording();
  W[0x4700] = 0x3637c;
  W[0x46FC] = 0x3637c;
  W[0x46F8] = 0x3637c;
  W[0x4704] = 0;
  W16_SET(0x4708, 0);   /* ROM 0x00B0CA: move.w d0,$e04708 (16-bit) */
  iVar1 = W[0x0CBC] + -3;
  if ((iVar1 == 0) && (iVar1 = W[0x0CC0] + -2, iVar1 == 0)) {
    W[0x17034] = 0;
  }
  return iVar1;
}





/* ---- gameplay_sub5_service_run @ 0x0238F6 ---- */



uint32_t gameplay_sub5_service_run(void)

{
  /* Ghidra null ptr decl removed */
  undefined2 uVar1;
  int iVar2;
  
  W[0x3EA0] = W[0x16968];
  debug_profiler_mark(&g_sys.rom[0x3AC34]);
  objects_move_update();
  world_grid_calc_position();
  debug_profiler_mark(&g_sys.rom[0x3AC37]);
  world_render_all();
  debug_profiler_mark(&g_sys.rom[0x3AC3A]);
  bonus_camera_update();
  debug_profiler_mark(&g_sys.rom[0x3AC3D]);
  terrain_chunk_visibility(W[0x0CDC],W[0x0CE0],W[0x0CE4],W[0x0CEC]);
  debug_profiler_mark(&g_sys.rom[0x3AC40]);
  player_render();
  debug_profiler_mark(&g_sys.rom[0x3AC43]);
  terrain_props_dispatch();
  debug_profiler_mark(&g_sys.rom[0x3AC46]);
  FUN_0000e016();
  debug_profiler_mark(&g_sys.rom[0x3AC3A]);
  objects_render_master();
  bonus_sequence_animate(W[0x16968]);
  debug_profiler_mark(&g_sys.rom[0x3AC49]);
  balloon_render_and_hit_check();
  debug_profiler_mark(&g_sys.rom[0x3AC4C]);
  if (W[0x16968] / 0x3c < 3) {
    sprite_draw_2d(0,W[0x16968] / 0x3c + 0x153,0xf0,0x90,0,0x40,0x40,0,0);
  }
  if ((W[0x16968] < 0xb5) && (W[0x16968] % 0x3c == 0)) {
    sound_play_p(CONCAT22(10,(short)(W[0x16968] / -0x3c) + 0x86));
  }
  iVar2 = W[0x16968] + -1;
  W[0x16968] = iVar2;
  if (iVar2 < 0) {
    static int _printed_advance = 0;
    /* Timer expired — advance to sub=3 (gameplay_tick) when state==3.
     * Original behaviour, restored after slice 5a fixed
     * terrain_cell_find_triangle's host-pointer crashes. Mirrors the
     * pattern in stage_transition_run (line 800-811). */
    player_model_load_animation(0);
    W[0xEB06] = 1;
    W[0x0E18] = 0;
    if (W[0x0CBC] - 3 == 0) {
      W[0x0CC0] = 3;
      /* THE IN-GAME MUSIC STARTS HERE, and this call was missing -- which is
       * the whole of register row 141's "still missing: the music in slots
       * 0-3". ROM 0x023A36 is `jsr $fbf2` immediately after the same
       * `clr.l $e00e18` / `move.l #3,$e00cc0` pair above, and 0x00FBF2 is
       * sound_play_course_theme(): it reads the per-course id from the table
       * at 0x359DC (course 0 -> 0x3A -> command 0x0070, exactly the 0x8070
       * MAME holds in mailbox slot 0) and hands it to sound_play_or_defer.
       *
       * The function existed and was correct; it simply had NO CALLER in the
       * live tree. `game_ported.c` -- one of the three DEAD files -- does
       * carry the call, so the transpilation had it and the hand-edited live
       * copy of this block dropped it. */
      sound_play_course_theme();
      if (!_printed_advance) {
        if (propcycl_verbose()) printf("[GAMEPLAY] sub=5 timer expired, advancing to sub=3 (gameplay_tick) state=%d\n",
               (int)W[0x0CBC]);
        _printed_advance = 1;
      }
    } else {
      W[0x0CC0] = 0;
      if (!_printed_advance) {
        if (propcycl_verbose()) printf("[GAMEPLAY] sub=5 timer expired, advancing to sub=0 state=%d\n", (int)W[0x0CBC]);
        _printed_advance = 1;
      }
    }
  }
  uVar1 = (undefined2)((uint32_t)iVar2 >> 0x10);
  if ((W[0x2C0C] & 0x10) != 0) {
    debug_draw_value(5,6,1,((char*)(g_sys.rom + 0x3ac4f)),W16(0xE64) + 1,&g_sys.rom[0x3AC56],&g_sys.rom[0x40002]);
    debug_draw_value(3,7,1,((char*)(g_sys.rom + 0x3ac57)),W[0x15F60] + 1,&g_sys.rom[0x3AC56],&g_sys.rom[0x40002]);
    uVar1 = NULL;
  }
  if ((g_input_buttons_raw & 0x200) != 0) {
    W[0x0E44] = 600;
    g_time_display = 600;
  }
  return CONCAT22(uVar1,g_input_buttons_raw) & 0xffff0200;
}





/* ---- gameplay_vars_init @ 0x020D5E ---- */



int gameplay_vars_init(void)
{
  /* ROM 0x020D5E: the stats words are `.w`, and the four day bytes
   * 0xE1695E..61 are filled at stride ONE (`move.b #$ff,$e1695e(d1.l)`,
   * ext word 0x19B0, scale 1) -- register row 189 */
  int i;
  W[0x16944] = (int32_t)W[0x4010];
  W[0x16940] = (int32_t)W[0x4010];
  W16_SET(0x1695A, 0);
  W16_SET(0x16958, 0);
  W16_SET(0x16956, 0);
  W16_SET(0x16954, 0);
  W16_SET(0x16952, 1);
  W16_SET(0x16950, 1);
  for (i = 0; i < 4; i++) W[0x1695E + i] = -1;
  return 0;
}





/* ---- stage_transition_init @ 0x0137F4 ---- */


/* Stage transition init: set bg black, setup camera path interpolation between 3 waypoints */

void stage_transition_init(void)

{
  int iVar1;
  short sVar2;
  short sVar3;
  
  set_background_color(0,0,0);
  sVar2 = 0;
  do {
    sVar3 = 0;
    do {
      W[0x165DC + ((int)sVar3 + sVar2 * 3) * 4] = 0;
      W[0x16354 + ((int)sVar3 + sVar2 * 3) * 4] = 0;
      sVar3 = sVar3 + 1;
    } while (sVar3 < 3);
    sVar2 = sVar2 + 1;
  } while (sVar2 < 9);
  /* ROM 0x013842..0x013880: nine `move.l #imm` -- the spring chain's end
   * points and midpoint in WORLD UNITS. The transpile made six of them host
   * pointers (&g_sys.rom[0x520F3] ...), so the final stage's fly-in started
   * the player ~2e7 units out of the world (register row 185). */
  W[0x1645C] = 0x520f3;
  W[0x16460] = 0x15410;
  W[0x16464] = 0x89fb9;
  W[0x1648C] = 0x28e4f;
  W[0x16490] = 0x1eafe;
  W[0x16494] = 0x7b9c9;
  W[0x1642C] = 0x2d312;
  W[0x16430] = 0x1ec62;
  W[0x16434] = 0x9106e;
  sVar2 = 1;
  do {
    sVar3 = 0;
    do {
      iVar1 = (int)sVar2 * ((int)W[0x1645C + (sVar3) * 4] - W[0x1642C + (sVar3) * 4]);
      if (iVar1 < 0) {
        iVar1 = iVar1 + 3;
      }
      W[0x1642C + (sVar2 * 3 + (int)sVar3) * 4] = W[0x1642C + (sVar3) * 4] + (iVar1 >> 2);
      iVar1 = (int)sVar2 * ((int)W[0x1645C + (sVar3) * 4] - W[0x1648C + (sVar3) * 4]);
      if (iVar1 < 0) {
        iVar1 = iVar1 + 3;
      }
      W[0x1648C + (sVar2 * -3 + (int)sVar3) * 4] = W[0x1648C + (sVar3) * 4] + (iVar1 >> 2);
      sVar3 = sVar3 + 1;
    } while (sVar3 < 3);
    sVar2 = sVar2 + 1;
  } while (sVar2 < 4);
  sVar2 = 0;
  do {
    W[0x1324 + (sVar2 * 0x50) * 4] = 0;
    sVar2 = sVar2 + 1;
  } while (sVar2 < 4);
  g_transition_frame = 0;
  W[0x1664C] = 0;
  W[0x16650] = 0;
  W[0x0D0C] = 0;
  W[0x12BC] = 0;
  W[0x0E0C] = 0;
  world_props_init_dispatch(0);
  W[0x0E38] = 0;
  W[0x1F8C] = 0x99;
  W[0x12D8] = 0;
  g_fog_mode = 3;
  W[0xEB16] = 0xff;
  g_fog_b = 0;
  g_fog_g = 0;
  g_fog_r = 0;
  W[0x0CC0] = 0x13;
  stage_transition_run();
  return;
}





/* ---- state_gameplay_run @ 0x00AA4C ---- */

void state_gameplay_run(void)

{
  /* Gameplay sub-state dispatch (replaces ROM jump table at 0x35164)
   * W[0x0CC0] = current sub-state, set by state_gameplay_init to 0x14 (countdown) */
  name_entry_test_score();             /* PROPCYCL_TEST_HISCORE, a no-op otherwise */
  switch ((short)W[0x0CC0]) {
    case 0:  state_bonus_init(); break;
    case 1:  state_bonus_run(); break;
    case 2:  gameplay_sub_init(); break;
    case 3:  gameplay_tick(); break;
    case 4:  gameplay_sub4_service_init(); break;
    case 5:  gameplay_sub5_service_run(); break;
    case 6:  gameplay_sub6_init(); break;
    case 7:  gameplay_sub7_run(); break;
    case 8:  gameplay_sub8_init(); break;
    case 9:  gameplay_sub9_results_init(); break;
    case 10: gameplay_sub10_results_run(); break;
    case 11: gameplay_sub11_run(); break;
    case 12: gameplay_sub12_bonus_check_init(); break;
    case 13: gameplay_sub13_bonus_check_run(); break;
    case 14: gameplay_sub14_init(); break;
    case 15: gameplay_sub15_run(); break;
    case 16: gameplay_sub16_init(); break;
    case 17: gameplay_sub17_run(); break;
    case 18: stage_transition_init(); break;
    case 19: stage_transition_run(); break;
    case 20: gameplay_sub20_countdown_init(); break;
    case 21: gameplay_sub21_countdown_run(); break;
    case 22: gameplay_sub22_init(); break;
    case 23: gameplay_sub23_run(); break;
    case 24: gameplay_sub24_gameover_init(); break;
    case 25: gameplay_sub25_gameover_run(); break;
    case 26: gameplay_sub26_init(); break;
    case 27: gameplay_sub27_run(); break;
    case 28: gameplay_sub28_init(); break;
    case 29: gameplay_sub29_run(); break;
    case 30: gameplay_sub30_init(); break;
    case 31: gameplay_sub31_run(); break;
  }
  W[0x0C8C] = W[0x0C8C] + 1;
  return;
}


/*
 * Convert analog stick to pedal/steering force. Deadzone=0x50, clamp=0xC0, scale=0xAA(170)
 */



