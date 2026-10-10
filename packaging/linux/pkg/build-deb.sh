#!/bin/sh
# Wrap a staged tree (build-stage.sh) into a .deb.   build-deb.sh STAGE VERSION OUTDIR
# Build inside Ubuntu 22.04 so the package runs on Ubuntu 22.04+, Linux Mint 21+,
# LMDE 6 and Debian 12+ (glibc 2.35 / SDL 2.0.20 baseline).
set -e
STAGE=$1; VER=$2; OUT=$3; PKG=$(cd "$(dirname "$0")" && pwd)
install -d "$STAGE/DEBIAN"
cp "$STAGE/usr/share/doc/namco-2x-systems/LICENSE" "$STAGE/usr/share/doc/namco-2x-systems/copyright"      # Debian looks for /usr/share/doc/PACKAGE/copyright
SIZE=$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<CTL
Package: namco-2x-systems
Version: $VER
Section: games
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.34), libsdl2-2.0-0 (>= 2.0.20), zlib1g, libgl1, libopengl0
Recommends: zenity | kdialog, xdg-utils
Replaces: namco22
Conflicts: namco22
Provides: namco22
Installed-Size: $SIZE
Maintainer: cmcrann <cmcrann@protonmail.com>
Description: Prop Cycle, Rave Racer, Tokyo Wars, Dirt Dash, Time Crisis, Ace Driver, Cyber Commando, Cyber Sled and Time Crisis 2 -- decompiled Namco System 21 / 22 / 23 engines
 Native, decompiled engines for the Namco arcade games Prop Cycle (1996),
 Rave Racer (1995), Tokyo Wars (1996), Dirt Dash (1995), Time Crisis (1995), Ace Driver (1994), Cyber Commando (1994), Cyber Sled (1993) and Time Crisis II (1997), rendered with OpenGL.
 .
 NO GAME DATA IS INCLUDED. After installing, put the MAME ROM sets in
 ~/.local/share/namco-2x-systems/propcycle/roms/ (propcycl.zip) and
 ~/.local/share/namco-2x-systems/raverace/roms/ (raverace.zip, namcoc74.zip) and
 ~/.local/share/namco-2x-systems/tokyowar/roms/ (tokyowar.zip) and
 ~/.local/share/namco-2x-systems/dirtdash/roms/ (dirtdash.zip) and
 ~/.local/share/namco-2x-systems/timecris/roms/ (timecris.zip) and
 ~/.local/share/namco-2x-systems/acedriver/roms/ (acedrive.zip, namcoc74.zip) and
 ~/.local/share/namco-2x-systems/cybrcomm/roms/ (cybrcomm.zip, namcoc74.zip) and
 ~/.local/share/namco-2x-systems/cybsled/roms/ (cybsled.zip, namcoc67.zip, namcoc68.zip) and
 ~/.local/share/namco-2x-systems/tc2/roms/ (timecrs2.zip).
 Folders from an older namco22 install (~/.local/share/namco22/) are used as they are.
 See /usr/share/doc/namco-2x-systems/README.txt.
CTL
{ echo '#!/bin/sh'; echo 'cat <<"NOTE"'; cat "$PKG/rom-note.txt"; echo 'NOTE'
  echo 'command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true'
  echo 'command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -f /usr/share/icons/hicolor || true'
  echo 'exit 0'; } > "$STAGE/DEBIAN/postinst"
chmod 755 "$STAGE/DEBIAN/postinst"
mkdir -p "$OUT"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT/namco-2x-systems_${VER}_amd64.deb"
