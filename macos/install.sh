#!/bin/bash
# macos/install.sh -- install the built Metal apps as self-contained games.
#
#   ./macos/install.sh [apps-dir]        (default /Applications)
#
# Copies the prepared ROM sets (timecris/extracted, tc2/extracted from
# macos/prepare_roms.py) to ~/Library/Application Support/<app name>/extracted,
# where the game also keeps its settings, saves and high scores, and puts the
# two .app bundles into apps-dir. Nothing is replaced: an existing app is
# refused, and an existing ROM file must be byte-identical. The apps then never
# read the (iCloud-synced) source checkout, so there are no folder prompts.
set -euo pipefail
cd "$(dirname "$0")/.."

APPS="${1:-/Applications}"
DATA="$HOME/Library/Application Support"
BUILD="${BUILD_DIR:-macos/output/build}"

for name in "Time Crisis" "Time Crisis II"; do
    [ -e "$APPS/$name.app" ] && { echo "refusing to replace existing $APPS/$name.app" >&2; exit 1; }
done

copy_roms() { # <prepared dir> <app name>
    local src="$1" dst="$DATA/$2/extracted"
    [ -d "$src" ] || { echo "prepare the ROMs first: macos/prepare_roms.py ($src missing)" >&2; exit 1; }
    mkdir -p "$dst"
    for f in "$src"/*; do
        local t="$dst/$(basename "$f")"
        if [ -e "$t" ]; then
            cmp -s "$f" "$t" || { echo "refusing to replace a different $t" >&2; exit 1; }
        else
            cp "$f" "$t"
        fi
    done
    echo "ROMs -> $dst"
}
copy_roms timecris/extracted "Time Crisis"
copy_roms tc2/extracted "Time Crisis II"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/timecrisis-apps.XXXXXX")"
STAGE="$TMP/stage.noindex"                 # .noindex: the staging copies never show up in app search
BUILD_DIR="$BUILD" OUTPUT_DIR="$STAGE" ROM_ROOT="$DATA" ./macos/package.sh
mkdir -p "$APPS"
for name in "Time Crisis" "Time Crisis II"; do
    cp -R "$STAGE/$name.app" "$APPS/$name.app"
    echo "installed $APPS/$name.app"
done
rm -rf "$TMP"
