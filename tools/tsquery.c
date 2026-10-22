#define COBJMACROS
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3d11.h>
#include <d3dcompiler.h>

// Checks D3D11 TIMESTAMP queries against wall clock: a heavy pixel-shader workload is bracketed by timestamps,
// and its completion is timed with an event query. Writes C:\macquestlink\tsquery.log.
// Usage: tsquery.exe [draws per frame] [frames] [shader iterations] [CPU-bound pieces] [CPU-bound sleep ms]

static FILE *out;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (out) {
        va_start(ap, fmt);
        vfprintf(out, fmt, ap);
        va_end(ap);
        fflush(out);
    }
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
                             "  return a; }\n";

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

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11RenderTargetView *rtv[2];
static ID3D11Buffer *cb;

static void wait_query(ID3D11Query *q)
{
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, NULL, 0, 0) == S_FALSE) {
        Sleep(0);
    }
}

static int single_pass;

static void work(int draws)
{
    if (single_pass) {
        for (int i = 0; i < draws; i++) {
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        return;
    }
    static const float clear[4] = { 0, 0, 0, 1 };
    for (int i = 0; i < draws; i++) {
        ID3D11RenderTargetView *v = rtv[i & 1];
        if (i < 2) {
            ID3D11DeviceContext_ClearRenderTargetView(ctx, v, clear);
        }
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &v, NULL);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
    }
}

struct frame {
    ID3D11Query *disjoint, *t0, *t1, *ev;
};

static int read_frame(struct frame *f, double *gpu_ms)
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT64 t0 = 0, t1 = 0;
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)f->disjoint, &dj, sizeof(dj), 0) == S_FALSE) {
        Sleep(0);
    }
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)f->t0, &t0, sizeof(t0), 0) == S_FALSE) {
        Sleep(0);
    }
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)f->t1, &t1, sizeof(t1), 0) == S_FALSE) {
        Sleep(0);
    }
    *gpu_ms = (double)(int64_t)(t1 - t0) * 1000.0 / (double)dj.Frequency;
    return dj.Disjoint;
}

static void issue_frame(struct frame *f, int draws, int pre_draws)
{
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)f->disjoint);
    work(pre_draws);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f->t0);
    work(draws);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f->t1);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f->disjoint);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f->ev);
    ID3D11DeviceContext_Flush(ctx);
}

int main(int argc, char **argv)
{
    int draws = argc > 1 ? atoi(argv[1]) : 40;
    int frames = argc > 2 ? atoi(argv[2]) : 20;
    int iters = argc > 3 ? atoi(argv[3]) : 256;
    int pieces = argc > 4 ? atoi(argv[4]) : 4;
    int gap_ms = argc > 5 ? atoi(argv[5]) : 3;
    setvbuf(stdout, NULL, _IONBF, 0);
    out = fopen("C:\\macquestlink\\tsquery.log", "w");

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        say("D3D11CreateDevice hr=0x%08lx\n", hr);
        return 1;
    }
    ID3DBlob *vsb = compile("vs", "vs_5_0"), *psb = compile("ps", "ps_5_0");
    if (!vsb || !psb) {
        return 1;
    }
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);

    D3D11_TEXTURE2D_DESC td = { 2048, 2048, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET, 0, 0 };
    for (int i = 0; i < 2; i++) {
        ID3D11Texture2D *rt = NULL;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt);
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv[i]);
    }
    float k[4] = { 0.1f, 0.2f, 0.3f, (float)iters };
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA sd = { k, 0, 0 };
    ID3D11Device_CreateBuffer(dev, &bd, &sd, &cb);

    struct frame f[2];
    for (int i = 0; i < 2; i++) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        ID3D11Device_CreateQuery(dev, &qd, &f[i].disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        ID3D11Device_CreateQuery(dev, &qd, &f[i].t0);
        ID3D11Device_CreateQuery(dev, &qd, &f[i].t1);
        qd.Query = D3D11_QUERY_EVENT;
        ID3D11Device_CreateQuery(dev, &qd, &f[i].ev);
    }

    D3D11_VIEWPORT vp = { 0, 0, 2048, 2048, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
    D3D11_BLEND_DESC bld = { 0 };
    bld.RenderTarget[0] = (D3D11_RENDER_TARGET_BLEND_DESC){ TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
                                                           D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
                                                           D3D11_COLOR_WRITE_ENABLE_ALL };
    ID3D11BlendState *bs = NULL;
    ID3D11Device_CreateBlendState(dev, &bld, &bs);
    ID3D11DeviceContext_OMSetBlendState(ctx, bs, NULL, 0xffffffff);

    say("draws %d, 2048x2048 RGBA16F, %d-iteration sin loop per pixel, additive blend\n", draws, (int)k[3]);

    say("-- idle GPU: timestamps around the work, wait for each frame\n");
    double sum_gpu = 0, sum_wall = 0;
    for (int i = 0; i < frames; i++) {
        double w0 = now_ms();
        issue_frame(&f[0], draws, 0);
        wait_query(f[0].ev);
        double wall = now_ms() - w0, gpu;
        int disjoint = read_frame(&f[0], &gpu);
        say("frame %2d  ts %8.3f ms  wall %8.3f ms%s\n", i, gpu, wall, disjoint ? "  DISJOINT" : "");
        if (i >= 2) {
            sum_gpu += gpu;
            sum_wall += wall;
        }
    }
    int n = frames > 2 ? frames - 2 : 1;
    say("avg ts %.3f ms, wall %.3f ms\n", sum_gpu / n, sum_wall / n);

    say("-- half the work before t0 (expect ts ~ half of wall)\n");
    sum_gpu = sum_wall = 0;
    for (int i = 0; i < frames; i++) {
        double w0 = now_ms();
        issue_frame(&f[0], draws / 2, draws - draws / 2);
        wait_query(f[0].ev);
        double wall = now_ms() - w0, gpu;
        int disjoint = read_frame(&f[0], &gpu);
        say("frame %2d  ts %8.3f ms  wall %8.3f ms%s\n", i, gpu, wall, disjoint ? "  DISJOINT" : "");
        if (i >= 2) {
            sum_gpu += gpu;
            sum_wall += wall;
        }
    }
    say("avg ts %.3f ms, wall %.3f ms\n", sum_gpu / n, sum_wall / n);

    say("-- pipelined: next frame submitted before waiting on the previous one\n");
    sum_gpu = sum_wall = 0;
    double last = now_ms();
    issue_frame(&f[0], draws, 0);
    for (int i = 0; i < frames; i++) {
        issue_frame(&f[(i + 1) & 1], draws, 0);
        wait_query(f[i & 1].ev);
        double t = now_ms(), wall = t - last, gpu;
        last = t;
        int disjoint = read_frame(&f[i & 1], &gpu);
        say("frame %2d  ts %8.3f ms  interval %8.3f ms%s\n", i, gpu, wall, disjoint ? "  DISJOINT" : "");
        if (i >= 2) {
            sum_gpu += gpu;
            sum_wall += wall;
        }
    }
    wait_query(f[frames & 1].ev);
    say("avg ts %.3f ms, completion interval %.3f ms\n", sum_gpu / n, sum_wall / n);

    say("-- one render pass: same render target bound across the timestamps\n");
    single_pass = 1;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv[0], NULL);
    sum_gpu = sum_wall = 0;
    for (int i = 0; i < frames; i++) {
        double w0 = now_ms();
        issue_frame(&f[0], draws, 0);
        wait_query(f[0].ev);
        double wall = now_ms() - w0, gpu;
        int disjoint = read_frame(&f[0], &gpu);
        say("frame %2d  ts %8.3f ms  wall %8.3f ms%s\n", i, gpu, wall, disjoint ? "  DISJOINT" : "");
        if (i >= 2) {
            sum_gpu += gpu;
            sum_wall += wall;
        }
    }
    say("avg ts %.3f ms, wall %.3f ms\n", sum_gpu / n, sum_wall / n);
    say("-- deferred context: work recorded into a command list, executed between the timestamps\n");
    ID3D11DeviceContext *def = NULL, *imm = ctx;
    ID3D11Device_CreateDeferredContext(dev, 0, &def);
    ID3D11CommandList *cl = NULL;
    sum_gpu = sum_wall = 0;
    for (int i = 0; i < frames; i++) {
        ctx = def;
        single_pass = 0;
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
        ID3D11DeviceContext_OMSetBlendState(ctx, bs, NULL, 0xffffffff);
        work(draws);
        ID3D11DeviceContext_FinishCommandList(ctx, FALSE, &cl);
        ctx = imm;
        double w0 = now_ms();
        ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)f[0].disjoint);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f[0].t0);
        ID3D11DeviceContext_ExecuteCommandList(ctx, cl, FALSE);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f[0].t1);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f[0].disjoint);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f[0].ev);
        ID3D11DeviceContext_Flush(ctx);
        ID3D11CommandList_Release(cl);
        wait_query(f[0].ev);
        double wall = now_ms() - w0, gpu;
        int disjoint = read_frame(&f[0], &gpu);
        say("frame %2d  ts %8.3f ms  wall %8.3f ms%s\n", i, gpu, wall, disjoint ? "  DISJOINT" : "");
        if (i >= 2) {
            sum_gpu += gpu;
            sum_wall += wall;
        }
    }
    say("avg ts %.3f ms, wall %.3f ms\n", sum_gpu / n, sum_wall / n);

    say("-- CPU-bound: the work submitted in %d pieces with %d ms sleeps between the timestamps (expect ts ~ first "
        "pattern, not wall)\n", pieces, gap_ms);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_OMSetBlendState(ctx, bs, NULL, 0xffffffff);
    single_pass = 0;
    sum_gpu = sum_wall = 0;
    for (int i = 0; i < frames; i++) {
        double w0 = now_ms();
        ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)f[0].disjoint);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f[0].t0);
        for (int p = 0; p < pieces; p++) {
            Sleep(gap_ms);
            work(draws / pieces + (p < draws % pieces));
            ID3D11DeviceContext_Flush(ctx);
        }
        Sleep(gap_ms);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f[0].t1);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f[0].disjoint);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)f[0].ev);
        ID3D11DeviceContext_Flush(ctx);
        wait_query(f[0].ev);
        double wall = now_ms() - w0, gpu;
        int disjoint = read_frame(&f[0], &gpu);
        say("frame %2d  ts %8.3f ms  wall %8.3f ms%s\n", i, gpu, wall, disjoint ? "  DISJOINT" : "");
        if (i >= 2) {
            sum_gpu += gpu;
            sum_wall += wall;
        }
    }
    say("avg ts %.3f ms, wall %.3f ms\n", sum_gpu / n, sum_wall / n);
    if (out) {
        fclose(out);
    }
    return 0;
}
