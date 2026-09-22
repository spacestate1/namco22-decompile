#!/usr/bin/env python3
"""Extract known one-shot sound effects out of the C352 wave ROM into small
standalone WAV files, so the live game (src/audio_hle.c) never needs the
16MB wave ROM at runtime -- only these few KB per effect.

This is the asset side of AUDIO_PLAN.md phase 3's documented fallback: the
M37710 sound MCU core does not exist, so effects are HLE-triggered directly
from the decompiled call site that would have sent the real MCU command
(see include/audio_hle.h). Each table entry's voice parameters were
measured against MAME's own emulation for that specific trigger -- see
tools/overnight/dump_sound_trigger.lua and AUDIO_PLAN.md phase 3's log for
how AUDIO_SFX_COIN's numbers were found. Add a new entry here only once you
have done the same measurement; do not guess sample boundaries.

Run automatically by build.sh (matching the existing ROM-extraction step in
that file) whenever sounds/ is missing or the wave ROMs are newer than the
manifest, so a first build produces playable assets with no manual step.
Can also be run by hand:

    python3 tools/extract_sound_assets.py [--rom-dir extracted] [--out sounds]
"""
import argparse
import json
import os
import struct
import sys

ROM_SIZE = 0x1000000
WAVE_SIZE = 0x400000
WAVEB_OFF = 0x800000

C352_CLOCK = 24576000
C352_DIVIDER = 288
C352_HZ = C352_CLOCK / C352_DIVIDER  # 85333.33..

FLG_MULAW = 0x0008

# name -> (wave_bank, wave_start, wave_end, freq, flags)
# wave_end is INCLUSIVE (the real chip plays that byte, then stops --
# fetch_sample() reads the sample at the current position and only checks
# "was that wave_end" afterward).
SFX_TABLE = {
    "coin": dict(wave_bank=0x0007, wave_start=0x0362, wave_end=0x6c89,
                  freq=0x4268, flags=0x4008),

    # The gameplay voices measured off the real driver (the wind bed on
    # voices 11-14, the four-sample pedal cycle on voice 8) are NOT extracted
    # any more. Nothing played them except a fixed script standing in for the
    # driver, and that script is gone -- the M37710 core drives the chip from
    # the wave ROM directly, so these do not need pre-decoding. Their measured
    # parameters are in the git history of this file if they are wanted again.
}


def build_mulaw_table():
    """Identical construction to c352.c/c352.cpp's device_start table."""
    tab = [0] * 256
    j = 0
    for i in range(128):
        tab[i] = (j << 5) & 0xFFFF
        if i < 16:
            j += 1
        elif i < 24:
            j += 2
        elif i < 48:
            j += 4
        elif i < 100:
            j += 8
        else:
            j += 16
    for i in range(128):
        tab[i + 128] = (~tab[i]) & 0xffe0
    # sign-extend to s16
    return [v - 0x10000 if v & 0x8000 else v for v in tab]


def load_wave_rom(rom_dir):
    rom = bytearray(ROM_SIZE)
    a_path = os.path.join(rom_dir, "pr1wavea.2l")
    b_path = os.path.join(rom_dir, "pr1waveb.1l")
    with open(a_path, "rb") as f:
        data = f.read(WAVE_SIZE)
        rom[0:len(data)] = data
    with open(b_path, "rb") as f:
        data = f.read(WAVE_SIZE)
        rom[WAVEB_OFF:WAVEB_OFF + len(data)] = data
    return bytes(rom), (a_path, b_path)


def decode_sample(rom, entry, mulaw_tab):
    # LENGTH WRAPS ON THE LOW 16 BITS. c352.c advances `pos` and compares
    # `(pos & 0xFFFF) == wave_end`, so a sample whose wave_end is numerically
    # BELOW its wave_start is not empty -- it runs past the end of the bank
    # and on into the next one. The old `rom[start:end+1]` gave such a sample
    # ZERO bytes and wrote a silent WAV: the gameplay ambient bed decoded to
    # nothing until this was fixed.
    base = (entry["wave_bank"] << 16)
    start = base + entry["wave_start"]
    length = ((entry["wave_end"] - entry["wave_start"]) & 0xFFFF) + 1
    raw = rom[start:start + length]

    mulaw = bool(entry["flags"] & FLG_MULAW)
    pcm = bytearray()
    for b in raw:
        s = b - 256 if b >= 128 else b  # s8
        if mulaw:
            sample = mulaw_tab[s & 0xff]
        else:
            sample = (s << 8)
            if sample > 32767:
                sample -= 65536
        pcm += struct.pack("<h", sample)

    rate = round(entry["freq"] / 65536.0 * C352_HZ)
    return bytes(pcm), rate, len(raw)


def write_wav(path, pcm, rate):
    data_bytes = len(pcm)
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + data_bytes))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", data_bytes))
        f.write(pcm)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom-dir", default="extracted")
    ap.add_argument("--out", default="sounds")
    args = ap.parse_args()

    if not os.path.isdir(args.rom_dir):
        print(f"error: rom dir {args.rom_dir!r} not found", file=sys.stderr)
        return 1

    rom, sources = load_wave_rom(args.rom_dir)
    mulaw_tab = build_mulaw_table()
    os.makedirs(args.out, exist_ok=True)

    manifest = {"source_rom_dir": args.rom_dir, "sfx": {}}
    for name, entry in SFX_TABLE.items():
        pcm, rate, nbytes = decode_sample(rom, entry, mulaw_tab)
        out_path = os.path.join(args.out, f"sfx_{name}.wav")
        write_wav(out_path, pcm, rate)
        manifest["sfx"][name] = {
            **entry,
            "rate_hz": rate,
            "source_bytes": nbytes,
            "duration_s": round(nbytes / rate, 3),
            "file": os.path.basename(out_path),
        }
        print(f"  {name}: {nbytes} bytes @ {rate}Hz ({nbytes/rate:.3f}s) -> {out_path}")

    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"{len(SFX_TABLE)} sound asset(s) extracted to {args.out}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
