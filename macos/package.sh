#!/bin/bash
# macos/package.sh -- bundle the built Metal binaries as macOS .app bundles.
#
# Reads the binaries produced by macos/build.sh (in macos/output/build) and
# writes .app bundles into macos/output/. ROMs are NOT
# copied: each launcher points at the prepared <game>/extracted directory
# (override with TIMECRIS_ROMS / TIMECRIS2_ROMS) -- the originals stay read-only.
# ROM_ROOT=<dir> points the launchers at <dir>/<app name>/extracted instead
# (macos/install.sh uses ~/Library/Application Support). The game's settings,
# saves and high scores live next to that extracted folder; each start writes
# its log to ~/Library/Logs/<app name>/last-run.log. F11 is fullscreen.
#
# The bundle executable is macos/launcher.c, a compiled Mach-O that execs the
# game: macOS Game Mode never classifies an app whose executable is a script.
#
# SDL goes inside each app (Contents/Frameworks): Homebrew's sdl2-compat and
# the SDL3 it loads (sdl2-compat looks for @loader_path/libSDL3.dylib first),
# with the install names rewritten and the bundle signed again (ad hoc), so the
# app runs on a Mac without Homebrew. Package outside iCloud-synced folders:
# the file provider tags the bundle and codesign refuses it.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD="${BUILD_DIR:-macos/output/build}"
OUT="${OUTPUT_DIR:-macos/output}"

[ -x "$BUILD/Time Crisis" ] || { echo "build first: ./macos/build.sh"; exit 1; }

package_one() { # <display-name> <bundle-id> <binary> <rom-env> <default-roms> <roms-as-argv1>
    local name="$1" id="$2" bin="$3" env="$4" roms="$5" roms_arg="$6"
    [ -n "${ROM_ROOT:-}" ] && roms="$ROM_ROOT/$name/extracted"
    local app="$OUT/$name.app"
    if [ -e "$app" ]; then
        echo "refusing to replace existing bundle: $app (set OUTPUT_DIR to a new directory)" >&2
        return 1
    fi
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"

    cp "$BUILD/$bin" "$app/Contents/MacOS/$bin"
    clang -O2 -Wall -o "$app/Contents/MacOS/launcher" macos/launcher.c \
        -DLAUNCH_NAME="\"$name\"" -DLAUNCH_BIN="\"$bin\"" -DLAUNCH_ROM_ENV="\"$env\"" \
        -DLAUNCH_ROMS="\"$roms\"" -DLAUNCH_ROMS_ARG="$roms_arg"

    bundle_sdl "$app" "$bin"

    cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$name</string>
  <key>CFBundleDisplayName</key><string>$name</string>
  <key>CFBundleIdentifier</key><string>$id</string>
  <key>CFBundleExecutable</key><string>launcher</string>
  <key>CFBundleVersion</key><string>1.0</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
  <key>LSSupportsGameMode</key><true/>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
</dict></plist>
EOF
    xattr -cr "$app"
    codesign --force --sign - "$app" >/dev/null 2>&1   # seals the bundle (the Frameworks and the game were signed one by one)
    codesign --verify --strict "$app" || { echo "signature check failed: $app (inside an iCloud folder?)" >&2; return 1; }
    echo "packaged $app"
}

bundle_sdl() { # <app> <binary>: Homebrew's SDL into Contents/Frameworks, the game pointed at it
    local app="$1" bin="$2" fw="$1/Contents/Frameworks"
    local sdl2 sdl3
    sdl2=$(otool -L "$app/Contents/MacOS/$bin" | awk '/libSDL2-2.0.0.dylib/ {print $1; exit}')
    [ -n "$sdl2" ] || { echo "the game does not link SDL2?" >&2; return 1; }
    sdl3="$(brew --prefix sdl3 2>/dev/null || echo /opt/homebrew/opt/sdl3)/lib/libSDL3.0.dylib"
    [ -f "$sdl3" ] || { echo "no SDL3 at $sdl3 (brew install sdl3)" >&2; return 1; }
    mkdir -p "$fw"
    cp "$(realpath "$sdl2")" "$fw/libSDL2-2.0.0.dylib"
    cp "$(realpath "$sdl3")" "$fw/libSDL3.dylib"
    chmod u+w "$fw"/*.dylib
    install_name_tool -id @executable_path/../Frameworks/libSDL2-2.0.0.dylib "$fw/libSDL2-2.0.0.dylib" 2>/dev/null
    install_name_tool -id @loader_path/libSDL3.dylib "$fw/libSDL3.dylib" 2>/dev/null
    # the game is re-pointed and signed outside the bundle: signed in place it would be sealed to the bundle as it is now
    local tmp; tmp=$(mktemp -d)
    cp "$app/Contents/MacOS/$bin" "$tmp/game"
    install_name_tool -change "$sdl2" @executable_path/../Frameworks/libSDL2-2.0.0.dylib "$tmp/game" 2>/dev/null
    if otool -L "$tmp/game" | grep -q /opt/homebrew; then echo "the game still links Homebrew" >&2; return 1; fi
    xattr -c "$tmp/game"; codesign --force --sign - --identifier "$(basename "$app" .app | tr ' ' '-')-game" "$tmp/game" >/dev/null 2>&1
    mv -f "$tmp/game" "$app/Contents/MacOS/$bin"; rmdir "$tmp"
    xattr -cr "$app"                                    # Finder info and the like: codesign refuses files carrying them
    codesign --force --sign - "$fw/libSDL3.dylib" "$fw/libSDL2-2.0.0.dylib" >/dev/null 2>&1
}

package_one "Time Crisis" "com.namco22.timecris" "Time Crisis" \
    "TIMECRIS_ROMS" "$(cd timecris && pwd)/extracted" 1
if [ -x "$BUILD/Time Crisis II" ]; then
    package_one "Time Crisis II" "com.namco22.timecris2" "Time Crisis II" \
        "TIMECRIS2_ROMS" "$(cd tc2 && pwd)/extracted" 0
fi
echo "done -> $OUT"
