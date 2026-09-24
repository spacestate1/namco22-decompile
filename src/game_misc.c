/*
 * Miscellaneous (unnamed functions)
 * Auto-split from game_deps.c / game_ported.c
 */
#include "propcycl.h"
#include "ending_rd.h"
#include "ea68k.h"
#include "audio_hle.h"

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

/* Forward declarations (called before definition in this file) */
uint32_t FUN_0000a0f6();
uint32_t FUN_0000e5fc();
uint32_t FUN_0000ee8e();

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
uint32_t FUN_000268ce_any(intptr_t node_p, intptr_t list_p);
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

/* ---- FUN_0000a33e ---- */

void FUN_0000a33e(void)

{
  int iVar1;
  
  iVar1 = 0;
  do {
    cgram_load_tile_block((short)iVar1 + 0x10b, iVar1 * 6 + 0x120);
    iVar1 = iVar1 + 1;
  } while (iVar1 < 4);
  cz_load_color_ramp(0x10b, 1);   /* ROM 0x00A364 `move.b #$1,-(a7)` -- palette byte lost (row 118) */
  return;
}


/* ---- FUN_0000e016 ---- */

void FUN_0000e016(void)

{
  FUN_0000e0e8(W[0x0E48],0xfffffe78,0x130,0x2d0,0xfffffff0,0);
  if (W[0x0E0C] != 3) {
    if (W[0x0E50] < (W[0x0E4C] << 3) / 10) {
      W[0x0E50] = W[0x0E50] + 1;
    }
    FUN_0000e37e(1,W[0x0E50],0x13a,0xf3,0x220);
    FUN_0000e528(5,W[0x0E50],0x1b0,0,0x220);
  }
  FUN_0000e5fc();
  if ((W16(0xE10) != 0) && (W[0x0E0C] != 3)) {
    FUN_0000e832();
  }
  FUN_0000e8b8();
  if ((W[0x0CBC] == 3) && (W16(0xE10) == 0)) {
    FUN_0000ebf6();
    FUN_0000eb3a();
    FUN_0000e9f2();
  }
  FUN_0000ed9a();
  FUN_0000ee8e();
  return;
}


/* ---- FUN_0000e37e ---- */

int FUN_0000e37e(undefined4 param_1,int param_2,int param_3,undefined4 param_4,undefined4 param_5)

{
  int iVar1;
  int bVar2;
  short sVar4;
  uint32_t uVar3;
  int iVar5;
  int local_8;
  
  sVar4 = ((uint16_t)param_2 & 7) * -0x1000;
  param_2 = param_2 >> 3;
  dsp_cmd_place_object_rotated_abs(param_1,0x27d,param_3,param_4,param_5,0,0,0,0);
  dsp_cmd_place_object_rotated_abs(param_1,0x273,param_3,param_4,param_5,0,0,0,0);
  bVar2 = true;
  local_8 = 0;
  do {
    param_3 = param_3 + -0x28;
    uVar3 = param_2 % 10;
    if (param_2 == 0) {
      dsp_cmd_place_object_rotated_abs(param_1,0x271,param_3,param_4,param_5,0,0,0,0);
      if (bVar2) {
        dsp_cmd_place_object_rotated_abs(param_1,0x270,param_3,param_4,param_5,(int)sVar4,0,0,0);
        iVar5 = 0x274;
      }
      else {
        iVar5 = 0x270;
      }
    }
    else {
      dsp_cmd_place_object_rotated_abs
                (param_1,(uVar3 & 0xffff) + 0x27d,param_3,param_4,param_5,0,0,0,0);
      if (bVar2) {
        dsp_cmd_place_object_rotated_abs
                  (param_1,(uVar3 & 0xffff) + 0x273,param_3,param_4,param_5,(int)sVar4,0,0,0);
        iVar5 = ((uVar3 & 0xffff) + 1) % 10 + 0x273;
      }
      else {
        iVar5 = (uVar3 & 0xffff) + 0x273;
      }
    }
    dsp_cmd_place_object_rotated_abs(param_1,iVar5,param_3,param_4,param_5,0,0,0,0);
    param_2 = param_2 / 10;
    bVar2 = (bool)((short)uVar3 == 9 & bVar2);
    iVar5 = local_8 + 1;
    iVar1 = local_8 + -3;
    local_8 = iVar5;
  } while (iVar5 < 4);
  return iVar1;
}


/* ---- FUN_0000e528 ---- */

void FUN_0000e528(undefined4 param_1,uint32_t param_2,int param_3,int param_4,int param_5)

{
  undefined4 uVar1;
  
  if ((int)(param_2 & 7) >> 2 == 0) {
    uVar1 = 0xfffffff8;
  }
  else {
    uVar1 = 0xfffffff9;
  }
  sprite_draw_2d(param_1,uVar1,param_3,param_4,param_5 + 0x927d,0x20,0x20,0,1);
  if (W[0x15ED6] == 0) {
    if ((param_2 & 7) != 0) {
      W[0x15ED6] = 1;
    }
  }
  else {
    W[0x15ED6] = W[0x15ED6] + 1 & 0xf;
  }
  { extern int g_lampdbg; if (g_lampdbg) printf("[LAMP] f%d E50=%ld lampA=%d ctr=%ld tileB=%d score=%ld\n",
      (int)g_sys.frame_count, (long)param_2, ((int)(param_2 & 7) >> 2) ? -9 : -8,
      (long)W[0x15ED6], -(int)vrd16s(0x354B4 + (W[0x15ED6] & 0xf) * 2), (long)W[0x0E4C]); }
  sprite_draw_2d(param_1,-(int)vrd16s(0x354B4 + W[0x15ED6] * 2),param_3 + -0x3e,
                 param_4 + 0x10,param_5 + 0x927b,0x20,0x20,0,1);
  return;
}


/* ---- FUN_0000eaaa ---- */

void FUN_0000eaaa(int param_1)

{
  int iVar1;
  int iVar2;
  uint32_t uVar3;
  undefined2 *puVar4;
  undefined2 *puVar5;
  
  /* The message->block table at 0x354D4 is BE32 on a 4-byte stride:
   * ROM 0x00EABA is `move.l $354d4(d0.l*4),d2` (extension word 0x0DB0,
   * BS=1 IS=0 scale=4).  `(&R[0x354D4])[i]` read ONE BYTE at stride 1
   * -- the S3 class -- which returns 0 for every index but 3.  See the
   * sibling reads in FUN_0000e9f2. */
  iVar1 = vrd32s(0x354D4 + W[0x15EE4] * 4);
  /* BOTH pointers were at HALF their byte offset -- register row 116's
   * defect, here rather than in text_draw_number.  g_sys.cgram and R are
   * uint8_t[], so this arithmetic is in BYTES, but the M68K scales both:
   *
   *   0x00EAC6  lsl.l #$2,d0 ; lea (a0,d0.l*2),a3   -> 0x889000 + n*8
   *   0x00EAEA  add.l d0,d0  ; adda.l d0,a1         -> 0x238504 + 2*(...)
   *
   * A cgram tile is 0x80 bytes = 16 rows of 8, and param_1 is the row
   * this frame reveals.  At stride 4 the reveal wrote rows 0..7 twice
   * over and never touched 8..15, so the bottom half of every glyph in
   * the message stayed blank and the top half was doubled. */
  puVar5 = (undefined2 *)(&g_sys.cgram[0x9000] + param_1 * 8);
  puVar4 = (undefined2 *)(&R[0x238504]
           + ((int32_t)vrd32(0x1C4F40 + (iVar1 * 0x10)) * 0x40 + param_1 * 4) * 2);
  for (uVar3 = 0; iVar2 = iVar1 * 0x10,
      uVar3 < (uint32_t)((int32_t)vrd32(0x1C4F48 + iVar2) * (int32_t)vrd32(0x1C4F44 + iVar2));
      uVar3 = uVar3 + 1) {
    *puVar5 = *puVar4;
    puVar5[1] = puVar4[1];
    puVar5[2] = puVar4[2];
    puVar5[3] = puVar4[3];
    puVar5 = puVar5 + 0x40;
    puVar4 = puVar4 + 0x40;
  }
  return;
}


/* ---- FUN_0000eb28 ---- */

void FUN_0000eb28(int param_1)

{
  if (W[0x15EE4] < 0) {
    W[0x15EE4] = param_1;
  }
  return;
}


/* ---- FUN_0000efee ---- */

void FUN_0000efee(void)

{
  int iVar1;
  int iVar2;
  undefined4 uVar3;
  
  /* 0x5000 is an IMMEDIATE. sprite_draw_2d's 8th parameter is int16_t, so a
   * 64-bit host pointer truncated into it is garbage -- the same class as the
   * 0x238E rig bug. These two feed the sprite layer (rows 26/35). */
  sprite_draw_2d(6,W[0x0E0C] * 2 + 0x1e1,0x40,0x90,0,0x1c,0x1c,0x5000,0);
  sprite_draw_2d(6,W[0x0E0C] * 2 + 0x1e2,0x40,0x170,0,0x1c,0x1c,0x5000,0);
  FUN_0000f13e(W[0x0D00],W[0x0D08],0x38d);
  if ((W16(0xE68) < 10) && (W16(0xE10) == 0)) {
    iVar2 = W[0x16008] + W[0x15FE4] * 0x20;
    for (iVar1 = 0; iVar1 < W[0x15FF0]; iVar1 = iVar1 + 1) {
      if (W_A16(0x478C, iVar1) == 0) {
        if ((*(int *)(iVar2 + 0x10 + iVar1 * 0x20) < 1) && ((W[0x0C98] & 0x10) != 0)) {
          uVar3 = 0x391;
        }
        else {
          uVar3 = 0x390;
        }
        FUN_0000f13e(*(undefined4 *)(iVar2 + 4 + iVar1 * 0x20),
                     *(undefined4 *)(iVar2 + 0xc + iVar1 * 0x20),uVar3);
      }
    }
    for (iVar2 = 0; iVar2 < W[0x15FEC]; iVar2 = iVar2 + 1) {
      if ((&g_obj_active_flag)[iVar2 * 0x3c] == 0) {
        if (((int)W[0x4924 + (iVar2 * 0x3c)] < 1) && ((W[0x0C98] & 0x10) != 0)) {
          uVar3 = 0x391;
        }
        else {
          uVar3 = 0x390;
        }
        FUN_0000f13e((&g_obj_pos_x)[iVar2 * 0x3c],(&g_obj_pos_z)[iVar2 * 0x3c],uVar3);
      }
    }
  }
  return;
}


/* ==== THE SOUND CLUSTER, ROM 0x00F584..0x00FFB0 -- converted from the M68K ====
 *
 * These are the sound system's small helpers (register row 141 named the
 * cluster; the Ghidra `scene_*` names are wrong). Every one of them wrote the
 * MCU mailbox at 0xA04000 as BYTES at a BYTE index where the ROM does
 * `move.w ... (a0, d0.w*2)` -- 16-bit words, big-endian -- and most of them
 * had lost their stack arguments to the decompiler. The helpers below are the
 * mailbox as the 68K addresses it: command n at 0xA04000 + n*2, parameter n at
 * 0xA04100 + n*2. Indices are signed words; a negative one is dropped rather
 * than wrapped (we do not have the 16-bit bus wrap the hardware has). */
static inline void sm_cmd_w(int n, unsigned v) {
    int off = n * 2;
    if (off >= 0 && off + 1 < (int)COMMSRAM_SIZE) comms_w16(g_sys.commsram, (unsigned)off, v & 0xFFFFu);
}
static inline unsigned sm_cmd_r(int n) {
    int off = n * 2;
    return (off >= 0 && off + 1 < (int)COMMSRAM_SIZE) ? comms_r16(g_sys.commsram, (unsigned)off) : 0u;
}
static inline void sm_par_w(int n, unsigned v) {
    int off = 0x100 + n * 2;
    if (off >= 0 && off + 1 < (int)COMMSRAM_SIZE) comms_w16(g_sys.commsram, (unsigned)off, v & 0xFFFFu);
}
static inline unsigned sm_par_r(int n) {
    int off = 0x100 + n * 2;
    return (off >= 0 && off + 1 < (int)COMMSRAM_SIZE) ? comms_r16(g_sys.commsram, (unsigned)off) : 0u;
}
/* The sound table at ROM 0x355A4, 12-byte rows of BE16:
 *   +0 command slot  +2 command word  +4 parameter slot  +6 priority
 *   +8 second parameter slot  +10 its value. */
static inline int sm_tbl(int id, int field) { return vrd16s(0x355A4 + id * 12 + field); }


/* ---- FUN_0000f584 ---- */

/* ROM 0x00F584: stop sound `id` UNLESS the deferred-sound timer is running.
 *     tst.w $e15f56.l ; bpl rts ; move.w $4(a7),-(a7) ; jsr $f7ec
 * One 16-bit argument. Ghidra dropped it, and the body called sound_stop(0)
 * -- slot 0 is the MUSIC, so every "stop the results jingle" (0x43), "stop
 * the ending track" (0x45) and so on stopped the background music instead. */
void FUN_0000f584(int param_1)

{
  int id = (int)(int16_t)param_1;
  snd_trig("sound_stop_unless_held", id, -1);
  if (W16(0x15F56) < 0) {
    sound_stop(id);
  }
  return;
}


/* ---- FUN_0000f66a ---- */

/* ROM 0x00F66A: set a sound's SECOND parameter to a value -- its volume, in
 * every use -- without playing it:
 *     tst.w $8(a1) ; bmi rts ; move.w $8(a1),d0 ; move.w $6(a7),(a0,d0.w*2)
 * with a0 = 0xA04100 and a1 the table row. Two 16-bit arguments (id, value).
 * The C had one, and stored the ID into a byte-indexed mailbox. */
void FUN_0000f66a(int param_1, int param_2)

{
  int id = (int)(int16_t)param_1;
  unsigned v = (unsigned)(uint16_t)param_2;
  snd_trig("sound_param2_set", id, (int)v);
  if (id < 0 || id > 0x58) return;
  if (sm_tbl(id, 8) >= 0) sm_par_w(sm_tbl(id, 8), v);
  return;
}


/* ---- FUN_0000fa72 ---- */

/* ROM 0x00FA72: stop the voice named by the fixed row at 0x357D8 if its
 * command word reads (row command | 0x8000) -- i.e. it is still latched.
 *     d1 = row+0 ; d0 = cmd[d1] (zero-extended) ; d1 = ext(row+2) | 0x8000
 *     cmp.l d1,d0 ; bne rts ; andi.w #$7fff,cmd[row+0]                     */
void FUN_0000fa72(void)

{
  int slot = vrd16s(0x357D8);
  uint32_t want = (uint32_t)(int32_t)vrd16s(0x357DA);
  want = (want & 0xFFFF0000u) | ((want | 0x8000u) & 0xFFFFu);
  if ((uint32_t)sm_cmd_r(slot) == want) {
    sm_cmd_w(slot, sm_cmd_r(slot) & 0x7FFFu);
  }
  return;
}


/* ---- sound_play_course_theme ---- */

/* ROM 0x00FBF2 -- deferred play of the per-course id from the table at 0x359DC.
 * (Ghidra called this `FUN_0000fbf2`; it is the sound system, not a scene.) */
void sound_play_course_theme(void)

{
  sound_play_or_defer(vrd16s(0x359DC + W[0x0E0C] * 2));   /* ROM 0x00FBF8 */
  return;
}


/* ---- sound_play_event ---- */

/* ROM 0x00FC62 -- play the id the event table at 0x359F6 gives for an event index.
 * (Ghidra called this `FUN_0000fc62`; it is the sound system, not a scene.)
 *
 * One LONG argument n (`movea.l $4(a7),a1`); the table is BE16 at 0x359F6
 * indexed `(a0, a1.l*2)`. n <= 3 plays the entry through sound_play; n > 3
 * (`subq.l #3,d0 ; bgt`) plays sound 0x1E with the entry as its PARAMETER.
 * A negative entry plays nothing. The C read the table host-native as a
 * pointer plus an offset and had lost the n <= 3 id outright. */
int sound_play_event(int param_1)

{
  int v = vrd16s(0x359F6 + param_1 * 2);
  if (v < 0) return 0;
  if (param_1 - 3 > 0) {
    sound_play_p((int32_t)((0x1Eu << 16) | (uint16_t)v));
  }
  else {
    sound_play(v);
  }
  return 0;
}


/* ---- FUN_0000fcc6 ---- */

/* ROM 0x00FCC6: fade the per-course sound set UP -- run while the countdown
 * voice sequence is active (0xE15F34 != 0). The level word 0xE15F5A counts up
 * to 0xFF, and every parameter slot in the course's list at 0x35A04 +
 * course*16 (BE16, negative-terminated) is raised by one per frame, but never
 * below the level and never above 0xFF:
 *     d2 = par[n] + 1 ; d0 = (d2 > 0xFF) ? 0xFF : max(d2, level)          */
void FUN_0000fcc6(void)

{
  uint32_t lst = 0x35A04 + (uint32_t)W[0x0E0C] * 0x10;
  int lvl;
  W16_SET(0x15F5A, W16(0x15F5A) + 1);             /* addq.w #1,(a3) */
  if (W16(0x15F5A) > 0xff) W16_SET(0x15F5A, 0xff);
  lvl = W16(0x15F5A);
  { int k; for (k = 0; k < 64; k++) {
      int n = vrd16s(lst + k * 2);
      if (n < 0) break;
      int d2 = (int16_t)(sm_par_r(n) + 1);
      int d0 = (d2 > 0xff) ? 0xff : (d2 < lvl ? lvl : d2);
      sm_par_w(n, (unsigned)d0);
  } }
  return;
}


/* ---- FUN_0000fd30 ---- */

/* ROM 0x00FD30: every parameter slot in the course's list at 0x35A04 +
 * course*16 to 0xFF (full attenuation). 16-bit words, not bytes. */
void FUN_0000fd30(void)

{
  uint32_t lst = 0x35A04 + (uint32_t)W[0x0E0C] * 0x10;
  { int k; for (k = 0; k < 64; k++) {
      int n = vrd16s(lst + k * 2);
      if (n < 0) break;
      sm_par_w(n, 0xff);
  } }
  return;
}


/* ---- FUN_000256be ---- */

/* ROM 0x0256BE -- THE RANK REVEAL on the NOVICE results page: steps the
 * shown rank W[0x169C4] up one row at a time toward the rank the score
 * earns (the first of the five thresholds at 0x15C478 + (i + course*8)*4
 * the score does not reach), sliding the pointer 0xE169BA toward that row's
 * y from the table at 0x37508, and returns 1 once it has arrived.
 *
 * Ported from the ROM. The C read the BIG-ENDIAN word table at 0x37508
 * (`move.w $37508(d0.l*2),d2`, ext 0x0BB0: scale 2) as single BYTES at
 * stride 1, and kept the three 16-bit fields 0xE169BA (pointer y, the LOW
 * half of slot 0x169B8), 0xE169BC (speed) and 0xE169BE (landed flag, the
 * LOW half of slot 0x169BC) as whole slots -- the two 2-mod-4 ones rebuilt
 * from their neighbours' bytes every frame. */
undefined4 FUN_000256be(void)

{
  /* ROM 0x0256BE, the results-screen RANK LADDER. Rewritten from the listing:
   *  - the ladder table at 0x37508 is SIXTEEN-BIT (`move.w $37508(d0.l*2)`:
   *    -2384 -1872 -1360 -864 -352 176) and was read one BYTE at a time;
   *  - 0xE169BA (the pointer's Y, 2-mod-4), 0xE169BC (its speed) and 0xE169BE
   *    (the "landed" flag, 2-mod-4) are all `.w` fields -- 0x169BA shares its
   *    slot with 0x169B8, and 0x169BC with 0x169BE -- and were written as whole
   *    slots, each clobbering its neighbour.
   * 0xE169C0 (the settle countdown) and 0xE169C4 (the rank) are longs. */
  int16_t target;
  int d1 = 0;
  do {
    if (W[0x169AC] < (int32_t)vrd32(0x15C478 + ((d1 + W[0x0E0C] * 8) * 4))) break;
    d1 = d1 + 1;
  } while (d1 < 5);
  target = vrd16s(0x37508 + (int)W[0x169C4] * 2);
  if ((W[0x2BA6] & 1) != 0) {                       /* button: jump to the end */
    W[0x169C4] = (int16_t)d1;
    W16_SET(0x169BA, vrd16s(0x37508 + (int16_t)d1 * 2));
  }
  if (-1 < (int32_t)W[0x169C0]) {                   /* 0x02572A: settling */
    W16_SET(0x169BA, (int16_t)(W16(0x169BA) - 0x20));
    if (target > W16(0x169BA)) {
      W16_SET(0x169BA, target);
      W16_SET(0x169BE, 1);
    }
    W[0x169C0] = (int32_t)W[0x169C0] - 1;
    if ((int32_t)W[0x169C0] < 0) {
      if ((int32_t)W[0x169C4] == (int16_t)d1) return 1;
      W[0x169C4] = (int32_t)W[0x169C4] + 1;
      W16_SET(0x169BE, 0);
    }
    return 0;
  }
  if (target == W16(0x169BA) && W16(0x169BE) != 0) { /* 0x025758: landed */
    W[0x169C0] = 0x10;
    return 0;
  }
  if ((int16_t)(target + 0x80) > W16(0x169BA)) {    /* 0x025768: rising */
    W16_SET(0x169BA, (int16_t)(W16(0x169BA) + W16(0x169BC)));
    W16_SET(0x169BC, (int16_t)(W16(0x169BC) + 3));
  }
  if ((int16_t)(target + 0x80) <= W16(0x169BA)) {   /* 0x02577A: overshoot, bounce */
    sound_play(0x16);
    W16_SET(0x169BA, (int16_t)(target + 0x80));
    W[0x169C0] = 0x10;
    W16_SET(0x169BC, 0x10);
  }
  return 0;
}


/* ---- FUN_000268ce ---- */

/* The articulated-animation node chains live in work RAM (0xE0AB04 for the
 * rider/bike, 0xE0B704/0xE0B784 for the cutscene casts), and in the _W[]
 * model a node is addressed by its BYTE OFFSET: node field +N is W[nb + N].
 * Callers of the functions below hand the node over in every spelling the
 * transpile produced -- a host pointer &W[nb], the bare offset 0xab04, and
 * the 68K address 0xE0AB04 -- so this turns any of them into the offset,
 * or -1 if it is none of them (a truncated host pointer, a lost argument). */
intptr_t anim_w_off(const void *p)
{
  uintptr_t v = (uintptr_t)p;
  uintptr_t lo = (uintptr_t)&_W[0], hi = (uintptr_t)&_W[WORK_RAM_SIZE];
  if (v >= lo && v < hi) return (intptr_t)((v - lo) / sizeof(intptr_t));
  if (v >= 0xE00000u && v < 0xE00000u + WORK_RAM_SIZE) return (intptr_t)(v - 0xE00000u);
  if (v > 0 && v < WORK_RAM_SIZE) return (intptr_t)v;
  return -1;
}

/* ROM 0x0268CE -- load a NULL-terminated list of ROM node descriptors into a
 * chain of 0x80-byte animation nodes starting at `a1`, and return the offset
 * of the -1 terminator node after the last one (so lists can be chained).
 *
 * Node layout, from this routine and its consumer 0x0265EC:
 *   +0x00 model (long; 0x7FFF = the CAMERA node)   +0x04 long, HIGH word =
 *   parent link, LOW word = the animation length    +0x08 long, HIGH word =
 *   DOF flags (bit per channel via the mask table 0x37664, 0x40 = the model
 *   id is a sequence)   +0x0C..0x20 six channel KEYFRAME-STREAM pointers
 *   (ROM addresses)   +0x24..0x38 six channel values   +0x3C..0x50 six
 *   spline accumulators   +0x54..0x68 six segment-start values
 *   +0x6C..0x76 six 16-bit segment start frames   +0x78 transform slot id
 *   +0x7C model-sequence cursor (ROM address, 0 if none).
 *
 * The transpile dereferenced every ROM address here as a host pointer, so
 * any real list crashed and the NULL lists Ghidra left at the call sites
 * were the only ones that "worked". d5 -- the slot-id counter -- is a callee-
 * saved register the ROM inherits from its caller; a root node (+0x04 == 0)
 * resets it to 4 before any child uses it, which is what every list here
 * starts with. */
intptr_t anim_load_list(intptr_t a1, uint32_t d1)
{
  static int16_t d5 = 4;
  int guard = 0;
  if (a1 < 0 || a1 + 0x80 > WORK_RAM_SIZE) return a1;
  W[0x169F4] = (int32_t)d1;
  while (d1 != 0 && d1 + 4 <= ROM_SIZE && vrd32(d1) != 0 && guard++ < 64 &&
         a1 + 0x100 <= WORK_RAM_SIZE) {
    uint32_t src = vrd32(d1), d3;
    int32_t flags;
    int k;
    if (src + 0x24 > ROM_SIZE) break;
    W[a1 + 0x00] = (int32_t)vrd32(src);
    W[a1 + 0x04] = (int32_t)vrd32(src + 4);
    W[a1 + 0x08] = (int32_t)vrd32(src + 8);
    if (W_HI16(a1 + 4) == 0)      { d5 = 4;  W[a1 + 0x78] = d5; }   /* 0x0268F0 */
    else if (W_HI16(a1 + 4) < 0)  { d5++;    W[a1 + 0x78] = d5; }
    flags = W_HI16(a1 + 8);                                         /* movea.w $8(a1) */
    if (flags & 0x40) {                                             /* 0x026924 */
      W[a1 + 0x7C] = W[a1 + 0x00];
      W[a1 + 0x00] = vrd16s((uint32_t)W[a1 + 0x7C]);
    } else {
      W[a1 + 0x7C] = 0;
    }
    d3 = src + 0x0C;
    for (k = 0; k < 6; k++) {                                       /* 0x026940 */
      uint32_t a0;
      int32_t v;
      W[a1 + 0x3C + k * 4] = 0;
      W_A16_SET(a1 + 0x6C, k, 1);
      if ((flags & (int32_t)vrd32(0x37664 + k * 4)) == 0) {
        a0 = d3;                              /* the value itself */
      } else {
        a0 = vrd32(d3);                       /* a keyframe stream */
        W[a1 + 0x0C + k * 4] = (int32_t)(a0 + 4);
      }
      d3 += 4;
      v = (int32_t)vrd32(a0);
      W[a1 + 0x54 + k * 4] = v;
      W[a1 + 0x24 + k * 4] = v;
    }
    if ((int32_t)W[a1] == 0x7fff) {                                 /* 0x026976 */
      W[0x0CDC] = (int32_t)W[a1 + 0x24];
      W[0x0CE0] = (int32_t)W[a1 + 0x28];
      W[0x0CE4] = (int32_t)W[a1 + 0x2C];
      W[0x0CE8] = -(int32_t)W[a1 + 0x30];
      W[0x0CEC] = -(int32_t)W[a1 + 0x34];
      W[0x0CF0] = -(int32_t)W[a1 + 0x38];
    }
    a1 += 0x80;
    d1 += 4;
  }
  W[a1] = -1;                                                       /* 0x0269AC */
  W[0xEB06] = 0;
  W[0xEB04] = 0;
  return a1;
}

/* Both call conventions of the two ports merged here (register row 188): the
 * node as &W[..], a bare offset or a 68K address, the list as a ROM address
 * or a host pointer into the ROM image; returns the 68K ADDRESS of the -1
 * terminator node, which story-merge's ending chains straight back in. */
uint32_t FUN_000268ce(intptr_t node_in, intptr_t list_in)
{
  return FUN_000268ce_any(node_in, list_in);
}
uint32_t FUN_000268ce_any(intptr_t node_p, intptr_t list_p)
{
  intptr_t node = anim_w_off((const void *)node_p);
  uintptr_t lv = (uintptr_t)list_p, rlo = (uintptr_t)&g_sys.rom[0];
  uint32_t list;
  if (node < 0) return 0;
  if (lv >= rlo && lv < rlo + ROM_SIZE) list = (uint32_t)(lv - rlo);
  else list = (uint32_t)lv;
  if (list == 0 || list >= ROM_SIZE) {    /* a lost argument: terminate */
    W[node] = -1;
    W[0xEB06] = 0;
    W[0xEB04] = 0;
    return 0xE00000u + (uint32_t)node;
  }
  return 0xE00000u + (uint32_t)anim_load_list(node, list);
}


/* The ranking tables keep every BYTE in its own _W[] slot (pinned in
 * game_init); these read/write one such byte (story-merge, register row 188). */
static inline int  hs_b(uint32_t off)            { return (int8_t)W[off]; }
static inline void hs_b_set(uint32_t off, int v) { W[off] = (int8_t)v; }

/* ---- FUN_00031d40 ---- */

short FUN_00031d40(int param_1,uint32_t param_2)
{
  int c = W16(0xE10) ? 3 : param_1;
  int n = (c == 3) ? 10 : 20;
  int32_t s = (int32_t)(param_2 & 0xffffff);
  if (W16(0xE12) != 0 && s > (int32_t)((uint32_t)W[0x40A0] & 0xffffff))
    return 1;
  int e = hs_b(0x15E70 + c);
  int i;
  for (i = 0; i < n; i++) {
    uint32_t ent = 0x15BF0 + c * 0xA0 + e * 8;
    if (s > (int32_t)((uint32_t)W[ent] & 0xffffff))
      return (short)(i + 1);
    e = hs_b(ent + 7);
  }
  W[0x3EA0] = (int32_t)(int16_t)i;
  return 0;
}

/* ---- score_check_quota ---- */

undefined4 score_check_quota(void)

{
  undefined4 uVar1;
  
  if (W[0x169AC] < W[0x0E60]) {
    uVar1 = 0x8000;
  }
  else {
    uVar1 = 0;
  }
  return uVar1;
}


/* ---- FUN_0000e832 ---- */

void FUN_0000e832(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  
  iVar2 = 0x19d;
  iVar1 = 0;
  iVar3 = W[0x0E60];
  do {
    dsp_cmd_place_object_rotated_abs(3,iVar3 % 10 + 0x347,iVar2,0xf0,0x3bc,0,0,0,0);
    iVar3 = iVar3 / 10;
    iVar2 = iVar2 + -0x29;
    iVar1 = iVar1 + 1;
  } while (iVar1 < 5);
  dsp_cmd_place_object_rotated_abs(1,0x288,0xf2,0xa0,0x220,0x8000,0,0,0);
  return;
}

/* ---- FUN_0000e8b6 ---- */

void FUN_0000e8b6(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x00E8B6 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. Bare RTS in the ROM.
   */
  return;
}

/* ---- FUN_0000e8b8 ---- */

void FUN_0000e8b8(void)

{
  undefined4 uVar1;
  
  if (((0 < W16(0xE68)) && (W16(0xE68) < 10)) && ((W[0x0C98] & 0x10) != 0)) {
    sprite_draw_2d(6,0xfffffea1,0x1c0,0x88,0,8,8,0,1);
    sprite_draw_2d(6,0xfffffea0,0x200,0x88,0,8,8,0,1);
    if (W16(0xE68) == 1) {
      uVar1 = 0xfffffe51;
    }
    else {
      uVar1 = 0xfffffe52;
    }
    sprite_draw_2d(6,uVar1,0x1c8,0xa0,1,0x20,0x20,0,1);
    sprite_draw_2d(6,-0x1b0 - W16(0xE68),0x1c0,0xa0,0,0x20,0x20,0,1);
  }
  return;
}

/* ---- FUN_0000e9a4 ---- */

void FUN_0000e9a4(void)

{
  int iVar1;
  
  iVar1 = 0;
  do {
    /* ROM 0x00E9A6 is a single `move.l $354f4(d1.l*4),$e15eec(d1.l*4)`:
     * BOTH sides are 4-byte strides and the source is BIG-ENDIAN.  The
     * _W[] destination was indexed at stride 1 and the ROM read with
     * rom_nr32 (host-native), so the per-message repeat counts came out
     * as [4294905600, 16777216, ...] instead of [1048575, 1, 65535, 3,
     * 1, 65535, 65535, 65535] -- the 1 and the 3 are messages meant to
     * stop after that many showings. */
    W[0x15EEC + (iVar1) * 4] = vrd32s(0x354F4 + (iVar1) * 4);
    iVar1 = iVar1 + 1;
  } while (iVar1 < 8);
  W[0x15EE4] = (int32_t)0xffffffff;
  W[0x15EE8] = 0;
  W[0x15F10] = 0;
  cgram_load_tile_block(0xa5,0x170);
  /* ROM 0x00E9DE pushes the palette as a BYTE: `move.b #$7,-(a7)` before
   * `pea $a5.w`.  The 0 here loaded this banner's colour ramp into
   * palette slot 0 while the banner itself draws with palette 7 (see the
   * text_draw_rect_blink call in FUN_0000e9f2), so it was painted with
   * whatever happened to be in slot 7.  Register row 118's class, at a
   * call site rather than inside the callee. */
  cz_load_color_ramp(0xa5, 7);
  return;
}

/* ---- FUN_0000ebf6 ---- */

void FUN_0000ebf6(void)

{
  uint16_t uVar1;
  
  if ((W[0x0E0C] == 1) &&
     ((((W[0x0D24] == 0x5b || (W[0x0D24] == 0x5c)) || (W[0x0D24] == 99)) ||
      (W[0x0D24] == 100)))) {
    W[0x15F16] = FUN_0000f26a();
  }
  else {
    W[0x15F16] = 0;
  }
  W[0x15F0E] = (uint16_t)(W[0x0E44] < 900);
  if (899 < W[0x0E44]) {
    W[0x15F10] = 0;
  }
  W[0x15F0C] = 0;
  uVar1 = (uint16_t)((int16_t)W16(0x15ED8) + (int32_t)W[0x0D10]);   /* 0x00EC6C movea.w */
  if ((uVar1 < 0xb000) && (0x5000 < uVar1)) {
    if (W16(0x15EDA) != 0) {                                           /* 0x00EC8C tst.w */
      W[0x15F12] = 1;
    }
  }
  else {
    W[0x15F12] = 0;
  }
  if ((W[0x0E0C] == 1) && (W[0x0D24] == 0x11)) {
    W[0x15F14] = 1;
  }
  else {
    W[0x15F14] = 0;
  }
  return;
}

/* ---- FUN_0000ecca ---- */

void FUN_0000ecca(void)

{
  undefined2 uVar1;
  
  if (W16(0xE10) == 0) {
    uVar1 = 0x93;
  }
  else {
    uVar1 = 0x94;
  }
  /* ROM 0x00ECCA: every cz_load_color_ramp here pushes its palette as a BYTE
     (`move.b #$N,-(a7)`), and the decompiler lost all six (register row 118). */
  cgram_load_tile_block(uVar1,0x1c0);
  cz_load_color_ramp(uVar1, 1);
  cgram_load_tile_block(0xf5,0x260);
  cz_load_color_ramp(0xf5, 0xd);
  if (W16(0xE10) != 0) {
    cgram_load_tile_block(W16(0xE66) + 0xd9,0x1d0);
  }
  if (W16(0xE10) == 0) {
    cgram_load_tile_block(0x10a,0x200);
    cz_load_color_ramp(0x10a, 0xc);
    cgram_load_tile_block(0x10f,0x210);
    cgram_load_tile_block(0x110, 0x218);
    cz_load_color_ramp(0x10f, 5);
    cz_load_color_ramp(0x110, 9);
    cgram_load_tile_block(0x111,0x220);
    cgram_load_tile_block(0x112,0x240);
    cz_load_color_ramp(0x111, 0xb);
  }
  return;
}

/* ---- FUN_0000ee4e ---- */

void FUN_0000ee4e(void)

{
  W[0x15F1C] = 0xff;
  W[0x15F18] = (int32_t)0xffffffff;
  W[0x15F20] = (uint32_t)(W16(0xE10) != 0);
  if (W16(0xE10) == 0) {
    W[0x15F24] = 0x4b0;
  }
  else {
    W[0x15F24] = (int32_t)0xffffffff;
  }
  return;
}

/* ---- FUN_0000f13e ---- */

void FUN_0000f13e(int param_1,int param_2,int param_3)

{
  int iVar1;
  int bVar2;
  int iVar3;
  int iVar4;
  int iVar5;
  
  iVar1 = W[0x0E0C] * 0x20;
  bVar2 = false;
  iVar4 = (int32_t)vrd32(0x35530 + iVar1) * (param_1 >> 8);
  if (iVar4 < 0) {
    iVar4 = iVar4 + 0xff;
  }
  iVar4 = (int32_t)vrd32(0x35524 + iVar1) + (iVar4 >> 8);
  iVar3 = (int32_t)vrd32(0x35530 + iVar1) * (param_2 >> 8);
  if (iVar3 < 0) {
    iVar3 = iVar3 + 0xff;
  }
  iVar3 = (int32_t)vrd32(0x35528 + iVar1) + (iVar3 >> 8);
  if (param_3 == 0x38d) {
    if ((W[0x0C98] & 8) == 0) {
      param_3 = 3;
    }
    else {
      param_3 = 2;
    }
    iVar3 = ((short)vrd16s(0x20B006 + (W[0x0D10] >> 1 & 0x7ffe) * 2) * 0x180 >> 0xf) + iVar3;
    iVar4 = iVar4 - ((short)vrd16s(0x20B004 + (W[0x0D10] >> 1 & 0x7ffe) * 2) * 0x180 >> 0xf);
    iVar5 = W[0x0D10] + 0x8000;
  }
  else {
    iVar5 = 0;
  }
  if (((int32_t)vrd32(0x35534 + iVar1) < iVar4) || (iVar4 < (int32_t)vrd32(0x35538 + iVar1))) {
    bVar2 = true;
  }
  if (((int32_t)vrd32(0x3553C + iVar1) < iVar3) || (iVar3 < (int32_t)vrd32(0x35540 + iVar1))) {
    bVar2 = true;
  }
  if (!bVar2) {
    dsp_cmd_place_object_rotated_abs
              (2,param_3,iVar4,iVar3,((int32_t*)&g_sys.rom[0x3552C])[W[0x0E0C] * 8],0,0,iVar5,0);
  }
  return;
}

/* ---- FUN_0000fb38 ---- */

undefined4 FUN_0000fb38(int param_1,int param_2,int param_3)

{
  undefined4 uVar1;
  snd_trig_contact(param_1, param_2, param_3);
  if (6 < param_1 - 2U && param_1 + -9 != 0) {
    sound_play(SND_ID_UNRECOVERED);   /* id lost by the decompiler */
    if (param_3 < 0x801) {
      param_2 = 0x3c;
    }
    if (W[0x15F3E] < param_2) {
      W[0x15F3E] = (short)param_2;
    }
    return 1;
  }
                    
                    
  /* unknown dispatch */; uVar1 = 0;
  return uVar1;
}

/* ---- sound_play_cooldown ---- */

/* ROM 0x00FC08 -- play sound 0x1A with the caller parameter, behind a 45-frame cooldown.
 * (Ghidra called this `FUN_0000fc08`; it is the sound system, not a scene.) */
void sound_play_cooldown(int param_1)

{
  /* ROM 0x00FC08. Plays sound id 0x1A with the caller's PARAMETER, behind a
   * 45-frame cooldown (0xE15F48). Ghidra dropped the parameter and every
   * call passed 0 -- and parameter 0 resolves to the sample descriptor at
   * ROM 0x210786, whose start/end/loop are 0x0000/0x003F/0x001B: a
   * 36-sample loop at the very start of the wave ROM. At this voice's pitch
   * (freq 0x301F, 16040 samples/s) that is a 446 Hz tone, which is exactly
   * the buzz measured over gameplay -- a single FFT bin carrying 24-43% of
   * the output energy where MAME's spectrum has nothing above 5%.
   *
   * SND_ID_UNRECOVERED here means the caller's own parameter was lost too;
   * play nothing rather than the buzz. */
  snd_trig("sound_play_cooldown", param_1, -1);
  if (param_1 == SND_ID_UNRECOVERED) return;
  if ((((W[0x15F48] < 1) && (W[0x0E18] < 1)) && (W[0x0CC0] != 0x13)) &&
     ((W[0x3FFC] != 0 || (W[0x0CBC] != 1)))) {
    sound_play_p(0x001A0000 | (param_1 & 0xFFFF));
    W[0x15F48] = 0x2d;
  }
  return;
}

/* ---- sound_play_pop_extra ---- */

/* ROM 0x00FC4C -- called right after a balloon pop: sound 0x52 through the cooldown wrapper, unless W[0x0D50] bit 1 is set.
 * (Ghidra called this `FUN_0000fc4c`; it is the sound system, not a scene.) */
uint32_t sound_play_pop_extra(void)

{
  uint32_t uVar1;
  
  uVar1 = W[0x0D50] & 2;
  if (uVar1 == 0) {
    sound_play_cooldown(0x52);   /* ROM 0x00FC56 `move.w #$52,-(a7)` */ uVar1 = 0;
  }
  return uVar1;
}

/* ---- FUN_0000fc9a ---- */

void FUN_0000fc9a(undefined2 param_1,undefined2 param_2)

{
  /* ROM 0x00FC9A -- two 16-bit parameter words for one sound channel in the
   * MCU mailbox: `move.w $4(a1),d0` with a1 = 0x357A8 gives the channel
   * (BE16 at 0x357AC = 12), then `move.w d1,(0xA04100, d0.w*2)` and
   * `(0xA04102, d0.w*2)`. The transpile read the index as a byte at
   * 0x357AC*4 (the (ADDR)*4 artifact, past the end of the 4 MB ROM) and
   * stored each word a byte at a time at a byte stride -- register row
   * 141's class. Called every intro-orbit frame with the wind level and
   * pitch (bonus_sequence_animate). */
  int ch = vrd16s(0x357AC);
  comms_w16(g_sys.commsram, 0x100 + ch * 2, (uint16_t)param_1);
  comms_w16(g_sys.commsram, 0x102 + ch * 2, (uint16_t)param_2);
  return;
}

/* ---- FUN_0000fd56 ---- */

void FUN_0000fd56(void)

{
  if (((W[0x4010] - W[0x15F30] == 0x16b2) && (W[0x15F34] == 0)) && (W[0x3FFA] == 1)) {
    W[0x15F34] = 1;
  }
  return;
}

/* ---- FUN_0000fd86 ---- */

int FUN_0000fd86(int param_1)

{
  /* The clears are WORDS at 0xA04100 + d1*2 -- `clr.w (a0, d1.w*2)` at
   * 0x00FD94 and siblings -- i.e. commsram byte offset 0x100 + d1*2. The old
   * body wrote bytes at 0x100 + d1, a different block entirely (register
   * row 141's stride class), so the stem mailbox was never actually
   * cleared. The fixed-address clears are `clr.w $a04226.l` etc. -- word
   * writes, and the old byte writes also landed one byte early. */
  short sVar1;
  
  sVar1 = 0x90;
  do {
    comms_w16(g_sys.commsram, 0x0100 + (unsigned)sVar1 * 2, 0);
    sVar1 = sVar1 + 1;
  } while (sVar1 < 0x93);
  if (param_1 == 1) {
    comms_w16(g_sys.commsram, 0x0226, 0);
  }
  sVar1 = 0x94;
  do {
    comms_w16(g_sys.commsram, 0x0100 + (unsigned)sVar1 * 2, 0);
    sVar1 = sVar1 + 1;
  } while (sVar1 < 0x97);
  if (param_1 == 1) {
    comms_w16(g_sys.commsram, 0x022E, 0);
  }
  sVar1 = 0x98;
  do {
    comms_w16(g_sys.commsram, 0x0100 + (unsigned)sVar1 * 2, 0);
    sVar1 = sVar1 + 1;
  } while (sVar1 < 0xd0);
  if (param_1 == 1) {
    comms_w16(g_sys.commsram, 0x02A0, 0);
    comms_w16(g_sys.commsram, 0x02A2, 0);
    comms_w16(g_sys.commsram, 0x02A4, 0);
  }
  sVar1 = 0xd3;
  do {
    comms_w16(g_sys.commsram, 0x0100 + (unsigned)sVar1 * 2, 0);
    sVar1 = sVar1 + 1;
  } while (sVar1 < 0xe0);
  if (param_1 + -1 == 0) {
    comms_w16(g_sys.commsram, 0x02C0, 0);
    comms_w16(g_sys.commsram, 0x02C2, 0);
    comms_w16(g_sys.commsram, 0x02C4, 0);
  }
  sVar1 = 0xe3;
  do {
    comms_w16(g_sys.commsram, 0x0100 + (unsigned)sVar1 * 2, 0);
    sVar1 = sVar1 + 1;
  } while (sVar1 < 0xfa);
  return param_1 + -1;
}

/* ---- FUN_0000fe42 ---- */

/* ROM 0x00FE42 -- the WIND WOODS MUSIC STEM SEQUENCER, called every frame
 * in state 3 when the course is 1 (stage_camera_path_update). The dispatch
 * Ghidra could not decode (`jmp (pc,d0)` at 0x00FEA8, offset table at
 * 0x00FEAC) switches on the CURRENT stem and walks the music through its
 * sections as the player's course cell (W[0x0CF4] -> target stem from the
 * ROM table at 0x35A44) and position change. The triggers are WORDS in the
 * MCU's stem mailbox at 0xA04220..0xA04226; 0xA0422E is the MCU's
 * "section finished" ack. With this stubbed, Wind Woods had no music at
 * all: nothing ever keyed the stems.
 *
 * The 16-bit state lives in the sound block as FIELDS, not whole slots:
 * 0xE15F50 = cooldown timer, 0xE15F52 = current stem, 0xE15F54 = the
 * "restart the theme" flag (the next frame plays id 0x3B). The old body
 * read them as whole _W[] slots -- W[0x15F52] spans ROM words
 * 0xE15F52 AND 0xE15F54, so the stem compare and the flag test were made
 * on neighbouring data (row 161's sound-block note). */

void FUN_0000fe42(void)

{
  int32_t d2;

  if (W16(0x15F54) != 0) {                          /* 0x00FE6A */
    sound_play(0x3B);
    W16_SET(0x15F54, 0);
  }
  if (W16(0x15F50) > 0) {                           /* 0x00FE7A */
    W16_SET(0x15F50, W16(0x15F50) - 1);
    return;
  }
  d2 = vrd16s(0x35A44 + (W[0x0CF4] * 2));           /* target stem at this cell */
  { extern int g_stagedbg; static int last_st = -99, last_ack = -99;
    int st = W16(0x15F52), ack = (int)comms_r16(g_sys.commsram, 0x22E);
    if (g_stagedbg && (st != last_st || ack != last_ack))
      fprintf(stderr, "[STEM] f%u state=%d target=%d cell=%d cam=(%ld,%ld) plyz=%ld mbox 220=%04X 222=%04X 224=%04X 226=%04X 22E=%04X\n",
              g_sys.frame_count, st, d2, (int)W[0x0CF4], (long)(int32_t)W[0x0CDC], (long)(int32_t)W[0x0CE4],
              (long)(int32_t)W[0x0D08], comms_r16(g_sys.commsram, 0x220), comms_r16(g_sys.commsram, 0x222),
              comms_r16(g_sys.commsram, 0x224), comms_r16(g_sys.commsram, 0x226), ack);
    last_st = st; last_ack = ack; }
  if (d2 == W16(0x15F52)) return;                   /* already there */
  switch (W16(0x15F52)) {
  case 0:                                           /* 0x00FEB6 */
    W16_SET(0x15F52, 1);
    W16_SET(0x15F50, 0x1e);
    comms_w16(g_sys.commsram, 0x220, 1);
    break;
  case 1:                                           /* 0x00FECA */
    if (d2 == 0) goto stem_theme_off;
    if (d2 == 3 || (uint32_t)W[0x0CDC] > 0x58000) {      /* 0x00FEEA: bls = unsigned */
      W16_SET(0x15F52, 2);
      W16_SET(0x15F50, 0x1e);
      comms_w16(g_sys.commsram, 0x222, 1);
    }
    break;
  case 2:                                           /* 0x00FF10 */
    if (d2 == 3 ||
        (d2 == -2 && (uint32_t)W[0x0CDC] > 0x68000 && (uint32_t)W[0x0CE4] > 0xD0000)) {
      W16_SET(0x15F52, 3);
      W16_SET(0x15F50, 0x1e);
      comms_w16(g_sys.commsram, 0x224, 1);
      break;
    }
    if (d2 == 0) goto stem_theme_off;               /* 0x00FF42 */
    if (d2 == 1) {
      W16_SET(0x15F52, 1);
      W16_SET(0x15F50, 0x1e);
      comms_w16(g_sys.commsram, 0x222, 2);
    }
    break;
  case 3:                                           /* 0x00FF5E: no timer here */
    if (d2 == 1 || d2 == -1 || W[0x0D08] < 0xD0000) {
      W16_SET(0x15F52, 4);
      comms_w16(g_sys.commsram, 0x226, 1);
    }
    break;
  case 4:                                           /* 0x00FF84: wait for the ack */
    if (comms_r16(g_sys.commsram, 0x22E) == 1) {
      W16_SET(0x15F52, 1);
      W16_SET(0x15F50, 0x78);
      sound_stop(0x3B);
      W16_SET(0x15F54, 1);
      FUN_0000fd86(0);
    }
    break;
  default:
    break;
  }
  return;

stem_theme_off:                                     /* 0x00FECE */
  W16_SET(0x15F52, 0);
  W16_SET(0x15F50, 0x1e);
  sound_stop(0x3B);
  W16_SET(0x15F54, 1);
  FUN_0000fd86(1);
}

/* ---- result_screen_render_bonus ---- */

void result_screen_render_bonus(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  uint32_t uVar5;
  undefined4 *puVar6;
  
  *(int32_t*)W[0x0CA4] = 0x8010;
  ((int32_t*)W[0x0CA4])[1] = 3;
  puVar6 = (int32_t*)W[0x0CA4] + 3;
  ((int32_t*)W[0x0CA4])[2] = 0xfffb0000;
  W[0x0CA4] = W[0x0CA4] + 4 * 4;
  *puVar6 = 0xffffffff;
  { extern intptr_t anim_w_off(const void *p);
    intptr_t nb = anim_w_off((const void *)W[0x166E4]);
    if (nb < 0) nb = 0;
    iVar1 = (int32_t)W[nb + 0x9d8] - (int32_t)W[nb + 0x9a8]; }   /* node 19 +0x58 - +0x28 */
  iVar3 = (iVar1 * 0xdb0) / 0xf00 + 0x90;
  if (0x300 < iVar3) {
    iVar3 = 0x300;
  }
  sprite_draw_2d(6,W[0x0E0C] * 2 + 0x1e1,0x40,iVar3,0,0x1c,0x1c,0,0);
  sprite_draw_2d(6,W[0x0E0C] * 2 + 0x1e2,0x40,iVar3 + 0xe0,0,0x1c,0x1c,0,0);
  iVar3 = (iVar1 * -0x85e) / 0xf00;
  if ((W16(0xE10) != 0) && (W[0x16720] < 0x3333)) {
    if (W16(0xE6A) < 4) {
      iVar2 = (int)W16(0xE6A);
    }
    else {
      iVar2 = 3;
    }
    uVar5 = (int32_t)vrd32(0x15C328 + ((W16(0xE64) * 6 + W[0x0E0C] * 0x12 + W[0x3FFA] * 2) * 4)) -
            R[0x15C4F8] * iVar2;
    iVar2 = 0x87;
    iVar4 = 0;
    do {
      dsp_cmd_place_object_rotated_abs(0,uVar5 % 10 + 0x347,iVar2,iVar3,0x1c0,0,0,0,0);
      dsp_cmd_place_object_rotated_abs(0,0x351,iVar2 + 4,iVar3 + -5,0x1c0,0,0,0,0);
      uVar5 = uVar5 / 10;
      iVar2 = iVar2 + -0x2b;
      iVar4 = iVar4 + 1;
    } while (iVar4 < 5);
  }
  *(int32_t*)W[0x0CA4] = 0x8010;
  ((int32_t*)W[0x0CA4])[1] = 3;
  puVar6 = (int32_t*)W[0x0CA4] + 3;
  ((int32_t*)W[0x0CA4])[2] = 0xffff4000;
  W[0x0CA4] = W[0x0CA4] + 4 * 4;
  *puVar6 = 0xffffffff;
  iVar1 = -iVar1;
  dsp_cmd_place_object_rotated_abs(0,0x37d,0,iVar1,0x350,0,0,0,0);
  dsp_cmd_place_object_rotated_abs(0,W16(0xE66) + 0x36e,0,iVar1,0x350,0,0,0,0);
  dsp_cmd_place_object_rotated_abs(0,0x364,0,iVar1,0x350,0,0,0,0);
  return;
}

/* ---- result_screen_render_score ---- */

void result_screen_render_score(void)

{
  int iVar1;
  uint32_t uVar2;
  int iVar3;
  int iVar4;
  undefined4 *puVar5;
  undefined4 uVar6;
  
  *(int32_t*)W[0x0CA4] = 0x8010;
  ((int32_t*)W[0x0CA4])[1] = 3;
  puVar5 = (int32_t*)W[0x0CA4] + 3;
  ((int32_t*)W[0x0CA4])[2] = 0xfffb0000;
  W[0x0CA4] = W[0x0CA4] + 4 * 4;
  *puVar5 = 0xffffffff;
  hud_draw_3d_sprite(0,(0x353 - W16(0xE10)) + W[0x0E0C] * 2,0xfffffee0,0x7c,
                     (int)(W[0x16720] * 0x12) / -0x3333 + 0x350,0,0,0,0);
  hud_draw_3d_sprite(0,0x358,0xfffffef0,0x6c,0x350,0,0,0,0);
  if ((W16(0xE10) != 0) && ((int)W[0x16720] < 0x3333)) {
    uVar2 = vrd32(0x15C328 + ((W16(0xE64) * 6 + W[0x0E0C] * 0x12 + W[0x3FFA] * 2) * 4));
    iVar3 = 0x87 - ((vrd16s(0x20B004 + (W[0x16720] & 0xfffc))) * 0x140 >> 0xf);
    if (iVar3 < -0x58) {
      iVar3 = -0x58;
    }
    iVar4 = 0x1c0 - ((vrd16s(0x20B004 + ((int)(W[0x16720] << 0xe) / 0x3333 & 0xfffcU))) * -400 >> 0xf);
    if (0x350 < iVar4) {
      iVar4 = 0x350;
    }
    iVar1 = 0;
    do {
      hud_draw_3d_sprite(0,uVar2 % 10 + 0x347,iVar3,0,iVar4,0,0,0,0);
      hud_draw_3d_sprite(0,0x351,iVar3 + 4,0xfffffffb,iVar4,0,0,0,0);
      uVar2 = uVar2 / 10;
      iVar3 = iVar3 + -0x2b;
      iVar1 = iVar1 + 1;
    } while (iVar1 < 5);
  }
  *(int32_t*)W[0x0CA4] = 0x8010;
  ((int32_t*)W[0x0CA4])[1] = 3;
  puVar5 = (int32_t*)W[0x0CA4] + 3;
  ((int32_t*)W[0x0CA4])[2] = 0xffff4000;
  W[0x0CA4] = W[0x0CA4] + 4 * 4;
  *puVar5 = 0xffffffff;
  hud_draw_3d_sprite(0,0x37d,0,0,0x350,0,0,0,0);
  if (W16(0xE10) == 0) {
    hud_draw_3d_sprite(0,0x366,0,0,0x350,0,0,0,0);
    uVar6 = 0x372;
  }
  else {
    hud_draw_3d_sprite(0,W16(0xE66) + 0x36e,0,0,0x350,0,0,0,0);
    uVar6 = 0x364;
  }
  hud_draw_3d_sprite(0,uVar6,0,0,0x350,0,0,0,0);
  return;
}

/* ---- result_screen_render_targets ---- */

void result_screen_render_targets(int param_1)

{
  undefined4 *puVar1;
  
  *(int32_t*)W[0x0CA4] = 0x8010;
  ((int32_t*)W[0x0CA4])[1] = 3;
  if (param_1 == 0) {
    W[0x4704] = (int32_t)0xffffd000;
  }
  else {
    W[0x4704] = (int32_t)0xffffe000;
  }
  puVar1 = (int32_t*)W[0x0CA4] + 3;
  ((int32_t*)W[0x0CA4])[2] = W[0x4704];
  W[0x0CA4] = W[0x0CA4] + 4 * 4;
  *puVar1 = 0xffffffff;
  hud_draw_3d_sprite(0,0x37d,0,0,0x350,0,0,0,0);
  hud_draw_3d_sprite(0,W16(0xE66) + 0x36e,0,0,0x350,0,0,0,0);
  hud_draw_3d_sprite(0,0x364,0,0,0x350,0,0,0,0);
  return;
}


/* ---- course-3 scenery (register row 186) ----------------------------------
 * The castle / tower / bridge complexes of SOLITAR, ROM 0x01605C..0x016A14,
 * ported from the listing. Their position source is 68020 MEMORY-INDIRECT:
 * `move.l ([, d3.l]), d0` with d3 = 0xE046F8 reads the POINTER held there and
 * then the long it points at -- in gameplay the three pointers at 0xE046F8 /
 * 0xE046FC / 0xE04700 all hold the ROM address 0x3637C. The transpile read
 * the pointer slot itself (or 0xE04704 / 0xE04708, which are the zsort latch
 * and the scenery flag word) as the coordinate. The ending points the same
 * slots at _W[] blocks or host stack arrays, so sc_ptr32 takes every form.
 * The flag word at 0xE04708 is 16-bit, the HIGH half of its slot (its low half
 * is the windmill counter, register row 125). */
static int32_t sc_ptr32(intptr_t p, int off)
{
  uintptr_t v = (uintptr_t)p;
  uintptr_t wlo = (uintptr_t)&_W[0], whi = (uintptr_t)&_W[WORK_RAM_SIZE];
  uintptr_t rlo = (uintptr_t)&g_sys.rom[0], rhi = rlo + ROM_SIZE;
  if (v >= wlo && v < whi) return (int32_t)_W[(v - wlo) / sizeof(intptr_t) + off];
  if (v >= rlo && v < rhi) return vrd32s((uint32_t)(v - rlo) + off);
  if (v >= 0xE00000u && v < 0xE00000u + WORK_RAM_SIZE) return (int32_t)_W[v - 0xE00000u + off];
  if (v < ROM_SIZE) return vrd32s((uint32_t)v + off);
  return ((const int32_t *)v)[off / 4];
}
static uint32_t sc_rom_off(const void *p)
{
  uintptr_t v = (uintptr_t)p, rlo = (uintptr_t)&g_sys.rom[0];
  if (v >= rlo && v < rlo + ROM_SIZE) return (uint32_t)(v - rlo);
  return (uint32_t)v;
}
static int16_t sc_flags(void) { return (int16_t)W_HI16(0x4708); }
static int32_t *sc_vp_head(int32_t *dl)
{
  W[0x169E8] = 0x8002;
  *dl++ = 0x8002;
  *dl++ = (W[0x0CC0] == 0xf) ? 3 : 0;
  return dl;
}
/* Set the zsort bias latch 0xE04704 to `v` (the `0x8010, 3, v, -1` marker),
 * or clear it (`0x8010, -1`) for v == 0 -- only when it changes. */
static int32_t *sc_bias(int32_t *dl, int32_t v)
{
  if (v != 0) {
    if ((int32_t)W[0x4704] != v) { *dl++ = 0x8010; *dl++ = 3; W[0x4704] = v; *dl++ = v; *dl++ = -1; }
  } else if (W[0x4704] != 0) {
    *dl++ = 0x8010; *dl++ = -1; W[0x4704] = 0;
  }
  return dl;
}
/* one 11-word 0x8002 entry: model, x, y, z, (0, 0x7fff), (s, c), (0, 0x7fff), 4 */
static int32_t *sc_ent(int32_t *dl, int32_t model, int32_t x, int32_t y, int32_t z, int32_t s, int32_t c)
{
  *dl++ = model; *dl++ = x; *dl++ = y; *dl++ = z;
  *dl++ = 0; *dl++ = 0x7fff; *dl++ = s; *dl++ = c; *dl++ = 0; *dl++ = 0x7fff; *dl++ = 4;
  return dl;
}

uint32_t * render_gate_or_ring(ea_t param_1_, ea_t param_2_, uint32_t *param_3)

{
  /* ROM 0x0161FC: rec = (flag mask, model when masked, model otherwise,
   * -, anim offset x, y, z) BE32 at `param_1`; pos = 3 longs. */
  uint32_t rec = sc_rom_off((const void *)param_1_);
  int32_t pos[3];                /* a C array, a _W[] block or a 68K address */
  pos[0] = sc_ptr32(param_2_, 0);
  pos[1] = sc_ptr32(param_2_, 4);
  pos[2] = sc_ptr32(param_2_, 8);
  int32_t *dl = (int32_t *)param_3;
  if (((int32_t)sc_flags() & vrd32s(rec)) != 0) {
    *dl++ = vrd32s(rec + 4);
    *dl++ = pos[0]; *dl++ = pos[1]; *dl++ = pos[2];
    return (uint32_t *)dl;
  }
  *dl++ = vrd32s(rec + 8);
  *dl++ = pos[0]; *dl++ = pos[1]; *dl++ = pos[2];
  dl = sc_vp_head(dl);
  dl = sc_ent(dl, ((int32_t)W[0x0C8C] & 0xf) + 0x23e,
              pos[0] + vrd32s(rec + 0x10), pos[1] + vrd32s(rec + 0x14), pos[2] + vrd32s(rec + 0x18),
              (int32_t)W[0x16668], (int32_t)W[0x1666C]);
  return (uint32_t *)dl;
}


/* ---- render_player_bike_model ---- */

int * render_player_bike_model(int *param_1)

{
  /* ROM 0x01605C -- the castle complex's rotating props, relative to the
   * castle position [0xE046F8] + (0x3f0cc, 0x19028, 0x11c90b). The fixed
   * pair is the low halves of the longs at 0x20C292/0x20C294 = (sin, cos) of
   * 0x1290 -- the transpile built it with CONCAT22 of single ROM BYTES. */
  intptr_t pp = W[0x46F8];
  int32_t x = sc_ptr32(pp, 0) + 0x3f0cc  - (int32_t)W[0x0CDC];
  int32_t y = sc_ptr32(pp, 4) + 0x19028  - (int32_t)W[0x0CE0];
  int32_t z = sc_ptr32(pp, 8) + 0x11c90b - (int32_t)W[0x0CE4];
  int32_t s = vrd16s(0x20C294), c = vrd16s(0x20C296);
  int32_t alt = (sc_flags() & 0x60) != 0;
  int32_t *dl = (int32_t *)param_1;
  dl = sc_ent(dl, 0x1fb + alt, x, y, z, s, c);
  if ((sc_flags() & 1) == 0) {
    int32_t d2;
    dl = sc_ent(dl, 0x1f5 + alt, x, y, z, s, c);
    dl = sc_ent(dl, 0x1f8, x, y, z, s, c);
    dl = sc_ent(dl, 0x1f9 + alt, x, y, z, s, c);
    if (sc_flags() & 0x40) {
      for (d2 = 0; d2 < 0x10000; d2 += 0x3333) {
        uint32_t a = (uint32_t)(((int32_t)W[0x0C8C] << 11) + d2) & 0xfffc;
        dl = sc_ent(dl, (((int32_t)W[0x0C8C] + d2) & 0xf) + 0x21e,
                    x + ((int32_t)vrd16s(0x20B004 + a) * 0xe00 >> 15), y,
                    z + ((int32_t)vrd16s(0x20B006 + a) * 0xe00 >> 15), s, c);
      }
    }
  }
  return (int *)dl;
}


/* ========== FINAL TIER - LEAF FUNCTIONS ========== */
/* 30 functions extracted from game_all.c */

/* ---- FUN_0000f26a ---- */

undefined4 FUN_0000f26a(void)

{
  int iVar1;
  int iVar2;
  short sVar3;
  uint16_t uVar4;
  
  iVar1 = W[0x0D00] + -0x5f5db;
  if (iVar1 < 0) {
    iVar1 = W[0x0D00] + -0x5f5d8;
  }
  iVar1 = iVar1 >> 2;
  iVar2 = W[0x0D08] + -0x120c49;
  if (iVar2 < 0) {
    iVar2 = W[0x0D08] + -0x120c46;
  }
  iVar2 = iVar2 >> 2;
  if (iVar2 * iVar2 + iVar1 * iVar1 < 0x5ed0aa9) {
    sVar3 = math_atan2(iVar1,iVar2);
    uVar4 = (sVar3 - (short)W[0x0D10]) + 0x8000;
    if ((uVar4 < 0x2000) || (0xe000 < uVar4)) {
      return 1;
    }
  }
  return 0;
}

/* ---- physics_spring_update_general ---- */
/* ROM 0x012764 -- one step of the final stage's spring chain (point p1 of the
 * 3-long positions at 0xE1642C, stride 12). The transpile indexed _W[] as
 * `int *` at `p1 * 3`, i.e. neither the byte stride nor the element width
 * (register row 185). Per axis k, with P = 0xE1642C + p1*12:
 *   F    = (p3*(prev[k] - P[k]) + p4*(P[k+3] - P[k])) / p2 + vec[k]
 *          - V[k]*42 / p2                       -> 0xE16354 + p1*12
 *   P[k] += V[k]/0xf0 + F/0xe100
 *   V[k] += F/0x3c                             (V = 0xE165DC + p1*12)
 * where prev = 0xE1648C + p1*12 is the copy of point p1-1 the previous call
 * saved (0xE16498 + (p1-1)*12) before it moved; this call saves P first. */
int physics_spring_update_general(int p1, int p2, int p3, int p4, const int32_t *vec)

{
  int32_t o = p1 * 12, k;
  for (k = 0; k < 3; k++) W[0x16498 + o + k * 4] = (int32_t)W[0x1642C + o + k * 4];
  for (k = 0; k < 3; k++) {
    int32_t P  = (int32_t)W[0x1642C + o + k * 4];
    int32_t Pn = (int32_t)W[0x1642C + o + 12 + k * 4];
    int32_t Pp = (int32_t)W[0x1648C + o + k * 4];
    int32_t V  = (int32_t)W[0x165DC + o + k * 4];
    int32_t d1 = (int32_t)((int64_t)(Pn - P) * p4);
    int32_t d0 = (int32_t)((int64_t)(Pp - P) * p3);
    int32_t F  = (d0 + d1) / p2 + vec[k] - (V * 42) / p2;
    W[0x16354 + o + k * 4] = F;
    W[0x1642C + o + k * 4] = P + (V / 0xf0 + F / 0xe100);
    W[0x165DC + o + k * 4] = V + F / 0x3c;
  }
  return 0;
}

/* ---- render_windmill ---- */

void render_windmill(void)

{
  uint32_t uVar1;
  undefined4 *puVar2;
  
  if (W[0x1257] < 0x12) {
    if (W[0x4704] != 0) {
      puVar2 = (int32_t*)W[0x0CA4] + 1;
      *(int32_t*)W[0x0CA4] = 0x8010;
      W[0x0CA4] = W[0x0CA4] + 2 * 4;
      *puVar2 = 0xffffffff;
      W[0x4704] = 0;
    }
    W[0x169E8] = 0x8002;
    *(int32_t*)W[0x0CA4] = 0x8002;
    ((int32_t*)W[0x0CA4])[1] = 0;
    /* 0x470A is a 2-mod-4 offset and a SIXTEEN-BIT value: ROM 0x014F80
     * `move.w d0,$e0470a.l` writes it and 0x01420E `adda.w $e0470a.l,a0`
     * reads it sign-extended. Read as a whole `_W[]` slot it was rebuilt from
     * neighbouring bytes by sync_wram_to_W every frame, so the model id
     * 0x251 + it ran off into the tens of thousands -- ~640 junk placements a
     * run once the per-course dispatch started calling this. */
    ((int32_t*)W[0x0CA4])[2] = (int)(int16_t)W_LO16(0x4708) + 0x251;
    ((int32_t*)W[0x0CA4])[3] = 0x54752 - W[0x0CDC];
    ((int32_t*)W[0x0CA4])[4] = 0x1866d - W[0x0CE0];
    ((int32_t*)W[0x0CA4])[5] = 0x73a56 - W[0x0CE4];
    ((int32_t*)W[0x0CA4])[6] = 0;
    ((int32_t*)W[0x0CA4])[7] = 0x7fff;
    uVar1 = (short)vrd16s(0x20B004 + ((W[0x0C8C] & 0x1f) * 0x400) * 2) + 0xd40 >> 6 & 0xfffc;
    ((int32_t*)W[0x0CA4])[8] = vrd16s(0x20B002 + uVar1);
    ((int32_t*)W[0x0CA4])[9] = vrd16s(0x20B004 + uVar1);
    ((int32_t*)W[0x0CA4])[10] = 0;
    puVar2 = (int32_t*)W[0x0CA4] + 0xc;
    ((int32_t*)W[0x0CA4])[0xb] = 0x7fff;
    W[0x0CA4] = W[0x0CA4] + 0xd * 4;
    *puVar2 = 4;
  }
  return;
}

/* ---- final 5 leaf functions ---- */

/* ---- calc_direction_and_distance ---- */

int calc_direction_and_distance(undefined4 *param_1,uint32_t *param_2)

{
  uint32_t uVar1;
  int iVar2;
  
  ((int32_t*)(intptr_t)param_2)[2] = 0;
  uVar1 = math_atan2(*param_1,((int32_t*)(intptr_t)param_1)[2]);
  ((int32_t*)(intptr_t)param_2)[1] = uVar1;
  iVar2 = m68k_divs((int32_t)(((int32_t*)(intptr_t)param_1)[2] << 0xf), (int)(vrd16s(0x20B006 + (uVar1 & 0xfffc))));
  uVar1 = math_atan2(iVar2,((int32_t*)(intptr_t)param_1)[1]);
  *param_2 = uVar1;
  iVar2 = m68k_divs(iVar2 << 0xd, (int)(vrd16s(0x20B004 + (uVar1 & 0xfffc))) >> 2);
  ((int32_t*)(intptr_t)param_2)[1] = -((int32_t*)(intptr_t)param_2)[1];
  *param_2 = -*param_2;
  if (iVar2 < 0) {
    iVar2 = -iVar2;
  }
  return iVar2;
}

/* ---- state handler deps (7) ---- */

/* ---- FUN_000098dc ---- */

uint32_t FUN_000098dc(void)

{
  int iVar1;
  undefined2 extraout_D0u;
  undefined2 uVar2;
  int iVar3;
  uint32_t uVar4;
  int iVar5;
  int iVar6;
  
  if (W[0x0C78] == 3) {
    sync_post();
  }
  dsp_cmd_set_camera(0,0x37d,0,0,0x350,0,0,0,0);
  dsp_cmd_set_camera(0,W16(0xE66) + 0x36e,0,0,0x350,0,0,0,0);
  dsp_cmd_set_camera(0,0x364,0,0,0x350,0,0,0,0);
  sprite_draw_2d(6,W[0x0E0C] * 2 + 0x1e1,0x40,0x90,0,0x1c,0x1c,0,0);
  sprite_draw_2d(6,W[0x0E0C] * 2 + 0x1e2,0x40,0x170,0,0x1c,0x1c,0,0);
  FUN_0000a0f6(W[0x0C78] >> 1);
  uVar2 = extraout_D0u;
  if (W16(0xE10) != 0) {
    if (W16(0xE6A) < 4) {
      iVar3 = (int)W16(0xE6A);
    }
    else {
      iVar3 = 3;
    }
    iVar5 = (int32_t)vrd32(0x15C328 + ((W16(0xE64) * 6 + W[0x0E0C] * 0x12 + W[0x3FFA] * 2) * 4)) -
            R[0x15C4F8] * iVar3;
    iVar3 = 0x87;
    iVar6 = 0;
    do {
      dsp_cmd_set_camera(2,iVar5 % 10 + 0x347,iVar3,0,0x1c0,0,0,0,0);
      dsp_cmd_set_camera(2,0x351,iVar3 + 4,0xfffffffb,0x1c1,0,0,0,0);
      iVar5 = iVar5 / 10;
      iVar3 = iVar3 + -0x2b;
      iVar1 = iVar6 + 1;
      uVar2 = (undefined2)((uint32_t)(iVar6 + -4) >> 0x10);
      iVar6 = iVar1;
    } while (iVar1 < 5);
  }
  uVar4 = (((uint32_t)(uVar2) << 16) | (uint16_t)(W[0x2B3A])) & 0xffff0100;
  if (((W[0x2B3A] & 0x100) != 0) ||
     (uVar4 = (((uint32_t)(uVar2) << 16) | (uint16_t)(W[0x2BA6])) & 0xffff0001, (W[0x2BA6] & 1) != 0)) {
    W[0x0C78] = 0x100;
  }
  W[0x0C78] = W[0x0C78] + 1;
  if (0x100 < W[0x0C78]) {
    uVar4 = 2;
    W[0x0CC0] = 2;
  }
  return uVar4;
}

/* ---- check_confirm_button ---- */

undefined2 check_confirm_button(void)

{
  undefined2 uVar1;
  
  if ((((W[0x2BA6] & 1) != 0) || ((W[0x2B3A] & 0x800) != 0)) ||
     (uVar1 = 0, (W[0x2B3A] & 0x100) != 0)) {
    uVar1 = 1;
  }
  return uVar1;
}

/* ---- highscore_table_reset_defaults ---- */

void highscore_table_reset_defaults(void)

{
  int iVar1;
  char cVar2;
  int iVar3;
  undefined1 *puVar4;
  undefined1 *puVar5;
  
  /* ROM 0x03179A: each top-ten entry is 8 BYTES -- `lea $100(a1,d3.l*8),a0`
   * with a1 = 0xE03F30 -- a long `0x7F000000 | (1700 - 100*i)`, three name
   * bytes copied from 0x39838, and the next-index byte. The long was written
   * at `W[0x4030 + i*2]`, which is Ghidra's element index on a 4-byte
   * pointer (i*2 elements = i*8 bytes) carried over into the byte-offset
   * `_W[]` model: every entry after the first landed inside its predecessor.
   * The same *2 on the per-course best scores at 0xE04088 (`move.l
   * #$7f0007d0,$158(a1,d1.l*8)` @0x0317FA). The byte fields were already
   * at the right offsets. Measured against MAME's own table (its pinned
   * NVRAM holds exactly these defaults): the RANKING screen now lists
   * EGA 1700 .. TKP 800 as the machine does. */
  puVar4 = &R[0x39838];
  iVar3 = 0;
  do {
    iVar1 = iVar3 * 8;
    W[0x4030 + (iVar1)] = (int32_t)(iVar3 * -100 + 0x6a4U | 0x7f000000);
    W[0x4034 + (iVar1)] = *puVar4;
    puVar5 = puVar4 + 2;
    W[0x4035 + (iVar1)] = puVar4[1];
    puVar4 = puVar4 + 3;
    W[0x4036 + (iVar1)] = *puVar5;
    if (iVar3 < 9) {
      cVar2 = (char)iVar3 + '\x01';
    }
    else {
      cVar2 = -1;
    }
    W[0x4037 + (iVar1)] = cVar2;
    iVar3 = iVar3 + 1;
  } while (iVar3 < 10);
  W[0x40A8] = 0;
  iVar3 = 0;
  do {
    W[0x15E70 + (iVar3)] = 0;
    W[0x4088 + (iVar3 * 8)] = 0x7f0007d0;
    W[0x408C + (iVar3 * 8)] = 0x31;
    W[0x408D + (iVar3 * 8)] = 0x31;
    W[0x408E + (iVar3 * 8)] = 0x31;
    iVar3 = iVar3 + 1;
  } while (iVar3 < 4);
  eeprom_write_block(&W[0x4030],0x79);
  return;
}

/* ---- FUN_0000fa9e ---- */

void FUN_0000fa9e(undefined4 param_1,int param_2)

{
  int iVar1;
  uint16_t uVar2;
  uint32_t uVar3;
  
  uVar2 = math_atan2(param_1,param_2);
  /* ROM 0x00FAC0..0x00FACE: `asr.l #5` of cos then `divs.l d1,d2`. The
   * divisor is ZERO for every angle within ~0.06 deg of 90/270 (|cos| < 32),
   * and the 68K's zero-divide vector (5 -> 0x00C0B4) is a bare `rte`: the
   * division is skipped and d2 keeps the dividend. So is `divs.l` overflow
   * (INT_MIN / -1). C raised SIGFPE instead -- a user's Wind Woods flight
   * died here, lined up with a scenery sound source at dz = -31 (register
   * row 180). */
  { int32_t num = (int32_t)((uint32_t)param_2 << 8);
    int32_t den = (int32_t)vrd16s(0x20B006 + ((uint32_t)(uVar2 & 0xfffc))) >> 5;
    uVar3 = (uint32_t)m68k_divs(num, den); }
  if ((int)uVar3 < 0) {
    uVar3 = -uVar3;
  }
  iVar1 = (uint32_t)uVar2 - (W[0x0CEC] & 0xffff);
  if ((((iVar1 < 0xc00) && (-0xc00 < iVar1)) && ((int)uVar3 < 0x7000)) || ((int)uVar3 < 0x1000)) {
    FUN_0000f5ca(0x27, (int)(uint16_t)(uVar3 >> 0xf));   /* 0x00FB0A..0x00FB14: two WORD pushes */
  }
  return;
}

/* ---- FUN_0003e128 ---- */

uint64_t FUN_0003e128(void)

{
  /* void */;
  /* void */;
  
  watchdog_timer_service();
  input_read_service_buttons();
  input_decode_buttons();
  read_mcu_inputs_process();
  W[0xAB10] = W[0xAB10] + 1;
  W[0xAB12] = W[0xAB12] + 1;
  if ((((W[0x2B38] & 0x800) == 0) && ((W[0x2BA4] & 1) == 0)) && ((W[0x2B80] & 8) != 0))
  {
    W[0xAB12] = 0;
  }
  if (((W[0x2B38] & 8) == 0) && ((W[0x2BD8] & 0xf) == 0)) {
    W[0xAB10] = 0;
  }
  W[0xAB26] = 1;
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- next level deps (10) ---- */

/* ---- input_read_service_buttons ---- */

void input_read_service_buttons(void)

{
  uint8_t bVar1;
  uint16_t uVar2;
  uint16_t uVar3;
  
  uVar3 = W[0x2BA4];
  uVar2 = W[0x2B80];
  bVar1 = g_sys.commsram[0x7D02];
  W[0x2B80] = (uint16_t)bVar1;
  W[0x2B82] = (uint16_t)bVar1 & ~uVar2;
  bVar1 = g_sys.commsram[0x7D04];
  W[0x2BA4] = (uint16_t)bVar1;
  W[0x2BA6] = (uint16_t)bVar1 & ~uVar3;
  return;
}

/* ---- read_mcu_inputs_process ---- */

int read_mcu_inputs_process(void)

{
  uint16_t uVar1;
  int iVar2;
  
  /* THE EIGHT ADC CHANNELS ARE 16-BIT AND STRIDE TWO.
   *
   * ROM 0x0222C2 is `move.w (a0, d2.l*2), (a3, d2.l*2)` with a0 = 0xA0BD0A
   * (MCU shared RAM 0x7D0A) and a3 = 0xE02BC8 -- eight BIG-ENDIAN WORDS into
   * W[0x2BC8], W[0x2BCA], ... W[0x2BD6]. The transpile copied eight single
   * BYTES at stride one, so W[0x2BC8] -- the handlebar -- received the ADC's
   * high byte, and the direction test below (`W[0x2BC8]` against
   * `W[0x3FD0] +- 0x80`) could never resolve LEFT or RIGHT.
   *
   * That is why this whole function was EXCLUDED from the frame loop: it
   * would overwrite the handlebar slots that `input.c` owns with garbage, so
   * `game_logic.c` left it out and the direction flags W[0x2BD8]/0x2BDC --
   * which only this function computes -- were never produced at all. The
   * MODE SELECT and STAGE SELECT cursors read those flags, so the stick
   * could not move between menu options. `PROPCYCL_NO_MENUSTICK=1` restores
   * the old behaviour for A/B.
   *
   * With the copy correct the function is safe to call, and `input.c` now
   * publishes the live handlebar into the same shared-RAM words the machine
   * uses, so the game derives its own direction flags from its own data
   * rather than having them synthesised. */
  { extern int g_menustick;
    const uint8_t *cs = g_sys.commsram;
    iVar2 = 0;
    do {
      if (g_menustick) {
        /* CHANNELS 0 AND 1 ARE THE HANDLEBAR AND input.c OWNS THEM.
         *
         * On the machine the MCU samples the real ADC into shared RAM and
         * this copy brings it across. Here input.c IS the ADC stand-in: it
         * writes W[0x2BC8]/W[0x2BCA] directly and pins them, and the MCU
         * emulation is meanwhile running the real sound driver out of
         * pr1data.8k, which writes that block for its own purposes. Copying
         * channels 0/1 over therefore replaced a live 511 with whatever the
         * driver had left there -- measured: with the stick dead centre the
         * direction word came out 0x0009 (LEFT | Y-down), which drove the
         * MODE SELECT cursor to ADVANCED and sent the whole chain to sub 22
         * instead of the stage select, so gameplay was never reached.
         *
         * What this function is needed FOR is the direction/edge computation
         * below -- nothing else produces W[0x2BD8]/0x2BDC -- so take that and
         * leave the two slots their owner maintains. The other six channels
         * are copied as the ROM does. */
        if (iVar2 >= 2) {
          int a = 0x7D0A + iVar2 * 2;
          W[0x2BC8 + iVar2 * 2] = (int32_t)(int16_t)((cs[a] << 8) | cs[a + 1]);
        }
      } else {
        W[0x2BC8 + (iVar2)] = g_sys.commsram[0x7D0A + (iVar2)];
      }
      uVar1 = (uint32_t)(W_HI16(0x2BD8) & 0xFFFF);
      iVar2 = iVar2 + 1;
    } while (iVar2 < 8); }
  /* STICK DIRECTION (0x2BD8) AND ITS EDGE (0x2BDA) ARE 16-BIT HALVES OF ONE
   * SLOT. ROM 0x0222D0 `move.w (a2),d4` reads 0x2BD8 as a word (the HIGH
   * half); 0x022328 `move.w d0,$2(a2)` writes the edge to 0x2BDA (the LOW
   * half). Written full-width, the direction landed in the edge's half and
   * the edge -- which the STAGE SELECT cursor keys on (`& 1`) -- sat on a
   * 2-mod-4 slot the _W[] sync rebuilds every frame. Rows 46/108's class;
   * it is why a headless stick pulse never moved the course cursor. */
  /* 0x2BDA IS A RISING EDGE -- `dir & ~prev`. CORRECTS REGISTER ROW 149,
   * which read this as the intersection `dir & prev` and changed the code to
   * match. ROM 0x022320:
   *     moveq  #$0, d0
   *     move.w d4, d0        ; d4 = the PREVIOUS direction word
   *     not.l  d0            ; <<< THE not.l ROW 149 MISSED
   *     and.w  (a2), d0      ; d0 = dir & ~prev
   *     move.w d0, $2(a2)    ; -> 0x2BDA
   *     clr.w  $4(a2)        ; 0x2BDC = 0
   *     move.w $2(a2), d0
   *     or.w   d0, $4(a2)    ; 0x2BDC |= 0x2BDA
   * d0 is zeroed by the moveq and loaded with a WORD, so `not.l` leaves the
   * low half as ~prev and the `and.w` takes dir & ~prev: TRUE ONLY ON THE
   * FRAME THE STICK CROSSES THE THRESHOLD.
   *
   * The intersection is what a user reported as "I have to really tilt to move
   * the selection and then it skips some": with `dir & prev` the bit is set on
   * every frame the stick is held past the threshold, so a menu cursor keyed
   * on 0x2BDC steps EVERY FRAME and one push runs through several options.
   * The repeat the ROM does want is the block below -- `W[0x2BE4] > 0x14` then
   * every 10 frames -- which only makes sense on top of a one-shot edge.
   *
   * Row 149's own reasoning ("a rising edge is true for one frame ... the menu
   * saw a one-frame blip instead of a steady push") was the right description
   * of the wrong thing: the cursor was dead before row 149 because this whole
   * function was not called and the ADC copy was byte-wise, which row 149 also
   * fixed. Those two were the fix; this third change rode along and was wrong.
   * `PROPCYCL_NO_EDGEFIX=1` restores the intersection for A/B.
   *
   * 0x2BDC is kept as a FULL SLOT as well as its high half: the ROM word
   * lives at 0x2BDC (4-aligned, so the high half) and the high half is what
   * `sync_W_to_wram` must carry, but all fourteen consumers in this tree
   * read `W[0x2BDC] & n` on the whole slot -- and the repeat-delay line
   * below ORs whole slots too. Writing both keeps work_ram faithful for a
   * MAME comparison and every existing reader correct; 0x2BDE is unused. */
  { int dir = ((int)W[0x2BC8] < W[0x3FD0] + -0x80) ? 1 : 0;
    if (W[0x3FD0] + 0x80 < (int)W[0x2BC8]) dir |= 2;
    if ((int)W[0x2BCA] < W[0x3FD2] + -0xc0) dir |= 8;
    if (W[0x3FD2] + 0xc0 < (int)W[0x2BCA]) dir |= 4;
    extern int g_menustick, g_edgefix;
    int held = !g_menustick ? (dir & ~(int)uVar1)          /* row 149 revert  */
             : g_edgefix    ? (dir & ~(int)uVar1)          /* ROM: not.l+and.w */
                            : (dir & (int)uVar1);          /* row 149, for A/B */
    W_SET_HI16(0x2BD8, (int16_t)dir);
    W_SET_LO16(0x2BD8, (int16_t)held);          /* 0x2BDA */
    W[0x2BDC] = ((intptr_t)(int16_t)held << 16) | (uint16_t)held;
  }
  if (uVar1 == (uint32_t)(W_HI16(0x2BD8) & 0xFFFF)) {
    W[0x2BE4] = W[0x2BE4] + 1;
  }
  else {
    W[0x2BE4] = 0;
  }
  if ((0x14 < W[0x2BE4]) && (W[0x2BE8] = W[0x2BE8] + 1, 9 < W[0x2BE8])) {
    W[0x2BE8] = 0;
    W[0x2BDC] = W[0x2BD8] | W[0x2BDC];
  }
  if ((W[0x2BD8] & 9) == 9) {
    W[0x2C0D] = 1;
    iVar2 = 9;
  }
  else if ((W[0x2BD8] & 5) == 5) {
    W[0x2C0D] = 2;
    iVar2 = 0;
  }
  else if ((W[0x2BD8] & 10) == 10) {
    W[0x2C0D] = 3;
    iVar2 = 10;
  }
  else {
    iVar2 = (W[0x2BD8] & 6) - 6;
    if (iVar2 == 0) {
      W[0x2C0D] = 4;
    }
    else {
      W[0x2C0D] = 0;
    }
  }
  return iVar2;
}

/* ---- fixed_mul_16x16_to_32 ---- */

uint32_t fixed_mul_16x16_to_32(undefined4 param_1)

{
  return (int)param_1 * (int)param_1 * 0x20000 |
         (uint32_t)((int)param_1 * (int)param_1 * 2) >> 0x10;
}

/* ---- inverse_transform_point ---- */

void inverse_transform_point(int *param_1,int *param_2,int *param_3)

{
  short *psVar1;
  int iVar2;
  int iVar3;
  
  psVar1 = (short *)((int32_t)rom_nr32(0x37660) + (((int32_t*)(intptr_t)param_2)[3] & 0xfffc));
  iVar3 = (((int32_t*)(intptr_t)param_1)[1] * (int)*psVar1 >> 0xf) - (((int32_t*)(intptr_t)param_1)[2] * (int)psVar1[-1] >> 0xf);
  iVar2 = (((int32_t*)(intptr_t)param_1)[2] * (int)*psVar1 >> 0xf) + (((int32_t*)(intptr_t)param_1)[1] * (int)psVar1[-1] >> 0xf);
  psVar1 = (short *)((int32_t)rom_nr32(0x37660) + (((int32_t*)(intptr_t)param_2)[4] & 0xfffc));
  ((int32_t*)(intptr_t)param_3)[2] = iVar2 * *psVar1 >> 0xf;
  ((int32_t*)(intptr_t)param_3)[2] = ((int32_t*)(intptr_t)param_3)[2] - ((*param_1 * (int)psVar1[-1] >> 0xf) - ((int32_t*)(intptr_t)param_2)[2]);
  iVar2 = (*param_1 * (int)*psVar1 >> 0xf) + (iVar2 * psVar1[-1] >> 0xf);
  psVar1 = (short *)((int32_t)rom_nr32(0x37660) + (((int32_t*)(intptr_t)param_2)[5] & 0xfffc));
  *param_3 = iVar2 * *psVar1 >> 0xf;
  *param_3 = *param_3 - ((iVar3 * psVar1[-1] >> 0xf) - *param_2);
  ((int32_t*)(intptr_t)param_3)[1] = iVar2 * psVar1[-1] >> 0xf;
  ((int32_t*)(intptr_t)param_3)[1] = ((int32_t*)(intptr_t)param_2)[1] + (iVar3 * *psVar1 >> 0xf) + ((int32_t*)(intptr_t)param_3)[1];
  return;
}

/* ---- inverse_transform_point_pair ---- */

void inverse_transform_point_pair(int *param_1,int *param_2,int *param_3,int *param_4)

{
  short *psVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  int iVar5;
  
  psVar1 = (short *)((int32_t)rom_nr32(0x37660) + (((int32_t*)(intptr_t)param_2)[3] & 0xfffc));
  iVar4 = (((int32_t*)(intptr_t)param_1)[1] * (int)*psVar1 >> 0xf) - (((int32_t*)(intptr_t)param_1)[2] * (int)psVar1[-1] >> 0xf);
  iVar2 = (((int32_t*)(intptr_t)param_1)[2] * (int)*psVar1 >> 0xf) + (((int32_t*)(intptr_t)param_1)[1] * (int)psVar1[-1] >> 0xf);
  psVar1 = (short *)((int32_t)rom_nr32(0x37660) + (((int32_t*)(intptr_t)param_2)[4] & 0xfffc));
  iVar3 = ((int32_t*)(intptr_t)param_2)[2] + (iVar2 * *psVar1 >> 0xf);
  iVar5 = *param_1 * (int)psVar1[-1] >> 0xf;
  ((int32_t*)(intptr_t)param_3)[2] = iVar3 - iVar5;
  ((int32_t*)(intptr_t)param_4)[2] = iVar5 + iVar3;
  iVar5 = iVar2 * psVar1[-1] >> 0xf;
  iVar3 = *param_1 * (int)*psVar1 >> 0xf;
  iVar2 = iVar3 + iVar5;
  iVar5 = iVar5 - iVar3;
  psVar1 = (short *)((int32_t)rom_nr32(0x37660) + (((int32_t*)(intptr_t)param_2)[5] & 0xfffc));
  iVar3 = (iVar4 * psVar1[-1] >> 0xf) - *param_2;
  *param_3 = (iVar2 * *psVar1 >> 0xf) - iVar3;
  *param_4 = (iVar5 * *psVar1 >> 0xf) - iVar3;
  iVar3 = ((int32_t*)(intptr_t)param_2)[1] + (iVar4 * *psVar1 >> 0xf);
  ((int32_t*)(intptr_t)param_3)[1] = iVar3 + (iVar2 * psVar1[-1] >> 0xf);
  ((int32_t*)(intptr_t)param_4)[1] = iVar3 + (iVar5 * psVar1[-1] >> 0xf);
  return;
}

/* ---- process_dsp_results ---- */

void process_dsp_results(void)

{
  short sVar1;
  
  sVar1 = g_sys.commsram[0x7E82];
  W[0x0CB0] = (uint16_t)(sVar1 != -1);
  if ((sVar1 != -1) != 0) {
    g_sys.commsram[0x7E82] = 0xffff;
  }
  return;
}

/* ---- render_bridge_structure @ 0x016900 ---------------------------------
 * The bridge (model 499 = 0x1F3) at the position block W[0x4700] points at,
 * with a zsort bias chosen by its camera-relative height/depth, then its
 * gate. PORTED FROM THE ROM (0x016900..0x0169FC): `move.l ([,d1.l]),d0`
 * with d1 = 0xE04700 DEREFERENCES the pointer held there -- the transpile
 * subtracted the camera from the POINTER SLOTS 0xE04700/0xE04704/0xE04708
 * themselves. W[0x4700] holds a 68K address or a host pointer to a C array
 * (include/ea68k.h). */

/* ROM 0x016900, ported from the machine code: the bridge (model 0x1F3) at
 * [W[0x4700]] relative to the camera, with the zsort bias the camera height
 * and depth select (0x8010, 3, bias, -1 on a change; 0x8010, -1 to clear),
 * then its gate/ring from table 0x3645C. W[0x4700] holds a 68K address or a
 * host pointer; the camera-relative vector is a stack local on the machine. */
void render_bridge_structure(void)
{
  /* ROM 0x016900 -- position [0xE04700] - camera; bias by camera-relative
   * height/depth; model 0x1f3, then the gate record at 0x3645C. */
  intptr_t pp = W[0x4700];
  int32_t v[3];
  int32_t *dl;
  v[0] = sc_ptr32(pp, 0) - (int32_t)W[0x0CDC];
  v[1] = sc_ptr32(pp, 4) - (int32_t)W[0x0CE0];
  v[2] = sc_ptr32(pp, 8) - (int32_t)W[0x0CE4];
  dsp_cmd_emit_object_mode_8000();
  dl = (int32_t *)W[0x0CA4];
  if (v[1] > -0x28000)        dl = sc_bias(dl, -0x5c00);
  else if (v[1] > -0x32000)   dl = sc_bias(dl, (v[2] > -0x128000) ? -0x2a40 : -0x2000);
  else                        dl = sc_bias(dl, 0);
  *dl++ = 0x1f3; *dl++ = v[0]; *dl++ = v[1]; *dl++ = v[2];
  dl = sc_bias(dl, 0);
  W[0x0CA4] = (intptr_t)dl;
  dsp_cmd_emit_object_mode_8000();
  W[0x0CA4] = (intptr_t)render_gate_or_ring((ea_t)0x3645C, (ea_t)v,
                                            (uint32_t *)W[0x0CA4]);
  return;
}

/* ---- render_castle_complex ---- */

/* ROM 0x016690, ported from the machine code: the castle (model 0x1F4) at
 * [W[0x46F8]] relative to the camera, its zsort bias, the rotating town
 * (0x16432) under a mode-0x8002 header, its gate/ring (table 0x36424) and the
 * 0x1605C structure. W[0x46F8] holds a 68K address or a host pointer to a C
 * array (include/ea68k.h); the camera-relative vector is a stack local on the
 * machine: EFRAME(0). */
void render_castle_complex(void)
{
  /* ROM 0x016690 -- the town/castle at [0xE046F8]: model 500 (0x1f4), the
   * eight rotating town pieces, the gate record at 0x36424, the props. */
  intptr_t pp = W[0x46F8];
  int32_t v[3];
  int32_t *dl;
  v[0] = sc_ptr32(pp, 0) - (int32_t)W[0x0CDC];
  v[1] = sc_ptr32(pp, 4) - (int32_t)W[0x0CE0];
  v[2] = sc_ptr32(pp, 8) - (int32_t)W[0x0CE4];
  dsp_cmd_emit_object_mode_8000();
  dl = (int32_t *)W[0x0CA4];
  dl = sc_bias(dl, (v[1] > -0x19000) ? -0x7000 : 0);
  *dl++ = 0x1f4; *dl++ = v[0]; *dl++ = v[1]; *dl++ = v[2];
  dl = sc_vp_head(dl);
  dl = (int32_t *)render_town_with_rotation((int *)pp, (undefined4 *)dl);
  dl = sc_bias(dl, 0);
  W[0x0CA4] = (intptr_t)dl;
  dsp_cmd_emit_object_mode_8000();
  W[0x0CA4] = (intptr_t)render_gate_or_ring((ea_t)0x36424, (ea_t)v,
                                            (uint32_t *)W[0x0CA4]);
  dsp_cmd_emit_object_mode_8002();
  e_setcur((int32_t *)render_player_bike_model((int *)e_cur()));
}

/* ---- render_castle_turrets ---- */

int render_castle_turrets(int param_1)

{
  /* ROM 0x0165C8 -- four DYNAMIC COLLISION ZONES around the tower at
   * [0xE046FC] + (0x338ce, 0x22c18, 0x1486b0), appended to the zone arrays
   * 0xE02920 (x) / 50 (y) / 80 (z) / B0 (heading) / E0 / 0xE02A10 / 40,
   * index W[0x291C], 4-byte stride. */
  intptr_t pp = W[0x46FC];
  int32_t bx = sc_ptr32(pp, 0) + 0x338ce;
  int32_t by = sc_ptr32(pp, 4) + 0x22c18;
  int32_t bz = sc_ptr32(pp, 8) + 0x1486b0;
  int32_t n = (int32_t)W[0x291C], k;
  uint32_t a = (uint32_t)param_1 + 0x2000;
  W[0x291C] = n + 4;
  for (k = 0; k < 4 && n + k < 12; k++) {
    uint32_t o = (uint32_t)(n + k) * 4, idx = (uint16_t)a & 0xfffc;
    W[0x2A40 + o] = 0x9858;
    W[0x2A10 + o] = 0x5dc;
    W[0x29E0 + o] = 0x9c4;
    W[0x29B0 + o] = (int32_t)(0x17d3 - a);
    W[0x2980 + o] = bz - ((int32_t)vrd16s(0x20B004 + idx) * 0x470e >> 15);
    W[0x2950 + o] = by;
    W[0x2920 + o] = bx + ((int32_t)vrd16s(0x20B006 + idx) * 0x470e >> 15);
    a += 0x4000;
  }
  return 0;
}

/* ---- render_floating_bonus_object ---- */

void render_floating_bonus_object(void)

{
  /* ROM 0x0124EC. The three `addi.l #$2b0b0 / #$1ebb0 / #$8651b` are
   * IMMEDIATES (a world position), not ROM addresses; the transpile made two
   * of them host pointers. The position is a 3-long stack vector, passed to
   * dsp_emit_dual_arrows by address (`pea -$c(a6)`). */
  int32_t iVar1;
  int32_t pos[3];
  
  iVar1 = (int)(short)vrd16s(0x20B004 + (((uint32_t)W[0x0C8C] << 6) & 0xfffc)) >> 2;
  pos[0] = (iVar1 * 0x29000 >> 0xd) + 0x2b0b0;
  pos[1] = ((iVar1 << 0xc) >> 0xd) + 0x1ebb0;
  pos[2] = (iVar1 * 0x3000 >> 0xd) + 0x8651b;
  dsp_cmd_place_object_abs
            (0,0x1b7,pos[0] - (int32_t)W[0x0CDC],pos[1] - (int32_t)W[0x0CE0],
             pos[2] - (int32_t)W[0x0CE4]);
  dsp_emit_dual_arrows(pos);
  return;
}

/* ---- render_stage_landmarks ---- */

void render_stage_landmarks(void)

{
  scenery_objects_render_dynamic();
  if (W[0x1249] < 0x10) {
    render_animated_birds();
  }
  if ((W[0x0CF4] == 0x3c) || (((W[0x0CF4] + 1U | 9) == 0x3d && (W[0x0CE0] < 0x13800)))) {
    render_ferris_wheel();
  }
  return;
}

/* ---- render_tower_complex ---- */

/* ROM 0x01679E, ported from the machine code: the tower (model 0x1FF) at
 * [W[0x46FC]] relative to the camera with its zsort bias, its gate/ring
 * (table 0x36440), the spinning top (model 0x1FE, 11-word 0x8002 entry turned
 * by -frame << 7, or << 8 while flag 1 of 0xE04708 is set), then the four
 * turret collision zones at that angle. The vector is EFRAME(1). */
void render_tower_complex(void)
{
  /* ROM 0x01679E -- the tower at [0xE046FC]: model 0x1ff, the gate record at
   * 0x36440, then the spinning top 0x1fe (pair = (sin, cos) of -fc<<8 or <<7,
   * the low halves at 0x20B002+idx -- register row 87) and its four zones. */
  intptr_t pp = W[0x46FC];
  int32_t v[3], a;
  uint32_t idx;
  int32_t *dl;
  v[0] = sc_ptr32(pp, 0) - (int32_t)W[0x0CDC];
  v[1] = sc_ptr32(pp, 4) - (int32_t)W[0x0CE0];
  v[2] = sc_ptr32(pp, 8) - (int32_t)W[0x0CE4];
  dsp_cmd_emit_object_mode_8000();
  dl = (int32_t *)W[0x0CA4];
  dl = sc_bias(dl, (v[1] > -0x226f0) ? -0x4400 : 0);
  *dl++ = 0x1ff; *dl++ = v[0]; *dl++ = v[1]; *dl++ = v[2];
  dl = sc_bias(dl, 0);
  dl = (int32_t *)render_gate_or_ring((ea_t)0x36440, (ea_t)v, (uint32_t *)dl);
  dl = sc_vp_head(dl);
  a = (sc_flags() & 1) ? (-(int32_t)W[0x0C8C] << 8) : (-(int32_t)W[0x0C8C] << 7);
  idx = (uint16_t)a & 0xfffc;
  dl = sc_ent(dl, 0x1fe, v[0] + 0x338ce, v[1] + 0x22830, v[2] + 0x1486b0,
              vrd16s(0x20B004 + idx), vrd16s(0x20B006 + idx));
  W[0x0CA4] = (intptr_t)dl;
  render_castle_turrets(a);
  return;
}

/* ---- sincos_scale ---- */

void sincos_scale(int param_1,int param_2,int *param_3,int *param_4)

{
  *param_4 = param_2 * (int16_t)vrd16s(0x20B004 + (param_1 + 0x4000U >> 1 & 0x7ffe) * 2) >> 0xf;
  *param_3 = param_2 * (int16_t)vrd16s(0x20B006 + (param_1 + 0x4000U >> 1 & 0x7ffe) * 2) >> 0xf;
  return;
}

/* ---- spline_interpolate_channel ---- */

/* ROM 0x0269C6 -- evaluate one CUBIC keyframe segment at frame t:
 *     p = stream + 2 : four big-endian longs c3, c2, c1, c0
 *     v = (c3 * T3[t] >> 24) + (c2 * T2[t] >> 16) + ((*acc += c1) >> 8) + c0
 * with T3 = 0x153B2C, T2 = 0x15332C (both BE32, indexed t*4). The two
 * products are 64-bit (`muls.l (a0)+,dh:dl`) and the ROM keeps bits 55..24
 * and 47..16 of them (`and`/`or`/`rol.l #8`, `move.w`/`swap`).
 *
 * `param_1` is the stream's ROM ADDRESS and `param_2` a real int32 -- the
 * transpile dereferenced the address as a host pointer. */
int spline_interpolate_channel(intptr_t param_1,intptr_t param_2_any,int param_3)

{
  uint32_t p = (uint32_t)param_1 + 2;
  int t = (int)(int16_t)(uint32_t)param_3;
  int64_t a, b;
  int32_t d0, acc;
  if (p + 16 > ROM_SIZE || t < 0 || t > 0x1ff) return 0;
  a = (int64_t)(int32_t)vrd32(p)     * (int64_t)(int32_t)vrd32(0x153B2C + t * 4);
  b = (int64_t)(int32_t)vrd32(p + 4) * (int64_t)(int32_t)vrd32(0x15332C + t * 4);
  d0 = (int32_t)(uint32_t)((uint64_t)a >> 24) + (int32_t)(uint32_t)((uint64_t)b >> 16);
  /* the accumulator: a host int (master's anim code) or a 68K work-RAM
   * address (story-merge's ending) -- register row 188 */
  if (param_2_any >= 0xE00000 && param_2_any < 0xE00000 + WORK_RAM_SIZE) {
    acc = (int32_t)W[param_2_any - 0xE00000] + (int32_t)vrd32(p + 8);
    W[param_2_any - 0xE00000] = acc;
  } else {
    int *param_2 = (int *)param_2_any;
    acc = (param_2 ? *param_2 : 0) + (int32_t)vrd32(p + 8);
    if (param_2) *param_2 = acc;
  }
  return d0 + (acc >> 8) + (int32_t)vrd32(p + 12);
}

/* ---- vector_to_euler_angles ---- */

void vector_to_euler_angles
               (undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 *param_4,
               int *param_5,int *param_6)

{
  int iVar1;
  int iVar2;
  
  iVar1 = math_atan2(param_1,param_3);
  rotate_vector_euler_xyz(param_1,param_2,param_3,0,-iVar1,0,&param_1,&param_2,&param_3);
  iVar2 = math_atan2(param_3,param_2);
  rotate_vector_euler_xyz(param_1,param_2,param_3,-(iVar2 + 0x4000),0,0,&param_1,&param_2,&param_3);
  *param_4 = param_3;
  *param_5 = iVar2 + 0x4000;
  *param_6 = iVar1;
  return;
}

/* ---- viewport_params_write ---- */

void viewport_params_write(void)

{
  *(int *)(&g_sys.dspram[0x101B8] + W[0x0CA0] * 0x8000) = (int)W16(0x172F2);
  *(int *)(&g_sys.dspram[0x101BC] + W[0x0CA0] * 0x8000) = (int)W16(0x172F8);
  *(int *)(&g_sys.dspram[0x101C0] + W[0x0CA0] * 0x8000) = (int)W16(0x172F6);
  *(undefined4 *)(&g_sys.dspram[0x101C4] + W[0x0CA0] * 0x8000) = 0;
  *(uint32_t *)(&g_sys.dspram[0x101D4] + W[0x0CA0] * 0x8000) =
       ((0x140 - W16(0x172F8)) * (int)W16(0x172F4) * 2 >> 8) - (0x140 - W16(0x172F8)) & 0xffff;
  *(uint32_t *)(&g_sys.dspram[0x101D8] + W[0x0CA0] * 0x8000) =
       ((0xf0 - W16(0x172F6)) * 0x1a0 >> 8) - (0xf0 - W16(0x172F6)) & 0xffff;
  *(undefined4 *)(&g_sys.dspram[0x101F4] + W[0x0CA0] * 0x8000) = 0x10000;
  *(undefined4 *)(&g_sys.dspram[0x101F8] + W[0x0CA0] * 0x8000) = 0;
  *(undefined4 *)(&g_sys.dspram[0x101FC] + W[0x0CA0] * 0x8000) = 0;
  return;
}

int g_train_ybias = 0;    /* row 158: see train_render's def_y */

/* ---- waterfall_render -- THE TRAIN. The Ghidra name is wrong. ---- */

/* ROM 0x013F0E. It draws TWO CARS, models 0x388 and 0x389, 3000 units apart
 * in z, at a position driven by the frame counter:
 *
 *   013F2A: move.l $e00c8c.l, d3     ; the frame counter
 *   013F30: andi.l #$1ff, d3         ; & 0x1FF
 *   013F36: muls.l #$ffffff7e, d3    ; * -130   -> -130 units per frame,
 *                                    ;             looping every 512 frames
 *   013F7A: addi.l #$bb8, d3         ; +3000 for the second car
 *   013F80: cmpi.l #$389, d2 ; bls   ; models 0x388 and 0x389
 *
 * Point-ROM 973/974 render as two loaded wagons, and course 2 (INDUSTARN)
 * places those same models as STATIC scenery at chunks 25, 60 and 85 --
 * chunk 60 carries the 904+905 PAIR, which is this pair. So this is the
 * moving train on the industrial level's tracks, not a waterfall.
 *
 * THREE TRANSPILATION FAULTS FIXED HERE, all the documented classes:
 *  - param_2 was `int *` holding the ROM ADDRESS 0x36108 and every field was
 *    a HOST dereference of it, read little-endian. It is a big-endian ROM
 *    table: three LONGs (x, y, z offsets) then three WORDs (the rotations).
 *  - the second rotation argument was `*(int16_t *)param_2 + 0xe` -- the
 *    value at +0 plus fourteen -- where ROM 0x013F58 reads the WORD at
 *    $e(a2). The other two were right.
 *  - param_1 was an `int` used as a pointer (`*(int *)(param_1 + 4)`).
 *
 * `rec` is the caller's record: the ROM's own callers pass a landmark record
 * whose first long is an id and whose next two are the base x and z. */
void waterfall_render(const int32_t *rec, uint32_t tbl)
{
  int32_t x = rec[1] + vrd32s(tbl + 0);
  int32_t y = (int32_t)g_camera_offset_y + vrd32s(tbl + 4) + g_train_ybias;
  int32_t z = rec[2] + vrd32s(tbl + 8)
            + ((int32_t)(W[0x0C8C] & 0x1ff)) * -0x82;     /* -130 per frame */
  uint32_t model = 0x388;
  do {
    dsp_cmd_set_camera(0, model, x, y, z,
                       vrd16s(tbl + 0x0c), vrd16s(tbl + 0x0e), vrd16s(tbl + 0x10), 0);
    model++;
    z += 3000;                                            /* ROM: addi.l #$bb8 */
  } while (model < 0x38a);
}

/* THE TRAIN IS NOT REACHABLE IN THE RETAIL ROM, and this runs it anyway
 * because a user asked for it. Stated plainly so nobody later mistakes it
 * for hardware behaviour:
 *
 * waterfall_render is called from exactly four sites -- inside
 * stage_handler_course0..3 (ROM 0x13F8E/13FC0/13FF2/14024) -- and those four
 * functions have ZERO references anywhere in the 4 MB program ROM: no
 * absolute pointer, no PC-relative lea/jsr/bsr, no pointer table. Scanned
 * every form. That independently confirms register row 135, which reached
 * the same conclusion from the other side with ~1450 simulated seconds of
 * MAME breakpoints across all four courses and zero hits, and MAME's 2821
 * captured frames draw codes 970/971/972 but never 973/974. So the animation
 * path is dead code in this revision and the real machine cannot show it
 * either.
 *
 * Each handler fires on a per-course record id and passes a per-course path
 * table: course 0 id 0x44 table 0x36108, course 1 id 0x15 table 0x36130,
 * course 2 id 0x10 and course 3 id 0x0A both table 0x3611C. With no caller
 * there is no record to supply the base position, so the base here is the
 * world origin of the chunk whose scenery records place the two cars --
 * chunk 60 on course 2, the only course whose LOD tables reference them.
 * That placement is a CHOICE, not ROM data; everything else is the ROM's.
 *
 * PROPCYCL_NO_TRAIN=1 turns it off and restores retail behaviour. */
/* DEFAULT OFF, and the reason is a correction: THESE ARE NOT THE TRAIN THE
 * PLAYER SEES. Flying real MAME to the red track on course 0 and dumping its
 * polygon RAM there shows the train is codes **970 (a locomotive) + 972 x3
 * (carriages)** -- green carriages with red wheels -- spaced ~6450 apart, each
 * car carrying its own yaw because it follows a CURVED track. `waterfall_render`
 * draws 973/974, two loaded WAGONS, which appear in none of the 2833 MAME
 * frames captured here. So this whole path is a different, genuinely dead
 * object, and running it puts something on screen the machine never shows.
 *
 * The real carriages are NOT in MAME's CPU command list either -- scanned all
 * 12 dumps including the mode-0x8000 4-word entries, 108 distinct codes, none
 * in 965..980 -- so the master DSP expands them. That is register row 48/111's
 * gap, the largest one left in this project, and no amount of wiring here
 * reaches it. PROPCYCL_TRAIN=1 turns these wagons on anyway. */
int g_train = 0;
int g_train_chunk = -1;        /* PROPCYCL_TRAIN_CHUNK=<n> overrides the base */

void train_render(void)
{
  /* The ROM has a path table for EVERY course, so the train was meant to run
   * on all four, not just the one that also places static cars:
   *     course 0  id 0x44  table 0x36108      course 1  id 0x15  table 0x36130
   *     course 2  id 0x10  table 0x3611C      course 3  id 0x0A  table 0x3611C
   *
   * What the ROM does NOT give us is the base position, because that comes
   * from the record the (nonexistent) caller would pass. So:
   *
   *   course 0  base = chunk 9, from the player: the RED TRACK it should run
   *             on is the terrain the object picker shows as 1251 at the
   *             start of the level, and for course 0 the picker number is
   *             1242 + cell, so 1251 is cell 9 -- grid (gx 1, gz 1). With the
   *             table's own offset that puts the train at x 141288 with z
   *             sweeping 164685 -> 98125 over the 512-frame loop, i.e. running
   *             along the gx=1 column through cells 9 and 17, which is where
   *             the track lies.
   *   course 2  base = chunk 60: the only chunk on the only course whose
   *             scenery records place the 904+905 pair, i.e. the two cars this
   *             renderer draws.
   *   1 and 3   base = the world ORIGIN, so the train runs exactly where the
   *             course's own ROM table puts it and nothing is invented. No
   *             evidence has been seen for where they belong yet.
   *
   * The trigger ids are not scenery record types -- checked, none of the four
   * is referenced by its course's LOD tables, and their records carry
   * unrelated model bases (430, 305, 776, 775) -- so that route to a real
   * placement is closed too.
   *
   * PROPCYCL_TRAIN_CHUNK=<n> moves the base to chunk n for the current course,
   * which is how to put it somewhere specific without editing code. */
  static const uint32_t tbl[4] = { 0x36108, 0x36130, 0x3611C, 0x3611C };
  static const int      def[4] = { 9, 0, 60, 0 };
  /* HEIGHT BIAS. The path table's y is all the height the ROM gives, and on
   * course 0 that is 10405 -- measured against the terrain along the train's
   * own path (141288, ., 164685), the floor there is ~43500, so the ROM's
   * value sits the train ~33,000 units UNDER the ground. `g_camera_offset_y`
   * is not the culprit: it is `-param_2` from ROM 0x00ABF6 (`move.l $c(a6),d0
   * ; neg.l d0 ; move.l d0,$e01230.l`), which our C matches instruction for
   * instruction, and it measures 5. So this bias is mine, not the ROM's --
   * it is what puts the cars on the red track (the terrain the picker shows
   * as 1251 at the level start) instead of inside the hill under it.
   * PROPCYCL_TRAIN_Y=<n> overrides. */
  static const int      def_y[4] = { 33100, 0, 0, 0 };
  int course = (int)W[0x0E0C];
  int chunk;
  int32_t rec[3];
  if (!g_train || course < 0 || course > 3) return;
  chunk = (g_train_chunk >= 0 && g_train_chunk < 128) ? g_train_chunk : def[course];
  /* CAMERA-RELATIVE: dsp_cmd_set_camera does not subtract the camera itself,
   * so the record the ROM's caller passes must already be relative. Absolute
   * world coordinates here put the train 831,000 units out while the static
   * cars at the same chunk sat 48,000 off -- that difference is the camera. */
  rec[0] = 0;
  rec[1] = (int32_t)(chunk % 8) * 0x18000 - (int32_t)W[0x0CDC];
  rec[2] = (int32_t)(chunk / 8) * 0x18000 - (int32_t)W[0x0CE4];
  { extern int g_train_y;
    int bias = (g_train_y != (-2147483647-1)) ? g_train_y : def_y[course];
    g_train_ybias = bias; }
  waterfall_render(rec, tbl[course]);
}

/* ---- 4 rotation matrix functions ---- */

/* ---- DSP deps (3) ---- */

/* ---- error_report_and_return ---- */

uint64_t error_report_and_return(void)

{
  undefined2 in_D1w;
  uint64_t in_stack_00000000;
  
  W[0xC800] = in_D1w;
  error_code_save();
  return in_stack_00000000;
}

/* ---- render_town_with_rotation ---- */

undefined4 * render_town_with_rotation(int *param_1,undefined4 *param_2)

{
  /* ROM 0x016432 -- eight rotating pieces around [param] + (0x3c000,
   * 0x16000, 0x114000): model BE16 at 0x36414 + k*2, each a 0x8008 slot-3
   * frame (translate op from the BE32 pair at 0x3543C + model*8, rotate op
   * (0,1), (sin,cos) of fc<<5 + k*0x2000, (sin,cos) of fc<<7, flags 4) and an
   * 0x800a draw 0x1e00>>11 out along that angle. Each also becomes a dynamic
   * collision zone (x/y/z/heading at 0xE02920.. and the three size words from
   * the BE32 triple at 0x34BA0 + model*12). The transpile indexed all of it
   * as host `int *` and read the ROM tables natively or byte-wise. */
  intptr_t pp = (intptr_t)param_1;
  int32_t base[3], rel[3], k;
  int32_t n = (int32_t)W[0x291C];
  uint32_t d6 = (uint32_t)((int32_t)W[0x0C8C] << 7) & 0xfffc;
  int32_t *dl = (int32_t *)param_2;
  base[0] = sc_ptr32(pp, 0) + 0x3c000;
  base[1] = sc_ptr32(pp, 4) + 0x16000;
  base[2] = sc_ptr32(pp, 8) + 0x114000;
  rel[0] = base[0] - (int32_t)W[0x0CDC];
  rel[1] = base[1] - (int32_t)W[0x0CE0];
  rel[2] = base[2] - (int32_t)W[0x0CE4];
  W[0x291C] = n + 8;
  for (k = 0; k < 8; k++) {
    int32_t m = vrd16s(0x36414 + k * 2);
    int32_t a = ((int32_t)W[0x0C8C] << 5) + (k << 13);
    uint32_t idx = (uint16_t)a & 0xfffc;
    uint32_t o = (uint32_t)(n + k) * 4;
    int32_t dx = (int32_t)vrd16s(0x20B004 + idx) * 0x1e00 >> 11;
    int32_t dz = (int32_t)vrd16s(0x20B006 + idx) * 0x1e00 >> 11;
    int ok = (n + k) < 12;
    *dl++ = 0x8008; *dl++ = 3; *dl++ = 0;
    *dl++ = vrd32s(0x3543C + m * 8); *dl++ = vrd32s(0x35440 + m * 8);
    *dl++ = 0; *dl++ = 1; *dl++ = 0; *dl++ = 0x7fff;
    *dl++ = vrd16s(0x20B004 + idx); *dl++ = vrd16s(0x20B006 + idx);
    *dl++ = vrd16s(0x20B004 + d6);  *dl++ = vrd16s(0x20B006 + d6);
    *dl++ = 4; *dl++ = -1;
    *dl++ = 0x800a; *dl++ = m; *dl++ = 3;
    *dl++ = rel[0] + dx; *dl++ = rel[1]; *dl++ = rel[2] + dz;
    if (ok) {
      W[0x29B0 + o] = -a;
      W[0x2920 + o] = base[0] + dx;
      W[0x2980 + o] = base[2] + dz;
      W[0x29E0 + o] = vrd32s(0x34BA0 + m * 12);
      W[0x2950 + o] = base[1] - vrd32s(0x34BA4 + m * 12);
      W[0x2A10 + o] = vrd32s(0x34BA4 + m * 12);
      W[0x2A40 + o] = vrd32s(0x34BA8 + m * 12);
    }
  }
  return (undefined4 *)dl;
}

/* ---- error_code_save ---- */
void error_code_save(void) { W[0xD000] = W[0xC800]; }


#define g_audio_env_params W[0x3FE0]

/* Forward declarations for gameplay functions with non-standard return types */
uint16_t gameplay_sub17_run(void);
uint16_t gameplay_sub25_gameover_run(void);
uint32_t gameplay_sub27_run(void);
uint32_t highscore_table_insert(int param_1, intptr_t param_2);
uint32_t replay_record_frame(void);

uint32_t FUN_0000a532(void);
uint8_t * gameplay_timer_update(void);
int * render_flag_banner(int idx, uint32_t pos, int scale, uint32_t angle, int add, int *out_);

/* ========== GAMEPLAY LOOP (131 functions) ========== */

/* ---- check_game_state_change ---- */

uint32_t check_game_state_change(void)

{
  undefined2 uVar1;
  uint32_t uVar2;
  
  uVar2 = 0;
  if (W[0x0CBC] + -7 != 0) {
    uVar1 = (undefined2)((uint32_t)(W[0x0CBC] + -7) >> 0x10);
    if (((W[0x2B5E] & 0x100) == 0) ||
       (uVar1 = (undefined2)((uint32_t)(W[0x0CBC] + -1) >> 0x10), W[0x0CBC] + -1 != 0)) {
      if ((W[0x2B80] & 8) == 0) {
        return (((uint32_t)(uVar1) << 16) | (uint16_t)(W[0x2B80])) & 0xffff0008;
      }
      W[0x0E30] = 0;
    }
    else {
      W[0x0E30] = 1;
    }
    uVar2 = 6;
    W[0x0CBC] = 6;
  }
  return uVar2;
}

/* ---- coin_credit_init ---- */

void coin_credit_init(void)

{
  W[0x2C12] = 0;
  W[0x2C0E] = 0;
  W[0x2C10] = 0;
  g_credit_countdown = 0x16;
  return;
}

/* ---- coin_credit_update ---- */

undefined4 coin_credit_update(void)

{
  undefined4 uVar1;
  undefined2 uVar2;
  undefined2 uVar3;
  
  uVar1 = 0;
  if ((((W[0x0CBC] != 6) && (uVar1 = 0, W[0x0CBC] != 7)) && (uVar1 = 0, W[0x0CBC] != 4)) &&
     (uVar1 = 0, W[0x0CBC] != 5)) {
    if ((W[0x2C12] != 0) && ((W[0x2B3A] & 0x100) != 0)) {
      W[0x0CAC] = 0;
      W[0x0CBC] = 2;
      W[0x2C12] = 0;
    }
    if (((W[0x2B82] & 1) != 0) || ((W[0x2B82] & 4) != 0)) {
      W[0x2C0E] = W[0x2C0E] + 1;
      if ((W[0x2B82] & 1) != 0) {
        g_sys.commsram[0x7D26] = 1;
        audio_hle_trigger(AUDIO_SFX_COIN);
      }
      sound_play(0x15);
      if (((g_time_limit <= W[0x2C0E]) || (W16(0x3FF4) != 0)) && (W[0x2C12] != 0)) {
        if (W16(0x3FF4) == 0) {
          W[0x2C0E] = W[0x2C0E] - g_time_limit;
        }
        W[0x0CAC] = 0;
        W[0x0CBC] = 2;
        W[0x2C12] = 0;
      }
    }
    if ((((W[0x2BA6] & 1) != 0) && ((g_time_limit <= W[0x2C0E] || (W16(0x3FF4) != 0)))
        ) && (W[0x2C12] != 0)) {
      if (W16(0x3FF4) == 0) {
        W[0x2C0E] = W[0x2C0E] - g_time_limit;
      }
      W[0x0CAC] = 0;
      W[0x0CBC] = 2;
      W[0x2C12] = 0;
    }
    if (((W[0x2C0E] != 0) && (W[0x2C12] != 0)) &&
       ((W16(0x3FF4) == 0 && (W[0x0CBC] != 0xd)))) {
      W[0x0CAC] = 0;
      W[0x0CBC] = 0xc;
    }
    coin_status_hud_draw();
    uVar1 = 0xc;
    if (((W[0x0CBC] != 0xc) && (uVar1 = 0xd, W[0x0CBC] != 0xd)) && (W[0x2C12] != 0)) {
      if ((W[0x0C98] & 0x20) == 0) {
        if (W16(0x3FF4) == 0) {
          /* base tile code 0x360 and palette 3, from the M68K at 0x028C9E
           * (`move.w #$360,-(A7)` -- a WORD push, which is why the frame pop
           * is 0x12 not 0x14). This is the INSERT COIN banner: MAME's tilemap
           * carries 3360 3361 ... at row 22 col 11, i.e. palette 3, codes
           * 0x360.., which is where cgram_load_tile_block(0x19, 0x360) put
           * block 0x19. Both arguments were lost in decompilation, so every
           * banner drew tile code 0 with palette 0. */
          text_draw_rect_blink(0x19, 0xb, g_credit_countdown, 0x360, 3);
          /* The COIN-COUNT DIGIT of "INSERT n COIN(S)". At 0x028CB8 the base
           * is table-driven:
           *   move.w ($c0,A2),D0 ; lea $38c.w,A0 ; lea (A0,D0.w*4),A0
           * with A2 = 0xE03F30, so ($c0,A2) is W[0x3FF0] and the base is
           * 0x38c + W[0x3FF0]*4. W[0x3FF0] is COINS PER CREDIT -- despite the
           * `g_time_limit` name this file's macro gives it, coin_credit_update
           * subtracts it from the coin count W[0x2C0E] when granting a credit
           * (see ~line 2360), and settings_time_limit_defaults defaults it to
           * 4. With 4 the base is 0x39C, which is exactly the tile MAME's
           * tilemap carries at row 22 col 19. */
          text_draw_rect_blink(0x20, 0x13, (int)g_credit_countdown,
                               0x38c + (int)g_time_limit * 4, 3); uVar1 = 0;
        }
        else {
          /* 0x028C7C: `move.w #$2f0,-(A7)`, palette 3 -- block 0x1b is loaded
           * at code 0x2f0 by cgram_load_tile_block(0x1b, 0x2f0). */
          text_draw_rect_blink(0x1b, 5, g_credit_countdown, 0x2f0, 3); uVar1 = 0;
        }
      }
      else {
        if (W16(0x3FF4) == 0) {
          text_draw_rect_solid(0x19,0xb,g_credit_countdown);
          uVar3 = 0x13;
          uVar2 = 0x20;
        }
        else {
          uVar3 = 5;
          uVar2 = 0x1b;
        }
        text_draw_rect_solid(uVar2,uVar3,g_credit_countdown); uVar1 = 0;
      }
    }
  }
  return uVar1;
}

/* ---- countdown_balloon_ring_draw ---- */

int countdown_balloon_ring_draw(int *param_1,int param_2)

{
  int iVar1;
  int iVar2;
  uint32_t uVar3;
  int iVar4;
  int iVar5;
  int iVar6;
  int iVar7;
  int iVar8;
  uint32_t uVar9;
  int iVar10;
  int iVar11;
  int iVar12;
  int iVar13;
  int *piVar14;
  
  dsp_cmd_emit_object_mode_8002();
  iVar4 = *param_1 - W[0x0CDC];
  iVar5 = ((int32_t*)(intptr_t)param_1)[1] - W[0x0CE0];
  iVar6 = ((int32_t*)(intptr_t)param_1)[2] - W[0x0CE4];
  iVar10 = (int)(vrd16s(0x20B004 + (-W[0x0CE8] & 0xfffcU)));
  iVar11 = (int)(vrd16s(0x20B006 + (-W[0x0CE8] & 0xfffcU)));
  iVar12 = (int)(vrd16s(0x20B004 + (-W[0x0CEC] & 0xfffcU)));
  iVar13 = (int)(vrd16s(0x20B006 + (-W[0x0CEC] & 0xfffcU)));
  uVar9 = 0;
  do {
    uVar3 = *(int *)(param_2 + uVar9 * 0x1c) * (int)(short)W16(0x172EA) + uVar9 * 0x4000 & 0xfffc;
    iVar7 = *(int *)(param_2 + 0x18 + uVar9 * 0x1c) *
            (int)(vrd16s(0x20B004 + (*(int *)(param_2 + 0x14 + uVar9 * 0x1c) * (int)(short)W16(0x172EA) & 0xfffcU))) >> 0xf;
    iVar1 = iVar4 + ((*(int *)(param_2 + 4 + uVar9 * 0x1c) + iVar7) *
                     (int)(vrd16s(0x20B006 + uVar3)) >> 0xf);
    iVar2 = (*(int *)(param_2 + 0xc + uVar9 * 0x1c) *
             (int)(vrd16s(0x20B004 + (*(int *)(param_2 + 8 + uVar9 * 0x1c) * (int)(short)W16(0x172EA) & 0xfffcU))) >> 0xf) + *(int *)(param_2 + 0x10 + uVar9 * 0x1c) + iVar5;
    iVar7 = iVar6 - ((*(int *)(param_2 + 4 + uVar9 * 0x1c) + iVar7) *
                     (int)(vrd16s(0x20B004 + uVar3)) >> 0xf);
    if ((W16(0x172EA) & 0x2c) == 0) {
      iVar8 = 0x32a;
    }
    else {
      iVar8 = ((int)(short)W16(0x172EA) >> 3 & 3U) + 0x326;
    }
    *(int32_t*)W[0x0CA4] = iVar8;
    ((int32_t*)W[0x0CA4])[1] = iVar1;
    ((int32_t*)W[0x0CA4])[2] = iVar2;
    ((int32_t*)W[0x0CA4])[3] = iVar7;
    ((int32_t*)W[0x0CA4])[4] = iVar10;
    ((int32_t*)W[0x0CA4])[5] = iVar11;
    ((int32_t*)W[0x0CA4])[6] = iVar12;
    ((int32_t*)W[0x0CA4])[7] = iVar13;
    ((int32_t*)W[0x0CA4])[8] = 0;
    ((int32_t*)W[0x0CA4])[9] = 0x7fff;
    piVar14 = (intptr_t)((int32_t*)W[0x0CA4] + 0xb);
    ((int32_t*)W[0x0CA4])[10] = 0;
    if (((int)(short)W16(0x172EA) >> 4 & 3U) == (uVar9 & 3)) {
      *piVar14 = (int32_t)vrd32(0x391F8 + (((int)(short)W16(0x172EA) >> 1 & 7U) * 4));
      ((int32_t*)W[0x0CA4])[0xc] = iVar1;
      ((int32_t*)W[0x0CA4])[0xd] = iVar2;
      ((int32_t*)W[0x0CA4])[0xe] = iVar7;
      ((int32_t*)W[0x0CA4])[0xf] = iVar10;
      ((int32_t*)W[0x0CA4])[0x10] = iVar11;
      ((int32_t*)W[0x0CA4])[0x11] = iVar12;
      ((int32_t*)W[0x0CA4])[0x12] = iVar13;
      ((int32_t*)W[0x0CA4])[0x13] = 0;
      ((int32_t*)W[0x0CA4])[0x14] = 0x7fff;
      piVar14 = (intptr_t)((int32_t*)W[0x0CA4] + 0x16);
      ((int32_t*)W[0x0CA4])[0x15] = 0;
    }
    uVar3 = uVar9 + 1;
    iVar1 = uVar9 - 3;
    uVar9 = uVar3;
    W[0x0CA4] = piVar14;
  } while ((int)uVar3 < 4);
  return iVar1;
}

/* ---- countdown_camera_set ---- */

void countdown_camera_set(undefined4 *param_1,int param_2)

{
  undefined4 *puVar1;
  short sVar2;
  undefined4 *puVar3;
  
  sVar2 = 0;
  puVar3 = &W[0x0CDC];
  do {
    puVar1 = param_1 + 1;
    *puVar3 = *param_1;
    sVar2 = sVar2 + 1;
    puVar3 = puVar3 + 1;
    param_1 = puVar1;
  } while (sVar2 < 6);
  if (param_2 != 0) {
    W16_SET(0x17360, (undefined2)*puVar1);
  }
  return;
}

/* ---- countdown_cloud_ring_draw ---- */

void countdown_cloud_ring_draw(int param_1)

{
  short sVar1;
  short sVar2;
  short sVar3;
  short sVar4;
  uint32_t uVar5;
  int iVar6;
  undefined4 *puVar7;
  undefined4 *puVar8;
  int *piVar9;
  
  dsp_cmd_emit_object_mode_8002();
  uVar5 = -W[0x0CE8] - 0x4000U & 0xfffc;
  sVar1 = (vrd16s(0x20B004 + uVar5));
  sVar2 = (vrd16s(0x20B006 + uVar5));
  sVar3 = (vrd16s(0x20B004 + (-W[0x0CEC] & 0xfffcU)));
  sVar4 = (vrd16s(0x20B006 + (-W[0x0CEC] & 0xfffcU)));
  puVar8 = W[0x0CA4];
  for (piVar9 = &R[0x39228]; -1 < *piVar9; piVar9 = piVar9 + 5) {
    if ((*piVar9 <= param_1) && (param_1 <= ((int32_t*)(intptr_t)piVar9)[1])) {
      *puVar8 = 0x8010;
      puVar8[1] = 3;
      W[0x4704] = 0x7000;
      puVar8[2] = 0x7000;
      puVar8[3] = 2;
      puVar8[4] = (int)(*(int16_t*)((intptr_t)piVar9 >> 2 & 3U) * 2 + 0x39344);
      puVar8[5] = 0xffffffff;
      puVar8[6] = 0x38a;
      uVar5 = ((int32_t*)(intptr_t)piVar9)[4];
      puVar8[7] = (((vrd16s(0x20B002 + (uVar5 & 0xfffc))) * 0x1200 >> 0xb) - W[0x0CDC])
                  + 0x3c000;
      iVar6 = math_lerp_int(((int32_t*)(intptr_t)piVar9)[2],((int32_t*)(intptr_t)piVar9)[3],*piVar9,((int32_t*)(intptr_t)piVar9)[1],param_1);
      puVar8[8] = (iVar6 - W[0x0CE0]) + 0x18600;
      puVar8[9] = (((vrd16s(0x20B004 + (uVar5 & 0xfffc))) * 0x1200 >> 0xb) -
                  W[0x0CE4]) + 0x114000;
      puVar8[10] = (int)sVar1;
      puVar8[0xb] = (int)sVar2;
      puVar8[0xc] = (int)sVar3;
      puVar8[0xd] = (int)sVar4;
      iVar6 = ((uint32_t)piVar9 & 0xf) * 0x400 + 0x3000;
      puVar8[0xe] = iVar6;
      puVar7 = puVar8 + 0x10;
      puVar8[0xf] = iVar6;
      puVar8 = puVar8 + 0x11;
      *puVar7 = 0;
    }
  }
  W[0x0CA4] = puVar8;
  return;
}

/* ---- countdown_ferris_wheel_draw ---- */

int countdown_ferris_wheel_draw(int param_1,int param_2,int param_3)

{
  undefined4 *puVar1;
  int iVar2;
  int iVar3;
  uint32_t uVar4;
  int iVar5;
  uint32_t uVar6;
  undefined4 *puVar7;
  
  iVar5 = 0;
  dsp_cmd_emit_object_mode_8002();
  puVar1 = W[0x0CA4];
  puVar7 = (int32_t*)W[0x0CA4] + 1;
  *(int32_t*)W[0x0CA4] = 0x8010;
  *puVar7 = 3;
  W[0x4704] = (int32_t)0xffff0000;
  puVar1[2] = 0xffff0000;
  puVar1[3] = 0xffffffff;
  uVar4 = 0;
  puVar1 = puVar1 + 4;
  do {
    puVar7 = puVar1;
    iVar3 = param_3 + (((int)uVar4 >> 1) + (uVar4 & 1) * 4) * -10;
    if (iVar3 == 6) {
      if (uVar4 == 7) {
        iVar5 = 1;
      }
      else {
        iVar5 = uVar4 + 1;
      }
    }
    iVar2 = param_1;
    if (5 < iVar3) {
      iVar2 = param_1 + ((iVar3 + -6) * (param_2 - param_1)) / 0xae;
    }
    *puVar7 = 0x8008;
    puVar7[1] = 3;
    puVar7[2] = 0;
    puVar7[3] = 0;
    puVar7[4] = 0;
    puVar7[5] = 0xffffa600;
    puVar7[6] = 1;
    if (iVar3 < 0x24) {
      uVar6 = 0x4000;
    }
    else {
      uVar6 = math_lerp_int(0,0x8000,0x24,0x168,iVar3);
      uVar6 = ((int)(vrd16s(0x20B006 + (uVar6 & 0xfffc))) *
              (int)(*(int16_t*)(uVar4 * 2 + 0x39218))) / -0x8000 + (int)(*(int16_t*)(uVar4 * 2 + 0x39218))
              + 0x4000;
    }
    puVar7[7] = vrd16s(0x20B002 + (uVar6 & 0xfffc));
    puVar7[8] = vrd16s(0x20B004 + (uVar6 & 0xfffc));
    iVar3 = uVar4 * 0x10000;
    if (iVar3 < 0) {
      iVar3 = iVar3 + 7;
    }
    uVar6 = (iVar3 >> 3) + 0x8000U & 0xfffc;
    puVar7[9] = vrd16s(0x20B002 + uVar6);
    puVar7[10] = vrd16s(0x20B004 + uVar6);
    puVar7[0xb] = 0;
    puVar7[0xc] = 0x7fff;
    puVar7[0xd] = 4;
    puVar7[0xe] = 0xffffffff;
    puVar7[0xf] = 0x800a;
    puVar7[0x10] = (int)(short)(&R[0x36414])[uVar4];
    puVar7[0x11] = 3;
    iVar3 = uVar4 * 0x10000;
    if (iVar3 < 0) {
      iVar3 = iVar3 + 7;
    }
    uVar6 = (iVar3 >> 3) + 0x8000U & 0xfffc;
    puVar7[0x12] = ((((int)(vrd16s(0x20B004 + uVar6)) << 0xc) >> 0xb) - W[0x0CDC])
                   + 0x3c000;
    puVar7[0x13] = (iVar2 - W[0x0CE0]) + 0x18600;
    puVar7[0x14] = ((((int)(vrd16s(0x20B006 + uVar6)) << 0xc) >> 0xb) - W[0x0CE4])
                   + 0x114000;
    uVar4 = uVar4 + 1;
    puVar1 = puVar7 + 0x15;
  } while ((int)uVar4 < 8);
  puVar7[0x15] = 0x8010;
  puVar7[0x16] = 0xffffffff;
  W[0x4704] = 0;
  W[0x0CA4] = puVar7 + 0x17;
  return iVar5;
}

/* ---- countdown_stage_flythrough ---- */

void countdown_stage_flythrough(int param_1)

{
  uint32_t uVar1;
  uint16_t uVar6;
  int iVar2;
  undefined4 *puVar3;
  int iVar4;
  undefined4 uVar5;
  int iVar7;
  undefined4 *puVar8;
  int local_38 [8];
  int local_18 [4];
  uint8_t *local_8;
  
  local_38[0] = 0;
  local_38[2] = 0;
  local_38[3] = 0x615;
  local_38[1] = R[0x39380] + (param_1 * ((int)(int32_t)rom_nr32(0x39384) - R[0x39380])) / 0x21c;
  iVar7 = 0;
  do {
    local_18[iVar7] = local_38[iVar7];
    local_38[iVar7 + 4] = local_38[iVar7];
    iVar7 = iVar7 + 1;
  } while (iVar7 < 4);
  if (param_1 < 300) {
    uVar1 = (param_1 << 0xe) / 300 & 0xfffc;
    local_38[6] = ((int)(vrd16s(0x20B004 + uVar1)) << 0xc) / -0x8000 + 0x1000 +
                  local_38[6];
    local_18[0] = (int)((uint32_t)(vrd16s(0x20B004 + uVar1)) << 0x10) / -0x8000 + 0x10000 +
                  local_18[0];
  }
  if (W16(0x172EC) == 0) {
    iVar7 = 0;
    do {
      W[0x17294 + (iVar7)] = (int32_t)rom_nr32(0x3934C + (iVar7) * 4);
      iVar7 = iVar7 + 1;
    } while (iVar7 < 7);
  }
  W[0x17294] = (int32_t)rom_nr32(0x3934C);
  W[0x1729C] = (int32_t)rom_nr32(0x39354);
  W[0x172A4] = (int32_t)rom_nr32(0x3935C);
  W[0x17298] = R[0x39368];
  uVar6 = math_atan2((int)(int32_t)rom_nr32(0x39370) - (int)(int32_t)rom_nr32(0x3934C),
                     (int)(int32_t)rom_nr32(0x39378) - (int)(int32_t)rom_nr32(0x39354));
  iVar7 = math_atan2(R[0x39374] + (local_38[1] - W[0x17298]) >> 2,
                     (((int)(int32_t)rom_nr32(0x39378) - (int)W[0x1729C]) * 0x100) /
                     ((int)(vrd16s(0x20B006 + ((uint32_t)(uVar6 & 0xfffc)))) >> 5));
  W[0x172A0] = -iVar7;
  countdown_camera_set(&W[0x17294],1);
  ending_sky_draw_alt();
  W[0x46F8] = local_38;
  scene_objects_draw_list(&R[0x37A7A],W[0x46F8]);
  W[0x46FC] = local_38 + 4;
  scene_objects_draw_list(&R[0x37A86],W[0x46FC]);
  W[0x4700] = local_18;
  scene_objects_draw_list(&R[0x37A94],W[0x4700]);
  W[0x291C] = 0;
  W16_SET(0x4708, 0xffff);   /* ROM 0x0307D8: move.w #$ffff,$e04708 */
  render_tower_complex();
  render_bridge_structure();
  iVar7 = 0;
  do {
    W[0x17294 + (iVar7)] = (int32_t)rom_nr32(0x393D0 + (iVar7) * 4) + local_38[iVar7];
    local_38[iVar7 + 4] = (int)((int32_t)rom_nr32(0x393DC + (iVar7) * 4) + local_38[iVar7 + 4]);
    local_18[iVar7] = (int)((int32_t)rom_nr32(0x393E8 + (iVar7) * 4) + local_18[iVar7]);
    iVar7 = iVar7 + 1;
  } while (iVar7 < 3);
  countdown_balloon_ring_draw(&W[0x17294],&R[0x39188]);
  countdown_balloon_ring_draw(local_38 + 4,&R[0x39188]);
  countdown_balloon_ring_draw(local_18,&R[0x39188]);
  local_38[0] = local_38[0] - W[0x0CDC];
  local_38[1] = local_38[1] - W[0x0CE0];
  local_38[2] = local_38[2] - W[0x0CE4];
  dsp_cmd_emit_object_mode_8000();
  puVar8 = W[0x0CA4];
  puVar3 = W[0x0CA4];
  if ((-0x19200 < local_38[1]) && (W[0x4704] != -0x7000)) {
    puVar3 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = 0x8010;
    *puVar3 = 3;
    W[0x4704] = -0x7000;
    puVar8[2] = 0xffff9000;
    puVar3 = puVar8 + 4;
    puVar8[3] = 0xffffffff;
  }
  *puVar3 = 500;
  puVar3[1] = local_38[0];
  puVar3[2] = local_38[1];
  puVar8 = puVar3 + 4;
  puVar3[3] = local_38[2];
  if (W[0x4704] != -0x10000) {
    *puVar8 = 0x8010;
    puVar3[5] = 3;
    W[0x4704] = -0x10000;
    puVar3[6] = 0xffff0000;
    puVar8 = puVar3 + 8;
    puVar3[7] = 0xffffffff;
  }
  W[0x169E8] = 0x8002;
  *puVar8 = 0x8002;
  puVar3 = puVar8 + 2;
  puVar8[1] = 0;
  if (param_1 < 0xf1) {
    local_8 = (uint8_t *)math_lerp_int((int32_t)rom_nr32(0x39394),(int32_t)rom_nr32(0x39388),0,0xf0,param_1);
    iVar2 = math_lerp_int(R[0x39398],R[0x3938C],0,0xf0,param_1);
    iVar7 = math_lerp_int(R[0x3939C],R[0x39390],0,0xf0,param_1);
  }
  else {
    local_8 = (int32_t)rom_nr32(0x39388);
    iVar7 = R[0x39390];
    iVar2 = R[0x3938C];
  }
  local_38[1] = local_38[1] - iVar7;
  if (param_1 < 0xf1) {
    iVar7 = 0;
    do {
      iVar4 = math_lerp_int((int)(short)(&R[0x393C0])[iVar7],0,0,0xf0,param_1);
      local_38[1] = iVar4 + local_38[1];
      uVar5 = math_lerp_int((int)(short)(&R[0x393B0])[iVar7],0,0,0xf0,param_1);
      iVar4 = math_lerp_int((int)(short)(&R[0x393A0])[iVar7],0,0,0xf0,param_1);
      puVar3 = (undefined4 *)render_flag_banner(iVar7,local_38,local_8,iVar2 + iVar4, 0, NULL);
      iVar7 = iVar7 + 1;
    } while (iVar7 < 8);
  }
  else {
    iVar7 = 0;
    do {
      puVar3 = (undefined4 *)render_flag_banner(iVar7,local_38,local_8,iVar2,0,puVar3);
      iVar7 = iVar7 + 1;
    } while (iVar7 < 8);
  }
  *puVar3 = 0x8010;
  puVar3[1] = 0xffffffff;
  W[0x4704] = 0;
  W[0x0CA4] = puVar3 + 2;
  return;
}

/* ---- countdown_stage_town_overview ---- */

void countdown_stage_town_overview(uint32_t param_1)

{
  int iVar1;
  int iVar2;
  uint32_t uVar3;
  int iVar4;
  uint8_t *puVar5;
  int iVar6;
  uint8_t **ppuVar7;
  int *piVar8;
  int *piVar9;
  undefined2 uVar10;
  int local_2c [6];
  undefined4 local_14;
  
  uVar10 = (undefined2)param_1;
  if ((int)param_1 < 0x1fe) {
    iVar6 = 0;
    do {
      uVar3 = math_lerp_int(0,0x4000,0,0x21c,uVar10);
      iVar4 = (&R[0x3971C])[iVar6] - (int)(int32_t)rom_nr32(0x39700 + (iVar6) * 4);
      if (iVar4 < 0) {
        iVar4 = iVar4 + 0x1f;
      }
      local_2c[iVar6] =
           (int)((int32_t)rom_nr32(0x39700 + (iVar6) * 4) +
                ((int)(vrd16s(0x20B004 + (uVar3 & 0xfffc))) * (iVar4 >> 5) >> 10));
      iVar6 = iVar6 + 1;
    } while (iVar6 < 6);
  }
  else {
    iVar6 = 0;
    do {
      uVar3 = math_lerp_int(0,0,0,0,0);
      iVar4 = (&R[0x3971C])[iVar6] - (int)(int32_t)rom_nr32(0x39700 + (iVar6) * 4);
      if (iVar4 < 0) {
        iVar4 = iVar4 + 7;
      }
      iVar4 = math_lerp_int(0, 0, 0, 0, 0); /* simplified multi-line */
      /* (args removed) */
      /* simplified broken expression */
      local_2c[iVar6] = iVar4;
      iVar6 = iVar6 + 1;
    } while (iVar6 < 6);
  }
  local_14 = math_lerp_int(R[0x39718],R[0x39734],0,0x21c,uVar10);
  countdown_camera_set(local_2c,1);
  if (param_1 == 0) {
    W[0x0E0C] = 0;
    terrain_chunk_render(0);
  }
  terrain_chunk_visibility
            (W[0x0CDC],W[0x0CE0],W[0x0CE4],W[0x0CEC]);
  ending_sky_draw_alt();
  dsp_cmd_emit_object_mode_8000();
  *(int32_t*)W[0x0CA4] = R[0x390C8] + ((int)param_1 >> 1 & 0xfU) * 4 + 0x146;
  ((int32_t*)W[0x0CA4])[1] = -W[0x0CDC];
  piVar9 = (intptr_t)((int32_t*)W[0x0CA4] + 3);
  ((int32_t*)W[0x0CA4])[2] = 0xa000 - W[0x0CE0];
  W[0x0CA4] = W[0x0CA4] + 4 * 4;
  *piVar9 = -W[0x0CE4];
  W[0x12BC] = 1;
  if (param_1 == 0) {
    FUN_000268ce(0xE0B704,0x3949C);   /* ROM 0x030BB8: pea $3949c.l */
    ending_player_reset();
  }
  else if ((int)param_1 < 0x1ff) {
    scene_interpolation_evaluate(0xE0B704, (int)param_1);   /* ROM 0x030BDC: move.w d3,-(a7) */
  }
  ending_frame_render();
  W[0x16668] = (int)(vrd16s(0x20B004 + (-W[0x0CEC] & 0xfffcU)));
  W[0x1666C] = (int)(vrd16s(0x20B006 + (-W[0x0CEC] & 0xfffcU)));
  ending_object_strip_draw_anim(0x394A4,uVar10,(int32_t)0xffffe000);   /* ROM 0x030C22: pea $e000.w ; pea $394a4.l */
  dsp_cmd_emit_object_mode_8002();
  *(int32_t*)W[0x0CA4] = 0x8010;
  piVar9 = (intptr_t)((int32_t*)W[0x0CA4] + 2);
  ((int32_t*)W[0x0CA4])[1] = -1;
  W[0x4704] = 0;
  iVar6 = 0;
  for (ppuVar7 = (int32_t*)&g_sys.rom[0x39738]; -1 < (int)*ppuVar7; ppuVar7 = ppuVar7 + 4) {
    if (param_1 == 0) {
      W[0x17294 + (iVar6)] = 0;
    }
    if (-1 < (int)W[0x17294 + (iVar6)]) {
      if (W[0x17294 + (iVar6)] == 0) {
        puVar5 = ppuVar7[3] + ((int)param_1 >> 3 & 3);
      }
      else {
        puVar5 = (uint8_t *)((W[0x17294 + (iVar6)] + -1 >> 1) + 0x2f8);
      }
      *piVar9 = (int)puVar5;
      ((int32_t*)(intptr_t)piVar9)[1] = (int)*ppuVar7 - W[0x0CDC];
      ((int32_t*)(intptr_t)piVar9)[2] = (int)(ppuVar7[1] +
                       (((int)(int16_t)vrd16s(0x20B004 + ((param_1 & 0xff) * 0x80)) << 0xb) / 0x8000 -
                       W[0x0CE0]));
      ((int32_t*)(intptr_t)piVar9)[3] = (int)ppuVar7[2] - W[0x0CE4];
      ((int32_t*)(intptr_t)piVar9)[4] = 0;
      ((int32_t*)(intptr_t)piVar9)[5] = 0x7fff;
      ((int32_t*)(intptr_t)piVar9)[6] = W[0x16668];
      ((int32_t*)(intptr_t)piVar9)[7] = W[0x1666C];
      ((int32_t*)(intptr_t)piVar9)[8] = 0;
      piVar8 = piVar9 + 10;
      ((int32_t*)(intptr_t)piVar9)[9] = 0x7fff;
      piVar9 = piVar9 + 0xb;
      *piVar8 = 0;
    }
    if ((0 < (int)W[0x17294 + (iVar6)]) &&
       (iVar4 = W[0x17294 + (iVar6)], W[0x17294 + (iVar6)] = iVar4 + 1, 0x10 < iVar4 + 1)) {
      W[0x17294 + (iVar6)] = (int32_t)0xffffffff;
    }
    if ((W[0x17294 + (iVar6)] == 0) &&
       (iVar4 = (int)*ppuVar7 - W[0xB728] >> 7, iVar1 = (int)ppuVar7[1] - W[0xB72C] >> 7,
       iVar2 = (int)ppuVar7[2] - W[0xB730] >> 7,
       iVar2 * iVar2 + iVar1 * iVar1 + iVar4 * iVar4 < 0x736)) {
      W[0x17294 + (iVar6)] = 1;
    }
    iVar6 = iVar6 + 1;
  }
  W[0x0CA4] = piVar9;
  return;
}

/* ---- countdown_sub0_terrain_intro ---- */

void countdown_sub0_terrain_intro(void)

{
  W16_SET(0x172EC, W16(0x172EA));
  if (W16(0x172EA) == 0) {
    terrain_chunk_render(0);
  }
  if (W16(0x172EC) == 0x30) {
    sound_play_p2(0, 0);
  }
  if ((W16(0x172EC) < 0x41) && (W[0xEB16] = W16(0x172EC) * -4 + 0xff, W[0xEB16] < 0)) {
    W[0xEB16] = 0;
  }
  countdown_camera_set(0x97bc,0);
  W16_SET(0x17360, math_lerp_int(0xc00,0x800,0,0x10e,W16(0x172EC)));
  ending_sky_draw_alt();
  terrain_chunk_visibility(W[0x0CDC],W[0x0CE0],W[0x0CE4],W[0x0CEC]);
  W[0x46F8] = &R[0x39108];
  scene_objects_draw_list(&R[0x37A7A],&R[0x39108]);
  countdown_ferris_wheel_draw(R[0x3910C],R[0x3910C],0);
  dsp_cmd_emit_object_mode_8000();
  W[0x17294] = R[0x37AB4] - W[0x0CDC];
  W[0x17298] = R[0x37AB8] - W[0x0CE0];
  W[0x1729C] = R[0x37ABC] - W[0x0CE4];
  *(int32_t*)W[0x0CA4] = R[0x390C8] + ((int)W16(0x172EC) >> 1 & 0xfU) * 4 + 0x146;
  ((int32_t*)W[0x0CA4])[1] = W[0x17294];
  ((int32_t*)W[0x0CA4])[2] = W[0x17298];
  ((int32_t*)W[0x0CA4])[3] = W[0x1729C];
  ((int32_t*)W[0x0CA4])[4] = 0x8010;
  ((int32_t*)W[0x0CA4])[5] = 3;
  W[0x4704] = 0x3000;
  ((int32_t*)W[0x0CA4])[6] = 0x3000;
  ((int32_t*)W[0x0CA4])[7] = -1;
  ((int32_t*)W[0x0CA4])[8] = R[0x390C8] + ((int)W16(0x172EC) >> 1 & 0xfU) * 4 + 0x164;
  ((int32_t*)W[0x0CA4])[9] = W[0x17294];
  ((int32_t*)W[0x0CA4])[10] = W[0x17298];
  ((int32_t*)W[0x0CA4])[0xb] = W[0x1729C];
  W[0x0CA4] = W[0x0CA4] + 0xc * 4;
  return;
}

/* ---- countdown_sub1_flyover ---- */

undefined4 countdown_sub1_flyover(void)

{
  undefined4 uVar1;
  int iVar2;
  undefined4 *puVar3;
  
  W16_SET(0x172EC, W16(0x172EA) + -0x1e0);
  if (W16(0x172EC) == 0) {
    sound_play_p2(0x00230091, 0x14);
    iVar2 = 0;
    do {
      W[0x17294 + (iVar2)] = (&R[0x39108])[iVar2];
      W[0x172A4 + (iVar2)] = (int32_t)rom_nr32(0x397F4 + (iVar2) * 4) + (&R[0x39108])[iVar2];
      iVar2 = iVar2 + 1;
    } while (iVar2 < 4);
    W16_SET(0x172FC, 0);
  }
  countdown_camera_set(0x97d8,1);
  W[0x17298] = math_lerp_int(0x3e108,0x44108,0,0xf0,W16(0x172EC));
  W[0x172A8] = (int32_t)rom_nr32(0x397F8) + W[0x17298];
  if (0 < W[0xEB16]) {
    if ((W16(0x172FC) == 0) || (W[0xEB16] == 0xff)) {
      W[0xEB16] = W[0xEB16] - W16(0x172FA);
      if (W[0xEB16] < 0) {
        W[0xEB16] = 0;
      }
    }
    else {
      fog_set_from_table(0);
      W16_SET(0x172FA, 0x40);
    }
  }
  for (iVar2 = 0; -1 < (short)(&R[0x39804])[iVar2]; iVar2 = iVar2 + 1) {
    if ((&R[0x39804])[iVar2] == W16(0x172EC)) {
      fog_set_from_table(7);
      W16_SET(0x172FA, 1);
    }
  }
  ending_sky_draw_alt();
  terrain_chunk_visibility(W[0x0CDC],W[0x0CE0],W[0x0CE4],W[0x0CEC]);
  W[0x46F8] = (intptr_t)&W[0x17294];
  scene_objects_draw_list(&R[0x37A7A],&W[0x17294]);
  countdown_balloon_ring_draw(&W[0x172A4],&R[0x39118]);
  countdown_ferris_wheel_draw(R[0x3910C],0x41108,W16(0x172EC) + 0x3c);
  countdown_cloud_ring_draw((int)W16(0x172EC));
  dsp_cmd_emit_object_mode_8000();
  if (W[0x4704] != -0x6000) {
    *(int32_t*)W[0x0CA4] = 0x8010;
    ((int32_t*)W[0x0CA4])[1] = 3;
    W[0x4704] = -0x6000;
    puVar3 = (int32_t*)W[0x0CA4] + 3;
    ((int32_t*)W[0x0CA4])[2] = 0xffffa000;
    W[0x0CA4] = W[0x0CA4] + 4 * 4;
    *puVar3 = 0xffffffff;
  }
  *(int32_t*)W[0x0CA4] = 500;
  ((int32_t*)W[0x0CA4])[1] = W[0x17294] - W[0x0CDC];
  ((int32_t*)W[0x0CA4])[2] = W[0x17298] - W[0x0CE0];
  ((int32_t*)W[0x0CA4])[3] = W[0x1729C] - W[0x0CE4];
  W[0x172B4] = R[0x37AB4] - W[0x0CDC];
  W[0x172B8] = R[0x37AB8] - W[0x0CE0];
  W[0x172BC] = R[0x37ABC] - W[0x0CE4];
  ((int32_t*)W[0x0CA4])[4] = R[0x390C8] + ((int)W16(0x172EC) >> 1 & 0xfU) * 4 + 0x146;
  ((int32_t*)W[0x0CA4])[5] = W[0x172B4];
  ((int32_t*)W[0x0CA4])[6] = W[0x172B8];
  ((int32_t*)W[0x0CA4])[7] = W[0x172BC];
  uVar1 = 0xfffffff4;
  W[0x0CA4] = W[0x0CA4] + 8 * 4;
  if (1 < W16(0x172EC)) {
    uVar1 = 0x10;
    iVar2 = ((((short)vrd16s(0x20B006 + (((int)W16(0x172EC) & 0xfU) * 0x800) * 2) * 0x30) / 0x8000 + 0x10) *
            (int)(vrd16s(0x20B004 + ((((int)(short)vrd16s(0x20B004 + (((int)W16(0x172EC) & 0x1fU) * 0x400) * 2) << 8) / 0x8000 + 0x200) * (int)W16(0x172EC) & 0xfffcU)))) / 0x8000 + 0x10;
    mem_write32(0x810000, (undefined2)iVar2);
    mem_write32(0x810002, mem_read32(0x810000));
    mem_write32(0x810004, mem_read32(0x810000));
    mem_write32(0x810006, mem_read32(0x810000));
    if (iVar2 < 0) {
      mem_write32(0x810000, 0);
      mem_write32(0x810002, mem_read32(0x810000));
      mem_write32(0x810004, mem_read32(0x810000));
      mem_write32(0x810006, mem_read32(0x810000));
    }
  }
  return uVar1;
}

/* ---- countdown_sub2_stage_start ---- */

void countdown_sub2_stage_start(void)

{
  W16_SET(0x172EC, W16(0x172EA) + -0x2d0);
  if (W16(0x172EC) == 0) {
    sound_play_p2(0x00230092, 0x14);
  }
  if (W16(0x172EC) == 2) {
    g_sys.videomix[0x0007] = 0;
    g_sys.videomix[0x0006] = 0;
    g_sys.videomix[0x0005] = 0;
    mem_write32(0x810000, 0);
    mem_write32(0x810002, 0);
    mem_write32(0x810004, 0);
    mem_write32(0x810006, 0);
  }
  countdown_stage_flythrough((int)W16(0x172EC));
  return;
}

/* ---- countdown_sub3_fog_transition ---- */

void countdown_sub3_fog_transition(void)

{
  W16_SET(0x172EC, W16(0x172EA) + -0x3fc);
  if (W16(0x172EC) == 0) {
    sound_play_p2(0x00230093, 0x14);
    g_fog_b = 0xff;
    g_fog_g = 0xff;
    g_fog_r = 0xff;
  }
  countdown_stage_flythrough(W16(0x172EA) + -0x2d0);
  if (0xb3 < W16(0x172EC)) {
    W[0xEB16] = (undefined2)((W16(0x172EC) * 0xff + -0xb34c) / 0x3c);
  }
  return;
}

/* ---- countdown_sub4_town_reveal ---- */

void countdown_sub4_town_reveal(void)

{
  W16_SET(0x172EC, W16(0x172EA) + -0x4ec);
  if (W16(0x172EC) == 0) {
    sound_play_p2(0x00230094, 0x14);
  }
  if (W16(0x172EC) == 3) {
    g_sys.videomix[0x0007] = 0x80;
    g_sys.videomix[0x0006] = 0x80;
    g_sys.videomix[0x0005] = 0x80;
  }
  if (W16(0x172EC) < 0x3d) {
    W[0xEB16] = (short)((W16(0x172EC) * 0xff) / -0x3c) + 0xff;
  }
  else {
    W[0xEB16] = 0;
  }
  countdown_stage_town_overview((int)W16(0x172EC));
  return;
}

/* ---- countdown_sub5_town_fadeout ---- */

void countdown_sub5_town_fadeout(void)

{
  W16_SET(0x172EC, W16(0x172EA) + -0x5dc);
  if (W16(0x172EC) == 0) {
    sound_play_p2(0x00230096, 0x14);
    g_fog_b = 0;
    g_fog_g = 0;
    g_fog_r = 0;
  }
  countdown_stage_town_overview(W16(0x172EA) + -0x4ec);
  if (0x11c < W16(0x172EC)) {
    W[0xEB16] = (undefined2)((W16(0x172EC) * 0xff + -0x11be3) / 0xf);
  }
  return;
}

/* ---- countdown_sub6_gameplay_start ---- */

void countdown_sub6_gameplay_start(void)

{
  int *piVar1;
  short sVar2;
  undefined2 uVar3;
  undefined4 uVar4;
  
  W16_SET(0x172EC, W16(0x172EA) + -0x708);
  if (W16(0x172EC) == 0) {
    uVar4 = 0x970014;
    uVar3 = 0x23;
    sound_play_p2(0x00230097, 0x14);
    stage_objects_load_list(0x0A);    /* ROM 0x03144A `move.w #$a` */
    player_model_render_only();
  }
  else {
    W[0x169E8] = 0x8002;
    piVar1 = (intptr_t)((int32_t*)W[0x0CA4] + 1);
    *(int32_t*)W[0x0CA4] = 0x8002;
    W[0x0CA4] = piVar1;
    piVar1 = (intptr_t)((int32_t*)W[0x0CA4] + 1);
    *(int32_t*)W[0x0CA4] = (int)W[0x169F0];
    W[0x0CA4] = piVar1;
    sVar2 = W[0x169F0] >> 0xf;
    dsp_viewport_setup(W[0x169F0],(int)W[0x169F2]);
    W[0xEB04] = W[0xEB04] + 1;
    /* ROM 0x03149E: `addq.w #1` then `move.w (a0),-(a7) ; pea $e0ab04` --
     * the frame is the counter, not the viewport word. */
    { extern void anim_update_and_render(intptr_t nb, int t);
      anim_update_and_render(0xAB04, (int16_t)W[0xEB04]); }
  }
  W16_SET(0x17360, (undefined2)W[0xABA8]);
  stage_static_objects_draw();
  if (W16(0x172EC) < 0x10) {
    W[0xEB16] = (short)((W16(0x172EC) * 0xff) / -0xf) + 0xff;
  }
  else if (W16(0x172EC) < 0x96) {
    W[0xEB16] = 0;
  }
  else {
    g_fog_mode = 3;
    W[0xEB16] = (short)((W16(0x172EC) * 0xff + -0x956a) / 0x78);
    if (0xff < W[0xEB16]) {
      W[0xEB16] = 0xff;
    }
  }
  if (W16(0x172EC) == 0x96) {
    sound_play_p2(0x00230098, 0x14);
  }
  return;
}

/* ---- cpu_led_blink ---- */

uint32_t cpu_led_blink(void)

{
  uint32_t uVar1;
  
  uVar1 = W[0x0C98] & 1;
  if (uVar1 == 0) {
    uVar1 = ~(uint32_t)W[0x0C90];
    mem_write32(0x430000, (short)uVar1);
    if (W[0x0C94] == 0) {
      if (W[0x0C90] != 0x80) {
        W[0x0C90] = W[0x0C90] << 1;
        return uVar1;
      }
      uVar1 = 1;
      W[0x0C94] = 1;
    }
    else if (W[0x0C90] == 1) {
      W[0x0C90] = 2;
      W[0x0C94] = 0;
      return 0xfffffffe;
    }
    W[0x0C90] = W[0x0C90] >> 1;
  }
  return uVar1;
}

/* ---- gameover_stage_clear_text @ 0x034FA8 ---- */

/* ROM 0x034FA8..0x034FFE (the annotated file's extent runs on into the data
 * table at 0x035000): on the story game-over board's 4th frame load block
 * 0xE4 ("TOTAL POINTS") into cgram 0x120 with its colour ramp on palette 1,
 * then draw it from that frame on.
 *     034FAA: moveq #-3,d2 ; add.l $e173c8.l,d2 ; bne     -- d2 = frames - 3
 *     034FC8: move.b #$1,-(a7) ; pea $e4.w ; jsr cz_load_color_ramp
 * The palette is a BYTE argument (register row 118); the Ghidra call passed 0. */
void gameover_stage_clear_text(void)

{
  int iVar1;

  iVar1 = W[0x173C8] + -3;
  if (iVar1 == 0) {
    sync_post();
    cgram_load_tile_block(0xe4, 0x120);
    cz_load_color_ramp(0xe4, 1);
  }
  if (-1 < iVar1) {
    text_draw_rect_blink(0xe4, 0xe, 9, 0x120, 1);
  }
  return;
}

/* ---- highscore_display_init ---- */

void highscore_display_init(void)

{
  W[0x0E0C] = 2;
  gameplay_init_state_vars();
  sync_post();
  camera_state_reset();
  W[0x0CEC] = 0x5800;
  W[0x0D00] = 0x1400;                     /* 0x031866 move.l #$1400,(a2) -- an immediate */
  W[0x0D04] = 0xdb00;
  W[0x0D08] = (int32_t)0xffff8000;
  W[0x0D0C] = 0;
  W[0x0D10] = 0;
  W[0x0D14] = 0;
  g_fog_r = 0;
  g_fog_g = 0;
  g_fog_b = 0;
  g_fog_mode = 3;
  attract_highscore_sprites_load();
  W[0x1736C] = 0x1e0;
  if (W[0x0CBC] == 5) {
    W[0x1736C] = 0x1da;
  }
  W[0x0CC4] = 7;
  highscore_display_run();
  return;
}

/* ---- highscore_display_run ---- */

int highscore_display_run(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  char cVar4;
  uint32_t uVar5;
  undefined2 uVar6;
  undefined2 uVar7;
  undefined2 uVar8;
  undefined2 uVar9;
  undefined4 uVar10;
  
  /* THE "TODAY'S PERFECT SCORES" PAGE (ROM 0x0318FC..0x031AC6), rewritten
   * from the disassembly. The transpile had two loops of
   * `text_draw_rect_blink(.., 0, 0, 0, 0)` ("simplified" -- the arguments are
   * register expressions Ghidra lost, register row 62's class), which drew
   * tile 0 at the top-left corner over and over, stored each 16-bit score
   * tile as ONE BYTE, and walked the table at `course*0x28 + idx*2`. The
   * index scales below are hand-decoded from the brief extension words. */
  if (5 < 0x1e0 - W[0x1736C]) {
    int c, r, k;
    text_draw_rect_blink(0xa3, 7, 4, 0x250, 1);
    text_draw_rect_blink(0x97, 2, 7, 0x150, 1);
    text_draw_rect_blink(0x98, 0xe, 7, 0x168, 1);
    text_draw_rect_blink(0x99, 0x1a, 7, 0x180, 1);
    /* 0x03195E: three name letters per rank, from the blocks
     * attract_highscore_sprites_load put in cgram at 0x1a0 + c*36 + r*12 + k*4 */
    for (c = 0; c < 3; c++)
      for (r = 0; r < 3; r++)
        for (k = 0; k < 3; k++)
          text_draw_rect_blink(0x5d, c * 12 + k * 2 + 5, 0xa + r * 4,
                               (uint16_t)(r * 12 + k * 4 + c * 36 + 0x1a0), 1);
    /* 0x0319BA: the score digits and "PTS.", straight into the tilemap at
     * 0x89E610 = textram[0x610], right to left, leading zeros left blank */
    for (c = 0; c < 3; c++) {
      int e8 = (int8_t)W[0x15E70 + c];
      for (r = 0; r < 3; r++) {
        int ent = 0x15BF0 + c * 0xa0 + e8 * 8;
        int32_t sc = (int32_t)W[ent] & 0xffffff;
        for (k = 0; k < 5; k++) {
          if (sc != 0)
            tram_w16(&g_sys.textram[0x610 + r * 0x200 + (c * 12 - k) * 2],
                     (unsigned)((sc % 10 + 0x220) | 0x1000));
          sc /= 10;
        }
        for (k = 0; k < 4; k++)
          tram_w16(&g_sys.textram[0x612 + r * 0x200 + c * 24 + k * 2], 0x122a + k);
        e8 = (int8_t)W[ent + 7];
        if (e8 < 0 || e8 > 19) e8 = 0;                  /* host guard: the ROM trusts the link */
      }
    }
    /* 0x031A84: the big rank digits 1 2 3 */
    for (c = 0; c < 3; c++)
      for (r = 0; r < 3; r++)
        text_draw_rect_blink(0x78 + r, c * 12 + 2, 0xa + r * 4, (uint16_t)(0x240 + r * 4), 1);
  }
  W[0x0CE8] = 0x4000;
  W[0x0CEC] = W[0x0CEC] + 8;
  W[0x0D08] = W[0x0D08] + 0x50;
  dsp_cmd_place_object_abs(0,0x2a,0,0,0);
  player_render();
  if ((W[0x2B3A] & 8) != 0) {
    W[0x17364] = W[0x3EB4];
    W[0x17368] = 1;
    W[0x17369] = 2;
    W[0x1736A] = 3;
    highscore_table_insert(0,(intptr_t)&W[0x17364]);
  }
  iVar1 = 0;
  if (W[0x0CBC] != 5) {
    if (0x1e0 - W[0x1736C] < 0x80) {
      W[0xEB16] = ((short)W[0x1736C] + -0x161) * 2;
    }
    if (W[0x1736C] < 0x80) {
      W[0xEB16] = (0x7f - (short)W[0x1736C]) * 2;
    }
    iVar1 = W[0x1736C] + -1;
    W[0x1736C] = iVar1;
    if (iVar1 < 0) {
      iVar1 = attract_advance_sub_state();
    }
  }
  return iVar1;
}

/* ---- highscore_table_insert @ 0x031B7E ---- */

/* The record's work-RAM offset. The ROM passes an address (0xE17274 from name
 * entry); the one other caller in this tree (the debug screen in this file)
 * passes a host pointer into _W[]. Both resolve to the same byte offset. */
static uint32_t hs_rec_off(intptr_t rec)
{
  if ((uintptr_t)rec >= (uintptr_t)&W[0] && (uintptr_t)rec < (uintptr_t)&W[WORK_RAM_SIZE])
    return (uint32_t)((intptr_t *)rec - &W[0]);
  return (uint32_t)rec & (WORK_RAM_SIZE - 1);
}

/* ROM 0x031B7E, ported instruction by instruction. param_2 is the record
 * {long score; char name[3]}. The list walk remembers the entry BEFORE the
 * insertion point (d7; 0xFF = insert at the head) and ends on the TAIL (d3),
 * which is recycled for the new score: overwritten, then linked in after d7
 * (or made the head). The walk is bounded by the list length, so the stale
 * link the old second-to-last entry keeps is never followed.
 *   - head insert on courses 0..2 also refreshes the EEPROM per-course best
 *     0xE04088 + c*8 when the score beats the working best 0xE15E74 + c*8;
 *   - 0xE00E12 set (a cleared story) and a score above 0xE040A0 replaces the
 *     all-clear best;
 *   - a story run copies the whole story list + head to the EEPROM block
 *     0xE04030..0xE040A8, which is what the attract RANKING screen reads.
 * The transpile read the score through the pointer (the ROM address 0x7274 was
 * dereferenced as a host pointer and crashed), used a quarter of every table
 * offset, and copied the EEPROM block at the wrong strides. */
uint32_t highscore_table_insert(int param_1, intptr_t param_2)
{
  uint32_t rec = hs_rec_off(param_2);
  int32_t  d5 = (int32_t)W[rec];
  int c = W16(0xE10) ? 3 : param_1;
  int n = (c == 3) ? 10 : 20;
  int prev = -1, found = 0, d7 = -1;
  int d3 = hs_b(0x15E70 + c);
  int i, k;
  for (i = 0; i < n; i++) {
    uint32_t ent = 0x15BF0 + c * 0xA0 + d3 * 8;
    if ((d5 & 0xffffff) > (int32_t)((uint32_t)W[ent] & 0xffffff) && !found) {
      d7 = prev; found = 1;
    }
    if (i < n - 1) { prev = d3; d3 = hs_b(ent + 7); }
  }
  if (found) {
    uint32_t ent = 0x15BF0 + c * 0xA0 + d3 * 8;
    W[ent] = d5;
    hs_b_set(ent + 4, hs_b(rec + 4));
    hs_b_set(ent + 5, hs_b(rec + 5));
    hs_b_set(ent + 6, hs_b(rec + 6));
    if ((int8_t)d7 < 0) {                               /* new head */
      hs_b_set(ent + 7, hs_b(0x15E70 + c));
      hs_b_set(0x15E70 + c, d3);
      if (c < 3 && (d5 & 0xffffff) > (int32_t)((uint32_t)W[0x15E74 + c * 8] & 0xffffff)) {
        W[0x4088 + c * 8] = W[ent];                     /* 8 bytes -> EEPROM best */
        for (k = 4; k < 8; k++) hs_b_set(0x4088 + c * 8 + k, hs_b(ent + k));
      }
    } else {
      uint32_t pe = 0x15BF0 + c * 0xA0 + (int8_t)d7 * 8;
      hs_b_set(ent + 7, hs_b(pe + 7));
      hs_b_set(pe + 7, d3);
    }
  }
  if (W16(0xE12) != 0 && (d5 & 0xffffff) > (int32_t)((uint32_t)W[0x40A0] & 0xffffff)) {
    W[0x40A0] = d5;
    hs_b_set(0x40A4, hs_b(rec + 4));
    hs_b_set(0x40A5, hs_b(rec + 5));
    hs_b_set(0x40A6, hs_b(rec + 6));
  }
  if (W16(0xE10) != 0) {
    for (i = 0; i < 10; i++) {
      W[0x4030 + i * 8] = W[0x15DD0 + i * 8];
      for (k = 4; k < 8; k++) hs_b_set(0x4030 + i * 8 + k, hs_b(0x15DD0 + i * 8 + k));
    }
    hs_b_set(0x40A8, hs_b(0x15E73));
  }
  return 0;
}

/* ---- nop_stub ---- */

void nop_stub(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x00C02A is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. Bare RTS in the ROM -- the name is accurate.
   */
  return;
}

/* ---- nop_stub_2 ---- */

void nop_stub_2(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x00C0B2 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. Bare RTS in the ROM -- the name is accurate.
   */
  return;
}

/* ---- object_display_init @ 0x0316EE ---- */

/* ROM 0x0316EE -- NOT an object/display init: it builds the working
 * high-score tables at boot (the ROM's last init call, 0x00BD6A, after the
 * EEPROM block is loaded). Courses 0..2 get twenty default entries each,
 * 1700 - 50*i with name "111" (index 0x31) and next = i+1 (0xFF for the
 * last), head 0; then the story list, its head and the four bests are copied
 * in from the EEPROM block 0xE04030. Nothing called it, so FUN_00031d40
 * walked zeroed tables and every score ranked first. The transpile also used
 * Ghidra's int-pointer strides for the scores and 2-byte strides for the
 * 8-byte copies. Verified against MAME's own table at name entry (the
 * 0xE15BF0 dump in tools/overnight/snap_nameentry.lua's TBL lines). */
int object_display_init(void)
{
  int c, i, k;
  for (c = 0; c < 3; c++) {
    for (i = 0; i < 20; i++) {
      uint32_t ent = 0x15BF0 + c * 0xA0 + i * 8;
      W[ent] = (int32_t)(i * -50 + 0x6a4);
      hs_b_set(ent + 4, 0x31);
      hs_b_set(ent + 5, 0x31);
      hs_b_set(ent + 6, 0x31);
      hs_b_set(ent + 7, (i < 0x13) ? i + 1 : -1);
    }
    hs_b_set(0x15E70 + c, 0);
  }
  for (i = 0; i < 10; i++) {
    W[0x15DD0 + i * 8] = W[0x4030 + i * 8];
    for (k = 4; k < 8; k++) hs_b_set(0x15DD0 + i * 8 + k, hs_b(0x4030 + i * 8 + k));
  }
  hs_b_set(0x15E73, hs_b(0x40A8));
  for (i = 0; i < 4; i++) {
    W[0x15E74 + i * 8] = W[0x4088 + i * 8];
    for (k = 4; k < 8; k++) hs_b_set(0x15E74 + i * 8 + k, hs_b(0x4088 + i * 8 + k));
  }
  return 0;
}

/* ---- palette_clear ---- */

void palette_clear(void)

{
  undefined1 uVar1;
  int iVar2;
  
  iVar2 = 0;
  do {
    /* THE FINAL GAMMA for the whole picture, and it was reading ONE BYTE.
     * The M68K at 0x02293A is `move.w $21b004(d1.l*2),d0 ; asr.l #2,d0` --
     * a BE16 on a 2-byte stride. `R` is uint8_t[], so `(&R[0x21B004])[i]`
     * took byte i, which for this table of small big-endian words is the
     * HIGH byte and therefore 0 almost everywhere: the gamma LUT came out
     * all zeros, `fog_hw.c` set have_gamma = 0, and screenshot.c skipped
     * the gamma stage entirely. Measured against MAME's captured mixer
     * (mix_f391.bin), the corrected read reproduces its LUT **256/256
     * exactly** (0x40 -> 124, 0x80 -> 184, 0xFF -> 255). Register row 27's
     * S3 shape. Without it every live frame was too dark by gamma ~2:
     * the attract logo background read RGB(24,87,245) against MAME's
     * (69,149,250), and fitting MAME = ours^g gave g = 0.52 at every
     * sample. */
    uVar1 = (undefined1)(vrd16(0x21B004 + iVar2 * 2) >> 2);
    g_sys.videomix[0x0100 + (iVar2)] = uVar1;
    g_sys.videomix[0x0200 + (iVar2)] = uVar1;
    g_sys.videomix[0x0300 + (iVar2)] = uVar1;
    iVar2 = iVar2 + 1;
  } while (iVar2 < 0x100);
  return;
}

/* ---- palette_fog_init ---- */

void palette_fog_init(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  
  iVar1 = 0;
  iVar4 = 1;
  do {
    iVar3 = 0;
    do {
      iVar2 = iVar1;
      iVar1 = iVar3 + iVar4 * 0x100;
      g_sys.palette_ram[0 + (iVar1)] = (&R[0x21B204])[iVar2];
      g_sys.palette_ram[0x8000 + (iVar1)] = (&R[0x21B304])[iVar2];
      g_sys.palette_ram[0x10000 + (iVar1)] = (&R[0x21B404])[iVar2];
      iVar3 = iVar3 + 1;
      iVar1 = iVar2 + 1;
    } while (iVar3 < 0x100);
    iVar1 = iVar2 + 0x201;
    g_sys.syscon[0x14] = 0;
    iVar4 = iVar4 + 1;
  } while (iVar4 < 0x80);
  return;
}

/* ---- palette_update ---- */

void palette_update(void)

{
  /* THE SCREEN FADE / FOG PUBLISH RING, rewritten from ROM 0x022AC0.
   *
   * The mixer is not written from W[0xEB16] directly: the game keeps a
   * THREE-DEEP RING and advances an index each frame, which is the hardware's
   * own pipeline delay -- what reaches the screen this frame is the level from
   * three frames ago. The transpile had the ring's ELEMENT WIDTH AND INDEX
   * SCALE wrong, which is register row 71's class:
   *
   *   ROM  022AC8: move.w $e0eb22(d0.w * 2), d0   <- 16-BIT entry, index * 2
   *   was  W[0xEB22 + W[0xEB5E]]                  <- byte entry, stride 1
   *
   * At stride 1 the three entries land on 0xEB22/23/24 instead of
   * 0xEB22/24/26, so two of them sat on non-4-aligned offsets that
   * `sync_wram_to_W` rebuilt from their neighbours every frame -- and the
   * INDEX at 0xEB5E is itself a 16-bit value at a 2-mod-4 offset, read here as
   * a whole slot, so it could never advance past 0 either. Measured on the
   * ADVANCED screen: the ring read 0,0,0 and videomix[0x16..0x19] stayed
   * 0,0,0,0 -- a screen_fade_factor of zero, i.e. NO FADE ANYWHERE IN THE
   * GAME, which is why the attract logo hard-cuts instead of fading.
   *
   * The SOURCES were already right and must not be "fixed": ROM 022B42..022B88
   * is `move.w $e0eb16/18/1a/1c/1e/20.l, <ring>(d0.w*2)`, exactly the
   * g_fog_* macros below, and the halt gate is `tst.l $e00ca8.l` (a LONG).
   * `PROPCYCL_FADELOG=1` prints the whole chain, `=2` adds the mixer block --
   * which is byte-identical to MAME's on all 28 registers. */
  /* PROPCYCL_FADELOG=1: the whole screen-fade chain in one line -- the level
   * the game computed, the ring slot it is published through, and the byte
   * the RENDERER actually reads (fog_hw.c's fs.screen_fade_factor = mix[0x19]).
   * A fade that the player never sees is either a level that never moves or a
   * ring that does not survive the _W[] sync; this separates the two. */
  { extern int g_fadelog; static int n;
    if (g_fadelog && (n++ % 15) == 0)
      fprintf(stderr, "[FADE] f%u st=%ld/%ld EB16=%ld idx=%ld ring=%ld,%ld,%ld"
                      " -> mix19=%u mix16..18=%u,%u,%u\n",
              (unsigned)g_sys.frame_count, (long)W[0x0CBC], (long)W[0x0CC0],
              (long)W[0xEB16], (long)W_LO16(0xEB5C),
              (long)W_A16(0xEB22,0), (long)W_A16(0xEB22,1), (long)W_A16(0xEB22,2),
              g_sys.videomix[0x19], g_sys.videomix[0x16],
              g_sys.videomix[0x17], g_sys.videomix[0x18]);
    if (g_fadelog >= 2) {
      fprintf(stderr, "[MIX ] f%u ", (unsigned)g_sys.frame_count);
      { int _i; for (_i = 0; _i <= 0x1b; _i++) fprintf(stderr, "%02x ", g_sys.videomix[_i]); }
      fprintf(stderr, "\n"); } }
  { int _i = (int)W_LO16(0xEB5C);            /* the ring index, 16-bit @0xEB5E */

    g_sys.videomix[0x0019] = (undefined1)W_A16(0xEB22, _i);
    g_sys.videomix[0x001A] = (undefined1)W_A16(0xEB2C, _i);
    g_sys.videomix[0x0016] = (undefined1)W_A16(0xEB36, _i);
    g_sys.videomix[0x0017] = (undefined1)W_A16(0xEB40, _i);
    g_sys.videomix[0x0018] = (undefined1)W_A16(0xEB4A, _i);
    /* THE CZ FOG ENABLE. ROM 0x022B18: `tst.w $e0eb54(d0.w*2) ; beq ;
     * move.w #$4444,$810008` else `clr.w $810008 ; clr.b $82400d` --
     * SIXTEEN-bit stores to czattr[4]. mem_write32 here wrote the value's
     * high half (0) to czattr[4] and 0x4444 to czattr[5], so fog_quad's
     * `czattr[4] & 4` gate was never open and the live path drew NO CZ fog at
     * all. MAME's captured czattr at f1500..f4800 reads 0x810008 = 0x4444 and
     * 0x81000A = 0 -- this is the value the renderer's own comment in
     * fog_hw.c recorded the other way round ("the game itself holds 0x4444"
     * at 0x81000A). */
    if (W_A16(0xEB54, _i) == 0) {
      mem_write16(0x810008, 0);
      g_sys.videomix[0x000D] = 0;
    }
    else {
      mem_write16(0x810008, 0x4444);
    }
    if (_g_halt_flag == 0) {
      W_A16_SET(0xEB22, _i, (int16_t)W[0xEB16]);
      W_A16_SET(0xEB2C, _i, (int16_t)g_fog_mode);
      W_A16_SET(0xEB36, _i, (int16_t)g_fog_r);
      W_A16_SET(0xEB40, _i, (int16_t)g_fog_g);
      W_A16_SET(0xEB4A, _i, (int16_t)g_fog_b);
      /* move.w $e0eb20.l,... -- the fog enable is the HIGH half of slot
       * 0xEB20; the LOW half is ring entry 0 at 0xEB22, which the whole-slot
       * read used to return instead (a fade level where a flag belongs). */
      W_A16_SET(0xEB54, _i, W_HI16(0xEB20));
      W_SET_LO16(0xEB5C, (int16_t)((_i + 1) % 3));
    }
  }
  return;
}

/* ---- physics_spring_calc_velocity ---- */

int physics_spring_calc_velocity(int param_1,int param_2,int *param_3)

{
  int iVar1;
  int iVar2;
  int *piVar3;
  int iVar4;
  int *piVar5;
  int *piVar6;
  int *piVar7;
  int *piVar8;
  
  iVar4 = param_1 * 0xc;
  iVar2 = 0;
  piVar3 = (int *)(&W[0x16570 + iVar4]);
  piVar5 = (int *)(&W[0x163C0 + iVar4]);
  piVar6 = W[0x1642C + param_1] * 3;
  piVar7 = (int *)(&W[0x16504 + iVar4]);
  piVar8 = W[0x165DC + param_1] * 3;
  do {
    *piVar3 = (*piVar7 + (*param_3 + ((((int32_t*)(intptr_t)piVar6)[3] + ((int32_t*)(intptr_t)piVar6)[-3]) * 0xd00) / param_2) / 0x78) -
              (*piVar8 * (param_2 * -0x1e0 + 0x2a)) / (param_2 * 0x1e0);
    *piVar5 = *piVar7 / 0x1e0 - (*piVar6 * param_2 * -0x1e0) / (param_2 * 0x1e0);
    iVar4 = iVar2 + 1;
    iVar1 = iVar2 + -2;
    iVar2 = iVar4;
    piVar3 = piVar3 + 1;
    piVar5 = piVar5 + 1;
    piVar6 = piVar6 + 1;
    piVar7 = piVar7 + 1;
    piVar8 = piVar8 + 1;
    param_3 = param_3 + 1;
  } while (iVar4 < 3);
  return iVar1;
}

/* ---- physics_spring_integrate_pos ---- */

int physics_spring_integrate_pos(int param_1,int param_2,int *param_3)

{
  int *piVar1;
  int iVar2;
  int iVar3;
  int *piVar4;
  int *piVar5;
  int iVar6;
  int *piVar7;
  int *piVar8;
  int *piVar9;
  int *local_8;
  
  iVar3 = param_1 * 0xc;
  W[0x16498 + (iVar3)] = W[0x1642C + (param_1 * 3)];
  W[0x1649C + (iVar3)] = W[0x16430 + (param_1 * 3)];
  W[0x164A0 + (iVar3)] = W[0x16434 + (param_1 * 3)];
  piVar7 = W[0x1642C + param_1] * 3;
  piVar4 = (int *)(&W[0x16504 + iVar3]);
  piVar5 = W[0x165DC + param_1] * 3;
  iVar6 = 0;
  piVar8 = W[0x1648C + param_1] * 3;
  piVar9 = (int *)(&W[0x16570 + iVar3]);
  local_8 = W[0x16354 + param_1] * 3;
  do {
    piVar1 = param_3 + 1;
    *piVar4 = ((*param_3 + ((((int32_t*)(intptr_t)piVar7)[3] + *piVar8) * 0xd00) / param_2) / 0x3c -
              (*piVar9 * 0x2a) / (param_2 * 0xf0)) + *piVar4;
    *piVar7 = (*piVar9 - 0 / param_2) / 0xf0 + *piVar7;
    *piVar5 = *piVar4 + 0 / param_2;
    param_3 = param_3 + 2;
    *local_8 = *piVar1 + (*piVar5 * -0x2a + (((int32_t*)(intptr_t)piVar7)[3] + *piVar8 + *piVar7 * -2) * 0xd00) / param_2;
    iVar3 = iVar6 + 1;
    piVar7 = piVar7 + 1;
    iVar2 = iVar6 + -2;
    piVar4 = piVar4 + 1;
    piVar5 = piVar5 + 1;
    iVar6 = iVar3;
    piVar8 = piVar8 + 1;
    piVar9 = piVar9 + 1;
    local_8 = local_8 + 1;
  } while (iVar3 < 3);
  return iVar2;
}

/* ---- process_mcu_outputs ---- */

void process_mcu_outputs(void)

{
  /* COMPLETE, not a stub. ROM 0x02221C is exactly three instructions:
   * `movea.l $e00ca4.l,a0 ; moveq #$ff,d0 ; move.l d0,(a0)` -- the line
   * below is all of it. */
  /* It is the DISPLAY-LIST TERMINATOR, not an MCU routine: the master DSP
   * stops walking the list at a lone -1 command word. */
  { uint8_t *c = (uint8_t *)W[0x0CA4];
    if (c >= g_sys.dspram && c + 4 <= g_sys.dspram + DSPRAM_SIZE)
      *(int32_t*)c = (int32_t)0xffffffff; }
  return;
}

/* ---- read_mcu_inputs ---- */

void read_mcu_inputs(void)

{
  input_read_service_buttons();
  dipswitch_init();
  input_decode_buttons();
  read_mcu_inputs_process();
  input_process_analog_deltas();
  return;
}

/* ---- task_system_init ---- */

void task_system_init(void)

{
  W[0x3E70] = 0;
  W[0x3E74] = 0;
  W[0x3E78] = 0;
  W[0x3E7C] = 0;
  W[0x3E80] = 1;
  W[0x3E84] = 1;
  W[0x3E88] = 1;
  W[0x3E8C] = 1;
  W[0x3EB0] = 0;
  W[0x3EB4] = 0;
  W[0x3EB8] = 0;
  W[0x3EBC] = 0;
  W[0x3EC0] = 1;
  W[0x3EC4] = 1;
  W[0x3EC8] = 1;
  W[0x3ECC] = 1;
  W[0x3EF0] = 0;
  W[0x3EF4] = 0;
  W[0x3EF8] = 0;
  W[0x3EFC] = 0;
  W[0x3F00] = 1;
  W[0x3F04] = 1;
  W[0x3F08] = 1;
  W[0x3F0C] = 1;
  _g_task_list_head = 0xe02e4c;
  _g_task_list_tail = 0xe02e4c;
  _g_task_pool_size = 0x3c000;
  return;
}

/* ---- task_system_tick ---- */

void task_system_tick(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x00A86E is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. Empty in the retail ROM. The task system it would have driven is not used
   * by this build of the game.
   */
  return;
}

/* ---- wait_dsp_sync ---- */

void wait_dsp_sync(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x021054 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. One of a run of six consecutive RTS entries at 0x021050-0x02105A
   * (sync_wait_1/2/3, debug_profiler_mark, wait_dsp_sync). On real hardware
   * the DSP handshake is done by the IRQ, not by spinning here.
   */
  return;
}

/* ---- gameplay deps (23) ---- */

/* ---- FUN_0000a532 ---- */

uint32_t FUN_0000a532(void)

{
  uint32_t uVar1;
  
  if (W[0x0E18] == 0) {
    if (W[0x0E44] < 0x40) {
      W[0xEB16] = W[0xEB16] + 2;
    }
    else {
      W[0xEB16] = W[0xEB16] + -2;
    }
  }
  else {
    W[0xEB16] = W[0xEB16] + 2;
    g_scene_fade_level = g_scene_fade_level + 1;
    if (0x7e < g_scene_fade_level) {
      FUN_0000ffb2();
      if (W[0x0E0C] == 3) {
        W[0x0CC0] = 0xe;
      }
      else {
        W[0x0CC0] = 6;
      }
    }
  }
  if (W[0xEB16] < 0) {
    W[0xEB16] = 0;
  }
  if (0xff < W[0xEB16]) {
    W[0xEB16] = 0xff;
  }
  uVar1 = W[0x2C0C] & 0xffffff10;
  if ((W[0x2C0C] & 0x10) != 0) {
    if ((W[0x2B5E] & 0x100) != 0) {
      W[0x0E44] = 600;
      W[0x0E48] = 600;
      W[0x0E4C] = 1000;
    }
    uVar1 = W[0x2B5E] & 0xffff0200;
    if ((W[0x2B5E] & 0x200) != 0) {
      W[0x0E4C] = (int)W[0x15FE2];
      uVar1 = (int)W[0x15FE2] / 10 << 3;
      W[0x0E50] = uVar1;
    }
    if ((W[0x3EFC] != 0) && (uVar1 = W[0x4010] & 0x1ff, uVar1 == 0)) {
      W[0x0E4C] = (int)W[0x15FE2];
      uVar1 = (int)W[0x15FE2] / 10 << 3;
      W[0x0E50] = uVar1;
    }
  }
  if (W[0x0E44] < 1) {
    W[0x0E18] = 2;
    W[0x15FE8] = 1;
    sprite_draw_2d(1,0xfffffea5,0x80,0xa0,0x10,0x20,0x20,g_scene_fade_level << 9,0);
    uVar1 = sprite_draw_2d(1,0xfffffea4,0x180,0xa0,0x10,0x20,0x20,g_scene_fade_level << 9,0);
  }
  if (W[0x15FE2] <= W[0x0E4C]) {
    W[0x0E18] = 1;
    W[0x15FE8] = 1;
    sprite_draw_2d(1,0xfffffea1,0x60,0xa0,0x10,0x20,0x20,g_scene_fade_level << 9,0);
    uVar1 = sprite_draw_2d(1,0xfffffea0,0x160,0xa0,0x10,0x20,0x20,g_scene_fade_level << 9,0);
  }
  return uVar1;
}

/* ---- FUN_000226b0 ---- */

void FUN_000226b0(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x0226B0 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. One of six consecutive 2-byte RTS entries at 0x0226B0-0x0226BA.
   */
  return;
}

/* ---- FUN_000226b2 ---- */

void FUN_000226b2(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x0226B2 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. One of six consecutive 2-byte RTS entries at 0x0226B0-0x0226BA.
   */
  return;
}

/* ---- FUN_000226b4 ---- */

void FUN_000226b4(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x0226B4 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. One of six consecutive 2-byte RTS entries at 0x0226B0-0x0226BA.
   */
  return;
}

/* ---- FUN_000226b6 ---- */

void FUN_000226b6(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x0226B6 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. One of six consecutive 2-byte RTS entries at 0x0226B0-0x0226BA.
   */
  return;
}

/* ---- FUN_000226b8 ---- */

void FUN_000226b8(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x0226B8 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. One of six consecutive 2-byte RTS entries at 0x0226B0-0x0226BA.
   */
  return;
}

/* ---- FUN_000226ba ---- */

void FUN_000226ba(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x0226BA is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. One of six consecutive 2-byte RTS entries at 0x0226B0-0x0226BA.
   */
  return;
}

/* ---- render_flag_banner ---- */

/* ROM 0x01629A, ported from the machine code: one flag/banner model
 * (0x36414[idx]) of the ring about [pos] + (0x3C000, 0x16000, 0x114000),
 * offset by scale * (sin, cos)(frame*32 + idx*0x2000) >> 11, as an 11-word
 * 0x8002 entry with the pairs (angle), (frame*32 + idx*0x2000 + add) and
 * (frame*128). `pos` is a 68K address (a caller's stack frame on the machine). */
int * render_flag_banner(int idx, uint32_t pos, int scale, uint32_t angle, int add, int *out_)
{
  int32_t *out = (int32_t *)out_;
  uint32_t a = (uint32_t)((int32_t)W[0x0C8C] * 32 + idx * 0x2000);
  uint32_t b = a + (uint32_t)add;
  uint32_t f = (uint32_t)((int32_t)W[0x0C8C] << 7);
  *out++ = vrd16s(0x36414 + idx * 2);
  *out++ = e_rd32(pos)     + (int32_t)(((int64_t)e_sin(a) * scale) >> 11) + 0x3c000;
  *out++ = e_rd32(pos + 4) + 0x16000;
  *out++ = e_rd32(pos + 8) + (int32_t)(((int64_t)e_cos(a) * scale) >> 11) + 0x114000;
  *out++ = e_sin(angle); *out++ = e_cos(angle);
  *out++ = e_sin(b);     *out++ = e_cos(b);
  *out++ = e_sin(f);     *out++ = e_cos(f);
  *out++ = 4;
  return (int *)out;
}

/* ---- stub_empty ---- */

void stub_empty(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x02FF52 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. Bare RTS in the ROM -- the name is accurate.
   */
  return;
}

/* ---- wind_effect_level_calc ---- */

void wind_effect_level_calc(void)

{
  int iVar1;
  
  iVar1 = W[0x0D88] / 6 + W[0x0D48] / 0x600;
  if (5 < iVar1) {
    iVar1 = 5;
  }
  if (0x10 < W[0x0D8C]) {
    iVar1 = 6;
  }
  if (W[0x0D48] < 0) {
    iVar1 = 0;
  }
  if (W[0x0E18] != 0) {
    iVar1 = 0;
  }
  if (W[0x0E30] != 0) {
    iVar1 = 0;
  }
  tilemap_wind_flash_update(iVar1);
  return;
}

/* ========== FINAL BATCH - ALL REMAINING (144 functions) ========== */

uint16_t eeprom_read_status(void);
uint16_t eeprom_wait_ack(void);

/* ---- FUN_0000c970 @ 0x00C970 (attract title-logo run, case 1) ---- */
/* Rewritten from the M68K machine code (MAME dasm @0xC970). The previous
 * hand-written substitute composed a namco wordmark from sprite tiles
 * 0x330-0x34C with an invented two-copies-orbit animation; the real code:
 *   1. one model-0x27 billboard emission, fixed position, yaw = fc<<5
 *      (flags=2 billboard mode; pointrom[0x27] = -1 -> resolved via the
 *      C374 sprite/point-RAM path, not point ROM)
 *   2. eight background sprites, tiles 0x14A-0x151 (sky/clouds + logo
 *      composition; fidelity depends on the C374 sprite renderer)
 *   3. timer-based advance after 300 frames (0x12C) via
 *      attract_advance_sub_state — NOT input-driven.
 * L3-verified: DSP emission now matches MAME word-for-word in the logo
 * phase. */

void FUN_0000c970(void)

{
  int i;

  text_output_flush();
  W[0x0CD4] = W[0x0CD4] + 1;

  /* dasm 0xC986: dsp_cmd_set_camera(0, 0x27, 0x8000, 0xFFF78000,
   * 0xFFF40A00, 0, fc<<5, 0, 2) — one billboard, slow yaw spin */
  dsp_cmd_set_camera(0, 0x27, 0x8000, 0xfff78000, 0xfff40a00,
                     0, W[0x0C98] << 5, 0, 2);

  /* dasm 0xC9BA: three 256px-spaced sprites at y=0 (tiles 0x14A-0x14C),
   * three at y=0xA0 (tiles 0x14D-0x14F), singles 0x150 @(0x90,0x120)
   * and 0x151 @(0x190,0x120) */
  for (i = 0; i < 3; i++)
    sprite_draw_2d(5, 0x14a + i, i << 8, 0, 1, 0x20, 0x20, 0, 0);
  for (i = 0; i < 3; i++)
    sprite_draw_2d(5, 0x14d + i, i << 8, 0xa0, 1, 0x20, 0x20, 0, 0);
  sprite_draw_2d(5, 0x150, 0x90, 0x120, 1, 0x20, 0x20, 0, 0);
  sprite_draw_2d(5, 0x151, 0x190, 0x120, 1, 0x20, 0x20, 0, 0);

  /* dasm 0xCA68: advance after 300 frames */
  if (W[0x0CD4] >= 0x12c) {
    attract_advance_sub_state();
  }
  return;
}

/* ---- FUN_0000ca80 ---- */

void FUN_0000ca80(void)

{
  W[0x15E94] = 0x1e00;
  W[0x15E98] = 0;
  W[0x15E9C] = 0;
  W[0x15EA8] = 0;
  sync_post();
  camera_state_reset();
  W[0x0CD8] = 0;
  W[0x0CC4] = 3;
  W[0x0D48] = 0;
  W[0x0D50] = 0;
  W[0x0D54] = (int32_t)0xffffc000;
  W[0x2C04] = 0;
  sound_reset_all();
  player_model_load_animation(0);
  W[0xEB06] = 1;
  W[0x12BC] = 1;
  W[0x2A9C] = 0;
  W[0x2A90] = 0;
  W[0x2A84] = 0;
  W[0x2A78] = 0;
  W[0x2AA8] = 0;
  g_fog_r = 0xff;
  g_fog_g = 0xff;
  g_fog_b = 0xff;
  W[0xEB16] = 0;
  g_fog_mode = 1;
  FUN_0000cbf2();
  return;
}

/* ---- FUN_0000cb44 ---- */

void FUN_0000cb44(void)

{
  W[0x0D5C] = 0;
  W[0x0D60] = 0;
  W[0x0D58] = 0;
  W[0x0D0C] = 0;
  W[0x0D10] = 0x4000 - W[0x15E98];
  W[0x0D14] = (int32_t)0xffffe800;
  W[0x0D00] = W[0x0CDC] + W[0x15EA0];
  W[0x0D04] = W[0x0CE0];
  W[0x0D08] = W[0x0CE4] + W[0x15EA4] + W[0x15E94];
  W[0x0E08] = W[0x15EA8];
  W[0x0D50] = 0;
  W[0x0D54] = (int32_t)0xffffc000;
  W[0x0D98] = W[0x15E9C];
  W[0x2C04] = 0;
  /* &R[0x01480] was a HOST POINTER where a coordinate belongs (same bug as
   * the sky dome's Z); it is the immediate 0x1480. */
  dsp_cmd_set_camera(0,0x27,0,0,0,0x1480,0xe000,0,2);
  { extern int g_cmd_dump; intptr_t _b = W[0x0CA4];
    camera_update_main();
    if (g_cmd_dump) printf("[CMD]   camera_update_main (from FUN_0000cb44) wrote %ld words\n",
                           (long)((W[0x0CA4] - _b) / 4)); }
  return;
}

/* ---- FUN_0000cbf2 ---- */

void FUN_0000cbf2(void)

{
  int iVar1;
  uint32_t uVar2;
  int iVar3;
  int iVar4;
  
  iVar3 = -W[0x0CD8];
  iVar4 = iVar3 + 300;
  if (0x40 < iVar4) {
    text_output_flush();
    /* flyover orbit offset: sin/cos of W[0x15E98]. Were ONE-BYTE reads of the
     * 16-bit Q15 table at the half-stride index (fix-#11 class). */
    W[0x15EA0] = (int)vrd16s(0x20B004 + ((uint32_t)W[0x15E98] & 0xfffc)) >> 3;   /* sin */
    W[0x15EA4] = (int)vrd16s(0x20B002 + ((uint32_t)W[0x15E98] & 0xfffc)) >> 3;   /* cos */
    W[0x15E9C] = W[0x15E9C] + 1;
    if (0x3f < W[0x15E9C]) {
      W[0x15E9C] = 0;
    }
    FUN_0000cb44();
    /* NO player_render() HERE -- it double-emits the aircraft.
     *
     * The flyover's objects come from camera_update_main(), which
     * FUN_0000cb44() already calls. player_render() ALSO ends with
     * camera_update_main(), so calling it here wrote the 26-object
     * bike+rider set twice (649 words each) plus its own 6 bike
     * placements: 59 placements per frame against the recording's 26,
     * with the second copy landing at |t| 29569/39556 and smearing the
     * aircraft across the screen (code 113 bbox w=3227 vs the
     * recording's 354).
     *
     * An earlier revision added the call because the cinematic drew only
     * sky and the diff read `both=1 rec=26 live=0`. That symptom was the
     * PRIORITY bug (register row 23, fixed in renderer_3d.c): the objects
     * were being emitted and drawn all along, underneath a sky that
     * sorted nearest. Measure before adding an emitter -- PROPCYCL_CMDDUMP=1
     * prints every placement with its buffer word offset. */
    /* NOTE: the threshold is read ONCE IN main.c, not with getenv() here.
     * Calling getenv()+atoi() from inside the frame loop segfaults --
     * getenv returns a pointer that faults on read, and the address is a
     * sign-extended 32-bit truncation of a stack address
     * (0xffffffffee248af8 for a stack at 0x7fffee248af8). Something in the
     * reimpl writes a truncated 64-bit pointer over the environment block.
     * See the mismatch register. */
    if (getenv("PROPCYCL_FINDV")) { static int done;
      if (!done && g_sys.frame_count > 140) { done = 1;
        extern intptr_t _W[]; int i, n = 0;
        long want = atol(getenv("PROPCYCL_FINDV"));
        printf("    [FINDV] slots holding %ld (+-3):\n", want);
        for (i = 0; i < WORK_RAM_SIZE && n < 30; i++) {
          long v = (long)(int32_t)_W[i];
          if (v >= want - 3 && v <= want + 3) {
            printf("    [FINDV]   W[0x%04X] = %ld\n", i, v); n++; } } } }
    if (getenv("PROPCYCL_W23")) { static int done;
      if (!done && g_sys.frame_count > 140) { done = 1;
        extern intptr_t _W[]; int i, n = 0;
        printf("    [W23] work-RAM slots holding a +-2^23-shifted value:\n");
        for (i = 0; i < WORK_RAM_SIZE && n < 40; i++) {
          long v = (long)(int32_t)_W[i];
          if ((v > 8380000 && v < 8390000) || (v < -8380000 && v > -8390000)) {
            printf("    [W23]   W[0x%04X] = %ld   (v -+ 0x800000 = %ld)\n",
                   i, v, v > 0 ? v - 0x800000 : v + 0x800000);
            n++; } } } }
    if (getenv("PROPCYCL_FLY")) { static int n;
      if (n++ % 60 == 0)
        printf("    [FLY] f=%d cd8=%ld iVar4=%d E30=%ld 12BC=%ld CA4=%p ply=(%ld,%ld,%ld)\n",
               (int)g_sys.frame_count, (long)W[0x0CD8], iVar4,
               (long)W[0x0E30], (long)W[0x12BC], (void*)W[0x0CA4],
               (long)W[0x0D00], (long)W[0x0D04], (long)W[0x0D08]); }
    W[0x15EA8] = W[0x15EA8] + -0x1000;
    W[0x15E98] = W[0x15E98] - 0x80;
  }
  dsp_w32(0x10038 + 4 * (W[0x0CA0] * 0x2000), (int32_t)(0x780));
  iVar1 = iVar3 + 0xec;
  if ((iVar1 < 0x40) && (-1 < iVar1)) {
    W[0xEB16] = (0x3f - (short)iVar1) * 4;
  }
  if (iVar4 < 0x40) {
    g_fog_r = (short)iVar4 << 2;
    g_fog_g = g_fog_r;
    g_fog_b = g_fog_r;
  }
  iVar3 = iVar3 + 0xac;
  if ((iVar3 < 0x80) && (-1 < iVar3)) {
    uVar2 = (iVar3 * 0x80 >> 1 & 0x7ffe) * 2;
    iVar3 = (int)(int16_t)vrd16s(0x20B004 + (uVar2)) >> 7;
    iVar1 = (int)(int16_t)vrd16s(0x20B006 + (uVar2)) >> 7;
    sprite_draw_2d(0,0xfffffea7,iVar3 + 0x80,iVar1 + -0x40,0x100,0x20,0x20,0x4000,0);
    sprite_draw_2d(0,0xfffffea6,iVar3 + 0x180,iVar1 + -0x40,0x100,0x20,0x20,0x4000,0);
    sprite_draw_2d(0,0xfffffea7,0x80 - iVar3,0x1c0 - iVar1,0x100,0x20,0x20,0x4000,0);
    sprite_draw_2d(0,0xfffffea6,0x180 - iVar3,0x1c0 - iVar1,0x100,0x20,0x20,0x4000,0);
  }
  if (iVar4 < 0x80) {
    if (iVar4 < 0x40) {
      iVar3 = (0x3f - iVar4) * 0x400;
    }
    else {
      iVar3 = 0;
    }
    sprite_draw_2d(0,0xfffffea7,0x80,0xc0,0x100,0x20,0x20,iVar3,0);
    sprite_draw_2d(0,0xfffffea6,0x180,0xc0,0x100,0x20,0x20,iVar3,0);
  }
  W[0x0CD8] = W[0x0CD8] + 1;
  if (300 < W[0x0CD8]) {
    attract_advance_sub_state();
  }
  return;
}

/* ---- FUN_0000ce1e ---- */

void FUN_0000ce1e(void)

{
  dsp_param_init();
  W[0x0E0C] = 3;
  W16_SET(0xE64, 0);
  gameplay_init_player_and_world();
  gameplay_init_state_vars();
  g_fog_r = 0;
  g_fog_g = 0;
  g_fog_b = 0;
  W[0x1703C] = 0;
  W[0xEB16] = 0xff;
  W[0x15ED0] = 2;
  g_fog_mode = 3;
  W_SET_HI16(0xEB20, 0);
  W[0x0CDC] = 0x485fe;
  W[0x0CE0] = (int32_t)0xffef8000;
  /* AN IMMEDIATE, NOT AN ADDRESS. ROM 0x00CE8A is
   *     move.l  #$12e89c, $8(a2)      a2 = 0xE00CDC
   * i.e. the camera Z coordinate 0x12E89C. `&R[...]` put a HOST POINTER
   * there, and `FUN_0000ced2` feeds it to `terrain_chunk_visibility` as
   * `(short)`, so the low 16 bits of g_sys.rom's address -- ASLR-dependent,
   * different every run -- drove terrain visibility. Rows 4/63's class
   * (the sky dome's Z), latent until the attract playlist's case 20 was
   * wired up and gave this function its first caller.
   * Every other line of this block already matches the ROM exactly. */
  W[0x0CE4] = 0x12E89C;
  W[0x0CE8] = 0x4000;
  W[0x0CEC] = 0x1800;
  W[0x0CF0] = 0;
  W[0x0CF4] = 0x49;
  W[0x15EAC] = 0;
  W[0x15EB0] = 0;
  W[0x15EB4] = 0xf;
  W[0x0CC4] = 0x15;
  FUN_0000ced2();
  return;
}

/* ---- FUN_0000ced2 ---- */

void FUN_0000ced2(void)

{
  int iVar1;
  
  terrain_chunk_visibility(W[0x0CDC],W[0x0CE0],W[0x0CE4],W[0x0CEC]);
  terrain_props_dispatch();
  objects_render_master();
  W[0x0C8C] = W[0x0C8C] + 1;
  if (W[0x15EAC] < 0x40) {
    iVar1 = 0xffff;
  }
  else if (W[0x15EAC] < 0x140) {
    iVar1 = (0x13f - W[0x15EAC]) * 0x100;
  }
  else {
    iVar1 = 0;
  }
  if ((W[0x15EB0] != 0) && (0x5a - W[0x15EB0] < 0x40)) {
    iVar1 = (W[0x15EB0] + -0x1b) * 0x400;
  }
  sprite_draw_2d(0,0xfffffea7,0x80,0xc0,0x100,0x20,0x20,(short)iVar1,0);
  sprite_draw_2d(0,0xfffffea6,0x180,0xc0,0x100,0x20,0x20,iVar1,0);
  if (0xef < W[0x15EAC]) {
    g_fog_mode = 1;
    g_fog_r = 0xff;
    g_fog_g = 0xff;
    g_fog_b = 0xff;
    W[0x1703C] = 1;
    W[0x15ED0] = 4;
  }
  W[0x15EB4] = W[0x15EB4] + -1;
  if (W[0x15EB4] < 0) {
    if (W[0x1703C] == 0) {
      W[0xEB16] = W[0xEB16] - W[0x15ED0];
      if (W[0xEB16] < 0) {
        W[0xEB16] = 0;
      }
    }
    else {
      W[0xEB16] = W[0x15ED0] + W[0xEB16];
      if (0xff < W[0xEB16]) {
        W[0xEB16] = 0xff;
        W[0x15EB0] = W[0x15EB0] + 1;
        if (0x59 < W[0x15EB0]) {
          attract_advance_sub_state();
        }
      }
    }
    if (0x5a - W[0x15EB0] < 0x40) {
      g_fog_r = (0x5a - (short)W[0x15EB0]) * 4;
      g_fog_g = g_fog_r;
      g_fog_b = g_fog_r;
    }
  }
  if (W[0x15EAC] == 0xf0) {
    sound_hold_and_defer(0x0E);
  }
  W[0x15EAC] = W[0x15EAC] + 1;
  return;
}

/* ---- FUN_0000fb20 ---- */

/* ROM 0x00FB20: `subq.w #1,$e15f46 ; move.w $e15f46,d0 ; bgt ; move.w
 * $4(a7),-(a7) ; jsr $f394` -- count the 16-bit busy word down and, once it
 * reaches zero, play the sound id passed in (the transpile dropped the
 * argument). Its two callers push 0x2D (ROM 0x02BD18, 0x02F350). */
short FUN_0000fb20(int id)
{
  /* ROM 0x00FB20: `subq.w #1,$e15f46 ; move.w (a0),d0 ; bgt` -- a 16-bit
   * countdown at a 2-mod-4 offset (register row 166) -- then
   * `move.w $4(a7),-(a7) ; jsr sound_play`: the id is the CALLER's argument,
   * which the decompiler dropped (both ROM callers push #$2d). */
  short sVar1 = (short)(W16(0x15F46) - 1);
  W16_SET(0x15F46, sVar1);
  if (sVar1 < 1) sound_play((short)id);
  return sVar1;
}

/* ---- FUN_0000ffb2 ---- */

/* THE LOOP LIMITS HERE ARE 16-BIT BIG-ENDIAN ROM CONSTANTS (16 and 28).
 *
 * They were reading through rom_nr32(), a NATIVE-endian 32-bit read of a
 * big-endian ROM, so the limit came back as a huge byte-swapped number
 * instead of 28. `sVar2` is a short, so `sVar2 < <huge>` never goes false:
 * the loop spun forever, sVar2 wrapped negative, and it kept writing
 * g_sys.commsram[negative]. That is both the freeze right after
 * "entering main loop" AND the heap corruption behind the intermittent
 * "Aborted (core dumped)".
 *
 * CLAUDE.md fix #17 already documented this exact site as needing
 * (short)vrd16(); the later blanket tools/fix_addr_x4.py pass (269 sites)
 * reverted it to the rom_nr32 form. Restored here, and the commsram
 * indices are bounds-checked so a bad constant can never scribble again.
 */

void FUN_0000ffb2(void)

{
  short sVar1;
  short sVar2;
  
  sVar2 = (short)vrd16(0x356A0);
  if (W[0x0E0C] == 1) {
    /* The stem-mailbox clears are WORDS at 0xA04100 + d1*2 (ROM 0x00FFC4 and
     * 0x00FFE6: `clr.w (a0, d1.w*2)`) -- the old body wrote bytes at
     * 0x100 + d1, a different block entirely, so the Wind Woods stem
     * mailbox was never initialised. The stem state is a 16-bit field, not
     * a whole slot; and the final 1 is `move.w #$1,$a04226.l`, a big-endian
     * word -- the old byte write put 0x01 in the high byte (0x0100 to the
     * MCU). */
    if (W16(0x15F52) < 0) {
      sVar1 = 0x90;
      do {
        comms_w16(g_sys.commsram, 0x0100 + (unsigned)sVar1 * 2, 0);
        sVar1 = sVar1 + 1;
        sVar2 = (short)vrd16(0x356A0);
      } while (sVar1 < 0xa0);
    }
    else {
      sVar2 = 0x90;
      do {
        comms_w16(g_sys.commsram, 0x0100 + (unsigned)sVar2 * 2, 0);
        sVar2 = sVar2 + 1;
      } while (sVar2 < 0xa0);
      comms_w16(g_sys.commsram, 0x0226, 1);
      sVar2 = (short)vrd16(0x356A0);
    }
  }
  /* ROM 0x01001E / 0x010036: `andi.w #$7fff, (a0, d1.w*2)` off 0xA04000 --
   * clear bit 15 ("keep playing") on the COMMAND WORD of slots 17..27 and
   * 29..31 (the `bra` into the second loop's `addq` skips slot 28, the
   * balloon pop). The old body did `commsram[slot] &= 0x7fff` on a BYTE at a
   * byte index, which leaves the byte unchanged: these effects were never
   * stopped. */
  while (sVar2 = sVar2 + 1, sVar2 < (short)vrd16(0x3576C)) {
    if (sVar2 >= 0 && (unsigned)sVar2 * 2 + 1 < COMMSRAM_SIZE)
      comms_w16(g_sys.commsram, (unsigned)sVar2 * 2,
                comms_r16(g_sys.commsram, (unsigned)sVar2 * 2) & 0x7fffu);
  }
  while (sVar2 = sVar2 + 1, sVar2 < 0x20) {
    if (sVar2 >= 0 && (unsigned)sVar2 * 2 + 1 < COMMSRAM_SIZE)
      comms_w16(g_sys.commsram, (unsigned)sVar2 * 2,
                comms_r16(g_sys.commsram, (unsigned)sVar2 * 2) & 0x7fffu);
  }
  sound_params_reset();
  return;
}

/* ---- FUN_000231ec ---- */

/* Big-endian stores into g_sys.spriteram. sprite_hw.c reads that RAM as
 * BE 32-bit words, but this function wrote its 16-bit fields through
 * `undefined2 *` in host (little-endian) order, so every field arrived
 * byte-swapped. */
static void spr_w16(uint32_t off, uint32_t v)
{
    if (off + 1 >= SPRITERAM_SIZE) return;
    g_sys.spriteram[off]     = (uint8_t)(v >> 8);
    g_sys.spriteram[off + 1] = (uint8_t)v;
}
static void spr_w32(uint32_t off, uint32_t v)
{
    if (off + 3 >= SPRITERAM_SIZE) return;
    g_sys.spriteram[off]     = (uint8_t)(v >> 24);
    g_sys.spriteram[off + 1] = (uint8_t)(v >> 16);
    g_sys.spriteram[off + 2] = (uint8_t)(v >> 8);
    g_sys.spriteram[off + 3] = (uint8_t)v;
}

/* Bounded big-endian 16-bit read of a per-sprite ROM table. */
static uint16_t spr_rom16(uint32_t base, int idx)
{
    uint32_t a = base + (uint32_t)idx * 2;
    if (idx < 0 || a + 1 >= ROM_SIZE) return 0;
    return vrd16(a);
}

void FUN_000231ec(void)   /* sprite_update @ 0x231EC */

{
  /* SPRITE LIST -> SPRITE RAM. Never called, and mis-strided.
   *
   * sprite_draw_2d (game_core.c) builds the list in W[] with a 0x18-byte
   * record and a 0x1800-byte buffer:
   *     base  = 0x4AF0 + buf_sel * 0x1800
   *     entry = base + count * 0x18
   * This function read the SAME records with three different strides --
   * 0xc/0xc00, 6/0x600, and (correctly) 0x18/0x1800 -- the usual Ghidra
   * halving/quartering for 16- and 32-bit accessors. Unified to 0x18.
   *
   * It is also what resets the count (`W[0x4AEC] = 0`) and copies the
   * list into g_sys.spriteram, which is what sprite_hw.c actually draws.
   * The frame loop called sprite_dma_kick but never this, so:
   *   - the count climbed 0 -> 255 and stuck at 0xFF, after which
   *     sprite_draw_2d early-returned and dropped every later sprite;
   *   - g_sys.spriteram stayed empty, so the exact sprite layer drew
   *     NOTHING (spr=0 live vs spr=27170 in the recording) -- no namco
   *     logo, no game logo, no "INSERT COIN".
   * Documented main-loop order (annotations.md 0xBF2A):
   *   text_layer_update -> sprite_update -> palette_update
   */
  short sVar1;
  int iVar2;
  uint16_t uVar3;
  char cVar4;
  int iVar5;
  int iVar6;
  uint8_t bVar7;
  int iVar8;
  
  if ((_g_halt_flag == 0) && (W[0x0CAC] == 0)) {
    uVar3 = (uint16_t)W[0x0C98] & 3;
    iVar6 = 1;
    sVar1 = W[0x4B06 + ((short)uVar3 * 0x1800)];
    W_SET_HI16(0xAAF0, uVar3);   /* byte 0xAAF0: buf sel; LO half (0xAAF2) is target fog R */
    if (getenv("PROPCYCL_SPR2")) { static int n;
      if (n++ % 120 == 0)
        printf("    [SPR2] f=%d buf=%d head=%d count=%d halt=%ld CAC=%ld\n",
               (int)g_sys.frame_count, (int)(short)uVar3, (int)sVar1,
               (int)(int16_t)W[0x4AEC], (long)_g_halt_flag, (long)W[0x0CAC]); }
    while (iVar5 = (int)sVar1, iVar5 != 0) {
      iVar8 = (int)(short)W[0x4AF0 + ((short)uVar3 * 0x1800 + iVar5 * 0x18)];
      if (iVar8 < 0) {
        bVar7 = 0x80;
        iVar8 = -iVar8;
      }
      else {
        bVar7 = 0;
      }
      iVar2 = (int)(short)uVar3;
      { uint32_t _r = 0x4000 + (uint32_t)iVar6 * 0x10;   /* 16-byte record */
        spr_w16(_r + 0x0, (uint32_t)W[0x4AF2 + (iVar2 * 0x1800 + iVar5 * 0x18)]);
        spr_w16(_r + 0x2, (uint32_t)W[0x4AF4 + (iVar2 * 0x1800 + iVar5 * 0x18)]);
        spr_w16(_r + 0x4, (uint32_t)W[0x4AFC + (iVar2 * 0x1800 + iVar5 * 0x18)]);
        spr_w16(_r + 0x6, (uint32_t)W[0x4AFE + (iVar2 * 0x1800 + iVar5 * 0x18)]);
        spr_w16(_r + 0x8, 0xff);
        if (getenv("PROPCYCL_SPRREC")) { static int n;
          if (n++ < 6)
            printf("    [SPRREC] idx=%d  %04x %04x %04x %04x  ff %04x %04x %04x | attr %08x %04x\n",
                   iVar6,
                   (unsigned)(uint16_t)W[0x4AF2 + (iVar2*0x1800 + iVar5*0x18)],
                   (unsigned)(uint16_t)W[0x4AF4 + (iVar2*0x1800 + iVar5*0x18)],
                   (unsigned)(uint16_t)W[0x4AFC + (iVar2*0x1800 + iVar5*0x18)],
                   (unsigned)(uint16_t)W[0x4AFE + (iVar2*0x1800 + iVar5*0x18)],
                   (unsigned)spr_rom16(0xB79C0, iVar8),
                   (unsigned)spr_rom16(0xB7DBC, iVar8),
                   (unsigned)(uint16_t)W[0x4B00 + (iVar2*0x1800 + iVar5*0x18)],
                   (unsigned)W[0x4AF8 + (iVar2*0x1800 + iVar5*0x18)],
                   (unsigned)(uint16_t)((int16_t)spr_rom16(0xB81B8, iVar8) | (uint16_t)bVar7)); }
        spr_w16(_r + 0xa, spr_rom16(0xB79C0, iVar8));
        spr_w16(_r + 0xc, spr_rom16(0xB7DBC, iVar8));
        spr_w16(_r + 0xe, (uint32_t)W[0x4B00 + (iVar2 * 0x1800 + iVar5 * 0x18)]);
        spr_w32(0x20000 + (uint32_t)iVar6 * 8,
                (uint32_t)W[0x4AF8 + (iVar2 * 0x1800 + iVar5 * 0x18)]);
        spr_w16(0x20004 + (uint32_t)iVar6 * 8,
                (uint32_t)((int16_t)spr_rom16(0xB81B8, iVar8) | (uint16_t)bVar7)); }
      /* (the old native `undefined2 *` stores are gone -- they wrote
       * host-endian and clobbered the big-endian spr_w16 record above,
       * turning field 4 from 0x00ff into 0xff00 and so making the clip
       * index decode as 14 instead of 0, which selected an unwritten
       * clip window and discarded every sprite.) */
      /* ROM tables indexed by tile id. These were host-pointer derefs --
       * one even cast the pointer to `int` first, truncating it on 64-bit
       * (the segfault) -- and read the big-endian ROM natively. */
      if (W[0x4B02 + (iVar5 * 0x18 + iVar2 * 0x1800)] == 0) {
        cVar4 = (char)((int)W[0x4AF8 + ((short)uVar3 * 0x1800 + iVar5 * 0x18)] >> 0xd);
      }
      else {
        cVar4 = -1;
      }
      spr_w16(0x20006 + (uint32_t)iVar6 * 8, (uint32_t)(uint16_t)(short)cVar4);
      iVar6 = iVar6 + 1;
      sVar1 = W[0x4B06 + ((short)uVar3 * 0x1800 + iVar5 * 0x18)];
    }
    /* HEADER. sprite_hw.c reads:
     *     enable = !((W(0) >> 16) & 1)      bytes 0..1
     *     base   =   W(0) & 0xFFFF          bytes 2..3
     *     num    =  (W(1) >> 16) - base + 1 bytes 4..5
     * This wrote a single BYTE at offset 4, i.e. only the high half of the
     * 16-bit last-index, so num came out as count<<8, failed the
     * `num < 0x400` test and NO sprite was ever built. Records start at
     * index 1 (0x4010), so base = 1 and last = iVar6-1. */
    spr_w16(0x0000, 0x0006);                    /* flags: bit0 clear = on, ylow=0 */
    spr_w16(0x0002, 0x0001);                    /* base index (records start at 0x4010) */
    spr_w16(0x0004, (uint32_t)(uint16_t)(iVar6 - 1));   /* last index */
    /* The screen offset sprite_hw derives from words 1..3:
     *     g_dx = (W(1)&0xFFFF) + (W(2)&0xFFFF) + 0x2D
     *     g_dy = (W(3)>>16)    + 0x2A
     * These were never written, so g_dx/g_dy were 0x2D/0x2A and every
     * sprite landed ~600px off-screen -- the list built but drew nothing.
     * 0x0053 + 0x0200 + 0x2D = 0x280 and 0x0300 + 0x2A = 0x32A, which are
     * exactly the offsets sprite_draw_2d adds to x and y. Values match
     * the recording's header (dumps/attract_start/sprite_f231.bin:
     * W0=00060000 W1=00020053 W2=03000200 W3=03000000). */
    spr_w16(0x0006, 0x0053);
    spr_w32(0x0008, 0x03000200);
    spr_w32(0x000C, 0x03000000);
    /* CLIP WINDOWS at words 0x80..0x83 (bytes 0x200..0x20F). sprite_hw
     * clips every sprite to
     *     cxn = -g_dx + s16(W(0x80)>>16)   cxx = -g_dx + s16(W(0x80)&0xFFFF)
     *     cyn = -g_dy + s16(W(0x81)>>16)   cyx = -g_dy + s16(W(0x81)&0xFFFF)
     * Left at 0 these collapse to cxn = cxx = -g_dx, so `minx > maxx` and
     * EVERY sprite was discarded as offscreen -- sprite_collect returned
     * ni=0 even with a correct header and correctly decoded records.
     * The recording's values (sprite_f231.bin) are 0x028004ff / 0x032a0509,
     * i.e. exactly the full 640x480 screen after the offsets. */
    spr_w32(0x0200, 0x028004ff);
    spr_w32(0x0204, 0x032a0509);
    spr_w32(0x0208, 0x028004ff);
    spr_w32(0x020C, 0x032a0509);
    W[0x4AEC] = 0;
    W[0x4B04 + ((short)uVar3 * 0x1800)] = 0;
    W[0x4B06 + ((short)uVar3 * 0x1800)] = 0;
  }
  return;
}

/* ---- FUN_0003f36e ---- */

uint64_t FUN_0003f36e(void)

{
  /* void */;
  /* void */;
  
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- FUN_0003f574 ---- */

undefined4 FUN_0003f574(void)

{
  /* void */;
  
  return 0;
}

/* ---- FUN_00048352 ---- */

undefined4 FUN_00048352(void)

{
  return g_sys.dspram[0x002C];
}

/* ---- FUN_00049b6a ---- */

void FUN_00049b6a(void)

{
  mem_write32(0x600008, 0);
  mem_write32(0x60000A, 0x200);
  return;
}

/* ---- FUN_0004a21c ---- */

void FUN_0004a21c(void)

{
  W[0xC800] = 0x3030000;
  error_report_and_return();
  return;
}

/* ---- FUN_0004a22a ---- */

void FUN_0004a22a(void)

{
  W[0xC800] = 0x3040000;
  error_report_and_return();
  return;
}

/* ---- FUN_0004a4b2 ---- */

uint64_t FUN_0004a4b2(void)

{
  uint64_t in_stack_00000000;
  
  W[0xC800] = 0xa020000;
  error_code_save();
  return in_stack_00000000;
}

/* ---- FUN_0004a4c4 ---- */

uint64_t FUN_0004a4c4(void)

{
  uint64_t in_stack_00000000;
  
  W[0xC800] = 0xa030000;
  W[0xC838] = W[0xC838] & 0xcf | 0x20;
  error_code_save();
  return in_stack_00000000;
}

/* ---- remaining fixed stubs ---- */

/* ---- FUN_00008b6a @ 0x008B6A ---- */

void FUN_00008b6a(void)

{
  int iVar1;
  uint32_t uVar2;
  
  if ((W[0x2C0C] & 0x10) == 0) {
    iVar1 = 2;
  }
  else {
    iVar1 = 3;
  }
  W[0x0C80] = 0;
  for (uVar2 = 0; (int)uVar2 <= iVar1; uVar2 = uVar2 + 1) {
    if (((int)W16(0xE2C) >> (uVar2 & 0x3f) & 1U) != 0) {
      /* ROM 0x008B9E `move.b d2,$e00c7c(d0.w)`: a BYTE table at 0xE00C7C
       * indexed by the stage count. `(&W[0x0C7C])[n]` stepped intptr_t
       * slots -- 0x0C7D, 0x0C7E... which are not 4-aligned and never reach
       * work RAM -- so the table stayed empty and every stage read as course
       * 0. The four bytes live in slot 0x0C7C, big-endian, so pack them
       * there; the frame sync carries them to work RAM. */
      { int i = (int)(W[0x0C80] & 0xFFFF);
        if (i >= 0 && i < 4) {
            int sh = 8 * (3 - i);
            uint32_t v = (uint32_t)_W[0x0C7C];
            v = (v & ~(0xFFu << sh)) | (((uint32_t)uVar2 & 0xFFu) << sh);
            _W[0x0C7C] = (intptr_t)(int32_t)v;
        } }
      W[0x0C80] = W[0x0C80] + 1;
    }
  }
  W[0x0C82] = 0;
  iVar1 = 0;
  do {
    /* ROM 0x008BB6: clr.w $e00c56(d2.l*2) / clr.l $e00c5c(d2.l*4) */
    W16_SET(0x0C56 + iVar1 * 2, 0);
    W[0x0C5C + (iVar1) * 4] = 0;
    iVar1 = iVar1 + 1;
  } while (iVar1 < 3);
  sync_post();
  W[0x0C68] = 0;
  W16_SET(0x0C54, 0);
  W[0x0C6C] = 0;
  W[0x0C70] = 0;
  W[0x0C74] = 0;
  W[0x0C78] = 0;
  W[0x0C84] = 599;
  W[0x0C88] = 0;
  g_fog_mode = 3;
  W[0xEB16] = 0;
  W_SET_HI16(0xEB20, 0);
  bonus_model_init_simple();
  W[0x15F5C] = 0xf0;
  cgram_load_tile_block(0x1c,0x140);
  cz_load_color_ramp(0x1c, 5);   /* ROM 0x008C38 `move.b #$5,-(a7)` -- the palette argument was missing entirely */
  return;
}




/* ---- FUN_00008c82 @ 0x008C82 ---- */

void FUN_00008c82(void)

{
  short sVar1;
  uint32_t uVar2;
  short sVar3;
  int iVar4;
  undefined2 uVar6;
  int iVar5;
  uint32_t uVar7;
  undefined4 uVar8;
  int local_24;
  int local_1c [6];
  
  iVar5 = W[0x0C78] + -3;
  if (W[0x0C78] == 3) {
    FUN_0000a33e();
  }
  iVar4 = W[0x0C88] * 0x1c;
  if (iVar4 < 0) {
    iVar4 = iVar4 + 0xf;
  }
  FUN_0000e0e8((short)W[0x0C84],(short)((W[0x0C88] * 0x1c) / -0x10 + -0x188),
               (short)((iVar4 >> 4) + 0x130),0x2d0,(short)(-0x10 - W[0x0C88]),
               -(short)W[0x0C88]);
  sVar1 = W[0x0C82];
  if (W16(0x0C54) == 0) {
    if ((W_LO16(0x2BD8) & 2) != 0) {
      W[0x0C82] = W[0x0C82] + -1;
      if (W[0x0C82] < 0) {
        W[0x0C82] = 0;
      }
    }
    if (((W_LO16(0x2BD8) & 1) != 0) && (W[0x0C82] = W[0x0C82] + 1, W[0x0C80] <= W[0x0C82])
       ) {
      W[0x0C82] = W[0x0C80] + -1;
    }
    if ((W[0x2B3C] & 0x20) != 0) {
      W[0x0C82] = W[0x0C82] + -1;
      if (W[0x0C82] < 0) {
        W[0x0C82] = 0;
      }
    }
    if (((W[0x2B3C] & 0x10) != 0) &&
       (W[0x0C82] = W[0x0C82] + 1, W[0x0C80] <= W[0x0C82])) {
      W[0x0C82] = W[0x0C80] + -1;
    }
    if (sVar1 != W[0x0C82]) {
      W[0x15F5C] = (short)W[0x0C78] + 0x78;
      sound_play(0x10);
    }
    tilemap_scroll_set((uint16_t)W[0x0C98] & 0x48);
    /* ROM 0x008DD8: `move.w (a0),d0` with a0 = 0xE00C82 (the 16-bit cursor),
     * then `move.b $e00c7c(d0.w),d0 ; extb.l d0` -- a BYTE table in work RAM
     * indexed by the cursor. `(&W[0x0C7C])[idx]` stepped intptr_t slots
     * instead, i.e. 8 host bytes per cursor step, so every course but the
     * first read the wrong byte. */
    { int i = (int)(W[0x0C82] & 0xFFFF);            /* the 16-bit cursor */
      W[0x0E0C] = (i >= 0 && i < 4)
                  ? (uint32_t)(int8_t)(((uint32_t)_W[0x0C7C] >> (8 * (3 - i))) & 0xFF) : 0;
      { static int n; extern int g_stagedbg;
        if (g_stagedbg && n++ < 6)
          fprintf(stderr, "[STAGE] cursor=%d max=%ld avail=%08lX table=%08lX -> course=%ld\n",
                  i, (long)W[0x0C80], (long)W16(0xE2C) & 0xFFFFFFFF,
                  (unsigned long)(uint32_t)_W[0x0C7C], (long)W[0x0E0C]); } }
  }
  if ((W[0x2C0C] & 0x10) != 0) {
    if ((W[0x2B3A] & 0x80) != 0) {
      W[0x15F60] = W[0x15F60] + 1 & 3;
      debug_draw_value(5,6,1,0xa190,W[0x15F60] + 1,0xa199,2);
    }
    if ((W[0x2B3A] & 0x40) != 0) {
      W[0x15F60] = W[0x15F60] + 3 & 3;
      debug_draw_value(5,6,1,0xa190,W[0x15F60] + 1,0xa199,2);
    }
  }
  if (W16(0x0C54) != 0) {
    W[0x0C88] = W[0x0C88] + 8;
    if (0x100 < W[0x0C88]) {
      W[0x0C88] = 0x100;
    }
    if (W[0x0C68] == 0x80) {
      if (W16(0xE10) == 0) {
        uVar6 = 0x6e;
      }
      else {
        uVar6 = 0x6f;
      }
      sound_play_p(CONCAT22(0x26,uVar6));
    }
    W[0x0C68] = W[0x0C68] + 1;
    if (0x100 < W[0x0C68]) {
      W[0x0CC0] = 2;
    }
    if ((int)(W[0x0E0C] * -0x120) < W[0x0C6C]) {
      iVar4 = W[0x0E0C] * -0x120 - W[0x0C6C];
      if (iVar4 < 0) {
        iVar4 = iVar4 + 0xf;
      }
      W[0x0C6C] = (iVar4 >> 4) + -1 + W[0x0C6C];
    }
    if ((W[0x2B3A] & 0x100) != 0) {
      W[0x0CC0] = 2;
    }
    if ((W[0x2BA6] & 1) != 0) {
      W[0x0CC0] = 2;
    }
  }
  if ((W[0x2B3A] & 0x100) != 0) {
    if (W16(0x0C54) != 1) {
      sound_play(0x11);
    }
    W16_SET(0x0C54, 1);
    tilemap_scroll_set(0);
  }
  if ((W[0x2BA6] & 1) != 0) {
    if (W16(0x0C54) != 1) {
      sound_play(0x11);
    }
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
  if ((W16(0xE10) == 0) && (-1 < iVar5)) {
    if (W16(0x0C54) == 0) {
      /* The "VERY HARD" badge on the Level 3 plate. Ghidra dropped the last
       * TWO arguments (register row 62's class), so it drew tile code 0 with
       * palette 0 -- on screen, the digits "012 / 345" where the badge belongs.
       * The four blink blocks 0x10b..0x10e are 3x2 and their tile codes run
       * 0x120..0x137 consecutively, so base = 0x120 + i*6, palette 1.
       * Measured against MAME's own tilemap at fc 2266/2280/2288/2296/2304/2600
       * -- all four blink phases, 6/6 exact (tools/overnight/dump_menu_feed.lua). */
      text_draw_rect_blink((short)((W[0x0C98] >> 3 & 3) + 0x10b), 0x21, 9,
                           0x120 + (W[0x0C98] >> 3 & 3) * 6, 1);
    }
    else {
      text_draw_rect_solid(0x10b,0x21,9);
    }
  }
  if (W16(0x0C54) == 0) {
    text_draw_rect_blink(0x1c, 0x13, 0x1c, 0x140, 5);
  }
  else {
    text_draw_rect_solid(0x1c,0x13,0x1c);
  }
  dsp_cmd_set_camera(0,0x37d,0,0,0x350,0,0,0,0);
  if (W16(0xE10) == 0) {
    dsp_cmd_set_camera(0,0x366,0,0,0x350,0,0,0,0);
    uVar8 = 0x372;
  }
  else {
    dsp_cmd_set_camera(0,W16(0xE66) + 0x36e,0,0,0x350,0,0,0,0);
    uVar8 = 0x364;
  }
  dsp_cmd_set_camera(0,uVar8,0,0,0x350,0,0,0,0);
  /* THE SELECTED PLATE'S SWAY (ROM 0x009120..0x009220). Per plate n the
   * machine keeps a 16-bit AMPLITUDE at 0xE00C56 + n*2 (`move.w
   * $e00c56(d1.l*2)`) and a 32-bit PHASE at 0xE00C5C + n*4 (`addq.l #1,
   * $e00c5c(d0.l*4)`); the plate's Z angle is sin(amp * (sin(phase << 9) >>
   * 10) >> 4) -- a sine VALUE used as the angle, which bounds the rock at
   * about +-4.3 deg -- and the amplitude ramps 0 -> 0x80 while the plate is
   * selected. The transpile incremented the amplitude at stride 1, clamped
   * it at stride 4 (a different slot) and advanced the phase at stride 1:
   * for Level 2/3 the amplitude never met its clamp and the sway grew
   * without bound, and Level 3's phase sat on a 2-mod-4 slot the _W[] sync
   * rebuilds every frame. */
  uVar7 = W[0x0E0C];
  { int n = (int)W[0x0E0C];
    iVar5 = (int)(int16_t)W16(0x0C56 + n * 2) *
            ((int)(short)vrd16s(0x20B004 + ((uint32_t)(W[0x0C5C + n * 4] << 9) >> 1 & 0x7ffe) * 2) >> 10);
    if (iVar5 < 0) {
      iVar5 = iVar5 + 0xf;
    }
    sVar1 = vrd16s(0x20B004 + (iVar5 >> 5 & 0x7ffe) * 2);
    iVar5 = W[0x0E0C] * 0x120 + -0x120;
    if (W16(0x0C54) == 0) {                                       /* 0x0091A0 */
      W16_SET(0x0C56 + n * 2, (int16_t)W16(0x0C56 + n * 2) + 1);
      if (0x80 < (int16_t)W16(0x0C56 + n * 2)) W16_SET(0x0C56 + n * 2, 0x80);
    }
    else {                                                        /* 0x009182 */
      W16_SET(0x0C56 + n * 2, (int16_t)W16(0x0C56 + n * 2) - 1);
      if ((int16_t)W16(0x0C56 + n * 2) < 0) W16_SET(0x0C56 + n * 2, 0);
    }
    dsp_cmd_set_camera(4,(0x353 - W16(0xE10)) + W[0x0E0C] * 2,W[0x0C6C] + iVar5,0x7c,0x350,0,
                       0,sVar1,0);
    dsp_cmd_set_camera(3,0x358,W[0x0C6C] + 0x10 + iVar5,0x6c,0x350,0,0,(int)sVar1,0);
    { extern int g_stagedbg; if (g_stagedbg) printf("[SWAY] f%d n=%d amp=%d phase=%ld ang=%d confirm=%d\n",
        (int)g_sys.frame_count, n, (int)(int16_t)W16(0x0C56 + n * 2), (long)W[0x0C5C + n * 4], (int)sVar1, (int)W16(0x0C54)); }
    W[0x0C5C + n * 4] = W[0x0C5C + n * 4] + 1; }
  iVar5 = -0x120;
  uVar7 = 0;
  do {
    if (W16(0xE10) == 0) {
      local_1c[0] = 0x353;
    }
    else {
      local_1c[0] = 0x352;
    }
    iVar4 = (int)(int16_t)W16(0x0C56 + uVar7 * 2) *
            ((int)(short)vrd16s(0x20B004 + ((uint32_t)(W[0x0C5C + (uVar7) * 4] << 9) >> 1 & 0x7ffe) * 2) >> 10);
    if (iVar4 < 0) {
      iVar4 = iVar4 + 0xf;
    }
    sVar1 = vrd16s(0x20B004 + (iVar4 >> 5 & 0x7ffe) * 2);
    if (uVar7 != W[0x0E0C]) {
      W16_SET(0x0C56 + uVar7 * 2, (int16_t)W16(0x0C56 + uVar7 * 2) - 1);   /* 0x00929C */
      if ((int16_t)W16(0x0C56 + uVar7 * 2) < 0) {
        W16_SET(0x0C56 + uVar7 * 2, 0);
      }
      if ((W16(0x0C54) == 0) || (iVar4 = W[0x0C68] + uVar7 * -10 + -10, iVar4 < 0)) {
        iVar4 = 0;
      }
      sVar3 = (short)iVar4;
      sVar1 = sVar1 + sVar3 * ((short)(iVar5 / -4) + 0x70) * -2;
      sVar3 = sVar3 * sVar3;
      /* ROM 0x0092F4: `pea ([$ffe8,a6], d2.l*2)` -- a 68020 MEMORY-INDIRECT ea.
       * It loads the long at a6-0x18 (the base model id, 0x353 NOVICE / 0x352
       * ADVANCED, set at 0x009238/0x00923E) and ADDS d2*2. Ghidra emitted a
       * dereference of it instead, and `(int)local_1c` truncated the 64-bit
       * stack pointer -- SIGSEGV on entry to the stage select. Rows 30/90/92. */
      dsp_cmd_set_camera(3,local_1c[0] + uVar7 * 2,iVar5,0x7c - sVar3,0x350,0,0,
                         sVar1,0);
      if (((int)W16(0xE2C) >> (uVar7 & 0x3f) & 1U) == 0) {
        local_1c[5] = sVar1 + 0x2000;
        /* 0x009322/0x00933E: movea.w $2000(a0) / $2002(a0) off d5 =
         * 0x20B004 -- sin and cos of 45 deg as BE16 words, not single bytes */
        local_1c[1] = -((int)vrd16s(0x20D004) * 0xdc >> 0xf);
        local_1c[2] = 0xdc - ((int)vrd16s(0x20D006) * 0xdc >> 0xf);
        uVar2 = sVar1 + 0x2000 >> 1;
        local_1c[3] = (local_1c[1] * (short)vrd16s(0x20B006 + (uVar2 & 0x7ffe) * 2) >> 0xf) -
                      (local_1c[2] * (short)vrd16s(0x20B004 + (uVar2 & 0x7ffe) * 2) >> 0xf);
        local_1c[4] = (local_1c[2] * (short)vrd16s(0x20B006 + (uVar2 & 0x7ffe) * 2) >> 0xf) +
                      (local_1c[1] * (short)vrd16s(0x20B004 + (uVar2 & 0x7ffe) * 2) >> 0xf);
        /* ROM 0x0093F8: `pea ([$fff4,a6], d4.l)` -- same mode, same fix:
         * the long at a6-0x0c (local_1c[3]) PLUS d4, not a load from it. */
        dsp_cmd_set_camera(4,0x33f,local_1c[3] + iVar5,
                           ((short)local_1c[4] - sVar3) + 0x7c,0x350,0,0,sVar1 + 0x2000,0);
      }
      dsp_cmd_set_camera(3,0x358,iVar5 + 0x10,0x6c - sVar3,0x350,0,0,sVar1,0);
      if (W16(0x0C56 + uVar7 * 2) == 0) {                              /* 0x009434 tst.w */
        W[0x0C5C + (uVar7) * 4] = 0;
      }
      else {
        W[0x0C5C + (uVar7) * 4] = W[0x0C5C + (uVar7) * 4] + 1;
      }
    }
    iVar5 = iVar5 + 0x120;
    uVar7 = uVar7 + 1;
  } while ((int)uVar7 < 3);
  iVar5 = ((int)(short)vrd16s(0x20B004 + ((W[0x0C78] << 0xb) >> 1 & 0x7ffe) * 2) >> 0xb) + 0x150;
  if (W[0x0E0C] * 0xc0 != W[0x0C70]) {
    iVar4 = W[0x0E0C] * 0xc0 - W[0x0C70];
    if (iVar4 < 0) {
      iVar4 = iVar4 + 0xf;
    }
    W[0x0C70] = (iVar4 >> 4) + 1 + W[0x0C70];
  }
  W[0x0C74] = -(int)(short)vrd16s(0x20B004 + ((W[0x0C70] << 0xf) / 0xc0 >> 1 & 0x7ffe) * 2) >> 9;
  if (W16(0x0C54) != 0) {
    if (W[0x0C68] < 0x20) {
      iVar4 = W[0x0C68] << 3;
    }
    else {
      iVar4 = 0x100;
    }
    W[0x0C74] = iVar4 + W[0x0C74];
  }
  sprite_draw_2d(2,0x157,W[0x0C70] + W[0x0C68] * 4,(short)(W[0x0C74] + iVar5),0x100,0x20,
                 0x20,0,0);
  sprite_draw_2d(2,0x158,W[0x0C70] + W[0x0C68] * 4,W[0x0C74] + iVar5 + 0x100,0x100,0x20,
                 0x20,0,0);
/* Native little-endian reads of BIG-ENDIAN ROM tables (register row 52's
 * class, the spelling the 101-site sweep missed: an ADDRESS EXPRESSION
 * rather than a bare constant). Measured on the stage-select screen:
 * the table at 0x35070 holds [0x0195,0x0194,0x0196,0x0194] and this read
 * returned 0x9601 -- a sprite id 27135 below zero, so sprite_update
 * indexed its three per-sprite ROM tables far out of range and the board
 * drew as 8x7 tiles of garbage instead of the 3x2 level plate. */
  if (W16(0x0C54) == 0) {
    sprite_draw_2d(2,vrd16s(0x35070 + (W[0x0C78] >> 5 & 3U) * 2),0xc0,0x170,0,0x20
                   ,0x20,0,0);
    sprite_draw_2d(2,0x1ab,0x98,0x1bc,0,0x20,0x20,0,0);
    sprite_draw_2d(2,vrd16s(0x35078 + (W[0x0C78] >> 5 & 3U) * 2),0x150,0x180,0,
                   0x20,0x20,0,0);
  }
  if (W16(0xE10) != 0) {
    local_24 = vrd32s(0x15C328 +
                       (W16(0xE64) * 6 + W[0x0E0C] * 0x12 + W[0x3FFA] * 2) * 4);
    iVar5 = 0x87;
    iVar4 = 0;
    do {
      dsp_cmd_set_camera(2,local_24 % 10 + 0x347,iVar5,0,0x1c0,0,0,0,0);
      dsp_cmd_set_camera(2,0x351,iVar5 + 4,0xfffffffb,0x1c0,0,0,0,0);
      local_24 = local_24 / 10;
      iVar5 = iVar5 + -0x2b;
      iVar4 = iVar4 + 1;
    } while (iVar4 < 5);
  }
  if (W[0x0C78] == 10) {
    sound_play_p(0x0026006A);
  }
  if ((W16(0x0C54) != 1) && (W[0x0C78] == W[0x15F5C])) {
    sound_play_p(0x0026006C);
  }
  W[0x0C78] = W[0x0C78] + 1;
  return;
}




/* ---- FUN_0000971a @ 0x00971A ---- */

void FUN_0000971a(void)

{
  W16_SET(0xE66, 3);
  W[0x0E0C] = 3;
  W16_SET(0xE64, 0);
  W[0x15F60] = 0xffff;
  W[0x0C68] = 0;
  W[0x0C78] = 0;
  g_fog_mode = 3;
  W[0xEB16] = 0;
  W[0x1703C] = 0;
  W_SET_HI16(0xEB20, 0);
  set_background_color(0,0,0);
  sync_post();
  cgram_load_tile_block(0xf2,0x120);
  cz_load_color_ramp(0xf2, 1);   /* ROM 0x00977E `move.b #$1,-(a7)` -- the palette argument was missing entirely */
  return;
}




/* ---- FUN_00009792 @ 0x009792 ---- */

void FUN_00009792(void)

{
  dsp_cmd_set_camera(0,0x37d,0,0,0x350,0,0,0,0);
  dsp_cmd_set_camera(0,W16(0xE66) + 0x36e,0,0,0x350,0,0,0,0);
  dsp_cmd_set_camera(0,0x361,0,0,0x350,0,0,0,0);
  sprite_draw_2d(6,0x1e7,0x30,0x90,0,0x20,0x20,0,0);
  sprite_draw_2d(6,0x1e8,0x30,0x16c,1,0x20,0x20,0,0);
  text_draw_rect_blink(0xf2, 0xd, 0xc, 0x120, 1);
  W[0x0C78] = W[0x0C78] + 1;
  if (0x100 < W[0x0C78]) {
    W[0x1703C] = 1;
    W[0x15ED0] = 4;
  }
  if ((W[0x1703C] != 0) && (W[0xEB16] = W[0x15ED0] + W[0xEB16], 0xff < W[0xEB16])) {
    W[0xEB16] = 0xff;
    W[0x0CC0] = 2;
  }
  return;
}




/* ---- FUN_000098a4 @ 0x0098A4 ---- */

void FUN_000098a4(void)

{
  sync_post();
  W[0x0C68] = 0;
  W[0x0C78] = 0;
  g_fog_mode = 3;
  W[0xEB16] = 0;
  W_SET_HI16(0xEB20, 0);
  set_background_color(0,0,0);
  return;
}




/* ---- FUN_0000a0f6 @ 0x00A0F6 ---- */

uint32_t FUN_0000a0f6(int param_1)

{
  int iVar1;
  int iVar2;
  int puVar3;
  int iVar4;
  int iVar5;
  int iVar6;
  int iVar7;
  int iVar8;
  uint32_t uVar9;
  int bVar10;
  uint32_t uVar11;
  int iVar12;
  int iVar13;
  int iVar14;
  int iVar15;
  undefined4 uVar16;
  
  iVar14 = W[0x0E0C] * 0x20;
  iVar1 = vrd32s(0x3508C + iVar14);
  iVar2 = vrd32s(0x35090 + iVar14);
  puVar3 = vrd32s(0x35094 + iVar14);
  iVar4 = vrd32s(0x35098 + iVar14);
  iVar5 = vrd32s(0x3509C + iVar14);
  iVar6 = vrd32s(0x350A0 + iVar14);
  iVar7 = vrd32s(0x350A4 + iVar14);
  iVar8 = vrd32s(0x350A8 + iVar14);
  uVar11 = W[0x15FE4] << 3;
  iVar14 = W[0x16008] + W[0x15FE4] * 0x20;
  for (iVar13 = 0; iVar13 < W[0x15FF0]; iVar13 = iVar13 + 1) {
    bVar10 = false;
    iVar15 = iVar4 * (*(int *)(iVar14 + 4) >> 8);
    if (iVar15 < 0) {
      iVar15 = iVar15 + 0xff;
    }
    iVar15 = iVar1 + (iVar15 >> 8);
    if ((iVar5 < iVar15) || (iVar15 < iVar6)) {
      bVar10 = true;
    }
    iVar12 = iVar4 * (*(int *)(iVar14 + 0xc) >> 8);
    if (iVar12 < 0) {
      iVar12 = iVar12 + 0xff;
    }
    uVar9 = iVar2 + (iVar12 >> 8);
    if ((iVar7 < (int)uVar9) || ((int)uVar9 < iVar8)) {
      bVar10 = true;
    }
    uVar11 = uVar9;
    if ((*(int *)(iVar14 + 0x10) < 1) && (uVar11 = W[0x0C98] & 0x18, uVar11 == 0)) {
      uVar16 = 0x391;
    }
    else {
      uVar16 = 0x390;
    }
    if (!bVar10) {
      uVar11 = -(int)W_A16(0x478C, iVar13);
      if (0 < (int)uVar11) {
        uVar11 = dsp_cmd_place_object_rotated_abs(2,uVar16,iVar15,uVar9,puVar3,0,0,0,0);
      }
      if (W_A16(0x478C, iVar13) == 0) {
        uVar11 = dsp_cmd_place_object_rotated_abs(2,uVar16,iVar15,uVar9,puVar3,0,0,0,0);
      }
    }
    iVar14 = iVar14 + 0x20;
  }
  for (iVar14 = 0; iVar14 < W[0x15FEC]; iVar14 = iVar14 + 1) {
    bVar10 = false;
    uVar11 = (&g_obj_angle_delta)[iVar14 * 0x3c] * param_1 * 0x40 >> 1;
    iVar13 = iVar4 * ((&g_obj_base_x)[iVar14 * 0x3c] +
                      ((&g_obj_speed)[iVar14 * 0x3c] *
                       ((int)(short)vrd16s(0x20B006 + (uVar11 & 0x7ffe) * 2) >> 7) >> 8) >> 8);
    if (iVar13 < 0) {
      iVar13 = iVar13 + 0xff;
    }
    iVar13 = iVar1 + (iVar13 >> 8);
    iVar15 = iVar4 * ((&g_obj_base_z)[iVar14 * 0x3c] +
                      ((&g_obj_speed)[iVar14 * 0x3c] *
                       ((int)(short)vrd16s(0x20B004 + (uVar11 & 0x7ffe) * 2) >> 7) >> 8) >> 8);
    if (iVar15 < 0) {
      iVar15 = iVar15 + 0xff;
    }
    iVar15 = iVar2 + (iVar15 >> 8);
    if ((iVar5 < iVar13) || (iVar13 < iVar6)) {
      bVar10 = true;
    }
    if ((iVar7 < iVar15) || (iVar15 < iVar8)) {
      bVar10 = true;
    }
    uVar11 = iVar14 * 0x3c;
    if (((int)W[0x4924 + (iVar14 * 0x3c)] < 1) && (uVar11 = W[0x0C98] & 0x18, uVar11 == 0))
    {
      uVar16 = 0x391;
    }
    else {
      uVar16 = 0x390;
    }
    if (!bVar10) {
      uVar11 = dsp_cmd_place_object_rotated_abs(2,uVar16,iVar13,iVar15,puVar3,0,0,0,0);
    }
  }
  return uVar11;
}





/* ---- FUN_0000e0e8 @ 0x00E0E8 ---- */

void FUN_0000e0e8(int param_1,int param_2,undefined4 param_3,int param_4,int param_5,int param_6)

{
  int iVar1;
  int iVar2;
  int iVar3;
  uint32_t uVar4;
  uint32_t uVar5;
  undefined4 uVar6;
  
  if (param_1 < 0) {
    param_1 = 0;
  }
  if (5999 < param_1) {
    param_1 = 5999;
  }
  param_1 = param_1 + 0x3b;
  if ((param_1 < W[0x0E44]) || (param_1 < 600)) {
    W[0x15ED5] = W[0x15ED5] + 1 & 0xf;
    if (299 < param_1) goto LAB_0000e15c;
  }
  else if (W[0x15ED5] == 0) goto LAB_0000e15c;
  W[0x15ED5] = W[0x15ED5] + 1 & 0xf;
LAB_0000e15c:
  iVar1 = (param_1 / 0x3c) % 10;
  uVar4 = (param_1 / 600) % 10;
  iVar3 = param_1 % 0x3c;
  if (iVar3 < 0xf) {
    uVar6 = 0xfffffffd;
  }
  else {
    uVar6 = 0xfffffffe;
  }
  sprite_draw_2d(5,uVar6,param_5,param_6,param_4 + 0x931c,0x20,0x20,0,1);
  /* HOST POINTER + NATIVE READ OF A BIG-ENDIAN ROM TABLE, both at once.
   * `*(short *)((intptr_t)&g_sys.rom[0x35494] + N)` reads the BE table 0x35494
   * little-endian, so the tile id 0x0004 came back as 0x0400 = 1024. Measured:
   * this call draws the left HUD panel at screen (144,16); the real game's
   * record there carries t1=0x0033 t2=0x003A, i.e. tile id 4, and ours carried
   * t1=0x0074 t2=0x0009, i.e. tile id 1020 -- a garbage tile at the very end
   * of the table, which is what the noise block was. */
  sprite_draw_2d(5,-(int)vrd16s(0x35494 + (int8_t)W[0x15ED5] * 2),param_5 + 0xa0,
                 param_6 + 0x10,param_4 + 0x91dc,0x20,0x20,0,1);
  if (iVar3 < 0x10) {
    iVar2 = iVar3 << 0xb;
  }
  else {
    iVar2 = 0x8000;
  }
  uVar5 = uVar4;
  if (((iVar1 == 0) && (iVar3 < 0x10)) && (0x3b < param_1)) {
    dsp_cmd_place_object_rotated_abs(1,(uVar4 + 9) % 10 + 0x265,param_2,param_3,param_4,iVar2,0,0,0)
    ;
    uVar5 = (uVar4 + 9) % 10;
  }
  dsp_cmd_place_object_rotated_abs(1,uVar5 + 0x25b,param_2,param_3,param_4,0,0,0,0);
  dsp_cmd_place_object_rotated_abs(1,uVar4 + 0x265,param_2,param_3,param_4,0,0,0,0);
  dsp_cmd_place_object_rotated_abs(1,0x26f,param_2,param_3,param_4,0,0,0,0);
  param_2 = param_2 + 0x58;
  dsp_cmd_place_object_rotated_abs(1,(iVar1 + 9U) % 10 + 0x265,param_2,param_3,param_4,iVar2,0,0,0);
  dsp_cmd_place_object_rotated_abs(1,iVar1 + 0x265,param_2,param_3,param_4,0,0,0,0);
  if ((iVar3 < 0x10) && (0x3b < param_1)) {
    dsp_cmd_place_object_rotated_abs(1,(iVar1 + 9U) % 10 + 0x25b,param_2,param_3,param_4,0,0,0,0);
  }
  dsp_cmd_place_object_rotated_abs(1,0x26f,param_2,param_3,param_4,0,0,0,0);
  return;
}





/* ---- FUN_0000e5fc @ 0x00E5FC ---- */



uint32_t FUN_0000e5fc(void)

{
  uint16_t uVar1;
  uint32_t uVar2;
  uint16_t uVar3;
  uint32_t uVar4;
  undefined2 uVar5;
  undefined2 uVar6;
  
  /* ROM 0x00E648 `move.w $120df4(a0.l*2),d4` -- a BIG-ENDIAN word per
   * 0xC000 cell (bit 0: off-route flag, the rest: the route heading). Read
   * host-native it came back byte-swapped, so the direction plate yawed to a
   * nonsense heading (register row 115's class). */
  uVar1 = (uint16_t)vrd16(0x120DF4 +
           (uint32_t)(W[0x0E0C] * 0x200 + W[0x0D00] / 0xc000 + (W[0x0D08] / 0xc000) * 0x10)
           * 2);
  if ((uVar1 & 1) == 0) {
    W[0x15EDC] = W[0x15EDC] + -0xa0;
    if (W[0x15EDC] < 0) {
      W[0x15EDC] = 0;
    }
    /* 0x00E680..0x00E68C: `movea.w d4 ; suba.w (0xE15ED8) ; asr.l #4 ;
     * add.w` -- the route heading is a 16-bit word in the HIGH half of its
     * slot and the on-route flag 0xE15EDA the LOW half (move.w / clr.w) */
    W16_SET(0x15ED8, (int16_t)(((int)(int16_t)uVar1 - (int)(int16_t)W16(0x15ED8)) >> 4)
                     + (int16_t)W16(0x15ED8));
    W16_SET(0x15EDA, 1);
  }
  else {
    W[0x15EDC] = W[0x15EDC] + 0xa0;
    if (0xa80 < W[0x15EDC]) {
      W[0x15EDC] = 0xa80;
    }
    W16_SET(0x15EDA, 0);
  }
  sprite_draw_2d(5,0xfffffff4,0x10d,(short)-(W[0x15EDC] >> 4),0xab50,0x20,0x20,0,1);
  uVar3 = (uint16_t)((int16_t)W16(0x15ED8) + (int32_t)W[0x0D10]);   /* 0x00E6C4..0x00E6D2 */
  /* SKY SPHERE (model 0x322) placement.
   *
   * The Z coordinate was `&g_sys.rom[0x01900]` -- a HOST POINTER where a
   * coordinate belongs, the classic "ROM address used as a pointer"
   * transpilation bug. Truncated to int it gave ~2^30 garbage, which put
   * the sky 2,054,644,778 units away (and it is the scene background, so
   * losing it is very visible).
   *
   * It is an immediate: 0x1900 = 6400. Confirmed against the recording --
   * with x=0, y=0x8c0=2240, z=6400 the distance is
   *     sqrt(2240^2 + 6400^2) = 6780.7
   * and the recording's sky record (code 871) sits at |t| = 6780. */
  uVar2 = dsp_cmd_place_object_rotated_abs
                    (1,0x322,0,W[0x15EDC] + 0x8c0,0x1900,0,uVar3,0x4000,1);
  if ((((uVar3 < 0xb000) && (0x5000 < uVar3)) && (uVar2 = 0, (W[0x0C98] & 0x28) != 0)) &&
     (uVar2 = (uint32_t)(uVar1 & 1), (uVar1 & 1) == 0)) {
    uVar2 = sprite_draw_2d(1,0xfffffe9f,0xd0,0x70,0,0x20,0x20,0x4000,1);
  }
  if (W16(0xE10) != 0) {
    return uVar2;
  }
  uVar2 = CONCAT22((short)(uVar2 >> 0x10),uVar1) & 0xffff0001;
  if ((uVar1 & 1) == 0) {
    if ((uVar3 < 0xb000) && (0x9000 < uVar3)) {
      if ((W[0x0C98] & 0x30) == 0) {
        uVar2 = 0;
        uVar4 = uVar2;
      }
      else {
        uVar2 = 0xffffffff;
        uVar4 = uVar2;
      }
      goto LAB_0000e79c;
    }
    if ((0x5000 < uVar3) && (uVar3 < 0x7000)) {
      uVar2 = (uint32_t)((W[0x0C98] & 0x30) != 0);
      uVar4 = uVar2;
      goto LAB_0000e79c;
    }
  }
  uVar4 = 0;
LAB_0000e79c:
  if (W[0x0E18] != 0) {
    uVar4 = 0;
  }
  if (uVar4 != W[0x15F28]) {
    if ((int)uVar4 < 1) {
      if ((int)uVar4 < 0) {
        text_draw_rect_blink(0x111, 3, 0xe, 0x220, 11);
      }
      else {
        text_draw_rect_solid(0x111,3,0xe);
      }
      uVar6 = 0x1f;
      uVar5 = 0x112;
    }
    else {
      text_draw_rect_blink(0x112, 0x1f, 0xe, 0x240, 11);
      uVar6 = 3;
      uVar5 = 0x111;
    }
    uVar2 = text_draw_rect_solid(uVar5,uVar6,0xe);
  }
  W[0x15F28] = uVar4;
  return uVar2;
}





/* ---- FUN_0000e9f2 @ 0x00E9F2 ---- */

void FUN_0000e9f2(void)

{
  if (W[0x0E18] == 0) {
    if (W[0x15EE4] < 0) {
      return;
    }
    if (W[0x15EE8] < 0x10) {
      FUN_0000eaaa(W[0x15EE8]);
    }
    else {
      /* ROM 0x00EA3C..0x00EA62.  Two things were lost here:
       *
       *  - the BLOCK INDEX is `move.l $354d4(d0.l*4)` -- BE32, stride 4,
       *    giving blocks 166..173 for messages 0..7, each a 26x3 tilemap
       *    block.  Read as one byte at stride 1 it gave block 0 for every
       *    message but #3, and block 0's descriptor is 38x3 -- which is
       *    exactly the `38x3` the banner picker reported.  38 tiles per
       *    row stamped from the base where 26 belong is what garbled it.
       *
       *  - the BLINK: `moveq #$18,d0 ; and.l (a3),d0 ; beq` picks base
       *    0x120 when W[0x15EE8] & 0x18 is set and 0x170 when it is not.
       *    The transpile kept only the 0x170 arm, so the caption never
       *    alternated -- in a function called text_draw_rect_blink. */
      text_draw_rect_blink(vrd32s(0x354D4 + W[0x15EE4] * 4), 7, 0x14,
                           (W[0x15EE8] & 0x18) ? 0x120 : 0x170, 7);
    }
    W[0x15EE8] = W[0x15EE8] + 1;
    if (W[0x15EE8] < 0xf1) {
      return;
    }
    /* ROM 0x00EA84, same BE32/stride-4 read as the blink above. */
    text_draw_rect_solid(vrd32s(0x354D4 + W[0x15EE4] * 4),7,0x14);
    /* ROM 0x00EA94 `subq.l #$1,$e15eec(d0.l*4)` -- stride 4, not the
     * host-pointer stride `(&W[base])[N]` gives on an intptr_t[]. */
    W[0x15EEC + W[0x15EE4] * 4] = W[0x15EEC + W[0x15EE4] * 4] + -1;
  }
  else {
    /* ROM 0x00EA18 `move.l (a4),-(a7)` with a4 = 0x354D4: entry 0 of the
     * same BE32 table, i.e. block 166 -- not the byte 0. */
    text_draw_rect_solid(vrd32s(0x354D4),7,0x14);
  }
  W[0x15EE8] = 0;
  W[0x15EE4] = (int32_t)0xffffffff;
  return;
}




/* ---- FUN_0000eb3a @ 0x00EB3A ---- */



void FUN_0000eb3a(void)

{
  undefined4 uVar1;
  
  if (W[0x15F16] == 0) {
    if ((W[0x15F0C] == 0) || (W[0x15EF0] == 0)) {
      if ((g_terrain_needs_update == 0) || (W[0x15EEC] == 0)) {
        if (((W[0x15F0E] != 0) && (W[0x15EEC] != 0)) && (W[0x15F10] == 0)) {
          FUN_0000eb28(2);
          W[0x15F10] = 1;
          return;
        }
        if ((W[0x12FC] == 0) || (W[0x15F00] == 0)) {
          if ((W[0x15F12] == 0) || (W[0x15EF8] == 0)) {
            if ((W[0x15F14] == 0) || (W[0x15EFC] == 0)) {
              if (W[0x1300] == 0) {
                return;
              }
              if (W[0x15F08] == 0) {
                return;
              }
              uVar1 = 7;
            }
            else {
              uVar1 = 4;
            }
          }
          else {
            uVar1 = 3;
          }
        }
        else {
          uVar1 = 5;
        }
      }
      else {
        uVar1 = 0;
      }
    }
    else {
      uVar1 = 1;
    }
  }
  else {
    uVar1 = 6;
  }
  FUN_0000eb28(uVar1);
  return;
}




/* ---- FUN_0000ed9a @ 0x00ED9A ---- */

int FUN_0000ed9a(void)

{
  int iVar1;
  undefined2 uVar2;
  
  iVar1 = W[0x0CBC] + -3;
  if (iVar1 == 0) {
    if (W16(0xE10) == 0) {
      uVar2 = 0x93;
    }
    else {
      uVar2 = 0x94;
    }
    if (W[0x0E18] == 0) {
      text_draw_rect_blink(uVar2, 2, 0x1a, 0x1c0, 1);
    }
    else {
      iVar1 = text_draw_rect_solid(uVar2,2,0x1a);
    }
    if (W[0x0E18] == 0) {
      if (W16(0xE10) != 0) {
        text_draw_rect_blink(W16(0xE66) + 0xd9, 0xb, 0x1a, 0x1d0, 1);
      }
    }
    else if (0 < W[0x0E18]) {
      iVar1 = text_draw_rect_solid(W16(0xE66) + 0xd9,0xb,0x1a);
    }
  }
  return iVar1;
}





/* ---- FUN_0000ee8e @ 0x00EE8E ---- */

uint32_t FUN_0000ee8e(void)

{
  uint32_t uVar1;
  
  uVar1 = 0;
  if (((W[0x0E0C] != 3) && (uVar1 = W[0x0CBC] - 3, uVar1 == 0)) && (599 < W[0x0C8C]))
  {
    uVar1 = W[0x2BA6] & 0xffff0001;
    if ((W[0x2BA6] & 1) != 0) {
      if ((0 < W[0x15F1C]) && (W[0x15F18] < 0)) {
        W[0x15F18] = 0xf0;
        W[0x15F1C] = W[0x15F1C] + -1;
      }
      if (W[0x15F18] < 0xe1) {
        W[0x15F18] = 0;
      }
    }
    if ((-1 < (int)W[0x15F24]) && (W16(0xE68) < 10)) {
      if (W[0x15F18] < 0) {
        text_draw_rect_blink(0x10a, 5, 10, 0x200, 12);
        uVar1 = (W[0x0C98] & 0x20) >> 5;
        if (uVar1 != 0) {
          text_draw_rect_solid(0x10f,3,9);
        }
        /* base and palette are both functions of uVar1 here -- 0x00EF66:
         *   00EF66: lea $5.w,A0   ; 00EF6A: pea (A0,D3.l*4)   -> palette 5 + D3*4
         *   00EF6E: lea $210.w,A0 ; 00EF72: lea (A0,D3.l*8),A0
         *   00EF78: move.w D0,-(A7)                            -> base 0x210 + D3*8
         * with D3 = uVar1 = (W[0x0C98] & 0x20) >> 5, i.e. a blink phase. */
        text_draw_rect_blink((short)(uVar1 + 0x10f), 3, (short)(uVar1 + 9),
                             0x210 + (int)uVar1 * 8, 5 + (int)uVar1 * 4);
        tilemap_scroll_set(uVar1 == 0);
        if ((W[0x2BA6] & 1) != 0) {
          W[0x15F24] = 0;
        }
      }
      else {
        W[0x15F24] = 0;
        tilemap_scroll_set(0);
        W[0x15F18] = 0xf0;
      }
      uVar1 = W[0x15F24] - 1;
      W[0x15F24] = uVar1;
      if ((int)uVar1 < 0) {
        text_draw_rect_solid(0x10a,5,10);
        text_draw_rect_solid(0x10f,3,9);
        tilemap_scroll_set(0);
        uVar1 = 0;
      }
    }
    if (-1 < W[0x15F18]) {
      FUN_0000efee();
      uVar1 = 0;
      W[0x15F18] = W[0x15F18] + -1;
    }
  }
  return uVar1;
}





/* ---- FUN_0000f5ca @ 0x00F5CA ---- */

short * FUN_0000f5ca(int param_1, int param_2)

{
  /* ROM 0x00F5CA -- the TWO-argument sound trigger: `move.w $8(a7)` is the
   * sound id and `move.w $a(a7)` the value for its parameter slot, two WORD
   * pushes. The transpile took one 32-bit argument, so a caller's
   * CONCAT22(id, level) became the table index 0x0027xxxx: the lookup at
   * 0x355A4 + index*12 read past the table, came back slot 0 -- THE MUSIC --
   * and the byte stores that followed cleared the song's bit 15 ("keep
   * looping"). A scenery sound source on Wind Woods (FUN_0000fa9e, id 0x27)
   * did exactly that every frame the player flew past it, which is where the
   * music stopped halfway through the level (register row 180). Rewritten
   * from the listing: 16-bit fields at 0x355A4 + id*12 -- +0 command slot,
   * +2 command, +6 priority, +8 parameter slot -- written as 16-bit mailbox
   * words (register row 141's class). */
  int st = (int)(int32_t)W[0x0CBC];
  snd_trig("sound_play_p2val", param_1, param_2);
  if ((st & ~1) != 2 && (st & ~1) != 4 && W16(0x3FFC) == 0) return NULL;   /* 0x00F5D2..0x00F5E8 */
  if (st == 3 && (int32_t)W[0x0CC0] == 3) {                                /* 0x00F5EC..0x00F60C */
    if (0x38 > (int32_t)W[0x0E44]) return NULL;
    if (W[0x0E18] != 0) return NULL;
  }
  { uint32_t e = 0x355A4 + (uint32_t)((int16_t)param_1 * 12);
    int slot = vrd16s(e), pslot = vrd16s(e + 8);
    if (slot < 0) return NULL;                                             /* 0x00F624 */
    if (pslot >= 0)                                                        /* 0x00F628..0x00F638 */
      comms_w16(g_sys.commsram, 0x100 + pslot * 2, (uint16_t)param_2);
    comms_w16(g_sys.commsram, slot * 2, (uint16_t)(vrd16(e + 2) | 0x4000)); /* 0x00F63E..0x00F64E */
    if (vrd16s(e + 6) > W16(0x15F46)) W16_SET(0x15F46, vrd16s(e + 6));      /* 0x00F652..0x00F65E */
  }
  return NULL;
}





/* ---- FUN_0003b114 @ 0x03B114 ---- */

uint64_t FUN_0003b114(void)

{
  /* void */;
  /* void */;
  
  /* dispatch stub - needs ROM table */;
  return CONCAT44(0,0);
}





