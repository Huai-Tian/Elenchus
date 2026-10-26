// =====================================================================
// dwell.c — D1 探测器: RDTSC 夹逼 exit 驻留分布（S0 基线 / A1 向量 / C3 窗口）
//
// 原理: lfence+rdtsc 夹逼一条会触发 VM-exit 的指令（cpuid/sgdt），
//       delta 分布 = 该指令的执行开销。裸机 ~数百 cycles；
//       未补偿 hypervisor 下 delta 显著抬升（exit+entry 驻留）。
//
// 
//   -n N      样本数（默认 1000000）
//   -c CPU    钉核（默认 0）
//   -i instr  探测指令（默认 cpuid；sgdt=经典 red pill 描述符表读）
//   -o FILE   原始 CSV 输出 "i,tsc,delta"（默认只打印摘要）
//   -w        窗口模式: 无限循环持续采样输出 CSV（用于 C3 安装窗口:
//             先起本进程，再 sc start 框架驱动，Ctrl-C 结束）
//
// 编译: cl /O2 dwell.c
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>

// sgdt 桩: 0F 01 01 = sgdt [rcx]; C3 = ret（经 VirtualAlloc RWX 派发）
typedef void (*SGDT_FN)(void*);
static SGDT_FN MakeSgdtStub(void)
{
    UCHAR* p = (UCHAR*)VirtualAlloc(NULL, 64, MEM_COMMIT,
        PAGE_EXECUTE_READWRITE);
    if (!p) return NULL;
    p[0] = 0x0F; p[1] = 0x01; p[2] = 0x01; p[3] = 0xC3;
    return (SGDT_FN)p;
}

static int CmpU64(const void* a, const void* b)
{
    unsigned long long x = *(const unsigned long long*)a;
    unsigned long long y = *(const unsigned long long*)b;
    return (x > y) - (x < y);
}

int RunDwell(int argc, char** argv)
{
    long long n = 1000000;
    int cpu = 0, window = 0;
    const char* instr = "cpuid";
    const char* outFile = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoll(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) instr = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outFile = argv[++i];
        else if (!strcmp(argv[i], "-w")) window = 1;
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }

    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    int useSgdt = !strcmp(instr, "sgdt");
    int useAux = !strcmp(instr, "rdtscaux");
    SGDT_FN sgdt = NULL;
    UCHAR gdtrBuf[16];
    if (useSgdt)
    {
        sgdt = MakeSgdtStub();
        if (!sgdt) { fprintf(stderr, "VirtualAlloc 失败\n"); return 1; }
    }

    // ── TSC_AUX 一致性形态（kov.dev 2026 目录向量#5）──
    // 钉核下连续 RDTSCP 的 ECX(IA32_TSC_AUX)应恒定; 被劫持/漂移 = 可检测。
    // 输出 mismatch 计数(裸机/框架预期=0)。
    if (useAux)
    {
        unsigned int aux1 = 0, aux2 = 0;
        long long mismatch = 0;
        unsigned long long seen = 0;
        for (long long i = 0; i < n; i++)
        {
            __rdtscp(&aux1);
            __rdtscp(&aux2);
            if (aux1 != aux2)
            {
                mismatch++;
            }
            if (i == 0)
            {
                seen = aux1;
            }
            else if (aux1 != seen)
            {
                mismatch++;    // 首值之外的漂移也计
            }
        }
        printf("rdtscaux: n=%lld mismatch=%lld first_aux=0x%llX %s\n",
            n, mismatch, seen,
            mismatch == 0 ? "PASS(恒定)" : "FAIL(TSC_AUX漂移)");
        return 0;
    }

    FILE* out = NULL;
    if (outFile) { out = fopen(outFile, "w"); if (!out) { perror("fopen"); return 1; } }
    else if (window) { out = stdout; }
    if (out) fprintf(out, "i,tsc,delta\n");

    unsigned long long* deltas = NULL;
    if (!window)
    {
        deltas = (unsigned long long*)malloc((size_t)n * sizeof(*deltas));
        if (!deltas) { fprintf(stderr, "oom\n"); return 1; }
    }

    int regs[4];
    long long done = 0;

    // 预热（缓存/分支训练）
    for (int w = 0; w < 10000; w++)
    {
        if (useSgdt) sgdt(gdtrBuf); else __cpuid(regs, 1);
    }

    if (window)
    {
        // C3 窗口模式: 持续输出 (tsc, delta)，离线看毛刺
        for (;;)
        {
            unsigned long long t0, t1;
            _mm_lfence(); t0 = __rdtsc();
            if (useSgdt) sgdt(gdtrBuf); else __cpuid(regs, 1);
            _mm_lfence(); t1 = __rdtsc();
            fprintf(out, "%lld,%llu,%llu\n", done++, t0, t1 - t0);
            if (done % 64 == 0) fflush(out);
        }
    }

    for (long long i = 0; i < n; i++)
    {
        unsigned long long t0, t1;
        _mm_lfence(); t0 = __rdtsc();
        if (useSgdt) sgdt(gdtrBuf); else __cpuid(regs, 1);
        _mm_lfence(); t1 = __rdtsc();
        deltas[i] = t1 - t0;
    }

    // 摘要统计
    qsort(deltas, (size_t)n, sizeof(*deltas), CmpU64);
    unsigned long long med = deltas[n / 2];
    unsigned long long p90 = deltas[(long long)(n * 0.90)];
    unsigned long long p99 = deltas[(long long)(n * 0.99)];
    unsigned long long p999 = deltas[(long long)(n * 0.999)];
    unsigned long long mx = deltas[n - 1];
    double sum = 0;
    for (long long i = 0; i < n; i++) sum += (double)deltas[i];

    fprintf(stderr, "instr=%s cpu=%d n=%lld\n", instr, cpu, n);
    fprintf(stderr, "median=%llu p90=%llu p99=%llu p999=%llu max=%llu mean=%.1f\n",
        med, p90, p99, p999, mx, sum / n);

    if (outFile && out)
    {
        // 只输出前 100 万行原始数据防巨文件
        for (long long i = 0; i < n && i < 1000000; i++)
            fprintf(out, "%lld,%llu\n", i, deltas[i]);
        fclose(out);
    }
    if (deltas) free(deltas);
    return 0;
}
