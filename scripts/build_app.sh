#!/bin/bash
# Builds Surround Bridge for Apple Silicon, Intel and as a universal app:
#   build/{arm64,x86_64,universal}/Surround Bridge.app  (+ zips in build/)
# FFmpeg is linked statically, so the app needs nothing installed (except the virtual audio
# device, which the installer provides: scripts/make_installer.sh).
# ARCHS="arm64" scripts/build_app.sh builds a single architecture.
set -euo pipefail
cd "$(dirname "$0")/.."

ARCHS="${ARCHS:-arm64 x86_64}"
MIN_MACOS=14.0
NAME="Surround Bridge"
EXE=SurroundBridge
VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" Resources/Info.plist)
[ -f Resources/AppIcon.icns ] || scripts/make_icns.sh

ffmpeg_for() { # arch -> prefix: vendor/, else a sibling Surround Player checkout's (same options)
    for c in "vendor/ffmpeg/$1" "../music_player/vendor/ffmpeg/$1"; do
        [ -f "$c/lib/libavfilter.a" ] && { echo "$PWD/$c"; return; }
    done
    echo "No FFmpeg with libavfilter for $1: run ARCHS=$1 scripts/build_ffmpeg.sh" >&2; exit 1
}

make_bundle() { # <dir> <binary>
    local app="$1/$NAME.app"
    rm -rf "$app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cp "$2" "$app/Contents/MacOS/$EXE"
    cp Resources/Info.plist "$app/Contents/Info.plist"
    cp Resources/AppIcon.icns "$app/Contents/Resources/AppIcon.icns"
    codesign --force --sign - "$app"
    if otool -L "$app/Contents/MacOS/$EXE" | grep -E "^\s+/" | grep -vE "^\s+/(System|usr/lib)/"; then
        echo "error: non-system library linked"; exit 1
    fi
}

BINS=()
for ARCH in $ARCHS; do
    echo "== $ARCH"
    FF=$(ffmpeg_for "$ARCH")
    rm -f ".build/$ARCH/release/$EXE"   # SwiftPM doesn't track the static FFmpeg libs: force a relink
    FFMPEG_PREFIX="$FF" swift build -c release --product "$EXE" \
        --triple "$ARCH-apple-macosx$MIN_MACOS" --scratch-path ".build/$ARCH" 2>&1 \
        | grep -v "no platform load command found" | grep -E "error|warning: .*Sources|Compiling|Build" | grep -v Compiling || true
    BIN=".build/$ARCH/release/$EXE"
    [ -x "$BIN" ] || { echo "build failed for $ARCH"; exit 1; }
    BINS+=("$BIN")
    make_bundle "build/$ARCH" "$BIN"
done

if [ ${#BINS[@]} -gt 1 ]; then
    mkdir -p build/universal
    lipo -create "${BINS[@]}" -output "build/universal/$EXE"
    make_bundle build/universal "build/universal/$EXE"
    rm "build/universal/$EXE"
fi

for dir in build/arm64 build/x86_64 build/universal; do
    [ -d "$dir/$NAME.app" ] || continue
    label=$(basename "$dir"); [ "$label" = x86_64 ] && label=intel; [ "$label" = arm64 ] && label=apple-silicon
    zip="build/SurroundBridge-$VERSION-$label.zip"
    rm -f "$zip"
    ditto -c -k --keepParent "$dir/$NAME.app" "$zip"
    echo "$(lipo -archs "$dir/$NAME.app/Contents/MacOS/$EXE")	$zip"
done
