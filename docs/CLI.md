# Command-line options

Options go after the game's name. Options you set this way last for that run only and don't change your settings file.

| How you installed | Example |
|---|---|
| AppImage | `./TimeCrisis2-x86_64.AppImage --fullscreen` |
| `.deb` package | `namco-2x-launch tc2 --fullscreen` |
| Windows | `TimeCrisis2.exe --fullscreen` |

Names for `namco-2x-launch`: `prop` `rave` `tokyo` `dirt` `am` `tc` `ace` `cc` `r2` `cs` `tc2` `csz` `ak`

## All games

| Option | What it does |
|---|---|
| `--fullscreen` | Start full screen. |
| `--set KEY=VALUE` | Use this value for any line of the game's `.cfg` file. Repeat it for more settings. |
| `2`, `3`, ... | Window size (Linux only). In Prop Cycle, a number from `0` to `3` starts that level instead. |

Example:

```
./RaveRacer-x86_64.AppImage --fullscreen --set widescreen=1 --set volume=50
```

Your `.cfg` file lists the setting names. It's in `~/.local/share/namco-2x-systems/<game>/` on Linux, and beside the game on Windows.

## Time Crisis II and Crisis Zone: link play

| Option | What it does |
|---|---|
| `--host` | Host a LAN game. It's on your network only, not the public server. |
| `--join` | Join the first LAN game found. |
| `--server ADDR[:PORT]` | Connect to an online server. |
| `--name NAME` | Your player name. |
| `--instance N` | Run another copy on this PC, with its own settings and saves. Use `2` or higher. The first copy doesn't need it. |
| `--pos X,Y[,SIZE]` | Where to put the window, and optionally its size. |

`--host` and `--join` only connect the two copies. **The game starts after both players press Ready and one presses Start** in Esc > Online.

### Two players on one PC

The easiest way is **Esc > Online > Second player on this computer**. It starts the second copy for you, and the game begins when both are in. Give each player their own gun in Controls > Gun device.

To start both copies yourself, use two terminals:

```
./TimeCrisis2-x86_64.AppImage --host --pos 0,0
./TimeCrisis2-x86_64.AppImage --instance 2 --join --pos 960,0
```

To skip Ready and Start, set `TC2_NET_AUTOSTART=2` for both copies:

```
TC2_NET_AUTOSTART=2 ./TimeCrisis2-x86_64.AppImage --host --pos 0,0
TC2_NET_AUTOSTART=2 ./TimeCrisis2-x86_64.AppImage --instance 2 --join --pos 960,0
```

On Windows, run `set TC2_NET_AUTOSTART=2` in each Command Prompt first.

### Two PCs

Run `--host` on one PC and `--join` on the other. Both need the same version of the game, and UDP port 27750 open in the firewall.
