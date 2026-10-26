// =====================================================================
// bench.c — PERF R3 前端: 端到端每跳开销 / syscall 洪泛 / 上下文切换 / kbench
//
// 模式:
//   Executable.exe bench hits  [-n N] [-o raw.csv]   每次调用 rdtscp 夹逼 CloseHandle
//                                             (NtClose 框架钩子的端到端开销)
//   Executable.exe bench batch [-n N]                批量夹逼(1000次/批) 低噪声均值
//   Executable.exe bench flood [-n N]                10^7 次 syscall 吞吐(wall clock)
//   Executable.exe bench ctxsw [-n N]                双线程事件乒乓 = 上下文切换成本
//   Executable.exe bench k     [-n N] [--inline]     经 kbench 驱动内核侧精确每跳
//                                             --inline = 自产 inline 基线
//   Executable.exe bench kaddr                      打印 kbench 靶 VA(框架 demo 挂钩用)
//
// 全部输出 CSV + 控制台摘要; 三形态(裸/对照/框架)各跑一遍对照
// 编译: cl /O2 bench.c
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>

#define ELENCHUS_DEV "\\\\.\\Elenchus"
#define ELEN_IOCTL_KBENCH_RUN       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ELEN_IOCTL_KBENCH_SETINLINE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ELEN_IOCTL_KBENCH_GETADDR   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x822, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _KB_RUN_IN  { unsigned long long Iterations; } KB_RUN_IN;
typedef struct _KB_RUN_OUT { unsigned long long CyclesPerCall, Checksum, TargetVa; } KB_RUN_OUT;

static int CmpU64(const void* a, const void* b)
{
    unsigned long long x = *(const unsigned long long*)a;
    unsigned long long y = *(const unsigned long long*)b;
    return (x > y) - (x < y);
}

static void Stats(const char* tag, unsigned long long* d, long long n)
{
    qsort(d, (size_t)n, sizeof(*d), CmpU64);
    double sum = 0;
    for (long long i = 0; i < n; i++) sum += (double)d[i];
    fprintf(stderr, "[%s] n=%lld median=%llu p90=%llu p99=%llu p999=%llu max=%llu mean=%.1f\n",
        tag, n, d[n / 2], d[(long long)(n * 0.90)], d[(long long)(n * 0.99)],
        d[(long long)(n * 0.999)], d[n - 1], sum / n);
}

static HANDLE OpenKbench(void)
{
    HANDLE h = CreateFileA(ELENCHUS_DEV, GENERIC_READ | GENERIC_WRITE, 0,
        NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "kbench 驱动不可用(sc start elenchus?): %lu\n", GetLastError());
    }
    return h;
}

// ctxsw 工作线程(双线程事件乒乓, C 兼容: 无 lambda)
typedef struct { HANDLE mine, theirs; long long n; } CTX_ARG;
static DWORD WINAPI CtxWorker(LPVOID p)
{
    CTX_ARG* a = (CTX_ARG*)p;
    for (long long i = 0; i < a->n; i++)
    {
        SetEvent(a->theirs);
        WaitForSingleObject(a->mine, INFINITE);
    }
    return 0;
}

int RunBench(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "用法见文件头注释\n"); return 2; }
    const char* mode = argv[1];
    long long n = 10000000;
    int inlineOn = 0;
    const char* outFile = NULL;
    for (int i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--inline")) inlineOn = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outFile = argv[++i];
    }
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << 0);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    if (!strcmp(mode, "hits") || !strcmp(mode, "batch"))
    {
        // 预热
        for (int i = 0; i < 10000; i++) CloseHandle((HANDLE)-1);
        unsigned int aux;
        if (mode[0] == 'h')
        {
            unsigned long long* d = (unsigned long long*)malloc((size_t)n * sizeof(*d));
            for (long long i = 0; i < n; i++)
            {
                unsigned long long t0 = __rdtscp(&aux);
                CloseHandle((HANDLE)-1);
                unsigned long long t1 = __rdtscp(&aux);
                d[i] = t1 - t0;
            }
            if (outFile)
            {
                FILE* f = fopen(outFile, "w");
                if (f) { for (long long i = 0; i < n; i++) fprintf(f, "%llu\n", d[i]); fclose(f); }
            }
            Stats("hits:end-to-end CloseHandle", d, n);
            free(d);
        }
        else
        {
            const long long B = 1000;
            unsigned long long* d = (unsigned long long*)malloc((size_t)(n / B) * sizeof(*d));
            long long bi = 0;
            for (long long i = 0; i + B <= n; i += B)
            {
                unsigned long long t0 = __rdtscp(&aux);
                for (long long k = 0; k < B; k++) CloseHandle((HANDLE)-1);
                unsigned long long t1 = __rdtscp(&aux);
                d[bi++] = (t1 - t0) / B;
            }
            Stats("batch:end-to-end CloseHandle", d, bi);
            free(d);
        }
    }
    else if (!strcmp(mode, "flood"))
    {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        for (long long i = 0; i < n; i++) CloseHandle((HANDLE)-1);
        QueryPerformanceCounter(&t1);
        double sec = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
        printf("flood: %lld 次syscall %.3fs → %.0f 次/s, %.1f cycles/次(按标称频率折算见CSV)\n",
            n, sec, n / sec, 0.0);
    }
    else if (!strcmp(mode, "ctxsw"))
    {
        // 双线程事件乒乓: 每迭代 = 2次 SetEvent + 2次等待 ≈ 2 次上下文切换
        HANDLE eA = CreateEventA(NULL, FALSE, FALSE, NULL);
        HANDLE eB = CreateEventA(NULL, FALSE, FALSE, NULL);
        CTX_ARG arg;
        arg.mine = eA; arg.theirs = eB; arg.n = n;
        HANDLE th = CreateThread(NULL, 0, CtxWorker, &arg, 0, NULL);
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
        Sleep(50);                                  // B 就绪
        QueryPerformanceCounter(&t0);
        for (long long i = 0; i < n; i++)
        {
            SetEvent(eA);
            WaitForSingleObject(eB, INFINITE);
        }
        QueryPerformanceCounter(&t1);
        WaitForSingleObject(th, INFINITE);
        double sec = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
        printf("ctxsw: %lld 迭代(2切换/迭代) %.3fs → %.0f ns/切换\n",
            n, sec, sec * 1e9 / (n * 2));
        CloseHandle(th); CloseHandle(eA); CloseHandle(eB);
    }
    else if (!strcmp(mode, "k"))
    {
        HANDLE h = OpenKbench();
        if (h == INVALID_HANDLE_VALUE) return 1;
        DWORD br;
        if (inlineOn)
        {
            ULONG on = 1;
            if (!DeviceIoControl(h, ELEN_IOCTL_KBENCH_SETINLINE, &on, sizeof(on), &on, sizeof(on), &br, NULL))
            {
                fprintf(stderr, "SETINLINE 失败: %lu\n", GetLastError());
                return 1;
            }
            printf("[inline 基线已装]\n");
        }
        KB_RUN_IN in = { (unsigned long long)n };
        KB_RUN_OUT out = { 0 };
        if (!DeviceIoControl(h, ELEN_IOCTL_KBENCH_RUN, &in, sizeof(in), &out, sizeof(out), &br, NULL))
        {
            fprintf(stderr, "RUN 失败: %lu\n", GetLastError());
            return 1;
        }
        printf("kbench: %lld 次 cycles/call=%llu checksum=0x%llX(≠0=被hook改值,应≈原值)\n",
            n, out.CyclesPerCall, out.Checksum);
        if (inlineOn)
        {
            ULONG off = 0;
            DeviceIoControl(h, ELEN_IOCTL_KBENCH_SETINLINE, &off, sizeof(off), &off, sizeof(off), &br, NULL);
            printf("[inline 已卸, 还原]\n");
        }
        CloseHandle(h);
    }
    else if (!strcmp(mode, "kaddr"))
    {
        HANDLE h = OpenKbench();
        if (h == INVALID_HANDLE_VALUE) return 1;
        DWORD br;
        unsigned long long va = 0;
        if (DeviceIoControl(h, ELEN_IOCTL_KBENCH_GETADDR, NULL, 0, &va, sizeof(va), &br, NULL))
        {
            printf("KbTarget VA = 0x%llX（框架 demo main.c 以此为 Target 挂钩, 再跑 bench k）\n", va);
        }
        CloseHandle(h);
    }
    else
    {
        fprintf(stderr, "未知模式 %s\n", mode);
        return 2;
    }
    return 0;
}
