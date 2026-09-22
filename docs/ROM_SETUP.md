# Local ROM setup

**The easy way:** `./build.sh /path/to/propcycl.zip`. It runs
`tools/setup_roms.py`, which checks every file below by name and size and
copies them into `extracted/`. The rest of this page is only needed if you
want to do it by hand.

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
  c71.bin                                               # 8 KiB, optional
```

The four `pr2ver-a.*` program ROMs are enough for the decoder tests. The full
list is required for rendered 3D scenes, sprites, and model/course viewers.
`pr1data.8k` and the two wave ROMs provide the native sound path. The optional
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

For the regression suite, use the same location with:

```bash
PROPCYCL_ROM_DIR=/absolute/path/to/propcycl-roms ./tests/run_tests.sh
```
