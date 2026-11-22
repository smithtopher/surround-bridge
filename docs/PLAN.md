# Plan

## Phase 1: prototype (macOS, BlackHole) ← now
- [x] Live pipeline (`core/bridge.c`): capture ring → drift-steered resample → upmix → AC3/DTS → IEC 61937 → exclusive output.
- [x] CoreAudio capture back-end (`core/platform/apple/capture_coreaudio.c`).
- [x] `sbridge` CLI; file mode and regression tests (`scripts/regress.sh`).
- [x] **Live test with BlackHole → the Samsung over HDMI** (silent media): drift control settles, no
      overruns over 2 min, ~130 ms estimated delay (2026-11-21). Underruns aren't counted yet (add a
      counter in pc_output's render: keep-alive bursts played while real data was expected).
- [ ] **Measure end-to-end latency and lip sync**: Safari/YouTube, QuickTime, the TV app. This decides
      how much to optimise (smaller FFT window, DTS's shorter frames, smaller buffer target).
- [ ] Protected (DRM) content: does it play into a virtual device at all?

## Phase 2: our own virtual device (macOS)
- Audio Server plug-in ("Surround Bridge"), based on Apple's NullAudio sample (permissive licence;
  BlackHole is GPL, reference only).
- Reports its **latency** (`kAudioDevicePropertyLatency` / safety offset) so AVFoundation-based players
  delay their video to match.
- Hand audio to the agent over **shared memory**, not an input stream: reading an input needs microphone
  permission (TCC), which a "send my sound to the amp" tool shouldn't have to ask for.
- 2, 6 and 8 channel modes. Multichannel input (games, VLC, real 5.1) skips the upmix.
- Fixed 48 kHz; volume and mute handled before encoding (a bitstream can't be scaled afterwards).
- Installer (admin password, restarts coreaudiod), Developer ID signing and notarization.

## Phase 3: menu-bar app (macOS) ← 1.0.0 built (2026-11-21)
- [x] Menu-bar app: on/off, status, levels, upmix presets + sliders, Settings (output, format, login, upmix, about/uninstall).
- [x] Installer pkg (app only; BlackHole downloaded from its official site by the app, it can't be bundled).
- [ ] Live test of the app with the TV; the user runs the installer.
- [ ] Developer ID signing + notarization (needs the $99 Apple Developer account).
- On/off, mode (the upmix presets shared with Surround Player), output device, status (levels,
  latency, "locked" indicator). Launch at login (SMAppService). Sleep/wake and HDMI hot-plug
  (TV input changes). Auto-switches the system output to the bridge device and back.

## Windows ← started 2026-11-22
- [x] CMake + clang-cl build of the core and `sbridge.exe`; `regress.sh` ALL PASSED on Windows 11.
- [x] WASAPI capture back-end (`capture_wasapi.c`): capture endpoints and loopback of render
      endpoints, with a silent render stream keeping loopback continuous.
- [ ] Live test: VB-Cable → Dolby Digital → the Samsung over HDMI (silent media); drift, overruns.
- [ ] Tray app; switch the default output to the cable while on, restore it after.
- Same core. Capture: **WASAPI loopback** of a virtual endpoint. That endpoint needs a virtual audio
  driver, which on Windows is **kernel-mode** (Microsoft's SysVAD sample) and needs an EV certificate
  plus Microsoft attestation signing. Interim: use VB-Audio Cable as the virtual device.
- Output: `audio_wasapi.c` (already in core/, from Surround Player). Tray app instead of a menu-bar app.
- Build: CMake + clang-cl like Surround Player (to be added here).
