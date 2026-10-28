#!/usr/bin/env python3
"""make_prog.py -- Angler King's program images from the ROM files (no MAME needed).
The boot image at 0x1FC00000 (4 MB) is ag1vera.ic2 and ag1vera.ic1 byte-interleaved (.ic2 first; MAME: ROM_LOAD16_BYTE into "user1"),
as on Time Crisis 2. It holds the NMON boot monitor ("NMON 1.0.8-sys23-19990105 P for SYSTEM23 P1") and, at 0x20000, the game as a MIPS
ELF, as on Crisis Zone; the layout below is read from the ELF's own program headers, not assumed.
  python3 tools/make_prog.py [extracted]  ->  boot.bin (4 MB at 0x1FC00000), prog.bin (text, load at 0x80000000),
                                              sub.bin (H8/3002 ag1vera.ic3, word-swapped back), io.bin (the JVS I/O board's H8/3334)"""
import sys, os, struct
d = sys.argv[1] if len(sys.argv) > 1 else 'extracted'
rd = lambda n: open(os.path.join(d, n), 'rb').read()
def unswap(raw):
    out = bytearray(len(raw)); out[0::2] = raw[1::2]; out[1::2] = raw[0::2]; return out
a, b = rd('ag1vera.ic1'), rd('ag1vera.ic2')
boot = bytearray(len(a) + len(b)); boot[0::2] = b; boot[1::2] = a
open('boot.bin', 'wb').write(boot)
E = 0x20000
assert boot[E:E + 4] == b'\x7fELF', 'no ELF at boot.bin 0x20000'
entry, phoff = struct.unpack_from('>II', boot, E + 0x18)
phnum, = struct.unpack_from('>H', boot, E + 0x2C)
load = [struct.unpack_from('>8I', boot, E + phoff + 32 * k) for k in range(phnum)]
load = [p for p in load if p[0] == 1]                       # PT_LOAD: (type, off, vaddr, paddr, filesz, memsz, flags, align)
text = load[0]
assert text[2] == 0x80000000, 'text segment not at 0x80000000'
open('prog.bin', 'wb').write(boot[E + text[1]:E + text[1] + text[4]])
open('sub.bin', 'wb').write(unswap(rd('ag1vera.ic3')))
open('io.bin', 'wb').write(rd('asc3_io-c1.ic14'))
print('boot.bin %d bytes; ELF entry %08X' % (len(boot), entry))
for p in load:
    print('  PT_LOAD vaddr %08X  boot.bin 0x%06X  filesz 0x%06X  memsz 0x%06X  %s' % (p[2], E + p[1], p[4], p[5], 'RX' if p[6] & 1 else 'RW'))
print('prog.bin %d bytes (0x80000000..0x%08X), sub.bin, io.bin' % (text[4], 0x80000000 + text[4] - 1))
