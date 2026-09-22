#!/bin/bash
# Install everything needed to build Prop Cycle.
#
#   ./install-deps.sh
#
# Works out which Linux you are on and asks its package manager for:
# a C compiler (gcc), make, CMake, pkg-config, SDL2, OpenGL, zlib and
# Python 3 with numpy. It will ask for your password once (sudo).
set -e

if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo"; fi

# Only used to name the distro in the error message below.
PRETTY_NAME=""
if [ -r /etc/os-release ]; then
    . /etc/os-release
fi

say() { printf '\n==> %s\n\n' "$*"; }

if command -v apt-get >/dev/null 2>&1; then
    say "Debian / Ubuntu family detected -- using apt"
    $SUDO apt-get update
    $SUDO apt-get install -y build-essential cmake pkg-config \
        libsdl2-dev libgl-dev zlib1g-dev python3 python3-numpy
elif command -v dnf >/dev/null 2>&1; then
    say "Fedora / RHEL family detected -- using dnf"
    $SUDO dnf install -y gcc make cmake pkgconf-pkg-config \
        SDL2-devel mesa-libGL-devel zlib-devel python3 python3-numpy
elif command -v pacman >/dev/null 2>&1; then
    say "Arch family detected -- using pacman"
    $SUDO pacman -S --needed --noconfirm base-devel cmake pkgconf \
        sdl2 mesa zlib python python-numpy
elif command -v zypper >/dev/null 2>&1; then
    say "openSUSE detected -- using zypper"
    $SUDO zypper --non-interactive install gcc make cmake pkg-config \
        SDL2-devel Mesa-libGL-devel zlib-devel python3 python3-numpy
elif command -v apk >/dev/null 2>&1; then
    say "Alpine detected -- using apk"
    $SUDO apk add build-base cmake pkgconf sdl2-dev mesa-dev zlib-dev \
        python3 py3-numpy
elif command -v xbps-install >/dev/null 2>&1; then
    say "Void detected -- using xbps"
    $SUDO xbps-install -Sy gcc make cmake pkg-config SDL2-devel \
        MesaLib-devel zlib-devel python3 python3-numpy
elif command -v emerge >/dev/null 2>&1; then
    say "Gentoo detected -- using emerge"
    $SUDO emerge --noreplace dev-build/cmake dev-util/pkgconf \
        media-libs/libsdl2 media-libs/mesa sys-libs/zlib dev-python/numpy
else
    echo "Sorry, I don't recognise this Linux (${PRETTY_NAME:-unknown})."
    echo "Please install these with your package manager, then run ./build.sh:"
    echo "  gcc, make, cmake, pkg-config, SDL2 (development files),"
    echo "  OpenGL/Mesa (development files), zlib (development files),"
    echo "  python3 and python3 numpy"
    exit 1
fi

say "All dependencies installed. Next step:  ./build.sh /path/to/propcycl.zip"
