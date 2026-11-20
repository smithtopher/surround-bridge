#!/bin/bash
# Builds a slim, static, LGPL FFmpeg for arm64 and x86_64 into vendor/ffmpeg/<arch>.
# Only what the player needs: all demuxers/decoders/parsers, the AC3 encoder,
# the AC3 and DTS (dca) encoders, the spdif (IEC 61937) muxer, VideoToolbox. Needs nasm (`brew install nasm`)
# for the x86_64 assembly.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=9.0.2
SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
MIN_MACOS=14.0
ARCHS="${ARCHS:-arm64 x86_64}"
ROOT="$PWD/vendor/ffmpeg"
SRC_DIR="$ROOT/src"
JOBS=$(sysctl -n hw.ncpu)

mkdir -p "$SRC_DIR"
TARBALL="$SRC_DIR/ffmpeg-$VERSION.tar.xz"
if [ ! -f "$TARBALL" ]; then
    curl -fL --proto '=https' -o "$TARBALL.part" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "$SHA256  $TARBALL" | shasum -a 256 -c -

# Bump/extend this when the configure options change; it triggers a rebuild.
OPTIONS="encoders=ac3,dca muxers=spdif filters=abuffer,abuffersink,aformat,aresample,anull,surround,bass,lowpass,adelay,alimiter patches=$(cat patches/ffmpeg/*.patch | shasum | cut -c1-12) v3"

for ARCH in $ARCHS; do
    PREFIX="$ROOT/$ARCH"
    if [ -f "$PREFIX/lib/libavformat.a" ] && [ "$(cat "$PREFIX/.options" 2>/dev/null)" = "$OPTIONS" ]; then
        echo "ffmpeg $ARCH already built"; continue
    fi
    rm -rf "$PREFIX"
    BUILD="$SRC_DIR/build-$ARCH"
    rm -rf "$BUILD"; mkdir -p "$BUILD"
    tar -xf "$TARBALL" -C "$BUILD" --strip-components 1
    # local fixes to FFmpeg (see patches/ffmpeg/*.patch for what and why)
    for pf in "$PWD"/patches/ffmpeg/*.patch; do patch -d "$BUILD" -p1 --forward < "$pf"; done
    cd "$BUILD"
    ASM=()
    if [ "$ARCH" = x86_64 ] && ! command -v nasm >/dev/null; then
        echo "warning: nasm not found, building x86_64 without assembly (slower)"; ASM=(--disable-x86asm)
    fi
    CROSS=()
    [ "$ARCH" != "$(uname -m)" ] && CROSS=(--enable-cross-compile)
    ./configure \
        --prefix="$PREFIX" --arch="$ARCH" --target-os=darwin ${CROSS[@]+"${CROSS[@]}"} ${ASM[@]+"${ASM[@]}"} \
        --cc="clang -arch $ARCH" \
        --extra-cflags="-mmacosx-version-min=$MIN_MACOS" --extra-ldflags="-mmacosx-version-min=$MIN_MACOS" \
        --enable-static --disable-shared --enable-pic \
        --disable-programs --disable-doc --disable-debug \
        --disable-avdevice --disable-network \
        --disable-autodetect --enable-videotoolbox --enable-zlib --enable-bzlib \
        --disable-encoders --enable-encoder=ac3,dca \
        --disable-muxers --enable-muxer=spdif \
        --disable-filters --enable-filter=abuffer,abuffersink,aformat,aresample,anull,surround,bass,lowpass,adelay,alimiter \
        --disable-protocols --enable-protocol=file \
        --disable-indevs --disable-outdevs
    make -j"$JOBS"
    make install
    echo "$OPTIONS" > "$PREFIX/.options"
    cd - >/dev/null
    rm -rf "$BUILD"
done
echo "FFmpeg built for: $ARCHS"
