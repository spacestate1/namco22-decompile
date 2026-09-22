/* ---- FLIGHT RECORDER ------------------------------------------------------
 *
 * Records a real player's flight, frame by frame, so a defect that only a
 * human hits can be replayed headlessly and compared against MAME.
 *
 * WHY THIS EXISTS. Every headless probe in this tree either sits still, flies
 * a scripted steer wave, or is warped to a coordinate somebody typed. None of
 * those is the path a player actually flies, and CLAUDE.md already records
 * two bugs (rows 90 and 129) that survived the entire gate suite precisely
 * because no automated test ever went where a human goes. The tunnel reports
 * are the same shape: probing the cells by hand found our collision column
 * agreeing with MAME at the points probed, while the player still hit a wall.
 * The missing datum is WHERE they were, and what the column said THERE.
 *
 * So the recording carries both halves on every frame: the player's state
 * (position, attitude, speed, raw stick) AND the collision column the engine
 * resolved for the centre probe (layer count, object mask, every face the
 * layer walk produced with its kind and height, the contact flag and the
 * resolved floor). A recording is therefore self-diagnosing -- the frame
 * where `contact` flips to 1 is the wall, and the same line says which faces
 * were in the column when it happened.
 *
 * WHAT IT DOES NOT DO: it does not record INPUT for deterministic re-simulation.
 * The pedal is a pulse encoder driven off wall-clock time in `input.c`, so an
 * input-only replay would not reproduce the same speed, let alone the same
 * path. Replay therefore PINS the recorded position each frame and lets the
 * collision code run against it, which reproduces exactly the columns the
 * player flew through. That is the question being asked; it is not a
 * substitute for a deterministic re-run, and it is not claimed to be one.
 *
 * Usage:
 *   In game:   Escape -> Record -> Start, fly, Escape -> Record -> Stop.
 *              Files land in recordings/ -- the player's data, kept clear
 *              of the working directory so no cleanup can glob it away.
 *              Or the RECORD key (default F9) to toggle without the menu.
 *   Replay:    PROPCYCL_REPLAY=<file> ./build/propcycl extracted/ \
 *                  --autostart 0 --screenshot /dev/null <frames>
 *              prints a [REPLAY] line per frame with OUR column at the
 *              recorded position, and a [REPLAY-DIFF] line wherever the
 *              contact flag or the resolved floor disagrees with what was
 *              recorded.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include "propcycl.h"

extern intptr_t _W[];
#define W _W

/* The centre probe's collision record. Bases from CLAUDE.md's terrain
 * section: the six probe records start at 0xE0130C and stride 0x140. */
#define P0_CONTACT 0x1324
#define P0_FLOOR   0x1328
#define P0_SURF    0x132C
#define P0_NFACE   0x1330
#define P0_FACES   0x1334   /* stride 4 */
#define P0_TYPES   0x13AC   /* stride 1, byte */
#define P0_PARITY  0x13E8
/* ALL SIX PROBES, not just the centre. `world_objects_init_player` builds six:
 * 0 centre, 1 right wing tip, 2 left wing tip, 3 nose, 4 top, 5 a look-ahead
 * along the target attitude -- and `world_render_terrain`'s whole hit response
 * is gated on probe 5's flag (`tst.l $658(a3)` with a3 = 0xE0130C, register
 * row 91). Recording the centre alone would show a clean column on the exact
 * frame a wing or the nose stopped the bike, which is the failure mode this
 * file exists to avoid. Record stride 0x140. */
#define PROBE_CONTACT(i) (0x1324 + (i) * 0x140)
#define N_PROBES 6
/* The hard-reset branch (register row 97) forces speed to exactly 0x900 and
 * restores the saved position -- that is the "thrown back" a player feels as
 * hitting a wall, and it is worth marking explicitly rather than inferring. */
#define HARD_RESET_SPEED 0x900

static FILE *g_rec;
static char  g_rec_path[512];
static int   g_rec_frames;
static int   g_rec_contacts;
static int   g_rec_resets;

/* ---------------------------------------------------------------- recording */

const char *flight_rec_path(void)   { return g_rec ? g_rec_path : NULL; }
int         flight_rec_active(void) { return g_rec != NULL; }
int         flight_rec_frames(void) { return g_rec_frames; }
int         flight_rec_contacts(void){ return g_rec_contacts; }
int         flight_rec_resets(void)  { return g_rec_resets; }

void flight_rec_stop(void)
{
    if (!g_rec) return;
    fprintf(g_rec, "# END frames=%d contact_frames=%d resets=%d\n",
            g_rec_frames, g_rec_contacts, g_rec_resets);
    fclose(g_rec);
    g_rec = NULL;
    fprintf(stderr, "[REC] stopped: %d frames, %d with terrain contact, "
                    "%d hard resets -> %s\n",
            g_rec_frames, g_rec_contacts, g_rec_resets, g_rec_path);
    fflush(stderr);
}

void flight_rec_start(void)
{
    if (g_rec) flight_rec_stop();
    /* Name by wall-clock so repeated takes never overwrite each other -- a
     * player retrying a spot five times wants five files, not one. */
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    /* Into recordings/, NOT the working directory. A recording is the
     * PLAYER'S DATA and the only copy of a defect that took a human to
     * reproduce; it must not live where a scratch cleanup can glob it away.
     * (It did once: `rm -f flight_2026*.rec` between two measurements took
     * the user's own capture with it.) */
    mkdir("recordings", 0777);
    snprintf(g_rec_path, sizeof g_rec_path, "recordings/flight_%04d%02d%02d_%02d%02d%02d.rec",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    g_rec = fopen(g_rec_path, "w");
    if (!g_rec) {
        fprintf(stderr, "[REC] could not open %s for writing\n", g_rec_path);
        return;
    }
    g_rec_frames = 0;
    g_rec_contacts = 0;
    g_rec_resets = 0;
    /* The course is written on the FIRST RECORDED FRAME, not here: this
     * runs at startup, long before the STAGE SELECT sets W[0x0E0C], so a
     * header written now always said course 0 -- which read as "the stage
     * selection never reaches gameplay" when it reaches it perfectly well. */
    fprintf(g_rec, "# propcycl flight recording v1\n"
                   "# started_at_frame=%u (course is on the first F line)\n"
                   "# F <frame> st=<state>/<sub> p=<x>,<y>,<z> hdg=<h> pit=<p> rol=<r> spd=<s>\n"
                   "#   cell=<c> chunk=<model> adc=<x>,<y> pedal=<n> eul=<a0>,<a1>,<a2>\n"
                   "#     eul = W[0x0D0C]/0x0D10/0x0D14, the angles the six probes are\n"
                   "#     rotated by. The contact is attitude-dependent, so a comparison\n"
                   "#     against MAME is meaningless without them.\n"
                   "#   layers=<n> mask=<hex> contact=<0|1> probes=<hex bitmask>\n"
                   "#   pfloor=<f0..f5> each probe's resolved floor; pnf=<n0..n5> each\n"
                   "#     probe's FACE COUNT -- 0 faces means its layer walk found nothing\n"
                   "#     (row 144), a full column that still buries is the walk (row 146)\n"
                   "#   jump=<units moved this frame>[ RESET] floor=<y> surf=<hex> col=<n>[ S@h|P@h ...]\n"
                   "# S = solid face (blocks), P = pass-through face (fly through).\n"
                   "# probes bit0 centre, 1 right wing, 2 left wing, 3 nose, 4 top,\n"
                   "#   5 LOOK-AHEAD -- bit 5 is the one world_render_terrain's hit\n"
                   "#   response is gated on, so a wall can show there and nowhere else.\n"
                   "# RESET marks the hard-reset branch (speed forced to 0x900 and the\n"
                   "#   position restored) -- the 'thrown back' a player feels as a wall.\n",
            (unsigned)g_sys.frame_count);
    fflush(g_rec);
    fprintf(stderr, "[REC] recording to %s\n", g_rec_path);
    fflush(stderr);
}

void flight_rec_toggle(void) { if (g_rec) flight_rec_stop(); else flight_rec_start(); }

/* Called once per frame from the main loop, after game_frame(). */
void flight_rec_tick(void)
{
    if (!g_rec) return;
    /* Only gameplay is meaningful -- the menus have no player. */
    if ((int32_t)W[0x0CBC] != 3 || (int32_t)W[0x0CC0] != 3) return;

    long px = (long)(int32_t)W[0x0D00], py = (long)(int32_t)W[0x0D04],
         pz = (long)(int32_t)W[0x0D08];
    int  gx = (int)(px / 0x18000), gz = (int)(pz / 0x18000);
    int  cell = gx + gz * 8;
    int  course = (int)W[0x0E0C];
    int  base[4] = { 1173, 1301, 1429, 1557 };
    int  contact = (int)W[P0_CONTACT];
    int  probes = 0;
    char pfl[192]; int po = 0;
    char pxyz[512]; int qo = 0;
    char pnf[64]; int co = 0;
    char psurf[64]; int so = 0;
    for (int i = 0; i < N_PROBES; i++) {
        if ((int)W[PROBE_CONTACT(i)]) probes |= 1 << i;
        /* EVERY probe's resolved floor, not just the centre's. A player who
         * flies into a vertical face has the wing, nose and top probes in
         * contact while the CENTRE still reads clear air thousands of units
         * above its floor -- measured: contact on probes 2/3/4/5 with the
         * centre's floor 8000 below the bike. A recording that carries only
         * the centre cannot distinguish that from no contact at all, and a
         * MAME probe that reads only the centre cannot be compared against
         * it. Both now carry all six. */
        po += snprintf(pfl + po, sizeof pfl - po, "%s%d",
                       i ? "," : "", (int)(int32_t)W[PROBE_CONTACT(i) + 4]);
        /* and where each probe actually IS -- world_object_tick point-tests
         * X at 0x1318+i*0x140, Y at 0x131C+i*0x140, Z at 0x1320+i*0x140.
         * A probe reporting "buried" is either in the wrong place or
         * resolving the wrong column at the right place; only its position
         * separates those two, and they are fixed in different functions. */
        /* and how many faces each probe's column held. This is the one field
         * that separates the two failure modes cheaply: a probe with ZERO
         * terrain faces was reported buried by the parity rule because its
         * layer walk found nothing (register row 144's class), while a probe
         * with a full column that still reads buried is the walk OVER those
         * faces going wrong (row 146's class). They are fixed in completely
         * different functions, and inferring which from the centre probe's
         * column is guesswork. */
        co += snprintf(pnf + co, sizeof pnf - co, "%s%d",
                       i ? "," : "", (int)W[0x1330 + i * 0x140]);
        /* THE PARTICLE GATE, per layer. The spray/dust billboards near the
         * ground and water are 2D SPRITES, not objects, so they appear in no
         * object census -- `particle_system_update` fires an emitter only
         * when this field holds 0xE0, 0xE1 or 0xE5 (ROM 0x029040,
         * `cmpi.l #$e0/#$e1/#$e5, $11c(a3,d0.l)`, register row 131). "I fly
         * low and see no dust" is first a question about these six bytes. */
        so += snprintf(psurf + so, sizeof psurf - so, "%s%02X",
                       i ? "," : "", (unsigned)((int)W[0x1428 + i * 0x140] & 0xFF));
        qo += snprintf(pxyz + qo, sizeof pxyz - qo, "%s%d/%d/%d",
                       i ? " " : "",
                       (int)(int32_t)W[0x1318 + i * 0x140],
                       (int)(int32_t)W[0x131C + i * 0x140],
                       (int)(int32_t)W[0x1320 + i * 0x140]);
    }
    pfl[po] = 0; pxyz[qo] = 0; pnf[co] = 0; psurf[so] = 0;
    int  spd = (int)(int32_t)W[0x0D48];
    /* A reset teleports the bike; a plain jump in position does not, so use
     * both: the speed snap is the branch's signature and the distance
     * confirms it actually moved the player. */
    static long  prev_x, prev_y, prev_z;
    static int   have_prev;
    long dx = px - prev_x, dy = py - prev_y, dz = pz - prev_z;
    long jump = (have_prev && (int32_t)W[0x0CC0] == 3)
                ? (long)(dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + (dz < 0 ? -dz : dz)
                : 0;
    int  reset = (spd == HARD_RESET_SPEED && jump > 20000);
    prev_x = px; prev_y = py; prev_z = pz; have_prev = 1;
    int  nface = (int)W[P0_NFACE];
    if (nface < 0) nface = 0;
    if (nface > 16) nface = 16;

    char col[512];
    int o = 0;
    for (int k = 0; k < nface && o < (int)sizeof col - 24; k++) {
        int32_t v = (int32_t)W[P0_FACES + k * 4];
        o += snprintf(col + o, sizeof col - o, " %c@%d",
                      ((v & 0xf) == 4) ? 'P' : 'S', (int)(v >> 4));
    }
    col[o] = 0;

    if (probes) g_rec_contacts++;

    fprintf(g_rec,
            "F %u course=%d st=%d/%d p=%ld,%ld,%ld hdg=%ld pit=%ld rol=%ld spd=%ld "
            "cell=%d chunk=%d adc=%d,%d pedal=%d eul=%ld,%ld,%ld gates=%02X/%02X/%02X "
            "layers=%d mask=%02X contact=%d probes=%02X pfloor=%s pnf=%s "
            "psurf=%s nspr=%ld pxyz=[%s] jump=%ld%s "
            "floor=%d surf=%02X col=%d%s\n",
            (unsigned)g_sys.frame_count, (int)W[0x0E0C], (int)W[0x0CBC], (int)W[0x0CC0],
            px, py, pz,
            (long)(W[0x0DAC] & 0xFFFF), (long)(int32_t)W[0x0D9C],
            (long)(int32_t)W[0x0DB0], (long)(int32_t)W[0x0D48],
            cell,
            (course >= 0 && course < 4 && cell >= 0 && cell < 128)
                ? base[course] + cell : -1,
            (int)(int32_t)W[0x0D50], (int)(int32_t)W[0x0D54], (int)(int32_t)W[0x2C04],
            /* THE ANGLES THE PROBES ARE ACTUALLY PLACED WITH.
             * `world_objects_init_player` rotates each probe's fixed offset
             * through `rotate_euler_zxy_optimized(..., W[0x0D0C], W[0x0D10],
             * W[0x0D14], ...)`. Those are NOT the heading and pitch logged
             * above (0x0DAC / 0x0D9C), which are the smoothed targets. Without
             * them a recording cannot be compared against MAME at all: the
             * probes land somewhere else, and measured, the contact IS
             * attitude-dependent -- pinned at the player's wall position with
             * a different attitude, all six probes read clear on both sides. */
            (long)(int32_t)W[0x0D0C], (long)(int32_t)W[0x0D10], (long)(int32_t)W[0x0D14],
            /* THE PROP GATE BYTES. Several per-group tests in game_props.c are
             * a single WRAM byte compared against 0xFF -- ROM 0x5178
             * `cmpi.b #$ff, $e012a1.l` gates model 374, 0xE01267 gates 375,
             * 0xE01266 gates 376. "The grass / dust / spectators do not come
             * up" is first a question about whether those bytes hold what the
             * machine's hold, and that is cheaper to read than the emitter. */
            (unsigned)g_sys.work_ram[0x1266], (unsigned)g_sys.work_ram[0x1267],
            (unsigned)g_sys.work_ram[0x12A1],
            (int)W[0x0964], (unsigned)((int)W[0x0968] & 0xFF),
            contact, probes, pfl, pnf, psurf, (long)W[0x4AEC], pxyz, jump,
            reset ? " RESET" : "",
            (int)(int32_t)W[P0_FLOOR], (unsigned)((int)W[P0_SURF] & 0xFF),
            nface, col);
    if (reset) g_rec_resets++;
    g_rec_frames++;
    /* Flushed every frame on purpose: a recording is most valuable when the
     * run ends badly, and a crash must not take the evidence with it. */
    fflush(g_rec);
}

/* ----------------------------------------------------------------- playback */

typedef struct { unsigned frame; int32_t x, y, z, e0, e1, e2; int have_eul;
                 int32_t hdg, pit, rol, spd; int have_att;
                 int contact, floor, nface; } RepFrame;
static RepFrame *g_rep;
static int       g_rep_n, g_rep_i;
static int       g_rep_diffs;

void flight_replay_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[REPLAY] cannot open %s\n", path); return; }
    int cap = 4096;
    g_rep = (RepFrame *)malloc(cap * sizeof *g_rep);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (line[0] != 'F') continue;
        RepFrame r; memset(&r, 0, sizeof r);
        const char *p;
        if (sscanf(line, "F %u", &r.frame) != 1) continue;
        if ((p = strstr(line, " p=")) == NULL) continue;
        if (sscanf(p + 3, "%d,%d,%d", &r.x, &r.y, &r.z) != 3) continue;
        if ((p = strstr(line, " hdg=")) != NULL
            && sscanf(p + 5, "%d pit=%d rol=%d spd=%d", &r.hdg, &r.pit, &r.rol, &r.spd) == 4)
            r.have_att = 1;
        if ((p = strstr(line, " contact=")) != NULL) r.contact = atoi(p + 9);
        if ((p = strstr(line, " floor="))   != NULL) r.floor   = atoi(p + 7);
        if ((p = strstr(line, " col="))     != NULL) r.nface   = atoi(p + 5);
        if ((p = strstr(line, " eul=")) != NULL
            && sscanf(p + 5, "%d,%d,%d", &r.e0, &r.e1, &r.e2) == 3) r.have_eul = 1;
        if (g_rep_n == cap) { cap *= 2; g_rep = (RepFrame *)realloc(g_rep, cap * sizeof *g_rep); }
        g_rep[g_rep_n++] = r;
    }
    fclose(f);
    fprintf(stderr, "[REPLAY] %d frames from %s\n", g_rep_n, path);
}

int flight_replay_active(void) { return g_rep_n > 0 && g_rep_i < g_rep_n; }

/* Pin the player to the next recorded position. Called from the main loop
 * BEFORE game_frame(), the same place and the same way PROPCYCL_WARP writes,
 * i.e. into g_sys.work_ram as well as _W[] -- game_frame()'s first act is
 * sync_wram_to_W(), which would otherwise discard a _W[]-only write. */
void flight_replay_pin(void)
{
    if (!flight_replay_active()) return;
    if ((int32_t)W[0x0CBC] != 3 || (int32_t)W[0x0CC0] != 3) return;
    RepFrame *r = &g_rep[g_rep_i];
#define REP_SET32(off, v) do { \
        g_sys.work_ram[(off)]   = (uint8_t)(((uint32_t)(v)) >> 24); \
        g_sys.work_ram[(off)+1] = (uint8_t)(((uint32_t)(v)) >> 16); \
        g_sys.work_ram[(off)+2] = (uint8_t)(((uint32_t)(v)) >> 8);  \
        g_sys.work_ram[(off)+3] = (uint8_t)((uint32_t)(v)); } while (0)
    REP_SET32(0x0D00, r->x); REP_SET32(0x0D04, r->y); REP_SET32(0x0D08, r->z);
    W[0x0D00] = r->x; W[0x0D04] = r->y; W[0x0D08] = r->z;
    /* Attitude too, where the recording carries it: the six probes are fixed
     * offsets ROTATED by these angles, so pinning the position alone puts
     * them somewhere the player never was. */
    if (r->have_eul) {
        REP_SET32(0x0D0C, r->e0); REP_SET32(0x0D10, r->e1); REP_SET32(0x0D14, r->e2);
        W[0x0D0C] = r->e0; W[0x0D10] = r->e1; W[0x0D14] = r->e2;
    }
#undef REP_SET32
}

/* Called at the end of player_update_prev_pos (the flight integrator):
 * replace its results with the recorded frame, so the camera, the rider and
 * the six probes all see the attitude the player actually flew. The eul
 * words are the integrator's OUTPUTS (0x0D0C/10/14); hdg/pit/rol/spd are its
 * state (0x0DAC smoothed heading, 0x0D9C pitch, 0x0DB0 smoothed roll, 0x0D48
 * speed). The raw heading/roll and smoothed pitch are not recorded, so they
 * are set to their closest recorded counterparts, which keeps the next
 * frame's smoothing from pulling away. PROPCYCL_REPLAY_POSONLY=1 restores
 * the old position-only pin. */
void flight_replay_pin_attitude(void)
{
    static int posonly = -1;
    if (posonly < 0) { const char *e = getenv("PROPCYCL_REPLAY_POSONLY"); posonly = e && atoi(e); }
    if (posonly || !flight_replay_active()) return;
    if ((int32_t)W[0x0CBC] != 3 || (int32_t)W[0x0CC0] != 3) return;
    RepFrame *r = &g_rep[g_rep_i];
    W[0x0D00] = r->x; W[0x0D04] = r->y; W[0x0D08] = r->z;
    if (r->have_eul) {
        W[0x0D0C] = r->e0; W[0x0D10] = r->e1; W[0x0D14] = r->e2;
        W[0x0DA8] = r->e0;
    }
    if (r->have_att) {
        /* hdg is 0x0DAC's low 16 bits; unwrap it next to eul's heading */
        int32_t h = r->have_eul ? r->e1 + (int16_t)(r->hdg - r->e1) : r->hdg;
        W[0x0DAC] = h; W[0x0DA0] = h;
        W[0x0D9C] = r->pit;
        W[0x0DB0] = r->rol; W[0x0DA4] = r->rol;
        W[0x0D48] = r->spd;
    }
}

/* Called after game_frame(): report our column at the pinned position and
 * flag any disagreement with what the player's machine recorded. */
void flight_replay_report(void)
{
    if (!flight_replay_active()) return;
    if ((int32_t)W[0x0CBC] != 3 || (int32_t)W[0x0CC0] != 3) return;
    RepFrame *r = &g_rep[g_rep_i];

    int contact = (int)W[P0_CONTACT];
    int floor   = (int)(int32_t)W[P0_FLOOR];
    int nface   = (int)W[P0_NFACE];
    if (nface < 0) nface = 0; if (nface > 16) nface = 16;

    char col[512]; int o = 0;
    for (int k = 0; k < nface && o < (int)sizeof col - 24; k++) {
        int32_t v = (int32_t)W[P0_FACES + k * 4];
        o += snprintf(col + o, sizeof col - o, " %c@%d",
                      ((v & 0xf) == 4) ? 'P' : 'S', (int)(v >> 4));
    }
    col[o] = 0;

    long px = (long)(int32_t)W[0x0D00], pz = (long)(int32_t)W[0x0D08];
    int cell = (int)(px / 0x18000) + (int)(pz / 0x18000) * 8;

    fprintf(stderr, "[REPATT] rec_f%u hdg %d/%ld pit %d/%ld rol %d/%ld spd %d/%ld cam %ld,%ld,%ld eul %ld,%ld,%ld\n",
            r->frame, r->hdg, (long)(W[0x0DAC] & 0xFFFF), r->pit, (long)(int32_t)W[0x0D9C],
            r->rol, (long)(int32_t)W[0x0DB0], r->spd, (long)(int32_t)W[0x0D48],
            (long)(W[0x0CE8] & 0xFFFF), (long)(W[0x0CEC] & 0xFFFF), (long)(W[0x0CF0] & 0xFFFF),
            (long)(int32_t)W[0x0D0C], (long)(int32_t)W[0x0D10], (long)(int32_t)W[0x0D14]);
    fprintf(stderr, "[REPLAY] rec_f%u p=%d,%d,%d cell=%d layers=%ld mask=%02lX "
                    "ours: contact=%d floor=%d col=%d%s  (recorded: contact=%d floor=%d col=%d)\n",
            r->frame, r->x, r->y, r->z, cell,
            (long)W[0x0964], (long)W[0x0968] & 0xFF,
            contact, floor, nface, col, r->contact, r->floor, r->nface);

    if (contact != r->contact || nface != r->nface) {
        g_rep_diffs++;
        fprintf(stderr, "[REPLAY-DIFF] rec_f%u at (%d,%d,%d) cell %d: "
                        "contact %d->%d, faces %d->%d\n",
                r->frame, r->x, r->y, r->z, cell,
                r->contact, contact, r->nface, nface);
    }
    fflush(stderr);
    g_rep_i++;
    if (g_rep_i >= g_rep_n)
        fprintf(stderr, "[REPLAY] done: %d frames, %d disagreed with the recording\n",
                g_rep_n, g_rep_diffs);
}
