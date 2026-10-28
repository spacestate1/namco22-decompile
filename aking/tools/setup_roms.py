#!/usr/bin/env python3
"""setup_roms.py [ZIP-or-FOLDER ...] -- unpack MAME's aking.zip (Angler King) into
extracted/, every chip checked by name, size and MAME's CRC32. A source build needs this before building: the two H8 programs are
translated from the ROM at build time. The I/O board's H8/3334 program is the revision aking.zip carries, asc3_io-c1.ic14 (MAME 0.289's namco_fca10 lists asc3_io-c.ic14, CRC 2f272a7b).
  --check   exit 0 if extracted/ is already complete"""
import os, sys, zipfile, zlib
CHIPS = {"ag1vera.ic2": (0x200000, 0xdc98fefb), "ag1vera.ic1": (0x200000, 0xf1a08d5c), "ag1vera.ic3": (0x80000, 0x266ac71c), "asc3_io-c1.ic14": (0x20000, 0xb6627a0e), "ag1mtah.2j": (0x800000, 0xf2d8ca9d), "ag1mtal.2h": (0x800000, 0x7facbfd4), "ag1mtbh.2m": (0x800000, 0x890bdb52), "ag1mtbl.2f": (0x800000, 0x62d771c9), "ag1cgll.4m": (0x800000, 0x9db7e939), "ag1cglm.4k": (0x800000, 0x17792dba), "ag1cgum.4j": (0x800000, 0x5dfa863d), "ag1cguu.4f": (0x800000, 0x86396786), "ag1ccrl.7f": (0x400000, 0x86bbe1f9), "ag1ccrh.7e": (0x200000, 0xabe2aab1), "ag1pt0h.7a": (0x400000, 0xb5582ca7), "ag1pt0l.7c": (0x400000, 0x10e7f54f), "ag1pt1h.5a": (0x400000, 0x25e4776a), "ag1pt1l.5c": (0x400000, 0x5d3d7099), "ag1pt2h.4a": (0x400000, 0xf0eb9012), "ag1pt2l.4c": (0x400000, 0xbf92c054), "ag1pt3h.3a": (0x400000, 0xe11a12ce), "ag1pt3l.3c": (0x400000, 0x04b475db), "ag1wavel.2c": (0x800000, 0xd7fefbd4), "ag1waveh.2a": (0x800000, 0x37a61daa)}
DEST = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "extracted")
def complete():
    return all(os.path.isfile(os.path.join(DEST, n)) and os.path.getsize(os.path.join(DEST, n)) == s for n, (s, _) in CHIPS.items())
args = sys.argv[1:]
if args == ["--check"]: sys.exit(0 if complete() else 1)
if not args: args = [p for p in ("aking.zip", "roms", os.path.expanduser("~/Downloads")) if os.path.exists(p)]
os.makedirs(DEST, exist_ok=True)
got = {}
def take(name, data):
    n = name.lower().split("/")[-1]
    if n in CHIPS and n not in got:
        size, crc = CHIPS[n]
        if len(data) != size: print(f"  {n}: {len(data)} bytes, expected {size} -- skipped"); return
        if zlib.crc32(data) & 0xFFFFFFFF != crc: print(f"  {n}: CRC32 {zlib.crc32(data) & 0xFFFFFFFF:08x}, MAME's is {crc:08x} -- a different version or a bad dump, skipped"); return
        open(os.path.join(DEST, n), "wb").write(data); got[n] = 1
for a in args:
    paths = [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.lower() in ("aking.zip",)] if os.path.isdir(a) else [a]
    for p in paths:
        try:
            with zipfile.ZipFile(p) as z:
                for i in z.infolist():
                    if not i.is_dir(): take(i.filename, z.read(i))
        except (zipfile.BadZipFile, OSError) as e: print(f"{p}: {e}")
if complete(): print(f"ROMs ready in {DEST} ({len(CHIPS)} chips)"); sys.exit(0)
print("Still missing: " + ", ".join(n for n in CHIPS if not os.path.isfile(os.path.join(DEST, n))))
print("Give the path to MAME's aking.zip (Angler King, Japan, AG1 Ver. A):  python3 tools/setup_roms.py /path/to/aking.zip"); sys.exit(1)
