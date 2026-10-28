// =====================================================================
// detect.c — 归因否认 ROC 实验: 在场检测者仿真 (纯 R3, 无驱动无管理员)
//
// 论文 §3.6 的实证臂: 模拟恶意软件的探测工作流——它只能测量本机,
// 没有裸机真值参照。输出观测特征向量(CSV 一行), 阈值判决全部离线
// (Script/roc.py 扫阈值画 ROC), 本工具只采集不做终极裁决。
//
// 探针电池(对应 S3 设计 P1-P9):
//   P1 CPUID.1.ECX[31] hypervisor 位
//   P2 CPUID 0x40000000 管理程序 leaf(max leaf + vendor 编码)
//   P3 CPUID dwell 分布(RDTSC 夹逼)
//   P4 RDTSC dwell(测量开销基线)
//   P5 CPUID/RDTSC 成本比
//   P6 SGDT dwell(经典 red pill, RWX 桩)
//   P7 跨核 TSC 同步(双线程钉相邻核, 单调令牌屏障后同刻读 TSC)
//   P8 QPC-TSC 相干性(配对采样 OLS 去趋势残差, ~1s 慢采样窗)
//   P9 TSC 频率 QPC 标定 + 双窗口稳定性
//
// v3 (2026-10-28): P7 VM 安全化——判例: v2 在 VMware guest 死锁
//   (三方锁步屏障无超时; vCPU 被宿主反调度 → 对端无限自旋 →
//    guest UI 饿死窗口泛白, 无文件产出)。修复:
//   ① 全部屏障等待带 pause 计数超时(~150ms) → 协作 abort
//   ② 自旋每 4096 pause 让步 SwitchToThread(防同核饿死低优先级)
//   ③ 轮时长过滤: release→written >100µs 的轮视为被调度污染剔除
//   ④ 干净轮 <100 → xc 记无效; 整体软超时 8s
//   ⑤ worker 优先级降 HIGHEST(main 保持 TIME_CRITICAL 抢占推进)
//   ⑥ 新增末列 xc_n(干净轮数, 数据质量注记)
//
// 用法: Executable.exe detect [-t 标签] [-f bare|fw|vm|wsl|...] [-n N]
//            [-c CPU] [-o obs.csv]
//   -t   机器标签(默认: CPU brand 串 + 核数自动生成)
//   -f   形态标签(分析按此列分组, 建议必填)
//   -n   dwell 采样数(默认 200000)
//   -c   基准核(默认 0; P7 用 c 与 c+1, 主线程让位到 c+2)
//   -o   输出 CSV(追加一行; 无 -o 打印人类可读报告)
//
// 编译: 随 Executable.vcxproj; 纯 Win32 API + intrinsic, 无驱动依赖。
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

// ---------- 小工具 ----------
static int CmpU64(const void* a, const void* b)
{
    unsigned long long x = *(const unsigned long long*)a;
    unsigned long long y = *(const unsigned long long*)b;
    return (x > y) - (x < y);
}

static unsigned long long Pct(unsigned long long* sorted, long long n, double p)
{
    long long i = (long long)(n * p);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return sorted[i];
}

typedef void (*SGDT_FN)(void*);
static SGDT_FN MakeSgdtStub(void)
{
    UCHAR* p = (UCHAR*)VirtualAlloc(NULL, 64, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!p) return NULL;
    p[0] = 0x0F; p[1] = 0x01; p[2] = 0x01; p[3] = 0xC3;   // sgdt [rcx]; ret
    return (SGDT_FN)p;
}

// 自旋等待约 cycles 个周期(校准粗糙即可, 只需"毫秒级窗")
static void SpinCycles(unsigned long long cycles)
{
    unsigned long long t0 = __rdtsc();
    while (__rdtsc() - t0 < cycles) _mm_pause();
}

// ---------- P7: 跨核 TSC 同步(单调令牌屏障, VM 安全版) ----------
// 协议: go 为单调递增轮次令牌(2i+1=放行, 2i+2=收轮)。
//   worker: arrived++ → 自旋等 go>=2i+1 → rdtsc → 落盘 → written++
//           → 自旋等 go>=2i+2 → 下一轮
//   main  : 带 deadline 等 arrived → go=2i+1 → 带 deadline 等 written
//           → 读 tscA/tscB(x86-TSO: written 可见 ⇒ TSC 写可见)
//           → 记差+轮时长 → go=2i+2 → 下一轮
// 任何一方超 deadline → abort=1 → 全员协作退出 → 兜底强杀。
// 轮时长>100µs = 该轮被调度污染(某 vCPU 曾被反调度), 统计时剔除。
#define XC_YIELD_EVERY   4096      // 自旋让步周期(≈40µs @3.9GHz)
#define XC_WAIT_Pauses   20000000  // 单次等待上限(≈150ms @3.9GHz)
#define XC_ITERS         800
#define XC_CLEAN_MIN     100       // 干净轮下限, 不足记无效
#define XC_ROUND_FILT_US 100       // 轮污染过滤阈值

typedef struct {
    volatile LONG go;
    volatile LONG arrived;
    volatile LONG written;
    volatile LONG abort;
    unsigned long long tscA;
    unsigned long long tscB;
    long long iterations;
} XC_ARG;

static DWORD WINAPI XcWorkerA(LPVOID p)
{
    XC_ARG* a = (XC_ARG*)p;
    for (long long i = 0; i < a->iterations; i++)
    {
        LONG release = (LONG)(2 * i + 1);
        LONG end     = (LONG)(2 * i + 2);
        long long spin = 0;
        InterlockedIncrement(&a->arrived);
        while (a->go < release)
        {
            if (a->abort) return 0;
            if (++spin >= XC_YIELD_EVERY) { spin = 0; SwitchToThread(); }
            _mm_pause();
        }
        a->tscA = __rdtsc();
        InterlockedIncrement(&a->written);
        spin = 0;
        while (a->go < end)
        {
            if (a->abort) return 0;
            if (++spin >= XC_YIELD_EVERY) { spin = 0; SwitchToThread(); }
            _mm_pause();
        }
    }
    return 0;
}

static DWORD WINAPI XcWorkerB(LPVOID p)
{
    XC_ARG* a = (XC_ARG*)p;
    for (long long i = 0; i < a->iterations; i++)
    {
        LONG release = (LONG)(2 * i + 1);
        LONG end     = (LONG)(2 * i + 2);
        long long spin = 0;
        InterlockedIncrement(&a->arrived);
        while (a->go < release)
        {
            if (a->abort) return 0;
            if (++spin >= XC_YIELD_EVERY) { spin = 0; SwitchToThread(); }
            _mm_pause();
        }
        a->tscB = __rdtsc();
        InterlockedIncrement(&a->written);
        spin = 0;
        while (a->go < end)
        {
            if (a->abort) return 0;
            if (++spin >= XC_YIELD_EVERY) { spin = 0; SwitchToThread(); }
            _mm_pause();
        }
    }
    return 0;
}

static void XcoreTsc(int cpuA, int cpuB, long long iterations,
                     unsigned long long* med, unsigned long long* p99,
                     long long* cleanOut, int mainCpu, double tscMHz)
{
    XC_ARG a;
    ZeroMemory(&a, sizeof(a));
    a.iterations = iterations;
    unsigned long long* diffs = (unsigned long long*)malloc((size_t)iterations * sizeof(*diffs));
    unsigned long long* clean = (unsigned long long*)malloc((size_t)iterations * sizeof(*clean));
    if (!diffs || !clean)
    {
        *med = *p99 = ~0ULL; *cleanOut = 0;
        if (diffs) free(diffs);
        if (clean) free(clean);
        return;
    }

    // 主线程让位到第三核; 无第三核则全核浮动(靠让步防饿死)
    if (mainCpu >= 0)
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << mainCpu);
    else
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)-1);

    HANDLE tb = CreateThread(NULL, 0, XcWorkerB, &a, CREATE_SUSPENDED, NULL);
    HANDLE ta = CreateThread(NULL, 0, XcWorkerA, &a, CREATE_SUSPENDED, NULL);
    if (!ta || !tb)
    {
        if (ta) CloseHandle(ta);
        if (tb) CloseHandle(tb);
        free(diffs); free(clean);
        *med = *p99 = ~0ULL; *cleanOut = 0;
        return;
    }
    SetThreadAffinityMask(tb, (DWORD_PTR)1 << cpuB);
    SetThreadPriority(tb, THREAD_PRIORITY_HIGHEST);      // v3: 降级, main 可抢占
    SetThreadAffinityMask(ta, (DWORD_PTR)1 << cpuA);
    SetThreadPriority(ta, THREAD_PRIORITY_HIGHEST);
    ResumeThread(tb);
    ResumeThread(ta);

    long long nClean = 0;
    long long doneRounds = 0;
    LARGE_INTEGER qf, tAll0, tAll1;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&tAll0);

    for (long long i = 0; i < iterations; i++)
    {
        LONG release = (LONG)(2 * i + 1);
        LONG end     = (LONG)(2 * i + 2);
        long long spin = 0;

        while (a.arrived < (LONG)(2 * i + 2))
        {
            if (++spin >= XC_WAIT_Pauses) goto abort_out;
            if ((spin & (XC_YIELD_EVERY - 1)) == 0) SwitchToThread();
            _mm_pause();
        }
        unsigned long long tRel = __rdtsc();
        InterlockedExchange(&a.go, release);
        spin = 0;
        while (a.written < (LONG)(2 * i + 2))
        {
            if (++spin >= XC_WAIT_Pauses) goto abort_out;
            if ((spin & (XC_YIELD_EVERY - 1)) == 0) SwitchToThread();
            _mm_pause();
        }
        unsigned long long tDone = __rdtsc();
        unsigned long long x = a.tscA, y = a.tscB;
        diffs[doneRounds] = (x > y) ? (x - y) : (y - x);

        // 轮污染过滤: release→written 超 100µs = 有参与者被调度出去
        unsigned long long filtCycles =
            (unsigned long long)(XC_ROUND_FILT_US * tscMHz);
        if (tscMHz <= 0 || tDone - tRel < filtCycles)
        {
            clean[nClean++] = diffs[doneRounds];
        }
        doneRounds++;

        InterlockedExchange(&a.go, end);

        // 整体软超时 8s
        QueryPerformanceCounter(&tAll1);
        if ((double)(tAll1.QuadPart - tAll0.QuadPart) / qf.QuadPart > 8.0)
            break;
    }

abort_out:
    InterlockedExchange(&a.abort, 1);
    // 抬 go 让已过最后令牌的 worker 兜底出环
    InterlockedExchange(&a.go, (LONG)(2 * iterations + 4));
    if (WaitForSingleObject(ta, 3000) != WAIT_OBJECT_0)
        TerminateThread(ta, 0);                          // 防御性兜底
    if (WaitForSingleObject(tb, 3000) != WAIT_OBJECT_0)
        TerminateThread(tb, 0);
    CloseHandle(ta); CloseHandle(tb);

    if (nClean >= XC_CLEAN_MIN)
    {
        qsort(clean, (size_t)nClean, sizeof(*clean), CmpU64);
        *med = Pct(clean, nClean, 0.5);
        *p99 = Pct(clean, nClean, 0.99);
    }
    else
    {
        *med = *p99 = ~0ULL;    // 干净轮不足: 记无效(调度污染主导)
    }
    *cleanOut = nClean;
    free(diffs); free(clean);
}

// ---------- P8: QPC-TSC 相干性 ----------
typedef struct { unsigned long long tscMid, qpc; } CLKPAIR;

// OLS: qpc = a*tsc + b; 残差 |qpc-fit|/a 换算回 TSC tick
static void ClkCoherence(CLKPAIR* s, long long n,
                         unsigned long long* residMed, unsigned long long* residP99)
{
    double sx = 0, sy = 0;
    for (long long i = 0; i < n; i++) { sx += (double)s[i].tscMid; sy += (double)s[i].qpc; }
    double mx = sx / n, my = sy / n;
    double sxy = 0, sxx = 0;
    for (long long i = 0; i < n; i++)
    {
        double dx = (double)s[i].tscMid - mx;
        sxy += dx * ((double)s[i].qpc - my);
        sxx += dx * dx;
    }
    double a = sxx > 0 ? sxy / sxx : 0;
    double b = my - a * mx;
    unsigned long long* r = (unsigned long long*)malloc((size_t)n * sizeof(*r));
    if (!r) { *residMed = *residP99 = ~0ULL; return; }
    for (long long i = 0; i < n; i++)
    {
        double fit = a * (double)s[i].tscMid + b;
        double res = (double)s[i].qpc - fit;
        if (res < 0) res = -res;
        r[i] = (unsigned long long)(a > 0 ? res / a : res);
    }
    qsort(r, (size_t)n, sizeof(*r), CmpU64);
    *residMed = Pct(r, n, 0.5);
    *residP99 = Pct(r, n, 0.99);
    free(r);
}

// 一窗配对采样: nPairs 对, 对间自旋 gapCycles(0=紧采样)
static void SampleClkWindow(CLKPAIR* s, long long nPairs,
                            unsigned long long gapCycles,
                            unsigned long long* tscSpan)
{
    LARGE_INTEGER q;
    unsigned long long tStart = __rdtsc(), tEnd;
    for (long long i = 0; i < nPairs; i++)
    {
        unsigned long long t0, t1;
        _mm_lfence(); t0 = __rdtsc();
        QueryPerformanceCounter(&q);
        _mm_lfence(); t1 = __rdtsc();
        s[i].tscMid = (t0 + t1) / 2;
        s[i].qpc = (unsigned long long)q.QuadPart;
        if (gapCycles) SpinCycles(gapCycles);
    }
    tEnd = __rdtsc();
    if (tscSpan) *tscSpan = tEnd - tStart;
}

// 粗频标定(P7 轮过滤用): Sleep(10) 窗内 TSC 增量
static double EstimateTscMHz(void)
{
    unsigned long long t0 = __rdtsc();
    Sleep(10);
    unsigned long long t1 = __rdtsc();
    return (double)(t1 - t0) / 10.0 / 1000.0;   // cycles/10ms → MHz
}

// ---------- 主流程 ----------
int RunDetect(int argc, char** argv)
{
    const char* tag = NULL;
    const char* form = "unknown";
    const char* outFile = NULL;
    long long n = 200000;
    int cpu = 0;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) tag = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) form = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoll(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outFile = argv[++i];
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }
    if (n < 10000) n = 10000;

    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // ── 机器指纹 ──
    int regs[4]; char brand[49] = { 0 };
    __cpuid(regs, 0x80000000);
    unsigned int maxExt = (unsigned int)regs[0];
    if (maxExt >= 0x80000004u)    // brand 串在扩展 leaf 0x80000002-4
    {
        int rb[4];
        __cpuid(rb, 0x80000002); memcpy(brand + 0, rb, 16);
        __cpuid(rb, 0x80000003); memcpy(brand + 16, rb, 16);
        __cpuid(rb, 0x80000004); memcpy(brand + 32, rb, 16);
    }
    SYSTEM_INFO si; GetSystemInfo(&si);
    char autoTag[128];
    if (!tag)
    {
        char clean[64] = { 0 }; int j = 0, sp = 0;
        for (int i = 0; brand[i] && j < 40; i++)
        {
            if (brand[i] == ' ') { if (!sp) clean[j++] = ' '; sp = 1; }
            else { clean[j++] = brand[i]; sp = 0; }
        }
        _snprintf(autoTag, sizeof(autoTag), "%s_%dc", clean,
                  (int)si.dwNumberOfProcessors);
        tag = autoTag;
    }

    // ── P1: hypervisor 位 ──
    __cpuid(regs, 1);
    int hvBit = (regs[2] >> 31) & 1;

    // ── P2: 管理程序 leaf ──
    int hvMaxLeaf = 0; unsigned long long hvVendor = 0;
    __cpuid(regs, 0x40000000);
    hvMaxLeaf = regs[0];
    hvVendor = ((unsigned long long)(unsigned int)regs[1] << 32)
             | ((unsigned long long)(unsigned int)regs[2]);

    // ── 预热 ──
    UCHAR gdtr[16];
    SGDT_FN sgdt = MakeSgdtStub();
    for (int w = 0; w < 10000; w++)
    {
        __cpuid(regs, 1);
        if (sgdt) sgdt(gdtr);
    }

    // ── P3/P4/P6: dwell 分布 ──
    unsigned long long* dCpuid = (unsigned long long*)malloc((size_t)n * sizeof(*dCpuid));
    unsigned long long* dRdtsc = (unsigned long long*)malloc((size_t)n * sizeof(*dRdtsc));
    unsigned long long* dSgdt  = sgdt ? (unsigned long long*)malloc((size_t)n * sizeof(*dSgdt)) : NULL;
    if (!dCpuid || !dRdtsc || (sgdt && !dSgdt)) { fprintf(stderr, "oom\n"); return 1; }

    for (long long i = 0; i < n; i++)
    {
        unsigned long long t0, t1;
        _mm_lfence(); t0 = __rdtsc();
        __cpuid(regs, 1);
        _mm_lfence(); t1 = __rdtsc();
        dCpuid[i] = t1 - t0;

        _mm_lfence(); t0 = __rdtsc();
        _mm_lfence(); t1 = __rdtsc();
        dRdtsc[i] = t1 - t0;

        if (dSgdt)
        {
            _mm_lfence(); t0 = __rdtsc();
            sgdt(gdtr);
            _mm_lfence(); t1 = __rdtsc();
            dSgdt[i] = t1 - t0;
        }
    }
    qsort(dCpuid, (size_t)n, sizeof(*dCpuid), CmpU64);
    qsort(dRdtsc, (size_t)n, sizeof(*dRdtsc), CmpU64);
    if (dSgdt) qsort(dSgdt, (size_t)n, sizeof(*dSgdt), CmpU64);

    unsigned long long cpuidMed = Pct(dCpuid, n, 0.5);
    unsigned long long cpuidP90 = Pct(dCpuid, n, 0.90);
    unsigned long long cpuidP99 = Pct(dCpuid, n, 0.99);
    unsigned long long rdtscMed = Pct(dRdtsc, n, 0.5);
    unsigned long long sgdtMed = dSgdt ? Pct(dSgdt, n, 0.5) : 0;
    unsigned long long sgdtP99 = dSgdt ? Pct(dSgdt, n, 0.99) : 0;
    long long over2x = 0;
    for (long long i = 0; i < n; i++)
        if (cpuidMed > 0 && dCpuid[i] > 2 * cpuidMed) over2x++;
    double over2xFrac = (double)over2x / (double)n;
    double ratioMed = rdtscMed ? (double)cpuidMed / (double)rdtscMed : 0;

    // ── P7: 跨核 TSC 同步(核 cpu 与 cpu+1, 主线程让位 cpu+2) ──
    double tscMHzEst = EstimateTscMHz();     // 轮过滤阈值用
    unsigned long long xcMed = 0, xcP99 = 0;
    long long xcClean = 0;
    if ((DWORD)(cpu + 1) < si.dwNumberOfProcessors)
    {
        int mainCpu = ((DWORD)(cpu + 2) < si.dwNumberOfProcessors) ? cpu + 2 : -1;
        XcoreTsc(cpu, cpu + 1, XC_ITERS, &xcMed, &xcP99, &xcClean,
                 mainCpu, tscMHzEst);
        // 恢复基准核亲和(P8/P9 继续在 cpu 上)
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
    }
    else { xcMed = xcP99 = ~0ULL; xcClean = 0; }   // 单核形态记无效

    // ── P8/P9: QPC-TSC 相干性(慢采样窗) + 频率双窗口 ──
    long long nClk = 20000;
    CLKPAIR* s = (CLKPAIR*)malloc((size_t)nClk * sizeof(*s));
    unsigned long long residMed = 0, residP99 = 0;
    double tscMHzW1 = 0, tscMHzW2 = 0;
    if (s)
    {
        LARGE_INTEGER qf, q0, q1;
        QueryPerformanceFrequency(&qf);

        // 窗口1: ~1s 慢采样(对间 ~50µs 自旋), 相干性 + 频率
        QueryPerformanceCounter(&q0);
        unsigned long long span = 0;
        SampleClkWindow(s, nClk, 150000ULL, &span);   // 150k cyc ≈ 50µs @3GHz
        QueryPerformanceCounter(&q1);
        double dt = (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
        if (dt > 0) tscMHzW1 = (double)span / dt / 1e6;
        ClkCoherence(s, nClk, &residMed, &residP99);

        // 窗口2: 隔 1.5s 再测频率稳定性(~0.25s)
        Sleep(1500);
        QueryPerformanceCounter(&q0);
        unsigned long long span2 = 0;
        SampleClkWindow(s, 5000, 150000ULL, &span2);
        QueryPerformanceCounter(&q1);
        dt = (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
        if (dt > 0) tscMHzW2 = (double)span2 / dt / 1e6;
        free(s);
    }
    double tscDriftPpm = tscMHzW1 > 0 ?
        (tscMHzW2 - tscMHzW1) / tscMHzW1 * 1e6 : 0;

    double residNsMed = tscMHzW1 > 0 ? (double)residMed / tscMHzW1 : 0;
    double residNsP99 = tscMHzW1 > 0 ? (double)residP99 / tscMHzW1 : 0;

    // ── 输出 ──
    if (outFile)
    {
        int fresh = 0;
        FILE* f = fopen(outFile, "r");
        if (f) { fclose(f); } else fresh = 1;
        f = fopen(outFile, "a");
        if (!f) { perror("fopen"); return 1; }
        if (fresh)
            fprintf(f, "tag,form,hvbit,hvmaxleaf,hvvendor,cpuid_med,cpuid_p90,"
                       "cpuid_p99,cpuid_over2x,rdtsc_med,ratio_med,sgdt_med,"
                       "sgdt_p99,xc_med,xc_p99,resid_ns_med,resid_ns_p99,"
                       "tsc_mhz,tsc_drift_ppm,xc_n\n");
        fprintf(f, "%s,%s,%d,%d,%016llX,%llu,%llu,%llu,%.6f,%llu,%.3f,%llu,"
                   "%llu,%llu,%llu,%.1f,%.1f,%.1f,%.1f,%lld\n",
            tag, form, hvBit, hvMaxLeaf, hvVendor,
            cpuidMed, cpuidP90, cpuidP99, over2xFrac, rdtscMed, ratioMed,
            sgdtMed, sgdtP99, xcMed, xcP99,
            residNsMed, residNsP99, tscMHzW1, tscDriftPpm, xcClean);
        fclose(f);
        printf("[detect] 已追加 → %s (tag=%s form=%s, P7 干净轮=%lld/%d)\n",
               outFile, tag, form, xcClean, XC_ITERS);
    }
    else
    {
        printf("machine : %s\n", tag);
        printf("form    : %s\n", form);
        printf("P1 hvbit          : %d %s\n", hvBit,
               hvBit ? "(虚拟化暴露!)" : "(清洁)");
        printf("P2 hvleaf         : max=0x%X vendor=%016llX\n", hvMaxLeaf, hvVendor);
        printf("P3 cpuid dwell    : med=%llu p90=%llu p99=%llu over2x=%.4f\n",
               cpuidMed, cpuidP90, cpuidP99, over2xFrac);
        printf("P4 rdtsc dwell    : med=%llu\n", rdtscMed);
        printf("P5 ratio          : %.2f\n", ratioMed);
        printf("P6 sgdt dwell     : med=%llu p99=%llu\n", sgdtMed, sgdtP99);
        printf("P7 xcore tsc      : med=%llu p99=%llu (干净轮 %lld/%d) %s\n",
               xcMed, xcP99, xcClean, XC_ITERS,
               xcMed == ~0ULL ? (xcClean ? "(污染主导, 无效)" : "(单核, 无效)") : "");
        printf("P8 qpc-tsc resid  : med=%.1fns p99=%.1fns\n", residNsMed, residNsP99);
        printf("P9 tsc freq       : %.1f MHz drift=%.1f ppm\n", tscMHzW1, tscDriftPpm);
        printf("\n(阈值判决离线 roc.py —— 本工具只采集观测向量)\n");
    }

    free(dCpuid); free(dRdtsc); if (dSgdt) free(dSgdt);
    if (sgdt) VirtualFree(sgdt, 0, MEM_RELEASE);
    return 0;
}
