/*
 * HUD, Text & Sprites
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

/* Forward declarations (called before definition in this file) */
uint32_t tilemap_wind_flash_update();

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

int g_tdb_log = 0;   /* PROPCYCL_TDBLOG=1 */

/* ---- cgram_load_tile_block @ 0x021AE2 ---- */

/* Rewritten from the M68K, which Ghidra had wrong in four separate ways.
 * Disassembled (tools/overnight/cgram_call_gate.py drives MAME's debugger):
 *
 *   021AF8: lsl.l #4,D1            ; D1 = param_1 * 0x10   (table stride)
 *   021AFA: move.l (A3,D1.l),D0    ; D0 = table[param_1].offset
 *   021AFE: lsl.l #6,D0            ; * 0x40
 *   021B00: add.l  D0,D0           ; * 2   ->  offset * 0x80
 *   021B02: adda.l D0,A1           ; A1 = 0x238504 + offset*0x80
 *   021B04: move.l ($4,A3,D1.l),D0 ; w
 *   021B08: muls.l ($8,A3,D1.l),D0 ; w * h
 *   021B0E: move.w D0,D5           ; row count, kept to 16 bits
 *   ...
 *   021B16: move.l ($1c,A7),D0     ; param_2
 *   021B1A: add.l  D2,D0           ; + row
 *   021B1C: lsl.l  #7,D0           ; * 0x80   <-- 0x80 BYTES per row
 *   021B22: add.l  D1,D1           ; col * 2
 *   021B24: add.l  D1,D0
 *   021B2C: move.w (A1)+,(A0,D0.l) ; 16-bit copy into cgram at 0x880000
 *
 * So each row is 0x40 SIXTEEN-BIT words = 0x80 bytes, at both ends. What was
 * here before used 0x40 for both strides, stored only the low byte of each
 * word (g_sys.cgram is uint8_t[]), and read the source with a host-native
 * cast of the big-endian ROM.
 *
 * `move.w` reads two bytes big-endian and writes them big-endian, so at byte
 * level this is a straight copy -- no swap either way.
 *
 * The old "negative param_2" guard is gone with its cause: those callers did
 * not pass negative destinations at all. Ghidra had kept only the LOW BYTE of
 * their `pea $XXX.w` immediates and read it as signed, so 0x390 became -0x70.
 * 22 call-site constants across 5 files were corrected the same way; see
 * mismatch-register row 53. The range check below stays as a cheap net, but
 * it should now never fire -- if it does, that is a regression, not a known
 * defect. */
void cgram_load_tile_block(int param_1,int param_2)

{
    uint32_t tbl = 0x1C4F40 + (uint32_t)param_1 * 0x10;
    uint32_t src = 0x238504 + (uint32_t)(int32_t)vrd32(tbl) * 0x80;
    int w = (int)(int16_t)(int32_t)vrd32(tbl + 4);
    int h = (int)(int16_t)(int32_t)vrd32(tbl + 8);
    int rows = (int)(uint16_t)(w * h);          /* move.w D0,D5 then zero-extend */
    int row, col;

    /* PROPCYCL_CGLOG=1: every block load, with the ROM descriptor it resolved.
     * The destination is what a MAME cgram dump can be diffed against row for
     * row -- 666 of 960 rows differed on the CONTROLS tutorial, and 615 of
     * those were content we never loaded at all rather than content we
     * misplaced, which is a MISSING CALL, not a bad copy. */
    { extern int g_cglog;
      if (g_cglog) printf("[CG] f=%d blk=%-4d dst=0x%04X src=0x%06X w=%d h=%d rows=%d ra=%p\n",
              (int)g_sys.frame_count, param_1, param_2, src, w, h, rows,
              __builtin_return_address(0)); }

    for (row = 0; row < rows; row++) {
        uint32_t dst = (uint32_t)((param_2 + row) * 0x80);
        if (param_2 + row < 0 || dst + 0x80 > CGRAM_SIZE) {
            static int _warned;
            if (!_warned) { _warned = 1;
                printf("  [WARN] cgram_load_tile_block: dest row out of range "
                       "(param_1=%d param_2=%d row=%d) -- skipping\n",
                       param_1, param_2, row); }
            src += 0x80;
            continue;
        }
        for (col = 0; col < 0x40; col++) {
            g_sys.cgram[dst + col * 2]     = R[src];
            g_sys.cgram[dst + col * 2 + 1] = R[src + 1];
            src += 2;
        }
    }
}

/* ---- hud_draw_wings ---- */

void hud_draw_wings(void)

{
  intptr_t puVar1;
  short sVar2;
  uint32_t uVar3;
  int bVar4;
  int iVar5;
  undefined4 uVar6;
  int iVar7;
  undefined4 *puVar8;
  
  if (W[0x0E0C] == 3) {
    /* ROM 0x015C6A: `lea ([$4,D7.l],$0),A0` with D7 = 0xE00D00 -- 68020
     * memory-indirect, so A0 = MEM32[0xE00D04] = W[0x0D04], the PLAYER'S Y.
     * Ghidra collapsed the addressing mode to the constant 0xDC0D00, which
     * put 14,420,224 into every unresolved ground-height slot. Same class as
     * register rows 30 and 90.
     * AND the full extension word 0x79A3 carries a LONG OUTER DISPLACEMENT,
     * $fffc0000 (the bytes FFFC 0000 at 0x015C70): the no-ground shadow height
     * is the player's Y MINUS 0x40000, far under the bike. Without it the
     * shadow wings (codes 144/145) were drawn at the bike's own height over
     * SOLITAR, where MAME has them ~14000 px below the screen (row 186). */
    puVar1 = (int32_t)W[0x0D04] - 0x40000;
    if ((int)W[0x2AA8] < 1) {
      W[0x2AA8] = puVar1;
    }
    if ((int)W[0x2A90] < 1) {
      W[0x2A90] = puVar1;
    }
    if ((int)W[0x2A9C] < 1) {
      W[0x2A9C] = puVar1;
    }
    if ((int)W[0x2A84] < 1) {
      W[0x2A84] = puVar1;
    }
    if ((int)W[0x2A78] < 1) {
      W[0x2A78] = puVar1;
    }
  }
  uVar3 = -(W[0x0D0C] + (W[0x0D58] >> 3)) & 0xfffc;
  sVar2 = (vrd16s(0x20B006 + uVar3));
  if ((sVar2 < 0x5556) && (-0x5556 < sVar2)) {
    if ((vrd16s(0x20B004 + uVar3)) < 0) {
      iVar7 = -0x4e0;
    }
    else {
      iVar7 = 0x4e0;
    }
    W[0x166B4] = ((W[0x2A70] - W[0x2AA0]) * 0x800) / iVar7;
    W[0x166C0] = (W[0x166B4] << 9) / 0x800;
    W[0x166B8] = (((int)W[0x2A78] - (int)W[0x2AA8]) * 0x800) / iVar7;
    W[0x166C4] = (W[0x166B8] << 9) / 0x800;
    if ((W[0x166B8] < (((int)W[0x2AA8] - W[0x0CE0]) * 0x800) / 0x2800) &&
       (W[0x1824] == 0)) {
      return;
    }
    if (0x2b00 < W[0x166B8]) {
      return;
    }
    if (W[0x166B8] < -0x7fff) {
      return;
    }
    iVar5 = W[0x2A74] - W[0x2AA4];
  }
  else {
    if (sVar2 < 0) {
      iVar7 = -0x2b0;
    }
    else {
      iVar7 = 0x2b0;
    }
    W[0x166B4] = (W[0x2A7C] << 0xb) / iVar7;
    W[0x166C0] = (W[0x166B4] << 9) / 0x800;
    W[0x166B8] = (((int)W[0x2A84] - (int)W[0x2AA8]) * 0x800) / iVar7;
    W[0x166C4] = (W[0x166B8] << 9) / 0x800;
    if ((W[0x166B8] < (((int)W[0x2AA8] - W[0x0CE0]) * 0x800) / 0x2800) &&
       (W[0x16E4] == 0)) {
      return;
    }
    if (0x2b00 < W[0x166B8]) {
      return;
    }
    iVar5 = W[0x2A80];
    if (W[0x166B8] < -0x7fff) {
      return;
    }
  }
  W[0x166BC] = (iVar5 << 0xb) / iVar7;
  W[0x166C8] = (W[0x166BC] << 9) / 0x800;
  *(int32_t*)W[0x0CA4] = 0x8008;
  ((int32_t*)W[0x0CA4])[1] = 4;
  ((int32_t*)W[0x0CA4])[2] = 1;
  /* ROM 0x015E56-0x015E5A: `movea.l D6,A0` with D6 = 0x20B002 + (a & 0xfffc),
   * then `move.l (A0),(A2)+` and `move.l ($2,A0),(A2)+`. The master consumes
   * only the LOW 16 bits of each 32-bit word, so those are the entries at
   * 0x20B004 and 0x20B006 -- i.e. (sin, cos). Register row 87's class: the
   * transpile kept the HIGH halves and produced (cos_lagged, sin), a pair
   * that is 90 degrees out AND on the wrong trig lane. */
  ((int32_t*)W[0x0CA4])[3] = vrd16s(0x20B004 + uVar3);   /* sin */
  ((int32_t*)W[0x0CA4])[4] = vrd16s(0x20B006 + uVar3);   /* cos */
  ((int32_t*)W[0x0CA4])[5] = 0;
  ((int32_t*)W[0x0CA4])[6] = 0x7fff;
  uVar3 = -(W[0x0D14] + (W[0x0D5C] >> 3)) & 0xfffc;
  /* ROM 0x015E82 `move.l (A0),(A2)+` -> low half = 0x20B004 = sin; 0x015E84
   * `movea.w ($4,A0),A0` -> 0x20B006 = cos. [7] read the high half, so this
   * pair was (cos, cos) -- not a rotation at all. */
  ((int32_t*)W[0x0CA4])[7] = vrd16s(0x20B004 + uVar3);   /* sin */
  ((int32_t*)W[0x0CA4])[8] = (int)(vrd16s(0x20B006 + uVar3));   /* cos */
  ((int32_t*)W[0x0CA4])[9] = 1;
  puVar8 = (int32_t*)W[0x0CA4] + 0xb;
  ((int32_t*)W[0x0CA4])[10] = 0xffffffff;
  if ((W[0x12BC] == 0) || (W[0x0D04] - (int)W[0x2AA8] <= W[0x3EF4] + 0x1000)) {
    bVar4 = false;
  }
  else {
    *puVar8 = 0x8010;
    ((int32_t*)W[0x0CA4])[0xc] = 3;
    ((int32_t*)W[0x0CA4])[0xd] = W[0x3EF0] + -12000;
    puVar8 = (int32_t*)W[0x0CA4] + 0xf;
    ((int32_t*)W[0x0CA4])[0xe] = 0xffffffff;
    bVar4 = true;
  }
  W[0xEB08] = W[0x0D00] - W[0x0CDC];
  W[0xEB0C] = (int)W[0x2AA8] - W[0x0CE0];
  W[0xEB10] = W[0x0D08] - W[0x0CE4];
  if (W[0x12BC] == 0) {
    uVar6 = 0x4b;
  }
  else {
    uVar6 = 0x33;
  }
  W[0x0CA4] = puVar8;
  hud_draw_wing_element
            (uVar6,W[0x2A88] - W[0x166C0],W[0x2A90] + (-W[0x166C4] - (int)W[0x2AA8]),
             W[0x2A8C] - W[0x166C8],0x8c0);
  if (W[0x12BC] == 0) {
    uVar6 = 0x4c;
  }
  else {
    uVar6 = 0x32;
  }
  hud_draw_wing_element
            (uVar6,W[0x166C0] - W[0x2A94],W[0x2AA8] + (W[0x166C4] - (int)W[0x2A9C]),
             W[0x166C8] - W[0x2A98],0x8c0);
  if (bVar4) {
    puVar8 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = 0x8010;
    W[0x0CA4] = puVar8;
    puVar8 = (int32_t*)W[0x0CA4] + 1;
    *(int32_t*)W[0x0CA4] = (int32_t)0xffffffff;
    W[0x0CA4] = puVar8;
  }
  return;
}


/* ---- hud_init ---- */

int hud_init(void)

{
  int iVar1;
  
  W[0x0E4C] = 0;
  W[0x0E50] = 0;
  W[0x0E48] = W[0x0E44];
  W[0x0E5C] = 0;
  W[0x15ED4] = (W[0x2B38] & 8) != 0;
  W[0x15ED5] = 0;
  W[0x15ED6] = 0;
  W16_SET(0x15ED8, -(short)W[0x0D10]);          /* 0x00DFD4 move.w */
  W[0x15EDC] = 0xa80;
  W[0x15EE0] = 0xffff;
  W[0x15F28] = 0;
  FUN_0000e8b6();
  FUN_0000ee4e();
  iVar1 = W[0x0CBC] + -3;
  if (iVar1 == 0) {
    FUN_0000e9a4();
    iVar1 = FUN_0000ecca();
  }
  return iVar1;
}


/* ---- minimap_draw_targets ---- */

/* ROM 0x0258C4 -- the balloon markers on the results-page map, ported from
 * the ROM. Per course, the map transform is 8 longs at 0x3757C + course*0x20:
 * origin x, origin y, z, scale, xmax, xmin, ymax, ymin. Each target is drawn
 * as 0x390 (still standing -- its value at +0x10 > 0) or, blinking with
 * frame bit 0x18, 0x391, under VIEWPORT 2; popped ones reappear as the
 * results count up (`param` is frames * count / 240, compared against the
 * negative pop stamp at 0xE0478C / +0x30).
 *
 *  - the static balloons are the course's 0x20-byte ROM records from
 *    W[0x16008] + W[0x15FE4]*0x20 (`lea (a0,d0.l*4),a4` with d0 = n*8),
 *    x at +4 and z at +0xC -- BIG-ENDIAN ROM longs, read with vrd32. The C
 *    dereferenced the ROM offset as a host pointer (`*(int *)(iVar14+4)`).
 *  - the moving ones are the 0x3C-byte work-RAM records at 0xE0490C: angle
 *    rate +0x14, radius +0x10, centre +4/+0xC, value +0x18, pop stamp +0x30.
 *    Their cos/sin index is `(a >> 1) & 0x7ffe` scaled *2 (`$20b006(d0.l*2)`,
 *    ext 0x0BB0) -- the C had dropped the *2 (register row 72's class).
 *  - ymax is the long at +0x18 of the course block; the C read ONE BYTE of
 *    the ROM at 0x37594 + course*8 and compared a host pointer against it. */
uint32_t minimap_draw_targets(int param_1)

{
  int i;
  if (!(W16(0xE10) != 0 || W[0x16984] < 8 || W[0x169C4] > 3 || (W[0x0C98] & 0x10) == 0))
    return 0;                                        /* 0x0258D2..0x0258F6: blink off */
  uint32_t cb = 0x3757C + (uint32_t)W[0x0E0C] * 0x20;
  int32_t ox = vrd32s(cb), oy = vrd32s(cb + 4), oz = vrd32s(cb + 8), sc = vrd32s(cb + 12);
  int32_t xmax = vrd32s(cb + 16), xmin = vrd32s(cb + 20), ymax = vrd32s(cb + 24), ymin = vrd32s(cb + 28);
  int32_t lim = param_1 << 6;
  uint32_t rec = (uint32_t)W[0x16008] + (uint32_t)W[0x15FE4] * 0x20;
  for (i = 0; i < W[0x15FF0]; i++, rec += 0x20) {          /* 0x025940 */
    int32_t v, x, y; int skip = 0;
    v = (vrd32s(rec + 4) >> 8) * sc;  if (v < 0) v += 0xff;  x = ox + (v >> 8);
    if (x > xmax || x < xmin) skip = 1;
    v = (vrd32s(rec + 12) >> 8) * sc; if (v < 0) v += 0xff;  y = oy + (v >> 8);
    if (y > ymax || y < ymin) skip = 1;
    int32_t model = (vrd32s(rec + 16) > 0 || (W[0x0C98] & 0x18) != 0) ? 0x390 : 0x391;
    if (skip) continue;
    int32_t st = W_A16(0x478C, i);                   /* $e0478c(d2.l*2) */
    if (-st > lim) dsp_cmd_place_object_rotated_abs(2, model, x, y, oz, 0, 0, 0, 0);
    if (st == 0)   dsp_cmd_place_object_rotated_abs(2, model, x, y, oz, 0, 0, 0, 0);
  }
  for (i = 0; i < W[0x15FEC]; i++) {                       /* 0x025A30 */
    uint32_t r = 0x490C + i * 0x3c;
    int32_t a = (param_1 * (int32_t)W[r + 0x14]) << 6, idx = (a >> 1) & 0x7ffe;
    int32_t c = vrd16s(0x20B006 + idx * 2) >> 7, s = vrd16s(0x20B004 + idx * 2) >> 7;
    int32_t px = (((c * (int32_t)W[r + 0x10]) >> 8) + (int32_t)W[r + 4]) >> 8;
    int32_t pz = (((s * (int32_t)W[r + 0x10]) >> 8) + (int32_t)W[r + 12]) >> 8;
    int32_t v, x, y; int skip = 0;
    v = px * sc; if (v < 0) v += 0xff; x = ox + (v >> 8);
    v = pz * sc; if (v < 0) v += 0xff; y = oy + (v >> 8);
    if (x > xmax || x < xmin) skip = 1;
    if (y > ymax || y < ymin) skip = 1;
    int32_t model = ((int32_t)W[r + 0x18] > 0 || (W[0x0C98] & 0x18) != 0) ? 0x390 : 0x391;
    if (skip) continue;
    int32_t st = (int32_t)W[r + 0x30];
    if (-st > lim) dsp_cmd_place_object_rotated_abs(2, model, x, y, oz, 0, 0, 0, 0);
    if (st == 0)   dsp_cmd_place_object_rotated_abs(2, model, x, y, oz, 0, 0, 0, 0);
  }
  return 0;
}


/* ---- minimap_draw_trail ---- */

void minimap_draw_trail(int param_1)

{
  int iVar1;
  int iVar2;
  int iVar3;
  undefined4 uVar4;
  int iVar5;
  int iVar6;
  int iVar7;
  int bVar9;
  int iVar10;
  int iVar11;
  int iVar12;
  int iVar13;
  undefined4 uVar15;
  
  iVar10 = W[0x17034];
  if (param_1 < W[0x17034]) {
    iVar10 = param_1;
  }
  /* ROM 0x0257C8..0x0257DE: `movea.l (a4),a0 ; cmpa.w $e169da.l,a0` then
   * `move.w d0,$e169dc.l` / `move.w d2,$e169da.l`. Both are 16-bit fields:
   * 0x169DA is the LOW half of slot 0x169D8 (a 2-mod-4 offset that
   * sync_wram_to_W rebuilds from its neighbours when written whole), 0x169DC
   * the HIGH half of slot 0x169DC, whose LOW half is results_rank_blink's
   * flag 0x169DE. Written whole-slot, the "trail finished drawing" flag could
   * never become 1, and the ADVANCED results screen (results_screen_update_boss
   * phase 5, `tst.w $e169dc` at 0x024A26) waited on it forever. */
  W16_SET(0x169DC, ((int32_t)W[0x17034] == (int32_t)W16(0x169DA)) ? 1 : 0);
  W16_SET(0x169DA, iVar10);
  iVar1 = W[0x0E0C] * 0x20;
  iVar2 = (int32_t)vrd32(0x3757C + iVar1);
  iVar3 = (int32_t)vrd32(0x37580 + iVar1);
  uVar4 = vrd32(0x37584 + iVar1);
  iVar5 = (int32_t)vrd32(0x37588 + iVar1);
  iVar6 = (int32_t)vrd32(0x3758C + iVar1);
  iVar7 = (int32_t)vrd32(0x37590 + iVar1);
  /* ROM 0x025812: a3 = $e16d88(d2.l*4) and the loop reads `move.w (a3)+`
   * pairs stepping BACK one point per iteration (net -4, the reads at
   * 0x25822/0x2584A with the -8 at 0x258B2). The old body built a HOST
   * short* into _W[] at byte offset 0x16D88 + iVar10 (stride 1, not 4,
   * and host-half reads of the slots), so it never read a real point.
   * The lower map bound is the vrd32(0x37594) long, not a rom byte at
   * stride 8. */
  for (iVar13 = 0; iVar13 < iVar10; iVar13 = iVar13 + 1) {
    int idx = iVar10 - iVar13;              /* newest first, down to 1 */
    int sx = W16(0x16D88 + idx * 4);
    int sz = W16(0x16D88 + idx * 4 + 2);
    bVar9 = false;
    iVar11 = iVar5 * sx;
    if (iVar11 < 0) {
      iVar11 = iVar11 + 0xff;
    }
    iVar11 = iVar2 + (iVar11 >> 8);
    if ((iVar6 < iVar11) || (iVar11 < iVar7)) {
      bVar9 = true;
    }
    iVar12 = iVar5 * sz;
    if (iVar12 < 0) {
      iVar12 = iVar12 + 0xff;
    }
    iVar12 = iVar3 + (iVar12 >> 8);
    if ((((int32_t)vrd32(0x37594 + iVar1)) < iVar12) ||
        (iVar12 < (int32_t)vrd32(0x37598 + iVar1))) {
      bVar9 = true;
    }
    if (!bVar9) {
      if (iVar13 == 0) {
        uVar15 = 0x38d;
      }
      else {
        uVar15 = 0x38c;
      }
      dsp_cmd_place_object_rotated_abs(2,uVar15,iVar11,iVar12,uVar4,0,0,0,0);
    }
  }
  return;
}


/* ---- text_draw_hex_digits ---- */

void text_draw_hex_digits(int param_1,int param_2,int param_3,uint32_t param_4,undefined4 param_5)

{
  int iVar1;
  uint16_t *puVar2;
  uint16_t *puVar3;
  
  puVar3 = (uint16_t *)(&g_sys.cgram[0x1DFFE] + (param_3 + param_1) * 2 + param_2 * 0x80);
  iVar1 = param_3;
  while (iVar1 = iVar1 + -1, -1 < iVar1) {
    puVar2 = puVar3 + -1;
    /* ROM 0x021108 `move.w d0,(a0)` into the big-endian tilemap: the
     * host-native store laid the digit down byte-swapped (register row 59). */
    tram_w16(puVar3, (unsigned)(param_5 << 0xc) | ((uint16_t)param_4 & 0xf));
    param_4 = param_4 >> 4;
    puVar3 = puVar2;
    if (iVar1 == param_3 + -4) {
      param_5 = param_5 + 1;
    }
  }
  return;
}


/* ---- text_draw_number ---- */

void text_draw_number(int param_1, int param_2, int param_3)

{
  /* NOT a number printer -- it is the COLUMN REVEAL that copies a character
   * block's graphics into cgram one 8-byte slice per frame, which is how
   * every banner in the game wipes in. param_2 is the destination cgram TILE
   * ROW, param_3 the slice 0..15 (a 16x16 4bpp tile is 16 rows of 8 bytes).
   *
   * Both start pointers were computed at HALF their byte offset. Ghidra typed
   * them `undefined2 *` and then wrote the arithmetic against `&g_sys.cgram[0]`
   * / `&R[...]`, which are uint8_t[] -- so `param_2 * 0x40 + param_3 * 4` was
   * taken in BYTES where the M68K uses a 0x80 row stride and an 8-byte slice:
   *     021D02: lsl.l #2,d1 ; add.l d1,d1        ; param_3 * 8
   *     021D0A: lsl.l #7,d0                      ; param_2 * 0x80
   *     021D1C: move.l (a4,d0.l),d0 ; lsl.l #6,d0 ; add.l d1',d0 ; add.l d0,d0
   *                                              ; (srcidx*0x40 + param_3*4)*2
   * The row STRIDE inside the loop was right (`+ 0x40` on an undefined2 *),
   * so every block landed at half its row and read half its source index.
   * Measured on the CONTROLS tutorial: MAME's cgram row 0x160 is source index
   * 4535 (block 176, the "TO TURN LEFT" caption) and ours was index 64 -- the
   * gold plank that drew where the caption belongs. */
  uint32_t tbl  = 0x1C4F40 + (uint32_t)param_1 * 0x10;
  uint32_t srci = (uint32_t)(int32_t)vrd32(tbl);
  int      w    = (int32_t)vrd32(tbl + 4);
  int      h    = (int32_t)vrd32(tbl + 8);
  uint32_t src  = 0x238504 + srci * 0x80 + (uint32_t)param_3 * 8;
  uint32_t dst  = (uint32_t)param_2 * 0x80 + (uint32_t)param_3 * 8;
  int rows = w * h, i, k;

  if (param_2 < 0 || param_3 < 0 || param_3 > 15) return;
  for (i = 0; i < rows; i++) {
    if (dst + 8 > CGRAM_SIZE || src + 8 > ROM_SIZE) break;
    for (k = 0; k < 8; k++) g_sys.cgram[dst + k] = R[src + k];
    dst += 0x80;
    src += 0x80;
  }
}


/* ======= Banner picker (game_ui debug tool) =======
 * Every `text_draw_rect_blink` call is a rectangular block of the text
 * TILEMAP (not a sprite) -- a "banner" in the sense the player sees on
 * screen: title text, tutorial captions, the credits/coin banner, results
 * strings. Register row 62 already found 58 of these call sites had the
 * WRONG NUMBER OF ARGUMENTS from decompilation (the base tile code and
 * palette silently dropped, drawing tile 0), so a tool to look at each
 * one's actual (row, col, w, h, base, palette) live and highlight its
 * on-screen rectangle is the direct way to catch "that one still looks
 * wrong" -- built on the same PROPCYCL_TDBLOG instrumentation this file
 * already had for offline log-reading. */
typedef struct { int col, row, w, h, base, pal; } banner_call;
#define BANNER_MAX 64
static banner_call g_banner_calls[BANNER_MAX];
static int g_banner_n;
void banner_calls_reset(void) { g_banner_n = 0; }
int  banner_calls_count(void) { return g_banner_n; }
void banner_calls_get(int i, int *col, int *row, int *w, int *h, int *base, int *pal) {
    if (i < 0 || i >= g_banner_n) {
        if (col) *col=-1; if (row) *row=-1; if (w) *w=0; if (h) *h=0;
        if (base) *base=0; if (pal) *pal=0; return;
    }
    banner_call *b = &g_banner_calls[i];
    if (col) *col=b->col; if (row) *row=b->row; if (w) *w=b->w; if (h) *h=b->h;
    if (base) *base=b->base; if (pal) *pal=b->pal;
}

/* ---- text_draw_rect_blink ---- */

void text_draw_rect_blink(int param_1,int param_2,int param_3,undefined4 param_4,undefined4 param_5)

{
  int iVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  uint16_t *puVar5;
  
  /* Read width and height from ROM block descriptor table (big-endian) */
  {
    int roff = 0x1C4F44 + param_1 * 0x10;
    iVar1 = (R[roff]<<24)|(R[roff+1]<<16)|(R[roff+2]<<8)|R[roff+3];
    roff += 4;
    iVar2 = (R[roff]<<24)|(R[roff+1]<<16)|(R[roff+2]<<8)|R[roff+3];
    /* Sanity clamp — prevent wild loops from bad ROM reads */
    if (iVar1 < 0 || iVar1 > 64) iVar1 = 0;
    if (iVar2 < 0 || iVar2 > 64) iVar2 = 0;
  }
  W[0x3EA4] = iVar1;
  W[0x3EA8] = iVar2;
  /* PROPCYCL_TDBLOG=1: log every banner draw (block, col, row, base code,
   * palette) with the w/h the ROM descriptor gives. Row 35 turned on knowing
   * which call places which block -- several call sites lost their last two
   * arguments entirely in decompilation, so every banner drew tile code 0. */
  { extern int g_tdb_log;
    if (g_tdb_log) printf("[TDB] f=%d blk=%d col=%d row=%d base=0x%x pal=%d w=%d h=%d ret=%p\n",
        (int)g_sys.frame_count, param_1, param_2, param_3,
        (unsigned)(uintptr_t)param_4, (int)(uintptr_t)param_5, iVar1, iVar2,
        __builtin_return_address(0)); }
  if (g_banner_n < BANNER_MAX) {
      g_banner_calls[g_banner_n] = (banner_call){
          param_2, param_3, iVar1, iVar2,
          (int)(uintptr_t)param_4, (int)(uintptr_t)param_5 };
      g_banner_n++;
  }
  for (iVar3 = 0; iVar3 < iVar2; iVar3 = iVar3 + 1) {
    puVar5 = (uint16_t *)((intptr_t)&g_sys.textram[0] + param_2 * 2 + (iVar3 + param_3) * 0x80);
    for (iVar4 = 0; iVar4 < iVar1; iVar4 = iVar4 + 1) {
      tram_w16(puVar5, param_4 | param_5 << 0xc);
      param_4 = param_4 + 1;
      puVar5 = puVar5 + 1;
    }
  }
  return;
}


/* ---- text_draw_rect_solid ---- */

void text_draw_rect_solid(int param_1,int param_2,int param_3)

{
  uint32_t uVar1;
  uint32_t uVar2;
  uint32_t uVar3;
  uint32_t uVar4;
  undefined2 *puVar5;
  
  /* Read width and height from ROM block descriptor table (big-endian) */
  {
    int roff = 0x1C4F44 + param_1 * 0x10;
    uVar1 = (R[roff]<<24)|(R[roff+1]<<16)|(R[roff+2]<<8)|R[roff+3];
    roff += 4;
    uVar2 = (R[roff]<<24)|(R[roff+1]<<16)|(R[roff+2]<<8)|R[roff+3];
    if (uVar1 > 64) uVar1 = 0;
    if (uVar2 > 64) uVar2 = 0;
  }
  for (uVar3 = 0; uVar3 < uVar2; uVar3 = uVar3 + 1) {
    puVar5 = (undefined2 *)((intptr_t)&g_sys.textram[0] + param_2 * 2 + (uVar3 + param_3) * 0x80);
    for (uVar4 = 0; uVar4 < uVar1; uVar4 = uVar4 + 1) {
      tram_w16(puVar5, 0x20);
      puVar5 = puVar5 + 1;
    }
  }
  return;
}


/* ---- text_draw_signed_decimal ---- */

void text_draw_signed_decimal(int param_1,int param_2,uint32_t param_3,uint32_t param_4,undefined4 param_5)

{
  uint32_t uVar1;
  char cVar2;
  uint16_t *puVar3;
  
  cVar2 = '+';
  if ((int)param_4 < 0) {
    param_4 = -param_4;
    cVar2 = '-';
  }
  tram_w16(&g_sys.cgram[0x1DFFE] + param_1 * 2 + param_2 * 0x80,
           (unsigned)(param_5 << 0xc) | (uint8_t)cVar2);
  puVar3 = (uint16_t *)(&g_sys.cgram[0x1DFFE] + (param_3 + param_1) * 2 + param_2 * 0x80);
  for (uVar1 = 0; uVar1 < param_3; uVar1 = uVar1 + 1) {
    if ((param_4 == 0) && (uVar1 != 0)) {
      tram_w16(puVar3, 0x20);
    }
    else {
      tram_w16(puVar3, (unsigned)(param_5 << 0xc) | (uint16_t)(param_4 % 10));
      param_4 = param_4 / 10;
    }
    puVar3 = puVar3 + -1;
  }
  return;
}


/* ---- text_print_string ---- */

void text_print_string(int param_1,int param_2,char *param_3,undefined4 param_4)

{
  uint16_t *puVar1;

  /* Fix transpilation bug: raw M68K ROM addresses (< 0x400000) passed as pointers.
   * Redirect to g_sys.rom + offset. */
  if ((uintptr_t)param_3 < ROM_SIZE) {
    param_3 = (char *)&g_sys.rom[(uintptr_t)param_3];
  }

  puVar1 = (uint16_t *)((intptr_t)&g_sys.textram[0] + param_1 * 2 + param_2 * 0x80);
  int max_chars = (TEXTRAM_SIZE - (param_1 * 2 + param_2 * 0x80)) / 2;
  int count = 0;
  while (*param_3 != '\0' && count < max_chars && count < 64) {
    tram_w16(puVar1, (unsigned)(param_4 << 0xc) | (uint8_t)*param_3);
    puVar1 = puVar1 + 1;
    param_3 = param_3 + 1;
    count++;
  }
  return;
}


/* ---- tilemap_enable_set ---- */

void tilemap_enable_set(void)

{
  tilemap_wind_flash_update(0);
  return;
}


/* ---- tilemap_scroll_set ---- */

void tilemap_scroll_set(int param_1)

{
  uint16_t uVar1;
  
  if (param_1 == 0) {
    uVar1 = comms_r16(g_sys.commsram, 0x7D2E);
    comms_w16(g_sys.commsram, 0x7D2E, uVar1 & 0xfffb);
  }
  else {
    uVar1 = comms_r16(g_sys.commsram, 0x7D2E);
    comms_w16(g_sys.commsram, 0x7D2E, uVar1 | 4);
  }
  return;
}


/* ---- hud_draw_wing_element ---- */

void hud_draw_wing_element(undefined4 param_1,int param_2,int param_3,int param_4,int param_5)

{
  int iVar1;
  undefined4 *puVar2;
  
  iVar1 = (param_3 << 0xb) / param_5;
  if ((iVar1 < 0x2801) && (-0x2801 < iVar1)) {
    *(int32_t*)W[0x0CA4] = 0x8008;
    ((int32_t*)W[0x0CA4])[1] = 3;
    ((int32_t*)W[0x0CA4])[2] = 6;
    ((int32_t*)W[0x0CA4])[3] = (param_2 << 0xb) / param_5;
    ((int32_t*)W[0x0CA4])[4] = 0;
    ((int32_t*)W[0x0CA4])[5] = W[0x166B4];
    ((int32_t*)W[0x0CA4])[6] = iVar1;
    ((int32_t*)W[0x0CA4])[7] = 0;
    ((int32_t*)W[0x0CA4])[8] = W[0x166B8];
    ((int32_t*)W[0x0CA4])[9] = (param_4 << 0xb) / param_5;
    ((int32_t*)W[0x0CA4])[10] = 0;
    ((int32_t*)W[0x0CA4])[0xb] = W[0x166BC];
    ((int32_t*)W[0x0CA4])[0xc] = 0xffffffff;
    ((int32_t*)W[0x0CA4])[0xd] = 0x8009;
    ((int32_t*)W[0x0CA4])[0xe] = 4;
    ((int32_t*)W[0x0CA4])[0xf] = 3;
    ((int32_t*)W[0x0CA4])[0x10] = 3;
    ((int32_t*)W[0x0CA4])[0x11] = 0x800a;
    ((int32_t*)W[0x0CA4])[0x12] = param_1;
    ((int32_t*)W[0x0CA4])[0x13] = 3;
    ((int32_t*)W[0x0CA4])[0x14] = W[0xEB08];
    puVar2 = (int32_t*)W[0x0CA4] + 0x16;
    ((int32_t*)W[0x0CA4])[0x15] = W[0xEB0C];
    W[0x0CA4] = W[0x0CA4] + 0x17 * 4;
    *puVar2 = W[0xEB10];
  }
  return;
}

/* ---- sprite_3d_project_and_draw ---- */

/* A world-space BILLBOARD (the terrain spray/dust particles), rewritten from
 * the listing at ROM 0x023548 (register row 195). Three faults in the
 * transpile:
 *  - the per-sprite SHAPE word is `movea.w $b79c0(d0.l*2)` -- entry
 *    `param_2`, big-endian; the C read entry 0 host-native and added
 *    param_2*2 to its VALUE, so every billboard got a garbage size class;
 *  - the three field-of-view trig reads use extension word 0x0A02, a SCALE-2
 *    index capstone prints without its scale, and lost their *2;
 *  - the fifth argument to sprite_draw_2d is `pea ([$20,a6],d4.l)`: the
 *    caller's SEVENTH argument (a z bias, -0x400000) PLUS the projected
 *    depth. The C had no seventh parameter and passed z = 0, so the
 *    billboards never sorted against the scene. */
static inline int32_t s3d_cos(uint32_t a) { return vrd16s(0x20B006 + ((a >> 1) & 0x7ffe) * 2) >> 7; }
static inline int32_t s3d_sin(uint32_t a) { return vrd16s(0x20B004 + ((a >> 1) & 0x7ffe) * 2) >> 7; }

void sprite_3d_project_and_draw(int param_1, int param_2, int param_3, int param_4,
                                int param_5, int32_t param_6, int32_t param_7)
{
  int32_t t8[8];                                     /* ROM 0x3AC14: [8,1,2,...,7] */
  int i;
  for (i = 0; i < 8; i++) t8[i] = (int32_t)vrd32(0x3AC14 + i * 4);

  uint32_t h = (uint32_t)-(int32_t)W[0x0CEC];       /* heading, negated */
  uint32_t p = (uint32_t)-(int32_t)W[0x0CE8];       /* pitch, negated */
  uint32_t r = (uint32_t)(int32_t)W[0x0CF0];        /* roll */
  int32_t x = param_3, y = param_4, z = param_5;
  int32_t x1 = (s3d_cos(h) * x - s3d_sin(h) * z) >> 8;
  int32_t z1 = (s3d_sin(h) * x + s3d_cos(h) * z) >> 8;
  int32_t d  = (s3d_cos(p) * z1 - s3d_sin(p) * y) >> 8;   /* depth */
  int32_t y1 = (s3d_sin(p) * z1 + s3d_cos(p) * y) >> 8;
  int32_t x2 = (s3d_cos(r) * x1 - s3d_sin(r) * y1) >> 8;
  int32_t y2 = (s3d_sin(r) * x1 + s3d_cos(r) * y1) >> 8;
  if (d <= 0) return;

  uint32_t fov = (((uint32_t)dsp_r32(0x10038 + 4 * (W[0x0CA0] * 0x2000 + param_1 * 0x20)) >> 5) << 13) / 0x5a;
  W[0x3E90] = (int32_t)fov;
  uint32_t fi = ((int32_t)fov >> 1) & 0x7ffe;       /* ext 0x0A02: index *2 */
  int32_t fc = vrd16s(0x20B006 + fi * 2) >> 7;
  int32_t fs = vrd16s(0x20B004 + fi * 2) >> 7;
  int32_t focal = (fc * 0x140) >> 8;
  int32_t den = (fs * d) >> 8;
  if (den == 0) return;
  int32_t size = m68k_divs(fc * 0x3e800, den) >> 8;
  if (size < 1) size = 1;
  if (size > 0x100) size = 0x100;

  int32_t shape = vrd16s(0xB79C0 + param_2 * 2);
  int32_t hy = t8[shape & 7] * size;        if (hy < 0) hy++;  hy >>= 1;
  int32_t hx = t8[(shape >> 4) & 7] * size; if (hx < 0) hx++;  hx >>= 1;

  int32_t sx = m68k_divs(x2 * focal, den) - hx + 0x140;
  int32_t sy = m68k_divs(-y2 * focal, den) - hy + 0xf0;
  if (sx > 0x500 || sx < -0x500 || sy > 0x500 || sy < -0x500) return;

  extern int g_spr3d_tag;
  g_spr3d_tag = 1;
  sprite_draw_2d(7 - param_1, param_2, sx, sy, param_7 + d, size, size, param_6, 0);
  g_spr3d_tag = 0;
}

/* ---- hud_draw_3d_sprite ---- */

void hud_draw_3d_sprite(int param_1,undefined4 param_2,undefined4 param_3,int param_4,int param_5,
                       uint32_t param_6,uint32_t param_7,uint32_t param_8,undefined4 param_9)

{
  *(int32_t*)W[0x0CA4] = 0x8008;
  param_1 = param_1 + 3;
  ((int32_t*)W[0x0CA4])[1] = param_1;
  ((int32_t*)W[0x0CA4])[2] = 1;
  /* ROM 0x016D9E: `lea -$2(a4,d0.l),a3` with a4 = 0x20B004, then
   * `move.l (a3) ; move.l $2(a3)` -- the LOW halves, i.e. sin@0x20B004+a
   * then cos@0x20B006+a (register row 87). The transpile took the high
   * halves, (cos, sin): at angle 0 that is (0x7FFF, 0), read sin-first as
   * 90 degrees about every axis, which turned every plate of the count-down
   * info board ("TRY FOR A PERFECT SCORE", the map) edge-on. */
  ((int32_t*)W[0x0CA4])[3] = vrd16s(0x20B004 + (param_6 & 0xfffc));
  ((int32_t*)W[0x0CA4])[4] = vrd16s(0x20B006 + (param_6 & 0xfffc));
  ((int32_t*)W[0x0CA4])[5] = vrd16s(0x20B004 + (param_7 & 0xfffc));
  ((int32_t*)W[0x0CA4])[6] = vrd16s(0x20B006 + (param_7 & 0xfffc));
  ((int32_t*)W[0x0CA4])[7] = vrd16s(0x20B004 + (param_8 & 0xfffc));
  ((int32_t*)W[0x0CA4])[8] = vrd16s(0x20B006 + (param_8 & 0xfffc));
  ((int32_t*)W[0x0CA4])[9] = param_9;
  ((int32_t*)W[0x0CA4])[10] = 0;
  ((int32_t*)W[0x0CA4])[0xb] = param_3;
  ((int32_t*)W[0x0CA4])[0xc] = param_4 + 800;
  ((int32_t*)W[0x0CA4])[0xd] = param_5 + -0x350;
  ((int32_t*)W[0x0CA4])[0xe] = 0xffffffff;
  ((int32_t*)W[0x0CA4])[0xf] = 0x8009;
  ((int32_t*)W[0x0CA4])[0x10] = param_1;
  ((int32_t*)W[0x0CA4])[0x11] = 4;
  ((int32_t*)W[0x0CA4])[0x12] = param_1;
  ((int32_t*)W[0x0CA4])[0x13] = 0x800a;
  ((int32_t*)W[0x0CA4])[0x14] = param_2;
  ((int32_t*)W[0x0CA4])[0x15] = param_1;
  ((int32_t*)W[0x0CA4])[0x16] = W[0xEB08];
  ((int32_t*)W[0x0CA4])[0x17] = W[0xEB0C];
  ((int32_t*)W[0x0CA4])[0x18] = W[0xEB10];
  W[0x0CA4] = W[0x0CA4] + 0x19 * 4;
  return;
}

/* ---- tilemap_draw_time_value ---- */

void tilemap_draw_time_value(int param_1,int param_2,uint32_t param_3,uint32_t param_4,short param_5)

{
  uint32_t uVar1;
  short sVar2;
  short sVar3;
  short *psVar4;
  
  if (param_3 < 5) {
    uVar1 = param_3;
    if (2 < param_3) {
      uVar1 = param_3 + 1;
    }
  }
  else {
    uVar1 = param_3 + 2;
  }
  psVar4 = (short *)((intptr_t)&g_sys.textram[0] + (param_1 + uVar1) * 2 + param_2 * 0x80);
  param_4 = param_4 / 0x3c;
  sVar3 = 0;
  _safety_ctr = 0;
  do {
    if (param_3 <= (uint32_t)(int)sVar3) {
      return;
    }
    if (sVar3 == 2) {
      sVar2 = 0x22;
LAB_0001a38e:
      psVar4 = psVar4 + -1;
      tram_w16(psVar4, (unsigned)(param_5 * 0x1000 + sVar2));
    }
    else if (sVar3 == 4) {
      sVar2 = 0x27;
      goto LAB_0001a38e;
    }
    psVar4 = psVar4 + -1;
    tram_w16(psVar4, (unsigned)(param_5 * 0x1000 +
             (short)(param_4 % (uint32_t)(int)(*(int16_t*)(int32_t*)&g_sys.rom[0x36682] + sVar3 * 2))));
    param_4 = param_4 / (uint32_t)(int)(*(int16_t*)(int32_t*)&g_sys.rom[0x36682] + sVar3 * 2);
    sVar3 = sVar3 + 1;
  if (++_safety_ctr > 10000) break; } while( true ); _safety_ctr = 0;
}

/* ---- tilemap_draw_decimal ---- */

void tilemap_draw_decimal(int param_1,int param_2,int param_3,int param_4,uint16_t param_5)

{
  short sVar1;
  uint16_t *puVar2;
  
  puVar2 = (uint16_t *)((intptr_t)&g_sys.textram[0] + (param_3 + param_1) * 2 + param_2 * 0x80);
  for (sVar1 = 0; sVar1 < param_3; sVar1 = sVar1 + 1) {
    puVar2 = puVar2 + -1;
      tram_w16(puVar2, param_5 | (uint16_t)(param_4 % 10));
    param_4 = param_4 / 10;
  }
  return;
}

/* ---- sprite_config_load ---- */

undefined4 sprite_config_load(void)

{
  /* void */;
  short sVar1;
  undefined4 *puVar2;
  undefined2 *puVar3;
  
  W[0xAB84] = 0;
  sVar1 = 0x21;
  puVar2 = NULL + 1;
  puVar3 = &W[0xAB88];
  do {
    *puVar3 = *(undefined2 *)puVar2;
    sVar1 = sVar1 + -1;
    puVar2 = (undefined4 *)((intptr_t)puVar2 + 2);
    puVar3 = puVar3 + 1;
  } while (sVar1 != -1);
  return 0;
}

/* ---- sprite_data_unpack ---- */

uint64_t sprite_data_unpack(void)

{
  uint32_t uVar1;
  uint16_t uVar2;
  /* void */;
  /* void */;
  uint32_t uVar3;
  short sVar4;
  uint16_t *puVar5;
  uint32_t *puVar6;
  uint32_t *puVar7;
  uint32_t *puVar8;
  uint32_t *puVar9;
  
  uVar1 = 0;
  uVar3 = uVar1 & 0xfff;
  if ((0 & 1) != 0) {
    uVar3 = uVar3 | 0x8000;
  }
  // 0 = uVar3;
  /* null access: NULL[1] = uVar1 >> 0xc & 0xfff; */
  sVar4 = (*(int16_t*)NULL + 6);
  uVar2 = (*(int16_t*)(0));
  puVar7 = NULL + 4;
  /* null access: NULL[2] = (*(int16_t*)NULL + 0xe) & 0xf | */
             (uint16_t)((*(int16_t*)(0)) << 4) & 0xf0 |
             (uint32_t)(uint16_t)((*(int16_t*)NULL + 10) << 0xf) |
             (uint16_t)((*(int16_t*)(0)) << 8) & 0x7f00;
  puVar8 = NULL + 4;
  /* null access: NULL[3] = uVar2 & 3 | (uint16_t)(sVar4 << 2) & 0x7ffc; */
  sVar4 = 3;
  do {
    uVar1 = *puVar7;
    *puVar8 = (uint32_t)*(uint16_t *)puVar7;
    puVar8[1] = (uint32_t)(uint16_t)uVar1;
    uVar2 = (*(int16_t*)puVar7 + 6);
    puVar8[2] = (uint32_t)(*(int16_t*)(puVar7 + 1));
    puVar8[3] = (uint32_t)uVar2;
    puVar5 = (uint16_t *)((intptr_t)puVar7 + 10);
    puVar6 = puVar7 + 3;
    puVar7 = (uint32_t *)((intptr_t)puVar7 + 0xe);
    puVar9 = puVar8 + 5;
    puVar8[4] = (uint32_t)(uint16_t)(*(short *)puVar6 << 8) | *puVar5 & 0x3f;
    puVar8 = puVar8 + 6;
    *puVar9 = 0;
    sVar4 = sVar4 + -1;
  } while (sVar4 != -1);
  if ((0 & 1) != 0) {
    *puVar8 = 0xffffffff;
  }
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- sprite_dma_kick ---- */

void sprite_dma_kick(void)

{
  /* Stub — original ROM 0x023022 fires the DMA to spriteram. Our renderer
   * reads directly from the W[] sprite list each frame, so the actual DMA
   * isn't needed. The list reset MUST happen before the next frame's
   * sprite_draw_2d calls, not after them — see main.c renderer-side reset
   * (renderer2d_draw_tilemap) which clears the count after consuming the
   * list. */
  return;
}

/* ---- sprite_draw_multi_segment ---- */

void sprite_draw_multi_segment
               (undefined4 param_1,int param_2,int param_3,undefined4 param_4,undefined4 param_5,
               int param_6,undefined4 param_7,undefined4 param_8,undefined4 param_9)

{
  short sVar1;
  short sVar2;
  short sVar3;
  
  if (param_2 < 0) {
    sVar3 = -1;
    param_2 = -param_2;
  }
  else {
    sVar3 = 1;
  }
  sVar1 = (vrd16s(0x154328 + (param_2 * 0x80)));
  for (sVar2 = 1; sVar2 < (short)(sVar1 + 1); sVar2 = sVar2 + 1) {
    sprite_draw_2d(param_1,(int)sVar3 *
                           (uint32_t)(vrd16s(0x154328 + (param_2 * 0x80 + sVar2 * 2))),param_3,
                   param_4,param_5,param_6,param_7,param_8,param_9);
    param_3 = param_6 + param_3;
  }
  return;
}

/* ---- sprite_entry_write ---- */

uint64_t sprite_entry_write(void)

{
  short sVar1;
  uint32_t uVar2;
  /* void */;
  int iVar3;
  /* void */;
  
  uVar2 = 0 & 0x1ff;
  iVar3 = uVar2 * 8;
  /* null access: sVar1 = NULL[1]; */
  g_sys.spriteram[0x4000 + (uVar2 * 8)] = 0 + 0x2d;
  g_sys.spriteram[0x4002 + (uVar2 * 8)] = sVar1 + 0x2a;
  /* null access: g_sys.spriteram[0x4004 + (uVar2 * 8)] = NULL[2]; */
  /* null access: g_sys.spriteram[0x4006 + (uVar2 * 8)] = NULL[3]; */
  g_sys.spriteram[0x4008 + (uVar2 * 8)] = 0xff;
  /* null access: g_sys.spriteram[0x400A + (uVar2 * 8)] = (NULL[5] & 0xfU) + (NULL[4] & 0xfU) * 0x10; */
  /* null access: g_sys.spriteram[0x400C + (uVar2 * 8)] = NULL[6]; */
  /* null access: g_sys.spriteram[0x400E + (uVar2 * 8)] = NULL[7]; */
  *(undefined4 *)(&g_sys.spriteram[0x20000] + iVar3) = *(undefined4 *)(0);
  /* null access: (*(int16_t*)(&g_sys.spriteram[0x20004] + iVar3)) = NULL[10]; */
  /* null access: (*(int16_t*)(&g_sys.spriteram[0x20006] + iVar3)) = NULL[0xb]; */
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- sprite_init ---- */

void sprite_init(void)

{
  sprite_ram_header_init();
  return;
}

/* ---- sprite_list_init ---- */

void sprite_list_init(void)

{
  undefined2 in_D0w;
  
  g_sys.spriteram[0x0002] = 0;
  g_sys.spriteram[0x0004] = in_D0w;
  return;
}

/* ---- sprite_mode_set_6 ---- */

void sprite_mode_set_6(void)

{
  g_sys.spriteram[0] = 6;
  return;
}

/* ---- sprite_mode_set_7 ---- */

void sprite_mode_set_7(void)

{
  g_sys.spriteram[0] = 7;
  return;
}

/* ---- sprite_state_init ---- */

void sprite_state_init(void)

{
  _g_sprite_dirty = 0;
  return;
}

/* ---- text_draw_decimal ---- */

void text_draw_decimal(int param_1,int param_2,uint32_t param_3,uint32_t param_4,undefined4 param_5)

{
  uint32_t uVar1;
  uint16_t *puVar2;
  
  if ((int)param_4 < 0) {
    param_4 = -param_4;
  }
  puVar2 = (uint16_t *)(&g_sys.cgram[0x1DFFE] + (param_3 + param_1) * 2 + param_2 * 0x80);
  for (uVar1 = 0; uVar1 < param_3; uVar1 = uVar1 + 1) {
    if ((param_4 == 0) && (uVar1 != 0)) {
      *puVar2 = 0x20;
    }
    else {
      *puVar2 = param_5 << 0xc | (uint16_t)(param_4 % 10);
      param_4 = param_4 / 10;
    }
    puVar2 = puVar2 + -1;
  }
  return;
}

/* ---- text_layer_update ---- */

void text_layer_update(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x021D74 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. NOT the reason the title screen is missing "INSERT 4 COIN(S)". This is a
   * bare RTS in the ROM; the text tilemap is filled by text_output_flush and
   * the draw helpers above, and textram measures ~4098 non-zero words during
   * attract. The on-screen text defect is in the text_hw feed, not here.
   */
  return;
}

/* ---- text_output_flush ---- */

void text_output_flush(void)

{
  int iVar1;
  int iVar2;
  
  iVar2 = W[0x0CA0];
  iVar1 = W[0x0CA0] * 0x2000;
  dsp_w32(0x10004 + 4 * (W[0x0CA0] * 0x2000), (int32_t)((int)(short)vrd16s(0x20B004 + (W[0x0CE8] >> 1 & 0x7ffe) * 2)));
  dsp_w32(0x10008 + 4 * (iVar1), (int32_t)((int)(short)vrd16s(0x20B006 + (W[0x0CE8] >> 1 & 0x7ffe) * 2)));
  dsp_w32(0x1000C + 4 * (iVar2 * 0x2000), (int32_t)((int)(short)vrd16s(0x20B004 + (W[0x0CEC] >> 1 & 0x7ffe) * 2)));
  dsp_w32(0x10010 + 4 * (iVar2 * 0x2000), (int32_t)((int)(short)vrd16s(0x20B006 + (W[0x0CEC] >> 1 & 0x7ffe) * 2)));
  dsp_w32(0x10014 + 4 * (iVar2 * 0x2000), (int32_t)((int)(short)vrd16s(0x20B004 + (W[0x0CF0] >> 1 & 0x7ffe) * 2)));
  dsp_w32(0x10018 + 4 * (iVar2 * 0x2000), (int32_t)((int)(short)vrd16s(0x20B006 + (W[0x0CF0] >> 1 & 0x7ffe) * 2)));
  dsp_w32(0x1001C + 4 * (iVar2 * 0x2000), (int32_t)(2));
  return;
}

/* ---- text_print_at_position ---- */

uint64_t text_print_at_position(void)

{
  uint16_t uVar1;
  /* void */;
  /* void */;
  uint16_t *puVar2;
  short *psVar3;
  
  /* null access: uVar1 = NULL[2]; */
  puVar2 = NULL + 3;
  /* null access: psVar3 = (short *)((intptr_t)&g_sys.textram[0] + (uint32_t)0 * 2 + (uint32_t)NULL[1] * 0x80); */
  while( true ) { if (++_safety_ctr > 10000) break;
    if (*(uint8_t *)puVar2 == 0) break;
    *psVar3 = uVar1 + *(uint8_t *)puVar2;
    puVar2 = (uint16_t *)((intptr_t)puVar2 + 1);
    psVar3 = psVar3 + 1;
  }
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- text_ram_clear_all ---- */

undefined4 text_ram_clear_all(void)

{
  /* void */;
  short sVar1;
  undefined4 *puVar2;
  
  sVar1 = 0x7ff;
  puVar2 = &g_sys.textram[0];
  _safety_ctr = 0;
  do {
    /* The immediate 0x00200020 -- two BE tile words of 0x0020 (space) --
     * not a host pointer. Written big-endian so the tilemap reads as
     * spaces instead of whatever the truncated address happened to be. */
    tram_w16((uint8_t *)puVar2,     0x0020);
    tram_w16((uint8_t *)puVar2 + 2, 0x0020);
    sVar1 = sVar1 + -1;
    puVar2 = puVar2 + 1;
  } while (sVar1 != -1);
  return 0;
}

/* ---- tilemap_attr_init ---- */

void tilemap_attr_init(void)

{
  int iVar1;
  
  mem_write32(0x8A0000, 0x35c);
  mem_write32(0x8A0002, 0);
  mem_write32(0x8A0004, 0x6e);
  mem_write32(0x8A0006, 0);
  mem_write32(0x8A0008, 0x1ff);
  iVar1 = 0x3b;
  do {
    iVar1 = iVar1 + -1;
  } while (-1 < iVar1);
  return;
}

/* ---- tilemap_fill_sequential ---- */

void tilemap_fill_sequential(void)

{
  int iVar1;
  int iVar2;
  short sVar3;
  short sVar4;
  
  sVar3 = 0;
  iVar1 = 0;
  do {
    iVar2 = 0;
    sVar4 = sVar3;
    do {
      sVar3 = sVar4 + 1;
      g_sys.textram[0x028A + (iVar1 * 0x40 + iVar2)] = sVar4;
      iVar2 = iVar2 + 1;
      sVar4 = sVar3;
    } while (iVar2 < 0x20);
    iVar1 = iVar1 + 1;
  } while (iVar1 < 0x10);
  return;
}

/* ---- tilemap_init ---- */

void tilemap_init(void)

{
  tilemap_attr_init();
  tilemap_setup();
  sync_post();
  return;
}

/* ---- tilemap_post_update ---- */

void tilemap_post_update(void)

{
  FUN_000226b0();
  FUN_000226b2();
  FUN_000226b4();
  FUN_000226b6();
  FUN_000226b8();
  FUN_000226ba();
  return;
}

/* ---- tilemap_setup ---- */

void tilemap_setup(void)

{
  short sVar1;
  int iVar2;
  uint32_t uVar3;
  uint32_t uVar4;
  short sVar5;
  undefined2 *puVar6;
  short *psVar7;
  short *psVar8;
  undefined2 *puVar9;
  
  puVar6 = &R[0x230504];
  uVar4 = 0;
  do {
    uVar3 = 0;
    puVar9 = puVar6;
    do {
      puVar6 = puVar9 + 1;
      g_sys.cgram[0 + (uVar4 * 0x40 + uVar3)] = *puVar9;
      uVar3 = uVar3 + 1;
      puVar9 = puVar6;
    } while (uVar3 < 0x40);
    g_sys.syscon[0x14] = 0;
    uVar4 = uVar4 + 1;
  } while (uVar4 < 0x3c0);
  for (uVar4 = 0; uVar4 < 0x40; uVar4 = uVar4 + 1) {
    g_sys.cgram[0x8000 + (uVar4)] = 0;
  }
  sVar5 = 0;
  for (uVar4 = 0x101; uVar4 < 0x110; uVar4 = uVar4 + 1) {
    psVar7 = &R[0x37448];
    uVar3 = 0;
    do {
      iVar2 = uVar3 * 2 + uVar4 * 0x80;
      psVar8 = psVar7 + 1;
      sVar1 = *psVar7;
      g_sys.cgram[0 + (uVar4 * 0x40 + uVar3)] = sVar5 + sVar1;
      (*(int16_t*)(&g_sys.cgram[0x0004] + iVar2)) = sVar5 + sVar1;
      psVar7 = psVar7 + 2;
      sVar1 = *psVar8;
      g_sys.cgram[0x0002 + (uVar4 * 0x40 + uVar3)] = sVar5 + sVar1;
      (*(int16_t*)(&g_sys.cgram[0x0006] + iVar2)) = sVar5 + sVar1;
      uVar3 = uVar3 + 4;
    } while (uVar3 < 0x40);
    g_sys.syscon[0x14] = 0;
    sVar5 = sVar5 + 0x1111;
  }
  for (uVar4 = 0; uVar4 < 0x40; uVar4 = uVar4 + 1) {
    g_sys.cgram[0x8800 + (uVar4)] = 0xffff;
  }
  puVar6 = &R[0x238504];
  uVar4 = 0;
  do {
    uVar3 = 0;
    puVar9 = puVar6;
    do {
      puVar6 = puVar9 + 1;
      g_sys.cgram[0x9000 + (uVar4 * 0x40 + uVar3)] = *puVar9;
      uVar3 = uVar3 + 1;
      puVar9 = puVar6;
    } while (uVar3 < 0x40);
    uVar4 = uVar4 + 1;
  } while (uVar4 < 0x100);
  return;
}

/* ---- cgram_burn_test ---- */

void cgram_burn_test(void)

{
  STUB_HIT(0x048464, "test mode only");
  /* Ghidra decompilation broken - stubbed */
  return;
}


/* ---- cgram_fill_ones ---- */

uint64_t cgram_fill_ones(void)

{
  /* void */;
  /* void */;
  short sVar1;
  short sVar2;
  
  vics_bank_select();
  sVar2 = 2;
  do {
    sVar1 = (vrd16s(0x486FE + (sVar2 * 4)));
    do {
      mem_write32(0x840000, 0xffffffff);
      sVar1 = sVar1 + -1;
    } while (sVar1 != -1);
    sVar2 = sVar2 + -1;
  } while (sVar2 != -1);
  vics_bank_deselect();
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- cgram_pattern_verify ---- */

void cgram_pattern_verify(void)

{
  STUB_HIT(0x04870A, "test mode only");
  /* Ghidra decompilation broken - stubbed */
  return;
}


/* ---- cgram_read_all_planes ---- */

void cgram_read_all_planes(void)

{
  STUB_HIT(0x048680, "test mode only");
  /* Ghidra decompilation broken - stubbed */
  return;
}


/* ---- cgram_scan_test ---- */

uint64_t cgram_scan_test(void)

{
  uint32_t uVar1;
  /* void */;
  short sVar2;
  /* void */;
  short sVar3;
  short sVar4;
  short sVar5;
  /* void */;
  uint32_t uVar6;
  uint32_t uVar7;
  uint32_t *puVar8;
  uint32_t *puVar9;
  
  uVar1 = mem_read32(0x850000);
  puVar8 = (uint32_t *)(0x850000);
  sVar4 = 1;
  sVar2 = (short)0;
  if (sVar2 != 0) {
    sVar4 = 3;
  }
  uVar6 = mem_read32(0x850000) & 0xffff;
  do {
    *puVar8 = uVar6;
    sVar5 = (vrd16s(0x486FE + (sVar2 * 2)));
    // 0 = CONCAT22((short)((uint32_t)0 >> 0x10),vrd16(0x48700 + (sVar2 * 2)));
    do {
      uVar7 = 0xffffffff;
      sVar3 = 0x3fff;
      puVar9 = (uint32_t *)(0x840000);
      do {
        puVar8 = puVar9 + 1;
        uVar7 = *puVar9 & uVar7;
        sVar3 = sVar3 + -1;
        puVar9 = puVar8;
      } while (sVar3 != -1);
      mem_write32(0x840000, 0x70707070);
      if (uVar7 == 0xffffffff) goto LAB_00048666;
      *puVar8 = 0 + *puVar8;
      sVar5 = sVar5 + -1;
    } while (sVar5 != -1);
    uVar6 = uVar6 + 1;
    sVar4 = sVar4 + -1;
  } while (sVar4 != -1);
  mem_write32(0x840000, 0xd0d0d0d0);
LAB_00048666:
  *puVar8 = uVar1;
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- sprite_ram_header_init @ 0x023022 ---- */



void sprite_ram_header_init(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  undefined4 *puVar4;
  undefined1 *puVar5;
  undefined1 *puVar6;
  
  g_sys.spriteram[0] = 6;
  g_sys.spriteram[0x0002] = 0;
  g_sys.spriteram[0x0004] = 1;
  g_sys.spriteram[0x0006] = 0x53;
  g_sys.spriteram[0x0008] = 0x300;
  g_sys.spriteram[0x000A] = 0x200;
  g_sys.spriteram[0x000C] = 0x300;
  g_sys.spriteram[0x000E] = 0;
  g_sys.spriteram[0x0010] = 0x20;
  g_sys.spriteram[0x0012] = 0x20;
  g_sys.spriteram[0x0014] = 0x280;
  g_sys.spriteram[0x0016] = 0x4ff;
  g_sys.spriteram[0x0018] = 0x32a;
  g_sys.spriteram[0x001A] = 0x509;
  iVar2 = 0;
  do {
    (&g_sys.spriteram[0x0200])[iVar2 * 4] = 0x280;
    (&g_sys.spriteram[0x0202])[iVar2 * 4] = 0x4ff;
    (&g_sys.spriteram[0x0204])[iVar2 * 4] = 0x32a;
    (&g_sys.spriteram[0x0206])[iVar2 * 4] = 0x509;
    iVar2 = iVar2 + 1;
  } while (iVar2 < 8);
  puVar4 = &g_sys.rom[0x37488];
  iVar2 = 0;
  do {
    (&g_sys.spriteram[0x0400])[iVar2] = *puVar4;
    (&g_sys.spriteram[0x0600])[iVar2] = *puVar4;
    puVar4 = puVar4 + 1;
    iVar2 = iVar2 + 1;
  } while (iVar2 < 0x20);
  iVar2 = 0;
  do {
    (&g_sys.spriteram[0x0800])[iVar2] = (short)iVar2;
    iVar2 = iVar2 + 1;
  } while (iVar2 < 0x400);
  g_sys.spriteram[0x20000] = 0;
  g_sys.spriteram[0x20004] = 0x70;
  g_sys.spriteram[0x20006] = 0;
  g_sys.spriteram[0x4000] = 0x280;
  g_sys.spriteram[0x4002] = 0x52a;
  g_sys.spriteram[0x4004] = 1;
  g_sys.spriteram[0x4006] = 1;
  g_sys.spriteram[0x4008] = 0xff;
  g_sys.spriteram[0x400A] = 0x11;
  g_sys.spriteram[0x400C] = 0;
  g_sys.spriteram[0x400E] = 0xfe00;
  W[0x4AEC] = 0;
  iVar2 = 0;
  do {
    W[0x4AF0 + (iVar2 * 0xc00) * 4] = 0;
    W[0x4AF2 + (iVar2 * 0xc00) * 4] = 0;
    W[0x4AF4 + (iVar2 * 0xc00) * 4] = 0;
    W[0x4AF8 + (iVar2 * 0x600) * 4] = 0;
    W[0x4AFC + (iVar2 * 0xc00) * 4] = 1;
    W[0x4AFE + (iVar2 * 0xc00) * 4] = 1;
    W[0x4B00 + (iVar2 * 0xc00) * 4] = 0;
    W[0x4B04 + (iVar2 * 0xc00) * 4] = 0;
    W[0x4B06 + (iVar2 * 0xc00) * 4] = 0;
    iVar2 = iVar2 + 1;
  } while (iVar2 < 4);
  W_SET_HI16(0xAAF0, (uint16_t)W[0x0C98] & 3);   /* byte 0xAAF0: buf sel; LO half is fog R */
  puVar5 = &g_sys.rom[0xB5EC0];
  for (iVar2 = 0; iVar2 < g_sys.rom[0xB85B4]; iVar2 = iVar2 + 1) {
    iVar3 = 0;
    do {
      iVar1 = iVar3 + iVar2 * 0x100;
      (&g_sys.palette_ram[0x7400])[iVar1] = *puVar5;
      puVar6 = puVar5 + 2;
      (&g_sys.palette_ram[0xF400])[iVar1] = puVar5[1];
      puVar5 = puVar5 + 3;
      (&g_sys.palette_ram[0x17400])[iVar1] = *puVar6;
      iVar3 = iVar3 + 1;
    } while (iVar3 < 0x100);
  }
  return;
}





/* ---- tilemap_wind_flash_update @ 0x022604 ---- */

uint32_t tilemap_wind_flash_update(int param_1)

{
  uint16_t uVar1;
  uint32_t uVar2;
  
  if (vrd16s(0x373D8 + ((W[0x0C98] & 7) + param_1 * 8) * 2) == 0) {
    uVar1 = comms_r16(g_sys.commsram, 0x7D2E);
    comms_w16(g_sys.commsram, 0x7D2E, uVar1 & 0xfffd);
  }
  else {
    uVar1 = comms_r16(g_sys.commsram, 0x7D2E);
    comms_w16(g_sys.commsram, 0x7D2E, uVar1 | 2);
  }
  uVar2 = CONCAT31((int32_t)((uint32_t)param_1 >> 8),W[0x2C0C]) & 0xffffff10;
  if ((W[0x2C0C] & 0x10) != 0) {
    uVar2 = debug_draw_value(0xf,1,1,&g_sys.rom[0x3ABB2],param_1,&g_sys.rom[0x3ABB7],2);
  }
  return uVar2;
}





