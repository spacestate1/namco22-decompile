/*
 * Debug, Test & Service Menu
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

/* ---- debug_profiler_mark ---- */

void debug_profiler_mark()

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x021056 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. A profiling hook. The retail ROM ships it as a bare RTS, so the dozens of
   * calls scattered through player_render and friends cost nothing there and
   * nothing here.
   */
  return;
}


/* ---- debug_flight_dispatch ---- */

/* NOT A DEBUG FUNCTION -- it runs in every game. ROM 0x018392, called once
 * from bonus_sequence_animate at 0x017A7E when its frame reaches 0xDC, i.e.
 * inside the intro orbit (gameplay_sub5_service_run). Disassembly:
 *
 *   018392 movea.l #$e166e4,a1          ; a1 -> the node-base POINTER
 *   018398 move.l  $e00e0c,d0 ; subq.l #3,d0 ; bhi $183d4   ; course > 3
 *   0183A4 move.w  $183b4(pc,d0.w*2),d0 ; jmp $183ae(pc,d0.w)
 *   0183AE dc.w 0008 0008 0008 0008     ; courses 0..3 -> 0x0183B6
 *   0183B6 movea.l (a1),a0 ; move.l #$3e43f,$24(a0)
 *          movea.l (a1),a0 ; move.l #$1b05f,$40(a0)
 *          movea.l (a1),a0 ; move.l #$1418a,$2c(a0)
 *   0183D4 movea.l (a1),a0 ; clr.l $30(a0) ; clr.l $38(a0) ; clr.l $3b0(a0)
 *          moveq #-1,d0 ; move.l d0,$880(a0) ; rts
 *
 * The offsets are relative to the node region W[0x166E4] POINTS AT
 * (0xE0B784, set by bonus_model_init_full), and are 32-bit cells. In the _W[]
 * model a 32-bit cell at byte displacement N is W[base + N].
 *
 * The transpile replaced the jump table with a call to
 * terrain_lod_update_wrapper() -- the stand-in register row 65 found in
 * terrain_props_dispatch -- and returned, so for courses 0-3 (every game)
 * the three position writes and the four clears never happened. */

void debug_flight_dispatch(void)

{
  /* The node region is addressed as _W[] byte offsets from the base held
   * in W[0x166E4] (0xB784 -- see BNODE in game_ending.c); the host-packed
   * int32 view this function used is gone. */
  extern intptr_t anim_w_off(const void *p);
  intptr_t nb = anim_w_off((const void *)W[0x166E4]);

  if (nb <= 0 || nb + 0x884 > WORK_RAM_SIZE) return;  /* no node region yet */
  if ((uint32_t)(int32_t)W[0x0E0C] <= 3) {           /* courses 0..3 -> 0x0183B6 */
    W[nb + 0x24] = 0x3E43F;
    W[nb + 0x40] = 0x1B05F;
    W[nb + 0x2C] = 0x1418A;
  }
  W[nb + 0x30] = 0;                                   /* 0x0183D4, every course */
  W[nb + 0x38] = 0;
  W[nb + 0x3B0] = 0;
  W[nb + 0x880] = -1;
}

/* ---- state_test_mode_init ---- */

void state_test_mode_init(void)

{
  sync_post();
  W[0x16964] = 600;
  W[0x0CBC] = 9;
  return;
}

/* ---- state_test_mode_run ---- */

void state_test_mode_run(void)

{
  W[0x0CBC] = 0;
  W[0x0CC8] = 0xf;
  return;
}

/* ---- menu_draw_items ---- */

void menu_draw_items(int *param_1,undefined4 param_2)

{
  char cVar1;
  int iVar2;
  uint16_t uVar3;
  short sVar4;
  short sVar5;
  uint16_t *puVar6;
  uint16_t *puVar7;
  
  cVar1 = W[0x3FB6 + ((int)W[0x3FB4] >> 1)];
  sVar5 = 0;
  puVar6 = &W[0x168EC];
  do {
    sVar5 = sVar5 + 1;
    if (param_2 < sVar5) {
      return;
    }
    uVar3 = *puVar6;
    iVar2 = menu_item_get_value((short)param_1);
    if ((iVar2 == (*(int16_t*)param_1 + 0x12)) && ((*(int16_t*)param_1 + 0x16) != -1)) {
      uVar3 = 0x4000;
    }
    if (((short)cVar1 == (*(int16_t*)param_1 + 0x16)) && (0x7f < param_2)) {
      if ((W[0x0C98] & 8) == 0) {
        if (uVar3 == 0x2000) {
          uVar3 = 0;
        }
      }
      else if (uVar3 != 0x2000) {
        uVar3 = 0x2000;
      }
    }
    if (((int32_t*)(intptr_t)param_1)[1] == 0) {
      tilemap_draw_decimal
                (*(undefined2 *)(param_1 + 2),*(undefined2 *)((intptr_t)param_1 + 10),
                 *(undefined2 *)(param_1 + 4),(short)iVar2,uVar3);
    }
    else if (((int32_t*)(intptr_t)param_1)[1] == -1) {
      puVar7 = (uint16_t *)
               ((intptr_t)&g_sys.textram[0] +
               ((int)(*(int16_t*)(param_1 + 2)) + (int)(*(int16_t*)(param_1 + 4))) * 2 +
               (*(int16_t*)param_1 + 10) * 0x80);
      for (sVar4 = 0; sVar4 < (*(int16_t*)(param_1 + 4)); sVar4 = sVar4 + 1) {
        puVar7 = puVar7 + -1;
        *puVar7 = uVar3 | (uint16_t)iVar2 & 0xf;
        iVar2 = iVar2 >> 4;
      }
    }
    else if (((int32_t*)(intptr_t)param_1)[1] == -2) {
      tilemap_draw_time_value
                (*(undefined2 *)(param_1 + 2),*(undefined2 *)((intptr_t)param_1 + 10),
                 *(undefined2 *)(param_1 + 4),(short)iVar2,(short)((int)(uint32_t)uVar3 >> 0xc));
    }
    else {
      text_print_string(*(undefined2 *)(param_1 + 2),*(undefined2 *)((intptr_t)param_1 + 10),
                        (short)*(undefined4 *)((int32_t*)(intptr_t)param_1)[iVar2 + 1], 0);
    }
    param_1 = param_1 + 6;
    puVar6 = puVar6 + 1;
  } while (*param_1 != 0);
  return;
}

/* ---- menu_draw_labels ---- */

void menu_draw_labels(uint32_t param_1, int param_2)

{
  /* Decompiled from the M68K at 0x01A55A. The old body dereferenced the ROM
   * table as a HOST pointer in NATIVE byte order (register row 52's class),
   * read the string pointer's top half as a coordinate, and had no highlight
   * at all -- which is why the operator menus drew nothing usable.
   *
   * The table is an array of 12-byte records, terminated by a zero string
   * pointer:
   *      +0x00  u32  string (ROM offset)
   *      +0x04  u16  column          +0x06  u16  row
   *      +0x08  u16  palette         +0x0A  u16  item index
   *
   * The record whose item index equals the page's current selection
   * (byte at 0xE03FB6 + (W[0x3FB4] >> 1)) is drawn in palette 2 while
   * bit 3 of the frame counter is set -- the menu cursor's blink. The ROM
   * guards that with `cmpi.w #$7f,count ; ble` at 0x01A592. */
  int sel, n;
  uint32_t rec = param_1;

  sel = (int16_t)(int8_t)vrd8(0xE03FB6 + ((int32_t)(int16_t)W[0x3FB4] >> 1));

  n = 0;
  do {
    int pal;
    n++;
    if (param_2 < n) return;
    if ((int16_t)vrd16(rec + 0x0a) == sel &&
        (W[0x0C98] & 8) != 0 && param_2 > 0x7f)
        pal = 2;                                   /* 0x01A59A */
    else
        pal = (int16_t)vrd16(rec + 0x08);          /* 0x01A59E */
    text_print_string((int16_t)vrd16(rec + 0x04),   /* col */
                      (int16_t)vrd16(rec + 0x06),   /* row */
                      (char *)(uintptr_t)vrd32(rec),
                      pal);
    rec += 0x0c;
  } while (vrd32(rec) != 0);
  return;
}

/* ---- menu_handle_page_select ---- */

undefined4 menu_handle_page_select(undefined4 param_1)

{
  char *pcVar1;
  uint16_t uVar2;
  uint32_t uVar3;
  int iVar4;
  undefined2 uVar5;
  
  uVar5 = 0;
  pcVar1 = &W[0x3FB6] + ((int)W[0x3FB4] >> 1);
  if (((W[0x2BDA] & 4) != 0) ||
     (uVar2 = (uint16_t)((uint32_t)pcVar1 >> 0x10), (W[0x2B3A] & 0x80) != 0)) {
    uVar3 = (uint32_t)pcVar1 & 0xffff0000;
    *pcVar1 = *pcVar1 + -1;
    if (*pcVar1 < '\0') {
      uVar3 = (int)param_1 - 1;
      *pcVar1 = (char)uVar3;
    }
    uVar2 = (uint16_t)(uVar3 >> 0x10);
    uVar5 = 1;
  }
  if (((W[0x2BDA] & 8) != 0) || (iVar4 = (uint32_t)uVar2 << 0x10, (W[0x2B3A] & 0x40) != 0)) {
    *pcVar1 = *pcVar1 + '\x01';
    iVar4 = (int)*pcVar1;
    if (param_1 <= iVar4) {
      *pcVar1 = '\0';
    }
    uVar5 = 1;
  }
  return CONCAT22((short)((uint32_t)iVar4 >> 0x10),uVar5);
}

/* ---- menu_handle_value_adjust ---- */

short menu_handle_value_adjust(int *param_1)

{
  int iVar1;
  short sVar2;
  
  sVar2 = 0;
  do {
    if ((int)(char)W[0x3FB6 + ((int)W[0x3FB4] >> 1)] == (int)(*(int16_t*)param_1 + 0x16))
    break;
    param_1 = param_1 + 6;
  } while (*param_1 != 0);
  if (*param_1 != 0) {
    iVar1 = menu_item_get_value(param_1);
    if (((W[0x2BDC] & 2) != 0) || ((W[0x2B3C] & 0x20) != 0)) {
      iVar1 = iVar1 + -1;
      if (iVar1 < (*(int16_t*)(param_1 + 3))) {
        iVar1 = (int)(*(int16_t*)param_1 + 0xe);
        sVar2 = -0x10;
      }
      sVar2 = sVar2 + -1;
    }
    if (((W[0x2BDC] & 1) != 0) || ((W[0x2B3C] & 0x10) != 0)) {
      iVar1 = iVar1 + 1;
      if ((*(int16_t*)param_1 + 0xe) < iVar1) {
        iVar1 = (int)(*(int16_t*)(param_1 + 3));
        sVar2 = sVar2 + 0x10;
      }
      sVar2 = sVar2 + 1;
    }
    menu_item_set_value(param_1,iVar1);
  }
  return sVar2;
}

/* ---- menu_items_clear_highlights ---- */

void menu_items_clear_highlights(int *param_1)

{
  int iVar1;
  undefined2 *puVar2;
  
  puVar2 = &W[0x168EC];
  do {
    *puVar2 = 0;
    iVar1 = *param_1;
    puVar2 = puVar2 + 1;
    param_1 = param_1 + 6;
  } while (iVar1 != 0);
  return;
}

/* ---- menu_items_copy_to_work ---- */

void menu_items_copy_to_work(int *param_1)

{
  int iVar1;
  int *piVar2;
  
  piVar2 = &W[0xAB04];
  do {
    *piVar2 = *param_1;
    piVar2[1] = ((int32_t*)(intptr_t)param_1)[1];
    piVar2[2] = ((int32_t*)(intptr_t)param_1)[2];
    piVar2[3] = ((int32_t*)(intptr_t)param_1)[3];
    piVar2[4] = ((int32_t*)(intptr_t)param_1)[4];
    piVar2[5] = ((int32_t*)(intptr_t)param_1)[5];
    iVar1 = *param_1;
    piVar2 = piVar2 + 6;
    param_1 = param_1 + 6;
  } while (iVar1 != 0);
  return;
}

/* ---- test_background_color_cycle ---- */

undefined4 test_background_color_cycle(void)

{
  uint32_t uVar1;
  /* void */;
  
  uVar1 = W[0xAB20] >> 5 & 7;
  g_sys.videomix[0x0008] = *(undefined1 *)((int)(int32_t*)&g_sys.rom[0x3F3D0] + uVar1);
  g_sys.videomix[0x0009] = (&R[0x3F3D8])[uVar1];
  g_sys.videomix[0x000A] = (&R[0x3F3E0])[uVar1];
  return 0;
}

/* ---- debug_adjust_complete ---- */

uint16_t debug_adjust_complete(void)

{
  text_print_string(0xb,2,(char*)&g_sys.rom[0x3a407], 0);
  text_print_string(8,8,(char*)(g_sys.rom + 0x3a419), 0);
  if ((W[0x2B3A] & 0x800) != 0) {
    g_stage_timer = 0;
  }
  return W[0x2B3A] & 0x800;
}

/* ---- debug_draw_player_state ---- */

void debug_draw_player_state(void)

{
  debug_draw_value(0,6,2,(char*)(g_sys.rom + 0x3a16c),W[0x0D24],&R[0x3A172],8);
  text_print_string(0,0xb,0x3a173, 0);
  debug_draw_value(0,0xc,8,&R[0x3A172],W[0x0D00],&R[0x3A17A],0x20000);
  debug_draw_value(0,0xd,8,&R[0x3A172],W[0x0D04],&R[0x3A17C],0x20000);
  debug_draw_value(0,0xe,8,&R[0x3A172],W[0x0D08],&R[0x3A17E],0x20000);
  debug_draw_value(0,0xf,8,&R[0x3A172],W[0x0D0C],&R[0x3A180],&R[0xA0000]);
  debug_draw_value(0,0x10,8,&R[0x3A172],W[0x0D10],&R[0x3A183],&R[0xA0000]);
  debug_draw_value(0,0x11,8,&R[0x3A172],W[0x0D14],&R[0x3A186],&R[0xA0000]);
  text_print_string(0,0x12,0x3a189, 0);
  debug_draw_value(0,0x13,8,&R[0x3A172],W[0x0CDC],&R[0x3A17A],&R[0x20004]);
  debug_draw_value(0,0x14,8,&R[0x3A172],W[0x0CE0],&R[0x3A17C],&R[0x20004]);
  debug_draw_value(0,0x15,8,&R[0x3A172],W[0x0CE4],&R[0x3A17E],&R[0x20004]);
  debug_draw_value(0,0x16,8,&R[0x3A172],W[0x0CE8],&R[0x3A180],&R[0x40002]);
  debug_draw_value(0,0x17,8,&R[0x3A172],W[0x0CEC],&R[0x3A183],&R[0x40002]);
  debug_draw_value(0,0x18,8,&R[0x3A172],W[0x0CF0],&R[0x3A186],&R[0x40002]);
  return;
}

/* ---- debug_freecam_init ---- */

void debug_freecam_init(void)

{
  W[0x0C24] = 0;
  sync_post();
  set_background_color(0x40,0x60,0x50);
  text_print_string(9,3,0x3a158, 0);
  W[0x0D00] = &R[0x78F7C];
  W[0x0D04] = 0xd398;
  W[0x0D08] = &R[0xDEA0B];
  W[0x0D0C] = 0;
  W[0x0D10] = 0x9700;
  W[0x0D14] = 0;
  W[0x0C28] = (int32_t)0xffffc000;
  W[0x0C2C] = (int32_t)0xffffc000;
  W[0x0C34] = (int32_t)0xffffd000;
  W[0x0CD0] = 5;
  W[0x0C30] = 0;
  terrain_chunk_render(0);
  world_props_init_dispatch(0);
  camera_state_reset();
  W[0x0C48] = 0;
  W[0x0C4C] = 0;
  W[0x0C50] = 0;
  return;
}

/* ---- debug_freecam_input ---- */

uint32_t debug_freecam_input(void)

{
  undefined2 uVar1;
  /* void */;
  uint32_t uVar2;
  
  uVar1 = (undefined2)((uint32_t)0 >> 0x10);
  if ((W[0x2B3A] & 8) != 0) {
    uVar1 = 0;
    W[0x0C24] = W[0x0C24] ^ 1;
  }
  if ((W[0x2B38] & 0x10) != 0) {
    W[0x0C3C] = (int32_t)0xffffff00;
  }
  if ((W[0x2B38] & 0x20) != 0) {
    W[0x0C3C] = 0x100;
  }
  if ((W[0x2B38] & 0x80) != 0) {
    W[0x0C38] = (int32_t)0xffffff00;
  }
  if ((W[0x2B38] & 0x40) != 0) {
    W[0x0C38] = 0x100;
  }
  if ((W[0x2B38] & 1) != 0) {
    W[0x0C40] = 0x100;
  }
  if ((W[0x2B38] & 2) != 0) {
    W[0x0C40] = (int32_t)0xffffff00;
  }
  if ((W[0x2B38] & 0x800) != 0) {
    if ((W[0x2B40] & 0x800) == 0) {
      W[0x0C44] = (int32_t)0xffffff00;
    }
    else {
      W[0x0C44] = (int32_t)0xfffffb00;
    }
  }
  if ((W[0x2B38] & 4) != 0) {
    if ((W[0x2B40] & 4) == 0) {
      W[0x0C44] = 0x100;
    }
    else {
      W[0x0C44] = 0x500;
    }
  }
  if ((W[0x2B5C] & 0x100) == 0) {
    if ((W[0x2B5C] & 0x10) != 0) {
      if ((W[0x2B64] & 0x10) == 0) {
        W[0x0C2C] = W[0x0C2C] + 0x100;
      }
      else {
        W[0x0C2C] = W[0x0C2C] + 0x300;
      }
    }
    if ((W[0x2B5C] & 0x20) != 0) {
      if ((W[0x2B64] & 0x20) == 0) {
        W[0x0C2C] = W[0x0C2C] + -0x100;
      }
      else {
        W[0x0C2C] = W[0x0C2C] + -0x300;
      }
    }
    if ((W[0x2B5C] & 0x80) != 0) {
      if ((W[0x2B64] & 0x80) == 0) {
        W[0x0C28] = W[0x0C28] + -0x100;
      }
      else {
        W[0x0C28] = W[0x0C28] + -0x300;
      }
    }
    if ((W[0x2B5C] & 0x40) != 0) {
      if ((W[0x2B64] & 0x40) == 0) {
        W[0x0C28] = W[0x0C28] + 0x100;
      }
      else {
        W[0x0C28] = W[0x0C28] + 0x300;
      }
    }
    if ((W[0x2B5C] & 8) != 0) {
      if ((W[0x2B64] & 8) == 0) {
        W[0x0C34] = W[0x0C34] + 0x80;
      }
      else {
        W[0x0C34] = W[0x0C34] + 0x300;
      }
    }
    uVar2 = (((uint32_t)(uVar1) << 16) | (uint16_t)(W[0x2B5C])) & 0xffff0800;
    if ((W[0x2B5C] & 0x800) != 0) {
      uVar2 = (((uint32_t)(uVar1) << 16) | (uint16_t)(W[0x2B64])) & 0xffff0800;
      if ((W[0x2B64] & 0x800) == 0) {
        uVar2 = 0xffffff80;
        W[0x0C34] = W[0x0C34] + -0x80;
      }
      else {
        W[0x0C34] = W[0x0C34] + -0x300;
      }
    }
  }
  else {
    if ((W[0x2B60] & 0x10) != 0) {
      uVar1 = 0;
      W[0x0C48] = W[0x0C48] + 0x10;
    }
    if ((W[0x2B60] & 0x20) != 0) {
      uVar1 = 0xffff;
      W[0x0C48] = W[0x0C48] + -0x10;
    }
    if ((W[0x2B60] & 0x80) != 0) {
      uVar1 = 0;
      W[0x0C4C] = W[0x0C4C] + 0x10;
    }
    if ((W[0x2B60] & 0x40) != 0) {
      uVar1 = 0xffff;
      W[0x0C4C] = W[0x0C4C] + -0x10;
    }
    if ((W[0x2B60] & 8) != 0) {
      uVar1 = 0;
      W[0x0C50] = W[0x0C50] + 0x10;
    }
    uVar2 = (((uint32_t)(uVar1) << 16) | (uint16_t)(W[0x2B60])) & 0xffff0800;
    if ((W[0x2B60] & 0x800) != 0) {
      uVar2 = 0xfffffff0;
      W[0x0C50] = W[0x0C50] + -0x10;
    }
  }
  return uVar2;
}

/* ---- debug_io_monitor ---- */

int debug_io_monitor(void)

{
  int iVar1;
  undefined1 uVar2;
  int iVar3;
  int iVar4;
  undefined2 uVar5;
  undefined2 uVar6;
  undefined2 uVar7;
  undefined2 uVar8;

  text_print_string(10,3,(char*)&g_sys.rom[0x3a34e], 0);
  text_print_string(6,6,0x3a360, 0);
  uVar2 = g_sys.commsram[0x7D01];
  text_draw_hex_digits(0xd,6,4,uVar2, 0);
  uVar8 = 0;
  uVar7 = 3;
  uVar6 = 0;
  uVar5 = 0;
  text_print_string(6,8,0x3a367, 0);
  text_draw_hex_digits(0xd,8,4,W[0x2B80],uVar5);
  text_draw_hex_digits(0xd,9,4,W[0x2BA4], 0);
  iVar4 = 0;
  do {
    text_print_string(9,iVar4 + 10,&R[0x3A36E], 0);
    text_draw_hex_digits(0xd,iVar4 + 10,4,W[0x2B38 + (iVar4 * 0x12)], 0);
    iVar4 = iVar4 + 1;
  } while (iVar4 < 2);
  text_print_string(9,0xd,&R[0x3A372], 0);
  text_draw_hex_digits(0xd,0xd,4,W[0x2C0C], 0);
  iVar4 = 0;
  do {
    text_print_string(0x18,iVar4 + 6,&R[0x3A376], 0);
    uVar5 = 0;
    text_draw_hex_digits(0x1c,iVar4 + 6,1,(short)iVar4, 0);
    text_draw_hex_digits(0x1e,iVar4 + 6,4,W[0x2BC8 + (iVar4)],uVar5);
    iVar4 = iVar4 + 1;
  } while (iVar4 < 8);
  iVar4 = 0;
  do {
    text_print_string(0x19,iVar4 + 0xf,&R[0x3A37B], 0);
    uVar5 = 0;
    text_draw_hex_digits(0x1c,iVar4 + 0xf,1,(short)iVar4, 0);
    text_draw_hex_digits(0x1e,iVar4 + 0xf,8,(short)W[0x2BFC + (iVar4 * 2)],uVar5);
    iVar4 = iVar4 + 1;
  } while (iVar4 < 2);
  iVar4 = 0;
  do {
    text_print_string(2,iVar4 * 6 + 0xf,&R[0x3A37B], 0);
    uVar6 = 0;
    text_draw_hex_digits(2,iVar4 * 6 + 0x10,4,W[0x2C00 + (iVar4 * 4)], 0);
    uVar5 = 0;
    text_draw_hex_digits(2,iVar4 * 6 + 0x11,4,W[0x2C02 + (iVar4 * 4)],uVar6);
    text_draw_hex_digits(2,(short)(iVar4 * 6 + 0x12),8,(short)W[0x2BFC + (iVar4 * 2)],uVar5);
    iVar1 = iVar4 + 1;
    iVar3 = iVar4 + -1;
    iVar4 = iVar1;
  } while (iVar1 < 2);
  if (0x200 < W[0x2C04]) {
    uVar7 = 0;
    text_draw_hex_digits(2,0x1a,4,W[0x2C08], 0);
    uVar6 = 0;
    uVar5 = 0;
    text_draw_hex_digits(2,0x1b,4,W[0x2C0A],uVar7);
    text_draw_hex_digits(2,0x1c,8,(short)W[0x2C04],uVar5); iVar3 = 0;
  }
  if (W[0x2C04] < -0x200) {
    uVar7 = 0;
    text_draw_hex_digits(2,0x1a,4,W[0x2C08], 0);
    uVar6 = 0;
    uVar5 = 0;
    text_draw_hex_digits(2,0x1b,4,W[0x2C0A],uVar7);
    text_draw_hex_digits(2,0x1c,8,(short)W[0x2C04],uVar5); iVar3 = 0;
  }
  return iVar3;
}

/* ---- debug_sprite_viewer_run ---- */

void debug_sprite_viewer_run(void)

{
  int iVar1;

  if ((W[0x2B3A] & 0x100) != 0) {
    W[0x168A8] = W[0x168A8] ^ 1;
  }
  if ((W[0x2B3A] & 0x200) != 0) {
    W[0x16898] = W[0x16898] + 1 & 7;
    W[0x167B4 + (W[0x16898] * 8)] = 0;
  }
  if ((W[0x2B3C] & 8) != 0) {
    W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] =
         W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] + 1;
  }
  if ((W[0x2B3C] & 0x800) != 0) {
    W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] =
         W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] + -1;
  }
  if ((W[0x2B3C] & 4) != 0) {
    W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] =
         W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] + 0x10;
  }
  if ((W[0x2B3C] & 0x400) != 0) {
    W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] =
         W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] + -0x10;
  }
  if ((W[0x2B3C] & 0x80) != 0) {
    W[0x1689C] = (W[0x1689C] + 6) % 7;
  }
  if ((W[0x2B3C] & 0x40) != 0) {
    W[0x1689C] = (W[0x1689C] + 1) % 7;
  }
  if ((W[0x2B3C] & 2) != 0) {
    W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] =
         W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] + -0x100;
  }
  if ((W[0x2B3C] & 1) != 0) {
    W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] =
         W[0x16798 + (W[0x16898] * 8 + W[0x1689C])] + 0x100;
  }
  if (W[0x168A8] == 0) {
    g_sys.videomix[0x0008] = 0;
    g_sys.videomix[0x0009] = 0;
    g_sys.videomix[0x000A] = 0;
  }
  else {
    g_sys.videomix[0x0008] = 0x30;
    g_sys.videomix[0x0009] = 0x30;
    g_sys.videomix[0x000A] = 0x80;
  }
  iVar1 = 0;
  do {
    if (-1 < (int)W[0x167B4 + (iVar1 * 8)]) {
      sprite_draw_2d(6,W[0x16798 + (iVar1 * 8)],W[0x1679C + (iVar1 * 8)],
                     W[0x167A0 + (iVar1 * 8)],W[0x167A4 + (iVar1 * 8)],
                     W[0x167A8 + (iVar1 * 8)],W[0x167AC + (iVar1 * 8)],
                     W[0x167B0 + (iVar1 * 8)],0);
    }
    iVar1 = iVar1 + 1;
  } while (iVar1 < 8);
  sprite_draw_multi_segment(1,W[0x3EB0],W[0x3EB4],W[0x3EB8],0,W[0x3EBC] + 0x20,0x20,0,0)
  ;
  debug_draw_value(1,0x13,8,&R[0x3A43E],W[0x16798 + (W[0x16898] * 8)],(char*)(g_sys.rom + 0x3a43f),8);
  debug_draw_value(1,0x14,8,&R[0x3A43E],W[0x1679C + (W[0x16898] * 8)],&R[0x3A449],
                   &R[0x40008]);
  debug_draw_value(1,0x15,8,&R[0x3A43E],W[0x167A0 + (W[0x16898] * 8)],&R[0x3A44B],
                   &R[0x40008]);
  debug_draw_value(1,0x16,8,&R[0x3A43E],W[0x167A4 + (W[0x16898] * 8)],&R[0x3A44D],
                   &R[0x40008]);
  debug_draw_value(1,0x17,8,&R[0x3A43E],W[0x167A8 + (W[0x16898] * 8)],(char*)(g_sys.rom + 0x3a44f),
                   &R[0x40008]);
  debug_draw_value(1,0x18,8,&R[0x3A43E],W[0x167AC + (W[0x16898] * 8)],(char*)(g_sys.rom + 0x3a455),
                   &R[0x40008]);
  debug_draw_value(1,0x19,8,&R[0x3A43E],W[0x167B0 + (W[0x16898] * 8)],&R[0x3A45B],
                   &R[0x40008]);
  text_draw_hex_digits
            (1,W[0x1689C] + 0x13,8,(short)W[0x16798 + (W[0x16898] * 8 + W[0x1689C])], 0);
  return;
}


/* ---- debug_texture_viewer_init ---- */

void debug_texture_viewer_init(void)

{
  sync_post();
  W[0x168B8] = 1;
  W[0x168BC] = 0;
  W[0x168C0] = 0;
  W[0x168C4] = 10;
  cgram_load_tile_block(0,0x120);
  cz_load_color_ramp(W[0x168BC], 0);
  g_stage_timer = 0x1f;
  return;
}

/* ---- debug_texture_viewer_run ---- */

void debug_texture_viewer_run(void)

{
  undefined2 uVar1;
  undefined1 uVar2;
  short sVar3;
  undefined2 uVar4;
  undefined2 uVar5;

  uVar5 = 4;
  uVar4 = 3;
  uVar2 = 0;
  uVar1 = 0;
  text_print_string(0xb,3,0x3a460, 0);
  text_draw_rect_blink((int)W[0x168BC], W[0x168C0], W[0x168C4], 0, 0);


  if ((W[0x2B3C] & 4) != 0) {
    W[0x168B8] = W[0x168B8] + 1 & 0xf;
  }
  if ((W[0x2B3C] & 0x400) != 0) {
    W[0x168B8] = W[0x168B8] - 1 & 0xf;
  }
  if ((W[0x2B3C] & 8) != 0) {
    W[0x168BC] = W[0x168BC] + 1;
    if (299 < W[0x168BC]) {
      W[0x168BC] = 299;
    }
    sync_post();
    uVar1 = 0;
    sVar3 = W[0x168BC] >> 0xf;
    cgram_load_tile_block(W[0x168BC],0x120);
    cz_load_color_ramp(W[0x168BC],sVar3);
  }
  if ((W[0x2B3C] & 0x800) != 0) {
    W[0x168BC] = W[0x168BC] + -1;
    if (W[0x168BC] < 0) {
      W[0x168BC] = 0;
    }
    sync_post();
    uVar1 = 0;
    sVar3 = W[0x168BC] >> 0xf;
    cgram_load_tile_block(W[0x168BC],0x120);
    cz_load_color_ramp(W[0x168BC],sVar3);
  }
  if ((W[0x2B3C] & 1) != 0) {
    W[0x168BC] = W[0x168BC] + 10;
    if (299 < W[0x168BC]) {
      W[0x168BC] = 299;
    }
    sync_post();
    uVar1 = 0;
    sVar3 = W[0x168BC] >> 0xf;
    cgram_load_tile_block(W[0x168BC],0x120);
    cz_load_color_ramp(W[0x168BC],sVar3);
  }
  if ((W[0x2B3C] & 2) != 0) {
    W[0x168BC] = W[0x168BC] + -10;
    if (W[0x168BC] < 0) {
      W[0x168BC] = 0;
    }
    sync_post();
    uVar1 = 0;
    sVar3 = W[0x168BC] >> 0xf;
    cgram_load_tile_block(W[0x168BC],0x120);
    cz_load_color_ramp(W[0x168BC],sVar3);
  }
  if ((W[0x2B3C] & 0x80) != 0) {
    W[0x168C4] = W[0x168C4] + -1;
  }
  if ((W[0x2B3C] & 0x40) != 0) {
    W[0x168C4] = W[0x168C4] + 1;
  }
  if ((W[0x2B3C] & 0x20) != 0) {
    W[0x168C0] = W[0x168C0] + -1;
  }
  if ((W[0x2B3C] & 0x10) != 0) {
    W[0x168C0] = W[0x168C0] + 1;
  }
  if ((W[0x2B3A] & 0x200) != 0) {
    g_stage_timer = 0;
  }
  debug_draw_value(3,5,3,0xa473,(char)W[0x168BC],0xa47b,4);
  debug_draw_value(3,6,1,(char*)(g_sys.rom + 0x3a47c),W[0x168B8],&R[0x3A47B],&R[0x60004]);
  debug_draw_value(3,7,4,(char*)(g_sys.rom + 0x3a484),W[0x168C0],&R[0x3A47B],&R[0x60004]);
  debug_draw_value(3,8,4,(char*)(g_sys.rom + 0x3a48c),W[0x168C4],&R[0x3A47B],&R[0x60004]);
  debug_draw_value(3,9,4,(char*)(g_sys.rom + 0x3a494),vrd32(0x1C4F44 + (W[0x168BC] * 0x10)),
                   &R[0x3A47B],&R[0x60004]);
  debug_draw_value(3,10,4,(char*)(g_sys.rom + 0x3a49c),vrd32(0x1C4F48 + (W[0x168BC] * 0x10)),
                   &R[0x3A47B],&R[0x60004]);
  return;
}


/* ---- debug_viewer_process_input ---- */

uint32_t debug_viewer_process_input(void)

{
  int iVar1;
  int iVar2;
  uint16_t uVar3;
  undefined2 uVar4;
  /* void */;
  uint32_t uVar5;
  undefined4 uVar6;
  int local_10;
  int local_c;
  uint32_t local_8;
  
  if ((W[0x2B38] & 0x10) != 0) {
    W[0x0BEC] = W[0x0BEC] + -0xf0;
  }
  if ((W[0x2B38] & 0x20) != 0) {
    W[0x0BEC] = W[0x0BEC] + 0xf0;
  }
  if ((W[0x2B38] & 0x80) != 0) {
    W[0x0BE8] = W[0x0BE8] + -0xf0;
  }
  if ((W[0x2B38] & 0x40) != 0) {
    W[0x0BE8] = W[0x0BE8] + 0xf0;
  }
  uVar5 = 0 & 0xffff0000;
  if ((W[0x2B38] & 0x800) != 0) {
    if ((W[0x2B40] & 0x800) == 0) {
      uVar6 = 0x200;
    }
    else {
      uVar6 = 0x600;
    }
    spherical_to_cartesian(W[0x0BE8],W[0x0BEC],uVar6,&local_10,&local_c,&local_8);
    W[0x0BDC] = W[0x0BDC] - local_10;
    W[0x0BE0] = W[0x0BE0] - local_c;
    W[0x0BE4] = W[0x0BE4] - local_8;
    uVar5 = local_8;
  }
  uVar5 = uVar5 & 0xffff0000;
  if ((W[0x2B38] & 4) != 0) {
    if ((W[0x2B40] & 4) == 0) {
      uVar6 = 0x200;
    }
    else {
      uVar6 = 0x600;
    }
    spherical_to_cartesian(W[0x0BE8],W[0x0BEC],uVar6,&local_10,&local_c,&local_8);
    W[0x0BDC] = local_10 + W[0x0BDC];
    W[0x0BE0] = local_c + W[0x0BE0];
    W[0x0BE4] = local_8 + W[0x0BE4];
    uVar5 = local_8;
  }
  uVar3 = (uint16_t)(uVar5 >> 0x10);
  if ((W[0x2B3A] & 8) != 0) {
    iVar1 = W[0x0BCC] + 1;
    iVar2 = W[0x0BCC] + -1;
    uVar3 = (uint16_t)((uint32_t)iVar2 >> 0x10);
    W[0x0BCC] = iVar1;
    if (iVar2 != 0 && 1 < iVar1) {
      W[0x0BCC] = 0;
    }
  }
  uVar5 = (uint32_t)uVar3 << 0x10;
  if (((W[0x2B3C] & 2) != 0) && (uVar5 = W[0x0BD8] - 1, W[0x0BD8] = uVar5, (int)uVar5 < 0))
  {
    W[0x0BD8] = 0;
  }
  if ((W[0x2B3C] & 1) != 0) {
    W[0x0BD8] = W[0x0BD8] + 1;
  }
  uVar5 = uVar5 & 0xffff0000;
  if ((W[0x2B5C] & 0x10) != 0) {
    if ((W[0x2B64] & 0x10) == 0) {
      uVar6 = 0x100;
    }
    else {
      uVar6 = 0x300;
    }
    sincos_scale(W[0x0BEC] + -0x4000,uVar6,&local_10,&local_8);
    W[0x0BF4] = local_10 + W[0x0BF4];
    W[0x0BFC] = local_8 + W[0x0BFC];
    uVar5 = local_8;
  }
  uVar5 = uVar5 & 0xffff0000;
  if ((W[0x2B5C] & 0x20) != 0) {
    if ((W[0x2B64] & 0x20) == 0) {
      uVar6 = 0xffffff00;
    }
    else {
      uVar6 = 0xfffffd00;
    }
    sincos_scale(W[0x0BEC] + -0x4000,uVar6,&local_10,&local_8);
    W[0x0BF4] = local_10 + W[0x0BF4];
    W[0x0BFC] = local_8 + W[0x0BFC];
    uVar5 = local_8;
  }
  uVar5 = uVar5 & 0xffff0000;
  if ((W[0x2B5C] & 0x80) != 0) {
    if ((W[0x2B64] & 0x80) == 0) {
      uVar6 = 0x100;
    }
    else {
      uVar6 = 0x300;
    }
    sincos_scale(W[0x0BEC],uVar6,&local_10,&local_8);
    W[0x0BF4] = local_10 + W[0x0BF4];
    W[0x0BFC] = local_8 + W[0x0BFC];
    uVar5 = local_8;
  }
  uVar5 = uVar5 & 0xffff0000;
  if ((W[0x2B5C] & 0x40) != 0) {
    if ((W[0x2B64] & 0x40) == 0) {
      uVar6 = 0xffffff00;
    }
    else {
      uVar6 = 0xfffffd00;
    }
    sincos_scale(W[0x0BEC],uVar6,&local_10,&local_8);
    W[0x0BF4] = local_10 + W[0x0BF4];
    W[0x0BFC] = local_8 + W[0x0BFC];
    uVar5 = local_8;
  }
  uVar4 = (undefined2)(uVar5 >> 0x10);
  if ((W[0x2B5E] & 0x800) != 0) {
    iVar1 = W[0x2C1C] + 1;
    iVar2 = W[0x2C1C] + -1;
    uVar4 = (undefined2)((uint32_t)iVar2 >> 0x10);
    W[0x2C1C] = iVar1;
    if (iVar2 != 0 && 1 < iVar1) {
      W[0x2C1C] = 0;
    }
  }
  if ((W[0x2B5C] & 0x100) != 0) {
    W[0x0C20] = W[0x0C20] + 0x600;
  }
  if ((W[0x2B5C] & 0x200) != 0) {
    W[0x0C20] = W[0x0C20] + -0x600;
  }
  return (((uint32_t)(uVar4) << 16) | (uint16_t)(W[0x2B5C])) & 0xffff0200;
}

/* ---- debug_viewer_restore_camera ---- */

void debug_viewer_restore_camera(void)

{
  W[0x0CDC] = W[0x0BDC];
  W[0x0CE0] = W[0x0BE0];
  W[0x0CE4] = W[0x0BE4];
  W[0x0CE8] = W[0x0BE8];
  W[0x0CEC] = W[0x0BEC];
  W[0x0CF0] = W[0x0BF0];
  return;
}

/* ---- debug_viewer_restore_player ---- */

void debug_viewer_restore_player(void)

{
  W[0x0D00] = W[0x0BF4];
  W[0x0D04] = W[0x0BF8];
  W[0x0D08] = W[0x0BFC];
  W[0x0D0C] = W[0x0C00];
  W[0x0D10] = W[0x0C04];
  W[0x0D14] = W[0x0C08];
  return;
}

/* ---- debug_viewer_update ---- */

void debug_viewer_update(void)

{
  int iVar1;
  
  debug_viewer_process_input();
  debug_viewer_restore_player();
  W[0x0BD4] = W[0x0BD8] % 0x80;
  iVar1 = W[0x0BD8];
  if (W[0x0BD8] < 0) {
    iVar1 = W[0x0BD8] + 0x7f;
  }
  W[0x0BD0] = iVar1 >> 7;
  world_props_init_dispatch(W[0x0BD0]);
  W[0x0D24] = W[0x0BD4];
  W[0x0C1C] = (int)*(char *)(W[0x2910] + W[0x0BD4]);
  W[0x0C0C] = W[0x0BD4] & 7;
  W[0x0C10] = (int)W[0x0BD4] >> 3;
  W[0x0C14] = W[0x0C0C] * 0x18000;
  W[0x0C18] = W[0x0C10] * 0x18000;
  W[0x0D00] = W[0x0C14] + W[0x0BF4];
  W[0x0D08] = W[0x0C18] + W[0x0BFC];
  W[0x0D04] = 0;
  world_stage_update();
  W[0x1308] = 1;
  W[0x1318] = W[0x0D00];
  W[0x131C] = W[0x0D04];
  W[0x1320] = W[0x0D08];
  world_objects_tick_all();
  debug_viewer_restore_camera();
  text_output_flush();
  debug_viewer_render_scene();
  debug_viewer_draw_hud();
  return;
}

/* ---- io_test_display ---- */

void io_test_display(void)

{
  STUB_HIT(0x005AD0, "test mode only");
  /* Ghidra decompilation broken - stubbed */
  return;
}


/* ---- io_test_init ---- */

void io_test_init(void)

{
  int iVar1;
  
  sync_post();
  W[0x0AE4] = (intptr_t)&W[0xC800];
  eeprom_hw_init_and_validate();
  W[0x0AE0] = 1;
  W[0x0B48] = 0;
  iVar1 = 0;
  do {
    W[0x0AE8 + (iVar1 * 2)] = 0;
    iVar1 = iVar1 + 1;
  } while (iVar1 < 0xc);
  g_stage_timer = 3;
  return;
}

/* ---- service_draw_value_with_range ---- */

void service_draw_value_with_range
               (int param_1,undefined4 param_2,int param_3,int param_4,int param_5)

{
  uint8_t *puVar1;
  
  text_draw_hex_digits(param_1,param_2,4,param_3, 0);
  if ((param_3 < param_4) || (param_5 < param_3)) {
    puVar1 = &R[0x3A6AD];
  }
  else {
    puVar1 = &R[0x3667F];
  }
  text_print_string(param_1 + 5,param_2,puVar1, 0);
  return;
}

/* ---- service_game_test_run ---- */

void service_game_test_run(void)

{
  short sVar1;
  undefined2 uVar2;
  undefined2 uVar3;
  undefined2 uVar4;

  if (W[0x16930] == 0) {
    uVar4 = 0xb;
    menu_handle_page_select(0);
    uVar3 = 0x100;
    uVar2 = 3;
    menu_draw_labels(0x369fc,uVar4);
    title_substate_dispatch(uVar2);
    sVar1 = check_confirm_button();
    if (sVar1 != 0) {
      if (W[0x3FBE] == '\n') {
        W[0x3FB4] = 0;
        W[0x3FBE] = '\0';
      }
      else {
        W[0x16930] = 1;
        g_sys.videomix[0x001B] = 0x7e;
        /* dispatch */;
      }
    }
  }
  else {
    sVar1 = check_confirm_button();
    if (sVar1 != 0) {
      W[0x16930] = 0;
      g_sys.videomix[0x001B] = 0x7f;
      sync_post();
    }
  }
  return;
}


/* ---- service_input_test_run ---- */

uint16_t service_input_test_run(void)

{
  uint16_t uVar1;
  undefined2 uVar2;
  undefined2 uVar3;
  
  W[0x16928] = W[0x2BCA] - W[0x3FD2];
  W[0x1692A] = W[0x2BC8] - W[0x3FD0];
  dipswitch_display_update();
  service_draw_value_with_range(0x19,0xc,(int)W[0x16928],0xfffffef0,0x110);
  service_draw_value_with_range(0x19,0xe,(int)W[0x1692A],0xfffffef0,0x110);
  service_draw_onoff(0x19,0x10,W[0x2BA4] & 1);
  service_draw_onoff(0x19,0x12,W[0x2B80] & 1);
  service_draw_onoff(0x19,0x14,W[0x2B80] & 4);
  uVar3 = 3;
  uVar2 = 3;
  menu_draw_labels(0x368cc, 0);
  menu_draw_labels(0x3692c,uVar2);
  if (((((W[0x2BDA] & 4) != 0) && (uVar1 = W[0x2BA4] & 1, (W[0x2BA4] & 1) != 0)) ||
      (((W[0x2BA6] & 1) != 0 &&
       (uVar1 = W[0x2BD8] & 4, (W[0x2BD8] & 4) != 0)))) ||
     (uVar1 = W[0x2B3C] & 0x100, (W[0x2B3C] & 0x100) != 0)) {
    W[0x3FB4] = 6;
  }
  return uVar1;
}

/* ---- service_output_test_run ---- */

uint32_t service_output_test_run(void)

{
  int iVar1;
  uint32_t uVar2;
  char *pcVar3;
  uint32_t uVar4;
  undefined2 uVar5;
  undefined2 uVar6;
  undefined2 uVar7;
  undefined2 uVar8;

  dipswitch_display_update();
  uVar6 = 4;
  uVar7 = 3;
  menu_draw_labels(0x368cc, 0);
  menu_draw_labels(0x36974,uVar7);
  if ((W[0x0C98] & 0x10) == 0) {
    iVar1 = (int)W[0x16928] - 2;
    if ((uint32_t)(int)W[0x16928] < 2 || iVar1 == 0) {


      uVar2 = 0; /* dispatch */
      return uVar2;
    }
  }
  else {
    if (W[0x16928] != 2) {
      text_print_string(0xd,0x12,0x36670, 0);
    }
    text_print_string(0xd,0x14,0x3666c, 0);
  }
  uVar7 = 0;
  if ((W[0x0C98] & 8) == 0) {
    if (W[0x16928] == 0) {
      pcVar3 = (char*)(g_sys.rom + 0x3a70a);
    }
    else if (W[0x16928] == 1) {
      pcVar3 = (char*)(g_sys.rom + 0x3a6f8);
    }
    else {
      pcVar3 = &R[0x3A706];
    }
  }
  else {
    pcVar3 = &R[0x36674];
  }
  uVar8 = (undefined2)((uint32_t)pcVar3 >> 0x10);
  uVar5 = 0;
  uVar6 = 0;
  text_print_string(0xf,0x10,(short)pcVar3, 0);
  text_draw_hex_digits(0x19,0xe,4,W[0x2C0A],uVar6);
  uVar2 = check_confirm_button();
  if ((short)uVar2 != 0) {
    uVar8 = 0;
    uVar5 = 3;
    uVar6 = 0;
    uVar7 = 0;
    text_print_string(0xf,0x10,0x36674, 0);
    text_print_string(0xd,0x12,0x36670,uVar7);
    text_print_string(0xd,0x14,0x3666c, 0);
    W[0x16928] = W[0x16928] + 1;
    uVar2 = (int)W[0x16928] - 2;
    if (W[0x16928] == 1 || uVar2 == 0) {


      uVar2 = 0; /* dispatch */
      return uVar2;
    }
    W[0x16928] = 0;
  }
  uVar4 = uVar2 & 0xffff0000;
  if ((((W[0x2BDA] & 4) == 0) ||
      (uVar2 = CONCAT22((short)(uVar2 >> 0x10),W[0x2BA4]) & 0xffff0001, uVar4 = uVar2,
      (W[0x2BA4] & 1) == 0)) &&
     ((uVar2 = uVar4 & 0xffff0000, (W[0x2BA6] & 1) == 0 ||
      (uVar2 = CONCAT22((short)(uVar4 >> 0x10),W[0x2BD8]) & 0xffff0004,
      (W[0x2BD8] & 4) == 0)))) {
    uVar7 = (undefined2)(uVar2 >> 0x10);
    if ((W[0x2B38] & 0x80) == 0) {
      return (((uint32_t)(uVar7) << 16) | (uint16_t)(W[0x2B38])) & 0xffff0080;
    }
    uVar2 = (((uint32_t)(uVar7) << 16) | (uint16_t)(W[0x2B38])) & 0xffff0100;
    if ((W[0x2B38] & 0x100) == 0) {
      return uVar2;
    }
  }
  W[0x3FBB] = 0;
  W[0x3FB4] = 6;
  return uVar2;
}

/* ---- service_sound_test_init ---- */

void service_sound_test_init(void)

{
  sync_post();
  W[0xAC44] = 0;
  menu_items_copy_to_work((int32_t*)&g_sys.rom[0x36714]);
  menu_items_clear_highlights((int32_t*)&g_sys.rom[0x36714]);
  if ((W[0x2BA4] & 2) != 0) {
    W[0xAB14] = 4;
    W[0xAB46] = 2;
  }
  W[0x16928] = W[0x2C0C] & 0x10;
  if ((W[0x2C0C] & 0x10) == 0) {
    menu_items_copy_to_work((int32_t*)&g_sys.rom[0x36A98]);
    menu_items_clear_highlights((int32_t*)&g_sys.rom[0x36A98]);
    W[0x16938] = 0;
    W[0x1693A] = 0;
    W[0x1693C] = 0;
  }
  else {
    menu_items_copy_to_work((int32_t*)&g_sys.rom[0x36B88]);
    menu_items_clear_highlights((int32_t*)&g_sys.rom[0x36B88]);
    W[0x16938] = W[0x3FE8] & 0xf;
    W[0x1693A] = (short)W[0x3FE8] >> 4 & 0xf;
    W[0x1693C] = (short)(char)(W[0x3FE8] >> 8) & 0xf;
  }
  if ((W[0x2BA4] & 2) != 0) {
    W[0xAB14] = 0x2b;
    W[0xAB2C] = 0x2b;
  }
  W[0x16934] = g_audio_env_params;
  W[0x16936] = W[0x3FE2];
  W[0x1692C] = 0;
  W[0x1692A] = 0x89;
  W[0x16924] = scene_get_max_priority();
  W[0x3FB4] = 0x13;
  return;
}

/* ---- service_sound_test_run ---- */

uint32_t service_sound_test_run(void)

{
  uint8_t **ppuVar1;
  char cVar7;
  short sVar4;
  uint16_t uVar5;
  short sVar6;
  uint32_t uVar2;
  uint32_t uVar3;
  short sVar8;
  char *pcVar9;
  char *pcVar10;
  undefined2 uVar11;
  undefined2 uVar12;
  undefined2 uVar13;
  undefined2 uVar14;
  undefined2 *local_8;

  uVar14 = 0x100;
  if (W[0x16928] == 0) {
    ppuVar1 = (int32_t*)&g_sys.rom[0x36b28];
  }
  else {
    ppuVar1 = (int32_t*)&g_sys.rom[0x36c48];
  }
  uVar13 = (undefined2)((uint32_t)(intptr_t)ppuVar1 >> 0x10);
  /* was `(short)ppuVar1` -- a host pointer truncated to 16 bits. The two
   * candidate tables are ROM 0x36B28 / 0x36C48; pass the offset. */
  menu_draw_labels(W[0x16928] == 0 ? 0x36b28 : 0x36c48, 0x100);
  uVar12 = 0x100;
  uVar11 = 0xe0;
  menu_draw_items(0xab04,uVar13);
  title_substate_dispatch(uVar11);
  local_8 = &g_sys.commsram[0x0300];
  sVar4 = 0;
  pcVar10 = &W[0xAC44];
  while( true ) { if (++_safety_ctr > 10000) break;
    uVar14 = *local_8;
    cVar7 = (char)uVar14;
    *pcVar10 = cVar7;
    pcVar9 = pcVar10 + 1;
    if (cVar7 == '\0') break;
    cVar7 = (char)((uint16_t)uVar14 >> 8);
    pcVar9 = pcVar10 + 2;
    pcVar10[1] = cVar7;
    if ((cVar7 == '\0') ||
       (sVar4 = sVar4 + 1, pcVar10 = pcVar9, local_8 = local_8 + 1, 0x73 < sVar4)) break;
  }
  pcVar10 = pcVar9 + -1;
  for (; sVar4 < 0x74; sVar4 = sVar4 + 1) {
    *pcVar10 = ' ';
    pcVar10 = pcVar10 + 1;
  }
  *pcVar10 = '\0';
  sVar4 = menu_handle_value_adjust(0xab04);
  uVar5 = (uint16_t)W[0x3FBF];
  if (W[0x16934] != g_audio_env_params) {
    g_audio_env_params = W[0x16934];
    W[0x3FE4] = W[0x16934];
    sound_env_apply();
  }
  if (W[0x16936] != W[0x3FE2]) {
    W[0x3FE2] = W[0x16936];
    W[0x3FE6] = W[0x16936];
    sound_env_apply();
  }
  if (sVar4 < 0x10) {
    if (sVar4 < -0xf) {
      if ((uVar5 == 4) && (W[0x1693A] = W[0x1693A] + -1, W[0x1693A] < 0)) {
        if (W[0x16928] == 0) {
          W[0x1693A] = 9;
        }
        else {
          W[0x1693A] = 0xf;
        }
        uVar5 = 3;
      }
      if ((uVar5 == 3) && (W[0x1693C] = W[0x1693C] + -1, W[0x1693C] < 0)) {
        if (W[0x16928] == 0) {
          W[0x1693C] = 9;
        }
        else {
          W[0x1693C] = 0xf;
        }
      }
    }
  }
  else {
    if (uVar5 == 4) {
      W[0x1693A] = W[0x1693A] + 1;
      if (W[0x16928] == 0) {
        sVar6 = 10;
      }
      else {
        sVar6 = 0x10;
      }
      if (sVar6 <= W[0x1693A]) {
        W[0x1693A] = 0;
        uVar5 = 3;
      }
    }
    if (uVar5 == 3) {
      W[0x1693C] = W[0x1693C] + 1;
      if (W[0x16928] == 0) {
        sVar6 = 10;
      }
      else {
        sVar6 = 0x10;
      }
      if (sVar6 <= W[0x1693C]) {
        W[0x1693C] = 0;
      }
    }
  }
  if (W[0x16928] == 0) {
    sVar8 = W[0x1693C] * 100;
    sVar6 = W[0x1693A] * 10;
  }
  else {
    sVar8 = W[0x1693C] << 8;
    sVar6 = W[0x1693A] << 4;
  }
  W[0x3FE8] = W[0x16938] + sVar6 + sVar8;
  if (sVar4 != 0) {
    if ((uVar5 < 2) || (4 < uVar5)) {
      W[0x168EC + (uVar5)] = 0x2000;
    }
    else {
      W[0x168F0] = 0x2000;
      W[0x168F2] = 0x2000;
      W[0x168F4] = 0x2000;
    }
  }
  sVar4 = menu_handle_page_select(0);
  if (sVar4 != 0) {
    eeprom_write_block((intptr_t)0x3fe0,10);
  }
  W[0x16932] = W[0x16932] + -1;
  if ((W[0x16932] < 1) || (uVar5 = g_sys.commsram[0], (uVar5 & 0x8000) == 0)) {
    sound_stop_all();
    W[0x16932] = 0;
  }
  uVar2 = check_confirm_button();
  if ((short)uVar2 != 0) {
    if (W[0x16928] == 0) {
      uVar3 = 5;
    }
    else {
      uVar3 = 7;
    }
    uVar2 = (uint32_t)W[0x3FBF];
    if (uVar2 == uVar3) {
      sound_stop_all();
      W[0x3FB4] = 0;
      W[0x3FBF] = '\0';
      eeprom_sync_all(); uVar2 = 0;
    }
    else if (W[0x16932] < 1) {
      W[0x16932] = 600;
      if (W[0x16928] != 0) {
        uVar2 = CONCAT22(W[0x3FBF] >> 7,W[0x1692C]);
        g_sys.commsram[0x0100 + (W[0x1692C])] = W[0x1692A];
      }
      if ((short)W[0x3FE8] <= W[0x16924]) {
        uVar2 = CONCAT22((short)(uVar2 >> 0x10),W[0x3FE8]) | 0x4000;
        g_sys.commsram[0] = W[0x3FE8] | 0x4000;
      }
      W[0x16920] = 0;
    }
    else {
      W[0x16932] = 0;
      sound_stop_all(); uVar2 = 0;
    }
  }
  W[0x16920] = W[0x16920] + 1;
  return uVar2;
}


/* ---- service_statistics_run ---- */

uint32_t service_statistics_run(void)

{
  uint16_t extraout_D0u;
  uint16_t extraout_D0u_00;
  uint16_t extraout_D0u_01;
  uint16_t extraout_D0u_02;
  uint16_t extraout_D0u_03;
  uint16_t uVar1;
  short sVar5;
  int iVar2;
  uint32_t uVar3;
  uint32_t uVar4;
  short sVar6;
  short sVar7;

  sVar5 = menu_handle_page_select(0);
  if (sVar5 != 0) {
    sync_post();
    title_substate_dispatch(0);
  }
  sVar5 = (short)W[0x3FC0];
  /* was a NATIVE 32-bit read of a big-endian ROM pointer table, then
   * truncated to 16 bits. The table at 0x36FD8 holds menu-table offsets. */
  menu_draw_labels(vrd32(0x36fd8 + sVar5*4), 0x100);
  if (sVar5 == 1) {
    sVar5 = 0;
    do {
      sVar7 = 0;
      _safety_ctr = 0;
      do {
        tilemap_draw_decimal
                  (sVar5 * 0xc + 8,sVar7 * 2 + 9,4,W[0x412A + (sVar5 * 8 - (int)sVar7)],0);
        sVar7 = sVar7 + 1;
      } while (sVar7 < 6);
      sVar5 = sVar5 + 1;
      uVar1 = extraout_D0u;
    } while (sVar5 < 3);
  }
  else if (sVar5 == 2) {
    sVar5 = 0;
    do {
      sVar7 = 0;
      do {
        if (sVar7 < 3) {
          iVar2 = sVar7 * 8 + 4;
        }
        else {
          iVar2 = sVar7 * 6 + 9;
        }
        tilemap_draw_decimal(iVar2,sVar5 * 5 + 10,4,W[0x40E2 + (sVar5 * 10 + (int)sVar7)],0);
        uVar1 = extraout_D0u_00;
        if (sVar7 != 0) {
          if (sVar7 < 3) {
            iVar2 = sVar7 * 8 + 4;
          }
          else {
            iVar2 = sVar7 * 6 + 9;
          }
          tilemap_draw_decimal(iVar2,sVar5 * 5 + 0xc,4,W[0x40EC + (sVar5 * 10 + (int)sVar7)],0);
          uVar1 = extraout_D0u_01;
        }
        sVar7 = sVar7 + 1;
      } while (sVar7 < 5);
      sVar5 = sVar5 + 1;
    } while (sVar5 < 3);
  }
  else if (sVar5 == 3) {
    sVar5 = 0;
    do {
      sVar7 = 0;
      sVar6 = 0;
      do {
        sVar7 = W[0x40B0 + (sVar5 * 5 + (int)sVar6)] + sVar7;
        tilemap_draw_decimal
                  (sVar6 * 5 + 8,sVar5 * 3 + 10,4,W[0x40B0 + (sVar5 * 5 + (int)sVar6)],0);
        sVar6 = sVar6 + 1;
      } while (sVar6 < 5);
      tilemap_draw_decimal(0x22,sVar5 * 3 + 10,4,sVar7,0);
      sVar5 = sVar5 + 1;
      uVar1 = extraout_D0u_02;
    } while (sVar5 < 5);
  }
  else {
    menu_draw_items((short)(*(int32_t*)(&g_sys.rom[0x36fe8 + sVar5*4])), 0);
    uVar1 = extraout_D0u_03;
  }
  uVar4 = (uint32_t)uVar1 << 0x10;
  if (((((W[0x2BDA] & 1) == 0) ||
       (uVar4 = (((uint32_t)(uVar1) << 16) | (uint16_t)(W[0x2BA4])) & 0xffff0001, (W[0x2BA4] & 1) == 0)) &&
      ((uVar3 = uVar4 & 0xffff0000, (W[0x2BA6] & 1) == 0 ||
       (uVar4 = CONCAT22((short)(uVar4 >> 0x10),W[0x2BD8]) & 0xffff0001, uVar3 = uVar4,
       (W[0x2BD8] & 1) == 0)))) &&
     ((((uVar4 = uVar3 & 0xffff0000, (W[0x2B3A] & 0x10) == 0 ||
        (uVar4 = CONCAT22((short)(uVar3 >> 0x10),W[0x2B38]) & 0xffff0100,
        (W[0x2B38] & 0x100) == 0)) &&
       ((uVar3 = uVar4 & 0xffff0000, (W[0x2B3A] & 0x100) == 0 ||
        (uVar4 = CONCAT22((short)(uVar4 >> 0x10),W[0x2B38]) & 0xffff0010, uVar3 = uVar4,
        (W[0x2B38] & 0x10) == 0)))) &&
      (((uVar4 = uVar3 & 0xffff0000, (W[0x2B3A] & 0x10) == 0 ||
        (uVar4 = CONCAT22((short)(uVar3 >> 0x10),W[0x2B38]) & 0xffff0800,
        (W[0x2B38] & 0x800) == 0)) &&
       (((W[0x2B3A] & 0x800) == 0 ||
        (uVar4 = CONCAT22((short)(uVar4 >> 0x10),W[0x2B38]) & 0xffff0010,
        (W[0x2B38] & 0x10) == 0)))))))) {
    uVar4 = check_confirm_button();
    if ((short)uVar4 != 0) {
      W[0x3FB4] = 0;
    }
  }
  else {
    W[0x3FB4] = 0x1a;
  }
  return uVar4;
}

/* Orphaned tail from service_statistics_run — removed */

/* ---- test_mode_display_update ---- */

void test_mode_display_update(void)

{
  uint16_t uVar1;
  short sVar2;
  short sVar3;
  short extraout_D1w;
  uint16_t uVar4;
  char cVar6;
  uint16_t uVar5;
  /* null decl removed */
  undefined2 *puVar7;
  short *psVar8;
  short *psVar9;
  undefined2 *puVar10;
  
  if (W[0xAB08] == 0) {
    text_ram_clear_all();
    text_print_at_position();
    g_sys.videomix[0x0015] = 0;
    g_sys.videomix[0x0011] = 0;
    g_sys.videomix[0x001E] = 0;
    g_sys.videomix[0x000E] = 0;
    mem_write32(0x800000, 0x38);
    sVar2 = 0xb;
    puVar10 = &R[0x3F8E8];
    puVar7 = &W[0xAB44];
    do {
      *puVar7 = *puVar10;
      sVar2 = sVar2 + -1;
      puVar10 = puVar10 + 1;
      puVar7 = puVar7 + 1;
    } while (sVar2 != -1);
    sprite_config_load();
    FUN_0003f36e();
    sVar2 = 0x20;
    uVar1 = 0 /* *NULL */;
  /* null subscript: uVar4 = NULL[1]; */
    uVar5 = uVar1;
    psVar8 = NULL;
    do {
      psVar9 = psVar8 + 1;
      *psVar8 = sVar2;
      sVar2 = sVar2 + 1;
      if (sVar2 == 0x5b) break;
      cVar6 = (char)uVar5;
      uVar5 = (uint16_t)(uint8_t)(cVar6 - 1);
      if (cVar6 == '\0') {
        psVar9 = NULL + 0x40;
        uVar5 = uVar1;
      }
      uVar4 = uVar4 - 1;
      psVar8 = psVar9;
    } while (uVar4 != 0xffff);
    FUN_0003f36e();
    // 0 = 0x5b;
  /* null subscript: NULL[1] = 0x5c; */
  /* null subscript: NULL[2] = 0x5d; */
  /* null subscript: NULL[3] = 0x5f; */
  /* null subscript: NULL[4] = 0x45f; */
  /* null subscript: NULL[5] = 0x85f; */
  /* null subscript: NULL[6] = 0xc5f; */
    FUN_0003f36e();
    sVar3 = 0xf;
    sVar2 = 0x1f;
    psVar8 = NULL + 7;
    do {
      psVar9 = psVar8 + 1;
      *psVar8 = sVar2;
      sVar2 = sVar2 + 0x1000;
      sVar3 = sVar3 + -1;
      psVar8 = psVar9;
    } while (sVar3 != -1);
    do {
      sVar2 = FUN_0003f36e();
      *psVar9 = 0x5e;
    } while (sVar2 != 0);
    puVar10 = &R[0x3F87E];
    sprite_list_init();
    do {
      puVar7 = puVar10 + 1;
      W[0xAB44] = *puVar10;
      puVar10 = puVar10 + 2;
      W[0xAB46] = *puVar7;
      sVar2 = sprite_entry_write();
    } while (sVar2 != 0);
    sprite_mode_set_6();
    sVar2 = 1;
    do {
      puVar10 = &R[0x3F888];
      W[0xAB88] = 2;
      W[0xAB90] = 0;
      sVar3 = R[0x3F886];
      do {
        W[0xAB98] = *puVar10;
        W[0xAB9A] = puVar10[1];
        W[0xABA4] = puVar10[2];
        W[0xABA8] = puVar10[3];
        W[0xABB4] = puVar10[4];
        W[0xABB6] = puVar10[5];
        puVar7 = puVar10 + 7;
        W[0xABC0] = puVar10[6];
        puVar10 = puVar10 + 8;
        W[0xABC4] = *puVar7;
        if (sVar3 == 3) {
          W[0xAB88] = 5;
          W[0xAB90] = 1;
        }
        sprite_data_unpack();
        sVar3 = extraout_D1w + -1;
      } while (sVar3 != -1);
      sVar2 = sVar2 + -1;
    } while (sVar2 != -1);
  }
  W[0xAB08] = W[0xAB08] + 1;
  return;
}

/* ---- test_text_fill_alpha ---- */

void test_text_fill_alpha(void)

{
  short sVar1;
  int iVar2;
  short sVar3;
  short sVar4;
  short sVar5;
  /* null decl removed */
  short *psVar6;
  
  iVar2 = FUN_0003f36e();
  sVar1 = 0 /* *NULL */;
  /* null subscript: sVar4 = NULL[1]; */
  sVar5 = 0x41;
  sVar3 = sVar1;
  psVar6 = NULL;
  do {
    do {
      // 0 = sVar5;
      sVar5 = sVar5 + 1;
      if (sVar5 == 0x5b) {
        sVar5 = 0x41;
      }
      sVar3 = sVar3 + -1;
    } while (sVar3 != -1);
    sVar4 = sVar4 + -1;
    sVar3 = sVar1;
    psVar6 = NULL;
  } while (sVar4 != -1);
  return;
}

/* ---- test_text_fill_blank ---- */

void test_text_fill_blank(void)

{
  short sVar1;
  int iVar2;
  short sVar3;
  short sVar4;
  /* null decl removed */
  undefined2 *puVar5;
  
  iVar2 = FUN_0003f36e();
  sVar1 = 0 /* *NULL */;
  /* null subscript: sVar4 = NULL[1]; */
  sVar3 = sVar1;
  puVar5 = NULL;
  do {
    do {
      // 0 = 0x5f;
      sVar3 = sVar3 + -1;
    } while (sVar3 != -1);
    sVar4 = sVar4 + -1;
    sVar3 = sVar1;
    puVar5 = NULL;
  } while (sVar4 != -1);
  return;
}

/* ---- debug_sound_eeprom ---- */

uint32_t debug_sound_eeprom(void)

{
  uint16_t *puVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  uint16_t uVar5;
  undefined2 uVar6;
  undefined2 uVar7;
  undefined2 uVar8;
  undefined2 uVar9;
  uint16_t *local_c;
  uint16_t *local_8;

  if (W[0x16794] == 0) {
    text_print_string(5,0xe,0x3a39c, 0);
    if ((W[0x2B3C] & 0x10) != 0) {
      W[0x16780] = W[0x16780] + 1;
    }
    if (((W[0x2B3C] & 0x20) != 0) && (W[0x16780] = W[0x16780] + -1, W[0x16780] < 0)) {
      W[0x16780] = 0;
    }
    if ((W[0x2B3A] & 8) != 0) {
      g_sys.commsram[0] = (uint16_t)W[0x16780] | 0x4000;
    }
    if ((W[0x2B3A] & 0x800) != 0) {
      uVar5 = g_sys.commsram[0];
      g_sys.commsram[0] = uVar5 & 0x37ff;
    }
    if ((W[0x2B3A] & 4) != 0) {
      uVar5 = g_sys.commsram[0];
      g_sys.commsram[0] = uVar5 | 0x2000;
    }
  }
  else {
    text_print_string(5,0xe,0x3a393, 0);
    if ((W[0x2B3C] & 0x80) != 0) {
      W[0x16788] = W[0x16788] + 1;
      if (0xff < W[0x16788]) {
        W[0x16788] = 0xff;
      }
      W[0x1678C] = (uint32_t)(uint16_t)g_sys.commsram[0x0100 + (W[0x16788])];
    }
    if ((W[0x2B3C] & 0x40) != 0) {
      W[0x16788] = W[0x16788] + -1;
      if (W[0x16788] < 0) {
        W[0x16788] = 0;
      }
      W[0x1678C] = (uint32_t)(uint16_t)g_sys.commsram[0x0100 + (W[0x16788])];
    }
    if ((W[0x2B3C] & 0x10) != 0) {
      W[0x1678C] = W[0x1678C] + 1;
    }
    if ((W[0x2B3C] & 4) != 0) {
      W[0x1678C] = W[0x1678C] + 0x100;
    }
    if (((W[0x2B3C] & 0x20) != 0) && (W[0x1678C] = W[0x1678C] - 1, (int)W[0x1678C] < 0)) {
      W[0x1678C] = 0;
    }
    if ((W[0x2B3C] & 0x400) != 0) {
      W[0x1678C] = W[0x1678C] - 0x100;
      if ((int)W[0x1678C] < 0) {
        W[0x1678C] = 0;
      }
    }
    if ((W[0x2B3A] & 8) != 0) {
      g_sys.commsram[0x0100 + (W[0x16788])] = (short)W[0x1678C];
      W[0x16790] = 0x3c;
    }
  }
  if ((W[0x2B3C] & 1) != 0) {
    W[0x16784] = W[0x16784] + 1;
    if (0x3f < W[0x16784]) {
      W[0x16784] = 0x3f;
    }
    sound_env_fill(0);
    iVar4 = 0;
    do {
      (&g_audio_env_params)[iVar4] = (short)W[0x16784];
      iVar4 = iVar4 + 1;
    } while (iVar4 < 4);
    eeprom_write_block((intptr_t)0x3fe0,8);
  }
  if ((W[0x2B3C] & 2) != 0) {
    W[0x16784] = W[0x16784] + -1;
    if (W[0x16784] < 0) {
      W[0x16784] = 0;
    }
    sound_env_fill(0);
    iVar4 = 0;
    do {
      (&g_audio_env_params)[iVar4] = (short)W[0x16784];
      iVar4 = iVar4 + 1;
    } while (iVar4 < 4);
    eeprom_write_block((intptr_t)0x3fe0,8);
  }
  if ((W[0x2B3A] & 0x200) != 0) {
    W[0x16794] = W[0x16794] ^ 1;
  }
  uVar9 = 0;
  uVar8 = 3;
  uVar7 = 0;
  uVar6 = 0;
  text_print_string(5,7,0x3a3a5, 0);
  text_draw_hex_digits(0xf,7,3,(short)W[0x16780],uVar6);
  text_print_string(5,9,0x3a3b0, 0);
  text_draw_hex_digits(0xd,9,2,(short)W[0x16784], 0);
  text_print_string(5,0xb,0x3a3b9, 0);
  text_draw_hex_digits(0x11,0xb,2,(short)W[0x16788], 0);
  text_print_string(5,0xc,0x3a3c5, 0);
  text_draw_hex_digits(0x11,0xc,4,(short)W[0x1678C], 0);
  if (W[0x16790] == 0) {
    text_print_string(0x16,0xc,0x3a3d6, 0);
  }
  else {
    text_print_string(0x16,0xc,0x3a3d1, 0);
    W[0x16790] = W[0x16790] + -1;
  }
  text_print_string(0xf,8,0x3a3db, 0);
  local_8 = &g_sys.textram[0x041E];
  local_c = &g_sys.commsram[0x0300];
  do {
    uVar5 = *local_c;
    iVar4 = 0;
    do {
      if ((uVar5 & 0xff) != 0) {
        puVar1 = local_8 + 1;
        tram_w16(local_8, uVar5 & 0xff);
        local_8 = puVar1;
        if (iVar4 != 1) {
          uVar5 = uVar5 >> 8;
        }
      }
      iVar2 = iVar4 + 1;
      iVar3 = iVar4 + -1;
      iVar4 = iVar2;
    } while (iVar2 < 2);
    local_c = local_c + 1;
  } while ((uVar5 & 0xff) != 0);
  return CONCAT22((short)((uint32_t)iVar3 >> 0x10),uVar5) & 0xffff00ff;
}

/* ---- service_statistics_init ---- */

void service_statistics_init(void)

{
  sync_post();
  title_substate_dispatch(0);
  menu_items_clear_highlights(0x6cc0);
  if (W[0x4024] == 0) {
    W[0x1691C] = 0;
  }
  else {
    W[0x1691C] = W[0x4014] / W[0x4024];
  }
  if (W[0x4020] == 0) {
    W[0x16924] = 0;
  }
  else {
    W[0x16924] = W[0x4018] / W[0x4020];
  }
  if (W[0x4022] == 0) {
    W[0x16920] = 0;
  }
  else {
    W[0x16920] = W[0x401C] / W[0x4022];
  }
  W[0x3FB4] = 0x15;
  return;
}


/* ---- service_draw_onoff ---- */

void service_draw_onoff(undefined4 param_1,undefined4 param_2,int param_3)

{
  uint8_t *puVar1;

  if (param_3 == 0) {
    puVar1 = &R[0x3A508];
  }
  else {
    puVar1 = &R[0x3A50C];
  }
  text_print_string(param_1,param_2,puVar1, 0);
  return;
}

/* ---- debug_viewer_draw_hud @ 0x007DF4 ---- */

void debug_viewer_draw_hud(void)

{
  undefined2 uVar1;
  
  if (W[0x0BCC] < 2 || W[0x0BCC] - 2 == 0) {
                    
                    
    /* dispatch stub - needs ROM table */;
    return;
  }
  debug_draw_value(0,5,1,((char*)(g_sys.rom + 0x3a11d)),W[0x0BD0],&g_sys.rom[0x3A124],10);
  debug_draw_value(0,6,2,0xa125,(short)W[0x0D24],0xa124,8);
  debug_draw_value(0,8,1,0xa12b,(short)W[0x0C1C],0xa124,0);
  debug_draw_value(0,9,1,0xa135,(short)W[0x2C1C],0xa124,2);
  if (W[0x0C1C] < 1) {
    uVar1 = 0xe;
  }
  else {
    uVar1 = 2;
  }
  debug_draw_value(0,0xc,8,&g_sys.rom[0x3A124],W[0x1334],&g_sys.rom[0x3A13F],uVar1);
  if (W[0x0C1C] < 2) {
    uVar1 = 0xe;
  }
  else {
    uVar1 = 2;
  }
  debug_draw_value(0,0xd,8,&g_sys.rom[0x3A124],W[0x1338],&g_sys.rom[0x3A143],uVar1);
  if (W[0x0C1C] < 3) {
    uVar1 = 0xe;
  }
  else {
    uVar1 = 2;
  }
  debug_draw_value(0,0xe,8,&g_sys.rom[0x3A124],W[0x133C],&g_sys.rom[0x3A147],uVar1);
  text_print_string(0,0x15,0xa14b);
  debug_draw_value(0,0x16,8,0xa124,(short)W[0x0D00],0xa152,0);
  debug_draw_value(0,0x17,8,0xa124,(short)W[0x0D04],0xa154,0);
  debug_draw_value(0,0x18,8,0xa124,(short)W[0x0D08],0xa156,0);
  return;
}





/* ---- debug_viewer_render_scene @ 0x00787C ---- */

void debug_viewer_render_scene(void)

{
  int iVar1;
  
  dsp_cmd_place_object_rotated(0,W[0x0BC8],W[0x0BF4],0xa000,W[0x0BFC],0,0,0,0);
  dsp_cmd_place_object_rotated(0,0x45,W[0x0BF4],0xb388,W[0x0BFC],0,0,0,0);
  dsp_cmd_place_object_rotated
            (0,8,W[0x0BF4],W[0x0C20] + (W[0x1334] >> 4),W[0x0BFC],0,0,0,0);
  dsp_cmd_place_object_rotated
            (0,0xb,W[0x0BF4],W[0x0C20] + (W[0x1338] >> 4),W[0x0BFC],0,0,0,0);
  dsp_cmd_place_object_rotated
            (0,6,W[0x0BF4],W[0x0C20] + (W[0x133C] >> 4),W[0x0BFC],0,0,0,0);
  for (iVar1 = 0; iVar1 < W[0x1308]; iVar1 = iVar1 + 1) {
    dsp_cmd_place_object
              (0,2,W[0x130C + (iVar1 * 0x50) * 4] + W[0x0D00],
               W[0x1310 + (iVar1 * 0x50) * 4] + W[0x0D04],
               W[0x1314 + (iVar1 * 0x50) * 4] + W[0x0D08]);
  }
  dsp_cmd_end_list(&g_sys.rom[0x02800]);
  dsp_cmd_place_object(0,0x13,0,0xa000,0);
  if (W[0x0BD0] < 3 || W[0x0BD0] - 3 == 0) {
                    
                    
    /* dispatch stub - needs ROM table */;
    return;
  }
  if (W[0x0BCC] == 1 || W[0x0BCC] + -2 == 0) {
                    
                    
    /* dispatch stub - needs ROM table */;
    return;
  }
  dsp_cmd_end_frame();
  return;
}





/* ---- menu_item_get_value @ 0x01A5CE ---- */

int menu_item_get_value(undefined4 *param_1)

{
  int iVar1;
  
  iVar1 = *(short *)(param_1 + 5) + -4;
  if ((int)*(short *)(param_1 + 5) - 1U < 3 || iVar1 == 0) {
                    
                    
    iVar1 = 0; /* dispatch stub - needs ROM table */
    return iVar1;
  }
  return (int)*(short *)*param_1;
}





/* ---- menu_item_set_value @ 0x01A614 ---- */

void menu_item_set_value(undefined4 *param_1,undefined2 param_2)

{
  int iVar1;
  
  iVar1 = *(short *)(param_1 + 5) + -4;
  if ((int)*(short *)(param_1 + 5) - 1U < 3 || iVar1 == 0) {
                    
                    
    /* dispatch stub - needs ROM table */;
    return;
  }
  *(undefined2 *)*param_1 = param_2;
  return;
}





/* ---- ram_test_pattern @ 0x049C62 ---- */

void ram_test_pattern(void)

{
  short sVar1;
  int *pNull = NULL;
  
  /* TODO: ram_test_pattern - Ghidra failed to resolve RAM pointer.
   * Original code writes test patterns (0x55555555, 0xAAAAAAAA) to
   * a memory range and verifies. Hardware test - not needed for reimpl. */
  (void)sVar1; (void)pNull;
  return;
  W[0xC800] = 0xa010000;
  error_report_and_return();
  return;
}





