#!/usr/bin/env python3
"""make_prog.py -- build Time Crisis 2's main program image from the ROM files (no MAME needed).
The boot image at 0x1FC00000 (4 MB) is tss3verb.2 and tss3verb.1 byte-interleaved (.2 first); the boot code copies
0x21000..0x20A56F of it to RAM 0 (KSEG0 0x80000000) and runs it there -- measured against MAME's RAM at attract frames 300/1200/3000.
  python3 tools/make_prog.py [extracted]  ->  boot.bin (the 4 MB boot image) + prog.bin (the RAM program, load at 0x80000000)"""
import sys, os
d = sys.argv[1] if len(sys.argv) > 1 else 'extracted'
a = open(os.path.join(d, 'tss3verb.1'), 'rb').read(); b = open(os.path.join(d, 'tss3verb.2'), 'rb').read()
boot = bytearray(len(a) + len(b)); boot[0::2] = b; boot[1::2] = a
open('boot.bin', 'wb').write(boot)
open('prog.bin', 'wb').write(boot[0x21000:0x20A570])
# the H8 programs: the sub-CPU's (H8/3002) ROM is stored WORD-SWAPPED (MAME: ROM_LOAD16_WORD_SWAP "tss1vera.3"); the I/O board's
# H8/3334 program as it is (tssioprog.ic3)
sv = open(os.path.join(d, 'tss1vera.3'), 'rb').read()
sub = bytearray(len(sv)); sub[0::2] = sv[1::2]; sub[1::2] = sv[0::2]
open('sub.bin', 'wb').write(sub)
open('io.bin', 'wb').write(open(os.path.join(d, 'tssioprog.ic3'), 'rb').read())
print('sub.bin %d bytes (H8/3002), io.bin (H8/3334)' % len(sub))
print('boot.bin %d bytes, prog.bin %d bytes (RAM 0x80000000..0x%08X)' % (len(boot), 0x20A570 - 0x21000, 0x80000000 + 0x20A570 - 0x21000 - 1))
