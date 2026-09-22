/*
 * Terrain, Collision & World
 * Auto-split from game_deps.c / game_ported.c
 */
#include "propcycl.h"
#include "rom_decode.h"
extern int g_no_zones;

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

/* Read big-endian values from ROM (fix M68K→x86 endianness) */
#define ROM_READ32(off) ((int32_t)((R[off]<<24)|(R[(off)+1]<<16)|(R[(off)+2]<<8)|R[(off)+3]))
#define ROM_READ8(off) (((off) >= 0 && (off) < ROM_SIZE) ? (int8_t)R[off] : 0)

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

/* ---- collision_geometry_update ---- */

void collision_geometry_update(void)

{
  /* THE DYNAMIC COLLISION ZONES -- register row 96's open tail, and row 120.
   * MAME holds W[0x291C] = 7 for course 0 and the zone coordinates
   * (618527, 85424, 1038211) etc; ours held 0 because this never ran and,
   * when it did, addressed `_W[]` as a host `undefined4 *` array.
   *
   * ROM 0x01587E:
   *   tst.l $e00e0c -> d3 = course ? 4 : 7 ; move.l d3,$e0291c
   *   a3 = 0xE02920, a1 = 0xE04438, per record (stride 0x58), dst stride 4:
   *     move.w $e(a1),d0 ; ext.l ; lsl.l #2 ; neg.l ; move.l d0,$90(a3)
   *     move.l $8(a1),$60(a3)
   *     lea $fa4d.w,a0 ; adda.l $4(a1),a0 ; move.l a0,$30(a3)
   *     move.l (a1),(a3)+
   *   then a3 = 0xE029E0, per record:
   *     movea.w $44(a1),a0 ; adda.w $44(a1),a0 ; adda.w $44(a1),a0   (3*model)
   *     lea $33904(a0.l*4),a4
   *     move.l -(a4),$60(a3) ; move.l -(a4),$30(a3) ; move.l -(a4),(a3)+
   * i.e. three BE32 ROM words at 0x338F8 + 3*model*4 + {0,4,8}. The old C had
   * the second of those at `0x33900 + iVar1 + 2` (should be 0x338FC + iVar1)
   * and the third as `*(undefined4 *)(iVar1 + 0x338f8)` -- a HOST dereference
   * of a ROM offset. */
  int n, i;
  uint32_t nb, dst;

  { extern int g_zone_dbg; if (g_zone_dbg) { static int m;
      if (m++ < 4) fprintf(stderr, "[CGU] f=%d course=%d\n",
          (int)g_sys.frame_count, (int)W[0x0E0C]); } }

  n = (W[0x0E0C] == 0) ? 7 : 4;
  /* ONLY PUBLISH RECORDS THAT ARE ACTUALLY SPAWNED. The ROM publishes all n
   * unconditionally because on the machine these records always exist; in this
   * build the animated-object seed is still gated (g_animobj), so an
   * unconditional publish puts n zones of ZEROES at the world origin -- seven
   * phantom collision volumes, strictly worse than the none we had. Bit 15 of
   * the flag half at +0x48 is the ROM's own "not spawned" marker (set for
   * every slot by animated_objects_reset_all @0x014EE2), so stopping at the
   * first inactive record is a no-op on the machine and safe here. Remove this
   * once the seed is ungated AND the record positions are right -- see the
   * note below. */
  { int k;
    for (k = 0; k < n; k++)
      if ((W_HI16(0x4438 + (uint32_t)k * 0x58 + 0x48) & 0x8000) != 0) break;
    n = k; }
  W[0x291C] = n;

  nb = 0x4438;
  dst = 0x2920;
  for (i = 0; i < n; i++, nb += 0x58, dst += 4) {
    W[dst + 0x90] = -((int)(short)W_LO16(nb + 0x0c)) * 4;
    W[dst + 0x60] = (int32_t)W[nb + 0x08];
    /* `lea $fa4d.w,a0` @0x0158BC is an absolute SHORT address -- sign-extended
     * to 0xFFFFFA4D, i.e. -1459. Added as +0xfa4d the zone sat 65536 units
     * above its object, out of reach of every probe (register row 160). */
    W[dst + 0x30] = (int32_t)((uint32_t)W[nb + 0x04] + (uint32_t)(int32_t)(int16_t)0xfa4d);
    W[dst + 0x00] = (int32_t)W[nb + 0x00];
  }

  nb = 0x4438;
  dst = 0x29E0;
  for (i = 0; i < n; i++, nb += 0x58, dst += 4) {
    uint32_t base = 0x338F8 + (uint32_t)((int)(short)W_HI16(nb + 0x44) * 3) * 4;
    if (base + 11 >= ROM_SIZE) continue;
    W[dst + 0x00] = (int32_t)vrd32(base);
    W[dst + 0x30] = (int32_t)vrd32(base + 4);
    W[dst + 0x60] = (int32_t)vrd32(base + 8);
  }
  { extern int g_zone_dbg; if (g_zone_dbg) { static int m;
      if (m++ == 30) { fprintf(stderr, "[ZONES] n=%d\n", n);
        for (i = 0; i < n; i++) {
          uint32_t r = 0x4438 + (uint32_t)i * 0x58;
          fprintf(stderr, "   rec%d @0x%04X: x=%-12d y=%-12d z=%-12d f0c=%08X f44=%08X f48=%08X\n",
              i, r, (int)(int32_t)W[r], (int)(int32_t)W[r+4], (int)(int32_t)W[r+8],
              (unsigned)(uint32_t)W[r+0x0c], (unsigned)(uint32_t)W[r+0x44],
              (unsigned)(uint32_t)W[r+0x48]);
        }
        for (i = 0; i < n; i++)
          fprintf(stderr, "  %d  xyz=(%d,%d,%d) hdg=%d  ext=(%d,%d,%d)\n", i,
              (int)(int32_t)W[0x2920 + i*4], (int)(int32_t)W[0x2920 + i*4 + 0x30],
              (int)(int32_t)W[0x2920 + i*4 + 0x60], (int)(int32_t)W[0x2920 + i*4 + 0x90],
              (int)(int32_t)W[0x29E0 + i*4], (int)(int32_t)W[0x29E0 + i*4 + 0x30],
              (int)(int32_t)W[0x29E0 + i*4 + 0x60]); } } }
  /* STILL WRONG: the zone COORDINATES. With the seed on, the records carry
   * sane model ids (f44 = 0x385..0x38A) and headings, but x/y/z read as
   * garbage -- and y/z have ZERO low halves (0xB0490000, 0x83E70000), the
   * signature of a 16-bit value written into the HIGH half of the slot. MAME's
   * seven zones are (618527, 85424, 1038211) and friends. The next link is
   * `animated_object_script_update` @0x014FBC, which is what walks the script
   * at rec+0x3C and writes the position; its conversion is not finished. */
  return;
}


/* ---- collision_zone_check_objects ---- */

int g_no_zones = 0;
int g_wrt_dbg = 0;
int g_wot_frame = -1;            /* PROPCYCL_WOTDBG=<frame>: per-layer point-test trace at that frame */
/* PROPCYCL_NO_LAYERFIX=1 reverts ALL THREE of this session's terrain-layer
 * layout corrections together -- the face-accumulator stride, the
 * face-pointer table layout and the recovered 0x4CC collapse -- so the
 * multi-layer behaviour can be A/B'd against the pre-fix build. */
int g_layerfix = 1;
/* PROPCYCL_NO_VOTEFIX=1 reverts register row 146 -- the four-counter
 * correction in terrain_height_resolve -- so the collision walk can be A/B'd
 * against the pre-fix build. */
int g_votefix = 1;
#define g_facestride (g_layerfix ? 4 : 1)
int g_tunlog = 0;                /* PROPCYCL_TUNLOG=<n>: every n frames, print the CENTRE probe's
                                  * whole collision column -- the per-cell layer count and object
                                  * mask straight out of the ROM tables, every face the layer walk
                                  * found (kind 4 = pass-through, 12 = solid), the parity flag that
                                  * decides open-air vs inside-solid, and the resolved floor. This is
                                  * the one view that says whether a multi-layer TUNNEL cell is being
                                  * honoured, because a single number (the floor) cannot distinguish
                                  * "no tunnel here" from "tunnel found and ignored". */               /* PROPCYCL_WRTDBG=1: trace the look-ahead escape search */
static int g_cz_callback = 0;   /* ROM address of the per-type hit callback (d3) */
int g_zone_dbg = 0;              /* PROPCYCL_ZONEDBG=1: print every static-zone hit */

/* The six per-type hit callbacks, converted from ROM 0x0065D8..0x00685A. Each
 * computes W[0x0BB4] (TOP face offset from the zone's y) and W[0x0BB8] (BOTTOM
 * offset) for the probe layer currently being tested. `muls.l` is a 32x32->32
 * multiply on the 68020, then `asr.l #9`. Types 1, 4 and 5 pick the probe
 * delta through a sub-type table (0x6602 / 0x675E / 0x67E4):
 *   sub 0: W[0x0BA4] - W[0x0BA0]     sub 2: W[0x0BAC] - W[0x0B98]
 *   sub 1: W[0x0BA8] - W[0x0BA0]     sub 3: W[0x0BB0] - W[0x0B98]
 * (a sub above 3 leaves the delta as the layer index in D2 -- treated as 0). */
static int32_t cz_mul_shr9(int32_t a, int32_t b)
{
    return (int32_t)((int64_t)a * (int64_t)b) >> 9;
}
static int32_t cz_sub_delta(void)
{
    switch ((int)W[0x0B88]) {
    case 0: return (int32_t)(W[0x0BA4] - W[0x0BA0]);
    case 1: return (int32_t)(W[0x0BA8] - W[0x0BA0]);
    case 2: return (int32_t)(W[0x0BAC] - W[0x0B98]);
    case 3: return (int32_t)(W[0x0BB0] - W[0x0B98]);
    default: return 0;
    }
}
static void cz_type_callback(void)
{
    int32_t d, d2, a0, a1;
    switch ((int)W[0x0B4C]) {
    case 0:                                          /* 0x65D8 */
        W[0x0BB4] = W[0x0B84]; W[0x0BB8] = 0; break;
    case 1:                                          /* 0x65EA */
        d = cz_sub_delta(); if (d < 0) d = -d;
        W[0x0BB4] = cz_mul_shr9(d, (int32_t)W[0x0B8C]) + (int32_t)W[0x0B84]; W[0x0BB8] = 0; break;
    case 2:                                          /* 0x6664 */
        d = ((int)W[0x0B88] < 2) ? (int32_t)(W[0x0BC0] - W[0x0BA0]) : (int32_t)(W[0x0BBC] - W[0x0B98]);
        if (d < 0) d = -d;
        W[0x0BB4] = cz_mul_shr9(d, (int32_t)W[0x0B8C]) + (int32_t)W[0x0B84]; W[0x0BB8] = 0; break;
    case 3:                                          /* 0x66B2 */
        if ((int)W[0x0B88] < 2) { d  = (int32_t)(W[0x0BA0] - W[0x0BC0]); d2 = (int32_t)(W[0x0B98] - W[0x0BBC]); }
        else                     { d  = (int32_t)(W[0x0B98] - W[0x0BBC]); d2 = (int32_t)(W[0x0BA0] - W[0x0BC0]); }
        if (d < 0) d = -d;   a1 = cz_mul_shr9(d,  (int32_t)W[0x0B8C]) + (int32_t)W[0x0B84];
        if (d2 < 0) d2 = -d2; a0 = cz_mul_shr9(d2, (int32_t)W[0x0B90]) + (int32_t)W[0x0B84];
        W[0x0BB4] = (a0 > a1) ? a1 : a0; W[0x0BB8] = 0; break;
    case 4:                                          /* 0x6746 */
        d = cz_sub_delta(); if (d < 0) d = -d;
        d = cz_mul_shr9(d, (int32_t)W[0x0B8C]);
        W[0x0BB4] = (int32_t)W[0x0B84] + d; W[0x0BB8] = ((int32_t)W[0x0B90] << 4) + d; break;
    case 5:                                          /* 0x67CC */
        d = cz_sub_delta(); if (d < 0) d = -d;
        W[0x0BB4] = cz_mul_shr9(d, (int32_t)W[0x0B8C]) + (int32_t)W[0x0B84];
        W[0x0BB8] = cz_mul_shr9(d, (int32_t)W[0x0B90]) + ((int32_t)W[0x0B94] << 4); break;
    default: break;                                  /* type > 5: 0x63A2, no callback */
    }
}

void collision_zone_check_objects(void)

{
  int bVar1;
  int iVar2;
  int iVar3;
  /* code *0 removed - Ghidra artifact */

  /* THE TYPE DISPATCH Ghidra lost. ROM 0x00633E..0x00639C: `d0 = W[0x0B4C];
   * subq.l #5; bhi default` then a 6-entry word table at 0x6354 indexed by
   * type. Every handler adjusts the parse CURSOR (the entry is shorter than the
   * 32 bytes the parser reads) and loads a per-type HIT CALLBACK into d3, then
   * `bra $63a2` -- INTO the box test below, never past it. The old C returned
   * here for every type, so no static zone could ever do anything.
   *
   *   type 0  0x6360  cursor -= 6  cb 0x65D8        type 3  0x6384  -= 2  cb 0x66B2
   *   type 1  0x636C  cursor -= 4  cb 0x65EA        type 4  0x6390  -= 2  cb 0x6746
   *   type 2  0x6378  cursor -= 4  cb 0x6664        type 5  0x639C  -= 0  cb 0x67CC
   *   type >5 0x63A2  no adjust, no callback
   *
   * The callbacks (each dispatching again on the sub-type W[0x0B88]) are not
   * converted yet -- a hit reports once and inserts the default faces. */
  { static const int rew[6] = { 6, 4, 4, 2, 2, 0 };
    static const int cbs[6] = { 0x65D8, 0x65EA, 0x6664, 0x66B2, 0x6746, 0x67CC };
    int t = (int)W[0x0B4C];
    g_cz_callback = 0;
    if (t >= 0 && t <= 5) { W[0x0BC4] = W[0x0BC4] - rew[t]; g_cz_callback = cbs[t]; } }
  W[0x0B98] = W[0x1318] - W[0x0B54];
  W[0x0B9C] = W[0x131C] - W[0x0B58];
  W[0x0BA0] = W[0x1320] - W[0x0B5C];
  W[0x0BC0] = W[0x0B98] * W[0x0B70] >> 0xc;
  W[0x0BBC] = W[0x0BA0] * -W[0x0B70] >> 0xc;
  W[0x0B74] = W[0x0B68] + W[0x0BC0];
  W[0x0B78] = W[0x0BC0] - W[0x0B68];
  W[0x0B7C] = W[0x0B6C] + W[0x0BBC];
  W[0x0B80] = W[0x0BBC] - W[0x0B6C];
  bVar1 = false;
  if ((((W[0x0BA0] < W[0x0B74]) && (W[0x0B78] < W[0x0BA0])) &&
      (W[0x0B98] < W[0x0B7C])) && (W[0x0B80] < W[0x0B98])) {
    bVar1 = true;
  }
  if (bVar1) {
    for (iVar3 = 0; iVar3 < W[0x1308]; iVar3 = iVar3 + 1) {
      W[0x0B98] = W[0x1318 + (iVar3 * 0x140)] - W[0x0B54];
      W[0x0B9C] = W[0x131C + (iVar3 * 0x140)] - W[0x0B58];
      W[0x0BA0] = W[0x1320 + (iVar3 * 0x140)] - W[0x0B5C];
      W[0x0BC0] = W[0x0B98] * W[0x0B70] >> 0xc;
      W[0x0BBC] = W[0x0BA0] * -W[0x0B70] >> 0xc;
      W[0x0BA4] = W[0x0B60] + W[0x0BC0];
      W[0x0BA8] = W[0x0BC0] - W[0x0B60];
      W[0x0BAC] = W[0x0B64] + W[0x0BBC];
      W[0x0BB0] = W[0x0BBC] - W[0x0B64];
      bVar1 = false;
      if (((W[0x0BA0] < W[0x0BA4]) && (W[0x0BA8] < W[0x0BA0])) &&
         ((W[0x0B98] < W[0x0BAC] && (W[0x0BB0] < W[0x0B98])))) {
        bVar1 = true;
      }
      if (bVar1) {
        /* ROM 0x00652C..0x006532: `tst.l (-$4,A6); beq; movea.l D3,A0; jsr (A0)`
         * -- the per-type callback runs HERE, per probe layer that is inside the
         * zone, and sets W[0x0BB4] (top face offset) / W[0x0BB8] (bottom face
         * offset) from the probe deltas W[0x0B98]/W[0x0BA0] and the rotated
         * ones W[0x0BBC]/W[0x0BC0] computed just above. The two faces inserted
         * below are zone_y + those offsets. */
        cz_type_callback();
        if (g_zone_dbg)
            fprintf(stderr, "[ZONE] f%u hb=%#lx hit type %ld sub %ld layer %d zone=(%ld,%ld,%ld) ext68/6C=%ld/%ld rot70=%ld"
                            " -> top=%ld bot=%ld | probe0=(%ld,%ld,%ld) probeL=(%ld,%ld,%ld) d=(%ld,%ld,%ld) box z[%ld..%ld] x[%ld..%ld] | player=(%ld,%ld,%ld)\n",
                    g_sys.frame_count, (unsigned long)W[0x0B50], (long)W[0x0B4C], (long)W[0x0B88], iVar3,
                    (long)W[0x0B54], (long)W[0x0B58], (long)W[0x0B5C], (long)W[0x0B68], (long)W[0x0B6C], (long)W[0x0B70],
                    (long)W[0x0BB4], (long)W[0x0BB8],
                    (long)W[0x1318], (long)W[0x131C], (long)W[0x1320],
                    (long)W[0x1318 + iVar3*0x140], (long)W[0x131C + iVar3*0x140], (long)W[0x1320 + iVar3*0x140],
                    (long)W[0x0B98], (long)W[0x0B9C], (long)W[0x0BA0],
                    (long)W[0x0B78], (long)W[0x0B74], (long)W[0x0B80], (long)W[0x0B7C],
                    (long)W[0x0D00], (long)W[0x0D04], (long)W[0x0D08]);
        W[0x1334 + (iVar3 * 0x140 + (&W[0x1330])[iVar3 * 0x140] * 4)] =
             (W[0x0B58] + W[0x0BB4]) * 0x10 + 0xc;
        iVar2 = iVar3 * 0x140;
        W[0x13AC + ((&W[0x1330])[iVar3 * 0x140] + iVar2)] = (char)W[0x0B50];
        W[0x13CA + ((&W[0x1330])[iVar3 * 0x140] + iVar2)] = 1;
        W[0x1330 + (iVar3 * 0x140)] = W[0x1330 + (iVar3 * 0x140)] + 1;
        W[0x1334 + (iVar3 * 0x140 + (&W[0x1330])[iVar3 * 0x140] * 4)] =
             (W[0x0B58] + W[0x0BB8]) * 0x10 + 4;
        W[0x13AC + ((&W[0x1330])[iVar3 * 0x140] + iVar2)] = (char)W[0x0B50];
        W[0x13CA + ((&W[0x1330])[iVar3 * 0x140] + iVar2)] = 1;
        W[0x1330 + (iVar3 * 0x140)] = W[0x1330 + (iVar3 * 0x140)] + 1;
      }
    }
  }
  return;
}


/* ---- cz_load_color_ramp ---- */

void cz_load_color_ramp(int param_1,uint32_t param_2)

{
  int iVar1;
  int iVar2;
  undefined1 *puVar3;
  int iVar5;
  undefined1 *puVar6;
  undefined1 *puVar7;

  /* Read 32-bit BE data index from ROM descriptor table at 0x1C4F4C */
  {
    uint32_t desc_off = 0x1C4F4C + (uint32_t)param_1 * 0x10;
    int32_t data_idx = (R[desc_off]<<24)|(R[desc_off+1]<<16)|(R[desc_off+2]<<8)|R[desc_off+3];
    /* iVar5 = ROM byte offset to color ramp data (4 bytes per entry: padding,R,B,G) */
    iVar5 = data_idx * 0x40 + 0x1c6200;
  }
  /* param_2 is a BYTE argument -- ROM 0x021B50 is `move.b $1c(a7),d3`, then
   * 0x021B6C `moveq #0,d0 ; move.b d3,d0 ; lsl.l #4,d0`. Ghidra modelled the
   * byte-on-the-stack as the TOP byte of a long, so `>> 0x18` on the real
   * argument (1 and 5 at the two countdown call sites) gives 0 and every ramp
   * landed on palette index 0 instead of its own. */
  iVar1 = (int)(param_2 & 0xFF) * 0x10;
  iVar2 = 0;
  puVar3 = &g_sys.palette_ram[0x7F00] + iVar1;
  puVar6 = &g_sys.palette_ram[0x17F00] + iVar1;
  puVar7 = &g_sys.palette_ram[0xFF00] + iVar1;
  do {
    /* ROM format: [pad][R][G][B] per entry.
     * puVar3 = palette_ram R plane, puVar7 = G plane, puVar6 = B plane */
    if ((uint32_t)(iVar5 + 3) < ROM_SIZE) {
      *puVar3 = R[iVar5 + 1];   /* R */
      *puVar7 = R[iVar5 + 2];   /* G */
      *puVar6 = R[iVar5 + 3];   /* B */
    }
    palette_mark_written(0x7F00 + iVar1 + iVar2);
    iVar5 = iVar5 + 4;
    iVar2 = iVar2 + 1;
    puVar3 = puVar3 + 1;
    puVar6 = puVar6 + 1;
    puVar7 = puVar7 + 1;
  } while (iVar2 < 0x10);
  return;
}


/* ===================================================================
 * THE ENVIRONMENT ZONES -- per-cell fog colour / CZ-delta presets.
 * ROM 0x0284AA..0x028AFE, rewritten from the disassembly (2026-09-18).
 *
 * THE LAYOUT. Every field below is SIXTEEN BITS -- every ROM access is a
 * move.w / tst.w / clr.w / cmp.w -- and half of them sit at 2-mod-4
 * offsets, so in the _W[] model each one is a HALF of a 4-aligned slot:
 *
 *   0xAAF2 target fog R     0xAAFA current fog R     (LO of 0xAAF0 / 0xAAF8)
 *   0xAAF4 target fog G     0xAAFC current fog G     (HI of 0xAAF4 / 0xAAFC)
 *   0xAAF6 target fog B     0xAAFE current fog B     (LO of 0xAAF4 / 0xAAFC)
 *   0xAAF8 target CZ delta  0xAB00 current CZ delta  (HI of 0xAAF8 / 0xAB00)
 *   0xAB02 transition frames left                    (LO of 0xAB00)
 *   0x16A30 altitude-fog active    0x16A32 altitude-fog level
 *   0x16A34 altitude-fog target    0x16A36 CURRENT ZONE id
 *   0x16A38 camera cell            0x16A3A settle delay
 *   0x16A3C (long) this course's zone table, ROM 0x37758 + course*0x80
 *
 * The transpile stored every one of them as a whole slot, so each 2-mod-4
 * field was rebuilt from its neighbours by sync_wram_to_W every frame (the
 * target fog R read the sprite-buffer selector at 0xAAF0 in its top half),
 * and it wrote the CZ registers with mem_write32 where the ROM does
 * `move.w` -- so czattr[0..3] got the value's HIGH half (0 or 0xFFFF) and
 * the value itself landed one register further on.
 *
 * THE TABLES (ROM, big-endian 16-bit):
 *   0x37684 + preset*10 : fog R, G, B, CZ delta, fog enable (-> W16[0xEB20])
 *   0x376E0 + zone*4    : (preset, transition frames); frames < 0 marks a
 *                         SPECIAL zone, handled through the two jump tables
 *                         in environment_zone_transition/_special_event
 *   0x37758 + course*0x80 + cell : signed byte zone id per camera cell
 * =================================================================== */
#define ENVW(o)        ((int)W_A16((o), 0))
#define ENVW_SET(o, v) W_A16_SET((o), 0, (v))

void environment_params_load(int preset);
void environment_params_set_direct(int preset, int frames);
void environment_zone_transition(void);
void environment_zone_special_event(void);
void fog_altitude_blend(void);
void fog_altitude_fadeout(void);

/* ---- environment_params_load @0x0284AA -- snap to a preset, no fade ---- */

void environment_params_load(int preset)
{
  uint32_t a = 0x37684 + (int16_t)preset * 10;   /* move.w $4(a7),d0 ; ext.l ; *10 */
  int16_t v;

  v = vrd16s(a);     ENVW_SET(0xAAF2, v); ENVW_SET(0xAAFA, v); g_sys.videomix[0x0005] = (uint8_t)v;
  v = vrd16s(a + 2); ENVW_SET(0xAAF4, v); ENVW_SET(0xAAFC, v); g_sys.videomix[0x0006] = (uint8_t)v;
  v = vrd16s(a + 4); ENVW_SET(0xAAF6, v); ENVW_SET(0xAAFE, v); g_sys.videomix[0x0007] = (uint8_t)v;
  v = vrd16s(a + 6); ENVW_SET(0xAAF8, v); ENVW_SET(0xAB00, v);
  mem_write16(0x810000, (uint16_t)v);    /* czattr[0..3]: four `move.w d0,...` */
  mem_write16(0x810002, (uint16_t)v);
  mem_write16(0x810004, (uint16_t)v);
  mem_write16(0x810006, (uint16_t)v);
  ENVW_SET(0xAB02, 0);
  /* 0x028528 `tst.w (a0)` on the preset's 5th word, then `move.w #1` /
   * `clr.w $e0eb20` -- the FOG ENABLE palette_update publishes to czattr[4].
   * A 16-bit field at a 4-aligned offset: the HIGH half of slot 0xEB20. */
  W_SET_HI16(0xEB20, vrd16s(a + 8) != 0);
  ENVW_SET(0x16A34, 0);
  ENVW_SET(0x16A32, 0);
  g_sys.videomix[0x000D] = 0;
}


/* ---- environment_zone_force_update @0x028958 ---- */

void environment_zone_force_update(void)
{
  int zone;

  ENVW_SET(0x16A36, -1);                  /* move.w #$ffff,$e16a36 */
  camera_grid_calc_position();
  environment_zone_transition();
  /* 0x02896A: move.b (a0,d0.w),d0 ; extb.l ; lsl.l #2 ; tst.w $376e2(d0.l).
   * The transpile read rom[0x376E2 + table + cell], i.e. added the zone
   * TABLE'S ADDRESS in, where the ROM indexes 0x376E2 by zone*4. */
  zone = vrd8s((int32_t)W[0x16A3C] + (int16_t)ENVW(0x16A38));
  if (vrd16s(0x376E2 + zone * 4) < 0) {
    environment_zone_special_event();
  }
  if (vrd8((int32_t)W[0x0CF4] + (int32_t)W[0x16A3C]) == 0x16) {
    fog_altitude_blend();
  }
  else {
    fog_altitude_fadeout();
  }
  ENVW_SET(0xAB02, 1);
  ENVW_SET(0x16A3A, 3);
}


/* ---- environment_zone_tick @0x0289BA ---- */

void environment_zone_tick(void)
{
  int zone, n;

  /* movea.l $e00cf4,a0 ; cmpa.w $e16a38,a0 -- the word sign-extended */
  if ((int32_t)W[0x0CF4] != ENVW(0x16A38)) {
    environment_zone_transition();
  }
  zone = vrd8s((int32_t)W[0x16A3C] + (int16_t)ENVW(0x16A38));
  if (vrd16s(0x376E2 + zone * 4) < 0) {
    environment_zone_special_event();
  }
  if (vrd8((int32_t)W[0x0CF4] + (int32_t)W[0x16A3C]) == 0x16) {
    fog_altitude_blend();
  }
  else {
    fog_altitude_fadeout();
  }
  if (ENVW(0x16A3A) != 0) {               /* tst.w ; subq.w #1 ; bra rts */
    ENVW_SET(0x16A3A, ENVW(0x16A3A) - 1);
    return;
  }
  n = ENVW(0xAB02);
  if (n == 0) return;
  if (n == 1) {
    ENVW_SET(0xAAFA, ENVW(0xAAF2));
    ENVW_SET(0xAAFC, ENVW(0xAAF4));
    ENVW_SET(0xAAFE, ENVW(0xAAF6));
    ENVW_SET(0xAB00, ENVW(0xAAF8));
    ENVW_SET(0xAB02, 0);
  }
  else {
    /* movea.w target,a0 ; suba.w (cur),a0 ; divs.l frames ; add.w d0,(cur) */
    ENVW_SET(0xAAFA, ENVW(0xAAFA) + (ENVW(0xAAF2) - ENVW(0xAAFA)) / n);
    ENVW_SET(0xAAFC, ENVW(0xAAFC) + (ENVW(0xAAF4) - ENVW(0xAAFC)) / n);
    ENVW_SET(0xAAFE, ENVW(0xAAFE) + (ENVW(0xAAF6) - ENVW(0xAAFE)) / n);
    ENVW_SET(0xAB00, ENVW(0xAB00) + (ENVW(0xAAF8) - ENVW(0xAB00)) / n);
    ENVW_SET(0xAB02, n - 1);
  }
  g_sys.videomix[0x0005] = (uint8_t)ENVW(0xAAFA);
  g_sys.videomix[0x0006] = (uint8_t)ENVW(0xAAFC);
  g_sys.videomix[0x0007] = (uint8_t)ENVW(0xAAFE);
  mem_write16(0x810000, (uint16_t)ENVW(0xAB00));
  mem_write16(0x810002, (uint16_t)ENVW(0xAB00));
  mem_write16(0x810004, (uint16_t)ENVW(0xAB00));
  mem_write16(0x810006, (uint16_t)ENVW(0xAB00));
}


/* ---- terrain_chunk_render ---- */

/* Per-course terrain base addresses and bitmask pointers.
 * Decoded from the M68K jump table at 0xAD0A (tools/decode_terrain_dispatch.py;
 * the `move.w $ad10(pc,d0.w*2)` at 0x00AD00 indexes with d0 = course - 3, so
 * the table sits 3 entries below the 0xAD10 capstone prints: [0008 0016 0024
 * 0032] -> 0xAD12/0xAD20/0xAD2E/0xAD3C). terrain_base_addr = starting
 * point ROM model ID for the 8x16 terrain grid; terrain_bitmask_ptr
 * = ROM address of the per-camera-cell visibility bitmask.
 *
 * Earlier versions of this array had {0x615, 0, 0, 0} — that's
 * course 3's values applied to course 0, with courses 1-3 left at 0
 * (so they rendered model_id 0 + offset = junk). Replaced with the
 * full M68K-decoded mapping. */
static const int course_terrain_base[] = { 0x495, 0x515, 0x595, 0x615 };
static const int course_terrain_bitmask[] = {
    0x8B280, 0x8BA80, 0x8C280, 0x8CA80
};

/* The per-chunk visibility bytes at WRAM 0xE01234 live one per _W[] slot
 * (pinned); the prop gates read them as work-RAM bytes through vrd8, which
 * sync_W_to_wram would otherwise fill from the aligned slots' long images. */
static void chunk_lookup_mirror(void)
{
  int i;
  for (i = 0; i < 0x80; i++) g_sys.work_ram[0x1234 + i] = (uint8_t)W[0x1234 + i];
}

void terrain_chunk_render(uint32_t param_1)

{
  int iVar1;

  iVar1 = 0;
  do {
    (&g_chunk_lookup_table)[iVar1] = 0;
    iVar1 = iVar1 + 1;
  } while (iVar1 < 0x80);
  chunk_lookup_mirror();

  /* Course dispatch through the jump table at 0xAD0A. Every case stores
   * `move.l #base,(a0) ; move.l #mask,(a1)` (a0 = 0xE012B4, a1 = 0xE012B8)
   * and branches to the rts at 0x00AD48; param > 3 (`subq.l #3 ; bhi`) goes
   * straight there. */
  if (param_1 <= 3) {
    g_terrain_base_addr = course_terrain_base[param_1];
    g_terrain_bitmask_ptr = course_terrain_bitmask[param_1];
    if (propcycl_verbose()) printf("[TERRAIN] course %d: base=%d (0x%X), bitmask=0x%X\n",
           param_1, (int)g_terrain_base_addr, (int)g_terrain_base_addr,
           (int)g_terrain_bitmask_ptr);
  }
  /* The transpile ended with `terrain_lod_update_wrapper();` here -- the same
   * stand-in register row 65 removed from terrain_props_dispatch. The ROM
   * has no call: all four cases branch to the rts at 0x00AD48, and
   * gameplay_tick (ROM 0x00A3A0) is terrain_lod_update's only real caller. */
  return;
}


/* ---- terrain_chunk_visibility ---- */

void terrain_chunk_visibility(uint32_t param_1,int param_2,uint32_t param_3,int param_4)

{
  int iVar1;
  int iVar2;
  uint32_t uVar3;
  int iVar4;
  int iVar5;
  int iVar6;
  int iVar7;
  int iVar8;
  int *piVar9;
  int *piVar10;
  int *piVar11;
  undefined4 *local_2a;
  int local_a;
  uint8_t local_6;
  
  /* THE 32-LONG 0xFF FILL, and it was landing on the wrong slots entirely.
   *
   * ROM 0x00AB54..0x00AB70:
   *     move.l #$e01234, -$26(a6)      ; cursor
   *  .l movea.l -$26(a6), a0
   *     addq.l #$4, -$26(a6)           ; cursor += 4  (BYTES of WRAM)
   *     moveq  #$ff, d0                ; sign-extends to 0xFFFFFFFF
   *     move.l d0, (a0)
   *     addq.l #$1, d2 ; moveq #$20,d0 ; cmp.l d0,d2 ; blt .l
   * i.e. 32 longs at WRAM 0xE01234, 0xE01238, ... 0xE012B0 -- 128 bytes, all
   * 0xFF.
   *
   * `g_chunk_lookup_table` is `W[0x1234]`, so `&g_chunk_lookup_table` is
   * `&_W[0x1234]` -- an `intptr_t *`. The transpile cast it to `undefined4 *`
   * and stepped it 32 times, which advances FOUR HOST BYTES a step: 128 host
   * bytes = 16 `_W[]` elements, so it filled slots 0x1234..0x1243 instead of
   * the 32 slots 0x1234, 0x1238, ... 0x12B0 that the byte-offset model needs.
   * Register row 137's class -- a narrower type walking an `intptr_t[]`.
   *
   * WHAT IT COST: `game_props.c` gates three props on single bytes inside
   * that range -- ROM 0x5178 `cmpi.b #$ff, $e012a1.l` for model 374,
   * 0xE01267 for 375, 0xE01266 for 376, each "not 0xFF -> skip". Measured at
   * eight points of a player's own low-altitude flight, MAME holds
   * `1266:FF 1267:FF 12A1:FF` and emitted codes 443/444/445 at every one;
   * ours held `00/00/00` and emitted them at none, 0 of 24.
   *
   * The literal is cast because `0xffffffff` is `unsigned int` in C and
   * widening it to a 64-bit slot gives 4294967295, not -1 (register row 33). */
  /* ONE _W[] SLOT PER BYTE (register row 160). The ROM's 32 long stores set
   * all 128 BYTES to 0xFF, and every reader here takes one byte per chunk --
   * `cmpi.b #9, $e01234(chunk)` in the object drawers, `== -1` in
   * camera_dsp_*. Filling only the 4-aligned slots left the other three of
   * every four holding whatever an earlier frame put there (the sync never
   * rewrites an odd slot), so a chunk that had once been visible stayed
   * "visible" for good: MAME holds 0xFF for chunks 85/86 through the whole
   * attract demo, and we drew the brown train sitting in them. The region is
   * pinned in game_init and mirrored to work RAM below for the vrd8 readers. */
  for (iVar7 = 0; iVar7 < 0x80; iVar7++) {
    W[0x1234 + iVar7] = -1;
  }
  (void)local_2a;
  *(int32_t*)W[0x0CA4] = 0x8000;
  piVar9 = (intptr_t)((int32_t*)W[0x0CA4] + 2);
  ((int32_t*)W[0x0CA4])[1] = 0;
  iVar7 = param_1 / 0x18000 + (param_3 / 0x18000) * 8;
  local_a = g_terrain_bitmask_ptr + iVar7 * 0x10;
  /* ROM table at 0x8B200: 32 x 4-byte pointers to terrain chunk descriptors.
   * Each descriptor has 5 x 32-bit BE values: offset, grid_x, grid_z, dx, dz.
   * Ghidra read a byte instead of 32-bit, and used (int) casts that truncate on 64-bit. */
  {
    int _tbl_idx = (param_4 + 0x400U & 0xffff) >> 0xb;
    int _chunk_base = ROM_READ32(0x8B200 + _tbl_idx * 4);  /* ROM addr of chunk list */
    #define CHUNK_I32(n) ROM_READ32(_chunk_base + _chunk_iter * 20 + (n) * 4)
    int _chunk_iter;

  g_camera_offset_y = -param_2;
  g_visible_chunk_count = 0;
  iVar8 = 0;
  _chunk_iter = 0;
  do {
    iVar1 = CHUNK_I32(0) + iVar7;
    iVar2 = g_terrain_base_addr + iVar1;
    uVar3 = param_1 / 0x18000 + CHUNK_I32(1);
    iVar4 = param_3 / 0x18000 + CHUNK_I32(2);
    iVar5 = CHUNK_I32(3) - param_1 % 0x18000;
    iVar6 = CHUNK_I32(4) - param_3 % 0x18000;
    if ((((-1 < (int)uVar3) && ((int)uVar3 < 8)) && (-1 < iVar4)) &&
       ((iVar4 < 0x10 &&
        (local_6 = (uint8_t)(0x80 >> (uVar3 & 7)),
         /* Fix: local_a is a ROM address; read bitmask byte from ROM.
          * Original: **(uint8_t**)((int)&local_a + iVar4) — 64-bit truncation bug.
          * Each entry in the bitmask table is 0x10 bytes apart (set at line 478). */
         (local_6 & ((local_a + iVar4 >= 0 && local_a + iVar4 < ROM_SIZE) ?
                      R[local_a + iVar4] : 0)) != 0)
        ))) {
      /* STRIDE 0x10. Verified against the machine at all five sites: the
       * producer indexes `$e00e6c(d0.l)` after `lsl.l #4,d0` (0x00AC72), and
       * every consumer walks with `lea $10(a2),a2` or the same lsl #4 --
       * objects_render_attract_mode 0x014686, stage_render_environment
       * 0x016042, balloon_render_and_hit_check 0x0119D2, scenery_render_all_lod
       * 0x0140E0. At stride 4 the records OVERLAPPED (record N+1's chunk id
       * landed on record N's X field), which is why scenery_object_render
       * could not read an object's position from +4/+8 and the starting
       * platform drew at the wrong X/Z. */
      W[0x0E6C + (g_visible_chunk_count * 0x10)] = iVar1;
      W[0x0E70 + (g_visible_chunk_count * 0x10)] = iVar5;
      W[0x0E74 + (g_visible_chunk_count * 0x10)] = iVar6;
      (&g_chunk_lookup_table)[iVar1] = (char)g_visible_chunk_count;
      g_visible_chunk_count = g_visible_chunk_count + 1;
      *piVar9 = iVar2;
      ((int32_t*)(intptr_t)piVar9)[1] = iVar5;
      piVar11 = piVar9 + 3;
      ((int32_t*)(intptr_t)piVar9)[2] = 0xa000 - param_2;
      piVar9 = piVar9 + 4;
      *piVar11 = iVar6;
    }
    iVar8 = iVar8 + 1;
    _chunk_iter = _chunk_iter + 1;
  } while (iVar8 < 0x3c);
  #undef CHUNK_I32
  }  /* end ROM table block */
  W[0x0CA4] = piVar9;
  chunk_lookup_mirror();
  return;
}


/* ---- terrain_lod_reset ---- */

void terrain_lod_reset(void)

{
  g_terrain_needs_update = 0;
  W[0x12FC] = 0;
  W[0x1300] = 0;
  W[0x09A4] = 0;
  W[0x09A8] = 0;
  W[0x09AC] = 0;
  W[0x09B0] = 0;
  return;
}


/* ---- terrain_props_dispatch ---- */
/* MOVED to src/game_props.c, together with the four per-course routines the
 * M68K's computed jump at 0x004F6E actually reaches. The stub that used to
 * live here called terrain_lod_update_wrapper() in place of that jump --
 * which gameplay_tick already calls directly, so it was a duplicate call
 * standing in for 1708 bytes of prop placement that Ghidra never
 * disassembled. See the banner in game_props.c. */


/* ---- world_highscore_3d_draw ---- */

void world_highscore_3d_draw(void)

{
  /* ROM 0x034236 -- SOLITAR's 3D record board: the best NOVICE ranking
   * entry 0xE040A0 (score in the low 24 bits, flag byte in the top 8, name
   * bytes at 0xE040A4) drawn as three letter models (BE32 table 0x39C70,
   * indexed by the signed name byte * 4) and five digit models, at the eight
   * BE32 x/y/z triples of 0x39D88, gated on chunk 90's visibility byte
   * 0xE0128E (`cmpi.b #$ff ; beq` -- skip when 0xFF). Every record carries the
   * fixed pairs at 0x20DCD0/0x20C204/0x20B004 as WORDS (`movea.w`).
   * The transpile read all of it one BYTE at a time or through host pointers,
   * so nothing reached the list (register row 186).
   * The digit model is `divs.l d0,d1` at 0x03432E with Dr == Dq -- a plain
   * quotient, d0 keeps its 10 -- so the ROM draws model 0x47b + 10 for every
   * digit; only the flag byte 0x7F (no record) switches to 0x492. Faithful. */
  int32_t dy = ((int32_t)W[0x0CBC] == 1 || (int32_t)W[0x0CBC] == 5) ? -0xf027 : 0;
  int32_t flag = ((uint32_t)W[0x40A0] >> 24) & 0xff;
  uint32_t tbl = 0x39D88;
  int32_t *dl;
  int i;
  if (((uint32_t)W[0x128E] & 0xff) == 0xff) return;
  dl = (int32_t *)W[0x0CA4];
  *dl++ = 0x8002; *dl++ = 0;
  *dl++ = 0x8010; *dl++ = 3; *dl++ = -0x1a00; *dl++ = -1;
  for (i = 0; i < 8; i++) {
    int32_t model;
    if (i < 3) model = vrd32s(0x39C70 + (int32_t)(int8_t)W[0x40A4 + i] * 4);
    else       model = (flag == 0x7f) ? 0x492 : 0x47b + 10;
    *dl++ = model;
    *dl++ = vrd32s(tbl)     - (int32_t)W[0x0CDC];
    *dl++ = vrd32s(tbl + 4) - (int32_t)W[0x0CE0] + dy;
    *dl++ = vrd32s(tbl + 8) - (int32_t)W[0x0CE4];
    tbl += 12;
    *dl++ = vrd16s(0x20DCD0); *dl++ = vrd16s(0x20DCD2);
    *dl++ = vrd16s(0x20C204); *dl++ = vrd16s(0x20C206);
    *dl++ = vrd16s(0x20B004); *dl++ = vrd16s(0x20B006);
    *dl++ = 0;
  }
  *dl++ = 0x8010; *dl++ = -1;
  W[0x0CA4] = (intptr_t)dl;
  return;
}


/* ---- world_objects_tick_all ---- */

void world_objects_tick_all(void)

{
  int iVar1;
  
  for (iVar1 = 0; iVar1 < W[0x1308]; iVar1 = iVar1 + 1) {
    world_object_tick(iVar1);
  }
  return;
}


/* ---- world_render_all ---- */

int g_coll_dbg = 0;          /* PROPCYCL_COLLDBG=1 */
unsigned g_coll_ran, g_coll_gated;
unsigned g_c1324_zero, g_c1324_nz;

void world_render_all(void)

{
  if (g_coll_dbg) { if (W[0x0E38] == 0) g_coll_ran++; else g_coll_gated++;
    { extern unsigned g_c1324_zero, g_c1324_nz;
      if (_W[0x1324] == 0) g_c1324_zero++; else g_c1324_nz++; }
    if ((g_sys.frame_count % 60) == 0)
      printf("[COLL] f%u E38=%ld ran=%u probes=%ld px=%ld py=%ld pz=%ld "
             "probe=(%ld,%ld,%ld) hit=%ld\n",
             g_sys.frame_count, (long)_W[0x0E38], g_coll_ran, (long)_W[0x1308],
             (long)_W[0x0D00], (long)_W[0x0D04], (long)_W[0x0D08],
             (long)_W[0x1318], (long)_W[0x131C], (long)_W[0x1320],
             (long)_W[0x1324]);
      printf("[COLL]   timer W[0x0E44]=%ld  W[0x0E48]=%ld  state=%ld sub=%ld\n",
             (long)_W[0x0E44], (long)_W[0x0E48], (long)_W[0x0CBC], (long)_W[0x0CC0]);
      printf("[COLL]   W[0x1324]: zero=%u nonzero=%u\n", g_c1324_zero, g_c1324_nz); }
  if (W[0x0E38] == 0) {
    world_stage_update();
    world_render_objects();
    world_render_terrain();
    world_render_props();
    world_render_sky();
  }
  return;
}



/* ---- environment_zone_special_event @0x0286BA ----
 *
 * Runs every frame the camera's cell is a SPECIAL zone (transition frames
 * < 0 in the 0x376E0 table: zones 4, 9, 14, 19, 24, 29). The transpile had
 * an empty body -- Ghidra could not recover the jump table:
 *
 *   0286DE divs.l #5,d0 ; subq.l #5,d0 ; bhi $287ba   ; only zone/5 in 0..5
 *   0286E8 move.w $286fc(pc,d0.w*2),d0                ; ext 0x0212: SCALE 2,
 *   0286EE jmp    $286f2(pc,d0.w)                     ; d0 REBASED by -5, so
 *                                                     ; the table is 0x286F2
 *   0x286F2 = [000C 0030 0030 0030 0030 0034]
 *     q0 -> 0x0286FE   course 0 only: zone 5 (the cave's dark-blue preset 4)
 *                      while the camera is below y 0x13400 and west of
 *                      x 0x58000, zone 0 otherwise. Course 0's zone-4 cells
 *                      are 90, 98 and 99 -- the tunnel mouths.
 *     q1..q4 -> 0x028722   no change
 *     q5 -> 0x028726   course 2 only: zone 0xF (preset 2) or 0x19 (preset 8)
 *                      by camera cell (0x4C / 0x29 / other), camera X and
 *                      heading. The 0x028754 arm re-tests cell 0x4C and so
 *                      can never be taken; it is kept out, see below.
 * then 0x0287BA: if the target differs from W16[0x16A36], set_direct(the
 * target's 0x376E0 pair). The ROM does NOT store the target into 0x16A36:
 * while it differs, the transition is re-armed every frame.
 * All camera comparisons are UNSIGNED (bcc/bls/bhi/bcs). */

void environment_zone_special_event(void)
{
  int target = ENVW(0x16A36);                        /* d2 */
  int zone = vrd8s((int32_t)W[0x16A3C] + (int16_t)ENVW(0x16A38));
  int q = zone / 5;                                  /* divs.l truncates */

  if ((uint32_t)q <= 5) {
    uint32_t cx = (uint32_t)W[0x0CDC];               /* (a1), a1 = 0xE00CDC */
    uint32_t cy = (uint32_t)W[0x0CE0];               /* $4(a1) */
    int32_t cell = (int32_t)W[0x0CF4];               /* $18(a1) */
    uint32_t hdg = (uint32_t)W[0x0CEC];              /* $10(a1) */
    switch (q) {
    case 0:                                          /* 0x0286FE */
      if ((int32_t)W[0x0E0C] == 0) {
        target = (cy < 0x13400 && cx < 0x58000) ? 5 : 0;
      }
      break;
    case 5:                                          /* 0x028726 */
      if ((int32_t)W[0x0E0C] == 2) {
        if (cell == 0x4C) {                          /* 0x02873A */
          target = (cx <= 0x6C000 || (hdg & 0xFFFF) < 0x8000) ? 0xF : 0x19;
        }
        /* 0x028754 compares cell with 0x4C AGAIN and falls through to 0x29
         * when unequal, so its body (0x02875C: 0x4000+hdg < 0x8000 -> 0x19)
         * is unreachable on the machine as well. Not ported. */
        else if (cell == 0x29) {                     /* 0x02877C */
          if (cx > 0x27000) target = 0xF;
          else target = (((0x4000 + hdg) & 0xFFFF) >= 0x8000) ? 0x19 : 0xF;
        }
        else {                                       /* 0x02879C */
          target = (((0x2000 + hdg) & 0xFFFF) >= 0x8000) ? 0xF : 0x19;
        }
      }
      break;
    default:                                         /* q1..q4: 0x028722 */
      break;
    }
  }
  if ((int16_t)target != (int16_t)ENVW(0x16A36)) {  /* cmp.w $e16a36,d2 */
    uint32_t e = 0x376E0 + (int16_t)target * 4;      /* lea $376e0(d2.w*4) */
    environment_params_set_direct(vrd16s(e), vrd16s(e + 2));
  }
}

/* ---- environment_zone_transition @0x028614 ----
 *
 * Runs when the camera changes cell. ROM:
 *
 *   W16[0x16A38] = cell ; zone = (int8)table[cell]
 *   if zone == W16[0x16A36]: return             (0x028640 beq -> 0x0286B2)
 *   e = 0x376E0 + zone*4
 *   if W16(e+2) >= 0: set_direct(W16(e), W16(e+2))       ; 0x0286A6
 *   else dispatch on zone/5 through the table at 0x02866C (same rebased,
 *        scale-2 shape as above) = [000C 000E 0026 0026 0028 0038]:
 *     q0 -> 0x028678, q2/q3 -> 0x028692, q5 -> 0x0286A4 : nothing
 *     q1 -> 0x02867A : previous zone == 5  ? set_direct(0, 0x32) : set_direct(4, 0x1E)
 *     q4 -> 0x028694 : previous zone == 12 ? set_direct(4, 0x1E) : set_direct(4, 0x50)
 *   W16[0x16A36] = zone                          (0x0286B0, every path)
 *
 * The transpile's special branch was an empty "env zone dispatch" + return,
 * which also skipped the 0x16A36 store, and its NORMAL branch passed ONE
 * argument -- the duration word, read host-native (30 = 0x001E came back as
 * 0x1E00 = 7680) -- as the PRESET, so every zone change loaded fog colour
 * from `0x37684 + 76800`, ROM far outside the preset table. */

void environment_zone_transition(void)
{
  int cell = (int32_t)W[0x0CF4];
  int zone, prev;
  uint32_t e;

  ENVW_SET(0x16A38, cell);                           /* move.w d0,$e16a38 */
  zone = vrd8s((int32_t)W[0x16A3C] + (int16_t)cell); /* move.b (a0,d0.w) ; ext.w */
  prev = ENVW(0x16A36);
  if ((int16_t)zone == (int16_t)prev) return;
  e = 0x376E0 + (int16_t)zone * 4;
  if (vrd16s(e + 2) >= 0) {
    environment_params_set_direct(vrd16s(e), vrd16s(e + 2));
  }
  else {
    switch (zone / 5) {                              /* only 0..5 reach the table */
    case 1:                                          /* 0x02867A */
      if (prev == 5) environment_params_set_direct(0, 0x32);
      else           environment_params_set_direct(4, 0x1E);
      break;
    case 4:                                          /* 0x028694 */
      if (prev == 12) environment_params_set_direct(4, 0x1E);
      else            environment_params_set_direct(4, 0x50);
      break;
    default:                                         /* q0, q2, q3, q5, >5 */
      break;
    }
  }
  ENVW_SET(0x16A36, zone);
}

/* ---- world_object_tick ---- */

/* PER-LAYER HISTORY TABLES ARE 4-BYTE STRIDED (register row 99).
 * ROM: world_object_tick @0x004196 `cmp.l ($e028f0,D1.l*4),D0`, and
 * terrain_cell_history_store @0x004314 `addq.l #1,(A0,D0.l*4)` with A0 =
 * 0xE028F0 (the cached-triangle COUNT per layer, capped at 2) and A4 =
 * 0xE028FC (the ring index). world_props_init_dispatch zeroes exactly those
 * six longs. The transpile indexed both `W[0x28F0 + layer]` -- stride ONE --
 * so layers 1 and 2 read their count from slots 0x28F1/0x28F2, which are
 * 1-mod-4 / 2-mod-4 and get rebuilt from neighbouring bytes by the _W[] sync.
 * Measured at the cell 27->35 crossing (live f720): layer 2's count read 120,
 * the point test walked cache entries that never existed (record bases like
 * -926444288) and "found" a ceiling at height -3966 under the ground; two
 * faces where the machine sees one flipped the parity test below, every probe
 * became 'inside solid', the escape search failed and the bike was reset --
 * the invisible wall in the air. Five sites, this file. */

void world_object_tick(int param_1)

{
  undefined4 uVar1;
  undefined4 uVar2;
  int bVar3;
  int iVar4;
  int iVar5;
  undefined4 local_8;
  
  uVar1 = W[0x1318 + (param_1 * 0x140)];
  uVar2 = W[0x1320 + (param_1 * 0x140)];
  W[0x0974] = 0;
  W[0x096C] = 0;
  _safety_ctr = 0;
  do {
    /* Bound by the REAL layer capacity (12) as well as the count: W[0x0964]
     * has been observed at 130, which an int8_t producer cannot yield --
     * it is corruption from the unconverted (&W[base])[N] stride writes in
     * this file. Every per-layer array downstream holds 12 entries. */
    if (W[0x0964] <= (int)W[0x096C] || (int)W[0x096C] >= 12) {
      terrain_cell_sort_filter();
      iVar4 = W[0x0974];
      if (W[0x0E0C] == 3) {
        iVar4 = W[0x0974] + 1;
      }
      if ((iVar4 == 1) || (iVar4 == 3)) {
        local_8 = 0;
      }
      else {
        local_8 = 1;
      }
      W[0x1330 + (param_1 * 0x140)] = W[0x0974];
      W[0x13E8 + (param_1 * 0x140)] = local_8;
      if (g_tunlog && param_1 == 0 && (g_sys.frame_count % (unsigned)g_tunlog) == 0) {
        char b[256]; int o = 0, k;
        o += snprintf(b+o, sizeof b-o, "[TUN] f%u cell=%ld layers=%ld mask=0x%02lX faces=%ld:",
                      g_sys.frame_count, (long)W[0x0D24], (long)W[0x0964],
                      (long)W[0x0968] & 0xff, (long)W[0x0974]);
        for (k = 0; k < (int)W[0x0974] && k < 12 && o < (int)sizeof b - 32; k++)
          o += snprintf(b+o, sizeof b-o, " %s@%ld",
                        ((int)W[0x0978 + k * g_facestride] & 0xf) == 4 ? "PASS" : "SOLID",
                        (long)((int32_t)W[0x0978 + k * g_facestride] >> 4));
        fprintf(stderr, "%s  parity=%s\n", b, local_8 ? "INSIDE-SOLID" : "open-air");
      }
      for (iVar4 = 0; iVar4 < W[0x0974]; iVar4 = iVar4 + 1) {
        W[0x1334 + (param_1 * 0x140 + iVar4 * 4)] = W[0x0978 + (iVar4 * g_facestride)];
        iVar5 = iVar4 + param_1 * 0x140;
        W[0x13AC + (iVar5)] = (char)W[0x0984 + (iVar4 * g_facestride)];
        W[0x13CA + (iVar5)] = 0;
      }
      return;
    }
    if ((W[0x0968] >> (W[0x096C] & 0x3f) & 1U) == 0) {
      iVar4 = 0xc;
    }
    else {
      iVar4 = 4;
    }
    bVar3 = false;
    for (W[0x0970] = 0; W[0x0970] < (int)W[0x28F0 + W[0x096C] * 4];
        W[0x0970] = W[0x0970] + 1) {
      terrain_cell_history_load();
      iVar5 = terrain_cell_point_test(uVar1,uVar2);
      if (g_tunlog && param_1 == 0 && (g_sys.frame_count % (unsigned)g_tunlog) == 0)
        fprintf(stderr, "[TUNWALK]   layer %ld kind %d: point_test[hist %ld] = %d\n",
                (long)W[0x096C], iVar4, (long)W[0x0970], iVar5);
      if (g_wrt_dbg && g_sys.frame_count == (unsigned)g_wot_frame && param_1 == 0)
          fprintf(stderr, "[WOT] f720 layer0-probe terrain-layer %ld kind %d hist %ld: point_test -> %d (W095C=%ld) probe=(%ld,%ld,%ld)\n",
                  (long)W[0x096C], iVar4, (long)W[0x0970], iVar5, (long)W[0x095C], (long)uVar1, (long)W[0x131C], (long)uVar2);
      if (iVar5 != -1) {
        bVar3 = true;
        if (iVar5 != -2) {
          W[0x0978 + (W[0x0974] * g_facestride)] = iVar4 + iVar5 * 0x10;
          W[0x0984 + (W[0x0974] * g_facestride)] = W[0x095C];
          W[0x0974] = W[0x0974] + 1;
        }
      }
      if (bVar3) break;
    }
    if (!bVar3) {
      if (g_wrt_dbg && g_sys.frame_count == (unsigned)g_wot_frame && param_1 == 0) { int _k, _n = 0; long _L = (long)W[0x096C];
          fprintf(stderr, "[WOT]   layer %ld lookup grid (key: faceidx) near the probe:", _L);
          for (_k = 0; _k < 256; _k++) { int _v = (int)(int8_t)W[0x0014 + _L*0x100 + _k]; if (_v != -1) { _n++; if ((_k % 16) >= 6 && (_k % 16) <= 12 && (_k / 16) <= 1) fprintf(stderr, " %d:%d", _k, _v); } }
          fprintf(stderr, "  (%d non-empty)  W0920..0934=%ld,%ld,%ld,%ld,%ld,%ld W0950=%ld\n", _n,
                  (long)W[0x0920], (long)W[0x0924], (long)W[0x0928], (long)W[0x092C], (long)W[0x0930], (long)W[0x0934], (long)W[0x0950]); }
      iVar5 = terrain_cell_find_triangle(uVar1,uVar2);
      if (g_tunlog && param_1 == 0 && (g_sys.frame_count % (unsigned)g_tunlog) == 0)
        fprintf(stderr, "[TUNWALK]   layer %ld kind %d: hist=%ld point_test miss -> find_triangle = %d"
                "  (seed W[0x0914+L*4]=%ld, cachedTris=%ld)\n",
                (long)W[0x096C], iVar4, (long)W[0x0970], iVar5,
                (long)(int8_t)W[0x0914 + W[0x096C]*4], (long)W[0x28F0 + W[0x096C]*4]);
      if (g_wrt_dbg && g_sys.frame_count == (unsigned)g_wot_frame && param_1 == 0)
          fprintf(stderr, "[WOT] f720 layer0-probe terrain-layer %ld kind %d: find_triangle -> %d (W095C=%ld) face W0950=%ld slopes %ld,%ld base W0920..=%ld,%ld\n",
                  (long)W[0x096C], iVar4, iVar5, (long)W[0x095C], (long)W[0x0950], (long)W[0x0954], (long)W[0x0958], (long)W[0x0920], (long)W[0x0924]);
      if (iVar5 != -2) {
        W[0x0978 + (W[0x0974] * g_facestride)] = iVar4 + iVar5 * 0x10;
        W[0x0984 + (W[0x0974] * g_facestride)] = W[0x095C];
        W[0x0974] = W[0x0974] + 1;
      }
      terrain_cell_history_store();
    }
    W[0x096C] = W[0x096C] + 1;
    if (W[0x096C] > 100) return;  /* safety: prevent infinite loop with stubbed terrain */
  if (++_safety_ctr > 10000) break; } while( true ); _safety_ctr = 0;
}

/* ---- world_render_objects ---- */

void world_render_objects(void)

{
  world_objects_init_player();
  world_objects_tick_all();
  world_collision_zones_process();
  terrain_height_resolve();
  world_objects_copy_prev_state();
  terrain_collision_zone_resolve();
  world_objects_transition_check();
  return;
}

/* ---- world_render_props ---- */

void world_render_props(void)

{
  W[0x2AA0] = W[0x130C];
  W[0x2AA4] = W[0x1314];
  W[0x2AA8] = W[0x1328];
  W[0x2A88] = W[0x144C];
  W[0x2A8C] = W[0x1454];
  W[0x2A90] = W[0x1468];
  W[0x2A94] = W[0x158C];
  W[0x2A98] = W[0x1594];
  W[0x2A9C] = W[0x15A8];
  W[0x2A7C] = W[0x16CC];
  W[0x2A80] = W[0x16D4];
  W[0x2A84] = W[0x16E8];
  W[0x2A70] = W[0x180C];
  W[0x2A74] = W[0x1814];
  W[0x2A78] = W[0x1828];
  if (0x14 < W[0x12D8]) {
    W[0x2A84] = W[0x16DC];
    if (0x800 < W[0x131C] - W[0x1328]) {
      W[0x2AA8] = W[0x131C] + -0x800;
    }
    if (0x800 < W[0x145C] - W[0x1468]) {
      W[0x2A90] = W[0x145C] + -0x800;
    }
    if (0x800 < W[0x159C] - W[0x15A8]) {
      W[0x2A9C] = W[0x159C] + -0x800;
    }
  }
  return;
}

/* ---- world_render_sky ---- */

void world_render_sky(void)

{
  W[0x2ADC] = W[0x130C];
  W[0x2AE0] = W[0x1314];
  W[0x2AE4] = W[0x1310];
  W[0x2AC4] = W[0x144C];
  W[0x2AC8] = W[0x1454];
  W[0x2ACC] = W[0x1450];
  W[0x2AD0] = W[0x158C];
  W[0x2AD4] = W[0x1594];
  W[0x2AD8] = W[0x1590];
  W[0x2AB8] = W[0x16CC];
  W[0x2ABC] = W[0x16D4];
  W[0x2AC0] = W[0x16D0];
  W[0x2AAC] = W[0x180C];
  W[0x2AB0] = W[0x1814];
  W[0x2AB4] = W[0x1810];
  W[0x2AE8] = W[0x1324];
  W[0x2AEC] = W[0x1464];
  W[0x2AF0] = W[0x15A4];
  W[0x2AF4] = W[0x16E4];
  W[0x2AF8] = W[0x1824];
  W[0x2B10] = W[0x132C];
  W[0x2B14] = W[0x146C];
  W[0x2B18] = W[0x15AC];
  W[0x2B1C] = W[0x16EC];
  W[0x2B20] = W[0x182C];
  return;
}

/* ---- world_stage_update ---- */

void world_stage_update(void)

{
  if (W[0x0D24] != W[0x0010]) {
    W[0x0990] = W[0x0D24];
    W[0x0994] = W[0x0D24] & 7;
    W[0x0998] = (int)W[0x0D24] >> 3;
    W[0x099C] = W[0x0994] * 0x18000;
    W[0x09A0] = W[0x0998] * 0x18000;
    /* ROM table lookup: W[0x2910] and W[0x2914] hold ROM base addresses */
    {
      int addr1 = (int)W[0x2910] + (int)W[0x0D24];
      int addr2 = (int)W[0x2914] + (int)W[0x0D24];
      W[0x0964] = (addr1 >= 0 && addr1 < ROM_SIZE) ? (int)(int8_t)R[addr1] : 0;
      /* THE REAL LAYER CAP IS THREE, not the 12 assumed here for years.
       * Two independent proofs. (a) Memory layout: the tile->face mask is
       * 3 x 0x100 bytes at 0x0014 and ends exactly at 0x0314, where the
       * per-layer face-pointer table starts; that is 3 x 0x200 (ROM
       * 0x004526, `$e00314(a0, d3.l*4)` with a0 = layer << 9) and ends
       * exactly at 0x0914, the per-layer start-face cache. Three layers fit
       * with nothing left over. (b) Data: a census of the per-cell layer
       * count over all four courses' tables (0x0B971C/9C/981C/989C) gives a
       * maximum of 3 -- course 0 max 3, course 1 max 3, course 2 max 3,
       * course 3 max 2. A count of 4 would have layer 3's face pointers
       * overwrite the start-face cache. */
      if (W[0x0964] < 0 || W[0x0964] > (g_layerfix ? 3 : 12)) {
        if (W[0x0964] != 0) {
          static int warned;
          if (warned++ == 0 || propcycl_verbose())
            fprintf(stderr, "[GUARD] world_stage_update: layer count %d (cell %d) "
                    "above the 3 the memory layout allows -- clamped\n",
                    (int)W[0x0964], (int)W[0x0D24]);
        }
        W[0x0964] = (W[0x0964] < 0) ? 0 : (g_layerfix ? 3 : 0);
      }
      W[0x0968] = (addr2 >= 0 && addr2 < ROM_SIZE) ? (int)(int8_t)R[addr2] : 0;
      /* Layer-table layout caps layers at 12 (face ptrs W[0x0314 +
       * layer*0x80] run into the status array at W[0x0914] beyond
       * that). Counts of 12/16 observed in the attract demo replay —
       * either an out-of-grid cell index from the replay block or a
       * mis-decoded height map; producer needs L2 verification vs MAME
       * (tools/l3/BASELINE.md burn-down). Clamp loudly per G3. */
      if (W[0x0964] > 12 || W[0x0964] < 0) {
        static int warned;
        if (warned++ == 0 || propcycl_verbose())
              fprintf(stderr, "[GUARD] world_stage_update: layer count %d "
                  "(cell %d) out of range — clamped to 0\n",
                  (int)W[0x0964], (int)W[0x0D24]);
        W[0x0964] = 0;
      }
    }
    world_stage_geometry_load();
    W[0x0010] = W[0x0990];
    W[0x28F0] = 0;
    W[0x28F4] = 0;
    W[0x28F8] = 0;
    W[0x28FC] = 0;
    W[0x2900] = 0;
    W[0x2904] = 0;
  }
  world_stage_render(W[0x0D00],W[0x0D08]);
  return;
}

/* ========== 3 MISSING POINTER-RETURN FUNCTIONS ==========*/

/* ---- environment_params_set_direct @0x0285DC ----
 *
 * Arm a fade to `preset` over `frames` frames; environment_zone_tick walks
 * the current colour/CZ delta toward the target. ROM:
 *   move.w $4(a7),d0 ; ext.l ; *10 ; addi.l #$37684 ; movea.l d0,a0
 *   move.w (a0)+,$e0aaf2 / $e0aaf4 / $e0aaf6 / $e0aaf8
 *   move.w $6(a7),$e0ab02                       <- the SECOND argument
 * TWO 16-bit arguments. The transpile took one and stored the PRESET into
 * 0xAB02 as the frame count, and wrote all five 16-bit fields as whole
 * slots (see the layout note above environment_params_load). */

void environment_params_set_direct(int preset, int frames)
{
  uint32_t a = 0x37684 + (int16_t)preset * 10;

  ENVW_SET(0xAAF2, vrd16s(a));
  ENVW_SET(0xAAF4, vrd16s(a + 2));
  ENVW_SET(0xAAF6, vrd16s(a + 4));
  ENVW_SET(0xAAF8, vrd16s(a + 6));
  ENVW_SET(0xAB02, frames);
}

/* THE PER-LAYER FACE-POINTER TABLE.
 *
 * ROM 0x00451E..0x004526:
 *     move.l d2,d0 ; moveq #$9,d1 ; lsl.l d1,d0 ; movea.l d0,a0
 *     move.l a1, $e00314(a0, d3.l*4)
 * so the layout is `0xE00314 + (layer << 9) + face*4` -- layer stride 0x200,
 * face index scaled by FOUR. The transpile had `0x0314 + face + layer*0x80`:
 * the layer stride quartered and the index scale dropped (capstone prints the
 * scale here only because extension word 0x3D30 is FULL format, bit 8 set --
 * the brief-format sites in the same function lose it, the trap CLAUDE.md
 * records).
 *
 * The layout is confirmed a second way, by adjacency: the tile->face mask is
 * 3 x 0x100 at 0x0014 and ends exactly at 0x0314; this table is 3 x 0x200 and
 * ends exactly at 0x0914, which is the per-layer start-face cache. So the
 * real layer cap is THREE, not the 12 the old comments assumed -- and a ROM
 * census agrees: across all four courses the per-cell layer count never
 * exceeds 3 (course 0 max 3, course 1 max 3, course 2 max 3, course 3 max 2).
 *
 * Both spellings are self-consistent producer-to-consumer, which is why this
 * survived every gate: nothing else reads the table. It matters because the
 * 0x80 stride lets layer 0 spill into layer 1's range once a cell has more
 * than 128 faces, and because it made the whole multi-layer region look like
 * it had been reasoned about when it had not. */
#define FACEPTR(layer, face)  W[0x0314 + (g_layerfix ? (((layer) << 9) + (face) * 4)   \
                                                     : ((face) + (layer) * 0x80))]

/* ---- terrain_cell_find_triangle ---- */

int terrain_cell_find_triangle(int param_1,int param_2)

{
  short *psVar1;
  char *pcVar2;
  uint8_t bVar3;
  uint8_t bVar4;
  uint8_t bVar5;
  uint8_t bVar6;
  uint16_t uVar7;
  int iVar8;
  int iVar9;
  int iVar10;
  int iVar11;
  int bVar12;
  short sVar13;
  uint32_t uVar14;
  int iVar15;
  int iVar16;
  short sVar20;
  undefined4 uVar17;
  int iVar18;
  int iVar19;
  uint32_t uVar21;
  uint32_t uVar22;
  uint8_t *pbVar23;
  uint8_t *pbVar24;
  uint8_t *pbVar25;
  uint8_t *pbVar26;
  uint8_t *pbVar27;
  int local_a0;
  int local_94;
  int local_90;
  int local_8c;
  int local_88;
  /* THE THIRD VERTEX IS ASSIGNED ON ONE PATH ONLY, AND ON THE MACHINE IT IS
   * ZERO WHEN THAT PATH IS NOT TAKEN (register row 106).
   *
   * It is written at ROM 0x004A00/0x004A10, in the `else` of the
   * `*pbVar24 == local_7c` test below, and the walk can run all ten of its
   * iterations without ever taking that branch. The vertex store at the
   * bottom reads the slots anyway -- one branch reaches it (`bra $4c36` at
   * 0x004B52) and it is taken on the failure path too.
   *
   * Measured, not assumed: a breakpoint on the real machine at 0x004C36
   * (tools/overnight/walk_trace.lua) shows the ROM taking the
   * "wrapped-to-start, no v2" exit on EVERY call at this probe and storing
   * **v2 = (0, 0)** -- and still returning a correct height (51333, 49778,
   * 47664 against a resolved floor of ~49500), because v2 is not in the
   * height formula at all. It only reaches the cached triangle for a later
   * `terrain_cell_point_test`. So zero is the faithful value; leaving these
   * indeterminate put -513452310 in the cache. */
  int local_84 = 0, local_80 = 0; int v2_set = 0;
  uint32_t local_7c;
  int local_78;
  int local_74;
  uint32_t local_70;
  uint32_t local_6c;
  uint8_t *local_64;
  int local_60;
  int local_5c;
  int local_8;
  
  bVar12 = false;
  /* `layer * 4` -- ROM 0x004668 `move.l ($e00914,D0.l*4),(-$78,A6)`. At stride
   * one, layer 2's entry landed on slot 0x0916, a 2-mod-4 offset the `_W[]`
   * sync zeroes every frame (register row 106's class). */
  uVar21 = W[0x0914 + W[0x096C] * 4];
  /* world_stage_render writes 0xFF here when no face is found for this
   * layer. world_stage_geometry_load only populates W[0x0314+layer*0x80
   * + face_index] for face_index in [0, primary+secondary) — slot 0xFF
   * is uninitialised, so dereferencing pcVar2 below would walk a stale
   * or bogus pointer. Bail out with the same "no face" output local_6c
   * uses elsewhere in this function (lines 1163-1168). */
  if ((uint8_t)uVar21 == 0xFF) {
    W[0x0950] = 0xff;
    W[0x0954] = 0;
    W[0x0958] = 0;
    W[0x095C] = 0;
    if (g_tunlog) fprintf(stderr, "[TUNFT]   find_triangle bail: seed-is-0xFF (layer=%ld seed=%ld)\n", (long)W[0x096C], (long)(uint8_t)uVar21);
    return -2;
  }
  if (W[0x096C] > 11) {
    /* Layer index out of range: the face-pointer table spans layers
     * 0-11 (W[0x0314 + layer*0x80]; layer 12 lands exactly on the
     * 0xFF status array at W[0x0914], whose byte then gets used as a
     * host pointer -> SIGSEGV). First hit: attract demo replay via
     * world_object_tick at fc~642. The PRODUCER of layer 12 is the
     * bug (W[0x096C] select in world_stage_update, fix #23 family) —
     * see tools/l3/BASELINE.md burn-down. Loud per GUARDRAILS G3. */
    static int warned;
    if (warned++ == 0 || propcycl_verbose())
      fprintf(stderr, "[GUARD] terrain_cell_find_triangle: layer %d out "
              "of range (max 11) — producer bug, see BASELINE.md\n",
              (int)W[0x096C]);
    W[0x0950] = 0xff;
    W[0x0954] = 0;
    W[0x0958] = 0;
    W[0x095C] = 0;
    if (g_tunlog) fprintf(stderr, "[TUNFT]   find_triangle bail: layer>11 (layer=%ld seed=%ld)\n", (long)W[0x096C], (long)(uint8_t)uVar21);
    return -2;
  }
  pcVar2 = (char *)FACEPTR(W[0x096C], uVar21);
  if (pcVar2 == NULL) {
    /* Face-slot pointer never populated (world_stage_geometry_load has
     * not loaded this cell/layer — first hit: attract demo replay via
     * world_object_tick, a path sub=3 never takes). Same "no face"
     * result as the 0xFF guard above. Loud per GUARDRAILS G3. */
    static int warned;
    if (warned++ == 0 || propcycl_verbose())
      fprintf(stderr, "[GUARD] terrain_cell_find_triangle: NULL face ptr "
              "(layer=%d slot=%d)\n", (int)W[0x096C], (int)(uint8_t)uVar21);
    W[0x0950] = 0xff;
    W[0x0954] = 0;
    W[0x0958] = 0;
    W[0x095C] = 0;
    if (g_tunlog) fprintf(stderr, "[TUNFT]   find_triangle bail: NULL-face-ptr (layer=%ld seed=%ld)\n", (long)W[0x096C], (long)(uint8_t)uVar21);
    return -2;
  }
  iVar15 = (int)*pcVar2;
  pbVar25 = (uint8_t *)(pcVar2 + 2);
  iVar16 = (int)pcVar2[1];
  bVar3 = *pbVar25;
  bVar4 = *pbVar25;
  W[0x0950] = (uint32_t)(uint8_t)pcVar2[3];
  iVar8 = iVar15 * 0x2000 + -0x2000;
  iVar9 = iVar16 * 0x2000 + -0x2000;
  iVar10 = param_1 - (W[0x099C] + iVar8);
  iVar11 = param_2 - (W[0x09A0] + iVar9);
  sVar20 = math_atan2(iVar10,iVar11);
  uVar17 = math_atan2_coarse(
              *(char *)FACEPTR(W[0x096C], (uint32_t)(uint8_t)pcVar2[4]) - iVar15,
              ((char *)FACEPTR(W[0x096C], (uint32_t)(uint8_t)pcVar2[4]))[1] - iVar16);
  local_a0 = -1;
  pbVar26 = (uint8_t *)(pcVar2 + 5);
  uVar22 = W[0x0950];
  uVar14 = (uint32_t)bVar4;
  pbVar25 = pbVar25 + bVar3;
  while ((sVar13 = sVar20, local_5c = iVar11, local_60 = iVar10, local_64 = pbVar25,
         local_70 = uVar14, local_74 = iVar16, local_78 = iVar15, local_7c = uVar21,
         local_90 = iVar9, local_94 = iVar8, W[0x0950] = uVar22, W[0x093C] = uVar17, !bVar12
         && (local_a0 = local_a0 + 1, pbVar27 = pbVar26, local_a0 < 10))) {
    {
      int _ang_safety = 0;
      do {
        W[0x0938] = W[0x093C];
        pbVar26 = pbVar27 + 1;
        local_6c = (uint32_t)*pbVar27;
        if (pbVar26 == local_64) {
          pbVar26 = pbVar26 + (int32_t)(2 - local_70);  /* CLAUDE.md fix #25 */
        }
        pbVar27 = pbVar26 + 1;
        uVar21 = (uint32_t)*pbVar26;
        pcVar2 = (char *)FACEPTR(W[0x096C], uVar21);
        if (g_wrt_dbg) {
            const unsigned char *_r = (const unsigned char *)pcVar2;
            fprintf(stderr, "[FT]   inner: faceidx=%u layer=%ld rec=%p bytes=%02X %02X %02X %02X %02X %02X\n",
                    (unsigned)uVar21, (long)W[0x096C], (void *)pcVar2,
                    _r?_r[0]:0, _r?_r[1]:0, _r?_r[2]:0, _r?_r[3]:0, _r?_r[4]:0, _r?_r[5]:0);
        }
        iVar15 = (int)*pcVar2;
        pbVar25 = (uint8_t *)(pcVar2 + 2);
        iVar16 = (int)pcVar2[1];
        W[0x093C] = math_atan2_coarse(iVar15 - local_78,iVar16 - local_74);
        if (++_ang_safety > 512) break;  /* CLAUDE.md fix #25: angle traversal cycle */
      } while ((uint16_t)((short)W[0x093C] - (short)W[0x0938]) <=
               (uint16_t)(sVar13 - (short)W[0x0938]));
    }
    local_8c = iVar15 * 0x2000 + -0x2000;
    local_88 = iVar16 * 0x2000 + -0x2000;
    if (g_wrt_dbg) fprintf(stderr, "[FT]   v1 <- (%d,%d)\n", local_8c, local_88);
    iVar10 = param_1 - (W[0x099C] + local_8c);
    iVar11 = param_2 - (W[0x09A0] + local_88);
    sVar20 = math_atan2(iVar10,iVar11);
    W[0x0940] = (uint32_t)(uint16_t)((short)W[0x093C] + 0x7ff6);
    bVar3 = *pbVar25;
    bVar4 = *pbVar25;
    bVar5 = pcVar2[3];
    {
      int _ns_safety = 0;
      for (pbVar23 = (uint8_t *)(pcVar2 + 4);
           *pbVar23 != local_7c && _ns_safety < 256;
           pbVar23 = pbVar23 + 2, _ns_safety++) {
      }
      /* CLAUDE.md fix #25: 256-iter neighbor search guard */
    }
    pbVar23 = pbVar23 + 2;
    if (pbVar23 == pbVar25 + bVar3) {
      pbVar23 = pbVar23 + (int32_t)(2 - (uint32_t)bVar4);  /* CLAUDE.md fix #25 */
    }
    pbVar26 = pbVar23 + 1;
    bVar6 = *pbVar23;
    pcVar2 = (char *)FACEPTR(W[0x096C], (uint32_t)bVar6);
    iVar18 = (int)*pcVar2;
    pbVar23 = (uint8_t *)(pcVar2 + 2);
    iVar19 = (int)pcVar2[1];
    W[0x0944] = math_atan2_coarse(iVar18 - iVar15,iVar19 - iVar16);
    uVar17 = W[0x0944];
    uVar22 = (uint32_t)bVar5;
    iVar8 = local_8c;
    iVar9 = local_88;
    uVar14 = (uint32_t)bVar4;
    pbVar25 = pbVar25 + bVar3;
    if ((uint16_t)(sVar20 - (short)W[0x0940]) < (uint16_t)((short)W[0x0944] - (short)W[0x0940])
       ) {
      bVar3 = *pbVar23;
      bVar4 = *pbVar23;
      bVar5 = pcVar2[3];
      {
        int _ns2_safety = 0;
        for (pbVar24 = (uint8_t *)(pcVar2 + 4);
             uVar21 != *pbVar24 && _ns2_safety < 256;
             pbVar24 = pbVar24 + 2, _ns2_safety++) {
        }
        /* CLAUDE.md fix #25: 256-iter neighbor search guard */
      }
      pbVar24 = pbVar24 + 2;
      if (pbVar24 == pbVar23 + bVar3) {
        pbVar24 = pbVar24 + (int32_t)(2 - (uint32_t)bVar4);  /* CLAUDE.md fix #25 */
      }
      pbVar26 = pbVar27;
      iVar8 = local_94;
      iVar9 = local_90;
      uVar21 = local_7c;
      iVar15 = local_78;
      iVar16 = local_74;
      uVar14 = local_70;
      pbVar25 = local_64;
      if (*pbVar24 == local_7c) {
        if (g_wrt_dbg) fprintf(stderr, "[FT]   inner: wrapped to start (face %d) -- v2 NOT assigned on this path\n", (int)local_7c);
        bVar12 = true;
        W[0x0960] = 0;
        uVar17 = W[0x093C];
        uVar22 = W[0x0950];
        iVar10 = local_60;
        iVar11 = local_5c;
        sVar20 = sVar13;
      }
      else {
        W[0x094C] = math_atan2_coarse(
                        *(char *)FACEPTR(W[0x096C], (uint32_t)*pbVar24) - iVar18,
                        ((char *)FACEPTR(W[0x096C], (uint32_t)*pbVar24))[1] - iVar19);
        local_84 = iVar18 * 0x2000 + -0x2000;
        local_80 = iVar19 * 0x2000 + -0x2000;
        v2_set = 1;
        if (g_wrt_dbg) fprintf(stderr, "[FT]   v2 <- (%d,%d)\n", local_84, local_80);
        iVar10 = param_1 - (W[0x099C] + local_84);
        iVar11 = param_2 - (W[0x09A0] + local_80);
        sVar20 = math_atan2(iVar10,iVar11);
        uVar7 = (short)W[0x0944] + 0x7ff6;
        W[0x0948] = (uint32_t)uVar7;
        if ((uint16_t)(sVar20 - uVar7) < (uint16_t)((short)W[0x094C] - uVar7)) {
          bVar12 = true;
          W[0x0960] = 1;
          uVar17 = W[0x093C];
          uVar22 = W[0x0950];
          iVar10 = local_60;
          iVar11 = local_5c;
          sVar20 = sVar13;
        }
        else {
          W[0x093C] = W[0x094C];
          pbVar26 = pbVar24 + 1;
          uVar17 = W[0x093C];
          uVar22 = (uint32_t)bVar5;
          iVar8 = local_84;
          iVar9 = local_80;
          uVar21 = (uint32_t)bVar6;
          iVar15 = iVar18;
          iVar16 = iVar19;
          uVar14 = (uint32_t)bVar4;
          pbVar25 = pbVar23 + bVar3;
        }
      }
    }
  }
  if (((uint8_t)local_78 < 0x10) && ((uint8_t)local_74 < 0x10)) {
    W[0x0914 + W[0x096C] * 4] = local_7c;   /* ROM 0x004B26, D0.l*4 */
  }
  if (local_6c == 0xff) {
    W[0x0950] = 0xff;
    W[0x0954] = 0;
    W[0x0958] = 0;
    local_8 = -2;
    W[0x095C] = 0;
  }
  else {
    /* Two-level ROM pointer chain — same shape as the slice-2 rewrite of
     * world_stage_geometry_load. W[0x290C] holds a ROM byte address
     * (cell_descriptor table base, populated by world_props_init_dispatch
     * from rom_stage_geometry); each level is a 32-bit BE pointer.
     * Without these reads bounds-checked we'd dereference ROM addresses
     * as host pointers and crash on 64-bit. */
    uint32_t lvl1 = (uint32_t)W[0x290C] + (uint32_t)W[0x0990] * 4u;
    if (lvl1 + 4u > ROM_SIZE) {
      W[0x095C] = 0; W[0x0954] = 0; W[0x0958] = 0;
      local_8 = W[0x0950] * 0x400;
    } else {
      uint32_t cell_face_tbl = mem_read32(lvl1);
      uint32_t lvl2 = cell_face_tbl + (uint32_t)W[0x096C] * 4u;
      if (lvl2 + 4u > ROM_SIZE) {
        W[0x095C] = 0; W[0x0954] = 0; W[0x0958] = 0;
        local_8 = W[0x0950] * 0x400;
      } else {
        uint32_t face_record_off = mem_read32(lvl2) + (uint32_t)local_6c * 4u;
        if (face_record_off + 4u > ROM_SIZE) {
          W[0x095C] = 0; W[0x0954] = 0; W[0x0958] = 0;
          local_8 = W[0x0950] * 0x400;
        } else {
          /* psVar1[0..1] are two BE16s: the top two bits of each carry the
           * face type, the low 14 bits index a RECIPROCAL table at 0x203004
           * that yields the slope. */
          uVar21 = (uint32_t)(uint16_t)vrd16(face_record_off);
          uVar22 = (uint32_t)(uint16_t)vrd16(face_record_off + 2);
          W[0x095C] = (int)(uVar22 & 0xc000) >> 0xe | (int)(uVar21 & 0xc000) >> 0xc;
          {
            /* THE TABLE HAS 16-BIT ENTRIES AND IS INDEXED BY (idx * 2), AND
             * THE VALUE IS USED AS-IS. ROM 0x004BD8..0x004C02:
             *     movea.l d5,a0 ; andi.l #$3fff,(a0)        (W[0x0954] &= 0x3fff)
             *     move.l (a0),d0
             *     movea.w $203004(d0.l*2),a1 ; move.l a1,(a0)
             * -- `movea.w` sign-extends the 16-bit entry and it is stored
             * unchanged; the same for W[0x0958] through d6.
             *
             * This read the table at idx*1 and then DOUBLED the value. The
             * table is a reciprocal ramp (idx 100 -> -26699, 400 -> -6663,
             * 8191 -> -1), so idx*1 fetched the entry for idx/2 -- already
             * double the magnitude -- and the *2 doubled it again; odd
             * indices straddled two entries. Measured in the face census:
             * slopes of -34822, -53766, +45052 where a sane one is a few
             * hundred, and one cached triangle with base 40 and BOTH slopes
             * exactly -1024 whose height point_test walked from 4942 down
             * through zero as the bike crossed it -- so the floor resolved
             * below the bike and it fell through the terrain. The row-27 S3
             * lost-*2 shape, on a table the trig sweep never covered. */
            uint32_t s1 = 0x203004 + (uVar21 & 0x3fff) * 2;
            uint32_t s2 = 0x203004 + (uVar22 & 0x3fff) * 2;
            W[0x0954] = (s1 + 2 <= ROM_SIZE) ? (int)(int16_t)vrd16(s1) : 0;
            W[0x0958] = (s2 + 2 <= ROM_SIZE) ? (int)(int16_t)vrd16(s2) : 0;
          }
          local_8 = W[0x0950] * 0x400
                  + (local_5c * W[0x0958] >> 9)
                  + (local_60 * W[0x0954] >> 9);
        }
      }
    }
    (void)psVar1;  /* original local kept for shape; fix uses ROM offsets */
  }
  if (g_wrt_dbg)
      fprintf(stderr, "[FT] store v0=(%d,%d) v1=(%d,%d) v2=(%d,%d)%s cellorg=(%ld,%ld) "
              "face=%ld local_6c=%d local_7c=%d ret=%d\n",
              local_94, local_90, local_8c, local_88, local_84, local_80,
              v2_set ? "" : " [v2 NEVER ASSIGNED]",
              (long)W[0x099C], (long)W[0x09A0], (long)W[0x0950], (int)local_6c, (int)local_7c,
              (int)local_8);
  if (!v2_set) {   /* report only -- the stale slot above IS the ROM's behaviour */
      static int warned;
      if (warned++ == 0 || g_wrt_dbg)
          fprintf(stderr, "[INFO] terrain_cell_find_triangle: walk left the third "
                  "vertex unassigned; reusing the previous call's (%d,%d) as the M68K "
                  "frame does (register row 106)\n", local_84, local_80);
  }
  W[0x0920] = local_94 + W[0x099C];
  W[0x0924] = local_90 + W[0x09A0];
  W[0x0928] = local_8c + W[0x099C];
  W[0x092C] = local_88 + W[0x09A0];
  W[0x0930] = local_84 + W[0x099C];
  W[0x0934] = local_80 + W[0x09A0];
  return local_8;
}

/* ---- terrain_cell_history_load ---- */

void terrain_cell_history_load(void)

{
  int iVar1;
  
  iVar1 = W[0x0970] * 0x50 + W[0x096C] * 800;
  W[0x0920] = W[0x1F90 + (iVar1)];
  W[0x0924] = W[0x1F94 + (iVar1)];
  W[0x0928] = W[0x1F98 + (iVar1)];
  W[0x092C] = W[0x1F9C + (iVar1)];
  W[0x0930] = W[0x1FA0 + (iVar1)];
  W[0x0934] = W[0x1FA4 + (iVar1)];
  W[0x093C] = W[0x1FA8 + (iVar1)];
  W[0x0938] = W[0x1FAC + (iVar1)];
  W[0x0944] = W[0x1FB0 + (iVar1)];
  W[0x0940] = W[0x1FB4 + (iVar1)];
  W[0x094C] = W[0x1FB8 + (iVar1)];
  W[0x0948] = W[0x1FBC + (iVar1)];
  W[0x0950] = W[0x1FC0 + (iVar1)];
  W[0x0954] = W[0x1FC4 + (iVar1)];
  W[0x0958] = W[0x1FC8 + (iVar1)];
  W[0x095C] = W[0x1FCC + (iVar1)];
  W[0x0960] = W[0x1FD0 + (iVar1)];
  return;
}

/* ---- terrain_cell_history_store ---- */

int terrain_cell_history_store(void)

{
  int iVar1;
  int iVar2;
  
  /* LAYER RANGE GUARD. The per-layer history arrays at 0x28F0 and 0x28FC
   * hold 12 entries (layers 0..11 -- the face tables cap at 12, see the
   * "layer-count producer" item in CLAUDE.md). The caller world_object_tick
   * advances W[0x096C] with only a >100 safety cap, and once the bike
   * actually flew (spherical_to_cartesian fix) it reached layer 12, so
   * W[0x28FC + 12] == W[0x2908] -- the GEOMETRY CELL TABLE POINTER slot --
   * was read as a history counter: index 7.5e15, SIGSEGV. Measured at the
   * crash: W[0x096C]=12, W[0x2908]=93824992991014. Refuse layers outside
   * 0..11 (loud once); fixing the producer is the real repair. */
  if ((unsigned)W[0x096C] >= 12) {
    static int warned;
    if (warned++ == 0 || propcycl_verbose())
      fprintf(stderr, "[GUARD] terrain_cell_history_store: layer %ld out of "
              "0..11 -- skipped (layer-count producer bug, see CLAUDE.md)\n",
              (long)W[0x096C]);
    return 0;
  }
  iVar2 = W[0x28FC + W[0x096C] * 4] * 0x50 + W[0x096C] * 800;
  W[0x1F90 + (iVar2)] = W[0x0920];
  W[0x1F94 + (iVar2)] = W[0x0924];
  W[0x1F98 + (iVar2)] = W[0x0928];
  W[0x1F9C + (iVar2)] = W[0x092C];
  W[0x1FA0 + (iVar2)] = W[0x0930];
  W[0x1FA4 + (iVar2)] = W[0x0934];
  W[0x1FA8 + (iVar2)] = W[0x093C];
  W[0x1FAC + (iVar2)] = W[0x0938];
  W[0x1FB0 + (iVar2)] = W[0x0944];
  W[0x1FB4 + (iVar2)] = W[0x0940];
  W[0x1FB8 + (iVar2)] = W[0x094C];
  W[0x1FBC + (iVar2)] = W[0x0948];
  W[0x1FC0 + (iVar2)] = W[0x0950];
  W[0x1FC4 + (iVar2)] = W[0x0954];
  W[0x1FC8 + (iVar2)] = W[0x0958];
  W[0x1FCC + (iVar2)] = W[0x095C];
  W[0x1FD0 + (iVar2)] = W[0x0960];
  W[0x28F0 + W[0x096C] * 4] = W[0x28F0 + W[0x096C] * 4] + 1;
  if (2 < (int)W[0x28F0 + W[0x096C] * 4]) {
    W[0x28F0 + W[0x096C] * 4] = 2;
  }
  W[0x28FC + W[0x096C] * 4] = W[0x28FC + W[0x096C] * 4] + 1;
  iVar1 = W[0x096C];
  iVar2 = W[0x28FC + W[0x096C] * 4] + -1;
  if (iVar2 != 0 && 0 < (int)W[0x28FC + W[0x096C] * 4]) {
    W[0x28FC + W[0x096C] * 4] = 0;
    iVar2 = iVar1;
  }
  return iVar2;
}

/* ---- terrain_cell_point_test ---- */

int terrain_cell_point_test(int param_1,int param_2)

{
  int iVar1;
  int bVar2;
  short sVar3;
  short sVar4;
  short sVar5;
  int iVar6;
  
  iVar6 = param_1 - W[0x0920];
  iVar1 = param_2 - W[0x0924];
  bVar2 = false;
  if (W[0x0960] == 0) {
    sVar3 = math_atan2(iVar6,iVar1);
    sVar4 = math_atan2(param_1 - W[0x0928],param_2 - W[0x092C]);
    if (((uint16_t)((short)W[0x093C] - (short)W[0x0938]) <=
         (uint16_t)(sVar3 - (short)W[0x0938])) ||
       ((uint16_t)((short)W[0x0944] - (short)W[0x0940]) <= (uint16_t)(sVar4 - (short)W[0x0940])
       )) goto LAB_00004da4;
  }
  else {
    sVar3 = math_atan2(iVar6,iVar1);
    sVar4 = math_atan2(param_1 - W[0x0928],param_2 - W[0x092C]);
    sVar5 = math_atan2(param_1 - W[0x0930],param_2 - W[0x0934]);
    if ((((uint16_t)((short)W[0x093C] - (short)W[0x0938]) <=
          (uint16_t)(sVar3 - (short)W[0x0938])) ||
        ((uint16_t)((short)W[0x0944] - (short)W[0x0940]) <=
         (uint16_t)(sVar4 - (short)W[0x0940]))) ||
       ((uint16_t)((short)W[0x094C] - (short)W[0x0948]) <= (uint16_t)(sVar5 - (short)W[0x0948])
       )) goto LAB_00004da4;
  }
  bVar2 = true;
LAB_00004da4:
  if (bVar2) {
    if (W[0x0950] == 0xff) {
      iVar6 = -2;
    }
    else {
      iVar6 = W[0x0950] * 0x400 + (W[0x0958] * iVar1 >> 9) + (W[0x0954] * iVar6 >> 9);
    }
  }
  else {
    iVar6 = -1;
  }
  return iVar6;
}

/* ---- terrain_collision_zone_resolve ---- */

void terrain_collision_zone_resolve(void)

{
  { extern int g_contactlog; static int n;
    if (g_contactlog && (n++ % 60) == 0)
      fprintf(stderr, "[ZRES] terrain_collision_zone_resolve call %d (f%u)\n",
              n, g_sys.frame_count); }

  int bVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  uint8_t bVar5;
  
  iVar2 = 0;
  _safety_ctr = 0;
  do {
    if (W[0x1308] <= iVar2) {
      return;
    }
    bVar5 = 0;
    bVar1 = false;
    iVar4 = 0;
    for (iVar3 = 0; iVar3 < (int)W[0x13EC + (iVar2 * 0x140)]; iVar3 = iVar3 + 1) {
      if ((int)W[0x13F0 + (iVar2 * 0x140 + iVar3 * 4)] <= W[0x131C + (iVar2 * 0x140)] * 0x10 + 8) {
        if (iVar4 == 0) {
          bVar1 = false;
        }
        else {
          bVar1 = true;
        }
        break;
      }
      bVar5 = W[0x1418 + (iVar3 + iVar2 * 0x140)];
      if ((W[0x13F0 + (iVar2 * 0x140 + iVar3 * 4)] & 0xf) == 4) {
        iVar4 = iVar4 + -1;
      }
      else {
        iVar4 = iVar4 + 1;
      }
    }
    if (bVar1) {
      W[0x1424 + (iVar2 * 0x140)] = 1;
      W[0x1428 + (iVar2 * 0x140)] = (uint32_t)bVar5;
    }
    else {
      W[0x1424 + (iVar2 * 0x140)] = 0;
      W[0x1428 + (iVar2 * 0x140)] = 0;
    }
    iVar2 = iVar2 + 1;
  if (++_safety_ctr > 10000) break; } while( true ); _safety_ctr = 0;
}

/* ---- world_objects_copy_prev_state ---- */

void world_objects_copy_prev_state(void)

{
  int iVar1;
  
  for (iVar1 = 0; iVar1 < W[0x1308]; iVar1 = iVar1 + 1) {
    W[0x142C + (iVar1 * 0x140)] = W[0x1428 + (iVar1 * 0x140)];
  }
  return;
}

/* ---- world_objects_init_player ---- */

void world_objects_init_player(void)

{
  /* THE SIX COLLISION PROBES (centre + 2 wing tips + nose + top +
   * look-ahead) HAD GARBAGE X/Y/Z ON 5 OF 6 -- "sometimes hit invisible
   * walls" (user report, 2026-09-16). Measured live with PROPCYCL_WRTDBG:
   * layer 1 (right wing) probe X read -4294657170; layers 3 and 5 (nose,
   * look-ahead) probe Y both read 4295040683 -- both near +-2^32, the
   * signature of a 32-bit write landing in only HALF of a 64-bit `_W[]`
   * slot.
   *
   * `rotate_euler_zxy_optimized`'s three output parameters are `int *`
   * (32-bit) -- correctly, that is what the ROM's `move.l` stores into
   * registers/stack actually are. But every call here was handed
   * `&W[slot]` directly: `_W` is `intptr_t[]` (8 bytes/slot), so on this
   * little-endian host `*param = value` through that `int*` only wrote
   * the LOW 4 bytes of the 8-byte slot, leaving the HIGH 4 bytes exactly
   * as whatever was there before -- stale data from an earlier, unrelated
   * 64-bit write to that same address. Every later read of that slot AS
   * A FULL W[] VALUE (not through a 32-bit lens) then combines the
   * correct low half with garbage high bits, e.g.
   * `terrain_height_resolve`'s `(&W[0x131C])[layer*0x140] * 0x10 + 8`,
   * which corrupts the collision search's cell-height math for whichever
   * probe was hit and produces exactly the "everything just slams to a
   * stop and gets shoved backward" feel of an invisible wall (measured:
   * speed drops from 9500-15000 to exactly 0x900 in one frame, five times
   * over a 9000-frame autopilot flight -- the "hard-reset" branch already
   * documented in register row 97, over-triggering here on a garbage
   * probe position rather than the byte-swapped table that row fixed).
   *
   * Fixed by rotating into real C locals (matching the function's actual
   * `int*` signature) and writing every affected W[] slot -- both the
   * intermediate rotated offset AND the final player-relative target --
   * with an explicit `(int32_t)` cast, so the full 64-bit slot is set
   * correctly instead of half-written. */
  int32_t rx, ry, rz;

  W[0x1308] = 6;
  W[0x130C] = 0;
  W[0x1310] = 0;
  W[0x1314] = 0;
  W[0x1318] = W[0x0D00];
  W[0x131C] = W[0x0D04];
  W[0x1320] = W[0x0D08];
  rotate_euler_zxy_optimized
            (0x8c0,0x100,0x200,W[0x0D0C],W[0x0D10],W[0x0D14],&rx,&ry,&rz);
  W[0x144C] = rx; W[0x1450] = ry; W[0x1454] = rz;
  W[0x1458] = (int32_t)(rx + W[0x0D00]);
  W[0x145C] = (int32_t)(ry + W[0x0D04]);
  W[0x1460] = (int32_t)(rz + W[0x0D08]);
  rotate_euler_zxy_optimized
            (0xfffff740,0x100,0x200,W[0x0D0C],W[0x0D10],W[0x0D14],&rx,&ry,&rz);
  W[0x158C] = rx; W[0x1590] = ry; W[0x1594] = rz;
  W[0x1598] = (int32_t)(rx + W[0x0D00]);
  W[0x159C] = (int32_t)(ry + W[0x0D04]);
  W[0x15A0] = (int32_t)(rz + W[0x0D08]);
  rotate_euler_zxy_optimized
            (0,0,0x2b0,W[0x0D0C],W[0x0D10],W[0x0D14],&rx,&ry,&rz);
  W[0x16CC] = rx; W[0x16D0] = ry; W[0x16D4] = rz;
  W[0x16D8] = (int32_t)(rx + W[0x0D00]);
  W[0x16DC] = (int32_t)(ry + W[0x0D04]);
  W[0x16E0] = (int32_t)(rz + W[0x0D08]);
  rotate_euler_zxy_optimized
            (0,0x4e0,0x200,W[0x0D0C],W[0x0D10],W[0x0D14],&rx,&ry,&rz);
  W[0x180C] = rx; W[0x1810] = ry; W[0x1814] = rz;
  W[0x1818] = (int32_t)(rx + W[0x0D00]);
  W[0x181C] = (int32_t)(ry + W[0x0D04]);
  W[0x1820] = (int32_t)(rz + W[0x0D08]);
  rotate_euler_zxy_optimized
            (0,0,0x2b0,W[0x0D9C],W[0x0DA0],W[0x0DA4],&rx,&ry,&rz);
  W[0x194C] = rx; W[0x1950] = ry; W[0x1954] = rz;
  W[0x1958] = (int32_t)(rx + W[0x0D00]);
  W[0x195C] = (int32_t)(ry + W[0x0D04]);
  W[0x1960] = (int32_t)(rz + W[0x0D08]);
  return;
}

/* ---- world_objects_transition_check ---- */

void world_objects_transition_check(void)

{
  int iVar1;
  int iVar2;
  
  for (iVar2 = 0; iVar2 < W[0x1308]; iVar2 = iVar2 + 1) {
    if ((((W[0x142C + (iVar2 * 0x140)] == 0xe0) || (W[0x142C + (iVar2 * 0x140)] == 0xe1)) &&
        (W[0x1428 + (iVar2 * 0x140)] != 0xe0)) && (W[0x1428 + (iVar2 * 0x140)] != 0xe1)) {
      W[0x1430 + (iVar2 * 0x140)] = 0x14;
    }
    iVar1 = W[0x1430 + (iVar2 * 0x140)] + -1;
    W[0x1430 + (iVar2 * 0x140)] = iVar1;
    if (iVar1 < 0) {
      W[0x1430 + (iVar2 * 0x140)] = 0;
    }
    if (0 < (int)W[0x1430 + (iVar2 * 0x140)]) {
      W[0x1428 + (iVar2 * 0x140)] = 0xe5;
    }
  }
  return;
}

/* ---- world_stage_geometry_load ---- */

/* Per-cell face-list loader, called from world_stage_update when the
 * player crosses a cell boundary. For each layer in the current cell:
 *   1) Clear the layer's 16x16 neighbor table to 0xFF (no face).
 *   2) Walk the ROM-resident face record stream and, per record, store
 *      a host pointer in W[0x0314+...] and the face index in
 *      W[0x0014+...] keyed by the record's (col,row) header bytes.
 *
 * Source layout (all big-endian in ROM):
 *   W[0x2908] -> geom_cell_table[cell] -> layer_list[layer] -> records
 * The two upper levels are 32-bit BE pointers (ROM byte offsets), so
 * the original M68K-style host-pointer dereference crashed on 64-bit.
 * Now read with mem_read32(); record bytes still walked via &R[...]
 * because consumers (terrain_cell_find_triangle, world_stage_render)
 * read W[0x0314+...] back as (char *)W[N] host pointers. */
void world_stage_geometry_load(void)
{
  const uint32_t geom_table = (uint32_t)W[0x2908];
  const uint32_t cell       = (uint32_t)W[0x0990];
  uint8_t *rom_end = &R[ROM_SIZE];
  int layer;
  int j;
  int p;
  int s;

  /* LAYER CAP. W[0x0964] is a layer COUNT that must be 0..12 (every
   * per-layer table here holds 12 entries: W[0x0014 + layer*0x100],
   * W[0x0314 + layer*0x80], ...). The producer clamp in world_stage_update
   * only runs when the cell CHANGES, so a value corrupted between cell
   * changes reaches this loop unchecked -- measured at 1153 on the attract
   * demo path, which drove `W[0x0014 + layer*0x100 + j]` to index ~295k in
   * a 262144-entry _W[] and segfaulted at ~frame 940. Bound the loop
   * itself; the corruption source is the unconverted (&W[base])[N] stride
   * writes still in this file (88 sites). */
  { int _lc = (int)W[0x0964];
    if (_lc < 0 || _lc > 12) {
      static int warned;
      if (warned++ == 0 || propcycl_verbose())
        fprintf(stderr, "[GUARD] world_stage_geometry_load: layer count %d "
                "out of 0..12 -- clamped\n", _lc);
      _lc = (_lc < 0) ? 0 : 12;
      W[0x0964] = _lc;
    }
  }
  for (layer = 0; layer < (int)W[0x0964] && layer < (g_layerfix ? 3 : 12); layer++) {
    uint32_t cell_ptr_addr;
    uint32_t cell_list;
    uint32_t layer_ptr_addr;
    uint32_t layer_data_off;
    uint8_t *base;
    uint8_t *rec;
    int primary;
    int secondary;
    int face_index = 0;
    int safety = 0;

    for (j = 0; j < 0x100; j++) {
      W[0x0014 + layer * 0x100 + j] = 0xFF;
    }

    /* W[0x2908] is currently left at 0 by world_props_init_dispatch for
     * every course, so guard each ROM read — without it we'd walk off
     * the start of ROM and corrupt unrelated W slots. */
    cell_ptr_addr = geom_table + cell * 4u;
    if (cell_ptr_addr + 4u > ROM_SIZE) return;
    cell_list = mem_read32(cell_ptr_addr);

    layer_ptr_addr = cell_list + (uint32_t)layer * 4u;
    if (layer_ptr_addr + 4u > ROM_SIZE) return;
    layer_data_off = mem_read32(layer_ptr_addr);
    if (layer_data_off + 2u > ROM_SIZE) return;

    base      = &R[layer_data_off];
    primary   = base[0];
    secondary = base[1];
    rec       = base + 2;

    for (p = 0; p < primary; p++) {
      uint32_t key;
      if (rec + 3 > rom_end) goto layer_done;
      FACEPTR(layer, face_index) = (intptr_t)rec;
      key = (uint32_t)rec[0] + (uint32_t)rec[1] * 0x10u;
      W[0x0014 + key + layer * 0x100] = (int8_t)face_index;
      rec += 2 + rec[2];
      face_index++;
      if (++safety > 0x1000) goto layer_done;
    }
    for (s = 0; s < secondary; s++) {
      if (rec + 3 > rom_end) goto layer_done;
      FACEPTR(layer, face_index) = (intptr_t)rec;
      rec += 2 + rec[2];
      face_index++;
      if (++safety > 0x1000) goto layer_done;
    }
  layer_done: ;
    if (g_tunlog) {
      int _nk = 0, _j;
      for (_j = 0; _j < 0x100; _j++) if ((int8_t)W[0x0014 + layer*0x100 + _j] != -1) _nk++;
      fprintf(stderr, "[TUNLOAD] cell=%u layer=%d/%ld cell_list=0x%06X layer_off=0x%06X "
              "primary=%d secondary=%d faces=%d keyed=%d\n",
              cell, layer, (long)W[0x0964], cell_list, layer_data_off,
              primary, secondary, face_index, _nk);
    }
  }
}

/* ---- world_stage_render ---- */

void world_stage_render(uint32_t param_1,uint32_t param_2)

{
  short sVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  short *psVar5;
  short *psVar6;
  int local_c;
  
  iVar3 = (int)(param_1 - W[0x099C]) >> 0xd;
  local_c = iVar3 + 1;
  iVar2 = (int)(param_2 - W[0x09A0]) >> 0xd;
  iVar4 = iVar2 + 1;
  if (0xdff < (param_1 & 0x1fff)) {
    local_c = iVar3 + 2;
  }
  if (0xbff < (param_2 & 0x1fff)) {
    iVar4 = iVar2 + 2;
  }
  /* Per-layer delta-table scan. The original walks `psVar5` (a short*
   * into ROM at 0xB8618) two shorts at a time looking for an entry
   * whose (col, row) deltas + base index land on a non-0xFF face slot.
   * Without an upper bound the walk can run off the end of ROM —
   * triggers when W[0x0964] is non-zero, which it now is once
   * world_props_init_dispatch wires the per-course height map. The
   * safety counter caps the walk at 4096 iterations and falls through
   * to 0xFF (no face) if no match is found. */
  for (iVar3 = 0; iVar3 < W[0x0964]; iVar3 = iVar3 + 1) {
    int safety = 0;
    int found  = 0;
    int delta_off = 0xB8618;  /* short index, used with vrd16s */
    short col_d = 0, row_d = 0;
    int neighbor_idx = 0;
    while (safety < 4096) {
      /* Inner: walk delta pairs until column matches. */
      while (safety < 4096) {
        if (delta_off + 4 > (int)ROM_SIZE) { safety = 4096; break; }
        col_d = (short)vrd16s(delta_off);
        row_d = (short)vrd16s(delta_off + 2);
        delta_off += 4;
        safety++;
        if (((local_c + col_d) & 0xfffffff0U) == 0) break;
      }
      if (safety >= 4096) break;
      neighbor_idx = (iVar4 + row_d) * 0x10 + iVar3 * 0x100 + local_c + col_d;
      if (((iVar4 + row_d) & 0xfffffff0U) == 0 &&
          (uint8_t)W[0x0014 + neighbor_idx] != 0xff) {
        found = 1;
        break;
      }
    }
    W[0x0914 + iVar3 * 4] = found          /* ROM 0x00462C, D2.l*4 */
        ? (uint32_t)(uint8_t)W[0x0014 + neighbor_idx]
        : 0xFF;
  }
  /* psVar5/psVar6/sVar1 retained as decoded vars but unused after rewrite. */
  (void)psVar5; (void)psVar6; (void)sVar1;
  return;
}

/* ---- cz_ram_init ---- */

void cz_ram_init(void)

{
  int iVar1;
  undefined1 *puVar2;
  undefined1 *puVar3;
  undefined1 *puVar4;
  undefined1 *puVar5;
  undefined1 *puVar6;
  
  puVar3 = &R[0x80000];
  iVar1 = 0;
  puVar2 = &g_sys.palette_ram[0x7F00];
  puVar5 = &g_sys.palette_ram[0x17F00];
  puVar6 = &g_sys.palette_ram[0xFF00];
  do {
    *puVar2 = *puVar3;
    puVar4 = puVar3 + 2;
    *puVar6 = puVar3[1];
    puVar3 = puVar3 + 3;
    *puVar5 = *puVar4;
    g_sys.syscon[0x14] = 0;
    iVar1 = iVar1 + 1;
    puVar2 = puVar2 + 1;
    puVar5 = puVar5 + 1;
    puVar6 = puVar6 + 1;
  } while (iVar1 < 0x100);
  return;
}

/* ---- cz_depth_table_init ---- */

void cz_depth_table_init(void)

{
  short sVar1;
  short sVar2;
  undefined2 *puVar3;
  undefined2 *puVar4;
  undefined2 *puVar5;

  puVar4 = (undefined2 *)&R[0x3FF06];
  sVar1 = 3;
  do {
    sVar2 = 0xff;
    /* Original: puVar3 = (0x810200) — raw CZ RAM hardware address.
     * 0x810200 - 0x810000 (CZRAM_BASE) = offset 0x200 into CZ RAM. */
    puVar3 = (undefined2 *)(g_sys.czram + 0x200);
    puVar5 = puVar4;
    mem_write32(0x81000A, sVar1);  /* select CZ bank */
    do {
      puVar4 = puVar5 + 1;
      *puVar3 = *puVar5;
      sVar2 = sVar2 + -1;
      puVar3 = puVar3 + 1;
      puVar5 = puVar4;
    } while (sVar2 != -1);
    sVar1 = sVar1 + -1;
  } while (sVar1 != -1);
  mem_write32(0x810008, 0x7555);
  mem_write32(0x81000C, 0xe4);
  return;
}

/* ---- environment_params_reload ---- */

void environment_params_reload(void)

{
  environment_params_load(0);
  return;
}

/* ---- terrain_lod_update_wrapper ---- */

void terrain_lod_update_wrapper(void)

{
  terrain_lod_update();
  return;
}

/* collision_height_dispatch / _axis_dist / _biaxial / _slope / _slope_offset
 * (ROM 0x0065EA..0x0067CC) were deleted 2026-09-18: dead Ghidra renderings
 * with no caller anywhere in the live tree, and wrong besides (the dispatch
 * was a guessed if-chain on W[0x0B4C], not the ROM's tables). The live
 * implementation of those zone callbacks is cz_sub_delta() / cz_type_callback()
 * near the top of this file (register row 96). */

/* ---- cz_attr_init @ 0x022A16 ---- */



/* ROM: seven `clr.w $81000N`, then for d1 = 0..255
 *   d0 = d1*21 (lsl #2 ; add ; lsl #2 ; add) ; move.w d0,(a0,d1.l*2)
 * with a0 = 0x810200 -- extension word 0x1A00, scale 2: a 16-BIT ramp at
 * STRIDE 2 into the CZ window. The transpile wrote it 32 bits wide at
 * stride 4, which put 0 in every even word and i*21 in the odd ones and
 * ran off the end of the 0x200-byte window half way through. MAME's
 * captured bank 0 (czram0_f1500 .. _f4800) is exactly this ramp,
 * 0x0000 0x0015 0x002A ... -- the table the live CZ fog is built from. */
void cz_attr_init(void)

{
  int iVar1;

  mem_write16(0x810000, 0);
  mem_write16(0x810002, 0);
  mem_write16(0x810004, 0);
  mem_write16(0x810006, 0);
  mem_write16(0x810008, 0);
  mem_write16(0x81000A, 0);
  mem_write16(0x81000C, 0);
  iVar1 = 0;
  do {
    mem_write16(0x810200 + iVar1 * 2, (uint16_t)(iVar1 * 0x15));
    g_sys.syscon[0x14] = 0;
    iVar1 = iVar1 + 1;
  } while (iVar1 < 0x100);
  return;
}





/* ---- environment_zone_init @ 0x028552 ---- */



/* ROM 0x028552..0x0285C4:
 *   move.l $e00e0c,d0 ; lsl.l #7 ; addi.l #$37758 ; move.l d0,$e16a3c
 *   divu.l #$18000 on camera Z and X ; lea (a0,d0.l*8) -> cell
 *   move.w d0,$e16a38 ; move.b (a0,d0.w),d0 ; ext.w ; move.w d0,$e16a36
 *   move.w $376e0(d0.w*4),-(a7) ; jsr environment_params_load
 *   clr.w $e16a3a ; clr.w $e16a30
 * i.e. the level starts SNAPPED to the start cell's own preset. The
 * transpile loaded preset 0 whatever the zone, and stored the three 16-bit
 * fields as whole slots. */
void environment_zone_init(void)
{
  uint32_t gz = (uint32_t)W[0x0CE4] / 0x18000;       /* divu.l: unsigned */
  uint32_t gx = (uint32_t)W[0x0CDC] / 0x18000;
  int cell = (int)(gx + gz * 8);
  int zone;

  W[0x16A3C] = (intptr_t)(int32_t)(((int32_t)W[0x0E0C] << 7) + 0x37758);
  ENVW_SET(0x16A38, cell);
  zone = vrd8s((int32_t)W[0x16A3C] + (int16_t)cell);
  ENVW_SET(0x16A36, zone);
  environment_params_load(vrd16s(0x376E0 + (int16_t)ENVW(0x16A36) * 4));
  ENVW_SET(0x16A3A, 0);
  ENVW_SET(0x16A30, 0);
}






/* ---- terrain_cell_sort_filter @ 0x0043DA ---- */

int terrain_cell_sort_filter(void)

{
  undefined4 uVar1;
  undefined4 uVar2;
  int iVar3;
  int iVar4;
  
  if (W[0x0E0C] == 3) {
    if (W[0x0974] == 1) {
      W[0x0974] = 0;
    }
    if ((W[0x0974] == 2) && ((int)W[0x0978] < (int)W[0x097C])) {
      W[0x0974] = 0;
    }
  }
  if ((((W[0x0964] == 3) && (W[0x0974] == 2)) && ((W[0x0978] & 0xf) == 0xc)) &&
     (((W[0x097C] & 0xf) == 0xc && (W[0x0974] = 1, (int)W[0x097C] < (int)W[0x0978])))) {
    W[0x0978] = W[0x097C];
    W[0x0984] = W[0x0988];
  }
  iVar3 = W[0x0974] + -3;
  if (iVar3 == 0) {
    /* THE THREE-FACE SORT IS STRIDE 4, NOT 1. ROM 0x00445A/0x00445E:
     *     move.l $4(a0, d2.l*4), d0 ; cmp.l (a0, d2.l*4), d0
     * a0 = 0xE00978, a4 = 0xE00984. (The `$4(a0,d2.l)` capstone prints for
     * the first of those drops the index SCALE off a brief-format extension
     * word -- 0x2C04 has bits10-9 = 10, scale 4 -- the trap CLAUDE.md
     * already records. Decode the extension word, do not read the mnemonic.)
     * These arrays are exactly THREE entries: 0x0978/0x097C/0x0980 and
     * 0x0984/0x0988/0x098C, which is the ROM's own maximum terrain-layer
     * count. At stride one, entries 1 and 2 land on byte offsets 0x0979 and
     * 0x097A -- 1-mod-4 and 2-mod-4 slots that `sync_wram_to_W` rebuilds
     * from neighbouring bytes every frame, so a column with more than one
     * face lost every face above the first. The scalar accesses just above
     * (W[0x097C], W[0x0980], W[0x0988]) were already written at the right
     * offsets, i.e. the CONSUMER and the PRODUCER disagreed. */
    for (iVar3 = W[0x0974] + -2; -1 < iVar3; iVar3 = iVar3 + -1) {
      for (iVar4 = 0; iVar4 <= iVar3; iVar4 = iVar4 + 1) {
        if ((int)W[0x0978 + iVar4 * g_facestride] < (int)W[0x097C + iVar4 * g_facestride]) {
          uVar1 = W[0x0978 + iVar4 * g_facestride];
          uVar2 = W[0x0984 + iVar4 * g_facestride];
          W[0x0978 + iVar4 * g_facestride] = W[0x097C + iVar4 * g_facestride];
          W[0x0984 + iVar4 * g_facestride] = W[0x0988 + iVar4 * g_facestride];
          W[0x097C + iVar4 * g_facestride] = uVar1;
          W[0x0988 + iVar4 * g_facestride] = uVar2;
        }
      }
    }
    iVar3 = (W[0x0980] & 0xf | (W[0x097C] & 0xf | (W[0x0978] & 0xf) << 4) << 4) - 0x4cc;
    if (iVar3 == 0) {
      /* THE TUNNEL RULE -- a jump table Ghidra lost, and the whole reason a
       * cave or tunnel could never be flown through.
       *
       * The value just computed packs the three sorted faces' KIND nibbles
       * into one word, top to bottom. 0x4CC is [4, 12, 12]: a PASS-THROUGH
       * roof over two SOLID faces -- the signature of a tunnel mouth, and
       * the only triple the ROM special-cases.
       *
       * ROM 0x0044A6..0x0044C8:
       *     subi.l #$4cc,d0 ; cmpi.l #$0,d0 ; bhi.b $44ca
       *     move.w $44be(pc, d0.w*2), d0 ; nop ; jmp $44be(pc, d0.w)
       * `cmpi #0` + `bhi` lets only d0 == 0 through (anything below 0x4CC
       * wraps to a huge unsigned), so the table at 0x44BE has exactly ONE
       * entry: the word 0x0002, giving target 0x44BE + 2 = 0x44C0:
       *     0044C0: subq.l #$2,(a1)       ; a1 = 0xE00974, count 3 -> 1
       *     0044C2: move.l $8(a0),(a0)    ; W[0x0978] = W[0x0980]
       *     0044C6: move.l $8(a4),(a4)    ; W[0x0984] = W[0x098C]
       * i.e. KEEP ONLY THE BOTTOM FACE and report a single-face column. The
       * roof and the ceiling are discarded, so the player passes under them.
       *
       * Note 0x44BE reads as `ori.b #$91,d2` in a linear disassembly -- it
       * is the jump table plus the first word of the target, which is why
       * the decompiler produced an empty body here.
       *
       * The two scale-2 / scale-1 forms in that pair are not a typo: the
       * table READ is `(pc, d0.w*2)` (extension 0x0208, bits10-9 = 01) and
       * the JUMP is `(pc, d0.w)` (extension 0x0002, scale 0), because by
       * then d0 holds the table's byte displacement rather than the index. */
      if (g_layerfix) {
        W[0x0974] = W[0x0974] - 2;
        W[0x0978] = W[0x0980];
        W[0x0984] = W[0x098C];
      }
      iVar3 = 0;
      return iVar3;
    }
  }
  return iVar3;
}





/* ---- terrain_height_resolve @ 0x00C288 ---- */






void terrain_height_resolve(void)

{
  undefined4 uVar1;
  undefined1 uVar2;
  undefined1 uVar3;
  uint8_t bVar4;
  int iVar5;
  int bVar6;
  int bVar7;
  int iVar8;
  int iVar9;
  int iVar10;
  uint32_t uVar11;
  int local_3a;
  uint32_t local_36;
  uint32_t local_32;
  uint8_t local_2e;
  uint8_t local_2d;
  uint32_t local_28;
  int local_24;
  int local_8;
  
  for (iVar8 = 0; iVar8 < W[0x1308]; iVar8 = iVar8 + 1) {
    if ((&W[0x13E8])[iVar8 * 0x140] == 0) {
      (&W[0x1334])[iVar8 * 0x140 + (&W[0x1330])[iVar8 * 0x140] * 4] = (0x420004);
      (&W[0x13AC])[(&W[0x1330])[iVar8 * 0x140] + iVar8 * 0x140] = 1;
      (&W[0x13CA])[(&W[0x1330])[iVar8 * 0x140] + iVar8 * 0x140] = 0;
      (&W[0x1330])[iVar8 * 0x140] = (&W[0x1330])[iVar8 * 0x140] + 1;
      (&W[0x1334])[iVar8 * 0x140 + (&W[0x1330])[iVar8 * 0x140] * 4] = 0xfffe000c;
    }
    else {
      (&W[0x1334])[iVar8 * 0x140 + (&W[0x1330])[iVar8 * 0x140] * 4] = (0x42000C);
      (&W[0x13AC])[(&W[0x1330])[iVar8 * 0x140] + iVar8 * 0x140] = 1;
      (&W[0x13CA])[(&W[0x1330])[iVar8 * 0x140] + iVar8 * 0x140] = 0;
      (&W[0x1330])[iVar8 * 0x140] = (&W[0x1330])[iVar8 * 0x140] + 1;
      (&W[0x1334])[iVar8 * 0x140 + (&W[0x1330])[iVar8 * 0x140] * 4] = 0xfffe0004;
    }
    (&W[0x13AC])[(&W[0x1330])[iVar8 * 0x140] + iVar8 * 0x140] = 1;
    (&W[0x13CA])[(&W[0x1330])[iVar8 * 0x140] + iVar8 * 0x140] = 0;
    (&W[0x1330])[iVar8 * 0x140] = (&W[0x1330])[iVar8 * 0x140] + 1;
  }
  for (iVar8 = 0; iVar8 < W[0x1308]; iVar8 = iVar8 + 1) {
    for (local_3a = (&W[0x1330])[iVar8 * 0x140] + -2; -1 < local_3a; local_3a = local_3a + -1) {
      for (iVar9 = 0; iVar9 <= local_3a; iVar9 = iVar9 + 1) {
        if ((int)(&W[0x1334])[iVar8 * 0x140 + iVar9 * 4] < (int)(&W[0x1338])[iVar8 * 0x140 + iVar9 * 4])
        {
          uVar1 = (&W[0x1334])[iVar8 * 0x140 + iVar9 * 4];
          iVar10 = iVar9 + iVar8 * 0x140;
          uVar2 = (&W[0x13AC])[iVar10];
          uVar3 = (&W[0x13CA])[iVar10];
          (&W[0x1334])[iVar8 * 0x140 + iVar9 * 4] = (&W[0x1338])[iVar8 * 0x140 + iVar9 * 4];
          (&W[0x13AC])[iVar10] = (&W[0x13AD])[iVar10];
          (&W[0x13CA])[iVar10] = (&W[0x13CB])[iVar10];
          (&W[0x1338])[iVar8 * 0x140 + iVar9 * 4] = uVar1;
          (&W[0x13AD])[iVar10] = uVar2;
          (&W[0x13CB])[iVar10] = uVar3;
        }
      }
    }
  }
  for (iVar8 = 0; iVar8 < W[0x1308]; iVar8 = iVar8 + 1) {
    (&W[0x13EC])[iVar8 * 0x140] = 0;
    local_8 = 0;
    for (iVar9 = 0; iVar9 < (int)(&W[0x1330])[iVar8 * 0x140]; iVar9 = iVar9 + 1) {
      iVar10 = iVar8 * 0x140;
      /* PROPCYCL_CONTACTLOG: a histogram of terrain FACE TYPES. The particle
       * emitters -- the spray billboards near ground and water -- are gated on
       * a face type of 0xE0/0xE1/0xE5 reaching W[0x1428], and this loop is the
       * only thing that puts one there. If no face ever carries such a type,
       * the emitters cannot fire and the billboards cannot appear. */
      { extern int g_contactlog; static unsigned hist[256], seen;
        uint8_t _t = (uint8_t)(&W[0x13AC])[iVar9 + iVar10];
        if (g_contactlog) { hist[_t]++; seen++;
          if ((seen % 20000) == 0) {
            char b[256]; int o = 0, k, sp = 0;
            o += snprintf(b+o, sizeof b-o, "[SURF] %u faces:", seen);
            for (k = 0; k < 256 && o < (int)sizeof b - 16; k++)
              if (hist[k]) { o += snprintf(b+o, sizeof b-o, " %02X:%u", k, hist[k]);
                             if (k > 0xdf && k < 0xf0) sp += hist[k]; }
            fprintf(stderr, "%s   (0xE0-0xEF: %d)\n", b, sp); } } }
      if ((0xdf < (uint8_t)(&W[0x13AC])[iVar9 + iVar10]) &&
         ((uint8_t)(&W[0x13AC])[iVar9 + iVar10] < 0xf0)) {
        (&W[0x13EC])[iVar8 * 0x140] = (&W[0x13EC])[iVar8 * 0x140] + 1;
        (&W[0x13F0])[iVar8 * 0x140 + local_8 * 4] = (&W[0x1334])[iVar8 * 0x140 + iVar9 * 4];
        (&W[0x1418])[local_8 + iVar10] = (&W[0x13AC])[iVar9 + iVar10];
        local_8 = local_8 + 1;
      }
    }
  }
  iVar8 = 0;
  _safety_ctr = 0;
  do {
    if (W[0x1308] <= iVar8) {
      return;
    }
    iVar9 = (&W[0x131C])[iVar8 * 0x140] * 0x10 + 8;
    local_36 = 0x7fffffff;
    local_32 = 0xf8000001;
    local_2e = 0;
    local_2d = 0;
    local_28 = 0;
    bVar6 = false;
    bVar7 = false;
    /* FOUR COUNTERS, NOT TWO. ROM 0x00C4D6..0x00C4EC initialises:
     *     d6        = (W[0x13E8 + probe*0x140] == 0)   ordinary parity A
     *     -$20(a6)  = 0                                ordinary parity B
     *     -$1c(a6)  = 0                                SPECIAL-FACE vote A
     *     -$18(a6)  = 0                                SPECIAL-FACE vote B
     * The ordinary parity walk (0x00C61A) counts into d6 / -$20; the
     * 0xFD/0xFE/0xFF vote (0x00C75E / 0x00C76A) counts into -$1c / -$18.
     * Register row 94 recovered the vote's jump table correctly but routed
     * it into the ORDINARY counters, which do not exist for that purpose --
     * so a special-typed face polluted the parity and the probe came out
     * "inside solid". The vote counters do two things the ordinary ones
     * must not: they GATE which faces are eligible to become the ceiling or
     * the floor (0x00C536 / 0x00C552), and they MASK the parity when bVar6
     * is computed (0x00C5BC / 0x00C6CE). */
    uVar11 = (uint32_t)((&W[0x13E8])[iVar8 * 0x140] == 0);
    local_24 = 0;
    int vote_a = 0, vote_b = 0;
    int *vA = g_votefix ? &vote_a : (int *)&uVar11;   /* pre-fix: the vote */
    int *vB = g_votefix ? &vote_b : &local_24;        /* polluted the parity */
    for (iVar10 = 0; iVar10 < (int)(&W[0x1330])[iVar8 * 0x140]; iVar10 = iVar10 + 1) {
      bVar4 = (&W[0x13AC])[iVar10 + iVar8 * 0x140];
      if (bVar4 < 0xfd) {
        if ((bVar4 < 0xe0) || (0xef < bVar4)) {
          /* THE GATE IS NOT "0x13CA is 0 or 1" -- it is "this face's OWN vote
           * counter is still zero". ROM 0x00C52E..0x00C556:
           *     tst.b  $be(a2,d0.l)   ; W[0x13CA + ...]
           *     bne    .b             ; non-zero -> try the other pair
           *     tst.l  -$1c(a6)       ; vote A
           *     beq    select         ; 0x13CA == 0 AND voteA == 0 -> eligible
           *  .b cmpi.b #$1, $be(...)  ; 0x13CA == 1 ?
           *     bne    parity
           *     tst.l  -$18(a6)       ; vote B
           *     bne    parity         ; 0x13CA == 1 AND voteB == 0 -> eligible
           * So 0x13CA SELECTS which counter gates the face, and a face whose
           * counter is non-zero is skipped straight to the parity count. The
           * transpile admitted every face with 0x13CA in {0,1} unconditionally,
           * which is how a face under a special-typed one still became the
           * floor. */
          if (((&W[0x13CA])[iVar10 + iVar8 * 0x140] == '\0'
               && (!g_votefix || vote_a == 0)) ||
              ((&W[0x13CA])[iVar10 + iVar8 * 0x140] == '\x01'
               && (!g_votefix || vote_b == 0))) {
            if (bVar7) {
              if ((int)(&W[0x1334])[iVar8 * 0x140 + iVar10 * 4] < iVar9) {
                local_28 = (&W[0x1334])[iVar8 * 0x140 + iVar10 * 4];
                break;
              }
            }
            else {
              if ((int)(&W[0x1334])[iVar8 * 0x140 + iVar10 * 4] <= iVar9) {
                local_32 = (&W[0x1334])[iVar8 * 0x140 + iVar10 * 4];
                local_2d = (&W[0x13AC])[iVar10 + iVar8 * 0x140];
                /* bVar6 comes from the parity MASKED BY THE VOTE, not from the
                 * raw parity. ROM 0x00C5B2 / 0x00C6C4:
                 *     tmpA = d6 ;  if (voteA > 0) tmpA = 0;
                 *     tmpB = B  ;  if (voteB > 0) tmpB = 0;
                 *     bVar6 = !(tmpA == 0 && tmpB == 0);
                 * A face whose vote is live contributes nothing to "am I
                 * buried", which is the entire point of the vote. */
                { int _tA = (g_votefix && vote_a > 0) ? 0 : (int)uVar11;
                  int _tB = (g_votefix && vote_b > 0) ? 0 : (int)local_24;
                  bVar6 = !(_tA == 0 && _tB == 0); }
                goto LAB_0000c5e8;
              }
              local_36 = (&W[0x1334])[iVar8 * 0x140 + iVar10 * 4];
              local_2e = (&W[0x13AC])[iVar10 + iVar8 * 0x140];
            }
          }
          if (((&W[0x1334])[iVar8 * 0x140 + iVar10 * 4] & 0xf) == 4) {
            if ((&W[0x13CA])[iVar10 + iVar8 * 0x140] == '\0') {
              uVar11 = uVar11 - 1;
            }
            else {
              local_24 = local_24 + -1;
            }
          }
          else if ((&W[0x13CA])[iVar10 + iVar8 * 0x140] == '\0') {
            uVar11 = uVar11 + 1;
          }
          else {
            local_24 = local_24 + 1;
          }
        }
      }
      else {
        if (!bVar7) {
          if (iVar9 < (int)(&W[0x1334])[iVar8 * 0x140 + iVar10 * 4]) {
            local_36 = (&W[0x1334])[iVar8 * 0x140 + iVar10 * 4];
            local_2e = (&W[0x13AC])[iVar10 + iVar8 * 0x140];
          }
          else {
            local_32 = (&W[0x1334])[iVar8 * 0x140 + iVar10 * 4];
            local_2d = (&W[0x13AC])[iVar10 + iVar8 * 0x140];
            /* bVar6 comes from the parity MASKED BY THE VOTE, not from the
             * raw parity. ROM 0x00C5B2 / 0x00C6C4:
             *     tmpA = d6 ;  if (voteA > 0) tmpA = 0;
             *     tmpB = B  ;  if (voteB > 0) tmpB = 0;
             *     bVar6 = !(tmpA == 0 && tmpB == 0);
             * A face whose vote is live contributes nothing to "am I
             * buried", which is the entire point of the vote. */
            { int _tA = (g_votefix && vote_a > 0) ? 0 : (int)uVar11;
              int _tB = (g_votefix && vote_b > 0) ? 0 : (int)local_24;
              bVar6 = !(_tA == 0 && _tB == 0); }
            if ((0 < (int)uVar11) || (0 < local_24)) {
LAB_0000c5e8:
              local_28 = local_32;
              break;
            }
            bVar7 = true;
          }
        }
        iVar5 = (uint8_t)(&W[0x13AC])[iVar10 + iVar8 * 0x140] - 0xff;
        if ((uint8_t)(&W[0x13AC])[iVar10 + iVar8 * 0x140] - 0xfd < 2 || iVar5 == 0) {
          /* THE FACE-TYPE DISPATCH FOR TYPES 0xFD / 0xFE / 0xFF.
           *
           * Ghidra could not follow the jump table at 0x00C758 and this was
           * stubbed as "course-specific handler -- treat as floor and
           * break". It is neither. The table is [0x0006, 0x000C, 0x000E]
           * -> 0x00C75E / 0x00C764 / 0x00C766, and the three targets are
           * two instructions each:
           *     0xFD:  add.l d7,-$1c(a6)                 (uVar11   += d7)
           *     0xFE:  add.l d7,-$18(a6)                 (local_24 += d7)
           *     0xFF:  add.l d7,-$1c(a6) ; add.l d7,-$18(a6)   (both)
           * then fall into 0x00C76E `addq.l #1,d4` -- the NEXT face. d7 is
           * set just above at 0x00C71C..0x00C72C: +1 when the face's low
           * nibble is 0xC, else -1. So a special-typed face casts a vote
           * into the above/below counters and the scan continues.
           *
           * Breaking out with that face's height as the floor is what left
           * the resolved height (W[0x1328]) at -8192 / -602 / 2504 while the
           * bike sat at py 47000-54000 -- the probe never registered ground
           * until the bike was already through it. */
          { int _d7 = (((&W[0x1334])[iVar8 * 0x140 + iVar10 * 4] & 0xf) == 0xc) ? 1 : -1;
            int _ty = (uint8_t)(&W[0x13AC])[iVar10 + iVar8 * 0x140];
            /* ROM 0x00C75E / 0x00C766 / 0x00C76A: the vote lands on -$1c and
             * -$18, NOT on the ordinary parity counters d6 / -$20. */
            if (_ty == 0xfd || _ty == 0xff) *vA += _d7;
            if (_ty == 0xfe || _ty == 0xff) *vB += _d7; }
        }
      }
    }
    if (((((local_36 & 0xf) != 4) && (local_2e < 0xfd)) ||
        (((local_32 & 0xf) != 0xc && (local_2d < 0xfd)))) || (bVar6)) {
      (&W[0x1324])[iVar8 * 0x140] = 1;
      (&W[0x1328])[iVar8 * 0x140] = iVar9 >> 4;
      (&W[0x132C])[iVar8 * 0x140] = (uint32_t)local_2e;
    }
    else {
      (&W[0x1324])[iVar8 * 0x140] = 0;
      (&W[0x1328])[iVar8 * 0x140] = (int)local_28 >> 4;
      (&W[0x132C])[iVar8 * 0x140] = 0;
    }
    if (g_tunlog && iVar8 == 0 && (g_sys.frame_count % (unsigned)g_tunlog) == 0) {
      int _k; char _b[256]; int _o = 0;
      _o += snprintf(_b+_o, sizeof _b-_o, "[TUN]   column(%ld):", (long)W[0x1330]);
      for (_k = 0; _k < (int)W[0x1330] && _k < 16 && _o < (int)sizeof _b - 40; _k++)
        _o += snprintf(_b+_o, sizeof _b-_o, " %s@%ld/t%02X",
                       ((int)W[0x1334 + _k*4] & 0xf) == 4 ? "P" : "S",
                       (long)((int32_t)W[0x1334 + _k*4] >> 4),
                       (unsigned)(uint8_t)W[0x13AC + _k]);
      fprintf(stderr, "%s\n[TUN]   ceil=%d/t%02X floor=%d/t%02X above=%d below=%d bad=%d\n",
              _b, (int)local_36 & 0xf, local_2e, (int)local_32 & 0xf, local_2d,
              (int)uVar11, local_24, bVar6);
    }
    if (g_tunlog && iVar8 == 0 && (g_sys.frame_count % (unsigned)g_tunlog) == 0)
      fprintf(stderr, "[TUN]   -> py=%ld probeY=%ld contact=%ld floor=%ld surf=0x%02lX\n",
              (long)W[0x0D04], (long)(iVar9 >> 4), (long)W[0x1324],
              (long)(int32_t)W[0x1328], (long)W[0x132C] & 0xff);
    iVar8 = iVar8 + 1;
  if (++_safety_ctr > 10000) break; } while( true ); _safety_ctr = 0;
}





/* ---- terrain_lod_update @ 0x004DF6 ---- */



int terrain_lod_update(void)

{
  int iVar1;
  
  if ((W[0x0E5C] == 0x78) && (W[0x2C04] == 0)) {
    W[0x09A4] = 1;
  }
  if ((W[0x09A4] == 1) && (W[0x2C04] != 0)) {
    W[0x09A4] = 0;
  }
  if ((g_terrain_flags & g_terrain_mask == 0) != 0) {
    W[0x09A8] = W[0x09A8] + 1;
  }
  g_terrain_needs_update = (uint32_t)(W[0x09A4] != 0);
  iVar1 = 0;
  if (((g_terrain_flags & W[0x2C04] < 3) != 0) &&
     (iVar1 = g_terrain_lod_level + -3, g_terrain_lod_level < 3)) {
    iVar1 = 1;
    g_terrain_needs_update = 1;
  }
  if ((W[0x1964] != 0) && (0x20000 < W[0x0D04])) {
    iVar1 = 0x3c;
    W[0x09AC] = 0x3c;
  }
  if (W[0x09AC] != 0) {
    W[0x09AC] = W[0x09AC] + -1;
  }
  if (W[0x09AC] != 0) {
    iVar1 = 1;
  }
  W[0x12FC] = (uint32_t)(W[0x09AC] != 0);
  if (((W[0x1964] != 0) && (iVar1 = W[0x2C04] + -6, iVar1 != 0 && 5 < W[0x2C04])) &&
     (iVar1 = g_terrain_lod_level + -6, iVar1 != 0 && 5 < g_terrain_lod_level)) {
    iVar1 = 0x3c;
    W[0x09B0] = 0x3c;
  }
  if (W[0x09B0] != 0) {
    W[0x09B0] = W[0x09B0] + -1;
  }
  if (W[0x09B0] != 0) {
    iVar1 = 1;
  }
  W[0x1300] = (uint32_t)(W[0x09B0] != 0);
  return iVar1;
}




/* ---- world_collision_zones_process @ 0x005FB6 ---- */

void world_collision_zones_process(void)

{
  uint16_t *puVar1;
  uint16_t *puVar2;
  uint16_t *puVar3;
  uint16_t *puVar4;
  uint16_t *puVar5;
  uint16_t *puVar6;
  uint32_t uVar7;
  int iVar8;
  int iVar9;
  uint16_t uVar10;
  int iVar11;
  int local_c;
  int local_8;
  
  /* ROM table: W[0x2918] holds ROM base, index by course (W[0x0D24]) */
  {
    int addr = (int)W[0x2918] + (int)W[0x0D24] * 4;
    if (addr >= 0 && addr + 3 < ROM_SIZE)
      W[0x0BC4] = ROM_READ32(addr);
    else
      W[0x0BC4] = 0;
  }
  if (W[0x0BC4] != 0 && !g_no_zones) {   /* PROPCYCL_NO_ZONES=1: bisection aid, skips the static list */
    /* STATIC ZONE LIST, parsed exactly as ROM 0x005FEA..0x0060E4 does it: a
     * byte CURSOR in W[0x0BC4] (a2) that the M68K advances field by field.
     *
     *   move.w (a0),d0 ; addq #2,(a2)   flags: type = &0xF, sub = &0xF0>>4, &0xFF00>>8
     *   move.l (a0),..; addq #4,(a2)    x -> 0x0B54, y -> 0x0B58, z -> 0x0B5C
     *   movea.w (a0),a0 ; addq #2,(a2)  nine sign-extended 16-bit fields, in order:
     *        0x0B70, 0x0B60, 0x0B64, 0x0B68, 0x0B6C, 0x0B84, 0x0B8C, 0x0B90, 0x0B94
     *   lsl.l #4 on 0x0B68, 0x0B6C, 0x0B60, 0x0B64, 0x0B84
     *   jsr collision_zone_check_objects   (rewinds the cursor by TYPE, see there)
     *
     * The old form read x/y/z at +4/+12/+20, the 16-bit fields from +28 on, and
     * stepped 64 bytes -- cell 17's first zone came out as x = 0xF4000001 -- and at
     * cell 27 it never met the 0xFFFF terminator, walked 10000 entries per call
     * and, called up to 100 times a frame by world_render_terrain's direction
     * search, hung the game at speed (register row 96). Entry size is 26..32
     * bytes BY TYPE; the terminator is a 0xFFFF flags word. */
    _safety_ctr = 0;
    while (1) {
      int cur;
      uint32_t flags;
      if (++_safety_ctr > 4096) break;
      cur = (int)W[0x0BC4];
      if (cur < 0 || cur + 2 > ROM_SIZE) break;
      flags = vrd16(cur);
      W[0x0BC4] = cur + 2;
      if (flags == 0xffff) break;
      W[0x0B4C] = flags & 0xf;
      W[0x0B88] = (flags & 0xf0) >> 4;
      W[0x0B50] = (flags & 0xff00) >> 8;
      cur = (int)W[0x0BC4];
      if (cur + 30 > ROM_SIZE) break;
      W[0x0B54] = (int32_t)vrd32(cur);
      W[0x0B58] = (int32_t)vrd32(cur + 4);
      W[0x0B5C] = (int32_t)vrd32(cur + 8);
      W[0x0B70] = vrd16s(cur + 12);
      W[0x0B60] = vrd16s(cur + 14);
      W[0x0B64] = vrd16s(cur + 16);
      W[0x0B68] = vrd16s(cur + 18);
      W[0x0B6C] = vrd16s(cur + 20);
      W[0x0B84] = vrd16s(cur + 22);
      W[0x0B8C] = vrd16s(cur + 24);
      W[0x0B90] = vrd16s(cur + 26);
      W[0x0B94] = vrd16s(cur + 28);
      W[0x0BC4] = cur + 30;
      W[0x0B68] = W[0x0B68] << 4;
      W[0x0B6C] = W[0x0B6C] << 4;
      W[0x0B60] = W[0x0B60] << 4;
      W[0x0B64] = W[0x0B64] << 4;
      W[0x0B84] = W[0x0B84] << 4;
      if (g_zone_dbg >= 2)
          fprintf(stderr, "[ZONEALL] f%u cell=%ld hb=%#lx type=%ld sub=%ld xyz=(%ld,%ld,%ld)\n",
                  g_sys.frame_count, (long)W[0x0D24], (unsigned long)W[0x0B50],
                  (long)W[0x0B4C], (long)W[0x0B88], (long)W[0x0B54], (long)W[0x0B58], (long)W[0x0B5C]);
      collision_zone_check_objects();
    }
    W[0x0BC4] = W[0x0BC4] + 1;
    for (iVar11 = 0; iVar11 < W[0x291C]; iVar11 = iVar11 + 1) {
      W[0x0B54] = W[0x2920 + iVar11 * 4];
      W[0x0B58] = W[0x2950 + iVar11 * 4];
      W[0x0B5C] = W[0x2980 + iVar11 * 4];
      W[0x0B84] = W[0x2A40 + iVar11 * 4];
      iVar8 = W[0x0B54] - W[0x0D00];
      iVar9 = W[0x0B5C] - W[0x0D08];
      if (iVar8 < 0) {
        iVar8 = -iVar8;
      }
      if (iVar9 < 0) {
        iVar9 = -iVar9;
      }
      if (iVar9 + iVar8 < 45000) {
        W[0x0B4C] = 0;
        W[0x0B88] = 0;
        W[0x0B50] = 1;
        W[0x0B8C] = 0;
        W[0x0B90] = 0;
        W[0x0B94] = 0;
        uVar10 = (uint16_t)W[0x29B0 + iVar11 * 4];
        if (((uVar10 < 0x2000) || (0x5fff < uVar10)) && ((uVar10 < 0xa000 || (0xdfff < uVar10)))) {
          local_c = W[0x29E0 + iVar11 * 4];
          local_8 = W[0x2A10 + iVar11 * 4];
        }
        else {
          local_c = W[0x2A10 + iVar11 * 4];
          local_8 = W[0x29E0 + iVar11 * 4];
        }
        if ((uVar10 < 0x6000) || (0x9fff < uVar10)) {
          if ((uVar10 < 0x2000) || (0x5fff < uVar10)) {
            if ((0x9fff < uVar10) && (uVar10 < 0xe000)) {
              uVar10 = uVar10 + 0x4000;
            }
          }
          else {
            uVar10 = uVar10 + 0xc000;
          }
        }
        else {
          uVar10 = uVar10 + 0x8000;
        }
        iVar8 = (int)(short)uVar10;
        if (iVar8 < 0) {
          iVar8 = iVar8 + 1;
        }
        W[0x0B70] = (int)vrd16s(0x207004 + (iVar8 >> 1) * 2) << 3;
        iVar8 = (int)(short)vrd16s(0x20B004 + ((int)(short)uVar10 >> 1 & 0x7ffe) * 2);
        if (iVar8 < 0) {
          iVar8 = -iVar8;
        }
        iVar9 = (int)(short)uVar10;
        uVar7 = iVar9 >> 1;
        if (iVar9 < 0) {
          iVar9 = iVar9 + 1;
        }
        iVar9 = (int)vrd16s(0x207004 + (iVar9 >> 1) * 2);
        if (iVar9 < 0) {
          iVar9 = -iVar9;
        }
        W[0x0B60] = (iVar9 * (iVar8 * local_c >> 0xf) >> 9) +
                       ((short)vrd16s(0x20B006 + (uVar7 & 0x7ffe) * 2) * local_c >> 0xf);
        W[0x0B68] = W[0x0B60] + 0xbb0;
        W[0x0B64] = (iVar9 * (iVar8 * local_8 >> 0xf) >> 9) +
                       ((short)vrd16s(0x20B006 + (uVar7 & 0x7ffe) * 2) * local_8 >> 0xf);
        W[0x0B6C] = W[0x0B64] + 0xbb0;
        collision_zone_check_objects();
      }
    }
  }
  return;
}





/* ---- world_grid_calc_position @ 0x006EE8 ---- */

/* Compute grid cell from world position. cell = pos / 0x18000, sub-tile = (pos % 0x18000) >> 13 */

void world_grid_calc_position(void)

{
  g_grid_cell_x = (int)W[0x0D00] / 0x18000;
  g_grid_cell_z = (int)W[0x0D08] / 0x18000;
  /* THE GRID CELL INDEX: grid_x + grid_z*8, NOT an array lookup.
   *
   * ROM 0x006F10 is `lea ([$28,a1], d0.l*8), a0` with a1 = 0xE00D00 and
   * d0 = grid_cell_z -- a 68020 MEMORY-INDIRECT POST-INDEXED mode, so it
   * loads the long at (a1 + 0x28) = W[0x0D28] = grid_cell_x and then adds
   * d0*8.  The result is `grid_x + grid_z * 8`, which is exactly the
   * terrain layout this file already documents (`offset = grid_z*8 + grid_x`,
   * 128 chunks in an 8x16 grid, so the value is bounded 0..127).
   *
   * Ghidra rendered it as an ARRAY INDEX off `&g_grid_cell_x`, the
   * `(&W[base])[N]` stride class: `(&W[0x0D28])[z*2]` reads SLOT 0x0D28+2z.
   * At z == 0 that is W[0x0D28] and happens to be right, which is why it
   * survived every headless test -- those sit at z-cell 0.  At any other z
   * it reads a 2-mod-4 slot the sync rebuilds from neighbours, and
   * balloon_render_and_hit_check then indexes ROM with it:
   *     iVar19 = vrd32s(W[0x15FF4] + 4 + W[0x0D24]*8)
   * Measured from the user's core (PID 782441): that count came back
   * 1224683775, so the hit-check loop walked the 16-bit index list far past
   * its end and read iVar21 = -28521, faulting at 0xffff68ba. */
  W[0x0D24] = g_grid_cell_x + g_grid_cell_z * 8;
  g_grid_sub_x = (int)W[0x0D00] % 0x18000;
  g_grid_sub_z = (int)W[0x0D08] % 0x18000;
  g_grid_tile_x = (int)W[0x0D00] % 0x18000 >> 0xd;
  g_grid_tile_z = (int)W[0x0D08] % 0x18000 >> 0xd;
  g_grid_frac_x = W[0x0D00] & 0x1fff;
  g_grid_frac_z = W[0x0D08] & 0x1fff;
  return;
}





/* ---- world_props_init_dispatch @ 0x006BDA ---- */

void world_props_init_dispatch(uint32_t param_1)
{
  RomStageGeometry geom;

  W[0x28F0] = 0;
  W[0x28F4] = 0;
  W[0x28F8] = 0;
  W[0x28FC] = 0;
  W[0x2900] = 0;
  W[0x2904] = 0;

  if (param_1 > 3) return;

  /* terrain_base_addr / bitmask_ptr — same per-course values that
   * terrain_chunk_render writes every frame, set here too so any code
   * that runs between course init and the first terrain_chunk_render
   * sees consistent state. */
  g_terrain_base_addr   = course_terrain_base[param_1];
  g_terrain_bitmask_ptr = course_terrain_bitmask[param_1];

  /* W[0x2908..0x2918] — five ROM byte addresses pointing at the
   * geometry cell table, cell descriptor, height map, object mask, and
   * collision zone table for this course. Consumed by
   * world_stage_geometry_load (slice 2 rewrite) and world_stage_update
   * (W[0x2910] / W[0x2914] for the per-cell layer count). Without this
   * wiring those slots stay 0 and the consumers either short-circuit
   * (slice 2's bounds check) or read garbage from ROM[0..7]. */
  if (rom_stage_geometry((int)param_1, &geom)) {
    W[0x2908] = (intptr_t)(int32_t)geom.geom_cell_table;
    W[0x290C] = (intptr_t)(int32_t)geom.cell_descriptor;
    W[0x2910] = (intptr_t)(int32_t)geom.height_map;
    W[0x2914] = (intptr_t)(int32_t)geom.object_mask;
    W[0x2918] = (intptr_t)(int32_t)geom.collision_zones;
  }

  if (propcycl_verbose()) printf("[TERRAIN] world_props_init course %u: base=0x%X bitmask=0x%X "
         "geom=0x%X cells=0x%X heights=0x%X masks=0x%X zones=0x%X\n",
         (unsigned)param_1, (int)g_terrain_base_addr, (int)g_terrain_bitmask_ptr,
         (int)W[0x2908], (int)W[0x290C], (int)W[0x2910], (int)W[0x2914],
         (int)W[0x2918]);
}





/* ---- world_render_terrain @ 0x006F92 ---- */



void world_render_terrain(void)

{
  int wrt_d5 = 0, wrt_d4 = 0;   /* the M68K's d5/d4 -- see the switch below */
  int wrt_search_iters = 0;     /* escape-search iteration count, for the bail below */
  uint32_t uVar1;
  short sVar2;
  int iVar3;
  int iVar4;
  /* void */;
  /* void */;
  uint8_t *puVar5;
  int *piVar6;
  undefined1 *puVar7;
  int local_1c8;
  int local_1c4;
  int local_1c0;
  int local_1bc;
  int local_1b8;
  int local_1b4;
  int local_1b0;
  undefined4 local_1ac;
  int local_1a8;
  int local_1a4;
  undefined4 local_1a0;
  int local_19c;
  int local_198;
  undefined4 local_194;
  int local_190;
  int local_18c;
  uint32_t local_188;
  int local_184;
  int local_17c;
  int aiStack_178 [11];
  int local_14c;
  int local_148;
  int local_144;
  int local_140;
  int local_13c;
  int local_138;
  int local_134;
  int local_130;
  int local_12c;
  int local_128;
  int aiStack_124 [36];
  int aiStack_94 [36];
  
  /* Three ROM tables copied to the stack. The M68K does it with a BYTE loop
   * (`move.b (A1)+,(A0)+` at 0x006FB8, 0x2C bytes; 0x90 for the other two),
   * which on a big-endian CPU yields big-endian ints in the array. A host
   * memcpy into int[] on x86 yields the bytes LITTLE-endian instead -- and the
   * first table is the ESCAPE-PROBE RADIUS list the direction search below
   * escalates through, [1024, 2048, 3072, 4096, 5120, 8192, 12288, 16384,
   * 24576, 31744] terminated by 0x8000. Byte-swapped it reads [262144,
   * 524288, ...]: every entry is already >= 0x8000, so the first probe is the
   * give-up sentinel, `local_190 += 0x1800`, and every look-ahead contact took
   * the hard-reset branch -- speed 0x900 and the position restored to a
   * saved point -- which is the "hits the ground and teleports back up"
   * (register row 97). MAME rides the surface on the identical dive: its
   * largest per-frame |dy| over 900 frames is 302; ours was 64,580. */
  { int _i;
    for (_i = 0; _i < 11; _i++) aiStack_178[_i] = (int32_t)vrd32(0x39FA0 + _i * 4);
    for (_i = 0; _i < 36; _i++) aiStack_124[_i] = (int32_t)vrd32(0x39FCC + _i * 4);
    for (_i = 0; _i < 36; _i++) aiStack_94[_i]  = (int32_t)vrd32(0x3A05C + _i * 4); }
  /* Per-cell table at ROM 0x121DF4, indexed course*0x200 + cell. This was
   * an unbounded NATIVE `*(uint16_t*)` deref of big-endian ROM: byte-swapped
   * when in range, and a SIGSEGV as soon as the bike flew off the 16x32
   * cell grid (Z reached 1.6M once spherical_to_cartesian was fixed). Read
   * big-endian and bound the cell to the grid. */
  { long cx = W[0x0D00] / 0xc000, cz = W[0x0D08] / 0xc000;
    if (cx < 0) cx = 0; if (cx > 15) cx = 15;
    if (cz < 0) cz = 0; if (cz > 31) cz = 31;
    uint32_t off = 0x121DF4 + ((uint32_t)W[0x0E0C] * 0x200 + (uint32_t)cx + (uint32_t)cz * 0x10) * 2;
    local_18c = (off + 1 < ROM_SIZE) ? -(uint32_t)vrd16(off) : 0; }
  W[0x12D4] = 0;
  W[0x12DC] = 0;
  W[0x0D4C] = W[0x0D48];
  iVar4 = 0;
  if (W[0x12E4] == 0) {
LAB_00007050:
    iVar3 = 1;
  }
  else {
    iVar3 = (int)(short)((short)W[0x0DA0] - (short)W[0x0DAC]);
    if (iVar3 < 0) {
      iVar3 = -iVar3;
    }
    iVar3 = iVar3 >> 0xd;
    if (iVar3 < 1) goto LAB_00007050;
  }
  W[0x12CC] = W[0x12CC] - iVar3;
  if (W[0x12CC] < 0) {
    W[0x12CC] = 0;
  }
  W[0x12D0] = W[0x12D0] + -1;
  if (W[0x12D0] < 0) {
    W[0x12D0] = 0;
  }
  W[0x12E0] = W[0x12E0] + -1;
  if (W[0x12E0] < 0) {
    W[0x12E0] = 0;
  }
  W[0x12D8] = W[0x12D8] + -1;
  if (W[0x12D8] < 0) {
    W[0x12D8] = 0;
  }
  if (W[0x1964] == 0) goto LAB_0000767a;
  if (g_wrt_dbg) { int _L;
      fprintf(stderr, "[WRT] f%u look-ahead contact: player=(%ld,%ld,%ld) spd=%ld pitch=%ld hdg=%ld tgtpitch=%ld tgthdg=%ld\n",
              g_sys.frame_count, (long)W[0x0D00], (long)W[0x0D04], (long)W[0x0D08], (long)W[0x0D48],
              (long)W[0x0D0C], (long)W[0x0D10], (long)W[0x0D9C], (long)W[0x0DA0]);
      for (_L = 0; _L < 6; _L++) { int _i, _n = (int)W[0x1330 + _L*0x140];
          fprintf(stderr, "[WRT]   layer %d probe=(%ld,%ld,%ld) flag=%ld floor=%ld faces(%d):", _L,
                  (long)W[0x1318 + _L*0x140], (long)W[0x131C + _L*0x140], (long)W[0x1320 + _L*0x140],
                  (long)W[0x1324 + _L*0x140], (long)W[0x1328 + _L*0x140], _n);
          for (_i = 0; _i < _n && _i < 8; _i++) { long _w = (long)(int32_t)W[0x1334 + _L*0x140 + _i*4];
              fprintf(stderr, " %ld|%ld k%ld", _w >> 4, _w & 0xF, (long)(uint8_t)W[0x13AC + _L*0x140 + _i]); }
          fprintf(stderr, "  cellkeys=%ld,%ld\n", (long)W[0x0990], (long)W[0x0D24]); } }
  if (0xe9e < W[0x0D48]) {
    W[0x0D48] = W[0x0D48] - (W[0x0D48] >> 3);
  }
  local_190 = 0;
  local_17c = 0;
  local_188 = 0;
  local_184 = 0;
  if ((W[0x0D48] < 600) && (0x1000 < (short)W[0x0D9C])) {
    local_184 = 1;
    W[0x0D9C] = W[0x0D9C] + -0x4800;
  }
  puVar5 = &g_sys.rom[0x01400];
  iVar4 = aiStack_178[0];
  if (((W[0x0E0C] == 1) && (0xd1d < W[0x0D48])) &&
     (((0x20 < W[0x0D24] && (W[0x0D24] < 0x47)) ||
      ((W[0x0D24] == 0x11 || (W[0x0D24] == 0x19)))))) {
    local_130 = g_grid_sub_x >> 0xe;
    local_12c = g_grid_sub_z >> 0xe;
    local_128 = (g_grid_sub_x >> 0xe) + (g_grid_sub_z >> 0xe) * 6;
    if (W[0x0D24] == 0x21) {
      local_18c = aiStack_124[local_128];
    }
    if (W[0x0D24] == 0x44) {
      local_18c = aiStack_94[local_128];
    }
    local_134 = (int)(short)((short)local_18c - (short)W[0x0DA0]);
    if ((0x1000 < local_134) || (local_134 < -0x1000)) {
      puVar5 = (uint8_t *)0x1000;
    }
    if ((0x2000 < local_134) || (local_134 < -0x2000)) {
      puVar5 = (uint8_t *)0x800;
    }
  }
  _safety_ctr = 0;
  do {
    uVar1 = local_188;
    g_sys.syscon[0x14] = 0;
    if (3 < (int)local_188) {
      local_188 = 0;
      local_17c = local_17c + 1;
      iVar4 = aiStack_178[local_17c];
    }
    if (0x7fff < iVar4) {
      local_17c = 0;
      local_190 = local_190 + 0x1800;
      iVar4 = aiStack_178[0];
    }
    /* THE FOUR-WAY DIRECTION SWITCH, recovered from the jump table.
     *
     * Ghidra could not name either variable, emitted all eight assignments as
     * comments (`// 0 = iVar4;`) and then passed literal 0 for both -- so this
     * loop rotated by nothing at all. It also got case 0 wrong, duplicating
     * case 2.
     *
     * The M68K dispatch is at 0x007218:
     *     move.w $7228(pc, d0.w*2), d0
     *     nop
     *     jmp    $7222(pc, d0.w)
     * with the table at 0x7222 holding [0x0008, 0x000E, 0x0016, 0x001C], i.e.
     * targets 0x722A / 0x7230 / 0x7238 / 0x723E, which are:
     *     0x722A  move.l d6,d5 ; moveq #0,d4      -> ( d6, 0)
     *     0x7230  move.l d6,d5 ; neg.l d5 ; ...   -> (-d6, 0)
     *     0x7238  moveq #0,d5  ; move.l d6,d4     -> ( 0, d6)
     *     0x723E  moveq #0,d5  ; move.l d6,d4 ; neg.l d4  -> (0, -d6)
     * i.e. the four compass directions, d6 being iVar4.
     *
     * d5 and d4 are then the two angles the decompiler zeroed: at 0x007254
     * d5 is param_4 of rotate_vector_euler_xyz (the X Euler angle), and at
     * 0x007276 d4 is param_5 of rotate_euler_yxz_optimized -- the call whose
     * other two arguments, $9c(a2) and $a4(a2) with a2 = 0xE00D00, are
     * already correct as W[0x0D9C] and W[0x0DA4].
     *
     * They are registers in the original, so they persist across iterations
     * when the switch is skipped; hence declared outside the loop. */
    if (local_188 < 4) {
      switch(uVar1) {
      default:                                   /* 0x722A */
        wrt_d5 = iVar4;
        wrt_d4 = 0;
        break;
      case 1:                                    /* 0x7230 */
        wrt_d5 = -iVar4;
        wrt_d4 = 0;
        break;
      case 2:                                    /* 0x7238 */
        wrt_d5 = 0;
        wrt_d4 = iVar4;
        break;
      case 3:                                    /* 0x723E */
        wrt_d5 = 0;
        wrt_d4 = -iVar4;
      }
    }
    rotate_vector_euler_xyz(0,0,0x10000,wrt_d5,0,0,&local_1c8,&local_1c4,&local_1c0);
    rotate_euler_yxz_optimized
              (local_1c8,local_1c4,local_1c0,W[0x0D9C],wrt_d4,W[0x0DA4],&local_1c8,
               &local_1c4,&local_1c0);
    local_1b8 = math_atan2(local_1c8,local_1c0);
    rotate_vector_euler_xyz
              (local_1c8,local_1c4,local_1c0,0,-local_1b8,0,&local_1c8,&local_1c4,&local_1c0);
    local_1bc = math_atan2(local_1c0,local_1c4);
    local_1bc = local_1bc + 0x4000;
    local_1a4 = local_1b8 + W[0x0DA0];
    local_1a0 = 0;
    local_1a8 = local_1bc;
    rotate_euler_zxy_optimized
              (0,0,local_190 + 0x2b0,local_1bc,local_1a4,0,&local_1c8,&local_1c4,&local_1c0);
    if (g_wrt_dbg)
        fprintf(stderr, "[WRT]   search it=%d dir=%d ridx=%d radius=%d off190=%d dist=%d vec=(%d,%d,%d) rot=(%ld,%ld) tgt=(%ld,%ld)\n",
                (int)local_188, (int)local_184, (int)local_17c, (int)aiStack_178[local_17c], (int)local_190,
                (int)(local_190 + 0x2b0), (int)local_1c8, (int)local_1c4, (int)local_1c0, (long)wrt_d5, (long)wrt_d4,
                (long)local_1bc, (long)local_1a4);
    W[0x1318] = local_1c8 + W[0x0D00];
    W[0x131C] = local_1c4 + W[0x0D04];
    W[0x1320] = local_1c0 + W[0x0D08];
    W[0x1308] = 1;
    local_14c = W[0x1318];
    local_148 = W[0x131C];
    local_144 = W[0x1320];
    world_objects_tick_all();
    world_collision_zones_process();
    terrain_height_resolve();
    local_188 = local_188 + 1;
    /* Safety: break if terrain height never resolves. The old guard read
     * `local_188 > 100`, but local_188 wraps to 0 every four iterations (it
     * is the direction counter), so the guard could NEVER fire and a search
     * with no escape position hung the game outright -- measured: an
     * infinite loop at 99.7% CPU, frame counter frozen, reproducible by
     * pinning the player at the course-1 spawn on the first gameplay frame
     * (PROPCYCL_REPLAY / WARPSEQ). The ROM has no cap here; the guard is
     * ours by design, so make it count TOTAL iterations. 400 = the intended
     * 100 full four-direction cycles. */
    if (++wrt_search_iters > 400) { W[0x1324] = 0; break; }
  } while (W[0x1324] != 0);
  if (((0 <= -(int)puVar5) || ((int)puVar5 <= 0)) && (0xd00 < W[0x0D48])) {
    W[0x12D4] = 1;
    local_1b4 = (short)0 * 3;
    local_1b0 = 0;
    local_1ac = 0;
  }
  if (((0 <= -(int)puVar5) || ((int)puVar5 <= 0)) && (0xd00 < W[0x0D48])) {
    W[0x12D4] = 1;
    local_1b0 = (short)0 * 3;
    local_1b4 = 0;
    local_1ac = 0;
  }
  if (W[0x12D4] == 0) {
    if (local_184 == 0) {
      iVar4 = 0x38;
      W[0x12E4] = 3;
      iVar3 = (int)(short)W[0x0DA0] - (int)(short)local_1a4;
      if ((-0x2001 < iVar3) && (iVar3 < 0x2001)) goto LAB_0000761a;
      W[0x12E4] = 4;
    }
    else {
      W[0x12E4] = 5;
    }
    W[0x12E0] = 0x78;
    iVar4 = 0x78;
  }
  else {
    rotate_vector_euler_xyz(0,0,0x10000,local_1b4,0,0,&local_1c8,&local_1c4,&local_1c0);
    rotate_euler_yxz_optimized
              (local_1c8,local_1c4,local_1c0,W[0x0D9C],local_1b0,W[0x0DA4],&local_1c8,
               &local_1c4,&local_1c0);
    local_1b8 = math_atan2(local_1c8,local_1c0);
    rotate_vector_euler_xyz
              (local_1c8,local_1c4,local_1c0,0,-local_1b8,0,&local_1c8,&local_1c4,&local_1c0);
    local_1bc = math_atan2(local_1c0,local_1c4);
    local_1bc = local_1bc + 0x4000;
    local_198 = local_1b8 + W[0x0DA0];
    local_194 = 0;
    if ((short)local_18c != -1) {
      local_198 = (int)(short)local_18c;
    }
    local_19c = local_1bc;
    rotate_euler_zxy_optimized
              (0,0,local_190 + 0x2b0,local_1bc,local_198,0,&local_1c8,&local_1c4,&local_1c0);
    W[0x1318] = local_1c8 + W[0x0D00];
    W[0x131C] = local_1c4 + W[0x0D04];
    W[0x1320] = local_1c0 + W[0x0D08];
    W[0x1308] = 1;
    local_140 = W[0x1318];
    local_13c = W[0x131C];
    local_138 = W[0x1320];
    world_objects_tick_all();
    world_collision_zones_process();
    terrain_height_resolve();
    sVar2 = (short)W[0x0D9C];
    if (W[0x1324] == 0) {
      if (sVar2 < -0x1cff) {
        iVar4 = 0x60;
        W[0x12D0] = 0;
        W[0x12E4] = 1;
      }
      else {
        local_1a8 = local_19c;
        local_1a0 = local_194;
        local_1a4 = local_198;
        iVar4 = 0x60;
        W[0x12D0] = 0xe;
        W[0x12E4] = 0;
      }
    }
    else {
      W[0x12D4] = 0;
      iVar4 = 0x40;
      W[0x12D0] = 0xe;
      W[0x12E4] = 2;
    }
    if (sVar2 < 0) {
      W[0x0D48] = (int)-sVar2 / -10 + 0x900;
      if (W[0x0D48] < 0x500) {
        W[0x0D48] = 0x500;
      }
    }
    else {
      W[0x0D48] = 0x900;
    }
  }
LAB_0000761a:
  W[0x0D9C] = local_1a8;
  W[0x0DA0] = local_1a4;
  W[0x0DA4] = local_1a0;
  if (0 < local_190) {
    W[0x0D48] = 0x900;
    if (W[0x12D4] == 0) {
      W[0x0D00] = local_14c;
      W[0x0D04] = local_148;
      W[0x0D08] = local_144;
    }
    else {
      W[0x0D00] = local_140;
      W[0x0D04] = local_13c;
      W[0x0D08] = local_138;
    }
  }
  if (((W[0x12D4] == 0) && (0 == 0)) && (0xbab < W[0x0D48])) {
    W[0x12DC] = 1;
  }
LAB_0000767a:
  if (W[0x12D4] != 0) {
    W[0x12D8] = 0x1e;
  }
  if (W[0x12CC] < iVar4) {
    W[0x12CC] = iVar4;
  }
  return;
}




/* CONCAT/SBORROW defined as macros in header area above */

