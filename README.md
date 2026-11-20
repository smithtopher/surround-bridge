# Surround Bridge

Every app's sound, sent to your amp as real surround. The bridge takes what apps play into a
virtual audio device, upmixes stereo to 5.1, encodes it to **Dolby Digital (AC3)** or **DTS** and
sends it as an IEC 61937 bitstream over HDMI or optical to a receiver or soundbar. That's the
equivalent of the "Dolby Digital Live" sound cards used to offer, for macOS and, later, Windows.

It's built for setups where the amp only takes compressed surround (e.g. Mac → HDMI → TV → optical
→ AC3 amp), where multichannel PCM isn't an option.

Status: **prototype**. A command-line tool (`sbridge`) on macOS, using
[BlackHole](https://github.com/ExistentialAudio/BlackHole) as the virtual device. Our own virtual
device and a menu-bar app come next ([docs/PLAN.md](docs/PLAN.md)).

## Try it (macOS)

```sh
brew install blackhole-2ch          # the virtual device (asks for an admin password)
scripts/build.sh                    # builds build/<arch>/sbridge (needs FFmpeg, see below)
build/arm64/sbridge devices         # inputs (BlackHole) and outputs (your HDMI/optical device)
build/arm64/sbridge run --input BlackHole --output "SAMSUNG"
```

Then pick **BlackHole 2ch** as the output in System Settings ▸ Sound. Everything that plays now
reaches the amp as Dolby Digital 5.1. `sbridge run` prints the buffer, the clock-drift correction,
the estimated latency and input levels every second. Ctrl-C stops it and gives the device back.

Options: `--dts`, `--bitrate N`, `--no-upmix`, `--upmix-set surround=-3,center=0.5,width=0.6,lfe=-6,xover=100,move=0,bass=0,delay=10,highcut=7000`,
`--buffer-ms N` (output buffer target, default 64).

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
- `scripts/regress.sh`: behavioural tests in file mode (nothing plays on any device).

## Licence

Not decided yet. FFmpeg is used under the LGPL. "Dolby Digital" and "DTS" are trademarks of their
owners. This project isn't affiliated with or licensed by them.
