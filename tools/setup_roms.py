#!/usr/bin/env python3
"""Put the Prop Cycle ROM files where the game looks for them (extracted/).

    python3 tools/setup_roms.py /path/to/propcycl.zip
    python3 tools/setup_roms.py /path/to/folder-with-the-rom-files

Accepts the standard MAME set (propcycl.zip) or a folder holding the same
chip files (or holding propcycl.zip). Every file is checked by name and size
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
    "pr2ver-a.1": 0x100000, "pr2ver-a.2": 0x100000,
    "pr2ver-a.3": 0x100000, "pr2ver-a.4": 0x100000,
    "pr1ptrl0.18k": 0x80000, "pr1ptrl1.16k": 0x80000, "pr1ptrl2.15k": 0x80000,
    "pr1ptrm0.18j": 0x80000, "pr1ptrm1.16j": 0x80000, "pr1ptrm2.15j": 0x80000,
    "pr1ptru0.18f": 0x80000, "pr1ptru1.16f": 0x80000, "pr1ptru2.15f": 0x80000,
    "pr1cg0.12b": 0x200000, "pr1cg1.10d": 0x200000, "pr1cg2.12d": 0x200000,
    "pr1cg3.13d": 0x200000, "pr1cg4.14d": 0x200000, "pr1cg5.16d": 0x200000,
    "pr1cg6.18a": 0x200000, "pr1cg7.15a": 0x200000,
    "pr1ccrl.3d": 0x200000, "pr1ccrh.1d": 0x80000,
    "pr1scg0.12f": 0x200000, "pr1scg1.10f": 0x200000,
    "pr1data.8k": 0x80000,
    "pr1wavea.2l": 0x400000, "pr1waveb.1l": 0x400000,
}
# Nice to have; the game runs without it.
OPTIONAL = {"c71.bin": 0x2000}

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
    MAME's propcycl.zip also carries the Japanese set under propcyclj/,
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
            if entry.lower() == "propcycl.zip":
                return from_zip(os.path.join(path, entry))
    return found


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    if sys.argv[1] == "--check":        # is extracted/ already complete?
        return 0 if have_complete(DEST) else 1
    src = os.path.expanduser(sys.argv[1])
    if not os.path.exists(src):
        print(f"Can't find '{src}'. Check the path to your propcycl.zip.")
        return 1
    try:
        found = from_folder(src) if os.path.isdir(src) else from_zip(src)
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
        print("You need the Prop Cycle (World, PR2 Ver.A) set, MAME name 'propcycl'.")
        return 1

    os.makedirs(DEST, exist_ok=True)
    copied = 0
    for name, data in found.items():
        if name in OPTIONAL and len(data) != OPTIONAL[name]:
            continue
        with open(os.path.join(DEST, name), "wb") as f:
            f.write(data)
        copied += 1
    print(f"ROMs OK: {copied} files copied into extracted/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
