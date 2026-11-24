# Changelog

## [Windows 1.3.3] — 2026-11-24

- **Multichannel PCM too: stereo goes out as 2-channel PCM** with the upmix off and the cable at
  5.1 or 7.1, so the amp's own upmix can be used. PCM has no flag in the stream (the amp reads
  the HDMI format), so the bridge reopens the HDMI output at 2 channels after 3 seconds of
  stereo, and at 5.1 or 7.1 as soon as multichannel plays. Each change is a short silence while
  the TV and amp switch over.

## [Windows 1.3.2] — 2026-11-24

- **With the upmix off, stereo goes out as a 2.0 stream** (Dolby Digital and DTS, cable at 5.1 or
  7.1). It used to go out as 5.1 with four silent channels, so the amp's own upmix couldn't be
  turned on. Now the amp sees 2.0 while stereo plays (after 3 seconds of sound in the front pair
  only) and 5.1 as soon as 5.1 plays. Most amps mute for a moment when the stream changes.
  Multichannel PCM stays 5.1 (the HDMI format can't change without reopening the output).
- The status line shows what actually goes out (stereo, 5.1 or 7.1).

## [Windows 1.3.1] — 2026-11-24

- **Stereo or 5.1 is decided from the sound, not the cable's setting.** 1.3.0 decided by the
  channel count, but Windows mixes every app into the cable's format before the bridge sees it,
  so you had to switch Windows between stereo and 5.1 by hand. Now leave CABLE Input at 5.1 or 7.1:
  when anything plays outside the front left/right pair, it passes through as it is (at once);
  when only the front pair plays for 3 seconds, it's upmixed. The change crossfades over 43 ms.
  Pauses and silence don't change it. Tone applies either way.

## [Windows 1.3.0] — 2026-11-24

- **Upmix or pass-through by the cable's channel count.** With CABLE Input set to 1 or 2
  channels, stereo is upmixed to 5.1 as before (or stays stereo with the upmix off). Set it to
  more than 2 (Sound settings ▸ CABLE Input ▸ Configure speakers, e.g. 5.1 or 7.1) and what apps
  play passes through to the speakers they were meant for, with bass and treble boost still
  applied: 5.1 for Dolby Digital and DTS, 5.1 or 7.1 for multichannel PCM. 3-, 4- and 5-channel
  input used to be mixed down to stereo; it now passes through too.
- The bridge reads the cable's speaker layout from Windows instead of guessing it from the
  channel count, so quad's rear pair, for example, lands on the surround speakers.

## [Windows 1.2.0] — 2026-11-23 (the Mac code is updated but not yet built)

- **Bass and treble boost work on everything now**, not only while the upmix is on: plain stereo
  with the upmix off and 5.1 from apps that output it get the same shelves, before the sound goes
  to Dolby Digital, DTS or multichannel PCM. Turning tone on or off while it plays fades in and
  out over about a quarter of a second, with no click. With both at Off the sound is unchanged
  bit for bit.

## [Windows 1.1.0] — 2026-11-23

The Mac 1.2.0 features, on Windows:

- **Multichannel PCM** (Settings ▸ General ▸ Format): uncompressed 5.1 for an AV receiver connected
  to the PC by HDMI: the best quality and less delay (~107 ms estimated instead of ~153 ms with
  Dolby Digital), but optical can't carry it. Tested live: the Samsung over HDMI accepted 6-channel
  24-bit 48 kHz.
- **Treble boost**, next to the bass boost, in a new **Tone** section in Settings ▸ Upmix and in the
  tray menu. Turning both up gives a loudness curve for listening at low volume.
- Tone is no longer part of the upmix presets, so it stays when you change the mode, and moving
  it no longer turns the mode into Custom (Wide used to set the bass boost to +3 dB).
- Fixed: Settings ▸ General ▸ Send to showed the device's internal description instead of its name.

## [Windows 1.0.2] — 2026-11-23

- **Less delay (lip sync):** the output buffer settled at ~72 ms on Windows, because the virtual cable
  hands over a backlog at start-up. It's now capped at one frame + 24 ms (56 ms with Dolby Digital)
  and the excess is cut once, 2 s after switching on: about 16 ms less delay. If the output ever runs
  short after that, the buffer grows 8 ms at a time, so this can't bring the stutter back.
  Measured with the TV: 60 s, no dropouts.
- For reference, VB-Audio Virtual Cable itself adds ~52 ms that apps can't see (measured).

## [Windows 1.0.1] — 2026-11-23

- **Fixed: stuttery playback.** Windows' default timer is too coarse (15.6 ms) for reading the
  virtual cable every few milliseconds, so audio arrived late and the output filled the holes with
  silence about twice a second. The capture now sets a 1 ms timer while it runs and keeps a 100 ms
  capture buffer. Measured with the TV: from ~50 dropouts in 30 s to none after start-up.
- `sbridge` shows `gaps`: output underruns (a silence burst played in place of audio).

## [1.2.0] — 2026-11-23 (macOS)

- **Multichannel PCM** (Settings ▸ General ▸ Format): uncompressed 5.1 for an AV receiver connected
  to the Mac by HDMI, like Surround Player's option. It has the best quality and less delay
  (~90 ms instead of ~140 ms), but optical can't carry it. Upmixed stereo goes out as 5.1, in the
  speaker order the receiver reports.
- **Treble boost**, next to the bass boost, in a new **Tone** section (and in the menu). Turning
  both up gives a loudness curve for listening at low volume.
- Tone is no longer part of the upmix presets, so your settings stay when you change the mode (Wide
  used to set the bass boost to +3 dB).
- Under the hood: the shared Windows changes up to Windows 1.0.2 (auto buffer target cap, the
  output-gap counter).

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
