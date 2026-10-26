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

// 从 ACPI FADT 解析 PM_TMR 块物理地址(失败返回 0)
static ULONG64 FadtPmTmrBlock(void)
{
    // ACPI FADT 偏移(32位表): PM_TMR_BLK @ 0x4E (4B), PM_TMR_LEN @ 0x5D (1B)
    DWORD need = GetSystemFirmwareTable('ACPI', 0, NULL, 0);
    if (need == 0) return 0;
    BYTE* buf = (BYTE*)malloc(need);
    if (buf == NULL) return 0;
    // 枚举表签名找 'FACP'
    DWORD n = GetSystemFirmwareTable('ACPI', 0, buf, need);
    ULONG64 tmr = 0;
    if (n >= 4)
    {
        // buf = SSDT 头列表区(实际返回的是整个 ACPI 表数据, FACP 签名搜索)
        for (DWORD i = 0; i + 4 <= n; i++)
        {
            if (buf[i] == 'F' && buf[i+1] == 'A' && buf[i+2] == 'C' && buf[i+3] == 'P')
            {
                if (i + 0x60 <= n && buf[i + 0x5D] >= 3)   // PM_TMR_LEN 合法(3=24bit/4=32bit)
                {
                    ULONG32 blk = *(ULONG32*)(buf + i + 0x4E);
                    tmr = blk;
                }
                break;
            }
        }
    }
    free(buf);
    return tmr;
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
    if (axis == AX_HPET || axis == AX_PMTMR)
    {
        ULONG64 phys = (axis == AX_HPET) ? 0xFED00000ULL + 0xF0ULL   // HPET main counter
                                         : FadtPmTmrBlock();
        if (axis == AX_PMTMR && phys == 0)
        {
            fprintf(stderr, "FADT 中未找到 PM_TMR 块(或长度异常)\n");
            return 1;
        }
        if (!DrvOpen() || !DrvMap(phys, &slot))
        {
            fprintf(stderr, "hptmap 驱动不可用(sc start elenchus?): %lu\n", GetLastError());
            return 1;
        }
        fprintf(stderr, "轴=%s phys=%llX 槽=%u\n",
            axis == AX_HPET ? "HPET" : "PM_TMR", (unsigned long long)phys, slot);
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
    }
    return 0;
}
