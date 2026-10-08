#!/usr/bin/env python3
"""Put Cyber Sled's ROM files where the game looks for them and build its two 68000 program images.

    python3 tools/setup_roms.py /path/to/cybsled.zip /path/to/namcoc67.zip /path/to/namcoc68.zip
    python3 tools/setup_roms.py /path/to/folder-with-the-zips  (a folder holding the three zips)
    python3 tools/setup_roms.py --check                         exit 0 if extracted/ is already complete (build.sh)

Set: MAME `cybsled` (Cyber Sled CY2, World), namco/namcos21_c67.cpp -- Namco System 21 (NOT System 22): two 68000s (master +
slave), five C67 (TMS320C25) DSPs, a 6809 + YM2151 + C140 sound board, a C68 (M37450) I/O MCU, C355 sprites. The cybsleda/ files in
the zip are the CY1 set and are not used.

Every chip is checked by name, size AND CRC32 (MAME -listxml cybsled / namcoc67 / namcoc68) before anything is written.
Builds (MAME ROM_LOAD16_BYTE: the 'u' chip is the EVEN byte of each word, the 'l' chip the ODD byte):
  cybsled_master.bin  1 MB  master 68000 program (cy2-mpr-u/l)
  cybsled_slave.bin   1 MB  slave 68000 program  (cy2-spr-u/l)
  cybsled_data.bin    1 MB  the shared data ROM   (cy1-data-u/l)
  cybsled_edata.bin   1 MB  the extra data ROM    (cy1-edata0-u/l)
The other chips are written as they are.
"""
import os, sys, zlib, zipfile

# (region, name, offset in its region, size, crc32) -- from MAME -listxml cybsled
CHIPS = [
    ("maincpu", "cy2-mpr-u.3j", 0x0, 0x80000, 0xb35a72bc),
    ("maincpu", "cy2-mpr-l.1j", 0x1, 0x80000, 0xc4a25919),
    ("slave", "cy2-spr-u.6c", 0x0, 0x80000, 0x575a422d),
    ("slave", "cy2-spr-l.4c", 0x1, 0x80000, 0x4066291a),
    ("audiocpu", "cy1-snd0.8j", 0x0, 0x20000, 0x3dddf83b),
    ("c355spr", "cy1-obj0.5s", 0x0, 0x80000, 0x5ae542d5),
    ("c355spr", "cy1-obj1.5x", 0x1, 0x80000, 0x4aae3eff),
    ("c355spr", "cy1-obj2.3s", 0x2, 0x80000, 0xd64ec4c3),
    ("c355spr", "cy1-obj3.3x", 0x3, 0x80000, 0x3d1f7168),
    ("c355spr", "cy1-obj4.4s", 0x200000, 0x80000, 0x57904076),
    ("c355spr", "cy1-obj5.4x", 0x200001, 0x80000, 0x0e11ca47),
    ("c355spr", "cy1-obj6.2s", 0x200002, 0x80000, 0x7748b485),
    ("c355spr", "cy1-obj7.2x", 0x200003, 0x80000, 0xb6eb6ad2),
    ("data", "cy1-data-u.3a", 0x0, 0x80000, 0x570da15d),
    ("data", "cy1-data-l.1a", 0x1, 0x80000, 0x9cf96f9e),
    ("edata", "cy1-edata0-u.3b", 0x0, 0x80000, 0x77452533),
    ("edata", "cy1-edata0-l.1b", 0x1, 0x80000, 0xe812e290),
    ("namcos21dsp_c67:point24", "cy1-poi-h1.2f", 0x1, 0x80000, 0xeaf8bac3),
    ("namcos21dsp_c67:point24", "cy1-poi-lu1.2k", 0x2, 0x80000, 0xc544a8dc),
    ("namcos21dsp_c67:point24", "cy1-poi-ll1.2n", 0x3, 0x80000, 0x30acb99b),
    ("namcos21dsp_c67:point24", "cy1-poi-h2.2j", 0x200001, 0x80000, 0x4079f342),
    ("namcos21dsp_c67:point24", "cy1-poi-lu2.2l", 0x200002, 0x80000, 0x61d816d4),
    ("namcos21dsp_c67:point24", "cy1-poi-ll2.2p", 0x200003, 0x80000, 0xfaf09158),
    ("c140", "cy1-voi0.12b", 0x0, 0x80000, 0x99d7ce46),
    ("c140", "cy1-voi1.12c", 0x100000, 0x80000, 0x2b335f06),
    ("c140", "cy1-voi2.12d", 0x200000, 0x80000, 0x10cd15f0),
    ("c140", "cy1-voi3.12e", 0x300000, 0x80000, 0xc902b4a4),
    ("nvram", "cybsled.nv", 0x0, 0x2000, 0xaa18bf9e),
]
BIOS = {  # zip -> (name, size, crc32)
    "namcoc67.zip": ("c67.bin", 0x2000, 0x6bd8988e),   # the C67 (TMS320C25) DSP's internal ROM
    "namcoc68.zip": ("c68.bin", 0x8000, 0xca64550a),   # the C68 (M37450) I/O MCU's internal ROM
}
PAIRS = {"cybsled_master.bin": ("cy2-mpr-u.3j", "cy2-mpr-l.1j"), "cybsled_slave.bin": ("cy2-spr-u.6c", "cy2-spr-l.4c"),
         "cybsled_data.bin": ("cy1-data-u.3a", "cy1-data-l.1a"), "cybsled_edata.bin": ("cy1-edata0-u.3b", "cy1-edata0-l.1b")}

# for tools/rom_checksums.py (the shared checksum lister): every chip by name and size, and a zip's top-level files
REQUIRED = {c[1]: c[3] for c in CHIPS}
REQUIRED.update({n: sz for n, sz, _ in BIOS.values()})


def from_zip(path):
    out = {}
    with zipfile.ZipFile(path) as zf:
        for n in zf.namelist():
            if "/" not in n: out[n] = zf.read(n)
    return out


def find_zips(args):
    zips = []
    for a in args:
        if os.path.isdir(a):
            zips += [os.path.join(a, z) for z in ("cybsled.zip", "namcoc67.zip", "namcoc68.zip")]
        else:
            zips.append(a)
    return {os.path.basename(z): z for z in zips if os.path.exists(z)}

def read_all(zips):
    files = {}
    for z in zips.values():
        with zipfile.ZipFile(z) as zf:
            for n in zf.namelist():
                if "/" not in n: files.setdefault(n, zf.read(n))
    return files

def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    args = sys.argv[1:]
    if args == ["--check"]:
        out = os.path.join(here, "extracted")
        ok = all(os.path.isfile(os.path.join(out, n)) and os.path.getsize(os.path.join(out, n)) == sz for n, sz in REQUIRED.items())
        sys.exit(0 if ok else 1)
    if not args and os.path.isdir("/storage02/roms/mame"):
        args = ["/storage02/roms/mame"]                  # the authors' ROM folder
    if not args:
        print(__doc__.strip()); sys.exit(2)
    zips = find_zips(args)
    files = read_all(zips)
    want = [(c[1], c[3], c[4]) for c in CHIPS] + list(BIOS.values())
    bad = []
    for name, size, crc in want:
        b = files.get(name)
        if b is None: bad.append(f"{name}: missing")
        elif len(b) != size: bad.append(f"{name}: size {len(b):#x}, want {size:#x}")
        elif zlib.crc32(b) & 0xffffffff != crc: bad.append(f"{name}: crc {zlib.crc32(b) & 0xffffffff:08x}, want {crc:08x}")
    if bad:
        print("ROM set incomplete or wrong:\n  " + "\n  ".join(bad)); sys.exit(1)
    out = os.path.join(here, "extracted"); os.makedirs(out, exist_ok=True)
    for name, _, _ in want:
        open(os.path.join(out, name), "wb").write(files[name])
    for dst, (u, l) in PAIRS.items():
        a, b = files[u], files[l]
        img = bytearray(2 * len(a)); img[0::2] = a; img[1::2] = b
        open(os.path.join(here, dst), "wb").write(img)
        ssp = int.from_bytes(img[0:4], "big"); pc = int.from_bytes(img[4:8], "big")
        print(f"{dst}: {len(img):#x} bytes  (vectors: SSP {ssp:08X}  PC {pc:08X})")
    print(f"{len(want)} chips CRC-checked -> {out}")

if __name__ == "__main__":
    main()
