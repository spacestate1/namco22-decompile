/*
 * rig_viewer.c — standalone viewer for the bike + rider articulated model.
 *
 * WHY THIS EXISTS
 * ---------------
 * The rig is the hardest thing in the project to judge. Every automated gate
 * the repo owns is blind to a whole class of error in it:
 *
 *   - tools/attract_diff.py scores |t|, a POSITION metric. A 90 degree
 *     rotation does not move an object's centre.
 *   - tools/overnight/rig_orient_gate.py scores each part RELATIVE to the
 *     body anchor. A similarity transform cancels -- and so does any rotation
 *     shared by every part.
 *   - gameplay_rig_gate.py has the same anchor and the same blind spot.
 *
 * All three PASSED, two of them IMPROVED, on a build where the bike and rider
 * were visibly a mess (FAILED_APPROACHES.md 1.27). A human eye caught it in
 * one second. This gives the eye something to look at.
 *
 * WHAT IT DOES *NOT* DO
 * ---------------------
 * It does not reimplement the rig. That would defeat the purpose: a second
 * implementation can be right while the engine is wrong. It runs the ENGINE:
 *
 *   game_init() / game_frame()          the real game, so the rig is loaded
 *                                       from the ROM tables by
 *                                       player_model_load_animation and posed
 *                                       by the game's own animation code
 *   scene_node_render()                 the real 0x8008/0x8009/0x800a wire
 *                                       format
 *   renderer3d_render_frame()           the real display-list walk, slot-table
 *                                       composition and geo_hw projection
 *
 * and then only (a) drops every object that is not bike or rider, and
 * (b) magnifies what is left so it fills the window. Both live behind
 * g_rig_view in renderer_3d.c and are dead in propcycl itself.
 *
 * So if the viewer shows a mess, the engine draws a mess.
 *
 * USAGE
 *   ./build/rig_viewer [rom_dir]                     interactive
 *   ./build/rig_viewer [rom_dir] --shot out.ppm 700  headless capture
 *
 *   --phase flyover     jump to the attract flyover (frame ~150).  The rig
 *                       there is emitted through camera_update_main ->
 *                       scene_node_render.
 *   --phase demo        jump to the attract demo flight (frame ~700), whose
 *                       bike parts come through a different producer. The two
 *                       phases exercise DIFFERENT emitters, which is exactly
 *                       what is in question -- always check both.
 *   --frame N           jump straight to game frame N.
 *   --rot8008 N         start with that renderer pair order (see keys below).
 *   --snr-sinfirst 0|1  scene_node_render writes its pairs sin-first (ROM/MAME
 *                       order) instead of the shipped cos-first. Default 0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "propcycl.h"

/* main.c owns these in the game binary; we are its replacement here. */
int propcycl_verbose(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("PROPCYCL_VERBOSE"); v = (e && *e != '0'); }
    return v;
}

/* renderer_3d.c rig-viewer hooks */
extern int   g_rig_view, g_rig_only, g_rig_fit, g_rig_show_bike, g_rig_show_rider;
extern float g_rig_yaw, g_rig_pitch;
extern float g_rig_zoom, g_rig_panx, g_rig_pany;
extern int   g_rig_nquads;
extern int   g_rot8008, g_asm_mode, g_euler_8008, g_snr_sinfirst, g_root15;

static SDL_Window  *window;
static SDL_GLContext glctx;
static bool running = true, headless = false, paused = false;
static int  step_one = 0, phase = 1, turntable = 0, dragging = 0;
static float yaw_arg = 0, pitch_arg = 0;

static void on_quit(int sig) { (void)sig; running = false; }

static bool init_gl(void)
{
    if (headless) SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    Uint32 flags = SDL_WINDOW_OPENGL |
                   (headless ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    int scale = headless ? 1 : 2;
    window = SDL_CreateWindow("Prop Cycle - rig viewer",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              SCREEN_WIDTH * scale, SCREEN_HEIGHT * scale, flags);
    if (!window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
    glctx = SDL_GL_CreateContext(window);
    if (!glctx) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return false; }
    if (!headless) SDL_GL_SetSwapInterval(1);
    glViewport(0, 0, SCREEN_WIDTH * scale, SCREEN_HEIGHT * scale);
    return true;
}

static void save_ppm(const char *path)
{
    uint8_t *px = malloc((size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 3);
    if (!px) return;
    glReadPixels(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", SCREEN_WIDTH, SCREEN_HEIGHT);
        for (int y = SCREEN_HEIGHT - 1; y >= 0; y--)
            fwrite(px + (size_t)y * SCREEN_WIDTH * 3, 3, SCREEN_WIDTH, f);
        fclose(f);
        printf("wrote %s\n", path);
    }
    free(px);
}

static const char *ORDNAME[6] = { "ZXY", "XYZ", "ZYX", "XZY", "YZX", "YXZ" };

static void print_help(void)
{
    puts("");
    puts("  Prop Cycle rig viewer -- the bike and rider, through the engine's own pipeline");
    puts("  ---------------------------------------------------------------------------");
    puts("   SPACE  pause / resume            .  single-step one game frame");
    puts("   1      jump to attract FLYOVER   2  jump to attract DEMO FLIGHT");
    puts("          (these two use DIFFERENT rig emitters -- check both)");
    puts("   drag   ORBIT the rig (left mouse)   J/L yaw   I/K pitch   T auto-turntable");
    puts("   wheel  zoom                  +/-  zoom          arrows  pan");
    puts("   B      show/hide the bike       V  show/hide the rider");
    puts("          The flyover loops every 300 frames and the demo every ~550, so the");
    puts("          animation never sits in the logo phase (which has no rig at all).");
    puts("   F      toggle auto-framing       0  reset zoom/pan");
    puts("   A      toggle rig-only filter (A off = whole scene, for context)");
    puts("");
    puts("   [ ]    cycle g_rot8008 0/1/2  -- the 0x8008 sin/cos PAIR ORDER.");
    puts("          This is the open question (register row 95). If one setting");
    puts("          looks right and the others do not, that is the answer.");
    puts("   N      toggle scene_node_render pair order: 0 = shipped (cos-first),");
    puts("          1 = ROM/MAME order (sin-first). Pair with [ ] -- the demo needs");
    puts("          N=1 with rot8008=1; the flyover must stay right under the same.");
    puts("   R      toggle treating a 15-word 0x8008 straight into 0x800a as a ROOT");
    puts("          (the demo bike root's form; MAME's buffer proves it). Default on.");
    puts("   E      cycle the rig Euler order (g_euler_8008)");
    puts("   M      cycle assembly mode (5 = slot table, 0 = flat/no hierarchy)");
    puts("");
    puts("   P      print current state       S  save screenshot to rig_shot.ppm");
    puts("   H      this help                 Q / Esc  quit");
    puts("");
}

static void print_state(void)
{
    printf("  [state] frame=%u  quads=%d  rot8008=%d snr_sinfirst=%d root15=%d  euler_8008=%s  asm_mode=%d"
           "  zoom=%.2f pan=(%.0f,%.0f) fit=%d rig_only=%d\n",
           g_sys.frame_count, g_rig_nquads, g_rot8008, g_snr_sinfirst, g_root15,
           ORDNAME[g_euler_8008 % 6], g_asm_mode,
           g_rig_zoom, g_rig_panx, g_rig_pany, g_rig_fit, g_rig_only);
}

/* Run the game forward without drawing, to reach an attract phase. */
static void fast_forward_to(unsigned target)
{
    if (target <= g_sys.frame_count) return;
    printf("  advancing to frame %u ...\n", target);
    while (g_sys.frame_count < target && running) {
        g_sys.vblank_pending = true;
        input_poll();
        game_frame();
        g_sys.frame_count++;
    }
    print_state();
}

/* Re-run the game from power-on to a frame. Attract is deterministic, so this
 * is how the viewer LOOPS a phase: the flyover only exists for frames 1-300
 * and the logo phase after it has no rig at all, which read as "it freezes". */
static void restart_to(unsigned target)
{
    printf("  restarting game -> frame %u\n", target);
    game_init();
    g_sys.frame_count = 0;
    fast_forward_to(target);
}

int main(int argc, char **argv)
{
    struct sigaction sa = {0};
    sa.sa_handler = on_quit; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    const char *rom_dir  = "extracted";
    const char *shot     = NULL;
    unsigned    shot_at  = 700;
    unsigned    start_at = 1;                /* the flyover, from its first frame */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            headless = true; shot = argv[++i];
            if (i + 1 < argc && argv[i+1][0] != '-') shot_at = (unsigned)atoi(argv[++i]);
            start_at = shot_at;
        } else if (!strcmp(argv[i], "--phase") && i + 1 < argc) {
            const char *p = argv[++i];
            phase = (!strcmp(p, "demo")) ? 2 : 1;
            start_at = (phase == 2) ? 601 : 1;
        } else if (!strcmp(argv[i], "--frame") && i + 1 < argc) {
            start_at = (unsigned)atoi(argv[++i]); phase = 0;
        } else if (!strcmp(argv[i], "--rot8008") && i + 1 < argc) {
            g_rot8008 = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--snr-sinfirst") && i + 1 < argc) {
            g_snr_sinfirst = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--root15") && i + 1 < argc) {
            g_root15 = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) {
            g_rig_yaw = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) {
            g_rig_pitch = (float)atof(argv[++i]);
        } else if (argv[i][0] != '-') {
            rom_dir = argv[i];
        }
    }
    if (shot && shot_at > start_at) start_at = shot_at;

    /* The rig is the subject; everything else is noise. These are read once at
     * startup by the modules concerned (register row 40: getenv() from inside
     * the frame loop faults on a truncated environment pointer). */
    setenv("PROPCYCL_GEO_HW",      "1", 1);   /* the ported hardware geometry stage */
    setenv("PROPCYCL_NO_SPRITES",  "1", 1);
    setenv("PROPCYCL_NO_TEXT",     "1", 1);
    setenv("PROPCYCL_NO_2D",       "1", 1);
    setenv("PROPCYCL_NO_AUTOSTART","1", 1);   /* stay in attract */

    if (!rom_load_all(rom_dir)) {
        fprintf(stderr, "rig_viewer: cannot load ROMs from %s\n", rom_dir);
        return 1;
    }
    if (!init_gl()) return 1;

    input_init();
    renderer2d_init();
    renderer3d_init();
    renderer3d_load_palette(rom_dir);

    g_rig_view = 1;               /* arms the hooks in renderer_3d.c */

    game_init();
    if (!headless) print_help();
    fast_forward_to(start_at);

    while (running) {
        if (!headless) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = false;
                else if (ev.type == SDL_MOUSEWHEEL) {
                    g_rig_zoom *= (ev.wheel.y > 0) ? 1.12f : (1.0f / 1.12f);
                } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                    dragging = 1;
                } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
                    dragging = 0;
                } else if (ev.type == SDL_MOUSEMOTION && dragging) {
                    g_rig_yaw   += ev.motion.xrel * 0.5f;
                    g_rig_pitch += ev.motion.yrel * 0.5f;
                } else if (ev.type == SDL_KEYDOWN) {
                    switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE: case SDLK_q: running = false; break;
                    case SDLK_SPACE:  paused = !paused;
                                      printf("  %s\n", paused ? "paused" : "running"); break;
                    case SDLK_PERIOD: step_one = 1; break;
                    case SDLK_1:      phase = 1; restart_to(1);   break;
                    case SDLK_2:      phase = 2; restart_to(601); break;
                    case SDLK_j:      g_rig_yaw   -= 10; break;
                    case SDLK_l:      g_rig_yaw   += 10; break;
                    case SDLK_i:      g_rig_pitch -= 10; break;
                    case SDLK_k:      g_rig_pitch += 10; break;
                    case SDLK_t:      turntable = !turntable; break;
                    case SDLK_b:      g_rig_show_bike  = !g_rig_show_bike;  print_state(); break;
                    case SDLK_v:      g_rig_show_rider = !g_rig_show_rider; print_state(); break;
                    case SDLK_PLUS: case SDLK_EQUALS:  g_rig_zoom *= 1.12f; break;
                    case SDLK_MINUS: case SDLK_KP_MINUS: g_rig_zoom /= 1.12f; break;
                    case SDLK_LEFT:   g_rig_panx -= 16; break;
                    case SDLK_RIGHT:  g_rig_panx += 16; break;
                    case SDLK_UP:     g_rig_pany -= 16; break;
                    case SDLK_DOWN:   g_rig_pany += 16; break;
                    case SDLK_0:      g_rig_zoom = 1.0f; g_rig_yaw = g_rig_pitch = 0;
                                      g_rig_panx = g_rig_pany = 0; break;
                    case SDLK_f:      g_rig_fit  = !g_rig_fit;  print_state(); break;
                    case SDLK_a:      g_rig_only = !g_rig_only; print_state(); break;
                    case SDLK_LEFTBRACKET:
                        g_rot8008 = (g_rot8008 + 2) % 3; print_state(); break;
                    case SDLK_RIGHTBRACKET:
                        g_rot8008 = (g_rot8008 + 1) % 3; print_state(); break;
                    case SDLK_e:
                        g_euler_8008 = (g_euler_8008 + 1) % 6; print_state(); break;
                    case SDLK_n:
                        g_snr_sinfirst = !g_snr_sinfirst; print_state(); break;
                    case SDLK_r:
                        g_root15 = !g_root15; print_state(); break;
                    case SDLK_m:
                        g_asm_mode = (g_asm_mode == 5) ? 0 : 5; print_state(); break;
                    case SDLK_p:      print_state(); break;
                    case SDLK_h:      print_help();  break;
                    case SDLK_s:      save_ppm("rig_shot.ppm"); break;
                    default: break;
                    }
                }
            }
        }

        if (turntable && !paused) g_rig_yaw += 1.0f;
        /* Loop the phase so the animation never runs into the rig-less logo
         * phase (f302-600) or the late-demo divergence. */
        if (!headless && !paused) {
            if (phase == 1 && g_sys.frame_count >= 300)  restart_to(1);
            if (phase == 2 && g_sys.frame_count >= 1150) restart_to(601);
        }
        if (!paused || step_one) {
            /* The two things main.c does per frame before game_frame().
             * vblank_pending is what lets irq_vblank() run, and irq_vblank is
             * what initialises the display-list cursor W[0x0CA4]; without it
             * game_frame() runs but emits nothing at all (both command buffers
             * stay all-zero and the viewer draws 0 quads). */
            g_sys.vblank_pending = true;
            input_poll();
            game_frame();
            g_sys.frame_count++;
            step_one = 0;
        }

        glClearColor(0.16f, 0.17f, 0.20f, 1.0f);   /* neutral, so the model reads */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer3d_render_frame();

        /* An empty window is ambiguous -- "the tool is broken" and "this phase
         * has no rig in it" look identical. Attract runs flyover f1-300, LOGO
         * f302-600 (sky + a sprite logo, no bike at all), demo f601+. Say which
         * it is rather than letting the user guess. */
        {
            static int quiet = 0;
            if (g_rig_nquads == 0) {
                if (quiet == 0 || quiet % 180 == 0)
                    printf("  [no rig at frame %u] this attract phase emits no bike/rider"
                           " -- press 1 for the flyover or 2 for the demo flight"
                           " (A shows the whole scene)\n", g_sys.frame_count);
                quiet++;
            } else {
                quiet = 0;
            }
        }

        if (headless) {
            if (g_sys.frame_count >= shot_at) {
                print_state();
                if (shot) save_ppm(shot);
                break;
            }
        } else {
            SDL_GL_SwapWindow(window);
        }
    }

    if (glctx) SDL_GL_DeleteContext(glctx);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
