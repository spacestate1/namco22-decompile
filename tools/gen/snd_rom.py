"""Where Prop Cycle's sound program bytes come from: pr1data.8k (an asset).
The Super System 22 board maps it at 0x200000 and mirrors its first 64 KB at
0x000000, so the BIOS/driver at 0xC000-0xFFFF is pr1data.8k[0xC000..]
(src/mcu_sound.c bus_r). Coverage over attract, all four courses, the full coin
loop, ADVANCED/story and the ending shows no code executed from RAM."""
import os
ROMDIR = os.environ.get('PC_ROMDIR', 'extracted')
_data = open(f'{ROMDIR}/pr1data.8k', 'rb').read()
def source(addr, n):
    """(bytes, is_ram) for n bytes at a sound-CPU address; (None, None) if no ROM backs it"""
    if 0xC000 <= addr < 0x10000: return _data[addr: addr + n], False
    # 0x200000-0x27FFFF is the same file, but it is sample/sequence DATA: the oracle
    # executed nothing there over the full coverage. Backing it here let the static
    # walk decode 23,452 bogus "instructions" out of it -- a 80k-line switch that
    # OOM-killed cc1. Code reached there now traps loudly instead.
    return None, None
