#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <winternl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Per-thread CPU of a process (default vrserver.exe): thread times sampled over an interval, start address as
// module+offset, thread description, and optionally a RIP profile of the hot threads (suspend + GetThreadContext,
// plus the first return addresses outside ntdll/win32u/kernel*, found by scanning the stack).
// usage: threadtop [-p exe] [-t seconds] [-s samples-per-second] [-o log] [-f] [-r raw-log]
// -f lists every code address found on the stack (except ntdll's) instead of the first one per app module.
// -r writes every sample as "tid<TAB>chain<TAB>time" (Unix ns) for offline aggregation; -T <hex tid> samples only that thread.

#define MAX_THREADS 512
#define MAX_MODULES 1024
#define MAX_HITS 4096

typedef NTSTATUS(NTAPI *query_thread_fn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef HRESULT(WINAPI *describe_fn)(HANDLE, PWSTR *);

struct module {
    uintptr_t base, size;
    char name[64];
    int system;
};

struct thread {
    DWORD tid;
    HANDLE h;
    uintptr_t start;
    WCHAR desc[64];
    ULONGLONG k0, u0, k1, u1;
    double pct;
    int sampled;
};

struct hit {
    DWORD tid;
    char chain[400];
    int count;
};

static struct module modules[MAX_MODULES];
static int nmodules;
static struct thread threads[MAX_THREADS];
static int nthreads;
static struct hit hits[MAX_HITS];
static int nhits;
static FILE *out;
static int full_chain;
static FILE *raw;
static DWORD only_tid;

static void load_modules(HANDLE proc)
{
    HMODULE mods[MAX_MODULES];
    DWORD bytes;
    if (!EnumProcessModulesEx(proc, mods, sizeof(mods), &bytes, LIST_MODULES_ALL))
        return;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && nmodules < MAX_MODULES; i++) {
        MODULEINFO info;
        if (!GetModuleInformation(proc, mods[i], &info, sizeof(info)))
            continue;
        struct module *m = &modules[nmodules++];
        m->base = (uintptr_t)info.lpBaseOfDll;
        m->size = info.SizeOfImage;
        if (!GetModuleBaseNameA(proc, mods[i], m->name, sizeof(m->name)))
            strcpy(m->name, "?");
        m->system = !_stricmp(m->name, "ntdll.dll") || !_stricmp(m->name, "win32u.dll") ||
                    !_strnicmp(m->name, "kernel", 6) || !_stricmp(m->name, "ucrtbase.dll") ||
                    !_strnicmp(m->name, "msvcp", 5) || !_strnicmp(m->name, "vcruntime", 9);
    }
}

static struct module *find_module(uintptr_t addr)
{
    for (int i = 0; i < nmodules; i++)
        if (addr >= modules[i].base && addr < modules[i].base + modules[i].size)
            return &modules[i];
    return NULL;
}

static const char *symbolize(uintptr_t addr, char *buf, size_t len)
{
    struct module *m = find_module(addr);
    if (m)
        snprintf(buf, len, "%s+0x%llx", m->name, (unsigned long long)(addr - m->base));
    else
        snprintf(buf, len, "0x%llx", (unsigned long long)addr);
    return buf;
}

static ULONGLONG ft(FILETIME f)
{
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

static void times(struct thread *t, ULONGLONG *k, ULONGLONG *u)
{
    FILETIME c, e, kt, ut;
    *k = *u = 0;
    if (GetThreadTimes(t->h, &c, &e, &kt, &ut)) {
        *k = ft(kt);
        *u = ft(ut);
    }
}

static void add_hit(DWORD tid, const char *chain)
{
    for (int i = 0; i < nhits; i++)
        if (hits[i].tid == tid && !strcmp(hits[i].chain, chain)) {
            hits[i].count++;
            return;
        }
    if (nhits < MAX_HITS) {
        hits[nhits].tid = tid;
        lstrcpynA(hits[nhits].chain, chain, sizeof(hits[nhits].chain));
        hits[nhits++].count = 1;
    }
}

// Records the RIP plus the first stack slot pointing into each of the next non-system modules (likely callers;
// a stack scan, so an occasional stale slot shows up too).
static void sample_thread(HANDLE proc, struct thread *t)
{
    CONTEXT ctx = {.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER};
    uint64_t stack[1024];
    SIZE_T got = 0;
    char chain[400], buf[80];
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    if (SuspendThread(t->h) == (DWORD)-1)
        return;
    BOOL ok = GetThreadContext(t->h, &ctx);
    if (ok)
        ReadProcessMemory(proc, (void *)ctx.Rsp, stack, sizeof(stack), &got);
    ResumeThread(t->h);
    if (!ok)
        return;
    lstrcpynA(chain, symbolize(ctx.Rip, buf, sizeof(buf)), sizeof(chain));
    struct module *last = find_module(ctx.Rip);
    int depth = 0;
    for (SIZE_T i = 0; i < got / 8 && depth < (full_chain ? 8 : 3); i++) {
        struct module *c = find_module(stack[i]);
        if (!c || (full_chain ? !_stricmp(c->name, "ntdll.dll") : c->system || c == last))
            continue;
        size_t len = strlen(chain);
        snprintf(chain + len, sizeof(chain) - len, " < %s", symbolize(stack[i], buf, sizeof(buf)));
        last = c;
        depth++;
    }
    add_hit(t->tid, chain);
    if (raw)
        fprintf(raw, "%lx\t%s\t%llu\n", t->tid, chain,
                (unsigned long long)((((ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime) - 116444736000000000ull) * 100));
}

static BOOL CALLBACK list_window(HWND hwnd, LPARAM pid)
{
    DWORD owner, tid = GetWindowThreadProcessId(hwnd, &owner);
    if (owner != (DWORD)pid)
        return TRUE;
    char cls[64] = "", title[64] = "";
    RECT r;
    GetClassNameA(hwnd, cls, sizeof(cls));
    GetWindowTextA(hwnd, title, sizeof(title));
    GetWindowRect(hwnd, &r);
    fprintf(out, "  window %p tid %lx %s %ldx%ld at %ld,%ld style %08lx exstyle %08lx class \"%s\" title \"%s\"\n", hwnd,
            tid, IsWindowVisible(hwnd) ? "visible" : "hidden", r.right - r.left, r.bottom - r.top, r.left, r.top,
            GetWindowLongA(hwnd, GWL_STYLE), GetWindowLongA(hwnd, GWL_EXSTYLE), cls, title);
    return TRUE;
}

static int by_pct(const void *a, const void *b)
{
    const struct thread *x = a, *y = b;
    return x->pct < y->pct ? 1 : x->pct > y->pct ? -1 : 0;
}

static int by_count(const void *a, const void *b)
{
    return ((const struct hit *)b)->count - ((const struct hit *)a)->count;
}

int main(int argc, char **argv)
{
    const char *exe = "vrserver.exe", *logpath = "C:\\macquestlink\\threadtop.log";
    double seconds = 10, rate = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-f")) {
            full_chain = 1;
            memmove(&argv[i], &argv[i + 1], (argc - i) * sizeof(*argv));
            argc--;
        }
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-p"))
            exe = argv[i + 1];
        else if (!strcmp(argv[i], "-t"))
            seconds = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "-s"))
            rate = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "-o"))
            logpath = argv[i + 1];
        else if (!strcmp(argv[i], "-T"))
            only_tid = strtoul(argv[i + 1], NULL, 16);
        else if (!strcmp(argv[i], "-r"))
            raw = fopen(argv[i + 1], "w");
    }
    out = fopen(logpath, "w");
    if (!out)
        out = stdout;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
    PROCESSENTRY32 pe = {.dwSize = sizeof(pe)};
    DWORD pid = 0;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (!_stricmp(pe.szExeFile, exe))
            pid = pe.th32ProcessID;
    if (!pid) {
        fprintf(out, "no %s\n", exe);
        return 1;
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    load_modules(proc);

    query_thread_fn query = (query_thread_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    describe_fn describe = (describe_fn)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription");
    THREADENTRY32 te = {.dwSize = sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok && nthreads < MAX_THREADS; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid)
            continue;
        struct thread *t = &threads[nthreads];
        t->tid = te.th32ThreadID;
        t->h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, t->tid);
        if (!t->h)
            continue;
        if (query)
            query(t->h, 9, &t->start, sizeof(t->start), NULL);
        PWSTR d = NULL;
        if (describe && SUCCEEDED(describe(t->h, &d)) && d) {
            lstrcpynW(t->desc, d, 64);
            LocalFree(d);
        }
        nthreads++;
    }
    CloseHandle(snap);

    FILETIME c, e, pk0, pu0, pk1, pu1;
    DWORD handles0 = 0, handles1 = 0;
    GetProcessHandleCount(proc, &handles0);
    GetProcessTimes(proc, &c, &e, &pk0, &pu0);
    for (int i = 0; i < nthreads; i++)
        times(&threads[i], &threads[i].k0, &threads[i].u0);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    if (rate > 0) {
        // Find the hot threads over the first second, then profile them for the rest of the interval.
        // Wine only reports thread times inside the own process; then every thread is profiled.
        Sleep(1000);
        int any = 0;
        for (int i = 0; i < nthreads; i++) {
            ULONGLONG k, u;
            times(&threads[i], &k, &u);
            threads[i].sampled = (k + u - threads[i].k0 - threads[i].u0) > 200000; // > 2% of a core
            any |= k + u > 0;
        }
        for (int i = 0; i < nthreads && !any; i++)
            threads[i].sampled = 1;
        for (int i = 0; i < nthreads && only_tid; i++)
            threads[i].sampled = threads[i].tid == only_tid;
        int n = (int)((seconds - 1) * rate);
        for (int s = 0; s < n; s++) {
            for (int i = 0; i < nthreads; i++)
                if (threads[i].sampled)
                    sample_thread(proc, &threads[i]);
            Sleep((DWORD)(1000 / rate));
        }
    } else {
        Sleep((DWORD)(seconds * 1000));
    }

    QueryPerformanceCounter(&t1);
    for (int i = 0; i < nthreads; i++)
        times(&threads[i], &threads[i].k1, &threads[i].u1);
    GetProcessTimes(proc, &c, &e, &pk1, &pu1);
    GetProcessHandleCount(proc, &handles1);
    double wall = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart * 1e7;

    double total = 0;
    for (int i = 0; i < nthreads; i++) {
        struct thread *t = &threads[i];
        t->pct = 100.0 * (t->k1 + t->u1 - t->k0 - t->u0) / wall;
        total += t->pct;
    }
    qsort(threads, nthreads, sizeof(threads[0]), by_pct);
    fprintf(out, "%s pid %lu: %d threads, %.1f s, threads sum %.1f%%, process %.1f%% (kernel %.1f%%)\n", exe, pid,
            nthreads, wall / 1e7, total, 100.0 * (ft(pk1) + ft(pu1) - ft(pk0) - ft(pu0)) / wall,
            100.0 * (ft(pk1) - ft(pk0)) / wall);
    fprintf(out, "handles %lu -> %lu\n", handles0, handles1);
    fprintf(out, "  tid    cpu%%  kern%%  start                                  description\n");
    char buf[128];
    for (int i = 0; i < nthreads; i++) {
        struct thread *t = &threads[i];
        fprintf(out, "%5lx  %6.2f %6.2f  %-38s %ls\n", t->tid, t->pct, 100.0 * (t->k1 - t->k0) / wall,
                symbolize(t->start, buf, sizeof(buf)), t->desc);
    }

    EnumWindows(list_window, pid);

    if (nhits) {
        qsort(hits, nhits, sizeof(hits[0]), by_count);
        for (int i = 0; i < nthreads; i++) {
            struct thread *t = &threads[i];
            if (!t->sampled)
                continue;
            int sum = 0;
            for (int j = 0; j < nhits; j++)
                if (hits[j].tid == t->tid)
                    sum += hits[j].count;
            fprintf(out, "\nthread %lx (%.1f%%, %d samples):\n", t->tid, t->pct, sum);
            for (int j = 0, shown = 0; j < nhits && shown < 10; j++)
                if (hits[j].tid == t->tid) {
                    fprintf(out, "  %5d  %s\n", hits[j].count, hits[j].chain);
                    shown++;
                }
        }
    }
    fclose(out);
    if (raw)
        fclose(raw);
    return 0;
}
