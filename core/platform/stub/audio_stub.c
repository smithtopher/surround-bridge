// Stub audio back-end: no devices. Build with -DPC_STUB_BACKENDS to get an engine
// with no OS audio dependency; file dumps (PCConfig.dump_path) still work.
// Used to check the core is portable, and as the first step when bringing up a
// new OS (get `pctest dump` working there before writing the real back-end).
#ifdef PC_STUB_BACKENDS

#include "../../pc_audio_device.h"
#include <stdio.h>

PCAudioDevice *pcad_open(const char *uid, PCStreamType type, PCDeviceFormat *fmt, char *err, int errlen) {
    (void)uid; (void)type; (void)fmt;
    snprintf(err, errlen, "No audio devices (stub back-end)");
    return NULL;
}
int pcad_start(PCAudioDevice *d, PCRenderCallback cb, void *ctx) { (void)d; (void)cb; (void)ctx; return -1; }
void pcad_close(PCAudioDevice *d) { (void)d; }
int pcad_list(PCDevice *out, int max) { (void)out; (void)max; return 0; }
void pcad_set_change_callback(PCDeviceChangeCallback cb, void *ctx) { (void)cb; (void)ctx; }
int pcad_lost(PCAudioDevice *d) { (void)d; return 0; }

#endif
