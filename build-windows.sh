#!/bin/bash
# Build the WINDOWS version of Prop Cycle, from Linux.
#
#   ./build-windows.sh
#
# Makes windows-release/ -- everything Windows needs: PropCycle.exe, SDL2.dll,
# an empty roms/ folder and HOW TO PLAY.txt -- plus windows-release.zip.
# On Windows: put propcycl.zip (the MAME ROM set) in roms/ and double-click
# PropCycle.exe. The first start unpacks the ROMs into extracted/ beside it.
#
# Needs the MinGW-w64 cross compiler (Arch: mingw-w64-gcc, Debian/Ubuntu:
# gcc-mingw-w64-x86-64, Fedora: mingw64-gcc) plus cmake, curl, make and zip
# or python3. SDL2 and zlib are downloaded into build-win/deps the first time.
set -e
cd "$(dirname "$0")"

SDL_VER=2.32.10
ZLIB_VER=1.3.1
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
cmake --build build-win/cmake --target propcycl -j"$(nproc)"

# --- package: windows-release/ ---------------------------------------------------
# Everything Windows needs, in one folder. The instructions and the roms/
# note live in packaging/windows/.
REL="$TOP/windows-release"
rm -rf "$REL" "$TOP/windows-release.zip" "$TOP/dist"
mkdir -p "$REL"
cp -r "$TOP/packaging/windows/." "$REL/"
cp build-win/cmake/propcycl.exe "$REL/PropCycle.exe"
x86_64-w64-mingw32-strip "$REL/PropCycle.exe"
cp "$SDL/bin/SDL2.dll" "$REL/"
(cd "$TOP" && python3 -c "import shutil; shutil.make_archive('windows-release', 'zip', '.', 'windows-release')")
echo
echo "Done:  windows-release/      (copy this folder to a Windows PC)"
echo "       windows-release.zip   (the same folder, zipped for sharing)"
