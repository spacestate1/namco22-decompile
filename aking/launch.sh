#!/bin/bash
# launch.sh -- Angler King (Namco Super System 23) on the shared System 23 host: builds what is missing, then runs from power-on.
#   ./launch.sh                  a window (Esc = the menu)
#   ./launch.sh 2                at 2x            ./launch.sh --fullscreen
#   ./launch.sh --headless N     no window: N frames, screenshots every 300 frames into work/run/shots, the sound into work/run/aking.wav
# Needs MAME's aking set (tools/setup_roms.py aking.zip) unpacked in extracted/ and the lift in gen/ (tools/relift.sh). Memory: the build uses JOBS (default 4) gcc jobs of ~1 GB.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
if [ ! -f extracted/ag1vera.ic1 ]; then echo "launch.sh: put the aking ROM files (python3 tools/setup_roms.py aking.zip) in extracted/ first" >&2; exit 1; fi
if [ ! -f boot.bin ] || [ ! -f sub.bin ] || [ ! -f io.bin ]; then python3 tools/make_prog.py extracted || exit 1; fi
if [ ! -x build/aking ] || [ -n "$(find src include gen ../tc2/src ../tc2/include ../tc2/tools/h8 CMakeLists.txt ../tc2/host.cmake -newer build/aking -print -quit 2>/dev/null)" ]; then
    mkdir -p build && (cd build && cmake .. >/dev/null && make -j"${JOBS:-4}" aking) || exit 1
fi
if [ "$1" = "--headless" ]; then
    n=${2:-1800}; mkdir -p work/run/shots
    TC2_SHOTS=work/run/shots:300 TC2_WAV=work/run/aking.wav exec build/aking reset $((n * 2833000))
fi
[ -z "$WAYLAND_DISPLAY" ] && [ -z "$DISPLAY" ] && [ -S /run/user/$(id -u)/wayland-0 ] && export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/$(id -u)
exec build/aking --window "$@"
