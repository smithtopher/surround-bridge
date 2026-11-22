// Default output endpoint get/set (see include/bridge_win.h).
// Setting it has no public API: IPolicyConfig (Windows 7+, used by the Sound control panel and
// by every "audio switcher" tool) is undocumented but stable. Only SetDefaultEndpoint is called.
#if defined(_WIN32) && !defined(PC_STUB_BACKENDS)

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>

#include "bridge_win.h"

#include <stdio.h>

static const CLSID do_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID do_IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const CLSID do_CLSID_PolicyConfigClient = {0x870AF99C, 0x171D, 0x4F9E, {0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9}};
static const IID do_IID_IPolicyConfig = {0xF8679F50, 0x850A, 0x41CF, {0x9C, 0x72, 0x43, 0x0F, 0x29, 0x02, 0x90, 0xC8}};

// IPolicyConfig's vtable: only the slot order matters up to SetDefaultEndpoint.
typedef struct IPolicyConfig IPolicyConfig;
typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(IPolicyConfig *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(IPolicyConfig *);
    ULONG(STDMETHODCALLTYPE *Release)(IPolicyConfig *);
    void *GetMixFormat, *GetDeviceFormat, *ResetDeviceFormat, *SetDeviceFormat;
    void *GetProcessingPeriod, *SetProcessingPeriod, *GetShareMode, *SetShareMode;
    void *GetPropertyValue, *SetPropertyValue;
    HRESULT(STDMETHODCALLTYPE *SetDefaultEndpoint)(IPolicyConfig *, LPCWSTR id, ERole role);
    void *SetEndpointVisibility;
} IPolicyConfigVtbl;
struct IPolicyConfig { const IPolicyConfigVtbl *lpVtbl; };

static void ensure_mta(void) {
    static volatile LONG done;
    if (InterlockedCompareExchange(&done, 1, 0) == 0) {
        CO_MTA_USAGE_COOKIE cookie;
        CoIncrementMTAUsage(&cookie);
    }
}

int sb_win_default_output(int role, char *uid, int len) {
    if (!uid || len <= 0) return -1;
    uid[0] = 0;
    ensure_mta();
    IMMDeviceEnumerator *e = NULL;
    if (FAILED(CoCreateInstance(&do_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &do_IID_IMMDeviceEnumerator, (void **)&e)))
        return -1;
    IMMDevice *d = NULL;
    int rc = -1;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, eRender, role == 1 ? eCommunications : eConsole, &d))) {
        WCHAR *id = NULL;
        if (SUCCEEDED(IMMDevice_GetId(d, &id))) {
            if (WideCharToMultiByte(CP_UTF8, 0, id, -1, uid, len, NULL, NULL) > 0) rc = 0;
            CoTaskMemFree(id);
        }
        IMMDevice_Release(d);
    }
    IMMDeviceEnumerator_Release(e);
    return rc;
}

int sb_win_set_default_output(int role, const char *uid) {
    if (!uid || !uid[0]) return -1;
    WCHAR w[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, uid, -1, w, 512)) return -1;
    ensure_mta();
    IPolicyConfig *pc = NULL;
    if (FAILED(CoCreateInstance(&do_CLSID_PolicyConfigClient, NULL, CLSCTX_ALL, &do_IID_IPolicyConfig, (void **)&pc)))
        return -1;
    HRESULT hr;
    if (role == 1) {
        hr = pc->lpVtbl->SetDefaultEndpoint(pc, w, eCommunications);
    } else {
        hr = pc->lpVtbl->SetDefaultEndpoint(pc, w, eConsole);
        if (SUCCEEDED(hr)) hr = pc->lpVtbl->SetDefaultEndpoint(pc, w, eMultimedia);
    }
    pc->lpVtbl->Release(pc);
    return SUCCEEDED(hr) ? 0 : -1;
}

#endif
