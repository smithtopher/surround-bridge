// CoreAudio HAL audio device back-end (macOS).
//
// Bitstream path (equivalent of Kodi's WASAPI exclusive / AESinkDARWINOSX passthrough):
//   1. take exclusive ownership of the device (kAudioDevicePropertyHogMode)
//   2. disable mixing (kAudioDevicePropertySupportsMixing = 0) so the HAL does no
//      volume / mixing / dithering on our samples
//   3. switch the stream's physical format to a digital AC3 format if the device
//      offers one, otherwise to 2ch 16-bit integer LPCM @ 48 kHz (what HDMI exposes)
//   4. ask for a virtual format identical to the physical one, so IOProc buffers
//      reach the hardware unconverted (pc_output.c handles int16/int32/float buffers)
//   5. on close restore the original format, mixing and release hog mode.
//
// Multichannel PCM path (PC_STREAM_PCM_EXCLUSIVE, decoded surround untouched, for HDMI
// into an AV receiver): the same hog / no-mixing steps, then an N-channel LPCM physical
// format at the source rate (else 48 kHz) and a float virtual format of the same shape.
// The device's preferred channel layout says which speaker each channel feeds; the
// engine gets the matching speaker mask and the IOProc puts every channel in its slot.
#if defined(__APPLE__) && !defined(PC_STUB_BACKENDS)

#include "../../pc_audio_device.h"
#include "../../pc_platform.h"

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioFormat.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct PCAudioDevice {
    AudioDeviceID dev;
    AudioStreamID stream;
    int stream_buffer;              // index of our stream in the IOProc buffer list
    AudioDeviceIOProcID procid;
    int running;
    int hogged, mixing_changed, phys_changed, virt_changed;
    AudioStreamBasicDescription orig_phys, orig_virt;
    int bytes_per_frame;
    PCRenderCallback cb;
    void *ctx;
    // multichannel PCM: device channel k plays engine channel perm[k] (-1 = silence)
    int remap, dev_channels, eng_channels, sample_bytes;
    int perm[32];
    uint8_t *tmp;               // engine-order frames, rendered before the scatter
    int tmp_frames;
    double rate;
};

// ---------------------------------------------------------------------------
// property helpers

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

static void cfstr_to_c(CFStringRef s, char *buf, int len) {
    buf[0] = 0;
    if (s) CFStringGetCString(s, buf, len, kCFStringEncodingUTF8);
}

static void device_name(AudioDeviceID d, char *buf, int len) {
    CFStringRef s = NULL;
    getp(d, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, sizeof(s), &s);
    cfstr_to_c(s, buf, len);
    if (s) CFRelease(s);
}

static UInt32 device_transport(AudioDeviceID d) {
    UInt32 t = 0;
    getp(d, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, sizeof(t), &t);
    return t;
}

static const char *transport_name(UInt32 t) {
    switch (t) {
    case kAudioDeviceTransportTypeHDMI: return "HDMI";
    case kAudioDeviceTransportTypeDisplayPort: return "DisplayPort";
    case kAudioDeviceTransportTypeBuiltIn: return "built-in";
    case kAudioDeviceTransportTypeUSB: return "USB";
    case kAudioDeviceTransportTypeBluetooth: case kAudioDeviceTransportTypeBluetoothLE: return "Bluetooth";
    case kAudioDeviceTransportTypeAirPlay: return "AirPlay";
    case kAudioDeviceTransportTypeThunderbolt: return "Thunderbolt";
    case kAudioDeviceTransportTypeVirtual: return "virtual";
    case kAudioDeviceTransportTypeAggregate: return "aggregate";
    default: return "other";
    }
}

static AudioStreamID first_output_stream(AudioDeviceID d) {
    UInt32 size = 0;
    AudioStreamID *ids = getp_alloc(d, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, &size);
    AudioStreamID s = 0;
    if (ids && size >= sizeof(AudioStreamID)) s = ids[0];
    free(ids);
    return s;
}

static int is_compressed_format(UInt32 id) {
    return id == kAudioFormat60958AC3 || id == kAudioFormatAC3 || id == 'IAC3';
}

static int output_channel_count(AudioDeviceID d) {
    UInt32 size = 0;
    AudioBufferList *bl = getp_alloc(d, kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, &size);
    int ch = 0;
    if (bl) {
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++) ch += bl->mBuffers[i].mNumberChannels;
        free(bl);
    }
    return ch;
}

static int device_has_compressed(AudioDeviceID d) {
    AudioStreamID s = first_output_stream(d);
    if (!s) return 0;
    UInt32 size = 0;
    AudioStreamRangedDescription *f = getp_alloc(s, kAudioStreamPropertyAvailablePhysicalFormats, kAudioObjectPropertyScopeGlobal, &size);
    int found = 0;
    if (f) {
        for (UInt32 i = 0; i < size / sizeof(*f); i++)
            if (is_compressed_format(f[i].mFormat.mFormatID)) found = 1;
        free(f);
    }
    return found;
}

static AudioDeviceID default_output_device(void) {
    AudioDeviceID d = 0;
    getp(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, sizeof(d), &d);
    return d;
}

static AudioDeviceID device_for_uid(const char *uid) {
    if (!uid || !uid[0]) return default_output_device();
    CFStringRef s = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioDeviceID d = kAudioObjectUnknown;
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal);
    UInt32 size = sizeof(d);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(s), &s, &size, &d);
    CFRelease(s);
    if (d == kAudioObjectUnknown) return 0;
    return d;
}

int pcad_list(PCDevice *out, int max) {
    UInt32 size = 0;
    AudioDeviceID *ids = getp_alloc(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, &size);
    if (!ids) return 0;
    AudioDeviceID def = default_output_device();
    int n = 0;
    for (UInt32 i = 0; i < size / sizeof(AudioDeviceID) && n < max; i++) {
        int ch = output_channel_count(ids[i]);
        if (ch <= 0) continue;
        PCDevice *d = &out[n++];
        memset(d, 0, sizeof(*d));
        CFStringRef uid = NULL;
        getp(ids[i], kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, sizeof(uid), &uid);
        cfstr_to_c(uid, d->uid, sizeof(d->uid));
        if (uid) CFRelease(uid);
        device_name(ids[i], d->name, sizeof(d->name));
        d->output_channels = ch;
        UInt32 t = device_transport(ids[i]);
        d->is_hdmi = t == kAudioDeviceTransportTypeHDMI || t == kAudioDeviceTransportTypeDisplayPort;
        d->has_compressed_formats = device_has_compressed(ids[i]);
        d->is_digital = d->is_hdmi || d->has_compressed_formats;
        d->is_default = ids[i] == def;
    }
    free(ids);
    return n;
}

// ---------------------------------------------------------------------------
// format selection

static int range_has_48k(const AudioStreamRangedDescription *r) {
    if (r->mFormat.mSampleRate == 48000.0) return 1;
    return r->mSampleRateRange.mMinimum <= 48000.0 && r->mSampleRateRange.mMaximum >= 48000.0;
}

// Score a physical format for carrying IEC 61937; higher is better, <0 unusable.
static int score_bitstream_format(const AudioStreamRangedDescription *r) {
    const AudioStreamBasicDescription *f = &r->mFormat;
    if (!range_has_48k(r)) return -1;
    if (is_compressed_format(f->mFormatID)) return 100;   // true digital format (optical/S/PDIF, some HDMI)
    if (f->mFormatID != kAudioFormatLinearPCM) return -1;
    if (f->mChannelsPerFrame != 2) return -1;
    if (f->mFormatFlags & kAudioFormatFlagIsFloat) return -1;
    if (f->mFormatFlags & kAudioFormatFlagIsNonInterleaved) return -1;
    if (f->mBitsPerChannel == 16) return 90;               // HDMI: 2ch 16-bit 48k LPCM
    if (f->mBitsPerChannel == 24 || f->mBitsPerChannel == 32) return 50; // carries s16 in the MSBs
    return -1;
}

static int same_format(const AudioStreamBasicDescription *a, const AudioStreamBasicDescription *b) {
    return a->mFormatID == b->mFormatID && a->mSampleRate == b->mSampleRate &&
           a->mChannelsPerFrame == b->mChannelsPerFrame && a->mBitsPerChannel == b->mBitsPerChannel;
}

static int set_physical_format(AudioStreamID s, const AudioStreamBasicDescription *want) {
    AudioStreamBasicDescription cur;
    if (getp(s, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, sizeof(cur), &cur) == noErr &&
        same_format(&cur, want))
        return 0;
    if (setp(s, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, sizeof(*want), want) != noErr)
        return -1;
    // The change is asynchronous; wait for the HAL to report it (Kodi does the same).
    for (int i = 0; i < 200; i++) {
        if (getp(s, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, sizeof(cur), &cur) == noErr &&
            same_format(&cur, want))
            return 0;
        pc_sleep_us(10000);
    }
    return -1;
}

static void fourcc_str(UInt32 id, char out[5]) {
    out[0] = (char)(id >> 24); out[1] = (char)(id >> 16); out[2] = (char)(id >> 8); out[3] = (char)id; out[4] = 0;
}

// ---------------------------------------------------------------------------
// exclusive access (bitstream and multichannel PCM)

// Hog mode + mixing off: no other client, and the HAL does no mixing / volume / dither.
static int take_exclusive(PCAudioDevice *d, const char *name, char *err, int errlen) {
    pid_t hog = -1;
    getp(d->dev, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, sizeof(hog), &hog);
    if (hog != -1 && hog != getpid()) {
        snprintf(err, errlen, "'%s' is in exclusive use by another application (pid %d)", name, hog);
        return -1;
    }
    if (hog != getpid()) {
        pid_t me = getpid();
        if (setp(d->dev, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, sizeof(me), &me) == noErr)
            d->hogged = 1;
    }
    AudioObjectPropertyAddress ma = addr(kAudioDevicePropertySupportsMixing, kAudioObjectPropertyScopeGlobal);
    Boolean settable = false;
    if (AudioObjectHasProperty(d->dev, &ma) && AudioObjectIsPropertySettable(d->dev, &ma, &settable) == noErr && settable) {
        UInt32 mix = 0;
        if (setp(d->dev, kAudioDevicePropertySupportsMixing, kAudioObjectPropertyScopeGlobal, sizeof(mix), &mix) == noErr)
            d->mixing_changed = 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// multichannel PCM: format and channel layout

static int range_has(const AudioStreamRangedDescription *r, double rate) {
    if (r->mFormat.mSampleRate == rate) return 1;
    return r->mSampleRateRange.mMinimum <= rate && r->mSampleRateRange.mMaximum >= rate;
}

// Score a physical format for `ch` channels of PCM at `rate` (falling back to 48 kHz);
// higher is better, <0 unusable. *use_rate gets the rate to run at.
static int score_pcm_format(const AudioStreamRangedDescription *r, int ch, double rate, double *use_rate) {
    const AudioStreamBasicDescription *f = &r->mFormat;
    if (f->mFormatID != kAudioFormatLinearPCM || (f->mFormatFlags & kAudioFormatFlagIsNonInterleaved)) return -1;
    int n = (int)f->mChannelsPerFrame;
    if (n < ch || n > 32) return -1;
    int sc;
    if (range_has(r, rate)) { sc = 2000; *use_rate = rate; }
    else if (range_has(r, 48000.0)) { sc = 1000; *use_rate = 48000.0; }
    else return -1;
    sc -= (n - ch) * 50;                                    // exact channel count first
    int fl = (f->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    if (!fl && (f->mBitsPerChannel == 24 || f->mBitsPerChannel == 32)) sc += 30;
    else if (fl && f->mBitsPerChannel == 32) sc += 25;
    else if (!fl && f->mBitsPerChannel == 16) sc += 10;
    else return -1;
    return sc;
}

// CoreAudio channel label -> WAVE SPEAKER_* / FFmpeg AV_CH_* bit (0: none)
static uint32_t label_bit(AudioChannelLabel l) {
    switch (l) {
    case kAudioChannelLabel_Left: return 0x1;
    case kAudioChannelLabel_Right: return 0x2;
    case kAudioChannelLabel_Center: case kAudioChannelLabel_Mono: return 0x4;
    case kAudioChannelLabel_LFEScreen: return 0x8;
    case kAudioChannelLabel_RearSurroundLeft: return 0x10;     // back left
    case kAudioChannelLabel_RearSurroundRight: return 0x20;
    case kAudioChannelLabel_LeftCenter: return 0x40;
    case kAudioChannelLabel_RightCenter: return 0x80;
    case kAudioChannelLabel_CenterSurround: return 0x100;
    case kAudioChannelLabel_LeftSurround: case kAudioChannelLabel_LeftSurroundDirect: return 0x200;   // side left
    case kAudioChannelLabel_RightSurround: case kAudioChannelLabel_RightSurroundDirect: return 0x400;
    case kAudioChannelLabel_TopCenterSurround: return 0x800;
    case kAudioChannelLabel_VerticalHeightLeft: return 0x1000;
    case kAudioChannelLabel_VerticalHeightCenter: return 0x2000;
    case kAudioChannelLabel_VerticalHeightRight: return 0x4000;
    case kAudioChannelLabel_TopBackLeft: return 0x8000;
    case kAudioChannelLabel_TopBackCenter: return 0x10000;
    case kAudioChannelLabel_TopBackRight: return 0x20000;
    default: return 0;
    }
}

// Speaker bit of each of the first n channels of `lay` (descriptions, bitmap or a tag).
// Returns the number of channels with a known speaker (0: layout unknown).
static int layout_speakers(const AudioChannelLayout *lay, UInt32 size, int n, uint32_t *bits) {
    memset(bits, 0, sizeof(*bits) * (size_t)n);
    AudioChannelLayout *expanded = NULL;
    if (lay->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions &&
        lay->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelBitmap) {
        // a layout tag (e.g. MPEG 5.1 A): expand it to per-channel descriptions
        AudioChannelLayoutTag tag = lay->mChannelLayoutTag;
        UInt32 esz = 0;
        if (AudioFormatGetPropertyInfo(kAudioFormatProperty_ChannelLayoutForTag, sizeof(tag), &tag, &esz) != noErr || !esz) return 0;
        expanded = malloc(esz);
        if (AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutForTag, sizeof(tag), &tag, &esz, expanded) != noErr) {
            free(expanded);
            return 0;
        }
        lay = expanded;
        size = esz;
    }
    uint32_t used = 0;
    int known = 0;
    // (an expanded tag keeps its tag but carries the descriptions)
    if (expanded || lay->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) {
        UInt32 nd = lay->mNumberChannelDescriptions;
        if (size < offsetof(AudioChannelLayout, mChannelDescriptions) + nd * sizeof(AudioChannelDescription)) nd = 0;
        for (int i = 0; i < n && i < (int)nd; i++) {
            uint32_t b = label_bit(lay->mChannelDescriptions[i].mChannelLabel);
            if (b && !(used & b)) { bits[i] = b; used |= b; known++; }
        }
    } else if (lay->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap) {
        int i = 0;   // channels in bitmap order; kAudioChannelBit_X == 1 << (kAudioChannelLabel_X - 1)
        for (int k = 0; k < 18 && i < n; k++) {
            if (!(lay->mChannelBitmap & (1u << k))) continue;
            uint32_t b = label_bit((AudioChannelLabel)(k + 1));
            if (b && !(used & b)) { bits[i] = b; used |= b; known++; }
            i++;
        }
    }
    free(expanded);
    return known;
}

static int device_speakers(AudioDeviceID dev, int n, uint32_t *bits) {
    UInt32 size = 0;
    AudioChannelLayout *lay = getp_alloc(dev, kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput, &size);
    if (!lay) { memset(bits, 0, sizeof(*bits) * (size_t)n); return 0; }
    int known = layout_speakers(lay, size, n, bits);
    free(lay);
    return known;
}

static int popcount32(uint32_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

// The engine renders channels in ascending speaker-bit order (WAVE / FFmpeg native order);
// device channel i plays engine channel perm[i] (-1: no speaker, silence). Returns the mask.
static uint32_t speaker_perm(const uint32_t *bits, int nch, int *perm, int *identity) {
    uint32_t mask = 0;
    for (int i = 0; i < nch; i++) mask |= bits[i];
    *identity = 1;
    for (int i = 0; i < nch; i++) {
        if (!bits[i]) { perm[i] = -1; *identity = 0; continue; }
        perm[i] = popcount32(mask & (bits[i] - 1));      // rank in ascending speaker order
        if (perm[i] != i) *identity = 0;
    }
    return mask;
}

// Opens the multichannel PCM stream. `req` (channels, rate, channel_mask) comes from
// the engine; on success fmt describes what the engine must render.
static int open_pcm_exclusive(PCAudioDevice *d, const PCDeviceFormat *req, PCDeviceFormat *fmt,
                              const char *name, UInt32 transport, char *err, int errlen) {
    if (take_exclusive(d, name, err, errlen) != 0) return -1;
    UInt32 size = 0;
    AudioStreamRangedDescription *fmts = getp_alloc(d->stream, kAudioStreamPropertyAvailablePhysicalFormats, kAudioObjectPropertyScopeGlobal, &size);
    int best = -1, best_score = -1;
    double best_rate = 0;
    for (UInt32 i = 0; fmts && i < size / sizeof(*fmts); i++) {
        double r = 0;
        int sc = score_pcm_format(&fmts[i], req->channels, req->rate, &r);
        if (sc > best_score) { best_score = sc; best = (int)i; best_rate = r; }
    }
    if (best < 0) {
        free(fmts);
        snprintf(err, errlen, "'%s' offers no %d-channel PCM format", name, req->channels);
        return -1;
    }
    AudioStreamBasicDescription want = fmts[best].mFormat;
    free(fmts);
    want.mSampleRate = best_rate;
    if (set_physical_format(d->stream, &want) != 0) {
        snprintf(err, errlen, "Could not switch '%s' to %u-channel PCM at %.0f Hz", name, (unsigned)want.mChannelsPerFrame, best_rate);
        return -1;
    }
    d->phys_changed = !same_format(&want, &d->orig_phys);
    Float64 sr = best_rate;
    setp(d->dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, sizeof(sr), &sr);

    // float virtual format of the same shape (exact for 16/24-bit sources); else the physical one
    int nch = (int)want.mChannelsPerFrame;
    AudioStreamBasicDescription vf = { 0 };
    vf.mSampleRate = best_rate;
    vf.mFormatID = kAudioFormatLinearPCM;
    vf.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    vf.mChannelsPerFrame = (UInt32)nch;
    vf.mBitsPerChannel = 32;
    vf.mBytesPerFrame = vf.mBytesPerPacket = (UInt32)nch * 4;
    vf.mFramesPerPacket = 1;
    if (setp(d->stream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, sizeof(vf), &vf) == noErr) d->virt_changed = 1;
    AudioStreamBasicDescription virt;
    getp(d->stream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, sizeof(virt), &virt);
    int vfloat = (virt.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    int vbytes = virt.mChannelsPerFrame ? (int)(virt.mBytesPerFrame / virt.mChannelsPerFrame) : 0;
    int ok = virt.mFormatID == kAudioFormatLinearPCM && !(virt.mFormatFlags & kAudioFormatFlagIsNonInterleaved) &&
             (int)virt.mChannelsPerFrame == nch && virt.mSampleRate == best_rate &&
             (vfloat ? virt.mBitsPerChannel == 32 && vbytes == 4 : (vbytes == 2 && virt.mBitsPerChannel == 16) || vbytes == 4);
    if (!ok && setp(d->stream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, sizeof(want), &want) == noErr) {
        d->virt_changed = 1;
        getp(d->stream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, sizeof(virt), &virt);
        vfloat = (virt.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
        vbytes = virt.mChannelsPerFrame ? (int)(virt.mBytesPerFrame / virt.mChannelsPerFrame) : 0;
        ok = virt.mFormatID == kAudioFormatLinearPCM && !(virt.mFormatFlags & kAudioFormatFlagIsNonInterleaved) &&
             (int)virt.mChannelsPerFrame == nch && (vfloat ? vbytes == 4 : vbytes == 2 || vbytes == 4);
    }
    // the engine writes full-scale int32: 24 valid bits must sit in the high bytes
    if (ok && !vfloat && vbytes == 4 && virt.mBitsPerChannel < 32 && !(virt.mFormatFlags & kAudioFormatFlagIsAlignedHigh)) ok = 0;
    if (!ok) {
        snprintf(err, errlen, "Unsupported stream format on '%s' (%u ch, %u bit)", name,
                 (unsigned)virt.mChannelsPerFrame, (unsigned)virt.mBitsPerChannel);
        return -1;
    }

    // which speaker each device channel feeds -> the mask the engine renders, plus the scatter
    uint32_t bits[32];
    int known = device_speakers(d->dev, nch, bits);
    if (known == 0) {
        // layout unknown: assume the device takes the source's own order
        if (nch != req->channels || popcount32(req->channel_mask) != req->channels) {
            snprintf(err, errlen, "'%s' doesn't report its speaker layout", name);
            return -1;
        }
        uint32_t m = req->channel_mask;
        for (int i = 0; i < nch; i++) { bits[i] = m & (~m + 1); m &= m - 1; }
    }
    int identity;
    uint32_t mask = speaker_perm(bits, nch, d->perm, &identity);
    d->dev_channels = nch;
    d->eng_channels = popcount32(mask);
    d->sample_bytes = vbytes;
    d->rate = best_rate;
    if (!identity || d->eng_channels != nch) {
        d->remap = 1;
        d->tmp_frames = 4096;
        d->tmp = malloc((size_t)d->tmp_frames * (size_t)d->eng_channels * (size_t)vbytes);
    }
    fmt->channels = d->eng_channels;
    fmt->channel_mask = mask;
    fmt->rate = best_rate;
    fmt->is_float = vfloat;
    fmt->bits = vbytes * 8;
    d->bytes_per_frame = nch * vbytes;      // device frame (the IOProc's buffer)

    char order[160] = "";
    static const char *spk[] = { "L", "R", "C", "LFE", "Lb", "Rb", "Lc", "Rc", "Cb", "Ls", "Rs", "Tc",
                                 "Tfl", "Tfc", "Tfr", "Tbl", "Tbc", "Tbr" };
    for (int i = 0; i < nch; i++) {
        int b = bits[i] ? __builtin_ctz(bits[i]) : -1;
        size_t len = strlen(order);
        snprintf(order + len, sizeof(order) - len, "%s%s", i ? " " : "", b >= 0 && b < 18 ? spk[b] : "-");
    }
    snprintf(fmt->description, sizeof(fmt->description),
             "%s (%s), exclusive%s, physical LPCM %d ch %u-bit %.0f Hz, IOProc %s%d, channels %s%s",
             name, transport_name(transport), d->hogged ? "" : " (hog mode unavailable!)", nch,
             (unsigned)want.mBitsPerChannel, best_rate, vfloat ? "float" : "int", vbytes * 8, order,
             known ? "" : " (assumed)");
    return 0;
}

// ---------------------------------------------------------------------------
// IOProc

static OSStatus ioproc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in,
                       const AudioTimeStamp *intime, AudioBufferList *out, const AudioTimeStamp *outtime, void *ud) {
    (void)dev; (void)in; (void)intime;
    PCAudioDevice *d = ud;
    for (UInt32 i = 0; i < out->mNumberBuffers; i++)
        if ((int)i != d->stream_buffer) memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);
    if (d->stream_buffer >= (int)out->mNumberBuffers) return noErr;
    AudioBuffer *b = &out->mBuffers[d->stream_buffer];
    uint64_t host = (outtime->mFlags & kAudioTimeStampHostTimeValid) ? outtime->mHostTime : now->mHostTime;
    int64_t present = (int64_t)AudioConvertHostTimeToNanos(host);
    int frames = (int)(b->mDataByteSize / d->bytes_per_frame);
    if (!d->remap) {
        d->cb(d->ctx, b->mData, frames, present);
        return noErr;
    }
    // multichannel PCM in a different speaker order: render in engine order, then scatter
    uint8_t *dst = b->mData;
    int sb = d->sample_bytes, done = 0;
    while (done < frames) {
        int n = frames - done < d->tmp_frames ? frames - done : d->tmp_frames;
        d->cb(d->ctx, d->tmp, n, present + (int64_t)((double)done * 1e9 / d->rate));
        for (int f = 0; f < n; f++) {
            const uint8_t *src = d->tmp + (size_t)f * (size_t)d->eng_channels * (size_t)sb;
            uint8_t *out = dst + ((size_t)(done + f) * (size_t)d->dev_channels) * (size_t)sb;
            for (int c = 0; c < d->dev_channels; c++) {
                if (d->perm[c] >= 0) memcpy(out + c * sb, src + d->perm[c] * sb, (size_t)sb);
                else memset(out + c * sb, 0, (size_t)sb);
            }
        }
        done += n;
    }
    return noErr;
}

// ---------------------------------------------------------------------------
// open / close

static void restore_device(PCAudioDevice *d) {
    if (d->virt_changed && !d->phys_changed)
        setp(d->stream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, sizeof(d->orig_virt), &d->orig_virt);
    if (d->phys_changed) set_physical_format(d->stream, &d->orig_phys);
    if (d->mixing_changed) {
        UInt32 mix = 1;
        setp(d->dev, kAudioDevicePropertySupportsMixing, kAudioObjectPropertyScopeGlobal, sizeof(mix), &mix);
    }
    if (d->hogged) {
        pid_t pid = -1;
        setp(d->dev, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, sizeof(pid), &pid);
    }
    d->virt_changed = d->phys_changed = d->mixing_changed = d->hogged = 0;
}

PCAudioDevice *pcad_open(const char *uid, PCStreamType type, PCDeviceFormat *fmt, char *err, int errlen) {
    int bitstream = type == PC_STREAM_IEC61937_AC3 || type == PC_STREAM_IEC61937_DTS;   // one digital format carries both
    PCDeviceFormat req = *fmt;      // PC_STREAM_PCM_EXCLUSIVE: the request
    PCAudioDevice *d = calloc(1, sizeof(*d));
    memset(fmt, 0, sizeof(*fmt));
    d->dev = device_for_uid(uid);
    if (!d->dev) { snprintf(err, errlen, "Audio device not found"); goto fail; }
    d->stream = first_output_stream(d->dev);
    if (!d->stream) { snprintf(err, errlen, "Audio device has no output stream"); goto fail; }

    char name[256];
    device_name(d->dev, name, sizeof(name));
    UInt32 transport = device_transport(d->dev);
    getp(d->stream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, sizeof(d->orig_phys), &d->orig_phys);
    getp(d->stream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, sizeof(d->orig_virt), &d->orig_virt);

    if (type == PC_STREAM_PCM_EXCLUSIVE) {
        if (req.channels < 1 || req.channels > 32 || req.rate <= 0) { snprintf(err, errlen, "Bad PCM request"); goto fail; }
        if (open_pcm_exclusive(d, &req, fmt, name, transport, err, errlen) != 0) goto fail;
    } else if (bitstream) {
        // Never send IEC 61937 to something that would play it as PCM noise
        // (speakers, headphones, Bluetooth): only HDMI/DisplayPort or devices
        // that advertise a compressed format (optical / S/PDIF interfaces).
        int digital = transport == kAudioDeviceTransportTypeHDMI ||
                      transport == kAudioDeviceTransportTypeDisplayPort || device_has_compressed(d->dev);
        if (!digital) {
            snprintf(err, errlen, "'%s' is not a digital (HDMI/optical) output", name);
            goto fail;
        }
        // 1 + 2. exclusive access, no mixing -> the HAL passes our integer samples straight through
        if (take_exclusive(d, name, err, errlen) != 0) goto fail;
        // 3. choose a physical format that can carry IEC 61937
        UInt32 size = 0;
        AudioStreamRangedDescription *fmts = getp_alloc(d->stream, kAudioStreamPropertyAvailablePhysicalFormats, kAudioObjectPropertyScopeGlobal, &size);
        int best = -1, best_score = -1;
        for (UInt32 i = 0; fmts && i < size / sizeof(*fmts); i++) {
            int sc = score_bitstream_format(&fmts[i]);
            if (sc > best_score) { best_score = sc; best = (int)i; }
        }
        if (best < 0) {
            free(fmts);
            snprintf(err, errlen, "'%s' offers no 2-channel 48 kHz integer or AC3 format for bitstreaming", name);
            goto fail;
        }
        AudioStreamBasicDescription want = fmts[best].mFormat;
        free(fmts);
        want.mSampleRate = 48000.0;
        if (set_physical_format(d->stream, &want) != 0) {
            snprintf(err, errlen, "Could not switch '%s' to a bitstream-capable format", name);
            goto fail;
        }
        d->phys_changed = !same_format(&want, &d->orig_phys);
        Float64 sr = 48000.0;
        setp(d->dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, sizeof(sr), &sr);

        // 4. an integer virtual format identical to the physical one (no conversion)
        if (setp(d->stream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, sizeof(want), &want) == noErr)
            d->virt_changed = 1;
        AudioStreamBasicDescription virt;
        getp(d->stream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, sizeof(virt), &virt);
        fmt->channels = (int)virt.mChannelsPerFrame;
        fmt->bits = (int)virt.mBitsPerChannel;
        fmt->is_float = (virt.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
        fmt->rate = virt.mSampleRate;
        if (is_compressed_format(virt.mFormatID)) { fmt->is_float = 0; fmt->bits = 16; fmt->channels = 2; }
        if (fmt->channels != 2 || fmt->rate != 48000.0 ||
            !(fmt->bits == 16 || fmt->bits == 32) || (fmt->is_float && fmt->bits != 32)) {
            snprintf(err, errlen, "Unsupported stream format on '%s' (%u ch, %u bit, %.0f Hz)",
                     name, (unsigned)virt.mChannelsPerFrame, (unsigned)virt.mBitsPerChannel, virt.mSampleRate);
            goto fail;
        }
        char fcc[5];
        fourcc_str(want.mFormatID, fcc);
        snprintf(fmt->description, sizeof(fmt->description), "%s (%s), exclusive%s, physical %s %u-bit 48 kHz, IOProc %s%d",
                 name, transport_name(transport), d->hogged ? "" : " (hog mode unavailable!)",
                 is_compressed_format(want.mFormatID) ? fcc : "LPCM", (unsigned)want.mBitsPerChannel,
                 fmt->is_float ? "float" : "int", fmt->bits);
    } else {
        AudioStreamBasicDescription virt = d->orig_virt;
        if (virt.mFormatID != kAudioFormatLinearPCM || !(virt.mFormatFlags & kAudioFormatFlagIsFloat) ||
            virt.mBitsPerChannel != 32 || (virt.mFormatFlags & kAudioFormatFlagIsNonInterleaved)) {
            snprintf(err, errlen, "'%s' is not in a float PCM format (is another app bitstreaming to it?)", name);
            goto fail;
        }
        fmt->is_float = 1;
        fmt->bits = 32;
        fmt->channels = (int)virt.mChannelsPerFrame;
        fmt->rate = virt.mSampleRate;
        snprintf(fmt->description, sizeof(fmt->description), "%s (%s), shared, %d ch float %.0f Hz",
                 name, transport_name(transport), fmt->channels, fmt->rate);
    }
    if (type != PC_STREAM_PCM_EXCLUSIVE) d->bytes_per_frame = fmt->channels * fmt->bits / 8;

    UInt32 dlat = 0, slat = 0;
    getp(d->dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, sizeof(dlat), &dlat);
    getp(d->stream, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, sizeof(slat), &slat);
    fmt->latency = (double)(dlat + slat) / fmt->rate;

    if (AudioDeviceCreateIOProcID(d->dev, ioproc, d, &d->procid) != noErr) {
        snprintf(err, errlen, "AudioDeviceCreateIOProcID failed");
        goto fail;
    }
    return d;
fail:
    pcad_close(d);
    return NULL;
}

int pcad_start(PCAudioDevice *d, PCRenderCallback cb, void *ctx) {
    d->cb = cb;
    d->ctx = ctx;
    if (AudioDeviceStart(d->dev, d->procid) != noErr) return -1;
    d->running = 1;
    return 0;
}

// The macOS app watches kAudioHardwarePropertyDevices itself (PlayerModel.watchDevices).
void pcad_set_change_callback(PCDeviceChangeCallback cb, void *ctx) { (void)cb; (void)ctx; }
int pcad_lost(PCAudioDevice *d) { (void)d; return 0; }

void pcad_close(PCAudioDevice *d) {
    if (!d) return;
    if (d->procid) {
        if (d->running) AudioDeviceStop(d->dev, d->procid);
        AudioDeviceDestroyIOProcID(d->dev, d->procid);
    }
    if (d->dev) restore_device(d);
    free(d->tmp);
    free(d);
}

#endif
