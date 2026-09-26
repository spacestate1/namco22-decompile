#!/bin/bash
# Start a game.
#
#   ./launch.sh              this list
#
#   ./launch.sh prop         Prop Cycle, from the start (press 5 for a coin)
#   ./launch.sh prop 0       Prop Cycle, straight into level 0 (or 1, 2, 3)
#   ./launch.sh rave         Rave Racer (add a number for the window size: rave 3)
#   ./launch.sh tokyo        Tokyo Wars (add a number for the window size: tokyo 3)
#   ./launch.sh dirt         Dirt Dash (add a number for the window size: dirt 3)
#   ./launch.sh dirt jungle  Dirt Dash starting at a stage (city, jungle, hill, mountain, snow): the coins, stage and car are played for you
#
# Prop Cycle keys:  5 coin, Enter start, arrow keys steer, Space pedal,
#                   P pause, Esc menu, F12 picture.
# Rave Racer keys:  5 coin, X gas, Z brake, arrow keys steer, A/S shift,
#                   V view, P pause, Esc menu, F12 picture.
# Tokyo Wars keys:  5 coin, Enter start, arrows/A D steer, Up/W forward, Down/S back,
#                   X / Z triggers, P pause, Esc menu (widescreen ...), F12 picture.
# Dirt Dash keys:   5 coin (a game costs two), Z brake, X gas (throttle), C select (view change / confirm),
#                   arrows/A D steer, Q / E shift down / up, M motion stop, P pause, Esc menu, F12 picture.
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
    tokyo|tokyowar)
        if [ ! -f tokyowar/build/CMakeCache.txt ]; then
            echo "Tokyo Wars is not built yet. Run:  tokyowar/build.sh /path/to/tokyowar.zip"; exit 1
        fi
        rebuild tokyowar tw
        cd tokyowar
        if [ $# -eq 0 ] || [[ "$1" =~ ^[0-9]+$ ]]; then
            exec ./build/tw extracted --window ${1:+"$1"}
        fi
        exec ./build/tw extracted "$@"
        ;;
    dirt|dirtdash)
        if [ ! -f dirtdash/build/CMakeCache.txt ]; then
            echo "Dirt Dash is not built yet. Run:  dirtdash/build.sh /path/to/dirtdash.zip"; exit 1
        fi
        rebuild dirtdash dd
        cd dirtdash
        stage=""; rest=()
        while [ $# -gt 0 ]; do
            case "$1" in
                --stage)   stage="${2:-}"; shift; [ $# -gt 0 ] && shift ;;
                --stage=*) stage="${1#--stage=}"; shift ;;
                city|jungle|hill|mountain|snow) stage="$1"; shift ;;
                *) rest+=("$1"); shift ;;
            esac
        done
        set -- ${rest[@]+"${rest[@]}"}
        if [ $# -eq 0 ] || [[ "$1" =~ ^[0-9]+$ ]]; then
            exec ./build/dd extracted --window ${1:+"$1"} ${stage:+--stage "$stage"}
        fi
        exec ./build/dd extracted "$@" ${stage:+--stage "$stage"}
        ;;
    *)
        sed -n '2,20p' "$0" | sed 's/^# \?//'
        ;;
esac
