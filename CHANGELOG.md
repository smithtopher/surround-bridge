# Changelog

## [1.1.0] — 2026-11-23 (macOS)

- **Its own audio device:** the installer now adds the **Surround Bridge** audio device (2 channels,
  48 kHz) that your apps play into, so there's nothing else to download or install. It's built from
  the source code of BlackHole 0.7.1 (GPL-3; not an official BlackHole build) and installs alongside
  an existing BlackHole.
- If the device is missing, the menu offers to restart macOS audio, or to use BlackHole 2ch instead.
- Uninstall also removes the device.

## [Windows 1.0.0] — 2026-11-22

First Windows release: everything your PC plays, sent to your amp as Dolby Digital or DTS surround.

- **Tray app** (notification area), the counterpart of the Mac menu-bar app:
  - switch it on and off, see what it's doing (format, output, delay) and the input levels;
  - pick the upmix mode and adjust the main levels.
- **Settings:** output device, Dolby Digital or DTS and the bitrate, the virtual cable, start at sign-in;
  Upmix (the same presets and sliders as on the Mac, Advanced included); About and Uninstall.
  Light and dark, following Windows.
- **While it's on**, Windows' sound output (and the communications output) points at the virtual
  cable; the previous outputs come back when it's off, or at the next start after a crash.
- **It restarts by itself** when the TV re-appears (input change, wake from sleep).
- **First-run help:** installs VB-Audio Virtual Cable from VB-Audio's official package (downloaded,
  signature-checked, its own installer).
- **One-file installer:** per user, no administrator rights; Start menu (and optional desktop)
  shortcut, start at sign-in, an entry in Settings ▸ Apps to uninstall. `--install --quiet` and
  `--uninstall --quiet` for unattended use.
- **Under the hood:** the same C core as the Mac app (`bridgecore.dll`), with WASAPI capture
  (`capture_wasapi.c`) and the exclusive IEC 61937 output shared with Surround Player.

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
