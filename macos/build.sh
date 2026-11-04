#!/bin/bash
# macos/build.sh -- native arm64 Metal build of Time Crisis 1 and Time Crisis II.
#
# Uses Apple clang (no gcc), the macOS Metal shim (macos/glshim), SDL2 and zlib.
# No OpenGL is loaded or linked. ROMs are read-only inputs already imported by
# macos/prepare_roms.py into <game>/extracted.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD="${BUILD_DIR:-macos/output/build}"
mkdir -p "$BUILD"
BUILD="$(cd "$BUILD" && pwd)"

# iCloud-synced checkouts: macOS can offload files ("dataless"), and a build
# that reads one gets an EMPTY file without an error. Bring them back first,
# and refuse to build while any source or tool file is still offloaded.
offloaded() { find engine include macos timecris tc2 tools third_party -flags +dataless -print 2>/dev/null; }
if [ -n "$(offloaded)" ]; then
    echo "=== downloading $(offloaded | wc -l | tr -d ' ') iCloud-offloaded files ==="
    offloaded | while IFS= read -r f; do brctl download "$f" >/dev/null 2>&1 || true; done
    for _ in $(seq 1 60); do [ -z "$(offloaded)" ] && break; sleep 2; done
    if [ -n "$(offloaded)" ]; then
        echo "still offloaded by iCloud (refusing an incomplete build):" >&2; offloaded | head -20 >&2; exit 1
    fi
fi

SDL_CFLAGS="$(pkg-config --cflags sdl2)"
SDL_LIBS="$(pkg-config --libs sdl2)"

# Common flags: gnu11 for computed labels / statement expressions in lifted C.
# -w: the transpiled 68K/MIPS/H8 C is warning-noisy by design.
CFLAGS="-O2 -std=gnu11 -fno-strict-aliasing -fwrapv -w -DGLMC_METAL"
SHIM="-Imacos/glshim"
INCLUDE="-Iengine -Iengine/snd -Iengine/c25 -Itools/c25oracle -Iinclude"

FRAMEWORKS="-framework Metal -framework QuartzCore -framework Foundation -framework Cocoa -framework CoreGraphics"

compile() { # <out.o> <src> [extra cflags...]
    local out="$1" src="$2"; shift 2
    # file-specific includes ("$@") precede the shared engine includes so a
    # game's own lift_rt.h wins over engine/lift_rt.h (tc2's MIPS vs 68K).
    clang -c $CFLAGS $SHIM "$@" $INCLUDE $SDL_CFLAGS "$src" -o "$BUILD/$out"
}

compile_mm() { # <out.o> <src>
    clang -c -O2 -std=gnu++17 -fno-strict-aliasing -fwrapv -w -fobjc-arc -fmodules \
        $SHIM $INCLUDE $SDL_CFLAGS "$2" -o "$BUILD/$1"
}

if [ "${1:-}" != "tc2" ]; then
echo "=== compiling shim ==="
compile glmc_main.o macos/glshim/glmc_main.c
compile_mm glmc_metal.o macos/glshim/glmc_metal.mm

echo "=== compiling engine (shared) ==="
for f in eng geo_hw slave_list tex_bake render_target quad_gl hud_edges post_gl \
         fog_hw sprite_hw text_hw ss22_gl frame_rule \
         ss22_host ss22_out ss22_input eng_ffb twin_stick ss22_board ss22_link c139 ss22_dsp ss22_snd ss22_video ss22_env ss22_run \
         eng_cfg eng_display eng_ui eng_crosshair \
         romzip win_startup \
         lift_cpu lift_env \
         c25/c25_bus c25/c25_core c25/c71_bios \
         c352 audio_out snd/m377_periph; do
    compile "eng_${f//\//_}.o" "engine/$f.c"
done

echo "=== translating the TC1 master DSP and sound programs (as timecris/CMakeLists.txt does) ==="
(
    cd timecris
    python3 ../tools/gen/c25_translate.py --game tc --roms extracted --main-rom timecris_main.bin \
        --block ABA8C --block B91F4 --block B91F8 --block B9212 --block B93AE --block B9536 --block B9626 --block B9804 \
        --block B9CAE --block B9D3C --block BA11A --block BA580 --block BAE16 \
        --cov tools/gen/c25.cov --out "$BUILD/tc_c25.c" --func tc_c25_exec
    python3 ../tools/gen/snd_translate.py tools/gen/snd.cov --game tc --roms extracted --out "$BUILD/tc_snd_driver.c"
)
echo "=== compiling TC1 ==="
compile tc_game.o timecris/src/tc_game.c -Itimecris/include -Itimecris/gen
for f in timecris/gen/tc_lifted_*.c; do
    compile "tc_$(basename "${f%.c}").o" "$f" -Itimecris/include -Itimecris/gen
done
compile tc_c25.o "$BUILD/tc_c25.c" -Itimecris/include -Itimecris/gen
compile tc_snd_driver.o "$BUILD/tc_snd_driver.c" -Itimecris/include -Itimecris/gen

echo "=== linking TC1 ==="
clang -o "$BUILD/Time Crisis" \
    "$BUILD"/glmc_main.o "$BUILD"/glmc_metal.o \
    "$BUILD"/tc_*.o "$BUILD"/eng_*.o \
    $SDL_LIBS -lz $FRAMEWORKS -lc++
fi

if [ "${1:-}" != "tc1" ]; then
echo "=== generating TC2 H8 programs with recorded coverage ==="
(
    cd tc2
    shopt -s nullglob
    sub_coverage=(cov/h8sub_*.pcs)
    io_coverage=(cov/h8io_*.pcs)
    if (( ${#sub_coverage[@]} == 0 || ${#io_coverage[@]} == 0 )); then
        echo "TC2 H8 coverage files are missing; refusing an incomplete build" >&2
        exit 1
    fi
    for i in "${!sub_coverage[@]}"; do sub_coverage[$i]="${sub_coverage[$i]%.pcs}"; done
    for i in "${!io_coverage[@]}"; do io_coverage[$i]="${io_coverage[$i]%.pcs}"; done
    python3 tools/h8/h8_translate.py sub.bin 1 tc2sub "$BUILD/tc2_sub.c" "${sub_coverage[@]}"
    H8_ROMSIZE=0x4000 python3 tools/h8/h8_translate.py io.bin 0 tc2io "$BUILD/tc2_io.c" "${io_coverage[@]}"
)
echo "=== compiling TC2 ==="
compile tc2_game.o tc2/src/tc2_game.c -Itc2/include -Itc2/src -Itc2/src/eng -Itc2/gen
for f in tc2/src/tc2_cpu.c tc2/src/tc2_mem.c tc2/src/tc2_ctl.c tc2/src/tc2_chips.c tc2/src/tc2_screen.c \
         tc2/src/tc2_text.c tc2/src/tc2_window.c tc2/src/tc2_main.c \
         tc2/src/eng/eng_ui.c tc2/src/eng/eng_crosshair.c tc2/src/eng/eng_cfg.c tc2/src/eng/eng_display.c \
         tc2/src/eng/ss22_input.c tc2/src/eng/eng_ffb.c tc2/src/tc2_input.c \
         tc2/src/tc2_c435.c tc2/src/tc2_render.c tc2/src/tc2_gl.c tc2/src/h8_periph.c tc2/src/tc2_h8.c \
         tc2/src/tc2_audio.c tc2/src/tc2_rom.c tc2/src/tc2_out.c \
         tc2/src/eng/romzip.c tc2/src/eng/c352.c tc2/src/tc2_link.c \
         tc2/src/eng/net.c tc2/src/eng/net_host.c tc2/src/eng/eng_net_ui.c tc2/src/eng/eng_rawptr.c tc2/src/tc2_subs.c; do
    compile "tc2_$(basename "${f%.c}").o" "$f" -Itc2/include -Itc2/src -Itc2/src/eng -Itc2/gen
done
compile tc2_sub.o "$BUILD/tc2_sub.c" -Itc2/include -Itc2/gen
compile tc2_io.o "$BUILD/tc2_io.c" -Itc2/include -Itc2/gen
for f in tc2/gen/tc2_lifted_*.c; do
    compile "tc2_$(basename "${f%.c}").o" "$f" -Itc2/include -Itc2/gen -include tc2/include/tc2_mips_rt.h
done

echo "=== linking TC2 ==="
clang -o "$BUILD/Time Crisis II" \
    "$BUILD"/glmc_main.o "$BUILD"/glmc_metal.o \
    "$BUILD"/tc2_*.o \
    $SDL_LIBS -lz $FRAMEWORKS -lc++
fi

echo "=== done: $BUILD ==="
ls -la "$BUILD/"*.app 2>/dev/null || ls -la "$BUILD"/Time*
