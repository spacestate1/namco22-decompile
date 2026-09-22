/*
 * EEPROM & Settings
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

/* Forward declarations (called before definition in this file) */
uint16_t eeprom_read_status();
uint16_t eeprom_wait_ack();
uint16_t eeprom_sync_entry();

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

/* ---- eeprom_compare_block ---- */

short eeprom_compare_block(char *param_1,uint32_t param_2)

{
  short sVar1;
  short sVar2;
  char *pcVar3;

  /* Original: pcVar3 = (0x460000) + param_2 * 0x20 — raw EEPROM hardware address.
   * 0x460000 = EEPROM_BASE, so offset = param_2 * 0x20 into EEPROM. */
  pcVar3 = (char *)g_sys.eeprom + param_2 * 0x20;
  sVar1 = 0;
  sVar2 = 0;
  while( true ) {
    if (*pcVar3 != *param_1) {
      sVar1 = 1;
      goto LAB_00019d7e;
    }
    /* Original: pcVar3[0x2000] — second EEPROM bank at +0x2000 offset */
    if (pcVar3[0x2000] != ((int32_t*)(intptr_t)param_1)[1]) break;
    sVar2 = sVar2 + 1;
    pcVar3 = pcVar3 + 2;
    param_1 = param_1 + 2;
    if (0xf < sVar2) {
LAB_00019d7e:
      if (sVar1 != 0) {
        W[0x3F90 + (param_2 >> 3)] =
             (uint8_t)(0x80 >> (param_2 & 7)) | W[0x3F90 + (param_2 >> 3)];
      }
      return sVar1;
    }
  }
  sVar1 = 2;
  goto LAB_00019d7e;
}


/* ---- eeprom_dirty_bitmap_clear ---- */

void eeprom_dirty_bitmap_clear(void)

{
  short sVar1;
  undefined1 *puVar2;
  
  sVar1 = 0;
  puVar2 = &W[0x3F90];
  do {
    *puVar2 = 0;
    sVar1 = sVar1 + 1;
    puVar2 = puVar2 + 1;
  } while (sVar1 < 0x20);
  return;
}


/* ---- eeprom_dump_and_halt ---- */

void eeprom_dump_and_halt(void)

{
  uint16_t uVar1;
  int iVar2;
  
  sync_post();
  uVar1 = 0;
  do {
    iVar2 = (int)(short)uVar1;
    if (iVar2 < 0) {
      iVar2 = iVar2 + 0xf;
    }
    text_draw_hex_digits((uVar1 & 0xf) * 2 + 2,(iVar2 >> 4) + 0xe,2,W[0x3F38 + ((short)uVar1)], 0);
    uVar1 = uVar1 + 1;
  } while ((short)uVar1 < 0x58);
  for (uVar1 = 0; uVar1 < 0x20; uVar1 = uVar1 + 1) {
    iVar2 = (int)(short)uVar1;
    if (iVar2 < 0) {
      iVar2 = iVar2 + 0xf;
    }
    text_draw_hex_digits((uVar1 & 0xf) * 2 + 2,(iVar2 >> 4) + 0x14,2,W[0x3F90 + ((short)uVar1)], 0);
  }
  eeprom_fatal_error(0xa4a4);
  return;
}


/* ---- eeprom_erase_slot ---- */

void eeprom_erase_slot(int param_1)

{
  short sVar1;
  undefined1 *puVar2;

  /* Original: puVar2 = (0x460000) + param_1 * 0x20 — raw EEPROM address */
  puVar2 = (undefined1 *)g_sys.eeprom + param_1 * 0x20;
  sVar1 = 0;
  do {
    puVar2[0x2000] = 0xff;
    *puVar2 = 0xff;
    sVar1 = sVar1 + 1;
    puVar2 = puVar2 + 2;
  } while (sVar1 < 0x10);
  W[0x4174] = W[0x4174] | 0x10;
  return;
}


/* ---- eeprom_find_free_slot ---- */

uint32_t eeprom_find_free_slot(void)

{
  uint32_t uVar1;
  uint16_t uVar2;
  uint16_t uVar3;
  uint16_t uVar4;
  uint8_t *pbVar5;
  uint8_t *pbVar6;
  
  pbVar5 = &W[0x3F90];
  uVar1 = 0;
  do {
    if (*pbVar5 == 0xff) {
      uVar1 = uVar1 + 8;
    }
    else {
      uVar3 = 0x80;
      uVar4 = 0;
      do {
        if ((uVar3 & *pbVar5) == 0) {
          uVar2 = 0;
          for (pbVar6 = &W[0x3F38]; (uVar2 < 0x58 && (*pbVar6 != uVar1)); pbVar6 = pbVar6 + 1) {
            uVar2 = uVar2 + 1;
          }
          if (uVar2 == 0x58) {
            return uVar1;
          }
        }
        uVar4 = uVar4 + 1;
        uVar1 = uVar1 + 1;
        uVar3 = uVar3 >> 1;
      } while (uVar4 < 8);
    }
    pbVar5 = pbVar5 + 1;
    if (0xff < uVar1) {
      if ((W[0x4174] & 6) == 0) {
        W[0x4174] = W[0x4174] | 4;
        eeprom_dirty_bitmap_clear();
        uVar1 = eeprom_find_free_slot();
      }
      else {
        uVar1 = 0x100;
      }
      return uVar1;
    }
  } while( true );
}


/* ---- eeprom_flush_pending ---- */

undefined2 eeprom_flush_pending(void)

{
  /* Stubbed: eeprom_wait_write_cycle polls hardware timing registers
   * (0x8A000A/0x8A000E) that never change in reimplementation → hangs. */
  W[0x4174] = W[0x4174] & 0xffe7;
  return 0;
}


/* ---- eeprom_write_verify_block ---- */

char * eeprom_write_verify_block(char *param_1,int param_2)

{
  char cVar1;
  short sVar2;
  undefined1 *puVar3;
  char *pcVar4;
  char *pcVar5;
  char *pcVar6;
  char *pcVar7;
  
  /* Original: pcVar6 = (0x460000) + param_2 * 0x20 — raw EEPROM address */
  pcVar6 = (char *)g_sys.eeprom + param_2 * 0x20;
  puVar3 = &W[0x168CC];
  sVar2 = 0;
  pcVar7 = pcVar6;
  do {
    pcVar5 = param_1;
    pcVar4 = pcVar7;
    if (*pcVar6 == *pcVar5) {
      *puVar3 = 0;
    }
    else {
      *puVar3 = 1;
      W[0x4174] = W[0x4174] | 8;
    }
    cVar1 = pcVar6[0x2000];
    if (cVar1 == pcVar5[1]) {
      puVar3[1] = 0;
    }
    else {
      puVar3[1] = 1;
      W[0x4174] = W[0x4174] | 8;
    }
    puVar3 = puVar3 + 2;
    param_1 = pcVar5 + 2;
    pcVar6 = pcVar6 + 2;
    sVar2 = sVar2 + 1;
    pcVar7 = (char *)(intptr_t)(((uint32_t)(((int32_t)((uint32_t)pcVar4 >> 8)) << 8) | (uint8_t)(cVar1)));
  } while (sVar2 < 0x10);
  pcVar6 = (char *)(CONCAT22((short)((uint32_t)pcVar4 >> 0x10),W[0x4174]) & 0xffff0008);
  if ((W[0x4174] & 8) != 0) {
    eeprom_wait_write_cycle(0x184);
    param_1 = pcVar5 + -0x1e;
    pcVar6 = (char *)g_sys.eeprom + param_2 * 0x20;
    pcVar4 = &W[0x168CC];
    sVar2 = 0;
    pcVar7 = pcVar6;
    do {
      pcVar5 = pcVar4 + 1;
      if (*pcVar4 != '\0') {
        *pcVar7 = *param_1;
      }
      pcVar4 = pcVar4 + 2;
      if (*pcVar5 != '\0') {
        pcVar7[0x2000] = ((int32_t*)(intptr_t)param_1)[1];
      }
      param_1 = param_1 + 2;
      pcVar7 = pcVar7 + 2;
      sVar2 = sVar2 + 1;
    } while (sVar2 < 0x10);
  }
  sVar2 = 0;
  pcVar7 = &W[0x168CC];
  pcVar4 = param_1 + -0x20;
  do {
    *pcVar7 = *pcVar4;
    sVar2 = sVar2 + 1;
    pcVar7 = pcVar7 + 1;
    pcVar4 = pcVar4 + 1;
  } while (sVar2 < 0x20);
  return pcVar6;
}


/* ---- eeprom_fatal_error ---- */

void eeprom_fatal_error(undefined2 param_1)

{
  sync_reset();
  tilemap_enable_set();
  text_print_string(6, 0xc, param_1, 0);
  /* infinite loop removed - return instead */
  return;
}

/* ---- eeprom_wait_write_cycle ---- */

void eeprom_wait_write_cycle(short param_1)

{
  uint16_t uVar1;
  short sVar2;
  short sVar3;
  short sVar4;
  short sVar5;
  
  sVar4 = mem_read32(0x8A000E);
  sVar2 = mem_read32(0x8A000A);
  uVar1 = mem_read32(0x8A000A);
  if ((uint32_t)uVar1 != (int)sVar2) {
    sVar2 = mem_read32(0x8A000A);
  }
  if (sVar2 < 10) {
    sVar4 = mem_read32(0x8A000E);
  }
  sVar5 = 0;
  do {
    uVar1 = mem_read32(0x8A000E);
    if ((uint32_t)uVar1 != (int)sVar4) {
      sVar5 = sVar4 * 2 + 0x20b;
    }
    sVar3 = mem_read32(0x8A000A);
    uVar1 = mem_read32(0x8A000A);
    if ((uint32_t)uVar1 != (int)sVar3) {
      sVar3 = mem_read32(0x8A000A);
    }
  } while ((int)sVar3 + (int)sVar5 < (int)(short)(param_1 + sVar2));
  g_sys.syscon[0x14] = 0;
  keycus_write_1();
  return;
}

/* ---- dipswitch_display_update ---- */

void dipswitch_display_update(void)

{
  uint16_t uVar1;
  uint16_t uVar2;
  uint8_t bVar3;
  uint16_t *puVar4;
  
  bVar3 = 1;
  uVar2 = 1;
  puVar4 = &g_sys.textram[0x0322];
  do {
    if ((bVar3 & W[0x2C0C]) == 0) {
      uVar1 = 0;
    }
    else {
      uVar1 = 0x2000;
    }
    tram_w16(puVar4, uVar2 | uVar1);
    uVar2 = uVar2 + 1;
    bVar3 = bVar3 << 1;
    puVar4 = puVar4 + 1;
  } while ((short)uVar2 < 9);
  return;
}

/* ---- settings_ranking_defaults ---- */

void settings_ranking_defaults(void)

{
  /* BE16 defaults, not one byte each (the same table row 61 fixed at
   * 0x36726: `move.w $36806.l,D0`). 0x36806 is the PLAY-TIME setting index
   * into the 90/80/70-second table at 0x15C448 -- factory default 1 = 80 s,
   * which is what MAME's timer starts from (fc sub3+360: W[0x0E48] = 4440 =
   * 74 s). One byte of it read 0 and gave every level 90 s. */
  W[0x3FFE] = 1;
  W[0x3FFA] = vrd16(0x36806);
  W16_SET(0x3FF8, vrd16(0x3681E));
  W[0x3FFC] = vrd16(0x3684E);
  return;
}

/* ---- settings_score_table_reset ---- */

void settings_score_table_reset(void)

{
  highscore_table_reset_defaults();
  return;
}

/* ---- eeprom_factory_reset ---- */

void eeprom_factory_reset(void)

{
  int iVar1;
  undefined2 local_14 [8];
  
  iVar1 = 0;
  do {
    local_14[iVar1] = W[0x3FD0 + (iVar1)];
    iVar1 = iVar1 + 1;
  } while (iVar1 < 8);
  eeprom_ram_clear();
  eeprom_ram_init_defaults();
  settings_audio_defaults();
  settings_time_limit_defaults();
  settings_ranking_defaults();
  settings_score_table_reset();
  game_stats_reset();
  highscore_table_reset_defaults();
  iVar1 = 0;
  do {
    W[0x3FD0 + (iVar1)] = local_14[iVar1];
    iVar1 = iVar1 + 1;
  } while (iVar1 < 8);
  eeprom_sync_all();
  return;
}

/* ---- eeprom_sync_all ---- */

static void hiscore_save(void);

void eeprom_sync_all(void)

{
  /* ROM 0x019EEE marks ALL 0x12 32-byte EEPROM blocks dirty (`jsr $19dd2` for
   * d2 = 0..0x11), the ranking block among them. game_stats_accumulate calls
   * it at the end of every game (0x021044, the tail of GAME OVER), and that
   * is the ONLY way a NOVICE result reaches the EEPROM: the NOVICE insert at
   * 0x02A446 updates the bests at 0xE04088 and never calls
   * eeprom_write_block. So the ranking file is flushed here too. */
  hiscore_save();
  STUB_HIT(0x019EEE, "EEPROM sync -- free play is forced");
  /* DELIBERATE OVERRIDE, not a gap: 0x019EEE is real M68K code (first
   * word 0x2F02), verified against pr2ver-a.*.
   * Stubbed: EEPROM hardware not present in reimplementation.
   * The sync_entry retry loop hangs because eeprom_compare_block
   * dereferences raw hardware address 0x460000 and
   * eeprom_wait_write_cycle polls hardware timing registers. */
  return;
}

/* ---- eeprom_write_block ---- */

/* THE RANKING IS SAVED TO A FILE (2026-09-22).
 *
 * The machine keeps its high scores in the EEPROM block mirrored at
 * 0xE03F30: the ADVANCED top ten at 0xE04030 (10 x 8 bytes: a long
 * `rank << 24 | score`, three name bytes, a next-link byte), four bytes at
 * 0xE04080 the ROM never names, the four per-course "fastest perfect" bests
 * at 0xE04088 (8 bytes each, the fourth being ADVANCED's), and the top-ten
 * head byte at 0xE040A8 -- 0x79 bytes, exactly the range the ROM hands
 * eeprom_write_block after every insert (0x02A44C) and after a reset to
 * defaults (0x031824). This build has no EEPROM, so that call writes the
 * same 0x79 bytes, laid out as the machine's own memory, to g_score_path:
 *
 *     "PCHSCR01"  8-byte magic
 *     image[0x79] 0xE04030..0xE040A8, big-endian longs, bytes as bytes
 *     sum         4-byte big-endian sum of the image bytes
 *
 * and hiscore_load puts it back at boot, after the defaults and before
 * object_display_init seeds the TODAY'S tables from it -- the order the
 * machine's boot reads the EEPROM in. Settings writes (0xE03FE0, 0xE04010,
 * ...) are ignored: free play is forced (eeprom_settings_init).
 *
 * In the _W[] model these tables keep a LONG at each entry and ONE BYTE PER
 * SLOT after it (highscore_table_reset_defaults, object_display_init and the
 * insert all do), so the image is built field by field, not by copying work
 * RAM. */
char g_score_path[512];
static int g_score_ready;               /* no saving until the boot load ran */
static uint8_t g_score_last[0x79];      /* the image last loaded or written */
static int g_score_have_last;

static void hs_put32(uint8_t *p, uint32_t v)
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t hs_get32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* one 8-byte entry: long + three name bytes + link */
static void hs_entry_out(uint8_t *img, int a)
{ int k; hs_put32(img + (a - 0x4030), (uint32_t)W[a]);
  for (k = 4; k < 8; k++) img[a - 0x4030 + k] = (uint8_t)W[a + k]; }
static void hs_entry_in(const uint8_t *img, int a)
{ int k; W[a] = (int32_t)hs_get32(img + (a - 0x4030));
  for (k = 4; k < 7; k++) W[a + k] = img[a - 0x4030 + k];      /* names: unsigned */
  W[a + 7] = (int8_t)img[a - 0x4030 + 7]; }                   /* link: signed, -1 ends */

static void hs_build_image(uint8_t *img)
{
  int i;
  memset(img, 0, 0x79);
  for (i = 0; i < 10; i++) hs_entry_out(img, 0x4030 + i * 8);
  hs_put32(img + 0x50, (uint32_t)W[0x4080]);
  hs_put32(img + 0x54, (uint32_t)W[0x4084]);
  for (i = 0; i < 4; i++) hs_entry_out(img, 0x4088 + i * 8);
  img[0x78] = (uint8_t)W[0x40A8];
}

static void hiscore_save(void)
{
  uint8_t img[0x79], buf[8 + 0x79 + 4];
  uint32_t sum = 0;
  char tmp[600];
  FILE *f;
  int i;
  if (!g_score_path[0] || !g_score_ready) return;
  hs_build_image(img);
  /* eeprom_sync_all runs after EVERY game; only touch the file on a change */
  if (g_score_have_last && memcmp(img, g_score_last, sizeof img) == 0) return;
  for (i = 0; i < 0x79; i++) sum += img[i];
  memcpy(buf, "PCHSCR01", 8);
  memcpy(buf + 8, img, 0x79);
  hs_put32(buf + 8 + 0x79, sum);
  snprintf(tmp, sizeof tmp, "%s.tmp", g_score_path);
  f = fopen(tmp, "wb");
  if (!f) { fprintf(stderr, "[SCORES] cannot write %s\n", tmp); return; }
  if (fwrite(buf, 1, sizeof buf, f) != sizeof buf) { fclose(f); remove(tmp); return; }
  fclose(f);
  if (rename(tmp, g_score_path) != 0) { remove(tmp); return; }
  memcpy(g_score_last, img, sizeof img); g_score_have_last = 1;
  fprintf(stderr, "[SCORES] saved %s\n", g_score_path);
}

void hiscore_load(void)
{
  uint8_t buf[8 + 0x79 + 4];
  uint32_t sum = 0;
  FILE *f;
  int i, ok = 1;
  g_score_ready = 1;
  if (!g_score_path[0]) return;
  /* what is in RAM now is the defaults; a save only writes when it differs */
  hs_build_image(g_score_last); g_score_have_last = 1;
  f = fopen(g_score_path, "rb");
  if (!f) return;                       /* first run: keep the defaults */
  if (fread(buf, 1, sizeof buf, f) != sizeof buf || memcmp(buf, "PCHSCR01", 8) != 0) ok = 0;
  fclose(f);
  if (ok) {
    for (i = 0; i < 0x79; i++) sum += buf[8 + i];
    if (sum != hs_get32(buf + 8 + 0x79)) ok = 0;
  }
  /* the top ten must be a well-formed list: head and links in 0..9 or -1 */
  if (ok) {
    const uint8_t *img = buf + 8;
    if (img[0x78] > 9) ok = 0;
    for (i = 0; i < 10 && ok; i++) {
      int8_t l = (int8_t)img[i * 8 + 7];
      if (l != -1 && (l < 0 || l > 9)) ok = 0;
    }
  }
  if (!ok) { fprintf(stderr, "[SCORES] %s is not a valid score file -- ignored\n", g_score_path); return; }
  {
    const uint8_t *img = buf + 8;
    for (i = 0; i < 10; i++) hs_entry_in(img, 0x4030 + i * 8);
    W[0x4080] = (int32_t)hs_get32(img + 0x50);
    W[0x4084] = (int32_t)hs_get32(img + 0x54);
    for (i = 0; i < 4; i++) hs_entry_in(img, 0x4088 + i * 8);
    W[0x40A8] = img[0x78];
    memcpy(g_score_last, img, 0x79); g_score_have_last = 1;
  }
  fprintf(stderr, "[SCORES] loaded %s\n", g_score_path);
}

void eeprom_write_block(intptr_t param_1,int param_2)

{
  /* ROM 0x019F1A queues a range of the EEPROM mirror for writing. Only the
   * ranking block is persisted here (see the note above). param_1 is a WRAM
   * address in any of the spellings the callers use -- &W[off], the offset,
   * or 0xE0xxxx -- and the ranking callers passed &W[0x4030] into an `int`,
   * truncating the host pointer, until this took an intptr_t. */
  extern intptr_t anim_w_off(const void *p);
  intptr_t off = anim_w_off((const void *)param_1);
  if (off < 0 || param_2 <= 0) return;
  if (off < 0x4030 + 0x79 && off + param_2 > 0x4030) hiscore_save();
  return;
}

/* ---- eeprom_ram_clear ---- */

void eeprom_ram_clear(void)

{
  uint32_t uVar1;
  undefined1 *puVar2;
  
  puVar2 = &W[0x3F30];
  for (uVar1 = 0; uVar1 < 0x240; uVar1 = uVar1 + 1) {
    *puVar2 = 0;
    puVar2 = puVar2 + 1;
  }
  return;
}

/* ---- dipswitch_init ---- */

void dipswitch_init(void)

{
  undefined2 uVar1;
  
  uVar1 = mem_read32(0x440000);
  W[0x2C0C] = 0;
  return;
}

/* ---- eeprom_address_reset ---- */

void eeprom_address_reset(void)

{
  mem_write32(0x600008, 0);
  mem_write32(0x60000A, 0x200);
  return;
}

/* ---- eeprom_callback_clear ---- */

void eeprom_callback_clear(void)

{
  W[0xC894] = 0;
  W[0xC898] = 0;
  W[0xC89C] = 0;
  return;
}

/* ---- eeprom_callback_set ---- */

void eeprom_callback_set(void)

{
  /* void */;
  /* void */;
  
  W[0xC898] = 0;
  W[0xC894] = 0;
  if (0 != 1) {
    W[0xC894] = *(int *)(int)(short)0;
  }
  return;
}

/* ---- eeprom_check_valid ---- */

int eeprom_check_valid(void)

{
  int bVar1;
  short sVar2;
  char *pcVar3;
  char *pcVar4;
  char *pcVar5;
  
  sVar2 = 0;
  do {
    pcVar3 = &R[0x3653C];
    /* Original: pcVar5 = (0x460000) + sVar2 * 0x20 — raw EEPROM address */
    pcVar5 = (char *)g_sys.eeprom + sVar2 * 0x20;
    bVar1 = false;
    while (*pcVar3 != '\0') {
      pcVar4 = pcVar3 + 1;
      if ((*pcVar3 != *pcVar5) || (pcVar3 = pcVar3 + 2, *pcVar4 != pcVar5[0x2000])) {
        bVar1 = true;
        break;
      }
      pcVar5 = pcVar5 + 2;
    }
    if (!bVar1) {
      return (int)sVar2;
    }
    sVar2 = sVar2 + 1;
    if (0xff < sVar2) {
      return 0x100;
    }
  } while( true );
}

/* ---- eeprom_chip_erase ---- */

void eeprom_chip_erase(void)

{
  W[0xC800] = 0x8210000;
  error_report_and_return();
  return;
}

/* ---- eeprom_clear_control ---- */

void eeprom_clear_control(void)

{
  mem_write32(0x60000E, mem_read32(0x60000E) & 0xff00);
  return;
}

/* ---- eeprom_command_handler ---- */

uint64_t eeprom_command_handler(void)
{
  STUB_HIT(0x0489F8, "EEPROM command handler -- free play is forced");
    /* DELIBERATE OVERRIDE, not a gap: 0x0489F8 is real M68K code (first
     * word 0x48E7, movem.l), verified against pr2ver-a.*.
     * EEPROM command handler — simplified.
     * Original is 770 lines of complex state machine with many Ghidra artifacts.
     * Game works with free play enabled (bypasses EEPROM).
     * Returns success state. */
    return 0;
}


/* ---- eeprom_error_or_retry ---- */

uint64_t eeprom_error_or_retry(void)

{
  short in_D1w;
  short extraout_D1w;
  uint64_t uVar1;
  uint64_t in_stack_00000000;

  W[0xC800] = 0x2010000;
  if (in_D1w != 0x100) {
    uVar1 = error_report_and_return();
    return uVar1;
  }
  do {
    ram_test_pattern();
  } while (extraout_D1w != 0);
  eeprom_hw_init_and_validate();
  W[0xC800] = 0;
  return in_stack_00000000;
}

/* ---- eeprom_format_check ---- */

void eeprom_format_check(void)

{
  if ((W[0xC839] & 1) != 0) {
    return;
  }
  eeprom_sector_format();
  return;
}

/* ---- eeprom_format_error_handler ---- */

uint64_t eeprom_format_error_handler(void)

{
  undefined2 in_D1w;
  int bVar1;
  uint64_t uVar2;
  uint64_t in_stack_00000000;

  W[0xC800] = (((uint32_t)(0x302) << 16) | (uint16_t)(in_D1w));
  error_code_save();
  W[0xC84A] = 5;
  W[0xC854] = 0x20;
  eeprom_clear_control();
  mem_write32(0x600010, 0x200);
  eeprom_wait_not_busy();
  mem_write32(0x600004, 0x24);
  bVar1 = false;
  eeprom_wait_ack();
  if (!bVar1) {
    W[0xC800] = 0x3210000;
    uVar2 = error_report_and_return();
    return uVar2;
  }
  eeprom_read_status();
  if (bVar1) {
    eeprom_verify_integrity();
    W[0xC800] = 0;
    return in_stack_00000000;
  }
  W[0xC800] = 0x3220000;
  uVar2 = error_report_and_return();
  return uVar2;
}

/* ---- eeprom_hw_init_and_validate ---- */

undefined4 eeprom_hw_init_and_validate(void)

{
  uint8_t bVar1;
  /* void */;
  undefined4 uVar2;
  short extraout_D1w;
  int bVar3;
  
  uVar2 = W[0xD000];
  W[0xC800] = 0;
  W[0xC839] = 0x11;
  W[0xC844] = '\0';
  mem_write32(0x600006, 0x1234);
  W[0xC838] = 0x10;
  ram_test_pattern();
  W[0xD000] = uVar2;
  while( true ) {
    bVar1 = mem_read32(0x440000);
    mem_write32(0x600004, 0x40);
    mem_write32(0x60000E, 0x3c0);
    mem_write32(0x600010, bVar1 | 0x1a00);
    mem_write32(0x600012, 0x7020);
    mem_write32(0x600014, 0x5f7f);
    mem_write32(0x600016, 0x7f02);
    mem_write32(0x600018, 0x40ff);
    mem_write32(0x60001A, 0);
    eeprom_wait_not_busy();
    mem_write32(0x600004, 0x43);

    bVar3 = false;
    eeprom_wait_ack();
    if (bVar3) {
      eeprom_read_status();
      if (!bVar3) {
        W[0xC800] = 0x1020000;
        uVar2 = error_report_and_return();
        return uVar2;
      }
      mem_write32(0x600008, 0);
      mem_write32(0x60000A, 0x200);
      mem_write32(0x60000C, 0);
      mem_write32(0x60000E, 0x300);
      W[0xC843] = 0;
      W[0xC8A6] = 0;
      W[0xC846] = 1;
      eeprom_default_data_init();
      return 0;
    }
    W[0xC800] = 0x1010000;
    if (extraout_D1w != 0x100) break;
    W[0xC844] = W[0xC844] + '\x01';
    if (W[0xC844] == '\x03') {
      uVar2 = FUN_0004a4c4();
      return uVar2;
    }
  }
  uVar2 = error_report_and_return();
  return uVar2;
}

/* ---- eeprom_init_with_calibration ---- */

void eeprom_init_with_calibration(void)

{
  analog_center_read_from_mcu();
  eeprom_sync_all();
  return;
}

/* ---- eeprom_read_data_block ---- */

void eeprom_read_data_block(void)

{
  short sVar1;
  undefined2 *puVar2;
  undefined2 *puVar3;
  
  sVar1 = 7;
  puVar2 = (undefined2 *)(0x600010);
  puVar3 = &W[0xC808];
  do {
    *puVar3 = *puVar2;
    sVar1 = sVar1 + -1;
    puVar2 = puVar2 + 1;
    puVar3 = puVar3 + 1;
  } while (sVar1 != -1);
  return;
}

/* ---- eeprom_read_entry ---- */

void eeprom_read_entry(int param_1)

{
  eeprom_read_block(W[0x3F30 + param_1] * 0x20,W[0x3F38 + (param_1)]);
  return;
}

/* ---- eeprom_read_status ---- */

uint16_t eeprom_read_status(void)

{
  return mem_read32(0x600002) & 0x8000;
}

/* ---- eeprom_set_page_mode ---- */

void eeprom_set_page_mode(void)

{
  if (W[0xC846] == '\x01') {
    mem_write32(0x60000C, 0);
    return;
  }
  mem_write32(0x60000C, CONCAT11(0x80,W[0xC846]));
  mem_write32(0x60000E, (uint16_t)W[0xC847] << 8);
  return;
}

/* ---- eeprom_set_size ---- */

uint64_t eeprom_set_size(void)

{
  /* void */;

  W[0xC85E] = (short)0;
  return CONCAT44(0 >> 8,(0 & 0xff) - 1) & 0xffffffffff;
}


/* ---- eeprom_shift_right_1 ---- */

uint32_t eeprom_shift_right_1(void)

{
  STUB_HIT(0x049B7C, "returns 0 unconditionally");
  /* void */;

  return 0 >> 1;
}

/* ---- eeprom_update ---- */

void eeprom_update(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x01A0A0 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. The per-frame EEPROM poll. Empty in the retail ROM -- the real work is in
   * eeprom_command_handler, which this build overrides (see game_stubs.c).
   */
  return;
}

/* ---- eeprom_verify_integrity ---- */

void eeprom_verify_integrity(void)

{
  int bVar1;
  
  mem_write32(0x600004, 0x40);
  eeprom_wait_not_busy();
  mem_write32(0x600004, 0x43);
  bVar1 = false;
  eeprom_wait_ack();
  if (!bVar1) {
    W[0xC800] = 0x3910000;
    error_report_and_return();
    return;
  }
  eeprom_read_status();
  if (bVar1) {
    return;
  }
  W[0xC800] = 0x3920000;
  error_report_and_return();
  return;
}

/* ---- eeprom_wait_ack ---- */

uint16_t eeprom_wait_ready(void);

uint16_t eeprom_wait_ack(void)

{
  eeprom_wait_ready();
  return mem_read32(0x600004) & 0xff00;
}

/* ---- eeprom_wait_ready ---- */

uint16_t eeprom_wait_ready(void)

{
  do {
  } while ((mem_read32(0x600002) & 0x8000) == 0);
  return mem_read32(0x600002) & 0x8000;
}

/* ---- eeprom_write_enable ---- */

void eeprom_write_enable(void)

{
  W[0xC800] = 0x7110000;
  error_report_and_return();
  return;
}

/* ---- eeprom_write_ram_block ---- */

void eeprom_write_ram_block(void)

{
  undefined2 *puVar1;
  
  FUN_00049b6a();
  mem_write32(0x600006, 0xb);
  eeprom_wait_not_busy();
  mem_write32(0x600004, 0x34);
  do {
  } while ((mem_read32(0x600002) & 0x2000) != 0x2000);
  puVar1 = (undefined2 *)&W[0xC848];
  do {
    while ((mem_read32(0x600002) & 0x400) == 0x400) {
      mem_write32(0x600000, *puVar1);
      g_sys.syscon[0x14] = 0;
      puVar1 = puVar1 + 1;
    }
  } while (0); /* hw wait removed */
  return;
}

/* ---- eeprom_write_rom_block ---- */

void eeprom_write_rom_block(void)

{
  undefined2 *puVar1;
  
  FUN_00049b6a();
  mem_write32(0x600006, 4);
  eeprom_wait_not_busy();
  mem_write32(0x600004, 0x34);
  do {
  } while ((mem_read32(0x600002) & 0x2000) != 0x2000);
  puVar1 = &R[0x4A52A];
  do {
    while ((mem_read32(0x600002) & 0x400) == 0x400) {
      mem_write32(0x600000, *puVar1);
      g_sys.syscon[0x14] = 0;
      puVar1 = puVar1 + 1;
    }
  } while (0); /* hw wait removed */
  return;
}

/* ---- eeprom_ram_init_defaults @ 0x01A028 ---- */

void eeprom_ram_init_defaults(void)

{
  uint32_t uVar1;
  char *pcVar2;
  undefined1 *puVar3;
  char *pcVar4;
  
  pcVar2 = &W[0x3F30];
  for (pcVar4 = &g_sys.rom[0x3653C]; *pcVar4 != '\0'; pcVar4 = pcVar4 + 1) {
    *pcVar2 = *pcVar4;
    pcVar2 = pcVar2 + 1;
  }
  puVar3 = &W[0x3F38];
  for (uVar1 = 0; uVar1 < 0x12; uVar1 = uVar1 + 1) {
    *puVar3 = (char)uVar1;
    puVar3 = puVar3 + 1;
  }
  for (; (int)uVar1 < 0x58; uVar1 = uVar1 + 1) {
    *puVar3 = 0;
    puVar3 = puVar3 + 1;
  }
  g_eeprom_status = 0;
  eeprom_dirty_bitmap_clear();
  W[0x4174] = 0;
  return;
}





/* ---- eeprom_read_block @ 0x019D0C ---- */

void eeprom_read_block(undefined1 *param_1,int param_2)

{
  undefined1 *puVar1;
  short sVar2;
  undefined1 *puVar3;
  
  /* Original: puVar3 = (0x460000) + param_2 * 0x20 — raw EEPROM address */
  puVar3 = (undefined1 *)g_sys.eeprom + param_2 * 0x20;
  sVar2 = 0;
  do {
    puVar1 = param_1 + 1;
    *param_1 = *puVar3;
    param_1 = param_1 + 2;
    *puVar1 = puVar3[0x2000];
    sVar2 = sVar2 + 1;
    puVar3 = puVar3 + 2;
  } while (sVar2 < 0x10);
  return;
}





/* ---- eeprom_sync_entry @ 0x019DD2 ---- */

uint16_t eeprom_sync_entry(int param_1)

{
  undefined1 *puVar1;
  uint8_t bVar2;
  int iVar3;
  uint32_t uVar4;
  uint16_t uVar5;
  uint32_t uVar6;
  
  puVar1 = &W[0x3F30] + param_1 * 0x20;
  bVar2 = W[0x3F38 + (param_1) * 4];
  uVar6 = (uint32_t)bVar2;
  eeprom_write_verify_block(puVar1,uVar6);
  eeprom_flush_pending();
  uVar5 = W[0x4174] & 2;
  if ((W[0x4174] & 2) == 0) {
    iVar3 = eeprom_compare_block(&W[0x168CC],uVar6);
    uVar5 = 0;
    if (iVar3 != 0) {
      W[0x4174] = W[0x4174] | 1;
      uVar4 = uVar6;
      do {
        eeprom_erase_slot(uVar4);
        eeprom_flush_pending();
        uVar4 = eeprom_find_free_slot();
        if (0xff < (int)uVar4) {
          W[0x3F38 + (param_1) * 4] = bVar2;
          eeprom_write_verify_block(puVar1,uVar6);
          eeprom_flush_pending();
          uVar5 = W[0x4174] & 2;
          uVar4 = uVar6;
          if ((W[0x4174] & 2) == 0) {
            eeprom_dump_and_halt();
            uVar5 = 0;
            W[0x4174] = W[0x4174] | 2;
          }
          break;
        }
        W[0x3F38 + (param_1) * 4] = (char)uVar4;
        eeprom_write_verify_block(puVar1,uVar4);
        eeprom_flush_pending();
        iVar3 = eeprom_compare_block(&W[0x168CC],uVar4);
        uVar5 = 0;
      } while (iVar3 != 0);
      W[0x4174] = W[0x4174] & 0xfffb;
      if (param_1 != 0) {
        uVar5 = eeprom_sync_entry(param_1 + 8U >> 5);
        uVar4 = g_eeprom_status;
      }
      g_eeprom_status = uVar4;
      if (param_1 != 3) {
        uVar5 = eeprom_sync_entry(3);
      }
    }
  }
  return uVar5;
}





/* ---- settings_audio_defaults @ 0x01B7C8 ---- */

void settings_audio_defaults(void)

{
  g_audio_env_params = (int)(int16_t)vrd16(0x36AAA);   /* BE16 = 50 */
  if ((W[0x2BA4] & 2) != 0) {
    g_audio_env_params = 0x2b;
  }
  W[0x3FE4] = g_audio_env_params;
  W[0x3FE2] = (int)(int16_t)vrd16(0x36AC2);            /* BE16 = 50 */
  if ((W[0x2BA4] & 2) != 0) {
    W[0x3FE2] = 0x2b;
  }
  W[0x3FE6] = W[0x3FE2];
  W[0x3FE8] = 0;
  return;
}





/* ---- settings_time_limit_defaults @ 0x01AAE6 ---- */

void settings_time_limit_defaults(void)

{
  /* `move.w $36726.l,D0 ; ext.l D0` at 0x01AAFC -- a 16-bit BIG-ENDIAN
   * read. As a single byte this is the always-zero high half of the BE
   * word, so the setting came out 0 instead of 4. The ROM value matching
   * the branch's own `moveq #$4` fallback two lines below is the check
   * that the width is right. Same for the three siblings. */
  g_time_limit = (int)(int16_t)vrd16(0x36726);
  if ((W[0x2BA4] & 2) != 0) {
    g_time_limit = 4;
  }
  W16_SET(0x3FF6, (int)(int16_t)vrd16(0x3673E));   /* BE16 = 2, matching the fallback */
  if ((W[0x2BA4] & 2) != 0) {
    W16_SET(0x3FF6, 2);
  }
  W16_SET(0x3FF4, 0);
  return;
}






