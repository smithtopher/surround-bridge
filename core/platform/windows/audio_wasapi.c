// WASAPI audio device back-end (Windows), the equivalent of Kodi's AESinkWASAPI.
//
// Bitstream (PC_STREAM_IEC61937_*): exclusive mode, event-driven, with a
//   WAVEFORMATEXTENSIBLE 2 ch / 16-bit / 48 kHz whose SubFormat names the payload
//   (KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL or _DTS). If the endpoint refuses that
//   subtype (the sink doesn't advertise the codec) the open fails and the engine plays
//   decoded PCM; never a PCM-container fallback, which a sink may play as noise. Only
//   digital endpoints (HDMI/DisplayPort, S/PDIF) are ever used.
// PCM: shared mode in the engine's mix format (32-bit float), event-driven.
//
// Unlike CoreAudio, WASAPI changes no device state that has to be restored on close.
#if defined(_WIN32) && !defined(PC_STUB_BACKENDS)

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <propidl.h>
#include <avrt.h>

#include "../../pc_audio_device.h"
#include "../../pc_platform.h"

#include <stdio.h>
#include <string.h>

// GUIDs and property keys defined locally (no initguid.h / uuid.lib dependency).
static const CLSID pc_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID pc_IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID pc_IID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID pc_IID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const IID pc_IID_IAudioClock = {0xCD63314F, 0x3FBA, 0x4A1B, {0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7}};
static const IID pc_IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID pc_IID_IMMNotificationClient = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
#define PC_KSGUID(tag) {tag, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}}
static const GUID pc_SUBTYPE_PCM = PC_KSGUID(0x00000001);
static const GUID pc_SUBTYPE_IEEE_FLOAT = PC_KSGUID(0x00000003);
static const GUID pc_SUBTYPE_IEC61937_DTS = PC_KSGUID(0x00000008);
static const GUID pc_SUBTYPE_IEC61937_DOLBY_DIGITAL = PC_KSGUID(0x00000092);
static const PROPERTYKEY pc_PKEY_Device_FriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
static const PROPERTYKEY pc_PKEY_AudioEndpoint_FormFactor = {{0x1DA5D803, 0xD492, 0x4EDD, {0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E}}, 0};

// EndpointFormFactor values (mmdeviceapi.h)
enum { FF_SPEAKERS = 1, FF_HEADPHONES = 3, FF_HEADSET = 5, FF_DIGITAL_PASSTHROUGH = 7, FF_SPDIF = 8, FF_DISPLAY = 9 };

struct PCAudioDevice {
    IMMDevice *dev;
    IAudioClient *client;
    IAudioRenderClient *rc;
    IAudioClock *clock;
    HANDLE event, stop, thread;
    int exclusive;
    UINT32 buffer_frames;
    int bytes_per_frame;
    double rate;
    UINT64 clock_freq;
    int64_t written;                // frames handed to the device so far
    volatile LONG lost;             // device invalidated (unplugged / TV changed input)
    PCRenderCallback cb;
    void *ctx;
};

// ---------------------------------------------------------------------------
// helpers

// Keep the process MTA alive, so COM objects work from any thread (engine threads,
// the render thread, a UI thread) without per-thread CoInitialize bookkeeping.
static void ensure_mta(void) {
    static volatile LONG done;
    if (InterlockedCompareExchange(&done, 1, 0) == 0) {
        CO_MTA_USAGE_COOKIE cookie;
        CoIncrementMTAUsage(&cookie);
    }
}

#define RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

static void wide_to_utf8(const WCHAR *w, char *buf, int len) {
    buf[0] = 0;
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, len, NULL, NULL);
}

static IMMDeviceEnumerator *enumerator(void) {
    ensure_mta();
    IMMDeviceEnumerator *e = NULL;
    if (FAILED(CoCreateInstance(&pc_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &pc_IID_IMMDeviceEnumerator, (void **)&e)))
        return NULL;
    return e;
}

static void device_name(IMMDevice *d, char *buf, int len) {
    buf[0] = 0;
    IPropertyStore *ps = NULL;
    if (FAILED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &pc_PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
        wide_to_utf8(v.pwszVal, buf, len);
    PropVariantClear(&v);
    RELEASE(ps);
}

static int device_form_factor(IMMDevice *d) {
    int ff = -1;
    IPropertyStore *ps = NULL;
    if (FAILED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) return ff;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &pc_PKEY_AudioEndpoint_FormFactor, &v)) && v.vt == VT_UI4)
        ff = (int)v.ulVal;
    PropVariantClear(&v);
    RELEASE(ps);
    return ff;
}

static const char *form_factor_name(int ff) {
    switch (ff) {
    case FF_DISPLAY: return "HDMI/DisplayPort";
    case FF_SPDIF: return "S/PDIF";
    case FF_DIGITAL_PASSTHROUGH: return "digital";
    case FF_SPEAKERS: return "speakers";
    case FF_HEADPHONES: return "headphones";
    case FF_HEADSET: return "headset";
    default: return "other";
    }
}

static int is_digital_form_factor(int ff) {
    return ff == FF_DISPLAY || ff == FF_SPDIF || ff == FF_DIGITAL_PASSTHROUGH;
}

// 2 ch / 16-bit / 48 kHz, SubFormat = the IEC 61937 payload (or plain PCM).
static void iec_format(WAVEFORMATEXTENSIBLE *w, const GUID *subtype) {
    memset(w, 0, sizeof(*w));
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels = 2;
    w->Format.nSamplesPerSec = 48000;
    w->Format.wBitsPerSample = 16;
    w->Format.nBlockAlign = 4;
    w->Format.nAvgBytesPerSec = 48000 * 4;
    w->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w->Samples.wValidBitsPerSample = 16;
    w->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    w->SubFormat = *subtype;
}

static int activate(IMMDevice *d, IAudioClient **c) {
    *c = NULL;
    return SUCCEEDED(IMMDevice_Activate(d, &pc_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)c)) ? 0 : -1;
}

static int client_supports_iec_ac3(IAudioClient *c) {
    WAVEFORMATEXTENSIBLE w;
    iec_format(&w, &pc_SUBTYPE_IEC61937_DOLBY_DIGITAL);
    return IAudioClient_IsFormatSupported(c, AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, NULL) == S_OK;
}

static int is_float32(const WAVEFORMATEX *f) {
    if (f->wBitsPerSample != 32) return 0;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return 1;
    return f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           IsEqualGUID(&((const WAVEFORMATEXTENSIBLE *)f)->SubFormat, &pc_SUBTYPE_IEEE_FLOAT);
}

static const char *hr_message(HRESULT hr) {
    switch (hr) {
    case AUDCLNT_E_DEVICE_IN_USE: return "the device is in exclusive use by another application";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
        return "exclusive mode is disabled (Sound settings > device > Advanced: allow applications to take exclusive control)";
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return "format not supported";
    case AUDCLNT_E_DEVICE_INVALIDATED: return "the device was removed";
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED: return "endpoint creation failed";
    case E_OUTOFMEMORY: return "out of memory";
    default: return NULL;
    }
}

static void set_hr_err(char *err, int errlen, const char *what, const char *name, HRESULT hr) {
    const char *m = hr_message(hr);
    if (m) snprintf(err, errlen, "%s on '%s' failed: %s", what, name, m);
    else snprintf(err, errlen, "%s on '%s' failed (0x%08lx)", what, name, (unsigned long)hr);
}

static IMMDevice *device_for_uid(IMMDeviceEnumerator *e, const char *uid) {
    IMMDevice *d = NULL;
    if (!uid || !uid[0]) {
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, eRender, eConsole, &d))) return NULL;
        return d;
    }
    WCHAR w[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, uid, -1, w, 512)) return NULL;
    if (FAILED(IMMDeviceEnumerator_GetDevice(e, w, &d))) return NULL;
    DWORD state = 0;
    if (FAILED(IMMDevice_GetState(d, &state)) || state != DEVICE_STATE_ACTIVE) { RELEASE(d); return NULL; }
    return d;
}

// ---------------------------------------------------------------------------
// list

int pcad_list(PCDevice *out, int max) {
    IMMDeviceEnumerator *e = enumerator();
    if (!e) return 0;
    char def_id[256] = "";
    IMMDevice *def = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, eRender, eConsole, &def))) {
        WCHAR *id = NULL;
        if (SUCCEEDED(IMMDevice_GetId(def, &id))) { wide_to_utf8(id, def_id, sizeof(def_id)); CoTaskMemFree(id); }
        RELEASE(def);
    }
    IMMDeviceCollection *col = NULL;
    int n = 0;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(e, eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT count = 0;
        IMMDeviceCollection_GetCount(col, &count);
        for (UINT i = 0; i < count && n < max; i++) {
            IMMDevice *d = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
            PCDevice *o = &out[n];
            memset(o, 0, sizeof(*o));
            WCHAR *id = NULL;
            if (SUCCEEDED(IMMDevice_GetId(d, &id))) { wide_to_utf8(id, o->uid, sizeof(o->uid)); CoTaskMemFree(id); }
            device_name(d, o->name, sizeof(o->name));
            int ff = device_form_factor(d);
            IAudioClient *c = NULL;
            if (activate(d, &c) == 0) {
                WAVEFORMATEX *mix = NULL;
                if (SUCCEEDED(IAudioClient_GetMixFormat(c, &mix))) { o->output_channels = mix->nChannels; CoTaskMemFree(mix); }
                o->has_compressed_formats = client_supports_iec_ac3(c);
                RELEASE(c);
            }
            o->is_hdmi = ff == FF_DISPLAY;
            o->is_digital = is_digital_form_factor(ff) || o->has_compressed_formats;
            o->is_default = o->uid[0] && strcmp(o->uid, def_id) == 0;
            RELEASE(d);
            n++;
        }
        RELEASE(col);
    }
    RELEASE(e);
    return n;
}

// ---------------------------------------------------------------------------
// device change notifications (IMMNotificationClient, a static C COM object)

static SRWLOCK g_reg_lock = SRWLOCK_INIT;   // serialises (un)registration
static SRWLOCK g_cb_lock = SRWLOCK_INIT;    // guards g_cb / g_ctx; held shared while calling out
static PCDeviceChangeCallback g_cb;
static void *g_ctx;
static IMMDeviceEnumerator *g_notify_enum;

static void fire_change(void) {
    AcquireSRWLockShared(&g_cb_lock);
    if (g_cb) g_cb(g_ctx);
    ReleaseSRWLockShared(&g_cb_lock);
}

static HRESULT STDMETHODCALLTYPE nc_query_interface(IMMNotificationClient *This, REFIID riid, void **ppv) {
    if (IsEqualIID(riid, &pc_IID_IUnknown) || IsEqualIID(riid, &pc_IID_IMMNotificationClient)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE nc_add_ref(IMMNotificationClient *This) { (void)This; return 1; }   // static object
static ULONG STDMETHODCALLTYPE nc_release(IMMNotificationClient *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE nc_state_changed(IMMNotificationClient *This, LPCWSTR id, DWORD state) {
    (void)This; (void)id; (void)state;
    fire_change();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_added(IMMNotificationClient *This, LPCWSTR id) { (void)This; (void)id; fire_change(); return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_removed(IMMNotificationClient *This, LPCWSTR id) { (void)This; (void)id; fire_change(); return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_default_changed(IMMNotificationClient *This, EDataFlow flow, ERole role, LPCWSTR id) {
    (void)This; (void)id;
    if (flow == eRender && role == eConsole) fire_change();   // once, not per role
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_property_changed(IMMNotificationClient *This, LPCWSTR id, const PROPERTYKEY key) {
    (void)This; (void)id; (void)key;   // frequent and noisy; format changes show up as lost streams
    return S_OK;
}
static IMMNotificationClientVtbl g_nc_vtbl = {
    nc_query_interface, nc_add_ref, nc_release, nc_state_changed, nc_added, nc_removed,
    nc_default_changed, nc_property_changed,
};
static IMMNotificationClient g_nc = { &g_nc_vtbl };

void pcad_set_change_callback(PCDeviceChangeCallback cb, void *ctx) {
    AcquireSRWLockExclusive(&g_reg_lock);
    if (cb && !g_notify_enum) {
        g_notify_enum = enumerator();
        if (g_notify_enum && FAILED(IMMDeviceEnumerator_RegisterEndpointNotificationCallback(g_notify_enum, &g_nc)))
            RELEASE(g_notify_enum);
    }
    AcquireSRWLockExclusive(&g_cb_lock);   // after this no call to the old callback is running
    g_cb = cb;
    g_ctx = ctx;
    ReleaseSRWLockExclusive(&g_cb_lock);
    if (!cb && g_notify_enum) {
        IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(g_notify_enum, &g_nc);
        RELEASE(g_notify_enum);
    }
    ReleaseSRWLockExclusive(&g_reg_lock);
}

int pcad_lost(PCAudioDevice *d) { return d && InterlockedCompareExchange(&d->lost, 0, 0) != 0; }

static int is_invalidated(HRESULT hr) {
#ifdef AUDCLNT_E_RESOURCES_INVALIDATED
    if (hr == AUDCLNT_E_RESOURCES_INVALIDATED) return 1;
#endif
    return hr == AUDCLNT_E_DEVICE_INVALIDATED;
}

// The stream is dead for good (device removed, TV changed input, format changed).
static void mark_lost(PCAudioDevice *d) {
    InterlockedExchange(&d->lost, 1);
    fire_change();
}

// ---------------------------------------------------------------------------
// render thread

// pc_now_ns() time at which the next frame we write will be played.
// IAudioClock::GetPosition gives the position being played and the QPC time (in 100 ns
// units) at which it was sampled; pc_now_ns() is QPC in ns, the same timebase.
static int64_t next_present_ns(PCAudioDevice *d) {
    UINT64 pos = 0, qpc = 0;
    if (d->clock && d->clock_freq && SUCCEEDED(IAudioClock_GetPosition(d->clock, &pos, &qpc)) && qpc) {
        double played = (double)pos * d->rate / (double)d->clock_freq;
        return (int64_t)qpc * 100 + (int64_t)(((double)d->written - played) / d->rate * 1e9);
    }
    UINT32 pad = 0;
    if (!d->exclusive) IAudioClient_GetCurrentPadding(d->client, &pad);
    return pc_now_ns() + (int64_t)((double)(d->exclusive ? d->buffer_frames : pad) / d->rate * 1e9);
}

static DWORD WINAPI render_thread(LPVOID arg) {
    PCAudioDevice *d = arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    HANDLE waits[2] = { d->stop, d->event };
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (w == WAIT_OBJECT_0) break;
        if (w != WAIT_OBJECT_0 + 1) {
            // no event for 2 s: an unplugged endpoint can just stop signalling; probe it
            UINT32 pad = 0;
            if (is_invalidated(IAudioClient_GetCurrentPadding(d->client, &pad))) { mark_lost(d); break; }
            continue;
        }
        UINT32 n = d->buffer_frames;
        if (!d->exclusive) {
            UINT32 pad = 0;
            HRESULT hr = IAudioClient_GetCurrentPadding(d->client, &pad);
            if (is_invalidated(hr)) { mark_lost(d); break; }
            if (FAILED(hr)) continue;
            n = d->buffer_frames - pad;
        }
        if (n == 0) continue;
        BYTE *buf = NULL;
        HRESULT hr = IAudioRenderClient_GetBuffer(d->rc, n, &buf);
        if (is_invalidated(hr)) { mark_lost(d); break; }
        if (FAILED(hr)) continue;
        d->cb(d->ctx, buf, (int)n, next_present_ns(d));
        IAudioRenderClient_ReleaseBuffer(d->rc, n, 0);
        d->written += n;
    }
    if (task) AvRevertMmThreadCharacteristics(task);
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// open / close

// Exclusive, event-driven, one device period per buffer. Handles the documented
// AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED dance (re-activate with the aligned period).
static HRESULT init_exclusive(PCAudioDevice *d, const WAVEFORMATEX *f) {
    REFERENCE_TIME def = 0, min = 0;
    IAudioClient_GetDevicePeriod(d->client, &def, &min);
    if (def <= 0) def = 100000;   // 10 ms
    HRESULT hr = IAudioClient_Initialize(d->client, AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         def, def, f, NULL);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 frames = 0;
        IAudioClient_GetBufferSize(d->client, &frames);
        RELEASE(d->client);
        if (activate(d->dev, &d->client) != 0) return E_FAIL;
        REFERENCE_TIME period = (REFERENCE_TIME)(1e7 * frames / f->nSamplesPerSec + 0.5);
        hr = IAudioClient_Initialize(d->client, AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     period, period, f, NULL);
    }
    return hr;
}

// 5.1/5.0 side <-> back: drivers often accept only one of the two for the same 6 channels
static uint32_t alt_surround_mask(uint32_t m) {
    const uint32_t side = SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT, back = SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    if ((m & side) == side && !(m & back)) return (m & ~side) | back;
    if ((m & back) == back && !(m & side)) return (m & ~back) | side;
    return 0;
}

// Exclusive multichannel PCM: first format the endpoint accepts, trying the source rate
// then 48 kHz, the source speaker mask then its side/back twin, and 24-in-32, 32, 16-bit
// integer, then float.
static int find_pcm_format(IAudioClient *c, int channels, int rate, uint32_t mask, WAVEFORMATEXTENSIBLE *out) {
    int rates[2] = { rate, 48000 };
    uint32_t masks[2] = { mask, alt_surround_mask(mask) };
    static const struct { int bits, valid, is_float; } sf[] = { { 32, 24, 0 }, { 32, 32, 0 }, { 16, 16, 0 }, { 32, 32, 1 } };
    for (int ri = 0; ri < 2; ri++) {
        if (ri == 1 && rates[1] == rates[0]) break;
        for (int mi = 0; mi < 2; mi++) {
            if (mi == 1 && !masks[1]) break;
            for (int s = 0; s < 4; s++) {
                WAVEFORMATEXTENSIBLE *w = out;
                memset(w, 0, sizeof(*w));
                w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                w->Format.nChannels = (WORD)channels;
                w->Format.nSamplesPerSec = (DWORD)rates[ri];
                w->Format.wBitsPerSample = (WORD)sf[s].bits;
                w->Format.nBlockAlign = (WORD)(channels * sf[s].bits / 8);
                w->Format.nAvgBytesPerSec = w->Format.nSamplesPerSec * w->Format.nBlockAlign;
                w->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                w->Samples.wValidBitsPerSample = (WORD)sf[s].valid;
                w->dwChannelMask = masks[mi];
                w->SubFormat = sf[s].is_float ? pc_SUBTYPE_IEEE_FLOAT : pc_SUBTYPE_PCM;
                if (IAudioClient_IsFormatSupported(c, AUDCLNT_SHAREMODE_EXCLUSIVE, &w->Format, NULL) == S_OK) return 0;
            }
        }
    }
    return -1;
}

PCAudioDevice *pcad_open(const char *uid, PCStreamType type, PCDeviceFormat *fmt, char *err, int errlen) {
    int bitstream = type == PC_STREAM_IEC61937_AC3 || type == PC_STREAM_IEC61937_DTS;
    PCDeviceFormat req = *fmt;   // PC_STREAM_PCM_EXCLUSIVE: the wanted channels / rate / mask
    memset(fmt, 0, sizeof(*fmt));
    PCAudioDevice *d = calloc(1, sizeof(*d));
    char name[256] = "?";
    HRESULT hr;
    IMMDeviceEnumerator *e = enumerator();
    if (!e) { snprintf(err, errlen, "Cannot create the audio device enumerator"); goto fail; }
    d->dev = device_for_uid(e, uid);
    RELEASE(e);
    if (!d->dev) { snprintf(err, errlen, "Audio device not found"); goto fail; }
    device_name(d->dev, name, sizeof(name));
    int ff = device_form_factor(d->dev);
    if (activate(d->dev, &d->client) != 0) { snprintf(err, errlen, "Cannot activate '%s'", name); goto fail; }

    if (bitstream) {
        // Never send IEC 61937 to something that would play it as PCM noise.
        if (!is_digital_form_factor(ff) && !client_supports_iec_ac3(d->client)) {
            snprintf(err, errlen, "'%s' is not a digital (HDMI/optical) output", name);
            goto fail;
        }
        int dts = type == PC_STREAM_IEC61937_DTS;
        WAVEFORMATEXTENSIBLE w;
        iec_format(&w, dts ? &pc_SUBTYPE_IEC61937_DTS : &pc_SUBTYPE_IEC61937_DOLBY_DIGITAL);
        const char *sub = dts ? "IEC 61937 DTS" : "IEC 61937 Dolby Digital";
        // No fallback to a plain PCM container: Windows only refuses the IEC subtype when
        // the sink (HDMI EDID) doesn't list the codec, and a sink that can't decode it may
        // play the bursts as PCM noise. The engine falls back to decoded PCM instead.
        if (IAudioClient_IsFormatSupported(d->client, AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, NULL) != S_OK) {
            snprintf(err, errlen, "'%s' does not accept %s (the TV/receiver doesn't advertise %s)", name, sub,
                     dts ? "DTS" : "Dolby Digital");
            goto fail;
        }
        d->exclusive = 1;
        hr = init_exclusive(d, &w.Format);
        if (FAILED(hr)) { set_hr_err(err, errlen, "Exclusive 48 kHz bitstream output", name, hr); goto fail; }
        fmt->channels = 2;
        fmt->bits = 16;
        fmt->is_float = 0;
        fmt->rate = 48000.0;
        snprintf(fmt->description, sizeof(fmt->description), "%s (%s), exclusive, %s 2ch 16-bit 48 kHz",
                 name, form_factor_name(ff), sub);
    } else if (type == PC_STREAM_PCM_EXCLUSIVE) {
        WAVEFORMATEXTENSIBLE w;
        if (req.channels < 1 || req.channels > 8 ||
            find_pcm_format(d->client, req.channels, (int)req.rate, req.channel_mask, &w) != 0) {
            snprintf(err, errlen, "'%s' doesn't accept %d-channel PCM in exclusive mode (the TV/receiver advertises fewer channels)",
                     name, req.channels);
            goto fail;
        }
        d->exclusive = 1;
        hr = init_exclusive(d, &w.Format);
        if (FAILED(hr)) { set_hr_err(err, errlen, "Exclusive multichannel PCM output", name, hr); goto fail; }
        fmt->channels = w.Format.nChannels;
        fmt->bits = w.Format.wBitsPerSample;
        fmt->is_float = IsEqualGUID(&w.SubFormat, &pc_SUBTYPE_IEEE_FLOAT);
        fmt->rate = w.Format.nSamplesPerSec;
        fmt->channel_mask = w.dwChannelMask;
        snprintf(fmt->description, sizeof(fmt->description), "%s (%s), exclusive, %d ch %s %d-bit%s %.0f Hz", name,
                 form_factor_name(ff), fmt->channels, fmt->is_float ? "float" : "int", w.Samples.wValidBitsPerSample,
                 fmt->bits == 32 && w.Samples.wValidBitsPerSample == 24 ? " (in 32)" : "", fmt->rate);
    } else {
        WAVEFORMATEX *mix = NULL;
        hr = IAudioClient_GetMixFormat(d->client, &mix);
        if (FAILED(hr)) { set_hr_err(err, errlen, "GetMixFormat", name, hr); goto fail; }
        WAVEFORMATEXTENSIBLE w;
        const WAVEFORMATEX *use = mix;
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (!is_float32(mix)) {
            // ask the engine's mixer to convert from float for us
            memset(&w, 0, sizeof(w));
            w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            w.Format.nChannels = mix->nChannels;
            w.Format.nSamplesPerSec = mix->nSamplesPerSec;
            w.Format.wBitsPerSample = 32;
            w.Format.nBlockAlign = (WORD)(mix->nChannels * 4);
            w.Format.nAvgBytesPerSec = mix->nSamplesPerSec * w.Format.nBlockAlign;
            w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            w.Samples.wValidBitsPerSample = 32;
            w.dwChannelMask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? ((WAVEFORMATEXTENSIBLE *)mix)->dwChannelMask : 0;
            w.SubFormat = pc_SUBTYPE_IEEE_FLOAT;
            use = &w.Format;
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        }
        hr = IAudioClient_Initialize(d->client, AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, use, NULL);
        fmt->channels = use->nChannels;
        fmt->bits = 32;
        fmt->is_float = 1;
        fmt->rate = use->nSamplesPerSec;
        CoTaskMemFree(mix);
        if (FAILED(hr)) { set_hr_err(err, errlen, "Shared PCM output", name, hr); goto fail; }
        snprintf(fmt->description, sizeof(fmt->description), "%s (%s), shared, %d ch float %.0f Hz",
                 name, form_factor_name(ff), fmt->channels, fmt->rate);
    }
    d->rate = fmt->rate;
    d->bytes_per_frame = fmt->channels * fmt->bits / 8;
    // IAudioClock reports the position actually being played, so the buffer queue is
    // already inside present_ns; any further delay (TV/amp decoding) is the user's
    // "Audio latency" setting.
    fmt->latency = 0;

    if (FAILED(IAudioClient_GetBufferSize(d->client, &d->buffer_frames)) || !d->buffer_frames) {
        snprintf(err, errlen, "GetBufferSize failed on '%s'", name);
        goto fail;
    }
    d->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    d->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!d->event || !d->stop || FAILED(IAudioClient_SetEventHandle(d->client, d->event))) {
        snprintf(err, errlen, "SetEventHandle failed on '%s'", name);
        goto fail;
    }
    hr = IAudioClient_GetService(d->client, &pc_IID_IAudioRenderClient, (void **)&d->rc);
    if (FAILED(hr)) { set_hr_err(err, errlen, "GetService(IAudioRenderClient)", name, hr); goto fail; }
    if (SUCCEEDED(IAudioClient_GetService(d->client, &pc_IID_IAudioClock, (void **)&d->clock)))
        IAudioClock_GetFrequency(d->clock, &d->clock_freq);
    size_t dl = strlen(fmt->description);
    snprintf(fmt->description + dl, sizeof(fmt->description) - dl, ", buffer %.1f ms",
             d->buffer_frames * 1000.0 / d->rate);
    return d;
fail:
    pcad_close(d);
    return NULL;
}

int pcad_start(PCAudioDevice *d, PCRenderCallback cb, void *ctx) {
    d->cb = cb;
    d->ctx = ctx;
    // pre-roll one buffer of silence (WASAPI wants data before Start)
    BYTE *buf = NULL;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(d->rc, d->buffer_frames, &buf))) {
        IAudioRenderClient_ReleaseBuffer(d->rc, d->buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
        d->written += d->buffer_frames;
    }
    d->thread = CreateThread(NULL, 0, render_thread, d, 0, NULL);
    if (!d->thread) return -1;
    if (FAILED(IAudioClient_Start(d->client))) {
        SetEvent(d->stop);
        WaitForSingleObject(d->thread, INFINITE);
        CloseHandle(d->thread);
        d->thread = NULL;
        return -1;
    }
    return 0;
}

void pcad_close(PCAudioDevice *d) {
    if (!d) return;
    if (d->thread) {
        SetEvent(d->stop);
        WaitForSingleObject(d->thread, INFINITE);
        CloseHandle(d->thread);
    }
    if (d->client) IAudioClient_Stop(d->client);
    RELEASE(d->clock);
    RELEASE(d->rc);
    RELEASE(d->client);
    RELEASE(d->dev);
    if (d->event) CloseHandle(d->event);
    if (d->stop) CloseHandle(d->stop);
    free(d);
}

#endif
