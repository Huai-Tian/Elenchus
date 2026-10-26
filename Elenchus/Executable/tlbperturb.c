// =====================================================================
// tlbperturb.c — A4 探测器: exit 诱发 TLB 扰动（D'Elia BHEU'19 Detection 4）
//
// 原理: 分配 N 页 → 全量 touch 填满 TLB → 触发一次被拦截指令(exit)
//       → 复测全量 touch 时间。裸机: cpuid 不刷 TLB → 前后同量级;
//       VMM(其 root 执行/CR3 重载/INVEPT 可扰动 TLB) → exit 后访问
//       时间抬升。零 exit 的 hook 路径不产生扰动 = 本设计的正面证据;
//       时钟读路径(每次 exit)是否扰动 = 直接检验"封堵的代价转移"。
//
// 
//                      [-t cpuid|clock|none]
//   -t cpuid  触发=CPUID(经典被拦截指令)
//   -t clock  触发=读 HPET(经 hptmap 驱动——时钟封堵路径的 exit)
//   -t none   对照(无触发, 测噪声底)
//
// 输出: 每轮 (passA_cycles, passB_cycles) + 汇总 B/A 比值中位数
// 判读: 三形态(裸/VMware/框架)对照; 框架 hook 驻留下 -t cpuid 与
//       -t clock 分别给出两类 exit 的扰动画像。
// 编译: cl /O2 tlbperturb.c
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>

#define PAGE_SIZE 4096   // 用户态无 ntddk 的 PAGE_SIZE

#define ELENCHUS_DEV "\\\\.\\Elenchus"
#define ELEN_IOCTL_HPTMAP_MAP    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ELEN_IOCTL_HPTMAP_READ   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _ROW { double ratio; unsigned long long a, b; } ROW;

static int CmpDouble(const void* x, const void* y)
{
    double a = *(const double*)x, b = *(const double*)y;
    return (a > b) - (a < b);
}

int RunTlb(int argc, char** argv)
{
    int pages = 1024, reps = 50, cpu = 0;
    const char* trig = "cpuid";
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) pages = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) trig = argv[++i];
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }
    if (pages < 16) pages = 16;
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // 每页一 volatile word, 隔页距大于缓存行
    volatile LONG** pgs = (volatile LONG**)malloc(pages * sizeof(*pgs));
    if (!pgs) { fprintf(stderr, "oom\n"); return 1; }
    for (int i = 0; i < pages; i++)
    {
        pgs[i] = (volatile LONG*)VirtualAlloc(NULL, PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
        if (!pgs[i]) { fprintf(stderr, "VirtualAlloc 失败 @%d\n", i); return 1; }
    }

    // clock 触发需要 hptmap
    HANDLE hDrv = INVALID_HANDLE_VALUE;
    ULONG slot = 0;
    int useClock = !strcmp(trig, "clock");
    if (useClock)
    {
        hDrv = CreateFileA(ELENCHUS_DEV, GENERIC_READ | GENERIC_WRITE, 0,
            NULL, OPEN_EXISTING, 0, NULL);
        if (hDrv == INVALID_HANDLE_VALUE)
        {
            fprintf(stderr, "hptmap 不可用(sc start elenchus?): %lu\n", GetLastError());
            return 1;
        }
        ULONG64 in = 0xFED00000ULL + 0xF0ULL;
        ULONG outSlot = 0; DWORD br;
        if (!DeviceIoControl(hDrv, ELEN_IOCTL_HPTMAP_MAP, &in, sizeof(in),
            &outSlot, sizeof(outSlot), &br, NULL))
        {
            fprintf(stderr, "HPET map 失败\n"); return 1;
        }
        slot = outSlot;
    }

    ROW* rows = (ROW*)malloc(reps * sizeof(*rows));
    int regs[4];
    unsigned int aux;
    LONG sink = 0;

    // 预热
    for (int i = 0; i < pages; i++) *pgs[i] = i;
    __cpuid(regs, 1);

    for (int r = 0; r < reps; r++)
    {
        // 填满 TLB(全 touch)
        for (int i = 0; i < pages; i++) sink += *pgs[i];
        // pass A: 紧接着再全 touch(TLB 命中形态)
        unsigned long long a0 = __rdtscp(&aux);
        for (int i = 0; i < pages; i++) sink += *pgs[i];
        unsigned long long a1 = __rdtscp(&aux);

        // 重新填满
        for (int i = 0; i < pages; i++) sink += *pgs[i];
        // 触发
        if (useClock)
        {
            ULONG inS = slot; ULONG64 v = 0; DWORD br;
            DeviceIoControl(hDrv, ELEN_IOCTL_HPTMAP_READ, &inS, sizeof(inS),
                &v, sizeof(v), &br, NULL);
        }
        else if (!strcmp(trig, "cpuid"))
        {
            __cpuid(regs, 1);
        }
        // none: 无触发
        // pass B: 触发后全 touch
        unsigned long long b0 = __rdtscp(&aux);
        for (int i = 0; i < pages; i++) sink += *pgs[i];
        unsigned long long b1 = __rdtscp(&aux);

        rows[r].a = a1 - a0;
        rows[r].b = b1 - b0;
        rows[r].ratio = rows[r].a ? (double)rows[r].b / (double)rows[r].a : 0.0;
    }

    double* rs = (double*)malloc(reps * sizeof(*rs));
    for (int r = 0; r < reps; r++) rs[r] = rows[r].ratio;
    qsort(rs, reps, sizeof(*rs), CmpDouble);
    double medA = 0, medB = 0;
    for (int r = 0; r < reps; r++) { medA += (double)rows[r].a; medB += (double)rows[r].b; }
    medA /= reps; medB /= reps;

    printf("tlbperturb: trig=%s pages=%d reps=%d cpu=%d sink=%ld\n",
        trig, pages, reps, cpu, sink);
    printf("passA 均值=%.0f cycles  passB 均值=%.0f cycles  B/A 中位数=%.3f p90=%.3f\n",
        medA, medB, rs[reps / 2], rs[(int)(reps * 0.9)]);
    printf("判读: 三形态对照。裸机 B/A≈1; VMM 下 exit 扰动 TLB 则 B/A>1;\n");
    printf("      框架 hook 命中(零exit)不扰动; 时钟读路径每次exit=重点观察对象。\n");
    return 0;
}
