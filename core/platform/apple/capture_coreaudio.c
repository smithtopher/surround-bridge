// CoreAudio capture back-end (macOS): the input side of the virtual device apps play into.
// BlackHole (prototype) and our own Audio Server plug-in (later) both expose a loopback input.
#if defined(__APPLE__) && !defined(PC_STUB_BACKENDS)

#include "../../pc_capture.h"
#include "../../pc_platform.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP_BUFFER_FRAMES 256      // small IO size: capture latency ~5 ms at 48 kHz

struct PCCapture {
    AudioDeviceID dev;
    AudioDeviceIOProcID procid;
    int running, channels;
    int interleaved;               // one buffer with all channels (else one buffer per channel)
    float *scratch;                // interleaving buffer for non-interleaved devices
    int scratch_frames;
    PCCaptureCallback cb;
    void *ctx;
};

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector s, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { s, sc, kAudioObjectPropertyElementMain };
    return a;
}

static OSStatus getp(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope sc, UInt32 size, void *data) {
    AudioObjectPropertyAddress a = addr(s, sc);
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &size, data);
}

static OSStatus setp(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope sc, UInt32 size, const void *data) {
    AudioObjectPropertyAddress a = addr(s, sc);
    return AudioObjectSetPropertyData(o, &a, 0, NULL, size, data);
}

static void *getp_alloc(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope sc, UInt32 *size_out) {
    AudioObjectPropertyAddress a = addr(s, sc);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(o, &a, 0, NULL, &size) != noErr || size == 0) return NULL;
    void *buf = malloc(size);
    if (AudioObjectGetPropertyData(o, &a, 0, NULL, &size, buf) != noErr) { free(buf); return NULL; }
    if (size_out) *size_out = size;
    return buf;
}

static void cfstr(CFStringRef s, char *buf, int len) {
    buf[0] = 0;
    if (s) CFStringGetCString(s, buf, len, kCFStringEncodingUTF8);
}

static int input_channels(AudioDeviceID d, int *buffers) {
    UInt32 size = 0;
    AudioBufferList *bl = getp_alloc(d, kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput, &size);
    int ch = 0;
    if (bl) {
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++) ch += bl->mBuffers[i].mNumberChannels;
        if (buffers) *buffers = (int)bl->mNumberBuffers;
        free(bl);
    }
    return ch;
}

static AudioDeviceID device_for_uid(const char *uid) {
    if (!uid || !uid[0]) return 0;
    CFStringRef s = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioDeviceID d = kAudioObjectUnknown;
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal);
    UInt32 size = sizeof(d);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(s), &s, &size, &d);
    CFRelease(s);
    return d == kAudioObjectUnknown ? 0 : d;
}

int pccap_list(PCDevice *out, int max) {
    UInt32 size = 0;
    AudioDeviceID *ids = getp_alloc(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, &size);
    if (!ids) return 0;
    int n = 0;
    for (UInt32 i = 0; i < size / sizeof(AudioDeviceID) && n < max; i++) {
        int ch = input_channels(ids[i], NULL);
        if (ch <= 0) continue;
        PCDevice *d = &out[n++];
        memset(d, 0, sizeof(*d));
        CFStringRef uid = NULL, name = NULL;
        getp(ids[i], kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, sizeof(uid), &uid);
        getp(ids[i], kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, sizeof(name), &name);
        cfstr(uid, d->uid, sizeof(d->uid));
        cfstr(name, d->name, sizeof(d->name));
        if (uid) CFRelease(uid);
        if (name) CFRelease(name);
        d->output_channels = ch;
        UInt32 t = 0;
        getp(ids[i], kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, sizeof(t), &t);
        d->is_hdmi = 0;
        d->is_digital = t == kAudioDeviceTransportTypeVirtual;   // here: "virtual device" (loopback)
    }
    free(ids);
    return n;
}

static OSStatus ioproc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in,
                       const AudioTimeStamp *intime, AudioBufferList *out, const AudioTimeStamp *outtime, void *ud) {
    (void)dev; (void)out; (void)outtime;
    PCCapture *c = ud;
    if (!in || in->mNumberBuffers == 0) return noErr;
    uint64_t host = (intime->mFlags & kAudioTimeStampHostTimeValid) ? intime->mHostTime : now->mHostTime;
    int64_t ns = (int64_t)AudioConvertHostTimeToNanos(host);
    if (c->interleaved) {
        const AudioBuffer *b = &in->mBuffers[0];
        int frames = (int)(b->mDataByteSize / (sizeof(float) * (UInt32)c->channels));
        if (frames > 0 && b->mData) c->cb(c->ctx, b->mData, frames, ns);
        return noErr;
    }
    // one buffer per channel (or groups): interleave into scratch
    int frames = (int)(in->mBuffers[0].mDataByteSize / (sizeof(float) * in->mBuffers[0].mNumberChannels));
    if (frames > c->scratch_frames) frames = c->scratch_frames;
    int ch = 0;
    for (UInt32 i = 0; i < in->mNumberBuffers && ch < c->channels; i++) {
        const AudioBuffer *b = &in->mBuffers[i];
        const float *src = b->mData;
        int bc = (int)b->mNumberChannels;
        for (int k = 0; k < bc && ch < c->channels; k++, ch++)
            for (int f = 0; f < frames; f++) c->scratch[f * c->channels + ch] = src ? src[f * bc + k] : 0.f;
    }
    c->cb(c->ctx, c->scratch, frames, ns);
    return noErr;
}

PCCapture *pccap_open(const char *uid, PCCaptureFormat *fmt, char *err, int errlen) {
    memset(fmt, 0, sizeof(*fmt));
    PCCapture *c = calloc(1, sizeof(*c));
    c->dev = device_for_uid(uid);
    if (!c->dev) { snprintf(err, errlen, "Input device not found"); goto fail; }
    int buffers = 0;
    c->channels = input_channels(c->dev, &buffers);
    if (c->channels <= 0) { snprintf(err, errlen, "Device has no input"); goto fail; }
    c->interleaved = buffers == 1;

    // 48 kHz (the encoders' rate) and a small IO size; the virtual device follows our request
    Float64 sr = 48000;
    setp(c->dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, sizeof(sr), &sr);
    UInt32 bf = CAP_BUFFER_FRAMES;
    setp(c->dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, sizeof(bf), &bf);
    getp(c->dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, sizeof(sr), &sr);
    getp(c->dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, sizeof(bf), &bf);

    // the IOProc sees the input streams' virtual format: it must be 32-bit float
    UInt32 size = 0;
    AudioStreamID *streams = getp_alloc(c->dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, &size);
    if (streams) {
        AudioStreamBasicDescription v;
        if (getp(streams[0], kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, sizeof(v), &v) == noErr &&
            (v.mFormatID != kAudioFormatLinearPCM || !(v.mFormatFlags & kAudioFormatFlagIsFloat) || v.mBitsPerChannel != 32)) {
            free(streams);
            snprintf(err, errlen, "Input device is not in a 32-bit float format");
            goto fail;
        }
        free(streams);
    }
    c->scratch_frames = (int)bf * 4 > 4096 ? (int)bf * 4 : 4096;
    c->scratch = calloc((size_t)c->scratch_frames * (size_t)c->channels, sizeof(float));

    UInt32 dlat = 0, safety = 0;
    getp(c->dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput, sizeof(dlat), &dlat);
    getp(c->dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput, sizeof(safety), &safety);
    fmt->channels = c->channels;
    fmt->rate = sr;
    fmt->latency = (double)(dlat + safety + bf) / sr;
    char name[128] = "";
    CFStringRef nm = NULL;
    getp(c->dev, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, sizeof(nm), &nm);
    cfstr(nm, name, sizeof(name));
    if (nm) CFRelease(nm);
    snprintf(fmt->description, sizeof(fmt->description), "%s, %d ch float %.0f Hz, %u-frame IO%s", name, c->channels, sr,
             (unsigned)bf, c->interleaved ? "" : " (non-interleaved)");

    if (AudioDeviceCreateIOProcID(c->dev, ioproc, c, &c->procid) != noErr) {
        snprintf(err, errlen, "AudioDeviceCreateIOProcID failed");
        goto fail;
    }
    return c;
fail:
    pccap_close(c);
    return NULL;
}

int pccap_start(PCCapture *c, PCCaptureCallback cb, void *ctx) {
    c->cb = cb;
    c->ctx = ctx;
    if (AudioDeviceStart(c->dev, c->procid) != noErr) return -1;
    c->running = 1;
    return 0;
}

void pccap_close(PCCapture *c) {
    if (!c) return;
    if (c->procid) {
        if (c->running) AudioDeviceStop(c->dev, c->procid);
        AudioDeviceDestroyIOProcID(c->dev, c->procid);
    }
    free(c->scratch);
    free(c);
}

#endif
