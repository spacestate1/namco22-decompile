#!/bin/bash
# Start a game.
#
#   ./launch.sh              this list
#
#   ./launch.sh prop         Prop Cycle, from the start (press 5 for a coin)
#   ./launch.sh prop 0       Prop Cycle, straight into level 0 (or 1, 2, 3)
#   ./launch.sh rave         Rave Racer (add a number for the window size: rave 3)
#
# Prop Cycle keys:  5 coin, Enter start, arrow keys steer, Space pedal,
#                   P pause, Esc menu, F12 picture.
# Rave Racer keys:  5 coin, X gas, Z brake, arrow keys steer, A/S shift,
#                   V view, P pause, Esc menu, F12 picture.
set -e
cd "$(dirname "$0")"

# Rebuild if the code changed. rebuild DIR TARGET
rebuild() {
    local bin="$1/build/$2"
    if [ ! -x "$bin" ] || [ -n "$(find "$1/src" "$1/include" "$1/CMakeLists.txt" -newer "$bin" -print -quit 2>/dev/null)" ]; then
        echo "Rebuilding..."
        cmake --build "$1/build" --target "$2" -j"$(nproc)" >/dev/null
    fi
}

game="${1:-}"
[ $# -gt 0 ] && shift
case "$game" in
    prop|propcycle)
        if [ ! -f build/CMakeCache.txt ]; then
            echo "Prop Cycle is not built yet. Run:  ./build.sh /path/to/propcycl.zip"; exit 1
        fi
        rebuild . propcycl
        case "${1:-}" in
            0|1|2|3) c="$1"; shift; exec ./build/propcycl extracted/ --autostart "$c" "$@" ;;
            *)       exec env PROPCYCL_NO_AUTOSTART=1 ./build/propcycl extracted/ "$@" ;;
        esac
        ;;
    rave|raverace)
        if [ ! -f raverace/build/CMakeCache.txt ]; then
            echo "Rave Racer is not built yet. Run:  raverace/build.sh /path/to/raverace.zip /path/to/namcoc74.zip"; exit 1
        fi
        rebuild raverace rr
        cd raverace
        if [ $# -eq 0 ] || [[ "$1" =~ ^[0-9]+$ ]]; then
            exec ./build/rr extracted --window ${1:+"$1"}
        fi
        exec ./build/rr extracted "$@"
        ;;
    *)
        sed -n '2,13p' "$0" | sed 's/^# \?//'
        ;;
esac
