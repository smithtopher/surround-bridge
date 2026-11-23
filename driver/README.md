# The Surround Bridge audio device (driver/)

Apps play into **"Surround Bridge"**, a 2-channel loopback audio device. Its output side is what
apps play into, and its input side is what the bridge captures. It's built from the source code of
**BlackHole 0.7.1** by Existential Audio Inc.
(<https://github.com/ExistentialAudio/BlackHole>, tag `v0.7.1`, commit `e2b22aa`).

- **Licence:** everything in this folder is under the **GNU General Public License v3.0** (see
  `LICENSE`, BlackHole's own file, kept whole). The rest of Surround Bridge is a separate program:
  it only talks to the device through CoreAudio, like any app, and the installer just puts both on
  the Mac.
- **Not BlackHole:** BlackHole's name and branding belong to Existential Audio Inc. and aren't used
  by this build. The device, box, bundle (`SurroundBridge.driver`, `local.surroundbridge.driver`)
  and UIDs (`SurroundBridge_UID`) all have their own names, so it's installed alongside a real
  BlackHole without clashing. This isn't an official BlackHole build, and Existential Audio
  doesn't support it.
- **Changes to the source** (`BlackHole.c`, marked in the file):
  - The box name, box model name and manufacturer, which were fixed BlackHole strings, now use the
    `kDriver_Name` / `kManufacturer_Name` build definitions.
  - Nothing else.
- **Build definitions** (`scripts/build_driver.sh`):
  - name: `kDriver_Name "SurroundBridge"`, `kDevice_Name "Surround Bridge"`,
    `kHas_Driver_Name_Format false`;
  - bundle: `kPlugIn_BundleID "local.surroundbridge.driver"`;
  - format: `kNumber_Of_Channels 2`, `kSampleRates 48000`. That's the rate the bridge works at, so
    macOS converts other rates itself and nothing ever switches the device's rate.
  - The plug-in factory UUID (`Info.plist`) is our own.
- **Build:** `scripts/build_driver.sh` → `build/driver/SurroundBridge.driver`. It's universal and
  ad-hoc signed, made with clang alone (no Xcode). `scripts/make_installer.sh` puts it in
  `/Library/Audio/Plug-Ins/HAL/` and restarts macOS audio (coreaudiod).
- **Updating:** copy a newer `BlackHole/BlackHole.c` and `LICENSE` from a BlackHole tag, then redo
  the three changes above (search for "Surround Bridge:").
