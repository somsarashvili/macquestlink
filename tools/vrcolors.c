#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
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

int main(int argc, char **argv)
{
    g_log = fopen("C:\\macquestlink\\vrcolors.log", "w");
    int seconds = argc > 1 ? atoi(argv[1]) : 60;

    HMODULE vr = LoadLibraryA("openvr_api.dll");
    if (!vr) {
        say("cannot load openvr_api.dll (%lu)", GetLastError());
        return 1;
    }
    init_fn init = (init_fn)GetProcAddress(vr, "VR_InitInternal2");
    iface_fn get_iface = (iface_fn)GetProcAddress(vr, "VR_GetGenericInterface");
    shutdown_fn shutdown = (shutdown_fn)GetProcAddress(vr, "VR_ShutdownInternal");

    EVRInitError err = EVRInitError_VRInitError_None;
    init(&err, EVRApplicationType_VRApplication_Scene, NULL);
    say("VR_InitInternal2 -> %d", err);
    if (err) {
        return 1;
    }
    char name[128];
    snprintf(name, sizeof(name), "FnTable:%s", IVRSystem_Version);
    struct VR_IVRSystem_FnTable *sys = (void *)get_iface(name, &err);
    snprintf(name, sizeof(name), "FnTable:%s", IVRCompositor_Version);
    struct VR_IVRCompositor_FnTable *comp = (void *)get_iface(name, &err);
    if (!sys || !comp) {
        say("interfaces missing (err %d)", err);
        return 1;
    }
    uint32_t w = 0, h = 0;
    sys->GetRecommendedRenderTargetSize(&w, &h);
    say("render target %ux%u", w, h);

    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    say("D3D11CreateDevice -> 0x%08lx", hr);

    D3D11_TEXTURE2D_DESC td = { w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D *eye[2];
    ID3D11RenderTargetView *rtv[2];
    for (int i = 0; i < 2; i++) {
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &eye[i]);
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)eye[i], NULL, &rtv[i]);
    }

    static const float colors[3][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } };
    struct TrackedDevicePose_t poses[64];
    DWORD start = GetTickCount(), last_report = start;
    uint32_t frames = 0, wait_errors = 0, submit_errors = 0;
    EVRCompositorError last_wait = 0, last_submit = 0;
    while (GetTickCount() - start < (DWORD)seconds * 1000) {
        last_wait = comp->WaitGetPoses(poses, 64, NULL, 0);
        if (last_wait) {
            wait_errors++;
        }
        int c = ((GetTickCount() - start) / 1000) % 3;
        for (int i = 0; i < 2; i++) {
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv[i], colors[c]);
            Texture_t tex = { eye[i], ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Auto };
            EVRCompositorError e = comp->Submit(i ? EVREye_Eye_Right : EVREye_Eye_Left, &tex, NULL, EVRSubmitFlags_Submit_Default);
            if (e) {
                submit_errors++;
                last_submit = e;
            }
        }
        comp->PostPresentHandoff();
        frames++;
        if (GetTickCount() - last_report >= 1000) {
            say("t=%lus frames=%u color=%d wait_err=%u(last %d) submit_err=%u(last %d) hmd valid=%d connected=%d "
                "result=%d pos=(%.3f,%.3f,%.3f) fwd=(%.2f,%.2f,%.2f)",
                (GetTickCount() - start) / 1000, frames, c, wait_errors, last_wait, submit_errors, last_submit,
                poses[0].bPoseIsValid, poses[0].bDeviceIsConnected, poses[0].eTrackingResult,
                poses[0].mDeviceToAbsoluteTracking.m[0][3], poses[0].mDeviceToAbsoluteTracking.m[1][3],
                poses[0].mDeviceToAbsoluteTracking.m[2][3], -poses[0].mDeviceToAbsoluteTracking.m[0][2], -poses[0].mDeviceToAbsoluteTracking.m[1][2], -poses[0].mDeviceToAbsoluteTracking.m[2][2]);
            float since = 0;
            uint64_t vcount = 0;
            sys->GetTimeSinceLastVsync(&since, &vcount);
            Compositor_FrameTiming ft = { sizeof(ft) };
            int ok = comp->GetFrameTiming(&ft, 0);
            struct TrackedDevicePose_t sp[1];
            sys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding, 0, sp, 1);
            say("  vsync since=%.4f count=%llu | timing ok=%d idx=%u reproj=0x%x vsyncsReady=%u vsyncsToView=%u "
                "hmdPose result=%d valid=%d | sys standing result=%d valid=%d",
                since, (unsigned long long)vcount, ok, ft.m_nFrameIndex, ft.m_nReprojectionFlags,
                ft.m_nNumVSyncsReadyForUse, ft.m_nNumVSyncsToFirstView, ft.m_HmdPose.eTrackingResult,
                ft.m_HmdPose.bPoseIsValid, sp[0].eTrackingResult, sp[0].bPoseIsValid);
            last_report = GetTickCount();
            frames = 0;
        }
    }
    shutdown();
    say("done");
    return 0;
}
