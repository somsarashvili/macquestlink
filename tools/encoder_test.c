#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../common/mql_protocol.h"

// encoder_test <frames.bin> [out.hevc] : sends 120 synthetic NV12 frames to a running macquestlink-encoder (port
// MQL_PORT, default 47800) through the given frames.bin and prints round-trip times.
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <frames.bin> [out.hevc]\n", argv[0]);
        return 1;
    }
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror(argv[1]); return 1; }
    uint8_t *frames = mmap(NULL, (size_t)MQL_SLOT_SIZE * MQL_SLOT_COUNT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (frames == MAP_FAILED) { perror("mmap"); return 1; }
    const uint32_t w = 1216, h = 4864, pitch = 1280, chroma = pitch * h;
    FILE *out = fopen(argc > 2 ? argv[2] : "/dev/null", "wb");

    int s = socket(AF_INET, SOCK_STREAM, 0);
    uint16_t port = getenv("MQL_PORT") ? (uint16_t)atoi(getenv("MQL_PORT")) : MQL_PORT;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(s, (struct sockaddr *)&a, sizeof(a))) { perror("connect"); return 1; }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    uint8_t *buf = malloc(MQL_MAX_BITSTREAM);
    double total = 0, worst = 0;
    int failed = 0;
    for (int f = 0; f < 120; f++) {
        uint8_t *base = frames + (f % MQL_SLOT_COUNT) * (size_t)MQL_SLOT_SIZE;
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = base + y * pitch;
            for (uint32_t x = 0; x < w; x++) row[x] = (uint8_t)(16 + (x + f * 8) % 220);
        }
        for (uint32_t y = 0; y < h / 2; y++) {
            uint8_t *row = base + chroma + y * pitch;
            for (uint32_t x = 0; x < w / 2; x++) { row[2 * x] = (uint8_t)(64 + y % 128); row[2 * x + 1] = 128; }
        }
        mql_frame_request r = { MQL_MAGIC_FRAME, f % MQL_SLOT_COUNT, w, h, pitch, chroma, 0, f == 0 ? 3u : 0u, 100000, 72,
                                (uint64_t)f * 13889 };
        r.slices = 4;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        send(s, &r, sizeof(r), 0);
        mql_frame_reply rep;
        if (recv(s, &rep, sizeof(rep), MSG_WAITALL) != sizeof(rep)) { fprintf(stderr, "helper closed the connection\n"); return 1; }
        if (rep.size) recv(s, buf, rep.size, MSG_WAITALL);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        total += ms; if (ms > worst) worst = ms;
        failed += rep.status != 0 || !rep.size;
        fwrite(buf, 1, rep.size, out);
        if (f < 3 || f % 40 == 0) printf("frame %3d status=%d key=%u size=%u round-trip=%.2fms\n", f, rep.status, rep.keyframe, rep.size, ms);
    }
    printf("avg %.2f ms, worst %.2f ms, failed %d\n", total / 120, worst, failed);
    fclose(out);
    return failed != 0;
}
