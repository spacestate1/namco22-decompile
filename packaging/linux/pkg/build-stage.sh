#!/bin/sh
# Build the eight games from an exported public tree and lay out the installed files.
#   build-stage.sh SRC ROMDIR STAGE
# ROMDIR holds propcycl.zip, raverace.zip, namcoc74.zip, tokyowar.zip, dirtdash.zip, timecris.zip, acedrive.zip, cybrcomm.zip, cybsled.zip, namcoc67.zip and namcoc68.zip: each game's sound
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

( cd "$SRC/timecris" && python3 tools/setup_roms.py "$ROMS/timecris.zip" )
cmake -S "$SRC/timecris" -B "$SRC/timecris/build" -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORTABLE=ON
# tc_lifted_NN.c (16 parts), tc_c25.c and tc_snd_driver.c: one at a time
cmake --build "$SRC/timecris/build" --target tc -j1

( cd "$SRC/acedriver" && python3 tools/setup_roms.py "$ROMS/acedrive.zip" "$ROMS/namcoc74.zip" )
cmake -S "$SRC/acedriver" -B "$SRC/acedriver/build" -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORTABLE=ON
# rr_lifted.c (25 MB, ~2.5 GB of gcc), rr_c25.c and snd_driver.c: one at a time
cmake --build "$SRC/acedriver/build" --target ad -j1

( cd "$SRC/cybrcomm" && python3 ../acedriver/tools/setup_roms.py --game cybrcomm "$ROMS/cybrcomm.zip" "$ROMS/namcoc74.zip" )
cmake -S "$SRC/cybrcomm" -B "$SRC/cybrcomm/build" -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORTABLE=ON
# rr_lifted.c, rr_c25.c and snd_driver.c: one at a time
cmake --build "$SRC/cybrcomm/build" --target cc -j1

( cd "$SRC/cybsled" && python3 tools/setup_roms.py "$ROMS/cybsled.zip" "$ROMS/namcoc67.zip" "$ROMS/namcoc68.zip" )
cmake -S "$SRC/cybsled" -B "$SRC/cybsled/build" -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORTABLE=ON
# cm_lifted.c (19 MB, ~3.2 GB of gcc), cs_lifted.c and the translated C67 / 6809 / C68 programs: one at a time
cmake --build "$SRC/cybsled/build" --target cs21 -j1

rm -rf "$STAGE"
install -d "$STAGE/usr/lib/namco22" "$STAGE/usr/bin" "$STAGE/usr/share/applications" \
           "$STAGE/usr/share/icons/hicolor/256x256/apps" "$STAGE/usr/share/doc/namco22"
install -m755 -s "$SRC/build/propcycl" "$SRC/raverace/build/rr" "$SRC/tokyowar/build/tw" "$SRC/dirtdash/build/dd" "$SRC/timecris/build/tc" "$SRC/acedriver/build/ad" "$SRC/cybrcomm/build/cc" "$SRC/cybsled/build/cs21" "$STAGE/usr/lib/namco22/"
install -m755 "$PKG/namco22-launch" "$STAGE/usr/lib/namco22/"
install -m755 "$PKG/propcycle" "$PKG/raveracer" "$PKG/tokyowars" "$PKG/dirtdash" "$PKG/timecrisis" "$PKG/acedriver" "$PKG/cybercommando" "$PKG/cybersled" "$STAGE/usr/bin/"
install -m644 "$PKG/propcycle.desktop" "$PKG/raveracer.desktop" "$PKG/tokyowars.desktop" "$PKG/dirtdash.desktop" "$PKG/timecrisis.desktop" "$PKG/acedriver.desktop" "$PKG/cybercommando.desktop" "$PKG/cybersled.desktop" "$STAGE/usr/share/applications/"
install -m644 "$PKG/propcycle.png" "$PKG/raveracer.png" "$PKG/tokyowars.png" "$PKG/dirtdash.png" "$PKG/timecrisis.png" "$PKG/acedriver.png" "$PKG/cybercommando.png" "$PKG/cybersled.png" "$STAGE/usr/share/icons/hicolor/256x256/apps/"
install -m644 "$PKG/README.txt" "$PKG/rom-note.txt" "$STAGE/usr/share/doc/namco22/"
install -m644 "$SRC/LICENSE" "$STAGE/usr/share/doc/namco22/LICENSE"
echo "staged into $STAGE"
