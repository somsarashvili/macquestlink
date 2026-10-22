#define COBJMACROS
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "openvr_capi.h"

typedef intptr_t(OPENVR_FNTABLE_CALLTYPE *init_fn)(EVRInitError *, EVRApplicationType, const char *);
typedef intptr_t(OPENVR_FNTABLE_CALLTYPE *iface_fn)(const char *, EVRInitError *);
typedef void(OPENVR_FNTABLE_CALLTYPE *shutdown_fn)(void);

#define HISTORY 128

static void dump(FILE *log, const Compositor_FrameTiming *t)
{
    fprintf(log,
            "  #%u t=%.4f presents %u mis %u dropped %u reproj 0x%x | pre %.2f post %.2f total %.2f comp gpu %.2f "
            "cpu %.2f idle %.2f | interval %.2f present %.2f waitpresent %.2f submit %.2f | wgp %.2f poses %.2f "
            "ready %.2f upd %.2f-%.2f render %.2f | vsyncs ready %u first %u\n",
            t->m_nFrameIndex, t->m_flSystemTimeInSeconds, t->m_nNumFramePresents, t->m_nNumMisPresented,
            t->m_nNumDroppedFrames, t->m_nReprojectionFlags, t->m_flPreSubmitGpuMs, t->m_flPostSubmitGpuMs,
            t->m_flTotalRenderGpuMs, t->m_flCompositorRenderGpuMs, t->m_flCompositorRenderCpuMs,
            t->m_flCompositorIdleCpuMs, t->m_flClientFrameIntervalMs, t->m_flPresentCallCpuMs,
            t->m_flWaitForPresentCpuMs, t->m_flSubmitFrameMs, t->m_flWaitGetPosesCalledMs, t->m_flNewPosesReadyMs,
            t->m_flNewFrameReadyMs, t->m_flCompositorUpdateStartMs, t->m_flCompositorUpdateEndMs,
            t->m_flCompositorRenderStartMs, t->m_nNumVSyncsReadyForUse, t->m_nNumVSyncsToFirstView);
}

// Prints SteamVR's per-frame timing for the running scene app (averaged over the compositor frames of each second)
// without submitting frames. GetFrameTimings returns the newest N frames, so only frame indices not seen before count.
int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int raw = argc > 2 ? atoi(argv[2]) : 3;
    FILE *log = fopen("C:\\macquestlink\\frametiming.log", "w");
    HMODULE vr = LoadLibraryA("openvr_api.dll");
    init_fn init = (init_fn)GetProcAddress(vr, "VR_InitInternal2");
    iface_fn get_iface = (iface_fn)GetProcAddress(vr, "VR_GetGenericInterface");
    shutdown_fn shutdown = (shutdown_fn)GetProcAddress(vr, "VR_ShutdownInternal");
    EVRInitError err = EVRInitError_VRInitError_None;
    init(&err, EVRApplicationType_VRApplication_Background, NULL);
    if (err) {
        fprintf(log, "init error %d\n", err);
        return 1;
    }
    char name[128];
    snprintf(name, sizeof(name), "FnTable:%s", IVRCompositor_Version);
    struct VR_IVRCompositor_FnTable *comp = (void *)get_iface(name, &err);
    static Compositor_FrameTiming t[HISTORY];
    uint32_t last = 0;
    for (int s = 0; s <= seconds; s++) {
        if (s)
            Sleep(1000);
        memset(t, 0, sizeof(t));
        t[0].m_nSize = sizeof(Compositor_FrameTiming);
        uint32_t n = comp->GetFrameTimings(t, HISTORY);
        double interval = 0, pre = 0, post = 0, total = 0, comp_gpu = 0, cpu = 0, submit = 0;
        uint32_t frames = 0, reproj = 0, mispresented = 0, dropped = 0, presents = 0, newest = last;
        for (uint32_t i = 0; i < n; i++) {
            if (t[i].m_nFrameIndex <= last)
                continue;
            if (t[i].m_nFrameIndex > newest)
                newest = t[i].m_nFrameIndex;
            frames++;
            interval += t[i].m_flClientFrameIntervalMs;
            pre += t[i].m_flPreSubmitGpuMs;
            post += t[i].m_flPostSubmitGpuMs;
            total += t[i].m_flTotalRenderGpuMs;
            comp_gpu += t[i].m_flCompositorRenderGpuMs;
            cpu += t[i].m_flCompositorRenderCpuMs;
            submit += t[i].m_flSubmitFrameMs;
            reproj += t[i].m_nReprojectionFlags != 0;
            mispresented += t[i].m_nNumMisPresented;
            dropped += t[i].m_nNumDroppedFrames;
            presents += t[i].m_nNumFramePresents;
        }
        if (s && frames) {
            fprintf(log,
                    "%u new frames (of %u returned): app interval %.2f ms, app gpu pre/post/total %.2f/%.2f/%.2f ms, "
                    "compositor gpu %.2f cpu %.2f ms, submit %.2f ms, reprojected %u, presents %u, mispresented %u, "
                    "dropped %u\n",
                    frames, n, interval / frames, pre / frames, post / frames, total / frames, comp_gpu / frames,
                    cpu / frames, submit / frames, reproj, presents, mispresented, dropped);
            for (uint32_t i = n > (uint32_t)raw ? n - raw : 0; i < n; i++)
                dump(log, &t[i]);
            fflush(log);
        }
        last = newest;
    }
    fclose(log);
    shutdown();
    return 0;
}
