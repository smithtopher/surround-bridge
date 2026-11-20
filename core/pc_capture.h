// Capture back-end interface: reads what apps play into the virtual device. One per OS:
//   platform/apple/capture_coreaudio.c   input side of the virtual device (BlackHole for now,
//                                        our own Audio Server plug-in later)
//   platform/windows/…                   WASAPI loopback (to be written)
#ifndef PC_CAPTURE_H
#define PC_CAPTURE_H

#include <stdint.h>
#include "pc_types.h"

typedef struct PCCapture PCCapture;

typedef struct {
    int channels;           // what the device delivers (2 for BlackHole 2ch; 6/8 for multichannel)
    double rate;            // Hz (the back-end asks for 48 kHz, but the device may run at another rate)
    double latency;         // seconds from the app's sample reaching the device to our callback
    char description[256];
} PCCaptureFormat;

// Called on the device's real-time thread: `frames` interleaved float frames. Must not block.
// capture_ns: pc_now_ns() time the first frame entered the device.
typedef void (*PCCaptureCallback)(void *ctx, const float *data, int frames, int64_t capture_ns);

// uid: input device id from pccap_list. Asks the device for 48 kHz (BlackHole follows).
PCCapture *pccap_open(const char *uid, PCCaptureFormat *fmt, char *err, int errlen);
int pccap_start(PCCapture *c, PCCaptureCallback cb, void *ctx);
void pccap_close(PCCapture *c);
// Devices with input channels (output_channels holds their input channel count here).
int pccap_list(PCDevice *out, int max);

#endif
