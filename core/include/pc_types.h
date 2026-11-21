// Types shared with Surround Player's engine (copied from its include/playercore.h, see
// SHARED.md). Kept identical so the shared files (pc_output.c, the device back-ends,
// pc_upmix.c) can be synced between the two repos without edits.
#ifndef PC_TYPES_H
#define PC_TYPES_H

#include <stdint.h>

#if defined(_WIN32) && defined(PC_BUILD_DLL)
#define PC_API __declspec(dllexport)
#elif defined(_WIN32) && defined(PC_USE_DLL)
#define PC_API __declspec(dllimport)
#else
#define PC_API
#endif

typedef struct {
    char uid[256];
    char name[256];
    int output_channels;
    int is_hdmi;
    int is_digital;             // HDMI / DisplayPort / optical
    int has_compressed_formats; // device advertises an AC3 physical format
    int is_default;
} PCDevice;

PC_API int pc_list_output_devices(PCDevice *out, int max);

// Device hot-plug: `cb` runs on an OS thread when output devices appear, disappear or change
// (Windows: IMMNotificationClient; macOS: not implemented, watch CoreAudio yourself).
typedef void (*PCDeviceChangeCallback)(void *ctx);
PC_API void pc_set_device_change_callback(PCDeviceChangeCallback cb, void *ctx);

// Stereo upmix: mono/stereo sources become 5.1 (FFmpeg's "surround" filter: frequency-domain
// ambience extraction; centred sound to the front/centre, out-of-phase ambience to the rears)
// before they are encoded to Dolby Digital / DTS or sent as multichannel PCM. Only applies when
// the output is surround (bitstream transcode or multichannel PCM). Player-wide setting:
// `enabled` and `include_video` take effect at the next pc_player_open (reopen to apply);
// everything else is applied live (the bass crossover rebuilds the filter: a ~40 ms dropout).
typedef struct {
    int enabled;
    int include_video;      // also files with a video track (else music only)
    float surround_db;      // rear level, -40..+6 dB; -40 = rears silent
    float center;           // 0..1 dialogue/centre focus: 0 phantom centre (no centre speaker),
                            // 0.5 balanced, 1 centred sound locked to the centre speaker
    float width;            // 0..1 front stage width, 0.5 = as recorded
    float lfe_db;           // subwoofer (LFE) level, -40..+6 dB; -40 = no LFE
    int crossover_hz;       // 40..250: bass below this goes to the LFE channel
    int lfe_move;           // 0: copy bass to LFE (mains keep it), 1: move it (small speakers)
    float bass_db;          // 0..12 dB low-shelf bass boost (100 Hz)
    float rear_delay_ms;    // 0..30: rears slightly late keeps sounds anchored at the front
    int rear_highcut_hz;    // 0 = off, else 2000..20000: rolls treble off the rears
} PCUpmix;

#endif
