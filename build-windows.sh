#!/bin/bash
# Build the WINDOWS versions of Prop Cycle, Rave Racer, Tokyo Wars, Dirt Dash, Time Crisis, Ace Driver and Cyber Sled, from Linux.
#
#   ./build-windows.sh
#
# Makes windows-release/ -- everything Windows needs: PropCycle.exe,
# RaveRacer.exe and TokyoWars.exe (each ONE file: SDL2 and the C runtime are linked in; OpenGL
# comes with Windows and the GPU driver), an empty roms/ folder and
# HOW TO PLAY.txt -- plus windows-release.zip.
# On Windows: put the MAME ROM sets in roms/ (propcycl.zip for Prop Cycle;
# raverace.zip + namcoc74.zip for Rave Racer; tokyowar.zip for Tokyo Wars; dirtdash.zip for Dirt Dash; timecris.zip for Time Crisis; acedrive.zip + namcoc74.zip for Ace Driver; cybsled.zip + namcoc67.zip + namcoc68.zip for Cyber Sled) and
# double-click the game. The
# first start unpacks the ROMs into extracted/ beside it.
#
# Needs the MinGW-w64 cross compiler (Arch: mingw-w64-gcc, Debian/Ubuntu:
# gcc-mingw-w64-x86-64, Fedora: mingw64-gcc) plus cmake, curl, make, 7z (p7zip)
# and python3. SDL2, zlib and Mesa are downloaded into build-win/deps the first time.
#
# mesa/ (Mesa's software OpenGL, llvmpipe, from mesa-dist-win): Prop Cycle uses
# it ONLY when Windows offers no real OpenGL driver -- a virtual machine, Remote
# Desktop, a PC without its GPU driver ("GDI Generic"). See src/gl_dyn.c.
set -e
cd "$(dirname "$0")"

SDL_VER=2.32.10
ZLIB_VER=1.3.1
MESA_VER=26.2.1
CC=x86_64-w64-mingw32-gcc
TOP="$PWD"
DEPS="$TOP/build-win/deps"

command -v $CC >/dev/null || { echo "Install the MinGW-w64 cross compiler first ($CC)."; exit 1; }
mkdir -p "$DEPS"

# --- SDL2 (official MinGW development package) ------------------------------
SDL="$DEPS/SDL2-$SDL_VER/x86_64-w64-mingw32"
if [ ! -d "$SDL" ]; then
    echo "Downloading SDL2 $SDL_VER..."
    curl -fsSL -o "$DEPS/sdl2.tgz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-mingw.tar.gz"
    tar -C "$DEPS" -xzf "$DEPS/sdl2.tgz"
fi
sed -i "s|^prefix=.*|prefix=$SDL|" "$SDL/lib/pkgconfig/sdl2.pc"

# --- zlib, built as a static library -----------------------------------------
ZL="$DEPS/zlib"
if [ ! -f "$ZL/lib/libz.a" ]; then
    echo "Downloading and building zlib $ZLIB_VER..."
    curl -fsSL -o "$DEPS/zlib.tgz" \
        "https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz"
    tar -C "$DEPS" -xzf "$DEPS/zlib.tgz"
    make -C "$DEPS/zlib-$ZLIB_VER" -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- libz.a >/dev/null
    mkdir -p "$ZL/include" "$ZL/lib"
    cp "$DEPS/zlib-$ZLIB_VER"/{zlib.h,zconf.h} "$ZL/include/"
    cp "$DEPS/zlib-$ZLIB_VER/libz.a" "$ZL/lib/"
fi

# --- Mesa (software OpenGL fallback for Prop Cycle) --------------------------
MESA="$DEPS/mesa-$MESA_VER"
if [ ! -f "$MESA/libgallium_wgl.dll" ]; then
    echo "Downloading Mesa $MESA_VER (mesa-dist-win)..."
    command -v 7z >/dev/null || { echo "Install 7z (p7zip) to unpack Mesa."; exit 1; }
    curl -fsSL -o "$DEPS/mesa.7z" \
        "https://github.com/pal1000/mesa-dist-win/releases/download/$MESA_VER/mesa3d-$MESA_VER-release-mingw.7z"
    mkdir -p "$MESA"
    7z e -y -o"$MESA" "$DEPS/mesa.7z" x64/opengl32.dll x64/libgallium_wgl.dll >/dev/null
fi

# --- cross-compile -----------------------------------------------------------
cat > "$TOP/build-win/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER $CC)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 $SDL $ZL)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} $SDL/lib/pkgconfig)
set(ENV{PKG_CONFIG_PATH} "")
EOF
cmake -S . -B build-win/cmake -DCMAKE_TOOLCHAIN_FILE="$TOP/build-win/toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build build-win/cmake --target propcycl -j"${JOBS:-$(nproc)}"
# Rave Racer (raverace/): the same toolchain; its generated sources (gen/) come
# with the tree, the sound program is translated at build time with the host python
cmake -S raverace -B build-win/rr -DCMAKE_TOOLCHAIN_FILE="$TOP/build-win/toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-win/rr --target rr -j"${JOBS:-$(nproc)}"
# Tokyo Wars (tokyowar/): the same toolchain; its lifted program (gen/tw_lifted.c) comes with the tree, the sound and
# master-DSP programs are translated at build time from the ROM set (tokyowar/extracted/) with the host python
cmake -S tokyowar -B build-win/tw -DCMAKE_TOOLCHAIN_FILE="$TOP/build-win/toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-win/tw --target tw -j"${JOBS:-$(nproc)}"
# Dirt Dash (dirtdash/): the same again -- gen/dd_lifted.c comes with the tree, the sound and master-DSP programs are
# translated at build time from the ROM set (dirtdash/extracted/)
cmake -S dirtdash -B build-win/dd -DCMAKE_TOOLCHAIN_FILE="$TOP/build-win/toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-win/dd --target dd -j"${JOBS:-$(nproc)}"
# Time Crisis (timecris/): the same again (gen/tc_lifted_NN.c comes with the tree; the master DSP and sound programs are translated from timecris/extracted/)
cmake -S timecris -B build-win/tc -DCMAKE_TOOLCHAIN_FILE="$TOP/build-win/toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-win/tc --target tc -j"${JOBS:-$(nproc)}"
# Ace Driver (acedriver/): the Rave Racer board through raverace/src (gen/rr_lifted.c comes with the tree; the master DSP and sound
# programs are translated from acedriver/extracted/)
cmake -S acedriver -B build-win/ad -DCMAKE_TOOLCHAIN_FILE="$TOP/build-win/toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-win/ad --target ad -j"${JOBS:-$(nproc)}"
# Cyber Commando (cybrcomm/): the same board and runtime again (gen/rr_lifted.c comes with the tree; the master DSP and sound
# programs are translated from cybrcomm/extracted/)
cmake -S cybrcomm -B build-win/cc -DCMAKE_TOOLCHAIN_FILE="$TOP/build-win/toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-win/cc --target cc -j"${JOBS:-$(nproc)}"
# Cyber Sled (cybsled/, System 21): gen/c{m,s}_lifted.c come with the tree (~3 GB of gcc each: one at a time); the C67 DSP, 6809
# sound and C68 I/O programs are translated from cybsled/extracted/
cmake -S cybsled -B build-win/cs -DCMAKE_TOOLCHAIN_FILE="$TOP/build-win/toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-win/cs --target cs21 -j1

# --- package: windows-release/ ---------------------------------------------------
# Everything Windows needs, in one folder. The instructions and the roms/
# note live in packaging/windows/.
REL="$TOP/windows-release"
rm -rf "$REL" "$TOP/windows-release.zip" "$TOP/dist"
mkdir -p "$REL"
cp -r "$TOP/packaging/windows/." "$REL/"
cp "$TOP/LICENSE" "$REL/LICENSE.txt"
cp build-win/cmake/propcycl.exe "$REL/PropCycle.exe"
cp build-win/rr/rr.exe "$REL/RaveRacer.exe"
cp build-win/tw/tw.exe "$REL/TokyoWars.exe"
cp build-win/dd/dd.exe "$REL/DirtDash.exe"
cp build-win/tc/tc.exe "$REL/TimeCrisis.exe"
cp build-win/ad/ad.exe "$REL/AceDriver.exe"
cp build-win/cc/cc.exe "$REL/CyberCommando.exe"
cp build-win/cs/cs21.exe "$REL/CyberSled.exe"
mkdir -p "$REL/mesa"
cp "$MESA/opengl32.dll" "$MESA/libgallium_wgl.dll" "$REL/mesa/"
x86_64-w64-mingw32-strip "$REL/PropCycle.exe" "$REL/RaveRacer.exe" "$REL/TokyoWars.exe" "$REL/DirtDash.exe" "$REL/TimeCrisis.exe" "$REL/AceDriver.exe" "$REL/CyberCommando.exe" "$REL/CyberSled.exe"
# the .exe must need nothing beside it: every DLL it imports must ship with Windows
# (OPENGL32.dll does -- it hands over to the installed GPU driver and is never bundled)
for exe in PropCycle.exe RaveRacer.exe TokyoWars.exe DirtDash.exe TimeCrisis.exe AceDriver.exe CyberCommando.exe CyberSled.exe; do
    bad=$(x86_64-w64-mingw32-objdump -p "$REL/$exe" | awk '/DLL Name/ {print $3}' |
          grep -viE '^(kernel32|user32|gdi32|opengl32|advapi32|shell32|ole32|oleaut32|imm32|setupapi|version|winmm|dinput8|ws2_32|api-ms-win-crt-.*)\.dll$' || true)
    [ -z "$bad" ] || { echo "$exe needs DLLs Windows does not ship: $bad"; exit 1; }
done
(cd "$TOP" && python3 -c "import shutil; shutil.make_archive('windows-release', 'zip', '.', 'windows-release')")
echo
echo "Done:  windows-release/      (copy this folder to a Windows PC)"
echo "       windows-release.zip   (the same folder, zipped for sharing)"
