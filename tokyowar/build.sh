#!/bin/bash
# Build Tokyo Wars.
#
#   ./build.sh /path/to/tokyowar.zip                         first time
#   ./build.sh                                               later: just rebuild
#
# The ROM set is MAME's tokyowar.zip (World, TW2 Ver.A). If you don't pass a
# path, this looks for it in this folder, the folder above, roms/ and ~/Downloads.
# Missing tools? Run ../install-deps.sh first.
set -e
# Paths you give are relative to where you ran this; resolve them before
# moving into the raverace folder.
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
    for z in tokyowar.zip namcoc71.zip; do
        for d in . .. roms ../roms "$HOME/Downloads"; do
            if [ -f "$d/$z" ]; then ZIPS+=("$d/$z"); echo "Found ROMs: $d/$z"; break; fi
        done
    done
    [ ${#ZIPS[@]} -gt 0 ] && python3 tools/setup_roms.py "${ZIPS[@]}" || true
    python3 tools/setup_roms.py --check || fail \
"No ROMs yet. Run:   ./build.sh /path/to/tokyowar.zip
    (MAME's Tokyo Wars set. It is not included: you need your own copy.)"
fi

# The game's program is big generated C (gen/tw_lifted.c, 26 MB); the first build takes several minutes.
if [ -f build/CMakeCache.txt ] && ! grep -q 'CMAKE_C_COMPILER:[A-Z]*=.*gcc' build/CMakeCache.txt; then
    rm -rf build
fi
cmake -S . -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target tw -j"$(nproc)"

printf '\nDone! From the top folder, start the game with:   ./launch.sh tokyo\n'
printf '  5 = coin   Enter = start   arrows = steer   Esc = menu\n\n'
