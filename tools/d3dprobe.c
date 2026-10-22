#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

static void print_module(const char *name)
{
    char path[MAX_PATH] = "(not loaded)";
    HMODULE m = GetModuleHandleA(name);
    if (m) {
        GetModuleFileNameA(m, path, sizeof(path));
    }
    printf("%-10s %s\n", name, path);
}

static HANDLE g_last_handle;

static void report_shared(IUnknown *res, UINT misc)
{
    IDXGIResource1 *r = NULL;
    HANDLE h = NULL;
    HRESULT hr = IUnknown_QueryInterface(res, &IID_IDXGIResource1, (void **)&r);
    if (FAILED(hr)) {
        IDXGIResource *r0 = NULL;
        printf("%-34s hr=0x%08lx\n", "  -> QI IDXGIResource1", hr);
        if (SUCCEEDED(IUnknown_QueryInterface(res, &IID_IDXGIResource, (void **)&r0))) {
            hr = IDXGIResource_GetSharedHandle(r0, &h);
            printf("%-34s hr=0x%08lx handle=%p\n", "  -> IDXGIResource::GetSharedHandle", hr, h);
            IDXGIResource_Release(r0);
        }
        return;
    }
    if (misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
        hr = IDXGIResource1_CreateSharedHandle(r, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h);
    } else {
        hr = IDXGIResource1_GetSharedHandle(r, &h);
    }
    printf("%-34s hr=0x%08lx handle=%p\n", "  -> shared handle", hr, h);
    g_last_handle = h;
    IDXGIResource1_Release(r);
}

static void print_owner(const char *label, void *fn)
{
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)fn, &m)) {
        GetModuleFileNameA(m, path, sizeof(path));
    }
    printf("%-10s %p in %s\n", label, fn, path);
}

static void try_texture(ID3D11Device *dev, const char *label, UINT samples, UINT misc)
{
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = 2048;
    d.Height = 2048;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = samples;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = misc;
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, NULL, &t);
    printf("%-34s hr=0x%08lx\n", label, hr);
    if (SUCCEEDED(hr) && (misc & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE))) {
        report_shared((IUnknown *)t, misc);
    }
    if (t) {
        ID3D11Texture2D_Release(t);
    }
}

static void try_buffer(ID3D11Device *dev, const char *label, UINT misc)
{
    D3D11_BUFFER_DESC d = { 0 };
    d.ByteWidth = 256;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = (misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_CONSTANT_BUFFER;
    d.MiscFlags = misc;
    ID3D11Buffer *b = NULL;
    HRESULT hr = ID3D11Device_CreateBuffer(dev, &d, NULL, &b);
    printf("%-34s hr=0x%08lx\n", label, hr);
    if (b && (misc & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE))) {
        report_shared((IUnknown *)b, misc);
    }
    if (b) {
        ID3D11Buffer_Release(b);
    }
}

static const GUID IID_IDXMTPlanarTexture = { 0x6d1f2a3b, 0x8c4e, 0x4f5a, { 0x9b, 0x7d, 0x2e, 0x1c, 0x0a, 0x9f, 0x8b, 0x31 } };

typedef struct IDXMTPlanarTextureVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(void *);
    ULONG(STDMETHODCALLTYPE *Release)(void *);
    HRESULT(STDMETHODCALLTYPE *GetPlaneLayout)(void *, void **, UINT *, UINT *);
} IDXMTPlanarTextureVtbl;

typedef struct IDXMTPlanarTexture {
    const IDXMTPlanarTextureVtbl *lpVtbl;
} IDXMTPlanarTexture;

static void planar_test(ID3D11Device *dev, DXGI_FORMAT fmt, DXGI_FORMAT luma_fmt, DXGI_FORMAT chroma_fmt)
{
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = 1216;
    d.Height = 4864;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, NULL, &t);
    printf("planar fmt=%d create          hr=0x%08lx\n", fmt, hr);
    if (FAILED(hr)) {
        return;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd = { 0 };
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView *luma = NULL, *chroma = NULL;
    rd.Format = luma_fmt;
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)t, &rd, &luma);
    printf("  luma RTV                   hr=0x%08lx\n", hr);
    rd.Format = chroma_fmt;
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)t, &rd, &chroma);
    printf("  chroma RTV                 hr=0x%08lx\n", hr);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = { 0 };
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ud.Format = chroma_fmt;
    ID3D11UnorderedAccessView *uav = NULL;
    hr = ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)t, &ud, &uav);
    printf("  chroma UAV                 hr=0x%08lx\n", hr);
    if (!luma || !chroma) {
        return;
    }
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    const float lv[4] = { 0.5f, 0, 0, 0 }, cv[4] = { 0.25f, 0.75f, 0, 0 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, luma, lv);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, chroma, cv);
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ID3D11Query *q = NULL;
    ID3D11Device_CreateQuery(dev, &qd, &q);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_Flush(ctx);
    BOOL done = FALSE;
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, &done, sizeof(done), 0) != S_OK) {
        Sleep(1);
    }
    ID3D11Texture2D *copy = NULL;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &copy);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)copy, (ID3D11Resource *)t);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q);
    ID3D11DeviceContext_Flush(ctx);
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, &done, sizeof(done), 0) != S_OK) {
        Sleep(1);
    }
    printf("  (reading CopyResource destination)\n");
    t = copy;
    IDXMTPlanarTexture *pt = NULL;
    hr = ID3D11Texture2D_QueryInterface(t, &IID_IDXMTPlanarTexture, (void **)&pt);
    printf("  QI IDXMTPlanarTexture      hr=0x%08lx\n", hr);
    if (FAILED(hr)) {
        return;
    }
    void *data;
    UINT pitch, chroma_off;
    hr = pt->lpVtbl->GetPlaneLayout(pt, &data, &pitch, &chroma_off);
    printf("  layout hr=0x%08lx pitch=%u chroma_offset=%u\n", hr, pitch, chroma_off);
    if (fmt == DXGI_FORMAT_NV12) {
        UINT8 *b = data;
        printf("  luma[0]=%u luma[last]=%u chroma[0]=%u,%u\n", b[0], b[pitch * (d.Height - 1) + d.Width - 1],
               b[chroma_off], b[chroma_off + 1]);
    } else {
        UINT16 *y = data, *uv = (UINT16 *)((char *)data + chroma_off);
        printf("  luma[0]=0x%04x chroma[0]=0x%04x,0x%04x\n", y[0], uv[0], uv[1]);
    }
}

static UINT read_texel(ID3D11Device *dev, ID3D11Texture2D *t)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &st);
    ID3D11DeviceContext *ctx;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)t);
    D3D11_MAPPED_SUBRESOURCE m;
    UINT v = 0xdeadbeef;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        v = *(UINT *)m.pData;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    return v;
}

static int child_write_texture(ID3D11Device *dev, HANDLE h, int nt)
{
    ID3D11Texture2D *t = NULL;
    HRESULT hr;
    if (nt) {
        ID3D11Device1 *dev1 = NULL;
        ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&dev1);
        hr = ID3D11Device1_OpenSharedResource1(dev1, h, &IID_ID3D11Texture2D, (void **)&t);
    } else {
        hr = ID3D11Device_OpenSharedResource(dev, h, &IID_ID3D11Texture2D, (void **)&t);
    }
    printf("child open texture(%p)       hr=0x%08lx texel-before=0x%08x\n", h, hr, t ? read_texel(dev, t) : 0);
    if (FAILED(hr)) {
        return 1;
    }
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)t, NULL, &rtv);
    ID3D11DeviceContext *ctx;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    const float green[4] = { 0, 1, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, green);
    printf("child wrote green, reads back 0x%08x\n", read_texel(dev, t));
    return 0;
}

static void cross_process_texture(ID3D11Device *dev, UINT misc)
{
    D3D11_TEXTURE2D_DESC d = { 256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, misc };
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, NULL, &t);
    if (FAILED(hr)) {
        printf("shared texture create hr=0x%08lx\n", hr);
        return;
    }
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)t, NULL, &rtv);
    ID3D11DeviceContext *ctx;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    const float red[4] = { 1, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, red);
    printf("parent texel after red clear: 0x%08x\n", read_texel(dev, t));
    report_shared((IUnknown *)t, misc);
    char exe[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    HANDLE h = g_last_handle;
    if (misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
        HANDLE dup = NULL;
        STARTUPINFOA si0 = { sizeof(si0) };
        (void)si0;
        SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        dup = h;
        snprintf(cmd, sizeof(cmd), "\"%s\" childtex %p 1", exe, dup);
    } else {
        snprintf(cmd, sizeof(cmd), "\"%s\" childtex %p 0", exe, h);
    }
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    fflush(stdout);
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    printf("parent texel after child wrote: 0x%08x (green=0xff00ff00)\n", read_texel(dev, t));
}

static int child_read(ID3D11Device *dev, HANDLE h)
{
    ID3D11Buffer *shared = NULL;
    HRESULT hr = ID3D11Device_OpenSharedResource(dev, h, &IID_ID3D11Buffer, (void **)&shared);
    printf("child OpenSharedResource(%p)  hr=0x%08lx\n", h, hr);
    if (FAILED(hr)) {
        return 1;
    }
    D3D11_BUFFER_DESC d;
    ID3D11Buffer_GetDesc(shared, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Buffer *staging = NULL;
    ID3D11Device_CreateBuffer(dev, &d, NULL, &staging);
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)shared);
    D3D11_MAPPED_SUBRESOURCE m;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m);
    printf("child read                   hr=0x%08lx value=0x%08x\n", hr, SUCCEEDED(hr) ? *(UINT *)m.pData : 0);
    return 0;
}

static void cross_process(ID3D11Device *dev)
{
    UINT data[64] = { 0xC0FFEE01 };
    D3D11_BUFFER_DESC d = { 0 };
    d.ByteWidth = sizeof(data);
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
    ID3D11Buffer *b = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &d, &init, &b))) {
        return;
    }
    report_shared((IUnknown *)b, d.MiscFlags);
    if (!g_last_handle) {
        return;
    }
    char exe[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    snprintf(cmd, sizeof(cmd), "\"%s\" child %p", exe, g_last_handle);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    fflush(stdout);
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

int main(int argc, char **argv)
{
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    DXGI_ADAPTER_DESC1 desc;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)) ||
        FAILED(IDXGIFactory1_EnumAdapters1(factory, 0, &adapter))) {
        printf("no DXGI adapter\n");
        return 1;
    }
    IDXGIAdapter1_GetDesc1(adapter, &desc);
    printf("adapter    %ls vendor=0x%04x device=0x%04x vram=%lluMB\n", desc.Description, desc.VendorId,
           desc.DeviceId, (unsigned long long)desc.DedicatedVideoMemory >> 20);

    ID3D11Device *dev = NULL;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, NULL);
    printf("device     hr=0x%08lx feature_level=0x%x\n", hr, fl);
    if (argc > 3 && !strcmp(argv[1], "childtex")) {
        return child_write_texture(dev, (HANDLE)(uintptr_t)strtoull(argv[2], NULL, 16), atoi(argv[3]));
    }
    if (argc > 2 && !strcmp(argv[1], "child")) {
        return SUCCEEDED(hr) ? child_read(dev, (HANDLE)(uintptr_t)strtoull(argv[2], NULL, 16)) : 1;
    }
    print_module("d3d11.dll");
    print_module("dxgi.dll");
    print_module("winevulkan.dll");
    print_owner("CreateBuf", (void *)dev->lpVtbl->CreateBuffer);
    print_owner("D3D11Dev", (void *)GetProcAddress(GetModuleHandleA("d3d11.dll"), "D3D11CreateDevice"));
    if (FAILED(hr)) {
        return 1;
    }

    UINT q = 0;
    ID3D11Device_CheckMultisampleQualityLevels(dev, DXGI_FORMAT_R8G8B8A8_UNORM, 8, &q);
    printf("msaa8 quality levels: %u\n", q);
    try_texture(dev, "texture 2048 msaa8", 8, 0);
    try_texture(dev, "texture 2048 MISC_SHARED", 1, D3D11_RESOURCE_MISC_SHARED);
    try_texture(dev, "texture 2048 SHARED_NTHANDLE", 1,
                D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
    try_buffer(dev, "cbuffer plain", 0);
    try_buffer(dev, "cbuffer MISC_SHARED", D3D11_RESOURCE_MISC_SHARED);
    try_buffer(dev, "buffer SHARED_NTHANDLE", D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
    try_buffer(dev, "buffer SHARED_NTHANDLE|KEYEDMUTEX",
               D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
    planar_test(dev, DXGI_FORMAT_P010, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM);
    planar_test(dev, DXGI_FORMAT_NV12, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM);
    printf("--- cross-process texture (KMT handle)\n");
    cross_process_texture(dev, D3D11_RESOURCE_MISC_SHARED);
    printf("--- cross-process texture (NT handle + keyed mutex)\n");
    cross_process_texture(dev, D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
    printf("--- cross-process shared cbuffer\n");
    cross_process(dev);
    return 0;
}
