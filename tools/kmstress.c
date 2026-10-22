// Cross-process keyed-mutex ping-pong: the parent renders frame i into a shared texture (after `clears` filler
// copies) and releases key 1; the child acquires key 1, copies the texture (directly and through a private copy) and
// releases key 0 without waiting, then checks both copies hold frame i. Counts stale/in-progress reads.
// Usage: kmstress.exe [iters] [clears] [flags]; flags bit 0: producer Flush before ReleaseSync,
// bit 1: consumer Flush before ReleaseSync, bit 2: consumer burns GPU time before its copies, bit 3: child writes and
// exits right after ReleaseSync, bit 4: sync-texture mode (3 data textures in rotation), bit 5: sleep 1 ms between frames, bit 6 (sync mode): the
// child only acquires/releases on one device and reads on a second one, bit 7: the child polls
// AcquireSync with timeout 0, bit 8: filler copies are 1-pixel copies (CPU-heavy, GPU-light).
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#define W 2048
#define H 2048
#define RING 3

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

static ID3D11Device1 *create(ID3D11DeviceContext **ctx)
{
    ID3D11Device *dev = NULL;
    ID3D11Device1 *dev1 = NULL;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, ctx))) {
        return NULL;
    }
    ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&dev1);
    ID3D11Device_Release(dev);
    return dev1;
}

static ID3D11Texture2D *tex(ID3D11Device1 *dev, UINT w, UINT h, D3D11_USAGE usage, UINT bind, UINT misc)
{
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = usage;
    d.BindFlags = bind;
    d.MiscFlags = misc;
    d.CPUAccessFlags = usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ : 0;
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device1_CreateTexture2D(dev, &d, NULL, &t);
    if (FAILED(hr)) {
        lg("CreateTexture2D hr=0x%08lx", hr);
    }
    return t;
}

static void color(unsigned i, float *c)
{
    c[0] = (i & 255) / 255.0f;
    c[1] = ((i >> 8) & 255) / 255.0f;
    c[2] = 128 / 255.0f;
    c[3] = 1;
}

static UINT32 packed(unsigned i)
{
    return 0xFF800000u | (((i >> 8) & 255) << 8) | (i & 255);
}

static const float c_filler[4] = { 0, 0, 64 / 255.0f, 1 };

static int g_small;

static void burn(ID3D11DeviceContext *ctx, ID3D11Texture2D *dst, ID3D11Texture2D *src, int n)
{
    for (int k = 0; k < n; k++) {
        if (g_small) {
            D3D11_BOX b = { k % 64, 0, 0, k % 64 + 1, 1, 1 };
            ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)dst, 0, k % 64, 1, 0, (ID3D11Resource *)src, 0, &b);
        } else {
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)dst, (ID3D11Resource *)src);
        }
    }
}

static ID3D11Texture2D *filled(ID3D11Device1 *dev, ID3D11DeviceContext *ctx)
{
    ID3D11Texture2D *t = tex(dev, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0);
    ID3D11RenderTargetView *rtv;
    ID3D11Device1_CreateRenderTargetView(dev, (ID3D11Resource *)t, NULL, &rtv);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c_filler);
    ID3D11RenderTargetView_Release(rtv);
    return t;
}

static UINT32 readpx(ID3D11DeviceContext *ctx, ID3D11Texture2D *s)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m))) {
        return 0xDEADBEEF;
    }
    UINT32 v = *(UINT32 *)m.pData;
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s, 0);
    return v;
}

static volatile LONG *shared_counter(void)
{
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "Local\\kmstress_ctr");
    return m ? MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 4096) : NULL;
}

// Sync-texture pattern (as OpenVR uses it): the data texture is shared without a keyed mutex, a separate texture's
// keyed mutex (key 0 on both sides) orders the devices. The child must never see a frame older than the counter the
// parent published after its ReleaseSync.
static int child_sync(HANDLE *hdata, int clears, int flags)
{
    ID3D11DeviceContext *ctx;
    ID3D11Device1 *dev = create(&ctx);
    ID3D11Texture2D *t = NULL, *d[RING] = { 0 };
    HRESULT hr = ID3D11Device1_OpenSharedResourceByName(dev, L"Local\\kmstress", DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                        &IID_ID3D11Texture2D, (void **)&t);
    for (int k = 0; k < RING && SUCCEEDED(hr); k++) {
        hr = ID3D11Device1_OpenSharedResource(dev, hdata[k], &IID_ID3D11Texture2D, (void **)&d[k]);
    }
    if (FAILED(hr)) {
        lg("open hr=0x%08lx", hr);
        return 1;
    }
    volatile LONG *ctr = shared_counter();
    IDXGIKeyedMutex *km;
    ID3D11Texture2D_QueryInterface(t, &IID_IDXGIKeyedMutex, (void **)&km);
    if (flags & 64) {
        // Acquire/release on this device only to sync (as SteamVR drivers do with the sync texture), read on another.
        ID3D11Device1_Release(dev);
        dev = create(&ctx);
        for (int k = 0; k < RING && SUCCEEDED(hr); k++) {
            hr = ID3D11Device1_OpenSharedResource(dev, hdata[k], &IID_ID3D11Texture2D, (void **)&d[k]);
        }
        if (FAILED(hr)) {
            lg("open on second device hr=0x%08lx", hr);
            return 1;
        }
    }
    ID3D11Texture2D *priv = tex(dev, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0);
    ID3D11Texture2D *src = filled(dev, ctx);
    ID3D11Texture2D *s1 = tex(dev, 1, 1, D3D11_USAGE_STAGING, 0, 0), *s2 = tex(dev, 1, 1, D3D11_USAGE_STAGING, 0, 0);
    D3D11_BOX px = { W / 2, H / 2, 0, W / 2 + 1, H / 2 + 1, 1 };
    int n = 0, ok = 0, stale = 0, filler = 0, timeouts = 0;
    while (*ctr >= 0) {
        LONG c = *ctr;
        if (flags & 128) {
            for (int k = 0; k < 200000 && (hr = IDXGIKeyedMutex_AcquireSync(km, 0, 0)) == WAIT_TIMEOUT; k++) {
                SwitchToThread();
            }
            c = *ctr;
        } else {
            hr = IDXGIKeyedMutex_AcquireSync(km, 0, 2000);
        }
        if (hr != S_OK) {
            timeouts++;
            continue;
        }
        if (flags & 64) {
            IDXGIKeyedMutex_ReleaseSync(km, 0);
        }
        if (flags & 4) {
            burn(ctx, priv, src, clears);
        }
        ID3D11Texture2D *cur = d[c % RING];
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)s1, 0, 0, 0, 0, (ID3D11Resource *)cur, 0, &px);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)priv, (ID3D11Resource *)cur);
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)s2, 0, 0, 0, 0, (ID3D11Resource *)priv, 0, &px);
        if (flags & 2) {
            ID3D11DeviceContext_Flush(ctx);
        }
        if (!(flags & 64)) {
            IDXGIKeyedMutex_ReleaseSync(km, 0);
        }
        UINT32 v[2] = { readpx(ctx, s1), readpx(ctx, s2) };
        n++;
        int bad = 0;
        for (int k = 0; k < 2; k++) {
            LONG idx = (v[k] & 255) | ((v[k] >> 8) & 255) << 8;
            if (v[k] == 0xFF400000u) {
                filler++, bad = 1;
            } else if (c > 0 && idx != (c & 0xFFFF)) {
                stale++, bad = 1;
            }
            if (bad && stale + filler < 10) {
                lg("read %d: counter %ld, %s %08x", n, c, k ? "via copy" : "direct", v[k]);
            }
        }
        ok += !bad;
        if (flags & 32) {
            Sleep(1);
        }
    }
    lg("RESULT sync mode: reads %d ok %d stale %d filler %d timeouts %d", n, ok, stale, filler, timeouts);
    return stale + filler ? 2 : 0;
}

static int child(int iters, int clears, int flags)
{
    ID3D11DeviceContext *ctx;
    ID3D11Device1 *dev = create(&ctx);
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device1_OpenSharedResourceByName(dev, L"Local\\kmstress", DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                        &IID_ID3D11Texture2D, (void **)&t);
    if (FAILED(hr)) {
        lg("OpenSharedResourceByName hr=0x%08lx", hr);
        return 1;
    }
    IDXGIKeyedMutex *km;
    ID3D11Texture2D_QueryInterface(t, &IID_IDXGIKeyedMutex, (void **)&km);
    ID3D11Texture2D *priv = tex(dev, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0);
    ID3D11Texture2D *src = filled(dev, ctx);
    ID3D11Texture2D *s1 = tex(dev, 1, 1, D3D11_USAGE_STAGING, 0, 0), *s2 = tex(dev, 1, 1, D3D11_USAGE_STAGING, 0, 0);
    D3D11_BOX px = { W / 2, H / 2, 0, W / 2 + 1, H / 2 + 1, 1 };
    int ok = 0, stale1 = 0, stale2 = 0, filler1 = 0, filler2 = 0, other = 0, timeouts = 0;
    for (int i = 1; i <= iters; i++) {
        hr = IDXGIKeyedMutex_AcquireSync(km, 1, 2000);
        if (hr != S_OK) {
            timeouts++;
            lg("AcquireSync(1) #%d hr=0x%08lx", i, hr);
            break;
        }
        if (flags & 8) {
            ID3D11RenderTargetView *rtv;
            const float green[4] = { 0, 1, 0, 1 };
            ID3D11Device1_CreateRenderTargetView(dev, (ID3D11Resource *)t, NULL, &rtv);
            burn(ctx, t, src, clears);
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, green);
            IDXGIKeyedMutex_ReleaseSync(km, 0);
            lg("released after writing green, exiting");
            ExitProcess(0);
        }
        if (flags & 4) {
            burn(ctx, priv, src, clears);
        }
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)s1, 0, 0, 0, 0, (ID3D11Resource *)t, 0, &px);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)priv, (ID3D11Resource *)t);
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)s2, 0, 0, 0, 0, (ID3D11Resource *)priv, 0, &px);
        if (flags & 2) {
            ID3D11DeviceContext_Flush(ctx);
        }
        IDXGIKeyedMutex_ReleaseSync(km, 0);
        UINT32 a = readpx(ctx, s1), b = readpx(ctx, s2), want = packed(i);
        if (a == want && b == want) {
            ok++;
        } else {
            if (a != want) {
                a == 0xFF400000u ? filler1++ : a == packed(i - 1) ? stale1++ : other++;
            }
            if (b != want) {
                b == 0xFF400000u ? filler2++ : b == packed(i - 1) ? stale2++ : other++;
            }
            if (stale1 + stale2 + filler1 + filler2 + other < 10) {
                lg("#%d want %08x direct %08x via copy %08x", i, want, a, b);
            }
        }
    }
    lg("RESULT iters %d ok %d direct stale %d filler %d, via copy stale %d filler %d, other %d, timeouts %d", iters, ok,
       stale1, filler1, stale2, filler2, other, timeouts);
    return ok == iters ? 0 : 2;
}

int main(int argc, char **argv)
{
    GetModuleFileNameA(NULL, g_log, sizeof(g_log));
    char *dot = strrchr(g_log, '.');
    strcpy(dot ? dot : g_log + strlen(g_log), ".log");
    if (argc >= 4 + RING && !strcmp(argv[1], "childsync")) {
        HANDLE h[RING];
        g_role = "child";
        for (int k = 0; k < RING; k++) {
            h[k] = (HANDLE)(UINT_PTR)_strtoui64(argv[4 + k], NULL, 16);
        }
        return child_sync(h, atoi(argv[2]), atoi(argv[3]));
    }
    if (argc > 4 && !strcmp(argv[1], "child")) {
        g_role = "child";
        return child(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    }
    if (argc > 1 && !strncmp(argv[1], "child", 5)) {
        return 100;
    }
    int iters = argc > 1 ? atoi(argv[1]) : 500, clears = argc > 2 ? atoi(argv[2]) : 50, flags = argc > 3 ? atoi(argv[3]) : 0;
    g_small = flags & 256;
    DeleteFileA(g_log);
    ID3D11DeviceContext *ctx;
    ID3D11Device1 *dev = create(&ctx);
    if (!dev) {
        lg("no device");
        return 1;
    }
    ID3D11Texture2D *t = tex(dev, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                             D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
    IDXGIKeyedMutex *km;
    IDXGIResource1 *r;
    HANDLE h = NULL;
    ID3D11Texture2D_QueryInterface(t, &IID_IDXGIKeyedMutex, (void **)&km);
    ID3D11Texture2D_QueryInterface(t, &IID_IDXGIResource1, (void **)&r);
    IDXGIResource1_CreateSharedHandle(r, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, L"Local\\kmstress", &h);
    ID3D11RenderTargetView *rtv;
    ID3D11Device1_CreateRenderTargetView(dev, (ID3D11Resource *)t, NULL, &rtv);
    ID3D11Texture2D *src = filled(dev, ctx);
    ID3D11Texture2D *d[RING] = { 0 };
    ID3D11RenderTargetView *drtv[RING] = { 0 };
    HANDLE hdata[RING] = { 0 };
    volatile LONG *ctr = NULL;
    if (flags & 16) {
        for (int k = 0; k < RING; k++) {
            IDXGIResource *dr;
            d[k] = tex(dev, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_SHARED);
            ID3D11Texture2D_QueryInterface(d[k], &IID_IDXGIResource, (void **)&dr);
            IDXGIResource_GetSharedHandle(dr, &hdata[k]);
            ID3D11Device1_CreateRenderTargetView(dev, (ID3D11Resource *)d[k], NULL, &drtv[k]);
        }
        ctr = shared_counter();
        *ctr = 0;
    }

    char exe[MAX_PATH], cmd[2 * MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (flags & 16) {
        snprintf(cmd, sizeof(cmd), "\"%s\" childsync %d %d %p %p %p", exe, clears, flags, hdata[0], hdata[1], hdata[2]);
    } else {
        snprintf(cmd, sizeof(cmd), "\"%s\" child %d %d %d", exe, iters, clears, flags);
    }
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        lg("CreateProcess err=%lu", GetLastError());
        return 1;
    }

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int i;
    double release_ms = 0;
    for (i = 1; i <= iters; i++) {
        HRESULT hr = IDXGIKeyedMutex_AcquireSync(km, 0, 2000);
        if (hr != S_OK) {
            lg("AcquireSync(0) #%d hr=0x%08lx", i, hr);
            break;
        }
        float c[4];
        color(i, c);
        burn(ctx, ctr ? d[i % RING] : t, src, clears);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, ctr ? drtv[i % RING] : rtv, c);
        if (flags & 1) {
            ID3D11DeviceContext_Flush(ctx);
        }
        LARGE_INTEGER r0, r1;
        QueryPerformanceCounter(&r0);
        IDXGIKeyedMutex_ReleaseSync(km, ctr ? 0 : 1);
        QueryPerformanceCounter(&r1);
        release_ms += (r1.QuadPart - r0.QuadPart) * 1000.0 / f.QuadPart;
        if (ctr) {
            *ctr = i;
            if (flags & 32) {
                Sleep(1);
            }
        }
    }
    if (ctr) {
        *ctr = -1;
    }
    if (flags & 8) {
        QueryPerformanceCounter(&t0);
        HRESULT hr = IDXGIKeyedMutex_AcquireSync(km, 0, 5000);
        QueryPerformanceCounter(&t1);
        lg("AcquireSync(0) after child exit hr=0x%08lx %.1f ms", hr, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        ID3D11Texture2D *s1 = tex(dev, 1, 1, D3D11_USAGE_STAGING, 0, 0);
        D3D11_BOX px = { W / 2, H / 2, 0, W / 2 + 1, H / 2 + 1, 1 };
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)s1, 0, 0, 0, 0, (ID3D11Resource *)t, 0, &px);
        UINT32 v = readpx(ctx, s1);
        QueryPerformanceCounter(&t0);
        lg("RESULT after child exit: pixel %08x (green ff00ff00) after %.1f ms", v, (t0.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart);
    }
    QueryPerformanceCounter(&t1);
    lg("producer %d frames, %.3f ms/frame, ReleaseSync %.3f ms", i - 1, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / (i - 1),
       release_ms / (i - 1));
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 999;
    GetExitCodeProcess(pi.hProcess, &code);
    lg("child exit %lu", code);
    return code;
}
