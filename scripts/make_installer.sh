#!/bin/bash
# Builds the installer: build/SurroundBridge-<version>.pkg, with two components:
#   - SurroundBridge.driver (the "Surround Bridge" audio device, built from BlackHole's GPL-3 source
#     by build_driver.sh) -> /Library/Audio/Plug-Ins/HAL, then coreaudiod restarts;
#   - the universal app -> /Applications, then it opens.
#   scripts/make_installer.sh          (builds the app first if needed)
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" Resources/Info.plist)
APP="build/universal/Surround Bridge.app"
[ -d "$APP" ] || scripts/build_app.sh
[ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist")" = "$VERSION" ] \
    || { echo "build/universal app isn't $VERSION: run scripts/build_app.sh"; exit 1; }

scripts/build_driver.sh >/dev/null
DRIVER=build/driver/SurroundBridge.driver

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/root/Applications" "$W/pkgs"
ditto "$APP" "$W/root/Applications/Surround Bridge.app"
# always /Applications, even if a copy of the app exists elsewhere (no relocation)
pkgbuild --analyze --root "$W/root" "$W/component.plist" >/dev/null
/usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" "$W/component.plist"
pkgbuild --root "$W/root" --component-plist "$W/component.plist" --install-location / \
    --scripts installer/scripts --identifier local.surroundbridge.app --version "$VERSION" \
    "$W/pkgs/SurroundBridge-app.pkg" >/dev/null
mkdir -p "$W/droot"
ditto "$DRIVER" "$W/droot/SurroundBridge.driver"
pkgbuild --analyze --root "$W/droot" "$W/dcomponent.plist" >/dev/null
/usr/libexec/PlistBuddy -c "Add :0:BundleIsRelocatable bool false" "$W/dcomponent.plist"
pkgbuild --root "$W/droot" --component-plist "$W/dcomponent.plist" \
    --install-location /Library/Audio/Plug-Ins/HAL --scripts installer/driver-scripts \
    --identifier local.surroundbridge.driver --version "$VERSION" \
    "$W/pkgs/SurroundBridge-driver.pkg" >/dev/null
sed "s/@VERSION@/$VERSION/" installer/distribution.xml > "$W/distribution.xml"
OUT="build/SurroundBridge-$VERSION.pkg"
productbuild --distribution "$W/distribution.xml" --resources installer/resources --package-path "$W/pkgs" "$OUT" >/dev/null
echo "built $OUT ($(du -h "$OUT" | cut -f1 | tr -d ' '); unsigned: first open with right-click > Open)"
