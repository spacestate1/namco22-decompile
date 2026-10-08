#!/usr/bin/env python3
"""Put Ace Driver's ROM files where the game looks for them and build its 68020 program image.

    python3 tools/setup_roms.py [--game acedrive|victlap] /path/to/acedrive.zip [/path/to/namcoc74.zip]
    python3 tools/setup_roms.py --game victlap /path/to/folder-with-the-zips
    python3 tools/setup_roms.py [--game ...] --check      exit 0 if the destination is already complete (build.sh)

Sets (MAME `namcos22.cpp`, System 22 -- the same board as Rave Racer and Ridge Racer):
  acedrive  Ace Driver: Racing Evolution (World, AD2, 94/10/20)  -> extracted/
  victlap   Ace Driver: Victory Lap (World, ADV2 Ver.B, 96/05/21) -> extracted_victlap/
  cybrcomm  Cyber Commando (Japan, CY1, 94/10/14)                  -> ../cybrcomm/extracted/

Every chip is checked by name, size AND CRC32 before anything is written, so a wrong or incomplete set is
reported instead of producing a game that crashes later. c74.bin (the sound MCU BIOS) comes from namcoc74.zip;
the DSP BIOS (c71.bin) is built into the engine and not needed. Builds <game>_main.bin: the 2 MB program, the four
ROM_LOAD32_BYTE chips interleaved (prgll -> byte 3, prglm -> 2, prgum -> 1, prguu -> 0 of each long).
Exits 0 when the destination is complete.
"""
import os
import sys
import zlib
import zipfile

# (region, name, offset in its region, size, crc32) -- generated from MAME's ROM_START blocks
GAMES = {
    "acedrive": [
        ("maincpu", "ad2_prgll.4d", 0x3, 0x80000, 0x808c5ff8),
        ("maincpu", "ad2_prglm.2d", 0x2, 0x80000, 0x5f726a10),
        ("maincpu", "ad2_prgum.8d", 0x1, 0x80000, 0xd5042d6e),
        ("maincpu", "ad2_prguu.6d", 0x0, 0x80000, 0x86d4661d),
        ("mcu", "ad1data.6r", 0x0, 0x80000, 0x82024f74),
        ("textile", "ad1cg0.1a", 0x800000, 0x200000, 0xfaaa1ee2),
        ("textile", "ad1cg1.2a", 0xa00000, 0x200000, 0x1aab1eb7),
        ("textile", "ad1cg2.3a", 0xc00000, 0x200000, 0xcdcd1874),
        ("textile", "ad1cg3.5a", 0xe00000, 0x200000, 0xeffdd2cd),
        ("textilemap", "ad1ccrl.1c", 0x0, 0x200000, 0xbc3c9b12),
        ("textilemap", "ad1ccrh.2c", 0x200000, 0x80000, 0x71f44526),
        ("pointrom", "ad1potl0.5b", 0x0, 0x80000, 0xdfc7e729),
        ("pointrom", "ad1potl1.4b", 0x80000, 0x80000, 0x5914ef8e),
        ("pointrom", "ad1potm0.5c", 0x100000, 0x80000, 0x844bcd6b),
        ("pointrom", "ad1potm1.4c", 0x180000, 0x80000, 0x515cf541),
        ("pointrom", "ad1potu0.5d", 0x200000, 0x80000, 0xe0f44949),
        ("pointrom", "ad1potu1.4d", 0x280000, 0x80000, 0xf2cd2cbb),
        ("c352", "ad1wave0.10r", 0x0, 0x100000, 0xc7879a72),
        ("c352", "ad1wave1.10p", 0x200000, 0x100000, 0x69c1d41e),
        ("c352", "ad1wave2.10n", 0x100000, 0x100000, 0x365a6831),
        ("c352", "ad1wave3.10l", 0x300000, 0x100000, 0xcd8ecb0b),
        ("gamma_proms", "rr1gam.2d", 0x0, 0x100, 0xb2161bce),
        ("gamma_proms", "rr1gam.3d", 0x100, 0x100, 0xb2161bce),
        ("gamma_proms", "rr1gam.4d", 0x200, 0x100, 0xb2161bce),
    ],
    "victlap": [
        ("maincpu", "adv2_prgllb.4d", 0x3, 0x80000, 0x03ba6e42),
        ("maincpu", "adv2_prglmb.2d", 0x2, 0x80000, 0x9f4b49c0),
        ("maincpu", "adv2_prgumb.8d", 0x1, 0x80000, 0xccad3e90),
        ("maincpu", "adv2_prguub.6d", 0x0, 0x80000, 0xf3fffc41),
        ("mcu", "adv1data.6r", 0x0, 0x80000, 0x10eecdb4),
        ("textile", "adv1cg0.2a", 0x0, 0x200000, 0x13353848),
        ("textile", "adv1cg1.1c", 0x200000, 0x200000, 0x1542066c),
        ("textile", "adv1cg2.2d", 0x400000, 0x200000, 0x111f371c),
        ("textile", "adv1cg3.1e", 0x600000, 0x200000, 0xa077831f),
        ("textile", "adv1cg4.2f", 0x800000, 0x200000, 0x71abdacf),
        ("textile", "adv1cg5.1j", 0xa00000, 0x200000, 0xcd6cd798),
        ("textile", "adv1cg6.2k", 0xc00000, 0x200000, 0x94bdafba),
        ("textile", "adv1cg7.1n", 0xe00000, 0x200000, 0x18823475),
        ("textilemap", "adv1ccrl.5a", 0x0, 0x200000, 0xdd2b96ae),
        ("textilemap", "adv1ccrh.5c", 0x200000, 0x80000, 0x5719844a),
        ("pointrom", "adv1pot.l0", 0x0, 0x80000, 0x3b85b2a4),
        ("pointrom", "adv1pot.l1", 0x80000, 0x80000, 0x601d6488),
        ("pointrom", "adv1pot.l2", 0x100000, 0x80000, 0xa0323a84),
        ("pointrom", "adv1pot.m0", 0x180000, 0x80000, 0x20951aa2),
        ("pointrom", "adv1pot.m1", 0x200000, 0x80000, 0x5aed6fbf),
        ("pointrom", "adv1pot.m2", 0x280000, 0x80000, 0x00cbff92),
        ("pointrom", "adv1pot.u0", 0x300000, 0x80000, 0x6b73dd2a),
        ("pointrom", "adv1pot.u1", 0x380000, 0x80000, 0xc8788f74),
        ("pointrom", "adv1pot.u2", 0x400000, 0x80000, 0xe67f29c5),
        ("c352", "adv1wav0.10r", 0x0, 0x100000, 0xf07b2d9d),
        ("c352", "adv1wav1.10p", 0x200000, 0x100000, 0x737f3c7a),
        ("c352", "adv1wav2.10n", 0x100000, 0x100000, 0xc1a5ca5e),
        ("c352", "adv1wav3.10l", 0x300000, 0x100000, 0xfc6b8004),
        ("gamma_proms", "rr1gam.2d", 0x0, 0x100, 0xb2161bce),
        ("gamma_proms", "rr1gam.3d", 0x100, 0x100, 0xb2161bce),
        ("gamma_proms", "rr1gam.4d", 0x200, 0x100, 0xb2161bce),
    ],
    "cybrcomm": [
        ("maincpu", "cy1prgll.4d", 0x3, 0x80000, 0xb3eab156),
        ("maincpu", "cy1prglm.2d", 0x2, 0x80000, 0x884a5b0e),
        ("maincpu", "cy1prgum.8d", 0x1, 0x80000, 0xc9c4a921),
        ("maincpu", "cy1prguu.6d", 0x0, 0x80000, 0x5f22975b),
        ("mcu", "cy1data.6r", 0x0, 0x20000, 0x10d0005b),          # 128 KB, mirrored x4 in the 512 KB region
        ("textile", "cyc1cg0.1a", 0x800000, 0x200000, 0xe839b9bd),   # the board's second copies (.6a .7a .8a .9a) are identical
        ("textile", "cyc1cg1.2a", 0xa00000, 0x200000, 0x7d13993f),
        ("textile", "cyc1cg2.3a", 0xc00000, 0x200000, 0x7c464566),
        ("textile", "cyc1cg3.5a", 0xe00000, 0x200000, 0x2222e16f),
        ("textilemap", "cyc1ccrl.1c", 0x0, 0x100000, 0x1a0dc5f0),
        ("textilemap", "cyc1ccrh.2c", 0x200000, 0x80000, 0x8c4090b8),
        ("pointrom", "cyc1ptl0.5b", 0x0, 0x80000, 0xd91de03d),
        ("pointrom", "cyc1ptl1.4b", 0x80000, 0x80000, 0xe5b98021),
        ("pointrom", "cyc1ptl2.3b", 0x100000, 0x80000, 0x7ba786c6),
        ("pointrom", "cyc1ptm0.5c", 0x180000, 0x80000, 0xd454b5c6),
        ("pointrom", "cyc1ptm1.4c", 0x200000, 0x80000, 0x74fdf8cc),
        ("pointrom", "cyc1ptm2.3c", 0x280000, 0x80000, 0xb9c99a45),
        ("pointrom", "cyc1ptu0.5d", 0x300000, 0x80000, 0x4d40897f),
        ("pointrom", "cyc1ptu1.4d", 0x380000, 0x80000, 0x3bdaeeeb),
        ("pointrom", "cyc1ptu2.3d", 0x400000, 0x80000, 0xa0e73674),
        ("c352", "cy1wav0.10r", 0x0, 0x100000, 0xc6f366a2),
        ("c352", "cy1wav1.10p", 0x200000, 0x100000, 0xf30b5e37),
        ("c352", "cy1wav2.10n", 0x100000, 0x100000, 0xb98c1ca6),
        ("c352", "cy1wav3.10l", 0x300000, 0x100000, 0x43dbac19),
        ("gamma_proms", "rr1gam.2d", 0x0, 0x100, 0xb2161bce),
        ("gamma_proms", "rr1gam.3d", 0x100, 0x100, 0xb2161bce),
        ("gamma_proms", "rr1gam.4d", 0x200, 0x100, 0xb2161bce),
        ("eeprom", "cy1eeprm.9e", 0x0, 0x2000, 0x8432c066),
    ],
}

EXTRA = {"c74.bin": 0x4000}          # the sound MCU BIOS, from namcoc74.zip (size only). The DSP BIOS c71.bin is built into the engine.
MAIN_NAME = {"acedrive": "acedriver_main.bin", "victlap": "victlap_main.bin", "cybrcomm": "../cybrcomm/cybrcomm_main.bin"}   # <dir>_main.bin: the shared tools find the image by the game directory name
DEST_NAME = {"acedrive": "extracted", "victlap": "extracted_victlap", "cybrcomm": "../cybrcomm/extracted"}   # Cyber Commando lives in its own directory
TITLE = {"acedrive": "Ace Driver: Racing Evolution (World, AD2)", "victlap": "Ace Driver: Victory Lap (World, ADV2 Ver.B)",
         "cybrcomm": "Cyber Commando (Japan, CY1)"}
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def zip_files(path):
    """name -> bytes for every file at the top level of a zip (MAME sets that carry clones keep them in subfolders)."""
    out = {}
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            if info.is_dir() or "/" in info.filename.strip("/"):
                continue
            out[info.filename.lower()] = z.read(info)
    return out


# for tools/rom_checksums.py (the shared checksum lister): the acedrive chips by name and size, and a zip's top-level files
REQUIRED = {n: sz for _, n, _, sz, _ in GAMES["acedrive"]}
REQUIRED.update(EXTRA)


def from_zip(path):
    return zip_files(path)


def collect(args):
    found = {}
    for a in args:
        src = os.path.expanduser(a)
        if not os.path.exists(src):
            raise SystemExit(f"Can't find '{src}'. Check the path.")
        if os.path.isdir(src):
            for entry in sorted(os.listdir(src)):
                p = os.path.join(src, entry)
                if entry.lower().endswith(".zip"):
                    for k, v in zip_files(p).items():
                        found.setdefault(k, v)
                elif os.path.isfile(p):
                    with open(p, "rb") as f:
                        found.setdefault(entry.lower(), f.read())
        else:
            try:
                for k, v in zip_files(src).items():
                    found.setdefault(k, v)
            except zipfile.BadZipFile:
                raise SystemExit(f"'{src}' is not a zip file (or it is damaged).")
    return found


def main():
    args = sys.argv[1:]
    game = "acedrive"
    if args[:1] == ["--game"]:
        game = args[1]
        args = args[2:]
    dest = os.path.join(ROOT, DEST_NAME.get(game, ""))
    if args == ["--check"] and game in GAMES:
        need = [(n, sz) for _, n, _, sz, _ in GAMES[game]] + list(EXTRA.items())
        ok = all(os.path.isfile(os.path.join(dest, n)) and os.path.getsize(os.path.join(dest, n)) == sz for n, sz in need)
        return 0 if ok and os.path.isfile(os.path.join(ROOT, MAIN_NAME[game])) else 1
    if game not in GAMES or not args:
        print(__doc__.strip())
        return 2
    found = collect(args)
    chips = GAMES[game]
    problems = []
    for region, name, off, size, crc in chips:
        data = found.get(name.lower())
        if data is None:
            problems.append(f"  missing:        {name}")
        elif len(data) != size:
            problems.append(f"  wrong size:     {name} ({len(data)} bytes, expected {size})")
        elif zlib.crc32(data) & 0xFFFFFFFF != crc:
            problems.append(f"  wrong CRC:      {name} ({zlib.crc32(data) & 0xFFFFFFFF:08x}, expected {crc:08x})")
    for name, size in EXTRA.items():
        data = found.get(name)
        if data is None:
            problems.append(f"  missing:        {name}" + ("  (namcoc74.zip)" if name == "c74.bin" else ""))
        elif len(data) != size:
            problems.append(f"  wrong size:     {name} ({len(data)} bytes, expected {size})")
    if problems:
        print("This ROM set can't be used:")
        print("\n".join(problems))
        print(f"You need MAME's '{game}' ({TITLE[game]}) set, and namcoc74.zip for c74.bin.")
        return 1

    os.makedirs(dest, exist_ok=True)
    names = {n for _, n, _, _, _ in chips} | set(EXTRA)
    for n in sorted(names):
        with open(os.path.join(dest, n), "wb") as f:
            f.write(found[n.lower()])
    prg = bytearray(0x200000)
    for region, name, off, size, crc in chips:
        if region == "maincpu":
            prg[off::4] = found[name.lower()]
    with open(os.path.join(ROOT, MAIN_NAME[game]), "wb") as f:
        f.write(prg)
    sp, pc = int.from_bytes(prg[0:4], "big"), int.from_bytes(prg[4:8], "big")
    print(f"ROMs OK: {len(names)} files into {os.path.basename(dest)}/, {MAIN_NAME[game]} built "
          f"(reset SSP {sp:#010x} PC {pc:#010x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
