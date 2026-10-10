#!/bin/bash
# Build Time Crisis 2 from source (Linux).
#   ./build.sh /path/to/timecrs2.zip     first time (unpacks the ROM: the two H8 programs are translated from it at build time)
#   ./build.sh                           later: just rebuild
# Needs gcc, cmake, make, pkg-config, python3, and the SDL2 / zlib / OpenGL development files. JOBS=n limits the parallel compiles
# (the game program is ~100 MB of generated C: each part needs about 1 GB of gcc).
set -e
ARGS=(); for a in "$@"; do ARGS+=("$(realpath -m -- "$a")"); done; set -- "${ARGS[@]}"
cd "$(dirname "$0")"
fail() { printf '\n*** %s\n\n' "$*" >&2; exit 1; }
for t in gcc cmake make pkg-config python3; do command -v "$t" >/dev/null 2>&1 || fail "'$t' is not installed."; done
pkg-config --exists sdl2 || fail "SDL2 development files are missing (Debian/Ubuntu: libsdl2-dev, Fedora: SDL2-devel, Arch: sdl2)."
if [ $# -gt 0 ]; then python3 tools/setup_roms.py "$@" || exit 1
elif ! python3 tools/setup_roms.py --check; then python3 tools/setup_roms.py || fail "No ROMs yet. Run:   ./build.sh /path/to/timecrs2.zip"; fi
python3 tools/make_prog.py extracted >/dev/null
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target tc2 -j"${JOBS:-$(nproc)}"
printf '\nDone! Start the game with:   ./launch.sh      (5 coin, the mouse aims, left button shoots, right button / Z the pedal, Esc menu)\n\n'
