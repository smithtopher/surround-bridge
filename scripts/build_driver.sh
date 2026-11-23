#!/bin/bash
# Builds the virtual audio device: build/driver/SurroundBridge.driver (universal, ad-hoc signed).
# It's BlackHole 0.7.1's source (driver/BlackHole.c, GPL-3, unmodified) configured with build
# definitions only: our own name, UIDs and bundle ID, 2 channels, 48 kHz. See driver/README.md.
# Installed by the .pkg into /Library/Audio/Plug-Ins/HAL (scripts/make_installer.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" Resources/Info.plist)
OUT=build/driver/SurroundBridge.driver
rm -rf "$OUT" build/driver/obj
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources" build/driver/obj

DEFS=(
    -DkDriver_Name='"SurroundBridge"'
    -DkPlugIn_BundleID='"local.surroundbridge.driver"'
    -DkPlugIn_Icon='"SurroundBridge.icns"'
    -DkHas_Driver_Name_Format=false
    -DkDevice_Name='"Surround Bridge"'
    -DkDevice2_Name='"Surround Bridge Mirror"'
    -DkManufacturer_Name='"Surround Bridge"'
    -DkNumber_Of_Channels=2
    -DkSampleRates=48000
)
for arch in arm64 x86_64; do
    clang -arch $arch -mmacosx-version-min=14.0 -Os -std=gnu11 -Wall -Wno-unused-parameter -Wno-format-extra-args -Wno-unused-but-set-variable \
        -fvisibility=default "${DEFS[@]}" -bundle -o build/driver/obj/SurroundBridge-$arch \
        driver/BlackHole.c -framework CoreAudio -framework CoreFoundation -framework Accelerate
done
lipo -create build/driver/obj/SurroundBridge-arm64 build/driver/obj/SurroundBridge-x86_64 \
    -output "$OUT/Contents/MacOS/SurroundBridge"
sed "s/@VERSION@/$VERSION/g" driver/Info.plist > "$OUT/Contents/Info.plist"
cp Resources/AppIcon.icns "$OUT/Contents/Resources/SurroundBridge.icns"
cp driver/LICENSE "$OUT/Contents/Resources/LICENSE-BlackHole.txt"
codesign --force --sign - "$OUT" >/dev/null
nm -gU "$OUT/Contents/MacOS/SurroundBridge" | grep -q "_BlackHole_Create" || { echo "factory symbol missing"; exit 1; }
echo "built $OUT ($VERSION, $(lipo -archs "$OUT/Contents/MacOS/SurroundBridge"))"
