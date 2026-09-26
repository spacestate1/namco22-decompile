# Namco System 22 games for PC

Namco arcade games from the 1990s, rebuilt so they run on a normal
computer. You need your own copy of each game's files (the MAME versions);
they are not included here.

| Game | Year | Status | Game files you need | Linux | Windows |
|---|---|---|---|---|---|
| **Prop Cycle** | 1996 | Playable from start to finish, with sound | `propcycl.zip` | yes | yes |
| **Rave Racer** | 1995 | Playable: races, with sound | `raverace.zip` + `namcoc74.zip` (+ `namcoc71.zip`, see below) | yes | yes |
| **Tokyo Wars** | 1996 | Playable: attract, play, sound, widescreen | `tokyowar.zip` (+ `namcoc71.zip`, see below) | yes | yes |
| **Dirt Dash** | 1995 | Playable: five stages, sound, widescreen | `dirtdash.zip` (+ `namcoc71.zip`, see below) | yes | yes |

**`c71.bin`, the DSP BIOS (Rave Racer, Tokyo Wars and Dirt Dash).** These three
games need one more file, `c71.bin`. In some MAME sets it is already inside the
game's own zip. In others it is a separate MAME set, **`namcoc71.zip`**. If a
game says `c71.bin` is missing, put `namcoc71.zip` in the same place as the
game's other zips (the `roms` folder on Windows and in the packages), or add it
to the `build.sh` command. Leave it zipped, and there is no need to copy
`c71.bin` into another zip. (Prop Cycle does not need it.)

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
- The sound programs of all four games are turned into C when you build,
  from your own copy of the game files.

This repository has **only the code**. It has no game files and no Ghidra
project or tools. Everything the games show or play is read from your own
zips.

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
```

If a build says `c71.bin` is missing, add `namcoc71.zip` to that command, for
example `raverace/build.sh ~/Downloads/raverace.zip ~/Downloads/namcoc74.zip ~/Downloads/namcoc71.zip`.

Rave Racer's, Tokyo Wars' and Dirt Dash's first builds take a few minutes.

Then play:

```bash
./launch.sh prop      # Prop Cycle
./launch.sh rave      # Rave Racer
./launch.sh tokyo     # Tokyo Wars
./launch.sh dirt      # Dirt Dash (`./launch.sh dirt jungle` starts in a stage: city, jungle, hill, mountain, snow)
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
Wars, `dirtdash.zip` for Dirt Dash. If a game says `c71.bin` is missing, put
`namcoc71.zip` in the same folder. Then double-click **PropCycle.exe**,
**RaveRacer.exe**, **TokyoWars.exe** or **DirtDash.exe**.

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

**On a game pad or a Steam Deck** (no keyboard): the menu opens with **R3**
(click the right stick) or by **holding Start for a second** (a quick tap is
still the game's own Start). The cabinet's **Test mode** (the service switch,
`F2` on a keyboard) and **Service** button are in the menu too -- the **File**
page in Rave Racer and Prop Cycle, the **Controls** page in Tokyo Wars and
Dirt Dash. Test mode is a switch: turn it on to enter the operator menu (its
screen says which controls choose, enter and change a value), turn it off from
the menu to leave. A hint on the screen says how to open the menu for the first
few seconds after the game starts, when a pad is connected.

## Pictures

Tokyo Wars:

![Tokyo Wars: widescreen, with the menu open](docs/images/tokyowar-widescreen.png)

Rave Racer:

![Rave Racer: a race in widescreen](docs/images/raverace-widescreen.png)

Prop Cycle:

![Prop Cycle: gameplay, over the river](docs/images/propcycle-gameplay.png)
![The title screen](docs/images/attract-title.png)

Tokyo Wars, the title screen:

![Tokyo Wars: the title screen](docs/images/tokyowar-title.png)

## Screen settings (Prop Cycle, Tokyo Wars and Dirt Dash)

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
- **"can't be used"**: the zip is the wrong game or version. Prop Cycle
  needs the one called `propcycl`; Rave Racer needs `raverace` and
  `namcoc74`; Tokyo Wars needs `tokyowar`; Dirt Dash needs `dirtdash`.
- **"c71.bin is missing"** (Rave Racer, Tokyo Wars, Dirt Dash): the DSP BIOS is
  not inside the game's zip. It is in MAME's separate `namcoc71.zip`: put that
  zip beside the game's zip (or add it to the `build.sh` command). Do not unzip
  it. Copying `c71.bin` into `namcoc74.zip` also works, but is no longer needed.
- **Black or white screen**: update your graphics driver.

More about the game files: [docs/ROM_SETUP.md](docs/ROM_SETUP.md)
