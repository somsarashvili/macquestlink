#define COBJMACROS
#include <windows.h>
#include <unknwn.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "ffnvcodec/nvEncodeAPI.h"
#include "encoder_client.h"

void audio_hook_attach(void);
void net_hook_attach(void);

#define LOG_DIR  "C:\\macquestlink"
#define LOG_PATH LOG_DIR "\\nvenc_shim.log"

static CRITICAL_SECTION g_log_lock;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static char g_proc[MAX_PATH];

static void log_open(void)
{
    char path[MAX_PATH];
    InitializeCriticalSection(&g_log_lock);
    CreateDirectoryA(LOG_DIR, NULL);
    g_log = CreateFileA(LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    GetModuleFileNameA(NULL, path, sizeof(path));
    const char *base = strrchr(path, '\\');
    snprintf(g_proc, sizeof(g_proc), "%s", base ? base + 1 : path);
}

void trace(const char *fmt, ...)
{
    char buf[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d [%s:%lu] ", t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds, g_proc, GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2) {
        n = sizeof(buf) - 2;
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_log_lock);
    DWORD written;
    WriteFile(g_log, buf, n, &written, NULL);
    LeaveCriticalSection(&g_log_lock);
}

static const char *guid_name(const GUID *g)
{
    if (IsEqualGUID(g, &NV_ENC_CODEC_H264_GUID)) return "H264";
    if (IsEqualGUID(g, &NV_ENC_CODEC_HEVC_GUID)) return "HEVC";
    if (IsEqualGUID(g, &NV_ENC_CODEC_AV1_GUID)) return "AV1";
    if (IsEqualGUID(g, &NV_ENC_HEVC_PROFILE_MAIN_GUID)) return "HEVC_MAIN";
    if (IsEqualGUID(g, &NV_ENC_HEVC_PROFILE_MAIN10_GUID)) return "HEVC_MAIN10";
    if (IsEqualGUID(g, &NV_ENC_H264_PROFILE_HIGH_GUID)) return "H264_HIGH";
    if (IsEqualGUID(g, &NV_ENC_PRESET_P1_GUID)) return "P1";
    if (IsEqualGUID(g, &NV_ENC_PRESET_P4_GUID)) return "P4";
    if (IsEqualGUID(g, &NV_ENC_PRESET_P7_GUID)) return "P7";
    static char buf[64];
    snprintf(buf, sizeof(buf), "{%08lx-%04x-%04x-...}", g->Data1, g->Data2, g->Data3);
    return buf;
}

static const GUID *const k_codecs[] = { &NV_ENC_CODEC_HEVC_GUID, &NV_ENC_CODEC_H264_GUID };
static const GUID *const k_hevc_profiles[] = { &NV_ENC_HEVC_PROFILE_MAIN_GUID, &NV_ENC_HEVC_PROFILE_MAIN10_GUID };
static const GUID *const k_h264_profiles[] = { &NV_ENC_H264_PROFILE_BASELINE_GUID, &NV_ENC_H264_PROFILE_MAIN_GUID,
                                               &NV_ENC_H264_PROFILE_HIGH_GUID };
static const GUID *const k_presets[] = { &NV_ENC_PRESET_P1_GUID, &NV_ENC_PRESET_P2_GUID, &NV_ENC_PRESET_P3_GUID,
                                         &NV_ENC_PRESET_P4_GUID, &NV_ENC_PRESET_P5_GUID, &NV_ENC_PRESET_P6_GUID,
                                         &NV_ENC_PRESET_P7_GUID };
static const NV_ENC_BUFFER_FORMAT k_formats[] = { NV_ENC_BUFFER_FORMAT_NV12, NV_ENC_BUFFER_FORMAT_ARGB,
                                                  NV_ENC_BUFFER_FORMAT_ABGR, NV_ENC_BUFFER_FORMAT_YUV420_10BIT,
                                                  NV_ENC_BUFFER_FORMAT_ABGR10 };

#define COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

static uint32_t copy_guids(GUID *out, uint32_t cap, const GUID *const *src, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && i < cap; i++) {
        out[i] = *src[i];
    }
    return i;
}

static const GUID *const *profiles_for(GUID codec, uint32_t *n)
{
    if (IsEqualGUID(&codec, &NV_ENC_CODEC_HEVC_GUID)) {
        *n = COUNT(k_hevc_profiles);
        return k_hevc_profiles;
    }
    *n = COUNT(k_h264_profiles);
    return k_h264_profiles;
}

static int caps_value(NV_ENC_CAPS cap)
{
    switch (cap) {
    case NV_ENC_CAPS_NUM_MAX_BFRAMES: return 0;
    case NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES: return 0x3f;
    case NV_ENC_CAPS_SUPPORT_FIELD_ENCODING: return 0;
    case NV_ENC_CAPS_SUPPORT_MONOCHROME: return 0;
    case NV_ENC_CAPS_SUPPORT_FMO: return 0;
    case NV_ENC_CAPS_SUPPORT_QPELMV: return 1;
    case NV_ENC_CAPS_SUPPORT_BDIRECT_MODE: return 0;
    case NV_ENC_CAPS_SUPPORT_CABAC: return 1;
    case NV_ENC_CAPS_SUPPORT_ADAPTIVE_TRANSFORM: return 1;
    case NV_ENC_CAPS_SUPPORT_STEREO_MVC: return 0;
    case NV_ENC_CAPS_NUM_MAX_TEMPORAL_LAYERS: return 1;
    case NV_ENC_CAPS_SUPPORT_HIERARCHICAL_PFRAMES: return 0;
    case NV_ENC_CAPS_SUPPORT_HIERARCHICAL_BFRAMES: return 0;
    case NV_ENC_CAPS_LEVEL_MAX: return 62;
    case NV_ENC_CAPS_LEVEL_MIN: return 1;
    case NV_ENC_CAPS_SEPARATE_COLOUR_PLANE: return 0;
    case NV_ENC_CAPS_WIDTH_MAX: return 8192;
    case NV_ENC_CAPS_HEIGHT_MAX: return 8192;
    case NV_ENC_CAPS_SUPPORT_TEMPORAL_SVC: return 0;
    case NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE: return 1;
    case NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE: return 1;
    case NV_ENC_CAPS_SUPPORT_DYN_FORCE_CONSTQP: return 1;
    case NV_ENC_CAPS_SUPPORT_DYN_RCMODE_CHANGE: return 0;
    case NV_ENC_CAPS_SUPPORT_SUBFRAME_READBACK: return 0;
    case NV_ENC_CAPS_SUPPORT_CONSTRAINED_ENCODING: return 1;
    case NV_ENC_CAPS_SUPPORT_INTRA_REFRESH: return 1;
    case NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE: return 1;
    case NV_ENC_CAPS_SUPPORT_DYNAMIC_SLICE_MODE: return 1;
    case NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION: return 1;
    case NV_ENC_CAPS_PREPROC_SUPPORT: return 0;
    case NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT: return 1;
    case NV_ENC_CAPS_MB_NUM_MAX: return (8192 / 16) * (8192 / 16);
    case NV_ENC_CAPS_MB_PER_SEC_MAX: return 4096 * 2304 * 120 / 256;
    case NV_ENC_CAPS_SUPPORT_YUV444_ENCODE: return 0;
    case NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE: return 0;
    case NV_ENC_CAPS_SUPPORT_SAO: return 1;
    case NV_ENC_CAPS_SUPPORT_MEONLY_MODE: return 0;
    case NV_ENC_CAPS_SUPPORT_LOOKAHEAD: return 0;
    case NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ: return 0;
    case NV_ENC_CAPS_SUPPORT_10BIT_ENCODE: return 1;
    case NV_ENC_CAPS_NUM_MAX_LTR_FRAMES: return 0;
    case NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION: return 0;
    case NV_ENC_CAPS_DYNAMIC_QUERY_ENCODER_CAPACITY: return 100;
    case NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE: return 0;
    case NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP: return 0;
    case NV_ENC_CAPS_WIDTH_MIN: return 145;
    case NV_ENC_CAPS_HEIGHT_MIN: return 49;
    case NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES: return 1;
    case NV_ENC_CAPS_SUPPORT_ALPHA_LAYER_ENCODING: return 0;
    case NV_ENC_CAPS_NUM_ENCODER_ENGINES: return 1;
    default: return 0;
    }
}

typedef struct session {
    uint32_t width;
    uint32_t height;
    uint64_t frames;
} session;

typedef struct handle {
    void *resource;
    NV_ENC_BUFFER_FORMAT format;
    encoder_output output;
    uint64_t timestamp;
    uint32_t frame_idx;
    volatile LONG ready;
    volatile LONG dead;
} handle;

// vrlink drives NVENC in async mode, so EncodePicture only queues; a worker encodes in submission order
// and signals the completion event the way the hardware encoder would.
typedef struct job {
    IUnknown *texture;
    handle *out;
    HANDLE event;
    uint32_t flags;
    uint64_t timestamp;
    LARGE_INTEGER queued;
    struct job *next;
} job;

static CRITICAL_SECTION g_queue_lock;
static CONDITION_VARIABLE g_queue_ready;
static CONDITION_VARIABLE g_queue_idle;
static job *g_queue_head;
static job *g_queue_tail;
static LONG g_queue_pending;
static HANDLE g_worker;
static uint64_t g_encoded;

static volatile DWORD g_last_done;

static DWORD WINAPI watchdog(void *arg)
{
    BOOL reported = FALSE;
    for (;;) {
        Sleep(500);
        BOOL stuck = g_queue_pending > 0 && GetTickCount() - g_last_done > 1500;
        if (stuck && !reported) {
            trace("worker stuck for %lums in stage %ld (1=gpu wait 2=copy 3=helper) queue=%ld",
                  GetTickCount() - g_last_done, g_stage, g_queue_pending);
        } else if (!stuck && reported) {
            trace("worker resumed");
        }
        reported = stuck;
    }
    return 0;
}

static DWORD WINAPI encode_worker(void *arg)
{
    for (;;) {
        EnterCriticalSection(&g_queue_lock);
        while (!g_queue_head) {
            SleepConditionVariableCS(&g_queue_ready, &g_queue_lock, INFINITE);
        }
        job *j = g_queue_head;
        g_queue_head = j->next;
        if (!g_queue_head) {
            g_queue_tail = NULL;
        }
        LeaveCriticalSection(&g_queue_lock);

        if (j->texture) {
            static double sum[4], max[4];
            LARGE_INTEGER start, end, freq;
            QueryPerformanceCounter(&start);
            QueryPerformanceFrequency(&freq);
            DWORD t0 = GetTickCount();
            int rc = encoder_client_encode(j->texture, j->flags, j->timestamp, &j->out->output);
            g_encoded++;
            QueryPerformanceCounter(&end);
            double st[4] = { (start.QuadPart - j->queued.QuadPart) * 1000.0 / freq.QuadPart, g_stage_ms[0],
                             g_stage_ms[1] + g_stage_ms[2],
                             (end.QuadPart - j->queued.QuadPart) * 1000.0 / freq.QuadPart };
            for (int i = 0; i < 4; i++) {
                sum[i] += st[i];
                max[i] = max[i] > st[i] ? max[i] : st[i];
            }
            if (g_encoded % 450 == 0) {
                trace("timing avg/max ms: queued %.1f/%.1f gpu %.1f/%.1f copy+helper %.1f/%.1f total %.1f/%.1f "
                      "(last copy %.1f helper %.1f)", sum[0] / 450, max[0], sum[1] / 450, max[1], sum[2] / 450, max[2],
                      sum[3] / 450, max[3], g_stage_ms[1], g_stage_ms[2]);
                memset(sum, 0, sizeof(sum));
                memset(max, 0, sizeof(max));
            }
            if (g_encoded <= 5 || g_encoded % 450 == 0 || rc) {
                trace("encoded #%llu flags=0x%x rc=%d size=%u key=%u %lums queue=%ld", g_encoded, j->flags, rc,
                      j->out->output.size, j->out->output.keyframe, GetTickCount() - t0, g_queue_pending);
            }
            IUnknown_Release(j->texture);
        }
        if (j->out) {
            InterlockedExchange(&j->out->ready, 1);
        }
        if (j->event) {
            SetEvent(j->event);
        }
        free(j);
        g_last_done = GetTickCount();

        EnterCriticalSection(&g_queue_lock);
        if (--g_queue_pending == 0) {
            WakeAllConditionVariable(&g_queue_idle);
        }
        LeaveCriticalSection(&g_queue_lock);
    }
    return 0;
}

static void queue_job(job *j)
{
    EnterCriticalSection(&g_queue_lock);
    if (!g_worker) {
        g_last_done = GetTickCount();
        g_worker = CreateThread(NULL, 0, encode_worker, NULL, 0, NULL);
        CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
        SetThreadPriority(g_worker, THREAD_PRIORITY_ABOVE_NORMAL);
    }
    if (g_queue_tail) {
        g_queue_tail->next = j;
    } else {
        g_queue_head = j;
    }
    g_queue_tail = j;
    g_queue_pending++;
    WakeConditionVariable(&g_queue_ready);
    LeaveCriticalSection(&g_queue_lock);
}

static void drain_queue(void)
{
    EnterCriticalSection(&g_queue_lock);
    while (g_queue_pending) {
        SleepConditionVariableCS(&g_queue_idle, &g_queue_lock, INFINITE);
    }
    LeaveCriticalSection(&g_queue_lock);
}

static uint8_t g_empty_bitstream[16];

// vrlink can still lock a bitstream buffer after destroying it during an encoder reset; the handle is kept
// (only its data freed) so that call gets an error instead of reading freed memory.
static void retire_handle(handle *h)
{
    if (h) {
        InterlockedExchange(&h->dead, 1);
        h->output.size = 0;
        free(h->output.data);
        h->output.data = NULL;
    }
}

static NVENCSTATUS NVENCAPI open_session_ex(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS *p, void **encoder)
{
    trace("OpenEncodeSessionEx ver=0x%08x deviceType=%d device=%p apiVersion=0x%08x", p->version, p->deviceType,
         p->device, p->apiVersion);
    *encoder = calloc(1, sizeof(session));
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI open_session(void *device, uint32_t deviceType, void **encoder)
{
    trace("OpenEncodeSession deviceType=%u device=%p", deviceType, device);
    *encoder = calloc(1, sizeof(session));
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_guid_count(void *enc, uint32_t *n)
{
    *n = COUNT(k_codecs);
    trace("GetEncodeGUIDCount -> %u", *n);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_guids(void *enc, GUID *out, uint32_t cap, uint32_t *n)
{
    *n = copy_guids(out, cap, k_codecs, COUNT(k_codecs));
    trace("GetEncodeGUIDs cap=%u -> %u", cap, *n);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_profile_count(void *enc, GUID codec, uint32_t *n)
{
    profiles_for(codec, n);
    trace("GetEncodeProfileGUIDCount %s -> %u", guid_name(&codec), *n);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_profiles(void *enc, GUID codec, GUID *out, uint32_t cap, uint32_t *n)
{
    uint32_t total;
    const GUID *const *src = profiles_for(codec, &total);
    *n = copy_guids(out, cap, src, total);
    trace("GetEncodeProfileGUIDs %s -> %u", guid_name(&codec), *n);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_input_format_count(void *enc, GUID codec, uint32_t *n)
{
    *n = COUNT(k_formats);
    trace("GetInputFormatCount %s -> %u", guid_name(&codec), *n);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_input_formats(void *enc, GUID codec, NV_ENC_BUFFER_FORMAT *out, uint32_t cap,
                                              uint32_t *n)
{
    uint32_t i;
    for (i = 0; i < COUNT(k_formats) && i < cap; i++) {
        out[i] = k_formats[i];
    }
    *n = i;
    trace("GetInputFormats %s -> %u", guid_name(&codec), *n);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_caps(void *enc, GUID codec, NV_ENC_CAPS_PARAM *p, int *value)
{
    *value = caps_value(p->capsToQuery);
    trace("GetEncodeCaps %s cap=%d -> %d", guid_name(&codec), p->capsToQuery, *value);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_preset_count(void *enc, GUID codec, uint32_t *n)
{
    *n = COUNT(k_presets);
    trace("GetEncodePresetCount %s -> %u", guid_name(&codec), *n);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_preset_guids(void *enc, GUID codec, GUID *out, uint32_t cap, uint32_t *n)
{
    *n = copy_guids(out, cap, k_presets, COUNT(k_presets));
    trace("GetEncodePresetGUIDs %s -> %u", guid_name(&codec), *n);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_preset_config(void *enc, GUID codec, GUID preset, NV_ENC_PRESET_CONFIG *cfg)
{
    trace("GetEncodePresetConfig %s preset=%s ver=0x%08x", guid_name(&codec), guid_name(&preset), cfg->version);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_preset_config_ex(void *enc, GUID codec, GUID preset, NV_ENC_TUNING_INFO tuning,
                                                 NV_ENC_PRESET_CONFIG *cfg)
{
    trace("GetEncodePresetConfigEx %s preset=%s tuning=%d ver=0x%08x", guid_name(&codec), guid_name(&preset),
         tuning, cfg->version);
    return NV_ENC_SUCCESS;
}

static void log_rc(const char *what, const NV_ENC_INITIALIZE_PARAMS *ip)
{
    static volatile LONG budget = 40;
    const NV_ENC_RC_PARAMS *rc = &ip->encodeConfig->rcParams;
    if (InterlockedDecrement(&budget) >= 0) {
        trace("%s %ux%u rc mode=%d avg=%u max=%u vbv=%u init=%u qp=%u/%u/%u aq=%u lookahead=%u multipass=%d tq=%u",
              what, ip->encodeWidth, ip->encodeHeight, rc->rateControlMode, rc->averageBitRate, rc->maxBitRate,
              rc->vbvBufferSize, rc->vbvInitialDelay, rc->constQP.qpIntra, rc->constQP.qpInterP, rc->constQP.qpInterB,
              rc->enableAQ, rc->enableLookahead, rc->multiPass, rc->targetQuality);
        const NV_ENC_CONFIG *c = ip->encodeConfig;
        const NV_ENC_CONFIG_HEVC *h = &c->encodeCodecConfig.hevcConfig;
        trace("%s gop=%u pint=%d idr=%u ir=%u/%u/%u ltr=%u/%u/%u dpb=%u refl0=%d slice=%u/%u cip=%u rpsei=%u", what,
              c->gopLength, c->frameIntervalP, h->idrPeriod, h->enableIntraRefresh, h->intraRefreshPeriod,
              h->intraRefreshCnt, h->enableLTR, h->ltrNumFrames, h->ltrTrustMode, h->maxNumRefFramesInDPB, h->numRefL0,
              h->sliceMode, h->sliceModeData, h->useConstrainedIntraPred, h->outputRecoveryPointSEI);
    }
}

static uint32_t g_slices = 1;

static void apply_slices(const NV_ENC_INITIALIZE_PARAMS *ip)
{
    const NV_ENC_CONFIG_HEVC *h = &ip->encodeConfig->encodeCodecConfig.hevcConfig;
    g_slices = h->sliceMode == 3 && h->sliceModeData > 1 && h->sliceModeData <= 16 ? h->sliceModeData : 1;
    if (GetFileAttributesA(LOG_DIR "\\noslices") != INVALID_FILE_ATTRIBUTES)
        g_slices = 1;
    encoder_client_set_slices(g_slices);
}

static NVENCSTATUS NVENCAPI initialize(void *enc, NV_ENC_INITIALIZE_PARAMS *p)
{
    session *s = enc;
    s->width = p->encodeWidth;
    s->height = p->encodeHeight;
    trace("InitializeEncoder ver=0x%08x codec=%s preset=%s %ux%u fps=%u/%u async=%u ptd=%u", p->version,
         guid_name(&p->encodeGUID), guid_name(&p->presetGUID), p->encodeWidth, p->encodeHeight, p->frameRateNum,
         p->frameRateDen, p->enableEncodeAsync, p->enablePTD);
    if (p->encodeConfig) {
        log_rc("init", p);
        apply_slices(p);
        const NV_ENC_RC_PARAMS *rc = &p->encodeConfig->rcParams;
        encoder_client_set_rate(rc->averageBitRate, rc->maxBitRate, rc->vbvBufferSize,
                                p->frameRateDen ? (p->frameRateNum + p->frameRateDen / 2) / p->frameRateDen : 0);
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI reconfigure(void *enc, NV_ENC_RECONFIGURE_PARAMS *p)
{
    if (p->reInitEncodeParams.encodeConfig) {
        log_rc(p->forceIDR ? "reconf idr" : "reconf", &p->reInitEncodeParams);
        apply_slices(&p->reInitEncodeParams);
        const NV_ENC_RC_PARAMS *rc = &p->reInitEncodeParams.encodeConfig->rcParams;
        const NV_ENC_INITIALIZE_PARAMS *ip = &p->reInitEncodeParams;
        encoder_client_set_rate(rc->averageBitRate, rc->maxBitRate, rc->vbvBufferSize,
                                ip->frameRateDen ? (ip->frameRateNum + ip->frameRateDen / 2) / ip->frameRateDen : 0);
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI create_input_buffer(void *enc, NV_ENC_CREATE_INPUT_BUFFER *p)
{
    handle *h = calloc(1, sizeof(handle));
    h->format = p->bufferFmt;
    p->inputBuffer = h;
    trace("CreateInputBuffer %ux%u fmt=0x%x", p->width, p->height, p->bufferFmt);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI destroy_input_buffer(void *enc, NV_ENC_INPUT_PTR buf)
{
    trace("DestroyInputBuffer %p", buf);
    free(buf);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI create_bitstream_buffer(void *enc, NV_ENC_CREATE_BITSTREAM_BUFFER *p)
{
    p->bitstreamBuffer = calloc(1, sizeof(handle));
    trace("CreateBitstreamBuffer ver=0x%08x -> %p", p->version, p->bitstreamBuffer);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI destroy_bitstream_buffer(void *enc, NV_ENC_OUTPUT_PTR buf)
{
    trace("DestroyBitstreamBuffer %p", buf);
    drain_queue();
    retire_handle(buf);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI encode_picture(void *enc, NV_ENC_PIC_PARAMS *p)
{
    session *s = enc;
    handle *in = p->inputBuffer;
    handle *out = p->outputBitstream;
    s->frames++;
    job *j = calloc(1, sizeof(job));
    j->event = p->completionEvent;
    j->out = out;
    j->timestamp = p->inputTimeStamp;
    QueryPerformanceCounter(&j->queued);
    if (out) {
        out->output.size = 0;
        out->timestamp = p->inputTimeStamp;
        out->frame_idx = p->frameIdx;
        InterlockedExchange(&out->ready, 0);
    }
    if (in && out && in->resource) {
        if (p->encodePicFlags & (NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_FORCEINTRA)) {
            j->flags |= 1;
        }
        if (p->encodePicFlags & NV_ENC_PIC_FLAG_OUTPUT_SPSPPS) {
            j->flags |= 2;
        }
        j->texture = in->resource;
        IUnknown_AddRef(j->texture);
    }
    queue_job(j);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS lock_bitstream_impl(void *enc, NV_ENC_LOCK_BITSTREAM *p);

static NVENCSTATUS NVENCAPI lock_bitstream(void *enc, NV_ENC_LOCK_BITSTREAM *p)
{
    return lock_bitstream_impl(enc, p);
}

static uint32_t count_slices(const uint8_t *b, uint32_t n, uint32_t *offsets)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i + 3 < n; i++) {
        if (b[i] || b[i + 1] || b[i + 2] != 1) {
            continue;
        }
        uint32_t start = i > 0 && b[i - 1] == 0 ? i - 1 : i;
        if (((b[i + 3] >> 1) & 0x3f) < 32) {
            if (offsets && count < g_slices) {
                offsets[count] = start;
            }
            count++;
        }
        i += 3;
    }
    return count ? count : 1;
}

static NVENCSTATUS lock_bitstream_impl(void *enc, NV_ENC_LOCK_BITSTREAM *p)
{
    handle *h = p->outputBitstream;
    if (!h || h->dead) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (!h->ready) {
        if (p->doNotWait) {
            return NV_ENC_ERR_LOCK_BUSY;
        }
        drain_queue();
    }
    if (h->output.size && h->output.data) {
        p->bitstreamBufferPtr = h->output.data;
        p->bitstreamSizeInBytes = h->output.size;
        p->pictureType = h->output.keyframe ? NV_ENC_PIC_TYPE_IDR : NV_ENC_PIC_TYPE_P;
        p->outputTimeStamp = h->timestamp;
        p->frameIdx = h->frame_idx;
    } else {
        p->bitstreamBufferPtr = g_empty_bitstream;
        p->bitstreamSizeInBytes = 0;
        p->pictureType = NV_ENC_PIC_TYPE_P;
    }
    p->numSlices = count_slices(p->bitstreamBufferPtr, p->bitstreamSizeInBytes, p->sliceOffsets);
    p->hwEncodeStatus = 0;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI unlock_bitstream(void *enc, NV_ENC_OUTPUT_PTR buf)
{
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI lock_input_buffer(void *enc, NV_ENC_LOCK_INPUT_BUFFER *p)
{
    static uint8_t *scratch;
    if (!scratch) {
        scratch = malloc(4096 * 4096 * 4);
    }
    p->bufferDataPtr = scratch;
    p->pitch = 4096 * 4;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI unlock_input_buffer(void *enc, NV_ENC_INPUT_PTR buf)
{
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_stats(void *enc, NV_ENC_STAT *p)
{
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI get_sequence_params(void *enc, NV_ENC_SEQUENCE_PARAM_PAYLOAD *p)
{
    trace("GetSequenceParams");
    if (p->outSPSPPSPayloadSize) {
        *p->outSPSPPSPayloadSize = 0;
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI register_async_event(void *enc, NV_ENC_EVENT_PARAMS *p)
{
    trace("RegisterAsyncEvent %p", p->completionEvent);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI unregister_async_event(void *enc, NV_ENC_EVENT_PARAMS *p)
{
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI register_resource(void *enc, NV_ENC_REGISTER_RESOURCE *p)
{
    handle *h = calloc(1, sizeof(handle));
    h->resource = p->resourceToRegister;
    h->format = p->bufferFormat;
    p->registeredResource = h;
    trace("RegisterResource ver=0x%08x type=%d %ux%u pitch=%u fmt=0x%x usage=%d res=%p", p->version,
         p->resourceType, p->width, p->height, p->pitch, p->bufferFormat, p->bufferUsage, p->resourceToRegister);
    if (p->resourceType == NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX && p->resourceToRegister) {
        encoder_client_register(p->resourceToRegister);
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI unregister_resource(void *enc, NV_ENC_REGISTERED_PTR res)
{
    trace("UnregisterResource %p", res);
    free(res);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI map_input_resource(void *enc, NV_ENC_MAP_INPUT_RESOURCE *p)
{
    static int logged;
    handle *h = p->registeredResource;
    if (logged++ < 3) {
        trace("MapInputResource ver=0x%08x reg=%p -> res=%p", p->version, p->registeredResource, h ? h->resource : NULL);
    }
    p->mappedResource = h;
    p->mappedBufferFmt = h ? h->format : NV_ENC_BUFFER_FORMAT_ARGB;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI unmap_input_resource(void *enc, NV_ENC_INPUT_PTR mapped)
{
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI destroy_encoder(void *enc)
{
    trace("DestroyEncoder %p", enc);
    drain_queue();
    free(enc);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI invalidate_ref_frames(void *enc, uint64_t ts)
{
    trace("InvalidateRefFrames %llu", ts);
    return NV_ENC_SUCCESS;
}

static const char *NVENCAPI get_last_error(void *enc)
{
    return "macquestlink nvenc shim";
}

static NVENCSTATUS NVENCAPI unimplemented(void)
{
    trace("unimplemented entry point called");
    return NV_ENC_ERR_UNIMPLEMENTED;
}

__declspec(dllexport) NVENCSTATUS NVENCAPI NvEncodeAPIGetMaxSupportedVersion(uint32_t *version)
{
    *version = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    trace("NvEncodeAPIGetMaxSupportedVersion -> %u.%u", NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
    return NV_ENC_SUCCESS;
}

__declspec(dllexport) NVENCSTATUS NVENCAPI NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST *fl)
{
    trace("NvEncodeAPICreateInstance ver=0x%08x (api %u.%u, struct %u)", fl->version, fl->version & 0xf,
         (fl->version >> 24) & 0xf, (fl->version >> 16) & 0xff);
    fl->nvEncOpenEncodeSession = open_session;
    fl->nvEncGetEncodeGUIDCount = get_guid_count;
    fl->nvEncGetEncodeProfileGUIDCount = get_profile_count;
    fl->nvEncGetEncodeProfileGUIDs = get_profiles;
    fl->nvEncGetEncodeGUIDs = get_guids;
    fl->nvEncGetInputFormatCount = get_input_format_count;
    fl->nvEncGetInputFormats = get_input_formats;
    fl->nvEncGetEncodeCaps = get_caps;
    fl->nvEncGetEncodePresetCount = get_preset_count;
    fl->nvEncGetEncodePresetGUIDs = get_preset_guids;
    fl->nvEncGetEncodePresetConfig = get_preset_config;
    fl->nvEncInitializeEncoder = initialize;
    fl->nvEncCreateInputBuffer = create_input_buffer;
    fl->nvEncDestroyInputBuffer = destroy_input_buffer;
    fl->nvEncCreateBitstreamBuffer = create_bitstream_buffer;
    fl->nvEncDestroyBitstreamBuffer = destroy_bitstream_buffer;
    fl->nvEncEncodePicture = encode_picture;
    fl->nvEncLockBitstream = lock_bitstream;
    fl->nvEncUnlockBitstream = unlock_bitstream;
    fl->nvEncLockInputBuffer = lock_input_buffer;
    fl->nvEncUnlockInputBuffer = unlock_input_buffer;
    fl->nvEncGetEncodeStats = get_stats;
    fl->nvEncGetSequenceParams = get_sequence_params;
    fl->nvEncRegisterAsyncEvent = register_async_event;
    fl->nvEncUnregisterAsyncEvent = unregister_async_event;
    fl->nvEncMapInputResource = map_input_resource;
    fl->nvEncUnmapInputResource = unmap_input_resource;
    fl->nvEncDestroyEncoder = destroy_encoder;
    fl->nvEncInvalidateRefFrames = invalidate_ref_frames;
    fl->nvEncOpenEncodeSessionEx = open_session_ex;
    fl->nvEncRegisterResource = register_resource;
    fl->nvEncUnregisterResource = unregister_resource;
    fl->nvEncReconfigureEncoder = reconfigure;
    fl->nvEncCreateMVBuffer = (void *)unimplemented;
    fl->nvEncDestroyMVBuffer = (void *)unimplemented;
    fl->nvEncRunMotionEstimationOnly = (void *)unimplemented;
    fl->nvEncGetLastErrorString = get_last_error;
    fl->nvEncSetIOCudaStreams = (void *)unimplemented;
    fl->nvEncGetEncodePresetConfigEx = get_preset_config_ex;
    fl->nvEncGetSequenceParamEx = (void *)unimplemented;
    fl->nvEncRestoreEncoderState = (void *)unimplemented;
    fl->nvEncLookaheadPicture = (void *)unimplemented;
    return NV_ENC_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_queue_lock);
        InitializeConditionVariable(&g_queue_ready);
        InitializeConditionVariable(&g_queue_idle);
        log_open();
        trace("loaded");
        audio_hook_attach();
        net_hook_attach();
    }
    return TRUE;
}
