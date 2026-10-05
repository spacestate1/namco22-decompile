# Windows notes

The Windows programs are built from Linux with `./build-windows.sh` (MinGW-w64). They can also be built on Windows
itself in an MSYS2 *MINGW64* shell: install `mingw-w64-x86_64-toolchain`, `mingw-w64-x86_64-cmake`,
`mingw-w64-x86_64-pkgconf`, `mingw-w64-x86_64-SDL2`, `mingw-w64-x86_64-zlib`, `mingw-w64-x86_64-python` and
`mingw-w64-x86_64-python-numpy`. SDL2, zlib and the C runtime are linked into the `.exe`, so it needs no DLL beside it
(zlib's static library is picked on its own with CMake 3.24 or newer). Either way, a game's ROM files must be unpacked in
its `extracted/` folder before building: its sound and DSP programs are translated from them at build time.

`include/win_compat.h` maps the few POSIX calls the code uses onto Windows.

## Why Windows needed its own performance work

The same C code ran at 60 fps on Linux and stuttered or ran in slow motion on Windows. Two things differ:

- **The C library.** MinGW links Microsoft's C runtime. Its `qsort` swaps elements one byte at a time, which glibc
  does not, so sorting large structs is many times slower.
- **The OpenGL driver.** NVIDIA's Windows driver (with its default *Threaded optimization*) runs OpenGL on a worker
  thread. Any call that returns a value (`glGet*`, `glGenTextures`) makes the game wait until that thread has caught up
  with everything sent before it. On Linux these waits were small enough not to show.

## What is Windows-only and what is shared

**Windows-only code** (compiled only on Windows, inside `#ifdef _WIN32`):

- The log file. A double-clicked program has no error stream, so everything the games print there (`[HOST]`,
  `[PACE]`, `[AUDIO]`) used to be lost from the log. `engine/win_startup.c`, `raverace/src/rr_win.c` and
  `src/main.c` now reopen it before pointing it at the log.

**Shared code, changed for Windows** (built on both platforms; none of it changes what is drawn -- 60 Dirt Dash frames
rendered before and after are byte-for-byte identical. Not measured on Linux, where these costs were already small):

| Change | File | What it cost on Windows |
|---|---|---|
| The depth sort orders small keys, then moves each polygon once | `engine/quad_gl.c` | ~9 ms a frame in a Dirt Dash race |
| The viewport is read once per batch of polygons, not per polygon | `engine/quad_gl.c` | ~1 ms a frame |
| Texture names are made 512 at a time | `engine/tex_bake.c` | ~21 ms in the first frame of a new scene |
| The text layer is not re-sent when it has not changed | `engine/ss22_gl.c`, `raverace/src/rr_gl.c` | ~2 ms a frame |

Measured on a GeForce GTX 1660, 60 Hz, windowed, over a minute of each game's attract mode:

| Game | Before | After |
|---|---|---|
| Dirt Dash | ~38 fps in races (slow motion), sound pops | 60 fps, no sound pops |
| Tokyo Wars | 40 missed frames, 4 sound pops | 14 missed frames, no sound pops |
| Prop Cycle | 14 of 3480 frames missed | 3 of 3480 |
| Rave Racer | 6-22 missed frames per 600 | 0-16 per 600 (runs varied) |

What remains is mostly the first frame of a new scene, when several hundred textures are made at once.

## Measuring

The log beside the `.exe` has a `[PACE]` line every 600 frames (Prop Cycle: run with `PROPCYCL_EXIT_AT=3600` for an
`[FPS]` line). With Esc > Display > Frame rate set to a fixed number (or `ENG_FPSLOG=1` in the environment) every game also
writes `[FPS] ... pictures/s (... game frames, 59.91/s)`: what was shown and how fast the game ran. The game runs at 59.906 Hz
whatever the frame rate setting and the driver's vsync setting. `interval mean 16.7 ms` with few frames over 20 ms is a steady 60 fps. The `work` figure is not
reliable on NVIDIA: the driver queues frames, so the wait for vsync is counted in it.

## Testing without a window

The headless `--shots DIR N` mode (a picture every N frames, no window, as fast as the machine runs) works on Windows too:
SDL's `offscreen` driver makes OpenGL only through EGL, which Windows lacks, so there it draws into a hidden window instead.
With `--autoplay` (the game plays itself) or `--replay FILE` (a player's session: Tokyo Wars, Dirt Dash and Time Crisis
record every windowed session to `<game>_last.rec`) it re-runs a session exactly, for tracking down a crash. Time Crisis also takes `--stage 1|2|3` and the
`TC_INF_TIME=1` / `TC_INF_LIFE=1` switches (README), to reach any stage's later scenes.
