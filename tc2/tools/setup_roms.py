#!/usr/bin/env python3
"""setup_roms.py [ZIP-or-FOLDER ...] -- unpack MAME's timecrs2.zip (and namco_tssio.zip, if the I/O board's program is there) into
extracted/, every chip checked by name, size and MAME's CRC32. A source build needs this before building: the two H8 programs are
translated from the ROM at build time. (The game itself does the same unpack on its first start: src/tc2_rom.c.)
  --check   exit 0 if extracted/ is already complete"""
import os, sys, zipfile, zlib
CHIPS = {"tss3verb.1": (0x200000, 0x6e3f232b), "tss3verb.2": (0x200000, 0xc7be691f), "tss1vera.3": (0x80000, 0x41e41994),
         "tssioprog.ic3": (0x40000, 0xedad4538), "tss1ccrh.7e": (0x200000, 0xf998de1a), "tss1ccrl.7f": (0x400000, 0x3a325fe7),
         "tss1cgll.4m": (0x800000, 0x18433aaa), "tss1cglm.4k": (0x800000, 0x669974c2), "tss1cgum.4j": (0x800000, 0xc22739e1),
         "tss1cguu.4f": (0x800000, 0x76924e04), "tss1mtah.2j": (0x800000, 0x697c26ed), "tss1mtal.2h": (0x800000, 0xbfc79190),
         "tss1mtbh.2m": (0x800000, 0x82582776), "tss1mtbl.2f": (0x800000, 0xe648bea4), "tss1pt0h.7a": (0x400000, 0xcdbe0ba8),
         "tss1pt0l.7c": (0x400000, 0x896f0fb4), "tss1pt1h.5a": (0x400000, 0x63647596), "tss1pt1l.5c": (0x400000, 0x5a09921f),
         "tss1pt2h.4a": (0x400000, 0x9b06e22d), "tss1pt2l.4c": (0x400000, 0x4b230d79), "tss1waveh.2a": (0x800000, 0x5c8758b4),
         "tss1wavel.2c": (0x800000, 0xdeaead26)}
DEST = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "extracted")
def complete():
    return all(os.path.isfile(os.path.join(DEST, n)) and os.path.getsize(os.path.join(DEST, n)) == s for n, (s, _) in CHIPS.items())
args = sys.argv[1:]
if args == ["--check"]: sys.exit(0 if complete() else 1)
if not args: args = [p for p in ("timecrs2.zip", "namco_tssio.zip", "roms", os.path.expanduser("~/Downloads")) if os.path.exists(p)]
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
    paths = [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.lower() in ("timecrs2.zip", "namco_tssio.zip")] if os.path.isdir(a) else [a]
    for p in paths:
        try:
            with zipfile.ZipFile(p) as z:
                for i in z.infolist():
                    if not i.is_dir(): take(i.filename, z.read(i))
        except (zipfile.BadZipFile, OSError) as e: print(f"{p}: {e}")
if complete(): print(f"ROMs ready in {DEST} ({len(CHIPS)} chips)"); sys.exit(0)
print("Still missing: " + ", ".join(n for n in CHIPS if not os.path.isfile(os.path.join(DEST, n))))
print("Give the path to MAME's timecrs2.zip (Time Crisis II, US TSS3 Ver. B):  python3 tools/setup_roms.py /path/to/timecrs2.zip"); sys.exit(1)
