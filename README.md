# Prop Cycle for PC

This is the 1996 Namco arcade game **Prop Cycle**, rebuilt so it runs on a
normal computer. You fly a pedal-powered glider and pop balloons.

![The rider flies past the namco logo](docs/images/attract-namco.png)
![The title screen](docs/images/attract-title.png)
![The demo, popping balloons](docs/images/attract-demo.png)

Press `Esc` any time for the menu:

![Title screen with the File menu open](docs/images/title-file-menu.png)
![The demo with the Levels menu open](docs/images/demo-levels-menu.png)

## What you need

Your own copy of the game file **`propcycl.zip`** (the MAME version).
It is not included here.

## Linux

Open a terminal in this folder and type:

```bash
./install-deps.sh
./build.sh ~/Downloads/propcycl.zip
./launch.sh
```

1. The first line installs the tools it needs. It asks for your password.
2. The second line builds the game. Change the path if your zip is somewhere else.
3. The third line starts the game.

Next time, just type `./launch.sh`.

## Windows

Open the `windows-release` folder. Put `propcycl.zip` in its `roms`
folder. Then double-click **PropCycle.exe**.

## How to play

| Key | What it does |
|---|---|
| `5` | Put in a coin |
| `Enter` | Start |
| Arrow keys | Steer and tilt |
| `Esc` | Menu (restart, exit, sound, levels) |
| `F12` | Take a picture |

A game controller works too.

## If it does not work

- **"not installed"**: run `./install-deps.sh` again.
- **"can't be used"**: your zip is the wrong game. You need the one
  called `propcycl`.
- **Black or white screen**: update your graphics driver.

More about the game files: [docs/ROM_SETUP.md](docs/ROM_SETUP.md)
