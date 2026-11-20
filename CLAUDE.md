# Surround Bridge — working notes

System-wide "Dolby Digital Live" for macOS (Windows later): apps play into a virtual audio device,
the bridge upmixes stereo to 5.1, encodes AC3/DTS and sends IEC 61937 to an HDMI/optical output.
Sister project of Surround Player (`~/code/music_player`, github smithtopher/SurroundPlayer), which
shares the output, upmix and encode code (copied, see SHARED.md). README.md is the user-facing doc,
docs/PLAN.md the roadmap.

## Status (2026-11-20)
- Phase 1 prototype: core pipeline, CoreAudio capture, `sbridge` CLI, file-mode regression tests (all pass).
- **Not yet tested live:** BlackHole wasn't installed and the TV wasn't connected when this was written.
  Next: `brew install blackhole-2ch` (the user runs it; needs admin), then
  `sbridge run --input BlackHole --output SAMSUNG` with **silent media** played into BlackHole, and
  watch the drift controller, overruns, latency.

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
- **Drift control (`steer`):** the input (virtual device) and output (HDMI) clocks differ. The output
  ring fill (EMA) is held at `target_buffer_ms` (default 64) with a PI controller on
  `swr_set_compensation`, updated every 24000 samples, clamped ±1000 ppm. Off in dump mode (a file
  sink drains instantly). **Untested on a real device yet.** Tune Kp/Ki once it runs live.
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
