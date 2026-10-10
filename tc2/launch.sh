#!/bin/bash
# launch.sh -- Time Crisis 2 (Namco System 23): builds what is missing, then runs the game in a window from power-on.
#   ./launch.sh                  a window (Esc = the menu: Display / Frame rate / Audio / Controls / crosshair)
#   ./launch.sh 2                at 2x
#   ./launch.sh --fullscreen
#   ./launch.sh --headless N     no window: N frames, screenshots every 300 frames into work/run/shots, the sound into work/run/tc2.wav
# Needs the ROM files unpacked in extracted/ (MAME's timecrs2 set). Keys: 5 coin, Space / left mouse trigger, Z / right mouse pedal,
# F2 test, 9 service; the mouse (or a pad's stick) aims, off-screen = reload.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
if [ ! -f extracted/tss3verb.1 ]; then echo "launch.sh: put the timecrs2 ROM files in extracted/ first" >&2; exit 1; fi
if [ ! -f boot.bin ] || [ ! -f sub.bin ] || [ ! -f io.bin ]; then python3 tools/make_prog.py extracted || exit 1; fi
if [ ! -x build/tc2 ] || [ -n "$(find src include gen tools/h8 CMakeLists.txt -newer build/tc2 -print -quit 2>/dev/null)" ]; then
    mkdir -p build && (cd build && cmake .. >/dev/null && make -j"$(nproc)" tc2) || exit 1
fi
if [ "$1" = "--headless" ]; then
    n=${2:-1800}; mkdir -p work/run/shots
    TC2_SHOTS=work/run/shots:300 TC2_WAV=work/run/tc2.wav exec build/tc2 reset $((n * 2830000))
fi
# a Wayland desktop session started from a plain shell
[ -z "$WAYLAND_DISPLAY" ] && [ -z "$DISPLAY" ] && [ -S /run/user/$(id -u)/wayland-0 ] && export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/$(id -u)
exec build/tc2 --window "$@"
