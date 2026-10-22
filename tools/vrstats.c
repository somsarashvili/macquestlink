#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openvr_capi.h"

typedef intptr_t(OPENVR_FNTABLE_CALLTYPE *init_fn)(EVRInitError *, EVRApplicationType, const char *);
typedef intptr_t(OPENVR_FNTABLE_CALLTYPE *iface_fn)(const char *, EVRInitError *);
typedef void(OPENVR_FNTABLE_CALLTYPE *shutdown_fn)(void);

enum { W = 300, H = 500 };

static int exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Small head-locked stats panel, text from C:\macquestlink\stats.txt redrawn once a second; C:\macquestlink\nostats hides
// it. vrstats.exe [x y] places it, in metres at 1 m distance (default lower left).
int main(int argc, char **argv)
{
    float x = argc > 2 ? (float)atof(argv[1]) : -0.32f, y = argc > 2 ? (float)atof(argv[2]) : -0.12f;
    HMODULE vr = LoadLibraryA("openvr_api.dll");
    init_fn init = (init_fn)GetProcAddress(vr, "VR_InitInternal2");
    iface_fn get_iface = (iface_fn)GetProcAddress(vr, "VR_GetGenericInterface");
    shutdown_fn shutdown = (shutdown_fn)GetProcAddress(vr, "VR_ShutdownInternal");
    EVRInitError err = EVRInitError_VRInitError_None;
    init(&err, EVRApplicationType_VRApplication_Overlay, NULL);
    if (err) {
        return 1;
    }
    char name[128];
    snprintf(name, sizeof(name), "FnTable:%s", IVROverlay_Version);
    struct VR_IVROverlay_FnTable *ov = (void *)get_iface(name, &err);
    VROverlayHandle_t h = 0;
    if (ov->CreateOverlay("macquestlink.stats", "macquestlink stats", &h)) {
        return 1;
    }
    HmdMatrix34_t m = { { { 1, 0, 0, x }, { 0, 1, 0, y }, { 0, 0, 1, -1.0f } } };
    ov->SetOverlayTransformTrackedDeviceRelative(h, 0, &m);
    ov->SetOverlayWidthInMeters(h, 0.2f);
    ov->SetOverlaySortOrder(h, 100);

    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB } };
    uint8_t *dib;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&dib, NULL, 0);
    SelectObject(dc, bmp);
    SelectObject(dc, CreateFontA(30, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, FIXED_PITCH,
                                 "Consolas"));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 235, 235));
    static uint8_t rgba[W * H * 4];
    int shown = 0;

    for (;;) {
        struct VREvent_t ev;
        while (ov->PollNextOverlayEvent(h, &ev, sizeof(ev))) {
            if (ev.eventType == EVREventType_VREvent_Quit) {
                shutdown();
                return 0;
            }
        }
        if (exists("C:\\macquestlink\\nostats")) {
            if (shown) {
                ov->HideOverlay(h);
                shown = 0;
            }
            Sleep(1000);
            continue;
        }
        char text[2048] = "no stats";
        FILE *f = fopen("C:\\macquestlink\\stats.txt", "rb");
        if (f) {
            text[fread(text, 1, sizeof(text) - 1, f)] = 0;
            fclose(f);
        }
        memset(dib, 0, W * H * 4);
        RECT r = { 14, 10, W - 10, H - 6 };
        DrawTextA(dc, text, -1, &r, DT_LEFT | DT_TOP | DT_NOPREFIX);
        GdiFlush();
        for (int i = 0; i < W * H; i++) {
            uint8_t *s = dib + i * 4, *d = rgba + i * 4;
            int lit = s[0] | s[1] | s[2];
            d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = lit ? 255 : 170;
        }
        if (ov->SetOverlayRaw(h, rgba, W, H, 4)) {
            break;
        }
        if (!shown) {
            ov->ShowOverlay(h);
            shown = 1;
        }
        Sleep(1000);
    }
    shutdown();
    return 0;
}
