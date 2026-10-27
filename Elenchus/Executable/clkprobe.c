// =====================================================================
// clkprobe.c — A2 跨时钟域相干性探测器 (R3 采样器)
//
// 原理: 同时读 TSC(rdtsc) 与另一时钟(HPET MMIO / PM_TMR / QPC),
//       配对采样 (tsc_mid, other) 输出 CSV。离线做线性回归残差分析:
//         裸机            → 残差 = 量化噪声(窄单峰)
//         未补偿 hypervisor → 残差 = exit 驻留(宽/双峰/台阶)
//         被测框架        → 期望与裸机同形(时间轴封堵成功)
//
// 
//   -h        HPET 轴 (默认, 经 hptmap 驱动读 0xFED00000+0xF0)
//   -p        PM_TMR 轴 (从 ACPI FADT 解析基址, 经 hptmap 驱动)
//   -q        QPC 轴    (纯 R3, 无需驱动; 注意 QPC 可能即 TSC 源)
//   -n N      采样对数 (默认 100000)
//   -c CPU    钉核 (默认 0)
//   -o FILE   输出 CSV (默认 stdout: "tsc,other" 每行一对)
//   -m US     采样间隔微秒 (默认 0 = 全速紧采样; A2 正是要暴露
//             紧采样下的驻留, 慢采样作对照可加 -m 1000)
//
// 需管理员 (打开 \\.\HptMap)。CSV 喂给 analyze.py。
// 编译: cl /O2 clkprobe.c
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>

#define ELENCHUS_DEV "\\\\.\\Elenchus"
#define ELEN_IOCTL_HPTMAP_MAP    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ELEN_IOCTL_HPTMAP_READ   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ELEN_IOCTL_HPTMAP_UNMAP  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

static HANDLE g_drv = INVALID_HANDLE_VALUE;

static BOOL DrvOpen(void)
{
    g_drv = CreateFileA(ELENCHUS_DEV, GENERIC_READ | GENERIC_WRITE, 0,
        NULL, OPEN_EXISTING, 0, NULL);
    return g_drv != INVALID_HANDLE_VALUE;
}

static BOOL DrvMap(ULONG64 phys, ULONG* slot)
{
    ULONG64 in = phys;
    ULONG out = 0;
    DWORD br = 0;
    if (!DeviceIoControl(g_drv, ELEN_IOCTL_HPTMAP_MAP, &in, sizeof(in), &out, sizeof(out), &br, NULL))
        return FALSE;
    *slot = out;
    return TRUE;
}

static BOOL DrvRead(ULONG slot, ULONG64* val)
{
    ULONG in = slot;
    ULONG64 out = 0;
    DWORD br = 0;
    if (!DeviceIoControl(g_drv, ELEN_IOCTL_HPTMAP_READ, &in, sizeof(in), &out, sizeof(out), &br, NULL))
        return FALSE;
    *val = out;
    return TRUE;
}

// 从 ACPI FADT 解析 PM_TMR 块(FADT 布局依据 struct acpi_table_fadt):
//   优先 GAS X_PM_TMR_BLK @ 0xD0 (SpaceId@0xD0, BitWidth@0xD1, Address@0xD4)
//   回退 legacy PM_TMR_BLK @ 0x4C + PM_TMR_LEN @ 0x5B(System I/O 语义)
// 出参: *space 0=SystemMemory 1=SystemIO; *bits 24/32; *space=-1 = FADT 不可读
// 返回地址; 0 = 本机未声明 PM_TMR(现代平台合法形态, 调用方软跳过)
static ULONG64 FadtPmTmrBlock(int* space, int* bits)
{
    *space = -1; *bits = 0;
    DWORD need = GetSystemFirmwareTable('ACPI', 'FACP', NULL, 0);
    if (need == 0 || need < 0x5C) return 0;
    BYTE* fadt = (BYTE*)malloc(need);
    if (fadt == NULL) return 0;
    ULONG64 addr = 0;
    DWORD got = GetSystemFirmwareTable('ACPI', 'FACP', fadt, need);
    if (got >= 0x5C && fadt[0]=='F' && fadt[1]=='A' && fadt[2]=='C' && fadt[3]=='P')
    {
        DWORD fLen = *(DWORD*)(fadt + 4);
        *space = 0;                     // FADT 可读; 默认"无 PM_TMR"
        if (fLen >= 0xDC && fadt[0xD0] <= 1)         // GAS X 字段合法
        {
            ULONG64 xaddr = *(ULONG64*)(fadt + 0xD4);
            if (xaddr != 0)
            {
                *space = fadt[0xD0];                // 0=mem 1=io
                *bits  = fadt[0xD1] ? fadt[0xD1] : 32;
                addr = xaddr;
            }
        }
        if (addr == 0)                               // legacy 回退
        {
            ULONG len = fadt[0x5B];
            ULONG32 blk = *(ULONG32*)(fadt + 0x4C);
            if ((len == 3 || len == 4) && blk != 0)
            {
                *space = 1;                         // legacy 固定块 = I/O 口
                *bits  = len * 8;
                addr = blk;
            }
        }
    }
    free(fadt);
    return addr;
}

// 串行化 + rdtsc(防止乱序把采样窗口污染)
static inline ULONG64 SerRdtsc(void)
{
    int regs[4];
    __cpuid(regs, 0);
    return __rdtsc();
}

int RunClk(int argc, char** argv)
{
    enum { AX_HPET, AX_PMTMR, AX_QPC } axis = AX_HPET;
    long long nSamples = 100000;
    int cpu = 0, usec = 0;
    const char* outFile = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-h")) axis = AX_HPET;
        else if (!strcmp(argv[i], "-p")) axis = AX_PMTMR;
        else if (!strcmp(argv[i], "-q")) axis = AX_QPC;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) nSamples = atoll(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) usec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outFile = argv[++i];
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }

    // 钉核: 跨核 TSC 同步误差会污染残差
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    ULONG slot = 0;
    ULONG64 pmMask = ~0ULL;    // PM_TMR 计数器宽度掩码(默认 64 位全通过)
    if (axis == AX_HPET || axis == AX_PMTMR)
    {
        ULONG64 phys;
        const char* axisName = "HPET";
        if (axis == AX_HPET)
        {
            phys = 0xFED00000ULL + 0xF0ULL;    // HPET main counter (MMIO, 64 位)
        }
        else
        {
            int space = -1, bits = 0;
            axisName = "PM_TMR";
            phys = FadtPmTmrBlock(&space, &bits);
            // 诊断行(auto 会捕获进报告): space -1=FADT不可读 0=MMIO/未声明 1=IO口
            fprintf(stderr, "[pmtmr] FADT: space=%d bits=%d addr=0x%llX\n",
                space, bits, (unsigned long long)phys);
            if (phys == 0)
            {
                fprintf(stderr, "PM_TMR: %s — 记 N/A, 跳过该轴(合法形态)\n",
                    space < 0 ? "FADT 不可读" : "本机未声明 PM_TMR 块");
                return 0;    // 软跳过, 不阻塞后续轴
            }
            pmMask = (bits == 24) ? 0xFFFFFFULL : 0xFFFFFFFFULL;
            if (space == 1)
            {
                phys |= 0x8000000000000000ULL;    // 驱动 bit63 = I/O 口模式
            }
        }
        if (!DrvOpen() || !DrvMap(phys, &slot))
        {
            if (axis == AX_PMTMR)
            {
                // PM_TMR 是辅助轴: 映射失败也不阻塞流程(HPET/QPC 才是主轴)
                fprintf(stderr, "PM_TMR 映射失败(%lu) — 记 N/A, 继续\n", GetLastError());
                if (g_drv != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(g_drv); g_drv = INVALID_HANDLE_VALUE;
                }
                return 0;
            }
            fprintf(stderr, "%s 驱动不可用(sc start elenchus?): %lu\n", axisName, GetLastError());
            return 1;
        }
        if (axis == AX_HPET)
        {
            fprintf(stderr, "轴=HPET phys=0x%llX 槽=%u\n",
                (unsigned long long)phys, slot);
        }
    }
    else
    {
        fprintf(stderr, "轴=QPC(纯R3)\n");
    }

    FILE* out = outFile ? fopen(outFile, "w") : stdout;
    if (out == NULL) { perror("fopen"); return 1; }

    LARGE_INTEGER qpc;
    for (long long i = 0; i < nSamples; i++)
    {
        ULONG64 other;
        ULONG64 t0 = SerRdtsc();
        if (axis == AX_QPC)
        {
            QueryPerformanceCounter(&qpc);
            other = (ULONG64)qpc.QuadPart;
        }
        else
        {
            if (!DrvRead(slot, &other)) { fprintf(stderr, "读失败 @%lld\n", i); break; }
            other &= pmMask;    // PM_TMR: 掩到计数器实际宽度(24/32bit)
        }
        ULONG64 t1 = SerRdtsc();
        fprintf(out, "%llu,%llu\n",
            (unsigned long long)((t0 + t1) / 2), (unsigned long long)other);
        if (usec > 0) Sleep(usec / 1000);    // 慢采样对照
    }

    if (out != stdout) fclose(out);
    if (axis != AX_QPC)
    {
        ULONG in = slot; DWORD br;
        DeviceIoControl(g_drv, ELEN_IOCTL_HPTMAP_UNMAP, &in, sizeof(in), NULL, 0, &br, NULL);
        if (g_drv != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_drv); g_drv = INVALID_HANDLE_VALUE;
        }
    }
    return 0;
}
