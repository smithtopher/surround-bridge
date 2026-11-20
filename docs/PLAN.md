# Plan

## Phase 1: prototype (macOS, BlackHole) ← now
- [x] Live pipeline (`core/bridge.c`): capture ring → drift-steered resample → upmix → AC3/DTS → IEC 61937 → exclusive output.
- [x] CoreAudio capture back-end (`core/platform/apple/capture_coreaudio.c`).
- [x] `sbridge` CLI; file mode and regression tests (`scripts/regress.sh`).
- [ ] **Live test with BlackHole → the Samsung over HDMI** (silent media only). Check the drift controller
      settles (buffer at target, ppm steady) and no overruns or underruns over a long run.
- [ ] **Measure end-to-end latency and lip sync**: Safari/YouTube, QuickTime, the TV app. This decides
      how much to optimise (smaller FFT window, DTS's shorter frames, smaller buffer target).
- [ ] Protected (DRM) content: does it play into a virtual device at all?

## Phase 2: our own virtual device (macOS)
- Audio Server plug-in ("Surround Bridge"), based on Apple's NullAudio sample (permissive licence;
  BlackHole is GPL, reference only).
- Reports its **latency** (`kAudioDevicePropertyLatency` / safety offset) so AVFoundation-based players
  delay their video to match.
- 2, 6 and 8 channel modes. Multichannel input (games, VLC, real 5.1) skips the upmix.
- Fixed 48 kHz; volume and mute handled before encoding (a bitstream can't be scaled afterwards).
- Installer (admin password, restarts coreaudiod), Developer ID signing and notarization.

## Phase 3: menu-bar app (macOS)
- On/off, mode (the upmix presets shared with Surround Player), output device, status (levels,
  latency, "locked" indicator). Launch at login (SMAppService). Sleep/wake and HDMI hot-plug
  (TV input changes). Auto-switches the system output to the bridge device and back.

## Windows
- Same core. Capture: **WASAPI loopback** of a virtual endpoint. That endpoint needs a virtual audio
  driver, which on Windows is **kernel-mode** (Microsoft's SysVAD sample) and needs an EV certificate
  plus Microsoft attestation signing. Interim: use VB-Audio Cable as the virtual device.
- Output: `audio_wasapi.c` (already in core/, from Surround Player). Tray app instead of a menu-bar app.
- Build: CMake + clang-cl like Surround Player (to be added here).
