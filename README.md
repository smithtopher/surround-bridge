# Surround Bridge

Every app's sound, sent to your amp as real surround. The bridge takes what apps play into a
virtual audio device, upmixes stereo to 5.1, encodes it to **Dolby Digital (AC3)** or **DTS** and
sends it as an IEC 61937 bitstream over HDMI or optical to a receiver or soundbar. That's the
equivalent of the "Dolby Digital Live" sound cards used to offer, for macOS and Windows.

It's built for setups where the amp only takes compressed surround (e.g. computer → HDMI → TV →
optical → AC3 amp), where multichannel PCM isn't an option.

Downloads are on the [`releases` branch](https://github.com/smithtopher/surround-bridge/tree/releases)
(`Windows/`, and `macOS/` once the Mac build is added there).

## Install (macOS 14+)

1. Open **SurroundBridge-1.1.0.pkg**. It isn't signed by a registered developer yet, so the first
   time: right-click it in Finder → **Open** → **Open**.
2. The installer puts **Surround Bridge** in Applications, adds the **Surround Bridge audio device**
   (the virtual output your apps play into; macOS audio restarts once for it) and starts the app. It
   lives in the menu bar (speaker icon, top right).
3. Click the icon and switch it on. The first time, **allow access** to the audio device (macOS
   treats listening to an audio device like a microphone).
4. Pick your TV, receiver or soundbar under Settings ▸ General if it isn't already chosen.

While it's on, the Mac's sound output points at the bridge, and everything plays through your amp
as Dolby Digital (or DTS) 5.1. Switch it off and your previous output comes back.

**Settings:**
- output device, start at login;
- format: Dolby Digital or DTS (and the bitrate), or **Multichannel PCM**. PCM is uncompressed 5.1
  for an AV receiver connected to the Mac by HDMI (Mac 1.2.0). It has the best quality and less
  delay, but optical can't carry it.

**Upmix:** the same presets and sliders as Surround Player (Music, Movies & TV, Wide, Custom; surround
level, centre focus, width, subwoofer; Advanced: crossover, bass routing, rear delay, rear high-cut).

**Tone:** bass boost and treble boost (Mac 1.2.0). Turn both up for listening at low volume, where
the ear loses bass and treble first (the Fletcher-Munson curves). They're kept when you change the
upmix mode.

Everything applies while it plays.

**Uninstall:** Settings ▸ About ▸ Uninstall (removes the app and its audio device).

## Install (Windows 10/11, 64-bit)

1. Open **SurroundBridge-Setup-1.1.0-windows-x64.exe**. It isn't code-signed yet, so Windows
   SmartScreen may warn the first time: **More info** → **Run anyway**.
2. Click **Install**. It installs for your account only (no administrator password), adds a Start menu
   shortcut, and starts. Surround Bridge lives in the **notification area** of the taskbar (the
   speaker icon; under **^** if Windows hides it: drag it onto the taskbar to keep it visible).
3. Click the icon and switch it on. The first time it offers to install **VB-Audio Virtual Cable**,
   the free virtual audio device your apps play into: it downloads VB-Audio's official installer,
   checks its signature and opens it; click **Install Driver** there (that part asks for
   administrator permission). Occasionally Windows needs a restart before the cable appears.
4. Pick your TV, receiver or soundbar under Settings ▸ General if it isn't already chosen.

While it's on, Windows' sound output points at the cable ("CABLE Input"), and everything plays
through your amp as Dolby Digital (or DTS) 5.1. Switch it off and your previous output comes back.
The settings, upmix and tone controls are the same as on the Mac, including **Multichannel PCM** for an
AV receiver connected by HDMI. If nothing reaches the amp, check that the HDMI/optical device allows
exclusive mode (Sound settings ▸ the device ▸ Advanced).

**Uninstall:** Settings ▸ About ▸ Uninstall, or Windows Settings ▸ Apps ▸ Installed apps ▸ Surround
Bridge. VB-Audio Virtual Cable stays (remove it with its own setup's "Remove Driver").

## Command line

`sbridge` (`scripts/build.sh`) runs the same pipeline from a terminal:

```sh
build/arm64/sbridge devices
build/arm64/sbridge run --output "SAMSUNG"      # input: the Surround Bridge device, else BlackHole
```

Options: `--dts` or `--pcm` (multichannel PCM), `--bitrate N`, `--no-upmix`, `--upmix-set surround=-3,center=0.5,width=0.6,lfe=-6,xover=100,move=0,bass=0,treble=0,delay=10,highcut=7000`,
`--buffer-ms N`.

## How it works

```
apps -> virtual device -> capture (48 kHz float) -> input ring -> worker thread:
   resample (steered: holds the output buffer against clock drift)
   -> stereo upmix (FFmpeg "surround" filter, the same settings as Surround Player)
   -> AC3 / DTS encode -> IEC 61937 bursts -> exclusive HDMI/optical output
      (encoded silence between sounds, so the amp never drops out or clips the start of a sound)
```

The output, upmix and encode code is shared with [Surround Player](https://github.com/smithtopher/SurroundPlayer),
copied for now (see [SHARED.md](SHARED.md)).

## Building

- macOS 14+, Command Line Tools (clang).
- FFmpeg 9.0.2 built with `scripts/build_ffmpeg.sh` (static, LGPL, only the pieces needed; nasm for
  x86_64). If a Surround Player checkout sits next to this one (`../music_player`), `build.sh` uses
  its vendored FFmpeg, which is built with the same options.
- `scripts/build_app.sh`: Apple Silicon, Intel and universal `Surround Bridge.app` (+ zips).
- `scripts/make_installer.sh`: `build/SurroundBridge-<version>.pkg` (unsigned).
- `scripts/regress.sh`: behavioural tests in file mode (nothing plays on any device).

Windows (the same repository and `main` branch; the C core is shared):

- Visual Studio 2022 Build Tools with the C++ workload and its clang-cl; the .NET 8 SDK for the app.
- FFmpeg: `scripts/build_ffmpeg_win.ps1` (MSYS2 needed), or a Surround Player checkout next to this one.
- `scripts/build_win.ps1` → `.build/win-release/sbridge.exe` and `bridgecore.dll`;
  `-App` also builds the tray app (`Windows/SurroundBridge`, WPF); `-Package` makes the release,
  `build/SurroundBridge-Setup-<version>-windows-x64.exe` (one self-contained exe that installs itself).
- `sbridge.exe run --output "SAMSUNG"` captures "CABLE Output" by default; any output's loopback
  works as the input too (`sbridge devices` lists them). `regress.sh` runs on Windows too (MSYS2 bash
  with ffmpeg, ffprobe and python3).

## Licence

Not decided yet for the app. FFmpeg is used under the LGPL. The Mac's virtual audio device
(`driver/`) is built from the source code of [BlackHole](https://github.com/ExistentialAudio/BlackHole)
0.7.1 by Existential Audio Inc. and is under the **GPL-3** like its source; see
[driver/README.md](driver/README.md) for what was changed. It's not an official BlackHole build and
doesn't use BlackHole's name or branding. The app is a separate program that uses the device through
CoreAudio. VB-Audio Virtual Cable is donationware whose licence
doesn't allow bundling, so the Windows app likewise downloads VB-Audio's official package. "Dolby
Digital" and "DTS" are trademarks of their owners; this project isn't affiliated with or licensed by
them, or by Existential Audio or VB-Audio.
