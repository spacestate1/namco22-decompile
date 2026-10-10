#!/bin/sh
# packaging/linux/build-appimage.sh BIN OUTDIR -- wrap a built tc2 (PORTABLE: glibc >= 2.34) into TimeCrisis2-x86_64.AppImage.
# SDL2 is bundled as a FALLBACK only: the AppRun uses the system's libSDL2 when there is one (SteamOS has it, with Steam Input).
# Needs appimagetool ($APPIMAGETOOL, or appimagetool-x86_64.AppImage on the PATH / in build-linux/).
set -e
BIN=$1; OUT=$2
HERE=$(cd "$(dirname "$0")" && pwd); TOP=$(cd "$HERE/../.." && pwd)
TOOL=${APPIMAGETOOL:-$(command -v appimagetool-x86_64.AppImage || echo "$TOP/build-linux/appimagetool-x86_64.AppImage")}
[ -x "$TOOL" ] || { echo "no appimagetool ($TOOL)"; exit 1; }
SDL=$(ldconfig -p | awk '/libSDL2-2.0.so.0 /{print $NF; exit}')
[ -n "$SDL" ] || { echo "no libSDL2-2.0.so.0 to bundle"; exit 1; }
AD=$(mktemp -d)/TimeCrisis2.AppDir
mkdir -p "$AD/usr/lib/tc2" "$AD/usr/lib/bundled" "$AD/usr/share/doc/tc2" "$OUT"
install -m755 -s "$BIN" "$AD/usr/lib/tc2/tc2"
install -m755 "$HERE/tc2-launch" "$AD/usr/lib/tc2/tc2-launch"
cp -L "$SDL" "$AD/usr/lib/bundled/libSDL2-2.0.so.0"
for f in "$TOP/README.md" "$TOP/LICENSE" "$HERE/HOW-TO-PLAY.txt"; do [ -f "$f" ] && cp "$f" "$AD/usr/share/doc/tc2/"; done
cp "$HERE/timecrisis2.desktop" "$AD/timecrisis2.desktop"
cp "$HERE/timecrisis2.png" "$AD/timecrisis2.png"; cp "$HERE/timecrisis2.png" "$AD/.DirIcon"
cat > "$AD/AppRun" <<'RUN'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
export TC2_LIB="$HERE/usr/lib/tc2"
LDC=$(command -v ldconfig || echo /sbin/ldconfig)
if ! "$LDC" -p 2>/dev/null | grep -q 'libSDL2-2.0.so.0'; then
    export LD_LIBRARY_PATH="$HERE/usr/lib/bundled${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec "$HERE/usr/lib/tc2/tc2-launch" "$@"
RUN
chmod 755 "$AD/AppRun"
ARCH=x86_64 "$TOOL" --appimage-extract-and-run --no-appstream "$AD" "$OUT/TimeCrisis2-x86_64.AppImage"
ls -la "$OUT/TimeCrisis2-x86_64.AppImage"
