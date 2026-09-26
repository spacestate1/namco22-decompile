Prop Cycle, Rave Racer, Tokyo Wars and Dirt Dash -- decompiled engines for the Namco System 22 arcade games
==============================================================================================================

WHERE THE ROMS GO  (do this after installing)
---------------------------------------------
This package contains NO game data. You need the MAME ROM sets, placed in
YOUR home folder at exactly these paths:

  Prop Cycle:   ~/.local/share/namco22/propcycle/roms/propcycl.zip
  Rave Racer:   ~/.local/share/namco22/raverace/roms/raverace.zip
                ~/.local/share/namco22/raverace/roms/namcoc74.zip
  Tokyo Wars:   ~/.local/share/namco22/tokyowar/roms/tokyowar.zip
  Dirt Dash:    ~/.local/share/namco22/dirtdash/roms/dirtdash.zip

Easiest way: start the game once from the applications menu. It creates the
folder, tells you what is missing and opens the folder; copy the zip(s) in and
start it again. Zips already in ~/Downloads are picked up automatically.
(~/.local is a hidden folder: in a file manager press Ctrl+H to see it.)
From a terminal:
  mkdir -p ~/.local/share/namco22/propcycle/roms ~/.local/share/namco22/raverace/roms ~/.local/share/namco22/tokyowar/roms ~/.local/share/namco22/dirtdash/roms
  cp propcycl.zip ~/.local/share/namco22/propcycle/roms/
  cp raverace.zip namcoc74.zip ~/.local/share/namco22/raverace/roms/
  cp tokyowar.zip ~/.local/share/namco22/tokyowar/roms/
  cp dirtdash.zip ~/.local/share/namco22/dirtdash/roms/

The first start unpacks the zips into the game folder (extracted/); after that
the zips may be removed. Settings, high scores and recordings are kept in the
same folder.

PLAYING
-------
Prop Cycle:  5 coin, Enter start, arrow keys steer, Space pedal, P pause,
             Esc menu, F12 picture.   `propcycle 0..3` starts straight at a level.
Rave Racer:  5 coin, X gas, Z brake, arrow keys steer, A/S shift, V view,
             P pause, Esc menu, F12 picture.   `raveracer 2` = window scale 2.
Tokyo Wars:  5 coin, Enter start, arrow keys (or A/D) steer, Up/W forward, Down/S backward,
             X / Z triggers, P pause, Esc menu (Display: widescreen ...), F12 picture.
             `tokyowars 2` = window scale 2.
Dirt Dash:   5 coin (a game costs two), Z brake, X gas (throttle), C select (view change / confirm), Left/Right
             (A/D) steer, Q / E shift down / up, M motion stop, P pause, Esc menu, F12 picture.   `dirtdash 2` = window scale 2.
A game controller works too (Esc -> Controls to rebind).
