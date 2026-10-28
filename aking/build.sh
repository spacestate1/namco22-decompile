#!/bin/bash
# Build Angler King from source (Linux). It builds against the System 23 host in ../tc2 (the same tree).
#   ./build.sh /path/to/aking.zip     first time (unpacks the ROM: the two H8 programs are translated from it at build time)
#   ./build.sh                           later: just rebuild
# Needs gcc, cmake, make, pkg-config, python3, and the SDL2 / zlib / OpenGL development files. JOBS=n limits the parallel compiles
# (the game program is ~48 MB of generated C (gen/aking_lifted_NN.c): each part needs about 1 GB of gcc).
set -e
ARGS=(); for a in "$@"; do ARGS+=("$(realpath -m -- "$a")"); done; set -- "${ARGS[@]}"
cd "$(dirname "$0")"
fail() { printf '\n*** %s\n\n' "$*" >&2; exit 1; }
for t in gcc cmake make pkg-config python3; do command -v "$t" >/dev/null 2>&1 || fail "'$t' is not installed."; done
pkg-config --exists sdl2 || fail "SDL2 development files are missing (Debian/Ubuntu: libsdl2-dev, Fedora: SDL2-devel, Arch: sdl2)."
if [ $# -gt 0 ]; then python3 tools/setup_roms.py "$@" || exit 1
elif ! python3 tools/setup_roms.py --check; then python3 tools/setup_roms.py || fail "No ROMs yet. Run:   ./build.sh /path/to/aking.zip"; fi
python3 tools/make_prog.py extracted >/dev/null
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target aking -j"${JOBS:-$(nproc)}"
printf '\nDone! Start the game with:   ./launch.sh      (5 coin, the mouse / arrows aim the rod, left button / Space casts and selects, R / Left Shift reels, Esc menu)\n\n'
