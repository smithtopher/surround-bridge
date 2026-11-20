#!/bin/bash
# Builds the core library objects and the sbridge CLI into build/<arch>/.
#   scripts/build.sh            (host arch)
#   ARCH=x86_64 scripts/build.sh
# FFmpeg: $FFMPEG_PREFIX, else vendor/ffmpeg/<arch> (scripts/build_ffmpeg.sh), else a sibling
# Surround Player checkout's vendored build (../music_player/vendor/ffmpeg/<arch>), same options.
set -euo pipefail
cd "$(dirname "$0")/.."
ARCH="${ARCH:-$(uname -m)}"
FF="${FFMPEG_PREFIX:-}"
if [ -z "$FF" ]; then
    for c in "vendor/ffmpeg/$ARCH" "../music_player/vendor/ffmpeg/$ARCH"; do
        [ -f "$c/lib/libavfilter.a" ] && { FF="$PWD/$c"; break; }
    done
fi
[ -n "$FF" ] && [ -f "$FF/lib/libavfilter.a" ] || { echo "No FFmpeg with libavfilter: run scripts/build_ffmpeg.sh or set FFMPEG_PREFIX"; exit 1; }
OUT="build/$ARCH"
mkdir -p "$OUT"
CFLAGS=(-arch "$ARCH" -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -mmacosx-version-min=14.0
        -Icore -I"$FF/include")
SRCS=(core/bridge.c core/pc_encode.c core/pc_output.c core/pc_upmix.c
      core/platform/apple/audio_coreaudio.c core/platform/apple/capture_coreaudio.c)
OBJS=()
for src in "${SRCS[@]}"; do
    obj="$OUT/$(basename "${src%.c}").o"
    clang "${CFLAGS[@]}" -c "$src" -o "$obj"
    OBJS+=("$obj")
done
FFLIBS=(-L"$FF/lib" -lavfilter -lavformat -lavcodec -lswresample -lswscale -lavutil -lz -lbz2)
FRAMEWORKS=(-framework CoreAudio -framework AudioToolbox -framework CoreFoundation -framework CoreVideo
            -framework VideoToolbox -framework CoreMedia -framework CoreServices -framework Security)
clang "${CFLAGS[@]}" tools/sbridge.c "${OBJS[@]}" "${FFLIBS[@]}" "${FRAMEWORKS[@]}" -lpthread -o "$OUT/sbridge" 2>&1 \
    | grep -v "no platform load command found" || true
[ -x "$OUT/sbridge" ] && echo "built $OUT/sbridge (FFmpeg: $FF)"
