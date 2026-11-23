// WASAPI capture back-end (Windows): reads what apps play into the virtual device.
//   - capture endpoints (e.g. VB-Audio's "CABLE Output"): shared-mode capture, uid = endpoint id
//   - render endpoints, loopback ("loopback:<endpoint id>"): what is mixed for that output
//     (e.g. "CABLE Input", or any device the user sets as the default output)
// Shared mode, the endpoint's mix format (32-bit float), polled every few ms on our own thread.
//
// Loopback only delivers packets while something plays on the endpoint. To get a continuous
// stream (the drift control needs one, as BlackHole gives on macOS) we also render silence into
// the loopbacked endpoint: zeros mixed in change nothing, but keep its engine running.
#if defined(_WIN32) && !defined(PC_STUB_BACKENDS)

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <propidl.h>
#include <avrt.h>
#include <mmsystem.h>   // timeBeginPeriod

#include "../../pc_capture.h"
#include "../../pc_platform.h"

#include <stdio.h>
#include <string.h>

#define CAP_BUFFER_HNS 1000000         // 100 ms shared-mode buffer: room for a late wake-up (no added delay:
                                       // we take packets as they arrive)
#define CAP_POLL_MS 3                  // poll interval (packets are 10 ms in shared mode)
// Waits and Sleep() only have the system timer's resolution, 15.6 ms by default, and since Windows 10
// 2004 a process's timeBeginPeriod only applies to itself. At 15.6 ms the poll (and the worker's 1 ms
// sleep in bridge.c) fell so far behind that the output ran short about twice a second: a 32 ms
// silence burst each time, i.e. stutter. sbridge set 1 ms itself; the app didn't. So the capture
// sets it while it runs.
#define LOOPBACK_PREFIX "loopback:"

#define KSGUID_(x) {x, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}}
static const CLSID cap_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID cap_IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID cap_IID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID cap_IID_IAudioCaptureClient = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const IID cap_IID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const GUID cap_SUBTYPE_IEEE_FLOAT = KSGUID_(0x00000003);
static const PROPERTYKEY cap_PKEY_Device_FriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

#define RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

struct PCCapture {
    IMMDevice *dev;
    IAudioClient *client;
    IAudioCaptureClient *cc;
    IAudioClient *sil_client;        // loopback: silent render stream keeping the endpoint running
    IAudioRenderClient *sil_rc;
    UINT32 sil_frames;
    HANDLE thread, stop;
    int loopback, channels;
    double rate;
    float *zeros;                    // AUDCLNT_BUFFERFLAGS_SILENT packets
    int zeros_frames;
    PCCaptureCallback cb;
    void *ctx;
    int timer_1ms;                   // timeBeginPeriod(1) held (see CAP_POLL_MS)
};

// Same as audio_wasapi.c: keep the process MTA alive so COM works from any thread.
static void ensure_mta(void) {
    static volatile LONG done;
    if (InterlockedCompareExchange(&done, 1, 0) == 0) {
        CO_MTA_USAGE_COOKIE cookie;
        CoIncrementMTAUsage(&cookie);
    }
}

static void wide_to_utf8(const WCHAR *w, char *buf, int len) {
    buf[0] = 0;
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, len, NULL, NULL);
}

static IMMDeviceEnumerator *enumerator(void) {
    ensure_mta();
    IMMDeviceEnumerator *e = NULL;
    if (FAILED(CoCreateInstance(&cap_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &cap_IID_IMMDeviceEnumerator, (void **)&e)))
        return NULL;
    return e;
}

static void device_name(IMMDevice *d, char *buf, int len) {
    buf[0] = 0;
    IPropertyStore *ps = NULL;
    if (FAILED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &cap_PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
        wide_to_utf8(v.pwszVal, buf, len);
    PropVariantClear(&v);
    RELEASE(ps);
}

static int is_float32(const WAVEFORMATEX *f) {
    if (f->wBitsPerSample != 32) return 0;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return 1;
    return f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           IsEqualGUID(&((const WAVEFORMATEXTENSIBLE *)f)->SubFormat, &cap_SUBTYPE_IEEE_FLOAT);
}

// Known virtual cables (flagged like CoreAudio's virtual transport type on macOS).
static int looks_virtual(const char *name) {
    static const char *const marks[] = { "VB-Audio", "CABLE", "Virtual", "VoiceMeeter" };
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
        if (strstr(name, marks[i])) return 1;
    return 0;
}

static int mix_channels(IMMDevice *d) {
    IAudioClient *c = NULL;
    int ch = 0;
    if (SUCCEEDED(IMMDevice_Activate(d, &cap_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&c))) {
        WAVEFORMATEX *mix = NULL;
        if (SUCCEEDED(IAudioClient_GetMixFormat(c, &mix))) { ch = mix->nChannels; CoTaskMemFree(mix); }
        RELEASE(c);
    }
    return ch;
}

static int list_flow(IMMDeviceEnumerator *e, EDataFlow flow, PCDevice *out, int n, int max) {
    IMMDeviceCollection *col = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(e, flow, DEVICE_STATE_ACTIVE, &col))) return n;
    UINT count = 0;
    IMMDeviceCollection_GetCount(col, &count);
    for (UINT i = 0; i < count && n < max; i++) {
        IMMDevice *d = NULL;
        if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
        PCDevice *o = &out[n];
        memset(o, 0, sizeof(*o));
        WCHAR *id = NULL;
        char uid[200] = "", name[200] = "";
        if (SUCCEEDED(IMMDevice_GetId(d, &id))) { wide_to_utf8(id, uid, sizeof(uid)); CoTaskMemFree(id); }
        device_name(d, name, sizeof(name));
        if (flow == eRender) {
            snprintf(o->uid, sizeof(o->uid), LOOPBACK_PREFIX "%s", uid);
            snprintf(o->name, sizeof(o->name), "%s (loopback)", name);
        } else {
            snprintf(o->uid, sizeof(o->uid), "%s", uid);
            snprintf(o->name, sizeof(o->name), "%s", name);
        }
        o->output_channels = mix_channels(d);
        o->is_digital = looks_virtual(name);   // here: "virtual device", as on macOS
        RELEASE(d);
        n++;
    }
    RELEASE(col);
    return n;
}

// Capture endpoints first (a virtual cable's capture side is the usual choice), then loopbacks.
int pccap_list(PCDevice *out, int max) {
    IMMDeviceEnumerator *e = enumerator();
    if (!e) return 0;
    int n = list_flow(e, eCapture, out, 0, max);
    n = list_flow(e, eRender, out, n, max);
    RELEASE(e);
    return n;
}

static void hr_err(char *err, int errlen, const char *what, HRESULT hr) {
    if (hr == AUDCLNT_E_DEVICE_IN_USE) snprintf(err, errlen, "%s failed: the device is in exclusive use", what);
    else if (hr == AUDCLNT_E_DEVICE_INVALIDATED) snprintf(err, errlen, "%s failed: the device was removed", what);
    else snprintf(err, errlen, "%s failed (0x%08lx)", what, (unsigned long)hr);
}

// QPC position (100 ns units) -> pc_now_ns timebase (QPC in ns)
static int64_t qpc100_to_ns(UINT64 q) { return (int64_t)q * 100; }

static void feed_silence(PCCapture *c) {
    if (!c->sil_rc) return;
    UINT32 pad = 0;
    if (FAILED(IAudioClient_GetCurrentPadding(c->sil_client, &pad))) return;
    UINT32 n = c->sil_frames - pad;
    if (n == 0) return;
    BYTE *p = NULL;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(c->sil_rc, n, &p)))
        IAudioRenderClient_ReleaseBuffer(c->sil_rc, n, AUDCLNT_BUFFERFLAGS_SILENT);
}

static DWORD WINAPI capture_thread(LPVOID arg) {
    PCCapture *c = arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    while (WaitForSingleObject(c->stop, CAP_POLL_MS) == WAIT_TIMEOUT) {
        feed_silence(c);
        for (;;) {
            UINT32 next = 0;
            HRESULT hr = IAudioCaptureClient_GetNextPacketSize(c->cc, &next);
            if (FAILED(hr) || next == 0) break;
            BYTE *data = NULL;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devpos = 0, qpc = 0;
            hr = IAudioCaptureClient_GetBuffer(c->cc, &data, &frames, &flags, &devpos, &qpc);
            if (FAILED(hr)) break;
            if (frames > 0) {
                const float *src = (const float *)data;
                int64_t ns = qpc ? qpc100_to_ns(qpc) : pc_now_ns();
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {   // no data in the buffer: zeros, in pieces
                    for (int left = (int)frames; left > 0; left -= c->zeros_frames)
                        c->cb(c->ctx, c->zeros, left < c->zeros_frames ? left : c->zeros_frames, ns);
                } else {
                    c->cb(c->ctx, src, (int)frames, ns);
                }
            }
            IAudioCaptureClient_ReleaseBuffer(c->cc, frames);
        }
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
    return 0;
}

// The loopbacked endpoint's own render stream of silence (see the top of the file).
static void open_silence(PCCapture *c) {
    if (FAILED(IMMDevice_Activate(c->dev, &cap_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&c->sil_client))) return;
    WAVEFORMATEX *mix = NULL;
    if (FAILED(IAudioClient_GetMixFormat(c->sil_client, &mix))) { RELEASE(c->sil_client); return; }
    HRESULT hr = IAudioClient_Initialize(c->sil_client, AUDCLNT_SHAREMODE_SHARED, 0, CAP_BUFFER_HNS * 5, 0, mix, NULL);
    CoTaskMemFree(mix);
    if (FAILED(hr) || FAILED(IAudioClient_GetBufferSize(c->sil_client, &c->sil_frames)) ||
        FAILED(IAudioClient_GetService(c->sil_client, &cap_IID_IAudioRenderClient, (void **)&c->sil_rc))) {
        RELEASE(c->sil_rc);
        RELEASE(c->sil_client);
        return;
    }
    feed_silence(c);
}

PCCapture *pccap_open(const char *uid, PCCaptureFormat *fmt, char *err, int errlen) {
    memset(fmt, 0, sizeof(*fmt));
    PCCapture *c = calloc(1, sizeof(*c));
    IMMDeviceEnumerator *e = enumerator();
    WAVEFORMATEX *mix = NULL;
    if (!c || !e) { snprintf(err, errlen, "Audio device enumerator unavailable"); goto fail; }
    if (!uid || !uid[0]) { snprintf(err, errlen, "No input device given"); goto fail; }
    c->loopback = strncmp(uid, LOOPBACK_PREFIX, strlen(LOOPBACK_PREFIX)) == 0;
    const char *id = c->loopback ? uid + strlen(LOOPBACK_PREFIX) : uid;
    WCHAR w[512];
    DWORD state = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, id, -1, w, 512) || FAILED(IMMDeviceEnumerator_GetDevice(e, w, &c->dev)) ||
        FAILED(IMMDevice_GetState(c->dev, &state)) || state != DEVICE_STATE_ACTIVE) {
        snprintf(err, errlen, "Input device not found");
        goto fail;
    }
    char name[200] = "";
    device_name(c->dev, name, sizeof(name));
    HRESULT hr = IMMDevice_Activate(c->dev, &cap_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&c->client);
    if (FAILED(hr)) { hr_err(err, errlen, "Activating the input", hr); goto fail; }
    hr = IAudioClient_GetMixFormat(c->client, &mix);
    if (FAILED(hr)) { hr_err(err, errlen, "Reading the input format", hr); goto fail; }
    if (!is_float32(mix)) { snprintf(err, errlen, "Input device's mix format is not 32-bit float"); goto fail; }
    c->channels = mix->nChannels;
    c->rate = mix->nSamplesPerSec;
    hr = IAudioClient_Initialize(c->client, AUDCLNT_SHAREMODE_SHARED, c->loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                                 CAP_BUFFER_HNS, 0, mix, NULL);
    if (FAILED(hr)) { hr_err(err, errlen, "Opening the input", hr); goto fail; }
    hr = IAudioClient_GetService(c->client, &cap_IID_IAudioCaptureClient, (void **)&c->cc);
    if (FAILED(hr)) { hr_err(err, errlen, "Opening the capture client", hr); goto fail; }
    if (c->loopback) open_silence(c);

    REFERENCE_TIME lat = 0, period = 0;
    IAudioClient_GetStreamLatency(c->client, &lat);
    IAudioClient_GetDevicePeriod(c->client, &period, NULL);
    c->zeros_frames = (int)(c->rate * 0.1);   // 100 ms, more than any shared-mode packet
    c->zeros = calloc((size_t)c->zeros_frames * (size_t)c->channels, sizeof(float));
    c->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!c->zeros || !c->stop) { snprintf(err, errlen, "Out of memory"); goto fail; }

    fmt->channels = c->channels;
    fmt->rate = c->rate;
    // a packet waits up to one device period, plus our poll interval
    fmt->latency = (double)(lat + period) / 1e7 + CAP_POLL_MS / 2000.0;
    snprintf(fmt->description, sizeof(fmt->description), "%s%s, %d ch float %.0f Hz, shared%s", name,
             c->loopback ? " (loopback)" : "", c->channels, c->rate,
             c->loopback && !c->sil_rc ? " (no silence stream: gaps between sounds)" : "");
    CoTaskMemFree(mix);
    RELEASE(e);
    return c;
fail:
    if (mix) CoTaskMemFree(mix);
    RELEASE(e);
    pccap_close(c);
    return NULL;
}

int pccap_start(PCCapture *c, PCCaptureCallback cb, void *ctx) {
    c->cb = cb;
    c->ctx = ctx;
    if (c->sil_client && FAILED(IAudioClient_Start(c->sil_client))) { RELEASE(c->sil_rc); RELEASE(c->sil_client); }
    if (FAILED(IAudioClient_Start(c->client))) return -1;
    timeBeginPeriod(1);
    c->timer_1ms = 1;
    c->thread = CreateThread(NULL, 0, capture_thread, c, 0, NULL);
    if (!c->thread) { IAudioClient_Stop(c->client); return -1; }
    return 0;
}

void pccap_close(PCCapture *c) {
    if (!c) return;
    if (c->thread) {
        SetEvent(c->stop);
        WaitForSingleObject(c->thread, INFINITE);
        CloseHandle(c->thread);
    }
    if (c->client) IAudioClient_Stop(c->client);
    if (c->sil_client) IAudioClient_Stop(c->sil_client);
    RELEASE(c->cc);
    RELEASE(c->client);
    RELEASE(c->sil_rc);
    RELEASE(c->sil_client);
    RELEASE(c->dev);
    if (c->stop) CloseHandle(c->stop);
    if (c->timer_1ms) timeEndPeriod(1);
    free(c->zeros);
    free(c);
}

#endif
