#!/bin/bash
# Start a game.
#
#   Any game also takes --fullscreen and --set KEY=VALUE (a line of its settings file, for this run only),
#   e.g.  ./launch.sh rave 2 --set widescreen=1 --set volume=50
#
#   ./launch.sh              this list
#
#   ./launch.sh prop         Prop Cycle, from the start (press 5 for a coin)
#   ./launch.sh prop 0       Prop Cycle, straight into level 0 (or 1, 2, 3)
#   ./launch.sh rave         Rave Racer (add a number for the window size: rave 3)
#   ./launch.sh tokyo        Tokyo Wars (add a number for the window size: tokyo 3)
#   ./launch.sh dirt         Dirt Dash (add a number for the window size: dirt 3)
#   ./launch.sh dirt jungle  Dirt Dash starting at a stage (city, jungle, hill, mountain, snow): the coins, stage and car are played for you
#   ./launch.sh armadillo    Armadillo Racing (add a number for the window size: armadillo 2) -- the mouse is the trackball
#   ./launch.sh tc           Time Crisis (add a number for the window size: tc 2) -- the mouse is the gun
#   ./launch.sh ad           Ace Driver (add a number for the window size: ad 3)
#   ./launch.sh cc           Cyber Commando (add a number for the window size: cc 3)
#   ./launch.sh r2           Ridge Racer 2 (add a number for the window size: r2 3)
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
# Armadillo Racing keys: the mouse is the trackball (captured while playing; Esc releases it), click / Enter start,
#                   5 coin (a game costs four), arrows or the left stick roll the ball too, Esc menu, F2 test switch.
# Time Crisis keys: the mouse aims, left button shoots, right button / Z is the foot pedal, 5 coin (a game costs three),
#                   F2 test switch, F8 a white border for light guns, Esc menu.
# Ace Driver keys:  5 coin (a game costs two), X gas, Z brake, arrow keys steer, A/S shift,
#                   V view, F2 test switch, Esc menu (Debug: developer screens), F12 picture.
# Cyber Commando keys: 5 coin (a game costs two), arrow keys drive (Shift + Left/Right strafe), E/D/S/F + I/K/J/L the two
#                   sticks, Z gun, X missile, C view, F2 test switch, Esc menu, F12 picture. (Cyber Sled's layout.)
# Ridge Racer 2 keys: 5 coin (a game costs three), X or Up gas (also confirms: no Start button), Z or Down brake,
#                   Left/Right steer, A/S shift, F2 test switch, 9 service, Esc menu (Debug: developer screens), F12 picture.
#                   Pad: left stick steer, A gas, B brake, shoulders shift, Back coin, Start or R3 menu.
# Cyber Sled keys:  5 coin, 1/Enter start, arrow keys drive, E/D/S/F + I/K/J/L the two levers, Z gun, X missile,
#                   C view, F2 test switch, Esc menu (Online: play against a friend), F12 picture.

# THE PLAY OPTIONS (GitHub #85): when every argument is one of these the game starts in its window with them -- a window-scale
# number, --fullscreen, --set KEY=VALUE (any line of the game's settings file, this run only). Anything else passes to the program.
play_args() {
    SCALE=""; PARGS=()
    while [ $# -gt 0 ]; do
        case "$1" in
            [0-9]|[1-9][0-9]) SCALE=$1 ;;
            --window) ;;
            --fullscreen|--set=*) PARGS+=("$1") ;;
            --set) [ $# -ge 2 ] || return 1; PARGS+=("$1" "$2"); shift ;;
            *) return 1 ;;
        esac
        shift
    done
    return 0
}

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
        if play_args "$@"; then
            exec ./build/rr extracted --window $SCALE ${PARGS[@]+"${PARGS[@]}"}
        fi
        exec ./build/rr extracted "$@"
        ;;
    tokyo|tokyowar)
        if [ ! -f tokyowar/build/CMakeCache.txt ]; then
            echo "Tokyo Wars is not built yet. Run:  tokyowar/build.sh /path/to/tokyowar.zip"; exit 1
        fi
        rebuild tokyowar tw
        cd tokyowar
        if play_args "$@"; then
            exec ./build/tw extracted --window $SCALE ${PARGS[@]+"${PARGS[@]}"}
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
        if play_args "$@"; then
            exec ./build/dd extracted --window $SCALE ${PARGS[@]+"${PARGS[@]}"} ${stage:+--stage "$stage"}
        fi
        exec ./build/dd extracted "$@" ${stage:+--stage "$stage"}
        ;;
    am|armadillo|adillor)
        if [ ! -f adillor/build/CMakeCache.txt ]; then
            echo "Armadillo Racing is not built yet. Run:  adillor/build.sh /path/to/adillor.zip"; exit 1
        fi
        rebuild adillor am
        cd adillor
        if play_args "$@"; then
            exec ./build/am extracted --window $SCALE ${PARGS[@]+"${PARGS[@]}"}
        fi
        exec ./build/am extracted "$@"
        ;;
    tc|timecris)
        if [ ! -f timecris/build/CMakeCache.txt ]; then
            echo "Time Crisis is not built yet. Run:  timecris/build.sh /path/to/timecris.zip"; exit 1
        fi
        rebuild timecris tc
        cd timecris
        if play_args "$@"; then
            exec ./build/tc extracted --window $SCALE ${PARGS[@]+"${PARGS[@]}"}
        fi
        exec ./build/tc extracted "$@"
        ;;
    ad|ace|acedriver)
        if [ ! -f acedriver/build/CMakeCache.txt ]; then
            echo "Ace Driver is not built yet. Run:  acedriver/build.sh /path/to/acedrive.zip /path/to/namcoc74.zip"; exit 1
        fi
        rebuild acedriver ad
        cd acedriver
        if play_args "$@"; then
            exec ./build/ad extracted --window $SCALE ${PARGS[@]+"${PARGS[@]}"}
        fi
        exec ./build/ad extracted "$@"
        ;;
    cc|commando|cybrcomm|cybercommando)
        if [ ! -f cybrcomm/build/CMakeCache.txt ]; then
            echo "Cyber Commando is not built yet. Run:  cybrcomm/build.sh /path/to/cybrcomm.zip /path/to/namcoc74.zip"; exit 1
        fi
        rebuild cybrcomm cc
        cd cybrcomm
        if play_args "$@"; then
            exec ./build/cc extracted --window $SCALE ${PARGS[@]+"${PARGS[@]}"}
        fi
        exec ./build/cc extracted "$@"
        ;;
    r2|ridgera2|ridgeracer2|rr2)
        if [ ! -f ridgera2/build/CMakeCache.txt ]; then
            echo "Ridge Racer 2 is not built yet. Run:  ridgera2/build.sh /path/to/ridgera2.zip /path/to/namcoc74.zip"; exit 1
        fi
        rebuild ridgera2 r2
        cd ridgera2
        if play_args "$@"; then
            exec ./build/r2 extracted --window $SCALE ${PARGS[@]+"${PARGS[@]}"}
        fi
        exec ./build/r2 extracted "$@"
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
        if play_args "$@"; then
            exec ./build/cs21 extracted --window $SCALE ${PARGS[@]+"${PARGS[@]}"}
        fi
        exec ./build/cs21 extracted "$@"
        ;;
    *)
        sed -n '2,42p' "$0" | sed 's/^# \?//'
        ;;
esac
