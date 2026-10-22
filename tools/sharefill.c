#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <stdint.h>
#include <stdio.h>

// Cross-process check of how a shared (legacy handle) texture gets its contents: sharefill.exe [fmt] [size] [hold s]
// Fills one shared texture per method (clear, initial data, copy, UpdateSubresource), a child process reads them back.
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

static uint32_t g_px[1024 * 1024];
static UINT g_size = 64;

static ID3D11Texture2D *make(ID3D11Device *dev, DXGI_FORMAT fmt, UINT misc, BOOL init)
{
    D3D11_TEXTURE2D_DESC td = { g_size, g_size, 1, 1, fmt, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, misc };
    D3D11_SUBRESOURCE_DATA data = { g_px, g_size * 4, 0 };
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &td, init ? &data : NULL, &t);
    if (FAILED(hr)) {
        say("create fmt %u misc 0x%x -> 0x%08lx", fmt, misc, hr);
    }
    return t;
}

static uint32_t readback(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *t)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(t, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)t);
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t v = 0xdeadbeef;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        v = ((uint32_t *)((uint8_t *)m.pData + 32 * m.RowPitch))[32];
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    return v;
}

int main(int argc, char **argv)
{
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                      D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (argc > 2 && !strcmp(argv[1], "child")) {
        g_log = fopen("C:\\macquestlink\\sharefill.log", "a");
        for (int i = 2; i < argc; i++) {
            ID3D11Texture2D *t = NULL;
            HRESULT hr = ID3D11Device_OpenSharedResource(dev, (HANDLE)(uintptr_t)strtoull(argv[i], NULL, 0),
                                                         &IID_ID3D11Texture2D, (void **)&t);
            say("child %d: open 0x%08lx pixel %08x", i - 2, hr, t ? readback(dev, ctx, t) : 0);
        }
        return 0;
    }
    g_log = fopen("C:\\macquestlink\\sharefill.log", "w");
    DXGI_FORMAT fmt = argc > 1 ? atoi(argv[1]) : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    g_size = argc > 2 ? atoi(argv[2]) : 64;
    for (int i = 0; i < 1024 * 1024; i++) {
        g_px[i] = 0xff00ff00;
    }
    static const float green[4] = { 0, 1, 0, 1 };
    ID3D11Texture2D *t[5];
    t[0] = make(dev, fmt, D3D11_RESOURCE_MISC_SHARED, FALSE);
    ID3D11RenderTargetView *rtv;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)t[0], NULL, &rtv);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, green);
    t[1] = make(dev, fmt, D3D11_RESOURCE_MISC_SHARED, TRUE);
    t[2] = make(dev, fmt, D3D11_RESOURCE_MISC_SHARED, FALSE);
    ID3D11Texture2D *src = make(dev, DXGI_FORMAT_R8G8B8A8_UNORM, 0, TRUE);
    D3D11_BOX box = { 0, 0, 0, g_size, g_size, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)t[2], 0, 0, 0, 0, (ID3D11Resource *)src, 0, &box);
    t[3] = make(dev, fmt, D3D11_RESOURCE_MISC_SHARED, FALSE);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)t[3], 0, NULL, g_px, g_size * 4, 0);
    t[4] = make(dev, fmt, D3D11_RESOURCE_MISC_SHARED, FALSE);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)t[4], (ID3D11Resource *)t[0]);
    ID3D11DeviceContext_Flush(ctx);
    Sleep(200);
    static const char *names[] = { "clear", "initdata", "copyregion", "update", "copy of shared" };
    char cmd[512];
    int n = snprintf(cmd, sizeof(cmd), "C:\\macquestlink\\sharefill.exe child");
    for (int i = 0; i < 5; i++) {
        IDXGIResource *r;
        HANDLE h = NULL;
        ID3D11Texture2D_QueryInterface(t[i], &IID_IDXGIResource, (void **)&r);
        IDXGIResource_GetSharedHandle(r, &h);
        say("%d %-15s local pixel %08x handle %p", i, names[i], readback(dev, ctx, t[i]), h);
        n += snprintf(cmd + n, sizeof(cmd) - n, " %llu", (unsigned long long)(uintptr_t)h);
    }
    fclose(g_log);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    WaitForSingleObject(pi.hProcess, 30000);
    if (argc > 3) {
        Sleep(atoi(argv[3]) * 1000);
    }
    return 0;
}
