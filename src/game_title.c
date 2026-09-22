/*
 * Title Screen & Attract Mode
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
uint16_t state_attract_run();
static void attract_tilemap_cmd_exec(uint32_t cmd_addr);
static void attract_tilemap_cmd_dispatch(uint32_t dest_byte_addr, uint32_t src_addr);

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

/* ---- attract_ground_plane_draw ---- */

void attract_ground_plane_draw(void)

{
  *(int32_t*)W[0x0CA4] = 0x8000;
  ((int32_t*)W[0x0CA4])[1] = 0;
  ((int32_t*)W[0x0CA4])[2] = 0x27;
  ((int32_t*)W[0x0CA4])[3] = 0;
  ((int32_t*)W[0x0CA4])[4] = 0xfffea000;
  ((int32_t*)W[0x0CA4])[5] = 0;
  ((int32_t*)W[0x0CA4])[6] = 0x28;
  ((int32_t*)W[0x0CA4])[7] = 0;
  ((int32_t*)W[0x0CA4])[8] = 0xfffea000;
  ((int32_t*)W[0x0CA4])[9] = 0;
  ((int32_t*)W[0x0CA4])[10] = R[0x39DE8] + (W[0x0C98] >> 1 & 0xf) * 4 + 0x146;
  ((int32_t*)W[0x0CA4])[0xb] = -W[0x0CDC];
  ((int32_t*)W[0x0CA4])[0xc] = 0xa000 - W[0x0CE0];
  ((int32_t*)W[0x0CA4])[0xd] = -W[0x0CE4];
  W[0x0CA4] = W[0x0CA4] + 0xe * 4;
  return;
}


/* ---- state_attract_init ---- */

void state_attract_init(void)

{
  W[0x0CAC] = 0;
  W[0x2C12] = 1;
  tilemap_scroll_set(0);
  sound_stop_all();
  tilemap_enable_set();
  ranking_sound_init();
  sync_post();
  W_SET_HI16(0xEB20, 0);   /* clr.w $e0eb20: 16-bit, the HIGH half */
  /* Real 68020 (dasm @0xC20E): movea.w ($3510C,D0.l*2),A0 / move.l A0,$E00CC4
   * — the sign-extended table word IS the case index; the dispatch at
   * 0xC232 does lsl.l #2 into the pointer table at 0x351E4. The earlier
   * ">> 1" here halved every case (t[2]=4 gameplay-replay became 2
   * flyover, t[15]=2 flyover became 1 logo) and scrambled the whole
   * attract program. Verified against MAME L3 trace: seq 15->case 3,
   * seq 1->case 1, seq 2->case 5. */
  {
    uint32_t tbl_off = 0x3510C + (uint32_t)W[0x0CC8] * 2;
    W[0x0CC4] = (int16_t)((g_sys.rom[tbl_off] << 8) | g_sys.rom[tbl_off + 1]);
  }
  W[0x0CBC] = 1;
  state_attract_run();
  return;
}

/* ---- state_attract_run ---- */

/* Attract mode sub-state handlers */
void FUN_0000c970(void);  /* Namco logo sprite animation (case 1) */
void FUN_0000ca80(void);  /* Cinematic flyover init (case 2) */
void FUN_0000cbf2(void);  /* Cinematic flyover run (case 3) */
void FUN_0000ce1e(void);  /* gameplay-replay-style flythrough init (case 20), game_misc.c */
void FUN_0000ced2(void);  /* ...its per-frame tick (case 21), game_misc.c */
void attract_flyover_init(void);  /* case 16 -- see its definition below */
void attract_cinematic_init(void);  /* case 18 -- see its definition below */
void highscore_display_init(void);
void highscore_display_run(void);
void attract_logo_init(void);
void attract_logo_run(void);
void attract_terrain_flyover_init(void);
int attract_terrain_flyover_run(void);
void attract_overview_init(void);
void attract_overview_run(void);
void attract_gameplay_tick(void);
void attract_flyover_tick(void);
void attract_cinematic_tick(void);
void replay_cam_copy9(uint32_t dst, uint32_t src);   /* game_replay.c */
void attract_highscore_overlay_draw(int param_1);

uint16_t state_attract_run(void)

{
  uint16_t uVar1;

  /* Attract sub-state dispatch — ROM pointer table at 0x351E4, read via
   * `move.l $e00cc4,d0 ; lsl.l #2,d0 ; jsr ([$351e4,d0.l])` (ROM 0xC232-
   * 0xC23A) -- the value in W[0x0CC4] IS the case index directly, no
   * scaling (state_attract_init's own comment already corrects an
   * earlier, wrong ">> 1" belief; this block used to separately claim a
   * "divide by 2" that was never true either -- both were stale).
   *
   * The sequence table at 0x3510C (read by state_attract_init and
   * attract_advance_sub_state) holds the actual case values directly,
   * confirmed by reading the ROM: [20,0,4,2,0,16,6,12,2,0,18,10,2,0,14]
   * for sequence slots 0-14 -- i.e. the sequence can and does land on
   * values >15, which is what case 16-23 below are for.
   *
   * ROM table function pointers, read directly from 0x351E4 (case 5's
   * address here was previously mistyped as 0x031838 -- two bytes off
   * case 6's real address, 0x03183A):
   *   case 0  → 0x00C918  case 1  → 0x00C970  (Namco logo run)
   *   case 2  → 0x00CA80  case 3  → 0x00CBF2  (cinematic flyover)
   *   case 4  → 0x00D05C  case 5  → 0x00D134  (gameplay replay)
   *   case 6  → 0x03183A  case 7  → 0x0318D8  (highscore display)
   *   case 8  → 0x032466  case 9  → 0x0324A2  (logo cycle 2)
   *   case 10 → 0x03385E  case 11 → 0x0338FA  (stage overview)
   *   case 12 → 0x031DFC  case 13 → 0x031E4C  (terrain flyover)
   *   case 14 → 0x03157A  case 15 → 0x03164C  (cinematic dispatch)
   *   case 16 → 0x00D508  case 17 → 0x00D594  (attract_flyover_tick)
   *   case 18 → 0x00D8FE  case 19 → 0x00D97A  (2nd flyover cluster)
   *   case 20 → 0x00CE1E  case 21 → 0x00CED2  (replay-style flythrough)
   *   case 22 → 0x018546  case 23 → 0x018548  (state_stage_start_run tail)
   * See the case-16-23 block below for which of these are wired.
   */
  /* PROPCYCL_ATTRACT_LOG=1: one line per attract PHASE CHANGE. The attract
   * program is a playlist of sub-states in W[0x0CC4]; knowing which case runs
   * for which frames is the difference between measuring the phases and
   * guessing at them (register row 24). */
  { extern int g_attract_log; static int last = -1;
    if (g_attract_log && (short)W[0x0CC4] != last) {
      last = (short)W[0x0CC4];
      printf("[ATT] f%-5u phase W[0x0CC4]=%-3d  seq W[0x15ED0]=%-4d ctr W[0x15ECC]=%-6d W[0x15EB8]=%d\n",
             g_sys.frame_count, last, (int)(short)W[0x15ED0],
             (int)W[0x15ECC], (int)W[0x15EB8]);
      printf("      replay ptr W[0x16D7C]=%#lx  W[0x0C8C]=%ld  W[0x0D50]=%ld W[0x0D54]=%ld W[0x2C04]=%ld\n",
             (unsigned long)W[0x16D7C], (long)W[0x0C8C],
             (long)W[0x0D50], (long)W[0x0D54], (long)W[0x2C04]);
    } }
  switch ((short)W[0x0CC4]) {
    case 0:  /* 0x00C918: title-logo init (from MAME dasm) */
             sync_post();
             sound_reset_all();
             camera_state_reset();
             W[0xEB18] = 3;   /* fog mode */
             W[0xEB16] = 0;
             W_SET_HI16(0xEB20, 0);   /* clr.w $e0eb20: 16-bit, the HIGH half */
             /* sound cue alternates 0x16/0x17 */
             W[0x2C14] = ((W[0x2C14] + 1) % 2) + 0x16;
             W[0x0CD4] = 0;   /* logo frame counter */
             W[0x0CC4] = 1;   /* advance to logo run */
             FUN_0000c970();  /* run first frame immediately */
             break;
    case 1:  FUN_0000c970(); break;   /* title-logo run (billboard + sprites) */
    case 2:  FUN_0000ca80(); break;   /* Cinematic flyover init */
    case 3:  FUN_0000cbf2(); break;   /* Cinematic flyover run */
    case 4:  attract_gameplay_tick(); break;  /* 0x00D05C: gameplay replay init/tick */
    case 5:  attract_gameplay_tick(); break;  /* Gameplay replay tick */
    case 6:  highscore_display_init(); break; /* High score display init */
    case 7:  highscore_display_run(); break;  /* High score display run */
    case 8:  attract_logo_init(); break;      /* Logo init (second cycle) */
    case 9:  attract_logo_run(); break;       /* Logo run (second cycle) */
    case 10: attract_overview_init(); break;  /* Stage overview init */
    case 11: attract_overview_run(); break;   /* Stage overview run */
    case 12: attract_terrain_flyover_init(); break;  /* Terrain flyover init */
    case 13: attract_terrain_flyover_run(); break;   /* Terrain flyover run */
    /* NOTE (found wiring case 19): `attract_cinematic_tick()` below is
     * actually the port of ROM 0xD97A -- its own header comment names
     * "The M68K switch at 0x00DA0A", which is inside 0xD97A's body, not
     * anywhere near 0x03157A/0x03164C. Cases 14/15 calling it were
     * already wired this way before this session; whatever really lives
     * at 0x03157A/0x03164C has not been separately ported. Not fixed
     * here (needs its own disassembly pass) -- flagging so it is not
     * mistaken for case 19's own wiring below, which IS correct. */
    case 14: attract_cinematic_tick(); break; /* 0x03157A: cinematic dispatch */
    case 15: attract_cinematic_tick(); break; /* 0x03164C: stage cutscene */
    /* Cases 16-23: the ROM sequence table (0x3510C) and its dispatch
     * pointer table (0x351E4) both actually hold 24+ entries, not 16 --
     * measured directly from ROM, not guessed (this switch's own case
     * list above was built from disassembly that stopped at 15). Ghidra's
     * "Could not recover jumptable" on `attract_flyover_tick`'s internal
     * table (register row 84 / this file's own history) hid the real
     * bug: with cases 16-23 missing, the sequence table's own early
     * entries (0/5/10 -> values 20/16/18) landed on `default: break;`
     * and froze the whole attract loop the first time the sequence
     * cycled through one of them -- reproduced live: W[0x0CC4] gets
     * stuck at 16 by fc=3903 and never changes again.
     * case 20/21 (0xCE1E/0xCED2) are fully decompiled already (game_misc.c)
     * and were simply never wired in -- done below.
     * case 16/18/22/23 are shared-tail reentry points into OTHER already
     * -addressed functions (attract_advance_sequence, this file's own
     * attract_flyover_text +0xae, and state_stage_start_run), not new
     * top-level handlers; case 19 is a large, distinct camera/flythrough
     * subsystem (structure copy + trig-based placement, comparable in
     * scope to the rider-rig work) not yet ported. Left OPEN rather than
     * wired to an incomplete handler, which would just move the freeze. */
    case 16: attract_flyover_init(); break;  /* newly ported, see its definition below */
    case 17: attract_flyover_tick(); break;  /* was missing entirely -- attract_flyover_tick
                                               * exists and is called once from case 16's own
                                               * init, but nothing re-invoked it per frame */
    case 18: attract_cinematic_init(); break; /* newly ported, see its definition below */
    case 19: attract_cinematic_tick(); break; /* real wiring for this function -- see the
                                                * note on case 14/15 above */
    case 20: FUN_0000ce1e(); break;  /* gameplay-replay-style flythrough init */
    case 21: FUN_0000ced2(); break;  /* ...its per-frame tick */
    default: break;
  }
  attract_update_common();
  uVar1 = W[0x2BA6] & 1;
  if ((W[0x2BA6] & 1) != 0) {
    uVar1 = attract_advance_sub_state();
  }
  return uVar1;
}

int g_attract_log = 0;

/* ---- state_title_init ---- */

void state_title_init(void)

{
  sync_reset();
  set_background_color(0,0,0);
  W[0x3FB4] = W[0x3FB4] & 0xfffe;
  W[0x0CBC] = 7;
  if (W[0x3FB0] + 600U < W[0x4010]) {
    W[0x3FB4] = 0;
  }
  if ((W[0x2B80] & 4) == 0) {
    if (W[0x3FB4] == 0x20) {
      W[0x3FB4] = 0;
    }
  }
  else {
    W[0x3FB4] = 0x20;
  }
  W[0x16918] = 0xffff;
  W[0xEB16] = 0;
  W[0x2C24] = 0;
  W[0x0CAC] = 0;
  sound_stop_all();
  tilemap_enable_set();
  tilemap_scroll_set(0);
  W_SET_HI16(0xEB20, 0);   /* clr.w $e0eb20: 16-bit, the HIGH half */
  cz_ram_init();
  return;
}

/* ---- state_title_run ---- */

/* Forward declarations for title sub-state handlers */
void title_attract_init(void);
uint16_t title_option_a_init(void);
void title_option_a_run(void);
void title_option_b_init(void);
void title_option_b_run(void);

#ifndef g_credit_countdown
#define g_credit_countdown W[0x2C14]
#endif
int title_rom_checksum_test(void);

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

void title_service_menu_run(void);
void title_rom_test_run(void);
void title_data_menu_run(void);
void title_clear_stats_confirm(void);
void title_hardware_info_init(void);
void title_hardware_info_run(void);
void title_factory_reset_confirm(void);
int title_rom_checksum_test(void);

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

void title_sprite_animation_update(void);
void coin_status_hud_draw(void);
void dipswitch_display_update(void);

/* Recreated arcade mode-select / course-select menu — see src/arcade_menu.c.
 * The original ROM implemented this in the first three slots of the
 * state_stage_start_run jump table at 0x18546; those slots were stripped to
 * `rts; rts` no-ops in this debug ROM, so we drive it here. */
extern void arcade_menu_step(void);
extern void arcade_menu_trigger(void);
extern int  arcade_menu_active(void);


/* Handlers that live in game_debug.c / later in this file. */
void service_input_test_run(void);
void service_output_test_run(void);
void service_game_test_run(void);
void service_sound_test_init(void);
uint32_t service_sound_test_run(void);
void service_statistics_init(void);
void service_statistics_run(void);
uint64_t attract_gfx_init(void);
void sync_post(void);
void menu_draw_labels(uint32_t table, int count);
undefined4 menu_handle_page_select(undefined4 param_1);
undefined2 check_confirm_button(void);
void menu_items_clear_highlights(int *param_1);
void eeprom_init_with_calibration(void);

/* ===================================================================
 * TITLE SUB-STATE INIT HANDLERS -- decompiled from the ROM.
 *
 * state_title_run @0x01A2AE dispatches through a 32-bit POINTER TABLE at
 * ROM 0x36548 indexed by W[0x3FB4]:
 *     movea.l ($36548,D0.w*4),A0 ; jsr (A0)
 * Our tree had a hand-written switch "replacing" that table, and from entry
 * 0x06 up almost every arm pointed at the wrong handler (0x13 ran
 * title_rom_test_run where the ROM runs service_sound_test_run, 0x14 ran
 * title_data_menu_run where the ROM runs service_statistics_init, and so on),
 * which is what made the menu tree misbehave.
 *
 * The table's even slots are INIT routines that Ghidra never emitted as
 * functions. They share one shape:
 *     sync_post(); title_substate_dispatch(N); <reset state>; W[0x3FB4] = run;
 * Each is transcribed below from its own disassembly, address in the banner.
 * =================================================================== */

/* ---- 0x01A940 (slot 0x01) -- THE MENU ITSELF ----
 *     move.w #7,-(A7)        ; jsr menu_handle_page_select   (7 items)
 *     move.w #$100,-(A7)     ; pea $36698 ; jsr menu_draw_labels
 *     jsr check_confirm_button
 *     tst.w D0 ; beq out
 *     move.b $e03fb6,D0 ; extb.l ; add.l D0,D0
 *     move.w ($36704,D0.l),$e03fb4      ; selection -> next sub-state
 * The 7 targets at 0x36704 are 02 04 06 10 12 14 16. */
static void title_sub01_run(void)
{
  menu_handle_page_select(7);
  menu_draw_labels(0x36698, 0x100);
  if (check_confirm_button() != 0) {
      int sel = (int8_t)vrd8(0xE03FB6);
      W[0x3FB4] = (int16_t)vrd16(0x36704 + sel * 2);
  }
}

/* ---- 0x01A928 (slot 0x00) ---- */
static void title_sub00_init(void)
{
  sync_post();
  title_substate_dispatch(0);
  W[0x3FB4] = 1;
}

/* ---- 0x01AC5A (slot 0x06) ---- */
static void title_sub06_init(void)
{
  sync_post();
  title_substate_dispatch(0);
  W[0x3FB4] = 7;
}

/* ---- 0x01AD02 (slot 0x08) ---- */
static void title_sub08_init(void)
{
  sync_post();
  title_substate_dispatch(2);
  W[0x3FB4] = 9;
  W[0x3FBA] = 0xfe;                 /* move.b #$fe,$e03fba */
}

/* ---- 0x01AECE (slot 0x0A) ---- */
static void title_sub0a_init(void)
{
  sync_post();
  title_substate_dispatch(2);
  W[0x1692A] = 0;
  W[0x16928] = 0;
  W[0x3FB4] = 0xb;
}

/* ---- 0x01B150 (slot 0x0C) ---- */
static void title_sub0c_init(void)
{
  sync_post();
  title_substate_dispatch(0);
  W[0x16928] = 0;
  W[0x3FB4] = 0xd;
}

/* ---- 0x01B23E (slot 0x0E) ---- */
static void title_sub0e_init(void)
{
  sync_post();
  title_substate_dispatch(0);
  W[0x1692E] = 0;
  W[0x3FB4] = 0xf;
}

/* ---- 0x01B328 (slot 0x10) ---- */
static void title_sub10_init(void)
{
  (void)attract_gfx_init();         /* jsr $1c0b0 */
  mem_write8(0x82401b, 0x7f);       /* move.b #$7f,$82401b */
  sync_post();
  W[0x16930] = 0;
  W[0x3FB4] = 0x11;
}

/* ---- 0x01BB2A (slot 0x16) ---- */
static void title_sub16_init(void)
{
  sync_post();
  W[0x3FB4] = 0x17;
}

/* ---- 0x01BC66 (slot 0x18) ---- */
static void title_sub18_init(void)
{
  int i;
  sync_post();
  title_substate_dispatch(5);
  for (i = 0; i < 0xc; i++)         /* move.w #$ffff,($e168fc,D1.w*2) x12 */
      W[0x168FC + i * 2] = 0xffff;
  W[0x0E1C]  = 1;
  W[0x1691A] = 0xc;
  W[0x16914] = 0;                   /* clr.l */
  W[0x3FB4]  = 0x19;
}

/* ---- 0x01BDCE (slot 0x1A) ---- */
static void title_sub1a_init(void)
{
  sync_post();
  title_substate_dispatch(0);
  W[0x3FC3] = 1;
  W[0x3FB4] = 0x1b;
}

/* ---- 0x01BE36 (slot 0x1C) ---- */
static void title_sub1c_init(void)
{
  sync_post();
  title_substate_dispatch(0);
  W[0x3FC4] = 1;
  W[0x3FB4] = 0x1d;
}

/* ---- 0x01C030 (slot 0x20) ---- */
static void title_sub20_init(void)
{
  sync_post();
  eeprom_init_with_calibration();   /* jsr $1a0be */
  menu_items_clear_highlights((int *)(uintptr_t)0x372d0);
  W[0x3FC6] = 0;
  W[0x3FB4] = 0x21;
}

/* Slots whose RUN handler exists only in the dead src/game_all.c
 * (service_screen_flip_run @0x01B16E, service_stereo_toggle_run @0x01B25C)
 * and slot 0x21 @0x01C058, not yet transcribed. Reported once each rather
 * than silently doing nothing, so a wrong menu page is visible instead of
 * looking like a hang. */
static void title_sub_unported(const char *what)
{
  static const char *said[8]; static int n;
  int i; for (i = 0; i < n; i++) if (said[i] == what) return;
  if (n < 8) said[n++] = what;
  printf("[TITLE] sub-state handler not yet transcribed: %s\n", what);
}
static void title_sub0d_run(void) { title_sub_unported("service_screen_flip_run @0x01B16E"); }
static void title_sub0f_run(void) { title_sub_unported("service_stereo_toggle_run @0x01B25C"); }
static void title_sub21_run(void) { title_sub_unported("slot 0x21 @0x01C058"); }

/* The ROM's own table at 0x36548, in order. */
static void (* const title_dispatch[0x22])(void) = {
  /* 0x00 */ title_sub00_init,              /* 0x01A928 */
  /* 0x01 */ title_sub01_run,                /* 0x01A940 -- the MENU itself */
  /* 0x02 */ (void (*)(void))title_option_a_init,   /* returns uint16_t */
  /* 0x03 */ title_option_a_run,
  /* 0x04 */ title_option_b_init,
  /* 0x05 */ title_option_b_run,
  /* 0x06 */ title_sub06_init,
  /* 0x07 */ title_service_menu_run,
  /* 0x08 */ title_sub08_init,
  /* 0x09 */ service_input_test_run,
  /* 0x0A */ title_sub0a_init,
  /* 0x0B */ service_output_test_run,
  /* 0x0C */ title_sub0c_init,
  /* 0x0D */ title_sub0d_run,
  /* 0x0E */ title_sub0e_init,
  /* 0x0F */ title_sub0f_run,
  /* 0x10 */ title_sub10_init,
  /* 0x11 */ service_game_test_run,
  /* 0x12 */ service_sound_test_init,
  /* 0x13 */ (void (*)(void))service_sound_test_run,
  /* 0x14 */ service_statistics_init,
  /* 0x15 */ service_statistics_run,
  /* 0x16 */ title_sub16_init,
  /* 0x17 */ title_data_menu_run,
  /* 0x18 */ title_sub18_init,
  /* 0x19 */ title_rom_test_run,
  /* 0x1A */ title_sub1a_init,
  /* 0x1B */ title_clear_stats_confirm,
  /* 0x1C */ title_sub1c_init,
  /* 0x1D */ title_factory_reset_confirm,
  /* 0x1E */ title_hardware_info_init,
  /* 0x1F */ title_hardware_info_run,
  /* 0x20 */ title_sub20_init,
  /* 0x21 */ title_sub21_run,
};

uint16_t state_title_run(void)

{
  uint16_t uVar1;

  /* The `arcade_menu` RECREATION used to intercept here and own the frame,
   * which meant the ROM's own title dispatch below never ran. Removed: this
   * file now runs the machine's table at 0x36548 instead. src/arcade_menu.c
   * is no longer reachable from the title path. */

  /* THE ROM'S OWN DISPATCH: movea.l ($36548,D0.w*4),A0 ; jsr (A0), with
   * D0 = W[0x3FB4]. The hand-written switch this replaces had almost every
   * arm from 0x06 up pointing at the wrong handler. */
  { int idx = (int)(int16_t)W[0x3FB4];
    if (idx >= 0 && idx < (int)(sizeof title_dispatch / sizeof title_dispatch[0])
        && title_dispatch[idx])
        title_dispatch[idx]();
  }
  if (((W[0x0E30] == 0) || (uVar1 = 0, (W[0x2B5E] & 0x100) != 0)) &&
     (uVar1 = W[0x2B80] & 8, uVar1 == 0)) {
    if (W[0x3FB4] == 3) {
      title_option_a();
    }
    if (W[0x3FB4] == 5) {
      title_option_b();
    }
    W[0x3FB4] = W[0x3FB4] & 0xfffe;
    if ((W[0x3FB4] == 0x18) || (W[0x3FB4] == 0x1e)) {
      W[0x3FB4] = 0x16;
    }
    W[0x3FB0] = W[0x4010];
    uVar1 = title_menu_select();
  }
  return uVar1;
}

/* ---- attract_update_common ---- */

void attract_update_common(void)

{
  if (W[0x168C8] <= W[0x4010]) {
    W[0x168C8] = W[0x4010] + 36000;
    eeprom_write_block(&W[0x4010],4);
  }
  return;
}

/* ---- title_menu_select ---- */

void title_menu_select(void)

{
  W[0x3FEA] = 1;
  eeprom_sync_all();
  entry_reset();
  return;
}

/* ---- title_option_a ---- */

void title_option_a(void)

{
  g_time_limit = (undefined2)W[0x1691C];
  W16_SET(0x3FF6, (undefined2)W[0x16920]);
  W16_SET(0x3FF4, W[0x16928]);
  eeprom_write_block(&g_time_limit,8);
  return;
}

/* ---- title_option_b ---- */

void title_option_b(void)

{
  W[0x3FFA] = W[0x1692A];
  W16_SET(0x3FF8, (undefined2)W[0x16920]);
  W[0x3FFC] = W[0x1692C];
  eeprom_write_block(&W[0x3FF8],6);
  return;
}

/* ========== TITLE SCREEN COMPLETE (31 functions) ========== */

/* ---- attract_gfx_init ---- */

uint64_t attract_gfx_init(void)

{
  /* void */;
  /* void */;
  
  attract_enable_display();
  attract_load_palette();
  attract_load_char_rom();
  attract_load_sprite_table();
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- attract_tilemap_cmd_dispatch @ 0x01C966 ---- */
/* Sub-dispatch called once per output row from attract_tilemap_cmd_exec.
 * Reads one 16-bit opcode from the ROM command stream at src_addr, then:
 *   opcode 0 -- RLE fill: repeated (count,value) 16-bit word pairs, each
 *     writing 'value' to (count+1) consecutive dest tile words,
 *     terminated by a negative count word (ROM 0x01C978-0x01C986:
 *     `move.w (a4)+,d6 ; bpl.b .. ; rts` / `move.w (a4)+,d0 ; move.w
 *     d0,(a3)+ ; dbra d6,.. ; bra.w ..`)
 *   opcode 1 -- literal copy: one (count) word then (count+1) raw tile
 *     words copied verbatim from the ROM stream to the dest tilemap row
 *     (ROM 0x01C98A-0x01C992: `move.w (a4)+,d6 ; move.w (a4)+,(a3)+ ;
 *     dbra d6,.. ; rts`)
 * ROM 0x01C966: `move.w (a4)+,d0 ; jmp ([$1c970,pc,d0.w*4])` -- capstone
 * drops the index scale on a memory-indirect jmp EA (this project's known
 * trap, see the CLAUDE.md note "capstone DROPS THE INDEX SCALE"); the
 * table at 0x1c970 holds two 32-bit absolute targets (0x1c978, 0x1c98a).
 * Both opcodes were hand-decoded from the disassembly and cross-checked
 * by mechanically replaying this function's only live caller's actual ROM
 * data end to end -- every one of the four reachable blocks there takes
 * opcode 1, but opcode 0's RLE path is disassembled directly from the
 * ROM, not guessed, so it is kept even though unexercised by this call
 * site (some other, currently-unidentified caller may use it). */
/* BOUNDS. `dest_byte_addr - TEXTRAM_BASE` is UNSIGNED arithmetic and the
 * caller's own offset is a signed 16-bit ROM field, so a negative one puts
 * dest below the base and wraps the index to ~4 G; and `cnt` is a signed
 * 16-bit ROM field too, so a block can ask for up to 64 KB of writes into an
 * 8 KB array. The hand-written implementation this replaced checked both and
 * the port dropped the checks. Not reachable with the one live command
 * stream (replayed byte for byte: it touches textram 128..3663 of 8192 and
 * every block uses opcode 1) -- but this is the only implementation now. */
static void tilemap_put16(uint32_t dest_byte_addr, uint16_t val)
{
  uint32_t off = dest_byte_addr - (uint32_t)TEXTRAM_BASE;
  if (dest_byte_addr < (uint32_t)TEXTRAM_BASE) return;
  if (off + 1 >= (uint32_t)TEXTRAM_SIZE) return;
  tram_w16(&g_sys.textram[off], val);
}

static void attract_tilemap_cmd_dispatch(uint32_t dest_byte_addr, uint32_t src_addr)
{
  int16_t op, cnt;
  uint16_t val;
  int i;

  if (src_addr + 2 > (uint32_t)ROM_SIZE) return;
  op = (int16_t)vrd16(src_addr); src_addr += 2;
  if (op == 0) {
    for (;;) {
      /* A src that runs off the ROM reads back 0, which is not < 0, so
       * without this the loop never terminates and marches dest through
       * memory two bytes at a time. */
      if (src_addr + 4 > (uint32_t)ROM_SIZE) return;
      cnt = (int16_t)vrd16(src_addr); src_addr += 2;
      if (cnt < 0) return;
      val = (uint16_t)vrd16(src_addr); src_addr += 2;
      for (i = 0; i <= cnt; i++) {
        tilemap_put16(dest_byte_addr, val);
        dest_byte_addr += 2;
      }
    }
  } else if (op == 1) {
    cnt = (int16_t)vrd16(src_addr); src_addr += 2;
    for (i = 0; i <= cnt; i++) {
      if (src_addr + 2 > (uint32_t)ROM_SIZE) return;
      val = (uint16_t)vrd16(src_addr); src_addr += 2;
      tilemap_put16(dest_byte_addr, val);
      dest_byte_addr += 2;
    }
  }
  /* any other opcode: the ROM's own jump table has only these two
   * entries -- unreachable on real hardware, so nothing to do here */
}

/* ---- attract_tilemap_init ---- */

uint64_t attract_tilemap_init(void)

{
  /* void */;
  /* void */;
  
  attract_clear_textram();
  attract_tilemap_cmd_exec(0x1C0E4);   /* ROM 0x01C0D4: lea.l $1c0e4(pc),a0 */
  return (((uint64_t)(0) << 32) | (uint32_t)(0));
}

/* ---- coin_status_hud_draw ---- */

void coin_status_hud_draw(void)

{
  int iVar1;
  uint16_t uVar2;
  undefined2 uVar3;
  undefined4 uVar4;
  char *pcVar5;
  
  iVar1 = (short)((uint16_t)(W[0x0C98] >> 6) & 7) * 4;
  g_sys.palette_ram[0x7FE0] = (&R[0x3795A])[iVar1];
  g_sys.palette_ram[0x7FE1] = 0;
  g_sys.palette_ram[0xFFE0] = (&R[0x3795B])[iVar1];
  g_sys.palette_ram[0xFFE1] = 0;
  g_sys.palette_ram[0x17FE0] = (&R[0x3795C])[iVar1];
  g_sys.palette_ram[0x17FE1] = 0;
  if (W[0x2C12] != 0) {
    uVar2 = g_credit_countdown & 1;
    if (W[0x2C0E] < 10) {
      iVar1 = (int)W[0x2C0E];
    }
    else {
      iVar1 = 9;
    }
    if ((W[0x2BA4] & 2) != 0) {
      uVar2 = 0;
    }
    if (W16(0x3FF4) == 0) {
      uVar3 = 0;
      text_draw_signed_decimal(0x20, uVar2 + 0x1c, 4, iVar1, 0);
      text_draw_hex_digits(0x25,uVar2 + 0x1c,1,g_time_limit,uVar3);
      text_print_string(0x19, uVar2 + 0x1c, 0x3acba, 0);
      pcVar5 = &R[0x3ACC4];
      uVar4 = 0x24;
    }
    else {
      pcVar5 = (char*)(g_sys.rom + 0x3acaf) + 1;
      uVar4 = 0x1c;
    }
    text_print_string(uVar4, uVar2 + 0x1c, pcVar5, 0);
    text_print_string(0x19, (uVar2 ^ 1) + 0x1c, 0x3acc6, 0);
  }
  return;
}

/* ---- title_attract_init ---- */

void title_attract_init(void)

{
  undefined2 *puVar1;
  
  g_sys.spriteram[0] = 0;
  g_sys.spriteram[0x0002] = 0;
  g_sys.spriteram[0x0004] = 0;
  g_sys.spriteram[0x0006] = 0;
  g_sys.spriteram[0x0008] = 0x2ff;
  g_sys.spriteram[0x000A] = 0;
  g_sys.spriteram[0x000C] = 0;
  g_sys.spriteram[0x000E] = 0x7ff;
  g_sys.spriteram[0x0010] = 0x20;
  g_sys.spriteram[0x0012] = 0x20;
  g_sys.spriteram[0x0014] = 0;
  g_sys.spriteram[0x0016] = 0x2ff;
  g_sys.spriteram[0x0018] = 0;
  g_sys.spriteram[0x001A] = 0x7ff;
  for (puVar1 = &g_sys.spriteram[0x0200]; puVar1 < (undefined2 *)&g_sys.spriteram[0x0240]; puVar1 = puVar1 + 2) {
    *puVar1 = 0;
    puVar1[1] = 0x7ff;
  }
  W[0x16918] = 0x17;
  boot_hardware_init();
  W[0x16918] = 0xffff;
  W[0x3FB0] = W[0x4010];
  title_menu_select();
  return;
}

/* ---- title_clear_stats_confirm ---- */

void title_clear_stats_confirm(void)

{
  short sVar1;
  undefined2 uVar2;
  undefined2 uVar3;
  
  uVar3 = 0x100;
  uVar2 = 3;
  menu_draw_labels(0x37130, 0);
  menu_handle_page_select(uVar2);
  sVar1 = check_confirm_button();
  if (sVar1 != 0) {
    if (W[0x3FC3] == '\0') {
      game_stats_reset();
      eeprom_sync_all();
      W[0x3FC0] = 0;
    }
    W[0x3FB4] = 0x14;
  }
  return;
}

/* ---- title_data_menu_run ---- */

void title_data_menu_run(void)

{
  int iVar1;
  short sVar2;
  undefined2 uVar3;
  undefined2 uVar4;
  undefined2 uVar5;
  
  uVar5 = 0x100;
  uVar4 = 3;
  menu_draw_labels(0x36ff8, 0);
  uVar3 = 0;
  title_substate_dispatch(uVar4);
  menu_handle_page_select(uVar3);
  sVar2 = check_confirm_button();
  if ((sVar2 != 0) && (iVar1 = (int)W[0x3FC1] - 4, (uint32_t)(int)W[0x3FC1] < 4 || iVar1 == 0)) {
                    
                    
    /* hw/menu dispatch */;
    return;
  }
  return;
}

/* ---- title_factory_reset_confirm ---- */

void title_factory_reset_confirm(void)

{
  undefined1 uVar1;
  short sVar2;
  undefined2 uVar3;
  undefined2 uVar4;
  
  uVar4 = 0x100;
  uVar3 = 3;
  menu_draw_labels(0x37178, 0);
  menu_handle_page_select(uVar3);
  sVar2 = check_confirm_button();
  uVar1 = W[0x3FB6];
  if (sVar2 != 0) {
    if (W[0x3FC4] == '\0') {
      eeprom_factory_reset();
      W[0x3FC1] = 0;
    }
    W[0x3FB4] = 0x16;
  }
  W[0x3FB6] = uVar1;
  return;
}

/* ---- title_hardware_info_init ---- */

void title_hardware_info_init(void)

{
  short sVar1;
  
  sync_post();
  menu_draw_labels(0x371cc, 0);
  sVar1 = 0;
  do {
    text_draw_hex_digits
              (0x11,(int)(*(int16_t*)((int32_t*)&g_sys.rom[0x371D2] + sVar1 * 3)),4,(&R[0x372BC])[sVar1], 0);
    sVar1 = sVar1 + 1;
  } while (sVar1 < 9);
  text_print_string(0x18, 6, 0x36663, 0);
  W[0x16914] = 0xc;
  title_rom_checksum_test();
  text_print_string(0x18, 6, 0x3667a, 0);
  title_substate_dispatch(0);
  W[0x3FB4] = 0x1f;
  return;
}

/* ---- title_hardware_info_run ---- */

void title_hardware_info_run(void)

{
  short sVar1;
  undefined2 uVar2;
  
  sVar1 = 0;
  do {
    if ((&R[0x372BC])[sVar1] == W[0xABD6 + (sVar1)]) {
      text_draw_hex_digits
                (0x18,(int)(*(int16_t*)((int32_t*)&g_sys.rom[0x371D2] + sVar1 * 3)),4,W[0xABD6 + (sVar1)], 0);
      uVar2 = 0xa6ad;
    }
    else {
      text_draw_hex_digits
                (0x18,(int)(*(int16_t*)((int32_t*)&g_sys.rom[0x371D2] + sVar1 * 3)),4,W[0xABD6 + (sVar1)], 0);
      uVar2 = 0xab28;
    }
    text_print_string(0x1e, (int)(*(int16_t*)((int32_t*)&g_sys.rom[0x371D2] + sVar1 * 3)), uVar2, 0);
    sVar1 = sVar1 + 1;
  } while (sVar1 < 9);
  sVar1 = check_confirm_button();
  if (sVar1 != 0) {
    W[0x3FB4] = 0x16;
    W[0x3FB0] = W[0x4010];
    title_menu_select();
  }
  return;
}

/* ---- title_option_a_init ---- */

uint16_t title_option_a_init(void)

{
  sync_post();
  title_substate_dispatch(0);
  menu_items_copy_to_work(0x6714);
  menu_items_clear_highlights((int32_t*)&g_sys.rom[0x36714]);
  if ((W[0x2BA4] & 2) != 0) {
    W[0xAB14] = 4;
    W[0xAB46] = 2;
  }
  W[0x1691C] = (int)g_time_limit;
  W[0x16920] = (int)W16(0x3FF6);
  W[0x16928] = W16(0x3FF4);
  W[0x3FB4] = 3;
  return W[0x2BA4] & 2;
}

/* ---- title_option_a_run ---- */

void title_option_a_run(void)

{
  short sVar1;
  undefined2 uVar2;
  undefined2 uVar3;
  
  uVar3 = 0x100;
  uVar2 = 3;
  menu_draw_labels(0x36774, 0);
  menu_draw_items(0xab04, uVar2);
  sVar1 = 0;
  do {
    text_print_string((&R[0x367EC])[sVar1 * 2],(&R[0x367EE])[sVar1 * 2],
                      (short)((int32_t*)&g_sys.rom[0x36614])[*(int *)W[0xAB04 + (sVar1 * 6)] != 1], 0);
    sVar1 = sVar1 + 1;
  } while (sVar1 < 2);
  sVar1 = menu_handle_page_select(0);
  if (sVar1 != 0) {
    title_option_a();
  }
  sVar1 = menu_handle_value_adjust(0xab04);
  if (sVar1 != 0) {
    W[0x168EC + (W[0x3FB7])] = 0x2000;
  }
  sVar1 = check_confirm_button();
  if ((sVar1 != 0) && (W[0x3FB7] == '\x03')) {
    W[0x3FB4] = 0;
    W[0x3FB7] = '\0';
  }
  return;
}

/* ---- title_option_b_init ---- */

void title_option_b_init(void)

{
  sync_post();
  title_substate_dispatch(0);
  menu_items_clear_highlights(0x67f4);
  W[0x1692A] = W[0x3FFA];
  W[0x16920] = (int)W16(0x3FF8);
  W[0x16928] = 0;
  W[0x1692C] = W[0x3FFC];
  W[0x3FB4] = 5;
  return;
}

/* ---- title_option_b_run ---- */

void title_option_b_run(void)

{
  short sVar1;
  undefined2 uVar2;
  undefined2 uVar3;
  undefined2 uVar4;
  undefined2 uVar5;
  
  uVar5 = 0x100;
  uVar4 = 3;
  menu_draw_items(0x67f4, 0);
  uVar3 = 0x100;
  uVar2 = 3;
  menu_draw_labels(0x3686c, uVar4);
  sVar1 = menu_handle_page_select(uVar2);
  if (sVar1 != 0) {
    title_option_b();
  }
  sVar1 = menu_handle_value_adjust(0x67f4);
  if (sVar1 != 0) {
    W[0x168EC + (W[0x3FB8])] = 0x2000;
  }
  sVar1 = check_confirm_button();
  if ((sVar1 != 0) && (W[0x3FB8] == '\x04')) {
    if (W[0x16928] != 0) {
      settings_score_table_reset();
    }
    W[0x3FB4] = 0;
    W[0x3FB8] = '\0';
  }
  return;
}

/* ---- title_rom_checksum_test ---- */

int title_rom_checksum_test(void)

{
  W[0x16918] = 0x19;
  FUN_0003b114();
  W[0x16918] = 0xffff;
  /* Original reads ROM checksum result - simplified */
  return W[0x16914] != 0;
}

/* ---- title_rom_test_run ---- */

void title_rom_test_run(void)

{
  short sVar1;
  undefined2 uVar2;
  short *psVar3;
  
  menu_draw_labels(0x37070, 0);
  psVar3 = &W[0x168FC];
  sVar1 = 0;
  _safety_ctr = 0;
  do {
    if (*psVar3 == 0) {
      uVar2 = 0xa6ad;
LAB_0001bd34:
      text_print_string(0x1d, sVar1 + 7, uVar2, 0);
    }
    else if (*psVar3 == 1) {
      uVar2 = 0xa6df;
      goto LAB_0001bd34;
    }
    sVar1 = sVar1 + 1;
    psVar3 = psVar3 + 1;
    if (0xb < sVar1) {
      sVar1 = check_confirm_button();
      if (sVar1 != 0) {
        W[0x3FB4] = 0x16;
        W[0x3FC2] = 0;
        W[0x3FB0] = W[0x4010];
        title_menu_select();
      }
      if (W[0x1691A] != 0) {
        text_print_string(0x1d, W[0x16914] + 7, 0x36663, 0);
        uVar2 = title_rom_checksum_test();
        W[0x168FC + (W[0x16914])] = uVar2;
        text_print_string(0x1d, W[0x16914] + 7, 0x3667a, 0);
        W[0x1691A] = W[0x1691A] + -1;
        if (W[0x1691A] == 0) {
          W[0x16914] = -1;
        }
        else {
          W[0x16914] = W[0x16914] + 1;
        }
      }
      return;
    }
  if (++_safety_ctr > 10000) break; } while( true ); _safety_ctr = 0;
}

/* ---- title_service_menu_run ---- */

void title_service_menu_run(void)

{
  short sVar1;
  undefined2 uVar2;
  
  uVar2 = 5;
  menu_handle_page_select(0);
  menu_draw_labels(0x368cc,uVar2);
  dipswitch_display_update();
  sVar1 = check_confirm_button();
  if (sVar1 != 0) {
    if (W[0x3FB9] < '\x04') {
      W[0x3FB4] = W[0x3FB9] * 2 + 8;
    }
    else {
      W[0x3FB4] = 0;
      W[0x3FB9] = '\0';
    }
  }
  return;
}

/* ---- title_sprite_animation_update ---- */

void title_sprite_animation_update(void)

{
  uint16_t uVar2;
  uint16_t *puVar1;
  uint16_t uVar3;
  
  if (W[0x16918] == 0x19) {
    dsp_frame_counter_decrement();
    if (-1 < W[0x16914]) {
      W[0x0C98] = W[0x0C98] + 1;
      if ((W[0x0C98] & 8) == 0) {
        uVar2 = 0;
      }
      else {
        uVar2 = 0x4000;
      }
      if (W[0x16914] == 0xc) {
        puVar1 = &g_sys.textram[0x0330];
      }
      else {
        puVar1 = (uint16_t *)(&g_sys.textram[0x03BA] + W[0x16914] * 0x80);
      }
      uVar3 = 0;
      do {
        tram_w16(puVar1, uVar2 | (tram_r16(puVar1) & 0xfff));
        uVar3 = uVar3 + 1;
        puVar1 = puVar1 + 1;
      } while (uVar3 < 7);
    }
  }
  else {
    if (W[0x16918] != 0x17) goto LAB_0001a200;
    FUN_0003e128();
  }
  g_sys.syscon[0x14] = 0;
LAB_0001a200:
  keycus_write_1();
  return;
}

/* ---- title_substate_dispatch ---- */

void title_substate_dispatch(undefined4 param_1)

{
  /* THE MENU'S CONTROL LEGEND. Recovered from the M68K at 0x01A40E; this was
   * a stub whose own comment said "Original uses ROM jump table at 0x1A440
   * ... For now, do basic setup", i.e. the lost-jump-table class (register
   * rows 65/85/94/104). The invented set_background_color/tilemap calls it
   * used instead are NOT in the ROM function and are gone.
   *
   * The dispatch is
   *     move.w ($c,A7),D0 ; ext.l D0 ; subq.l #6,D0 ; bhi default
   *     move.w ($14,PC,D0.w*2),D0 ; jmp ($2,PC,D0.w)
   * with the table at 0x01A434 holding
   *     000E 0022 0038 004E 008C 00C8 00DC
   * relative to 0x01A434, so the seven cases are sub-states 6..12. A2 is
   * 0x02108C = text_print_string and A3 is the string at 0x36654; every call
   * is `pea col ; pea row ; pea str ; move.w #$c` -> print(col,row,str,12).
   *
   * Case 6 deliberately FALLS THROUGH into case 7 (its body ends at 0x01A454
   * and case 7 begins at 0x01A456). */
  switch ((int)param_1) {
  case 6:                                                  /* 0x01A442 */
    text_print_string(0x16, 0x1b, (char *)0x36654, 0xc);   /* UP/DOWN:CHOOSE */
    /* fall through -- the ROM does */
  case 7:                                                  /* 0x01A456 */
    text_print_string(0x04, 0x1b, (char *)0x3663d, 0xc);   /* START:ENTER */
    break;
  case 8:                                                  /* 0x01A46C */
    text_print_string(0x01, 0x1b, (char *)0x3a528, 0xc);   /* TO EXIT, PUSH START AND HANDLEBARS UP */
    break;
  case 9:                                                  /* 0x01A482 */
    text_print_string(0x04, 0x19, (char *)0x3663d, 0xc);   /* START:ENTER */
    text_print_string(0x16, 0x19, (char *)0x36654, 0xc);   /* UP/DOWN:CHOOSE */
    text_print_string(0x04, 0x1b, (char *)0x3661c, 0xc);   /* LEFT:DECREMENT   RIGHT:INCREMENT */
    break;
  case 10:                                                 /* 0x01A4C0 */
    text_print_string(0x04, 0x17, (char *)0x3a54e, 0xc);   /* START:ENTER/REQUEST/STOP */
    text_print_string(0x04, 0x19, (char *)0x36654, 0xc);   /* UP/DOWN:CHOOSE */
    text_print_string(0x04, 0x1b, (char *)0x3661c, 0xc);   /* LEFT:DECREMENT   RIGHT:INCREMENT */
    break;
  case 11:                                                 /* 0x01A4FC */
    text_print_string(0x04, 0x1b, (char *)0x36649, 0xc);   /* START:EXIT */
    break;
  case 12:                                                 /* 0x01A510 */
    text_print_string(0x02, 0x19, (char *)0x3a567, 0xc);   /* UP/DOWN:PAGE */
    text_print_string(0x16, 0x19, (char *)0x36649, 0xc);   /* START:EXIT */
    text_print_string(0x02, 0x1b, (char *)0x3a574, 0xc);   /* HANDLEBARS RIGHT AND START:RESET ADS */
    break;
  default:                                                 /* 0x01A554 */
    break;
  }
  return;
}

/* ========== FINAL STUB REPLACEMENTS (14) ========== */

/* ---- attract_clear_textram ---- */

void attract_clear_textram(void)

{
  short sVar1;
  short sVar2;
  int _c;
  uint8_t *puVar3;
  uint8_t *puVar4;

  /* THE TILEMAP IS BIG-ENDIAN; `g_sys.textram` is uint8_t[]. The transpile
   * had `undefined4 *` here and stored the literal 0x03BF03BF, which on this
   * host lays down BF 03 BF 03 -- `text_hw.c` then reads back 0xBF03, i.e.
   * tile 0x303 with flip-X, flip-Y and palette 11 instead of a plain 0x3BF.
   * Register row 59's class. Latent until the `game_logic.c` shadow of
   * `attract_tilemap_init` was removed (row 136), which is what first gave
   * this function a live caller; the shadow it replaced wrote the two bytes
   * explicitly and was correct. */
  puVar3 = &g_sys.textram[0];
  sVar2 = 0x1d;
  do {
    sVar1 = 3;
    do {
      puVar4 = puVar3;
      for (_c = 0; _c < 10; _c++) tram_w16(puVar4 + _c * 2, 0x3bf);
      sVar1 = sVar1 + -1;
      puVar3 = puVar4 + 5 * 4;
    } while (sVar1 != -1);
    puVar3 = puVar4 + 0x11 * 4;
    sVar2 = sVar2 + -1;
  } while (sVar2 != -1);
  return;
}

/* ---- attract_enable_display ---- */

void attract_enable_display(void)

{
  g_sys.videomix[0x001B] = 0x7e;
  return;
}

/* ---- attract_load_char_rom ---- */

void attract_load_char_rom(void)

{
  short sVar1;
  
  mem_write32(0x8A0000, R[0x1C8D2]);
  mem_write32(0x8A0002, R[0x1C8D4]);
  mem_write32(0x8A0004, R[0x1C8D6]);
  mem_write32(0x8A0006, R[0x1C8D8]);
  sVar1 = 0x40;
  do {
    sVar1 = sVar1 + -1;
  } while (sVar1 != -1);
  return;
}

/* ---- attract_load_palette ---- */

void attract_load_palette(void)

{
  short sVar1;
  undefined1 *puVar2;
  undefined1 *puVar3;
  undefined1 *puVar4;
  undefined1 *puVar5;
  
  sVar1 = 0xff;
  puVar2 = &g_sys.palette_ram[0x7E00];
  puVar3 = &g_sys.palette_ram[0xFE00];
  puVar4 = &g_sys.palette_ram[0x17E00];
  puVar5 = &R[0x20994];
  do {
    *puVar4 = puVar5[0x200];
    *puVar3 = puVar5[0x100];
    *puVar2 = *puVar5;
    sVar1 = sVar1 + -1;
    puVar2 = puVar2 + 1;
    puVar3 = puVar3 + 1;
    puVar4 = puVar4 + 1;
    puVar5 = puVar5 + 1;
  } while (sVar1 != -1);
  return;
}

/* ---- attract_load_sprite_table ---- */

void attract_load_sprite_table(void)

{
  short sVar1;
  undefined4 *puVar2;
  undefined4 *puVar3;
  undefined4 *puVar4;
  undefined4 *puVar5;
  
  puVar4 = &R[0x1C994];
  puVar2 = &g_sys.cgram[0x1A000];
  do {
    sVar1 = 7;
    do {
      *puVar2 = *puVar4;
      puVar2[1] = puVar4[1];
      puVar5 = puVar4 + 3;
      puVar3 = puVar2 + 3;
      puVar2[2] = puVar4[2];
      puVar4 = puVar4 + 4;
      puVar2 = puVar2 + 4;
      *puVar3 = *puVar5;
      sVar1 = sVar1 + -1;
    } while (sVar1 != -1);
  } while ((puVar2 != &g_sys.textram[0]) && ((int)puVar4 < 0x20994));
  return;
}

/* ---- attract_highscore_sprites_load ---- */

void attract_highscore_sprites_load(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  char *pcVar5;
  char *pcVar6;
  
  cgram_load_tile_block(0xa3, 0x250);
  cgram_load_tile_block(0x97,0x150);
  cgram_load_tile_block(0x98,0x168);
  cgram_load_tile_block(0x99,0x180);
  /* 0x032372: the three name letters of each course's top three, from the
   * TODAY'S tables, into cgram blocks 0x1a0 + course*36 + rank*12 + k*4.
   * The transpile walked `char *pcVar5 = &W[0x15BF4 + e]` -- the host bytes
   * of ONE _W[] slot -- so the letters and the next-link came out of a
   * pointer's bytes, and every name was a junk glyph. The table keeps one
   * byte per slot (see object_display_init). */
  (void)pcVar5; (void)pcVar6; (void)iVar1; (void)iVar2; (void)iVar4;
  { int c, r, k;
    for (c = 0; c < 3; c++) {
      int e8 = (int8_t)W[0x15E70 + c];
      for (r = 0; r < 3; r++) {
        int ent = 0x15BF0 + c * 0xa0 + e8 * 8;
        for (k = 0; k < 3; k++)
          cgram_load_tile_block((int8_t)W[ent + 4 + k] + 0x5d,
                                c * 36 + r * 12 + k * 4 + 0x1a0);
        e8 = (int8_t)W[ent + 7];
        if (e8 < 0 || e8 > 19) e8 = 0;                  /* host guard */
      }
    } }
  iVar3 = 0;
  do {
    cgram_load_tile_block((short)iVar3 + 0x43, iVar3 + 0x220);
    iVar3 = iVar3 + 1;
  } while (iVar3 < 10);
  cgram_load_tile_block(0x38, 0x22a);
  cgram_load_tile_block(0x3c,0x22b);
  cgram_load_tile_block(0x3b,0x22c);
  cgram_load_tile_block(0x51,0x22d);
  iVar3 = 0;
  do {
    cgram_load_tile_block((short)iVar3 + 0x78, iVar3 * 4 + 0x240);
    iVar3 = iVar3 + 1;
  } while (iVar3 < 3);
  cz_load_color_ramp(0xa3, 1);   /* 0x032450 `move.b #$1,-(a7)` -- the palette byte (row 118) */
  return;
}

/* ---- attract_advance_sequence @ 0x00D484 ---- */
/* Rewritten from the M68K machine code (MAME dasm @0xD484) — the
 * transpiled version had constant-folded conditionals ("0 < 3",
 * "0 == 2"), lost the stage variable ("// 0 = iVar1"), and read the
 * replay-id table at 0x353A8 native-LE (fed replay_load_builtin
 * 0x0A000000 instead of 10).
 *
 * Stage thresholds @0x35394: [0, 1200, 1800, 2700] (frames; entry 4 =
 * 0x7FFF sentinel). Replay ids @0x353A8: [10, 13, 15, 12]. At each
 * stage boundary (stages 0-2) the matching builtin replay is loaded;
 * near the end of stage 2 (< 0x40 frames left) the fade-out flags are
 * set. */
void attract_advance_sequence(void)

{
  int stage = 0;
  int i;
  int32_t since, until;

  for (i = 0; i < 4; i++) {
    if ((int32_t)W[0x15EB8] >= (int32_t)vrd32(0x35394 + i * 4))
      stage = i;
  }
  since = (int32_t)W[0x15EB8] - (int32_t)vrd32(0x35394 + stage * 4);
  until = (int32_t)vrd32(0x35394 + stage * 4 + 4) - (int32_t)W[0x15EB8];
  if (stage <= 2 && since == 0) {
    W[0x16D88] = 0xe;
    replay_load_builtin((int32_t)vrd32(0x353A8 + stage * 4));
  }
  replay_frame_tick();
  camera_grid_calc_position();
  FUN_0000e016();
  if (stage == 2 && until < 0x40) {
    W[0x1703C] = 1;
    W[0x15ED0] = 4;
  }
  return;
}

/* ---- attract_cinematic_tick ---- */

void attract_cinematic_tick(void)

{
  int iVar1;
  short sVar2;
  /* void */;
  int iVar3;
  undefined4 *puVar4;
  int iVar5;
  undefined4 *puVar6;
  
  /* THE STAGE VARIABLE. Ghidra dropped the assignment ("// 0 = iVar3;") and
   * then constant-folded every use of it to 0, so this whole function ran as
   * "stage 0, for ever" and attract never left the cinematic flyover — MAME
   * emits only [0, 108] during the logo phase (sky + a non-object record; the
   * PROP CYCLE logo is a SPRITE) while we drew all 26 flyover objects over the
   * top of it. Register row 24.
   *
   * From the M68K at 0x00D9A0:
   *     moveq #0,d5
   *   loop: move.l (a3),d0            ; a3 = 0xE15ECC
   *         cmp.l $353f4(d5.l*4),d0
   *         blt  skip
   *         move.l d5,d2              ; <-- stage = i, the lost line
   *   skip: addq.l #1,d5 ; moveq #$a,d0 ; cmp.l d0,d5 ; blt loop
   * i.e. TEN entries, and the table is read BIG-ENDIAN 32-bit — the C had
   * `(&R[0x353F4])[i]`, which indexes a uint8_t[] and reads ONE byte.
   * Thresholds: 0, 255, 612, 730, 1210, 1687, 1931, 2164, 2404, 3124. */
  int stage = 0;
  iVar5 = (int32_t)W[0x15ECC];
  for (iVar3 = 0; iVar3 < 10; iVar3++) {
    if ((int32_t)vrd32(0x353F4 + iVar3 * 4) <= iVar5) stage = iVar3;
  }
  iVar3 = (int32_t)vrd32(0x353F4 + stage * 4);          /* since == iVar5-iVar3 */
  iVar1 = (int32_t)vrd32(0x353F4 + stage * 4 + 4) - iVar5;   /* until */
  attract_draw_text_overlay();
  /* ROM 0x00D9DC / 0x00D9EC: both are `$XXXXX(d2.l*4)` off the stage, and
   * both were raw host-pointer dereferences of a ROM ADDRESS.
   * replay ids  @0x35430 = [0,1,3,16,4,5,6,7,8,9]
   * W[0x16D88]  @0x35458 = [6,4,3,0,4,2,5,1,0,0] */
  if ((iVar5 == iVar3) && (stage <= 8)) {
    replay_load_builtin((int32_t)vrd32(0x35430 + stage * 4));
  }
  W[0x16D88] = (int32_t)vrd32(0x35458 + stage * 4);
  if (iVar5 == iVar3) {
    W[0x16D88] = W[0x16D88] + 10;
  }
  replay_frame_tick();
  /* The M68K switch at 0x00DA0A sends stages 0..8 to one block, 0xFF to the
   * big init, and anything else straight to the tail. Ghidra folded all three
   * comparisons against the lost stage into constants. Note stage can only be
   * 0..9 out of the loop above, so the 0xFF arm is unreachable from here and
   * is kept only for fidelity. */
  if (stage > 8) {
    if (stage == 0xff) {
      if (iVar5 == iVar3) {
        W[0x0E0C] = 3;
        W16_SET(0xE64, 0);
        W[0x15F60] = 0;
        gameplay_init_player_and_world();
        gameplay_init_state_vars();
        W[0x1703C] = 0;
        W[0xEB16] = 0xff;
        W[0x15ED0] = 4;
        g_fog_r = 0;
        g_fog_g = 0;
        g_fog_b = 0;
        g_fog_mode = 1;
        W_SET_HI16(0xEB20, 0);   /* clr.w $e0eb20: 16-bit, the HIGH half */
        /* IMMEDIATES, NOT ADDRESSES -- the camera X and Z. ROM 0x00DAE2 is
         *     move.l  #$4e5fe, (a2)          a2 = 0xE00CDC
         * and 0x00DAF0  move.l  #$12589c, $8(a2)
         * Rows 4/63's class (the sky dome's Z). `&R[...]` put host pointers
         * in the camera block, so the attract cinematic's X and Z were
         * ASLR-dependent garbage -- different every run. Unlike the twin in
         * `FUN_0000ce1e`, this one was ALREADY LIVE via playlist cases
         * 14/15, so it has been wrong in every attract run to date. Every
         * other line of this block already matches the ROM. */
        W[0x0CDC] = 0x4E5FE;
        W[0x0CE0] = (int32_t)0xffef2000;
        W[0x0CE4] = 0x12589C;
        W[0x0CE8] = 0x4000;
        W[0x0CEC] = 0;
        W[0x0CF0] = 0;
        W[0x0CF4] = 0x49;
      }
      debug_profiler_mark();
      objects_move_update();
      debug_profiler_mark();
      debug_profiler_mark();
      terrain_chunk_visibility(W[0x0CDC],W[0x0CE0],W[0x0CE4],W[0x0CEC]);
      debug_profiler_mark();
      terrain_props_dispatch();
      debug_profiler_mark();
      objects_render_master();
      debug_profiler_mark();
      balloon_render_and_hit_check();
      debug_profiler_mark();
      W[0x0C8C] = W[0x0C8C] + 1;
      W[0x0CEC] = W[0x0CEC] + 0x20;
      if (iVar1 < 0x40) {
        g_fog_mode = 3;
        W[0x1703C] = 1;
        W[0x15ED0] = 4;
      }
    }
    goto LAB_0000db9e;
  }
  if ((stage == 8) && (iVar1 < 0x20)) {
    W[0x1703C] = 1;
    W[0x15ED0] = 8;
  }
LAB_0000db9e:
  /* ROM 0x00DBA6 / 0x00DC2E: `moveq #8,d1 ; move.l (a1)+,(a0)+ ; dbra` --
   * NINE longs, the replay camera block 0xE17040 over the real one at
   * 0xE00CDC. The transpile walked it with an `undefined4 *` over the 8-byte
   * `_W[]` slots: 36 host bytes, i.e. x and half of y, and the cinematic's
   * z, pitch, heading and roll were never taken from the replay camera
   * (register row 161). */
  if (stage == 8) {
    replay_cam_copy9(0x0CDC, 0x17040);
    if (iVar1 < 0xf1) {
      if (iVar1 < 0x79) {
        iVar1 = 0x78;
      }
      else {
        iVar1 = 0xf0 - iVar1;
      }
    }
    else {
      iVar1 = 0;
    }
    /* 0x00DBEE `movea.w $20b006(d0.l*2)` -- extension word 0x0BB0, scale 2:
     * the lost-*2 half-stride class (register row 72). */
    iVar3 = (int)(int16_t)vrd16s(0x20B006 + ((iVar1 << 0xf) / 0x78 >> 1 & 0x7ffe) * 2);
    if (iVar3 < 0) {
      iVar3 = iVar3 + 1;
    }
    iVar3 = (iVar3 >> 1) + 0x4000;
    W[0x0CE8] = (uint32_t)((0x7fff - iVar3) * 0x4000 + iVar3 * W[0x1704C]) >> 0xf;
  }
  else {
    replay_cam_copy9(0x0CDC, 0x17040);
  }
  camera_grid_calc_position();
  if (W[0x15ECC] < 0x40) {
    iVar3 = 0x400 - ((short)vrd16s(0x20B004 + ((W[0x15ECC] << 8) >> 1 & 0x7ffe) * 2) * 0x2c0 >> 0xf);
    iVar5 = (short)vrd16s(0x20B004 + ((int)(short)vrd16s(0x20B004 + ((W[0x15ECC] << 8) >> 1 & 0x7ffe) * 2) >> 2 & 0x7ffe) * 2) +
            -0x7fff;
  }
  else {
    iVar3 = 0x140;
    iVar5 = 0;
  }
  dsp_cmd_place_object_rotated_abs(2,0x36b,0xfffffe40,iVar3,0x500,0,iVar5,0,0);
  if ((W[0x2C0C] & 0x10) != 0) {
    debug_draw_value(5,0xf,4,(char*)(g_sys.rom + 0x3a267),W[0x15ECC],&R[0x3A26E],&R[0x60004]);
    debug_draw_value(5,0x10,4,(char*)(g_sys.rom + 0x3a26f),W[0x16D88],&R[0x3A26E],&R[0x60004]);
    debug_draw_value(5,0x11,4,(char*)(g_sys.rom + 0x3a276),W[0x15BE4],&R[0x3A26E],&R[0x60004]);
  }
  if (W[0x1703C] == 0) {
    W[0xEB16] = W[0xEB16] - W[0x15ED0];
    if (W[0xEB16] < 0) {
      W[0xEB16] = 0;
    }
  }
  else {
    W[0xEB16] = W[0x15ED0] + W[0xEB16];
    if ((0xff < W[0xEB16]) && (W[0xEB16] = 0xff, stage >= 6)) {
      attract_advance_sub_state();
      FUN_0000ffb2();
    }
  }
  W[0x15ECC] = W[0x15ECC] + 1;
  return;
}

/* ---- attract_course_load ---- */

void attract_course_load(int param_1)

{
  int iVar1;
  
  W[0x0E0C] = param_1;
  gameplay_init_player_and_world();
  gameplay_init_state_vars();
  g_fog_mode = 3;
  /* ROM 0x032072..0x032096: `lea $3986c(d0.l*4),a0` with d0 = course*7,
   * then seven `move.l (a0)+` into the camera block 0xE00CDC..0xE00CF4 --
   * all BIG-ENDIAN longs. Four of the seven were read with rom_nr32, the
   * host-NATIVE accessor, so X/Y/Z/heading came out byte-swapped (course 0's
   * X 415888 read as -1873279488): the terrain flyover camera started
   * nowhere near the course. */
  iVar1 = param_1 * 0x1c;
  W[0x0CDC] = vrd32s(0x3986C + iVar1);
  W[0x0CE0] = vrd32s(0x39870 + iVar1);
  W[0x0CE4] = vrd32s(0x39874 + iVar1);
  W[0x0CE8] = vrd32s(0x39878 + iVar1);
  W[0x0CEC] = vrd32s(0x3987C + iVar1);
  W[0x0CF0] = vrd32s(0x39880 + iVar1);
  W[0x0CF4] = vrd32s(0x39884 + iVar1);
  g_fog_r = 0;
  g_fog_g = 0;
  g_fog_b = 0;
  return;
}

/* ---- attract_draw_text_overlay ---- */

/* Rewritten from ROM 0x00DEDE. The transpile had lost the stage index
 * (`// 0 = iVar1;`, register row 84), read the two 5-entry BE32 tables at
 * 0x3541C / 0x35480 one BYTE at a time, and dropped the base tile and
 * palette of the second banner -- which drew tiles 0x000.. (the whole
 * character set) across the bottom of the screen. */
void attract_draw_text_overlay(void)
{
  int32_t t = (int32_t)W[0x15ECC] - 3;             /* moveq #-3 ; add.l */
  int stage = 0, i;
  if (t < 0) t = 0;
  for (i = 0; i < 5; i++)                          /* 0x00DEF0 */
    if ((int32_t)vrd32(0x3541C + i * 4) <= t) stage = i;
  t -= (int32_t)vrd32(0x3541C + stage * 4);
  int32_t blk = (int32_t)vrd32(0x35480 + stage * 4);
  if (blk <= 0) {                                  /* 0x00DF72 */
    text_draw_rect_solid(0, 1, 0x19);
  } else if (t < 0x10) {                           /* 0x00DF24 */
    text_draw_number(blk, 0x1a0, t);
    text_draw_rect_blink(0, 1, 0x19, 0x120, 1);
  } else {                                         /* 0x00DF54 */
    text_draw_rect_blink(blk, 1, 0x19, 0x1a0, 5);
  }
}

/* NOTE: ROM 0xD72E (called from attract_flyover_tick, case 17, once the
 * fade/threshold gate at 0xD608-0xD626 is armed) turned out to be a
 * near-duplicate of the already-correct `replay_frame_tick` @0x00DDBC in
 * game_replay.c: same read of the 16-byte replay record at W[0x16D7C],
 * same player_bounds_check/objects_move_update/world_grid_calc_position/
 * conditional world_render_all(>300)/camera update/terrain_chunk_
 * visibility/terrain_props_dispatch/player_render/objects_render_master/
 * balloon_render_and_hit_check/stage_camera_path_update/
 * environment_zone_tick/particle_system_update sequence, same +0x10
 * record advance. The two ROM addresses are not literally the same
 * function -- 0xD72E calls camera_update_wrapper()+
 * camera_grid_calc_position() where replay_frame_tick calls the more
 * elaborate replay_camera_update() -- but the rest matches call-for-call,
 * and rather than re-derive the ROM-address-vs-host-pointer handling
 * replay_frame_tick's own header already documents fixing (register row
 * 6's class), attract_flyover_tick calls replay_frame_tick() directly:
 * the already-verified camera update is the safer choice, not a guess. */

/* ---- attract_flyover_text ---- */

void attract_flyover_text(void)

{
  int iVar1;
  int iVar2;
  int iVar_selected;

  /* Same threshold-bucket search as attract_flyover_tick, a different
   * 5-entry table (ROM 0x353CC), and the same register-row-84 dropped-
   * assignment class: `// 0 = iVar1;` and the two hardcoded `[0]` indices
   * below were both meant to be this bucket index. Verified against the
   * real M68K (ROM 0xD850): `cmp.l 353cc(d1*4),d4 ; blt.b ... ; move.l
   * d1,d5` is the store (register d5 from here on); `tst.l d5 ; bpl.b ...
   * ; moveq #0,d5` is the clamp the dead `if (0 < 0)` block was hallucinated
   * from; and `sub.l 353cc(d5*4),d4` / `movea.l 353e0(d5*4),a0` both index
   * by d5, not literal 0. `uVar3`/`uVar4` in the old decompile were
   * write-only (never read) and are dropped. The `text_draw_rect_blink`
   * "no number" call also had two wrong constant args (0,0 instead of
   * the ROM's 0x1a0,5 -- register row 62's class, on this call).
   *
   * Both table reads were ALSO byte-truncated independently of the
   * dropped assignment -- `(&R[ADDR])[idx]` on `R` (uint8_t[]) reads one
   * byte where the disassembly above already says these are 32-bit reads
   * at a 4-byte stride (the P5/S3 class). This function's own comment
   * had the right stride in the disassembly note but the C below it
   * still used the byte form -- fixed here, not just documented. */
  iVar2 = W[0x15ECC] + -3;
  if (iVar2 < 0) {
    iVar2 = 0;
  }
  iVar_selected = 0;
  iVar1 = 0;
  do {
    if ((int32_t)vrd32(0x353CC + iVar1 * 4) <= iVar2) {
      iVar_selected = iVar1;
    }
    iVar1 = iVar1 + 1;
  } while (iVar1 < 5);
  if (iVar_selected < 0) {
    iVar_selected = 0;
  }
  iVar2 = iVar2 - (int32_t)vrd32(0x353CC + iVar_selected * 4);
  iVar1 = (int32_t)vrd32(0x353E0 + iVar_selected * 4);
  if (iVar2 < 0x10) {
    if (0 < iVar1) {
      text_draw_number((short)iVar1,0x1a0,iVar2);
      text_draw_rect_blink(0, 1, 0x19, 0x120, 1);
      return;
    }
  }
  else if (0 < iVar1) {
    text_draw_rect_blink((short)iVar1,1,0x19, 0x1a0, 5);
    return;
  }
  text_draw_rect_solid(0,1,0x19);
  return;
}

/* ---- attract_flyover_tick ---- */

void attract_flyover_tick(void)

{
  int iVar1;
  int iVar_selected;
  uint32_t uVar2;
  int iVar3;

  /* Threshold-bucket search over the 5-entry table at ROM 0x353B8: finds
   * the largest bucket index whose threshold W[0x15ECC] has passed.
   * Ghidra dropped the assignment (`// 0 = uVar2;`) and then folded every
   * downstream use of the result to the literal 0, which is what turned
   * BOTH gates below into dead code -- register row 84's class. Verified
   * against the real M68K (ROM 0xD594): `cmp.l (a0,d3*4),d0 ; blt.b ...
   * ; move.l d3,d2` is exactly this store, unconditionally reached; the
   * function's own jump table at 0xD5FA has 5 entries that are all the
   * identical value 0x000A, so it is NOT a real branch -- Ghidra modelled
   * that degeneracy as the spurious outer "if (3 < 0 && 0 - 4 != 0)" this
   * replaces. iVar_selected is register d2 from that point on.
   *
   * The table read itself was ALSO wrong, independently of the dropped
   * assignment: `(&R[0x353B8])[uVar2]` reads one BYTE (R is uint8_t[]),
   * where the real M68K (`cmp.l (a0,d3*4),d0` at ROM 0xD5C0) does a
   * 32-bit read at a 4-byte stride -- the P5/S3 class CLAUDE.md already
   * tracks elsewhere, not yet caught here. Confirmed by reading the ROM
   * table directly: [0, 60, 240, 420, 1500] -- entry 4 (1500) cannot
   * round-trip through a byte read at all. */
  iVar_selected = 0;
  uVar2 = 0;
  do {
    if ((int32_t)vrd32(0x353B8 + uVar2 * 4) <= W[0x15ECC]) {
      iVar_selected = uVar2;
    }
    uVar2 = uVar2 + 1;
  } while ((int)uVar2 < 5);
  attract_flyover_text();
  /* ROM 0xD5EC-0xD5FA: a bounds check (iVar_selected can only be 0-4, so
   * this never branches away) followed by a 5-entry internal jump table
   * whose entries are all the identical value 0x000A -- Ghidra's "Could
   * not recover jumptable at 0x0000d5fa" warning on this exact table is
   * what hid the rest of this function (register row -- attract phase
   * table). Every entry resolving to the same target means it is not a
   * real branch; the call it always reaches is replay_frame_tick(), see
   * the note above attract_flyover_text for why that call is used here
   * instead of a fresh port of the near-duplicate ROM 0xD72E. */
  replay_frame_tick();
  /* ROM 0xD60E-0xD626: the real, separate gate this function's dead code
   * used to hide (distinct from the jump-table bounds check just above,
   * which this project's earlier reading conflated with it). Verified
   * empirically against real MAME (tools/overnight/watch_attract_phase17.lua):
   * W[0x1703C] flips from 0 to nonzero at the exact frame W[0x15ECC]
   * crosses the table's last threshold (1500), confirming iVar_selected
   * reaching 4 (the loop above) is the gating condition, not a
   * coincidence of timing. */
  if (iVar_selected == 4 &&
      (int)((int32_t)vrd32(0x353B8 + 5 * 4) - W[0x15ECC]) < 0x20) {
    W[0x1703C] = 1;
    W[0x15ED0] = 4;
  }
  {
    if (W[0x15ECC] < 0x40) {
      iVar1 = 0x400 - ((short)vrd16s(0x20B004 + ((W[0x15ECC] << 8) >> 1 & 0x7ffe) * 2) * 0x2c0 >> 0xf);
      iVar3 = (short)vrd16s(0x20B004 + ((int)(short)vrd16s(0x20B004 + ((W[0x15ECC] << 8) >> 1 & 0x7ffe) * 2) >> 2 & 0x7ffe) * 2)
              + -0x7fff;
    }
    else {
      iVar1 = 0x140;
      iVar3 = 0;
    }
    dsp_cmd_place_object_rotated_abs(2,0x369,0xfffffe40,iVar1,0x500,0,iVar3,0,0);
    if ((W[0x2C0C] & 0x10) != 0) {
      debug_draw_value(5,0xf,4,(char*)(g_sys.rom + 0x3a220),W[0x15ECC],(int32_t*)&g_sys.rom[0x3A226],&R[0x60004]);
    }
    if (W[0x1703C] == 0) {
      W[0xEB16] = W[0xEB16] - W[0x15ED0];
      if (W[0xEB16] < 0) {
        W[0xEB16] = 0;
      }
    }
    else {
      W[0xEB16] = W[0x15ED0] + W[0xEB16];
      /* real gate (ROM 0xD6FC-0xD710): fade clamped to 0xff AND the
       * threshold search reached the LAST bucket (iVar_selected == 4) --
       * i.e. the flyover has both faded in and run its full duration. */
      if ((0xff < W[0xEB16]) && (W[0xEB16] = 0xff, iVar_selected >= 4)) {
        attract_advance_sub_state();
        FUN_0000ffb2();
      }
    }
    W[0x15ECC] = W[0x15ECC] + 1;
    return;
  }
}

/* ---- attract_flyover_init @ 0x00D508 ---- */
/* Case 16's real handler. Newly ported (register row -- attract phase
 * table): resets the flyover counter/fade, loads replay slot 9, sets up
 * the flyover's fog/cgram/palette state (same shape as case 18's own
 * init below), sets the phase directly to 17 and runs attract_flyover_tick()
 * once -- the same init-calls-its-own-run-once pattern every other
 * case pair in this switch already uses (case 0, case 18). Ghidra folded
 * this whole function into attract_advance_sequence's decompilation
 * (there is no function-boundary comment for it in the raw decompile),
 * but the real M68K has attract_advance_sequence's own `rts` at 0xD506,
 * immediately followed by this as a fresh, independent routine. */
void attract_flyover_init(void)

{
  W[0x15ECC] = 0;
  W[0x1703C] = 0;
  W[0x15ED0] = 0x100;
  replay_load_builtin(9);
  W[0x012BC] = 0;
  W[0xEB1A] = 0;
  W[0xEB1C] = 0;
  W[0xEB1E] = 0;
  W[0xEB18] = 3;
  W[0x0E44] = 0xe10;
  W[0x0E48] = 0xe10;
  cgram_load_tile_block(0, 0x120);
  cz_load_color_ramp(0, 1);
  cz_load_color_ramp(0xb, 5);
  W[0x0CC4] = 0x11;
  attract_flyover_tick();
  return;
}

/* ---- attract_cinematic_init @ 0x00D8FE ---- */
/* Case 18's real handler -- same init-calls-its-own-run-once shape as
 * attract_flyover_init above (same fog/cgram/palette setup, no
 * replay_load_builtin here), phase set to 19 and attract_cinematic_tick()
 * (the case-19 function, ROM 0xD97A) run once immediately. Ghidra folded
 * this into attract_flyover_text's decompilation with no function
 * boundary of its own; the real M68K has attract_flyover_text's `rts` at
 * 0xD8FC immediately followed by this as a fresh, independent routine. */
void attract_cinematic_init(void)

{
  W[0x15ECC] = 0;
  W[0x1703C] = 0;
  W[0x15ED0] = 0x100;
  W[0xEB1A] = 0;
  W[0xEB1C] = 0;
  W[0xEB1E] = 0;
  W[0xEB18] = 1;
  W[0x0E44] = 0xe10;
  W[0x0E48] = 0xe10;
  cgram_load_tile_block(0, 0x120);
  cz_load_color_ramp(0, 1);
  cz_load_color_ramp(0xb, 5);
  W[0x0CC4] = 0x13;
  attract_cinematic_tick();
  return;
}

/* ---- attract_gameplay_tick ---- */

void attract_gameplay_tick(void)

{
  int iVar1;
  
  if (W[0x0CAC] == 0) {
    attract_text_stage_labels();
    attract_advance_sequence();
    W[0x0E44] = W[0x0E44] + -1;
    if ((int)W[0x0E44] < 0) {
      W[0x0E44] = (uint8_t *)0x0;
    }
    if ((int)W[0x0E44] < (int)W[0x0E48]) {
      W[0x0E48] = W[0x0E48] + -1;
    }
    if ((int)W[0x0E48] < (int)W[0x0E44]) {
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
    W[0x3EAC] = W[0x0C8C];
    iVar1 = 0;
    do {
      if (W[0x15EB8] == (&R[0x35354])[iVar1 * 2]) {
        W[0x0CAC] = 1;
        W[0x15EBC] = (&R[0x35358])[iVar1 * 2] + -1;
        W[0x15EC0] = iVar1;
      }
      iVar1 = iVar1 + 1;
    } while (iVar1 < 2);
    if (W[0x15BE0] - W[0x15BE4] < 0x80) {
      g_fog_mode = 3;
      W[0xEB16] = (((short)W[0x15BE4] - (short)W[0x15BE0]) + 0x7f) * 2;
    }
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
        attract_advance_sub_state();
      }
    }
  }
  else {
    if ((W[0x2C0C] & 0x10) != 0) {
      debug_draw_value(5,0x16,4,0xa20c,(short)W[0x15EBC],0xa213,4);
    }
    stage_camera_path_update();
    W[0x15EBC] = W[0x15EBC] + -1;
    if (-1 < W[0x15EBC]) {
      return;
    }
    text_print_string(5,0x14,0x3a214, 0);
    W[0x0CAC] = 0;
  }
  W[0x15EB8] = W[0x15EB8] + 1;
  return;
}

/* ---- attract_highscore_overlay_draw ---- */

void attract_highscore_overlay_draw(int param_1)

{
  int iVar1;
  uint32_t uVar2;
  int iVar3;
  
  /* ROM 0x0320C4: `move.l $e04088(d0.l*8),d4` -- the per-course best score
   * is a long at 0xE04088 + course*8 (the same 8-byte entry layout as the
   * top-ten list at 0xE04030: long, three name bytes, one spare). The
   * transpile indexed it `W[0x4088 + course*2]`, right only for course 0. */
  if (-1 < param_1) {
    uVar2 = W[0x4088 + (W[0x0E0C] * 8)] & 0xffffff;
    iVar3 = 0x130;
    iVar1 = 0;
    do {
      if (uVar2 != 0) {
        attract_tile_reveal_effect((int)uVar2 % 10 + 0x77,iVar3,param_1);
      }
      uVar2 = (int)uVar2 / 10;
      iVar3 = iVar3 + -4;
      iVar1 = iVar1 + 1;
    } while (iVar1 < 5);
    attract_tile_reveal_effect(0x6c,0x134,param_1);
    attract_tile_reveal_effect(0x70,0x138,param_1);
    attract_tile_reveal_effect(0x6f,0x13c,param_1);
    attract_tile_reveal_effect(0x85,0x140,param_1);
    iVar1 = 0;
    do {
      attract_tile_reveal_effect
                ((char)W[0x408C + (iVar1 + W[0x0E0C] * 8)] + 0x5d,iVar1 * 4 + 0x150,param_1);
      iVar1 = iVar1 + 1;
    } while (iVar1 < 3);
  }
  return;
}

/* ---- attract_logo_init ---- */

void attract_logo_init(void)

{
  sync_post();
  g_fog_r = 0;
  g_fog_g = 0;
  g_fog_b = 0;
  W[0xEB16] = 0;
  W[0x17374] = 0;
  W[0x17370] = 0xffe0;
  W[0x17372] = 0xffe8;
  attract_logo_run();
  return;
}

/* ---- attract_logo_run ---- */

void attract_logo_run(void)

{
  int iVar1;
  int iVar2;
  short sVar3;
  undefined2 uVar4;
  
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
  if (((W[0x17374] == 0x96) || (W[0x17374] == 0x9b)) || (W[0x17374] == 0xbe)) {
    fog_set_from_table(7);
    W16_SET(0x172FA, 1);
  }
  if (0x97 < W[0x17374]) {
    iVar2 = ((int)(short)vrd16s(0x20B004 + (0x960000 / (W[0x17374] + -0x34) + -0xc000 >> 1 & 0x7ffe) * 2)
            << 4) / 0x8000 + 2;
    iVar1 = iVar2 * (short)vrd16s(0x20B006 + (W[0x17374] * 0x1000 + -0x90000 >> 1 & 0x7ffe) * 2);
    if (iVar1 < 0) {
      iVar1 = iVar1 + 3;
    }
    W[0x17370] = (short)((iVar1 >> 2) / 0x8000) + -0x1c;
    iVar2 = iVar2 * (short)vrd16s(0x20B006 + (W[0x17374] * 0x2000 + -0x128000 >> 1 & 0x7ffe) * 2);
    if (iVar2 < 0) {
      iVar2 = iVar2 + 1;
    }
    W[0x17372] = (short)((iVar2 >> 1) / 0x8000) + -0x10;
  }
  attract_logo_tiles_draw();
  if (W[0x17374] < 0x87) {
    uVar4 = 0x392;
  }
  else {
    uVar4 = 0x393;
  }
  if (W[0x17374] < 0x3c) {
    sVar3 = 0x1cc - (short)W[0x17374];
  }
  else if (W[0x17374] < 0x5a) {
    sVar3 = 400;
  }
  else if (W[0x17374] < 0x78) {
    sVar3 = 0x1ea - (short)W[0x17374];
  }
  else {
    sVar3 = 0x172;
  }
  iVar1 = W[0x17370] * 0x40 + 0x800;
  if (iVar1 < 0) {
    iVar1 = W[0x17370] * 0x40 + 0x81f;
  }
  dsp_cmd_set_camera(2,uVar4,(iVar1 >> 5) + (int)sVar3,(W[0x17372] * 0x40 + 0x600) / 0x18 + -0x78,
                     0x350,0,0,0,0);
  W[0x17374] = W[0x17374] + 1;
  if (0x10e < W[0x17374]) {
    W[0x17374] = 0;
  }
  return;
}

/* ---- attract_logo_tiles_draw ---- */

void attract_logo_tiles_draw(undefined4 param_1)

{
  short sVar1;
  short sVar2;
  short sVar3;
  
  sVar3 = param_1 * 6 + 0x1ba;
  sVar2 = 0;
  do {
    sVar1 = 0;
    do {
      sprite_draw_2d(7,(int)sVar3,(int)W[0x17370] + sVar1 * 0x110,
                     (int)W[0x17372] + sVar2 * 0x110,0,0x22,0x22,0,0);
      sVar3 = sVar3 + 1;
      sVar1 = sVar1 + 1;
    } while (sVar1 < 3);
    sVar2 = sVar2 + 1;
  } while (sVar2 < 2);
  return;
}

/* ---- attract_overview_init @ 0x03385E ---- */
/* Attract playlist value 10: the RANKING screen's init. Checked against the
 * disassembly (ROM 0x03385E..0x0338F8); it matched the transpile line for
 * line except the camera Z, which was `&R[0x11998E]` -- a HOST POINTER where
 * ROM 0x03389E has the immediate `move.l #$11998e,$8(a2)` (register rows
 * 4/63's class). */

void attract_overview_init(void)

{
  W[0x0E0C] = 0;
  W16_SET(0xE10, 0);
  gameplay_init_player_and_world();
  gameplay_init_state_vars();
  sync_post();
  camera_state_reset();
  attract_tilemap_clear();
  W[0x0CDC] = 0x2cdc3;
  W[0x0CE0] = 0x157ed;
  W[0x0CE4] = 0x11998e;          /* immediate, ROM 0x03389E */
  W[0x0CEC] = 0xb323;
  W[0x0CE8] = 0;
  W[0x0CF0] = 0;
  W[0x0CF4] = 0x59;
  g_fog_r = 0;
  g_fog_g = 0;
  g_fog_b = 0;
  g_fog_mode = 3;
  W[0xEB16] = 0;
  attract_score_sprites_load();
  W[0x1736C] = 0;
  W[0x0CC4] = 0xb;
  attract_overview_run();
  return;
}

/* ---- attract_score_sprites_load ---- */

void attract_score_sprites_load(void)

{
  int iVar1;
  int iVar2;
  char cVar3;
  undefined2 uVar4;
  undefined1 uVar5;
  undefined2 uVar6;
  undefined1 uVar7;
  
  iVar1 = 0;
  do {
    cgram_load_tile_block(iVar1 + 0x77,iVar1 * 4 + 0x160);
    iVar1 = iVar1 + 1;
  } while (iVar1 < 10);
  cgram_load_tile_block(0x6c,0x188);
  cgram_load_tile_block(0x70,0x18c);
  cgram_load_tile_block(0x6f,400);
  cgram_load_tile_block(0x85, 0x194);
  iVar1 = 0;
  cVar3 = W[0x40A8];
  do {
    iVar2 = 0;
    do {
      cgram_load_tile_block
                ((char)W[0x4034 + (iVar2 + cVar3 * 8)] + 0x5d,iVar1 * 0xc + iVar2 * 4 + 0x198);
      iVar2 = iVar2 + 1;
    } while (iVar2 < 3);
    cgram_load_tile_block(iVar1 + 0x120,iVar1 * 4 + 0x250);
    cVar3 = W[0x4037 + (cVar3 * 8)];
    iVar1 = iVar1 + 1;
  } while (iVar1 < 10);
  cgram_load_tile_block(0x96,0x210);
  cgram_load_tile_block(0xa1,0x240);
  uVar7 = 0x82;
  cgram_load_tile_block(0xa2,0x24c);
  /* ROM 0x034482..0x03449E: the palette index is a BYTE push (`move.b #$1/$5/$7,-(a7)`),
     which the decompiler lost (register row 118's class) and left as 0. */
  (void)uVar4; (void)uVar5; (void)uVar6; (void)uVar7;
  cz_load_color_ramp(0x29, 1);
  cz_load_color_ramp(0x120, 5);
  cz_load_color_ramp(0x121, 7);
  return;
}

/* ---- attract_terrain_flyover_init ---- */

void attract_terrain_flyover_init(void)

{
  sync_post();
  sound_reset_all();
  attract_tilemap_clear();
  attract_course_load(0);
  g_fog_mode = 3;
  W[0xEB16] = 0;
  W[0x1736C] = 0;
  cgram_load_tile_block(0x95,0x200);
  W[0x0CC4] = 0xd;
  attract_terrain_flyover_run();
  return;
}

/* ---- attract_terrain_flyover_run @ 0x031E4C ---- */
/* Attract playlist value 13: the three-course flyover ("CLIFF ROCK / WIND
 * WOODS / INDUSTARN", 256 frames each, the course's best score wiping in).
 * REWRITTEN FROM THE DISASSEMBLY (ROM 0x031E4C..0x032048). The transpile read
 * the course-boundary table at 0x39858 = [0, 256, 512, 768, MAX] ONE BYTE at
 * a time (`(&R[0x39858])[i]`, all zero), so the course search always ran off
 * the end: it loaded course 3 on the first frame, never matched the exit test
 * `tbl[d3+1] - ctr - 1 == 0`, and ATTRACT FROZE ON THIS SCREEN (measured:
 * phase 13 from our f5949 for the rest of a 20000-frame run, where MAME
 * leaves it after 766 frames). It also had `*(int *)(iVar4*4 + 0x39854)`, a
 * host dereference of a ROM offset, the text calls with base/palette lost
 * (`text_draw_rect_blink(0x95,3,3,0,0)`, register row 62's class) and 16-bit
 * casts on the terrain_chunk_visibility arguments the ROM pushes as longs.
 *
 * capstone prints two of the EAs here without their scale (brief extension
 * words): `-$4(a2,d2.l)` is 0x2CFC -> d2.l*4, and `$4(a2,d3.l)` is 0x3C04
 * -> d3.l*4. Both decoded by hand. */
int attract_terrain_flyover_run(void)

{
  int32_t ctr  = (int32_t)W[0x1736C];
  int32_t ctr3 = ctr - 3;                               /* d4 */
  int     d2, d3, i;
  int32_t lift;                                         /* d5 */
  int     ret = 0;

  for (d2 = 0; d2 < 4; d2++)                            /* 0x031E72 */
    if (vrd32s(0x39858 + d2 * 4) > ctr) break;
  d3 = d2 - 1;                                          /* the course on screen */
  if (d3 != (int32_t)W[0x0E0C])
    attract_course_load(d3);

  for (d2 = 0; d2 < 5; d2++)                            /* 0x031E98 */
    if (ctr3 < vrd32s(0x39858 + d2 * 4)) break;
  if (d2 > 0) {
    int32_t since = ctr3 - vrd32s(0x39858 + (d2 - 1) * 4) - 0x40;
    if (since < 0x40)                                   /* 0x031EB6: the best-score wipe */
      attract_highscore_overlay_draw(since);
    if (vrd32s(0x39858 + d2 * 4) - ctr3 - 1 == 0)       /* last frame of the course */
      attract_tilemap_clear();
  }
  if (ctr3 >= 0)                                        /* 0x031EE2 */
    text_draw_rect_blink(0x95, 3, 3, 0x200, 1);

  /* 0x031EFE: fade in over 32 frames, out over the last 64 */
  {
    int32_t lo = vrd32s(0x39858 + d3 * 4), hi = vrd32s(0x3985C + d3 * 4);
    int32_t f;
    if (ctr - lo < 0x20)       f = (lo - ctr + 0x1f) << 3;
    else if (hi - ctr < 0x40)  f = (ctr - hi + 0x3f) << 2;
    else                       f = 0;
    W[0xEB16] = (int16_t)f;                             /* move.w d0,$e0eb16 */
    lift = (ctr - lo < 0x40) ? 0x400 - ((ctr - lo) << 4) : 0;
    lift += (int32_t)0xffffff00;                        /* addi.l #$ffffff00 */
  }

  for (i = 0; i < 9; i++)                               /* 0x031F62: the course name */
    text_draw_rect_blink(0x5d, 0x12 + i * 2, 0x10, 0x120 + i * 4, 1);
  for (i = 0; i < 3; i++)                               /* 0x031F92: the name's initials */
    text_draw_rect_blink(0x5d, 0x10 + i * 2, 0xd, 0x150 + i * 4, 1);

  camera_grid_calc_position();                          /* 0x006EB4 */
  terrain_chunk_visibility(W[0x0CDC], W[0x0CE0], W[0x0CE4], W[0x0CEC]);
  terrain_props_dispatch();                             /* 0x004F22 */
  objects_render_master();                              /* 0x014524 */
  world_highscore_3d_draw();                            /* 0x034236 */
  if ((int32_t)W[0x0E0C] - 3 < 0)                       /* 0x031FF2: the course's START deck */
    dsp_cmd_place_object_abs(2, 0x352 + (int32_t)W[0x0E0C] * 2, lift, 0xa0, 0x350);
  W[0x1736C] = ctr + 1;
  W[0x0C8C] = (int32_t)W[0x0C8C] + 1;
  if (W[0x0CBC] != 5 && d3 == 2 &&                      /* 0x032024 */
      vrd32s(0x3985C + d3 * 4) - (ctr + 1) - 1 == 0)
    ret = 1, attract_advance_sub_state();
  return ret;
}

/* ---- attract_text_stage_labels ---- */

static int g_stage_lbl;   /* attract_text_stage_labels' stage (row 84) */

void attract_text_stage_labels(void)

{
  int iVar1;
  uint32_t uVar2;
  /* void */;
  int iVar3;
  undefined2 uVar4;
  undefined2 uVar5;
  undefined2 uVar6;
  
  iVar1 = W[0x15EB8] + -3;
  if (iVar1 < 0) {
    return;
  }
  /* THE STAGE VARIABLE, lost the same way `attract_cinematic_tick`'s was
   * (register row 24): Ghidra dropped `// 0 = iVar3;` and then folded every
   * use of it to the constant 0, so this ran as "stage 0, for ever".
   *
   * From the M68K at 0x00D2F0:
   *     moveq #0,d5
   *   loop: movea.l d7,a0            ; d7 = 0x35320
   *         cmp.l (a0,d5.l*4),d6     ; BE32 on a 4-byte stride
   *         blt skip
   *         move.l d5,d2             ; <-- stage = i, the lost line
   *   skip: addq.l #1,d5 ; moveq #$c,d0 ; cmp.l d0,d5 ; blt loop
   *     tst.l d2 ; bpl ; moveq #0,d2
   * i.e. TWELVE entries, read BIG-ENDIAN 32-bit -- `(&R[0x35320])[i]` indexes
   * a uint8_t[] and returns ONE BYTE. The second table at 0x35364 is indexed
   * the same way (`move.l $35364(d2.l*4),d4` at 0x00D31C). */
  { int stage_ = 0;
    for (iVar3 = 0; iVar3 < 0xc; iVar3++)
      if ((int32_t)vrd32(0x35320 + iVar3 * 4) <= iVar1) stage_ = iVar3;
    if (stage_ < 0) stage_ = 0;
    g_stage_lbl = stage_; }
  uVar2 = (uint32_t)(iVar1 - (int32_t)vrd32(0x35320 + g_stage_lbl * 4));
  iVar1 = (int32_t)vrd32(0x35364 + g_stage_lbl * 4);
  if ((int)uVar2 < 0x10) {
    if (iVar1 < 1) {
      text_draw_rect_solid(0,1,0x19);
    }
    else {
      text_draw_rect_blink(0, 1, 0x19, 0x120, 1);
      text_draw_number(iVar1,0x1a0,uVar2);
      if (g_stage_lbl == 3) {
        text_draw_number(0xf8,0x220,uVar2);
      }
      else if ((g_stage_lbl == 5) || (g_stage_lbl == 6)) {
        text_draw_number(0xf7,0x220,uVar2);
      }
    }
    if (g_stage_lbl != 4) {
      if (g_stage_lbl != 6) {
        if (g_stage_lbl != 7) {
          return;
        }
LAB_0000d3ce:
        text_draw_rect_solid(0xf7,0x14,7);
        return;
      }
LAB_0000d3b2:
      text_draw_rect_solid(0xf7,9,6);
      return;
    }
LAB_0000d396:
    text_draw_rect_solid(0xf8,0x18,5);
  }
  else {
    if (iVar1 < 1) {
      text_draw_rect_solid(0,1,0x19);
    }
    else {
      /* ROM 0x00D3E8: `pea $1 ; move.w #$1a0` -- base tile 0x1A0, palette 1.
       * Ghidra lost both and passed 0, which drew tiles 0x000.. -- the whole
       * character set -- across the bottom of the attract demo. */
      text_draw_rect_blink((short)iVar1,1,0x19, 0x1a0, 1);
    }
    if (g_stage_lbl == 3) {
      if ((uVar2 & 0x10) != 0) goto LAB_0000d396;
      uVar6 = 5;
      uVar5 = 0x18;
      uVar4 = 0xf8;
    }
    else {
      if (g_stage_lbl == 5) {
        if ((uVar2 & 0x10) != 0) goto LAB_0000d3b2;
        uVar6 = 6;
        uVar5 = 9;
      }
      else {
        if (g_stage_lbl != 6) {
          return;
        }
        if ((uVar2 & 0x10) != 0) goto LAB_0000d3ce;
        uVar6 = 7;
        uVar5 = 0x14;
      }
      uVar4 = 0xf7;
    }
    /* ROM 0x00D420/0x00D444/0x00D462: every branch pushes `pea $5 ; move.w
     * #$220` -- base tile 0x220, palette 5 (lost in decompilation). */
    text_draw_rect_blink(uVar4,uVar5,uVar6, 0x220, 5);
  }
  return;
}

/* ---- attract_tile_reveal_effect ---- */

void attract_tile_reveal_effect(int param_1,int param_2,int param_3)

{
  int iVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  int iVar5;
  int iVar6;
  
  /* The index arithmetic below matched ROM 0x032178..0x032234; the MEMORY
   * accesses did not. The ROM works in 16-bit words on both sides:
   *   a3 = 0x880000 + p2*0x80                (cgram row, BYTES)
   *   a1 = 0x238504 + srcidx*0x80            (`lsl.l #6 ; add.l d0,d0`)
   *   move.w (a1,d6.l*2),d0 ; and.w/or.w masks ; move.w d0,(a3,d6.l*2)
   * The transpile indexed `g_sys.cgram` (a uint8_t[], stored big-endian --
   * see cgram_load_tile_block) at `p2*0x40 + d6` and read ONE BYTE of the
   * source word, so every wipe landed at half its row with half its data:
   * register row 57's class. Only reached once the attract terrain flyover's
   * threshold table is read correctly (attract_terrain_flyover_run). */
  iVar1 = (int32_t)vrd32(0x1C4F40 + (param_1 * 0x10));
  for (iVar4 = 0; iVar4 < param_3; iVar4 = iVar4 + 1) {
    iVar6 = param_3 - iVar4;
    if ((iVar4 < 0x20) && (iVar6 < 0x20)) {
      uint32_t src, dst;
      uint16_t v;
      iVar2 = iVar4;
      if (iVar4 < 0) {
        iVar2 = iVar4 + 0xf;
      }
      iVar3 = iVar6;
      if (iVar6 < 0) {
        iVar3 = iVar6 + 0xf;
      }
      iVar5 = iVar6 % 4;
      if (iVar6 < 0) {
        iVar6 = iVar6 + 3;
      }
      iVar6 = (iVar6 >> 2) % 4 + ((iVar3 >> 4) + (iVar2 >> 4) * 2) * 0x40 + (iVar4 % 0x10) * 4;
      src = 0x238504 + (uint32_t)iVar1 * 0x80 + (uint32_t)iVar6 * 2;
      dst = (uint32_t)param_2 * 0x80 + (uint32_t)iVar6 * 2;
      if (iVar5 < 0 || dst + 2 > CGRAM_SIZE) continue;
      v = (uint16_t)((vrd16(src) & vrd16(0x398DC + iVar5 * 2)) | vrd16(0x398E4 + iVar5 * 2));
      g_sys.cgram[dst]     = (uint8_t)(v >> 8);
      g_sys.cgram[dst + 1] = (uint8_t)v;
    }
  }
  return;
}

/* ---- attract_tile_sparkle_effect ---- */

void attract_tile_sparkle_effect(int param_1)

{
  int iVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  int iVar5;
  int iVar6;
  undefined2 *puVar7;
  
  if (-1 < param_1) {
    puVar7 = &g_sys.cgram[0x9000];
    iVar6 = 0;
    do {
      for (iVar5 = 0; iVar5 < param_1; iVar5 = iVar5 + 1) {
        iVar4 = param_1 - iVar5;
        if ((iVar5 < 0x20) && (iVar4 < 0x20)) {
          iVar1 = iVar5;
          if (iVar5 < 0) {
            iVar1 = iVar5 + 0xf;
          }
          iVar3 = iVar4;
          if (iVar4 < 0) {
            iVar3 = iVar4 + 0xf;
          }
          iVar2 = iVar4 % 4;
          if (iVar4 < 0) {
            iVar4 = iVar4 + 3;
          }
          puVar7[(iVar4 >> 2) % 4 + ((iVar3 >> 4) + (iVar1 >> 4) * 2) * 0x40 + (iVar5 % 0x10) * 4] =
               (vrd16s(0x398EC + (iVar2 * 2))) |
               puVar7[(iVar4 >> 2) % 4 +
                      ((iVar3 >> 4) + (iVar1 >> 4) * 2) * 0x40 + (iVar5 % 0x10) * 4];
        }
      }
      puVar7 = puVar7 + 0x100;
      iVar6 = iVar6 + 1;
    } while (iVar6 < 5);
  }
  return;
}

/* ---- attract_tilemap_clear ---- */

void attract_tilemap_clear(void)

{
  int iVar1;
  int iVar2;
  
  /* ROM 0x0322FA: `move.w #$ffff,(0x889000, row*0x80 + col*2)` for 64 rows x 64 WORDS --
     0x2000 bytes. g_sys.cgram is uint8_t[], so the transpile's byte store at
     row*0x40 + col set only the first half, one byte per word. */
  (void)iVar1; (void)iVar2;
  memset(&g_sys.cgram[0x9000], 0xff, 0x2000);
  cz_load_color_ramp(0x29, 1);   /* ROM 0x032326 `move.b #$1,-(a7)` -- palette byte lost */
  return;
}

/* ---- attract_advance_sub_state @ 0x00C25C ---- */

void attract_advance_sub_state(void)
{
  W[0x0CAC] = 0;
  g_sub_state_max = (g_sub_state_max + 1) % 0xf;
  /* Real 68020 (dasm @0xC278): the sign-extended table word IS the case
   * index — no ">> 1" (see state_attract_init). */
  uint32_t tbl_off = 0x3510C + (uint32_t)g_sub_state_max * 2;
  g_sub_state_attract = (int16_t)((g_sys.rom[tbl_off] << 8) | g_sys.rom[tbl_off + 1]);
  return;
}





/* ---- attract_overview_run @ 0x0338FA ---- */
/* Attract playlist value 11: the RANKING screen -- a 720-frame shot of the
 * town overview with the camera swinging in, and the top-ten table revealed
 * in two pages (ranks 6-10, then 1-5). REWRITTEN FROM THE DISASSEMBLY
 * (ROM 0x0338FA..0x034234); the transpile was unusable in four ways:
 *
 *  1. THE PAGE DISPATCH WAS A STUB THAT RETURNED EARLY. ROM 0x033D08:
 *       move.l -$1c(a6),d0 ; subq.l #1,d0 ; bhi 0x0340A8
 *       move.w $33d1e(pc,d0.w*2),d0 ; jmp $33d1c(pc,d0.w)
 *     -$1c(a6) is the PAGE (0/1/2) of the counter minus 3. subq/bhi sends
 *     page 2 to 0x0340A8 and lets pages 0 (d0 = -1) and 1 (d0 = 0) through
 *     to a two-entry table at 0x33D1C = [0x0004, 0x01FC]: page 0 -> 0x033D20
 *     (ranks 6-10), page 1 -> 0x033F18 (ranks 1-5). Ghidra could not follow
 *     it and emitted `if (page == 0 || page == 1) { dispatch stub; return; }`
 *     -- which also skipped the COMMON TAIL at 0x0340B8, including the
 *     `W[0x1736C] += 1` that is the screen's only clock. The counter stayed
 *     at 0 forever and attract froze on this screen until START.
 *  2. Every threshold table was read ONE BYTE at a time. 0x39D38, 0x39D4C,
 *     0x39D60 and 0x39D74 are five BE32 entries each, read
 *     `cmp.l $39d4c(d2.l*4),d0` (0x0339A4), i.e. [0,240,480,720,MAX],
 *     [0,120,600,720,MAX], [0,300,540,720,MAX] and [0,420,600,720,MAX];
 *     `(&g_sys.rom[0x39D4C])[i]` gave [0,0,0,0,0], so every segment search
 *     landed on the sentinel and the camera sat in its final pose.
 *  3. `W[0x173B8] = &g_sys.rom[0x01200]` -- a host pointer where ROM
 *     0x033AA4 has the immediate `move.l #$1200,$e173b8.l`.
 *  4. The tail's 3x3 terrain grid put Z at `iVar5 * -0x8000 - 0x4000`; ROM
 *     0x034126..0x034136 is `d2 * 0x18000 + 0xFC000 - W[0x0CE4]`, and every
 *     coordinate was cut to 16 bits by `(short)` casts the ROM does not have
 *     (it pushes longs to terrain_chunk_visibility and dsp_cmd_set_camera).
 *
 * Also note the page-0 rank labels use W[0x0E4C] -- the SCORE -- as their
 * scratch digit (ROM 0x033E82 `move.l d0,$e00e4c.l`), so this screen leaves
 * it at 1. That is the machine's behaviour and is kept. */

/* ROM 0x0339A0-style segment search: the LAST index whose threshold is
 * <= ctr, starting from `init` (whatever the ROM leaves in d3). */
static int ov_segment(uint32_t tbl, int32_t ctr, int init)
{
  int seg = init, i;
  for (i = 0; i < 5; i++)
    if (ctr >= vrd32s(tbl + (uint32_t)i * 4)) seg = i;
  return seg;
}

/* `asr.l #1 ; andi.l #$7ffe ; movea.w $20b006(d0.l*2)` -- the cos lane. */
static int32_t ov_cos(int32_t t)
{
  return vrd16s(0x20B006 + (uint32_t)((t >> 1) & 0x7ffe) * 2);
}

/* Append n longs to the DSP display list at W[0x0CA4], bounds-checked like
 * dsp_cmd_ptr() (register row 19): drop the words rather than write outside
 * dspram. */
static void ov_dsp_emit(const int32_t *w, int n)
{
  int32_t *p = (int32_t *)W[0x0CA4];
  uint8_t *lo = g_sys.dspram, *hi = g_sys.dspram + DSPRAM_SIZE;
  int i;
  if ((uint8_t *)p < lo || (uint8_t *)(p + n) > hi) return;
  for (i = 0; i < n; i++) p[i] = w[i];
  W[0x0CA4] = (intptr_t)(p + n);
}

/* The high-score list at WRAM 0xE04030: 10 entries of 8 bytes, a long
 * `rank << 24 | score` then three name bytes and a NEXT-index byte at +7;
 * the head index is the byte at 0xE040A8. The C side keeps the byte fields
 * one per `_W[]` slot (highscore_table_reset_defaults, the EEPROM copy and
 * attract_score_sprites_load all do), so they are read the same way here. */
static int ov_hs_next(int e)
{
  if (e < 0 || e > 9) return 0;          /* host guard: the ROM trusts the link */
  e = (int8_t)W[0x4037 + e * 8];
  return (e < 0 || e > 9) ? 0 : e;
}

/* One five-row block of the table (ROM 0x033D46..0x033E3A for ranks 6-10,
 * 0x033F3A..0x03402E for ranks 1-5 -- the same code twice). */
static void ov_draw_rank_rows(int e)
{
  int row, k;
  for (row = 0; row < 5; row++) {
    int32_t ent   = (int32_t)W[0x4030 + e * 8];
    int32_t score = ent & 0xffffff;
    int32_t rank  = ent >> 0x18;              /* asr.l #24 */
    int     r     = row * 3 + 8;
    for (k = 0; k < 5; k++) {                 /* five score digits, right to left */
      text_draw_rect_blink(0x5d, 0x1b - k * 2, r, (uint16_t)(0x160 + (score % 10) * 4), 1);
      score /= 10;
    }
    text_draw_rect_blink(0xa2, 0x1d, r, 0x24c, 1);
    if (rank != 0x7f)                         /* 0x7F = no stage reached */
      text_draw_rect_blink(rank + 0x120, 0x20, r, (uint16_t)(0x250 + rank * 4),
                           rank != 0 ? 7 : 5);
    e = ov_hs_next(e);
  }
}

void attract_overview_run(void)

{
  int32_t ctr  = (int32_t)W[0x1736C];
  int32_t ctr3 = ctr - 3;                               /* -$24(a6) */
  int32_t m10  = vrd32s(0x39D54) - ctr;                 /* -$10(a6) */
  int32_t mC   = vrd32s(0x39D68) - ctr;                 /* -$c(a6)  */
  int page  = ctr  < 0xf0 ? 0 : (ctr  < 0x1e0 ? 1 : 2); /* d3 */
  int page3 = ctr3 < 0xf0 ? 0 : (ctr3 < 0x1e0 ? 1 : 2); /* -$1c(a6) */
  int seg, row, k;
  int32_t d5, rem, t, d0;

  /* ---- 0x0339A0: the orbit RADIUS, W[0x173B0] ---- */
  seg = ov_segment(0x39D4C, ctr, page);
  d5  = ctr - vrd32s(0x39D4C + seg * 4);
  rem = vrd32s(0x39D50 + seg * 4) - ctr;
  if (seg == 0) {
    W[0x173B0] = (int32_t)(0x1c0 * m10 + 0x370);
  } else if (seg == 1) {
    t  = (d5 << 15) / (d5 + rem);
    d0 = (ov_cos(t) + 0x8000) * 0x1c0 / 0xffff;         /* adda.l #$8000 ; *7 <<6 ; divs.l #$ffff */
    d0 = d0 * m10 - (int32_t)W[0x173B0] + 0x370;
    if (d0 < 0) d0 += 3;
    W[0x173B0] = (int32_t)((int32_t)W[0x173B0] + (d0 >> 2));
  } else {
    W[0x173B0] = 0x370;
  }

  /* ---- 0x033A70: the HEADING offset, W[0x173B8] ---- */
  seg = ov_segment(0x39D38, ctr, seg);
  d5  = ctr - vrd32s(0x39D38 + seg * 4);
  rem = vrd32s(0x39D3C + seg * 4) - ctr;
  if (seg == 1) {
    t = (d5 << 15) / (d5 + rem) - 0x8000;               /* addi.l #$ffff8000 */
    W[0x173B8] = ov_cos(t) + 0x9200;
  } else {
    W[0x173B8] = 0x1200;                                /* immediate, ROM 0x033AA4 / 0x033AF2 */
  }

  /* ---- 0x033AFC: the camera HEIGHT, W[0x173BC] ---- */
  seg = ov_segment(0x39D60, ctr, seg);
  d5  = ctr - vrd32s(0x39D60 + seg * 4);
  rem = vrd32s(0x39D64 + seg * 4) - ctr;
  if (seg == 0) {
    W[0x173BC] = (int32_t)(0x60 * mC + 0x15b20);
  } else if (seg == 1) {
    t  = (d5 << 15) / (d5 + rem);
    d0 = (ov_cos(t) * 0x60) >> 15;
    d0 += 0x60;
    if (d0 < 0) d0 += 1;
    d0 >>= 1;
    d0 = d0 * mC - (int32_t)W[0x173BC] + 0x15b20;
    if (d0 < 0) d0 += 3;
    W[0x173BC] = (int32_t)((int32_t)W[0x173BC] + (d0 >> 2));
  } else {
    W[0x173BC] = 0x15b20;
  }

  /* ---- 0x033BBC: the camera PITCH, W[0x173B4] ---- */
  seg = ov_segment(0x39D74, ctr, seg);
  d5  = ctr - vrd32s(0x39D74 + seg * 4);
  rem = vrd32s(0x39D78 + seg * 4) - ctr;
  if (seg == 0) {
    W[0x173B4] = 0;
  } else if (seg == 1) {
    uint32_t p;
    t = d5 * 0x7fff / (d5 + rem);                       /* muls.l #$7fff, not lsl #15 */
    /* muls.w #$e700,d1 ; lsr.l #15,d1 ; d0 = $e700.w - d1 ; lsr.l #1,d0 --
     * both shifts LOGICAL, as the ROM has them. Only the low 16 bits reach
     * the trig tables, where logical and arithmetic agree. */
    p = (uint32_t)((int32_t)ov_cos(t) * (int32_t)(int16_t)0xe700) >> 15;
    W[0x173B4] = (int32_t)(((uint32_t)(int32_t)-0x1900 - p) >> 1);
  } else {
    W[0x173B4] = (int32_t)0xffffe700;
  }

  /* ---- 0x033C50: aim the camera ---- */
  W[0x0CE0] = W[0x173BC];
  W[0x0CE8] = W[0x173B4];
  W[0x0CEC] = (int32_t)-(int32_t)W[0x173B8];
  {
    uint32_t idx = (((uint32_t)-(int32_t)W[0x0CEC] >> 1) & 0x7ffe) * 2;   /* lsr.l #1 */
    int32_t  r   = (int32_t)W[0x173B0];
    W[0x0CE4] = (int32_t)(0x11b1fb - (((int32_t)vrd16s(0x20B006 + idx) >> 2) * r >> 0xd));
    W[0x0CDC] = (int32_t)(0x3e560  - (((int32_t)vrd16s(0x20B004 + idx) >> 2) * r >> 0xd));
  }

  if (ctr3 >= 0) {                                      /* 0x033CD2: the two title blocks */
    text_draw_rect_blink(0x96, 0xd, 2, 0x210, 1);
    text_draw_rect_blink(0xa1, 0x11, 5, 0x240, 1);
  }

  /* ---- 0x033D08: the page dispatch (see 1. above) ---- */
  if (page3 == 0) {                                     /* 0x033D20: ranks 6-10 */
    if (ctr3 >= 0) {
      int e = (int8_t)W[0x40A8];                        /* head byte, 0xE040A8 */
      if (e < 0 || e > 9) e = 0;
      for (k = 0; k < 5; k++)                           /* skip the first five */
        e = ov_hs_next(e);
      ov_draw_rank_rows(e);
      for (row = 0; row < 5; row++)                     /* 0x033E3C: the names */
        for (k = 0; k < 3; k++)
          text_draw_rect_blink(0x5d, 0xb + k * 2, row * 3 + 8,
                               0x1d4 + row * 0xc + k * 4, 1);
      for (row = 0; row < 5; row++) {                   /* 0x033E80: rank numbers 6..10 */
        W[0x0E4C] = 6 + row;
        text_draw_rect_blink(0x5d, 8, row * 3 + 8,
                             0x160 + ((int32_t)W[0x0E4C] % 10) * 4, 1);
        W[0x0E4C] = (int32_t)W[0x0E4C] / 10;
        if (W[0x0E4C] != 0)
          text_draw_rect_blink(0x5d, 6, row * 3 + 8,
                               0x160 + ((int32_t)W[0x0E4C] % 10) * 4, 1);
      }
    }
  } else if (page3 == 1) {                              /* 0x033F18: ranks 1-5 */
    if (ctr3 == 0xf0) sync_post();
    if (ctr3 > 0x10e) {
      int e = (int8_t)W[0x40A8];
      if (e < 0 || e > 9) e = 0;
      ov_draw_rank_rows(e);
      for (row = 0; row < 5; row++)                     /* 0x034030: the names */
        for (k = 0; k < 3; k++)
          text_draw_rect_blink(0x5d, 0xb + k * 2, row * 3 + 8,
                               0x198 + row * 0xc + k * 4, 1);
      for (row = 0; row < 5; row++)                     /* 0x034074: rank numbers 1..5 */
        text_draw_rect_blink(0x5d, 8, row * 3 + 8, 0x164 + row * 4, 1);
    }
  } else {                                              /* 0x0340A8 */
    if (ctr3 == 0x1e0) sync_post();
  }

  /* ---- 0x0340B8: the common tail ---- */
  camera_grid_calc_position();
  terrain_chunk_visibility(W[0x0CDC], W[0x0CE0], W[0x0CE4], W[0x0CEC]);
  {
    static const int32_t shift[4] = { 0x8010, 3, (int32_t)0xfffffd23, -1 };
    ov_dsp_emit(shift, 4);
  }
  for (row = 0; row < 3; row++)                         /* d2 = row, d4 = k */
    for (k = 0; k < 3; k++)
      dsp_cmd_set_camera(0, 0x666 + k + row * 8,
                         (int32_t)(k * 0x18000 + 0x24000 - (int32_t)W[0x0CDC]),
                         (int32_t)(-0x5023 - (int32_t)W[0x0CE0]),   /* lea $afdd.w = -0x5023 */
                         (int32_t)(row * 0x18000 + 0xfc000 - (int32_t)W[0x0CE4]),
                         0, 0, 0, 0);
  {
    static const int32_t endseg[2] = { 0x8010, -1 };
    ov_dsp_emit(endseg, 2);
  }
  dsp_cmd_set_camera(0, 0x1f4, (int32_t)-(int32_t)W[0x0CDC],
                     (int32_t)(-0x5023 - (int32_t)W[0x0CE0]),
                     (int32_t)-(int32_t)W[0x0CE4], 0, 0, 0, 0);
  ending_town_overview_draw();                          /* 0x02C6FC */
  attract_ground_plane_draw();                          /* 0x0344AA */
  objects_render_master();                              /* 0x014524 */
  world_highscore_3d_draw();                            /* 0x034236 */
  if (W[0x0CBC] != 5 && 0x2d0 - ctr < 0x80)             /* 0x0341F0: fade out */
    W[0xEB16] = (int16_t)((ctr - 0x251) << 1);          /* lsl.w #1 ; move.w */
  W[0x1736C] = ctr + 1;
  if (ctr + 1 > 0x2d0)
    attract_advance_sub_state();
  return;
}





/* ---- attract_tilemap_cmd_exec @ 0x01C936 ---- */
/* Draws a data-driven tilemap background pattern into g_sys.textram. The
 * M68K passes the command-stream ROM address in register A0 -- a
 * caller-supplied argument Ghidra could not recover ("Ghidra failed to
 * resolve the base pointer for this function", this project's row-82/83
 * NULL-substituted-implicit-register-parameter class). The one live call
 * site, attract_tilemap_init (ROM 0x01C0D4: `lea.l $1c0e4(pc),a0`), is
 * now threaded through explicitly as cmd_addr.
 *
 * Format at cmd_addr, all big-endian, verified by mechanically replaying
 * this ROM's actual data end to end (byte-for-byte, not eyeballed):
 *   [0] x, [1] y (16-bit each) -- dest tile column/row:
 *       dest_byte_offset = (int16_t)((x<<1) + (y<<7))  (ROM 0x01C93C-
 *       0x01C948: `add.w d0,d0 ; lsl.w #7,d1 ; add.w d1,d0`, then
 *       `adda.w d0,a1` off a1 = TEXTRAM_BASE -- 16-bit math, sign-extended
 *       on the final add exactly like the M68K's adda.w)
 *   [2] row stride in BYTES between consecutive output rows, also
 *       adda.w'd per row (0x80 = one full 64-word tilemap row in the one
 *       observed call)
 *   then repeated blocks, each:
 *     [word] row_count - 1 (a negative value terminates the whole stream,
 *            ROM 0x01C94C: `bmi.w ..`)
 *     [long] ROM address of the per-row command for
 *            attract_tilemap_cmd_dispatch -- read ONCE per block and
 *            reused UNCHANGED for every row in that block (ROM
 *            0x01C950/0x01C954: `movea.l (a0)+,a2` happens once, then
 *            `movea.l a2,a4` every inner dbra iteration), so one block
 *            paints row_count IDENTICAL rows, advancing dest by stride
 *            each time
 * The one live call (attract_tilemap_init, cmd_addr = 0x1C0E4) draws 28
 * rows (rows 1-28 of the tilemap, i.e. everything below the top row) as
 * four 7-row bands, each band opcode-1 (literal copy, 40 words/row) from
 * its own 0x1C1AC/0x1C158/0x1C200/0x1C104 source -- confirmed by a full
 * Python replay of this exact command stream against the merged ROM
 * before writing this port, not guessed from the shape of the code. */
static void attract_tilemap_cmd_exec(uint32_t cmd_addr)
{
  int16_t xw, yw, stride, off16, rows_m1;
  uint32_t dest, src;
  int i;

  xw = (int16_t)vrd16(cmd_addr); cmd_addr += 2;
  yw = (int16_t)vrd16(cmd_addr); cmd_addr += 2;
  stride = (int16_t)vrd16(cmd_addr); cmd_addr += 2;
  off16 = (int16_t)((xw << 1) + (yw << 7));
  dest = (uint32_t)((int32_t)TEXTRAM_BASE + off16);

  for (;;) {
    rows_m1 = (int16_t)vrd16(cmd_addr); cmd_addr += 2;
    if (rows_m1 < 0) return;
    src = vrd32(cmd_addr); cmd_addr += 4;
    for (i = 0; i <= rows_m1; i++) {
      attract_tilemap_cmd_dispatch(dest, src);
      dest = (uint32_t)((int32_t)dest + stride);
    }
  }
}





