"""Where the C74 sound program's bytes come from: the ROM files (assets)."""
import os
ROMDIR = os.environ.get('RR_ROMDIR', 'extracted')
# Per game (System 22 C74 board), from the environment; the defaults are Rave Racer's:
#   SND_DATA     the data ROM file                          (rv1data.6r; Ace Driver ad1data.6r)
#   SND_RAMCODE  LO:HI:BASE -- MCU RAM LO..HI holds the data ROM byte at (addr - BASE), copied there by the BIOS
#                (BA74:C000:BA74; Ace Driver BB00:C000:BA7D, measured against its shared RAM)
#   SND_UPLOAD   LO:HI:OFF -- MCU RAM LO..HI holds the 68K program byte at OFF + (addr - LO), uploaded by the 68K
#                (4200:BA74:419E; "none" for a game whose 68K uploads no sound code -- Ace Driver)
#   SND_PRG      the four 68K program chips (uu,um,lm,ll), comma-separated (Rave Racer's rv2_prg*)
_bios = open(f'{ROMDIR}/c74.bin', 'rb').read()
_data = open(f"{ROMDIR}/{os.environ.get('SND_DATA', 'rv1data.6r')}", 'rb').read()
_rc = [int(x, 16) for x in os.environ.get('SND_RAMCODE', 'BA74:C000:BA74').split(':')]
_up = os.environ.get('SND_UPLOAD', '4200:BA74:419E')
_up = None if _up == 'none' else [int(x, 16) for x in _up.split(':')]
def _main_program():
    """The 2 MB 68020 program, byte-interleaved from the four program chips
    (MAME's order: uu, um, lm, ll) -- built here so a build needs only
    the ROM files."""
    names = os.environ.get('SND_PRG', 'rv2_prguub.6d,rv2_prgumb.8d,rv2_prglmb.2d,rv2_prgllb.4d').split(',')
    chips = [open(f'{ROMDIR}/{n}', 'rb').read() for n in names]
    out = bytearray(4 * len(chips[0]))
    for k, c in enumerate(chips):
        out[k::4] = c
    return bytes(out)
_m68k = _main_program() if _up else None
def source(addr, n):
    """(bytes, is_ram) for n bytes at a C74 address; None if no ROM backs it"""
    if 0xC000 <= addr < 0x10000: return _bios[addr - 0xC000: addr - 0xC000 + n], False
    if 0x200000 <= addr < 0x280000: return _data[addr - 0x200000: addr - 0x200000 + n], False
    if _rc[0] <= addr < _rc[1]: return _data[addr - _rc[2]: addr - _rc[2] + n], True     # copied by the BIOS
    if _up and _up[0] <= addr < _up[1]: return _m68k[_up[2] + addr - _up[0]: _up[2] + addr - _up[0] + n], True   # uploaded by the 68K
    return None, None
