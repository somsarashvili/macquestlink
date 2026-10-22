#include <libproc.h>
#include <sys/proc_info.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

// Per-thread CPU of a macOS process (e.g. a Wine process) by thread id, in creation order, with pthread names.
// usage: macthreads <pid> [seconds]

#ifndef PROC_PIDLISTTHREADIDS
#define PROC_PIDLISTTHREADIDS 28
#endif
#define MAX_THREADS 1024

struct sample {
    uint64_t id, user, sys;
    char name[MAXTHREADNAMESIZE];
};

static int snapshot(int pid, struct sample *s)
{
    uint64_t ids[MAX_THREADS];
    int n = proc_pidinfo(pid, PROC_PIDLISTTHREADIDS, 0, ids, sizeof(ids)) / sizeof(uint64_t), m = 0;
    for (int i = 0; i < n; i++) {
        struct proc_threadinfo info;
        if (proc_pidinfo(pid, PROC_PIDTHREADID64INFO, ids[i], &info, sizeof(info)) != sizeof(info))
            continue;
        s[m].id = ids[i];
        s[m].user = info.pth_user_time;
        s[m].sys = info.pth_system_time;
        strncpy(s[m].name, info.pth_name, sizeof(s[m].name));
        m++;
    }
    return m;
}

static int by_id(const void *a, const void *b)
{
    uint64_t x = ((const struct sample *)a)->id, y = ((const struct sample *)b)->id;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    static struct sample a[MAX_THREADS], b[MAX_THREADS];
    if (argc < 2)
        return 1;
    int pid = atoi(argv[1]);
    double seconds = argc > 2 ? atof(argv[2]) : 10;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int na = snapshot(pid, a);
    usleep((useconds_t)(seconds * 1e6));
    int nb = snapshot(pid, b);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec), total = 0, totalsys = 0;
    qsort(b, nb, sizeof(b[0]), by_id);
    printf("     thread id   cpu%%   sys%%  name\n");
    for (int i = 0; i < nb; i++) {
        uint64_t u0 = 0, s0 = 0;
        for (int j = 0; j < na; j++)
            if (a[j].id == b[i].id) {
                u0 = a[j].user;
                s0 = a[j].sys;
            }
        double cpu = 100.0 * (b[i].user + b[i].sys - u0 - s0) / wall, sys = 100.0 * (b[i].sys - s0) / wall;
        total += cpu;
        totalsys += sys;
        printf("%14llu %6.2f %6.2f  %s\n", (unsigned long long)b[i].id, cpu, sys, b[i].name);
    }
    printf("total %.1f%% (sys %.1f%%) over %.1f s\n", total, totalsys, wall / 1e9);
    return 0;
}
