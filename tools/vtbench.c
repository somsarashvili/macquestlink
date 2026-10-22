// VideoToolbox HEVC latency benchmark: synthetic moving detail, per-frame submit->output latency.
// usage: vtbench <width> <height> <10bit 0|1> <sessions> <lowlatency 0|1> <speed 0|1> <frames> [split 0|1]
// env: REAL=1 gradients/shapes/texture panning a few px per frame with a scene cut every SCENE (144) frames (NV12 only),
// GRAIN=<amp> adds per-frame luma noise (shimmer) on top.
// Rate control: CBR=<Mbit/s> | VBR=<Mbit/s> (+VBVMAX=<Mbit/s>) | AVG=<Mbit/s> AverageBitRate (default 100/DIV unless NOAVG);
// VBVDUR=<frames> VBV buffer in 1/FPS s units, VBVINIT=<%>, DRL=<bytes> per DRLW (1) frames, MAXQP/MINQP, PERFRAME=<n> re-sets the
// CBR/VBR/average rate every n frames with +-10% jitter (NOJITTER=1: same value, REVBV=1: rate and VBVDUR in one VTSessionSetProperties).
// RATE2=<Mbit/s> (or CBR2) switches the rate (and scales DRL) halfway through. BUDGET=<bytes> per frame (default rate/FPS) for the over-budget count, SIZES=1 prints every frame size.
// FPS=<n> frame rate for pacing and timestamps (72), EFR=<n> ExpectedFrameRate (FPS), EFR2=<n> switches it halfway through.
// PTS=<x> spaces timestamps x frame intervals apart (scales the CBR budget per frame), PTS2=<x> from halfway through, PTSJ=1 +-10% jitter per frame.
// QP=<n> per-frame BaseFrameQP (ignored by the M4 HEVC encoder). Frame sizes and Mbit/s exclude HEVC filler data NALs.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    VTCompressionSessionRef s;
    dispatch_semaphore_t done;
    size_t bytes, max_frame, *sizes, filler;
    int w, h, ten, count;
    double *lat;
    int n, frame, flat;
    pthread_t thread;
} bench;

static int g_fps = 72;

static double now_ms(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1e6; }

static void on_out(void *ref, void *src, OSStatus st, VTEncodeInfoFlags f, CMSampleBufferRef sb)
{
    bench *b = ref;
    if (sb) {
        size_t n = CMSampleBufferGetTotalSampleSize(sb);
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sb);
        size_t total = 0, pos = 0;
        char *data = NULL;
        if (!getenv("H264") && CMBlockBufferGetDataPointer(block, 0, NULL, &total, &data) == noErr)
            while (pos + 4 <= total) {
                uint32_t len = ((uint8_t)data[pos] << 24) | ((uint8_t)data[pos + 1] << 16) | ((uint8_t)data[pos + 2] << 8) | (uint8_t)data[pos + 3];
                if (pos + 4 + len > total)
                    break;
                int type = ((uint8_t)data[pos + 4] >> 1) & 0x3f;
                if (type < 32 && b->count == 5)
                    fprintf(stderr, "slice nal type %d, %u bytes\n", type, len);
                if (type == 38) {
                    b->filler += len + 4;
                    n -= len + 4;
                }
                pos += 4 + len;
            }
        b->bytes += n;
        if (n > b->max_frame)
            b->max_frame = n;
        b->sizes[b->count++] = n;
    }
    dispatch_semaphore_signal(b->done);
}

static void set(VTCompressionSessionRef s, CFStringRef k, CFTypeRef v)
{
    OSStatus st = VTSessionSetProperty(s, k, v);
    if (st)
        fprintf(stderr, "set %s: %d\n", CFStringGetCStringPtr(k, kCFStringEncodingUTF8), (int)st);
}

static void set_int(VTCompressionSessionRef s, CFStringRef k, int32_t v)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    set(s, k, n);
    CFRelease(n);
}

static void set_float(VTCompressionSessionRef s, CFStringRef k, double v)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &v);
    set(s, k, n);
    CFRelease(n);
}

static int rate_bps(void)
{
    const char *r = getenv("CBR") ? getenv("CBR") : getenv("VBR") ? getenv("VBR") : getenv("AVG");
    if (r)
        return (int)(atof(r) * 1e6);
    return getenv("NOAVG") ? 0 : 100000000 / (getenv("DIV") ? atoi(getenv("DIV")) : 1);
}

static CFStringRef rate_key(void)
{
    return getenv("CBR") ? kVTCompressionPropertyKey_ConstantBitRate
         : getenv("VBR") ? kVTCompressionPropertyKey_VariableBitRate
                         : kVTCompressionPropertyKey_AverageBitRate;
}

static void create(bench *b, int lowlat, int speed)
{
    const void *k[] = { kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
                        kVTVideoEncoderSpecification_EnableLowLatencyRateControl };
    const void *v[] = { kCFBooleanTrue, kCFBooleanTrue };
    CFDictionaryRef spec = CFDictionaryCreate(NULL, k, v, lowlat ? 2 : 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    int32_t fmt = b->ten ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    if (getenv("BGRA"))
        fmt = kCVPixelFormatType_32BGRA;
    CFNumberRef pf = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
    CFDictionaryRef ios = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *ak[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey };
    const void *av[] = { pf, ios };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus st = VTCompressionSessionCreate(NULL, b->w, b->h, getenv("H264") ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC, spec, attrs, NULL, on_out, b, &b->s);
    if (st) {
        fprintf(stderr, "create failed %d\n", (int)st);
        exit(1);
    }
    set(b->s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    set(b->s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    if (speed)
        set(b->s, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
    if (getenv("NOPOWER"))
        set(b->s, kVTCompressionPropertyKey_MaximizePowerEfficiency, kCFBooleanFalse);
    if (getenv("NOSAQ"))
        set_int(b->s, kVTCompressionPropertyKey_SpatialAdaptiveQPLevel, kVTQPModulationLevel_Disable);
    if (getenv("SLICES"))
        fprintf(stderr, "NumberOfSlices -> %d\n", (int)VTSessionSetProperty(b->s, CFSTR("NumberOfSlices"), CFNumberCreate(NULL, kCFNumberSInt32Type, &(int32_t){atoi(getenv("SLICES"))})));
    if (getenv("REFS"))
        set_int(b->s, kVTCompressionPropertyKey_ReferenceBufferCount, atoi(getenv("REFS")));
    if (getenv("MAXQP"))
        set_int(b->s, kVTCompressionPropertyKey_MaxAllowedFrameQP, atoi(getenv("MAXQP")));
    if (getenv("MINQP"))
        set_int(b->s, kVTCompressionPropertyKey_MinAllowedFrameQP, atoi(getenv("MINQP")));
    if (getenv("MAXDELAY"))
        set_int(b->s, kVTCompressionPropertyKey_MaxFrameDelayCount, 0);
    if (!getenv("H264"))
        set(b->s, kVTCompressionPropertyKey_ProfileLevel, b->ten ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel);
    set_int(b->s, kVTCompressionPropertyKey_MaxKeyFrameInterval, 0x7fffffff);
    set_int(b->s, kVTCompressionPropertyKey_ExpectedFrameRate, getenv("EFR") ? atoi(getenv("EFR")) : g_fps);
    if (rate_bps())
        set_int(b->s, rate_key(), rate_bps());
    if (getenv("VBVMAX"))
        set_int(b->s, kVTCompressionPropertyKey_VBVMaxBitRate, atoi(getenv("VBVMAX")) * 1000000);
    if (getenv("VBVDUR"))
        set_float(b->s, kVTCompressionPropertyKey_VBVBufferDuration, atof(getenv("VBVDUR")) / g_fps);
    if (getenv("VBVINIT"))
        set_float(b->s, kVTCompressionPropertyKey_VBVInitialDelayPercentage, atof(getenv("VBVINIT")));
    if (getenv("DRL")) {
        double bytes = atof(getenv("DRL")), secs = (getenv("DRLW") ? atof(getenv("DRLW")) : 1) / g_fps;
        CFNumberRef nb = CFNumberCreate(NULL, kCFNumberDoubleType, &bytes), nt = CFNumberCreate(NULL, kCFNumberDoubleType, &secs);
        const void *lv[] = { nb, nt };
        CFArrayRef lim = CFArrayCreate(NULL, lv, 2, &kCFTypeArrayCallBacks);
        set(b->s, kVTCompressionPropertyKey_DataRateLimits, lim);
    }
    if (getenv("FLAT"))
        b->flat = 1;
    VTCompressionSessionPrepareToEncodeFrames(b->s);
    b->done = dispatch_semaphore_create(0);
}

static void fill(CVPixelBufferRef pb, int frame, int ten, int yoff)
{
    CVPixelBufferLockBaseAddress(pb, 0);
    if (!CVPixelBufferIsPlanar(pb)) {
        uint8_t *d = CVPixelBufferGetBaseAddress(pb);
        size_t pitch = CVPixelBufferGetBytesPerRow(pb), w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w * 4; x++) {
                size_t sx = x / 4 + frame * 3;
                uint32_t r = (uint32_t)(sx * 40503u) ^ (uint32_t)((y + yoff) * 2654435761u) ^ (uint32_t)(x & 3) * 0x9e3779b9u;
                r ^= r >> 13;
                r *= 0x5bd1e995;
                d[y * pitch + x] = (uint8_t)(((sx / 8 + (y + yoff) / 8) & 1 ? 160 : 90) + (r >> 27));
            }
        CVPixelBufferUnlockBaseAddress(pb, 0);
        return;
    }
    for (int p = 0; p < 2; p++) {
        uint8_t *d = CVPixelBufferGetBaseAddressOfPlane(pb, p);
        size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(pb, p);
        size_t w = CVPixelBufferGetWidthOfPlane(pb, p), h = CVPixelBufferGetHeightOfPlane(pb, p);
        for (size_t y = 0; y < h; y++) {
            uint32_t seed = (uint32_t)((y + yoff) * 2654435761u);
            for (size_t x = 0; x < w * (p ? 2 : 1); x++) {
                size_t sx = x + frame * 3;
                uint32_t r = (uint32_t)(sx * 40503u) ^ seed;
                r ^= r >> 13;
                r *= 0x5bd1e995;
                uint32_t val = ((sx / 8 + (y + yoff) / 8) & 1 ? 160 : 90) + (r >> 27);
                if (ten)
                    ((uint16_t *)(d + y * pitch))[x] = (uint16_t)(val << 8);
                else
                    d[y * pitch + x] = (uint8_t)val;
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

enum { PAD = 512 };

static uint32_t rnd(uint32_t *st)
{
    *st ^= *st << 13;
    *st ^= *st >> 17;
    *st ^= *st << 5;
    return *st;
}

// One scene as an NV12 image PAD px larger than the frame in both directions, so frames can pan across it.
static uint8_t *make_scene(int w, int h, uint32_t seed)
{
    int sw = w + PAD, sh = h + PAD;
    uint8_t *img = malloc((size_t)sw * sh * 3 / 2), *uv = img + (size_t)sw * sh;
    uint32_t st = seed * 2654435761u + 1;
    double fx = 0.002 + (rnd(&st) % 100) * 0.00003, fy = 0.0015 + (rnd(&st) % 100) * 0.00003;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            img[(size_t)y * sw + x] = (uint8_t)(110 + 50 * sin(x * fx + seed) * cos(y * fy) + 30 * sin((x + y) * 0.0007) +
                                                (int)(rnd(&st) % 9) - 4);
    for (int y = 0; y < sh / 2; y++)
        for (int x = 0; x < sw / 2; x++) {
            uv[(size_t)y * sw + 2 * x] = (uint8_t)(128 + 20 * sin(x * fx * 3 + seed));
            uv[(size_t)y * sw + 2 * x + 1] = (uint8_t)(128 + 20 * cos(y * fy * 3));
        }
    for (int i = 0; i < 400; i++) {
        int rw = 20 + rnd(&st) % 300, rh = 20 + rnd(&st) % 300, x0 = rnd(&st) % (sw - rw), y0 = rnd(&st) % (sh - rh);
        int base = 30 + rnd(&st) % 190, tex = rnd(&st) % 3 ? 4 + rnd(&st) % 40 : 0, round = rnd(&st) & 1;
        int u = 90 + rnd(&st) % 76, v = 90 + rnd(&st) % 76;
        for (int y = y0; y < y0 + rh; y++)
            for (int x = x0; x < x0 + rw; x++) {
                double dx = (x - x0) / (double)rw - 0.5, dy = (y - y0) / (double)rh - 0.5;
                if (round && dx * dx + dy * dy > 0.25)
                    continue;
                int val = base + (int)(40 * dy);
                if (tex)
                    val += (int)(rnd(&st) % tex) - tex / 2 + ((x / 4 + y / 4) & 1) * tex / 2;
                img[(size_t)y * sw + x] = (uint8_t)(val < 16 ? 16 : val > 235 ? 235 : val);
                if (!(x & 1) && !(y & 1)) {
                    uv[(size_t)(y / 2) * sw + x] = (uint8_t)u;
                    uv[(size_t)(y / 2) * sw + x + 1] = (uint8_t)v;
                }
            }
    }
    return img;
}

static void fill_real(CVPixelBufferRef pb, uint8_t *scene, int w, int h, int frame, const int8_t *grain)
{
    int sw = w + PAD, sh = h + PAD;
    int ox = (frame * 3) % (PAD * 2), oy = (frame * 2) % (PAD * 2);
    ox = (ox >= PAD ? 2 * PAD - ox : ox) & ~1;
    oy = (oy >= PAD ? 2 * PAD - oy : oy) & ~1;
    CVPixelBufferLockBaseAddress(pb, 0);
    for (int p = 0; p < 2; p++) {
        uint8_t *d = CVPixelBufferGetBaseAddressOfPlane(pb, p);
        size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(pb, p);
        const uint8_t *src = scene + (p ? (size_t)sw * sh : 0);
        for (int y = 0; y < (p ? h / 2 : h); y++)
            memcpy(d + y * pitch, src + (size_t)(y + (p ? oy / 2 : oy)) * sw + ox, w);
    }
    if (grain) {
        uint8_t *d = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), shift = (size_t)(frame * 7919) % 65536;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int v = d[y * pitch + x] + grain[((size_t)y * w + x + shift) & 0xffffff];
                d[y * pitch + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

static void *run(void *arg)
{
    int inflight = getenv("INFLIGHT") ? atoi(getenv("INFLIGHT")) : 0;
    double start = 0;
    bench *b = arg;
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(b->s);
    CVPixelBufferRef pbs[8];
    uint8_t *scenes[3] = { 0 };
    int real = getenv("REAL") != NULL, scene_len = getenv("SCENE") ? atoi(getenv("SCENE")) : 144;
    if (real)
        for (int i = 0; i < 3; i++)
            scenes[i] = make_scene(b->w, b->h, i + 1);
    uint32_t st = 12345;
    double set_ms = 0, pts_s = 0;
    int amp = getenv("GRAIN") ? atoi(getenv("GRAIN")) : 0;
    int8_t *grain = amp ? malloc(1 << 24) : NULL;
    for (int i = 0; grain && i < 1 << 24; i++)
        grain[i] = (int8_t)((int)(rnd(&st) % (2 * amp + 1)) - amp);
    for (int i = 0; i < 8 && !real; i++) {
        CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pbs[i]);
        fill(pbs[i], b->flat ? 0 : i, b->ten, b->frame);
    }
    for (int i = 0; i < b->n; i++) {
        const char *r2 = getenv("RATE2") ? getenv("RATE2") : getenv("CBR2");
        if (i == b->n / 2 && getenv("EFR2"))
            set_int(b->s, kVTCompressionPropertyKey_ExpectedFrameRate, atoi(getenv("EFR2")));
        if (i == b->n / 2 && r2 && rate_bps()) {
            fprintf(stderr, "first half %.1f Mbit/s\n", b->bytes * 8.0 / (i / (double)g_fps) / 1e6);
            b->bytes = 0;
            set_int(b->s, rate_key(), (int32_t)(atof(r2) * 1e6));
            if (getenv("DRL")) {
                double bytes = atof(getenv("DRL")) * atof(r2) * 1e6 / rate_bps(), secs = (getenv("DRLW") ? atof(getenv("DRLW")) : 1) / g_fps;
                CFNumberRef nb = CFNumberCreate(NULL, kCFNumberDoubleType, &bytes), nt = CFNumberCreate(NULL, kCFNumberDoubleType, &secs);
                const void *lv[] = { nb, nt };
                CFArrayRef lim = CFArrayCreate(NULL, lv, 2, &kCFTypeArrayCallBacks);
                set(b->s, kVTCompressionPropertyKey_DataRateLimits, lim);
                CFRelease(lim);
                CFRelease(nb);
                CFRelease(nt);
            }
        }
        double scale = getenv("PTS2") && i >= b->n / 2 ? atof(getenv("PTS2")) : getenv("PTS") ? atof(getenv("PTS")) : 1;
        if (getenv("PTSJ"))
            scale *= 0.9 + (rnd(&st) % 1000) / 5000.0;
        pts_s += i ? scale / g_fps : 0;
        CMTime pts = CMTimeMakeWithSeconds(pts_s, 90000);
        CVPixelBufferRef pb = NULL;
        if (real) {
            CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb);
            fill_real(pb, scenes[i / scene_len % 3], b->w, b->h, i, grain);
        }
        double t0 = now_ms();
        if (getenv("PERFRAME") && rate_bps() && i % atoi(getenv("PERFRAME")) == 0) {
            int32_t r = (int32_t)(rate_bps() * (getenv("NOJITTER") ? 1 : 0.9 + (rnd(&st) % 1000) / 5000.0));
            if (getenv("VBVDUR") && getenv("REVBV")) {
                double d = atof(getenv("VBVDUR")) / g_fps;
                CFNumberRef nr = CFNumberCreate(NULL, kCFNumberSInt32Type, &r), nd = CFNumberCreate(NULL, kCFNumberDoubleType, &d);
                const void *k[] = { rate_key(), kVTCompressionPropertyKey_VBVBufferDuration }, *v[] = { nr, nd };
                CFDictionaryRef props = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                OSStatus err = VTSessionSetProperties(b->s, props);
                if (err)
                    fprintf(stderr, "set properties: %d\n", (int)err);
                CFRelease(props);
                CFRelease(nr);
                CFRelease(nd);
            } else
                set_int(b->s, rate_key(), r);
            set_ms += now_ms() - t0;
        }
        CFDictionaryRef opts = NULL;
        if (getenv("QP")) {
            int32_t qp = atoi(getenv("QP"));
            CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &qp);
            const void *k[] = { kVTEncodeFrameOptionKey_BaseFrameQP }, *v[] = { n };
            opts = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFRelease(n);
        }
        VTCompressionSessionEncodeFrame(b->s, real ? pb : pbs[i % 8], pts, kCMTimeInvalid, opts, NULL, NULL);
        if (opts)
            CFRelease(opts);
        if (inflight) {
            if (i == 0)
                start = t0;
            if (i + 1 >= inflight)
                dispatch_semaphore_wait(b->done, DISPATCH_TIME_FOREVER);
            if (pb)
                CVPixelBufferRelease(pb);
            continue;
        }
        dispatch_semaphore_wait(b->done, DISPATCH_TIME_FOREVER);
        b->lat[i] = now_ms() - t0;
        if (pb)
            CVPixelBufferRelease(pb);
        double next = t0 + 1000.0 / g_fps;
        while (now_ms() < next)
            ;
    }
    if (inflight) {
        for (int k = 1; k < inflight && k < b->n; k++)
            dispatch_semaphore_wait(b->done, DISPATCH_TIME_FOREVER);
        double ms = now_ms() - start;
        printf("inflight %d: %d frames in %.0f ms = %.1f fps, %.2f ms per frame\n", inflight, b->n, ms, b->n * 1000.0 / ms, ms / b->n);
    }
    if (set_ms)
        fprintf(stderr, "rate updates avg %.3f ms\n", set_ms / b->n);
    return NULL;
}

static int cmp(const void *a, const void *b) { double d = *(double *)a - *(double *)b; return d < 0 ? -1 : d > 0; }

static int cmp_size(const void *a, const void *b) { size_t x = *(size_t *)a, y = *(size_t *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr, "usage: vtbench w h ten sessions lowlat speed frames\n");
        return 1;
    }
    int w = atoi(argv[1]), h = atoi(argv[2]), ten = atoi(argv[3]), ns = atoi(argv[4]);
    int lowlat = atoi(argv[5]), speed = atoi(argv[6]), n = atoi(argv[7]);
    int split = argc > 8 && atoi(argv[8]);
    if (getenv("FPS"))
        g_fps = atoi(getenv("FPS"));
    bench b[8] = { 0 };
    for (int i = 0; i < ns; i++) {
        b[i].w = w;
        b[i].h = split ? h / ns : h;
        b[i].ten = ten;
        b[i].n = n;
        b[i].frame = split ? i * h / ns : 0;
        b[i].lat = calloc(n, sizeof(double));
        b[i].sizes = calloc(n, sizeof(size_t));
        create(&b[i], lowlat, speed);
    }
    for (int i = 0; i < ns; i++)
        pthread_create(&b[i].thread, NULL, run, &b[i]);
    for (int i = 0; i < ns; i++)
        pthread_join(b[i].thread, NULL);
    for (int i = 0; i < ns; i++) {
        int skip = 10;
        qsort(b[i].lat + skip, n - skip, sizeof(double), cmp);
        double sum = 0;
        for (int j = skip; j < n; j++)
            sum += b[i].lat[j];
        int m = n - skip;
        printf("session %d %dx%d: avg %.2f p50 %.2f p95 %.2f max %.2f ms, %.1f Mbit/s, max frame %zu B\n", i, b[i].w, b[i].h,
               sum / m, b[i].lat[skip + m / 2], b[i].lat[skip + m * 95 / 100], b[i].lat[n - 1],
               b[i].bytes * 8.0 / (n / (double)g_fps) / 1e6, b[i].max_frame);
        if (getenv("SIZES"))
            for (int j = 0; j < b[i].count; j++)
                fprintf(stderr, "%zu%c", b[i].sizes[j], j % 16 == 15 ? '\n' : ' ');
        int c = b[i].count - 1;
        size_t budget = getenv("BUDGET") ? atol(getenv("BUDGET")) : rate_bps() / 8 / g_fps, over = 0;
        for (int j = 1; j <= c; j++)
            over += budget && b[i].sizes[j] > budget + budget / 100;
        qsort(b[i].sizes + 1, c, sizeof(size_t), cmp_size);
        if (c > 0)
            printf("  filler %zu B dropped; frames (excl. first) p50 %zu p95 %zu p99 %zu max %zu B, budget %zu B, over %.1f%%\n", b[i].filler, b[i].sizes[1 + c / 2],
                   b[i].sizes[1 + c * 95 / 100], b[i].sizes[1 + c * 99 / 100], b[i].sizes[c], budget, 100.0 * over / c);
        VTCompressionSessionInvalidate(b[i].s);
    }
    return 0;
}
