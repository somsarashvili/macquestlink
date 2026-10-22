// d3dshare.exe : D3D11 checks for the streaming path, results in d3dshare.log next to the exe. 4: adapters (vendor
// spoof), 3: NV12/P010 texture and staging support, 1: cross-process sharing (NT handle + keyed mutex, legacy KMT
// handle) with a child process.
#define COBJMACROS
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#define RED 0xFFFF0000u
#define GREEN 0xFF00FF00u
#define BLUE 0xFF0000FFu
#define YELLOW 0xFFFFFF00u
#define BAD 0xDEADBEEFu

static const float c_red[4] = { 1, 0, 0, 1 }, c_green[4] = { 0, 1, 0, 1 }, c_blue[4] = { 0, 0, 1, 1 },
                   c_yellow[4] = { 1, 1, 0, 1 };

static char g_log[MAX_PATH];
static const char *g_role = "parent";

static void lg(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("[%s] %s\n", g_role, buf);
    fflush(stdout);
    FILE *f = fopen(g_log, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", g_role, buf);
        fclose(f);
    }
}

static void result(const char *name, int ok, const char *detail)
{
    lg("RESULT %-34s %s %s", name, ok ? "PASS" : "FAIL", detail);
}

static void print_module(const char *name)
{
    char path[MAX_PATH] = "(not loaded)";
    HMODULE m = GetModuleHandleA(name);
    if (m) {
        GetModuleFileNameA(m, path, sizeof(path));
    }
    lg("module %-10s %s", name, path);
}

static ID3D11Device1 *create_d3d11(ID3D11DeviceContext **ctx)
{
    ID3D11Device *dev = NULL;
    ID3D11Device1 *dev1 = NULL;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                   D3D11_SDK_VERSION, &dev, NULL, ctx);
    if (FAILED(hr)) {
        fl = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                               D3D11_SDK_VERSION, &dev, NULL, ctx);
    }
    lg("D3D11CreateDevice hr=0x%08lx", hr);
    if (FAILED(hr)) {
        return NULL;
    }
    hr = ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&dev1);
    lg("QI ID3D11Device1 hr=0x%08lx", hr);
    ID3D11Device_Release(dev);
    return dev1;
}

static ID3D11Texture2D *create_tex(ID3D11Device1 *dev, UINT misc)
{
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = 256;
    d.Height = 256;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = misc;
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device1_CreateTexture2D(dev, &d, NULL, &t);
    lg("CreateTexture2D misc=0x%x hr=0x%08lx", misc, hr);
    return t;
}

static void clear(ID3D11Device1 *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *t, const float *c)
{
    ID3D11RenderTargetView *rtv = NULL;
    HRESULT hr = ID3D11Device1_CreateRenderTargetView(dev, (ID3D11Resource *)t, NULL, &rtv);
    if (FAILED(hr)) {
        lg("CreateRenderTargetView hr=0x%08lx", hr);
        return;
    }
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
    ID3D11DeviceContext_Flush(ctx);
    ID3D11RenderTargetView_Release(rtv);
}

static void gpu_wait(ID3D11Device1 *dev, ID3D11DeviceContext *ctx)
{
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ID3D11Query *q = NULL;
    if (FAILED(ID3D11Device1_CreateQuery(dev, &qd, &q))) {
        return;
    }
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_Flush(ctx);
    BOOL done = FALSE;
    for (int i = 0; i < 2000 && ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, &done, sizeof(done), 0) != S_OK; i++) {
        Sleep(1);
    }
    ID3D11Query_Release(q);
}

static UINT32 read_pixel(ID3D11Device1 *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *t)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *s = NULL;
    HRESULT hr = ID3D11Device1_CreateTexture2D(dev, &d, NULL, &s);
    if (FAILED(hr)) {
        lg("staging create hr=0x%08lx", hr);
        return BAD;
    }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s, (ID3D11Resource *)t);
    D3D11_MAPPED_SUBRESOURCE m;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m);
    UINT32 px = BAD;
    if (SUCCEEDED(hr)) {
        px = ((UINT32 *)((BYTE *)m.pData + 128 * m.RowPitch))[128];
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s, 0);
    } else {
        lg("staging Map hr=0x%08lx", hr);
    }
    ID3D11Texture2D_Release(s);
    return px;
}

static HANDLE dup_from_parent(DWORD pid, HANDLE h)
{
    HANDLE parent = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid), out = NULL;
    if (!parent || !DuplicateHandle(parent, h, GetCurrentProcess(), &out, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        lg("DuplicateHandle from parent failed err=%lu", GetLastError());
        out = NULL;
    }
    if (parent) {
        CloseHandle(parent);
    }
    return out;
}

static DWORD spawn(const char *args)
{
    char exe[MAX_PATH], cmd[2 * MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, args);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        lg("CreateProcess failed err=%lu", GetLastError());
        return 999;
    }
    DWORD code = 998;
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        lg("child timeout");
        TerminateProcess(pi.hProcess, 997);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

static int child_keyed(ID3D11Device1 *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *t, const char *how)
{
    char name[64], detail[128];
    IDXGIKeyedMutex *km = NULL;
    HRESULT hr = ID3D11Texture2D_QueryInterface(t, &IID_IDXGIKeyedMutex, (void **)&km);
    snprintf(name, sizeof(name), "1 child %s QI KeyedMutex", how);
    snprintf(detail, sizeof(detail), "hr=0x%08lx", hr);
    result(name, SUCCEEDED(hr), detail);
    if (FAILED(hr)) {
        return 1;
    }
    hr = IDXGIKeyedMutex_AcquireSync(km, 1, 2000);
    snprintf(name, sizeof(name), "1 child %s AcquireSync(1)", how);
    snprintf(detail, sizeof(detail), "hr=0x%08lx", hr);
    result(name, hr == S_OK, detail);
    UINT32 px = read_pixel(dev, ctx, t);
    snprintf(name, sizeof(name), "1 child %s pixel==red", how);
    snprintf(detail, sizeof(detail), "got 0x%08x", px);
    result(name, px == RED, detail);
    clear(dev, ctx, t, c_green);
    if (getenv("D3DSHARE_GPUWAIT")) {
        gpu_wait(dev, ctx);
    }
    hr = IDXGIKeyedMutex_ReleaseSync(km, 0);
    lg("ReleaseSync(0) hr=0x%08lx", hr);
    IDXGIKeyedMutex_Release(km);
    return px == RED ? 0 : 2;
}

static int child_nt(DWORD pid, HANDLE ph)
{
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device1 *dev = create_d3d11(&ctx);
    if (!dev) {
        return 10;
    }
    char detail[128];
    HANDLE h = dup_from_parent(pid, ph);
    ID3D11Texture2D *t = NULL, *tn = NULL;
    HRESULT hr = ID3D11Device1_OpenSharedResource1(dev, h, &IID_ID3D11Texture2D, (void **)&t);
    snprintf(detail, sizeof(detail), "hr=0x%08lx handle=%p", hr, h);
    result("1 child OpenSharedResource1", SUCCEEDED(hr), detail);
    hr = ID3D11Device1_OpenSharedResourceByName(dev, L"Local\\d3dshare_tex", DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                &IID_ID3D11Texture2D, (void **)&tn);
    snprintf(detail, sizeof(detail), "hr=0x%08lx", hr);
    result("1 child OpenSharedResourceByName", SUCCEEDED(hr), detail);
    if (t) {
        return child_keyed(dev, ctx, t, "nt");
    }
    if (tn) {
        return child_keyed(dev, ctx, tn, "byname");
    }
    return 11;
}

static int child_kmt(HANDLE h)
{
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device1 *dev = create_d3d11(&ctx);
    if (!dev) {
        return 10;
    }
    char detail[128];
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device1_OpenSharedResource(dev, h, &IID_ID3D11Texture2D, (void **)&t);
    snprintf(detail, sizeof(detail), "hr=0x%08lx handle=%p", hr, h);
    result("1b child OpenSharedResource(KMT)", SUCCEEDED(hr), detail);
    if (!t) {
        return 11;
    }
    UINT32 px = read_pixel(dev, ctx, t);
    snprintf(detail, sizeof(detail), "got 0x%08x", px);
    result("1b child pixel==blue", px == BLUE, detail);
    clear(dev, ctx, t, c_yellow);
    gpu_wait(dev, ctx);
    return px == BLUE ? 0 : 2;
}

static void test1(ID3D11Device1 *dev, ID3D11DeviceContext *ctx)
{
    char detail[160], args[128];
    ID3D11Texture2D *t = create_tex(dev, D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
    if (!t) {
        result("1 create NTHANDLE|KEYEDMUTEX tex", 0, "");
        return;
    }
    IDXGIKeyedMutex *km = NULL;
    IDXGIResource1 *r = NULL;
    HANDLE h = NULL;
    HRESULT hr = ID3D11Texture2D_QueryInterface(t, &IID_IDXGIKeyedMutex, (void **)&km);
    lg("QI KeyedMutex hr=0x%08lx", hr);
    hr = km ? IDXGIKeyedMutex_AcquireSync(km, 0, 2000) : E_NOINTERFACE;
    lg("parent AcquireSync(0) hr=0x%08lx", hr);
    clear(dev, ctx, t, c_red);
    hr = km ? IDXGIKeyedMutex_ReleaseSync(km, 1) : E_NOINTERFACE;
    lg("parent ReleaseSync(1) hr=0x%08lx", hr);
    hr = ID3D11Texture2D_QueryInterface(t, &IID_IDXGIResource1, (void **)&r);
    if (r) {
        hr = IDXGIResource1_CreateSharedHandle(r, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                               L"Local\\d3dshare_tex", &h);
    }
    snprintf(detail, sizeof(detail), "hr=0x%08lx handle=%p", hr, h);
    result("1 CreateSharedHandle(named)", SUCCEEDED(hr) && h, detail);
    snprintf(args, sizeof(args), "c1 %lu %p", GetCurrentProcessId(), h);
    DWORD code = spawn(args);
    lg("child c1 exit code %lu", code);
    hr = km ? IDXGIKeyedMutex_AcquireSync(km, 0, 2000) : E_NOINTERFACE;
    snprintf(detail, sizeof(detail), "hr=0x%08lx", hr);
    result("1 parent AcquireSync(0) after child", hr == S_OK, detail);
    UINT32 px = read_pixel(dev, ctx, t);
    snprintf(detail, sizeof(detail), "got 0x%08x", px);
    result("1 parent sees child green", px == GREEN, detail);
    if (km) {
        IDXGIKeyedMutex_ReleaseSync(km, 0);
        IDXGIKeyedMutex_Release(km);
    }

    ID3D11Texture2D *tk = create_tex(dev, D3D11_RESOURCE_MISC_SHARED);
    if (!tk) {
        result("1b create MISC_SHARED tex", 0, "");
        return;
    }
    IDXGIResource *r0 = NULL;
    HANDLE hk = NULL;
    clear(dev, ctx, tk, c_blue);
    gpu_wait(dev, ctx);
    hr = ID3D11Texture2D_QueryInterface(tk, &IID_IDXGIResource, (void **)&r0);
    if (r0) {
        hr = IDXGIResource_GetSharedHandle(r0, &hk);
    }
    snprintf(detail, sizeof(detail), "hr=0x%08lx handle=%p", hr, hk);
    result("1b GetSharedHandle(KMT)", SUCCEEDED(hr) && hk, detail);
    snprintf(args, sizeof(args), "c1k %p", hk);
    code = spawn(args);
    lg("child c1k exit code %lu", code);
    px = read_pixel(dev, ctx, tk);
    snprintf(detail, sizeof(detail), "got 0x%08x", px);
    result("1b parent sees child yellow", px == YELLOW, detail);
}

static void test3(ID3D11Device1 *dev, ID3D11DeviceContext *ctx)
{
    static const struct { DXGI_FORMAT f; const char *n; } fmts[] = { { DXGI_FORMAT_P010, "P010" }, { DXGI_FORMAT_NV12, "NV12" } };
    char name[64], detail[160];
    for (int i = 0; i < 2; i++) {
        UINT s = 0;
        HRESULT hr = ID3D11Device1_CheckFormatSupport(dev, fmts[i].f, &s);
        snprintf(name, sizeof(name), "3 CheckFormatSupport %s", fmts[i].n);
        snprintf(detail, sizeof(detail), "hr=0x%08lx flags=0x%08x tex2d=%d rt=%d srv=%d", hr, s,
                 !!(s & D3D11_FORMAT_SUPPORT_TEXTURE2D), !!(s & D3D11_FORMAT_SUPPORT_RENDER_TARGET),
                 !!(s & D3D11_FORMAT_SUPPORT_SHADER_LOAD));
        result(name, SUCCEEDED(hr) && (s & D3D11_FORMAT_SUPPORT_TEXTURE2D), detail);
        D3D11_TEXTURE2D_DESC d = { 0 };
        d.Width = 256;
        d.Height = 256;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = fmts[i].f;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D *t = NULL, *s2 = NULL;
        hr = ID3D11Device1_CreateTexture2D(dev, &d, NULL, &t);
        snprintf(name, sizeof(name), "3 create %s RT|SRV", fmts[i].n);
        snprintf(detail, sizeof(detail), "hr=0x%08lx", hr);
        result(name, SUCCEEDED(hr), detail);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = ID3D11Device1_CreateTexture2D(dev, &d, NULL, &s2);
        snprintf(name, sizeof(name), "3 create %s STAGING", fmts[i].n);
        snprintf(detail, sizeof(detail), "hr=0x%08lx", hr);
        result(name, SUCCEEDED(hr), detail);
        if (t && s2) {
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s2, (ID3D11Resource *)t);
        }
        if (s2) {
            D3D11_MAPPED_SUBRESOURCE m;
            hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s2, 0, D3D11_MAP_READ, 0, &m);
            snprintf(name, sizeof(name), "3 Map %s STAGING", fmts[i].n);
            snprintf(detail, sizeof(detail), "hr=0x%08lx rowpitch=%u depthpitch=%u", hr, m.RowPitch, m.DepthPitch);
            result(name, SUCCEEDED(hr), detail);
            if (SUCCEEDED(hr)) {
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s2, 0);
            }
            ID3D11Texture2D_Release(s2);
        }
        if (t) {
            ID3D11Texture2D_Release(t);
        }
    }
}

static void test4(void)
{
    IDXGIFactory1 *f = NULL;
    IDXGIAdapter1 *a = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f);
    lg("CreateDXGIFactory1 hr=0x%08lx", hr);
    for (UINT i = 0; f && IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(a, &d);
        char desc[128];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, desc, sizeof(desc), NULL, NULL);
        lg("RESULT 4 adapter %u '%s' VendorId=0x%04x DeviceId=0x%04x SubSys=0x%08x Rev=%u VRAM=%llu MB flags=0x%x", i,
           desc, d.VendorId, d.DeviceId, d.SubSysId, d.Revision, (unsigned long long)d.DedicatedVideoMemory >> 20, d.Flags);
        IDXGIAdapter1_Release(a);
    }
}

int main(int argc, char **argv)
{
    GetModuleFileNameA(NULL, g_log, sizeof(g_log));
    char *dot = strrchr(g_log, '.');
    strcpy(dot ? dot : g_log + strlen(g_log), ".log");
    if (argc > 1) {
        g_role = argv[1];
        if (!strcmp(argv[1], "c1") && argc > 3) {
            return child_nt(strtoul(argv[2], NULL, 10), (HANDLE)(UINT_PTR)_strtoui64(argv[3], NULL, 16));
        }
        if (!strcmp(argv[1], "c1k") && argc > 2) {
            return child_kmt((HANDLE)(UINT_PTR)_strtoui64(argv[2], NULL, 16));
        }
        return 100;
    }
    DeleteFileA(g_log);
    test4();
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device1 *dev = create_d3d11(&ctx);
    print_module("d3d11.dll");
    print_module("dxgi.dll");
    if (dev) {
        test3(dev, ctx);
        test1(dev, ctx);
    }
    lg("done");
    return 0;
}
