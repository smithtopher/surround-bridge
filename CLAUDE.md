# Surround Bridge — working notes

System-wide "Dolby Digital Live" for macOS and Windows: apps play into a virtual audio device,
the bridge upmixes stereo to 5.1, encodes AC3/DTS and sends IEC 61937 to an HDMI/optical output.
Sister project of Surround Player (`~/code/music_player`, github smithtopher/SurroundPlayer), which
shares the output, upmix and encode code (copied, see SHARED.md). README.md is the user-facing doc,
docs/PLAN.md the roadmap.

## Status (2026-11-21)
- **1.0.0: menu-bar app + installer** (`Sources/SurroundBridge`, `scripts/make_installer.sh`). Built and
  inspected; the running path of the app (switch on → DD 5.1 to the TV) is **not yet tested in the
  app** (the TV was off), only via `sbridge`. The installer hasn't been run (needs admin: the user).
- Phase 1 prototype works live with `sbridge`: BlackHole 2ch → Dolby Digital 5.1 (upmixed) → the
  Samsung over HDMI (`cac3`). 2-minute runs: no overruns, buffer held at target, ~0–100 ppm, ~130 ms
  estimated app→HDMI delay. TV released cleanly every run.
- **Microphone permission:** capturing any input needs TCC microphone access (the terminal for
  `sbridge`, the app's own grant for the app: `NSMicrophoneUsageDescription`). Without it
  `AudioDeviceCreateIOProcID` blocks forever; the app checks `AVCaptureDevice.authorizationStatus`
  first and `sbridge` prints a hint after 3 s.
- **Our own virtual device (1.1.0, 2026-11-23):** `driver/` = BlackHole 0.7.1's `BlackHole.c` (GPL-3)
  built under our name by `scripts/build_driver.sh` (clang only, universal, ad-hoc signed; build
  definitions for name/UIDs/bundle ID, 2 ch, 48 kHz only; three branding strings in the source switched
  to the macros, marked "Surround Bridge:"; details in driver/README.md). Device "Surround Bridge",
  UID `SurroundBridge_UID`, bundle `local.surroundbridge.driver` in `/Library/Audio/Plug-Ins/HAL`.
  The .pkg installs it first (`installer/driver-scripts/postinstall`: permissions + kickstart
  coreaudiod), then the app. **Never use BlackHole's name/branding or its official binaries** (their
  LICENSE header: binaries/installers "All rights reserved", branding needs permission); building
  from source under our own name is what the GPL allows. Verified 2026-11-23: coreaudiod loads the
  ad-hoc build, and a 440/660 Hz tone played into it came out of `sbridge run --dump` exact (L/R).
  Fallback when it's missing: `BlackHoleSetup.swift` downloads the official BlackHole installer
  (sha256 pin / Existential Audio Developer ID), "Use BlackHole 2ch instead…" in the popover.
- Next: test the app live with the TV (on our device), lip sync check.

## Windows port (started 2026-11-22)
- **Built:** `scripts/build_win.ps1` (VS 2022 Build Tools: clang-cl 19 + CMake + Ninja, `CMakeLists.txt`)
  → `.build\win-release\sbridge.exe`. FFmpeg: `vendor/ffmpeg/windows-x64`, else `..\music_player`'s
  (same options). Clean build, no warnings.
- **Tests:** `regress.sh` prints ALL PASSED on Windows 11 (MSYS2 UCRT64 ffmpeg/ffprobe/python on PATH:
  `PATH=/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH bash scripts/regress.sh .build/win-release/sbridge.exe`).
- **Capture:** `core/platform/windows/capture_wasapi.c`. Shared mode, the endpoint's float mix
  format, polled every 3 ms on an MMCSS thread. Lists capture endpoints (uid = endpoint id), then
  render endpoints as loopback (`loopback:<id>`, "<name> (loopback)"). Loopback only delivers packets
  while something plays, so it also renders a **silent shared stream into the loopbacked endpoint**
  to keep it running (verified: 4 s of loopback with nothing playing → 3.94 s of continuous AC3).
  `bridge.c` uses `PC_HAVE_CAPTURE` (pc_capture.h) instead of `__APPLE__`.
- `sbridge` on Windows: default input "CABLE Output" (VB-Audio Virtual Cable), `timeBeginPeriod(1)`,
  refuses an input that is the loopback of the output itself (feedback). `run --dump F` writes the
  live stream to a file instead of a device (tests capture without touching the TV).
- **Not yet tested:** VB-Cable (not installed on this PC: the only endpoint is the Samsung over
  NVIDIA HDMI, which offers AC3 bitstream; over Remote Desktop only "Remote Audio" exists), the live
  capture → exclusive HDMI path, drift control against a real cable clock, hot-plug, the app's
  default-output switching (IPolicyConfig) on real devices, the VB-Cable download/signature check
  (the signer name match "VB-Audio"/"Burel" is a guess: check it on the first real install).

## App (Windows 1.0.0, 2026-11-22)
- `Windows/SurroundBridge/` (C#, .NET 8, **WPF** + WinForms NotifyIcon; WPF because it starts fast
  as a single file and its runtime packs were cached; Surround Player's WinUI had start-up hangs).
  P/Invoke into `bridgecore.dll` (CMake target `bridgecore`, PC_BUILD_DLL); `Native.cs` mirrors the
  C structs. `core/include/bridge_win.h` + `platform/windows/default_output.c`: get/set the default
  render endpoint (IPolicyConfig::SetDefaultEndpoint; role 0 console+multimedia, 1 communications).
- `BridgeModel.cs` is a port of BridgeModel.swift (same states, restart rules, output switching with
  `restoreOutputUID`/`restoreCommsOutputUID`); C calls on one `SerialQueue` thread. Device changes via
  `pc_set_device_change_callback` (debounced 600 ms); wake via SystemEvents.PowerModeChanged.
  Virtual device = input named "CABLE Output"; apps play to the output "CABLE Input".
- UI: `FlyoutWindow` (tray popover), `SettingsWindow` (General/Upmix/About), `UpmixSliders`
  (compact/full), `Theme.xaml` + `Theme.cs` (own control templates, light/dark from AppsUseLightTheme,
  accent = the icon's indigo). `CableSetup.cs`: downloads VBCABLE_Driver_Pack45.zip from vb-audio.com,
  WinVerifyTrust + signer check, runs `VBCABLE_Setup_x64.exe` elevated.
- **The installer is the app itself** (`Installer.cs`, `SetupWindow`): the release exe, run from
  anywhere but `%LOCALAPPDATA%\Programs\Surround Bridge\`, shows Install/Update/Reinstall; copies
  itself there, Start menu (+ desktop) shortcut, HKCU Uninstall entry (`--uninstall`), HKCU Run
  (`--background`). Single instance (mutex `Local\SurroundBridge`; events `.Show`, `.Quit`: updates
  and uninstall ask the running copy to quit so it restores the output). Dev builds (bridgecore.dll
  beside the exe) always run as the app. Crash log: `%TEMP%\SurroundBridge\crash.log`.
- Settings: `%APPDATA%\Surround Bridge\settings.json`. **Tests: always set
  `SURROUND_BRIDGE_SETTINGS=<scratch file>` and `SURROUND_BRIDGE_TEST_ROOT=<scratch dir>`** (install
  dir, shortcuts and registry under `HKCU\Software\SurroundBridgeTest`), never the real ones.
- `--snapshot <dir> [--theme light|dark]` renders every window to PNG (never switches on): the way
  to check the UI without clicking. Verified 2026-11-22: all windows in both themes; packaged-exe
  snapshot; quiet install/reinstall/uninstall in a test root; single instance + `.Quit`.
- Release: `scripts\build_win.ps1 -Package` → `build\SurroundBridge-Setup-<ver>-windows-x64.exe`
  (~69 MB). Version = `<Version>` in SurroundBridge.csproj (bump per release). Committed to the
  `releases` branch under `Windows/` (like Surround Player). Icon: `scripts/make_ico.ps1`
  (Resources/AppIcon-1024.png, cropped to the tile) → `Resources/AppIcon.ico`.

## App (macOS, 1.0.0)
- `BridgeApp.swift`: `MenuBarExtra(.window)` + `Settings` scene, `LSUIElement`; AppDelegate handles
  SIGINT/SIGTERM and quit by calling `BridgeModel.shutdown()` (restores the Mac's output, releases the TV).
  `Uninstaller` (About tab) removes the app and SurroundBridge.driver (and optionally BlackHole, only
  offered when it's installed) with an admin prompt, then restarts coreaudiod.
- `BridgeModel.swift`: the only caller of the C API, always on a serial queue (start can take ~1–2 s
  while the TV switches format). States: off / starting / running / needsPermission /
  noVirtualDevice / noOutput / failed. While on it sets the default output **and** the alerts
  output to the virtual device (`AudioSystem.swift`; `SurroundBridge_UID`, else a device named BlackHole), saving the previous ones under `restoreOutputUID` /
  `restoreSystemOutputUID` (also restored at launch after a crash). Restarts when the chosen output
  re-appears (matched by name: the TV's UID changes), on wake, and when the pipeline stops by itself.
- `Upmix.swift` is Surround Player's (same keys and presets, SHARED.md); the bridge defaults to Music.
- Settings (UserDefaults `local.surroundbridge`): `enabled`, `outputUID`, `outputName`, `codec`
  (0 AC3, 1 DTS), `ac3Bitrate`, `dtsBitrate`, `autoSwitchOutput`, the `upmix*` keys, the restore keys.
  Launch at login via `SMAppService.mainApp`.
- Debug: `SURROUND_BRIDGE_DEBUG=1` (state changes + 1 s status to stderr), `SURROUND_BRIDGE_AUTOSTART=1`,
  `SURROUND_BRIDGE_DEFAULTS=<suite>` (**always use a test suite in tests**, never the real domain).
- Icon: `scripts/make_icon.swift` (drawn in code) → `scripts/make_icns.sh` → `Resources/AppIcon.icns`.
- Installer: `scripts/make_installer.sh` (pkgbuild, not relocatable, → productbuild with
  `installer/distribution.xml` + `installer/resources/*.html`; `installer/scripts/postinstall` opens
  the app for the console user). Unsigned: right-click → Open the first time.

## The user's setup
- Mac → **HDMI → Samsung TV → optical → AC3 amp**. Optical can't carry multichannel PCM, hence AC3/DTS.
  Over HDMI the Samsung offers `cac3`; the TV's CoreAudio UID can change on input/mode changes (match by name).
- Whether the TV forwards DTS over optical is unverified (on Windows it refused DTS over HDMI).

## Layout
```
core/bridge.c, include/bridge.h   PORTABLE live pipeline + public API (sb_*); include/pc_types.h shared types
Sources/SurroundBridge/    the menu-bar app (SwiftUI; see "App" above)
driver/                    the Mac virtual device: BlackHole 0.7.1 source (GPL-3) + our Info.plist; scripts/build_driver.sh
Package.swift              SwiftPM: BridgeCore (C, core/), SurroundBridge (app), sbridge (CLI)
core/pc_encode.c/.h        PORTABLE AC3/DTS encoder + IEC 61937 packer (planar AC3, packed s32 DTS)
core/pc_capture.h          capture back-end interface (pccap_*)
core/platform/apple/       capture_coreaudio.c (new), audio_coreaudio.c (shared, output)
core/pc_output.*, pc_upmix.*, pc_audio_device.h, pc_platform.h, pc_types.h   shared (SHARED.md)
tools/sbridge.c            CLI: devices / run / file
scripts/build.sh           clang build -> build/<arch>/sbridge (FFmpeg: $FFMPEG_PREFIX, vendor/, or ../music_player/vendor/)
scripts/regress.sh         file-mode behavioural tests (must print ALL PASSED)
core/platform/windows/     capture_wasapi.c (new), audio_wasapi.c (shared, output)
CMakeLists.txt, scripts/build_win.ps1, win_env.ps1   Windows build (clang-cl + Ninja) -> .build/win-<config>/sbridge.exe, bridgecore.dll
Windows/SurroundBridge/    the Windows tray app (WPF; see "App (Windows)" above)
scripts/build_ffmpeg*.sh   same as Surround Player's (static 9.0.2, avfilter with only the needed filters)
```

## Pipeline decisions
- **Input ring:** SPSC, interleaved float, 1 s. The capture callback drops (counts `overruns`) when full; never blocks.
- **Worker:** 1024-frame chunks → swr (input channels/rate → 48 kHz planar; stereo when upmixing) →
  upmix graph (pc_upmix.c, output 5.1(side) for both codecs, since **FFmpeg's DTS encoder has no
  5.1(back)**) → AVAudioFifo → encoder frames → bursts → `pc_out_write` (blocks only if the 0.4 s
  output ring is full).
- **Drift control (`steer`):** the input (virtual device) and output (HDMI) clocks differ, so the
  output ring fill (EMA) is held at a target by a PI controller on `swr_set_compensation`, updated
  every 24000 samples. P 40 ppm per ms of error, I 0.8 ppm/ms/s held to ±300 ppm (**anti-windup:
  an unbounded integral pinned the correction at the ±1000 ppm limit**), total ±1000 ppm.
  - **Auto target** (`target_buffer_ms` 0): 2 s after the first audio it locks onto the fill reached
    then, min one burst + 8 ms (40 ms with AC3). At start the HDMI device pulls ~35 ms ahead into
    its own buffer, so the ring reads ~27 ms; that audio isn't lost, it's in the device latency.
    A fixed 64 ms ring target only added delay, and filling it at ±1000 ppm took a minute.
    Prefilling silence at start didn't help: the capture device starts later and the output plays
    the prefill away meanwhile.
  - **Jumps:** more than 40 ms over target → drop that much input at once (2 s cooldown). Seen once:
    +57 ms when the app feeding BlackHole stopped. Not exercised since.
  - Off in dump mode (a file sink drains instantly).
- **Keep-alive:** the output plays an encoded silence burst whenever the ring is short (from
  pc_output.c), so the amp stays locked between sounds.
- **Encoder sample formats:** AC3 = fltp (planar), DTS = s32 **packed**. Anything that copies planes
  must use `av_sample_fmt_is_planar` (a planar-only copy crashed the DTS path).
- **Latency estimate** (`SBStatus.latency_ms`): capture IO + half a chunk + upmix 2048 samples + one
  encoder frame + ring fill + device latency. Expect roughly 120–150 ms with AC3 and upmix; needs measuring.
- **Upmix defaults** = Surround Player's "Music" preset.

## Testing — IMPORTANT
- **Never play audible test signals on a real device.** The TV is often the default output. Use silent
  media (`anullsrc`) for anything that reaches a device. Tones are fine in file mode (`sbridge file`)
  and into the virtual device (Surround Bridge / BlackHole) *only when the bridge's output is a dump*
  (`sbridge run --dump F`), since the virtual device itself is inaudible.
- **Never touch the user's real data** (Surround Player's playlist session / `local.surroundplayer`
  defaults, or this app's `local.surroundbridge` settings: test with `SURROUND_BRIDGE_DEFAULTS`). The app
  switches the Mac's default output: record it before a test and check it's restored after. Back up and restore, never delete. A previous test cleanup
  destroyed the user's 5,683-item playlist.
- **zsh:** the Bash tool's shell is zsh, where `"$1:s..."` is a substitution modifier. Use `${1}`,
  cap generated media with `-t`, and run `ffmpeg -nostdin`.
- `scripts/regress.sh` after any core change.
- Device state after a live run (hog released, physical format back) can be checked with a small
  CoreAudio program. Surround Player's session had one: read-only, prints hog/mixing/physical format.
