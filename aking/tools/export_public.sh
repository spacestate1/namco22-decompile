#!/bin/bash
# tools/export_public.sh DEST -- the PUBLIC source tree of Angler King: what a player needs to build and play it from their own aking.zip
# (beside ../tc2's export: Angler King builds against the System 23 host there), and nothing else -- no ROM data, no Ghidra project,
# no MAME traces or disassembly (cov/*.dasm), no scratch. Included: the game program as translated C (gen/aking_lifted_*.c: made with
# Ghidra, which a player should not need), the H8 coverage ADDRESSES the build-time translator reads (cov/h8*.pcs/.calls/.jumps: numbers
# only), the English subtitles, the sources and the tools the build runs.
set -e
DEST=${1:?usage: tools/export_public.sh DEST}
cd "$(dirname "$0")/.."
mkdir -p "$DEST"
FILES=(CMakeLists.txt build.sh launch.sh build-windows.sh README.md aking_subtitles.txt
       tools/make_prog.py tools/setup_roms.py tools/export_public.sh
       include/tc2_lifted.h gen/aking_lifted.h gen/aking_lifted_tab.c src/ak_game.c)
for f in gen/aking_lifted_[0-9][0-9].c cov/h8sub_*.pcs cov/h8sub_*.calls cov/h8sub_*.jumps cov/h8io_*.pcs cov/h8io_*.calls cov/h8io_*.jumps; do
    [ -e "$f" ] && FILES+=("$f")
done
ls gen/aking_lifted_[0-9][0-9].c >/dev/null 2>&1 || { echo "gen/aking_lifted_NN.c missing: run the lift first (tools/relift.sh)"; exit 1; }
for f in "${FILES[@]}"; do
    [ -f "$f" ] || { echo "missing: $f"; exit 1; }
    mkdir -p "$DEST/$(dirname "$f")"; cp -p "$f" "$DEST/$f"
done
[ -f ../LICENSE ] && cp -p ../LICENSE "$DEST/LICENSE"
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
/gen/ak_sub.c
/gen/ak_io.c
/aking.cfg
/tc2.log
/aking*.log
/aking_backup.nv
/aking_backup.nv.tmp
/aking_last.rec
/aking_prev.rec
/screenshots/
__pycache__/
IGN
echo "Exported ${#FILES[@]} files to $DEST"
