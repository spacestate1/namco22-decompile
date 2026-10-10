#!/bin/bash
# tools/export_public.sh DEST -- the PUBLIC source tree of Time Crisis 2: what a player needs to build and play it from their own
# timecrs2.zip, and nothing else -- no ROM data, no Ghidra project, no MAME traces or disassembly (cov/*.dasm), no scratch.
# Included: the game program as translated C (gen/tc2_lifted_*.c: made with Ghidra, which a player should not need), the H8 coverage
# ADDRESSES the build-time translator reads (cov/h8*.pcs/.calls/.jumps: numbers only), the sources, the tools the build runs.
set -e
DEST=${1:?usage: tools/export_public.sh DEST}
cd "$(dirname "$0")/.."
mkdir -p "$DEST"
FILES=(CMakeLists.txt build.sh launch.sh build-windows.sh README.md LICENSE
       tools/make_prog.py tools/setup_roms.py tools/export_public.sh tools/release.sh
       gen/tc2_lifted.h gen/tc2_lifted_tab.c)
for f in gen/tc2_lifted_[0-9][0-9].c include/*.h src/*.c src/*.h src/eng/*.c src/eng/*.h third_party/* \
         tools/h8/h8_translate.py tools/h8/h8_decode.py tools/h8/h8_ops.txt tools/h8/h8_timing.txt \
         cov/h8sub_*.pcs cov/h8sub_*.calls cov/h8sub_*.jumps cov/h8io_*.pcs cov/h8io_*.calls cov/h8io_*.jumps; do
    [ -e "$f" ] && FILES+=("$f")
done
while IFS= read -r f; do FILES+=("$f"); done < <(find packaging -type f)
for f in "${FILES[@]}"; do
    [ -f "$f" ] || { echo "missing: $f"; exit 1; }
    mkdir -p "$DEST/$(dirname "$f")"; cp -p "$f" "$DEST/$f"
done
cat > "$DEST/.gitignore" <<'IGN'
# Game files are yours and never belong in git.
/extracted/
/roms/
*.zip
# Made by the build or the game.
/build/
/build-win/
/windows-release/
/boot.bin
/prog.bin
/sub.bin
/io.bin
/gen/tc2_sub.c
/gen/tc2_io.c
/tc2.cfg
/tc2.log
/tc2_backup.nv
/tc2_backup.nv.tmp
/tc2_last.rec
/tc2_prev.rec
/screenshots/
__pycache__/
IGN
echo "Exported ${#FILES[@]} files to $DEST"
