"""Where a Super System 22 game's sound program bytes come from: its data ROM (an asset) --
pr1data.8k (Prop Cycle) or tw1data.8k (Tokyo Wars), the same S22-BIOS ver1.41 chip. SND_GAME=pc|tw and
SND_ROMDIR select it (tools/gen/snd_translate.py --game/--roms set them).
Prop Cycle: 
The Super System 22 board maps it at 0x200000 and mirrors its first 64 KB at
0x000000, so the BIOS/driver at 0xC000-0xFFFF is pr1data.8k[0xC000..]
(src/mcu_sound.c bus_r). Coverage over attract, all four courses, the full coin
loop, ADVANCED/story and the ending shows no code executed from RAM."""
import os
GAME = os.environ.get('SND_GAME', 'pc')
ROMDIR = os.environ.get('SND_ROMDIR') or os.environ.get('PC_ROMDIR', 'extracted')
_data = open(f"{ROMDIR}/{ {'pc': 'pr1data.8k', 'tw': 'tw1data.8k'}[GAME] }", 'rb').read()
def source(addr, n):
    """(bytes, is_ram) for n bytes at a sound-CPU address; (None, None) if no ROM backs it"""
    if 0xC000 <= addr < 0x10000: return _data[addr: addr + n], False
    # 0x200000-0x27FFFF is the same file, but it is sample/sequence DATA: the oracle
    # executed nothing there over the full coverage. Backing it here let the static
    # walk decode 23,452 bogus "instructions" out of it -- a 80k-line switch that
    # OOM-killed cc1. Code reached there now traps loudly instead.
    return None, None
