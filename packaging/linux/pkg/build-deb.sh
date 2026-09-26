#!/bin/sh
# Wrap a staged tree (build-stage.sh) into a .deb.   build-deb.sh STAGE VERSION OUTDIR
# Build inside Ubuntu 22.04 so the package runs on Ubuntu 22.04+, Linux Mint 21+,
# LMDE 6 and Debian 12+ (glibc 2.35 / SDL 2.0.20 baseline).
set -e
STAGE=$1; VER=$2; OUT=$3; PKG=$(cd "$(dirname "$0")" && pwd)
install -d "$STAGE/DEBIAN"
SIZE=$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<CTL
Package: namco22
Version: $VER
Section: games
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.34), libsdl2-2.0-0 (>= 2.0.20), zlib1g, libgl1, libopengl0
Recommends: zenity | kdialog, xdg-utils
Installed-Size: $SIZE
Maintainer: cmcrann <cmcrann@protonmail.com>
Description: Prop Cycle, Rave Racer and Tokyo Wars -- decompiled Namco System 22 engines
 Native, decompiled engines for the Namco arcade games Prop Cycle (1996),
 Rave Racer (1995) and Tokyo Wars (1996), rendered with OpenGL.
 .
 NO GAME DATA IS INCLUDED. After installing, put the MAME ROM sets in
 ~/.local/share/namco22/propcycle/roms/ (propcycl.zip) and
 ~/.local/share/namco22/raverace/roms/ (raverace.zip, namcoc74.zip) and
 ~/.local/share/namco22/tokyowar/roms/ (tokyowar.zip).
 See /usr/share/doc/namco22/README.txt.
CTL
{ echo '#!/bin/sh'; echo 'cat <<"NOTE"'; cat "$PKG/rom-note.txt"; echo 'NOTE'
  echo 'command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true'
  echo 'command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -f /usr/share/icons/hicolor || true'
  echo 'exit 0'; } > "$STAGE/DEBIAN/postinst"
chmod 755 "$STAGE/DEBIAN/postinst"
mkdir -p "$OUT"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT/namco22_${VER}_amd64.deb"
