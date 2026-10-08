#!/bin/bash
# Build Ace Driver.
#
#   ./build.sh /path/to/acedrive.zip /path/to/namcoc74.zip     first time
#   ./build.sh                                                  later: just rebuild
#
# The ROM sets are MAME's acedrive.zip (Ace Driver: Racing Evolution, World AD2) and namcoc74.zip (the sound CPU's
# BIOS, c74.bin).
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
    for z in acedrive.zip namcoc74.zip; do
        for d in . .. roms ../roms "$HOME/Downloads"; do
            if [ -f "$d/$z" ]; then ZIPS+=("$d/$z"); echo "Found ROMs: $d/$z"; break; fi
        done
    done
    [ ${#ZIPS[@]} -gt 0 ] && python3 tools/setup_roms.py "${ZIPS[@]}" || true
    python3 tools/setup_roms.py --check || fail \
"No ROMs yet. Run:   ./build.sh /path/to/acedrive.zip /path/to/namcoc74.zip
    (MAME's Ace Driver and C74 sets. They are not included: you need your own copy.)"
fi

# The game's program is big generated C (gen/rr_lifted.c); the first build takes several minutes.
if [ -f build/CMakeCache.txt ] && ! grep -q 'CMAKE_C_COMPILER:[A-Z]*=.*gcc' build/CMakeCache.txt; then
    rm -rf build
fi
cmake -S . -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target ad -j"$(nproc)"

printf '\nDone! From the top folder, start the game with:   ./launch.sh ad\n'
printf '  5 = coin (two a credit)   X = gas   Z = brake   arrows = steer   A/S = shift   V = view   Esc = menu\n\n'
