#!/usr/bin/env python3
"""Put the Dirt Dash ROM files where the game looks for them (extracted/), and
build dirtdash_main.bin, the 4 MB 68EC020 program (MAME ROM_LOAD32_WORD_SWAP:
dt2vera.2 -> the HIGH 16-bit word of each long, dt2vera.1 -> the LOW word, each
file's words byte-swapped).

    python3 tools/setup_roms.py /path/to/dirtdash.zip
    python3 tools/setup_roms.py /path/to/folder-with-the-rom-files

Accepts MAME's set dirtdash.zip (it carries the World DT2 Ver.A program under
dirtdasha/ and the Japanese DT1 Ver.A one under dirtdashj/; this game is
World DT2 Ver.A, MAME's `dirtdasha`) or a folder holding the same chip files
or the zip. Every file is checked by name and size before anything is copied,
so a wrong or incomplete set is reported instead of producing a game that
crashes later. Exits 0 when extracted/ is complete.
"""
import os
import sys
import zipfile

# name -> size in bytes. Program chips are looked up as dirtdasha/<name> in the
# zip (the Japanese set's dt1vera.* sit under dirtdashj/ and are NOT this game).
REQUIRED = {
    "c71.bin": 0x2000,
    "dt1ccrh.1d": 0x80000,
    "dt1ccrl.3d": 0x200000,
    "dt1cg0.8d": 0x200000,
    "dt1cg1.10d": 0x200000,
    "dt1cg2.12d": 0x200000,
    "dt1cg3.13d": 0x200000,
    "dt1cg4.14d": 0x200000,
    "dt1cg5.16d": 0x200000,
    "dt1cg6.18d": 0x200000,
    "dt1cg7.19d": 0x200000,
    "dt1dataa.8k": 0x80000,
    "dt1ptrl0.18k": 0x80000,
    "dt1ptrl1.16k": 0x80000,
    "dt1ptrl2.15k": 0x80000,
    "dt1ptrm0.18j": 0x80000,
    "dt1ptrm1.16j": 0x80000,
    "dt1ptrm2.15j": 0x80000,
    "dt1ptru0.18f": 0x80000,
    "dt1ptru1.16f": 0x80000,
    "dt1ptru2.15f": 0x80000,
    "dt1scg0.12f": 0x200000,
    "dt1scg1.10f": 0x200000,
    "dt1wavea.2l": 0x400000,
    "dt1waveb.1l": 0x400000,
    "dt2vera.1": 0x200000,
    "dt2vera.2": 0x200000,
}
PROGRAM_DIR = "dirtdasha/"
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
    """name -> bytes for every wanted file in the zip: top-level chips, plus the
    two program chips from dirtdasha/ (not dirtdashj/, dirtdasha's Japanese sibling)."""
    found = {}
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            fn = info.filename.lower().strip("/")
            if "/" in fn:
                if not fn.startswith(PROGRAM_DIR):
                    continue
                fn = fn[len(PROGRAM_DIR):]
                if "/" in fn:
                    continue
            if fn in REQUIRED or fn in OPTIONAL:
                found[fn] = z.read(info)
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
            if entry.lower() == "dirtdash.zip":
                found.update(from_zip(os.path.join(path, entry)))
    return found


def build_program(dest):
    """4 MB big-endian program: ROM_LOAD32_WORD_SWAP("dt2vera.1", 2), ("dt2vera.2", 0)."""
    hi = open(os.path.join(dest, "dt2vera.2"), "rb").read()
    lo = open(os.path.join(dest, "dt2vera.1"), "rb").read()
    prog = bytearray(4 * len(hi) // 2)
    for i in range(0, len(hi), 2):
        j = 2 * i
        prog[j], prog[j + 1] = hi[i + 1], hi[i]          # word-swapped high half
        prog[j + 2], prog[j + 3] = lo[i + 1], lo[i]      # word-swapped low half
    return prog


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
        print("You need MAME's Dirt Dash set 'dirtdash' (with the World DT2 Ver.A program, 'dirtdasha').")
        return 1

    os.makedirs(DEST, exist_ok=True)
    copied = 0
    for name, data in found.items():
        if name in OPTIONAL and len(data) != OPTIONAL[name]:
            continue
        with open(os.path.join(DEST, name), "wb") as f:
            f.write(data)
        copied += 1
    with open(os.path.join(os.path.dirname(DEST), "dirtdash_main.bin"), "wb") as f:
        f.write(build_program(DEST))
    print(f"ROMs OK: {copied} files copied into extracted/, dirtdash_main.bin built")
    return 0


if __name__ == "__main__":
    sys.exit(main())
