#!/bin/bash
# Build Prop Cycle.
#
#   ./build.sh /path/to/propcycl.zip      first time: unpack your ROMs + build
#   ./build.sh                            later: just rebuild
#
# The ROM set is the standard MAME one, propcycl.zip (a folder with the chip
# files in it works too). If you don't pass a path, this looks for
# propcycl.zip in this folder, in roms/ and in ~/Downloads.
# Missing tools? Run ./install-deps.sh first.
set -e
cd "$(dirname "$0")"

fail() { printf '\n*** %s\n\n' "$*" >&2; exit 1; }

# --- 0. the tools -----------------------------------------------------------
for t in gcc cmake make pkg-config python3; do
    command -v "$t" >/dev/null 2>&1 || fail "'$t' is not installed. Run ./install-deps.sh first."
done
pkg-config --exists sdl2 || fail "SDL2 development files are missing. Run ./install-deps.sh first."

# --- 1. the ROMs ------------------------------------------------------------
ROM_ARG="${1:-}"
if [ -n "$ROM_ARG" ]; then
    python3 tools/setup_roms.py "$ROM_ARG" || exit 1
elif ! python3 tools/setup_roms.py --check; then
    for c in propcycl.zip roms/propcycl.zip "$HOME/Downloads/propcycl.zip"; do
        if [ -f "$c" ]; then
            echo "Found ROMs: $c"
            python3 tools/setup_roms.py "$c" || exit 1
            break
        fi
    done
    python3 tools/setup_roms.py --check || fail \
"No ROMs yet. Run:   ./build.sh /path/to/propcycl.zip
    (propcycl.zip is the MAME ROM set for Prop Cycle. It is not included:
     you need your own copy.)"
fi

# --- 2. sound effects, decoded from the wave ROMs (needs numpy) -------------
if [ ! -f sounds/manifest.json ]; then
    if python3 -c 'import numpy' 2>/dev/null; then
        echo "Extracting sound effects..."
        python3 tools/extract_sound_assets.py --rom-dir extracted --out sounds
    else
        echo "Note: python3 numpy is missing, skipping sound effects (./install-deps.sh adds it)."
    fi
fi
# Music loops only exist if you have made MAME reference captures
# (music_refs/, see tools/extract_music_assets.py). Optional.
if [ ! -f sounds/music_gameplay_course0.wav ] && [ -d music_refs ]; then
    echo "Extracting music assets..."
    python3 tools/extract_music_assets.py --captures-dir music_refs --out sounds
fi

# --- 3. compile -------------------------------------------------------------
# Always gcc: the transpiled game code relies on gcc's gnu89 leniency and
# clang rejects it. A build/ configured with another compiler is redone.
if [ -f build/CMakeCache.txt ] && ! grep -q 'CMAKE_C_COMPILER:[A-Z]*=.*gcc' build/CMakeCache.txt; then
    rm -rf build
fi
cmake -S . -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

printf '\nDone! Start the game with:   ./launch.sh prop\n'
printf '  5 = insert coin   Enter = start   arrow keys = steer   Esc = menu\n\n'
