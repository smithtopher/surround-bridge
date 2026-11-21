# Changelog

## [1.0.0] — 2026-11-21

First release: everything your Mac plays, sent to your amp as Dolby Digital or DTS surround.

- **Menu-bar app:**
  - Switch it on and off, see what it's doing (format, output, delay) and the input levels.
  - Pick the upmix mode and adjust the main levels.
- **Settings:**
  - output device, Dolby Digital or DTS and the bitrate, start at login;
  - Upmix (the same presets and sliders as Surround Player, Advanced included);
  - About and Uninstall.
- **While it's on**, the Mac's sound output points at the bridge, and the previous output comes back
  when it's off.
- **It restarts by itself** when the TV re-appears (input change, wake).
- **First-run help:** installs BlackHole 2ch from its official installer (downloaded and
  signature-checked), restarts macOS audio, and asks for access to the audio device.
- **Installer** (`.pkg`), and an original app icon.
- **Under the hood:**
  - live pipeline: capture → drift-steered resample → stereo upmix → AC3/DTS → IEC 61937 →
    exclusive HDMI/optical output with keep-alive silence;
  - `sbridge` command-line tool and file-mode regression tests.
