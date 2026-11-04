# macOS notes

Time Crisis and Time Crisis II build and run natively on Apple silicon Macs: the lifted 68EC020, MIPS and H8 programs compile to
arm64 with Apple clang, and every picture is drawn with Metal. There is no OpenGL context, no MoltenVK, no Rosetta and no software
frame upload. The other games have not been tried on the Mac.

## Building

Needs macOS 13 or newer, the Xcode command line tools (`xcode-select --install`; full Xcode is not needed -- the Metal shaders are
compiled at run time), Homebrew's `sdl2` (sdl2-compat) and `pkg-config`, and Python 3.

```bash
python3 macos/prepare_roms.py --tc1 <folder with the Time Crisis ROMs> --tc2 <folder with the Time Crisis II ROMs>
./macos/build.sh            # both games, into macos/output/build (./macos/build.sh tc1 or tc2 for one)
./macos/package.sh          # .app bundles into macos/output
./macos/install.sh          # or: copy the ROMs to ~/Library/Application Support/<game>/extracted and install to /Applications
```

`prepare_roms.py` only reads the folders it is given (loose chips or MAME zips), checks every chip's size and CRC32 against the
upstream setup tables, copies the listed chips into the git-ignored `extracted/` folders and builds the program images the TC2 build
translates (`tc2/boot.bin`, `sub.bin`, `io.bin`). `install.sh` refuses to replace an installed app: move the old one aside first.

`package.sh` copies Homebrew's sdl2-compat and the SDL3 it loads into each app (`Contents/Frameworks`), so the apps run on a Mac
without Homebrew; package outside an iCloud-synced folder, where codesign refuses the bundle. The apps are signed ad hoc only: on
another Mac the first start needs right-click > Open.

## How it works

`macos/glshim` puts a GL-shaped surface over Metal, so the engine's renderers run unchanged:

- `glmc_main.c` keeps the GL state the games use (fixed function, client arrays and VBOs, textures, framebuffers, the TC2 program
  uniforms) and turns each draw into a Metal draw request; `glmc_metal.mm` owns the device, the pipelines, the render passes and the
  window's `CAMetalLayer`.
- `glmc_msl.h` holds hand-ported Metal versions of the shaders: the fixed-function path TC1 uses, and TC2's polygon (plain and
  exact-depth), text and gamma programs. A GLSL program is matched to its Metal version by its source.
- Pixels are stored y-up as GL keeps them and every vertex stage negates NDC y; the window blit samples its source flipped. The
  orientation, ownership and upload rules are covered by the offscreen suite in `macos/tests` (78 pixel-asserting checks, and every
  shader unit compiled on its own; no window or ROMs needed: `./macos/tests/run.sh`).
- `launcher.c` is the bundle's executable (a compiled program, not a script, so macOS Game Mode recognises the game): it finds the
  ROMs, runs the game from the folder holding them -- settings, saves and high scores are written beside them -- logs to
  `~/Library/Logs/<game>/last-run.log`, and then execs the game.

## What is macOS-only and what is shared

**macOS-only** (`macos/`, and code inside `#ifdef GLMC_METAL` or `#ifdef __APPLE__`):

- The Metal shim, the launcher, the build, package and install scripts, the ROM importer and the tests.
- **High refresh rates.** Fullscreen on a display the frame rate does not divide (a 144 Hz screen with Frame rate Auto = 120), the
  screen is switched to a rate it does divide while the game runs, for this app only (macOS restores it at quit); the pacer follows
  the new rate (`g_eng_disp_refresh`, `eng_vsync_want`). `window_fill=1` in a game's cfg makes the window fill the desktop (menu bar
  and Dock visible), treated like fullscreen for this. Off by default.
- **The pointer in play.** SDL hides it through the window's cursor rect, which left the arrow over a light-gun game's crosshair;
  the system's own hide is used as well while the pointer is over the focused window (`glmc_pointer_hide`).
- **File > Restart.** `/proc/self/exe` does not exist on macOS: the program re-executes its own path (`_NSGetExecutablePath`).
  Time Crisis II now restarts too (it quit).

**Shared code, changed for macOS** (no change on Linux or Windows):

| Change | File |
|---|---|
| `ENG_WINDOW_DISPLAY=n` opens the window on monitor n (tests on a second screen) | `engine/ss22_host.c`, `tc2/src/tc2_window.c` |
| Headless runs keep the real video driver on Apple (the Metal shim has no offscreen driver) | `engine/ss22_gl.c` |
| The TC2 GPU gate's hidden context is the Metal-backed one | `tc2/src/tc2_main.c` |

## Measured

Apple M5 Pro, macOS 27, a 120 Hz-capable 1080p monitor, installed apps:

| Game | Pictures/s at 120 Hz | Game speed |
|---|---|---|
| Time Crisis | 116-117 (each 59.906 Hz frame shown twice) | 59.91 frames/s |
| Time Crisis II (Motion smoothing on) | 109-118 | 59.91 frames/s |

Time Crisis II's Metal picture against its software renderer: mean difference 0.085/255 at frame 4000.

## Known limits

- Apple silicon only; an Intel Mac has not been tried.
- The first start of a new build pauses once (0.3-1 s) while macOS builds its shader cache.
- Time Crisis II's H8 programs are translated from recorded coverage (`tc2/cov`), as on the other platforms.
- Mouse aiming and sound were checked by a player on one Mac only.
