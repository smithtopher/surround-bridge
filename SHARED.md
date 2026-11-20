# Code shared with Surround Player

These files were **copied** from Surround Player (`smithtopher/SurroundPlayer`, local checkout
`~/code/music_player`) at commit **953e76d** (2026-11-20), `Sources/PlayerCore/` → `core/`.
When either copy gets a fix, carry it to the other. If both keep evolving, extract them into a shared
library repo (git submodule) instead.

| File here | From (Sources/PlayerCore/…) | Changes |
|---|---|---|
| `core/pc_platform.h` | `pc_platform.h` | none |
| `core/pc_audio_device.h` | `pc_audio_device.h` | `#include "pc_types.h"` instead of `playercore.h` |
| `core/pc_output.c/.h` | `pc_output.c/.h` | same include change |
| `core/pc_upmix.c/.h` | `pc_upmix.c/.h` | same include change |
| `core/platform/apple/audio_coreaudio.c` | same path | none |
| `core/platform/windows/audio_wasapi.c` | same path | none (not built here yet) |
| `core/platform/stub/audio_stub.c` | same path | none |
| `core/pc_types.h` | `include/playercore.h` | only PC_API, PCDevice, pc_list_output_devices, PCDeviceChangeCallback, PCUpmix |
| `core/pc_encode.c/.h` | new: extracted from `engine.c` (open_bs_encoder, spdif_*, make_silence_burst) | standalone object; also handles packed sample formats (DTS) |
| `scripts/build_ffmpeg.sh`, `build_ffmpeg_win.sh`, `patches/ffmpeg/` | same paths | none |

## Fixes made here first (to carry over)

- **2026-11-20, DTS + upmix:** FFmpeg's DTS encoder has no 5.1 with back surrounds, so the upmix
  must be encoded as 5.1(side); the graph's aformat stage maps back → side with levels intact.
  Fixed in Surround Player too (engine.c `setup_audio`, after its 1.3.0 release, plus a regress check).
- `pc_encode.c` is new. Surround Player could replace its inline encoder/packer with it.
