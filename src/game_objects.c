/*
 * Game Objects (Balloons, Particles)
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
static int _safety_ctr = 0;

/* Read big-endian 32-bit from ROM */
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
uint32_t animated_object_script_update(uint32_t nb);

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

/* ---- animated_objects_reset_all ---- */

void animated_objects_reset_all(void)

{
  short sVar1;
  
  /* ROM 0x014EBA..0x014EEE: `move.w #$8000,$e041c0(d0.l)` with d0 = i*0x58
   * (the `lsl #2 / add / add / add / lsl #3` chain at 0x014ECC), 16 records.
   * This had stride 0x2c -- half of 0x58, the (&W[base])[N] class -- and wrote
   * a whole slot where the machine writes a word. */
  sVar1 = 0;
  do {
    W_SET_HI16(0x41C0 + (sVar1 * 0x58), 0x8000);
    sVar1 = sVar1 + 1;
  } while (sVar1 < 0x10);
  W_SET_HI16(0x166A0, 0);                      /* clr.w $e166a0 @0x014EEE */

  /* THE PER-COURSE SEED. Ghidra lost the jump table at 0x014F0A (`move.w
   * $14f0e(pc,d0.w*2),d0 ; jmp $14f0a(pc,d0.w)` with d0 = course - 2) and left
   * an unrelated `terrain_lod_update_wrapper()` call in its place, so NOTHING
   * was ever seeded -- every one of the 16 records stayed flagged 0x8000 and
   * the birds, the bonus balloons and the rest of the animation set never
   * existed. a2 = animated_object_keyframe_update (0x014C6C), a3 = 0xB5D8C.
   * Entries: course 0 -> 0x014F10, course 1 -> 0x014F54 (none),
   * course 2 -> 0x014F2E; course 3 falls out at the `bhi` @0x014EFC. */
  /* Still gated on g_animobj while the spawned objects are validated against
   * MAME. animated_object_spawn is converted now, so lifting this is a
   * one-line change once the emitted set is checked. */
  { extern int g_aolog; if (g_aolog)
      fprintf(stderr, "[AO] reset_all: course %ld, animobj gate %d\n",
              (long)W[0x0E0C], ({extern int g_animobj; g_animobj;})); }
  { extern int g_animobj; if (g_animobj)
  switch ((int)W[0x0E0C]) {
  case 0:
    animated_object_keyframe_update(0x0B5D8C,        3, 0x10AF);  /* 0x014F10 */
    animated_object_keyframe_update(0x0B5D8C + 0x54, 4, 0x0EFD);  /* 0x014F20 */
    break;
  case 2:
    animated_object_keyframe_update(0x0B5D8C + 0xC4, 4, 0x1247);  /* 0x014F2E */
    animated_object_keyframe_update(0x0B3084,        1, 0x0564);  /* 0x014F40 */
    break;
  default:
    break;                                     /* courses 1 and 3 seed nothing */
  } }
  /* animated_object_spawn's record (0x1C bytes), field map from ROM 0x014942:
   *   0x00 long  animation-script ROM pointer (also read as a byte)
   *   0x04 word   0x06 word   0x08 word (flags, 11 accesses)   0x0a word
   *   0x0c word   0x0e word   0x10 long   0x14 long   0x18 word
   *   0x1a word  SLOT INDEX  (`move.w $1a(a2),d0` @0x01495C, then *0x58 +
   *              0xE04178) -- this file previously recorded it at 0x18. */
  /* The ROM does NOT return here: 0x014F50 falls through to the shared tail at
   * 0x014F54, so this runs for every course. The old code returned early for
   * course < 2, which was part of the same lost dispatch. */
  /* `move.w d0,$e0470a.l` @0x014F80 -- 16-bit, the LOW half of slot 0x4708. */
  W_SET_LO16(0x4708, (uint16_t)((W[0x2C0A] & 1) * 2 | W[0x2BCA] & 1 |
                                (W[0x2BC8] & 1) << 2));
  sVar1 = 0;
  do {
    W[0x166A6 + (sVar1 * 2)] = 0;
    W[0x166A4 + (sVar1 * 2)] = 0;
    sVar1 = sVar1 + 1;
  } while (sVar1 < 4);
  return;
}


/* ---- balloon_render_and_hit_check ---- */

void balloon_render_and_hit_check(void)

{
  /* balloon_system_init is no longer stubbed, but keep the early-out: a
   * course with no balloon data loaded still has nothing to walk. */
  if (W[0x15FF0] == 0 && W[0x15FEC] == 0) return;

  uint32_t uVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  short sVar5;
  short sVar6;
  short sVar7;
  short sVar8;
  short sVar9;
  short sVar10;
  short sVar11;
  uint32_t pair12;  /* was `uint32_t *puVar12` -- likewise      */
  uint32_t rec13;   /* was `int *piVar13` -- a ROM byte address */
  uint32_t uVar14;
  uint32_t uVar15;
  int iVar16;
  int iVar17;
  int iVar18;
  int iVar19;
  uint32_t uVar20;
  int iVar21;
  undefined4 *puVar22;
  undefined4 *puVar23;
  int local_5c;
  uint32_t local_58;
  short local_34;
  
  sVar5 = vrd16s(0x20B004 + (-W[0x0D10] >> 1 & 0x7ffe) * 2);
  sVar6 = vrd16s(0x20B006 + (-W[0x0D10] >> 1 & 0x7ffe) * 2);
  sVar7 = vrd16s(0x20B004 + ((uint32_t)-W[0x0CEC] >> 1 & 0x7ffe) * 2);
  sVar8 = vrd16s(0x20B006 + ((uint32_t)-W[0x0CEC] >> 1 & 0x7ffe) * 2);
  sVar9 = vrd16s(0x20B004 + ((uint32_t)-W[0x0CE8] >> 1 & 0x7ffe) * 2);
  sVar10 = vrd16s(0x20B006 + ((uint32_t)-W[0x0CE8] >> 1 & 0x7ffe) * 2);
  iVar18 = vrd32s((uint32_t)W[0x15FF4] + (uint32_t)W[0x0D24] * 8);
  iVar19 = vrd32s((uint32_t)W[0x15FF4] + 4 + (uint32_t)W[0x0D24] * 8);
  iVar17 = 0;
  _safety_ctr = 0;
  do {
    if (iVar19 <= iVar17) {
      puVar23 = (int32_t *)W[0x0CA4];
      for (iVar18 = 0; iVar18 < W[0x15FEC]; iVar18 = iVar18 + 1) {
        /* DSP CURSOR BOUNDS, checked before every object rather than once:
         * this function advances puVar23 by 0xf..0x1a words per object with
         * no check of its own. Latent until the chunk-list stride fix let it
         * walk every visible chunk; ASAN then caught a write past g_sys.
         * Same guard as dsp_cmd_ptr(), register row 19. */
        { const uint8_t *lo = g_sys.dspram, *hi = g_sys.dspram + DSPRAM_SIZE;
          if ((const uint8_t *)puVar23 < lo ||
              (const uint8_t *)(puVar23 + 0x40) > hi) {
              static int warned;
              if (warned++ == 0)
                  fprintf(stderr, "[GUARD] balloon_render_and_hit_check: DSP "
                                  "cursor full -- remaining objects dropped\n");
              break;
          } }
        if ((&g_chunk_lookup_table)[(&g_obj_grid_cell)[iVar18 * 0x3c]] != -1) {
          if ((&g_obj_active_flag)[iVar18 * 0x3c] == 0) {
            /* FULL 32-bit positions. ROM 0x01170C..0x011718 pushes
             * `move.l ($28,A3,D0.l)`, `($24,..)`, `($20,..)` -- three longs
             * from the object record -- and balloon_proximity_test @0x011D7E
             * reads them as longs (`move.l ($c,A7),D4` ...). The (short) casts
             * Ghidra put here truncated 200k-1M world coordinates to 16 bits,
             * so the proximity test compared garbage and a balloon could never
             * pop (register row 98). */
            iVar19 = balloon_proximity_test
                               ((int)(&g_obj_pos_x)[iVar18 * 0x3c],
                                (int)(&g_obj_pos_y)[iVar18 * 0x3c],
                                (int)(&g_obj_pos_z)[iVar18 * 0x3c]);
            if (iVar19 == -1) {
              if ((int)W[0x4924 + (iVar18 * 0x3c)] < 0) {
                W[0x0E44] = W[0x0E44] + W[0x4924 + (iVar18 * 0x3c)] * -0x3c;
              }
              else {
                W[0x0E4C] = W[0x4924 + (iVar18 * 0x3c)] + W[0x0E4C];
                W16_SET(0xE68, W16(0xE68) + -1);
              }
              (&g_obj_active_flag)[iVar18 * 0x3c] = (&g_obj_active_flag)[iVar18 * 0x3c] + 1;
              W[0x4944 + (iVar18 * 0x3c)] = (int32_t)0xffffffff;
              sound_play(0x05);
              sound_play_pop_extra();
            }
            else {
              W[0x4944 + (iVar18 * 0x3c)] = iVar19;
            }
          }
          if ((&g_obj_active_flag)[iVar18 * 0x3c] == 0) {
            *puVar23 = 0x8010;
            puVar23[1] = 3;
            puVar23[2] = W[0x4928 + (iVar18 * 0x3c)];
            puVar23[3] = 0xffffffff;
            iVar19 = (int32_t)vrd32(0x35CD0 + (W[0x490C + (iVar18 * 0x3c)] * 4));
            if (((int)W[0x4924 + (iVar18 * 0x3c)] < 0) && ((W[0x0C8C] & 0x2c) == 0)) {
              iVar19 = 0x2e3;
            }
            if ((int)(&g_obj_angle_delta)[iVar18 * 0x3c] < 1) {
              iVar17 = -0x4000;
            }
            else {
              iVar17 = 0x4000;
            }
            uVar20 = iVar17 - (&g_obj_angle)[iVar18 * 0x3c];
            puVar23[4] = ((int)W[0x0C8C] >> 3 & 3U) + iVar19;
            puVar23[5] = (&g_obj_pos_x)[iVar18 * 0x3c] - W[0x0CDC];
            puVar23[6] = (&g_obj_pos_y)[iVar18 * 0x3c] - W[0x0CE0];
            puVar23[7] = (&g_obj_pos_z)[iVar18 * 0x3c] - W[0x0CE4];
            puVar23[8] = 0;
            puVar23[9] = 0x7fff;
            puVar23[10] = (int)(int16_t)vrd16s(0x20B004 + (uVar20 >> 1 & 0x7ffe) * 2);
            puVar23[0xb] = (int)(int16_t)vrd16s(0x20B006 + (uVar20 >> 1 & 0x7ffe) * 2);
            puVar23[0xc] = 0;
            puVar23[0xd] = 0x7fff;
            puVar23[0xe] = 0;
            puVar23[0xf] = 0x2c2;
            puVar23[0x10] = (&g_obj_pos_x)[iVar18 * 0x3c] - W[0x0CDC];
            puVar23[0x11] = (&g_obj_pos_y)[iVar18 * 0x3c] - W[0x0CE0];
            puVar23[0x12] = (&g_obj_pos_z)[iVar18 * 0x3c] - W[0x0CE4];
            puVar23[0x13] =
                 (int)(short)vrd16s(0x20B004 + ((int)(W[0x0C8C] * -0x400 & 0xffff) >> 1) * 2);
            puVar23[0x14] =
                 (int)(short)vrd16s(0x20B006 + ((int)(W[0x0C8C] * -0x400 & 0xffff) >> 1) * 2);
            puVar23[0x15] = (int)(int16_t)vrd16s(0x20B004 + (uVar20 >> 1 & 0x7ffe) * 2);
            puVar23[0x16] = (int)(int16_t)vrd16s(0x20B006 + (uVar20 >> 1 & 0x7ffe) * 2);
            puVar23[0x17] = 0;
            puVar22 = puVar23 + 0x19;
            puVar23[0x18] = 0x7fff;
            puVar23 = puVar23 + 0x1a;
            *puVar22 = 1;
          }
          else if (0 < (int)(&g_obj_active_flag)[iVar18 * 0x3c]) {
            *puVar23 = 0x8010;
            puVar23[1] = 3;
            puVar23[2] = W[0x4928 + (iVar18 * 0x3c)];
            puVar23[3] = 0xffffffff;
            puVar23[4] = ((int)(&g_obj_active_flag)[iVar18 * 0x3c] >> 1) + 0x2f8;
            puVar23[5] = (&g_obj_pos_x)[iVar18 * 0x3c] - W[0x0CDC];
            puVar23[6] = (&g_obj_pos_y)[iVar18 * 0x3c] - W[0x0CE0];
            puVar23[7] = (&g_obj_pos_z)[iVar18 * 0x3c] - W[0x0CE4];
            puVar23[8] = 0;
            puVar23[9] = 0x7fff;
            puVar23[10] = (int)sVar5;
            puVar23[0xb] = (int)sVar6;
            puVar23[0xc] = 0;
            puVar22 = puVar23 + 0xe;
            puVar23[0xd] = 0x7fff;
            puVar23 = puVar23 + 0xf;
            *puVar22 = 0;
            iVar19 = (&g_obj_active_flag)[iVar18 * 0x3c];
            (&g_obj_active_flag)[iVar18 * 0x3c] = iVar19 + 1;
            if (0xf < iVar19 + 1) {
              (&g_obj_active_flag)[iVar18 * 0x3c] = 0x10 - W[0x0C8C];
            }
          }
        }
      }
      local_58 = 0x28;
      if (g_visible_chunk_count < 0x29) {
        local_58 = g_visible_chunk_count;
      }
      if (W[0x0E0C] == 3) {
        local_58 = g_visible_chunk_count + 1;
      }
      local_5c = 0;
      _safety_ctr = 0;
      do {
        if ((int)local_58 <= local_5c) {
          *puVar23 = 0x8010;
          puVar23[1] = 0xffffffff;
          W[0x0CA4] = puVar23 + 2;
          return;
        }
        if (local_5c < 10) {
          iVar18 = 0;
        }
        else if (local_5c < 0x14) {
          iVar18 = 1;
        }
        else {
          iVar18 = 2;
        }
        uVar14 = (int)W[0x0C8C] >> 3 & 3;
        /* ROM byte address: the spawn-list pair [first index, count],
         * two BE32 words. Was a host pointer. */
                /* stride 0x10 -- ROM 0x0119D2 `lsl.l #4,d0` */
        /* DSP CURSOR BOUNDS. This function advances puVar23 by 0xf..0x1a
         * words per object with no check anywhere; ASAN caught a 4-byte
         * write past g_sys once the chunk-list stride fix let it actually
         * walk every visible chunk. Same guard as dsp_cmd_ptr() (row 19). */
        { const uint8_t *lo = g_sys.dspram, *hi = g_sys.dspram + DSPRAM_SIZE;
          if ((const uint8_t *)puVar23 < lo ||
              (const uint8_t *)(puVar23 + 0x40) > hi) {
              static int warned;
              if (warned++ == 0)
                  fprintf(stderr, "[GUARD] balloon_render_and_hit_check: DSP "
                                  "cursor full -- remaining objects dropped\n");
              /* RETURN, not break. The render section is the TAIL of this
               * function in the ROM: nothing between 0x0116EA and the
               * `unlk a6/rts` at 0x011D7A branches back below 0x0116EA, so
               * the machine cannot re-enter the hit-check loop above.
               * `break` did, with iVar17/iVar18/iVar19 all clobbered by the
               * render body, and the 16-bit index list read at
               * W[0x15FF8] + (iVar18+iVar17)*2 then returned garbage
               * (measured: -27199) which indexed _W[] 27k slots out of
               * bounds -- the SIGSEGV after inserting a coin. Terminate the
               * list the way the normal exit does and leave. */
              /* Only terminate if the cursor is still INSIDE the buffer.
               * It can be below it as well as past it -- an emitter that
               * left W[0x0CA4] wild puts it anywhere, and `+ 2 <= hi` is
               * true for every such address. Writing the terminator through
               * one is how this guard turned a dropped object into a
               * SIGSEGV of its own. */
              if ((const uint8_t *)puVar23 >= lo &&
                  (const uint8_t *)(puVar23 + 2) <= hi) {
                  *puVar23 = 0x8010;
                  puVar23[1] = 0xffffffff;
                  W[0x0CA4] = puVar23 + 2;
              }
              return;
          } }
        pair12 = (uint32_t)W[0x0E6C + local_5c * 0x10] * 8 + (uint32_t)W[0x16004];
        /* The pair address is chunk_id*8 off a ROM base, and the chunk id
         * comes from a list the producer fills; a stale or unwritten entry
         * puts it past the 4 MB ROM (seen: 0x400007). Skip rather than read
         * out of range -- the same failure mode dsp_cmd_ptr() is guarded
         * against in register row 19. */
        if (pair12 + 8 > (uint32_t)ROM_SIZE) { local_5c = local_5c + 1; continue; }
        uVar20 = (uint32_t)vrd32s(pair12);
        uVar1  = (uint32_t)vrd32s(pair12 + 4);
        for (iVar19 = 0; iVar19 < (int)uVar1; iVar19 = iVar19 + 1) {
          /* W_A16: 0x478C and 0x470C are int16_t arrays, stride 2 -- the
           * machine reads them with `tst.w $e0478c(d0.l*2)` @0x011580 and
           * `move.w $e0470c(d0.l*2),d0` @0x011592, and 0x480C with
           * `move.l ...,$e0480c(d0.l*4)` @0x01166C. See include/vaddr.h. */
          sVar11 = W_A16(0x478C, (int)(uVar20 - (uint32_t)W[0x15FE4]));
          /* 0x20-byte object record, 8 BE32 fields, in ROM. */
          rec13 = uVar20 * 0x20 + (uint32_t)W[0x16008];
          iVar17 = vrd32s(rec13);
          iVar2 = vrd32s(rec13 + 0x04);
          iVar21 = vrd32s(rec13 + 0x08);
          iVar16 = vrd32s(rec13 + 0x0c);
          iVar3 = vrd32s(rec13 + 0x10);
          iVar4 = vrd32s(rec13 + 0x1c);
          if (sVar11 == 0) {
            local_34 = (short)(vrd32s(rec13 + 0x18) *
                               (int)(short)vrd16s(0x20B004 + ((int)W_A16(0x470C, (int)(uVar20 - (uint32_t)W[0x15FE4])) >> 1 & 0x7ffe) * 2) >> 0xf);
            /* ROM 0x011AF4 `add.w d1,$e0470c(d0.l*2)` -- a 16-bit
             * read-modify-write, not a 32-bit one. */
            W_A16_SET(0x470C, (int)(uVar20 - (uint32_t)W[0x15FE4]),
                      (short)vrd32s(rec13 + 0x14)
                      + W_A16(0x470C, (int)(uVar20 - (uint32_t)W[0x15FE4])));
            if (W[0x0E0C] == 3) goto LAB_00011b06;
          }
          else {
LAB_00011b06:
            local_34 = 0;
          }
          iVar2 = iVar2 - W[0x0CDC];
          iVar21 = (local_34 - W[0x0CE0]) + iVar21;
          iVar16 = iVar16 - W[0x0CE4];
          if (sVar11 == 0) {
            if (W[0x0E0C] == 3) {
              iVar17 = (int32_t)vrd32(0x35C70 + (iVar18 * 0x2c));
              if (iVar18 != 2) {
                iVar17 = uVar14 + iVar17;
              }
              if ((iVar3 < 0) && ((W[0x0C8C] & 0x2c) == 0)) {
                iVar17 = (int32_t)vrd32(0x35C74 + (iVar18 * 0x2c));
              }
            }
            else {
              iVar17 = (int32_t)vrd32(0x35C4C + (iVar18 * 0x2c + iVar17 * 4));
              if ((iVar3 < 0) && ((W[0x0C8C] & 0x2c) == 0)) {
                iVar17 = (int32_t)vrd32(0x35C6C + (iVar18 * 0x2c));
              }
              if (iVar18 == 0) {
                iVar17 = uVar14 + iVar17;
              }
            }
            *puVar23 = 0x8010;
            puVar23[1] = 3;
            puVar23[2] = iVar4;
            puVar23[3] = 0xffffffff;
            puVar23[4] = iVar17;
            puVar23[5] = iVar2;
            puVar23[6] = iVar21;
            puVar23[7] = iVar16;
            if (W[0x0E0C] == 3) {
              puVar23[8] = (int)sVar9;
              puVar23[9] = (int)sVar10;
            }
            else {
              puVar23[8] = 0;
              puVar23[9] = 0x7fff;
            }
            puVar23[10] = (int)sVar7;
            puVar23[0xb] = (int)sVar8;
            puVar23[0xc] = 0;
            puVar23[0xd] = 0x7fff;
            puVar22 = puVar23 + 0xf;
            puVar23[0xe] = 0;
            if ((W[0x0E0C] == 3) && (((int)W[0x0C8C] >> 4 & 3U) == (uVar20 & 3))) {
              uVar15 = (int)W[0x0C8C] >> 1;
              *puVar22 = 0x8010;
              puVar23[0x10] = 3;
              puVar23[0x11] = iVar4;
              puVar23[0x12] = 0xffffffff;
              puVar23[0x13] = vrd32(0x35D0C + ((uVar15 & 7) * 4));
              puVar23[0x14] = iVar2;
              puVar23[0x15] = iVar21;
              puVar23[0x16] = iVar16;
              puVar23[0x17] = (int)sVar9;
              puVar23[0x18] = (int)sVar10;
              puVar23[0x19] = (int)sVar7;
              puVar23[0x1a] = (int)sVar8;
              puVar23[0x1b] = 0;
              puVar23[0x1c] = 0x7fff;
              puVar23[0x1d] = 0;
              puVar22 = puVar23 + 0x1e;
            }
          }
          else {
            puVar22 = puVar23;
            if (0 < sVar11) {
              *puVar23 = 0x8010;
              puVar23[1] = 3;
              puVar23[2] = iVar4;
              puVar23[3] = 0xffffffff;
              if (W[0x0E0C] == 3) {
                iVar17 = ((int)W_A16(0x478C, (int)(uVar20 - (uint32_t)W[0x15FE4])) >> 1) + 0x335;
              }
              else {
                iVar17 = ((int)W_A16(0x478C, (int)(uVar20 - (uint32_t)W[0x15FE4])) >> 1) + 0x2f8;
              }
              puVar23[4] = iVar17;
              puVar23[5] = iVar2;
              puVar23[6] = iVar21;
              puVar23[7] = iVar16;
              puVar23[8] = 0;
              puVar23[9] = 0x7fff;
              puVar23[10] = (int)sVar5;
              puVar23[0xb] = (int)sVar6;
              puVar23[0xc] = 0;
              puVar23[0xd] = 0x7fff;
              puVar22 = puVar23 + 0xf;
              puVar23[0xe] = 0;
              iVar17 = (int)(uVar20 - (uint32_t)W[0x15FE4]);
              /* ROM 0x011D28 `addq.w #1,(a0,d0.l*2)`, then
               * `cmpi.w #$10,d0; blt` and `move.w d1,...` @0x011D42. */
              W_A16_SET(0x478C, iVar17, W_A16(0x478C, iVar17) + 1);
              if (0xf < W_A16(0x478C, iVar17)) {
                W_A16_SET(0x478C, iVar17, 0x10 - (short)W[0x0C8C]);
              }
            }
          }
          uVar20 = uVar20 + 1;
          puVar23 = puVar22;
        }
        local_5c = local_5c + 1;
      if (++_safety_ctr > 10000) {
        /* Same reasoning as the DSP-cursor guard above: this is the tail of
         * the function, so bail out of the whole thing rather than falling
         * into the hit-check loop's dead tail. */
        fprintf(stderr, "[GUARD] balloon_render_and_hit_check: render loop "
                        "runaway at chunk %d/%d\n", local_5c, (int)local_58);
        *puVar23 = 0x8010;
        puVar23[1] = 0xffffffff;
        W[0x0CA4] = puVar23 + 2;
        return;
      } } while( true ); _safety_ctr = 0;
    }
    sVar11 = (int16_t)vrd16s((uint32_t)W[0x15FF8] + iVar18 * 2 + iVar17 * 2);
    iVar21 = (int)sVar11;
    iVar2 = W[0x16008] + iVar21 * 0x20;
    if (W_A16(0x478C, iVar21 - (int)W[0x15FE4]) == 0) {
      local_34 = (short)(vrd32s((uint32_t)W[0x16008] + 0x18 + iVar21 * 0x20) *
                         (int)(short)vrd16s(0x20B004 + ((int)W_A16(0x470C, iVar21 - (int)W[0x15FE4]) >> 1 & 0x7ffe) * 2) >> 0xf);
      if (W[0x0E0C] == 3) {
        local_34 = 0;
      }
      /* ROM 0x0115D4..0x0115F2: x = rec+4 (long), y = rec+8 + the 16-bit bob
       * `(-$30,A6)` (sign-extended, added with adda.l), z = rec+c (long); all
       * three pushed as longs. Only the BOB is 16-bit -- Ghidra cast the whole
       * sum. Same defect as the site above (register row 98). */
      iVar16 = balloon_proximity_test
                         ((int)vrd32s((uint32_t)(iVar2 + 4)),
                          (int)vrd32s((uint32_t)(iVar2 + 8)) + (int)local_34,
                          (int)vrd32s((uint32_t)(iVar2 + 0xc)));
      if (iVar16 == -1) {
        /* THE BALLOON'S POINT VALUE IS A BIG-ENDIAN LONG IN THE ROM RECORD.
         * ROM 0x01160E..0x011648: `tst.l ($10,A0)`, `move.l ($10,A0),D0 ;
         * add.l D0,$e00e4c` (score), `cmpi.l #$fa,($10,A0)`, and the
         * time-penalty branch `move.l ($10,A0),D1 ; ... sub.l D0,$e00e44`.
         * iVar2 is a ROM OFFSET (W[0x16008] is ROM_READ32(0x35BDC+course*4)),
         * so `*(int *)(iVar2 + 0x10)` was a host dereference of address
         * ~0x35Cxx -- garbage into the score (a 50-point balloon reported
         * as PERFECT the moment it popped: target 2100 <= score) or a fault. */
        int32_t pts = (int32_t)vrd32s((uint32_t)(iVar2 + 0x10));
        if (pts < 1) {
          W[0x0E44] = W[0x0E44] + pts * -0x3c;
LAB_0001164e:
          sound_play_pop_extra();
        }
        else {
          W[0x0E4C] = pts + W[0x0E4C];
          W16_SET(0xE68, W16(0xE68) + -1);
          if (0xf9 < pts) goto LAB_0001164e;
        }
        W_A16_SET(0x478C, iVar21 - (int)W[0x15FE4],
                  W_A16(0x478C, iVar21 - (int)W[0x15FE4]) + 1);
        W[0x480C + (iVar21 - (int)W[0x15FE4]) * 4] = (int32_t)0xffffffff;
        sound_play(0x05);
        if ((W[0x2C0C] & 0x10) != 0) {
          text_draw_signed_decimal(0xf,0xf,4,sVar11 - (short)W[0x15FE4], 0);
        }
      }
      else {
        W[0x480C + (iVar21 - (int)W[0x15FE4]) * 4] = iVar16;
      }
    }
    iVar17 = iVar17 + 1;
  if (++_safety_ctr > 10000) break; } while( true ); _safety_ctr = 0;
}


/* ---- balloon_system_init ---- */

/* UNSTUBBED. The stub that used to be here returned with W[0x15FEC] = 0 and,
 * critically, never loaded W[0x15FE2] -- the course's TARGET SCORE. The stage
 * ends on `W[0x15FE2] <= W[0x0E4C]` ("target <= score", FUN_0000a532), so with
 * both at zero the game awarded the goal on the first frame of play: every
 * course ended ~2 seconds after GO with a perfect result and no balloons ever
 * appeared. Both symptoms are this one function.
 *
 * The reason it was stubbed was "ROM pointer endianness"; the eight per-course
 * tables at 0x35BCC..0x35C3C hold 32-bit BE ROM ADDRESSES and every read of
 * them went through a host pointer. All are converted to vrd16s/vrd32s below,
 * and every table entry was checked to land inside the 4 MB ROM. */
void balloon_system_init(void) {
  int iVar1;
  uint32_t pair_addr;      /* was `short *psVar2` -- a ROM byte address */
  uint32_t obj_addr;       /* was `undefined4 *puVar6` -- likewise      */
  undefined2 uVar3;
  int iVar4;
  int iVar5;
  int local_1c;
  int local_18;
  int local_14;
  
  W16_SET(0xE68, 0);
  /* ROM 0x01107A is `tst.w (a0)` + `bpl` with a0 = 0xE15F60 -- a SIXTEEN-BIT
   * signed test. The slot holds 0xFFFF ("no stage chosen yet"), which as a
   * whole _W[] slot reads 65535, not -1, so this guard never fired and
   * W[0x15F60] kept 65535. Everything downstream is indexed by it:
   * pair_addr lands 65535*4 bytes past its table, the spawn count read from
   * there is garbage, and the spawn loop then walks W[0x490C + i*0x3c] over
   * W[0x16008] (the balloon record base) at i~1189 -- which is the SIGSEGV in
   * balloon_render_and_hit_check, a third route into the line rows 88 and 90
   * already visited. */
  if ((W[0x0CBC] == 3) && ((int16_t)W[0x15F60] < 0)) {
    W[0x15F60] = (short)(W[0x0C98] % vrd32(0x35CFC + (W[0x0E0C] * 4)));
    if ((W16(0xE6A) == 0) && ((W[0x0E0C] != 3 && (W[0x2C0D] != 0)))) {
      W[0x15F60] = (short)((int)(W[0x2C0D] - 1) % (int32_t)vrd32(0x35CFC + (W[0x0E0C] * 4)));
    }
    if (((W16(0xE10) != 0) && (W16(0xE64) == 0)) && (W[0x0E0C] != 3)) {
      W[0x15F60] = 0;
    }
    if (W16(0xE10) == 0) {
      W[0x15F60] = 1;
    }
  }
  /* Eight per-course tables of 32-bit BE ROM ADDRESSES. `(&R[base])[course]`
   * indexes a uint8_t array, so it read ONE BYTE of the pointer -- the
   * fix-#11 class. Verified: every entry lands inside the 4 MB ROM. */
  W[0x15FF4] = (intptr_t)vrd32(0x35C1C + (uint32_t)W[0x0E0C] * 4) +
                 (W16(0xE64) * 0x400 + W[0x15F60] * 0x100) * 4;
  W[0x15FF8] = (intptr_t)vrd32(0x35C0C + (uint32_t)W[0x0E0C] * 4);
  /* psVar2 walked a ROM address as a host short*; keep it as a ROM byte
   * ADDRESS and read through vrd16s at each use. */
  pair_addr = vrd32(0x35BEC + (uint32_t)W[0x0E0C] * 4) +
                    ((uint32_t)W[0x15F60] + W16(0xE64) * 4) * 4;
  W[0x16000] = (intptr_t)vrd32(0x35BFC + (uint32_t)W[0x0E0C] * 4);
  /* ROM table pointers: 4 tables indexed by course (W[0x0E0C]), 32-bit BE entries.
   * These store M68K ROM addresses that get dereferenced later as pointers. */
  {
    int ci = (int)W[0x0E0C];
    W[0x16004] = ROM_READ32(0x35BCC + ci * 4) +
                   (W16(0xE64) * 0x400 + W[0x15F60] * 0x100) * 4;
    W[0x16008] = ROM_READ32(0x35BDC + ci * 4);
    W[0x1600C] = ROM_READ32(0x35C2C + ci * 4) + ((int)W[0x15F60] + W16(0xE64) * 4) * 2;
    W[0x16010] = ROM_READ32(0x35C3C + ci * 4) + ((int)W[0x15F60] + W16(0xE64) * 4) * 2;
  }
  W[0x15FF0] = 0;
  iVar4 = 0;
  do {
    iVar5 = vrd32s((uint32_t)W[0x16004] + iVar4 * 8);
    if (-1 < iVar5) {
      iVar1 = vrd32s((uint32_t)W[0x16004] + iVar4 * 8 + 4);
      if (0 < iVar1) {
        W[0x15FF0] = iVar1 + W[0x15FF0];
      }
      for (local_1c = 0; local_1c < iVar1; local_1c = local_1c + 1) {
        if (0 < vrd32s((uint32_t)W[0x16008] + (local_1c + iVar5) * 0x20 + 0x10)) {
          W16_SET(0xE68, W16(0xE68) + 1);
        }
      }
    }
    iVar4 = iVar4 + 1;
  } while (iVar4 < 0x80);
  W[0x15FFC] = (intptr_t)pair_addr;
  if (W[0x0CBC] == 10) {
    iVar4 = 0;
    do {
      if (-W[0x0C8C] != (&g_obj_active_flag)[iVar4 * 0x3c] &&
          W[0x0C8C] <= (int)-(&g_obj_active_flag)[iVar4 * 0x3c]) {
        (&g_obj_active_flag)[iVar4 * 0x3c] = 0x80000000;
      }
      iVar4 = iVar4 + 1;
    } while (iVar4 < 8);
  }
  else {
    iVar4 = 0;
    do {
      (&g_obj_active_flag)[iVar4 * 0x3c] = 0x80000000;
      iVar4 = iVar4 + 1;
    } while (iVar4 < 8);
  }
  if (W[0x0E0C] != 3) {
    /* pair_addr points at a 2 x 16-bit record: [template index, count].
     * Each balloon template is 0x3C bytes = 15 BE32 fields. */
    iVar4 = vrd16s(pair_addr + 2);                       /* count     */
    obj_addr = (uint32_t)W[0x16000] + vrd16s(pair_addr) * 0x3c;  /* first     */
    W[0x15FEC] = iVar4;
    if (iVar4 != 0) {
      for (iVar5 = 0; iVar5 < iVar4; iVar5 = iVar5 + 1) {
        W[0x490C + (iVar5 * 0x3c)]           = vrd32s(obj_addr);
        (&g_obj_active_flag)[iVar5 * 0x3c]   = 0;
        (&g_obj_base_x)[iVar5 * 0x3c]        = vrd32s(obj_addr + 0x04);
        (&g_obj_base_y)[iVar5 * 0x3c]        = vrd32s(obj_addr + 0x08);
        (&g_obj_base_z)[iVar5 * 0x3c]        = vrd32s(obj_addr + 0x0c);
        (&g_obj_speed)[iVar5 * 0x3c]         = vrd32s(obj_addr + 0x10);
        (&g_obj_angle_delta)[iVar5 * 0x3c]   = vrd32s(obj_addr + 0x14);
        W[0x4924 + (iVar5 * 0x3c)]           = vrd32s(obj_addr + 0x18);
        W[0x4928 + (iVar5 * 0x3c)]           = vrd32s(obj_addr + 0x1c);
        (&g_obj_angle)[iVar5 * 0x3c] = W[0x0C8C] * (&g_obj_angle_delta)[iVar5 * 0x3c];
        W[0x4944 + (iVar5 * 0x3c)] = 0x7fffffff;
        if (0 < (int)W[0x4924 + (iVar5 * 0x3c)]) {
          W16_SET(0xE68, W16(0xE68) + 1);
        }
        obj_addr += 0x3c;
      }
    }
  }
  /* THE COURSE'S TARGET SCORE, and the reason every level used to end
   * within two seconds of GO with a "goal reached".
   *
   * W[0x1600C] / W[0x16010] hold ROM BYTE ADDRESSES (set ~60 lines above
   * from ROM_READ32(0x35C2C + course*4) plus an index), so dereferencing
   * them as host pointers is the usual class -- it left W[0x15FE2] at 0.
   * FUN_0000a532 ends the stage on `W[0x15FE2] <= W[0x0E4C]`, i.e. "target
   * <= score", and with the target 0 and the score 0 that is TRUE on the
   * first frame of gameplay: the game awarded the goal instantly.
   *
   * They are 16-BIT entries, not 32 -- the index above is scaled by 2, and
   * read as BE16 the table gives the per-course targets 2100 / 1920 / 2200
   * / 2700, which are the game's actual point goals. Read as 32 bits the
   * first entry comes out 137628650. */
  { extern int g_loop_dbg;
    if (g_loop_dbg) printf("[LOOP] balloon_system_init: course=%ld W[0x1600C]=0x%lX "
                           "W[0x16010]=0x%lX -> target=%d\n",
                           (long)W[0x0E0C], (long)W[0x1600C], (long)W[0x16010],
                           (int)vrd16s(W[0x1600C])); }
  W[0x15FE2] = vrd16s(W[0x1600C]);
  W[0x15FE4] = vrd16s(W[0x16010]);
  local_14 = 0;
  do {
    local_18 = 0;
    do {
      W[0x2C38 + (local_14 * 0x20 + local_18 * 4)] = 0;
      local_18 = local_18 + 1;
    } while (local_18 < 8);
    local_14 = local_14 + 1;
  } while (local_14 < 0x10);
  if (W[0x0CBC] == 10) {
    iVar4 = 0;
    do {
      if (-W[0x0C8C] != (int)W_A16(0x478C, iVar4) &&
          W[0x0C8C] <= -(int)W_A16(0x478C, iVar4)) {
        if (W[0x15FF0] < iVar4) {
          uVar3 = 0x8000;
        }
        else {
          uVar3 = 0;
        }
        W_A16_SET(0x478C, iVar4, uVar3);
        /* ROM 0x011416: `movea.w ([,d6.l]),a0` with d6 = 0xE16010 is a
         * MEMORY-INDIRECT read -- the BE16 at the ROM address W[0x16010]
         * holds, i.e. the same value W[0x15FE4] was loaded from. Then
         * `move.l $10(a0,d0.l*4)` with d0 = (base+i)*8 is the BE32 at
         * W[0x16008] + (base+i)*0x20 + 0x10, and `muls.w` is 16x16.
         * Both were host-pointer derefs of ROM addresses. */
        W_A16_SET(0x470C, iVar4,
             (short)W[0x0C8C] *
             (short)vrd32s((uint32_t)W[0x16008]
                           + (uint32_t)((vrd16s((uint32_t)W[0x16010]) + iVar4) * 0x20 + 0x10)));
        W[0x480C + iVar4 * 4] = 0x7fffffff;
      }
      iVar4 = iVar4 + 1;
    } while (iVar4 < 0x40);
  }
  else {
    iVar4 = 0;
    do {
      if (W[0x15FF0] < iVar4) {
        uVar3 = 0x8000;
      }
      else {
        uVar3 = 0;
      }
      W_A16_SET(0x478C, iVar4, uVar3);
      /* ROM 0x011416: `movea.w ([,d6.l]),a0` with d6 = 0xE16010 is a
       * MEMORY-INDIRECT read -- the BE16 at the ROM address W[0x16010]
       * holds, i.e. the same value W[0x15FE4] was loaded from. Then
       * `move.l $10(a0,d0.l*4)` with d0 = (base+i)*8 is the BE32 at
       * W[0x16008] + (base+i)*0x20 + 0x10, and `muls.w` is 16x16.
       * Both were host-pointer derefs of ROM addresses. */
      W_A16_SET(0x470C, iVar4,
           (short)W[0x0C8C] *
           (short)vrd32s((uint32_t)W[0x16008]
                         + (uint32_t)((vrd16s((uint32_t)W[0x16010]) + iVar4) * 0x20 + 0x10)));
      W[0x480C + iVar4 * 4] = 0x7fffffff;
      iVar4 = iVar4 + 1;
    } while (iVar4 < 0x40);
  }
  W[0x15FE8] = 0;
  return;
}


/* (end balloon_system_init) */

/* ---- objects_move_update ---- */

int objects_move_update(void)

{
  int iVar1;
  uint32_t uVar2;
  int iVar3;
  int iVar4;
  
  iVar4 = 0;
  do {
    if (-1 < (int)(&g_obj_active_flag)[iVar4 * 0x3c]) {
      uVar2 = ((int)(&g_obj_angle)[iVar4 * 0x3c] >> 1 & 0x7ffe) * 2;
      (&g_obj_pos_x)[iVar4 * 0x3c] =
           (&g_obj_base_x)[iVar4 * 0x3c] +
           ((&g_obj_speed)[iVar4 * 0x3c] * ((int)(int16_t)vrd16s(0x20B006 + (uVar2)) >> 4) >> 0xb);
      (&g_obj_pos_y)[iVar4 * 0x3c] = (&g_obj_base_y)[iVar4 * 0x3c];
      (&g_obj_pos_z)[iVar4 * 0x3c] =
           (&g_obj_base_z)[iVar4 * 0x3c] +
           ((&g_obj_speed)[iVar4 * 0x3c] * ((int)(int16_t)vrd16s(0x20B004 + (uVar2)) >> 4) >> 0xb);
      (&g_obj_grid_cell)[iVar4 * 0x3c] =
           (int)(&g_obj_pos_x)[iVar4 * 0x3c] / 0x18000 +
           ((int)(&g_obj_pos_z)[iVar4 * 0x3c] / 0x18000) * 8;
      (&g_obj_angle)[iVar4 * 0x3c] = (&g_obj_angle_delta)[iVar4 * 0x3c] + (&g_obj_angle)[iVar4 * 0x3c];
    }
    iVar1 = iVar4 + 1;
    iVar3 = iVar4 + -7;
    iVar4 = iVar1;
  } while (iVar1 < 8);
  return iVar3;
}


/* ---- the per-course SCENERY DISPATCH, ROM table at 0x03631C ----------------
 *
 * `objects_render_master` @0x014524 and `objects_render_attract_mode` @0x0145E8
 * both end with
 *     move.l  $e00e0c.l, d0                 ; d0 = course
 *     movea.l $3631c(d0.l*4), a0            ; a0 = table[course]
 *     jsr     (a0)
 * (at 0x01458C and 0x01469A). Ghidra could not resolve either and emitted
 * "objects render dispatch" / "attract dispatch" comments, so NO course has
 * ever run its scenery renderer. The table holds:
 *     0  0x0144D2   scenery_objects_render_dynamic(); render_windmill();
 *     1  0x01415C   one animated placement (below)
 *     2  0x0144DE   render_stage_landmarks()
 *     3  0x015FAC   stage_render_environment()
 * This is what leaves W[0x291C] at 0 where MAME holds 7: the only caller of
 * collision_geometry_update is course 0's scenery_objects_render_dynamic --
 * register row 96's open tail, and row 120. */
static void scenery_course1_render(void)
{
    /* ROM 0x01415C..0x0141C4, a small self-contained routine Ghidra folded
     * into the tail of scenery_render_all_lod. It closes any open cz segment,
     * writes an 0x8000 header and emits ONE placement of the 4-frame animated
     * model 0x1AA + ((W[0x0C8C] >> 2) & 3) at (-camx, 0xA000-camy, -camz). */
    int32_t *cur = (int32_t *)W[0x0CA4];
    const uint8_t *lo = g_sys.dspram, *hi = g_sys.dspram + DSPRAM_SIZE;
    if ((const uint8_t *)cur < lo || (const uint8_t *)(cur + 0x10) > hi) return;

    if (W[0x4704] != 0) {              /* 0x014168 tst.l $e04704 */
        *cur++ = 0x8010;
        *cur++ = -1;
        W[0x4704] = 0;
    }
    W[0x169E8] = 0x8000;
    *cur++ = 0x8000;
    *cur++ = 0;
    *cur++ = (int32_t)(((((int)W[0x0C8C]) >> 2) & 3) + 0x1aa);
    *cur++ = (int32_t)(-(int32_t)W[0x0CDC]);
    *cur++ = (int32_t)(0xa000 - (int32_t)W[0x0CE0]);
    *cur++ = (int32_t)(-(int32_t)W[0x0CE4]);
    W[0x0CA4] = (intptr_t)cur;
}

static void scenery_course_dispatch(void)
{
    extern int g_scenery_off;
    if (g_scenery_off) return;
    /* THE MOVING TRAIN -- OFF BY DEFAULT, and the reason is not squeamishness
     * about dead code: `waterfall_render` draws the WRONG TRAIN. It emits
     * point-ROM 973/974, a brown wood-panelled carriage, and a census of the
     * master's own primitive records over 2827 captured frames finds those two
     * codes ZERO times. The train the machine runs is 970 + 972 x3 (the same
     * vehicle in green, red wheels) plus the flatbed 971, and the CPU names
     * none of them -- they are master-DSP expanded, register row 48/111's gap.
     * PROPCYCL_TRAIN=1 draws the wagons anyway. See register row 158. */
    train_render();
    switch ((int)W[0x0E0C]) {
    case 0:  scenery_objects_render_dynamic(); render_windmill();  break;
    case 1:  scenery_course1_render();                             break;
    case 2:  render_stage_landmarks();                             break;
    case 3:  stage_render_environment();                           break;
    default: break;
    }
}

/* ---- objects_render_master ---- */

undefined4 objects_render_master(void)

{
  undefined4 *puVar1;
  undefined4 uVar2;
  
  W[0x16670] = -(short)W[0x0CEC];
  /* THE TRIG PAIR IS (sin, cos), AND IT IS THE **LOW** HALF OF EACH READ.
   *
   * The ROM does two 32-BIT reads, not two 16-bit ones (0x014552 /
   * 0x014628):
   *     addi.l  #$20b002,d0 ; movea.l d0,a1
   *     move.l  (a1),$e16668 ; move.l $2(a1),$e1666c
   * The table is interleaved [sin, cos] on a 4-byte stride with
   * sin(a) @0x20B004+a and cos(a) @0x20B006+a (verified against math.sin/cos:
   * 0x20B006 is exact, 0x20B002 is the cos lane lagging one entry). So as
   * big-endian 32-bit words:
   *     (a1)      @0x20B002+a = [cos(a-4) : sin(a)]   low half = sin(a)
   *     $2(a1)    @0x20B004+a = [sin(a)   : cos(a)]   low half = cos(a)
   * and the master consumes only the LOW 16 bits of a matrix word.
   *
   * Taking the HIGH halves -- which is what `vrd16s(0x20B002+a)` /
   * `vrd16s(0x20B004+a)` did -- yields (cos, sin), i.e. the pair backwards.
   * Measured: our code-874 Y pair came out (11980, 30503) = (cos .3656,
   * sin .9309) where MAME's own object matrix for 874 is a PURE Y ROTATION
   * [[5983,0,-15252],[0,16383,0],[15251,0,5983]], cos .3652 / sin .9309 --
   * same numbers, swapped. That is what made the renderer look as though
   * 0x8002 entries were cos-first; they are not. The ROM's other emitter
   * settles it twice over: scenery_object_render @0x013AC0 writes the X and
   * Z pairs as the literals (0, 0x7FFF) -- identity read SIN-first, ninety
   * degrees read cos-first -- and dsp_cmd_place_object_rotated @0x022008
   * writes 0x20B004 (sin) before 0x20B006 (cos). */
  W[0x16668] = vrd16s(0x20B004 + (W[0x16670] & 0xfffc));   /* sin */
  W[0x1666C] = vrd16s(0x20B006 + ((uint32_t)(W[0x16670] & 0xfffc)));  /* cos */
  W[0x16672] = 1;
  W[0x169E8] = 0x8002;
  puVar1 = (int32_t*)W[0x0CA4] + 1;
  *(int32_t*)W[0x0CA4] = 0x8002;
  W[0x0CA4] = puVar1;
  puVar1 = (int32_t*)W[0x0CA4] + 1;
  *(int32_t*)W[0x0CA4] = 0;
  W[0x0CA4] = puVar1;
  W[0x4704] = 0;
  scenery_render_all_lod();
  animated_objects_render_all();
  scenery_course_dispatch();
  uVar2 = 0;
  if (W[0x0E0C] != 3) {
    uVar2 = bonus_render_vehicle();
  }
  if (W[0x4704] != 0) {
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = 0x8010;
    W[0x0CA4] = puVar1;
    uVar2 = 0xffffffff;
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = (int32_t)0xffffffff;
    W[0x0CA4] = puVar1;
  }
  if (W[0x169E8] != 0x8002) {
    uVar2 = 0x8002;
    W[0x169E8] = 0x8002;
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = 0x8002;
    W[0x0CA4] = puVar1;
    puVar1 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = 0;
    W[0x0CA4] = puVar1;
  }
  return uVar2;
}


void sprite_3d_project_and_draw(int, int, int, int, int, int32_t, int32_t);

/* ---- particle_system_init ---- */

/* THE TERRAIN SPRAY / DUST BILLBOARDS, rewritten from the listing (register
 * row 195). Layout, all ROM-verified:
 *   0xE16A40 + e*0xA0 + slot*0x14   5 emitters x 4 slots x FIVE LONGS:
 *                                   x, y, z, model, phase (phase < 0 = free)
 *   0xE16D60 + e*4                  per emitter two 16-bit words: the burst
 *                                   COUNTER (high half, `tst.w (a0,d3.l*4)`)
 *                                   and the MODEL (low half, `movea.w
 *                                   $2(a0,d3.l*4)` -- ext 0x3C02, scale 4,
 *                                   which capstone prints without the scale)
 *   0xE02AE8 / 0xE02B10 + layer*4   the per-layer contact flag and its kind,
 *                                   copied there by world_objects_tick_all
 * The transpile read the last two at stride 1, kept the counter/model as
 * whole slots at stride 2, and stored each particle's X and Y at
 * `e*0x28 + slot*5` while Z and the rest used the byte layout -- so X/Y sat on
 * 1-/2-mod-4 slots that sync_wram_to_W rebuilds every frame, and the
 * billboards jumped about; emitters for layers 1-4 fired off rebuilt garbage
 * with random kinds. */
int particle_system_init(void)                                 /* 0x028ECC */
{
  int i;
  for (i = 0; i < 200; i++) W[0x16A40 + i * 4] = -1;
  for (i = 0; i < 5; i++) W_SET_HI16(0x16D60 + i * 4, 0xffff);
  W[0x16D74] = 0;
  W[0x16D78] = 0;
  return 0;
}


/* ---- particle_system_update ---- */

void particle_system_update(void)                              /* 0x028F2E */
{
  /* the seventh argument is a Z BIAS for the billboard's sort key, not a
   * tint: sprite_3d_project_and_draw adds the projected depth to it */
  int32_t tint = W[0x12BC] ? (int32_t)0xffe04000 : (int32_t)0xffc00000;
  int e, k;

  for (e = 0; e < 5; e++) {
    if (W[0x2AE8 + e * 4] != 0) {                             /* 0x028F6A */
      int32_t kind = (int32_t)W[0x2B10 + e * 4];
      if (kind == 3 || kind == 6) {
        int32_t v = (int32_t)W[0x16D78];
        kind = v > 0x2000 ? 8 : v > 0x1800 ? 3 : 7;
        if (W[0x12D8] != 0) kind = 8;
      } else if (kind == 2) {
        kind = ((int32_t)W[0x0E0C] == 1 && (int32_t)W[0x0D24] >= 0x48) ? 9 : 2;
      } else if (kind == 1 || kind == 5 || kind == 4) {
        if (kind != 4 && ((int32_t)W[0x0C98] & 0x14) != 0) kind = 4;
        if ((int32_t)W[0x16D78] < 0xb90) kind = 0;             /* 0x02900A */
      } else if (kind > 6) {
        kind = 0;
      }
      if (kind != 0) particle_emitter_activate(e, kind);
    }
    if (e != 0) {                                             /* 0x02903A */
      int r = 0x130C + e * 0x140;
      if (W[r + 0x118] != 0 && (W[r + 0x11c] == 0xe0 || W[r + 0x11c] == 0xe1))
        particle_emitter_activate(e, 3);
      if (W[r + 0x11c] == 0xe5)
        particle_emitter_activate(e, 10);
    }
  }

  W[0x16D74] = (W[0x16D74] + 1) & 0xf;                        /* 0x02909C */
  int emit = (W[0x16D74] & 3) == 0;
  uint32_t slot = (uint32_t)(int32_t)W[0x16D74] >> 2;
  for (e = 0; e < 5; e++) {
    int st = 0x16D60 + e * 4;
    if (emit && (int16_t)W_HI16(st) >= 0) {                   /* 0x0290D0 */
      int a = 0x16A40 + e * 0xa0 + (int)slot * 0x14;
      int r = 0x130C + e * 0x140;
      W[a + 0x00] = (int32_t)((int16_t)keycus_read() >> 6) + (int32_t)W[r + 0x0c];
      W[a + 0x04] = (int32_t)((int16_t)keycus_read() >> 6) + (int32_t)W[r + 0x10];
      W[a + 0x08] = (int32_t)((int16_t)keycus_read() >> 6) + (int32_t)W[r + 0x14];
      W[a + 0x0c] = (int16_t)W_LO16(st);
      W[a + 0x10] = 3 - (int16_t)W_HI16(st);
      W_SET_HI16(st, W_HI16(st) - 1);
    }
    for (k = 0; k < 3; k++) {                                 /* 0x02915C */
      int a = 0x16A40 + e * 0xa0 + (int)slot * 0x14;
      int32_t x = W[a], y = W[a + 4], z = W[a + 8], model = W[a + 0x0c];
      int32_t phase = W[a + 0x10];
      if (phase >= 0) {
        int32_t scale = model == 4 ? phase * 0x1fff + 0x8000 : phase * 0x3fff;
        /* ROM 0x0291DC: `pea ([$ffe6,a6], d5.l)` -- memory-indirect: the
         * value is model + phase, an ADD, not a load (rows 30/90/92). */
        sprite_3d_project_and_draw(0, model + phase,
                                   x - (int32_t)W[0x0CDC], y - (int32_t)W[0x0CE0],
                                   z - (int32_t)W[0x0CE4], scale, tint);
        if (emit) {
          W[a + 0x10] = phase + 1;
          if (phase + 1 > 3) W[a + 0x10] = -1;
        }
      }
      slot = (slot - 1) & 3;
    }
  }
  {
    int32_t v = (int32_t)W[0x16D78] + (int32_t)W[0x0D4C];     /* 0x029238 */
    if (v < 0) v++;
    W[0x16D78] = v >> 1;
  }
}


/* ---- balloon_proximity_test ---- */

int balloon_proximity_test(int param_1,int param_2,int param_3)

{
  int iVar1;
  int iVar2;
  int iVar3;
  
  if (W[0x15FE8] == '\0') {
    iVar3 = param_1 - W[0x0D00] >> 7;
    iVar1 = param_2 - W[0x0D04] >> 7;
    iVar2 = param_3 - W[0x0D08] >> 7;
    iVar3 = iVar2 * iVar2 + iVar1 * iVar1 + iVar3 * iVar3;
    if (iVar3 < 0x736) {
      iVar3 = -1;
    }
  }
  else {
    iVar3 = 0;
  }
  return iVar3;
}

/* ---- particle_emitter_activate ---- */

void particle_emitter_activate(int param_1,int param_2)       /* 0x028F04 */
{
  { extern int g_contactlog; static int n;
    if (g_contactlog) { n++; if ((n % 20) == 1)
      fprintf(stderr, "[PARTICLE] emitter fired: layer %d kind %d  (call %d, frame %u)\n",
              param_1, param_2, n, g_sys.frame_count); } }
  /* lea $e16d60(d0.l*4) ; move.w cnt,(a0)+ ; move.w $3797a(d0.l*2),(a0) */
  W_SET_HI16(0x16D60 + param_1 * 4, (int32_t)W[0x15F18] > 0 ? 1 : 3);
  W_SET_LO16(0x16D60 + param_1 * 4, vrd16(0x3797A + param_2 * 2));
}

/* ---- scenery_render_all_lod ---- */

int scenery_render_all_lod(void)

{
  /* UNSTUBBED, rewritten from the M68K at 0x014056.
   *
   * This was `return 0;` followed by `if (0) { ...original... }`, so NO
   * scenery rendered at all -- and CLAUDE.md's stub table called it
   * "Working", which is why nobody looked. What that costs on screen: the
   * starting platform the bike sits on during the intro orbit is scenery
   * record 1 of the table at 0xAF0D8, whose three LOD tiers are all model
   * 805 (record code 874 -- the red roof on its pole). MAME's gameplay
   * capture carries 874 in every frame; we carried none.
   *
   * The reason it was stubbed was "uses ROM addresses as raw pointers", and
   * the machine says exactly where:
   *
   *   01409A: d6 = W[0x0E0C]; lsl.l #9  ; addi.l #$b0938  -> the batch table
   *   0140B8: lea (a0, d0.l*4), a1      ; a1 = batchbase + chunk*4, a ROM addr
   *   0140BC: tst.b (a1)                ; a BYTE test
   *   0140CC: lea ([a4], d0.l*4), a1    ; MEMORY INDIRECT:
   *                                     ;   a1 = MEM32[0xE16664] + chunk*4
   *                                     ;      = W[0x16664] + chunk*4, a ROM addr
   *   0140D0: tst.w (a1)                ; a WORD test
   *   0140E0: lea $10(a3), a3           ; the visible-chunk list strides 0x10
   *                                     ; BYTES = 4 W[] slots, not 4 ints
   *
   * The transpile had `W[0x16664 + chunk]`, which indexes the W array with a
   * ROM offset, and `piVar6 + 4` on an int* into an intptr_t[] array. Both
   * are the classes CLAUDE.md fix #21 and the `(&W[base])[N]` stride entry
   * describe. */
  uint32_t base, batch, a1;
  uint32_t chunk_i = 0, n = 0;

  /* ON. PROPCYCL_NO_SCENERY=1 disables.
   *
   * This was `return 0;` followed by `if (0) { ...original... }`, so NO
   * scenery rendered at all -- including the starting platform the bike sits
   * on during the intro orbit, which is scenery record 1 of the table at
   * 0xAF0D8 (all three LOD tiers = model 805, record code 874; MAME's capture
   * carries 874 in every frame and we carried none). */
  { extern int g_scenery_off; if (g_scenery_off) return 0; }

  /* The ROM does call this here (`jsr $15b10` at 0x014072), but our
   * animated_objects_tick still walks 16 slots with `uint8_t *p = &W[0x4178]`
   * and byte offsets +0x48/+0x58 -- W is intptr_t[], so those land 9 and 11
   * SLOTS away and animated_object_deactivate gets a garbage host pointer.
   * That is register rows 10/29, and it was latent only because nothing
   * called this function. Guarded exactly like its sibling
   * animated_objects_render_all: nothing can spawn (every slot flag reads
   * 0x8000 = inactive), so skipping it loses nothing on screen. */
  { extern int g_animobj; if (g_animobj) animated_objects_tick(); }

  W_SET_HI16(0x16674, 2);                                   /* move.w #2 */
  base  = (uint32_t)((int32_t)W[0x0E0C] * 0x600 + 0xADCD8);  /* LOD0 */
  W[0x16664] = (intptr_t)(int32_t)base;
  batch = (uint32_t)(((int32_t)W[0x0E0C] << 9) + 0xB0938);

  /* PROPCYCL_LODLOG=1: the visible-chunk list as the LOD bands see it --
   * position, chunk id, and the chunk's distance from the camera. The bands
   * are assigned by LIST POSITION (0-8 near, 9-15 mid, 16-22 far), so if the
   * list is not ordered near-to-far the wrong records are chosen: register
   * row 127 drew distant impostors up close. This is the measurement that
   * decides whether row 127's read can be turned on. */
  { extern int g_lodlog; static int nl;
    if (g_lodlog && (nl++ % 120) == 0) {
      uint32_t k, m; char buf[512]; int o = 0;
      o += snprintf(buf+o, sizeof buf-o, "[LOD] f%u count %d:",
                    g_sys.frame_count, g_visible_chunk_count);
      for (k = 0, m = 0; k < 23 && k < (uint32_t)g_visible_chunk_count;
           k++, m += 0x10) {
        uint32_t cid = (uint32_t)W[0x0E6C + m];
        long dx = (long)(cid % 8) * 0x18000 - (long)(int32_t)W[0x0CDC];
        long dz = (long)(cid / 8) * 0x18000 - (long)(int32_t)W[0x0CE4];
        long d = (long)(dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if ((int)sizeof buf - o > 24)
          o += snprintf(buf+o, sizeof buf-o, " %s%u:%ldk",
                        (k==9||k==16) ? "| " : "", cid, d/1000);
      }
      fprintf(stderr, "%s\n", buf);
    } }
  for (chunk_i = 0; chunk_i <= 8; chunk_i++, n += 0x10) {
      if (chunk_i >= (uint32_t)g_visible_chunk_count) goto done;
      { uint32_t cid = (uint32_t)W[0x0E6C + n];
        if ((int8_t)vrd8(batch + cid * 4) >= 0)
            animated_objects_spawn_batch(batch + cid * 4);
        a1 = (uint32_t)(int32_t)W[0x16664] + cid * 4;
        if (vrd16s(a1) >= 0) scenery_object_render(0x0E6C + n, a1); }
  }

  /* LOD1: chunks 9..0x0F, table base steps back 0x200 */
  W_SET_HI16(0x16674, W_HI16(0x16674) - 1);
  W[0x16664] = (intptr_t)(int32_t)((uint32_t)W[0x16664] - 0x200);
  for (; chunk_i <= 0x0F; chunk_i++, n += 0x10) {
      if (chunk_i >= (uint32_t)g_visible_chunk_count) goto done;
      { uint32_t cid = (uint32_t)W[0x0E6C + n];
        a1 = (uint32_t)(int32_t)W[0x16664] + cid * 4;
        if (vrd16s(a1) >= 0) scenery_object_render(0x0E6C + n, a1); }
  }

  /* LOD2: chunks 0x10..0x16 */
  W_SET_HI16(0x16674, W_HI16(0x16674) - 1);
  W[0x16664] = (intptr_t)(int32_t)((uint32_t)W[0x16664] - 0x200);
  for (; chunk_i <= 0x16; chunk_i++, n += 0x10) {
      if (chunk_i >= (uint32_t)g_visible_chunk_count) goto done;
      { uint32_t cid = (uint32_t)W[0x0E6C + n];
        a1 = (uint32_t)(int32_t)W[0x16664] + cid * 4;
        if (vrd16s(a1) >= 0) scenery_object_render(0x0E6C + n, a1); }
  }
done:
  return 0;
}

/* ---- animated_objects_render_all ---- */

/* A REAL PROTOTYPE. This file builds with implicit declarations allowed and
 * no game_funcs.h, so without one the call below is `int f()` and the
 * returned display-list cursor is cut to 32 bits (register row 160). */
extern intptr_t player_vehicle_render_if_active(uint32_t nb, intptr_t cur);

uint8_t ** animated_objects_render_all(void)

{
  { extern int g_aolog; static int n;
    if (g_aolog && (n++ % 120) == 0) {
      int live = 0, r;
      for (r = 0; r < 16; r++)
        if ((W_HI16(0x41C0 + r * 0x58) & 0x8000) == 0) live++;
      fprintf(stderr, "[AO] render_all f%u: %d of 16 records ACTIVE\n",
              g_sys.frame_count, live);
    } }

  /* DISABLED -- UNCONVERTED, AND NOTHING CAN LEGITIMATELY SPAWN.
   *
   * Two independent reasons this must not run as written:
   *  1. It writes the DSP command list through `uint8_t **ppuVar6`, an
   *     8-BYTE cursor, into a buffer of 4-byte words -- every store is at
   *     twice the intended stride and runs off the end (segfault at
   *     game_objects.c:1157 / :1143 on the attract path, ~frame 940).
   *  2. Its record fields are read as `*(int16_t*)(piVar8 + N)` with
   *     piVar8 an `int32_t*` into the `intptr_t[]` _W array, so every
   *     offset lands on the wrong slot -- the documented (&W[base])[N]
   *     stride class.
   * Its entire output was garbage: 16 placements per frame at
   * |t| = 284,545,592 and 48-62 bogus copies of object 124 at a
   * degenerate (0,-187168,-1) -- mismatch-register rows 10 and 11.
   *
   * And there is nothing for it to draw: animated_object_spawn is
   * disabled (CLAUDE.md "Stubbed/Disabled Functions"), so no slot is ever
   * spawned -- confirmed from a core dump, _W[0x4178 + 0x48] == 0x8000
   * (bit 15 = inactive) for every record.
   *
   * Re-enable only after converting BOTH the cursor width and the field
   * offsets; PROPCYCL_ANIMOBJ=1 forces it on for that work. */
  /* Register row 40: getenv() inside the frame loop segfaults (the returned
   * pointer faults on read). main.c already latches this flag once at
   * startup as g_animobj -- use that, as animated_objects_tick's call site
   * does. */
  { extern int g_animobj; if (!g_animobj) return (int32_t *)W[0x0CA4]; }

  short sVar1;
  int32_t  puVar2;
  int32_t *ppuVar3;
  uint16_t uVar5;
  uint32_t uVar4;
  int32_t *ppuVar6;
  int32_t *ppuVar7;
  uint32_t nb;          /* the record's _W[] byte offset (0x4178 + i*0x58) */

  /* SLOT-BOUNDED WALK. This iterated `piVar8 += 0x16` on an `int32_t*`
   * (0x58 BYTES) but terminated on `piVar8 != &W[0x4438]`, an `intptr_t*`
   * one -- in _W[] (8 bytes/slot) those strides can never coincide, so the
   * `!=` never fired and the walk ran off the end of the array. It reached
   * player_vehicle_render_if_active with param_1 = 988366840 and
   * segfaulted at ~frame 940 of attract.
   *
   * The M68K record is 0x58 bytes over 0xE04178..0xE04438, i.e. exactly
   * 8 records; in this model the slot index IS the byte offset.
   * The FIELD reads inside the body are still the unconverted
   * (&W[base])[N] form (mismatch-register row 11) -- this bounds the walk
   * only, it does not make the emitted objects correct. */
  /* THE DSP WRITE CURSOR, which Ghidra dropped entirely.
   *
   * ROM 0x01562E is `movea.l $e00ca4.l,a1` in the prologue and 0x015872 is
   * `move.l a1,$e00ca4.l` just before the rts -- the same cursor discipline
   * every other emitter follows. `ppuVar6` was declared and then READ at its
   * first use with no initialiser at all, i.e. whatever was on the stack: a
   * wild pointer. With PROPCYCL_ANIMOBJ=1 that wrote through 0x5555555fff36
   * -- inside the binary's own image, not dspram -- and the process died at
   * the first object the walk found. */
  /* THE TRIG PAIRS (register row 160, row 87's class): every pair here is
   * `lea -$2(a0,d0.l*4),a4 ; move.l (a4),(a1)+ ; move.l $2(a4),(a1)+` off
   * a0 = 0x20B004 (0x01572E/0x01574C/0x015770/0x0157DA/0x0157EE/0x015802), so
   * the master takes the LOW halves -- sin at 0x20B004+a*4, cos at 0x20B006. */
  ppuVar6 = (int32_t *)W[0x0CA4];
  ppuVar7 = ppuVar6;
  ppuVar3 = ppuVar6;

  { intptr_t _nb;
  for (_nb = 0x4178; _nb < 0x4438; _nb += 0x58) {
  nb = (uint32_t)_nb;
  /* DSP CURSOR BOUNDS, per record. The body advances the cursor by up to
   * 0x19 words per object and the ROM checks nothing, so a full buffer runs
   * straight off the end -- the same guard dsp_cmd_ptr() and
   * balloon_render_and_hit_check already carry (register rows 19/29). */
  { const uint8_t *lo = g_sys.dspram, *hi = g_sys.dspram + DSPRAM_SIZE;
    if ((const uint8_t *)ppuVar6 < lo ||
        (const uint8_t *)(ppuVar6 + 0x40) > hi) {
        static int warned;
        if (warned++ == 0)
            fprintf(stderr, "[GUARD] animated_objects_render_all: DSP cursor "
                            "full -- remaining objects dropped\n");
        break;
    } }
  /* ACTIVE-SLOT TEST, at the right offset.
   *
   * The record's flag word is at BYTE offset 0x48; bit 15 set means the
   * slot is not spawned. The body tests `W_HI16(nb + 0x48)`,
   * and piVar8 is an int32_t* into an intptr_t[] array, so +0x12 lands on
   * slot +9 -- a different field. Every dead slot therefore read as live
   * and the body ran on uninitialised data: 16 garbage placements per
   * frame at |t| = 284,545,592 (mismatch-register rows 10/11), and a
   * segfault once it reached the DSP writes.
   *
   * Verified from the core dump: _W[0x4178+0x48] == 0x8000 for every
   * slot -- all correctly inactive, because animated_object_spawn is
   * disabled and nothing ever spawns. */
  /* HIGH HALF. The flag word at byte offset 0x48 is 16-bit and 0x48 is
   * 4-aligned, so it lives in the HIGH half of the slot -- which is how
   * animated_objects_reset_all writes it (`W_SET_HI16(0x41C0 + i*0x58,
   * 0x8000)`) and how every other line in this function reads it. Reading
   * the WHOLE slot here tested bit 15 of the LOW half, a different field
   * entirely, so the skip decision was made on unrelated data: with the
   * seed working (7 records spawned into slots 8..14 and 3 more into 0/3/4)
   * the renderer still emitted nothing, and PROPCYCL_ANIMOBJ=1 changed the
   * drawn set by zero codes. */
  if ((W_HI16(_nb + 0x48) & 0x8000) != 0) continue;

    ppuVar3 = (int32_t *)(uint32_t)((W_HI16(nb + 0x48)) & 0x8000);
    if ((ppuVar3 == (int32_t *)0x0) &&
       (ppuVar3 = (int32_t *)(uint32_t)W_LO16(nb + 0x4c),
       (uint8_t)(&g_chunk_lookup_table)[(short)W_LO16(nb + 0x4c)] < 10)) {
      W_SET_HI16(nb + 0x48, (W_HI16(nb + 0x48)) | 0x800);
      ppuVar3 = (int32_t *)(uint32_t)((W_HI16(nb + 0x48)) & 0x4000);
      if (((W_HI16(nb + 0x48)) & 0x4000) == 0) {
        if (((W_HI16(nb + 0x48)) & 8) == 0) {
          sVar1 = W_LO16(nb + 0x48);
          if (sVar1 == 1) {
            uVar5 = 0;
          }
          else if (sVar1 == 8) {
            uVar5 = (uint16_t)((int)W_LO16(nb + 0x10) >>
                            ((int)(W_HI16(nb + 0x4c)) & 0x3fU)) & 7;
          }
          else {
            uVar5 = (uint16_t)(((int)W_LO16(nb + 0x10) >>
                             ((int)(W_HI16(nb + 0x4c)) & 0x3fU)) % (int)sVar1);
          }
          sVar1 = ((W_HI16(nb + 0x44)) - uVar5) + -1 + sVar1;
          if (((W_HI16(nb + 0x48)) & 4) == 0) {
            puVar2 = (int32_t)(g_camera_offset_y + W[nb + 0x4]);
            if (W[0x4704] != (int32_t)W[nb + 0x50]) {
              ppuVar3 = ppuVar6 + 1;
              *ppuVar6 = (int32_t)0x8010;
              W[0x4704] = (int32_t)W[nb + 0x50];
              if (W[0x4704] != (int32_t)0x0) {
                *ppuVar3 = (int32_t)0x3;
                ppuVar3 = ppuVar6 + 3;
                ppuVar6[2] = W[0x4704];
              }
              ppuVar6 = ppuVar3 + 1;
              *ppuVar3 = (int32_t)0xffffffff;
            }
            *ppuVar6 = (int32_t)sVar1;
            ppuVar6[1] = (int32_t)(W[nb] - W[0x0CDC]);
            ppuVar6[2] = puVar2;
            ppuVar6[3] = (int32_t)(W[nb + 0x8] - W[0x0CE4]);
            uVar5 = (W_HI16(nb + 0xc));
            ppuVar6[4] = (int32_t)vrd16s(0x20B004 + ((uint32_t)(uVar5 & 0x3fff) * 4));
            ppuVar6[5] = (int32_t)vrd16s(0x20B006 + ((uint32_t)(uVar5 & 0x3fff) * 4));
            if (((W_HI16(nb + 0x48)) & 0x400) == 0) {
              ppuVar6[6] = W[0x16668];
              ppuVar6[7] = W[0x1666C];
            }
            else {
              uVar4 = (uint32_t)(W_LO16(nb + 0xc) & 0x3fff);
              ppuVar6[6] = (int32_t)vrd16s(0x20B004 + (uVar4 * 4));
              ppuVar6[7] = (int32_t)vrd16s(0x20B006 + (uVar4 * 4));
            }
            uVar5 = (W_HI16(nb + 0x10));
            ppuVar6[8] = (int32_t)vrd16s(0x20B004 + ((uint32_t)(uVar5 & 0x3fff) * 4));
            ppuVar7 = ppuVar6 + 10;
            ppuVar6[9] = (int32_t)vrd16s(0x20B006 + ((uint32_t)(uVar5 & 0x3fff) * 4));
            /* 0x01577A `moveq #$4,d0 ; bra $15862 ; move.l d0,(a1)+`: the
             * entry's flags word is 4, like every other placement -- Ghidra
             * wrote the register as NULL (register row 160). */
            ppuVar3 = (int32_t *)(intptr_t)4;
          }
          else {
            if (W[0x16672] != 0) {
              *ppuVar6 = (int32_t)0x8008;
              ppuVar6[1] = (int32_t)0x2;
              ppuVar6[2] = (int32_t)0x1;
              ppuVar6[3] = (int32_t)0x0;
              ppuVar6[4] = (int32_t)0x7fff;
              ppuVar6[5] = W[0x16668];
              ppuVar6[6] = W[0x1666C];
              ppuVar6[7] = (int32_t)0x0;
              ppuVar6[8] = (int32_t)0x7fff;
              ppuVar3 = ppuVar6 + 10;
              ppuVar6[9] = (int32_t)0x4;
              ppuVar6 = ppuVar6 + 0xb;
              *ppuVar3 = (int32_t)0xffffffff;
              W[0x16672] = 0;
            }
            *ppuVar6 = (int32_t)0x8008;
            ppuVar6[1] = (int32_t)0x3;
            ppuVar6[2] = (int32_t)0x1;
            uVar5 = (W_HI16(nb + 0xc));
            ppuVar6[3] = (int32_t)vrd16s(0x20B004 + ((uint32_t)(uVar5 & 0x3fff) * 4));
            ppuVar6[4] = (int32_t)vrd16s(0x20B006 + ((uint32_t)(uVar5 & 0x3fff) * 4));
            uVar4 = (uint32_t)(W_LO16(nb + 0xc) & 0x3fff);
            ppuVar6[5] = (int32_t)vrd16s(0x20B004 + (uVar4 * 4));
            ppuVar6[6] = (int32_t)vrd16s(0x20B006 + (uVar4 * 4));
            uVar5 = (W_HI16(nb + 0x10));
            ppuVar6[7] = (int32_t)vrd16s(0x20B004 + ((uint32_t)(uVar5 & 0x3fff) * 4));
            ppuVar6[8] = (int32_t)vrd16s(0x20B006 + ((uint32_t)(uVar5 & 0x3fff) * 4));
            ppuVar6[9] = (int32_t)0x4;
            ppuVar6[10] = (int32_t)0x0;
            ppuVar6[0xb] = (int32_t)W[nb];
            ppuVar6[0xc] = (int32_t)W[nb + 0x4];
            ppuVar6[0xd] = (int32_t)W[nb + 0x8];
            ppuVar6[0xe] = (int32_t)0xffffffff;
            ppuVar6[0xf] = (int32_t)0x8009;
            ppuVar6[0x10] = (int32_t)0x3;
            ppuVar6[0x11] = (int32_t)0x2;
            ppuVar6[0x12] = (int32_t)0x3;
            ppuVar6[0x13] = (int32_t)0x800a;
            ppuVar6[0x14] = (int32_t)sVar1;
            ppuVar6[0x15] = (int32_t)0x3;
            ppuVar6[0x16] = (int32_t)(W[0x0D00] - W[0x0CDC]);
            ppuVar7 = ppuVar6 + 0x18;
            ppuVar6[0x17] = (int32_t)(W[0x0D04] - W[0x0CE0]);
            ppuVar3 = (int32_t *)(W[0x0D08] - W[0x0CE4]);
          }
          ppuVar6 = ppuVar7 + 1;
          *ppuVar7 = (int32_t)ppuVar3;
        }
        else {
          ppuVar3 = (int32_t *)player_vehicle_render_if_active(nb, (intptr_t)ppuVar6);
          ppuVar6 = ppuVar3;
        }
      }
    }
  }
  }
  W[0x0CA4] = ppuVar6;
  return ppuVar3;
}


/* ---- animated_objects_spawn_batch ---- */

/* param_1 is a ROM BYTE ADDRESS. 0x014C32 is `move.b (a2),d0 ; extb.l d0`,
 * a SIGNED BYTE, then *0x1C into the record table at 0xB2B78. */
void animated_objects_spawn_batch(uint32_t param_1)

{
  short sVar1;
  int iVar2;
  
  iVar2 = (int)(int8_t)vrd8(param_1) * 0x1c + 0xb2b78;
  for (sVar1 = 0; (int)sVar1 < (int)(int8_t)vrd8(param_1 + 1); sVar1 = sVar1 + 1) {
    animated_object_spawn(iVar2);
    iVar2 = iVar2 + 0x1c;
  }
  return;
}

/* ---- animated_objects_tick ---- */

void animated_objects_tick(void)

{
  short sVar1;
  
  /* ROM 0x015B10: a2 = 0xE04178, `lea $58(a2),a2` each iteration, `cmpi.w
   * #$10,d4` -- 16 records of 0x58 BYTES with a 16-bit flag at +0x48. The old
   * walk used `uint8_t *p = &W[0x4178]` with byte offsets +0x48/+0x58, but W
   * is intptr_t[], so those landed 9 and 11 SLOTS away and
   * animated_object_deactivate(int) truncated the pointer -- the SIGSEGV that
   * kept the whole animated-object system disabled (register row 10, and with
   * it the birds, the bonus balloons and the rest of the animation set).
   * _W[] holds the 32-bit word at each 4-aligned byte offset; base, stride and
   * +0x48 are all 4-aligned, so `move.w $48(a2)` is that long's HIGH half. */
  { uint32_t nb = 0x4178;
  sVar1 = 0;
  do {
    if ((W_HI16(nb + 0x48) & 0x8000) == 0) {
      animated_object_script_update(nb);
      if ((W_HI16(nb + 0x48) & 0x1080) == 0) {
        animated_object_deactivate(nb);
      }
      W_SET_HI16(nb + 0x48, W_HI16(nb + 0x48) & 0xf7ff);
    }
    sVar1 = sVar1 + 1;
    nb += 0x58;
  } while (sVar1 < 0x10); }
  /* ROM 0x015B72..0x015BA8 (a4 = 0xE16678, a3 = 0xE00D00): the first three are
   * `move.l`, the last four `move.w` into +0x0c/0x0e/0x10/0x12 -- two pairs of
   * halves, not four whole slots. */
  W[0x16678] = W[0x0D00];
  W[0x1667C] = W[0x0D04];
  W[0x16680] = W[0x0D08];
  W_SET_HI16(0x16684, (uint16_t)(short)(-W[0x0D0C] >> 2));
  W_SET_LO16(0x16684, (uint16_t)(short)(-W[0x0D10] >> 2));
  W_SET_HI16(0x16688, (uint16_t)(short)(-W[0x0D14] >> 2));
  W_SET_LO16(0x16688, (uint16_t)(short)W[0x0D24]);
  return;
}

/* ---- scenery_object_render ---- */

/* param_2 is a ROM BYTE ADDRESS, not a host pointer. The M68K at 0x0139C0 is
 * `move.w (a3),d2 ; ext.l d2 ; lsl.l #5 ; addi.l #$af0d8` -- a BE16 read -- and
 * the loop bound at 0x013EF8 is `cmp.w $2(a3),d0`, a BE16 at +2, NOT the
 * 32-bit +4 the transpile used. */
/* Scenery world position, per CLAUDE.md key-fix #20.
 *
 * The chunk grid is 8 wide and each cell is 0x18000 units, so the object's
 * world position is its cell origin plus the record's LOCAL offset, minus the
 * camera. The transpile had `*(int*)(param_1 + xf*2 + 4)`, treating that local
 * offset -- e.g. -11034 for record 0 -- as a byte index into the W[] heap.
 * With this function newly live that wrote _W[-18384] and corrupted the heap,
 * which surfaced as a segfault inside fclose() at exit rather than here. */
/* The M68K at 0x013A9E / 0x013AB8:
 *     lea.l ([$4,a2], d0.w*2), a0     ; X = MEM32[param_1+4] + xf*2
 *     lea.l ([$8,a2], d0.w*2), a0     ; Z = MEM32[param_1+8] + zf*2
 * and the result is STORED (`move.l a0,(a4)+`), never dereferenced -- so
 * those are cell-local OFFSET VALUES written by terrain_chunk_visibility,
 * with the record's own +0x0c/+0x0e added at double scale. Ghidra flattened
 * the memory-indirect into `*(int *)(param_1 + xf*2 + 4)`, which reads a
 * coordinate as an address. */
#define SCEN_CHUNK(p1)      ((int32_t)W[(p1)])
#define SCEN_X(p1, rec)     ((int32_t)W[(p1) + 4] + (int)vrd16s((rec) + 0x0c) * 2)
#define SCEN_Z(p1, rec)     ((int32_t)W[(p1) + 8] + (int)vrd16s((rec) + 0x0e) * 2)

/* PROPCYCL_SCENPROBE=<lo>,<hi>: name the ROM record, LOD tier and frame
 * count behind every scenery placement whose model id lands in [lo,hi].
 * "Which record draws the lake, and what does its tier-1 entry say?" is not
 * answerable from the display list alone -- the list carries only the final
 * model id. */
int g_scenprobe_lo = -1, g_scenprobe_hi = -1;
int g_lod_fartier = 0;
#define SCEN_PROBE(kind, mdl) do { \
    if (g_scenprobe_lo >= 0 && (int)(mdl) >= g_scenprobe_lo && \
        (int)(mdl) <= g_scenprobe_hi) \
        fprintf(stderr, "[SCEN] f%u kind%d model %d  rec 0x%X (type %d)  " \
                "tier %d  bases %d/%d/%d  counts %d/%d/%d  shift %d\n", \
                g_sys.frame_count, (kind), (int)(mdl), iVar5, \
                (iVar5 - 0xaf0d8) / 0x20, (int)sVar3, \
                (int)vrd16s(iVar5), (int)vrd16s(iVar5+2), (int)vrd16s(iVar5+4), \
                (int)vrd16s(iVar5+6), (int)vrd16s(iVar5+8), (int)vrd16s(iVar5+10), \
                (int)(int8_t)vrd8(iVar5+0x1b)); \
  } while (0)

void scenery_object_render(int param_1,uint32_t param_2)

{
  int iVar1;
  short sVar2;
  short sVar3;
  uint32_t uVar4;
  int iVar5;
  int iVar6;
  int *piVar7;
  int *piVar8;
  short local_6;
  
  /* THE LOD TIER IS A 16-BIT VALUE AT A 4-ALIGNED OFFSET, i.e. the HIGH half
   * of the _W[] slot -- `sVar3 = W[0x16674]` read the LOW half, which is 0 on
   * the gameplay path, so EVERY scenery object was drawn from its FARTHEST
   * LOD entry no matter how close it was.
   *
   * ROM, all three accesses 16-bit and all at 0xE16674:
   *     014080  move.w #$2,(a0)       a0 = 0xE16674   (scenery_render_all_lod)
   *     0140EC  subq.w #$1,(a0)       once per tier
   *     0139CC  move.w $e16674.l,d7                   (here)
   * and the record is indexed `$6(a0,d7.w*2)` at 0139E2 -- extension word
   * 0x7206 has bits10-9 = 01, scale 2, the scale capstone drops (see
   * CLAUDE.md). So the tier selects a 16-bit slot pair, and tier 2 is the
   * NEAREST: the record's three bases are laid out [LOD2, LOD1, LOD0].
   *
   * What it cost: the near-only animated records -- 28 (bases 0/0/151),
   * 29 (0/0/238), 30 (0/0/266), 31 (0/0/309) -- carry 0 in the slot the
   * broken read selected, so they resolved to model 0 and drew nothing.
   * Those are the flags and the cheering spectators.
   *
   * THE PAIRING IS PROVEN, NOT INFERRED. Each per-chunk table must be used
   * with exactly one base/count slot, because ROM 0x013A60 divides by the
   * count (`divs.l d4,d1`) and a zero there traps the 68K. Testing every
   * (table, slot) pair against the ROM data, across all four courses, only
   * one assignment never divides by zero:
   *     table 0 (0xADCD8) <-> slot +4   (20 of 37 records would trap at +0)
   *     table 1 (0xADAD8) <-> slot +2
   *     table 2 (0xAD8D8) <-> slot +0
   * which is exactly d7 = 2/1/0 over the three loops. The loop bounds agree
   * too (0x0140E8 `subq.l #$8 ; bls` = 0..8; 0x014116 `cmp.l #$f ; bls`).
   *
   * AND YET TURNING IT ON MAKES THE PICTURE WORSE, so it is OFF by default.
   * Frame-locked at the level start against dumps/gameplay, codes present
   * on >=3 frames:
   *     read        ours  shared  MAME-only  OURS-ONLY
   *     low  (off)   146      83         33         63
   *     high (on)    181      91         25         90
   * Shared up 83 -> 91 and MAME-only down 33 -> 25 -- it really does add
   * content the machine draws (codes 273, 331, 670, 671, 822-824, 828).
   * But ours-only goes 63 -> 90, and four of the newcomers are the visible
   * damage a user reported as "random parts of the terrain popping up all
   * over": codes 895, 1335, 1355 and 1407, which MAME draws ZERO times in
   * 600 frames across dumps/gameplay and dumps/gameplay_steer while we draw
   * each 48-64 times at |t| 77k-360k. Model 1335 rendered is a low-detail
   * green slab with a building on it -- a distant impostor, drawn near.
   *
   * So the READ is right and something downstream of it is not: most likely
   * the visible-chunk list's ORDER or LENGTH, which is what decides whether
   * a chunk lands in positions 0-8 (the near band) at all. Until that is
   * measured against the machine, shipping the correct read ships the
   * impostors. Turn it on with PROPCYCL_LOD_NEARTIER=1 to continue the work. */
  /* ON by default now: the collateral that kept it off was a SEPARATE bug in
   * the count<0 branch below (the 0x36184 ping-pong table read), not this
   * read. With that fixed, measured against MAME's full gameplay captures:
   * shared codes 96 -> 130, MAME-only 82 -> 48, ours-only 12 -> 12.
   * PROPCYCL_LOD_FARTIER=1 restores the old low-half read for A/B. */
  { extern int g_lod_fartier;
    sVar3 = g_lod_fartier ? (short)W[0x16674] : (short)W_HI16(0x16674); }
  iVar5 = (int)vrd16s(param_2) * 0x20 + 0xaf0d8;
  local_6 = 0;
  piVar8 = (int32_t *)W[0x0CA4];
  /* BOUNDS. Every write below is `piVar8[N] = ...` straight into dspram with
   * no check -- the same shape dsp_cmd_ptr() was given a guard for in
   * register row 19. With this function newly live it walks real records and
   * ran off the end, which showed up as heap corruption much later (a
   * segfault inside fclose() at exit) rather than here. */

  _safety_ctr = 0;
  do {
    if ((int)vrd16s(param_2 + 2) <= (int)local_6) {
      W[0x0CA4] = (intptr_t)piVar8;
      return;
    }
    /* PER-RECORD bounds. The cursor advances 0xb words per record and the
     * record count comes from ROM, so a single check at entry is not enough:
     * ASAN caught a 4-byte write one byte past g_sys from the tail of this
     * loop. Same failure mode dsp_cmd_ptr() is guarded against (row 19) --
     * drop the rest of this chunk's scenery rather than corrupt the heap. */
    { const uint8_t *lo = g_sys.dspram, *hi = g_sys.dspram + DSPRAM_SIZE;
      if ((const uint8_t *)piVar8 < lo || (const uint8_t *)(piVar8 + 0x20) > hi) {
          static int warned;
          if (warned++ == 0)
              fprintf(stderr, "[GUARD] scenery_object_render: DSP cursor full "
                              "-- remaining scenery dropped\n");
          W[0x0CA4] = (intptr_t)piVar8;
          return;
      } }
    iVar6 = (int)(vrd16s(iVar5 + 6 + sVar3 * 2));
    if (vrd32s(iVar5 + 0x1c) != W[0x4704]) {
      piVar7 = piVar8 + 1;
      *piVar8 = 0x8010;
      W[0x4704] = vrd32s(iVar5 + 0x1c);
      if (W[0x4704] != 0) {
        *piVar7 = 3;
        piVar7 = piVar8 + 3;
        piVar8[2] = vrd32s(iVar5 + 0x1c);
      }
      piVar8 = piVar7 + 1;
      *piVar7 = -1;
    }
    if ((int8_t)vrd8(iVar5 + 0x1a) == '\0') {
      if (iVar6 < 0) {
        /* THE 6-ENTRY PING-PONG TABLE AT 0x36184 IS 16-BIT, INDEXED *2, AND
         * ITS VALUE IS USED AS-IS. ROM 0x013A72:
         *     013A74  move.b  $1b(a0),d0          the shift
         *     013A7E  asr.l   d0,d1               W[0x0C8C] >> shift, NO mask
         *     013A82  divs.l  #6,d1               d0 = remainder (Dr = d0)
         *     013A86  movea.w $36184(d0.l*2),a0   INDEX SCALED BY 2
         *     013A90  adda.w  (a1,d7.w*2),a0      + the tier base, value AS-IS
         * The C had three errors in this one line: a stray `& 0x3f` (that
         * mask belongs to the SHIFT COUNT in the sibling branches, not to the
         * shifted value), the table index taken in BYTES so it straddled
         * entries, and the value multiplied by 2 on top. The table is
         * [0,1,2,3,2,1]; read that way it yields [0,0,2,512,4,1024].
         *
         * What it cost: this is the `count < 0` branch, which is what the
         * NEAR-tier records of the animated scenery use. Record type 14 has
         * bases 0/0/242 and counts 0/0/-1, so base 242 + 1024 = model 1266 --
         * a TERRAIN CHUNK id drawn as scenery. That is the "random parts of
         * the terrain popping up all over" of register row 127, and it is why
         * the ROM-correct LOD tier read had to be defaulted off. */
        { int _sh = (int)(int8_t)vrd8(iVar5 + 0x1b);
          int _ph = (int)(((int32_t)W[0x0C8C] >> _sh) % 6);
          if (_ph < 0) _ph += 6;
          iVar6 = (int)vrd16s(iVar5 + sVar3 * 2) +
                  (int)vrd16s(0x36184 + (uint32_t)_ph * 2); }
      }
      else {
        if (iVar6 == 8) {
          uVar4 = W[0x0C8C] + (vrd16s(iVar5 + 0x16)) >>
                  ((int)(int8_t)vrd8(iVar5 + 0x1b) & 0x3fU) & 7;
        }
        else if (iVar6 == 1) {
          uVar4 = 0;
        }
        else {
          uVar4 = (W[0x0C8C] + (vrd16s(iVar5 + 0x16)) >>
                  ((int)(int8_t)vrd8(iVar5 + 0x1b) & 0x3fU)) % (iVar6 ? iVar6 : 1);
        }
        iVar6 = uVar4 + (int)(vrd16s(iVar5 + sVar3 * 2));
      }
      *piVar8 = iVar6;
      SCEN_PROBE(0, iVar6);
      piVar8[1] = SCEN_X(param_1, iVar5);
      piVar8[2] = g_camera_offset_y + vrd32s(iVar5 + 0x10);
      piVar8[3] = SCEN_Z(param_1, iVar5);
      piVar8[4] = 0;
      piVar8[5] = 0x7fff;
      piVar8[6] = W[0x16668];
      piVar8[7] = W[0x1666C];
      piVar8[8] = 0;
      piVar8[9] = 0x7fff;
      piVar8[10] = 0;
    }
    else if ((int8_t)vrd8(iVar5 + 0x1a) == '\x01') {
      if (iVar6 == 8) {
        uVar4 = W[0x0C8C] >> ((int)(int8_t)vrd8(iVar5 + 0x1b) & 0x3fU) & 7;
      }
      else if (iVar6 == 1) {
        uVar4 = 0;
      }
      else {
        uVar4 = iVar6 ? (W[0x0C8C] >> ((int)(int8_t)vrd8(iVar5 + 0x1b) & 0x3fU)) % (iVar6 ? iVar6 : 1) : 0;
      }
      *piVar8 = uVar4 + (int)(vrd16s(iVar5 + sVar3 * 2));
      SCEN_PROBE(1, *piVar8);
      piVar8[1] = SCEN_X(param_1, iVar5);
      piVar8[2] = g_camera_offset_y + vrd32s(iVar5 + 0x10);
      piVar8[3] = SCEN_Z(param_1, iVar5);
LAB_00013e9c:
      uVar4 = (uint32_t)((vrd16s(iVar5 + 0x14)) & 0xfffc);
      /* (sin, cos) -- the LOW half of the ROM's two 32-BIT reads.
       * ROM 0x013EB2/0x013ECA/0x013EE2: `moveq #$fe,d0; add.l d5,d0` puts
       * the base at trigbase-2 = 0x20B002, then `move.l (a0),(a4)+` and
       * `move.l $2(a0),(a4)+` -- 32-bit, so the low halves are sin(a) and
       * cos(a), and the master uses only the low 16 bits. Reading the HIGH
       * halves (0x20B002/0x20B004 as 16-bit) gives (cos_lagged, sin), the
       * pair backwards. This function's own type-4 branch proves the
       * convention: it writes the X and Z pairs as the literals (0, 0x7FFF).
       * Measured: code 874's Y pair was (11980, 30503) = (cos .3656,
       * sin .9309) where MAME's object matrix for 874 is a pure Y rotation
       * with cos .3652 / sin .9309 -- same numbers, swapped. */
      piVar8[4] = vrd16s(0x20B004 + uVar4);
      piVar8[5] = vrd16s(0x20B006 + uVar4);
      uVar4 = (uint32_t)((vrd16s(iVar5 + 0x16)) & 0xfffc);
      piVar8[6] = vrd16s(0x20B004 + uVar4);
      piVar8[7] = vrd16s(0x20B006 + uVar4);
      uVar4 = (uint32_t)((vrd16s(iVar5 + 0x18)) & 0xfffc);
LAB_00013edc:
      piVar8[8] = vrd16s(0x20B004 + uVar4);
      piVar8[9] = vrd16s(0x20B006 + uVar4);
      piVar8[10] = 4;
    }
    else if ((int8_t)vrd8(iVar5 + 0x1a) == '\x05') {
      /* ROM 0x013B62..0x013BA2 -- the FRAME TABLE AT 0x36144 IS 16-BIT,
       * INDEXED *2, AND ITS VALUE IS USED AS-IS: `moveq #0,d1 ; move.w
       * $16(a0),d1` (ZERO-extended) `; add.l (frame counter) ; asr.l <byte
       * +0x1b> ; divs.l <count>` then `movea.w $36144(d0.l*2)` on the
       * REMAINDER, + the tier base; the sound fires when that table value is
       * 3 on a multiple of 8 frames. The table is [0..8, then 5,6,7,8 ...].
       *
       * The C read the table at a BYTE index and then doubled the value --
       * register row 133's error on the sibling table at 0x36184 -- so odd
       * phases straddled two words (0x0004 | 0x0005 -> 0x0400) and the model
       * came out base + 1024, 2048, 2560, 3072: other courses' TERRAIN CHUNKS
       * and out-of-range ids, blinking in over Wind Woods' animated scenery. */
      { uint32_t _t = (uint32_t)W[0x0C8C] + (uint32_t)vrd16(iVar5 + 0x16);
        int32_t _v = (int32_t)_t >> ((int)(int8_t)vrd8(iVar5 + 0x1b) & 0x3f);
        int _ph = (int)(_v % (iVar6 ? iVar6 : 1));
        sVar2 = vrd16s(0x36144 + (uint32_t)_ph * 2); }
      *piVar8 = (int)sVar2 + (int)(vrd16s(iVar5 + sVar3 * 2));
      if ((sVar2 == 3) && ((((uint32_t)W[0x0C8C] + (uint32_t)vrd16(iVar5 + 0x16)) & 7) == 0)) {
        iVar6 = SCEN_X(param_1, iVar5);
        piVar8[1] = iVar6;
        piVar8[2] = g_camera_offset_y + vrd32s(iVar5 + 0x10);
        iVar1 = SCEN_Z(param_1, iVar5);
        piVar8[3] = iVar1;
        FUN_0000fa9e(iVar6,iVar1);
      }
      else {
        piVar8[1] = SCEN_X(param_1, iVar5);
        piVar8[2] = g_camera_offset_y + vrd32s(iVar5 + 0x10);
        piVar8[3] = SCEN_Z(param_1, iVar5);
      }
      uVar4 = (uint32_t)((vrd16s(iVar5 + 0x14)) & 0xfffc);
      piVar8[4] = vrd16s(0x20B004 + uVar4);
      piVar8[5] = vrd16s(0x20B006 + uVar4);
      uVar4 = W[0x0C8C] * (vrd16s(iVar5 + 0x16)) & 0xfffc;
      piVar8[6] = vrd16s(0x20B004 + uVar4);
      piVar8[7] = vrd16s(0x20B006 + uVar4);
      uVar4 = (uint32_t)((vrd16s(iVar5 + 0x18)) & 0xfffc);
      piVar8[8] = vrd16s(0x20B004 + uVar4);
      piVar8[9] = vrd16s(0x20B006 + uVar4);
      piVar8[10] = 0;
    }
    else if ((int8_t)vrd8(iVar5 + 0x1a) == '\x02') {
      *piVar8 = (int)(vrd16s(iVar5 + sVar3 * 2));
      piVar8[1] = SCEN_X(param_1, iVar5);
      piVar8[2] = g_camera_offset_y + vrd32s(iVar5 + 0x10);
      piVar8[3] = SCEN_Z(param_1, iVar5);
      uVar4 = (uint32_t)((vrd16s(iVar5 + 0x14)) & 0xfffc);
      piVar8[4] = vrd16s(0x20B004 + uVar4);
      piVar8[5] = vrd16s(0x20B006 + uVar4);
      uVar4 = W[0x0C8C] * (vrd16s(iVar5 + 0x16)) & 0xfffc;
      piVar8[6] = vrd16s(0x20B004 + uVar4);
      piVar8[7] = vrd16s(0x20B006 + uVar4);
      uVar4 = (uint32_t)((vrd16s(iVar5 + 0x18)) & 0xfffc);
      piVar8[8] = vrd16s(0x20B004 + uVar4);
      piVar8[9] = vrd16s(0x20B006 + uVar4);
      piVar8[10] = 0;
    }
    else {
      if ((int8_t)vrd8(iVar5 + 0x1a) == '\x03') {
        *piVar8 = (int)(vrd16s(iVar5 + sVar3 * 2));
        piVar8[1] = SCEN_X(param_1, iVar5);
        piVar8[2] = g_camera_offset_y + vrd32s(iVar5 + 0x10);
        piVar8[3] = SCEN_Z(param_1, iVar5);
        uVar4 = (uint32_t)((vrd16s(iVar5 + 0x14)) & 0xfffc);
        piVar8[4] = vrd16s(0x20B004 + uVar4);
        piVar8[5] = vrd16s(0x20B006 + uVar4);
        uVar4 = (uint32_t)((vrd16s(iVar5 + 0x16)) & 0xfffc);
        piVar8[6] = vrd16s(0x20B004 + uVar4);
        piVar8[7] = vrd16s(0x20B006 + uVar4);
        uVar4 = W[0x0C8C] * (vrd16s(iVar5 + 0x18)) & 0xfffc;
        goto LAB_00013edc;
      }
      if ((int8_t)vrd8(iVar5 + 0x1a) != '\x04') {
        if (iVar6 == 8) {
          uVar4 = W[0x0C8C] >> ((int)(int8_t)vrd8(iVar5 + 0x1b) & 0x3fU) & 7;
        }
        else if (iVar6 == 1) {
          uVar4 = 0;
        }
        else {
          uVar4 = iVar6 ? (W[0x0C8C] >> ((int)(int8_t)vrd8(iVar5 + 0x1b) & 0x3fU)) % (iVar6 ? iVar6 : 1) : 0;
        }
        *piVar8 = uVar4 + (int)(vrd16s(iVar5 + sVar3 * 2));
        piVar8[1] = -W[0x0CDC];
        piVar8[2] = -W[0x0CE0];
        piVar8[3] = -W[0x0CE4];
        goto LAB_00013e9c;
      }
      if (iVar6 == 8) {
        uVar4 = W[0x0C8C] >> ((int)(int8_t)vrd8(iVar5 + 0x1b) & 0x3fU) & 7;
      }
      else if (iVar6 == 1) {
        uVar4 = 0;
      }
      else {
        uVar4 = iVar6 ? (W[0x0C8C] >> ((int)(int8_t)vrd8(iVar5 + 0x1b) & 0x3fU)) % (iVar6 ? iVar6 : 1) : 0;
      }
      *piVar8 = uVar4 + (int)(vrd16s(iVar5 + sVar3 * 2));
      piVar8[1] = SCEN_X(param_1, iVar5);
      piVar8[2] = ((int)(vrd16s(iVar5 + 0x18)) *
                   (int)(vrd16s(0x20B004 + (W[0x0C8C] * (vrd16s(iVar5 + 0x14)) & 0xfffcU))) >> 0xf) +
                  g_camera_offset_y + vrd32s(iVar5 + 0x10);
      piVar8[3] = SCEN_Z(param_1, iVar5);
      piVar8[4] = 0;
      piVar8[5] = 0x7fff;
      uVar4 = (uint32_t)((vrd16s(iVar5 + 0x16)) & 0xfffc);
      piVar8[6] = vrd16s(0x20B004 + uVar4);
      piVar8[7] = vrd16s(0x20B006 + uVar4);
      piVar8[8] = 0;
      piVar8[9] = 0x7fff;
      piVar8[10] = 0;
    }
    piVar8 = piVar8 + 0xb;
    local_6 = local_6 + 1;
    iVar5 = iVar5 + 0x20;
  if (++_safety_ctr > 10000) break; } while( true ); _safety_ctr = 0;
}

/* ---- animated_object_deactivate ---- */

/* ROM 0x014FAA: movea.l $4(a7),a0 ; move.w #$8000,$48(a0) ; subq.w #1,$e166a0
 * param_1 is the record's _W[] byte offset, not a host pointer. */
void animated_object_deactivate(uint32_t param_1)

{
  W_SET_HI16(param_1 + 0x48, 0x8000);
  W_SET_HI16(0x166A0, (uint16_t)(W_HI16(0x166A0) - 1));
  return;
}

/* ---- animated_object_spawn ---- */

/* param_1 is a ROM ADDRESS -- a 0x1C-byte spawn record (0xB5D8C and friends,
 * from the per-course seed in animated_objects_reset_all), not a host pointer.
 * Field map from ROM 0x014942: long at 0x00 (the ANIMATION SCRIPT's own ROM
 * address), words at 0x04/06/08/0a/0c/0e/18, longs at 0x10/14, and the SLOT
 * INDEX at 0x1a (`move.w $1a(a2),d0` @0x01495C, then *0x58 + 0xE04178).
 * The script record it points at is longs at 0x00/04/08 and words at
 * 0x0c/0e/10 (ROM 0x014A5A / 0x014ADA). Every one of those was a host
 * dereference, which is why the whole animated-object system was disabled. */
/* a 16-bit record field at byte offset o, in the _W[] model: the HIGH half of
 * a 4-aligned slot, the LOW half of a 2-mod-4 one */
static int16_t aos_w16(uint32_t o) { return (o & 2) ? W_LO16(o - 2) : W_HI16(o); }
static void aos_set16(uint32_t o, int v)
{
  if (o & 2) W_SET_LO16(o - 2, (int16_t)v); else W_SET_HI16(o, (int16_t)v);
}
/* ROM 0x0146B2: muls.l to 64 bits, `add.l d0,d0 ; addx.l d1,d1`, keep the middle
 * 32 bits -- i.e. the product >> 15. (game_core.c's fixed_point_mul_32x32 uses
 * >> 16; see register row 160 before relying on it.) */
static int32_t aos_mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
/* 32-bit `divs.l` that cannot trap the host (the 68K would take an exception) */
static int32_t aos_div(int32_t n, int32_t d)
{
  if (d == 0) return 0;
  if (d == -1) return (int32_t)(0u - (uint32_t)n);
  return n / d;
}
/* ROM 0x0148CA: rotate the vector at `src` about Y by the word at src+0x0E,
 * into `dst` (whose y is cleared). Both are _W[] byte offsets -- the C
 * matrix_rotate_yaw_only indexes _W[] as int32_t, which is not the layout. */
static void aos_rot_yaw(uint32_t src, uint32_t dst)
{
  uint32_t a = (uint16_t)aos_w16(src + 0x0e) & 0x3fff;
  int32_t s = vrd16s(0x20B004 + a * 4), c = vrd16s(0x20B006 + a * 4);
  int32_t x = (int32_t)W[src], z = (int32_t)W[src + 8];
  W[dst + 8] = (int32_t)((uint32_t)aos_mul(z, c) - (uint32_t)aos_mul(x, s));
  W[dst]     = (int32_t)((uint32_t)aos_mul(z, s) + (uint32_t)aos_mul(x, c));
  W[dst + 4] = 0;
}
/* ROM 0x0147F8: pitch by the word at src+0x0C, then yaw by src+0x0E. (The trig
 * EA at 0x01482E prints as `$2(a4, d0.l)` but extension word 0x0C02 is scale 4.) */
static void aos_rot_ypr(uint32_t src, uint32_t dst)
{
  uint32_t a = (uint16_t)aos_w16(src + 0x0c) & 0x3fff;
  int32_t s = vrd16s(0x20B004 + a * 4), c = vrd16s(0x20B006 + a * 4);
  int32_t x = (int32_t)W[src], y = (int32_t)W[src + 4], z = (int32_t)W[src + 8], z1;
  W[dst + 4] = (int32_t)((uint32_t)aos_mul(y, c) - (uint32_t)aos_mul(z, s));
  z1 = (int32_t)((uint32_t)aos_mul(y, s) + (uint32_t)aos_mul(z, c));
  a = (uint16_t)aos_w16(src + 0x0e) & 0x3fff;
  s = vrd16s(0x20B004 + a * 4); c = vrd16s(0x20B006 + a * 4);
  W[dst + 8] = (int32_t)((uint32_t)aos_mul(z1, c) - (uint32_t)aos_mul(x, s));
  W[dst]     = (int32_t)((uint32_t)aos_mul(z1, s) + (uint32_t)aos_mul(x, c));
}

void animated_object_spawn(uint32_t param_1)
{
  /* REWRITTEN FROM ROM 0x014942 (register row 160). The Ghidra form wrote
   * nine 16-bit record fields as WHOLE _W[] slots -- the model at +0x44 among
   * them, which landed in the slot's LOW half while every reader takes the
   * HIGH half, so each object reached the drawer as model 0 -- rotated
   * through helpers that index _W[] as int32_t, and took the drop-in term's
   * multiplier from +0x18 where the ROM reads +0x06 (0x014A7A). */
  const uint32_t p = param_1;
  const uint32_t PV = 0x1668C;          /* a4: scratch vector 0xE1668C       */
  const uint32_t SP = 0x16678;          /* d3: spawner block 0xE16678        */
  int32_t idx = vrd16s(p + 0x1a);
  uint32_t nb = (uint32_t)(0x4178 + idx * 0x58);
  uint32_t sp;
  int16_t f = vrd16s(p + 0x08);

  { extern int g_aolog; static int n;
    if (g_aolog && n++ < 40)
      fprintf(stderr, "[AO] spawn #%d from ROM 0x%X -> slot %d\n", n, (unsigned)p, (int)idx); }

  if (idx < 0 || idx >= 16) return;
  if ((W_HI16(nb + 0x48) & 0x8000) == 0) return;          /* 0x014982: slot busy */
  sp = (uint32_t)vrd32(p);
  if (sp < 0x10000u || sp >= 0x400000u - 0x28u) return;

  if (f & 0x10) {                                         /* 0x014988: this cell only */
    uint32_t cell = ((uint32_t)W[SP + 8] >> 15) * 0x18 + ((uint32_t)W[SP] >> 15);
    if ((int32_t)cell != (int32_t)vrd16s(p + 0x0e)) return;
  }
  if ((f & 0x20) && vrd16s(p + 0x0e) != W_LO16(SP + 0x10)) /* 0x0149B8: word 0xE1668A */
    return;
  if (f & 0x40) {                                         /* 0x0149D0: height gate */
    if (f & 6) {
      if (((uint16_t)W_HI16(SP + 0x0c) & 0x3fff) < 0x2000) return;
      if (vrd32s(p + 0x10) > (int32_t)W[SP + 4]) return;
    } else {
      int32_t d = (int32_t)((uint32_t)vrd32s(sp + 4) - (uint32_t)W[SP + 4]);
      if (d < 0) d = -d;
      if (d > vrd32s(p + 0x10)) return;
    }
  }
  if (vrd16s(p + 0x18) >= 0) {                            /* 0x014A22: heading window */
    int32_t d = (int32_t)vrd16s(p + 0x18) - ((int32_t)W[0x0D10] >> 2);
    uint32_t u = (uint16_t)d & 0x3fff;
    if (u < 0x3800 && u > 0x800) return;
  }

  { extern int g_animobj; static int n;
    if (g_animobj && n++ < 24)
      fprintf(stderr, "[SPAWN] f=%d rec=0x%X slot=%d nb=0x%X script=0x%X flag=%04X\n",
              (int)g_sys.frame_count, p, (int)idx, nb, sp, (unsigned)(uint16_t)f); }

  W[nb + 0x3c] = (int32_t)sp;                             /* 0x014A52 */
  W[nb + 0x40] = (int32_t)sp;
  aos_set16(nb + 0x0c, vrd16s(sp + 0x0c));
  aos_set16(nb + 0x0e, vrd16s(sp + 0x0e));
  aos_set16(nb + 0x10, vrd16s(sp + 0x10));
  aos_set16(nb + 0x12, 0);
  W[PV + 4] = 0;
  W[PV] = 0;
  W[PV + 8] = (int32_t)(((int32_t)vrd16s(p + 0x06) * (int32_t)W[0x0D48]) >> 4);
  aos_set16(PV + 0x0c, W_HI16(SP + 0x0c));                /* 0xE16684 */
  aos_set16(PV + 0x0e, W_LO16(SP + 0x0c));                /* 0xE16686 */
  aos_rot_ypr(PV, nb + 0x14);                             /* jsr 0x0147F8 */
  W[nb + 0x14] = (int32_t)((uint32_t)W[nb + 0x14] + (uint32_t)W[SP]);
  W[nb + 0x18] = (int32_t)((uint32_t)W[nb + 0x18] + (uint32_t)W[SP + 4]);
  W[nb + 0x1c] = (int32_t)((uint32_t)W[nb + 0x1c] + (uint32_t)W[SP + 8]);
  aos_set16(nb + 0x24, 0);
  aos_set16(nb + 0x20, 0);
  aos_set16(nb + 0x22, W_LO16(SP + 0x0c));                /* word 0xE16686 */

  if (f & 2) {                                            /* 0x014ADA */
    W[PV] = vrd32s(sp); W[PV + 4] = vrd32s(sp + 4); W[PV + 8] = vrd32s(sp + 8);
    aos_rot_yaw(PV, nb);                                  /* jsr 0x0148CA */
    W[nb + 0x14] = (int32_t)((uint32_t)W[nb + 0x14] - (uint32_t)W[nb]);
    W[nb + 0x18] = (int32_t)((uint32_t)W[nb + 0x18] - (uint32_t)W[nb + 4]);
    W[nb + 0x1c] = (int32_t)((uint32_t)W[nb + 0x1c] - (uint32_t)W[nb + 8]);
    aos_set16(PV + 0x0c, W_HI16(SP + 0x0c));
    aos_rot_ypr(PV, nb);                                  /* jsr 0x0147F8 */
    W[nb + 0x14] = (int32_t)((uint32_t)W[nb + 0x14] + (uint32_t)W[nb]);
    W[nb + 0x18] = (int32_t)((uint32_t)W[nb + 0x18] + (uint32_t)W[nb + 4]);
    W[nb + 0x1c] = (int32_t)((uint32_t)W[nb + 0x1c] + (uint32_t)W[nb + 8]);
    W[nb + 4] = (int32_t)((uint32_t)W[nb + 4] + (uint32_t)W[SP + 4]);
    if ((f & 0x40) && vrd32s(p + 0x10) > (int32_t)W[nb + 4]) return;
  } else {
    if (f & 1) {                                          /* 0x014B5E */
      W[PV] = vrd32s(sp); W[PV + 8] = vrd32s(sp + 8); W[PV + 4] = 0;
      aos_rot_yaw(PV, nb);
    } else {                                              /* 0x014B78 */
      W[nb] = vrd32s(sp);
      W[nb + 8] = vrd32s(sp + 8);
    }
    W[nb + 4] = vrd32s(sp + 4);                           /* 0x014B82 */
  }
  if (f & 3) {                                            /* 0x014B8A */
    W[nb]     = (int32_t)((uint32_t)W[nb] + (uint32_t)W[nb + 0x14]);
    W[nb + 8] = (int32_t)((uint32_t)W[nb + 8] + (uint32_t)W[nb + 0x1c]);
    aos_set16(nb + 0x0e, (aos_w16(nb + 0x0e) + aos_w16(nb + 0x22)) & 0x3fff);
  }
  aos_set16(nb + 0x44, vrd16s(p + 0x04));                 /* 0x014BB0: the MODEL */
  aos_set16(nb + 0x4a, vrd16s(p + 0x0a));
  aos_set16(nb + 0x4c, vrd16s(p + 0x0c));
  aos_set16(nb + 0x48, f);
  W[nb + 0x50] = vrd32s(p + 0x14);
  W[nb + 0x54] = (f & 0x100) ? vrd32s(p + 0x10) : 0;
  if ((f & 4) == 0)                                       /* 0x014BE4: chunk */
    aos_set16(nb + 0x4e, ((int32_t)W[nb] / 0x18000) + ((int32_t)W[nb + 8] / 0x18000) * 8);
  else
    aos_set16(nb + 0x4e, (int16_t)W[0x0D24]);
  W_SET_HI16(nb + 0x48, W_HI16(nb + 0x48) | 0x3000);
  W_SET_HI16(0x166A0, W_HI16(0x166A0) + 1);               /* addq.w #1,$e166a0 */
}

/* ---- objects_interaction_check ---- */

uint32_t objects_interaction_check(void)

{
  int bVar1;
  uint32_t uVar2;
  int iVar3;
  int iVar4;
  undefined4 *puVar5;
  
  if (W[0x2C24] == 6) {
    uVar2 = W[0x2B3A] & 0xffff0100;
    if ((W[0x2B3A] & 0x100) != 0) {
      uVar2 = 1;
      W[0x16348] = 1;
      W[0x1631C] = W[0x0D00];
      W[0x16320] = W[0x0D04];
      W[0x16324] = W[0x0D08];
      W[0x3EB4] = 0;
      W[0x3EB8] = 1;
      W[0x3EBC] = 0;
    }
    if (W[0x16348] != 0) {
      W[0x1632C] = 0x10000 / (W[0x3EB8] * 0x3c);
      W[0x16328] = W[0x3EB4];
      iVar3 = W[0x0C8C] * W[0x1632C];
      W[0x16338] = W[0x1631C] +
                     (W[0x3EB4] * ((int)(int16_t)vrd16s(0x20B006 + (iVar3 >> 1 & 0x7ffe) * 2) >> 7) >> 8);
      W[0x1633C] = W[0x16320];
      W[0x16340] = W[0x16324] +
                     (W[0x3EB4] * ((int)(int16_t)vrd16s(0x20B004 + (iVar3 >> 1 & 0x7ffe) * 2) >> 7) >> 8);
      W[0x16334] = W[0x3EBC];
      W[0x1634C] = iVar3;
      *(int32_t*)W[0x0CA4] = 0x8010;
      ((int32_t*)W[0x0CA4])[1] = 3;
      ((int32_t*)W[0x0CA4])[2] = W[0x3EBC];
      ((int32_t*)W[0x0CA4])[3] = 0xffffffff;
      ((int32_t*)W[0x0CA4])[4] = 0x2e3;
      ((int32_t*)W[0x0CA4])[5] = W[0x16338] - W[0x0CDC];
      ((int32_t*)W[0x0CA4])[6] = W[0x1633C] - W[0x0CE0];
      ((int32_t*)W[0x0CA4])[7] = W[0x16340] - W[0x0CE4];
      ((int32_t*)W[0x0CA4])[8] = 0;
      ((int32_t*)W[0x0CA4])[9] = 0x7fff;
      if (W[0x1632C] < 1) {
        iVar4 = -0x4000;
      }
      else {
        iVar4 = 0x4000;
      }
      ((int32_t*)W[0x0CA4])[10] = (int)(int16_t)vrd16s(0x20B004 + (iVar4 - iVar3 >> 1 & 0x7ffe) * 2);
      if (W[0x1632C] < 1) {
        iVar4 = -0x4000;
      }
      else {
        iVar4 = 0x4000;
      }
      ((int32_t*)W[0x0CA4])[0xb] = (int)(int16_t)vrd16s(0x20B006 + (iVar4 - iVar3 >> 1 & 0x7ffe) * 2);
      ((int32_t*)W[0x0CA4])[0xc] = 0;
      ((int32_t*)W[0x0CA4])[0xd] = 0x7fff;
      ((int32_t*)W[0x0CA4])[0xe] = 0;
      ((int32_t*)W[0x0CA4])[0xf] = 0x2c2;
      ((int32_t*)W[0x0CA4])[0x10] = W[0x16338] - W[0x0CDC];
      ((int32_t*)W[0x0CA4])[0x11] = W[0x1633C] - W[0x0CE0];
      ((int32_t*)W[0x0CA4])[0x12] = W[0x16340] - W[0x0CE4];
      ((int32_t*)W[0x0CA4])[0x13] =
           (int)(short)vrd16s(0x20B004 + ((int)(W[0x0C8C] * -0x400 & 0xffffU) >> 1) * 2);
      ((int32_t*)W[0x0CA4])[0x14] =
           (int)(short)vrd16s(0x20B006 + ((int)(W[0x0C8C] * -0x400 & 0xffffU) >> 1) * 2);
      if (W[0x1632C] < 1) {
        iVar4 = -0x4000;
      }
      else {
        iVar4 = 0x4000;
      }
      ((int32_t*)W[0x0CA4])[0x15] = (int)(int16_t)vrd16s(0x20B004 + (iVar4 - iVar3 >> 1 & 0x7ffe) * 2);
      if (W[0x1632C] < 1) {
        iVar4 = -0x4000;
      }
      else {
        iVar4 = 0x4000;
      }
      ((int32_t*)W[0x0CA4])[0x16] = (int)(int16_t)vrd16s(0x20B006 + (iVar4 - iVar3 >> 1 & 0x7ffe) * 2);
      ((int32_t*)W[0x0CA4])[0x17] = 0;
      ((int32_t*)W[0x0CA4])[0x18] = 0x7fff;
      ((int32_t*)W[0x0CA4])[0x19] = 0;
      puVar5 = (int32_t*)W[0x0CA4] + 0x1b;
      ((int32_t*)W[0x0CA4])[0x1a] = 0x8010;
      uVar2 = 0xffffffff;
      W[0x0CA4] = W[0x0CA4] + 0x1c * 4;
      *puVar5 = 0xffffffff;
    }
  }
  else {
    if ((W[0x2B3A] & 0x100) != 0) {
      bVar1 = false;
      iVar3 = 0;
      do {
        if ((short)W[0x16014 + (iVar3 * 0xc)] < 0) {
          bVar1 = true;
          break;
        }
        iVar3 = iVar3 + 1;
      } while (iVar3 < 0x20);
      if (bVar1) {
        W[0x16014 + (iVar3 * 0xc)] = 0;
        W[0x16018 + (iVar3 * 6)] = W[0x0D00];
        W[0x1601C + (iVar3 * 6)] = W[0x0D04];
        W[0x16020 + (iVar3 * 6)] = W[0x0D08];
        W[0x16028 + (iVar3 * 0xc)] = 0x2c1;
        W[0x16024 + (iVar3 * 6)] = 0;
        W[0x16314] = (short)iVar3;
      }
    }
    if ((W[0x2B3A] & 8) != 0) {
      bVar1 = false;
      iVar3 = 0;
      do {
        W[0x16314] = (short)(W[0x16314] + 1) % 0x20;
        if (-1 < (short)W[0x16014 + (W[0x16314] * 0xc)]) {
          bVar1 = true;
          break;
        }
        iVar3 = iVar3 + 1;
      } while (iVar3 < 0x20);
      if (bVar1) {
        W[0x0D00] = W[0x16018 + (W[0x16314] * 6)];
        W[0x0D04] = W[0x1601C + (W[0x16314] * 6)];
        W[0x0D08] = W[0x16020 + (W[0x16314] * 6)];
      }
    }
    if ((W[0x2B60] & 0x80) != 0) {
      W[0x16024 + (W[0x16314] * 6)] = W[0x16024 + (W[0x16314] * 6)] + 0x100;
    }
    if ((W[0x2B60] & 0x40) != 0) {
      W[0x16024 + (W[0x16314] * 6)] = W[0x16024 + (W[0x16314] * 6)] + -0x100;
    }
    if ((W[0x2B3A] & 0x400) != 0) {
      W[0x16014 + (W[0x16314] * 0xc)] = 0xffff;
    }
    iVar3 = 0;
    do {
      if (-1 < (short)W[0x16014 + (iVar3 * 0xc)]) {
        *(int32_t*)W[0x0CA4] = 0x8010;
        ((int32_t*)W[0x0CA4])[1] = 3;
        ((int32_t*)W[0x0CA4])[2] = W[0x16024 + (iVar3 * 6)];
        ((int32_t*)W[0x0CA4])[3] = 0xffffffff;
        ((int32_t*)W[0x0CA4])[4] = (int)(short)W[0x16028 + (iVar3 * 0xc)];
        ((int32_t*)W[0x0CA4])[5] = W[0x16018 + (iVar3 * 6)] - W[0x0CDC];
        ((int32_t*)W[0x0CA4])[6] = W[0x1601C + (iVar3 * 6)] - W[0x0CE0];
        ((int32_t*)W[0x0CA4])[7] = W[0x16020 + (iVar3 * 6)] - W[0x0CE4];
        ((int32_t*)W[0x0CA4])[8] = 0;
        ((int32_t*)W[0x0CA4])[9] = 0x7fff;
        ((int32_t*)W[0x0CA4])[10] = (int)(short)vrd16s(0x20B004 + (-W[0x0D10] >> 1 & 0x7ffe) * 2);
        ((int32_t*)W[0x0CA4])[0xb] = (int)(short)vrd16s(0x20B006 + (-W[0x0D10] >> 1 & 0x7ffe) * 2);
        ((int32_t*)W[0x0CA4])[0xc] = 0;
        puVar5 = (int32_t*)W[0x0CA4] + 0xe;
        ((int32_t*)W[0x0CA4])[0xd] = 0x7fff;
        W[0x0CA4] = W[0x0CA4] + 0xf * 4;
        *puVar5 = 0;
      }
      iVar3 = iVar3 + 1;
    } while (iVar3 < 0x20);
    puVar5 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = 0x8010;
    W[0x0CA4] = W[0x0CA4] + 2 * 4;
    *puVar5 = 0xffffffff;
    W[0x3E90] = (int)W[0x16314];
    W[0x3E94] = W[0x16018 + (W[0x16314] * 6)];
    W[0x3E98] = W[0x1601C + (W[0x16314] * 6)];
    W[0x3E9C] = W[0x16020 + (W[0x16314] * 6)];
    uVar2 = W[0x16314] * 0x18;
    W[0x3EA0] = W[0x16024 + (W[0x16314] * 6)];
  }
  return uVar2;
}

/* ---- objects_render_attract_mode ---- */

void objects_render_attract_mode(void)

{
  short *psVar1;
  undefined4 *puVar2;
  short sVar3;
  int *piVar4;
  
  W[0x169E8] = 0x8002;
  puVar2 = (int32_t*)W[0x0CA4] + 1;
  *(int32_t*)W[0x0CA4] = 0x8002;
  W[0x0CA4] = puVar2;
  puVar2 = (int32_t*)W[0x0CA4] + 1;
  *(int32_t*)W[0x0CA4] = 0;
  W[0x0CA4] = puVar2;
  W[0x16670] = -(short)W[0x0D10];
  /* THE TRIG PAIR IS (sin, cos), AND IT IS THE **LOW** HALF OF EACH READ.
   *
   * The ROM does two 32-BIT reads, not two 16-bit ones (0x014552 /
   * 0x014628):
   *     addi.l  #$20b002,d0 ; movea.l d0,a1
   *     move.l  (a1),$e16668 ; move.l $2(a1),$e1666c
   * The table is interleaved [sin, cos] on a 4-byte stride with
   * sin(a) @0x20B004+a and cos(a) @0x20B006+a (verified against math.sin/cos:
   * 0x20B006 is exact, 0x20B002 is the cos lane lagging one entry). So as
   * big-endian 32-bit words:
   *     (a1)      @0x20B002+a = [cos(a-4) : sin(a)]   low half = sin(a)
   *     $2(a1)    @0x20B004+a = [sin(a)   : cos(a)]   low half = cos(a)
   * and the master consumes only the LOW 16 bits of a matrix word.
   *
   * Taking the HIGH halves -- which is what `vrd16s(0x20B002+a)` /
   * `vrd16s(0x20B004+a)` did -- yields (cos, sin), i.e. the pair backwards.
   * Measured: our code-874 Y pair came out (11980, 30503) = (cos .3656,
   * sin .9309) where MAME's own object matrix for 874 is a PURE Y ROTATION
   * [[5983,0,-15252],[0,16383,0],[15251,0,5983]], cos .3652 / sin .9309 --
   * same numbers, swapped. That is what made the renderer look as though
   * 0x8002 entries were cos-first; they are not. The ROM's other emitter
   * settles it twice over: scenery_object_render @0x013AC0 writes the X and
   * Z pairs as the literals (0, 0x7FFF) -- identity read SIN-first, ninety
   * degrees read cos-first -- and dsp_cmd_place_object_rotated @0x022008
   * writes 0x20B004 (sin) before 0x20B006 (cos). */
  W[0x16668] = vrd16s(0x20B004 + ((uint32_t)W[0x16670] & 0xfffc));   /* sin */
  W[0x1666C] = vrd16s(0x20B006 + ((uint32_t)W[0x16670] & 0xfffc));   /* cos */
  W[0x16672] = 1;
  /* 16-BIT at a 4-aligned offset = the HIGH half, the same store
   * scenery_render_all_lod makes (ROM 0x014080 `move.w #$2,(a0)`).
   * Writing the whole slot put 2 in the LOW half; the reader in
   * scenery_object_render then read the low half too, so attract happened
   * to agree with itself while gameplay did not. Both halves of that
   * disagreement are fixed together -- the value attract sees is
   * unchanged. */
  W_SET_HI16(0x16674, 2);
  piVar4 = &W[0x0E6C];
  W[0x16664] = W[0x0E0C] * 0x600 + 0xadcd8;          /* LOD0 table, ROM address */
  for (sVar3 = 0; (uint32_t)(int)sVar3 < g_visible_chunk_count; sVar3 = sVar3 + 1) {
    /* Per-chunk entry = table + chunk_id*4, a 16-bit big-endian type index
     * (CLAUDE.md fix #21). This used a ROM address as a HOST pointer and
     * read it natively. scenery_object_render reads *param_2 as a short,
     * so hand it a decoded local. */
    /* stride 0x10 -- ROM 0x014686 `lea $10(a2),a2`. The list entry is a W[]
     * SLOT INDEX now, and scenery_object_render takes a ROM address for its
     * second argument (the per-chunk LOD entry), not a decoded local. */
    uint32_t rec_slot = 0x0E6C + (uint32_t)sVar3 * 0x10;
    uint32_t entry = (uint32_t)W[0x16664] + (uint32_t)W[rec_slot] * 4;
    if (entry + 1 < ROM_SIZE && vrd16s(entry) >= 0) {
      scenery_object_render(rec_slot, entry);
    }
  }
  (void)psVar1;
  *(int32_t*)W[0x0CA4] = (int32_t)0xffffffff;
  scenery_course_dispatch();
  return;
}

/* ---- animated_object_keyframe_update ---- */

void animated_object_keyframe_update(int param_1,int param_2,int param_3)
{
  /* REWRITTEN FROM ROM 0x014C6C (register row 160). THE PRE-ROLL: at level
   * start the per-course seed spawns `param_2` consecutive records (0x1C bytes
   * each, from ROM `param_1`) and fast-forwards every one of them to
   *   phase = frame_counter mod param_3
   * frames into its script, so the train is already somewhere along its track
   * rather than always at the first keyframe. The Ghidra form read the script
   * natively (byte-swapped -- this is where the train's x = 526715136, i.e.
   * 615711 swapped, came from), wrote six 16-bit fields as whole slots, left
   * the jump table at 0x014CEA as a bare `return`, and stored the chunk at a
   * 0x2C record stride where the record is 0x58. */
  uint32_t p = (uint32_t)param_1;
  int32_t period = param_3, phase, t = (int32_t)W[0x0C8C];
  int k;

  if (period <= 0) return;
  while (t < 0) t += period;                              /* 0x014C80 */
  phase = t % period;                                     /* divsl.l d5,d0:d1 */
  for (k = 0; k < param_2; k++, p += 0x1c) {              /* 0x014EA6 */
    int32_t idx = vrd16s(p + 0x1a), d2 = phase, cnt, i, guard = 0;
    uint32_t nb, a4, d4;

    animated_object_spawn(p);
    if (idx < 0 || idx >= 16) continue;
    nb = (uint32_t)(0x4178 + idx * 0x58);
    a4 = (uint32_t)W[nb + 0x3c];
    if (a4 < 0x10000u || a4 >= 0x400000u - 0x28u) continue;
    d4 = a4;
    while (d2 >= (int32_t)W_LO16(nb + 0x10)) {            /* 0x014D2C */
      d2 -= W_LO16(nb + 0x10);
      d4 = a4;
      while (vrd16s(a4 + 0x12) < 0) {                     /* 0x014D1A */
        switch ((uint16_t)vrd16s(a4 + 0x12)) {            /* table at 0x014CEA */
        case 0x82F3:
          W[nb + 0x54] = vrd32s(a4 + 0x14);
          W_SET_HI16(nb + 0x48, W_HI16(nb + 0x48) ^ 0x200);
          break;
        case 0x82F9: W[nb + 0x54] = vrd32s(a4 + 0x14); break;
        case 0x82FA: W[nb + 0x50] = vrd32s(a4 + 0x14); break;
        default: break;                                   /* 0x014D16: skipped */
        }
        a4 += 0x14;
        if (++guard > 4096 || a4 >= 0x400000u - 0x28u) goto next;
      }
      W_SET_LO16(nb + 0x10, vrd16s(a4 + 0x12));           /* 0x014D20 */
      a4 += 0x14;
      if (++guard > 4096 || a4 >= 0x400000u - 0x28u) goto next;
    }
    /* 0x014D34: sit on keyframe d4, heading for a4 */
    for (i = 0; i < 3; i++) W[nb + i * 4] = vrd32s(d4 + i * 4);
    for (i = 0; i < 3; i++) aos_set16(nb + 0x0c + i * 2, vrd16s(d4 + 0x0c + i * 2));
    W[nb + 0x3c] = (int32_t)a4;
    for (i = 0; i < 5; i++) W[nb + 0x28 + i * 4] = vrd32s(a4 + i * 4);
    for (i = 0; i < 3; i++) {                             /* 0x014D6A: short way round */
      uint32_t cur = nb + 0x0c + i * 2, tgt = nb + 0x34 + i * 2;
      int32_t d = (int32_t)aos_w16(tgt) - (int32_t)aos_w16(cur);
      if (d > 0x2000)       aos_set16(cur, aos_w16(cur) + 0x4000);
      else if (d < -0x2000) aos_set16(cur, aos_w16(cur) - 0x4000);
    }
    cnt = W_LO16(nb + 0x10);                              /* 0x014DE2: d2/cnt of the way */
    for (i = 0; i < 3; i++) {
      int32_t d = (int32_t)((uint32_t)W[nb + 0x28 + i * 4] - (uint32_t)W[nb + i * 4]);
      d = (int32_t)((uint32_t)d * (uint32_t)d2);          /* muls.l: 32-bit product */
      W[nb + i * 4] = (int32_t)((uint32_t)W[nb + i * 4] + (uint32_t)aos_div(d, cnt));
    }
    for (i = 0; i < 3; i++) {
      uint32_t cur = nb + 0x0c + i * 2, tgt = nb + 0x34 + i * 2;
      int32_t d = (int32_t)aos_w16(tgt) - (int32_t)aos_w16(cur);
      d = (int32_t)((uint32_t)d * (uint32_t)d2);
      aos_set16(cur, aos_w16(cur) + (int16_t)aos_div(d, cnt));
    }
    W_SET_LO16(nb + 0x10, W_LO16(nb + 0x10) - d2);        /* 0x014E80 */
    aos_set16(nb + 0x4e, ((int32_t)W[nb] / 0x18000) + ((int32_t)W[nb + 8] / 0x18000) * 8);
  next: ;
  }
}


/* ---- animated_object_script_update @ 0x014FBC ---- */

/* REWRITTEN FROM THE DISASSEMBLY (register row 160) -- this is what walks the
 * keyframe script of an animated object: the TRAIN (slots 8..14, spawned from
 * ROM 0xB5D8C) and every other record in the 0xE04178 array.
 *
 * `nb` is the record's _W[] byte offset (0x4178 + i*0x58). a3 is the SCRIPT
 * CURSOR, a ROM address held in +0x3C. The Ghidra version read the script
 * through a host pointer -- which did not crash only because the loader maps
 * the program ROM at its own M68K addresses -- so every script long came back
 * BYTE-SWAPPED: y = 85424 read as -1337130752, the exact value register row
 * 124 recorded. It also dropped the value of four opcodes (0x82FB set the
 * MODEL word +0x44 to 0, which is why every car reached the drawer as
 * model 0), the sound id of 0x82F8, the spawn of 0x82FD, and indexed the
 * 0x82F4 table at *8 where the ROM has `(d0.w*4)`.
 *
 * Script entry = 0x14 bytes: x,y,z (longs) then words +0x0C/+0x0E/+0x10 (angles)
 * and +0x12 (a DURATION when positive, an OPCODE 0x82F3..0x8300 when not).
 * An opcode's parameter is the first long of the entry after it.
 *
 * Record fields (words live in the HIGH half of a 4-aligned slot, LOW half of
 * a 2-mod-4 one): 0x00/04/08 pos, 0x0C/0E/10 angles, 0x12 count, 0x14/18/1C
 * base, 0x22 base yaw, 0x28/2C/30 target pos, 0x34/36/38 target angles,
 * 0x3C cursor, 0x40 loop point, 0x44 model, 0x48 flags, 0x4E chunk,
 * 0x50/0x54 longs set by 0x82FA/0x82F9. */

extern int32_t sound_play(int32_t scene_id);

uint32_t animated_object_script_update(uint32_t nb)
{
  uint32_t a3 = (uint32_t)W[nb + 0x3c];
  int i;

  if (a3 < 0x10000u || a3 >= 0x400000u - 0x28u) {   /* not a ROM script address */
    static int _warned;
    if (!_warned) { _warned = 1;
      fprintf(stderr, "[AO] record 0x%X: script cursor 0x%X is not in ROM, skipping\n",
              (unsigned)nb, (unsigned)a3); }
    return 0;
  }

  /* 0x014FD8 `tst.w $12(a2) ; bgt 0x15258` -- a running keyframe skips the script */
  if (W_LO16(nb + 0x10) <= 0) {
    int guard = 0;
    do {
      uint32_t a0 = a3;                                   /* 0x014FE2 */
      W_SET_HI16(nb + 0x48, W_HI16(nb + 0x48) & 0xbfff);
      a3 += 0x14;
      W_SET_LO16(nb + 0x10, vrd16s(a0 + 0x12));
      switch ((uint16_t)W_LO16(nb + 0x10)) {              /* table at 0x015014 */
      case 0x82F3:                                        /* 0x015106 */
        W[nb + 0x54] = vrd32s(a3);
        W_SET_HI16(nb + 0x48, W_HI16(nb + 0x48) ^ 0x200);
        break;
      case 0x82F4: {                                      /* 0x0150F6 */
        /* move.w d1, $e166a6(d0.w*4) -- 0xE166A6 is 2-mod-4, the LOW half */
        int32_t off = 0x166A4 + (int32_t)W_HI16(nb + 0x44) * 4;
        if (off >= 0 && off < WORK_RAM_SIZE - 4)
          W_SET_LO16((uint32_t)off, (int16_t)vrd32s(a3));
        break; }
      case 0x82F5:                                        /* 0x0150A0: nothing */
        break;
      case 0x82F6:                                        /* 0x01503E: set loop point */
        W[nb + 0x40] = W[nb + 0x3c];
        break;
      case 0x82F7:                                        /* 0x01504A: loop */
        W_SET_LO16(nb + 0x10, vrd16s(a3 + 0x12));
        a3 = (uint32_t)W[nb + 0x40];
        break;
      case 0x82F8:                                        /* 0x015094: jsr $f394 */
        sound_play((int16_t)vrd32s(a3));
        break;
      case 0x82F9:                                        /* 0x0150F0 */
        W[nb + 0x54] = vrd32s(a3);
        break;
      case 0x82FA:                                        /* 0x0150EA */
        W[nb + 0x50] = vrd32s(a3);
        break;
      case 0x82FB:                                        /* 0x0150E2: the MODEL */
        W_SET_HI16(nb + 0x44, (int16_t)vrd32s(a3));
        break;
      case 0x82FC:                                        /* 0x0150D4: hold */
        W_SET_LO16(nb + 0x10, (int16_t)vrd32s(a3));
        W_SET_HI16(nb + 0x48, W_HI16(nb + 0x48) | 0x7000);
        break;
      case 0x82FD: {                                      /* 0x0150A4: spawn a batch */
        int8_t b0 = (int8_t)vrd32s(a3), b1 = (int8_t)vrd32s(a3 + 4);
        W[0x16678] = (int32_t)W[nb];
        W[0x1667C] = (int32_t)W[nb + 0x4];
        W[0x16680] = (int32_t)W[nb + 0x8];
        W_SET_LO16(0x16684, W_LO16(nb + 0xc));            /* 0xE16686 <- word +0x0E */
        /* jsr $14c2a with a pointer to the two bytes -- spawn_batch inlined,
         * because its C form takes a ROM address and these bytes are not in ROM */
        for (i = 0; i < b1; i++)
          animated_object_spawn((uint32_t)(0xb2b78 + ((int)b0 + i) * 0x1c));
        break; }
      case 0x82FE: {                                      /* 0x015072: jump + set pose */
        int32_t n = vrd32s(a3);
        a3 += (uint32_t)(n * 0x14);
        for (i = 0; i < 5; i++) W[nb + i * 4] = vrd32s(a3 + i * 4);
        a3 += 0x14;
        break; }
      case 0x82FF: {                                      /* 0x01505A: jump */
        int32_t n;
        W_SET_LO16(nb + 0x10, vrd16s(a3 + 0x12));
        n = vrd32s(a3);
        a3 += (uint32_t)(n * 0x14);
        break; }
      case 0x8300:                                        /* 0x015030: end */
        animated_object_deactivate(nb);
        return 0;
      default:                                            /* 0x015112: a keyframe */
        if ((W_HI16(nb + 0x48) & 0x2000) == 0)
          W_SET_HI16(nb + 0x48, W_HI16(nb + 0x48) & 0xefff);
        if ((W_HI16(nb + 0x48) & 0x880) == 0)
          W_SET_HI16(nb + 0x48, W_HI16(nb + 0x48) & 0xdfff);
        else
          W_SET_HI16(nb + 0x48, W_HI16(nb + 0x48) | 0x2000);
        break;
      }
      if (a3 < 0x10000u || a3 >= 0x400000u - 0x28u) return 0;
      if (++guard > 4096) break;
    } while (W_LO16(nb + 0x10) < 0);                      /* 0x01513A */

    W[nb + 0x3c] = (int32_t)a3;                           /* 0x015142 */
    if ((W_HI16(nb + 0x48) & 3) == 0) {
      for (i = 0; i < 5; i++) W[nb + 0x28 + i * 4] = vrd32s(a3 + i * 4);
    }
    else {                                                /* 0x015166: yaw-relative */
      int32_t vx = vrd32s(a3), vz = vrd32s(a3 + 8), s, c;
      uint32_t ang;
      W[0x1668C] = vx;
      W[0x16690] = (W_HI16(nb + 0x48) & 2) ? vrd32s(a3 + 4) : 0;
      W[0x16694] = vz;
      W_SET_LO16(0x16698, W_LO16(nb + 0x20));             /* 0xE1669A <- word +0x22 */
      /* jsr 0x0148CA (matrix_rotate_yaw_only) inlined: its C form indexes the
       * _W[] array as int32_t, which is not the layout */
      ang = (uint16_t)W_LO16(0x16698) & 0x3fff;
      s = vrd16s(0x20B004 + ang * 4);
      c = vrd16s(0x20B006 + ang * 4);
      W[nb + 0x30] = aos_mul(vz, c) - aos_mul(vx, s);
      W[nb + 0x28] = aos_mul(vz, s) + aos_mul(vx, c);
      W[nb + 0x2c] = 0;
      W[nb + 0x28] = (int32_t)((uint32_t)W[nb + 0x28] + (uint32_t)W[nb + 0x14]);
      W[nb + 0x30] = (int32_t)((uint32_t)W[nb + 0x30] + (uint32_t)W[nb + 0x1c]);
      if (W_HI16(nb + 0x48) & 2)
        W[nb + 0x2c] = (int32_t)((uint32_t)W[nb + 0x2c] + (uint32_t)W[nb + 0x18]);
      else
        W[nb + 0x2c] = vrd32s(a3 + 4);
      W_SET_HI16(nb + 0x34, vrd16s(a3 + 0xc));
      W_SET_LO16(nb + 0x34, (int16_t)((W_LO16(nb + 0x20) + vrd16s(a3 + 0xe)) & 0x3fff));
      W_SET_HI16(nb + 0x38, vrd16s(a3 + 0x10));
    }
    /* 0x0151E0: take the short way round to each target angle */
    for (i = 0; i < 3; i++) {
      uint32_t cur = nb + 0x0c + i * 2, tgt = nb + 0x34 + i * 2;
      int32_t d = (int32_t)aos_w16(tgt) - (int32_t)aos_w16(cur);
      if (d > 0x2000)       aos_set16(cur, aos_w16(cur) + 0x4000);
      else if (d < -0x2000) aos_set16(cur, aos_w16(cur) - 0x4000);
    }
  }

  /* 0x015258 */
  if (W_HI16(nb + 0x48) & 0x4000) {                       /* holding */
    W_SET_LO16(nb + 0x10, W_LO16(nb + 0x10) - 1);
    return 0;
  }
  {
    int32_t cnt = W_LO16(nb + 0x10);                      /* move.w ; ext.l ; divs.l */
    int32_t chunk;
    for (i = 0; i < 3; i++) {                             /* 0x01526A: position */
      int32_t d = (int32_t)((uint32_t)W[nb + 0x28 + i * 4] - (uint32_t)W[nb + i * 4]);
      if (d != 0)
        W[nb + i * 4] = (int32_t)((uint32_t)W[nb + i * 4] + (uint32_t)aos_div(d, cnt));
    }
    for (i = 0; i < 3; i++) {                             /* 0x0152AE: angles */
      uint32_t cur = nb + 0x0c + i * 2, tgt = nb + 0x34 + i * 2;
      int32_t d = (int32_t)aos_w16(tgt) - (int32_t)aos_w16(cur);
      if (d != 0) aos_set16(cur, aos_w16(cur) + (int16_t)aos_div(d, cnt));
    }
    if ((W_HI16(nb + 0x48) & 0x400) == 0)                 /* 0x015302: face the camera */
      aos_set16(nb + 0x0e, (int16_t)W[0x16670]);          /* writers store it full-slot */
    W_SET_LO16(nb + 0x10, W_LO16(nb + 0x10) - 1);
    if (W_HI16(nb + 0x48) & 4)                            /* 0x015318 */
      chunk = (int32_t)W[0x0D24];
    else
      chunk = ((int32_t)W[nb + 8] / 0x18000) * 8 + (int32_t)W[nb] / 0x18000;
    W_SET_LO16(nb + 0x4c, (int16_t)chunk);                /* 0x015344: word +0x4E */
    return (uint32_t)chunk;
  }
}





/* ---- render_animated_birds @ 0x01433E ---- */



void render_animated_birds(void)

{
  uint8_t *puVar1;
  uint32_t uVar2;
  int iVar3;
  int iVar4;
  int iVar5;
  int iVar6;
  int *piVar7;
  int *piVar8;
  int *piVar9;
  uint8_t **ppuVar10;
  uint8_t **ppuVar11;
  
  piVar7 = W[0x0CA4];
  if (W[0x4704] != 0) {
    piVar8 = (intptr_t)((int32_t*)W[0x0CA4] + 1);
    *(int32_t*)W[0x0CA4] = 0x8010;
    piVar7 = piVar7 + 2;
    *piVar8 = -1;
    W[0x4704] = 0;
  }
  piVar8 = &g_sys.rom[0x361DC];
  iVar6 = 0;
  do {
    *piVar7 = ((W[0x0C8C] >> 2) - iVar6 & 7U) + 0x1d8;
    uVar2 = *piVar8 + W[0x0C8C] * 0x20 & 0xfffc;
    iVar3 = piVar8[1] * (int)vrd16s(0x20B004 + uVar2) >> 0xf;
    iVar4 = piVar8[2] * (int)vrd16s(0x20B006 + uVar2) >> 0xf;
    iVar5 = math_atan2(iVar3,iVar4);
    piVar9 = piVar8 + 4;
    piVar7[1] = iVar3 + (piVar8[3] - W[0x0CDC]);
    piVar8 = piVar8 + 5;
    iVar4 = *piVar9 + iVar4;
    piVar7[2] = ((iVar4 * 0xe4 + -0x292926c) / 0x5812 + 0x12b53) - W[0x0CE0];
    piVar7[3] = iVar4 - W[0x0CE4];
    piVar7[4] = 0;
    piVar7[5] = 0x7fff;
    piVar7[6] = vrd16s(0x20B002 + (-iVar5 & 0xfffcU));
    piVar7[7] = vrd16s(0x20B004 + (-iVar5 & 0xfffcU));
    piVar7[8] = 0;
    piVar9 = piVar7 + 10;
    piVar7[9] = 0x7fff;
    piVar7 = piVar7 + 0xb;
    *piVar9 = 4;
    iVar6 = iVar6 + 1;
  } while (iVar6 < 0x10);
  ppuVar10 = &g_sys.rom[0x36190];
  iVar6 = 0;
  do {
    *piVar7 = ((W[0x0C8C] >> 3) - iVar6 & 3U) + 0x1d4;
    piVar7[1] = (int)*ppuVar10 - W[0x0CDC];
    piVar7[2] = (int)ppuVar10[1] - W[0x0CE0];
    piVar7[3] = (int)ppuVar10[2] - W[0x0CE4];
    puVar1 = ppuVar10[3];
    piVar7[4] = vrd16s(0x20B002 + ((uint32_t)puVar1 & 0xfffc));
    piVar7[5] = vrd16s(0x20B004 + ((uint32_t)puVar1 & 0xfffc));
    ppuVar11 = ppuVar10 + 5;
    puVar1 = ppuVar10[4];
    piVar7[6] = vrd16s(0x20B002 + ((uint32_t)puVar1 & 0xfffc));
    piVar7[7] = vrd16s(0x20B004 + ((uint32_t)puVar1 & 0xfffc));
    ppuVar10 = ppuVar10 + 6;
    puVar1 = *ppuVar11;
    piVar7[8] = vrd16s(0x20B002 + ((uint32_t)puVar1 & 0xfffc));
    piVar8 = piVar7 + 10;
    piVar7[9] = vrd16s(0x20B004 + ((uint32_t)puVar1 & 0xfffc));
    piVar7 = piVar7 + 0xb;
    *piVar8 = 4;
    iVar6 = iVar6 + 1;
  } while (0 < (int)*ppuVar10);
  W[0x0CA4] = piVar7;
  W[0x41BC] = ((uint16_t)(W[0x0C8C] >> 2) & 7) + 0x1d8;
  return;
}





/* ---- render_ferris_wheel @ 0x01428C ---- */

void render_ferris_wheel(void)

{
  int *piVar1;
  
  if (W[0x4704] != 0) {
    piVar1 = (intptr_t)((int32_t*)W[0x0CA4] + 1);
    *(int32_t*)W[0x0CA4] = 0x8010;
    W[0x0CA4] = W[0x0CA4] + 2 * 4;
    *piVar1 = -1;
    W[0x4704] = 0;
  }
  *(int32_t*)W[0x0CA4] = (W[0x0C8C] >> 2 & 7U) + 0x1e1;
  ((int32_t*)W[0x0CA4])[1] = 400000 - W[0x0CDC];
  ((int32_t*)W[0x0CA4])[2] = 0xa000 - W[0x0CE0];
  ((int32_t*)W[0x0CA4])[3] = 690000 - W[0x0CE4];
  ((int32_t*)W[0x0CA4])[4] = 0;
  ((int32_t*)W[0x0CA4])[5] = 0x7fff;
  ((int32_t*)W[0x0CA4])[6] = CONCAT22(g_sys.rom[0x21A1CA],g_sys.rom[0x21A1CA]);
  ((int32_t*)W[0x0CA4])[7] = CONCAT22(g_sys.rom[0x21A1CA],g_sys.rom[0x21A1CE]);
  ((int32_t*)W[0x0CA4])[8] = 0;
  ((int32_t*)W[0x0CA4])[9] = 0x7fff;
  ((int32_t*)W[0x0CA4])[10] = 4;
  W[0x169E8] = 0x8000;
  ((int32_t*)W[0x0CA4])[0xb] = 0x8000;
  ((int32_t*)W[0x0CA4])[0xc] = 0;
  ((int32_t*)W[0x0CA4])[0xd] = 0x1e0;
  ((int32_t*)W[0x0CA4])[0xe] = -W[0x0CDC];
  ((int32_t*)W[0x0CA4])[0xf] = 0xa000 - W[0x0CE0];
  ((int32_t*)W[0x0CA4])[0x10] = -W[0x0CE4];
  W[0x0CA4] = W[0x0CA4] + 0x11 * 4;
  return;
}





/* ---- scenery_objects_render_dynamic @ 0x015912 ---- */

void scenery_objects_render_dynamic(void)

{
  /* THE RENDERER FOR ANIMATED-OBJECT SLOTS 8..15, and the only caller of
   * collision_geometry_update -- which is why W[0x291C] read 0 where MAME
   * holds 7 (register row 96's open tail).
   *
   * ROM 0x01592E `movea.l #$e04438,a1` .. 0x015AF2 `lea $58(a1),a1 ;
   * cmpa.l #$e046f8,a1 ; bne` -- eight records of 0x58 bytes. The C walked it
   * `piVar6 += 0x16` on an `int *`, i.e. 0x58 HOST bytes, but `_W` is
   * `intptr_t[]` indexed BY BYTE OFFSET, so the sentinel `piVar6 == &W[0x46F8]`
   * lies 0x1600 host bytes away and 0x1600/0x58 is not an integer -- the
   * comparison could never fire. The loop ran to its 10000-iteration safety
   * guard every frame and the tail, including the collision_geometry_update
   * call at 0x015B06, was never reached.
   *
   * Field offsets confirmed against the disassembly:
   *   0x00 x  0x04 y  0x08 z  0x0c angA  0x0e angB  0x10 angC
   *   0x44 model  0x48 flags  0x4e chunk  0x50 czA  0x54 czB
   * `move.w $48(a1)` is the HIGH half of the 4-aligned slot and `$4e` the LOW
   * half of slot 0x4c -- the W_HI16/W_LO16 rule.
   *
   * The DSP cursor is loaded ONCE at 0x015928 and written back at 0x015B00. */
  uint32_t nb;
  int iVar2;
  int iVar3;
  uint32_t uVar4;
  int iVar5;
  int32_t *cur = (int32_t *)W[0x0CA4];
  const uint8_t *lo = g_sys.dspram, *hi = g_sys.dspram + DSPRAM_SIZE;

  if ((const uint8_t *)cur < lo || (const uint8_t *)(cur + 0x40) > hi) return;

  { extern int g_aorec; static int _shown; uint32_t _n;
    if (g_aorec && _shown < 3) { _shown++;
      for (_n = 0x4438; _n != 0x46F8; _n += 0x58)
        fprintf(stderr, "[AOREC] slot%2u model=%-5d flags=%04X chunk=%-6d "
                "x=%-12d y=%-12d z=%-12d\n",
                (unsigned)((_n - 0x4438) / 0x58 + 8),
                (int)W_HI16(_n + 0x44), (unsigned)(W_HI16(_n + 0x48) & 0xFFFF),
                (int)(short)W_LO16(_n + 0x4c),
                (int)(int32_t)W[_n], (int)(int32_t)W[_n + 4],
                (int)(int32_t)W[_n + 8]); } }

  for (nb = 0x4438; nb != 0x46F8; nb += 0x58) {
    if ((W_HI16(nb + 0x48) & 0x8000) != 0) continue;
    if ((uint8_t)(&g_chunk_lookup_table)[(short)W_LO16(nb + 0x4c)] >= 10) continue;
    W_SET_HI16(nb + 0x48, W_HI16(nb + 0x48) | 0x800);

    iVar2 = g_camera_offset_y + (int)(int32_t)W[nb + 0x04];
    if ((W_HI16(nb + 0x48) & 0x200) == 0) {
      if (iVar2 < 1) { iVar3 = (int)(int32_t)W[nb + 0x50]; goto emit; }
      if ((W[0x0E0C] == 2) && ((W[0x0CF4] | 8) == 0x3d) &&
          (0x1000 < (W[0x0CE8] & 0xffff)))
        iVar3 = (int)(int32_t)W[nb + 0x54] + (int)(int32_t)W[nb + 0x50] + 0x1000;
      else
        iVar3 = (int)(int32_t)W[nb + 0x54] + (int)(int32_t)W[nb + 0x50];
    }
    else {
      if ((iVar2 < 0) && (-0x2000 < iVar2)) {
        iVar3 = (int)(int32_t)W[nb + 0x08] - (int)W[0x0CE4];
        if (iVar3 < 0) iVar3 = -iVar3;
        if (iVar3 < 0x2000) { iVar3 = (int)(int32_t)W[nb + 0x50]; goto emit; }
        /* 0x1599C..0x1599E: the 0x16000/0xe000 constant is compared against a
         * register that is 0 here, so that branch is never taken and the
         * heading test below always runs. */
        iVar3 = (int)(((int)(short)W_LO16(nb + 0x0c) + 0x2000U & 0x3fff) -
                      ((int)W[0x16670] >> 2 & 0x3fffU));
        if (iVar3 < 0) iVar3 = -iVar3;
        iVar5 = (W[0x0E0C] == 0) ? 0x400 : 0x300;
        if ((iVar3 < iVar5) || (0x4000 - iVar5 < iVar3) ||
            ((0x2000 - iVar5 < iVar3) && (iVar3 < iVar5 + 0x2000))) {
          iVar3 = (int)(int32_t)W[nb + 0x50]; goto emit;
        }
      }
      iVar3 = (int)(int32_t)W[nb + 0x54];
    }
  emit:
    if ((const uint8_t *)(cur + 0x20) > hi) break;   /* 0x11 words max below */
    if (iVar3 != (int)W[0x4704]) {
      *cur++ = 0x8010;
      W[0x4704] = iVar3;
      if (iVar3 != 0) { *cur++ = 3; *cur++ = (int32_t)W[0x4704]; }
      *cur++ = -1;
    }
    *cur++ = (int)(short)W_HI16(nb + 0x44);
    *cur++ = (int32_t)((int32_t)W[nb] - (int32_t)W[0x0CDC]);
    *cur++ = iVar2;
    *cur++ = (int32_t)((int32_t)W[nb + 0x08] - (int32_t)W[0x0CE4]);
    /* THE TRIG PAIRS ARE `move.l` READS OFF trigbase-2 -- register row 87.
     * 0x015AB0 `lea $20b002(d0.l*4),a4 ; move.l (a4),(a3)+ ; move.l $2(a4),(a3)+`
     * takes 32 bits at 0x20B002+a*4 and at 0x20B004+a*4, and the master uses
     * only the LOW 16 of each: sin at 0x20B004+a*4, cos at 0x20B006+a*4. Read
     * as 16-bit AT the `move.l` addresses -- which is what this did -- they are
     * the HIGH halves, i.e. (cos_lagged, sin): the pair swapped. */
    uVar4 = (uint32_t)(W_HI16(nb + 0x0c) & 0x3fff) * 4;
    *cur++ = vrd16s(0x20B004 + uVar4);
    *cur++ = vrd16s(0x20B006 + uVar4);
    uVar4 = (uint32_t)(W_LO16(nb + 0x0c) & 0x3fff) * 4;
    *cur++ = vrd16s(0x20B004 + uVar4);
    *cur++ = vrd16s(0x20B006 + uVar4);
    uVar4 = (uint32_t)(W_HI16(nb + 0x10) & 0x3fff) * 4;
    *cur++ = vrd16s(0x20B004 + uVar4);
    *cur++ = vrd16s(0x20B006 + uVar4);
    *cur++ = 4;
  }
  W[0x0CA4] = (intptr_t)cur;
  collision_geometry_update();
}






int g_animobj = 1;   /* PROPCYCL_ANIMOBJ=0 turns it off (register row 160) */
int g_aolog = 0;
int g_lodlog = 0;   /* PROPCYCL_ANIMOBJ=1 -- see rows 10/29 */

int g_scenery_off = 0;   /* PROPCYCL_NO_SCENERY=1 */
