#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "openvr_capi.h"

typedef intptr_t(OPENVR_FNTABLE_CALLTYPE *init_fn)(EVRInitError *, EVRApplicationType, const char *);
typedef intptr_t(OPENVR_FNTABLE_CALLTYPE *iface_fn)(const char *, EVRInitError *);
typedef void(OPENVR_FNTABLE_CALLTYPE *shutdown_fn)(void);

// Live-edits a SteamVR setting: vrsetting.exe <section> <key> <0|1|float|s:string|remove>
int main(int argc, char **argv)
{
    if (argc < 4) {
        return 2;
    }
    FILE *log = fopen("C:\\macquestlink\\vrsetting.log", "w");
    HMODULE vr = LoadLibraryA("openvr_api.dll");
    init_fn init = (init_fn)GetProcAddress(vr, "VR_InitInternal2");
    iface_fn get_iface = (iface_fn)GetProcAddress(vr, "VR_GetGenericInterface");
    shutdown_fn shutdown = (shutdown_fn)GetProcAddress(vr, "VR_ShutdownInternal");
    EVRInitError err = EVRInitError_VRInitError_None;
    init(&err, EVRApplicationType_VRApplication_Background, NULL);
    char name[128];
    snprintf(name, sizeof(name), "FnTable:%s", IVRSettings_Version);
    struct VR_IVRSettings_FnTable *s = (void *)get_iface(name, &err);
    EVRSettingsError e = 0;
    if (!strcmp(argv[3], "remove")) {
        s->RemoveKeyInSection(argv[1], argv[2], &e);
    } else if (!strncmp(argv[3], "s:", 2)) {
        s->SetString(argv[1], argv[2], argv[3] + 2, &e);
    } else if (strchr(argv[3], '.')) {
        s->SetFloat(argv[1], argv[2], (float)atof(argv[3]), &e);
    } else {
        s->SetBool(argv[1], argv[2], atoi(argv[3]) != 0, &e);
    }
    EVRSettingsError ge = 0;
    fprintf(log, "set %s.%s=%s err=%d now=%d / %f\n", argv[1], argv[2], argv[3], e, s->GetBool(argv[1], argv[2], &ge),
            s->GetFloat(argv[1], argv[2], &ge));
    fclose(log);
    shutdown();
    return 0;
}
