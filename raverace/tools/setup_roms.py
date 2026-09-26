#!/usr/bin/env python3
"""Put the Rave Racer ROM files where the game looks for them (extracted/).

    python3 tools/setup_roms.py /path/to/raverace.zip /path/to/namcoc74.zip
    python3 tools/setup_roms.py /path/to/raverace.zip /path/to/namcoc74.zip /path/to/namcoc71.zip   (if c71.bin is not in the others)
    python3 tools/setup_roms.py /path/to/folder-with-the-rom-files

Accepts the standard MAME sets -- raverace.zip plus namcoc74.zip, the sound
chip's BIOS (c74.bin), plus namcoc71.zip (c71.bin, the DSP BIOS) when your
raverace.zip does not carry it -- or a folder holding the same chip files or zips. Every file is checked by name and size
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
    "c71.bin": 0x2000, "c74.bin": 0x4000,
    "rr1gam.2d": 0x100, "rr1gam.3d": 0x100, "rr1gam.4d": 0x100,
    "rv1ccrh.5c": 0x80000, "rv1ccrl.5a": 0x200000,
    "rv1cg0.1a": 0x200000, "rv1cg1.1c": 0x200000, "rv1cg2.1d": 0x200000,
    "rv1cg3.1e": 0x200000, "rv1cg4.1f": 0x200000, "rv1cg5.1j": 0x200000,
    "rv1cg6.1k": 0x200000, "rv1cg7.1n": 0x200000,
    "rv1data.6r": 0x80000, "rv1eeprm.9e": 0x2000,
    "rv1potl0.5b": 0x80000, "rv1potl1.4b": 0x80000, "rv1potl2.3b": 0x80000, "rv1potl3.2b": 0x80000,
    "rv1potm0.5c": 0x80000, "rv1potm1.4c": 0x80000, "rv1potm2.3c": 0x80000, "rv1potm3.2c": 0x80000,
    "rv1potu0.5d": 0x80000, "rv1potu1.4d": 0x80000, "rv1potu2.3d": 0x80000, "rv1potu3.2d": 0x80000,
    "rv1wav0.10r": 0x100000, "rv1wav1.10p": 0x100000, "rv1wav2.10n": 0x100000, "rv1wav3.10l": 0x100000,
    "rv2_prgllb.4d": 0x80000, "rv2_prglmb.2d": 0x80000, "rv2_prgumb.8d": 0x80000, "rv2_prguub.6d": 0x80000,
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
    MAME's raverace.zip also carries the Japanese sets under raveracej/,
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
            if entry.lower() in ("raverace.zip", "namcoc74.zip", "namcoc71.zip"):
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
        print("You need MAME's Rave Racer (World, RV2 Ver.B) set 'raverace' and 'namcoc74' (c74.bin).")
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
