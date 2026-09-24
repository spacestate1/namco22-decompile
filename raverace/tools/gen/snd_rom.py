"""Where the C74 sound program's bytes come from: the ROM files (assets)."""
import os
ROMDIR = os.environ.get('RR_ROMDIR', 'extracted')
_bios = open(f'{ROMDIR}/c74.bin', 'rb').read()
_data = open(f'{ROMDIR}/rv1data.6r', 'rb').read()
def _main_program():
    """The 2 MB 68020 program, byte-interleaved from the four rv2_prg chips
    (MAME's order: uub, umb, lmb, llb) -- built here so a build needs only
    the ROM files."""
    chips = [open(f'{ROMDIR}/{n}', 'rb').read() for n in
             ('rv2_prguub.6d', 'rv2_prgumb.8d', 'rv2_prglmb.2d', 'rv2_prgllb.4d')]
    out = bytearray(4 * len(chips[0]))
    for k, c in enumerate(chips):
        out[k::4] = c
    return bytes(out)
_m68k = _main_program()
def source(addr, n):
    """(bytes, is_ram) for n bytes at a C74 address; None if no ROM backs it"""
    if 0xC000 <= addr < 0x10000: return _bios[addr - 0xC000: addr - 0xC000 + n], False
    if 0x200000 <= addr < 0x280000: return _data[addr - 0x200000: addr - 0x200000 + n], False
    if 0xBA74 <= addr < 0xC000: return _data[addr - 0xBA74: addr - 0xBA74 + n], True     # copied by the BIOS
    if 0x4200 <= addr < 0xBA74: return _m68k[0x419E + addr - 0x4200: 0x419E + addr - 0x4200 + n], True   # uploaded by the 68K
    return None, None
