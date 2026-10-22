#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11_4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/mql_protocol.h"
#include "encoder_client.h"

static SRWLOCK g_lock = SRWLOCK_INIT;
static SOCKET g_socket = INVALID_SOCKET;
static uint8_t *g_frames;
static ID3D11Texture2D *g_staging;
static uint32_t g_slot;
static uint32_t g_slices = 1;
static uint32_t g_bitrate_kbps, g_traced_kbps, g_max_bitrate_kbps, g_vbv_bits, g_fps = 90;
volatile long g_stage;
double g_stage_ms[4];

static double now_ms(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

// vrlink retunes its rate control every frame; the helper decides which changes reach the encoder.
void encoder_client_set_slices(uint32_t slices)
{
    if (slices != g_slices) {
        trace("slices %u -> %u", g_slices, slices);
        g_slices = slices;
    }
}

void encoder_client_set_rate(uint32_t avg_bps, uint32_t max_bps, uint32_t vbv_bits, uint32_t fps)
{
    char env[32];
    g_max_bitrate_kbps = max_bps / 1000;
    g_vbv_bits = vbv_bits;
    if (fps && fps != g_fps) {
        trace("frame rate %u -> %u", g_fps, fps);
        g_fps = fps;
    }
    uint32_t kbps = avg_bps / 1000;
    if (!kbps || GetEnvironmentVariableA("MQL_BITRATE_KBPS", env, sizeof(env))) {
        return;
    }
    if (kbps * 20 > g_traced_kbps * 21 || kbps * 20 < g_traced_kbps * 19) {
        trace("bitrate %u kbps max %u vbv %u", kbps, g_max_bitrate_kbps, vbv_bits);
        g_traced_kbps = kbps;
    }
    g_bitrate_kbps = kbps;
}

// MQL_DIR and MQL_PORT let tools/client_test.exe run against a separate helper.
static const char *path_of(char *buf, size_t size, const char *name)
{
    char dir[MAX_PATH];
    if (!GetEnvironmentVariableA("MQL_DIR", dir, sizeof(dir))) {
        strcpy(dir, "C:\\macquestlink");
    }
    CreateDirectoryA(dir, NULL);
    snprintf(buf, size, "%s\\%s", dir, name);
    return buf;
}

static uint16_t helper_port(void)
{
    char env[16];
    return GetEnvironmentVariableA("MQL_PORT", env, sizeof(env)) ? (uint16_t)atoi(env) : MQL_PORT;
}

static uint8_t *map_file(const char *name, uint64_t size)
{
    char path[MAX_PATH];
    HANDLE file = CreateFileA(path_of(path, sizeof(path), name), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        trace("%s: CreateFile failed %lu", name, GetLastError());
        return NULL;
    }
    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READWRITE, (DWORD)(size >> 32), (DWORD)size, NULL);
    CloseHandle(file);
    if (!mapping) {
        trace("%s: CreateFileMapping failed %lu", name, GetLastError());
        return NULL;
    }
    uint8_t *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    CloseHandle(mapping);
    if (!view) {
        trace("%s: MapViewOfFile failed %lu", name, GetLastError());
    }
    return view;
}

static uint8_t *map_frames(void)
{
    if (!g_frames) {
        g_frames = map_file("frames.bin", (uint64_t)MQL_SLOT_SIZE * MQL_SLOT_COUNT);
    }
    return g_frames;
}

static void launch_helper(void)
{
    typedef char *(CDECL * unix_name_fn)(const WCHAR *);
    unix_name_fn to_unix = (unix_name_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
    if (!to_unix) {
        return;
    }
    char *path = to_unix(L"C:\\macquestlink\\macquestlink-encoder");
    if (!path) {
        return;
    }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "C:\\windows\\system32\\start.exe /unix \"%s\"", path);
    HeapFree(GetProcessHeap(), 0, path);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        trace("launched encoder helper");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        trace("failed to launch encoder helper: %lu", GetLastError());
    }
}

static SOCKET try_connect(void)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(helper_port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    DWORD timeout_ms = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    return s;
}

static int ensure_connected(void)
{
    if (g_socket != INVALID_SOCKET) {
        return 0;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_socket = try_connect();
    if (g_socket == INVALID_SOCKET && helper_port() == MQL_PORT) {
        launch_helper();
        for (int i = 0; i < 40 && g_socket == INVALID_SOCKET; i++) {
            Sleep(50);
            g_socket = try_connect();
        }
    }
    if (g_socket == INVALID_SOCKET) {
        trace("encoder helper not reachable on port %u", helper_port());
        return -1;
    }
    trace("connected to encoder helper");
    return 0;
}

static int send_full(const void *buf, int len)
{
    const char *p = buf;
    while (len > 0) {
        int n = send(g_socket, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

static int recv_full(void *buf, int len)
{
    char *p = buf;
    while (len > 0) {
        int n = recv(g_socket, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

// The worker thread shares vrlink's immediate context, so the device's own per-call lock has to be on.
void encoder_client_register(void *texture)
{
    ID3D11Device *dev = NULL;
    ID3D11Multithread *mt = NULL;
    ID3D11Texture2D_GetDevice((ID3D11Texture2D *)texture, &dev);
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Multithread, (void **)&mt))) {
        BOOL was = ID3D11Multithread_SetMultithreadProtected(mt, TRUE);
        if (!was) {
            trace("enabled multithread protection on device %p", dev);
        }
        ID3D11Multithread_Release(mt);
    } else {
        trace("device %p has no ID3D11Multithread", dev);
    }
    ID3D11Device_Release(dev);
}

static ID3D11Texture2D *get_staging(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *desc)
{
    if (g_staging) {
        D3D11_TEXTURE2D_DESC sd;
        ID3D11Device *sdev = NULL;
        ID3D11Texture2D_GetDesc(g_staging, &sd);
        ID3D11Texture2D_GetDevice(g_staging, &sdev);
        ID3D11Device_Release(sdev);
        if (sdev == dev && sd.Width == desc->Width && sd.Height == desc->Height && sd.Format == desc->Format) {
            return g_staging;
        }
        ID3D11Texture2D_Release(g_staging);
        g_staging = NULL;
    }
    D3D11_TEXTURE2D_DESC sd = *desc;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &sd, NULL, &g_staging);
    trace("staging %ux%u fmt=%u -> hr=0x%08lx", sd.Width, sd.Height, sd.Format, hr);
    return SUCCEEDED(hr) ? g_staging : NULL;
}

static HRESULT map_staging(ID3D11DeviceContext *ctx, ID3D11Texture2D *staging, D3D11_MAPPED_SUBRESOURCE *mapped)
{
    ID3D11Resource *res = (ID3D11Resource *)staging;
    return ID3D11DeviceContext_Map(ctx, res, 0, D3D11_MAP_READ, 0, mapped);
}

void encoder_client_reset(void)
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_socket != INVALID_SOCKET) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

int encoder_client_encode(void *texture, uint32_t flags, uint64_t timestamp, encoder_output *out)
{
    ID3D11Texture2D *tex = texture;
    out->size = 0;
    out->keyframe = 0;
    if (!tex) {
        return -1;
    }
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(tex, &desc);
    if (desc.Format != DXGI_FORMAT_NV12 && desc.Format != DXGI_FORMAT_P010) {
        trace("unsupported input format %u", desc.Format);
        return -1;
    }

    AcquireSRWLockExclusive(&g_lock);
    int rc = -1;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Texture2D_GetDevice(tex, &dev);
    ID3D11Device_GetImmediateContext(dev, &ctx);

    double t0 = now_ms();
    g_stage = 1;
    uint32_t pitch, chroma_offset, frame_slot = 0;
    ID3D11Texture2D *staging = get_staging(dev, &desc);
    if (!staging) {
        goto done;
    }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
    ID3D11DeviceContext_Flush(ctx);
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = map_staging(ctx, staging, &mapped);
    g_stage = 2;
    double t1 = now_ms();
    g_stage_ms[0] = t1 - t0;
    if (FAILED(hr)) {
        trace("staging map failed hr=0x%08lx", hr);
        goto done;
    }
    pitch = mapped.RowPitch;
    chroma_offset = pitch * desc.Height;
    if (chroma_offset + pitch * (desc.Height / 2) > MQL_SLOT_SIZE || !map_frames() || ensure_connected() != 0) {
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
        goto done;
    }
    frame_slot = g_slot++ % MQL_SLOT_COUNT;
    uint8_t *dst = g_frames + (size_t)frame_slot * MQL_SLOT_SIZE;
    const uint8_t *src = mapped.pData;
    memcpy(dst, src, chroma_offset);
    memcpy(dst + chroma_offset, src + chroma_offset, pitch * (desc.Height / 2));
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    g_stage_ms[1] = now_ms() - t1;
    if (!g_bitrate_kbps) {
        char env[32];
        g_bitrate_kbps = GetEnvironmentVariableA("MQL_BITRATE_KBPS", env, sizeof(env)) ? (uint32_t)atoi(env) : 150000;
    }

    mql_frame_request req = { 0 };
    req.magic = MQL_MAGIC_FRAME;
    req.slot = frame_slot;
    req.width = desc.Width;
    req.height = desc.Height;
    req.pitch = pitch;
    req.chroma_offset = chroma_offset;
    req.ten_bit = desc.Format != DXGI_FORMAT_NV12;
    req.flags = flags;
    req.bitrate_kbps = g_bitrate_kbps;
    req.fps = g_fps;
    req.timestamp = timestamp;
    req.max_bitrate_kbps = g_max_bitrate_kbps;
    req.vbv_bits = g_vbv_bits;
    req.slices = g_slices;

    mql_frame_reply rep;
    double t2 = now_ms();
    g_stage = 3;
    if (send_full(&req, sizeof(req)) || recv_full(&rep, sizeof(rep)) || rep.magic != MQL_MAGIC_OUTPUT ||
        rep.size > MQL_MAX_BITSTREAM) {
        trace("encoder helper connection lost");
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        goto done;
    }
    if (!out->data) {
        out->data = malloc(MQL_MAX_BITSTREAM);
    }
    if (rep.size && recv_full(out->data, (int)rep.size)) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        goto done;
    }
    g_stage_ms[2] = now_ms() - t2;
    out->size = rep.size;
    out->keyframe = rep.keyframe;
    rc = rep.status;
done:
    g_stage = 0;
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    ReleaseSRWLockExclusive(&g_lock);
    return rc;
}
