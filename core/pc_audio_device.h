// Audio device back-end interface. One implementation per OS:
//   platform/apple/audio_coreaudio.c   CoreAudio HAL (hog mode, physical format switch)
//   platform/windows/…                 WASAPI (to be written; see docs/PORTING_WINDOWS.md)
//   platform/stub/audio_stub.c         no devices (file dumps only), for bring-up/tests
//
// The back-end only owns the device. Everything else (ring buffer, IEC 61937 burst
// handling, keep-alive silence, pause/flush, the clock) is portable code in
// pc_output.c, which the back-end drives through the render callback.
#ifndef PC_AUDIO_DEVICE_H
#define PC_AUDIO_DEVICE_H

#include <stdint.h>
#include "pc_types.h"

typedef struct PCAudioDevice PCAudioDevice;

// What the stream carries. WASAPI exclusive mode must declare the IEC 61937 payload
// (KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL / _DTS); CoreAudio's digital format
// is payload-agnostic and only cares about PCM vs IEC 61937.
typedef enum {
    PC_STREAM_PCM = 0,
    PC_STREAM_IEC61937_AC3 = 1,
    PC_STREAM_IEC61937_DTS = 2,
    // Decoded multichannel PCM with exclusive access, bit-exact (no OS mixer, no Windows
    // speaker setup). The request is read from *fmt: channels, rate, channel_mask.
    // Back-ends without it (CoreAudio for now) refuse it; the engine then uses PC_STREAM_PCM.
    PC_STREAM_PCM_EXCLUSIVE = 3,
} PCStreamType;

// Format of the buffers handed to the render callback (interleaved).
// Bitstream mode: must be 2 channels at 48 kHz, as 16-bit int, 32-bit int (the
//   16-bit IEC words go in the top bits) or 32-bit float (s16 / 32768, which the
//   OS converts back bit-exactly when nothing else touches the samples).
// PCM mode: must be 32-bit float, any channel count and rate.
// PCM exclusive mode: 16-bit int, 32-bit int (24 or 32 valid bits) or 32-bit float.
typedef struct {
    int channels;
    int bits;           // 16 or 32
    int is_float;
    double rate;
    double latency;     // seconds from a buffer's present time until it is audible
    uint32_t channel_mask;  // speaker positions in channel order (WAVE SPEAKER_* == FFmpeg AV_CH_*), 0 = default
    char description[512];
} PCDeviceFormat;

// Called on the device's real-time thread: fill `frames` frames at dst.
// present_ns: pc_now_ns() time at which the first frame leaves the device
// (excluding `latency`, which the clock adds itself).
typedef void (*PCRenderCallback)(void *ctx, uint8_t *dst, int frames, int64_t present_ns);

// uid: device id from pcad_list ("" or NULL = system default).
// IEC 61937 types: exclusive access in a format that carries the bursts untouched.
//   Must refuse (return NULL with a message) on outputs that would play the data
//   as PCM noise (speakers, headphones, Bluetooth): only HDMI/DisplayPort/S/PDIF.
// PC_STREAM_PCM: shared mode.
// PC_STREAM_PCM_EXCLUSIVE: *fmt holds the request on entry (see above).
PCAudioDevice *pcad_open(const char *uid, PCStreamType type, PCDeviceFormat *fmt, char *err, int errlen);
int pcad_start(PCAudioDevice *d, PCRenderCallback cb, void *ctx);
// Stops, then undoes everything open changed (exclusive access, formats, mixing).
void pcad_close(PCAudioDevice *d);
// Backs pc_list_output_devices.
int pcad_list(PCDevice *out, int max);
// Backs pc_set_device_change_callback (may be a no-op where the app watches devices itself).
void pcad_set_change_callback(PCDeviceChangeCallback cb, void *ctx);
// 1 once the open stream was invalidated (device removed / reconfigured); it stays dead.
int pcad_lost(PCAudioDevice *d);

#endif
