#!/bin/bash
# Start Prop Cycle.
#
#   ./launch.sh          the game, from the start (press 5 for a coin)
#   ./launch.sh 0        jump straight into level 0 (or 1, 2, 3)
#
# Keys: 5 coin, Enter start, arrow keys steer, Esc menu, F12 picture.
set -e
cd "$(dirname "$0")"

if [ ! -f build/CMakeCache.txt ]; then
    echo "Not built yet. Run:  ./build.sh /path/to/propcycl.zip"
    exit 1
fi
# Rebuild if the code changed.
if [ ! -x build/propcycl ] || [ -n "$(find src include CMakeLists.txt -newer build/propcycl -print -quit 2>/dev/null)" ]; then
    echo "Rebuilding..."
    cmake --build build --target propcycl -j"$(nproc)" >/dev/null
fi

case "${1:-}" in
    0|1|2|3)
        c="$1"; shift
        exec ./build/propcycl extracted/ --autostart "$c" "$@"
        ;;
    *)
        exec env PROPCYCL_NO_AUTOSTART=1 ./build/propcycl extracted/ "$@"
        ;;
esac
