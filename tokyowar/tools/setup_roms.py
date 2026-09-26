#!/usr/bin/env python3
"""Put the Tokyo Wars ROM files where the game looks for them (extracted/), and
build tokyowar_main.bin, the 4 MB 68EC020 program (MAME ROM_LOAD32_BYTE:
tw2ver-a.4 -> byte 0, .3 -> 1, .2 -> 2, .1 -> 3 of each long).

    python3 tools/setup_roms.py /path/to/tokyowar.zip
    python3 tools/setup_roms.py /path/to/folder-with-the-rom-files

Accepts MAME's set tokyowar.zip (World, TW2 Ver.A) or a folder holding the same
chip files or the zip. Every file is checked by name and size
before anything is copied, so a wrong or incomplete set is reported instead
of producing a game that crashes later. Exits 0 when extracted/ is complete.
"""
import os
import shutil
import sys
import zipfile

# name -> size in bytes. The runtime (src/rom_loader.c, src/mcu_sound.c,
# src/audio_hle.c) reads exactly these.
REQUIRED = {
    "c71.bin": 0x2000,
    "tokyowar_defaults.nv": 0x2000,
    "tw1ccrh.1d": 0x80000,
    "tw1ccrl.3d": 0x200000,
    "tw1cg0.8d": 0x200000,
    "tw1cg1.10d": 0x200000,
    "tw1cg2.12d": 0x200000,
    "tw1cg3.13d": 0x200000,
    "tw1cg4.14d": 0x200000,
    "tw1cg5.16d": 0x200000,
    "tw1cg6.18d": 0x200000,
    "tw1cg7.19d": 0x200000,
    "tw1data.8k": 0x80000,
    "tw1ptrl0.18k": 0x80000,
    "tw1ptrl1.16k": 0x80000,
    "tw1ptrl2.15k": 0x80000,
    "tw1ptrl3.14k": 0x80000,
    "tw1ptrm0.18j": 0x80000,
    "tw1ptrm1.16j": 0x80000,
    "tw1ptrm2.15j": 0x80000,
    "tw1ptrm3.14j": 0x80000,
    "tw1ptru0.18f": 0x80000,
    "tw1ptru1.16f": 0x80000,
    "tw1ptru2.15f": 0x80000,
    "tw1ptru3.14f": 0x80000,
    "tw1scg0.12f": 0x200000,
    "tw1scg1.10f": 0x200000,
    "tw1scg2.8f": 0x200000,
    "tw1scg3.7f": 0x200000,
    "tw1wavea.2l": 0x400000,
    "tw2ver-a.1": 0x100000,
    "tw2ver-a.2": 0x100000,
    "tw2ver-a.3": 0x100000,
    "tw2ver-a.4": 0x100000,
}
OPTIONAL = {}


HERE = os.path.dirname(os.path.abspath(__file__))
DEST = os.path.join(os.path.dirname(HERE), "extracted")


def have_complete(folder):
    for name, size in REQUIRED.items():
        p = os.path.join(folder, name)
        if not os.path.isfile(p) or os.path.getsize(p) != size:
            return False
    return True


def from_zip(path):
    """name -> bytes for every wanted file in the zip (top level only;
    MAME's tokyowar.zip also carries the Japanese set under tokyowarj/,
    which is a different program and must not be picked up)."""
    found = {}
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            if info.is_dir() or "/" in info.filename.strip("/"):
                continue
            name = info.filename.lower()
            if name in REQUIRED or name in OPTIONAL:
                found[name] = z.read(info)
    return found


def from_folder(path):
    found = {}
    for entry in os.listdir(path):
        name = entry.lower()
        if name in REQUIRED or name in OPTIONAL:
            with open(os.path.join(path, entry), "rb") as f:
                found[name] = f.read()
    if not all(n in found for n in REQUIRED):
        # A folder that holds the zip rather than the chips.
        for entry in sorted(os.listdir(path)):
            if entry.lower() in ("tokyowar.zip", "namcoc71.zip"):
                found.update(from_zip(os.path.join(path, entry)))
    return found


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__.strip())
        return 2
    if args[0] == "--check":            # is extracted/ already complete?
        return 0 if have_complete(DEST) else 1
    found = {}
    for a in args:
        src = os.path.expanduser(a)
        if not os.path.exists(src):
            print(f"Can't find '{src}'. Check the path.")
            return 1
        try:
            found.update(from_folder(src) if os.path.isdir(src) else from_zip(src))
        except zipfile.BadZipFile:
            print(f"'{src}' is not a zip file (or it is damaged).")
            return 1

    missing = [n for n in REQUIRED if n not in found]
    wrong = [n for n in REQUIRED if n in found and len(found[n]) != REQUIRED[n]]
    if missing or wrong:
        print("This ROM set can't be used:")
        for n in missing:
            print(f"  missing:        {n}")
        for n in wrong:
            print(f"  wrong size:     {n} ({len(found[n])} bytes, expected {REQUIRED[n]})")
        if "c71.bin" in missing:
            print("  c71.bin is the C71 DSP BIOS. Some MAME sets carry it inside the game's zip; others keep it in a separate")
            print("  'namcoc71.zip' -- add that zip to this command (it can be listed after the game's zip).")
        print("You need MAME's Tokyo Wars (World, TW2 Ver.A) set 'tokyowar'.")
        return 1

    os.makedirs(DEST, exist_ok=True)
    copied = 0
    for name, data in found.items():
        if name in OPTIONAL and len(data) != OPTIONAL[name]:
            continue
        with open(os.path.join(DEST, name), "wb") as f:
            f.write(data)
        copied += 1
    lanes = [open(os.path.join(DEST, "tw2ver-a.%d" % k), "rb").read() for k in (4, 3, 2, 1)]
    prog = bytearray(4 * len(lanes[0]))
    for l in range(4): prog[l::4] = lanes[l]
    with open(os.path.join(os.path.dirname(DEST), "tokyowar_main.bin"), "wb") as f:
        f.write(prog)
    print(f"ROMs OK: {copied} files copied into extracted/, tokyowar_main.bin built")
    return 0


if __name__ == "__main__":
    sys.exit(main())
