#!/bin/sh
# Build the four games from an exported public tree and lay out the installed files.
#   build-stage.sh SRC ROMDIR STAGE
# ROMDIR holds propcycl.zip, raverace.zip, namcoc74.zip, tokyowar.zip and dirtdash.zip: each game's sound
# program is translated to C from the ROM at BUILD time (tools/gen/snd_translate.py).
# The ROMs are used only for that and are never copied into STAGE.
set -e
SRC=$(cd "$1" && pwd); ROMS=$(cd "$2" && pwd); STAGE=$3
PKG=$(cd "$(dirname "$0")" && pwd)
# gen/snd_driver.c and rr_lifted.c need GBs of RAM each; JOBS=1 on a small machine
J=${JOBS:-$(nproc)}

( cd "$SRC" && python3 tools/setup_roms.py "$ROMS/propcycl.zip" )
cmake -S "$SRC" -B "$SRC/build" -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORTABLE=ON
cmake --build "$SRC/build" --target propcycl -j"$J"

( cd "$SRC/raverace" && python3 tools/setup_roms.py "$ROMS/raverace.zip" "$ROMS/namcoc74.zip" )
cmake -S "$SRC/raverace" -B "$SRC/raverace/build" -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORTABLE=ON
# rr_lifted.c and snd_driver.c each need GBs of RAM to compile: one at a time
cmake --build "$SRC/raverace/build" --target rr -j1

( cd "$SRC/tokyowar" && python3 tools/setup_roms.py "$ROMS/tokyowar.zip" )
cmake -S "$SRC/tokyowar" -B "$SRC/tokyowar/build" -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORTABLE=ON
# tw_lifted.c (26 MB of goto C), tw_c25.c and tw_snd_driver.c: one at a time
cmake --build "$SRC/tokyowar/build" --target tw -j1

( cd "$SRC/dirtdash" && python3 tools/setup_roms.py "$ROMS/dirtdash.zip" )
cmake -S "$SRC/dirtdash" -B "$SRC/dirtdash/build" -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORTABLE=ON
# dd_lifted.c, dd_c25.c and dd_snd_driver.c: one at a time
cmake --build "$SRC/dirtdash/build" --target dd -j1

rm -rf "$STAGE"
install -d "$STAGE/usr/lib/namco22" "$STAGE/usr/bin" "$STAGE/usr/share/applications" \
           "$STAGE/usr/share/icons/hicolor/256x256/apps" "$STAGE/usr/share/doc/namco22"
install -m755 -s "$SRC/build/propcycl" "$SRC/raverace/build/rr" "$SRC/tokyowar/build/tw" "$SRC/dirtdash/build/dd" "$STAGE/usr/lib/namco22/"
install -m755 "$PKG/namco22-launch" "$STAGE/usr/lib/namco22/"
install -m755 "$PKG/propcycle" "$PKG/raveracer" "$PKG/tokyowars" "$PKG/dirtdash" "$STAGE/usr/bin/"
install -m644 "$PKG/propcycle.desktop" "$PKG/raveracer.desktop" "$PKG/tokyowars.desktop" "$PKG/dirtdash.desktop" "$STAGE/usr/share/applications/"
install -m644 "$PKG/propcycle.png" "$PKG/raveracer.png" "$PKG/tokyowars.png" "$PKG/dirtdash.png" "$STAGE/usr/share/icons/hicolor/256x256/apps/"
install -m644 "$PKG/README.txt" "$PKG/rom-note.txt" "$STAGE/usr/share/doc/namco22/"
echo "staged into $STAGE"
