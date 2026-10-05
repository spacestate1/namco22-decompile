# Namco System 22 games for PC

Namco arcade games from the 1990s, rebuilt so they run on a normal
computer. You need your own copy of each game's files (the MAME versions);
they are not included here. A ROM set from **MAME 0.271 or later** is
supported; the chip unpacker also accepts a few older chip-name spellings
where a MAME set has since renamed one (see [docs/ROM_CHECKSUMS.md](docs/ROM_CHECKSUMS.md)).

| Game | Year | Status | Online play | Game files you need | Linux | Windows |
|---|---|---|---|---|---|---|
| **Prop Cycle** | 1996 | Playable from start to finish, with sound | no | `propcycl.zip` | yes | yes |
| **Rave Racer** | 1995 | Playable: races, with sound | **yes**: up to 8 players, LAN and internet ([how](#playing-online)) | `raverace.zip` + `namcoc74.zip` | yes | yes |
| **Tokyo Wars** | 1996 | Playable: attract, play, sound, widescreen | no | `tokyowar.zip` | yes | yes |
| **Dirt Dash** | 1995 | Playable: five stages, sound, widescreen | no | `dirtdash.zip` | yes | yes |
| **Time Crisis** | 1995 | Playable: attract, three-coin play through the stages, the operator's test mode, sound, widescreen; **the mouse is the gun**, and light guns work (see below) | no | `timecris.zip` | yes | yes |

**Are you running the same ROMs as the authors?** The checksums of every ROM file the games use are in
[docs/ROM_CHECKSUMS.md](docs/ROM_CHECKSUMS.md), with a list in `md5sum` format for each game.

**No extra BIOS file.** Earlier instructions asked for `c71.bin` or MAME's `namcoc71.zip` (the DSP's BIOS). The games have it built in now, so you never need it; if you have it, ignore it.

## Pictures

Tokyo Wars:

![Tokyo Wars: widescreen, with the menu open](docs/images/tokyowar-widescreen.png)

Rave Racer:

![Rave Racer: a race in widescreen](docs/images/raverace-widescreen.png)

Time Crisis:

![Time Crisis: the submarine hangar, with the gun crosshair](docs/images/timecrisis-gameplay.png)

Dirt Dash:

![Dirt Dash: a race in widescreen](docs/images/dirtdash-gameplay.png)

Prop Cycle:

![Prop Cycle: gameplay, over the river](docs/images/propcycle-gameplay.png)
![The title screen](docs/images/attract-title.png)

Tokyo Wars, the title screen:

![Tokyo Wars: the title screen](docs/images/tokyowar-title.png)

## Download (nothing to build)

Each release on the [Releases page](https://github.com/spacestate1/namco22-decompile/releases) has ready-made packages.
**None of them contains game files**: after installing, you put your own zips in a folder (the `INSTALL.txt` next to the packages says where).

| Package | For | How to install |
|---|---|---|
| `namco22_VERSION_amd64.deb` | Ubuntu 22.04 or newer, Linux Mint 21 or newer, LMDE 6, Debian 12 or newer | `sudo apt install ./namco22_*_amd64.deb` |
| `namco22-VERSION-1.fc40.x86_64.rpm` | Fedora 40 or newer | `sudo dnf install ./namco22-*.x86_64.rpm` |
| `PropCycle-x86_64.AppImage`, `RaveRacer-...`, `TokyoWars-...`, `DirtDash-...`, `TimeCrisis-...` | **The Steam Deck**, and any 64-bit Linux without installing anything | `chmod +x` the file and run it (on a Deck: Desktop Mode, right-click it > Add to Steam) |
| `windows-release-VERSION.zip` | Windows 10 or 11, 64-bit | Unzip it anywhere, put the game zips in its `roms` folder, double-click the game's `.exe` |

On Linux, start a game from the applications menu (or type `propcycle`, `raveracer`, `tokyowars`, `dirtdash` or `timecrisis`). The first time, it makes the folder
`~/.local/share/namco22/<game>/roms`, tells you which zip is missing and copies any it finds in `~/Downloads`. To uninstall: `sudo apt remove namco22`
(or `sudo dnf remove namco22`); your game files, settings and scores stay in `~/.local/share/namco22/`.

**Steam Deck:** download the AppImage of each game you want, put your ROM zips in `~/.local/share/namco22/<game>/roms/` (Desktop Mode: Dolphin > Ctrl+H shows the
hidden `.local` folder; zips in `~/Downloads` are found automatically), then in Desktop Mode right-click the AppImage > *Add to Steam* (or Steam >
*Add a Non-Steam Game*) and start it from Game Mode. The AppImages use the Deck's own SDL2, so Steam Input works: the menu opens with **R3** or by holding
**Start** for a second. Each AppImage uses the same folders as the `.deb`, so the ROMs and settings are shared. If an AppImage will not start, run it from a
terminal (`./DirtDash-x86_64.AppImage`) and read the message; on a system without FUSE, `--appimage-extract-and-run` works.

The AppImage names carry no version number, so **updating is overwriting the file** and a Steam shortcut keeps working. ROM zips placed **next to the
AppImage** (or in a `roms` folder beside it) are found too. **Portable mode:** make a folder named `namco22-data` next to the AppImages and each game
keeps everything there (`namco22-data/<game>/`: its roms, the unpacked chips, settings, scores and recordings) instead of `~/.local/share/namco22` --
one disk, e.g. a microSD card, then works the same on a Steam Deck and another PC. The Windows zip is portable already (everything lives in its folder).

## How it was made

Each game's original program was taken apart with
[Ghidra](https://ghidra-sre.org/) (the NSA's free reverse-engineering tool)
and checked, piece by piece, against the arcade machine running in
[MAME](https://www.mamedev.org/).

- **Prop Cycle**: Ghidra's output was turned into C and fixed by hand.
- **Rave Racer**: the program is translated to C by a tool of this project
  (`raverace/gen/rr_lifted.c` is its output), and parts are being rewritten
  by hand.
- **Tokyo Wars**: the program is translated to C by the same tool
  (`tokyowar/gen/tw_lifted.c`); its master DSP and sound programs are
  turned into C when you build, from your own copy of the game files.
- **Dirt Dash**: the program is translated to C by the same tool
  (`dirtdash/gen/dd_lifted.c`); its master DSP and sound programs are
  turned into C when you build, from your own copy of the game files.
- **Time Crisis**: the program is translated to C by the same tool
  (`timecris/gen/tc_lifted_NN.c`, 16 parts); its master DSP and sound programs are
  turned into C when you build, from your own copy of the game files.
  It is the newest game here: the program's code was checked against MAME's
  own coverage of the real game, and the DSP and sound translations against
  the interpreters they were made from; it has **not yet been compared with MAME
  frame by frame or note by note**, so expect small differences.
- The sound programs of all five games are turned into C when you build,
  from your own copy of the game files.

This repository has **only the code**. It has no game files and no Ghidra
project or tools. Everything the games show or play is read from your own
zips.

## What you need to build it yourself

- **Your own copy of each game's zip** (see the table at the top). They are needed while building, not only while playing: each game's sound program
  and DSP program is turned into C from them.
- **Linux (64-bit):** a C compiler (`gcc`), `make`, CMake, `pkg-config`, the SDL2, OpenGL (Mesa) and zlib development files, and Python 3 with numpy.
  `./install-deps.sh` installs all of these on Debian / Ubuntu, Fedora, Arch, openSUSE, Alpine, Void and Gentoo.
- **Windows programs** are built from Linux (`./build-windows.sh`): the MinGW-w64 cross compiler (`x86_64-w64-mingw32-gcc`), CMake, `make`, `curl`, `7z` (p7zip)
  and Python 3. The first run downloads SDL2, zlib and Mesa (the fallback OpenGL for computers with no graphics driver), so it needs the internet.
- **Building the `.deb`, `.rpm` and Windows zip the way a release is made** is done by the author with a script that is not part of this repository (it needs
  Podman for the Ubuntu and Fedora containers); you do not need it to build or play.

## Linux

Open a terminal in this folder. First, once:

```bash
./install-deps.sh
```

This installs the tools the games need. It asks for your password.

Then build each game you have. Change the paths if your zips are
somewhere else:

```bash
./build.sh ~/Downloads/propcycl.zip                                       # Prop Cycle
raverace/build.sh ~/Downloads/raverace.zip ~/Downloads/namcoc74.zip        # Rave Racer
tokyowar/build.sh ~/Downloads/tokyowar.zip                                 # Tokyo Wars
dirtdash/build.sh ~/Downloads/dirtdash.zip                                 # Dirt Dash
timecris/build.sh ~/Downloads/timecris.zip                                 # Time Crisis
```

Rave Racer's, Tokyo Wars', Dirt Dash's and Time Crisis' first builds take a few minutes.

Then play:

```bash
./launch.sh prop      # Prop Cycle
./launch.sh rave      # Rave Racer
./launch.sh tokyo     # Tokyo Wars
./launch.sh dirt      # Dirt Dash (`./launch.sh dirt jungle` starts in a stage: city, jungle, hill, mountain, snow)
./launch.sh tc        # Time Crisis (the mouse is the gun)
./launch.sh           # the list of games and options
```

## Windows

All four games run on Windows. The Windows version is built from Linux. Type:

```bash
./build-windows.sh
```

This makes a `windows-release` folder. Copy it to the Windows computer.
Put the game files in its `roms` folder: `propcycl.zip` for Prop Cycle,
`raverace.zip` and `namcoc74.zip` for Rave Racer, `tokyowar.zip` for Tokyo
Wars, `dirtdash.zip` for Dirt Dash, `timecris.zip` for Time Crisis. Then double-click **PropCycle.exe**,
**RaveRacer.exe**, **TokyoWars.exe**, **DirtDash.exe** or **TimeCrisis.exe**.

Building on Windows itself, and what in the code is there for Windows only:
[docs/WINDOWS.md](docs/WINDOWS.md).

## How to play

**Prop Cycle.** You fly a pedal-powered glider and pop balloons.

| Key | What it does |
|---|---|
| `5` | Put in a coin |
| `Enter` | Start |
| Arrow keys | Steer and tilt |
| `Space` | Pedal |
| `Esc` | Menu (restart, exit, sound, levels, screen) |
| `P` | Pause. While paused, the arrow keys turn the camera around the rider and `+` / `-` zoom |
| `F12` | Take a picture |

**Rave Racer.** A street race against the clock.

| Key | What it does |
|---|---|
| `5` | Put in a coin |
| `X` (or Up) | Gas |
| `Z` (or Down) | Brake |
| Left / Right | Steer |
| `A` / `S` | Shift down / up |
| `V` | Change the view |
| `F2` | Test mode on / off (press again to leave it) |
| `9` | Service |
| `P` | Pause |
| `Esc` | Menu |
| `F12` | Take a picture |

<a id="playing-online"></a>**Playing online (Rave Racer).** Up to eight players can race together, each on
their own computer. Open the menu with `Esc` and go to the **Online** page,
then **Host / join a game...**:

- **Local LAN**: one player picks *Host a LAN game*; everyone else picks *Find
  LAN games* and clicks the game it finds.
- **Internet game**: type the address of a server (`host` or `host:port`, UDP
  27750 by default) and *Connect*. Anyone can run such a server on a machine
  that is always on: `raverace/server/` is a small Rust program for exactly
  this (see its README; the wire protocol is `raverace/NETPLAY.md`).

Everyone in the lobby then picks **Ready**, and anyone can start the race once
all players are ready. When the race begins the game turns free play on for
you and gives everyone a moment to step on the gas; choose your car and wait
for the others at the course select. Your name on the Online page is what the
other players see.

**Tokyo Wars.** You command a tank in a city battle.

| Key | What it does |
|---|---|
| `5` | Put in a coin |
| `Enter` | Start |
| Left / Right (or `A` / `D`) | Steer |
| Up / Down (or `W` / `S`) | Forward / backward pedal |
| `X` / `Z` | Right / left trigger |
| `9` | Service |
| `F2` | Test mode on / off |
| `Esc` | Menu (screen, sound, keys) |
| `P` | Pause |
| `F11` | Full screen |
| `F12` | Take a picture |

**Dirt Dash.** An off-road race against the clock.

| Key | What it does |
|---|---|
| `5` | Put in a coin (a game costs two) |
| Left / Right (or `A` / `D`) | Steer |
| `X` | Gas |
| `Z` | Brake |
| `Q` / `E` | Shift down / up |
| `C` | Select (change the view, confirm) |
| `M` | Motion stop |
| `9` | Service |
| `Esc` | Menu (screen, sound, keys) |
| `P` | Pause |
| `F11` | Full screen |
| `F12` | Take a picture |

A game controller works in all the games. In Rave Racer the keys can be
changed in `raverace/rr_controls.cfg`; in Tokyo Wars and Dirt Dash, in the menu
(**Controls**) or in `tokyowar/tw_controls.cfg` / `dirtdash/dd_controls.cfg`.

**Time Crisis.** A light-gun shooter. **The mouse is the gun.**

| Key | What it does |
|---|---|
| Mouse | Aim |
| Left button (or `Space`) | Shoot (also starts the game and picks menu items) |
| Right button (or `Z`) | The foot pedal: step out of cover to attack, step back, reload |
| `5` | Put in a coin (a game costs three) |
| `9` | Service |
| `F2` | Test mode on / off (the operator's menu: shoot inside the screen = up, outside = down, pedal = enter) |
| `F8` | A white border round the picture (cycles its width; some light guns need it) |
| Arrow keys / a pad's right stick | Aim without a mouse; `A` / right shoulder shoots, `B` / left shoulder is the pedal |
| `Esc` | Menu (screen, sound, keys) |
| `F11` / `F12` | Full screen / picture |

**Light guns.** A gun that acts as an absolute mouse (Sinden, Gun4IR, OpenFIRE, Reaper, AimTrak) works as it is: aim at the screen and shoot.
To **reload**, aim a little OFF the screen and shoot (the pointer sits on the window's edge, which the game reads as "off screen"), or hold `R` or the
gun's side button. The widescreen picture keeps the game's 4:3 aiming area in the middle. Guns with their own calibration need it done once in their own software. Not tested here: any real light gun hardware.

**Choosing a stage.** `--stage 1`, `--stage 2` or `--stage 3` on the command line starts the game's own **Timed Game** at stage 1, 2 or 3 (unlimited lives, a best time per stage):
the coins are put in and the choices made for you, then the gun is yours. It works from the attract screens, not during a game.
**No crosshair:** *Esc > Display > Crosshair* turns the red aiming
cross off (a real light gun needs none).

**For testing** (environment variables, set before starting the game): `TC_INF_TIME=1` keeps the clock from running out and
`TC_INF_LIFE=1` gives back every life lost, so a test can reach the later scenes of any stage. They act only during a game.

**Steering with the stick** (Tokyo Wars and Dirt Dash): *Esc > Controls > Stick steering* sets how the stick turns
the wheel. **Medium** (the default) is gentle near the centre and still reaches full lock at the edge; **Smooth** and
**Very smooth** are gentler still; **Linear** turns the wheel in step with the stick.

**On a game pad or a Steam Deck** (no keyboard): the menu opens with **R3**
(click the right stick) or by **holding Start for a second** (a quick tap is
still the game's own Start). The cabinet's **Test mode** (the service switch,
`F2` on a keyboard) and **Service** button are in the menu too -- the **File**
page in Rave Racer and Prop Cycle, the **Controls** page in Tokyo Wars and
Dirt Dash. Test mode is a switch: turn it on to enter the operator menu (its
screen says which controls choose, enter and change a value), turn it off from
the menu to leave. A hint on the screen says how to open the menu for the first
few seconds after the game starts, when a pad is connected.

## Screen settings (Prop Cycle, Tokyo Wars, Dirt Dash and Time Crisis)

Press `Esc` and open **Display**. Your choices are saved by themselves.

- **Widescreen**: turn it on to fill a wide screen. You see more of the
  world at the sides, not a stretched picture. The time and score gauges
  move out to the corners.
- **Window mode**: a window, or full screen.
- **Resolution**: how many pixels the game draws. It is scaled to fit the
  window, so a smaller number runs faster on a slow computer.
- **Aspect ratio** (when widescreen is off): 4:3 like the arcade screen, or
  stretched to fill the window.

## If it does not work

- **"not installed"**: run `./install-deps.sh` again.
- **"can't be used"**: the zip is the wrong game or version (compare your files with
  [docs/ROM_CHECKSUMS.md](docs/ROM_CHECKSUMS.md)). Prop Cycle
  needs the one called `propcycl`; Rave Racer needs `raverace` and
  `namcoc74`; Tokyo Wars needs `tokyowar`; Dirt Dash needs `dirtdash`; Time Crisis needs `timecris` (World, TS2 Ver.B).
- **"still missing dt2vera.1"** (Dirt Dash): the program chips `dt2vera.1` and `dt2vera.2` were not found
  in `dirtdash.zip`. They can be at the top of the zip, in a `dirtdasha/` folder or in any other folder, or
  in a separate `dirtdasha.zip` beside it. If the message says the chip "cannot be used", it says why (wrong
  size, damaged zip, or a compression method that is not Deflate or Store).
- **"c71.bin is missing"**: that message is from an older version. The DSP's BIOS is built in now, so
  `c71.bin` and `namcoc71.zip` are not needed; update to a current build.
- **Black or white screen**: update your graphics driver.
- **Slow, or the picture stutters.** Open the game's log (`raveracer.log`, `tokyowar.log`, `dirtdash.log`, `timecris.log`,
  `propcycl.log` beside the `.exe` on Windows; the terminal window on Linux) and look at these lines:
  - `[HOST] OpenGL: ...` (Prop Cycle: `OpenGL: ...`) names the graphics chip the game is using. It should be
    your graphics card, for example *NVIDIA GeForce RTX 3050 Ti*. If a laptop with **two** graphics chips shows the
    weaker one (*AMD Radeon Graphics*, *Intel ...*), the game is on the wrong chip. If it says *llvmpipe*,
    *GDI Generic* or *Microsoft Basic Render Driver*, it is drawing in software: install the graphics driver.
  - `[HOST] display N Hz, vsync` or `timer-paced`, and every 600 frames `[PACE] ... interval mean 16.7 ms`.
    Interval near 16.7 ms with few frames over 20 ms is a steady 60 fps. `[POST] final colour stage: GLSL` is the
    fast path; `pixel map (slow)` is not: please send us that log.
  - **Laptops with two graphics chips, Windows:** the newest builds ask Windows for the fast chip themselves. If
    yours still uses the other one, set it by hand: *Settings > System > Display > Graphics*, add the game's `.exe`,
    *Options > High performance* (or *NVIDIA Control Panel > Manage 3D settings > Program settings*).
  - **Linux, two graphics chips:** `__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia ./launch.sh dirt`
    (or `prime-run ./launch.sh dirt`); with AMD or Intel drivers `DRI_PRIME=1 ./launch.sh dirt`.
  - Plug the laptop in and pick a *Best performance* power mode. On a 120/144/165 Hz screen the games still run at the
    arcade's 60 Hz, which looks uneven; a 60 Hz screen mode is smoothest. A lower *Esc > Display > Resolution* also helps.

More about the game files: [docs/ROM_SETUP.md](docs/ROM_SETUP.md)

## License

The code in this repository is released under the [MIT License](LICENSE).

- The games themselves (their programs, graphics, sound and music) belong to Namco / Bandai Namco. This repository has no game files and the MIT
  License does not cover them; the files under `*/gen/` that are translated from the games' programs are derived from Namco's code, and the license
  covers this project's own work, not theirs.
- Other people's code keeps its own license: [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) (`third_party/`, MIT / public domain).
  The Windows programs also contain [SDL2](https://libsdl.org) and [zlib](https://zlib.net) (both under the zlib license), and the `mesa` folder of the
  Windows download is [Mesa](https://mesa3d.org) (MIT and other licenses).
