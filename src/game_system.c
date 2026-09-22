/*
 * System, Boot & Hardware
 * Auto-split from game_deps.c / game_ported.c
 */
#include "propcycl.h"
#include "ending_rd.h"

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

/* ---- entry_reset ---- */

void entry_reset(void) {
    /* On real hardware, entry_reset is the CPU reset vector — full restart.
     * In our reimplementation, stay in current mode to avoid wiping state. */
    if (W[0x0CBC] == 7 || W[0x0CBC] == 1) {
        return;  /* already in title/attract, don't reset */
    }
    W[0x0CBC] = 0;  /* go to attract_init */
}

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


/* ---- the play-statistics block (ROM 0x020C94..0x02104E) ------------------
 * The per-game bookkeeping the test mode shows: 0xE16950.. are WORDS
 * (0x16950 active, 0x16952 lap pending, 0x16954 stage open, 0x16956,
 * 0x16958 stage count, 0x1695A result, 0x1695C day), 0xE16940/44/48/4C are
 * LONGS, and 0xE1695E..0xE16961 are BYTES (the course played on each day,
 * 0xFF = none). The accumulated tables live in the EEPROM bookkeeping block
 * off 0xE03F30: longs 0xE04014/18/1C and 0xE04150 + course*4, words
 * 0xE04020/22/24, 0xE040B0 + (stages*5 + result)*2, 0xE040E2 + course*20 +
 * branch*10 + k*2 and 0xE0411E + course*16 + result*2. The transpile used a
 * whole slot for every word (a 2-mod-4 one is rebuilt by the sync every
 * frame) and for every byte, and indexed the tables by a garbage course
 * byte: the game-over tail game_stats_accumulate SIGSEGVed after the bad
 * ending. PORTED FROM THE ROM, every access at its own width. */
#define GS_INC16(off) W16_SET((off), (int16_t)(W16(off) + 1))

/* ---- game_stats_record_end @ 0x020DF2 ---- */

void game_stats_record_end(void)
{
  /* ROM 0x020DF2 */
  if (W16(0x16950) == 0 || W16(0x16954) == 0) return;
  if (W16(0xE10) != 0) {
    W16_SET(0x1695A, ((int32_t)W[0x0E60] <= (int32_t)W[0x169AC]) ? 1 : 0);   /* sle ; neg.b */
  } else {
    int16_t r;
    W[0x16948] = (int32_t)W[0x4010] - (int32_t)W[0x16948];
    r = (int16_t)((int32_t)W[0x169C4] + 1);
    if (r > 6) r = 6;
    if (r < 1) r = 1;
    W16_SET(0x1695A, r);
  }
  W16_SET(0x16954, 0);
  return;
}


/* ---- game_timer_lap @ 0x020E5E ---- */

void game_timer_lap(void)
{
  /* ROM 0x020E5E */
  if (W16(0x16952) != 0) {
    W[0x1694C] = (int32_t)W[0x4010];
    W[0x16944] = (int32_t)W[0x4010] - (int32_t)W[0x16944];
  } else {
    W16_SET(0x16956, W16(0x16956) + 1);
  }
  W16_SET(0x16958, W16(0x16958) + 1);
  W16_SET(0x16952, 0);
  return;
}


/* ---- set_background_color ---- */

void set_background_color(undefined1 param_1,undefined1 param_2,undefined1 param_3)

{
  g_sys.videomix[0x0008] = param_1;
  g_sys.videomix[0x0009] = param_2;
  g_sys.videomix[0x000A] = param_3;
  return;
}


/* ---- sync_post ---- */

void sync_post(void)

{
  int iVar1;

  g_sys.videomix[0x000D] = 0;
  vwr16(0x8A0002, 0);   /* ROM 0x021070: clr.w $8a0002 -- one word; the 32-bit
                         * write also cleared the register at 0x8A0004 */
  /* Fill textram with tile 0x0020 (space character).
   * Must be big-endian since the renderer reads (byte[0]<<8)|byte[1]. */
  for (iVar1 = 0; iVar1 < 0x2000; iVar1 += 2) {
    g_sys.textram[iVar1]     = 0x00;
    g_sys.textram[iVar1 + 1] = 0x20;
  }
  return;
}


/* ---- fog_altitude_blend @0x0287DC ----
 *
 * Runs every frame the camera's cell holds zone 0x16 (course 1's valley):
 * the CZ delta TARGET follows the camera's height between preset 6's and
 * preset 7's delta. Every field is 16-bit -- see the layout note above
 * environment_params_load in game_terrain.c; half of them sit at 2-mod-4
 * offsets and were rebuilt from their neighbours every frame while stored as
 * whole slots. The two table words are `movea.w $42(a2)` / `$4c(a2)` with
 * a2 = 0x37684, i.e. BE16 at 0x376C6 / 0x376D0 (4 and 72); the transpile read
 * the first host-native as 32 bits and the second as ONE BYTE.
 *   0x02886A: if y < 0xA800, AAF8 = 2*d4c - d42 + 0xC -- always overwritten
 *   by the y <= 0xD000 arm right after it, so it is not ported. */

#define ENVW(o)        ((int)W_A16((o), 0))
#define ENVW_SET(o, v) W_A16_SET((o), 0, (v))

void fog_altitude_blend(void)
{
  uint32_t pitch = (uint32_t)W[0x0CE8] & 0xFFFF;     /* moveq #0,d4 ; move.w d1,d4 */
  int s, y, d42, d4c;

  if (ENVW(0x16A30) != 0) {
    if (ENVW(0xAB02) < 0x1e) ENVW_SET(0xAB02, 0x1e);
    ENVW_SET(0x16A34, 0);
    ENVW_SET(0x16A30, 0);
  }
  else {
    if (ENVW(0xAB02) == 0) ENVW_SET(0xAB02, 1);
    ENVW_SET(0x16A32, ENVW(0x16A32) + (ENVW(0x16A34) - ENVW(0x16A32)) / ENVW(0xAB02));
    g_sys.videomix[0x000D] = (uint8_t)ENVW(0x16A32);
  }
  s = vrd16s(0x20B004 + ((pitch >> 1) & 0x7FFE) * 2);  /* movea.w $20b004(d4.l*2) */
  y = ((int32_t)((uint32_t)s << 9) >> 13) + (int32_t)W[0x0CE0];
  d42 = vrd16s(0x376C6);
  d4c = vrd16s(0x376D0);
  if (y > 0xD000) {
    ENVW_SET(0xAAF8, d42);
  }
  else {
    ENVW_SET(0xAAF8, (int16_t)(((d4c - d42) * 2 + 0xC) * (y - 0xD000) / -0x2800) + d42);
  }
}

/* ---- fog_altitude_fadeout @0x02890A ---- */

void fog_altitude_fadeout(void)
{
  if (ENVW(0x16A30) == 0) return;
  ENVW_SET(0x16A34, 0);
  if (ENVW(0xAB02) > 0) {
    ENVW_SET(0x16A32, ENVW(0x16A32) - ENVW(0x16A32) / ENVW(0xAB02));
    g_sys.videomix[0x000D] = (uint8_t)ENVW(0x16A32);
  }
  else {
    g_sys.videomix[0x000D] = 0;
    ENVW_SET(0x16A30, 0);
    mem_write16(0x8A0002, 0);                         /* move.w d0,$8a0002 */
  }
}

/* ---- keycus_read ---- */

undefined2 keycus_read(void)

{
  undefined2 uVar1;
  
  uVar1 = mem_read32(0x40000C);
  return uVar1;
}

/* ---- keycus_write_1 ---- */

void keycus_write_1(void)

{
  mem_write32(0x40000C, 0xa532);
  return;
}

/* ---- sync_reset ---- */

void sync_reset(void)

{
  g_sys.dspram[0x10400] = 0xffffffff;
  g_sys.dspram[0x18400] = 0xffffffff;
  return;
}

/* ---- boot_hardware_init ---- */

void boot_hardware_init(void)

{
  short sVar1;
  short sVar2;
  
  g_sys.syscon[0x1C] = 0;
  g_sys.syscon[0x16] = 0;
  mem_write32(0x700009, 0x62);
  mem_write32(0x70000A, 0x62);
  mem_write32(0x700010, 0xe0);
  mem_write32(0x70000B, 0x57);
  mem_write32(0x70000C, 0x52);
  mem_write32(0x70000E, 0x52);
  mem_write32(0x70000F, 0x71);
  mem_write32(0x70000D, 0x52);
  mem_write32(0x700011, 0x2c);
  mem_write32(0x700012, 0x50);
  mem_write32(0x700013, 0xff);
  g_sys.syscon[0x17] = 0xf;
  g_sys.syscon[0x00] = 0x24;
  W[0xAB24] = 0;
  W[0x2B38] = 0;
  W[0x2B5C] = 0;
  W[0x2B3C] = 0;
  W[0x2B60] = 0;
  g_sys.syscon[0x16] = 1;
  rom_data_copy_to_dsp();
  palette_test_init();
  cz_depth_table_init();
  g_sys.videomix[0x001B] = 0x7f;
  g_sys.syscon[0x1C] = 0;
  g_sys.dspram[0x0028] = 0;
  g_sys.dspram[0x0030] = 0;
  g_sys.dspram[0x0050] = 0;
  g_sys.dspram[0x0038] = 0xffffffff;
  g_sys.dspram[0x003C] = 0x1c;
  g_sys.dspram[0] = 0;
  g_sys.dspram[0x0004] = 1;
  g_sys.syscon[0x1C] = 1;
  mem_write32(0x860006, 0);
  mem_write32(0x860000, 0);
  sVar1 = 0xff;
  sVar2 = 0xfe;
  do {
    mem_write32(0x860002, sVar2);
    mem_write32(0x860002, sVar2);
    mem_write32(0x860002, sVar2);
    mem_write32(0x860002, sVar2);
    sVar2 = sVar2 + -1;
    sVar1 = sVar1 + -1;
  } while (sVar1 != -1);
  mem_write32(0x860006, 1);
  W[0xAB04] = 0;
  W[0xAB08] = 0;
  /* Hardware DSP init loop — skipped in reimplementation.
   * Original waits for vblank (W[0xAB26]) and DSP ready (W[0xAB12]).
   * We don't have the vblank IRQ running during init. */
  W[0xAB26] = 1;
  W[0xAB12] = 1;
  return;
}

/* ---- game_stats_reset ---- */

void game_stats_reset(void)

{
  short sVar1;
  short sVar2;
  
  W[0x4014] = 0;
  W[0x4010] = 0;
  W[0x401C] = 0;
  W[0x4018] = 0;
  W[0x4024] = 0;
  W[0x4022] = 0;
  W[0x4020] = 0;
  sVar1 = 0;
  do {
    W[0x4150 + (sVar1)] = 0;
    sVar1 = sVar1 + 1;
  } while (sVar1 < 4);
  sVar1 = 0;
  do {
    sVar2 = 0;
    do {
      W[0x411E + (sVar1 * 8 + (int)sVar2)] = 0;
      sVar2 = sVar2 + 1;
    } while (sVar2 < 8);
    sVar1 = sVar1 + 1;
  } while (sVar1 < 3);
  sVar1 = 0;
  do {
    sVar2 = 0;
    do {
      W[0x40B0 + (sVar1 * 5 + (int)sVar2)] = 0;
      sVar2 = sVar2 + 1;
    } while (sVar2 < 5);
    sVar2 = 0;
    do {
      W[0x40EC + (sVar2 * 10 + (int)sVar1)] = 0;
      W[0x40E2 + (sVar2 * 10 + (int)sVar1)] = 0;
      sVar2 = sVar2 + 1;
    } while (sVar2 < 3);
    sVar1 = sVar1 + 1;
  } while (sVar1 < 5);
  return;
}

/* ---- input_decode_buttons ---- */

void input_decode_buttons(void)

{
  int *piVar1;
  uint16_t uVar2;
  short sVar3;
  int iVar4;
  int iVar5;
  undefined4 *puVar6;
  undefined1 *puVar7;
  undefined4 auStack_c [2];
  
  sVar3 = 7;
  puVar6 = auStack_c;
  puVar7 = &R[0x3ABAA];
  do {
    *(undefined1 *)puVar6 = *puVar7;
    sVar3 = sVar3 + -1;
    puVar6 = (undefined4 *)((intptr_t)puVar6 + 1);
    puVar7 = puVar7 + 1;
  } while (sVar3 != -1);
  iVar4 = 1;
  do {
    uVar2 = W[0x2B38 + (iVar4 * 0x12)];
    W[0x2B38 + (iVar4 * 0x12)] = 0;
    *(undefined2 *)auStack_c[iVar4] = 0;
    iVar5 = 0;
    do {
      W[0x2B38 + (iVar4 * 0x12)] = W[0x2B38 + (iVar4 * 0x12)] << 1;
      W[0x2B38 + (iVar4 * 0x12)] = ~*(uint16_t *)auStack_c[iVar4] | W[0x2B38 + (iVar4 * 0x12)];
      iVar5 = iVar5 + 1;
    } while (iVar5 < 0xc);
    W[0x2B38] = 0;
    W[0x2B5C] = 0;
    W[0x2B3A + (iVar4 * 0x12)] = W[0x2B38 + (iVar4 * 0x12)] & ~uVar2;
    (*(int16_t*)(iVar4 * 0x24 + 0x2b3e)) = uVar2 & ~W[0x2B38 + (iVar4 * 0x12)];
    W[0x2B3C + (iVar4 * 0x12)] = 0;
    W[0x2B3C + (iVar4 * 0x12)] = W[0x2B3A + (iVar4 * 0x12)] | W[0x2B3C + (iVar4 * 0x12)];
    if (uVar2 == W[0x2B38 + (iVar4 * 0x12)]) {
      piVar1 = (int *)(iVar4 * 0x24 + 0x2b44);
      *piVar1 = *piVar1 + 1;
    }
    else {
      *(undefined4 *)(iVar4 * 0x24 + 0x2b44) = 0;
    }
    iVar5 = iVar4 * 0x24;
    if ((10 < *(uint32_t *)(iVar5 + 0x2b44)) &&
       (*(int *)(iVar5 + 0x2b48) = *(int *)(iVar5 + 0x2b48) + 1, 2 < *(uint32_t *)(iVar5 + 0x2b48)
       )) {
      *(undefined4 *)(iVar5 + 0x2b48) = 0;
      W[0x2B3C + (iVar4 * 0x12)] = W[0x2B38 + (iVar4 * 0x12)] | W[0x2B3C + (iVar4 * 0x12)];
    }
    iVar5 = iVar4 * 0x24;
    *(int *)(iVar5 + 0x2b4c) = *(int *)(iVar5 + 0x2b4c) + 1;
    if (W[0x2B3A + (iVar4 * 0x12)] != 0) {
      *(undefined4 *)(iVar5 + 0x2b50) = 0;
      if ((*(uint32_t *)(iVar5 + 0x2b4c) < 0x10) &&
         ((uint32_t)(uint16_t)W[0x2B3A + (iVar4 * 0x12)] == *(uint32_t *)(iVar5 + 0x2b58))) {
        *(undefined4 *)(iVar5 + 0x2b50) = 1;
      }
      iVar5 = iVar4 * 0x24;
      *(undefined4 *)(iVar5 + 0x2b4c) = 0;
      *(uint32_t *)(iVar5 + 0x2b54) = (uint32_t)(uint16_t)W[0x2B38 + (iVar4 * 0x12)];
      *(uint32_t *)(iVar5 + 0x2b58) = (uint32_t)(uint16_t)W[0x2B3A + (iVar4 * 0x12)];
    }
    iVar5 = iVar4 * 0x24;
    if (((*(int *)(iVar5 + 0x2b50) != 0) && (10 < *(uint32_t *)(iVar5 + 0x2b44))) &&
       ((uint32_t)(uint16_t)W[0x2B38 + (iVar4 * 0x12)] == *(uint32_t *)(iVar5 + 0x2b54))) {
      W[0x2B40 + (iVar4 * 0x12)] =
           W[0x2B40 + (iVar4 * 0x12)] | (uint16_t)*(undefined4 *)(iVar5 + 0x2b58);
    }
    W[0x2B40 + (iVar4 * 0x12)] =
         ~(*(int16_t*)(iVar4 * 0x24 + 0x2b3e)) & W[0x2B40 + (iVar4 * 0x12)];
    iVar4 = iVar4 + -1;
  } while (-1 < iVar4);
  return;
}

/* ---- palette_test_init ---- */

void palette_test_init(void)

{
  undefined1 uVar1;
  undefined1 uVar2;
  undefined1 uVar3;
  short sVar4;
  uint8_t bVar5;
  uint8_t bVar6;
  uint8_t bVar7;
  short sVar8;
  short sVar9;
  undefined1 *puVar10;
  uint8_t *pbVar11;
  undefined1 *puVar12;
  undefined1 *puVar13;
  uint8_t *pbVar14;
  uint8_t *pbVar15;
  undefined1 *puVar16;
  undefined1 *puVar17;
  uint8_t *pbVar18;
  uint8_t *pbVar19;
  char *pcVar20;
  undefined1 *puVar21;
  undefined1 *puVar22;
  uint8_t *pbVar23;
  uint8_t *pbVar24;
  char *pcVar25;
  int bVar26;
  
  sVar4 = 0xff;
  bVar6 = 0;
  pbVar11 = &g_sys.palette_ram[0];
  pbVar14 = &g_sys.palette_ram[0x8000];
  pbVar18 = &g_sys.palette_ram[0x10000];
  pcVar20 = &g_sys.palette_ram[0x0100];
  pcVar25 = &g_sys.palette_ram[0x8100];
  pbVar23 = &g_sys.palette_ram[0x10100];
  do {
    *pbVar11 = bVar6;
    *pbVar14 = bVar6;
    *pbVar18 = bVar6;
    *pcVar20 = bVar6 << 5;
    *pcVar25 = (bVar6 & 0x38) << 2;
    *pbVar23 = bVar6 & 0xc0;
    bVar6 = bVar6 + 1;
    sVar4 = sVar4 + -1;
    pbVar11 = pbVar11 + 1;
    pbVar14 = pbVar14 + 1;
    pbVar18 = pbVar18 + 1;
    pcVar20 = pcVar20 + 1;
    pcVar25 = pcVar25 + 1;
    pbVar23 = pbVar23 + 1;
  } while (sVar4 != -1);
  sVar4 = 0x100;
  puVar10 = &g_sys.palette_ram[0x0200];
  puVar12 = &g_sys.palette_ram[0x8200];
  puVar16 = &g_sys.palette_ram[0x10200];
  do {
    *puVar10 = 0xff;
    *puVar12 = 0xff;
    *puVar16 = 0xff;
    sVar4 = sVar4 + -1;
    puVar10 = puVar10 + 1;
    puVar12 = puVar12 + 1;
    puVar16 = puVar16 + 1;
  } while (sVar4 != -1);
  sVar4 = 0x100;
  puVar10 = &g_sys.palette_ram[0x0300];
  puVar12 = &g_sys.palette_ram[0x8300];
  puVar16 = &g_sys.palette_ram[0x10300];
  do {
    *puVar10 = 0xff;
    *puVar12 = 0;
    *puVar16 = 0;
    sVar4 = sVar4 + -1;
    puVar10 = puVar10 + 1;
    puVar12 = puVar12 + 1;
    puVar16 = puVar16 + 1;
  } while (sVar4 != -1);
  sVar4 = 0x100;
  puVar10 = &g_sys.palette_ram[0x0400];
  puVar12 = &g_sys.palette_ram[0x8400];
  puVar16 = &g_sys.palette_ram[0x10400];
  do {
    *puVar10 = 0;
    *puVar12 = 0xff;
    *puVar16 = 0;
    sVar4 = sVar4 + -1;
    puVar10 = puVar10 + 1;
    puVar12 = puVar12 + 1;
    puVar16 = puVar16 + 1;
  } while (sVar4 != -1);
  sVar4 = 0x100;
  puVar10 = &g_sys.palette_ram[0x0500];
  puVar12 = &g_sys.palette_ram[0x8500];
  puVar16 = &g_sys.palette_ram[0x10500];
  do {
    *puVar10 = 0;
    *puVar12 = 0;
    *puVar16 = 0xff;
    sVar4 = sVar4 + -1;
    puVar10 = puVar10 + 1;
    puVar12 = puVar12 + 1;
    puVar16 = puVar16 + 1;
  } while (sVar4 != -1);
  puVar10 = &g_sys.palette_ram[0x0800];
  puVar12 = &g_sys.palette_ram[0x8800];
  puVar16 = &g_sys.palette_ram[0x10800];
  puVar21 = &R[0x3F7F0];
  sVar4 = 2;
  sVar8 = 0x76;
  do {
    sVar9 = 0xff;
    uVar1 = *puVar21;
    puVar22 = puVar21 + 2;
    uVar2 = puVar21[1];
    puVar21 = puVar21 + 3;
    uVar3 = *puVar22;
    puVar22 = puVar10;
    puVar13 = puVar12;
    puVar17 = puVar16;
    do {
      puVar10 = puVar22 + 1;
      *puVar22 = uVar1;
      puVar12 = puVar13 + 1;
      *puVar13 = uVar2;
      puVar16 = puVar17 + 1;
      *puVar17 = uVar3;
      sVar9 = sVar9 + -1;
      puVar22 = puVar10;
      puVar13 = puVar12;
      puVar17 = puVar16;
    } while (sVar9 != -1);
    bVar26 = sVar4 == 0;
    sVar4 = sVar4 + -1;
    if (bVar26) {
      sVar4 = 2;
      puVar21 = &R[0x3F7F0];
    }
    sVar8 = sVar8 + -1;
  } while (sVar8 != -1);
  pbVar11 = &g_sys.palette_ram[0x7F00];
  pbVar14 = &g_sys.palette_ram[0xFF00];
  pbVar18 = &g_sys.palette_ram[0x17F00];
  pbVar23 = &R[0x3F7F9];
  sVar8 = 0xf;
  sVar4 = 2;
  do {
    bVar6 = *pbVar23;
    pbVar24 = pbVar23 + 2;
    bVar5 = pbVar23[1];
    pbVar23 = pbVar23 + 3;
    bVar7 = *pbVar24;
    sVar9 = 0xf;
    pbVar24 = pbVar11;
    pbVar15 = pbVar14;
    pbVar19 = pbVar18;
    do {
      pbVar11 = pbVar24 + 1;
      *pbVar24 = bVar6;
      pbVar14 = pbVar15 + 1;
      *pbVar15 = bVar5;
      pbVar18 = pbVar19 + 1;
      *pbVar19 = bVar7;
      bVar26 = bVar6 < 0x10;
      bVar6 = bVar6 - 0x10;
      if (bVar26) {
        bVar6 = 0;
      }
      bVar26 = bVar5 < 0x10;
      bVar5 = bVar5 - 0x10;
      if (bVar26) {
        bVar5 = 0;
      }
      bVar26 = bVar7 < 0x10;
      bVar7 = bVar7 - 0x10;
      if (bVar26) {
        bVar7 = 0;
      }
      sVar9 = sVar9 + -1;
      pbVar24 = pbVar11;
      pbVar15 = pbVar14;
      pbVar19 = pbVar18;
    } while (sVar9 != -1);
    bVar26 = sVar4 == 0;
    sVar4 = sVar4 + -1;
    if (bVar26) {
      sVar4 = 2;
      pbVar23 = &R[0x3F7F9];
    }
    sVar8 = sVar8 + -1;
  } while (sVar8 != -1);
  g_sys.videomix[0x0005] = 0;
  g_sys.videomix[0x0006] = 0;
  g_sys.videomix[0x0007] = 0;
  return;
}

/* ---- rom_data_copy_to_dsp ---- */

void rom_data_copy_to_dsp(void)

{
  /* MISNAMED, AND IT COPIED NOTHING.
   *
   * The ROM at 0x03F30C is a generic block copy taking BOTH ends in
   * registers:
   *     movem.l d7,-(a7)
   *     move.w  (a0)+,d7      ; count-1, read from the head of the source
   *     move.l  (a0)+,(a1)+   ; loop body
   *     dbra    d7,-4
   * Ghidra lost a0 AND a1, substituted NULL for the source, and commented
   * the copy itself out -- `// 0 = *(undefined4 *)psVar2;`. With `sVar1 = 0`
   * the loop then ran exactly once and did nothing, so the call at
   * boot_hardware_init was a no-op that could never fail visibly.
   *
   * Its sole caller sets both ends immediately before the jsr (ROM 0x03DDD6):
   *     lea $40746.l,a0     ; source, in program ROM
   *     lea $880000.l,a1    ; destination
   *     jsr $3f30c
   * and 0x880000 is **CGRAM**, not the DSP -- the name is wrong. The count
   * word at 0x40746 is 3071, so this is 3072 longs = 12288 bytes of initial
   * character graphics, and the data there reads as tile bitmaps
   * (FFFF0000 0000FFFF FFF00000 ...).
   *
   * Kept parameterless because there is exactly one caller in the whole ROM
   * (`jsr $3f30c` appears once, at 0x03DDE2); the two addresses are its
   * arguments, quoted above. */
  uint32_t src = 0x40746;
  int n, i;
  n = (int)vrd16(src) + 1;              /* move.w (a0)+,d7 then dbra */
  src += 2;
  for (i = 0; i < n; i++) {
    mem_write32(0x880000 + (uint32_t)i * 4, vrd32(src));
    src += 4;
  }
  return;
}

/* ---- watchdog_timer_service ---- */

void watchdog_timer_service(void)

{
  if ((short)W[0xAB24] < 0) {
    W[0xAB24] = W[0xAB24] - 1;
    if (-1 < (short)W[0xAB24]) {
      W[0xAB24] = 1;
    }
  }
  else {
    W[0xAB24] = W[0xAB24] + 1;
    if (W[0xAB24] == 8) {
      W[0xAB24] = 0x8006;
    }
  }
  mem_write32(0x430000, vrd16(0x3F7E0 + ((short)(W[0xAB24] & 7) * 2)));
  return;
}

/* ========== DSP/3D RENDERING PIPELINE (58 functions) ========== */

/* ---- watchdog_spin_forever ---- */

void watchdog_spin_forever(void)

{
  /* Original: infinite loop kicking watchdog.
   * In reimplementation: just return to avoid hanging.
   * The DSP ready check that calls this will proceed anyway. */
  printf("  [WARN] watchdog_spin_forever bypassed\n");
  return;
}

/* ---- globals_init ---- */

void globals_init(void)

{
  W[0x0C98] = 0;
  W[0x0CA0] = 0;
  _g_halt_flag = 0;
  W[0x0CAC] = 0;
  _g_system_halt = 0;
  W[0x3F10] = 0;
  W[0x3F14] = 0;
  W[0x3F18] = 0;
  W[0x3F1C] = 0;
  W[0x3F20] = 0;
  W[0x3F24] = 0;
  W[0x3F28] = 0;
  W[0x3F2C] = 0;
  W[0x2C24] = 0;
  W[0x2C28] = 0;
  W[0x2E38] = 0;
  W[0x2E3C] = 1;
  W[0x2C2C] = 1;
  W[0x2C30] = 4;
  W[0x2C34] = 0;
  W[0x3E50] = 0;
  W[0x3E54] = 0;
  W[0x3E58] = 0;
  W[0x3E5C] = 0;
  W[0x3E60] = 0;
  W[0x3E64] = 0;
  W[0x3E68] = 0;
  W[0x3E6C] = 0;
  W[0x3E90] = 0;
  W[0x3E94] = 0;
  W[0x3E98] = 0;
  W[0x3E9C] = 0;
  W[0x3EA0] = 0;
  W[0x3EA4] = 0;
  W[0x3EA8] = 0;
  W[0x3EAC] = 0;
  W[0x3ED0] = 0;
  W[0x3ED4] = 0;
  W[0x3ED8] = 0;
  W[0x3EDC] = 0;
  W[0x3EE0] = 0;
  W[0x3EE4] = 0;
  W[0x3EE8] = 0;
  W[0x3EEC] = 0;
  return;
}

/* ---- irq_hblank_ack ---- */

uint64_t irq_hblank_ack(void)

{
  /* void */;
  /* void */;
  
  g_sys.syscon[0x05] = 0;
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- irq_high_ack ---- */

uint64_t irq_high_ack(void)

{
  /* void */;
  /* void */;
  
  mem_write32(0x700008, 0);
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- irq_sci_ack ---- */

uint64_t irq_sci_ack(void)

{
  /* void */;
  /* void */;
  
  g_sys.syscon[0x06] = 0;
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- irq_unknown_ack ---- */

uint64_t irq_unknown_ack(void)

{
  /* void */;
  /* void */;
  
  g_sys.syscon[0x07] = 0;
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- irq_vblank ---- */

uint64_t irq_vblank(void)

{
  /* void */;
  /* void */;
  
  g_sys.syscon[0x04] = 0;
  keycus_write_2();
  _g_vblank_flag = 1;
  if ((((W[0x0CAC] == 0) && (_g_halt_flag == 0)) && (_g_system_halt == 0)) &&
     ((g_sys.dspram[0] & 0xffffff) == 0)) {
    if ((g_sys.dspram[0x0004] & 0xffffff) == 0) {
      _g_dsp_render_result_0 = g_sys.dspram[0x0058] & 0xffffff;
      _g_dsp_render_result_1 = g_sys.dspram[0x005C] & 0xffffff;
      g_sys.dspram[0x0010] = W[0x0CA0] ^ 1;
      W[0x0CA4] = &g_sys.dspram[0x10400] + g_sys.dspram[0x0010] * 0x8000;
      g_sys.dspram[0x0004] = 1;
      W[0x0CA0] = g_sys.dspram[0x0010];
    }
    else {
      W[0x0CA4] = &g_sys.dspram[0x10400] + W[0x0CA0] * 0x8000;
    }
  }
  W[0x4010] = W[0x4010] + 1;
  if (W[0x0CBC] == 7) {
    title_sprite_animation_update();
  }
  g_sys.commsram[0x7D00] = 0xa0;
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- main_loop ---- */

void main_loop(void)

{
  STUB_HIT(0x00BF2A, "the ROM's own frame loop; ours in main.c supersedes it");
                    
  /* infinite loop removed - return instead */
  return;
}

/* ---- sync_wait_1 ---- */

void sync_wait_1(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x021050 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. One of the six consecutive RTS entries at 0x021050-0x02105A.
   */
  return;
}

/* ---- sync_wait_2 ---- */

void sync_wait_2(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x021052 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. One of the six consecutive RTS entries at 0x021050-0x02105A.
   */
  return;
}

/* ---- sync_wait_3 ---- */

void sync_wait_3(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x021058 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. One of the six consecutive RTS entries at 0x021050-0x02105A.
   */
  return;
}

/* ---- fog_set_from_table ---- */

void fog_set_from_table(int param_1)

{
  W[0xEB16] = 0xff;
  W16_SET(0x172FC, (short)param_1);
  param_1 = param_1 * 0xc;
  /* ROM 0x02AF72: `move.l (a0,d1.l),d0` off a0 = 0x37B38 -- a BIG-ENDIAN
   * long like the two below it; the transpile read it host-native. */
  g_fog_r = (short)vrd32(0x37B38 + param_1);
  g_fog_g = (short)vrd32(0x37B3C + param_1);
  g_fog_b = (short)vrd32(0x37B40 + param_1);
  return;
}

/* ---- game_stats_accumulate @ 0x020E96 ---- */

/* THE AUDIT STATISTICS (register row 189), ROM 0x020D5E..0x02104A. Every
 * field here is a `.w` word (W16) except the longs 0xE16940/44/48/4C, and
 * 0xE1695E..0xE16961 are four BYTES (the course played on each story day,
 * 0xFF = none) kept one per _W[] slot and pinned in game_init. The transpile
 * stored the words as whole slots and indexed the bytes and the 16-bit audit
 * tables at 0xE040B0 / 0xE040E2 / 0xE0411E / 0xE04150 as whole slots, so a
 * garbage index ran off the array at GAME OVER. */
void game_stats_accumulate(void)
{
  /* ROM 0x020E96, a2 = 0xE03F30 */
  int16_t d2, d3;
  int b0;
  if (W16(0x16950) == 0) return;                       /* beq.w $2104a: no sync either */
  if (W16(0x16952) != 0) {
    W[0x16944] = (int32_t)W[0x4010] - (int32_t)W[0x16944];
  } else {
    W[0x401C] = (int32_t)W[0x401C] + ((int32_t)W[0x4010] - (int32_t)W[0x1694C]);
    W16_SET(0x16956, W16(0x16956) + 1);
  }
  W[0x4018] = (int32_t)W[0x4018] + (int32_t)W[0x16944];
  W[0x4014] = (int32_t)W[0x4014] + ((int32_t)W[0x4010] - (int32_t)W[0x16940]);
  W16_SET(0x16958, W16(0x16958) + 1);
  W16_SET(0x4020, W16(0x4020) + 1);
  W16_SET(0x4022, W16(0x4022) + W16(0x16956));
  W16_SET(0x4024, W16(0x4024) + W16(0x16958));
  W16_SET(0x16952, 0);
  W16_SET(0x16950, 0);
  b0 = (int8_t)W[0x1695E];
  if (W16(0xE10) != 0) {                                /* story run */
    int d0;
    d2 = (int16_t)(W16(0x16958) - 1);
    if (d2 >= 5) d2 = 4;
    d0 = W16(0x1695A) ? W16(0x1695C) + 1 : W16(0x1695C);
    if (d2 >= 0 && d0 >= 0 && d0 < 5) W16_SET(0x40B0 + d2 * 10 + d0 * 2, W16(0x40B0 + d2 * 10 + d0 * 2) + 1);
    if (b0 >= 0 && b0 < 4) {
      W16_SET(0x40E2 + b0 * 20, W16(0x40E2 + b0 * 20) + 1);
      if (b0 == 0) d3 = ((int8_t)W[0x1695F] == 1) ? 0 : 1;
      else         d3 = ((int8_t)W[0x1695F] == 0) ? 0 : 1;
      for (d2 = 1; d2 < 4; d2++)
        if ((int8_t)W[0x1695E + d2] >= 0) {
          int o = 0x40E2 + b0 * 20 + d3 * 10 + d2 * 2;
          W16_SET(o, W16(o) + 1);
        }
      if (W16(0x1695C) == 3 && W16(0x1695A) != 0) {
        int o = 0x40E2 + b0 * 20 + d3 * 10 + 4 * 2;
        W16_SET(o, W16(o) + 1);
      }
    }
  } else if (b0 >= 0 && b0 < 4) {                       /* 0x021010 */
    int a;
    W[0x4150 + b0 * 4] = (int32_t)W[0x4150 + b0 * 4] + (int32_t)W[0x16948];
    W16_SET(0x411E + b0 * 16, W16(0x411E + b0 * 16) + 1);
    a = W16(0x1695A);
    if (a >= 0 && a < 8) W16_SET(0x411E + b0 * 16 + a * 2, W16(0x411E + b0 * 16 + a * 2) + 1);
  }
  eeprom_sync_all();
}

/* ---- keycus_write_2 ---- */

void keycus_write_2(void)

{
  mem_write32(0x400004, 0xcd66);
  return;
}

/* ---- analog_center_read_from_mcu ---- */

int analog_center_read_from_mcu(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  
  iVar3 = 0;
  do {
    W[0x3FD0 + (iVar3)] = g_sys.commsram[0x7D0A + (iVar3)];
    iVar1 = iVar3 + 1;
    iVar2 = iVar3 + -7;
    iVar3 = iVar1;
  } while (iVar1 < 8);
  return iVar2;
}

/* ---- boot_hang_loop ---- */

void boot_hang_loop(void)

{
  g_sys.syscon[0x1C] = 0;
  g_sys.syscon[0x1C] = 1;
  /* infinite loop removed - return instead */
  return;
}

/* ---- boot_hang_loop_alt ---- */

void boot_hang_loop_alt(void)

{
  g_sys.syscon[0x1C] = 0;
  g_sys.syscon[0x1C] = 1;
  /* infinite loop removed - return instead */
  return;
}

/* ---- chipselect_palette_init ---- */

void chipselect_palette_init(void)

{
  chipselect_setup();
  chipselect_palette_upload();
  return;
}

/* ---- chipselect_palette_upload ---- */

void chipselect_palette_upload(void)

{
  short sVar1;
  int iVar2;
  uint16_t uVar3;
  int iVar4;
  uint16_t *puVar5;
  undefined1 *puVar6;
  uint16_t local_20 [14];
  
  sVar1 = 0x1b;
  puVar5 = local_20;
  puVar6 = &R[0x3ABF8];
  do {
    *(undefined1 *)puVar5 = *puVar6;
    sVar1 = sVar1 + -1;
    puVar5 = (uint16_t *)((intptr_t)puVar5 + 1);
    puVar6 = puVar6 + 1;
  } while (sVar1 != -1);
  iVar2 = 0;
  do {
    uVar3 = local_20[iVar2];
    for (iVar4 = 0; iVar4 < (int)(uint32_t)local_20[iVar2 + 1]; iVar4 = iVar4 + 1) {
      mem_write32(0x800000, uVar3 & 1 | 0x828);
      mem_write32(0x800000, uVar3 & 1 | 0x82a);
      uVar3 = uVar3 >> 1;
    }
    iVar2 = iVar2 + 2;
  } while (iVar2 < 0xe);
  mem_write32(0x800000, 0x838);
  return;
}

/* ---- chipselect_setup ---- */

void chipselect_setup(void)

{
  short sVar1;
  int iVar2;
  uint16_t uVar3;
  int iVar4;
  uint16_t *puVar5;
  undefined1 *puVar6;
  uint16_t local_44 [32];
  
  sVar1 = 0x3f;
  puVar5 = local_44;
  puVar6 = &R[0x3ABB8];
  do {
    *(undefined1 *)puVar5 = *puVar6;
    sVar1 = sVar1 + -1;
    puVar5 = (uint16_t *)((intptr_t)puVar5 + 1);
    puVar6 = puVar6 + 1;
  } while (sVar1 != -1);
  iVar2 = 0;
  do {
    uVar3 = local_44[iVar2];
    for (iVar4 = 0; iVar4 < (int)(uint32_t)local_44[iVar2 + 1]; iVar4 = iVar4 + 1) {
      mem_write32(0x800000, uVar3 & 1 | 0x828);
      mem_write32(0x800000, uVar3 & 1 | 0x82c);
      uVar3 = uVar3 >> 1;
    }
    iVar2 = iVar2 + 2;
  } while (iVar2 < 0x20);
  return;
}

/* ---- clear_work_ram ---- */

void clear_work_ram(void)

{
  int iVar1;
  undefined4 *puVar2;
  
  iVar1 = 0;
  puVar2 = &g_sys.work_ram[0];
  do {
    *puVar2 = 0;
    iVar1 = iVar1 + 1;
    puVar2 = puVar2 + 1;
  } while (iVar1 < 0x7c00);
  return;
}

/* ---- exception_nop ---- */

uint64_t exception_nop(void)

{
  /* void */;
  /* void */;
  
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- game_timer_init ---- */

void game_timer_init(void)

{
  W16_SET(0x16950, 0);
  return;
}

/* ---- mcu_init ---- */

uint8_t mcu_init(void)

{
  uint8_t bVar1;
  int iVar2;

  g_sys.syscon[0x16] = 1;
  iVar2 = 0;
  do {
    g_sys.syscon[0x14] = 0;
    iVar2 = iVar2 + 1;
  } while (iVar2 < 50000);
  g_sys.commsram[0x7D00] = 0x80;
  iVar2 = 0;
  do {
    g_sys.syscon[0x14] = 0;
    bVar1 = g_sys.commsram[0x7D01];
    if ((bVar1 & 0x80) == 0x80) break;
    iVar2 = iVar2 + 1;
  } while (iVar2 < 50000);
  iVar2 = 0;
  do {
    g_sys.syscon[0x14] = 0;
    bVar1 = g_sys.commsram[0x7D01];
    if ((bVar1 & 0x80) == 0) break;
    iVar2 = iVar2 + 1;
  } while (iVar2 < 50000);
  iVar2 = 0;
  do {
    iVar2 = iVar2 + 1;
  } while (iVar2 < 50000);
  g_sys.commsram[0x7E82] = 0xffff;
  W[0x0CB0] = 0;
  return bVar1 & 0x80;
}

/* ---- syscon_init ---- */

void syscon_init(void)

{
  mem_write32(0x700009, 0x62);
  mem_write32(0x70000A, 0x62);
  mem_write32(0x70000B, 0x57);
  mem_write32(0x70000C, 0x40);
  mem_write32(0x70000D, 0x12);
  mem_write32(0x70000E, 0x52);
  mem_write32(0x70000F, 0x72);
  mem_write32(0x700010, 0xe0);
  mem_write32(0x700011, 0x2c);
  mem_write32(0x700012, 0x50);
  mem_write32(0x700013, 0xff);
  g_sys.syscon[0x17] = 0xf;
  g_sys.syscon[0x00] = 4;
  g_sys.syscon[0x01] = 2;
  g_sys.syscon[0x02] = 3;
  g_sys.syscon[0x03] = 1;
  mem_write32(0x700008, 0);
  return;
}

/* ---- video_mixer_init ---- */

void video_mixer_init(void)

{
  video_mixer_regs_init();
  fog_params_init();
  palette_clear();
  palette_fog_init();
  cz_ram_init();
  cz_attr_init();
  return;
}

/* ---- video_mixer_regs_init ---- */

void video_mixer_regs_init(void)

{
  g_sys.videomix[0x0000] = 0xff;
  g_sys.videomix[0x0001] = 0xff;
  g_sys.videomix[0x0002] = 0xff;
  g_sys.videomix[0x0003] = 0;
  g_sys.videomix[0x0004] = 0;
  g_sys.videomix[0x0005] = 0xff;
  g_sys.videomix[0x0006] = 0xff;
  g_sys.videomix[0x0007] = 0xff;
  g_sys.videomix[0x0008] = 0;
  g_sys.videomix[0x0009] = 0;
  g_sys.videomix[0x000A] = 0;
  g_sys.videomix[0x000B] = 0x7f;
  g_sys.videomix[0x000C] = 0;
  g_sys.videomix[0x000D] = 0xff;
  g_sys.videomix[0x000E] = 0;
  g_sys.videomix[0x000F] = 0;
  g_sys.videomix[0x0010] = 0;
  g_sys.videomix[0x0011] = 0;
  g_sys.videomix[0x0012] = 0xff;
  g_sys.videomix[0x0013] = 0;
  g_sys.videomix[0x0014] = 0xf;
  g_sys.videomix[0x0015] = 0;
  g_sys.videomix[0x0016] = 0;
  g_sys.videomix[0x0017] = 0;
  g_sys.videomix[0x0018] = 0;
  g_sys.videomix[0x0019] = 0;
  g_sys.videomix[0x001A] = 3;
  g_sys.videomix[0x001B] = 0x7f;
  g_sys.videomix[0x001C] = 0;
  g_sys.videomix[0x001D] = 1;
  g_sys.videomix[0x001E] = 0;
  g_sys.videomix[0x001F] = 7;
  g_sys.videomix[0x0020] = 0;
  g_sys.videomix[0x0021] = 0;
  g_sys.videomix[0x0022] = 0;
  g_sys.videomix[0x0023] = 1;
  return;
}

/* ---- fog_params_init @ 0x022A68 ---- */

int fog_params_init(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  
  iVar3 = 0;
  do {
    W[0xEB22 + (iVar3) * 4] = 0;
    W[0xEB2C + (iVar3) * 4] = 3;
    W[0xEB36 + (iVar3) * 4] = 0;
    W[0xEB40 + (iVar3) * 4] = 0;
    W[0xEB4A + (iVar3) * 4] = 0;
    W[0xEB54 + (iVar3) * 4] = 0;
    iVar1 = iVar3 + 1;
    iVar2 = iVar3 + -3;
    iVar3 = iVar1;
  } while (iVar1 < 4);
  W[0xEB16] = 0;
  g_fog_mode = 3;
  W_SET_HI16(0xEB20, 0);   /* clr.w $e0eb20: 16-bit, the HIGH half */
  W[0xEB5E] = 0;
  return iVar2;
}




/* ---- game_stats_record_start @ 0x020DAE ---- */



void game_stats_record_start(void)
{
  /* ROM 0x020DAE */
  int16_t d;
  if (W16(0x16950) == 0) return;
  W[0x16948] = (int32_t)W[0x4010];
  W16_SET(0x16954, 1);
  d = W16(0xE66);
  if (d > 3) d = 3;
  W16_SET(0x1695C, d);
  if (d >= 0 && d < 4) W[0x1695E + d] = (int8_t)W[0x0E0C];   /* move.b d1,$e1695e(d0.w) */
  return;
}



/* ---- input_process_analog_deltas @ 0x0223B8 ---- */

/*
 * The PEDAL's per-frame pulse count, plus one more rotary counter.
 *
 * Rewritten from the M68K at 0x0223B8 — the transpilation had the strides,
 * the access widths and the shared-RAM index all wrong, so it could not
 * have produced a usable number even when called (and it was not called;
 * see game_logic.c).  The machine is:
 *
 *   0223BA  movea.l #$00E02BFC,A1
 *   0223C0  move.l  ($08,A1),$00E01304        W[0x1304] = W[0x2C04]
 *   0223CA  move.w  (6,A1,D2.l*8),(4,A1,D2.l*8)   prev = cur
 *   0223D0  movea.l #$00A0BD1A,A0
 *   0223D6  move.w  (0,A0,D2.l*2),(6,A1,D2.l*8)   cur  = MCU counter
 *   0223DE  move.w  (6,A1,D2.l*8),D0  /  move.w (4,A1,D2.l*8),D1
 *   0223E8  sub.l   D1,D0
 *   0223EA  move.l  D0,(0,A1,D2.l*8)             delta = cur - prev
 *   ...     wrap the delta into +-0x8000
 *   022412  addq.l #1,D2 ; ... blt 0223CA        two axes
 *
 * so the per-axis record is EIGHT bytes, not sixteen, and prev/cur are
 * 16-bit halves of one slot:
 *
 *   0x2BFC  delta 0 (32-bit)      <- axis 0
 *   0x2C00  prev 0 | cur 0        <- two 16-bit halves of one slot
 *   0x2C04  delta 1 (32-bit)      <- THE PEDAL COUNT, read by
 *   0x2C08  prev 1 | cur 1           player_update_prev_pos as W[0x0D80]
 *
 * The source is a FREE-RUNNING 16-bit counter in MCU shared RAM, read
 * big-endian at 0x7D1A + i*2 — the transpile indexed it as BYTES, so axis
 * 1 read the low half of axis 0's counter.  The MCU increments it once per
 * pedal photo-interrupter pulse (namcos22.cpp propcycl_state::pedal_update
 * clocks timer A3), and the game takes the per-frame difference, which is
 * why the wrap to +-0x8000 is here at all.  input.c is that counter's
 * producer on this side.
 */
int input_process_analog_deltas(void)

{
  int iVar1;
  int iVar2;
  int iVar3;

  g_terrain_lod_level = W[0x2C04];
  iVar3 = 0;
  do {
    /* prev = cur, then cur = the MCU's counter. Both 16-bit halves of the
     * slot at 0x2C00 + i*8; the counter is a big-endian 16-bit word. */
    W_SET_HI16(0x2C00 + (iVar3) * 8, W_LO16(0x2C00 + (iVar3) * 8));
    W_SET_LO16(0x2C00 + (iVar3) * 8,
               (int16_t)(((uint16_t)g_sys.commsram[0x7D1A + (iVar3) * 2] << 8) |
                          (uint16_t)g_sys.commsram[0x7D1B + (iVar3) * 2]));

    /* Both operands are zero-extended before the subtract (moveq #0 then
     * move.w), so this is a 16-bit unsigned difference widened to 32. */
    W[0x2BFC + (iVar3) * 8] =
         (int32_t)((uint32_t)(uint16_t)W_LO16(0x2C00 + (iVar3) * 8) -
                   (uint32_t)(uint16_t)W_HI16(0x2C00 + (iVar3) * 8));
    if (0x200 < (int)W[0x2BFC + (iVar3) * 8]) {
      W[0x2BFC + (iVar3) * 8] = W[0x2BFC + (iVar3) * 8] + -0x8000;
    }
    if ((int)W[0x2BFC + (iVar3) * 8] < -0x200) {
      W[0x2BFC + (iVar3) * 8] = W[0x2BFC + (iVar3) * 8] + 0x8000;
    }
    iVar1 = iVar3 + 1;
    iVar2 = iVar3 + -1;
    iVar3 = iVar1;
  } while (iVar1 < 2);
  return iVar2;
}





/* ---- vics_bank_deselect @ 0x0488B2 ---- */



void vics_bank_deselect(void)

{
  if (((short)mem_read32(0x850008) == 0x211) || ((short)mem_read32(0x850008) == 0x212)) {
    mem_write32(0x85000C, 0);
  }
  return;
}





/* ---- vics_bank_select @ 0x04887C ---- */



void vics_bank_select(void)

{
  short in_D0w;
  
  if (((short)mem_read32(0x850008) == 0x211) || ((short)mem_read32(0x850008) == 0x212)) {
    mem_write32(0x85000C,
         CONCAT22((short)((uint32_t)mem_read32(0x850008) >> 0x10),
                  *(undefined2 *)((intptr_t)&g_sys.rom[0x488E2] + in_D0w * 2)));
  }
  return;
}





