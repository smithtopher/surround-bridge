// Surround Bridge: everything apps play (captured from a virtual audio device) becomes a
// Dolby Digital / DTS bitstream on an HDMI or optical output, with stereo upmixed to 5.1.
//
//   capture (virtual device, 48 kHz float) -> input ring -> worker thread:
//     resample (drift-steered) -> [stereo upmix] -> AC3/DTS encode -> IEC 61937 -> pc_output ring
//     -> exclusive device (keep-alive silence bursts whenever there's nothing to send)
//   or, with SB_CODEC_PCM: ... -> [stereo upmix] -> multichannel PCM, exclusive (HDMI receivers)
#ifndef SB_BRIDGE_H
#define SB_BRIDGE_H

#include "pc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SBBridge SBBridge;

// SB_CODEC_PCM: no encoding; multichannel PCM with exclusive access (for HDMI receivers; optical
// can't carry it). Upmixed stereo goes out as 5.1(side) at 48 kHz, 6/8-channel input in its own
// layout, stereo with the upmix off as stereo. The audio is laid out in whatever the device opens
// (its channel count and speaker order); the device must run at 48 kHz.
enum { SB_CODEC_AC3 = 0, SB_CODEC_DTS = 1, SB_CODEC_PCM = 2 };

typedef struct {
    const char *input_uid;      // capture (virtual) device, from sb_list_inputs
    const char *input_file;     // testing: feed this media file instead of a device
    int input_realtime;         // testing: pace input_file at real time (else as fast as it goes)
    const char *output_uid;     // HDMI/optical output ("" or NULL = system default)
    const char *dump_path;      // testing: write the IEC 61937 stream to this file, no device
    int codec;                  // SB_CODEC_*
    int ac3_bitrate_kbps;       // 0 = 640
    int dts_bitrate_kbps;       // 0 = 1509 (or 754)
    double target_buffer_ms;    // output fill the drift control holds (0 = auto: what it settles at)
} SBConfig;

typedef struct {
    int running;
    int upmixing;               // stereo input, upmix on
    char input[256];            // capture description
    char output[512];           // output description
    double buffer_ms;           // output ring fill now (smoothed)
    double drift_ppm;           // resampler correction holding the buffer (input vs output clock)
    double latency_ms;          // estimated app -> HDMI delay (excluding the receiver's decode)
    long long overruns;         // input frames dropped (worker couldn't keep up)
    long long jumps;            // sudden buffer jumps (>40 ms) cut by dropping input; PCM: also
                                // refills with silence after an underrun
    double target_ms;           // the buffer level being held (0 until it locks, 2 s after audio starts)
    float peak_db[8];           // input peak level per channel over the last ~0.5 s (-120 = silence)
    int channels;
    long long gaps;             // output underruns: silence played in place of audio (audible)
} SBStatus;

PC_API SBBridge *sb_bridge_create(void);
PC_API void sb_bridge_destroy(SBBridge *b);
PC_API int sb_bridge_start(SBBridge *b, const SBConfig *cfg, char *err, int errlen);
// With input_file: blocks until the file has gone through (and the dump is complete).
PC_API void sb_bridge_wait(SBBridge *b);
PC_API void sb_bridge_stop(SBBridge *b);
// Live: everything but enabled/include_video applies immediately; enabled takes effect at the
// next start (the encoder layout changes). include_video is ignored (no notion of video here).
PC_API void sb_bridge_set_upmix(SBBridge *b, const PCUpmix *u);
PC_API void sb_bridge_status(SBBridge *b, SBStatus *st);

PC_API int sb_list_inputs(PCDevice *out, int max);    // devices with an input (capture) side
PC_API int sb_list_outputs(PCDevice *out, int max);   // = pc_list_output_devices

#ifdef __cplusplus
}
#endif
#endif
