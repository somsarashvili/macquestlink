#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdarg.h>
#include <stdio.h>
#include "../nvenc_shim/encoder_client.h"

void trace(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

// client_test.exe : renders 30 NV12 frames the way vrlink does (plane render target, MISC_SHARED) and encodes them
// through nvenc_shim/encoder_client.c and a running helper (MQL_DIR / MQL_PORT select another frames.bin / port).
int main(void)
{
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    D3D11_TEXTURE2D_DESC d = { 1216, 4864, 1, 1, DXGI_FORMAT_NV12, { 1, 0 }, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED };
    ID3D11Texture2D *t;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, NULL, &t);
    printf("texture hr=0x%08lx\n", hr);
    if (FAILED(hr)) {
        return 1;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd = { DXGI_FORMAT_R8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D };
    ID3D11RenderTargetView *luma = NULL;
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)t, &rd, &luma);
    printf("luma view hr=0x%08lx\n", hr);
    encoder_client_register(t);
    encoder_output out = { 0 };
    int failed = 0;
    for (int i = 0; i < 30; i++) {
        float v[4] = { (i % 10) / 10.0f, 0, 0, 0 };
        if (luma) {
            ID3D11DeviceContext_ClearRenderTargetView(ctx, luma, v);
        }
        DWORD t0 = GetTickCount();
        int rc = encoder_client_encode(t, i == 0 ? 3 : 0, i * 13889ull, &out);
        failed += rc != 0 || !out.size;
        if (i < 3 || i % 10 == 0) {
            printf("frame %d rc=%d size=%u key=%u %lums\n", i, rc, out.size, out.keyframe, GetTickCount() - t0);
        }
    }
    printf("failed %d\n", failed);
    return failed != 0;
}
