# Time Crisis 2 for PC

Namco's 1997 arcade light-gun shooter **Time Crisis II** (System 23), rebuilt to run natively on a PC: the game's own programs
-- the MIPS main CPU, the H8 sound / sub CPU and the gun I/O board's H8 -- are translated to C, and the hardware around them
(the 3D chips, the text layer, the C352 sound chip, the JVS link, the light gun) is our own code. Nothing interprets ROM code;
MAME was used only as a reference to check the translation against, instruction for instruction.

**No game data is included.** You need your own copy of MAME's ROM set `timecrs2.zip` (Time Crisis II, US TSS3 Ver. B).

## Download

| Package | For |
|---|---|
| `TimeCrisis2-x86_64.AppImage` | Linux (64-bit) and the **Steam Deck** -- `chmod +x` it and run it |
| `TimeCrisis2-windows-VERSION.zip` | Windows 10 / 11 (64-bit) -- unzip, put `timecrs2.zip` in `roms`, double-click `TimeCrisis2.exe` |

Keys, light guns, recoil and where the files go: [HOW TO PLAY](packaging/linux/HOW-TO-PLAY.txt).

## Building from source (Linux)

```bash
./build.sh /path/to/timecrs2.zip     # unpacks and checks the ROM, translates the two H8 programs, builds
./launch.sh                          # play (./launch.sh 2 = a 2x window, --fullscreen)
```
Needs gcc, cmake, pkg-config, python3 and the SDL2 / zlib / OpenGL development files; the game program is ~100 MB of generated C
(`gen/tc2_lifted_*.c`), so allow about 1 GB of memory per parallel compile (`JOBS=2 ./build.sh ...`).
Windows: `./build-windows.sh` (MinGW-w64 cross compiler) after `./build.sh`. Releases: `tools/release.sh VERSION`.

## What works

Attract, coins, play with the light gun / mouse / pad (stages 1 and 2 checked so far), sound, the operator's test menu, the battery
RAM (settings and rankings) saved between sessions, widescreen, any internal resolution, frame-rate options, gun recoil output.
Not yet: the two-cabinet link (the 2-player linked game needs a second machine).

## Beyond MAME

Things this version does that MAME does not: the C404 chip's gamma table is applied (MAME does not use it), a GPU renderer at any
resolution and in widescreen, recoil output on the game's own solenoid signal.
