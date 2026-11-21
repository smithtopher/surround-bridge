# Surround Bridge — working notes

System-wide "Dolby Digital Live" for macOS (Windows later): apps play into a virtual audio device,
the bridge upmixes stereo to 5.1, encodes AC3/DTS and sends IEC 61937 to an HDMI/optical output.
Sister project of Surround Player (`~/code/music_player`, github smithtopher/SurroundPlayer), which
shares the output, upmix and encode code (copied, see SHARED.md). README.md is the user-facing doc,
docs/PLAN.md the roadmap.

## Status (2026-11-21)
- Phase 1 prototype **works live**: BlackHole 2ch → Dolby Digital 5.1 (upmixed) → the Samsung over HDMI
  (`cac3`), silent media fed into BlackHole with Surround Player's `pctest play --device BlackHole2ch_UID --pcm`.
  2-minute runs: no overruns, buffer held at target, correction settles to ~0–100 ppm, estimated
  app→HDMI delay **~130 ms** (excluding the amp's decode). TV released cleanly every run.
- **Microphone permission:** capturing any input (BlackHole included) needs TCC microphone access for
  the process (the terminal app, later our menu-bar app). Without it `AudioDeviceCreateIOProcID`
  blocks forever inside CoreAudio; `sbridge` prints a hint after 3 s. Our own driver could avoid
  TCC by handing audio to the agent over shared memory instead of an input stream (PLAN.md).
- Next: measure real lip sync with the user (Safari/YouTube, QuickTime). Then phase 2, our own
  Audio Server plug-in.

## The user's setup
- Mac → **HDMI → Samsung TV → optical → AC3 amp**. Optical can't carry multichannel PCM, hence AC3/DTS.
  Over HDMI the Samsung offers `cac3`; the TV's CoreAudio UID can change on input/mode changes (match by name).
- Whether the TV forwards DTS over optical is unverified (on Windows it refused DTS over HDMI).

## Layout
```
core/bridge.c/.h           PORTABLE live pipeline + public API (sb_*)
core/pc_encode.c/.h        PORTABLE AC3/DTS encoder + IEC 61937 packer (planar AC3, packed s32 DTS)
core/pc_capture.h          capture back-end interface (pccap_*)
core/platform/apple/       capture_coreaudio.c (new), audio_coreaudio.c (shared, output)
core/pc_output.*, pc_upmix.*, pc_audio_device.h, pc_platform.h, pc_types.h   shared (SHARED.md)
tools/sbridge.c            CLI: devices / run / file
scripts/build.sh           clang build -> build/<arch>/sbridge (FFmpeg: $FFMPEG_PREFIX, vendor/, or ../music_player/vendor/)
scripts/regress.sh         file-mode behavioural tests (must print ALL PASSED)
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
  and into BlackHole *only when the bridge's output is a dump*, since BlackHole itself is inaudible.
- **Never touch the user's real data** (Surround Player's playlist session / `local.surroundplayer`
  defaults, or later this app's settings). Back up and restore, never delete. A previous test cleanup
  destroyed the user's 5,683-item playlist.
- **zsh:** the Bash tool's shell is zsh, where `"$1:s..."` is a substitution modifier. Use `${1}`,
  cap generated media with `-t`, and run `ffmpeg -nostdin`.
- `scripts/regress.sh` after any core change.
- Device state after a live run (hog released, physical format back) can be checked with a small
  CoreAudio program. Surround Player's session had one: read-only, prints hog/mixing/physical format.
