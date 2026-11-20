#!/bin/bash
# Windows counterpart of build_ffmpeg.sh: slim, static, LGPL FFmpeg built with MSVC (cl.exe)
# into vendor/ffmpeg/windows-x64. Same component set as macOS (all demuxers/decoders/parsers,
# ac3 + dca encoders, spdif muxer), with D3D11VA/DXVA2 in place of VideoToolbox.
#
# Run it via scripts/build_ffmpeg_win.ps1, which sets up the Visual Studio x64 environment
# and starts MSYS2 bash (make, nasm, diffutils, pkgconf) with that environment inherited.
set -euo pipefail
cd "$(dirname "$0")/.."

command -v cl.exe >/dev/null || { echo "cl.exe not on PATH: run scripts/build_ffmpeg_win.ps1"; exit 2; }
# MSYS2's /usr/bin/link.exe (coreutils) would shadow MSVC's linker: put MSVC's bin dir first.
MSVC_BIN=$(dirname "$(command -v cl.exe)")
export PATH="$MSVC_BIN:$PATH"
LINK_BANNER=$(link.exe 2>&1 || true)   # exits non-zero without arguments
case "$LINK_BANNER" in *Microsoft*) ;; *) echo "link.exe is not MSVC's linker: $(command -v link.exe)"; exit 2;; esac

VERSION=9.0.2
SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
ARCH=windows-x64
ROOT="$PWD/vendor/ffmpeg"
SRC_DIR="$ROOT/src"
JOBS=$(nproc)

mkdir -p "$SRC_DIR"
TARBALL="$SRC_DIR/ffmpeg-$VERSION.tar.xz"
if [ ! -f "$TARBALL" ]; then
    curl -fL --proto '=https' -o "$TARBALL.part" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "$SHA256  $TARBALL" | sha256sum -c -

# Bump/extend this when the configure options change; it triggers a rebuild.
OPTIONS="msvc encoders=ac3,dca muxers=spdif hw=d3d11va,dxva2 filters=abuffer,abuffersink,aformat,aresample,anull,surround,bass,lowpass,adelay,alimiter patches=$(cat patches/ffmpeg/*.patch | sha1sum | cut -c1-12) v2"

PREFIX="$ROOT/$ARCH"
if [ -f "$PREFIX/lib/libavformat.a" ] && [ "$(cat "$PREFIX/.options" 2>/dev/null)" = "$OPTIONS" ]; then
    echo "ffmpeg $ARCH already built"; exit 0
fi
rm -rf "$PREFIX"
BUILD="$SRC_DIR/build-$ARCH"
rm -rf "$BUILD"; mkdir -p "$BUILD"
tar -xf "$TARBALL" -C "$BUILD" --strip-components 1
for pf in "$PWD"/patches/ffmpeg/*.patch; do patch -d "$BUILD" -p1 --forward < "$pf"; done
cd "$BUILD"
./configure \
    --prefix="$PREFIX" --toolchain=msvc --arch=x86_64 \
    --enable-static --disable-shared \
    --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-network \
    --disable-autodetect --enable-w32threads --enable-d3d11va --enable-dxva2 \
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
echo "FFmpeg built: $PREFIX"
