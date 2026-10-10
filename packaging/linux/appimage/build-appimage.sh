#!/bin/sh
# One AppImage per game from a staged tree (packaging/linux/pkg/build-stage.sh):
#   build-appimage.sh STAGE VERSION OUTDIR [APPIMAGETOOL]
# Built in the same Ubuntu 22.04 container as the .deb (glibc 2.35 / SDL 2.0.20 baseline), so the files run on the Steam Deck (SteamOS), Ubuntu 22.04+,
# Mint, Debian 12+, Fedora, Arch ... SDL2 is NOT taken from the host if the system has one (the Deck's own SDL2 carries Steam Input); the one in the
# image is the fallback. NO GAME DATA: the ROM zips go in ~/.local/share/namco-2x-systems/<game>/roms/ (the same folders as the .deb), see README.txt in the image.
set -e
STAGE=$(cd "$1" && pwd); VER=$2; OUT=$3
TOOL=${4:-/opt/appimagetool/appimagetool-x86_64.AppImage}
HERE=$(cd "$(dirname "$0")" && pwd); PKG=$(cd "$HERE/../pkg" && pwd)
mkdir -p "$OUT"
SDL=$(ldconfig -p | awk '/libSDL2-2.0.so.0 /{print $NF; exit}')
[ -n "$SDL" ] || { echo "no libSDL2-2.0.so.0 to bundle"; exit 1; }
# game: AppImage name | launcher key | binary | desktop/icon name
for g in "PropCycle|prop|propcycl|propcycle" "RaveRacer|rave|rr|raveracer" "TokyoWars|tokyo|tw|tokyowars" "DirtDash|dirt|dd|dirtdash" "TimeCrisis|tc|tc|timecrisis" "AceDriver|ace|ad|acedriver" "CyberCommando|cc|cc|cybercommando" "CyberSled|cs|cs21|cybersled" "TimeCrisis2|tc2|tc2|timecrisis2"; do
    IFS='|' read -r NAME KEY BIN ICON <<EOF
$g
EOF
    AD=$(mktemp -d)/$NAME.AppDir
    mkdir -p "$AD/usr/lib/namco-2x-systems" "$AD/usr/lib/bundled" "$AD/usr/share/doc/namco-2x-systems"
    cp "$STAGE/usr/lib/namco-2x-systems/$BIN" "$STAGE/usr/lib/namco-2x-systems/namco-2x-launch" "$AD/usr/lib/namco-2x-systems/"
    cp -L "$SDL" "$AD/usr/lib/bundled/libSDL2-2.0.so.0"
    cp "$STAGE/usr/share/doc/namco-2x-systems/"* "$AD/usr/share/doc/namco-2x-systems/"
    cp "$PKG/$ICON.desktop" "$AD/$ICON.desktop"
    cp "$PKG/$ICON.png" "$AD/$ICON.png"; cp "$PKG/$ICON.png" "$AD/.DirIcon"
    cat > "$AD/AppRun" <<RUN
#!/bin/sh
# $NAME: starts the game through the same launcher as the .deb (ROM folder, first-run unpack, settings under ~/.local/share/namco-2x-systems/)
HERE=\$(dirname "\$(readlink -f "\$0")")
export NAMCO2X_LIB="\$HERE/usr/lib/namco-2x-systems" NAMCO2X_APPIMAGE=1
# SDL2: the system's if there is one (SteamOS has it, with Steam Input), else the copy in this image
LDC=\$(command -v ldconfig || echo /sbin/ldconfig)
if ! "\$LDC" -p 2>/dev/null | grep -q 'libSDL2-2.0.so.0'; then
    export LD_LIBRARY_PATH="\$HERE/usr/lib/bundled\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
fi
exec "\$HERE/usr/lib/namco-2x-systems/namco-2x-launch" $KEY "\$@"
RUN
    chmod 755 "$AD/AppRun"
    ARCH=x86_64 "$TOOL" --appimage-extract-and-run --no-appstream "$AD" "$OUT/$NAME-x86_64.AppImage"   # no version in the name: an update overwrites the file (Steam shortcuts keep working)
done
ls -la "$OUT"/*.AppImage
