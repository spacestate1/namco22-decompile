# Angler King for PC

Namco's 1998 arcade fishing game **Angler King** (Super System 23), rebuilt to run natively on a PC on the same System 23 host as
Time Crisis 2 (`../tc2`) and Crisis Zone (`../crszone`): the game's own programs -- the MIPS main CPU, the H8 sub CPU and the I/O
board's H8 -- are translated to C, and the hardware around them is our own code. Nothing interprets ROM code; MAME was used only as a
reference.

**No game data is included.** You need your own copy of MAME's ROM set `aking.zip` (Angler King, Japan, AG1 Ver. A). It carries
the I/O board program as `asc3_io-c1.ic14`; no other zip is needed.

## Download

Angler King is in the combined release (0.10.0 and later) on the [Releases page](https://github.com/spacestate1/namco22-decompile/releases):

| Package | For |
|---|---|
| `AnglerKing-x86_64.AppImage` | Linux (64-bit) and the **Steam Deck** -- `chmod +x` it and run it |
| `namco-2x-systems_VERSION_amd64.deb` | Ubuntu / Debian / Mint: the `anglerking` command and a menu entry |
| `windows-release-VERSION.zip` | Windows 10 / 11 (64-bit) -- unzip, put `aking.zip` in `roms`, double-click `AnglerKing.exe` |

The first start unpacks `aking.zip` and checks every chip against MAME's checksums ([ROM_CHECKSUMS.md](../docs/ROM_CHECKSUMS.md)).

## Building from source (Linux)

```bash
./build.sh /path/to/aking.zip        # unpacks and checks the ROM, translates the two H8 programs, builds
./launch.sh                          # play (./launch.sh 2 = a 2x window, --fullscreen)
```
The game program is ~48 MB of generated C (`gen/aking_lifted_*.c`): allow about 1 GB of memory per parallel compile
(`JOBS=2 ./build.sh ...`). Windows: `./build-windows.sh` (MinGW-w64 cross compiler) after `./build.sh`.

## Controls

| | Keyboard / mouse | Pad |
|---|---|---|
| Coin | `5` | Back |
| Cast / select (the cabinet button; it also starts a game) | left button, `Space`, `Enter` | A, Start |
| Reel in | `R`, `Left Shift` (held) | B, right trigger |
| Aim the rod (and the arm) | the mouse pointer, the arrow keys | either stick |
| Menu selection up / down | `W` / `S`, `PageUp` / `PageDown` | shoulders |
| Test switch / Service | `F2` / `9` | |
| Menu | `Esc` | R3 |

Every key and button can be changed in Esc > Controls.

## What works

Power-on, the attract (with English **subtitles** for the Japanese captions: Esc > Subtitles), coins, a whole game -- the target map,
casting, hooking and reeling in a fish -- sound, the operator's test menu (its I/O TEST reads every control), widescreen, any internal
resolution and the frame-rate options. Three things are better than in MAME, which flags the game NOT WORKING: the reel winds the line
(MAME's is dead), the depth fog the game asks for is drawn (above and under the water), and the seams along the water tiles are gone.

Not yet: the cabinet's START switch is on an undumped chip (the game starts with the cast button, as MAME's does); not tested on real
Windows hardware or a real rod/reel controller.
