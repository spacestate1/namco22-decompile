#!/usr/bin/env python3
"""setup_roms.py [ZIP-or-FOLDER ...] -- unpack MAME's aking.zip (Angler King) into
extracted/, every chip checked by name, size and MAME's CRC32. A source build needs this before building: the two H8 programs are
translated from the ROM at build time. The I/O board's H8/3334 program is taken in either revision (GitHub #89): asc3_io-c1.ic14
(b6627a0e: older sets, in aking.zip or 500gp.zip) or asc3_io-c.ic14 (2f272a7b: current sets, in namco_fca10.zip). Their code is
byte-identical; only the board's JVS name string differs, and the game refuses the newer "ASCA-3" name ("Please connect FCA-1 PCB."),
so asc3_io-c.ic14 is converted to the FCA-1 image (checked against b6627a0e) as src/ak_game.c does. Zips inside a zip are read.
  --check   exit 0 if extracted/ is already complete"""
import io, os, sys, zipfile, zlib
ID_AT, ID_ASCA3, ID_FCA1 = 0x1082, b"namco ltd.;ASCA-3;Ver2.04;JPN,Multipurpose + Rotary Encoder\0", b"namco ltd.;FCA-1;Ver1.00;JPN,Multipurpose + Rotary Encoder\0"
def fca1(d):   # the ASCA-3 dump answering as the FCA-1 board: only the identification string changes
    if d[ID_AT:ID_AT + len(ID_ASCA3)] != ID_ASCA3: return d
    return d[:ID_AT] + ID_FCA1.ljust(len(ID_ASCA3), b"\0") + d[ID_AT + len(ID_ASCA3):]
ALT = {"asc3_io-c.ic14": ("asc3_io-c1.ic14", 0x2f272a7b)}   # another MAME name for a chip: (our name, its CRC32)
CHIPS = {"ag1vera.ic2": (0x200000, 0xdc98fefb), "ag1vera.ic1": (0x200000, 0xf1a08d5c), "ag1vera.ic3": (0x80000, 0x266ac71c), "asc3_io-c1.ic14": (0x20000, 0xb6627a0e), "ag1mtah.2j": (0x800000, 0xf2d8ca9d), "ag1mtal.2h": (0x800000, 0x7facbfd4), "ag1mtbh.2m": (0x800000, 0x890bdb52), "ag1mtbl.2f": (0x800000, 0x62d771c9), "ag1cgll.4m": (0x800000, 0x9db7e939), "ag1cglm.4k": (0x800000, 0x17792dba), "ag1cgum.4j": (0x800000, 0x5dfa863d), "ag1cguu.4f": (0x800000, 0x86396786), "ag1ccrl.7f": (0x400000, 0x86bbe1f9), "ag1ccrh.7e": (0x200000, 0xabe2aab1), "ag1pt0h.7a": (0x400000, 0xb5582ca7), "ag1pt0l.7c": (0x400000, 0x10e7f54f), "ag1pt1h.5a": (0x400000, 0x25e4776a), "ag1pt1l.5c": (0x400000, 0x5d3d7099), "ag1pt2h.4a": (0x400000, 0xf0eb9012), "ag1pt2l.4c": (0x400000, 0xbf92c054), "ag1pt3h.3a": (0x400000, 0xe11a12ce), "ag1pt3l.3c": (0x400000, 0x04b475db), "ag1wavel.2c": (0x800000, 0xd7fefbd4), "ag1waveh.2a": (0x800000, 0x37a61daa)}
DEST = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "extracted")
def complete():
    return all(os.path.isfile(os.path.join(DEST, n)) and os.path.getsize(os.path.join(DEST, n)) == s for n, (s, _) in CHIPS.items())
args = sys.argv[1:]
if args == ["--check"]: sys.exit(0 if complete() else 1)
if not args: args = [p for p in (".", "roms", os.path.expanduser("~/Downloads")) if os.path.exists(p)]
os.makedirs(DEST, exist_ok=True)
got = {}
def take(name, data):
    n = name.lower().replace("\\", "/").split("/")[-1]
    ok_crcs = None
    if n in CHIPS: ok_crcs = {CHIPS[n][1]} | ({0xb6627a0e, 0x2f272a7b} if n == "asc3_io-c1.ic14" else set())
    elif n in ALT: n, ok_crcs = ALT[n][0], {ALT[n][1], CHIPS[ALT[n][0]][1]}
    if ok_crcs and n not in got:
        size = CHIPS[n][0]
        if len(data) != size: print(f"  {n}: {len(data)} bytes, expected {size} -- skipped"); return
        c = zlib.crc32(data) & 0xFFFFFFFF
        if c not in ok_crcs: print(f"  {n}: CRC32 {c:08x}, MAME's is {' or '.join(f'{x:08x}' for x in sorted(ok_crcs))} -- a different version or a bad dump, skipped"); return
        if n == "asc3_io-c1.ic14" and c == 0x2f272a7b:
            data = fca1(data)
            if zlib.crc32(data) & 0xFFFFFFFF != 0xb6627a0e: print(f"  {n}: could not convert the ASCA-3 dump -- skipped"); return
            print(f"  {n}: the ASCA-3 dump (2f272a7b) converted to the FCA-1 image (b6627a0e)")
        open(os.path.join(DEST, n), "wb").write(data); got[n] = 1
def read_zip(z, label, depth=0):
    for i in z.infolist():
        if i.is_dir(): continue
        if i.filename.lower().endswith(".zip") and depth == 0:
            try:
                with zipfile.ZipFile(io.BytesIO(z.read(i))) as inner: print(f"  reading {i.filename} inside {label}"); read_zip(inner, i.filename, 1)
            except zipfile.BadZipFile as e: print(f"  {i.filename} in {label}: {e}")
        else: take(i.filename, z.read(i))
def wanted(f):
    f = f.lower()
    return f.endswith(".zip") and (f in ("aking.zip", "namco_fca10.zip", "500gp.zip") or f.startswith("angler king"))
for a in args:
    paths = [os.path.join(a, f) for f in sorted(os.listdir(a)) if wanted(f)] if os.path.isdir(a) else [a]
    for p in paths:
        try:
            with zipfile.ZipFile(p) as z: read_zip(z, p)
        except (zipfile.BadZipFile, OSError) as e: print(f"{p}: {e}")
if complete(): print(f"ROMs ready in {DEST} ({len(CHIPS)} chips)"); sys.exit(0)
print("Still missing: " + ", ".join(n for n in CHIPS if not os.path.isfile(os.path.join(DEST, n))))
print("Give the path to MAME's aking.zip (Angler King, Japan, AG1 Ver. A) -- and namco_fca10.zip, which current MAME sets keep the I/O ROM in:\n"
      "  python3 tools/setup_roms.py /path/to/aking.zip /path/to/namco_fca10.zip"); sys.exit(1)
