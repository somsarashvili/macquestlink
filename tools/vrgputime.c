#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "openvr_capi.h"

// OpenVR scene app with a known GPU load per frame: checks what SteamVR's frame timing (vrclient's timestamp queries
// on this device) reports as app GPU time against our own timestamps and wall clock. Writes C:\macquestlink\vrgputime.log.
// Usage: vrgputime.exe [seconds] [draws per eye] [shader iterations] [mode: 0 render after WaitGetPoses,
//        1 render on a deferred context, 2 flush after every draw]

typedef intptr_t(OPENVR_FNTABLE_CALLTYPE *init_fn)(EVRInitError *, EVRApplicationType, const char *);
typedef intptr_t(OPENVR_FNTABLE_CALLTYPE *iface_fn)(const char *, EVRInitError *);
typedef void(OPENVR_FNTABLE_CALLTYPE *shutdown_fn)(void);

static FILE *out;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f);
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static const char shader[] = "cbuffer c : register(b0) { float4 k; };\n"
                             "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
                             "  float2 p = float2(id & 2, (id << 1) & 2) * 2.0 - 1.0;\n"
                             "  return float4(p, 0, 1); }\n"
                             "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
                             "  float4 a = float4(pos.xy * 0.001, k.x, 1);\n"
                             "  [loop] for (int i = 0; i < (int)k.w; i++) a = sin(a * 1.0001 + k);\n"
                             "  return a * 0.0001; }\n";

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(shader, sizeof(shader) - 1, NULL, NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        say("D3DCompile %s hr=0x%08lx %s\n", entry, hr, err ? (char *)ID3D10Blob_GetBufferPointer(err) : "");
        return NULL;
    }
    return code;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int draws = argc > 2 ? atoi(argv[2]) : 4;
    int iters = argc > 3 ? atoi(argv[3]) : 64;
    int mode = argc > 4 ? atoi(argv[4]) : 0;
    out = fopen("C:\\macquestlink\\vrgputime.log", "w");

    HMODULE vr = LoadLibraryA("openvr_api.dll");
    init_fn init = (init_fn)GetProcAddress(vr, "VR_InitInternal2");
    iface_fn get_iface = (iface_fn)GetProcAddress(vr, "VR_GetGenericInterface");
    shutdown_fn shutdown = (shutdown_fn)GetProcAddress(vr, "VR_ShutdownInternal");
    EVRInitError err = EVRInitError_VRInitError_None;
    init(&err, EVRApplicationType_VRApplication_Scene, NULL);
    if (err) {
        say("init error %d\n", err);
        return 1;
    }
    char name[128];
    snprintf(name, sizeof(name), "FnTable:%s", IVRSystem_Version);
    struct VR_IVRSystem_FnTable *sys = (void *)get_iface(name, &err);
    snprintf(name, sizeof(name), "FnTable:%s", IVRCompositor_Version);
    struct VR_IVRCompositor_FnTable *comp = (void *)get_iface(name, &err);
    uint32_t w = 0, h = 0;
    sys->GetRecommendedRenderTargetSize(&w, &h);

    ID3D11Device *dev;
    ID3D11DeviceContext *imm, *def = NULL;
    D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &imm);
    ID3D11Device_CreateDeferredContext(dev, 0, &def);
    ID3DBlob *vsb = compile("vs", "vs_5_0"), *psb = compile("ps", "ps_5_0");
    if (!vsb || !psb) {
        return 1;
    }
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    float k[4] = { 0.1f, 0.2f, 0.3f, (float)iters };
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA sd = { k, 0, 0 };
    ID3D11Buffer *cb = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, &sd, &cb);

    D3D11_TEXTURE2D_DESC td = { w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D *eye[2];
    ID3D11RenderTargetView *rtv[2];
    for (int i = 0; i < 2; i++) {
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &eye[i]);
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)eye[i], NULL, &rtv[i]);
    }
    ID3D11Query *dj[2], *t0[2], *t1[2];
    for (int i = 0; i < 2; i++) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        ID3D11Device_CreateQuery(dev, &qd, &dj[i]);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        ID3D11Device_CreateQuery(dev, &qd, &t0[i]);
        ID3D11Device_CreateQuery(dev, &qd, &t1[i]);
    }
    char d3d_path[MAX_PATH] = "";
    GetModuleFileNameA(GetModuleHandleA("d3d11.dll"), d3d_path, sizeof(d3d_path));
    say("vrclient_x64.dll at %p, d3d11.dll %s\n", GetModuleHandleA("vrclient_x64.dll"), d3d_path);
    say("render target %ux%u, %d draws per eye, %d iterations, mode %d\n", w, h, draws, iters, mode);

    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    struct TrackedDevicePose_t poses[64];
    double start = now_ms(), last_report = start, own_sum = 0, pre_sum = 0, post_sum = 0, total_sum = 0;
    double cpu_sum = 0;
    uint32_t frames = 0, own_n = 0, pre_zero = 0;
    for (uint64_t f = 0; now_ms() - start < seconds * 1000.0; f++) {
        comp->WaitGetPoses(poses, 64, NULL, 0);
        double c0 = now_ms();
        ID3D11DeviceContext *ctx = mode == 1 ? def : imm;
        ID3D11DeviceContext_Begin(imm, (ID3D11Asynchronous *)dj[f & 1]);
        ID3D11DeviceContext_End(imm, (ID3D11Asynchronous *)t0[f & 1]);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
        for (int e = 0; e < 2; e++) {
            static const float clear[4] = { 0, 0, 0, 1 };
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv[e], clear);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv[e], NULL);
            for (int d = 0; d < draws; d++) {
                ID3D11DeviceContext_Draw(ctx, 3, 0);
                if (mode == 2) {
                    ID3D11DeviceContext_Flush(imm);
                }
            }
        }
        if (mode == 1) {
            ID3D11CommandList *cl = NULL;
            ID3D11DeviceContext_FinishCommandList(def, FALSE, &cl);
            ID3D11DeviceContext_ExecuteCommandList(imm, cl, FALSE);
            ID3D11CommandList_Release(cl);
        }
        ID3D11DeviceContext_End(imm, (ID3D11Asynchronous *)t1[f & 1]);
        ID3D11DeviceContext_End(imm, (ID3D11Asynchronous *)dj[f & 1]);
        for (int e = 0; e < 2; e++) {
            Texture_t tex = { eye[e], ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Auto };
            comp->Submit(e ? EVREye_Eye_Right : EVREye_Eye_Left, &tex, NULL, EVRSubmitFlags_Submit_Default);
        }
        comp->PostPresentHandoff();
        cpu_sum += now_ms() - c0;

        Compositor_FrameTiming ft = { sizeof(ft) };
        if (comp->GetFrameTiming(&ft, 0)) {
            pre_sum += ft.m_flPreSubmitGpuMs;
            post_sum += ft.m_flPostSubmitGpuMs;
            total_sum += ft.m_flTotalRenderGpuMs;
            pre_zero += ft.m_flPreSubmitGpuMs == 0.0f;
        }
        if (f) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d;
            UINT64 a = 0, b = 0;
            int p = (f - 1) & 1;
            if (ID3D11DeviceContext_GetData(imm, (ID3D11Asynchronous *)dj[p], &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ID3D11DeviceContext_GetData(imm, (ID3D11Asynchronous *)t0[p], &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ID3D11DeviceContext_GetData(imm, (ID3D11Asynchronous *)t1[p], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
                own_sum += (double)(int64_t)(b - a) * 1000.0 / (double)d.Frequency;
                own_n++;
            }
        }
        frames++;
        if (now_ms() - last_report >= 1000.0) {
            say("%u frames: steamvr app gpu pre %.2f post %.2f total %.2f ms (pre=0 in %u) | own ts %.2f ms (%u read) | "
                "cpu %.2f ms | last idx %u comp gpu %.2f\n",
                frames, pre_sum / frames, post_sum / frames, total_sum / frames, pre_zero, own_n ? own_sum / own_n : 0.0,
                own_n, cpu_sum / frames, ft.m_nFrameIndex, ft.m_flCompositorRenderGpuMs);
            last_report = now_ms();
            frames = own_n = pre_zero = 0;
            own_sum = pre_sum = post_sum = total_sum = cpu_sum = 0;
        }
    }
    shutdown();
    say("done\n");
    return 0;
}
