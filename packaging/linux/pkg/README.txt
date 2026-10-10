Namco 2x Systems: Prop Cycle, Rave Racer, Tokyo Wars, Dirt Dash, Time Crisis, Ace Driver, Cyber Commando, Cyber Sled and Time Crisis 2
-- decompiled engines for the Namco System 21 / 22 / 23 arcade games
(Cyber Sled runs on Namco System 21, Time Crisis 2 on System 23; the other seven on System 22 / Super System 22)
=============================================================================================================================

WHERE THE ROMS GO  (do this after installing)
---------------------------------------------
This package contains NO game data. You need the MAME ROM sets, placed in
YOUR home folder at exactly these paths:

  Prop Cycle:   ~/.local/share/namco-2x-systems/propcycle/roms/propcycl.zip
  Rave Racer:   ~/.local/share/namco-2x-systems/raverace/roms/raverace.zip
                ~/.local/share/namco-2x-systems/raverace/roms/namcoc74.zip
  Tokyo Wars:   ~/.local/share/namco-2x-systems/tokyowar/roms/tokyowar.zip
  Dirt Dash:    ~/.local/share/namco-2x-systems/dirtdash/roms/dirtdash.zip  (+ dirtdasha.zip if your MAME set has one)
  Time Crisis:  ~/.local/share/namco-2x-systems/timecris/roms/timecris.zip
  Ace Driver:   ~/.local/share/namco-2x-systems/acedriver/roms/acedrive.zip
                ~/.local/share/namco-2x-systems/acedriver/roms/namcoc74.zip
  Cyber Commando: ~/.local/share/namco-2x-systems/cybrcomm/roms/cybrcomm.zip
                ~/.local/share/namco-2x-systems/cybrcomm/roms/namcoc74.zip
  Cyber Sled:   ~/.local/share/namco-2x-systems/cybsled/roms/cybsled.zip
                ~/.local/share/namco-2x-systems/cybsled/roms/namcoc67.zip
                ~/.local/share/namco-2x-systems/cybsled/roms/namcoc68.zip
  Time Crisis 2: ~/.local/share/namco-2x-systems/tc2/roms/timecrs2.zip

Upgrading from 0.6 or older (the "namco22" package): the games keep using your
old ~/.local/share/namco22/<game>/ folders, ROMs, settings and scores included.

Easiest way: start the game once from the applications menu. It creates the
folder, tells you what is missing and opens the folder; copy the zip(s) in and
start it again. Zips already in ~/Downloads are picked up automatically.
(~/.local is a hidden folder: in a file manager press Ctrl+H to see it.)
From a terminal:
  mkdir -p ~/.local/share/namco-2x-systems/propcycle/roms ~/.local/share/namco-2x-systems/raverace/roms ~/.local/share/namco-2x-systems/tokyowar/roms ~/.local/share/namco-2x-systems/dirtdash/roms ~/.local/share/namco-2x-systems/timecris/roms ~/.local/share/namco-2x-systems/acedriver/roms ~/.local/share/namco-2x-systems/cybrcomm/roms ~/.local/share/namco-2x-systems/cybsled/roms ~/.local/share/namco-2x-systems/tc2/roms
  cp propcycl.zip ~/.local/share/namco-2x-systems/propcycle/roms/
  cp raverace.zip namcoc74.zip ~/.local/share/namco-2x-systems/raverace/roms/
  cp tokyowar.zip ~/.local/share/namco-2x-systems/tokyowar/roms/
  cp dirtdash.zip ~/.local/share/namco-2x-systems/dirtdash/roms/
  cp timecris.zip ~/.local/share/namco-2x-systems/timecris/roms/
  cp acedrive.zip namcoc74.zip ~/.local/share/namco-2x-systems/acedriver/roms/
  cp cybrcomm.zip namcoc74.zip ~/.local/share/namco-2x-systems/cybrcomm/roms/
  cp cybsled.zip namcoc67.zip namcoc68.zip ~/.local/share/namco-2x-systems/cybsled/roms/
  cp timecrs2.zip ~/.local/share/namco-2x-systems/tc2/roms/

The first start unpacks the zips into the game folder (extracted/); after that
the zips may be removed. Settings, high scores and recordings are kept in the
same folder.

APPIMAGES: zips placed next to the .AppImage file (or in a roms/ folder beside
it) are found too. PORTABLE MODE: make a folder named namco-2x-systems-data next
to the AppImages (an older namco22-data folder works too), and each game keeps
everything in it, in <game>/, instead of ~/.local/share/namco-2x-systems -- one disk then works on a Steam Deck and another PC.
The AppImage standard's portable folder works as well: a folder named after the
AppImage plus .home (TimeCrisis-x86_64.AppImage.home or TimeCrisis-x86_64.home).
The AppImage names have no version number: to update, overwrite the file.

PLAYING
-------
Prop Cycle:  5 coin, Enter start, arrow keys steer, Space pedal, P pause,
             Esc menu, F12 picture.   `propcycle 0..3` starts straight at a level.
Rave Racer:  5 coin, X gas, Z brake, arrow keys steer, A/S shift, V view,
             P pause, Esc menu, F12 picture.   `raveracer 2` = window scale 2.
Tokyo Wars:  5 coin, Enter start, arrow keys (or A/D) steer, Up/W forward, Down/S backward,
             X / Z triggers, P pause, Esc menu (Display: widescreen, frame rate ...), F12 picture.
             `tokyowars 2` = window scale 2.
Dirt Dash:   5 coin (a game costs two), Z brake, X gas (throttle), C select (view change / confirm), Left/Right
             (A/D) steer, Q / E shift down / up, M motion stop, P pause, Esc menu, F12 picture.   `dirtdash 2` = window scale 2.
Time Crisis: the MOUSE is the gun: move it to aim, left button = shoot, right button (or Z) = the foot pedal (come out of cover / reload),
             5 coin (a game costs 3), Esc menu, F2 the operator's test switch, F8 a white border round the picture for
             light guns that need one. An absolute-mouse light gun (Sinden, Gun4IR, OpenFIRE, Reaper, AimTrak) works as a
             mouse: shoot off the edge of the screen (or hold R / the side button) to reload. Arrow keys / the right stick aim
             too. `timecrisis 2` = window scale 2.
Ace Driver:  5 coin (a game costs two), X gas, Z brake, arrow keys steer, A/S shift, V view, F2 test switch,
             Esc menu (Debug: developer screens left in the game), F12 picture.   `acedriver 2` = window scale 2.
Cyber Commando: Cyber Sled's layout -- 5 coin (a game costs two), arrow keys drive (Shift + Left/Right strafe), E/D/S/F +
             I/K/J/L the two sticks, Z gun, X missile, C view, F2 test switch, Esc menu, F12 picture.
             Pad: the two sticks are the cabinet's two sticks, A gun, B missile, Y view.   `cybercommando 2` = window scale 2.
Cyber Sled:  5 coin, 1 / Enter start, arrow keys drive (Shift + Left/Right strafe), E/D/S/F + I/K/J/L the two levers,
             Z gun, X missile, C view, F2 test switch, Esc menu (Online: a battle against a friend; Debug), F12 picture.
             Pad: the two sticks are the two levers, A gun, B missile, Y view. Keys are fixed (no rebinding page yet).   `cybersled 2` = window scale 2.
Time Crisis 2: 5 coin, the mouse / a light gun aims, left button shoots, right button (or Z / X) the pedal, R reload,
             F2 test switch, F8 a white border for light guns, Esc menu, F12 picture.   `timecrisis2 2` = window scale 2.
A game controller works too (Esc -> Controls to rebind).
