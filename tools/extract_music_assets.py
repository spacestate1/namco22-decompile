#!/usr/bin/env python3
"""Extract looping music/ambient tracks from captured MAME reference audio
into small standalone WAV files, so the live game (src/audio_hle.c) never
runs MAME or the M37710 at runtime -- only these pre-rendered loops.

Companion to tools/extract_sound_assets.py, which decodes one-shot SFX
directly from wave-ROM byte ranges (no MAME execution needed, so it can
run unattended on every build). Music can't be extracted that way: on this
hardware the C352 chip has no distinct "background music" concept -- the
game drives several voices continuously with the M37710's own firmware
sequencing them, and we have not decompiled that firmware. So instead of
guessing the composition, this captures MAME's own OWN mixed audio output
for a specific game STATE the decompiled code already knows about (attract
demo, the menu sequence, a course's gameplay, ...), and treats that
recording as the asset -- the same "measure MAME, don't hand-model" rule
this whole project follows everywhere else, applied to audio. Each track's
STATE is played by the live game's own decompiled code, exactly as
before -- this only supplies the sound for it.

**Requires a one-time MAME capture per track** (needs MAME + the built
roms/propcycl set from tools/overnight/mame_romset.sh) -- this cannot run
unattended in build.sh the way the ROM-decoded SFX can. See
CAPTURE_RECIPES below for exactly how each entry here was produced; redo
that capture before re-running this tool if a recipe changes.

Usage:
    python3 tools/extract_music_assets.py [--out sounds]
"""
import argparse
import os
import struct
import sys
import wave

import numpy as np

# name -> (capture_wav, start_s, end_s, crossfade_ms)
# start_s/end_s were chosen by eye from a per-second RMS scan of the
# capture (a silent/transient window at the very start of a state, or a
# state-transition boundary, would make a bad loop).
TRACK_TABLE = {
    "attract_demo": dict(
        capture="attract_demo_ref.wav", start_s=10.0, end_s=17.0, crossfade_ms=150,
    ),
    "menu": dict(
        capture="menu_and_gameplay_ref.wav", start_s=8.0, end_s=20.0, crossfade_ms=150,
    ),
    "gameplay_course0": dict(
        capture="menu_and_gameplay_ref.wav", start_s=58.0, end_s=75.0, crossfade_ms=150,
    ),
    "gameplay_course1": dict(
        capture="course1_gameplay_ref.wav", start_s=58.0, end_s=75.0, crossfade_ms=150,
    ),
    "gameplay_course2": dict(
        capture="course2_gameplay_ref.wav", start_s=58.0, end_s=73.0, crossfade_ms=150,
    ),
}

# How each capture in TRACK_TABLE was produced -- reproduce this (with a
# fresh -wavwrite path) before adding a new entry pointing at a new
# capture. `roms/propcycl` must exist first (tools/overnight/mame_romset.sh).
CAPTURE_RECIPES = {
    "attract_demo_ref.wav": """
        SDL_VIDEODRIVER=dummy mame propcycl -rompath roms \\
            -nothrottle -video none -skip_gameinfo -seconds_to_run 25 \\
            -wavwrite attract_demo_ref.wav
        # No scripted input -- attract runs on its own. The flyover/logo
        # (~t=0-9s) is silent; the demo-flight ambient starts ~t=10s.
    """,
    "menu_and_gameplay_ref.wav": """
        # Drives coin+start+pedal+neutral-stick the same way
        # tools/overnight/snap_menu.lua does (reused, not reinvented --
        # that script already proves out coin/start/pedal timing) to reach
        # real course-0 gameplay (state 3, sub 3) and stay there a while:
        SDL_VIDEODRIVER=dummy mame propcycl -rompath roms \\
            -autoboot_script <snap_menu.lua with its exit delay
                              'fc > gp3 + 120' widened to 'fc > gp3 + 1200'
                              so the capture keeps running well past the
                              moment gameplay actually starts> \\
            -nothrottle -video none -skip_gameinfo \\
            -wavwrite menu_and_gameplay_ref.wav
        # Menu sequence (tutorial/mode-select/stage-select) is continuous
        # audio ~t=0-54s; real gameplay begins ~t=55.7s (a measured, not
        # guessed, step up in level) and continues to the capture's end.
    """,
    "course1_gameplay_ref.wav": """
        COURSE=1 SDL_VIDEODRIVER=dummy mame propcycl -rompath roms \\
            -autoboot_script tools/overnight/capture_course_music.lua \\
            -nothrottle -video none -skip_gameinfo -seconds_to_run 90 \\
            -wavwrite course1_gameplay_ref.wav
        # capture_course_music.lua drives the SAME coin+start recipe as
        # menu_and_gameplay_ref.wav, then during STAGE SELECT (state 3,
        # sub 0/1) pulses the stick left COURSE times to step the cursor:
        # empirically confirmed against real MAME
        # (tools/overnight/probe_course_select.lua) that 0/1/2 pulses land
        # on course 0/1/2 respectively (course 3 needs a different,
        # not-yet-found path -- see CLAUDE.md's "Multi-Course Status",
        # `stage_transition_init`). Menu navigation costs the same number
        # of frames regardless of course, so gameplay begins at the same
        # fc=3339 / t=56.24s as course 0's capture -- confirmed, not
        # assumed, by the script's own [CAP] print.
    """,
    "course2_gameplay_ref.wav": """
        COURSE=2 SDL_VIDEODRIVER=dummy mame propcycl -rompath roms \\
            -autoboot_script tools/overnight/capture_course_music.lua \\
            -nothrottle -video none -skip_gameinfo -seconds_to_run 90 \\
            -wavwrite course2_gameplay_ref.wav
        # Same recipe as course1_gameplay_ref.wav above, COURSE=2 (2 stick
        # pulses during stage select).
    """,
}


def load_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2
        raw = w.readframes(w.getnframes())
        data = np.frombuffer(raw, dtype="<i2").reshape(-1, w.getnchannels()).astype(np.float64)
        return data, w.getframerate()


def crossfade_loop(mono, rate, crossfade_ms):
    """Blends the segment's tail into its head so consecutive repeats have
    no click at the seam. Equal-power (sqrt) crossfade curve."""
    n = int(rate * crossfade_ms / 1000.0)
    n = min(n, len(mono) // 4)  # never eat more than a quarter of a short clip
    if n <= 0:
        return mono

    t = np.linspace(0.0, 1.0, n)
    fade_in = np.sqrt(t)
    fade_out = np.sqrt(1.0 - t)

    head = mono[:n]
    tail = mono[-n:]
    blended = tail * fade_out + head * fade_in

    return np.concatenate([blended, mono[n:-n]])


def write_wav_mono16(path, samples, rate):
    pcm = np.clip(samples, -32768, 32767).astype("<i2").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(pcm)))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(pcm)))
        f.write(pcm)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--captures-dir", default=os.path.dirname(__file__) + "/../music_refs",
                     help="directory holding the MAME -wavwrite captures named in TRACK_TABLE")
    ap.add_argument("--out", default="sounds")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    made = 0
    for name, spec in TRACK_TABLE.items():
        cap_path = os.path.join(args.captures_dir, spec["capture"])
        if not os.path.isfile(cap_path):
            print(f"  {name}: SKIPPED -- missing capture {cap_path}\n"
                  f"    recipe:\n{CAPTURE_RECIPES.get(spec['capture'], '(undocumented)')}",
                  file=sys.stderr)
            continue

        data, rate = load_wav(cap_path)
        i0 = int(spec["start_s"] * rate)
        i1 = int(spec["end_s"] * rate)
        # Fail loudly on a bad slice instead of silently writing a
        # truncated/empty WAV: numpy slicing clamps out-of-range indices
        # rather than raising, so a capture shorter than the recipe
        # expects (or start_s/end_s transposed) would otherwise produce a
        # 0-frame or tiny asset with no error -- exactly the "gate that
        # can't run reports PASS on stale/missing data" trap this project
        # keeps hitting elsewhere. The C loader (audio_hle.c) now rejects
        # a 0-frame WAV rather than crashing on it, but this should never
        # reach that point to begin with.
        if i0 < 0 or i1 <= i0 or i1 > len(data):
            print(f"  {name}: SKIPPED -- bad slice [{spec['start_s']}s, {spec['end_s']}s) "
                  f"= samples [{i0}, {i1}) against a {len(data)}-sample ({len(data)/rate:.1f}s) "
                  f"capture {cap_path}", file=sys.stderr)
            continue
        mono = data[i0:i1].mean(axis=1)  # downmix stereo -> mono
        looped = crossfade_loop(mono, rate, spec["crossfade_ms"])
        if len(looped) < 2:
            print(f"  {name}: SKIPPED -- slice produced only {len(looped)} sample(s), "
                  f"too short to loop", file=sys.stderr)
            continue

        out_path = os.path.join(args.out, f"music_{name}.wav")
        write_wav_mono16(out_path, looped, rate)
        dur = len(looped) / rate
        print(f"  {name}: {spec['end_s']-spec['start_s']:.1f}s slice -> "
              f"{dur:.2f}s loop @ {rate}Hz -> {out_path}")
        made += 1

    print(f"{made}/{len(TRACK_TABLE)} music asset(s) extracted to {args.out}/")
    return 0 if made == len(TRACK_TABLE) else 1


if __name__ == "__main__":
    sys.exit(main())
