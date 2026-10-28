#!/bin/bash
# Build the WINDOWS version of Angler King from Linux (Time Crisis 2's script, the same System 23 host).
#
#   ./build-windows.sh            -> windows-release/  (AnglerKing.exe: ONE file, SDL2 and the C runtime linked in; OpenGL comes
#                                    with Windows and the GPU driver), an empty roms/ folder, aking_subtitles.txt -- and windows-release.zip
#
# On Windows: put MAME's aking.zip in roms/ and double-click AnglerKing.exe; the first start unpacks it into extracted/ beside it.
# Needs the MinGW-w64 cross compiler (Arch: mingw-w64-gcc, Debian/Ubuntu: gcc-mingw-w64-x86-64, Fedora: mingw64-gcc), cmake, curl, make
# and python3, plus the ROM files in extracted/ (the two H8 programs are translated from them at build time: tools/make_prog.py).
# SDL2 and zlib are downloaded into build-win/deps the first time. JOBS=n limits the parallel compiles (the lifted files need GBs).
set -e
cd "$(dirname "$0")"
SDL_VER=2.32.10
ZLIB_VER=1.3.1
CC=x86_64-w64-mingw32-gcc
TOP="$PWD"
DEPS="$TOP/../tc2/build-win/deps"          # shared with Time Crisis 2
command -v $CC >/dev/null || { echo "Install the MinGW-w64 cross compiler first ($CC)."; exit 1; }
[ -f extracted/ag1vera.ic1 ] || { echo "Put the aking ROM files in extracted/ first (or run the Linux build once with aking.zip)."; exit 1; }
mkdir -p "$DEPS" build-win
SDL="$DEPS/SDL2-$SDL_VER/x86_64-w64-mingw32"
if [ ! -d "$SDL" ]; then
    echo "Downloading SDL2 $SDL_VER..."
    curl -fsSL -o "$DEPS/sdl2.tgz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-mingw.tar.gz"
    tar -C "$DEPS" -xzf "$DEPS/sdl2.tgz"
fi
sed -i "s|^prefix=.*|prefix=$SDL|" "$SDL/lib/pkgconfig/sdl2.pc"
ZL="$DEPS/zlib"
if [ ! -f "$ZL/lib/libz.a" ]; then
    echo "Downloading and building zlib $ZLIB_VER..."
    curl -fsSL -o "$DEPS/zlib.tgz" "https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz"
    tar -C "$DEPS" -xzf "$DEPS/zlib.tgz"
    make -C "$DEPS/zlib-$ZLIB_VER" -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- libz.a >/dev/null
    mkdir -p "$ZL/include" "$ZL/lib"
    cp "$DEPS/zlib-$ZLIB_VER"/{zlib.h,zconf.h} "$ZL/include/"; cp "$DEPS/zlib-$ZLIB_VER/libz.a" "$ZL/lib/"
fi
cat > build-win/toolchain.cmake <<TC
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 $SDL $ZL)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} $SDL/lib/pkgconfig)
set(ENV{PKG_CONFIG_PATH} "")
TC
[ -f boot.bin ] && [ -f sub.bin ] && [ -f io.bin ] || python3 tools/make_prog.py extracted
cmake -S . -B build-win/ak -DCMAKE_TOOLCHAIN_FILE="$TOP/build-win/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-win/ak --target aking -j"${JOBS:-$(nproc)}"
REL="$TOP/windows-release"
rm -rf "$REL" "$TOP/windows-release.zip"; mkdir -p "$REL/roms"
cp build-win/ak/aking.exe "$REL/AnglerKing.exe"
x86_64-w64-mingw32-strip "$REL/AnglerKing.exe"
cp aking_subtitles.txt "$REL/"                 # the English subtitles, read from beside the .exe
[ -f ../LICENSE ] && cp ../LICENSE "$REL/LICENSE.txt"
bad=$(x86_64-w64-mingw32-objdump -p "$REL/AnglerKing.exe" | awk '/DLL Name/ {print $3}' |
      grep -viE '^(kernel32|user32|gdi32|opengl32|advapi32|shell32|ole32|oleaut32|imm32|setupapi|version|winmm|dinput8|ws2_32|api-ms-win-crt-.*)\.dll$' || true)
[ -z "$bad" ] || { echo "AnglerKing.exe needs DLLs Windows does not ship: $bad"; exit 1; }
(cd "$TOP" && python3 -c "import shutil; shutil.make_archive('windows-release', 'zip', '.', 'windows-release')")
echo "Done:  windows-release/  and  windows-release.zip"
