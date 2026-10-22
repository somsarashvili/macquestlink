// audioprobe.exe [loopback-name [hook]]: lists MMDevice endpoints (all states, both flows) with their name properties,
// then plays a tone on the render endpoint whose friendly name contains loopback-name and loopback-captures it the way
// vrlink does. "hook" loads the NVENC shim from SteamVR first (nvenc_shim/audio_hook.c).
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static void loopback_test(IMMDevice *dev)
{
    IAudioClient *ac, *out;
    IAudioCaptureClient *cap;
    IAudioRenderClient *rc;
    WAVEFORMATEX *fmt;
    HRESULT hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    printf("loopback Activate 0x%08lx\n", hr);
    if (FAILED(hr)) {
        return;
    }
    IAudioClient_GetMixFormat(ac, &fmt);
    printf("mix format: %u ch %lu Hz %u bits tag 0x%x\n", fmt->nChannels, fmt->nSamplesPerSec, fmt->wBitsPerSample,
           fmt->wFormatTag);
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 0, 0, fmt, NULL);
    printf("loopback Initialize 0x%08lx\n", hr);
    if (FAILED(hr)) {
        return;
    }
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    IAudioClient_SetEventHandle(ac, ev);
    hr = IAudioClient_GetService(ac, &IID_IAudioCaptureClient, (void **)&cap);
    printf("capture client 0x%08lx\n", hr);
    IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&out);
    hr = IAudioClient_Initialize(out, AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, fmt, NULL);
    UINT32 frames = 0;
    IAudioClient_GetBufferSize(out, &frames);
    IAudioClient_GetService(out, &IID_IAudioRenderClient, (void **)&rc);
    printf("tone render Initialize 0x%08lx, %u frames\n", hr, frames);
    double phase = 0, sum = 0;
    long n = 0, packets = 0;
    BYTE *data;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc, frames, &data))) {
        float *f = (float *)data;
        for (UINT32 i = 0; i < frames; i++) {
            for (int c = 0; c < fmt->nChannels; c++) {
                f[i * fmt->nChannels + c] = 0.2f * sinf(phase);
            }
            phase += 2 * M_PI * 440 / fmt->nSamplesPerSec;
        }
        IAudioRenderClient_ReleaseBuffer(rc, frames, 0);
    }
    IAudioClient_Start(ac);
    IAudioClient_Start(out);
    DWORD end = GetTickCount() + 1500;
    while (GetTickCount() < end) {
        WaitForSingleObject(ev, 100);
        UINT32 got;
        DWORD flags;
        while (SUCCEEDED(IAudioCaptureClient_GetBuffer(cap, &data, &got, &flags, NULL, NULL)) && got) {
            float *f = (float *)data;
            for (UINT32 i = 0; i < got * fmt->nChannels; i++) {
                sum += f[i] * f[i];
            }
            n += got * fmt->nChannels;
            packets++;
            IAudioCaptureClient_ReleaseBuffer(cap, got);
        }
    }
    IAudioClient_Stop(out);
    IAudioClient_Stop(ac);
    printf("captured %ld packets, %ld samples, rms %f (tone 0.141)\n", packets, n, n ? sqrt(sum / n) : 0);
}

static void print_prop(IPropertyStore *ps, const PROPERTYKEY *key, const char *label)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, key, &v)) && v.vt == VT_LPWSTR) {
        printf("    %s: %ls\n", label, v.pwszVal);
    } else {
        printf("    %s: (vt %d)\n", label, v.vt);
    }
    PropVariantClear(&v);
}

int main(int argc, char **argv)
{
    wchar_t want[128] = L"";
    if (argc > 1) {
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, want, 128);
    }
    if (argc > 2 && !strcmp(argv[2], "hook")) {
        printf("hook dll %p\n", LoadLibraryA("C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\nvEncodeAPI64.dll"));
        Sleep(1000);
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
    printf("CoCreateInstance 0x%08lx\n", hr);
    if (FAILED(hr)) {
        return 1;
    }
    IMMDevice *loop_dev = NULL;
    for (int flow = eRender; flow <= eCapture; flow++) {
        IMMDeviceCollection *col;
        hr = IMMDeviceEnumerator_EnumAudioEndpoints(en, flow, DEVICE_STATEMASK_ALL, &col);
        UINT n = 0;
        if (SUCCEEDED(hr)) {
            IMMDeviceCollection_GetCount(col, &n);
        }
        printf("%s: hr 0x%08lx count %u\n", flow == eRender ? "render" : "capture", hr, n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice *dev;
            IMMDeviceCollection_Item(col, i, &dev);
            LPWSTR id = NULL;
            DWORD state = 0;
            IMMDevice_GetId(dev, &id);
            IMMDevice_GetState(dev, &state);
            printf("  [%u] state %lu id %ls\n", i, state, id);
            IPropertyStore *ps;
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
                print_prop(ps, &PKEY_Device_FriendlyName, "FriendlyName");
                print_prop(ps, &PKEY_Device_DeviceDesc, "DeviceDesc");
                print_prop(ps, &PKEY_DeviceInterface_FriendlyName, "InterfaceFriendlyName");
                PROPVARIANT v;
                PropVariantInit(&v);
                if (*want && flow == eRender && SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)) &&
                    v.vt == VT_LPWSTR && wcsstr(v.pwszVal, want) && !loop_dev) {
                    loop_dev = dev;
                    IMMDevice_AddRef(dev);
                }
                PropVariantClear(&v);
                IPropertyStore_Release(ps);
            }
            CoTaskMemFree(id);
            IMMDevice_Release(dev);
        }
    }
    IMMDevice *def;
    for (int flow = eRender; flow <= eCapture; flow++) {
        if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, flow, eConsole, &def))) {
            LPWSTR id;
            IMMDevice_GetId(def, &id);
            printf("default %s: %ls\n", flow == eRender ? "render" : "capture", id);
            CoTaskMemFree(id);
            IMMDevice_Release(def);
        }
    }
    if (loop_dev) {
        loopback_test(loop_dev);
    }
    fflush(stdout);
    return 0;
}
