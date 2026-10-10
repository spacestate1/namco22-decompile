#!/bin/bash
# THE RELEASE BUILD of Time Crisis 2: every package from the PUBLIC tree (tools/export_public.sh) with only the ROM zip -- so it is
# also the fresh-clone test: a file the public tree forgets fails here, not on a player's machine.
#   tools/release.sh VERSION [ROMDIR]          ROMDIR (default /storage02/roms/mame) holds timecrs2.zip; it is read at BUILD time only
#                                              (the two H8 programs are translated from it) and never copied into a package.
# Output in release/VERSION/: TimeCrisis2-x86_64.AppImage (built in Ubuntu 22.04: runs on 22.04+ / the Steam Deck), TimeCrisis2-windows-
# VERSION.zip (MinGW, on this machine), tc2-VERSION-src.tar.gz, SHA256SUMS. Needs podman (ENGINE=docker), the MinGW-w64 cross compiler.
# STEPS="src appimage win" picks steps; JOBS limits parallel compiles; MEM caps the container.
set -euo pipefail
VER=${1:?usage: tools/release.sh VERSION [ROMDIR]}
ROMS=$(cd "${2:-/storage02/roms/mame}" && pwd)
TOP=$(cd "$(dirname "$0")/.." && pwd)
OUT="$TOP/release/$VER"; WORK="$TOP/work/release-$VER"
ENGINE=${ENGINE:-podman}; STEPS=${STEPS:-src appimage win}; JOBS=${JOBS:-3}; MEM=${MEM:-10g}
[ -f "$ROMS/timecrs2.zip" ] || { echo "missing $ROMS/timecrs2.zip"; exit 1; }
step() { case " $STEPS " in *" $1 "*) return 0;; esac; return 1; }
log() { printf '\n=== %s ===\n' "$*"; }
mkdir -p "$OUT" "$WORK"
log "export the public tree"
SRC="$WORK/src"; rm -rf "$SRC"; mkdir -p "$SRC"
"$TOP/tools/export_public.sh" "$SRC"
if step src; then tar -C "$WORK" --transform "s|^src|tc2-$VER|" -czf "$OUT/tc2-$VER-src.tar.gz" src; fi
if step appimage; then
    log "AppImage (ubuntu:22.04)"
    tree="$WORK/tree-linux"; rm -rf "$tree"; cp -a "$SRC" "$tree"
    $ENGINE run --rm --memory="$MEM" --memory-swap="$MEM" -e JOBS="$JOBS" -v "$tree:/src" -v "$ROMS:/roms:ro" -v "$OUT:/out" \
        -v "$TOP/build-linux:/opt/appimagetool:ro" docker.io/library/ubuntu:22.04 bash -euc '
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends gcc make cmake pkg-config python3 libsdl2-dev zlib1g-dev \
            libgl-dev file ca-certificates >/dev/null
        cd /src && python3 tools/setup_roms.py /roms/timecrs2.zip && python3 tools/make_prog.py extracted >/dev/null
        cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORTABLE=ON >/dev/null && cmake --build build --target tc2 -j"$JOBS"
        APPIMAGETOOL=/opt/appimagetool/appimagetool-x86_64.AppImage sh packaging/linux/build-appimage.sh build/tc2 /out'
fi
if step win; then
    log "Windows (MinGW-w64)"
    tree="$WORK/tree-win"; rm -rf "$tree"; cp -a "$SRC" "$tree"
    [ -d "$TOP/build-win/deps" ] && { mkdir -p "$tree/build-win"; cp -a "$TOP/build-win/deps" "$tree/build-win/"; }
    ( cd "$tree" && python3 tools/setup_roms.py "$ROMS/timecrs2.zip" && JOBS=$JOBS ./build-windows.sh )
    rm -rf "$tree/windows-release/extracted" "$tree/windows-release/roms/"*.zip
    rm -f "$OUT/TimeCrisis2-windows-$VER.zip"
    ( cd "$tree" && mv windows-release "TimeCrisis2-$VER" && python3 -c "import shutil; shutil.make_archive('$OUT/TimeCrisis2-windows-$VER', 'zip', '.', 'TimeCrisis2-$VER')" )
fi
log "checksums"
( cd "$OUT" && rm -f SHA256SUMS && sha256sum -- *.tar.gz *.zip *.AppImage 2>/dev/null > SHA256SUMS || true )
ls -la "$OUT"
for f in "$OUT"/*.zip "$OUT"/*.tar.gz "$OUT"/*.AppImage; do      # the packages must hold no game data
    [ -f "$f" ] || continue
    case "$f" in
        *.zip) list=$(python3 -c "import zipfile,sys; print('\n'.join(zipfile.ZipFile(sys.argv[1]).namelist()))" "$f") ;;
        *.tar.gz) list=$(tar -tzf "$f") ;;
        *.AppImage) x=$(mktemp -d); ( cd "$x" && "$f" --appimage-extract >/dev/null 2>&1 ); list=$(cd "$x" && find squashfs-root -type f); rm -rf "$x" ;;
    esac
    if echo "$list" | grep -qiE '\.(zip|bin|dasm)$|/extracted/|tss[13]|tssioprog'; then echo "GAME DATA IN $f:"; echo "$list" | grep -iE '\.(zip|bin|dasm)$|/extracted/|tss[13]|tssioprog' | head; exit 1; fi
done
echo "no game data in any package"
