#!/bin/bash
# Build Cyber Sled.
#
#   ./build.sh /path/to/cybsled.zip /path/to/namcoc67.zip /path/to/namcoc68.zip     first time
#   ./build.sh                                                                        later: just rebuild
#
# The ROM sets are MAME's cybsled.zip (Cyber Sled, World CY2), namcoc67.zip (c67.bin, the C67 DSP's internal ROM) and
# namcoc68.zip (c68.bin, the C68 I/O MCU's internal ROM).
# Without paths this looks in this folder, the folder above, roms/ and ~/Downloads. Missing tools? Run ../install-deps.sh.
set -e
ARGS=()
for a in "$@"; do ARGS+=("$(realpath -m -- "$a")"); done
set -- "${ARGS[@]}"
cd "$(dirname "$0")"

fail() { printf '\n*** %s\n\n' "$*" >&2; exit 1; }

for t in gcc cmake make pkg-config python3; do
    command -v "$t" >/dev/null 2>&1 || fail "'$t' is not installed. Run ../install-deps.sh first."
done
pkg-config --exists sdl2 || fail "SDL2 development files are missing. Run ../install-deps.sh first."

if [ $# -gt 0 ]; then
    python3 tools/setup_roms.py "$@" || exit 1
elif ! python3 tools/setup_roms.py --check; then
    ZIPS=()
    for z in cybsled.zip namcoc67.zip namcoc68.zip; do
        for d in . .. roms ../roms "$HOME/Downloads"; do
            if [ -f "$d/$z" ]; then ZIPS+=("$d/$z"); echo "Found ROMs: $d/$z"; break; fi
        done
    done
    [ ${#ZIPS[@]} -gt 0 ] && python3 tools/setup_roms.py "${ZIPS[@]}" || true
    python3 tools/setup_roms.py --check || fail \
"No ROMs yet. Run:   ./build.sh /path/to/cybsled.zip /path/to/namcoc67.zip /path/to/namcoc68.zip
    (MAME's Cyber Sled, C67 and C68 sets. They are not included: you need your own copy.)"
fi

# The game's two 68000 programs are big generated C (gen/cm_lifted.c, 19 MB, about 3 GB of RAM for gcc; gen/cs_lifted.c): they are
# compiled one at a time (-j1), so the first build takes several minutes.
if [ -f build/CMakeCache.txt ] && ! grep -q 'CMAKE_C_COMPILER:[A-Z]*=.*gcc' build/CMakeCache.txt; then
    rm -rf build
fi
cmake -S . -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target cs21 -j1

printf '\nDone! From the top folder, start the game with:   ./launch.sh cs\n'
printf '  5 = coin   1/Enter = start   arrows = drive   E/D/S/F + I/K/J/L = the two levers   Z = gun   X = missile   C = view   Esc = menu\n\n'
