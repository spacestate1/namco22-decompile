/*
 * Replay System
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

/* ---- replay_camera_update @ 0x02979C ---- */
/* REWRITTEN FROM THE DISASSEMBLY (register row 161). The replay camera: it
 * keeps its own camera block at 0xE17040 -- x, y, z at +0/+4/+8 and pitch,
 * heading, roll at +0xC/+0x10/+0x14 -- chasing the player (0xE00D00) in a
 * style picked by W[0x16D88], through a PC-relative jump table at 0x029826:
 *   0/10 -> 0x029848  soft follow (x,z /64, y+0x400 /32; aim /16 /32)
 *   1/11 -> 0x0298D8  chase cam, rotated offset (0, 0x300, -0x800)
 *   2/12 -> 0x0299A6  dolly between replay path points, lerped by sin
 *   3/13 -> 0x029C34  jump to the replay path point, aim at the player
 *   4/14 -> 0x029D00  the ordinary gameplay camera (>=10: snap it behind)
 *   5/15 -> 0x029DA6  fixed at the path's first point, aim at the player
 *   6/16 -> 0x029E0C  fixed at (0x89971, 0x14C3B, 0x150414), aim /8 /16
 *   7,8,9,>16 -> 0x029ECE  ordinary camera, copied into the block
 * A mode >= 10 is the FIRST frame of a style: it snaps rather than eases,
 * then subtracts 10. The cinematic attract phase copies the block back over
 * the real camera (attract_cinematic_tick, playlist case 19, which runs modes
 * [6,4,3,0,4,2,5,1,0] from ROM 0x35458), and so does the ranking replay.
 *
 * What the transpile had wrong, all ROM-verified:
 *  - the nine-long block copies (0x029D92, 0x029EDA) went through an
 *    `undefined4 *` over the 8-byte `_W[]` slots, which moves 36 HOST bytes:
 *    x landed, half of y, and z and the three angles never did;
 *  - case 6's position was `&R[0x89971]` / `&R[0x150414]` -- host pointers --
 *    where 0x029E14/0x029E22 are `move.l #$89971` / `move.l #$150414`;
 *  - cases 2/3/5 dereferenced W[0x16D80] as a HOST pointer. It is a 68K
 *    ADDRESS: a ROM table of 16-byte [x, y, z, flag] records
 *    (replay_load_builtin: 0x15C590 + id*4), or 0xE17064 for a recorded run
 *    (state_ranking_init @0x0294C0). Case 2 also indexed it `+ idx*4` BYTES
 *    where 0x029A24 is `lea (a0, d0.l*4)` with d0 = idx<<2, i.e. idx*16. */
static int32_t rpath_rd32(uint32_t a)
{
  if (a < ROM_SIZE) return vrd32s(a);
  if (a >= 0xE00000u && a < 0xE00000u + WORK_RAM_SIZE - 4)
    return (int32_t)W[a - 0xE00000u];
  return 0;
}

/* `if (x < 0) x += (1<<n)-1; asr.l #n` -- divide rounding toward zero */
static inline int32_t rc_asr(int32_t x, int n)
{
  return (x < 0 ? x + ((1 << n) - 1) : x) >> n;
}

/* the nine longs 0xE00CDC.. <-> 0xE17040.. (x,y,z, 3 angles, cell, ...) */
void replay_cam_copy9(uint32_t dst, uint32_t src)
{
  int i;
  for (i = 0; i < 9; i++)
    W[dst + i * 4] = (int32_t)W[src + i * 4];
}

void replay_camera_update(void)

{
  int32_t d2, d3, d4, d6, d0, t;
  uint32_t ti;
  int32_t mode;
  uint32_t path = (uint32_t)W[0x16D80];

  /* 0x0297BC: the vector to the player, its heading, and its length along
   * that heading (d2) -- every case below aims from these. */
  d6 = (int32_t)W[0x0D00] - (int32_t)W[0x17040];
  d4 = (int32_t)W[0x0D04] - (int32_t)W[0x17044];
  d2 = (int32_t)W[0x0D08] - (int32_t)W[0x17048];
  d3 = (int32_t)math_atan2(d6, d2);
  ti = ((uint32_t)(-d3) >> 1 & 0x7ffe) * 2;
  d2 = ((int32_t)(vrd16s(0x20B004 + ti) >> 8) * d6 +
        (int32_t)(vrd16s(0x20B006 + ti) >> 8) * d2) >> 7;

  mode = (int32_t)W[0x16D88];
  if ((uint32_t)mode > 0x10) goto dflt;
  switch (mode) {
  case 0: case 10:                                       /* 0x029848 */
    W[0x17040] = (int32_t)W[0x17040] + rc_asr((int32_t)W[0x0D00] - (int32_t)W[0x17040], 6);
    W[0x17044] = (int32_t)W[0x17044] + rc_asr((int32_t)W[0x0D04] - (int32_t)W[0x17044] + 0x400, 5);
    W[0x17048] = (int32_t)W[0x17048] + rc_asr((int32_t)W[0x0D08] - (int32_t)W[0x17048], 6);
    d0 = (int32_t)math_atan2(d2, d4);
    W[0x1704C] = (int32_t)W[0x1704C] +
                 rc_asr((int16_t)((uint16_t)d0 - (uint16_t)W[0x1704C] + 0x4000), 4);
    W[0x17050] = (int32_t)W[0x17050] +
                 rc_asr((int16_t)((uint16_t)d3 - (uint16_t)W[0x17050]), 5);
    W[0x17054] = 0;
    break;

  case 1: case 11: {                                     /* 0x0298D8 */
    int32_t div = rc_asr((int32_t)W[0x12CC], 2) + 8, ox, oy, oz;
    W[0x1704C] = (int32_t)W[0x1704C] + (int16_t)((uint16_t)W[0x0D0C] - (uint16_t)W[0x1704C]) / div;
    div = rc_asr((int32_t)W[0x12CC], 2) + 8;
    W[0x17050] = (int32_t)W[0x17050] + (int16_t)((uint16_t)W[0x0D10] - (uint16_t)W[0x17050]) / div;
    div = rc_asr((int32_t)W[0x12CC], 2) + 8;
    W[0x17054] = (int32_t)W[0x17054] + (int16_t)((uint16_t)W[0x0D14] - (uint16_t)W[0x17054]) / div;
    rotate_euler_zxy_optimized(
        (int)vrd16s(0x20B004 + ((uint32_t)W[0x0D14] >> 1 & 0x7ffe) * 2) >> 5, 0x300, -0x800,
        (int32_t)W[0x1704C], (int32_t)W[0x17050], (int32_t)W[0x17054], &ox, &oy, &oz);
    W[0x17040] = (int32_t)W[0x0D00] + ox;
    W[0x17044] = (int32_t)W[0x0D04] + oy;
    W[0x17048] = (int32_t)W[0x0D08] + oz;
    break; }

  case 2: case 12: {                                     /* 0x0299A6 */
    int32_t last = (int32_t)W[0x15BE0] >> 8, i1, i2, half, mid, x0;
    int16_t w;
    uint32_t p1, p2;
    int k;
    i1 = ((int32_t)W[0x15BE4] >> 8) + 1;  if (last < i1) i1 = last;
    i2 = ((int32_t)W[0x15BE4] >> 8) + 2;  if (last < i2) i2 = last;
    /* 0x029A0A: cos of the frame-within-segment, >> 4 (move.w / asr.w) */
    w = (int16_t)(vrd16s(0x20B006 + ((((uint32_t)W[0x15BE4] & 0xff) << 7) >> 1 & 0x7ffe) * 2) >> 4);
    p1 = path + (uint32_t)i1 * 16;
    p2 = path + (uint32_t)i2 * 16;
    for (k = 0; k < 3; k++) {
      x0 = rpath_rd32(p1 + k * 4);
      half = rc_asr(rpath_rd32(p2 + k * 4) - x0, 1);
      mid = x0 + half;
      t = rc_asr(mid - ((int32_t)w * half >> 11) + (int32_t)W[0x0D00 + k * 4] * 3, 2);
      if (k == 1) t += 1000;
      W[0x17040 + k * 4] = t;
    }
    d6 = (int32_t)W[0x0D00] - (int32_t)W[0x17040];
    d4 = (int32_t)W[0x0D04] - (int32_t)W[0x17044];
    d2 = (int32_t)W[0x0D08] - (int32_t)W[0x17048];
    d3 = (int32_t)math_atan2(d6, d2);
    ti = ((uint32_t)(-d3) >> 1 & 0x7ffe) * 2;
    d2 = ((int32_t)(vrd16s(0x20B004 + ti) >> 8) * d6 +
          (int32_t)(vrd16s(0x20B006 + ti) >> 8) * d2) >> 7;
    if (mode < 10) {                                     /* 0x029BE6 */
      d0 = (int32_t)math_atan2(d2, d4);
      W[0x1704C] = (int32_t)W[0x1704C] +
                   rc_asr((int16_t)((uint16_t)d0 - (uint16_t)W[0x1704C] + 0x4000), 4);
      W[0x17050] = (int32_t)W[0x17050] +
                   rc_asr((int16_t)((uint16_t)d3 - (uint16_t)W[0x17050]), 5);
      W[0x17054] = 0;
    }
    else {                                               /* 0x029B80 */
      W[0x17040] = rpath_rd32(path + 0x10);
      W[0x17044] = rpath_rd32(path + 0x14);
      W[0x17048] = rpath_rd32(path + 0x18);
      d4 = (int32_t)W[0x0D04] - (int32_t)W[0x17044];
      d2 = (int32_t)W[0x0D08] - (int32_t)W[0x17048];
      d3 = (int32_t)math_atan2(d6, d2);    /* d6 NOT recomputed: as the ROM */
      d0 = (int32_t)math_atan2(d2, d4);
      W[0x1704C] = 0x4000 + (int16_t)d0;
      W[0x17050] = (int16_t)d3;
      W[0x17054] = 0;
      W[0x16D88] = mode - 10;
    }
    break; }

  case 3: case 13: {                                     /* 0x029C34 */
    uint32_t p = path + (uint32_t)((int32_t)W[0x15BE4] >> 8) * 16;
    if ((((int32_t)W[0x15BE0] >> 8) << 8) > (int32_t)W[0x15BE4]) {
      W[0x17040] = rpath_rd32(p);
      W[0x17044] = rpath_rd32(p + 4);
      W[0x17048] = rpath_rd32(p + 8);
    }
    if (mode < 10) {                                     /* 0x029CB2 */
      d0 = (int32_t)math_atan2(d2, d4);
      W[0x1704C] = (int32_t)W[0x1704C] +
                   rc_asr((int16_t)((uint16_t)d0 - (uint16_t)W[0x1704C] + 0x4000), 4);
      W[0x17050] = (int32_t)W[0x17050] +
                   rc_asr((int16_t)((uint16_t)d3 - (uint16_t)W[0x17050]), 5);
      W[0x17054] = 0;
    }
    else {                                               /* 0x029C6C */
      d0 = (int32_t)math_atan2(d2, d4);
      W[0x1704C] = 0x4000 + (int16_t)d0;
      W[0x17050] = (int16_t)d3;
      W[0x17054] = 0;
      /* 0x029C88: recomputes the vector and calls atan2 once more; the
       * result is discarded (lea $10(a7),a7). Kept for fidelity. */
      (void)math_atan2((int32_t)W[0x0D00] - (int32_t)W[0x17040],
                       (int32_t)W[0x0D08] - (int32_t)W[0x17048]);
      W[0x16D88] = mode - 10;
    }
    break; }

  case 4: case 14:                                       /* 0x029D00 */
    if (mode < 10) {                                     /* 0x029D86 */
      camera_update_wrapper();
      camera_grid_calc_position();
      replay_cam_copy9(0x17040, 0x0CDC);
    }
    else {
      int32_t ox, oy, oz;
      W[0x0CE8] = (int32_t)W[0x0D0C];
      W[0x0CEC] = (int32_t)W[0x0D10];
      W[0x0CF0] = (int32_t)W[0x0D14];
      rotate_euler_zxy_optimized(0, 0x400, -0x2480, (int32_t)W[0x0CE8], (int32_t)W[0x0CEC],
                                 (int32_t)W[0x0CF0], &ox, &oy, &oz);
      W[0x0CDC] = (int32_t)W[0x0D00] + ox;
      W[0x0CE0] = (int32_t)W[0x0D04] + oy;
      W[0x0CE4] = (int32_t)W[0x0D08] + oz;
      W[0x16D88] = mode - 10;
    }
    break;

  case 5: case 15:                                       /* 0x029DA6 */
    W[0x17040] = rpath_rd32(path);
    W[0x17044] = rpath_rd32(path + 4);
    W[0x17048] = rpath_rd32(path + 8);
    if (mode < 10) {
      d0 = (int32_t)math_atan2(d2, d4);
      W[0x1704C] = (int32_t)W[0x1704C] +
                   rc_asr((int16_t)((uint16_t)d0 - (uint16_t)W[0x1704C] + 0x4000), 4);
      W[0x17050] = (int32_t)W[0x17050] +
                   rc_asr((int16_t)((uint16_t)d3 - (uint16_t)W[0x17050]), 5);
      W[0x17054] = 0;
      break;
    }
    goto snap_aim;

  case 6: case 16:                                       /* 0x029E0C */
    W[0x17040] = 0x89971;
    W[0x17044] = 0x14C3B;
    W[0x17048] = 0x150414;
    if (mode < 10) {                                     /* 0x029E70 */
      d0 = (int32_t)math_atan2(d2, d4);
      W[0x1704C] = (int32_t)W[0x1704C] +
                   rc_asr((int16_t)((uint16_t)d0 - (uint16_t)W[0x1704C] + 0x4000), 3);
      W[0x17050] = (int32_t)W[0x17050] +
                   rc_asr((int16_t)((uint16_t)d3 - (uint16_t)W[0x17050]), 4);
      W[0x17054] = 0;
      break;
    }
  snap_aim:                                              /* 0x029E2A */
    d6 = (int32_t)W[0x0D00] - (int32_t)W[0x17040];
    d4 = (int32_t)W[0x0D04] - (int32_t)W[0x17044];
    d2 = (int32_t)W[0x0D08] - (int32_t)W[0x17048];
    d3 = (int32_t)math_atan2(d6, d2);
    d0 = (int32_t)math_atan2(d2, d4);
    W[0x1704C] = 0x4000 + (int16_t)d0;
    W[0x17050] = (int16_t)d3;
    W[0x17054] = 0;
    W[0x16D88] = mode - 10;
    break;

  default:
  dflt:                                                  /* 0x029ECE */
    camera_update_wrapper();
    camera_grid_calc_position();
    replay_cam_copy9(0x17040, 0x0CDC);
    break;
  }
  /* 0x029EEA */
  W[0x12BC] = (W[0x16D88] == 1 || W[0x16D88] == 4) ? 0 : 1;
  return;
}


/* ---- replay_start_recording ---- */

int replay_start_recording(void)

{
  short sVar1;
  int iVar2;
  undefined4 *puVar3;
  undefined4 *puVar4;
  
  iVar2 = W[0x0CBC] + -3;
  if (W[0x0CBC] + -3 == 0) {
    if (W[0x0CC0] == 2) {
      W[0x17268] = 0;
    }
    /* ROM 0x029272: the path-point flags are the +0xC long of each 16-byte
     * [x,y,z,flag] record (`lsl.l #4 ; clr.l $e17070(d0.l)`), not a
     * 4-byte array. 0x029288..0x0292C4: 64 longs 0xE00D00 -> 0xE17124,
     * 3 longs 0xE00E00 -> 0xE17224, 14 longs 0xE012C0 -> 0xE17230 --
     * `undefined4 *` walks over the 8-byte `_W[]` slots before, which
     * saved half as many slots as the ROM saves longs (row 161; the
     * restore in state_ranking_init had the same walk). */
    for (iVar2 = 0; iVar2 < 8; iVar2++)
      W[0x17070 + iVar2 * 0x10] = 0;
    for (iVar2 = 0; iVar2 < 0x40; iVar2++)
      W[0x17124 + iVar2 * 4] = (int32_t)W[0x0D00 + iVar2 * 4];
    W[0x17224] = W[0x0E00];
    W[0x17228] = W[0x0E04];
    W[0x1722C] = W[0x0E08];
    for (iVar2 = 0; iVar2 < 0xe; iVar2++)
      W[0x17230 + iVar2 * 4] = (int32_t)W[0x12C0 + iVar2 * 4];
    W[0x16D7C] = &g_replay_data;
    if (W[0x0CC0] != 0x18) {
      W[0x16D84] = 0;
    }
    W[0x15BE8] = (undefined2)W[0x0E0C];
    W[0x15BEA] = W16(0xE64);
    W[0x15BEC] = W[0x15F60];
    iVar2 = W[0x0E0C];
  }
  return iVar2;
}


/* ---- replay_record_frame ---- */

uint32_t replay_record_frame(void)

{
  int iVar1;
  int iVar2;
  uint32_t uVar3;
  
  if (W[0x0C8C] == 0) {
    replay_start_recording();
  }
  if (((W16(0xE10) == 0) || (W[0x0E0C] == 3)) && (W[0x17268] == 0)) {
    if (0x39d0 < W[0x0D48]) {
      replay_start_recording();
      W[0x17268] = 1;
    }
    if (W[0x0C8C] == 0x708) {
      replay_start_recording();
      W[0x17268] = 1;
    }
    if (W16(0xE68) == 9) {
      replay_start_recording();
      W[0x17268] = 1;
    }
  }
  if (W[0x17268] != 0) {
    if ((int)W[0x16D84] < 0x708) {
      iVar1 = W[0x16D84] * 0x10;
      *(undefined4 *)(&g_replay_data + iVar1) = W[0x0D50];
      W[0xEB64 + (iVar1)] = W[0x0D54];
      W[0xEB68 + (iVar1)] = W[0x2C04];
      W[0xEB6C + (iVar1)] = W[0x0C8C];
      if ((W[0x16D84] & 0xff) == 0) {
        iVar1 = (int)W[0x16D84] >> 8;
        iVar2 = iVar1 * 0x10;
        W[0x17064 + (iVar2)] = W[0x0D00];
        W[0x17068 + (iVar2)] = W[0x0D04];
        W[0x1706C + (iVar2)] = W[0x0D08];
        W[0x17070 + (iVar2)] = 1;   /* ROM 0x0293DE: the record's own +0xC */
      }
      W[0x16D84] = W[0x16D84] + 1;
      if ((W[0x0C98] & 0x18) != 0) {
        text_draw_rect_blink(0xf5, 0x20, 0x1a, 0x260, 13);
        goto LAB_00029424;
      }
    }
    text_draw_rect_solid(0xf5,0x20,0x1a);
  }
LAB_00029424:
  uVar3 = W[0x0C8C] & 0x3f;
  if ((uVar3 == 0) && ((int)W[0x17034] < 0xaa)) {
    /* ROM 0x029436..0x029452: two 16-bit stores into ONE word --
     * `move.w d1,$e16d8c(d0.l*4)` (hi) and `move.w d1,$e16d8e(d0.l*4)` (lo).
     * Full-slot stores at 0x16D8C and 0x16D8E clobber each other, leaving
     * x=0 and z in the wrong half -- the results map drew no trail. */
    W16_SET(0x16D8C + (W[0x17034] * 4), (short)((int32_t)W[0x0D00] >> 8));
    uVar3 = W[0x17034];
    W16_SET(0x16D8E + (W[0x17034] * 4), (short)((int32_t)W[0x0D08] >> 8));
    W[0x17034] = W[0x17034] + 1;
  }
  W[0x3EAC] = W[0x17034];
  return uVar3;
}

/* ---- replay_frame_tick @ 0x00DDBC ---- */

void replay_frame_tick(void)

{
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
  debug_profiler_mark(&g_sys.rom[0x3A27D]);
  player_bounds_check();
  debug_profiler_mark(&g_sys.rom[0x3A252]);
  objects_move_update();
  debug_profiler_mark(&g_sys.rom[0x3A280]);
  world_grid_calc_position();
  debug_profiler_mark(&g_sys.rom[0x3A283]);
  if (300 < W[0x0C8C]) {
    world_render_all();
  }
  debug_profiler_mark(&g_sys.rom[0x3A255]);
  replay_camera_update();
  debug_profiler_mark(&g_sys.rom[0x3A258]);
  terrain_chunk_visibility(W[0x0CDC],W[0x0CE0],W[0x0CE4],W[0x0CEC]);
  debug_profiler_mark(&g_sys.rom[0x3A25B]);
  terrain_props_dispatch();
  debug_profiler_mark(&g_sys.rom[0x3A286]);
  player_render();
  debug_profiler_mark(&g_sys.rom[0x3A25E]);
  objects_render_master();
  debug_profiler_mark(&g_sys.rom[0x3A261]);
  balloon_render_and_hit_check();
  debug_profiler_mark(&g_sys.rom[0x3A289]);
  stage_camera_path_update();
  debug_profiler_mark(&g_sys.rom[0x3A28C]);
  environment_zone_tick();
  debug_profiler_mark(&g_sys.rom[0x3A25E]);
  particle_system_update();
  debug_profiler_mark(&g_sys.rom[0x3A264]);
  /* next 16-byte record (was +4: one field, not one record) */
  W[0x16D7C] = W[0x16D7C] + (((uint32_t)W[0x16D7C] < ROM_SIZE) ? 0x10 : 16);
  W[0x15BE4] = W[0x15BE4] + 1;
  return;
}





/* ---- replay_load_builtin @ 0x02A068 ---- */
/* Rewritten from the M68K machine code (MAME dasm @0x2A068) — the
 * transpiled version used the replay INDEX as a host pointer in three
 * places (W[0x16D7C]/W[0x16D80]/the player-state copy source) and read
 * the parallel ROM tables native-LE. First-ever execution (attract demo
 * phase, unlocked by the playlist >>1 fix) crashed on
 * idx=0x0A000000 = byte-swapped 10.
 *
 * Parallel ROM tables indexed by replay id:
 *   0x15C508  replay record ptr (M68K addr; +0xC -> W[0x0C8C])
 *   0x15C590  input stream ptr  -> W[0x16D80]
 *   0x15C5D4  player state block ptr (0x40 longs -> W[0x0D00..])
 *   0x15C618  position triple ptr (3 longs -> W[0x0E00..])
 *   0x15C65C  terrain flag block ptr (6 longs -> W[0x12C0..],
 *             0 -> W[0x12D8], next 7 longs -> W[0x12DC..])
 *   0x15C6A0  course -> W[0x0E0C]
 *   0x15C6E4  lo16 -> W16(0xE64);  0x15C728 lo16 -> W[0x15F60] */
void replay_load_builtin(int param_1)

{
  int i;
  uint32_t idx = (uint32_t)param_1;
  uint32_t src;

  W[0x16D7C] = (int32_t)mem_read32(0x15C508 + idx * 4);
  W[0x0E0C]  = (int32_t)mem_read32(0x15C6A0 + idx * 4);
  W16_SET(0xE64, (int16_t)mem_read32(0x15C6E4 + idx * 4));
  W[0x15F60] = (int16_t)mem_read32(0x15C728 + idx * 4);
  W[0x16D80] = (int32_t)mem_read32(0x15C590 + idx * 4);
  W[0x15BE0] = 0xa000;
  W[0x15BE4] = 0;
  sync_post();
  W[0x0C8C] = (int32_t)mem_read32((uint32_t)W[0x16D7C] + 0xc);
  W[0x12BC] = 0;
  gameplay_init_player_and_world();
  gameplay_init_state_vars();
  g_fog_r = 0;
  g_fog_g = 0;
  g_fog_b = 0;
  /* player state: 0x40 longs */
  src = mem_read32(0x15C5D4 + idx * 4);
  for (i = 0; i < 0x40; i++)
    W[0x0D00 + i * 4] = (int32_t)mem_read32(src + i * 4);
  /* position triple */
  src = mem_read32(0x15C618 + idx * 4);
  W[0x0E00] = (int32_t)mem_read32(src);
  W[0x0E04] = (int32_t)mem_read32(src + 4);
  W[0x0E08] = (int32_t)mem_read32(src + 8);
  /* terrain flags: 6 longs, a zero, 7 longs (13 sequential source longs) */
  src = mem_read32(0x15C65C + idx * 4);
  for (i = 0; i < 6; i++)
    W[0x12C0 + i * 4] = (int32_t)mem_read32(src + i * 4);
  W[0x12D8] = 0;
  for (i = 0; i < 7; i++)
    W[0x12DC + i * 4] = (int32_t)mem_read32(src + 0x18 + i * 4);
  /* camera = player position; mirror camera block to W[0x17040..] */
  W[0x0CDC] = W[0x0D00];
  W[0x0CE0] = W[0x0D04];
  W[0x0CE4] = W[0x0D08];
  for (i = 0; i < 9; i++)
    W[0x17040 + i * 4] = W[0x0CDC + i * 4];
  environment_zone_force_update();
  return;
}





