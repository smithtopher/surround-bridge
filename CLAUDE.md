# Surround Bridge — working notes

System-wide "Dolby Digital Live" for macOS and Windows: apps play into a virtual audio device,
the bridge upmixes stereo to 5.1, encodes AC3/DTS and sends IEC 61937 to an HDMI/optical output.
Sister project of Surround Player (`~/code/music_player`, github smithtopher/SurroundPlayer), which
shares the output, upmix and encode code (copied, see SHARED.md). README.md is the user-facing doc,
docs/PLAN.md the roadmap.

## Status (2026-11-23: Mac 1.2.0 adds multichannel PCM and treble boost, see "Pipeline decisions")
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
- **Timer resolution (fixed in 1.0.1):** Sleep/waits default to 15.6 ms and, since Windows 10 2004,
  `timeBeginPeriod` is per process. The app never set it, so the capture poll and the worker's 1 ms
  sleep lagged and the output underran ~2/s (stutter); `sbridge` hid it by setting 1 ms itself. Now
  `capture_wasapi.c` holds timeBeginPeriod(1) while running and uses a 100 ms capture buffer.
  `SBStatus.gaps` / sbridge "gaps" = silence bursts played after real data started (1 at start-up is
  normal). Live check 2026-11-23, VB-Cable → Samsung, silence played into CABLE Input: 51 gaps in
  30 s before, 1 (start-up) in 40 s after.
- **Latency (1.0.2):** the auto target settled at ~72 ms here (start-up backlog from VB-Cable).
  `steer` now caps it at burst + 24 ms (56 ms AC3) and cuts the excess once at the lock
  (`skip_in`: input frames discarded as they arrive; the jump cut uses it too, since the input ring
  often holds less than the cut). Each output gap after the lock raises the target 8 ms. Live
  floor measured with fixed `--buffer-ms`: 40 → couldn't go below ~51 (burst quantisation), 50 and
  60 clean. VB-Cable's own delay, measured (click rendered into CABLE Input vs its render clock,
  captured from CABLE Output): ~52 ms, invisible to apps and not in `latency_ms`. Remaining budget:
  upmix window 43 ms (WIN_SIZE 4096, shared pc_upmix.c), AC3 frame 32 ms, exclusive device ~20 ms.
- **Tested live 2026-11-23:** VB-Cable installed; CABLE Output → Dolby Digital → Samsung over HDMI
  (exclusive), drift control holding ~74 ms, IPolicyConfig default-output switching (test script).
  The user ran 1.0.0 in the app and heard the stutter above. **Not yet tested:** hot-plug, music
  through 1.0.1 by ear, the VB-Cable download/signature check (signer match "VB-Audio"/"Burel" is a
  guess).

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
- **Multichannel PCM (`SB_CODEC_PCM`, Mac 1.2.0, 2026-11-23):**
  - No encoder (`b->enc` NULL, `b->pcm`). Output: `pc_out_open(PC_STREAM_PCM_EXCLUSIVE)`, the
    shared CoreAudio/WASAPI path from Surround Player, asking for 5.1(side) 48 kHz when upmixed,
    else the input's layout (6 → 5.1, 8 → 7.1, else stereo). The audio is laid out the way the
    device opened (`pc_out_channels/channel_mask` → `b->enc_layout`, packed float `b->fmt`; the
    upmix graph's aformat targets it), and the device must run at 48 kHz.
  - The fifo goes out as it fills (`send_pcm`). Steer granularity is one worker chunk (21 ms)
    instead of a burst.
  - **PCM needs a cushion:** it plays the moment it arrives (no burst gate), so without one the
    ring sat at ~10 ms with constant underruns and the drift control pinned. `pcm_pad` writes
    chunk + 8 ms of silence before the start, and refills to the target after each underrun.
    `pc_output.c` now counts PCM underruns in `pc_out_gaps` too, one per gap. Triggering on low
    fill instead fired on normal IO jitter and added latency.
  - Live 2026-11-23 (silent media): Surround Bridge device → the Samsung over HDMI accepted 6 ch
    24-bit 48 kHz (order L R LFE C Ls Rs), ~91 ms estimated delay vs ~140 ms AC3. Stable fill, a
    gap or two at start only. Built-in speakers: stereo works; 6 ch is refused with a clear message.
  - Windows: `Native.cs` has the constant; the tray app's UI doesn't offer it yet.
- **Tone (`PCUpmix.treble_db`, Mac 1.2.0):** a treble high shelf (6 kHz) next to the bass shelf
  (100 Hz), both before the surround filter, so they only work with the upmix on.
  - Our FFmpeg has no `treble` filter, so it's a second `bass` (low shelf) of −T dB plus +T dB of
    gain via surround's `fl_in`/`fr_in`. That's exactly a high shelf of +T in FFmpeg's SVF biquads
    (same poles, mix coefficients × A²). Surround's `level_in` does NOT work for this: the
    per-channel `fl_in`/`fr_in` override it.
  - regress "tone": treble +6 → +6.4 dB at 12 kHz with 60 Hz/1 kHz unchanged; bass the mirror
    image. Q 0.7 shelves overshoot ~0.3 dB.
  - The app keeps tone (bass + treble) out of the presets: a preset no longer sets bass (Wide used
    to set +3), so a loudness curve survives mode changes. Key `upmixTrebleDb`.
  - `Windows/SurroundBridge/Native.cs` PCUpmix has the field (it must match the C struct), but
    there's no UI there yet.

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
