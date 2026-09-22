/*
 * ui_menu.c — Nuklear menu bar: File (Restart / Exit) / Display / Audio / Levels / ...
 *
 * Nuklear is an immediate-mode GUI, so there is no widget tree to keep in
 * sync: the menu is rebuilt every frame from the state below.
 *
 * The SDL/GL2 backend wraps its draw in glPushAttrib/glPopAttrib, which
 * matters here because the rest of the renderer leans on fixed-function
 * state (texture env, alpha test, blend). Draw the UI LAST, after the
 * scene, or it will fight that state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_GL2_IMPLEMENTATION
#include "../third_party/nuklear.h"
#include "../third_party/nuklear_sdl_gl2.h"

#include "ui_menu.h"
#include "propcycl.h"

static struct nk_context *ctx;
static bool menu_open;
static SDL_Window *g_win;

/* ---- controls ---------------------------------------------------------- */
SDL_Scancode ui_binding[ACT_COUNT];
static const char *act_names[ACT_COUNT] = {
    "Insert coin", "Start", "Service", "Test",
    "Steer left", "Steer right", "Lean up", "Lean down",
    "Pedal"
};
const char *ui_action_name(ui_action a) { return act_names[a]; }

void ui_controls_defaults(void) {
    ui_binding[ACT_COIN]    = SDL_SCANCODE_5;
    ui_binding[ACT_START]   = SDL_SCANCODE_RETURN;
    ui_binding[ACT_SERVICE] = SDL_SCANCODE_9;
    ui_binding[ACT_TEST]    = SDL_SCANCODE_F2;
    ui_binding[ACT_LEFT]    = SDL_SCANCODE_LEFT;
    ui_binding[ACT_RIGHT]   = SDL_SCANCODE_RIGHT;
    ui_binding[ACT_UP]      = SDL_SCANCODE_UP;
    ui_binding[ACT_DOWN]    = SDL_SCANCODE_DOWN;
    /* Space: the pedal is the one control you hold continuously,
     * and every other key on the cabinet is already spoken for. */
    ui_binding[ACT_PEDAL]   = SDL_SCANCODE_SPACE;
}

#include "audio_hle.h"

#define CFG_PATH "propcycl_controls.cfg"

int ui_controls_save(void) {
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) return 0;
    fprintf(f, "# Prop Cycle control bindings (SDL scancodes)\n");
    for (int i = 0; i < ACT_COUNT; i++)
        fprintf(f, "%s=%d\n", act_names[i], (int)ui_binding[i]);
    fprintf(f, "volume=%d\n", (int)(audio_hle_volume() * 100.0f + 0.5f));
    fclose(f);
    return 1;
}

int ui_controls_load(void) {
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *eq = strrchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        if (!strcmp(line, "volume")) { audio_hle_set_volume(atoi(eq + 1) / 100.0f); continue; }
        for (int i = 0; i < ACT_COUNT; i++)
            if (!strcmp(line, act_names[i]))
                ui_binding[i] = (SDL_Scancode)atoi(eq + 1);
    }
    fclose(f);
    return 1;
}

/* Which action is waiting for a key, or -1. */
static int rebinding = -1;

/* ---- map viewer state --------------------------------------------------- */
static bool  map_on;
static int   map_course;
/* Start looking down at the grid from slightly off-axis: a top-down-ish
 * pitch shows the whole course, which is what the map view is for. */
static float map_yaw = 0.0f, map_pitch = 1.05f, map_zoom = 1.0f;
static float map_cx, map_cy, map_cz;

bool ui_map_active(void) { return map_on; }
int  ui_map_course(void) { return map_course; }

/* Mouse conventions follow the usual 3D-editor ones:
 *   middle drag        orbit          (left drag also orbits, for trackpads)
 *   shift + middle     pan
 *   right drag         pan
 *   wheel              zoom (up = closer)
 */
void ui_map_mouse(int dx, int dy, int buttons, int wheel) {
    if (!map_on) return;
    int mid   = (buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE)) != 0;
    int left  = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT))   != 0;
    int right = (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT))  != 0;
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];

    if ((mid && !shift) || left) {                 /* orbit */
        map_yaw   += dx * 0.005f;
        map_pitch += dy * 0.005f;
        if (map_pitch < -1.5f) map_pitch = -1.5f;
        if (map_pitch >  1.5f) map_pitch =  1.5f;
    } else if ((mid && shift) || right) {          /* pan along view axes */
        float sp = 9.0f * (map_zoom < 1.0f ? 1.0f : map_zoom);   /* see ui_map_input */
        float cy = cosf(map_yaw),   sy = sinf(map_yaw);
        float cp = cosf(map_pitch), sp_ = sinf(map_pitch);
        float rt[3] = { cy, 0.0f, -sy };
        float up[3] = { sy * sp_, cp, cy * sp_ };
        map_cx -= (rt[0] * dx + up[0] * dy) * sp;
        map_cy -= (rt[1] * dx + up[1] * dy) * sp;
        map_cz -= (rt[2] * dx + up[2] * dy) * sp;
    }
    if (wheel) {
        /* multiplicative so each notch feels the same at any distance */
        map_zoom *= (wheel > 0) ? 0.88f : 1.136f;
        if (map_zoom < 0.05f) map_zoom = 0.05f;
        if (map_zoom > 20.0f) map_zoom = 20.0f;
    }
}

void ui_map_input(const Uint8 *keys, float dt) {
    if (!map_on) return;
    /* Speed. The course grid is ~786k x 1474k units, so a usable base is
     * "cross the map in a few seconds" -- the old 40000/s took 37 s.
     *
     * Zoom scaling is clamped to >= 1: zooming OUT may speed travel up, but
     * zooming IN must never slow it down. Multiplying straight by map_zoom
     * (which shrinks as you close in) made the camera crawl exactly when
     * you were trying to move around detail. */
    float zs = map_zoom < 1.0f ? 1.0f : map_zoom;
    float sp = 500000.0f * dt * zs;
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) sp *= 4.0f;
    if (keys[SDL_SCANCODE_LCTRL]  || keys[SDL_SCANCODE_RCTRL])  sp *= 0.2f;

    /* Camera-relative movement: W goes where the camera LOOKS, whatever the
     * orientation, including pitch. The view matrix built in
     * ui_map_camera() is orthonormal with the m[src][dst] convention, so a
     * world direction mapping to a view axis is just the matching COLUMN:
     *   forward (view +Z) = ( sy*cp, -sp,  cy*cp )
     *   right   (view +X) = ( cy,     0,  -sy    )
     *   up      (view +Y) = ( sy*sp,  cp,  cy*sp )
     * The previous version used yaw only, so W drifted sideways as soon as
     * the view was pitched. */
    float cy = cosf(map_yaw),   sy = sinf(map_yaw);
    float cp = cosf(map_pitch), sp_ = sinf(map_pitch);
    float fwd[3]   = {  sy * cp, -sp_,  cy * cp };
    float right[3] = {  cy,       0.0f, -sy     };
    float up[3]    = {  sy * sp_, cp,    cy * sp_ };

    float mv[3] = { 0, 0, 0 };
    if (keys[SDL_SCANCODE_W]) for (int i=0;i<3;i++) mv[i] += fwd[i];
    if (keys[SDL_SCANCODE_S]) for (int i=0;i<3;i++) mv[i] -= fwd[i];
    if (keys[SDL_SCANCODE_D]) for (int i=0;i<3;i++) mv[i] += right[i];
    if (keys[SDL_SCANCODE_A]) for (int i=0;i<3;i++) mv[i] -= right[i];
    if (keys[SDL_SCANCODE_Q]) for (int i=0;i<3;i++) mv[i] += up[i];
    if (keys[SDL_SCANCODE_E]) for (int i=0;i<3;i++) mv[i] -= up[i];

    map_cx += mv[0] * sp;
    map_cy += mv[1] * sp;
    map_cz += mv[2] * sp;
}

/* Q15 orbit matrix + translation for geo_hw. */
void ui_map_camera(float m[3][3], float t[3], float *zoom) {
    float cy = cosf(map_yaw),  sy = sinf(map_yaw);
    float cp = cosf(map_pitch), sp = sinf(map_pitch);
    /* yaw about Y then pitch about X, in the m[src][dst] convention geo_hw
     * uses for the vertex transform */
    m[0][0] =  cy;      m[0][1] =  sy * sp; m[0][2] =  sy * cp;
    m[1][0] =  0.0f;    m[1][1] =  cp;      m[1][2] = -sp;
    m[2][0] = -sy;      m[2][1] =  cy * sp; m[2][2] =  cy * cp;
    t[0] = map_cx; t[1] = map_cy; t[2] = map_cz;
    *zoom = map_zoom;
}

/* ---- display ------------------------------------------------------------ */
static const struct { int w, h; const char *label; } modes[] = {
    { 640,  480,  "640 x 480 (native)" },
    { 960,  720,  "960 x 720" },
    { 1280, 960,  "1280 x 960" },
    { 1600, 1200, "1600 x 1200" },
    { 1920, 1440, "1920 x 1440" },
};
#define NMODES ((int)(sizeof modes / sizeof modes[0]))
static int cur_mode = 2;
static int win_mode;        /* 0 windowed, 1 fullscreen desktop, 2 exclusive */

/* Aspect: the game renders a fixed 640x480 (4:3). Stretching that to a
 * 16:9 window distorts it, so offer pillarboxing and the original 8:7 the
 * hardware actually scanned out. */
static const struct { float ar; const char *label; } aspects[] = {
    { 0.0f,        "Stretch to window" },
    { 4.0f / 3.0f, "4:3 (pillarboxed)" },
    { 8.0f / 7.0f, "8:7 (hardware pixels)" },
    { 16.0f / 9.0f,"16:9 (crop-free fill)" },
};
#define NASPECT ((int)(sizeof aspects / sizeof aspects[0]))
static int cur_aspect = 1;
float ui_aspect(void) { return aspects[cur_aspect].ar; }

static void apply_display(void) {
    if (!g_win) return;
    if (win_mode == 0) {
        SDL_SetWindowFullscreen(g_win, 0);
        SDL_SetWindowSize(g_win, modes[cur_mode].w, modes[cur_mode].h);
        SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else if (win_mode == 1) {
        SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_DisplayMode dm = { 0, modes[cur_mode].w, modes[cur_mode].h, 0, 0 };
        SDL_SetWindowDisplayMode(g_win, &dm);
        SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN);
    }
}

/* ---- lifecycle ---------------------------------------------------------- */
void ui_init(SDL_Window *win) {
    g_win = win;
    ctx = nk_sdl_init(win);
    struct nk_font_atlas *atlas;
    nk_sdl_font_stash_begin(&atlas);
    nk_sdl_font_stash_end();
    ui_controls_defaults();
    ui_controls_load();          /* a saved remap wins over the defaults */
    /* PROPCYCL_MENU_OPEN=1: start with the menu up, so a headless
     * screenshot can verify it actually renders. */
    { const char *e = getenv("PROPCYCL_MENU_OPEN"); if (e && *e != '0') menu_open = true; }
    /* PROPCYCL_MAP=<course>: open the map viewer straight away, so it can be
     * verified headlessly. */
    { const char *e = getenv("PROPCYCL_MAP");
      if (e) { map_course = atoi(e) & 3; map_on = true; } }
}

void ui_shutdown(void) { if (ctx) nk_sdl_shutdown(); ctx = NULL; }
bool ui_is_open(void) { return menu_open; }
void ui_toggle(void) {
    menu_open = !menu_open;
    if (!menu_open) rebinding = -1;
    SDL_SetRelativeMouseMode(SDL_FALSE);
}

void ui_input_begin(void) { if (ctx) nk_input_begin(ctx); }
void ui_input_end(void)   { if (ctx) nk_input_end(ctx); }

bool ui_handle_event(SDL_Event *e) {
    if (!ctx) return false;
    /* Capture a key for rebinding before Nuklear sees it. */
    if (menu_open && rebinding >= 0 && e->type == SDL_KEYDOWN) {
        if (e->key.keysym.scancode != SDL_SCANCODE_ESCAPE)
            ui_binding[rebinding] = e->key.keysym.scancode;
        rebinding = -1;
        return true;
    }
    if (!menu_open) return false;
    nk_sdl_handle_event(e);
    return true;                 /* menu open: the game does not see input */
}

/* The object picker's state, and the renderer side of it. */
static bool objects_open = false;
static int  pinned_code  = -1;
int  render_objlist_count(void);
void render_objlist_get(int i, int *code, int *quads);
void render_pick_set(int code);

/* ---- Billboards (2D sprite billboards -- particle effects etc, register
 * row 134/137/138) and Banners (text-tilemap blocks, register row 62) --
 * same picker UX as Objects, built on src/game_core.c / src/game_hud.c's
 * per-frame call lists and src/renderer_3d.c's screen-space highlight. */
static bool billboards_open = false;
static int  pinned_billboard = -1;   /* index, not a stable id -- see note below */
int  render_billboard_list_count(void);
void render_billboard_list_get(int i, int *tile, int *x0, int *y0, int *w, int *h);
void render_billboard_pick_set(int idx);

static bool banners_open = false;
static int  pinned_banner = -1;
int  banner_calls_count(void);
void banner_calls_get(int i, int *col, int *row, int *w, int *h, int *base, int *pal);
void render_banner_pick_set(int idx);

static bool restart_req = false;
bool ui_restart_requested(void) { return restart_req; }

void ui_draw(SDL_Window *win, bool *quit) {
    if (!ctx || !menu_open) return;
    int ww, wh;
    SDL_GetWindowSize(win, &ww, &wh);

    if (nk_begin(ctx, "menubar", nk_rect(0, 0, (float)ww, 28),
                 NK_WINDOW_NO_SCROLLBAR)) {
        nk_menubar_begin(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 20, 10);

        /* ---- File ---- */
        nk_layout_row_push(ctx, 50);
        if (nk_menu_begin_label(ctx, "File", NK_TEXT_LEFT, nk_vec2(200, 90))) {
            nk_layout_row_dynamic(ctx, 28, 1);
            /* Restart = power-cycle the cabinet: main() re-launches the
             * program with the same arguments. Scores and settings are
             * already saved to disk as they change. */
            if (nk_button_label(ctx, "Restart")) { restart_req = true; *quit = true; }
            if (nk_button_label(ctx, "Exit")) *quit = true;
            nk_menu_end(ctx);
        }

        /* ---- Display ---- */
        nk_layout_row_push(ctx, 90);
        if (nk_menu_begin_label(ctx, "Display", NK_TEXT_LEFT, nk_vec2(240, 320))) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Window mode", NK_TEXT_LEFT);
            int prev_wm = win_mode;
            if (nk_option_label(ctx, "Windowed", win_mode == 0)) win_mode = 0;
            if (nk_option_label(ctx, "Fullscreen (desktop)", win_mode == 1)) win_mode = 1;
            if (nk_option_label(ctx, "Fullscreen (exclusive)", win_mode == 2)) win_mode = 2;
            nk_label(ctx, "Resolution", NK_TEXT_LEFT);
            int prev_mode = cur_mode;
            for (int i = 0; i < NMODES; i++)
                if (nk_option_label(ctx, modes[i].label, cur_mode == i)) cur_mode = i;
            if (win_mode != prev_wm || cur_mode != prev_mode) apply_display();
            nk_label(ctx, "Aspect ratio", NK_TEXT_LEFT);
            for (int i = 0; i < NASPECT; i++)
                if (nk_option_label(ctx, aspects[i].label, cur_aspect == i)) cur_aspect = i;
            if (nk_button_label(ctx, "Apply")) apply_display();
            nk_menu_end(ctx);
        }

        /* ---- Audio ---- */
        nk_layout_row_push(ctx, 80);
        if (nk_menu_begin_label(ctx, "Audio", NK_TEXT_LEFT, nk_vec2(260, 200))) {
            nk_layout_row_dynamic(ctx, 20, 1);
            {
                int vol = (int)(audio_hle_volume() * 100.0f + 0.5f);
                char b[48];
                snprintf(b, sizeof b, "Volume  %d%%", vol);
                nk_label(ctx, b, NK_TEXT_LEFT);
                nk_layout_row_dynamic(ctx, 22, 1);
                if (nk_slider_int(ctx, 0, &vol, 100, 1))
                    audio_hle_set_volume(vol / 100.0f);
                nk_layout_row_dynamic(ctx, 20, 4);
                if (nk_button_label(ctx, "Mute"))  audio_hle_set_volume(0.0f);
                if (nk_button_label(ctx, "25%"))  audio_hle_set_volume(0.25f);
                if (nk_button_label(ctx, "50%"))  audio_hle_set_volume(0.50f);
                if (nk_button_label(ctx, "100%")) audio_hle_set_volume(1.00f);
                nk_layout_row_dynamic(ctx, 22, 1);
                if (nk_button_label(ctx, "Save")) ui_controls_save();
            }
            nk_menu_end(ctx);
        }

        /* ---- Levels ----
         * Start any stage directly (src/level_select.c). ADVANCED is the story
         * mode: day N puts the story state where the machine has it after the
         * N-1 stages before it, so the intermission that follows is that day's
         * one. The request walks the game's own chain -- DAY screen, STAGE
         * SELECT, intro orbit -- and presses START at the stage select. */
        nk_layout_row_push(ctx, 80);
        if (nk_menu_begin_label(ctx, "Levels", NK_TEXT_LEFT, nk_vec2(300, 450))) {
            extern void level_select_request(int adv, int course);
            extern const char *level_select_name(int adv, int course);
            static const char *adv_lbl[4] = {
                "Day 1  Level 1 CLIFF ROCK", "Day 2  Level 2 WIND WOODS",
                "Day 3  Level 3 INDUSTARN",  "Day 4  FINAL STAGE" };
            static const char *nov_lbl[3] = {
                "Level 1 CLIFF ROCK", "Level 2 WIND WOODS", "Level 3 INDUSTARN" };
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label(ctx, "ADVANCED (story mode)", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 22, 1);
            for (int c = 0; c < 4; c++)
                if (nk_button_label(ctx, adv_lbl[c])) {
                    level_select_request(1, c); menu_open = false; }
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label(ctx, "NOVICE (point attack)", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 22, 1);
            for (int c = 0; c < 3; c++)
                if (nk_button_label(ctx, nov_lbl[c])) {
                    level_select_request(0, c); menu_open = false; }
            /* The story ENDING, straight in (level_select_ending): the good
             * one (final stage cleared), the bad one (time up), or any of
             * the 13 phases of the cut scene / credits, to fix and compare
             * against MAME's captures phase by phase. */
            {   extern void level_select_ending(int cleared, int phase, int32_t total);
                static int end_phase = 0;
                static const char *ph_lbl[13] = {
                    "0 fly-in", "1", "2", "3 staff roll", "4", "5", "6",
                    "7 landing", "8 portrait", "9 BAD ending", "10", "11 re-enter", "12 END card" };
                nk_layout_row_dynamic(ctx, 18, 1);
                nk_label(ctx, "ENDING (cut scene + credits)", NK_TEXT_LEFT);
                nk_layout_row_dynamic(ctx, 22, 2);
                if (nk_button_label(ctx, "Cleared")) { level_select_ending(1, -1, 8995); menu_open = false; }
                if (nk_button_label(ctx, "Bad (time up)")) { level_select_ending(0, -1, 8995); menu_open = false; }
                nk_layout_row_dynamic(ctx, 22, 2);
                end_phase = nk_combo(ctx, ph_lbl, 13, end_phase, 18, nk_vec2(160, 260));
                if (nk_button_label(ctx, "Go to phase")) {
                    level_select_ending(end_phase != 9, end_phase, 8995); menu_open = false; }
            }
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label(ctx, "Keys: 1-3 NOVICE, 4 final stage.", NK_TEXT_LEFT);
            nk_menu_end(ctx);
        }

        /* ---- Maps ---- */
        nk_layout_row_push(ctx, 80);
        if (nk_menu_begin_label(ctx, "Maps", NK_TEXT_LEFT, nk_vec2(240, 260))) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Free-look course map", NK_TEXT_LEFT);
            for (int c = 0; c < 4; c++) {
                char b[32]; snprintf(b, sizeof b, "Course %d", c);
                if (nk_button_label(ctx, b)) {
                    map_course = c; map_on = true; menu_open = false;
                    map_yaw = 0.0f; map_pitch = 1.05f; map_zoom = 1.0f;
                    map_cx = map_cy = map_cz = 0.0f;
                }
            }
            if (nk_button_label(ctx, "Exit map view")) map_on = false;
            nk_label(ctx, "MMB/LMB drag = orbit", NK_TEXT_LEFT);
            nk_label(ctx, "Shift+MMB / RMB = pan", NK_TEXT_LEFT);
            nk_label(ctx, "wheel = zoom", NK_TEXT_LEFT);
            nk_label(ctx, "WASD/QE = fly, Shift fast", NK_TEXT_LEFT);
            nk_label(ctx, "Ctrl = slow", NK_TEXT_LEFT);
            nk_menu_end(ctx);
        }

        /* ---- Controls ---- */
        nk_layout_row_push(ctx, 90);
        if (nk_menu_begin_label(ctx, "Controls", NK_TEXT_LEFT, nk_vec2(300, 360))) {
            nk_layout_row_dynamic(ctx, 20, 2);
            for (int i = 0; i < ACT_COUNT; i++) {
                nk_label(ctx, act_names[i], NK_TEXT_LEFT);
                const char *k = (rebinding == i) ? "press a key..."
                                : SDL_GetScancodeName(ui_binding[i]);
                if (nk_button_label(ctx, (k && *k) ? k : "(none)")) rebinding = i;
            }
            nk_layout_row_dynamic(ctx, 22, 2);
            if (nk_button_label(ctx, "Save")) ui_controls_save();
            if (nk_button_label(ctx, "Defaults")) ui_controls_defaults();
            nk_menu_end(ctx);
        }

        /* ---- Objects ---- */
        nk_layout_row_push(ctx, 90);
        if (nk_menu_begin_label(ctx, "Objects", NK_TEXT_LEFT, nk_vec2(220, 110))) {
            nk_layout_row_dynamic(ctx, 22, 1);
            if (nk_button_label(ctx, objects_open ? "Hide object list"
                                                  : "Show object list"))
                objects_open = !objects_open;
            nk_label(ctx, "Hover a row to highlight", NK_TEXT_LEFT);
            nk_label(ctx, "it on screen; click to pin.", NK_TEXT_LEFT);
            nk_menu_end(ctx);
        }

        /* ---- Billboards ---- */
        nk_layout_row_push(ctx, 100);
        if (nk_menu_begin_label(ctx, "Billboards", NK_TEXT_LEFT, nk_vec2(240, 110))) {
            nk_layout_row_dynamic(ctx, 22, 1);
            if (nk_button_label(ctx, billboards_open ? "Hide billboard list"
                                                      : "Show billboard list"))
                billboards_open = !billboards_open;
            nk_label(ctx, "2D sprite billboards (particle", NK_TEXT_LEFT);
            nk_label(ctx, "effects etc), not full 3D objects.", NK_TEXT_LEFT);
            nk_menu_end(ctx);
        }

        /* ---- Record ----
         * A player is the only thing that flies a real route, and the two
         * defects this tree has lost the most time to (rows 90 and 129)
         * both survived every headless gate because nothing automated ever
         * went where a human goes. This writes the flight to a file the
         * offline tools replay. */
        nk_layout_row_push(ctx, 90);
        if (nk_menu_begin_label(ctx, "Record", NK_TEXT_LEFT, nk_vec2(300, 160))) {
            extern int flight_rec_active(void);
            extern int flight_rec_frames(void);
            extern int flight_rec_contacts(void);
            extern int flight_rec_resets(void);
            extern const char *flight_rec_path(void);
            extern void flight_rec_start(void);
            extern void flight_rec_stop(void);
            int on = flight_rec_active();
            nk_layout_row_dynamic(ctx, 22, 1);
            if (nk_button_label(ctx, on ? "Stop recording" : "Start recording")) {
                if (on) flight_rec_stop(); else flight_rec_start();
            }
            nk_layout_row_dynamic(ctx, 18, 1);
            if (on) {
                char b[256];
                snprintf(b, sizeof b, "REC  %d frames, %d contact, %d resets",
                         flight_rec_frames(), flight_rec_contacts(),
                         flight_rec_resets());
                nk_label(ctx, b, NK_TEXT_LEFT);
                const char *p = flight_rec_path();
                nk_label(ctx, p ? p : "", NK_TEXT_LEFT);
                nk_label(ctx, "Fly the route, then Stop.", NK_TEXT_LEFT);
            } else {
                nk_label(ctx, "Records position, attitude, stick", NK_TEXT_LEFT);
                nk_label(ctx, "and the COLLISION COLUMN per frame.", NK_TEXT_LEFT);
                nk_label(ctx, "A frame with contact=1 is a wall.", NK_TEXT_LEFT);
                nk_label(ctx, "Key: F9 toggles without the menu.", NK_TEXT_LEFT);
            }
            nk_menu_end(ctx);
        }

        /* ---- Banners ---- */
        nk_layout_row_push(ctx, 90);
        if (nk_menu_begin_label(ctx, "Banners", NK_TEXT_LEFT, nk_vec2(240, 110))) {
            nk_layout_row_dynamic(ctx, 22, 1);
            if (nk_button_label(ctx, banners_open ? "Hide banner list"
                                                   : "Show banner list"))
                banners_open = !banners_open;
            nk_label(ctx, "Text-tilemap blocks (title/tutorial/", NK_TEXT_LEFT);
            nk_label(ctx, "results text) -- spot bad tile/palette.", NK_TEXT_LEFT);
            nk_menu_end(ctx);
        }

        nk_layout_row_end(ctx);
        nk_menubar_end(ctx);
    }
    nk_end(ctx);

    /* ---- THE OBJECT PICKER -------------------------------------------------
     * Every object drawn this frame, scrollable, with its quad count. Hovering
     * a row paints that object magenta in the scene behind this panel; clicking
     * pins it so the mouse can be moved away.
     *
     * It exists because identifying an object by its code is otherwise a
     * guessing game: a user reporting "the water disappears" cost a whole
     * session of chasing the wrong code (395-402, the sea-level sheet, which
     * the watch later proved was drawing normally the whole time). Pointing at
     * the thing is not a nicety, it is the shortest path to the right object.
     *
     * Pause first (P), THEN open the menu -- while the menu is up the game does
     * not see keys, and a frozen display list makes the list stable to read. */
    if (objects_open) {
        int n = render_objlist_count();
        if (nk_begin(ctx, "Objects on screen", nk_rect(8, 34, 240, 420),
                     NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                     NK_WINDOW_CLOSABLE | NK_WINDOW_BORDER)) {
            int hovered = -1;
            char buf[64];
            nk_layout_row_dynamic(ctx, 20, 1);
            snprintf(buf, sizeof buf, "%d objects drawn", n);
            nk_label(ctx, buf, NK_TEXT_LEFT);
            if (nk_button_label(ctx, "Clear highlight")) pinned_code = -1;

            nk_layout_row_dynamic(ctx, 18, 1);
            for (int i = 0; i < n; i++) {
                int code = -1, quads = 0;
                render_objlist_get(i, &code, &quads);
                snprintf(buf, sizeof buf, "%-5d  %3d quad%s", code, quads,
                         quads == 1 ? "" : "s");
                struct nk_rect b = nk_widget_bounds(ctx);
                int sel = (code == pinned_code);
                if (nk_selectable_label(ctx, buf, NK_TEXT_LEFT, &sel))
                    pinned_code = sel ? code : -1;
                if (nk_input_is_mouse_hovering_rect(&ctx->input, b)) hovered = code;
            }
            render_pick_set(hovered >= 0 ? hovered : pinned_code);
        } else {
            objects_open = false;           /* the window's close box */
            render_pick_set(-1);
        }
        nk_end(ctx);
    } else if (pinned_code >= 0) {
        render_pick_set(-1);
        pinned_code = -1;
    }

    /* ---- THE BILLBOARD PICKER -----------------------------------------
     * Same UX as Objects, but for the 2D sprite billboards (particle
     * effects like the water-contact spray, register row 134/137/138)
     * that never appear in the Objects list -- those go through the
     * SPRITE layer (sprite_draw_2d), not the 3D geometry stage. Indexed
     * by POSITION in this frame's list (not a stable code the way object
     * codes are), which is fine because the list is only meant to be read
     * while paused (P), when the display list -- and so this list -- is
     * frozen and stable, exactly like the Objects picker's own contract. */
    if (billboards_open) {
        int n = render_billboard_list_count();
        if (nk_begin(ctx, "Billboards on screen", nk_rect(8, 34, 260, 420),
                     NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                     NK_WINDOW_CLOSABLE | NK_WINDOW_BORDER)) {
            int hovered = -1;
            char buf[64];
            nk_layout_row_dynamic(ctx, 20, 1);
            snprintf(buf, sizeof buf, "%d billboards drawn", n);
            nk_label(ctx, buf, NK_TEXT_LEFT);
            if (n == 0) nk_label(ctx, "(pause first -- P -- for a", NK_TEXT_LEFT);
            if (n == 0) nk_label(ctx, "stable, readable list)", NK_TEXT_LEFT);
            if (nk_button_label(ctx, "Clear highlight")) pinned_billboard = -1;

            nk_layout_row_dynamic(ctx, 18, 1);
            for (int i = 0; i < n; i++) {
                int tile, x0, y0, w, h;
                render_billboard_list_get(i, &tile, &x0, &y0, &w, &h);
                snprintf(buf, sizeof buf, "tile %-5d  (%d,%d) %dx%d", tile, x0, y0, w, h);
                struct nk_rect b = nk_widget_bounds(ctx);
                int sel = (i == pinned_billboard);
                if (nk_selectable_label(ctx, buf, NK_TEXT_LEFT, &sel))
                    pinned_billboard = sel ? i : -1;
                if (nk_input_is_mouse_hovering_rect(&ctx->input, b)) hovered = i;
            }
            render_billboard_pick_set(hovered >= 0 ? hovered : pinned_billboard);
        } else {
            billboards_open = false;
            render_billboard_pick_set(-1);
        }
        nk_end(ctx);
    } else if (pinned_billboard >= 0) {
        render_billboard_pick_set(-1);
        pinned_billboard = -1;
    }

    /* ---- THE BANNER PICKER ---------------------------------------------
     * Every text_draw_rect_blink call this frame (title/tutorial/results
     * text, register row 62 -- 58 of these lost their base-code/palette
     * arguments to decompilation and drew tile 0 for a long time; this is
     * the tool to spot any that STILL do, or that show z-fighting against
     * an overlapping banner at the same tilemap cells). Same
     * index-while-paused contract as Billboards above. */
    if (banners_open) {
        int n = banner_calls_count();
        if (nk_begin(ctx, "Banners on screen", nk_rect(8, 34, 280, 420),
                     NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                     NK_WINDOW_CLOSABLE | NK_WINDOW_BORDER)) {
            int hovered = -1;
            char buf[80];
            nk_layout_row_dynamic(ctx, 20, 1);
            snprintf(buf, sizeof buf, "%d banner draws this frame", n);
            nk_label(ctx, buf, NK_TEXT_LEFT);
            if (n == 0) nk_label(ctx, "(pause first -- P -- for a", NK_TEXT_LEFT);
            if (n == 0) nk_label(ctx, "stable, readable list)", NK_TEXT_LEFT);
            if (nk_button_label(ctx, "Clear highlight")) pinned_banner = -1;

            nk_layout_row_dynamic(ctx, 18, 1);
            for (int i = 0; i < n; i++) {
                int col, row, w, h, base, pal;
                banner_calls_get(i, &col, &row, &w, &h, &base, &pal);
                snprintf(buf, sizeof buf, "c%-2d r%-2d %2dx%-2d base 0x%-4x pal %d",
                         col, row, w, h, base, pal);
                struct nk_rect b = nk_widget_bounds(ctx);
                int sel = (i == pinned_banner);
                if (nk_selectable_label(ctx, buf, NK_TEXT_LEFT, &sel))
                    pinned_banner = sel ? i : -1;
                if (nk_input_is_mouse_hovering_rect(&ctx->input, b)) hovered = i;
            }
            render_banner_pick_set(hovered >= 0 ? hovered : pinned_banner);
        } else {
            banners_open = false;
            render_banner_pick_set(-1);
        }
        nk_end(ctx);
    } else if (pinned_banner >= 0) {
        render_banner_pick_set(-1);
        pinned_banner = -1;
    }

    nk_sdl_render(NK_ANTI_ALIASING_ON);
}
