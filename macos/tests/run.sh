#!/bin/bash
# macos/tests/run.sh -- build and run the offscreen Metal tests (no window, no ROMs): gpu_readback (pixel-asserting checks of the
# GL-shaped shim on the real GPU) and compile_msl (every Metal shader unit compiles). Exits non-zero on any failure.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="${TEST_DIR:-macos/output/tests}"; mkdir -p "$OUT"
F="-Imacos/glshim $(pkg-config --cflags sdl2)"
clang -c -O1 -g $F macos/glshim/glmc_main.c -o "$OUT/gm.o"
clang -c -O1 -g -fobjc-arc -fmodules $F macos/glshim/glmc_metal.mm -o "$OUT/gmm.o"
clang -c -O1 -g $F macos/tests/gpu_readback.c -o "$OUT/gr.o"
clang -fobjc-arc "$OUT/gm.o" "$OUT/gmm.o" "$OUT/gr.o" -framework Metal -framework QuartzCore -framework Foundation -framework Cocoa \
    $(pkg-config --libs sdl2) -lc++ -o "$OUT/gpu_readback"
clang -fobjc-arc -fmodules -Imacos macos/tests/compile_msl.m -framework Metal -framework Foundation -o "$OUT/compile_msl"
"$OUT/gpu_readback"
"$OUT/compile_msl"
