# Local ROM setup

**The easy way:** `./build.sh /path/to/propcycl.zip`. It runs
`tools/setup_roms.py`, which checks every file below by name and size and
copies them into `extracted/`. The rest of this page is only needed if you
want to do it by hand. To check that your files are the same ROMs the games were
made with, compare their checksums with [ROM_CHECKSUMS.md](ROM_CHECKSUMS.md).

This project does not provide, download, or redistribute game ROMs. Dump
your own legally obtained Prop Cycle board/set and place the files in a local
directory. The default directory is `extracted/` at the repository root; it
is ignored by Git.

The supported program revision is identified by the program-ROM reset values
`SP=0x00E20000` and `PC=0x0000BC4C`. Keep the original chip filenames. The
runtime expects this layout:

```
extracted/
  pr2ver-a.1  pr2ver-a.2  pr2ver-a.3  pr2ver-a.4      # 1 MiB each
  pr1ptrl0.18k  pr1ptrl1.16k  pr1ptrl2.15k            # 512 KiB each
  pr1ptrm0.18j  pr1ptrm1.16j  pr1ptrm2.15j            # 512 KiB each
  pr1ptru0.18f  pr1ptru1.16f  pr1ptru2.15f            # 512 KiB each
  pr1cg0.12b  pr1cg1.10d  pr1cg2.12d  pr1cg3.13d
  pr1cg4.14d  pr1cg5.16d  pr1cg6.18a  pr1cg7.15a      # 2 MiB each
  pr1ccrl.3d                                            # 2 MiB
  pr1ccrh.1d                                            # 512 KiB
  pr1scg0.12f  pr1scg1.10f                              # 2 MiB each
  pr1data.8k                                            # 512 KiB sound MCU program
  pr1wavea.2l  pr1waveb.1l                              # 4 MiB each sound data
```

The full list is needed to play. `pr1data.8k` and the two wave ROMs are the
sound: the build turns the sound program in `pr1data.8k` into C, so build
after the files are in place. The optional
`palette_mame_runtime.bin` is not a ROM and is not required: without it, the
renderer uses the static palette decoded from the program ROM.

After adding the files, verify that the program can read them by launching:

```bash
./build.sh
./launch.sh 0
```

The startup log should report `Program ROM: ... OK` and finish with `ROM
loading complete`. A `Cannot open` or `Short read` message means the
filename, directory, or dump size does not match the layout above.

To keep assets outside the checkout, point the executable at the directory
instead:

```bash
./build/propcycl /absolute/path/to/propcycl-roms --autostart 0
```

## Rave Racer

Rave Racer needs two MAME sets: `raverace.zip` (the game, World RV2 Ver.B)
and `namcoc74.zip` (`c74.bin`, the sound chip's BIOS). Give both to the
build script:

```bash
raverace/build.sh /path/to/raverace.zip /path/to/namcoc74.zip
```

It checks every file's name and size and copies them into
`raverace/extracted/`. A folder holding the chip files, or holding the zips,
works too.

The DSP's BIOS (`c71.bin`, MAME's `namcoc71` set) is built into the games, in all four:
you never need it, whatever your MAME version calls it or wherever it keeps it. The Japanese sets
inside `raverace.zip` (`raveracej/`, `raveraceja/`) are different programs and are not used.

## Tokyo Wars

Tokyo Wars needs one MAME set: `tokyowar.zip` (World, TW2 Ver.A). Give it
to the build script:

```bash
tokyowar/build.sh /path/to/tokyowar.zip
```

It checks all 33 files by name and size and copies them into
`tokyowar/extracted/`. The installed packages and the Windows version do
this themselves the first time the game starts, from `tokyowar.zip` in the
game's `roms` folder. The Japanese set inside the zip (`tokyowarj/`) is a
different program and is not used.

The program chips are read as either `tw2ver-a.1`-`.4` (older MAME sets) or
`tw2vera.1`-`.4` (MAME 0.271 and later, which dropped the hyphen) --
whichever spelling your zip actually has, either is taken.

## Time Crisis

Time Crisis needs one MAME set: `timecris.zip` (World, TS2 Ver.B). Give it to the build script:

```bash
timecris/build.sh /path/to/timecris.zip
```

It checks all 31 chip files by name (or CRC) and size and copies them into `timecris/extracted/`; the 68020 program is the four chips
`ts2verb.1` - `.4` (MAME's `timecris`), interleaved into `timecris_main.bin`. The Ver.A program that the same zip carries under `timecrisa/` is a
different version and is not used. The installed packages and the Windows version do all this themselves the first time the game starts, from
`timecris.zip` in the game's `roms` folder. Compare your files with [ROM_CHECKSUMS.md](ROM_CHECKSUMS.md).

## Dirt Dash

Dirt Dash needs one MAME set: `dirtdash.zip` (World, DT2 Ver.A). Give it
to the build script:

```bash
dirtdash/build.sh /path/to/dirtdash.zip
```

It checks all 26 files by name and size and copies them into
`dirtdash/extracted/`; the game's 68020 program is the pair of chips
`dt2vera.1` / `dt2vera.2` inside the zip's `dirtdasha/` folder. The
installed packages and the Windows version do this themselves the first time
the game starts, from `dirtdash.zip` in the game's `roms` folder. The
Japanese set inside the zip (`dirtdashj/`, chips named `dt1vera.*`) is a different program and is not
used.

The two program chips, `dt2vera.1` and `dt2vera.2`, are taken from `dirtdasha/` in the zip
first, then from the top level of the zip, then from any other folder in it, and a separate
`dirtdasha.zip` in the same folder is read too (MAME sets that are not merged keep them
there). If a chip is in the zip but cannot be used, the message says why: the wrong size
(a different or damaged dump), a damaged zip, or a compression method other than Deflate or
Store (re-zip it). Compare your files with [ROM_CHECKSUMS.md](ROM_CHECKSUMS.md).

## Ace Driver

Ace Driver needs two MAME sets: `acedrive.zip` (Ace Driver: Racing Evolution, World AD2) and `namcoc74.zip` (the sound CPU's BIOS,
`c74.bin` -- the same file Rave Racer uses). Give them to the build script:

```bash
acedriver/build.sh /path/to/acedrive.zip /path/to/namcoc74.zip
```

It checks all 25 chip files by name, size and CRC and copies them into `acedriver/extracted/`; the 68020 program is the four chips
`ad2_prguu.6d` .. `ad2_prgll.4d`, interleaved into `acedriver_main.bin`. The installed packages and the Windows version do this themselves the
first time the game starts, from the two zips in the game's `roms` folder. Compare your files with [ROM_CHECKSUMS.md](ROM_CHECKSUMS.md).

## Cyber Commando

Cyber Commando needs two MAME sets: `cybrcomm.zip` (Cyber Commando, Japan CY1) and `namcoc74.zip` (the sound CPU's BIOS, `c74.bin` --
the same file Rave Racer and Ace Driver use). It is the same board as Ace Driver and uses Ace Driver's unpacker:

```bash
cybrcomm/build.sh /path/to/cybrcomm.zip /path/to/namcoc74.zip
# (it runs: python3 acedriver/tools/setup_roms.py --game cybrcomm <zips>)
```

It checks all 29 chip files by name, size and CRC and copies them into `cybrcomm/extracted/`; the 68020 program is the four chips
`cy1prguu.6d` .. `cy1prgll.4d`, interleaved into `cybrcomm_main.bin`. The installed packages and the Windows version do this themselves
the first time the game starts, from the two zips in the game's `roms` folder. Compare your files with [ROM_CHECKSUMS.md](ROM_CHECKSUMS.md).

## Cyber Sled

Cyber Sled (Namco System 21) needs three MAME sets: `cybsled.zip` (Cyber Sled, World CY2), `namcoc67.zip` (`c67.bin`, the internal
ROM of the C67 3D DSPs) and `namcoc68.zip` (`c68.bin`, the internal ROM of the C68 I/O controller). Give them to the build script:

```bash
cybsled/build.sh /path/to/cybsled.zip /path/to/namcoc67.zip /path/to/namcoc68.zip
```

It checks all 30 chip files by name, size and CRC and copies them into `cybsled/extracted/`; the two 68000 programs are the chip pairs
`cy2-mpr-u.3j` / `cy2-mpr-l.1j` (master) and `cy2-spr-u.6c` / `cy2-spr-l.4c` (slave). The installed packages and the Windows version
do this themselves the first time the game starts, from the three zips in the game's `roms` folder. The first build compiles the two
68000 programs one at a time (about 3 GB of RAM for the larger). Compare your files with [ROM_CHECKSUMS.md](ROM_CHECKSUMS.md).
