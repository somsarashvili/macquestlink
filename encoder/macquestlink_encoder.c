#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <mach-o/dyld.h>
#include <VideoToolbox/VideoToolbox.h>
#include <arpa/inet.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../common/mql_protocol.h"

// macOS 26 keys by value: build with older SDKs, no missing symbol on older macOS
#define MQL_KEY_VBVBufferDuration CFSTR("VBVBufferDuration")

struct encoder {
    VTCompressionSessionRef session;
    uint32_t width;
    uint32_t height;
    uint32_t ten_bit;
    uint32_t bitrate_kbps;
    uint32_t fps;
    uint32_t slices;
    dispatch_semaphore_t done;
    uint8_t *out;
    uint32_t out_size;
    uint32_t keyframe;
    int irap;
    OSStatus status;
    int want_parameter_sets;
    uint64_t filler;
    double avg_kbps;
    uint64_t cut_since, applied_ns;
    uint32_t cut_kbps;
};

static FILE *g_log;
static char g_state_path[1100];

typedef struct encoder encoder;

static void save_last_size(const char *path, const encoder *e);

static void logf_(const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (g_log) {
        fprintf(g_log, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

static const uint8_t k_start_code[4] = { 0, 0, 0, 1 };

static int append(encoder *e, const void *data, size_t len)
{
    if (e->out_size + len > MQL_MAX_BITSTREAM) {
        return -1;
    }
    memcpy(e->out + e->out_size, data, len);
    e->out_size += (uint32_t)len;
    return 0;
}

static void append_parameter_sets(encoder *e, CMFormatDescriptionRef fmt)
{
    size_t count = 0;
    if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, 0, NULL, NULL, &count, NULL) != noErr) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        const uint8_t *ps;
        size_t ps_len;
        if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, i, &ps, &ps_len, NULL, NULL) == noErr) {
            append(e, k_start_code, 4);
            append(e, ps, ps_len);
        }
    }
}

static void on_encoded(void *ref, void *frame_ref, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
    encoder *e = ref;
    e->status = status;
    e->out_size = 0;
    e->keyframe = 0;
    e->irap = 0;
    if (status != noErr || !sample) {
        dispatch_semaphore_signal(e->done);
        return;
    }

    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    int keyframe = 1;
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict = CFArrayGetValueAtIndex(attachments, 0);
        keyframe = !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);
    }
    e->keyframe = keyframe;
    if (keyframe || e->want_parameter_sets) {
        append_parameter_sets(e, CMSampleBufferGetFormatDescription(sample));
    }

    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t total = 0;
    char *data = NULL;
    if (CMBlockBufferGetDataPointer(block, 0, NULL, &total, &data) == noErr) {
        size_t pos = 0;
        int irap = 0;
        while (pos + 4 <= total) {
            uint32_t nal_len = ((uint8_t)data[pos] << 24) | ((uint8_t)data[pos + 1] << 16) |
                               ((uint8_t)data[pos + 2] << 8) | (uint8_t)data[pos + 3];
            pos += 4;
            if (pos + nal_len > total) {
                break;
            }
            uint8_t type = ((uint8_t)data[pos] >> 1) & 0x3f;
            if (type >= 16 && type <= 21) {
                irap = 1;
            }
            // Constant bit rate pads with filler data; it only wastes air time and inflates vrlink's frame sizes.
            if (type == 38) {
                e->filler += nal_len + 4;
                pos += nal_len;
                continue;
            }
            append(e, k_start_code, 4);
            append(e, data + pos, nal_len);
            pos += nal_len;
        }
        e->irap = irap;
    }
    dispatch_semaphore_signal(e->done);
}

static void set_prop(VTCompressionSessionRef s, CFStringRef key, CFTypeRef value)
{
    OSStatus st = VTSessionSetProperty(s, key, value);
    if (st != noErr) {
        char name[128] = "";
        CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
        logf_("warning: %s not accepted (%d)", name, (int)st);
    }
}

static void set_int(VTCompressionSessionRef s, CFStringRef key, int32_t v)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    set_prop(s, key, n);
    CFRelease(n);
}

// Constant bit rate also caps bursts, and without low-latency rate control the hardware encodes up to ~7 Mpixel frames
// about 40% faster (1024x4096: 9.4 -> 5.4 ms on M4 Max, measured with tools/vtbench.c).
static void set_bitrate(VTCompressionSessionRef s, uint32_t kbps)
{
    set_int(s, kVTCompressionPropertyKey_ConstantBitRate, (int32_t)kbps * 1000);
}

// A short VBV keeps CBR frames near the per-frame budget (vrlink asks for about one frame). Below ~0.25 s the hardware
// behaves the same, and changing it later restarts rate control, so it is only set when the session is created.
static void set_vbv(VTCompressionSessionRef s, const mql_frame_request *req)
{
    uint32_t fps = req->fps ? req->fps : 90;
    double secs = req->vbv_bits && req->bitrate_kbps ? req->vbv_bits / (req->bitrate_kbps * 1000.0) : 1.0 / fps;
    secs = secs < 1.0 / fps ? 1.0 / fps : secs > 0.25 ? 0.25 : secs;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &secs);
    set_prop(s, MQL_KEY_VBVBufferDuration, n);
    CFRelease(n);
}

static void apply_bitrate(encoder *e, uint32_t kbps)
{
    set_bitrate(e->session, kbps);
    logf_("bitrate %u kbps", kbps);
    e->bitrate_kbps = kbps;
    e->avg_kbps = kbps;
    e->cut_since = 0;
    e->applied_ns = clock_gettime_nsec_np(CLOCK_MONOTONIC);
}

// Every VT bitrate change, up or down, restarts its rate control: the next frame bursts to 3-10x the per-frame budget
// and costs ~3 ms more (in Alyx ~20% of applied changes drew a vrlink bad link event). vrlink retunes every frame,
// often dipping for a frame or two, so cuts below 85% apply once they hold for 40 ms; otherwise the encoder follows a
// ~0.25 s average of vrlink's requests at most once a second (raises over 15%, cuts over 5%).
static void update_bitrate(encoder *e, uint32_t kbps)
{
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    uint32_t cur = e->bitrate_kbps;
    if ((uint64_t)kbps * 20 < (uint64_t)cur * 17) {
        if (!e->cut_since) {
            e->cut_since = now;
            e->cut_kbps = kbps;
        }
        e->cut_kbps = kbps > e->cut_kbps ? kbps : e->cut_kbps;
        if (now - e->cut_since >= 40000000ULL) {
            apply_bitrate(e, e->cut_kbps);
            return;
        }
    } else {
        e->cut_since = 0;
    }
    e->avg_kbps = e->avg_kbps > 0 ? e->avg_kbps + 0.05 * (kbps - e->avg_kbps) : kbps;
    if (now - e->applied_ns >= 1000000000ULL && (e->avg_kbps * 20 > cur * 23.0 || e->avg_kbps * 20 < cur * 19.0)) {
        apply_bitrate(e, (uint32_t)e->avg_kbps);
    }
}

static void destroy_session(encoder *e)
{
    if (e->session) {
        VTCompressionSessionInvalidate(e->session);
        CFRelease(e->session);
        e->session = NULL;
    }
}

static OSStatus create_session(encoder *e, const mql_frame_request *req)
{
    destroy_session(e);
    const void *keys[] = { kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder };
    const void *values[] = { kCFBooleanTrue };
    CFDictionaryRef spec = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    int32_t pixel_format = req->ten_bit ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                                        : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pf = CFNumberCreate(NULL, kCFNumberSInt32Type, &pixel_format);
    CFDictionaryRef iosurface = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
    const void *ak[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey };
    const void *av[] = { pf, iosurface };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFRelease(pf);
    CFRelease(iosurface);
    OSStatus st = VTCompressionSessionCreate(NULL, (int32_t)req->width, (int32_t)req->height, kCMVideoCodecType_HEVC,
                                             spec, attrs, NULL, on_encoded, e, &e->session);
    CFRelease(spec);
    CFRelease(attrs);
    if (st != noErr) {
        logf_("VTCompressionSessionCreate failed: %d", (int)st);
        return st;
    }

    VTCompressionSessionRef s = e->session;
    set_prop(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    set_prop(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    set_prop(s, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
    set_prop(s, kVTCompressionPropertyKey_ProfileLevel,
             req->ten_bit ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel);
    set_int(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, 0x7fffffff);
    set_int(s, kVTCompressionPropertyKey_ExpectedFrameRate, (int32_t)(req->fps ? req->fps : 90));
    set_bitrate(s, req->bitrate_kbps ? req->bitrate_kbps : 100000);
    set_vbv(s, req);
    set_prop(s, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
    set_prop(s, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
    set_prop(s, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    if (req->slices > 1) {
        set_int(s, CFSTR("NumberOfSlices"), (int32_t)req->slices);
    }
    VTCompressionSessionPrepareToEncodeFrames(s);

    e->width = req->width;
    e->height = req->height;
    e->ten_bit = req->ten_bit;
    e->bitrate_kbps = req->bitrate_kbps ? req->bitrate_kbps : 100000;
    e->fps = req->fps;
    e->slices = req->slices;
    e->cut_since = 0;
    logf_("session %ux%u %s %u kbps %u fps %u slices", req->width, req->height, req->ten_bit ? "Main10" : "Main",
          req->bitrate_kbps, req->fps, req->slices);
    return noErr;
}

static int encode(encoder *e, uint8_t *frames, const mql_frame_request *req)
{
    if (!e->session || e->width != req->width || e->height != req->height || e->ten_bit != req->ten_bit || e->slices != req->slices) {
        if (create_session(e, req) != noErr) {
            return -1;
        }
        save_last_size(g_state_path, e);
    }
    if (req->bitrate_kbps) {
        update_bitrate(e, req->bitrate_kbps);
    }

    CVPixelBufferRef pb = NULL;
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(e->session);
    uint8_t *base = frames + (size_t)req->slot * MQL_SLOT_SIZE;
    if (pool && CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) == kCVReturnSuccess) {
        // IOSurface-backed buffers go straight to the hardware encoder; wrapping our mapping would force VT to copy.
        CVPixelBufferLockBaseAddress(pb, 0);
        const uint8_t *src_planes[2] = { base, base + req->chroma_offset };
        size_t rows[2] = { req->height, req->height / 2 };
        size_t row_bytes = (size_t)req->width * (req->ten_bit ? 2 : 1);
        for (int plane = 0; plane < 2; plane++) {
            uint8_t *dst = CVPixelBufferGetBaseAddressOfPlane(pb, plane);
            size_t dst_pitch = CVPixelBufferGetBytesPerRowOfPlane(pb, plane);
            const uint8_t *src = src_planes[plane];
            if (dst_pitch == req->pitch) {
                memcpy(dst, src, req->pitch * rows[plane]);
            } else {
                for (size_t y = 0; y < rows[plane]; y++) {
                    memcpy(dst + y * dst_pitch, src + y * req->pitch, row_bytes);
                }
            }
        }
        CVPixelBufferUnlockBaseAddress(pb, 0);
    } else {
        void *planes[2] = { base, base + req->chroma_offset };
        size_t widths[2] = { req->width, req->width / 2 };
        size_t heights[2] = { req->height, req->height / 2 };
        size_t pitches[2] = { req->pitch, req->pitch };
        OSType fmt = req->ten_bit ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                                  : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        CVReturn cv = CVPixelBufferCreateWithPlanarBytes(NULL, req->width, req->height, fmt, NULL,
                                                         req->chroma_offset + req->pitch * (req->height / 2), 2,
                                                         planes, widths, heights, pitches, NULL, NULL, NULL, &pb);
        if (cv != kCVReturnSuccess) {
            logf_("CVPixelBufferCreateWithPlanarBytes failed: %d", cv);
            return -1;
        }
    }

    CFDictionaryRef opts = NULL;
    int force_idr = (req->flags & MQL_FLAG_FORCE_IDR) != 0;
    if (force_idr) {
        const void *k[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
        const void *v[] = { kCFBooleanTrue };
        opts = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    e->want_parameter_sets = (req->flags & MQL_FLAG_PARAMETER_SETS) != 0;
    // vrlink's inputTimeStamp isn't in real time units; rate control needs real frame spacing.
    CMTime pts = CMClockGetTime(CMClockGetHostTimeClock());
    OSStatus st = VTCompressionSessionEncodeFrame(e->session, pb, pts, kCMTimeInvalid, opts, NULL, NULL);
    if (opts) {
        CFRelease(opts);
    }
    if (st == noErr) {
        dispatch_semaphore_wait(e->done, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC));
        st = e->status;
    }
    CVPixelBufferRelease(pb);
    if (st != noErr) {
        logf_("encode failed: %d", (int)st);
        return -1;
    }
    return 0;
}

// Session creation costs ~400 ms; doing it on vrlink's first frame overflows its queue, so the last size is
// remembered and the session is created before any client connects.
static void save_last_size(const char *path, const encoder *e)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%u %u %u %u %u\n", e->width, e->height, e->ten_bit, e->bitrate_kbps, e->fps);
        fclose(f);
    }
}

static void prewarm(const char *path, encoder *e)
{
    FILE *f = fopen(path, "r");
    mql_frame_request req = { 0 };
    if (!f) {
        return;
    }
    if (fscanf(f, "%u %u %u %u %u", &req.width, &req.height, &req.ten_bit, &req.bitrate_kbps, &req.fps) == 5 &&
        req.width && req.height) {
        create_session(e, &req);
    }
    fclose(f);
}

static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static uint8_t *map_file(const char *path, size_t size)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror(path);
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    // Default: frames.bin next to the executable (the bottle's C:\macquestlink, where the shim maps it).
    char path[1024];
    uint32_t exe_size = sizeof(path);
    if (argc > 1) {
        snprintf(path, sizeof(path), "%s", argv[1]);
    } else if (_NSGetExecutablePath(path, &exe_size) == 0 && strrchr(path, '/') &&
               strrchr(path, '/') - path + sizeof("/frames.bin") <= sizeof(path)) {
        strcpy(strrchr(path, '/') + 1, "frames.bin");
    } else {
        fprintf(stderr, "usage: %s [frames.bin]\n", argv[0]);
        return 1;
    }
    char log_path[1100];
    snprintf(log_path, sizeof(log_path), "%s", path);
    char *slash = strrchr(log_path, '/');
    if (slash) {
        strcpy(slash + 1, "encoder.log");
        g_log = fopen(log_path, "a");
    }
    uint8_t *frames = map_file(path, (size_t)MQL_SLOT_SIZE * MQL_SLOT_COUNT);
    if (!frames) {
        return 1;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    uint16_t port = getenv("MQL_PORT") ? (uint16_t)atoi(getenv("MQL_PORT")) : MQL_PORT;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv, 4) != 0) {
        perror("bind/listen");
        return 1;
    }
    logf_("listening on 127.0.0.1:%u, frames at %s", port, path);

    encoder e = { 0 };
    e.done = dispatch_semaphore_create(0);
    e.out = malloc(MQL_MAX_BITSTREAM);
    char state_path[1100];
    snprintf(state_path, sizeof(state_path), "%s", path);
    char *dot = strrchr(state_path, '/');
    if (dot) {
        strcpy(dot + 1, "encoder.session");
    }
    snprintf(g_state_path, sizeof(g_state_path), "%s", state_path);
    prewarm(state_path, &e);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        logf_("client connected");
        uint64_t count = 0, bytes = 0;
        uint32_t empty = 0, keys = 0, iraps = 0, forced = 0, max_size = 0, big = 0;
        uint64_t prev_ns = 0;
        double encode_ms = 0;
        uint64_t window_start = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        e.filler = 0;
        mql_frame_request req;
        while (read_full(fd, &req, sizeof(req)) == 0 && req.magic == MQL_MAGIC_FRAME) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            mql_frame_reply rep = { MQL_MAGIC_OUTPUT, 0, 0, 0 };
            int bad = req.surface || req.slot >= MQL_SLOT_COUNT ||
                      req.chroma_offset + req.pitch * (req.height / 2) > MQL_SLOT_SIZE;
            if (bad || encode(&e, frames, &req) != 0) {
                rep.status = -1;
            } else {
                rep.keyframe = e.keyframe;
                rep.size = e.out_size;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (write_full(fd, &rep, sizeof(rep)) != 0 || (rep.size && write_full(fd, e.out, rep.size) != 0)) {
                break;
            }
            count++;
            bytes += rep.size;
            empty += rep.size == 0;
            keys += rep.keyframe;
            iraps += e.irap;
            forced += (req.flags & MQL_FLAG_FORCE_IDR) != 0;
            uint64_t start_ns = (uint64_t)t0.tv_sec * 1000000000ULL + (uint64_t)t0.tv_nsec;
            double interval_ms = prev_ns ? (start_ns - prev_ns) / 1e6 : 0;
            prev_ns = start_ns;
            double budget = e.bitrate_kbps * 0.125 * (interval_ms > 8 && interval_ms < 100 ? interval_ms : 1000.0 / 90);
            if (rep.size > 2 * budget) {
                if (big++ < 8) {
                    logf_("big frame %u bytes, %.1fx budget (%u kbps, vbv %u bytes, %.1f ms interval), %s, %.0f ms after rate change",
                          rep.size, rep.size / budget, e.bitrate_kbps, req.vbv_bits / 8, interval_ms,
                          rep.keyframe ? (req.flags & MQL_FLAG_FORCE_IDR ? "forced IDR" : "keyframe") : "P",
                          e.applied_ns ? (clock_gettime_nsec_np(CLOCK_MONOTONIC) - e.applied_ns) / 1e6 : -1.0);
                }
            }
            max_size = rep.size > max_size ? rep.size : max_size;
            encode_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
            if (count % 450 == 0) {
                uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
                double secs = (now - window_start) / 1e9;
                logf_("%llu frames, %.1f fps, avg %.2f ms encode, %.1f Mbit/s (+%.1f filler dropped), empty %u, keyframes %u (IRAP NALs %u, IDR requested %u), max frame %u bytes, over 2x budget %u",
                      count, 450 / secs, encode_ms / 450, bytes * 8.0 / 1e6 / secs, e.filler * 8.0 / 1e6 / secs, empty,
                      keys, iraps, forced, max_size, big);
                window_start = now;
                e.filler = 0;
                encode_ms = 0;
                bytes = 0;
                empty = keys = iraps = forced = max_size = big = 0;
            }
        }
        logf_("client disconnected after %llu frames", count);
        close(fd);
        save_last_size(state_path, &e);
    }
}
