// Audio fixes for vrserver.exe, where vrlink loads this DLL:
// - Wine's IConnector::GetDeviceIdConnectedTo is E_NOTIMPL, and SteamVR's audio device enumeration skips every active
//   endpoint whose topology lookup fails, so vrlink never finds "Steam Streaming Speakers"/"Microphone". The hook
//   answers with an existing endpoint ID (only that devnode's InstanceId is read from it).
// - Wine has no WASAPI loopback capture. vrlink loopback-captures the Steam Streaming Speakers render endpoint; on
//   macOS that endpoint is an aggregate over a loopback device (tools/mkaudiodev), so a loopback Initialize on a render
//   endpoint is redirected to the capture endpoint of the same device.
// - Wine's IAudioEndpointVolume::QueryHardwareSupport is E_NOTIMPL, which vrlink treats as fatal; it reports no
//   hardware support instead.
// - Wine has no IPolicyConfig, which vrserver uses to make the HMD's endpoints the default while SteamVR runs (and to
//   restore them after). CoCreateInstance is hooked in vrserver's and vrlink's imports to supply one whose
//   SetDefaultEndpoint sets Wine's default device (HKCU\Software\Wine\Drivers\<driver> DefaultOutput etc.).
// The first three are patched into mmdevapi's shared vtables.
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <devicetopology.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <wchar.h>

DEFINE_GUID(CLSID_MMDeviceEnumerator_, 0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
DEFINE_GUID(IID_IMMDeviceEnumerator_, 0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6);
DEFINE_GUID(IID_IMMEndpoint_, 0x1be09788, 0x6894, 0x4089, 0x85, 0x86, 0x9a, 0x2a, 0x6c, 0x26, 0x5a, 0xc5);
DEFINE_GUID(IID_IAudioClient_, 0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2);
DEFINE_GUID(CLSID_PolicyConfigVista, 0x294935ce, 0xf637, 0x4e7c, 0xa4, 0x1b, 0xab, 0x25, 0x54, 0x60, 0xb8, 0x62);
DEFINE_GUID(IID_IPolicyConfigVista, 0x568b9108, 0x44bf, 0x40b4, 0x90, 0x06, 0x86, 0xaf, 0xe5, 0xb5, 0xa6, 0x20);
DEFINE_GUID(IID_IAudioEndpointVolume_, 0x5cdf2c82, 0x841e, 0x4546, 0x97, 0x22, 0x0c, 0xf7, 0x40, 0x78, 0x22, 0x9a);
DEFINE_GUID(IID_IDeviceTopology_, 0x2a07407e, 0x6497, 0x4a18, 0x97, 0x87, 0x32, 0xf7, 0x9b, 0xd0, 0xd9, 0x8f);

static FILE *logf;
static WCHAR devnode_id[128];
static HRESULT(STDMETHODCALLTYPE *real_activate)(IMMDevice *, REFIID, DWORD, PROPVARIANT *, void **);
static HRESULT(STDMETHODCALLTYPE *real_connected_to)(IConnector *, LPWSTR *);
static HRESULT(STDMETHODCALLTYPE *real_hw_support)(IAudioEndpointVolume *, DWORD *);
static HRESULT(WINAPI *real_cocreate)(REFCLSID, IUnknown *, DWORD, REFIID, void **);

static void logmsg(const char *fmt, ...)
{
    if (!logf) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fflush(logf);
}

static void patch(void **slot, void *fn, void **orig)
{
    DWORD old;
    *orig = *slot;
    VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
    *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

static HRESULT STDMETHODCALLTYPE hook_connected_to(IConnector *This, LPWSTR *id)
{
    HRESULT hr = real_connected_to(This, id);
    if (hr != E_NOTIMPL || !id) {
        return hr;
    }
    size_t size = (wcslen(devnode_id) + 1) * sizeof(WCHAR);
    if (!(*id = CoTaskMemAlloc(size))) {
        return E_OUTOFMEMORY;
    }
    memcpy(*id, devnode_id, size);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hook_hw_support(IAudioEndpointVolume *This, DWORD *mask)
{
    HRESULT hr = real_hw_support(This, mask);
    if (hr != E_NOTIMPL || !mask) {
        return hr;
    }
    *mask = 0;
    return S_OK;
}

static BOOL interface_name(IMMDevice *dev, WCHAR *name, size_t len)
{
    IPropertyStore *ps;
    BOOL ok = FALSE;
    if (FAILED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
        return FALSE;
    }
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_DeviceInterface_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
        lstrcpynW(name, v.pwszVal, len);
        ok = TRUE;
    }
    PropVariantClear(&v);
    IPropertyStore_Release(ps);
    return ok;
}

static IMMDevice *capture_twin(IMMDevice *render)
{
    WCHAR want[256], name[256];
    IMMDeviceEnumerator *en;
    IMMDeviceCollection *col;
    IMMDevice *found = NULL;
    UINT n = 0;
    if (!interface_name(render, want, ARRAYSIZE(want)) ||
        FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator_, (void **)&en))) {
        return NULL;
    }
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eCapture, DEVICE_STATE_ACTIVE, &col))) {
        IMMDeviceCollection_GetCount(col, &n);
        for (UINT i = 0; i < n && !found; i++) {
            IMMDevice *dev;
            if (FAILED(IMMDeviceCollection_Item(col, i, &dev))) {
                continue;
            }
            if (interface_name(dev, name, ARRAYSIZE(name)) && !wcscmp(name, want)) {
                found = dev;
            } else {
                IMMDevice_Release(dev);
            }
        }
        IMMDeviceCollection_Release(col);
    }
    IMMDeviceEnumerator_Release(en);
    logmsg("loopback: capture twin of \"%ls\": %s\n", want, found ? "found" : "none");
    return found;
}

struct loopback_client {
    IAudioClient iface;
    LONG ref;
    IMMDevice *dev;
    IAudioClient *render, *capture;
};

static struct loopback_client *impl(IAudioClient *iface)
{
    return CONTAINING_RECORD(iface, struct loopback_client, iface);
}

static IAudioClient *cur(IAudioClient *iface)
{
    struct loopback_client *c = impl(iface);
    return c->capture ? c->capture : c->render;
}

static HRESULT STDMETHODCALLTYPE lc_QueryInterface(IAudioClient *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IAudioClient_)) {
        *out = iface;
        IAudioClient_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE lc_AddRef(IAudioClient *iface)
{
    return InterlockedIncrement(&impl(iface)->ref);
}

static ULONG STDMETHODCALLTYPE lc_Release(IAudioClient *iface)
{
    struct loopback_client *c = impl(iface);
    ULONG ref = InterlockedDecrement(&c->ref);
    if (!ref) {
        if (c->capture) {
            IAudioClient_Release(c->capture);
        }
        IAudioClient_Release(c->render);
        IMMDevice_Release(c->dev);
        HeapFree(GetProcessHeap(), 0, c);
    }
    return ref;
}

static HRESULT STDMETHODCALLTYPE lc_Initialize(IAudioClient *iface, AUDCLNT_SHAREMODE mode, DWORD flags,
                                               REFERENCE_TIME duration, REFERENCE_TIME period, const WAVEFORMATEX *fmt,
                                               LPCGUID session)
{
    struct loopback_client *c = impl(iface);
    if (!(flags & AUDCLNT_STREAMFLAGS_LOOPBACK) || c->capture) {
        return IAudioClient_Initialize(cur(iface), mode, flags, duration, period, fmt, session);
    }
    IMMDevice *twin = capture_twin(c->dev);
    if (!twin) {
        return IAudioClient_Initialize(c->render, mode, flags, duration, period, fmt, session);
    }
    IAudioClient *cap;
    HRESULT hr = real_activate(twin, &IID_IAudioClient_, CLSCTX_ALL, NULL, (void **)&cap);
    IMMDevice_Release(twin);
    if (FAILED(hr)) {
        return hr;
    }
    hr = IAudioClient_Initialize(cap, mode, flags & ~AUDCLNT_STREAMFLAGS_LOOPBACK, duration, period, fmt, session);
    logmsg("loopback: capture Initialize(flags 0x%lx, %u ch %lu Hz %u bit) = 0x%08lx\n", flags, fmt->nChannels,
           fmt->nSamplesPerSec, fmt->wBitsPerSample, hr);
    if (FAILED(hr)) {
        IAudioClient_Release(cap);
        return hr;
    }
    c->capture = cap;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE lc_GetBufferSize(IAudioClient *iface, UINT32 *frames)
{
    return IAudioClient_GetBufferSize(cur(iface), frames);
}

static HRESULT STDMETHODCALLTYPE lc_GetStreamLatency(IAudioClient *iface, REFERENCE_TIME *latency)
{
    return IAudioClient_GetStreamLatency(cur(iface), latency);
}

static HRESULT STDMETHODCALLTYPE lc_GetCurrentPadding(IAudioClient *iface, UINT32 *frames)
{
    return IAudioClient_GetCurrentPadding(cur(iface), frames);
}

static HRESULT STDMETHODCALLTYPE lc_IsFormatSupported(IAudioClient *iface, AUDCLNT_SHAREMODE mode,
                                                      const WAVEFORMATEX *fmt, WAVEFORMATEX **closest)
{
    return IAudioClient_IsFormatSupported(cur(iface), mode, fmt, closest);
}

static HRESULT STDMETHODCALLTYPE lc_GetMixFormat(IAudioClient *iface, WAVEFORMATEX **fmt)
{
    return IAudioClient_GetMixFormat(cur(iface), fmt);
}

static HRESULT STDMETHODCALLTYPE lc_GetDevicePeriod(IAudioClient *iface, REFERENCE_TIME *def, REFERENCE_TIME *min)
{
    return IAudioClient_GetDevicePeriod(cur(iface), def, min);
}

static HRESULT STDMETHODCALLTYPE lc_Start(IAudioClient *iface)
{
    return IAudioClient_Start(cur(iface));
}

static HRESULT STDMETHODCALLTYPE lc_Stop(IAudioClient *iface)
{
    return IAudioClient_Stop(cur(iface));
}

static HRESULT STDMETHODCALLTYPE lc_Reset(IAudioClient *iface)
{
    return IAudioClient_Reset(cur(iface));
}

static HRESULT STDMETHODCALLTYPE lc_SetEventHandle(IAudioClient *iface, HANDLE event)
{
    return IAudioClient_SetEventHandle(cur(iface), event);
}

static HRESULT STDMETHODCALLTYPE lc_GetService(IAudioClient *iface, REFIID iid, void **out)
{
    return IAudioClient_GetService(cur(iface), iid, out);
}

static IAudioClientVtbl lc_vtbl = {
    lc_QueryInterface, lc_AddRef,        lc_Release,       lc_Initialize,     lc_GetBufferSize,
    lc_GetStreamLatency, lc_GetCurrentPadding, lc_IsFormatSupported, lc_GetMixFormat, lc_GetDevicePeriod,
    lc_Start,          lc_Stop,          lc_Reset,         lc_SetEventHandle, lc_GetService,
};

static HRESULT STDMETHODCALLTYPE hook_activate(IMMDevice *dev, REFIID iid, DWORD ctx, PROPVARIANT *params, void **out)
{
    HRESULT hr = real_activate(dev, iid, ctx, params, out);
    if (FAILED(hr) || !IsEqualIID(iid, &IID_IAudioClient_)) {
        return hr;
    }
    IMMEndpoint *ep;
    EDataFlow flow = eCapture;
    if (SUCCEEDED(IMMDevice_QueryInterface(dev, &IID_IMMEndpoint_, (void **)&ep))) {
        IMMEndpoint_GetDataFlow(ep, &flow);
        IMMEndpoint_Release(ep);
    }
    struct loopback_client *c;
    if (flow != eRender || !(c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c)))) {
        return hr;
    }
    c->iface.lpVtbl = &lc_vtbl;
    c->ref = 1;
    c->dev = dev;
    IMMDevice_AddRef(dev);
    c->render = *out;
    *out = &c->iface;
    return hr;
}

typedef struct {
    const struct policy_vtbl *lpVtbl;
} policy_config;

struct policy_vtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(policy_config *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(policy_config *);
    ULONG(STDMETHODCALLTYPE *Release)(policy_config *);
    void *unsupported[9];
    HRESULT(STDMETHODCALLTYPE *SetDefaultEndpoint)(policy_config *, LPCWSTR, ERole);
    void *unsupported2;
};

static HRESULT STDMETHODCALLTYPE pc_QueryInterface(policy_config *This, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IPolicyConfigVista)) {
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE pc_AddRef(policy_config *This)
{
    return 2;
}

static ULONG STDMETHODCALLTYPE pc_Release(policy_config *This)
{
    return 1;
}

static HRESULT STDMETHODCALLTYPE pc_unsupported(void)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE pc_SetDefaultEndpoint(policy_config *This, LPCWSTR id, ERole role)
{
    BOOL capture = !wcsncmp(id, L"{0.0.1.", 7), streaming = FALSE;
    const WCHAR *value = role == eCommunications ? (capture ? L"DefaultVoiceInput" : L"DefaultVoiceOutput")
                                                 : (capture ? L"DefaultInput" : L"DefaultOutput");
    IMMDeviceEnumerator *en;
    IMMDevice *dev;
    WCHAR name[256];
    if (SUCCEEDED(real_cocreate(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator_, (void **)&en))) {
        if (SUCCEEDED(IMMDeviceEnumerator_GetDevice(en, id, &dev))) {
            streaming = interface_name(dev, name, ARRAYSIZE(name)) && wcsstr(name, L"Steam Streaming");
            IMMDevice_Release(dev);
        }
        IMMDeviceEnumerator_Release(en);
    }
    // SteamVR restores the previous default when it exits; dropping the override lets Wine follow macOS again.
    HKEY drivers = NULL, devices;
    WCHAR drv[64];
    LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Wine\\Drivers", 0, KEY_ALL_ACCESS, &drivers);
    for (DWORD i = 0, len = ARRAYSIZE(drv); !st && !RegEnumKeyExW(drivers, i, drv, &len, NULL, NULL, NULL, NULL);
         i++, len = ARRAYSIZE(drv)) {
        WCHAR sub[96];
        swprintf(sub, ARRAYSIZE(sub), L"%ls\\devices", drv);
        if (RegOpenKeyExW(drivers, sub, 0, KEY_READ, &devices)) {
            continue;
        }
        RegCloseKey(devices);
        st = streaming ? RegSetKeyValueW(drivers, drv, value, REG_SZ, id, (wcslen(id) + 1) * sizeof(WCHAR))
                       : RegDeleteKeyValueW(drivers, drv, value);
        st = st == ERROR_FILE_NOT_FOUND ? 0 : st;
        logmsg("policy: %ls\\%ls %s %ls -> %ld\n", drv, value, streaming ? "=" : "cleared, was asked for", id, st);
    }
    if (drivers) {
        RegCloseKey(drivers);
    }
    return st ? HRESULT_FROM_WIN32(st) : S_OK;
}

static const struct policy_vtbl pc_vtbl = {
    pc_QueryInterface, pc_AddRef, pc_Release,
    { pc_unsupported, pc_unsupported, pc_unsupported, pc_unsupported, pc_unsupported, pc_unsupported, pc_unsupported,
      pc_unsupported, pc_unsupported },
    pc_SetDefaultEndpoint, pc_unsupported,
};
static policy_config policy = { &pc_vtbl };

static HRESULT WINAPI hook_cocreate(REFCLSID clsid, IUnknown *outer, DWORD ctx, REFIID iid, void **out)
{
    if (IsEqualCLSID(clsid, &CLSID_PolicyConfigVista)) {
        return pc_QueryInterface(&policy, iid, out);
    }
    return real_cocreate(clsid, outer, ctx, iid, out);
}

static int hook_imports(HMODULE mod, void *target, void *fn)
{
    int n = 0;
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) {
        return 0;
    }
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (void *)(base + dir->VirtualAddress); imp->Name; imp++) {
        for (IMAGE_THUNK_DATA *t = (void *)(base + imp->FirstThunk); t->u1.Function; t++) {
            if ((void *)t->u1.Function == target) {
                void *old;
                patch((void **)&t->u1.Function, fn, &old);
                n++;
            }
        }
    }
    return n;
}

static DWORD WINAPI install(void *arg)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IDeviceTopology *topo = NULL;
    IConnector *conn = NULL;
    IAudioEndpointVolume *vol = NULL;
    LPWSTR id = NULL;
    real_cocreate = (void *)GetProcAddress(GetModuleHandleW(L"ole32.dll"), "CoCreateInstance");
    int n = hook_imports(GetModuleHandleW(NULL), real_cocreate, hook_cocreate);
    if (GetModuleHandleW(L"driver_vrlink.dll")) {
        n += hook_imports(GetModuleHandleW(L"driver_vrlink.dll"), real_cocreate, hook_cocreate);
    }
    logmsg("CoCreateInstance %p: %d imports hooked\n", real_cocreate, n);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator_, (void **)&en);
    if (SUCCEEDED(hr)) {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    }
    if (SUCCEEDED(hr)) {
        hr = IMMDevice_GetId(dev, &id);
    }
    if (SUCCEEDED(hr)) {
        lstrcpynW(devnode_id, id, ARRAYSIZE(devnode_id));
        patch((void **)&dev->lpVtbl->Activate, hook_activate, (void **)&real_activate);
        hr = real_activate(dev, &IID_IAudioEndpointVolume_, CLSCTX_ALL, NULL, (void **)&vol);
    }
    if (SUCCEEDED(hr)) {
        patch((void **)&vol->lpVtbl->QueryHardwareSupport, hook_hw_support, (void **)&real_hw_support);
        IAudioEndpointVolume_Release(vol);
        hr = real_activate(dev, &IID_IDeviceTopology_, CLSCTX_ALL, NULL, (void **)&topo);
    }
    if (SUCCEEDED(hr)) {
        hr = IDeviceTopology_GetConnector(topo, 0, &conn);
    }
    if (SUCCEEDED(hr)) {
        patch((void **)&conn->lpVtbl->GetDeviceIdConnectedTo, hook_connected_to, (void **)&real_connected_to);
    }
    logmsg("hooks installed: 0x%08lx (devnode %ls)\n", hr, devnode_id);
    if (conn) {
        IConnector_Release(conn);
    }
    if (topo) {
        IDeviceTopology_Release(topo);
    }
    CoTaskMemFree(id);
    if (dev) {
        IMMDevice_Release(dev);
    }
    if (en) {
        IMMDeviceEnumerator_Release(en);
    }
    CoUninitialize();
    return 0;
}

void audio_hook_attach(void)
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    const WCHAR *exe = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
    if (_wcsicmp(exe, L"vrserver.exe") && _wcsicmp(exe, L"audioprobe.exe")) {
        return;
    }
    logf = fopen("C:\\macquestlink\\audio_hook.log", "a");
    CloseHandle(CreateThread(NULL, 0, install, NULL, 0, NULL));
}
