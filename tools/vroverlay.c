#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <stdint.h>
#include <stdio.h>
#include "openvr_capi.h"

typedef intptr_t(OPENVR_FNTABLE_CALLTYPE *init_fn)(EVRInitError *, EVRApplicationType, const char *);
typedef intptr_t(OPENVR_FNTABLE_CALLTYPE *iface_fn)(const char *, EVRInitError *);
typedef void(OPENVR_FNTABLE_CALLTYPE *shutdown_fn)(void);

static FILE *g_log;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static int parent(void)
{
    g_log = fopen("C:\\macquestlink\\vroverlay_parent.log", "w");
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                      D3D11_SDK_VERSION, &dev, NULL, &ctx);
    D3D11_TEXTURE2D_DESC td = { 1860, 2048, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0,
                                D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE };
    ID3D11Texture2D *tex;
    say("create -> 0x%08lx", ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex));
    IDXGIResource1 *r1;
    HANDLE nt = NULL;
    ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&r1);
    say("CreateSharedHandle -> 0x%08lx %p",
        IDXGIResource1_CreateSharedHandle(r1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &nt), nt);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "C:\\macquestlink\\vroverlay.exe child %lu %llu", GetCurrentProcessId(),
             (unsigned long long)(uintptr_t)nt);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    WaitForSingleObject(pi.hProcess, 60000);
    say("child done");
    return 0;
}

// Head-locked test panel 1 m in front: vroverlay.exe panel <d3d|d3dclear|raw|file> <seconds>; "dash" opens the dashboard.
static int panel(const char *mode, int seconds)
{
    g_log = fopen("C:\\macquestlink\\vroverlay_panel.log", "w");
    HMODULE vr = LoadLibraryA("openvr_api.dll");
    init_fn init = (init_fn)GetProcAddress(vr, "VR_InitInternal2");
    iface_fn get_iface = (iface_fn)GetProcAddress(vr, "VR_GetGenericInterface");
    shutdown_fn shutdown = (shutdown_fn)GetProcAddress(vr, "VR_ShutdownInternal");
    EVRInitError err = EVRInitError_VRInitError_None;
    init(&err, EVRApplicationType_VRApplication_Overlay, NULL);
    say("VR_InitInternal2 -> %d", err);
    if (err) {
        return 1;
    }
    char name[128];
    snprintf(name, sizeof(name), "FnTable:%s", IVROverlay_Version);
    struct VR_IVROverlay_FnTable *ov = (void *)get_iface(name, &err);
    if (!strcmp(mode, "dash")) {
        ov->ShowDashboard("");
        for (int i = 0; i < seconds * 2; i++) {
            say("dashboard visible %d", ov->IsDashboardVisible());
            Sleep(500);
        }
        shutdown();
        return 0;
    }
    VROverlayHandle_t h = 0;
    say("CreateOverlay -> %d", ov->CreateOverlay("macquestlink.panel", "macquestlink panel", &h));
    HmdMatrix34_t m = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, -1.0f } } };
    say("transform -> %d", ov->SetOverlayTransformTrackedDeviceRelative(h, 0, &m));
    say("width -> %d", ov->SetOverlayWidthInMeters(h, 0.6f));
    say("sort -> %d", ov->SetOverlaySortOrder(h, 100));
    say("show -> %d", ov->ShowOverlay(h));

    enum { S = 256 };
    static uint8_t px[S * S * 4];
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            uint8_t v = ((x / 32) ^ (y / 32)) & 1 ? 255 : 0;
            uint8_t *p = px + (y * S + x) * 4;
            p[0] = v, p[1] = 255, p[2] = v, p[3] = 255;
        }
    }
    if (!strcmp(mode, "raw")) {
        say("SetOverlayRaw -> %d", ov->SetOverlayRaw(h, px, S, S, 4));
    } else if (!strcmp(mode, "file")) {
        FILE *f = fopen("C:\\macquestlink\\panel.ppm", "wb");
        fprintf(f, "P6\n%d %d\n255\n", S, S);
        for (int i = 0; i < S * S; i++) {
            fwrite(px + i * 4, 1, 3, f);
        }
        fclose(f);
        say("SetOverlayFromFile -> %d", ov->SetOverlayFromFile(h, "C:\\macquestlink\\panel.ppm"));
    } else {
        ID3D11Device *dev;
        ID3D11DeviceContext *ctx;
        D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                          D3D11_SDK_VERSION, &dev, NULL, &ctx);
        D3D11_TEXTURE2D_DESC td = { S, S, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        D3D11_SUBRESOURCE_DATA init_data = { px, S * 4, 0 };
        ID3D11Texture2D *tex = NULL;
        say("create -> 0x%08lx", ID3D11Device_CreateTexture2D(dev, &td, &init_data, &tex));
        if (strstr(mode, "clear")) {
            ID3D11RenderTargetView *rtv;
            static const float white[4] = { 1, 1, 1, 1 };
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv);
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, white);
        }
        ID3D11DeviceContext_Flush(ctx);
        Texture_t t = { tex, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Auto };
        for (int i = 0; i < seconds; i++) {
            EVROverlayError e = ov->SetOverlayTexture(h, &t);
            if (i == 0 || e) {
                say("SetOverlayTexture -> %d", e);
            }
            Sleep(1000);
        }
        shutdown();
        return 0;
    }
    Sleep(seconds * 1000);
    shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 3 && !strcmp(argv[1], "panel")) {
        return panel(argv[2], atoi(argv[3]));
    }
    if (argc > 1 && !strcmp(argv[1], "parent")) {
        return parent();
    }
    int is_child = argc > 3 && !strcmp(argv[1], "child");
    g_log = fopen(is_child ? "C:\\macquestlink\\vroverlay_child.log" : "C:\\macquestlink\\vroverlay.log", "w");
    int seconds = argc > 1 && !is_child ? atoi(argv[1]) : 0;

    HMODULE vr = LoadLibraryA("openvr_api.dll");
    init_fn init = (init_fn)GetProcAddress(vr, "VR_InitInternal2");
    iface_fn get_iface = (iface_fn)GetProcAddress(vr, "VR_GetGenericInterface");
    shutdown_fn shutdown = (shutdown_fn)GetProcAddress(vr, "VR_ShutdownInternal");

    EVRInitError err = EVRInitError_VRInitError_None;
    init(&err, EVRApplicationType_VRApplication_Overlay, NULL);
    say("VR_InitInternal2 -> %d", err);
    if (err) {
        return 1;
    }
    char name[128];
    snprintf(name, sizeof(name), "FnTable:%s", IVROverlay_Version);
    struct VR_IVROverlay_FnTable *ov = (void *)get_iface(name, &err);
    VROverlayHandle_t h = 0;
    say("CreateOverlay -> %d", ov->CreateOverlay("macquestlink.test", "macquestlink test", &h));
    ov->SetOverlayWidthInMeters(h, 1.0f);
    ov->ShowOverlay(h);

    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                      D3D11_SDK_VERSION, &dev, NULL, &ctx);
    ID3D11Device1 *dev1 = NULL;
    ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&dev1);

    if (is_child) {
        HANDLE src = OpenProcess(PROCESS_DUP_HANDLE, FALSE, strtoul(argv[2], NULL, 10)), nt = NULL;
        BOOL ok = DuplicateHandle(src, (HANDLE)(uintptr_t)strtoull(argv[3], NULL, 10), GetCurrentProcess(), &nt, 0,
                                  FALSE, DUPLICATE_SAME_ACCESS);
        ID3D11Texture2D *opened = NULL;
        HRESULT hr = ID3D11Device1_OpenSharedResource1(dev1, nt, &IID_ID3D11Texture2D, (void **)&opened);
        say("dup %d open 0x%08lx", ok, hr);
        if (opened) {
            Texture_t t = { opened, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Auto };
            say("cross-process opened SetOverlayTexture -> %d", ov->SetOverlayTexture(h, &t));
            IDXGIResource *lr;
            HANDLE legacy = NULL;
            ID3D11Texture2D_QueryInterface(opened, &IID_IDXGIResource, (void **)&lr);
            say("  GetSharedHandle 0x%08lx %p", IDXGIResource_GetSharedHandle(lr, &legacy), legacy);
            IDXGIResource1 *r1;
            HANDLE nt2 = NULL;
            ID3D11Texture2D_QueryInterface(opened, &IID_IDXGIResource1, (void **)&r1);
            say("  CreateSharedHandle 0x%08lx %p",
                IDXGIResource1_CreateSharedHandle(r1, NULL, DXGI_SHARED_RESOURCE_READ, NULL, &nt2), nt2);
        }
        shutdown();
        return 0;
    }
    static const UINT miscs[] = { 0, D3D11_RESOURCE_MISC_SHARED, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,
                                  D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
                                  D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE };
    ID3D11Texture2D *last = NULL;
    for (int i = 0; i < (int)(sizeof(miscs) / sizeof(miscs[0])); i++) {
        D3D11_TEXTURE2D_DESC td = { 512, 512, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, miscs[i] };
        ID3D11Texture2D *tex = NULL;
        HRESULT hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex);
        if (FAILED(hr)) {
            say("misc=0x%x create -> 0x%08lx", miscs[i], hr);
            continue;
        }
        ID3D11RenderTargetView *rtv;
        static const float color[4] = { 0, 0.5f, 1, 1 };
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, color);
        ID3D11DeviceContext_Flush(ctx);
        Texture_t t = { tex, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Auto };
        say("misc=0x%x SetOverlayTexture -> %d", miscs[i], ov->SetOverlayTexture(h, &t));

        IDXGIResource *legacy_res;
        HANDLE legacy = NULL;
        ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource, (void **)&legacy_res);
        if (SUCCEEDED(IDXGIResource_GetSharedHandle(legacy_res, &legacy)) && legacy) {
            Texture_t ts = { legacy, ETextureType_TextureType_DXGISharedHandle, EColorSpace_ColorSpace_Auto };
            say("  legacy handle %p as DXGISharedHandle -> %d", legacy, ov->SetOverlayTexture(h, &ts));
        }
        if ((miscs[i] & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) && dev1) {
            IDXGIResource1 *r1;
            HANDLE nt1 = NULL;
            ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&r1);
            IDXGIResource1_CreateSharedHandle(r1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL,
                                              &nt1);
            Texture_t tn = { nt1, ETextureType_TextureType_DXGISharedHandle, EColorSpace_ColorSpace_Auto };
            say("  NT handle %p as DXGISharedHandle -> %d", nt1, ov->SetOverlayTexture(h, &tn));
        }
        if ((miscs[i] & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) && dev1) {
            IDXGIResource1 *res;
            HANDLE nt = NULL;
            ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&res);
            hr = IDXGIResource1_CreateSharedHandle(res, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                   NULL, &nt);
            ID3D11Texture2D *opened = NULL;
            HRESULT hr2 = ID3D11Device1_OpenSharedResource1(dev1, nt, &IID_ID3D11Texture2D, (void **)&opened);
            say("  CreateSharedHandle 0x%08lx OpenSharedResource1 0x%08lx", hr, hr2);
            if (opened) {
                Texture_t t2 = { opened, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Auto };
                say("  opened copy SetOverlayTexture -> %d", ov->SetOverlayTexture(h, &t2));
            }
        }
        last = tex;
    }
    if (seconds && last) {
        Texture_t t = { last, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Auto };
        ov->SetOverlayTexture(h, &t);
        Sleep(seconds * 1000);
    }
    shutdown();
    say("done");
    return 0;
}
