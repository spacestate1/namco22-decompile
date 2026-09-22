/*
 * Player & Bicycle Physics
 * Auto-split from game_deps.c / game_ported.c
 */
#include "propcycl.h"
#include "ending_rd.h"

/* Work RAM as intptr_t array */
extern intptr_t _W[];
#define W _W

/* ROM as byte array */
#define R g_sys.rom

/* Helper for the bicycle-IK trig divisions: guard against divide-by-zero
 * when the cosine/sine table read returns 0 (90° / 0° edge cases). On
 * the M68K the divide instruction would have trapped to a vector that
 * the original game probably never hit in normal gameplay because the IK
 * inputs were always within the safe angle window. In our recreated
 * attract path we sometimes hit it; the safe fallback is 0 (i.e. the
 * limb stays at its previous position rather than tearing). */
#define SAFE_DIV(num, den) ({ int _d = (den); _d ? (int)(num) / _d : 0; })

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

/* ---- player_animation_keyframe_update ---- */

void player_animation_keyframe_update(int param_1)

{
  uint8_t *puVar1;
  uint32_t uVar2;
  int iVar3;
  int iVar4;
  int iVar5;

  /* NO GUARD HERE -- the ROM has none. 0x027E1E is movem / move.l
   * $28(a7),d3 / two immediates and straight into the arithmetic.
   *
   * The guard that used to stand here, `if (W[0x16A08]==0 && W[0x16A0A]==0)
   * return;`, was self-defeating: W[0x16A08] is written by THIS function as
   * its last act (0x027E9E), so the function could never run a first time.
   * That is why the ROM branch emitted 0 of 16 rider parts. */

  W[0x169F8] = (((W[0x0D50] >> 5) + (W[0x0D50] >> 4) + W[0x2C04] * -0x100) -
                  W[0x169F8] >> 3) + W[0x169F8];
  /* 0x1400 is an IMMEDIATE, not an address -- the M68K at 0x027E6A is
   * `addi.l #$1400,d2`, and W[0x16A0C] is read 16-bit (`movea.w
   * $e16a0c.l,a0` at 0x027E5C). Ghidra turned the immediate into
   * `&g_sys.rom[0x1400]`, a host pointer, so this index was garbage --
   * exactly the class as 0x238E five lines below, the sky dome's Z and
   * player_render's 0x11C6 (register row 63). */
  iVar4 = (int)(0x1400 + W[0x169F8] + W_HI16(0x16A0C)) >> 0xb;
  if (3 < iVar4) {
    iVar4 = 3;
  }
  if (iVar4 < 0) {
    iVar4 = 0;
  }
  if (W[0x16A08] < iVar4) {
    W[0x16A0C] = 0x80;
  }
  if (iVar4 < W[0x16A08]) {
    W[0x16A0C] = -0x80;
  }
  W[0x169FC] = ((W[0x0D54] >> 5) - W[0x169FC] >> 2) + W[0x169FC];
  iVar5 = W[0x169FC] + W[0x16A0E] + 0x480 >> 8;
  if (8 < iVar5) {
    iVar5 = 8;
  }
  if (iVar5 < 0) {
    iVar5 = 0;
  }
  if (W[0x16A04] < iVar5) {
    W[0x16A0E] = 0x10;
  }
  if (iVar5 < W[0x16A04]) {
    W[0x16A0E] = -0x10;
  }
  iVar3 = param_1 * 0x10000;
  if (iVar3 < 0) {
    iVar3 = iVar3 + 0x3f;
  }
  W[0xAE34] = iVar3 >> 6;
  /* 0x238E is an IMMEDIATE, not an address. The M68K is `lea 0x238E.w,a0`
   * (41F8 238E at ROM 0x027F24); Ghidra turned the short absolute into
   * `&g_sys.rom[0x238E]`, a host pointer, so this angle came out as
   * -647474834 and nodes 7/8 -- the pair hanging off node 6 -- were
   * oriented ~127 deg wrong in BOTH the flyover and gameplay. */
  W[0xAEB4] = 0x238E - W[0xAE34];
  /* ---- THE GAMEPLAY KEYFRAME TABLE ------------------------------------
   *
   * Rewritten from the M68K at 0x027F02..0x0280B0. Ghidra could not render
   * the addressing here and produced `W[0x16A08 + iVar4*2] + iVar5`, a
   * work-RAM slot that happens to hold the previous iVar4 -- which is why
   * RIDER_RIG.md recorded this table as unreversed and a ROM signature scan
   * as "1147 candidates, nothing conclusive".
   *
   *   027F02: movea.l d6,a0            ; a0 = 0x00E16A08
   *   027F04: move.l  (a0),d0          ; d0 = W[0x16A08] = iVar4
   *   027F06: lea.l   ([a0],d0.l*8),a0 ; 68020 FULL format, I/IS=101 =
   *                                    ; memory indirect POST-indexed:
   *                                    ;   a0 = MEM32[A0] + D0*8 = iVar4*9
   *   027F0A: add.l   a0,d2            ; d2 = iVar5 + iVar4*9
   *   027F3C: add.l   $144a5c(d2.l*4),d4
   *
   * So the table is at ROM 0x144A5C, 32-bit big-endian pointers, indexed
   * idx = iVar5 + iVar4*9 -- 4 rows x 9 columns, which is exactly what the
   * function's own clamps produce (iVar4 to 0..3 at 0x027E76, iVar5 to 0..8
   * at 0x027ED0). All 36 entries land inside the 4 MB ROM.
   *
   * Each entry+0xC is a record of TWELVE pointer fields, and the field ->
   * node map below is read straight off the a4 walk: a4 starts at node 9
   * (base+0x480) and steps by 0x80, so fields 0,1,4..13 drive nodes
   * 9,10,13,14,15,16,17,18,19,20,21,22. The twelve offsets agree with the
   * list already recorded in RIDER_RIG.md (+0x0c,+0x10,+0x1c,+0x20,...). */
  {
    uint32_t idx  = (uint32_t)(iVar5 + iVar4 * 9);
    uint32_t rec  = (uint32_t)vrd32(0x144A5C + idx * 4) + 0xC;
    uint32_t pa   = (uint32_t)param_1 * 4;
    #define KF_FLD(k)  ((uint32_t)vrd32(rec + (k) * 4))
    #define NB(n)      (0xAB04 + (n) * 0x80)

    /* A: field 0 -> node 9   (0x027F44-0x027F58) */
    { uint32_t a2 = KF_FLD(0) + 0xC;
      W[NB(9) + 0x24] = vrd32s((uint32_t)vrd32(a2) + pa);
      W[NB(9) + 0x38] = vrd32s(a2 + 0x14); }

    /* B: field 1 -> node 10  (0x027F5A-0x027F6E) */
    { uint32_t a2 = KF_FLD(1) + 0x18;
      W[NB(10) + 0x30] = vrd32s(a2);
      W[NB(10) + 0x38] = vrd32s(a2 + 8); }

    /* C: three indexed pointers -> the node's three Euler angles.
     * D: one indexed pointer through +0x18 -> the node's X angle. */
    #define KF_C(k, n) do { uint32_t a2 = KF_FLD(k) + 0x18; \
        W[NB(n) + 0x30] = vrd32s((uint32_t)vrd32(a2      ) + pa); \
        W[NB(n) + 0x34] = vrd32s((uint32_t)vrd32(a2 + 4  ) + pa); \
        W[NB(n) + 0x38] = vrd32s((uint32_t)vrd32(a2 + 8  ) + pa); } while (0)
    #define KF_D(k, n) do { uint32_t p = KF_FLD(k); \
        W[NB(n) + 0x30] = vrd32s((uint32_t)vrd32(p + 0x18) + pa); } while (0)
    /* E: a 16-bit field at +4, then six longs from +0xC into the node's
     * local offset AND its three angles (0x028026-0x028050). */
    #define KF_E(k, n) do { uint32_t p = KF_FLD(k), a2 = p + 0xC; \
        W_SET_HI16(NB(n) + 0x04, vrd16s(p + 4)); \
        int _i; for (_i = 0; _i < 6; _i++) \
            W[NB(n) + 0x24 + _i * 4] = vrd32s(a2 + _i * 4); } while (0)

    KF_C(4, 13);   KF_D(5, 14);
    KF_C(6, 15);   KF_D(7, 16);
    KF_C(8, 17);   KF_D(9, 18);
    KF_E(10, 19);
    KF_C(11, 20);  KF_D(12, 21);
    KF_E(13, 22);
    #undef KF_C
    #undef KF_D
    #undef KF_E
    #undef KF_FLD
    #undef NB
  }

  /* Tail, 0x0280B2-0x0280EE: node 12's MODEL ID is animated -- base 0x6E
   * or 0x77 depending on W[0x12BC] (the -0x19 variant flag), plus a 3-bit
   * phase off the frame counter shifted by a speed-derived amount.
   * Cross-checks against the live dump: gameplay has W[0x12BC]==0 and
   * shows node 12 model 113 = 0x6E + 3; the flyover shows 119 = 0x77. */
  {
    int32_t sh = (int32_t)W[0x0D48] >> 11;
    sh = (sh - 3 > 0) ? 0 : (3 - sh);
    int32_t basem = (W[0x12BC] != 0) ? 0x77 : 0x6E;
    W[0xAB04 + 12 * 0x80] = basem + (((int32_t)W[0x0C8C] >> sh) & 7);
  }
  W[0xAF34] = W[0xAEB4];
  W[0x16A04] = iVar5;
  W[0x16A08] = iVar4;
  camera_dsp_terrain_render();
  return;
}


/* ---- player_animation_state_update ---- */

static int g_anim_done_cmp;

void player_animation_state_update(void)

{
  /* Guard: the node chain must actually be loaded.
   *
   * This used to test `W[0x16A08] == 0 && W[0x16A0A] == 0`, but
   * player_model_load_animation sets W[0x16A08] to 0 as its LAST act (as
   * the original does), and nothing ever writes 0x16A0A -- so the guard
   * fired on every frame and the rider was never animated or emitted.
   * Test the thing that actually matters instead: the head of the chain
   * this function walks must hold a sane model id. */
  { intptr_t _h = ((int)W_HI16(0x169EC) == 0) ? 0xAE04 : 0xAB84;
    int32_t _m = (int32_t)W[_h];
    if (_m <= 0 || _m >= 0x8000) return; }

  short *psVar1;
  undefined4 *puVar2;
  int iVar3;
  uint32_t uVar4;
  int iVar5;
  int *piVar6;
  int *piVar7;
  
  if (W[0x12BC] == 0) {
    iVar3 = 0;
  }
  else {
    iVar3 = -0x19;
  }
  if (W[0x0D48] >> 0xb < 4) {
    uVar4 = 3 - (W[0x0D48] >> 0xb);
  }
  else {
    uVar4 = 0;
  }
  uVar4 = W[0x0C8C] >> (uVar4 & 0x3f) & 7;
  if (W[0x16A28] == 1) {
    if (W[0x12BC] == 0) {
      if (W[0xEB04] < 9) {
        iVar5 = 0x6e;
      }
      else {
        iVar5 = 0x76;
      }
      W[0xB104] = uVar4 + iVar5;
      W[0xB084] = 0x5d;
    }
    else {
      if (W[0xEB04] < 9) {
        iVar5 = 0x77;
      }
      else {
        iVar5 = 0x7f;
      }
      W[0xB104] = uVar4 + iVar5;
      W[0xB084] = 0x5d;
    }
  }
  else {
    if (W[0x12BC] == 0) {
      iVar5 = 0x6e;
    }
    else {
      iVar5 = 0x77;
    }
    W[0xB104] = uVar4 + iVar5;
  }
  W[0xEB08] = W[0x0D00] - W[0x0CDC];
  W[0xEB0C] = W[0x0D04] - W[0x0CE0];
  W[0xEB10] = W[0x0D08] - W[0x0CE4];
  /* RIDER / BIKE NODE WALK -- per-frame animation + emission.
   *
   * nb is a _W[] SLOT index. This walked with `int *piVar6/7` over an
   * intptr_t[] array (the (&W[base])[N] stride bug), so every field read
   * was misaligned and the walk emitted one wrong model 48 times instead
   * of the chain player_model_load_animation builds:
   *     0xAB04..0xAD84  6 bike nodes   (models 69-74, codes 138-143)
   *     0xAE04..        16 rider nodes (models 77-92,  codes 146-161)
   * stride 0x20 ints = 0x80 slots, terminated by a negative model id.
   *
   * Half-words use W_HI16/W_LO16: `*(short *)(int_ptr + N)` is the TOP
   * half of the word on the big-endian M68K (see vaddr.h).
   *
   * The per-DOF masks were read as `R[0x37664]` -- ONE BYTE of a 32-bit
   * big-endian word, i.e. always 0x00, so every animation delta was
   * skipped and the rider could never move even once it drew. They are
   * 1,2,4,8,0x10,0x20 and must be read with vrd32.
   */
  {
    intptr_t nb = ((int)W_HI16(0x169EC) == 0) ? 0xAE04 : 0xAB84;
    int _dbg = getenv("PROPCYCL_ANIM") != NULL;
    static int _fr; int _n = 0;
    if (_dbg && _fr++ % 200 == 0)
        printf("    [ANIM] f=%d enter nb=0x%lX head_model=%ld\n",
               (int)g_sys.frame_count, (long)nb, (long)(int32_t)W[nb]);
    for (;;) {
      uint32_t flags = (uint32_t)(uint16_t)W_HI16(nb + 8);
      if (flags != 0) {
        int d;
        for (d = 0; d < 6; d++) {
          uint32_t mask = vrd32(0x37664 + d * 4);
          if ((mask & flags) != 0) {
            uint32_t strm = (uint32_t)W[nb + (3 + d) * 4];   /* keyframe cursor */
            if (strm && strm < ROM_SIZE) {
              W[nb + (3 + d) * 4] = (int32_t)(strm + 2);
              W[nb + (9 + d) * 4] = (int32_t)vrd16s(strm) + (int32_t)W[nb + (9 + d) * 4];
            }
          }
        }
      }
      *(int32_t*)W[0x0CA4] = 0x8008;
      ((int32_t*)W[0x0CA4])[1] = 3;
      ((int32_t*)W[0x0CA4])[2] = 1;
      { uint32_t _a = (uint32_t)(W[nb + 0xc * 4] & 0xfffc);
        ((int32_t*)W[0x0CA4])[3] = (int32_t)vrd32(0x20B002 + _a);
        ((int32_t*)W[0x0CA4])[4] = (int32_t)vrd32(0x20B004 + _a); }
      { uint32_t _a = (uint32_t)(W[nb + 0xd * 4] & 0xfffc);
        ((int32_t*)W[0x0CA4])[5] = (int32_t)vrd32(0x20B002 + _a);
        ((int32_t*)W[0x0CA4])[6] = (int32_t)vrd32(0x20B004 + _a); }
      { uint32_t _a = (uint32_t)(W[nb + 0xe * 4] & 0xfffc);
        ((int32_t*)W[0x0CA4])[7] = (int32_t)vrd32(0x20B002 + _a);
        ((int32_t*)W[0x0CA4])[8] = (int32_t)vrd32(0x20B004 + _a); }
      ((int32_t*)W[0x0CA4])[9]   = (int)W_LO16(nb + 8);
      ((int32_t*)W[0x0CA4])[10]  = 0;
      ((int32_t*)W[0x0CA4])[0xb] = (int32_t)W[nb + 9 * 4];
      ((int32_t*)W[0x0CA4])[0xc] = (int32_t)W[nb + 10 * 4];
      ((int32_t*)W[0x0CA4])[0xd] = (int32_t)W[nb + 0xb * 4];
      ((int32_t*)W[0x0CA4])[0xe] = 0xffffffff;
      ((int32_t*)W[0x0CA4])[0xf] = 0x8009;
      ((int32_t*)W[0x0CA4])[0x10] = 3;
      { intptr_t _p = nb + (W_HI16(nb + 4) * 0x20 + 0x1e) * 4;
        ((int32_t*)W[0x0CA4])[0x11] =
            (_p >= 0 && _p < WORK_RAM_SIZE) ? (int32_t)W[_p] : 0; }
      ((int32_t*)W[0x0CA4])[0x12] = (int32_t)W[nb + 0x1e * 4];
      ((int32_t*)W[0x0CA4])[0x13] = 0x800a;
      ((int32_t*)W[0x0CA4])[0x14] = iVar3 + (int32_t)W[nb];
      ((int32_t*)W[0x0CA4])[0x15] = (int32_t)W[nb + 0x1e * 4];
      ((int32_t*)W[0x0CA4])[0x16] = W[0xEB08];
      ((int32_t*)W[0x0CA4])[0x17] = W[0xEB0C];
      ((int32_t*)W[0x0CA4])[0x18] = W[0xEB10];
      W[0x0CA4] = W[0x0CA4] + 0x19 * 4;
      if (_dbg && _fr % 200 == 1 && _n < 20)
          printf("      [ANIM] emit model=%ld (code %ld)\n",
                 (long)(iVar3 + (int32_t)W[nb]), (long)(iVar3 + (int32_t)W[nb] + 0x45));
      _n++;
      if (!(-1 < (int32_t)W[nb + 0x20 * 4])) break;
      nb += 0x20 * 4;
      if (nb + 0x20 * 4 >= WORK_RAM_SIZE) break;
    }
    g_anim_done_cmp = (int)W_LO16(nb + 4);
  }
  W[0xEB04] = W[0xEB04] + 1;
  W[0xEB06] = (uint16_t)(g_anim_done_cmp <= (int)W[0xEB04]);
  return;
}


/* ---- player_bounds_check ---- */

void player_bounds_check(void)

{
  player_update_prev_pos();
  player_clamp_position();
  player_clamp_analog();
  return;
}


/* ---- player_init_physics ---- */

void player_init_physics(void)

{
  W[0x0D48] = 0xc00;
  W[0x0D9C] = W[0x0D0C];
  W[0x0DA0] = W[0x0D10];
  W[0x0DA4] = W[0x0D14];
  W[0x0DA8] = W[0x0D0C];
  W[0x0DAC] = W[0x0D10];
  W[0x0DB0] = W[0x0D14];
  W[0x12E8] = 0;
  W[0x12EC] = 0;
  W[0x12F0] = 0;
  W[0x12F4] = 0;
  W[0x12CC] = 0;
  W[0x12D0] = 0;
  W[0x12E0] = 0;
  W[0x12E4] = 0;
  g_terrain_flags = 0;
  g_terrain_mask = 0;
  W[0x12C8] = 0;
  W[0x12D4] = 0;
  return;
}


/* ---- player_model_set_pose ---- */

void player_model_set_pose(void)

{
  int iVar1;
  
  W[0xAB28] = W[0x0D00];
  W[0xAB2C] = W[0x0D04];
  W[0xAB30] = W[0x0D08];
  W[0xAB34] = -(W[0x0D0C] + (W[0x0D58] >> 3));
  W[0xAB38] = (W[0x0D60] >> 3) - W[0x0D10];
  W[0xAB3C] = -(W[0x0D14] + (W[0x0D5C] >> 3));
  W[0xABBC] = W[0x0E08];
  iVar1 = W[0x0D54] >> 3;
  W[0xAC34] = iVar1 + (W[0x0D50] >> 3);
  W[0xACB4] = (W[0x0D50] >> 3) - iVar1;
  W[0xAD34] = W[0xAD4C] - iVar1;
  if (W[0x12BC] == 0) {
    iVar1 = 0;
  }
  else {
    iVar1 = -0x19;
  }
  W[0xAB04] = W[0xAB58] + iVar1;
  scene_node_render(&W[0xAB04]);
  W[0xAB84] = W[0xABD8];
  W[0xAC04] = W[0xAC58];
  W[0xAC84] = W[0xACD8];
  W[0xAD04] = W[0xAD58];
  W[0xAD84] = W[0xADD8];
/* 0x169EC AND 0x169EE ARE TWO INDEPENDENT 16-BIT FIELDS IN ONE 32-BIT SLOT.
 * 0x169EC is 4-aligned (the HIGH half) and 0x169EE is 2-mod-4 (the LOW half).
 * Every ROM access to either is 16-bit:
 *     026B38  move.w #$1, $e169ec.l      \
 *     026F34  tst.w  $e169ec.l            |  the rider/bike chain flag
 *     02818A  tst.w  $e169ec.l            |
 *     02841A  clr.w  $e169ec.l           /
 *     028444  move.w d0,(a4)             \
 *     028486  tst.w  (a4)                 |  the hit-animation counter,
 *     02848E  subq.w #$1,(a4)             |  a4 = 0xE169EE
 *     028490  tst.w  (a4)                /
 * Writing the flag FULL-WIDTH (`W[0x169EC] = 1`) put 1 in the LOW half -- on
 * top of the counter -- and 0 in the flag's own half, so it set the wrong
 * field and pinned the counter at 1 forever. Measured: the counter read 1 on
 * every frame of gameplay while all five terrain contact flags were 0 and
 * camera_mode_select returned 0, i.e. nothing had armed it.
 * That is why the hit flash (camera_dsp_sky_render, record codes 847-851 --
 * "the stars around the rider's head") was on permanently instead of only
 * after a hit. MAME draws none of 847-851 in 720 frames of gameplay capture. */
  W_SET_HI16(0x169EC, 1);
  return;
}


/* ---- player_render ---- */

void player_render(void)

{
  { extern int g_cmd_dump; extern int g_pr_calls;
    g_pr_calls++;
    if (g_cmd_dump) printf("[CMD] player_render() call #%d this frame\n", g_pr_calls); }
  int iVar1;
  int iVar2;
  int iVar3;
  undefined4 uVar4;
  uint8_t *puVar5;
  
  debug_profiler_mark();
  dsp_cmd_set_matrix(0);
  dsp_cmd_object_transform(0,0,0,0,-W[0x0D0C],-W[0x0D10],-W[0x0D14],4);
  debug_profiler_mark();
  if (W[0x0E30] == 0) {
    if (W[0x12BC] == 0) {
      iVar1 = W[0x0D08] - W[0x0CE4];
      iVar2 = W[0x0D04] - W[0x0CE0];
      iVar3 = W[0x0D00] - W[0x0CDC];
      uVar4 = 0x45;
    }
    else {
      iVar1 = W[0x0D08] - W[0x0CE4];
      iVar2 = W[0x0D04] - W[0x0CE0];
      iVar3 = W[0x0D00] - W[0x0CDC];
      uVar4 = 0x2c;
    }
  }
  else {
    iVar1 = W[0x0D08] - W[0x0CE4];
    iVar2 = W[0x0D04] - W[0x0CE0];
    iVar3 = W[0x0D00] - W[0x0CDC];
    uVar4 = W[0x0E34];
  }
  dsp_cmd_set_velocity(uVar4,0,iVar3,iVar2,iVar1);
  debug_profiler_mark();
  dsp_cmd_object_transform(1,0,0x29a,0xfffffd3c,0,0,W[0x0E08],4);
  dsp_cmd_object_param(1,0,1);
  if (W[0x12BC] == 0) {
    uVar4 = 0x46;
  }
  else {
    uVar4 = 0x2d;
  }
  dsp_cmd_set_velocity
            (uVar4,1,W[0x0D00] - W[0x0CDC],W[0x0D04] - W[0x0CE0],
             W[0x0D08] - W[0x0CE4]);
  debug_profiler_mark();
  iVar1 = W[0x0D50] >> 3;
  iVar2 = W[0x0D54] >> 3;
  iVar3 = -W[0x0D54];
  debug_profiler_mark();
  /* param_7 is an ANGLE, and 0x11C6 is an IMMEDIATE, not a ROM address --
   * `pea $11c6.w` at 0x00B9BA. It was `&R[0x011C6]`, a host pointer fed
   * straight into the trig index, so this part's Z rotation was garbage
   * every frame. Same class as the sky's Z coordinate and the rig's
   * 0x238E. The surrounding pushes confirm the whole argument list:
   *   pea $1 / $11c6 / $71c / (-8,A6) / $fe20 / $29e / $1f / $1 */
  dsp_cmd_object_transform(1,0x1f,0x29e,0xfffffe20,iVar2 + iVar1,0x71c,0x11C6,1);
  dsp_cmd_object_param(1,0,1);
  if (W[0x12BC] == 0) {
    uVar4 = 0x47;
  }
  else {
    uVar4 = 0x2e;
  }
  dsp_cmd_set_velocity
            (uVar4,1,W[0x0D00] - W[0x0CDC],W[0x0D04] - W[0x0CE0],
             W[0x0D08] - W[0x0CE4]);
  debug_profiler_mark();
  dsp_cmd_object_transform
            (1,0xffffffe1,0x29e,0xfffffe20,(iVar3 >> 3) + iVar1,0xfffff8e4,0xffffee3a,1);
  debug_profiler_mark();
  dsp_cmd_object_param(1,0,1);
  debug_profiler_mark();
  if (W[0x12BC] == 0) {
    debug_profiler_mark();
    dsp_cmd_set_velocity
              (0x48,1,W[0x0D00] - W[0x0CDC],W[0x0D04] - W[0x0CE0],
               W[0x0D08] - W[0x0CE4]);
    puVar5 = &R[0x3A1F9];
  }
  else {
    debug_profiler_mark();
    dsp_cmd_set_velocity
              (0x2f,1,W[0x0D00] - W[0x0CDC],W[0x0D04] - W[0x0CE0],
               W[0x0D08] - W[0x0CE4]);
    puVar5 = &R[0x3A1FF];
  }
  debug_profiler_mark();
  debug_profiler_mark();
  iVar1 = -W[0x0D54];
  dsp_cmd_object_transform(1,0x605,0xec,0xbd,(W[0x0D54] >> 2) + 0x1e,0x64c,0xfffffaac,1);
  dsp_cmd_object_param(1,0,1);
  if (W[0x12BC] == 0) {
    uVar4 = 0x49;
  }
  else {
    uVar4 = 0x30;
  }
  dsp_cmd_set_velocity
            (uVar4,1,W[0x0D00] - W[0x0CDC],W[0x0D04] - W[0x0CE0],
             W[0x0D08] - W[0x0CE4]);
  dsp_cmd_object_transform(1,0xfffff9fb,0xec,0xbd,(iVar1 >> 2) + -0x1e,0xfffff9b4,0x554,1);
  dsp_cmd_object_param(1,0,1);
  if (W[0x12BC] == 0) {
    uVar4 = 0x4a;
  }
  else {
    uVar4 = 0x31;
  }
  dsp_cmd_set_velocity
            (uVar4,1,W[0x0D00] - W[0x0CDC],W[0x0D04] - W[0x0CE0],
             W[0x0D08] - W[0x0CE4]);
  debug_profiler_mark();
  { extern int g_cmd_dump; intptr_t _b = W[0x0CA4];
    camera_update_main();
    if (g_cmd_dump) printf("[CMD]   camera_update_main (from player_render) wrote %ld words\n",
                           (long)((W[0x0CA4] - _b) / 4)); }
  debug_profiler_mark();
  return;
}


/* ---- player_clamp_analog ---- */

void player_clamp_analog(void)

{
  int iVar1;
  int iVar2;
  
  iVar1 = W[0x0E00];
  W[0x0E04] = W[0x0E00];
  iVar2 = (W[0x2C04] * 0x4a + 0x4a) * 0x10 - W[0x0E00];
  if (0x100 < iVar2) {
    iVar2 = 0x100;
  }
  W[0x0E00] = W[0x0E00] + iVar2;
  if (W[0x0E00] < 0) {
    W[0x0E00] = 0;
  }
  if (0x6f00 < W[0x0E00]) {
    W[0x0E00] = 0x6f00;
  }
  if (W[0x0E00] < W[0x0D48] / 3) {
    W[0x0E00] = W[0x0D48] / 3;
  }
  if (W[0x0E00] < iVar1) {
    W[0x0E00] = iVar1 + -0x1e;
  }
  W[0x0E08] = W[0x0E08] - W[0x0E00];
  W[0x0D94] = (W[0x2C04] * 0x4a + 0x4a >> 4) + W[0x0D94];
  if (W[0x0D94] < 0) {
    W[0x0D94] = W[0x0D94] + 0x280;
  }
  if (0x27f < W[0x0D94]) {
    W[0x0D94] = W[0x0D94] + -0x280;
  }
  W[0x0D98] = W[0x0D94] / 10;
  return;
}

/* ---- player_clamp_position ---- */

int player_clamp_position(void)

{
  if (W[0x0E38] == 0) {
    if (W[0x0E0C] == 3) {
      /* ROM 0x00B152 `cmpi.l #$3f7a0 ; blt ; move.l #$3f7a0,$4(a0)`: the final
       * stage's CEILING. The decompiler lost the value, so every frame the bike
       * reached it (it starts there) it dropped to y = 0 and the floor clamp
       * below put it at 10000, far under SOLITAR's islands (register row 185). */
      if (0x3f79f < (int)W[0x0D04]) {
        W[0x0D04] = 0x3f7a0;
      }
    }
    else if ((W[0x0E0C] == 0) && (W16(0xE10) == 0)) {
      if (0x1a99f < (int)W[0x0D04]) {
        W[0x0D04] = 0x1a9a0;
      }
    }
    else if (0x2269f < (int)W[0x0D04]) {
      W[0x0D04] = 0x226a0;
    }
  }
  if (W[0x0E0C] + -3 == 0) {
    if ((int)W[0x0D00] < 20000) {
      W[0x0D00] = 0x4e20;
    }
    if (0xbb1e0 < (int)W[0x0D00]) {
      W[0x0D00] = 0xBB1E0;
    }
    if ((int)W[0x0D08] < 0xbb1e0) {
      W[0x0D08] = 0xBB1E0;
    }
    if (0x17b1e0 < (int)W[0x0D08]) {
      W[0x0D08] = 0x17B1E0;
    }
    if ((int)W[0x0D04] < 10000) {
      W[0x0D04] = 0x2710;
    }
  }
  return W[0x0E0C] + -3;
}

/* ---- player_model_set_wheel_angle ---- */

void player_model_set_wheel_angle(int param_1)

{
  W[0xAE34] = param_1;
  W[0xAF34] = 0x238E - param_1;   /* immediate, not &R[...] -- see the note above */
  W[0xAEB4] = 0x238E - param_1;
  return;
}

/* ---- player_model_smoothed_inputs ---- */

void player_model_smoothed_inputs(void)

{
  uint32_t uVar1;
  int iVar2;
  
  W[0x3ED0] = W[0x0D50];
  W[0x169F8] = W[0x169F8] + ((W[0x0D50] >> 4) - W[0x169F8] >> 2);
  W[0x3ED4] = W[0x169F8];
  W[0x3ED8] = W[0x0D54];
  W[0x169FC] = W[0x169FC] + ((W[0x0D54] >> 5) - W[0x169FC] >> 1);
  W[0x3EDC] = W[0x169FC];
  W[0x3EE0] = W[0x2C04];
  W[0x16A00] = W[0x16A00] + (W[0x2C04] * 0x100 - W[0x16A00] >> 3);
  W[0x3EE4] = W[0x16A00];
  W[0xAFAC] = W[0xAFC4] + (W[0x16A00] >> 10);
  W[0x3EE8] = W[0xB04C];
  W[0xB034] = (W[0x169F8] >> 1) + W[0x169F8] + (W[0xB04C] - W[0x16A00]);
  W[0x3EEC] = W[0xB034];
  iVar2 = W[0x2C04];
  if (4 < W[0x2C04]) {
    iVar2 = 4;
  }
  W[0xAFA8] = (W[0x169FC] >> 4) +
                 W[0xAFC0] +
                 ((int)(vrd16s(0x20B004 + (W[0xAE34] & 0xfffc))) >>
                 (0xeU - iVar2 & 0x3f));
  W[0xAFB0] = W[0xAFC8] + (W[0x169F8] >> 5);
  W[0xB03C] = W[0x169FC] + W[0xB054];
  /* PELVIS Z -- left as Ghidra had it, DELIBERATELY. Flipping the crank
   * term's sign (W[0xAFD4] + W[0x169FC]) puts the pelvis/spine/head at 2-3
   * deg from the recording instead of 6, but bicycle_ik_solve rotates the
   * foot targets through this same pelvis matrix, and the hips/shins then
   * slip from 1 deg to 4-7. The two must change together; open item. */
  W[0xAFBC] = W[0xAFD4] - W[0x169FC];
  if (W[0x0D48] >> 0xb < 4) {
    uVar1 = 3 - (W[0x0D48] >> 0xb);
  }
  else {
    uVar1 = 0;
  }
  if (W[0x12BC] == 0) {
    iVar2 = 0x6e;
  }
  else {
    iVar2 = 0x77;
  }
  W[0xB104] = (W[0x0C8C] >> (uVar1 & 0x3f) & 7U) + iVar2;
  if (W[0x1824] == 0) {
    W[0xB084] = 0x5c;
  }
  else {
    W[0xB084] = 0x5d;
  }
  return;
}

/* ---- player_vehicle_render_if_active @ 0x0155EC ---- */

/* THE GULLS (register row 160). Animated-object records whose word +0x44 is
 * 0..2 are drawn by 0x015350 below: a body (model 0x3632C[type] = 900, record
 * code 969) plus two flapping wings, models 0x383 and 0x382 (codes 968/967) --
 * the three codes MAME's captures carry together on 230 attract frames.
 * `nb` is the record's _W[] byte offset; `cur` the display-list cursor. The
 * Ghidra form returned `undefined4`, truncating the cursor on the way out. */
static int32_t *gull_dsp_render(uint32_t nb, int32_t *c);

intptr_t player_vehicle_render_if_active(uint32_t nb, intptr_t cur)
{
  int16_t t = W_HI16(nb + 0x44);                 /* tst.w $44 ; bmi / cmpi.w #2 ; bgt */
  if (t >= 0 && t <= 2) return (intptr_t)gull_dsp_render(nb, (int32_t *)cur);
  return cur;
}

/* ---- player_model_set_direct_inputs ---- */

void player_model_set_direct_inputs(int param_1,int param_2,int param_3,int param_4)

{
  uint32_t uVar1;
  int iVar2;
  
  W[0x169FC] = param_2;
  W[0x169F8] = param_1;
  W[0x16A00] = (W[0x2C04] * 0x100 - W[0x16A00] >> 3) + W[0x16A00];
  W[0xAFAC] = param_3 + W[0xAFC4] + (W[0x16A00] >> 10);
  W[0xB034] = (param_1 >> 1) + param_1 + (W[0xB04C] - W[0x16A00]);
  iVar2 = W[0x2C04];
  if (4 < W[0x2C04]) {
    iVar2 = 4;
  }
  W[0xAFA8] = (param_2 >> 4) +
                 W[0xAFC0] +
                 ((int)(vrd16s(0x20B004 + (W[0xAE34] & 0xfffc))) >>
                 (0xeU - iVar2 & 0x3f));
  W[0xAFB0] = param_4 + W[0xAFC8] + (param_1 >> 5);
  W[0xB03C] = param_2 + W[0xB054];
  W[0xAFBC] = W[0xAFD4] - param_2;
  if (W[0x0D48] >> 0xb < 4) {
    uVar1 = 3 - (W[0x0D48] >> 0xb);
  }
  else {
    uVar1 = 0;
  }
  if (W[0x12BC] == 0) {
    iVar2 = 0x6e;
  }
  else {
    iVar2 = 0x77;
  }
  W[0xB104] = (W[0x0C8C] >> (uVar1 & 0x3f) & 7U) + iVar2;
  if (W[0x1824] == 0) {
    W[0xB084] = 0x5c;
  }
  else {
    W[0xB084] = 0x5d;
  }
  return;
}

/* ---- player_vehicle_dsp_render @ 0x015350 ---- */

/* Rewritten from the disassembly. The Ghidra form dereferenced the _W[] byte
 * offset as a host pointer (the SIGSEGV that kept the animated-object system
 * off), read the model table host-native, and emitted nothing for the wings.
 * Every trig pair is `lea -$2(a0,d0.l*4) ; move.l (a3)+ ; move.l $2(a3)` off
 * 0x20B004 -- the LOW halves, i.e. (sin, cos) -- register row 87. */
#define GULL_PAIR(ang) do { uint32_t _a = (uint32_t)(ang) & 0x3fff; \
    *c++ = vrd16s(0x20B004 + _a * 4); *c++ = vrd16s(0x20B006 + _a * 4); } while (0)
static int32_t *gull_dsp_render(uint32_t nb, int32_t *c)
{
  const uint8_t *lo = g_sys.dspram, *hi = g_sys.dspram + DSPRAM_SIZE;
  int32_t d5 = 0, d6 = 0, d7 = 0;
  int16_t type = W_HI16(nb + 0x44);
  uint32_t cnt = (uint32_t)(0x166A4 + (int32_t)type * 4);
  int i;

  if ((const uint8_t *)c < lo || (const uint8_t *)(c + 0x60) > hi) return c;   /* 86 words max */

  if (W[0x16672] != 0) {                         /* 0x015362: the yaw record, once a frame */
    *c++ = 0x8008; *c++ = 2; *c++ = 1; *c++ = 0; *c++ = 0x7fff;
    *c++ = (int32_t)W[0x16668]; *c++ = (int32_t)W[0x1666C];
    *c++ = 0; *c++ = 0x7fff; *c++ = 4; *c++ = -1;
    W[0x16672] = 0;
  }
  *c++ = 0x8008; *c++ = 3; *c++ = 1;              /* 0x0153A2: the body's own rotation */
  GULL_PAIR(W_HI16(nb + 0x0c)); GULL_PAIR(W_LO16(nb + 0x0c)); GULL_PAIR(W_HI16(nb + 0x10));
  *c++ = 4; *c++ = 0;
  *c++ = (int32_t)W[nb]; *c++ = (int32_t)W[nb + 4]; *c++ = (int32_t)W[nb + 8];
  *c++ = -1;
  *c++ = 0x8009; *c++ = 3; *c++ = 2; *c++ = 3;
  *c++ = 0x800a; *c++ = vrd16s(0x3632C + (int32_t)type * 2); *c++ = 3;   /* body, model 900 */
  W[0xEB08] = (int32_t)((uint32_t)W[0x0D00] - (uint32_t)W[0x0CDC]); *c++ = (int32_t)W[0xEB08];
  W[0xEB0C] = (int32_t)((uint32_t)W[0x0D04] - (uint32_t)W[0x0CE0]); *c++ = (int32_t)W[0xEB0C];
  W[0xEB10] = (int32_t)((uint32_t)W[0x0D08] - (uint32_t)W[0x0CE4]); *c++ = (int32_t)W[0xEB10];

  /* 0x01546E: the wing flap. 0xE166A4 + type*4 holds a PHASE word and a COUNT
   * word (script opcode 0x82F4 sets the count); while the count is positive
   * the phase advances 0x400 a frame and each wrap past 0x4000 spends one. */
  if (cnt < WORK_RAM_SIZE - 4 && W_LO16(cnt) > 0) {
    uint16_t ph = (uint16_t)W_HI16(cnt) + 0x400;
    int32_t s, v;
    W_SET_HI16(cnt, ph);
    if (ph >= 0x4000) { W_SET_LO16(cnt, W_LO16(cnt) - 1); W_SET_HI16(cnt, 0); }
    ph = (uint16_t)W_HI16(cnt);
    s  = vrd16s(0x20B004 + (uint32_t)(ph & 0x3fff) * 4);
    d7 = (int32_t)((uint32_t)s << 11) >> 15;
    d6 = (s * 0x8e3) >> 15;                                     /* muls.w #$8e3 */
    v  = 0x8000 - (int32_t)vrd16s(0x20B006 + (((uint32_t)ph * 2) & 0x3fff) * 4);
    d5 = (int32_t)((uint32_t)v * 0x471u) >> 15;                 /* muls.l #$471 */
  }
  for (i = 0; i < 2; i++) {                      /* 0x0154EE / 0x015570: the two wings */
    *c++ = 0x8008; *c++ = 4; *c++ = 1;
    GULL_PAIR(d6);
    GULL_PAIR(i == 0 ? -d5 : d5);
    GULL_PAIR(i == 0 ? d7 : -d7);
    *c++ = 4; *c++ = 0; *c++ = 0; *c++ = 0; *c++ = 0; *c++ = -1;
    *c++ = 0x8009; *c++ = 4; *c++ = 3; *c++ = 4;
    *c++ = 0x800a; *c++ = (i == 0) ? 0x383 : 0x382; *c++ = 4;
    *c++ = (int32_t)W[0xEB08]; *c++ = (int32_t)W[0xEB0C]; *c++ = (int32_t)W[0xEB10];
  }
  return c;
}
#undef GULL_PAIR

/* ========== STATE HANDLERS ========== */
/* 15 game state handlers extracted from game_all.c */

/* ---- player_flight_advance ---- */

void player_flight_advance(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  int local_10;
  int local_c;
  int local_8;
  
  iVar3 = 0x3b0 - W[0x2BCC];
  if (iVar3 < 0) {
    iVar3 = 0;
  }
  W[0x0C44] = iVar3 + W[0x0C44];
  spherical_to_cartesian(W[0x0D0C],W[0x0D10],W[0x0C44],&local_10,&local_c,&local_8);
  W[0x0D00] = local_10 + W[0x0D00];
  W[0x0D04] = local_c + W[0x0D04];
  W[0x0D08] = local_8 + W[0x0D08];
  W[0x0C38] = (W[0x0D50] >> 8) + W[0x0C38];
  W[0x0C3C] = W[0x0C3C] - ((W[0x0D54] >> 9) + (W[0x0D54] >> 8));
  rotate_vector_euler_xyz(0,0,0x10000,W[0x0C38],0,0,&local_10,&local_c,&local_8);
  rotate_euler_yxz_optimized
            (local_10,local_c,local_8,W[0x0D0C],W[0x0C3C],W[0x0D14],&local_10,&local_c,
             &local_8);
  iVar3 = math_atan2(local_10,local_8);
  rotate_vector_euler_xyz(local_10,local_c,local_8,0,-iVar3,0,&local_10,&local_c,&local_8);
  iVar1 = math_atan2(local_8,local_c);
  rotate_vector_euler_xyz(0x10000,0,0,0,0,W[0x0C40],&local_10,&local_c,&local_8);
  rotate_euler_yxz_optimized
            (local_10,local_c,local_8,W[0x0D0C],W[0x0C3C],W[0x0D14],&local_10,&local_c,
             &local_8);
  rotate_euler_yxz_optimized
            (local_10,local_c,local_8,-(iVar1 + 0x4000),-iVar3,0,&local_10,&local_c,&local_8);
  iVar2 = math_atan2(local_10,local_c);
  W[0x0D0C] = iVar1 + 0x4000;
  W[0x0D10] = iVar3 + W[0x0D10];
  W[0x0D14] = -0x4000 - iVar2;
  return;
}

/* ---- player_model_render_only ---- */

void player_model_render_only(void)

{
  /* ROM 0x02689A: `movea.w $e169f0.l` (a sign-extended WORD) to the
   * viewport, then scene_node_render on every 0x80-byte node from 0xE0AB04
   * while `tst.l (a2) ; bpl`, then `clr.l` at the list cursor. The walk was
   * `int *piVar1 = &W[0xAB04]; piVar1 += 0x20` -- 0x80 HOST bytes, i.e. 16
   * _W[] slots, where the next node is 0x80 slots on (row 137's class).
   * The walk has no count in the ROM: the ending portrait (phase 8,
   * ending_sequence_stage9) chains FOUR casts here, 17+19+18+17 = 71 nodes,
   * and a 64-node cap dropped the last seven of the fourth figure (measured
   * against MAME's CPU list at counter 5400: nodes 14..20, models 1028..1039
   * missing). The bound is now the end of work RAM. */
  intptr_t nb;
  dsp_cmd_set_matrix_and_render((int)(int16_t)W[0x169F0]);
  for (nb = 0xAB04; nb + 0x80 <= WORK_RAM_SIZE && (int32_t)W[nb] >= 0; nb += 0x80) {
    scene_node_render((int *)&W[nb]);
  }
  *(int32_t*)W[0x0CA4] = 0;
  return;
}

/* ---- player_model_update ---- */

void player_model_update(void)

{
  /* ROM 0x026830. The frame goes to the keyframe player as a WORD
   * (`move.w (a3),-(a7)`), the animation length is the LOW word of the root
   * node's +0x04 long (`move.w $6(a2),d1`), and the loader gets the FULL
   * 32-bit list pointer back out of 0xE169F4 (`move.l $e169f4.l,-(a7)`) --
   * the transpile passed no frame, compared the whole +0x04 long, and cut
   * the list pointer to a short. */
  extern void anim_update_and_render(intptr_t nb, int t);
  extern intptr_t anim_load_list(intptr_t a1, uint32_t d1);
  W[0xEB04] = W[0xEB04] + 1;
  dsp_cmd_set_matrix_and_render((int)(int16_t)W[0x169F0]);
  if (W[0xEB06] == 0) {
    anim_update_and_render(0xAB04, (int16_t)W[0xEB04]);
    W[0xEB06] = (uint16_t)(W_LO16(0xAB08) <= (int16_t)W[0xEB04]);
    *(int32_t*)W[0x0CA4] = 0;
  }
  else {
    anim_load_list(0xAB04, (uint32_t)W[0x169F4]);
    player_model_render_only();
  }
  return;
}

/* ---- player_obstacle_scan ---- */

void player_obstacle_scan(void)

{
  int bVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  int iVar5;
  int local_24;
  int local_20;
  int local_1c;
  int local_18;
  int local_14;
  int local_10;
  undefined4 local_c;
  undefined4 local_8;
  
  if (W[0x16E4] != 0) {
    bVar1 = false;
    local_c = 0;
    local_8 = 0;
    iVar5 = 0;
    do {
      if (bVar1) {
        local_10 = -iVar5;
      }
      else {
        iVar5 = iVar5 + 0x400;
        local_10 = iVar5;
      }
      rotate_vector_euler_xyz(0,0,0x10000,local_10,0,0,&local_24,&local_20,&local_1c);
      rotate_euler_yxz_optimized
                (local_24,local_20,local_1c,W[0x0D0C],local_c,W[0x0D14],&local_24,&local_20,
                 &local_1c);
      iVar3 = math_atan2(local_24,local_1c);
      rotate_vector_euler_xyz(local_24,local_20,local_1c,0,-iVar3,0,&local_24,&local_20,&local_1c);
      local_18 = math_atan2(local_1c,local_20);
      local_18 = local_18 + 0x4000;
      rotate_vector_euler_xyz(0x10000,0,0,0,0,local_8,&local_24,&local_20,&local_1c);
      rotate_euler_yxz_optimized
                (local_24,local_20,local_1c,W[0x0D0C],local_c,W[0x0D14],&local_24,&local_20,
                 &local_1c);
      rotate_euler_yxz_optimized
                (local_24,local_20,local_1c,-local_18,-iVar3,0,&local_24,&local_20,&local_1c);
      iVar4 = math_atan2(local_24,local_20);
      iVar2 = local_18;
      iVar4 = -0x4000 - iVar4;
      iVar3 = W[0x0D10] + iVar3;
      local_14 = iVar4;
      rotate_euler_zxy_optimized(0,0,0x2b0,local_18,iVar3,iVar4,&local_24,&local_20,&local_1c);
      W[0x1308] = 1;
      W[0x1318] = local_24 + W[0x0D00];
      W[0x131C] = local_20 + W[0x0D04];
      W[0x1320] = local_1c + W[0x0D08];
      world_objects_tick_all();
      world_collision_zones_process();
      terrain_height_resolve();
      bVar1 = (bool)(bVar1 ^ 1);
    } while (W[0x1324] != 0);
    W[0x0D0C] = iVar2;
    W[0x0D10] = iVar3;
    W[0x0D14] = iVar4;
    world_render_objects();
  }
  return;
}

/* ---- player_render_stub ---- */

void player_render_stub(void)

{
  /* EMPTY IN THE ROM -- NOT a gap, do not "implement" this.
   * 0x00B806 is a bare RTS (0x4E75) in pr2ver-a.*, verified by reading
   * the ROM directly. Intentional. The real bicycle rendering is player_render(), which IS
   * called; this entry point is an RTS in the ROM.
   */
  return;
}

/* ---- bicycle_front_rear_ik @ 0x027BEE ---- */

void bicycle_front_rear_ik(void)

{
  uint32_t uVar1;
  int iVar3;
  int iVar4;
  
  /* Trig-table base: 0x20B002 in ROM. Ghidra emitted `((uint8_t *)&g_sys.rom[0x20B002])`
   * (a 1-byte read used as a pointer base) instead of `&g_sys.rom[0x20B002]`.
   * The companion lines below at 0x20B006 use the correct convention. */
  uVar1 = W[0xAE34] & 0xfffc;
  transform_point_pair_by_node(&W[0xB1C0],&W[0xAFA8],&W[0xB1A8],&W[0xB2A8]);
  W[0x16A1C] = W[0xB1A8] + -0x98;
  W[0x16A10] = W[0xB2A8] + 0x98;
  iVar3 = ((int)vrd16s(0x20B004 + uVar1) >> 8) - ((int)vrd16s(0x20B004 + uVar1) >> 0xc);
  /* VALUE arithmetic, not pointer arithmetic. `*(int *)(&W[0xB1AC] + iVar3)`
   * read the slot iVar3 (an ANGLE-derived 0..127) past the node -- a random
   * field of node 13/14 -- so this atan2 input jumped between frames and
   * the shoulder flipped ~165 degrees. The mirror line two below does
   * `W[0xB2AC] - iVar3`; this is its twin. */
  W[0x16A20] = W[0xB1AC] + iVar3 + -0x196;
  W[0x16A14] = (W[0xB2AC] - iVar3) + -0x195;
  iVar3 = ((int)vrd16s(0x20B006 + uVar1) >> 8) - ((int)vrd16s(0x20B006 + uVar1) >> 0xc);
  W[0x16A24] = (W[0xB1B0] - iVar3) + 0x9a;
  W[0x16A18] = W[0xB2B0] + iVar3 + 0x9b;   /* twin of `W[0xB1B0] - iVar3` above */
  /* SHOULDERS: X and Z come out with the opposite sign from the recording
   * (ours 180-theta where MAME has 180+theta). Measured: A 29 -> 5.5 deg,
   * B 65 -> 11.6 with both negated. The Z is used below through the
   * cos() lookup, which is even, so only the stored value changes. */
  W[0xB1BC] = -math_atan2(W[0x16A1C],W[0x16A20]);
  {
    int _div = (int)(int16_t)vrd16s(0x20B006 + (W[0xB1BC] & 0xfffc));
    iVar4 = _div ? (W[0x16A20] << 0xf) / _div : 0;
  }
  iVar3 = math_atan2(-W[0x16A24],iVar4);
  {
    int _div = (int)(int16_t)vrd16s(0x20B006 + ((uint32_t)((uint16_t)iVar3 & 0xfffc)));
    iVar4 = _div ? (iVar4 << 0xf) / _div : 0;
  }
  iVar4 = math_slope_angle(iVar4 * iVar4 + -0x3a28f,0x33EA5);
  W[0xB234] = -iVar4;
  iVar4 = math_atan2((int)vrd16s(0x20B004 + (W[0xB234] & 0xfffc)) >> 7,
                     ((int)vrd16s(0x20B006 + (W[0xB234] & 0xfffc)) >> 7) + 0x1a0)
  ;
  W[0xB1B4] = iVar4 - iVar3;
  W[0xB1B8] = 0x8000;
  /* PARENT LINK + FLAG STORES ARE HALF-WORDS.
   *
   * The M68K writes the node's parent link as a 16-bit store into the
   * HIGH half of the word at node+4 (big-endian: the first two bytes), and
   * scene_node_render reads it back with W_HI16(nb + 4). The literal
   * 0xfff3 is -13 = "own index back" = node 0, the body. Stored as a
   * full-slot 32-bit value it landed in the LOW half, W_HI16 read 0, and
   * scene_node_render took the ROOT branch: the four animated joints
   * (both shoulders, both hips) were drawn with their own rotation alone,
   * never composed onto the body -- so they held still while the body
   * rolled under them, sweeping +-180 degrees relative to it (MAME's are
   * constant at (18,173,-22)/(20,-175,0)). The +0xA stores are the LOW
   * half of the word at node+8 (a 2-mod-4 slot, which the _W sync rebuilds
   * every frame); write them into the word they belong to. */
  W_SET_HI16(0xB188, (int16_t)0xfff3);
  W_SET_LO16(0xB18C, 2);
  W[0xB238] = 0;
  W[0xB23C] = 0;
  W[0xB2BC] = -math_atan2(W[0x16A10],W[0x16A14]);
  {
    int _div = (int)(int16_t)vrd16s(0x20B006 + (W[0xB2BC] & 0xfffc));
    iVar4 = _div ? (W[0x16A14] << 0xf) / _div : 0;
  }
  iVar3 = math_atan2(-W[0x16A18],iVar4);
  {
    int _div = (int)(int16_t)vrd16s(0x20B006 + ((uint32_t)((uint16_t)iVar3 & 0xfffc)));
    iVar4 = _div ? (iVar4 << 0xf) / _div : 0;
  }
  iVar4 = math_slope_angle(iVar4 * iVar4 + -0x3a28f,0x33EA5);
  W[0xB334] = -iVar4;
  iVar4 = math_atan2((int)vrd16s(0x20B004 + (W[0xB334] & 0xfffc)) >> 7,
                     ((int)vrd16s(0x20B006 + (W[0xB334] & 0xfffc)) >> 7) + 0x1a0)
  ;
  W[0xB2B4] = iVar4 - iVar3;
  W[0xB2B8] = 0x8000;
  W_SET_HI16(0xB288, (int16_t)0xfff1);
  { extern int g_ik_dump; if (g_ik_dump)
      printf("[IKA] f=%d in1=%ld,%ld,%ld in2=%ld,%ld,%ld shA=%ld,%ld,%ld shB=%ld,%ld,%ld\n",
             (int)g_sys.frame_count,
             (long)W[0x16A1C],(long)W[0x16A20],(long)W[0x16A24], (long)W[0x16A10],(long)W[0x16A14],(long)W[0x16A18],
             (long)(W[0xB1B4]&0xffff),(long)(W[0xB1B8]&0xffff),(long)(W[0xB1BC]&0xffff),
             (long)(W[0xB2B4]&0xffff),(long)(W[0xB2B8]&0xffff),(long)(W[0xB2BC]&0xffff)); }
  W[0xB28E] = 2;
  W[0xB338] = 0;
  W[0xB33C] = 0;
  return;
}





/* ---- bicycle_ik_solve @ 0x0274B6 ---- */

void bicycle_ik_solve(void)

{
  int iVar1;
  int iVar2;
  short sVar3;
  uint32_t puVar7;   /* ROM byte offset into the trig table, not a host pointer */
  int32_t node_buf[3];
  int32_t local_1c;
  int32_t local_18;
  int32_t local_14;
  int32_t uStack_10;
  int32_t iStack_c;
  int32_t iStack_8;

  /* Original M68K: two block copies of big-endian longs from ROM into
   * stack locals (3 longs @0x3AC8C, 6 longs @0x3AC98). The transpiled
   * byte-copy version (a) wrote 24 bytes from &local_1c assuming M68K
   * stack layout — stack smash on x86-64 (caught by the L3 coverage
   * build, attract demo fc~301 chain) — and (b) deposited BE bytes that
   * were read back as host-LE ints: byte-swapped IK constants. */
  node_buf[0] = (int32_t)vrd32(0x3AC8C);
  node_buf[1] = (int32_t)vrd32(0x3AC90);
  node_buf[2] = (int32_t)vrd32(0x3AC94);
  local_1c  = (int32_t)vrd32(0x3AC98);
  local_18  = (int32_t)vrd32(0x3AC9C);
  local_14  = (int32_t)vrd32(0x3ACA0);
  uStack_10 = (int32_t)vrd32(0x3ACA4);
  iStack_c  = (int32_t)vrd32(0x3ACA8);
  iStack_8  = (int32_t)vrd32(0x3ACAC);
  transform_point_pair_by_node((undefined4 *)node_buf,&W[0xB028],&W[0x16A10],&W[0x16A1C]);
  transform_point_by_node(&W[0x16A10],&W[0xAFA8],&W[0xB3A8]);
  transform_point_by_node(&W[0x16A1C],&W[0xAFA8],&W[0xB528]);
  W[0xB4A8] = local_1c;
  W[0x16A1C] = W[0xB3A8] - local_1c;
  W[0xB628] = -local_1c;
  W[0x16A10] = W[0xB528] + local_1c;
  iVar1 = W[0x169FC] * 2 >> 5;
  W[0xB4AC] = local_18 - iVar1;
  W[0x16A20] = W[0xB3AC] - W[0xB4AC];
  W[0xB62C] = local_18 + iVar1;
  W[0x16A14] = W[0xB52C] - W[0xB62C];
  W[0xB4B0] = local_14;
  W[0x16A24] = W[0xB3B0] - local_14;
  W[0xB630] = local_14;
  W[0x16A18] = W[0xB530] - local_14;
  W[0xB4B4] = uStack_10;
  W[0xB634] = uStack_10;
  /* FEET: the Y constant's sign is the mirror of what Ghidra emitted.
   * Measured against the recording (XZY, composed on the body): as
   * written 51 deg off on both feet; with Y negated 2.2 / 1.6 deg. */
  W[0xB4B8] = -iStack_c;
  W[0xB638] = iStack_c;
  W[0xB4BC] = iStack_8;
  W[0xB63C] = -iStack_8;
  iVar1 = math_atan2(W[0x16A1C],W[0x16A20]);
  iVar2 = math_slope_angle((int16_t)vrd16s(0x20B006 + ((uint32_t)((uint16_t)iVar1 & 0xfffc))) * 0x54 >>
                           0xf,W[0x16A20]);
  W[0xB3BC] = iVar1 + iVar2 + 0x4000;
  iVar1 = SAFE_DIV(
            W[0x16A20] * 0x8000 +
              (int16_t)vrd16s(0x20B002 + ((W[0xB3BC] & 0xfffc) + 2)) * -0x54,
            (int)(int16_t)vrd16s(0x20B002 + ((W[0xB3BC] & 0xfffc) + 4)));
  iVar2 = math_atan2(-W[0x16A24],iVar1);
  iVar1 = SAFE_DIV(iVar1 << 0xf,
            (int)(int16_t)vrd16s(0x20B006 + ((uint32_t)((uint16_t)iVar2 & 0xfffc))));
  iVar1 = iVar1 * iVar1 + -0x207d5;
  if (iVar1 < 0x1f4eb) {
    W[0xB434] = math_slope_angle(iVar1,0x1F4EB);
    puVar7 = 0x20B002 + (W[0xB434] & 0xfffc);
  }
  else {
    W[0xB434] = 0;
    puVar7 = 0x20B002;
  }
  iVar1 = math_atan2(vrd16s(puVar7 + 2) * 0x122 >> 0xf,
                     (vrd16s(puVar7 + 4) * 0x122 >> 0xf) + 0xdd);
  W[0xB3B4] = iVar2 + iVar1;
  W[0xB3B8] = 0;
  W_SET_LO16(0xB38C, 0);
  W_SET_HI16(0xB388, (int16_t)0xffef);
  if (W[0x16A10] < 0x54) {
    iVar1 = math_atan2(W[0x16A10],W[0x16A18]);
    iVar2 = math_slope_angle((int16_t)vrd16s(0x20B006 + ((uint32_t)((uint16_t)-iVar1 & 0xfffc))) * 0x54
                             >> 0xf,W[0x16A18]);
    W[0xB538] = (-iVar1 - iVar2) + 0x4000;
    iVar1 = SAFE_DIV(
              W[0x16A18] * 0x8000 +
                (int16_t)vrd16s(0x20B002 + ((W[0xB538] & 0xfffc) + 2)) * -0x54,
              (int)(int16_t)vrd16s(0x20B002 + ((W[0xB538] & 0xfffc) + 4)));
    iVar2 = math_atan2(iVar1,-W[0x16A14]);
    iVar1 = SAFE_DIV(iVar1 << 0xf,
              (int)(int16_t)vrd16s(0x20B004 + ((uint32_t)((uint16_t)(iVar2 + 0x8000) & 0xfffc))));
    iVar1 = iVar1 * iVar1 + -0x207d5;
    if (iVar1 < 0x1f4eb) {
      W[0xB5B4] = math_slope_angle(iVar1,0x1F4EB);
      puVar7 = 0x20B002 + (W[0xB5B4] & 0xfffc);
    }
    else {
      W[0xB5B4] = 0;
      puVar7 = 0x20B002;
    }
    iVar1 = math_atan2(vrd16s(puVar7 + 2) * 0x122 >> 0xf,
                       (vrd16s(puVar7 + 4) * 0x122 >> 0xf) + 0xdd);
    W[0xB534] = iVar2 + 0x8000 + iVar1;
    W[0xB53C] = 0;
    W_SET_LO16(0xB50C, 0);
  }
  else {
    iVar1 = math_atan2(W[0x16A10],W[0x16A14]);
    iVar2 = math_slope_angle((int16_t)vrd16s(0x20B006 + ((uint32_t)((uint16_t)iVar1 & 0xfffc))) * 0x54
                             >> 0xf,W[0x16A14]);
    W[0xB53C] = (iVar1 - iVar2) - 0x4000;
    iVar1 = SAFE_DIV(
              (int16_t)vrd16s(0x20B002 + ((W[0xB53C] & 0xfffc) + 2)) * 0x54 +
                W[0x16A14] * 0x8000,
              (int)(int16_t)vrd16s(0x20B002 + ((W[0xB53C] & 0xfffc) + 4)));
    iVar2 = math_atan2(-W[0x16A18],iVar1);
    iVar1 = SAFE_DIV(iVar1 << 0xf,
              (int)(int16_t)vrd16s(0x20B006 + ((uint32_t)((uint16_t)iVar2 & 0xfffc))));
    iVar1 = iVar1 * iVar1 + -0x207d5;
    if (iVar1 < 0x1f4eb) {
      W[0xB5B4] = math_slope_angle(iVar1,0x1F4EB);
      puVar7 = 0x20B002 + (W[0xB5B4] & 0xfffc);
    }
    else {
      W[0xB5B4] = 0;
      puVar7 = 0x20B002;
    }
    iVar1 = math_atan2(vrd16s(puVar7 + 2) * 0x122 >> 0xf,
                       (vrd16s(puVar7 + 4) * 0x122 >> 0xf) + 0xdd);
    W[0xB534] = iVar2 + iVar1;
    W[0xB538] = 0;
    W_SET_LO16(0xB50C, 2);
  }
  W_SET_HI16(0xB508, (int16_t)0xffec);
  { extern int g_ik_dump; if (g_ik_dump)
      printf("[IKL] f=%d crank=%ld n9ang=%ld,%ld,%ld tgtL=%ld,%ld,%ld tgtR=%ld,%ld,%ld hipL=%ld,%ld,%ld hipR=%ld,%ld,%ld br=%d\n",
             (int)g_sys.frame_count, (long)W[0x169FC],
             (long)(W[0xAFB4]&0xffff),(long)(W[0xAFB8]&0xffff),(long)(W[0xAFBC]&0xffff),
             (long)W[0x16A1C],(long)W[0x16A20],(long)W[0x16A24], (long)W[0x16A10],(long)W[0x16A14],(long)W[0x16A18],
             (long)(W[0xB3B4]&0xffff),(long)(W[0xB3B8]&0xffff),(long)(W[0xB3BC]&0xffff),
             (long)(W[0xB534]&0xffff),(long)(W[0xB538]&0xffff),(long)(W[0xB53C]&0xffff), (int)(W[0x16A10] < 0x54)); }
  /* PARENT LINKS FOR NODES 19 AND 22 -- HIGH half, like every other one.
   *
   * These two were the last survivors of RIDER_RIG.md bug #6 (a 16-bit M68K
   * store into a longword field targets the TOP half; written full-width the
   * value lands in the bottom half and `scene_node_render`'s W_HI16 reads 0,
   * so the node takes the ROOT branch and is never composed onto its parent).
   * Note the line just above already uses W_SET_HI16 for node 20 -- these two
   * were simply missed.
   *
   * Measured, not inferred: tools/overnight/probe_nodes.lua reads the whole
   * node array out of MAME, and over 24 consecutive flyover frames the
   * machine holds 0xFFED003F / 0xFFEA003F here while we held 0x0000FFED /
   * 0x0000FFEA.  A full-width store also destroys the low half, which is the
   * 0x3F all-six-DOF mask the loader put there -- W_SET_HI16 preserves it. */
  W_SET_HI16(0xB488, (int16_t)0xffed);
  W_SET_HI16(0xB608, (int16_t)0xffea);
  return;
}





/* ---- player_model_load_animation @ 0x026B46 ---- */



void player_model_load_animation(int param_1)

{
  /* PLAYER + RIDER ANIMATION NODE LOADER.
   *
   * This used to treat param_1 as a POINTER and bail:
   *     piVar6 = (int *)(intptr_t)param_1;
   *     if (piVar6 == NULL || (uintptr_t)piVar6 < 0x1000) return;
   * Every one of the 7 call sites passes the selector 0 or 1, so the guard
   * fired on every boot ("[WARN] player_model_load_animation: NULL
   * pointer, skipping") and the node chain was never built. The original
   * transpilation had it right:
   *     piVar6 = (int *)(&PTR_PTR_0012abb0)[param_1];
   * i.e. a ROM POINTER TABLE at 0x12ABB0 indexed by the selector -- the
   * same two-level BE pointer chain as CLAUDE.md fix #22.
   *
   * Verified against the ROM:
   *   0x12ABB0[0] = 0x1247B8  gameplay set  -> 16 nodes, models 77..92,110
   *   0x12ABB0[1] = 0x1266A8  ending set
   *   0x12ACAC    = 6 nodes,  models 69..74
   * Model id + 0x45 is the render record code, so table 2 is the BIKE
   * (codes 138-143, which already rendered) and table 1 is the RIDER
   * (codes 146-161) -- exactly the 17 body parts the recording shows at
   * the bike's own distance and which we were missing entirely.
   *
   * Node stride is 0x20 ints = 0x80 slots, and the two chains are
   * contiguous: 0xAB04 + 6*0x80 = 0xAD84. player_render() walks the whole
   * thing via scene_node_render(&W[0xAB04]).
   *
   * All ROM reads are vrd32 (big-endian). The old body used native-order
   * derefs, which byte-swap every word of a BE ROM -- the DOF mask table
   * at 0x37664 is 1,2,4,8,0x10,0x20 and would have read as 0x01000000...
   */
  int k;
  W[0x16A28] = (int16_t)param_1;

  uint32_t lp = vrd32(0x12ABB0 + (uint32_t)param_1 * 4);
  if (lp == 0 || lp >= ROM_SIZE) {
      static int n_w;
      if (n_w++ == 0)
          printf("  [WARN] player_model_load_animation: bad table entry for "
                 "selector %d\n", param_1);
      return;
  }

  intptr_t cur = 0xAD84, node = cur;
  int16_t seq = 10;
  for (;;) {
      uint32_t nd = vrd32(lp);
      if (nd == 0 || nd >= ROM_SIZE) break;
      node = cur;
      W[node + 0x20 * 4] = (int32_t)vrd32(nd);
      W[node + 0x21 * 4] = (int32_t)vrd32(nd + 4);
      W[node + 0x22 * 4] = (int32_t)vrd32(nd + 8);

      uint32_t pf = nd + 0xc;                 /* per-DOF source words */
      /* flags live in the HIGH half on this big-endian target */
      int16_t flags = (int16_t)(W[node + 0x22 * 4] >> 16);

      if (flags == 0) {
          int32_t v = (int32_t)vrd32(pf);
          W[node + 0x2f * 4] = v;  W[node + 0x29 * 4] = v;
          for (k = 0; k < 5; k++) {
              int32_t u = (int32_t)vrd32(nd + 0x10 + k * 4);
              W[node + (0x30 + k) * 4] = u;  W[node + (0x2a + k) * 4] = u;
          }
      } else {
          for (k = 0; k < 6; k++) {
              uint32_t mask = vrd32(0x37664 + k * 4);
              uint32_t src  = vrd32(pf);
              int32_t  v;
              if ((mask & (uint32_t)(uint16_t)flags) == 0) {
                  v = (int32_t)src;
              } else {
                  W[node + (0x23 + k) * 4] = (int32_t)(src + 4);
                  v = (src < ROM_SIZE) ? (int32_t)vrd32(src) : 0;
              }
              pf += 4;
              W[node + (0x2f + k) * 4] = v;
              W[node + (0x29 + k) * 4] = v;
          }
      }
      W[node + 0x3e * 4] = seq;
      lp += 4;
      cur = node + 0x20 * 4;
      seq++;
      if (vrd32(lp) == 0) break;
  }
  W[node + 0x40 * 4] = -1;

  /* Second chain: the bike itself, 6 nodes from the table at 0x12ACAC. */
  intptr_t bn = 0xAB04;
  uint32_t tp = 0x12ACAC;
  int16_t idx = 0;
  for (;;) {
      uint32_t nd = vrd32(tp);
      if (nd == 0 || nd >= ROM_SIZE) break;
      int32_t w0 = (int32_t)vrd32(nd);
      W[bn]            = w0;
      W[bn + 0x15 * 4] = w0;
      W[bn + 1 * 4]    = (int32_t)vrd32(nd + 4);
      W[bn + 2 * 4]    = (int32_t)vrd32(nd + 8);
      for (k = 0; k < 6; k++) {
          int32_t u = (int32_t)vrd32(nd + 0xc + k * 4);
          W[bn + (0x0f + k) * 4] = u;
          W[bn + (0x09 + k) * 4] = u;
      }
      W[bn + 0x1e * 4] = idx;
      bn += 0x20 * 4;
      tp += 4;
      idx++;
  }

  W[0x169F8] = W[0x0D50] >> 4;
  W[0x169FC] = W[0x0D54] >> 5;
  W[0x16A00] = 0;
  W[0x16A04] = 0;
  W[0x16A08] = 0;
  W[0x16A0E] = 0;
  W[0x16A0C] = 0;
  W_SET_LO16(0x169EC, 0);   /* 16-bit at a 2-mod-4 offset -- see game_dsp3d.c */
  W[0x16A2C] = 0;
  W[0x16A2A] = 0;
  W[0xEB06] = 0;
  W[0xEB04] = 0;
  return;
}

/* ---- player_update_prev_pos @ 0x00B30A ---- */

void player_update_prev_pos(void)

{
  int iVar1;
  int iVar2;
  int iVar3;
  int local_18;
  int local_14;
  int local_10 [2];
  int local_8;
  
  iVar2 = W[0x12C8];
  if (W[0x1464] != 0) {
    W[0x12EC] = W[0x12EC] + -0x100;
  }
  if (W[0x15A4] != 0) {
    W[0x12EC] = W[0x12EC] + 0x100;
  }
  if (W[0x1564] != 0) {
    if (W[0x1568] < 0xe2) {
      W[0x12EC] = W[0x12EC] + 0x200;
    }
    else {
      W[0x12EC] = W[0x12EC] + -0x200;
    }
  }
  if (W[0x16A4] != 0) {
    if (W[0x16A8] < 0xe2) {
      W[0x12EC] = W[0x12EC] + -0x200;
    }
    else {
      W[0x12EC] = W[0x12EC] + 0x200;
    }
  }
  if (0x1000 < W[0x12EC]) {
    W[0x12EC] = 0x1000;
  }
  if (W[0x12EC] < -0x1000) {
    W[0x12EC] = -0x1000;
  }
  if ((((W[0x1464] == 0) && (W[0x15A4] == 0)) && (W[0x1564] == 0)) && (W[0x16A4] == 0))
  {
    W[0x12EC] = W[0x12EC] - (W[0x12EC] >> 6);
    if ((W[0x12EC] != 0) && (W[0x12EC] >> 6 == 0)) {
      W[0x12EC] = 0;
    }
  }
  iVar3 = W[0x12EC];
  if (W[0x12EC] < 0) {
    iVar3 = -W[0x12EC];
  }
  local_8 = 0;
  if (W[0x17E4] != 0) {
    W[0x12CC] = 0x70;
    if (W[0x17E8] < 0xe2) {
      W[0x12F0] = W[0x12F0] + -0x300;
    }
    else if (W[0x17E8] < 0xe4) {
      W[0x12F0] = W[0x12F0] + 0x180;
      local_8 = 1;
    }
  }
  if (0x2000 < W[0x12F0]) {
    W[0x12F0] = 0x2000;
  }
  if (W[0x12F0] < -0x3000) {
    W[0x12F0] = -0x3000;
  }
  if (W[0x1824] != 0) {
    W[0x12F0] = W[0x12F0] + -0x180;
  }
  if ((W[0x17E4] == 0) && (W[0x1824] == 0)) {
    W[0x12F0] = W[0x12F0] - (W[0x12F0] >> 6);
  }
  W[0x0D80] = W[0x2C04] + 1;
  if ((local_8 != 0) && (W[0x0D48] < 0xe00)) {
    W[0x0D80] = (0xe00 - W[0x0D48] >> 4) + W[0x0D80];
  }
  W[0x0D80] = W[0x0D80] * 0x4a >> 4;
  if (W[0x0D80] < 0) {
    W[0x0D80] = 0;
  }
  W[0x0D80] = W[0x0D80] + 1;
  if (0 < W[0x12D0]) {
    W[0x0D80] = 0;
  }
  W[0x0D84] = W[0x0D48] * W[0x0D48] >> 0x16;
  W[0x0D8C] = (short)vrd16s(0x20B004 + (W[0x0D0C] >> 1 & 0x7ffe) * 2) * -0x1b >> 0xe;
  W[0x0D48] = W[0x0D8C] + ((W[0x0D80] + W[0x0D48]) - W[0x0D84]);
  if (W[0x0D48] < 0) {
    W[0x0D48] = 0;
  }
  if ((W[0x0E30] != 0) && (W[0x0D48] = 0, 5 < W[0x0D80])) {
    W[0x0E30] = 0;
    W[0x0D48] = 0xe9e;
  }
  W[0x0D58] = (W[0x0D50] - W[0x0D58] >> 4) + W[0x0D58];
  W[0x0D5C] = (W[0x0D54] - W[0x0D5C] >> 5) + W[0x0D5C];
  W[0x0D60] = (W[0x0D54] - W[0x0D60] >> 5) + W[0x0D60];
  if (W[0x0E30] == 0) {
    /* sin(pitch angle W[0x0D0C]) -- was a ONE-BYTE read of the 16-bit Q15
     * trig table at the half-stride index (fix-#11 class), feeding the
     * pitch target every frame. Big-endian 16-bit, sin at 0x20B004. */
    W[0x0D58] = (-(int)vrd16s(0x20B004 + ((uint32_t)W[0x0D0C] & 0xfffc)) >> 5) + W[0x0D58];
  }
  g_terrain_mask = g_terrain_flags;
  g_terrain_flags = 0;
  if ((W[0x0E30] == 0) && (W[0x0D48] < 0xbab)) {
    W[0x0D9C] = W[0x0D9C] - (0xbab - W[0x0D48]);
    if ((short)W[0x0D9C] < -0x3000) {
      W[0x0D9C] = -0x3000;
    }
    g_terrain_flags = 1;
    iVar1 = 0xbab - W[0x0D48];
    W[0x12C8] = iVar1 * (iVar1 * iVar1 >> 8);
    if (0x500000 < W[0x12C8]) {
      W[0x12C8] = 0x500000;
    }
    if (W[0x12C8] < iVar2) {
      W[0x12C8] = iVar2;
    }
    if (W[0x12F4] < W[0x12C8]) {
      W[0x12F4] = W[0x12C8];
    }
  }
  if (0 < iVar2) {
    W[0x12C8] = W[0x12C8] - (W[0x12F4] >> 5);
  }
  if (W[0x12C8] < 1) {
    W[0x12C8] = 0;
    W[0x12F4] = 0;
  }
  W[0x0D9C] = (W[0x0D58] >> 8) + W[0x0D9C];
  W[0x0DA0] = W[0x0DA0] - ((W[0x0D5C] >> 9) + (W[0x0D5C] >> 8));
  W[0x0DA4] = W[0x0D5C] >> 3;
  if ((W[0x0E0C] == 0) && (W16(0xE10) == 0)) {
    if ((short)W[0x0D9C] < -0x1000) {
      W[0x0D9C] = -0x1000;
    }
    if (0x1000 < (short)W[0x0D9C]) {
      W[0x0D9C] = 0x1000;
    }
  }
  else {
    if ((short)W[0x0D9C] < -0x4000) {
      W[0x0D9C] = -0x4000;
    }
    if (0x4000 < (short)W[0x0D9C]) {
      W[0x0D9C] = 0x4000;
    }
  }
  if ((((W[0x1464] != 0) || (W[0x15A4] != 0)) || (W[0x1564] != 0)) || (W[0x16A4] != 0))
  {
    W[0x0DA0] = W[0x0DA0] - (W[0x12EC] >> 5);
    W[0x0D9C] = W[0x0D9C] - (iVar3 >> 7);
  }
  if (((W[0x17E4] != 0) || (W[0x1824] != 0)) && ((short)W[0x0D9C] < 0x3800)) {
    W[0x0D9C] = (W[0x12F0] >> 5) + W[0x0D9C];
  }
  W[0x0D18] = W[0x0D00];
  W[0x0D1C] = W[0x0D04];
  W[0x0D20] = W[0x0D08];
  spherical_to_cartesian
            (W[0x0D9C],W[0x0DA0],W[0x0D48] >> 4,&local_18,&local_14,local_10);
  W[0x0D00] = local_18 + W[0x0D00];
  W[0x0D04] = local_14 + W[0x0D04];
  W[0x0D08] = local_10[0] + W[0x0D08];
  W[0x0DA8] = ((short)((short)W[0x0D9C] - (short)W[0x0DA8]) >> (W[0x12CC] >> 4 & 0x3fU))
                 + W[0x0DA8];
  W[0x0DAC] = ((short)((short)W[0x0DA0] - (short)W[0x0DAC]) >> (W[0x12CC] >> 4 & 0x3fU))
                 + W[0x0DAC];
  W[0x0DB0] = ((short)((short)W[0x0DA4] - (short)W[0x0DB0]) >> (W[0x12CC] >> 6 & 0x3fU))
                 + W[0x0DB0];
  W[0x12E8] = ((short)((short)(-(int)(short)((short)W[0x0DA0] - (short)W[0x0DAC]) >> 2) -
                         (short)W[0x12E8]) >> 1) + W[0x12E8] >> (W[0x12E0] >> 5 & 0x3fU);
  W[0x0D0C] = (W[0x12F0] >> 1) + W[0x0DA8] + (W[0x0D58] >> 3);
  W[0x0D10] = W[0x0DAC] - (W[0x0D60] >> 3);
  W[0x0D14] = W[0x12EC] + W[0x12E8] + W[0x0DB0] + (W[0x0D5C] >> 3);
  /* Flight-recorder PLAYBACK: overwrite what this function just integrated
   * with the recorded frame, here where the attitude is produced -- the
   * pin before game_frame() alone was re-integrated away (no stick, stall
   * pitch, hard-reset speed) and the rider flipped around every frame. */
  { extern void flight_replay_pin_attitude(void); flight_replay_pin_attitude(); }
  return;
}





