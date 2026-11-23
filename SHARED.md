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
| `scripts/build_ffmpeg.sh`, `build_ffmpeg_win.sh`, `build_ffmpeg_win.ps1`, `win_env.ps1`, `patches/ffmpeg/` | same paths | none |
| `CMakeLists.txt`, `scripts/build_win.ps1` | same paths | bridge targets instead of the player's; FFmpeg falls back to `../music_player`'s |
| `Sources/SurroundBridge/Upmix.swift` | `Sources/SurroundPlayer/Upmix.swift` (1.3.2) | `import BridgeCore`; no PlayerModel extension; `load(_:defaultMode:)` |
| `Sources/SurroundBridge/SettingsView.swift` (`UpmixBindings`, `UpmixSliders`) | `SettingsView.swift`'s upmix section | split into reusable views (menu + settings) |

## Fixes made here first (to carry over)

- **2026-11-20, DTS + upmix:** FFmpeg's DTS encoder has no 5.1 with back surrounds, so the upmix
  must be encoded as 5.1(side); the graph's aformat stage maps back → side with levels intact.
  Fixed in Surround Player too (engine.c `setup_audio`, after its 1.3.0 release, plus a regress check).
- `pc_encode.c` is new. Surround Player could replace its inline encoder/packer with it.
- **2026-11-23, treble boost:** `PCUpmix.treble_db` (pc_types.h; it's the player's playercore.h
  struct) + pc_upmix.c (`bass@ut` low shelf −T and `fl_in`/`fr_in` +T = a high shelf). Not in Surround
  Player yet. Carrying it over needs the field in playercore.h **and** in both Windows apps' C#
  mirrors of PCUpmix (`Native.cs`), or the struct sizes disagree.
- **2026-11-23, pc_output.c:** `pc_out_gaps` (added by the Windows side) now also counts PCM
  underruns (`starved`, one per gap).
