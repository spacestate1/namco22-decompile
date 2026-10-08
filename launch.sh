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
#   ./launch.sh tc           Time Crisis (add a number for the window size: tc 2) -- the mouse is the gun
#   ./launch.sh ad           Ace Driver (add a number for the window size: ad 3)
#   ./launch.sh cc           Cyber Commando (add a number for the window size: cc 3)
#   ./launch.sh cs           Cyber Sled (add a number for the window size: cs 2)
#
# Prop Cycle keys:  5 coin, Enter start, arrow keys steer, Space pedal,
#                   P pause, Esc menu, F12 picture.
# Rave Racer keys:  5 coin, X gas, Z brake, arrow keys steer, A/S shift,
#                   V view, P pause, Esc menu, F12 picture.
# Tokyo Wars keys:  5 coin, Enter start, arrows/A D steer, Up/W forward, Down/S back,
#                   X / Z triggers, P pause, Esc menu (widescreen ...), F12 picture.
# Dirt Dash keys:   5 coin (a game costs two), Z brake, X gas (throttle), C select (view change / confirm),
#                   arrows/A D steer, Q / E shift down / up, M motion stop, P pause, Esc menu, F12 picture.
# Time Crisis keys: the mouse aims, left button shoots, right button / Z is the foot pedal, 5 coin (a game costs three),
#                   F2 test switch, F8 a white border for light guns, Esc menu.
# Ace Driver keys:  5 coin (a game costs two), X gas, Z brake, arrow keys steer, A/S shift,
#                   V view, F2 test switch, Esc menu (Debug: developer screens), F12 picture.
# Cyber Commando keys: 5 coin (a game costs two), arrow keys drive (Shift + Left/Right strafe), E/D/S/F + I/K/J/L the two
#                   sticks, Z gun, X missile, C view, F2 test switch, Esc menu, F12 picture. (Cyber Sled's layout.)
# Cyber Sled keys:  5 coin, 1/Enter start, arrow keys drive, E/D/S/F + I/K/J/L the two levers, Z gun, X missile,
#                   C view, F2 test switch, Esc menu (Online: play against a friend), F12 picture.
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
    tc|timecris)
        if [ ! -f timecris/build/CMakeCache.txt ]; then
            echo "Time Crisis is not built yet. Run:  timecris/build.sh /path/to/timecris.zip"; exit 1
        fi
        rebuild timecris tc
        cd timecris
        if [ $# -eq 0 ] || [[ "$1" =~ ^[0-9]+$ ]]; then
            exec ./build/tc extracted --window ${1:+"$1"}
        fi
        exec ./build/tc extracted "$@"
        ;;
    ad|ace|acedriver)
        if [ ! -f acedriver/build/CMakeCache.txt ]; then
            echo "Ace Driver is not built yet. Run:  acedriver/build.sh /path/to/acedrive.zip /path/to/namcoc74.zip"; exit 1
        fi
        rebuild acedriver ad
        cd acedriver
        if [ $# -eq 0 ] || [[ "$1" =~ ^[0-9]+$ ]]; then
            exec ./build/ad extracted --window ${1:+"$1"}
        fi
        exec ./build/ad extracted "$@"
        ;;
    cc|commando|cybrcomm|cybercommando)
        if [ ! -f cybrcomm/build/CMakeCache.txt ]; then
            echo "Cyber Commando is not built yet. Run:  cybrcomm/build.sh /path/to/cybrcomm.zip /path/to/namcoc74.zip"; exit 1
        fi
        rebuild cybrcomm cc
        cd cybrcomm
        if [ $# -eq 0 ] || [[ "$1" =~ ^[0-9]+$ ]]; then
            exec ./build/cc extracted --window ${1:+"$1"}
        fi
        exec ./build/cc extracted "$@"
        ;;
    cs|cybsled|cybersled)
        if [ ! -f cybsled/build/CMakeCache.txt ]; then
            echo "Cyber Sled is not built yet. Run:  cybsled/build.sh /path/to/cybsled.zip /path/to/namcoc67.zip /path/to/namcoc68.zip"; exit 1
        fi
        # not rebuild(): the two lifted 68000 programs need ~3 GB of gcc each, so they are compiled one at a time (-j1)
        if [ ! -x cybsled/build/cs21 ] || [ -n "$(find cybsled/src cybsled/CMakeLists.txt -newer cybsled/build/cs21 -print -quit 2>/dev/null)" ]; then
            echo "Rebuilding..."
            cmake --build cybsled/build --target cs21 -j1 >/dev/null
        fi
        cd cybsled
        if [ $# -eq 0 ] || [[ "$1" =~ ^[0-9]+$ ]]; then
            exec ./build/cs21 extracted --window ${1:+"$1"}
        fi
        exec ./build/cs21 extracted "$@"
        ;;
    *)
        sed -n '2,32p' "$0" | sed 's/^# \?//'
        ;;
esac
