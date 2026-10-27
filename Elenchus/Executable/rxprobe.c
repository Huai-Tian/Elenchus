// =====================================================================
// rxprobe.c — C2 读-执行差分探测器 R3 前端
//
// 用法(管理员): Executable.exe rx [选项]
//   -t VA      目标内核地址(默认 0 = 驱动自解析 NtClose)
//   -l LEN     比对长度(默认 32, <=64)
//   -e N       执行次数(默认 1)
//   -c CPU     钉核(默认 0; 换核重跑 = 观察每核驻留形态)
//
// 输出: 读前/读后/磁盘三组 hexdump + 判定行:
//   BEFORE==DISK / AFTER==DISK / BEFORE==AFTER + 总判定(PASS/FAIL/描述)
// 编译: cl /O2 rxprobe.c
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>

#define ELENCHUS_DEV  "\\\\.\\Elenchus"
#define ELEN_IOCTL_RXDIFF_PROBE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RXDIFF_MAX  64

typedef struct _RXDIFF_IN
{
    unsigned long long TargetVa;
    unsigned long Len;
    unsigned long ExecCount;
} RXDIFF_IN;

typedef struct _RXDIFF_OUT
{
    unsigned long long TargetVa;
    unsigned long long NtBase;
    unsigned long Len;
    unsigned char Before[RXDIFF_MAX];
    unsigned char After[RXDIFF_MAX];
    unsigned char Disk[RXDIFF_MAX];
    long ExecStatus;
    long DiskStatus;
} RXDIFF_OUT;

static void HexDump(const char* tag, const unsigned char* p, ULONG len, ULONG rva)
{
    printf("%-8s", tag);
    for (ULONG i = 0; i < len; i++)
    {
        printf("%02X ", p[i]);
    }
    printf(" (nt+0x%llX)\n", (unsigned long long)rva);
}

static int MemEq(const unsigned char* a, const unsigned char* b, ULONG len)
{
    for (ULONG i = 0; i < len; i++)
    {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

int RunRx(int argc, char** argv)
{
    unsigned long long target = 0;
    unsigned long len = 32;
    unsigned long execN = 1;
    int cpu = 0;

    for (int i = 1; i < argc; i++)
    {
        //base16: 兼容裸十六进制("FFFF80...")与0x前缀; base0会把无前缀
        //十六进制判为十进制→返回0→静默回退自动解析NtClose(判例2026-10-28:
        //整轮矩阵探测了未挂钩的NtClose=平凡PASS, 数据作废)
        if (!strcmp(argv[i], "-t") && i + 1 < argc) target = strtoull(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) len = (unsigned long)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) execN = (unsigned long)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cpu = atoi(argv[++i]);
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }
    if (len > RXDIFF_MAX) len = RXDIFF_MAX;

    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);

    HANDLE h = CreateFileA(ELENCHUS_DEV, GENERIC_READ | GENERIC_WRITE, 0,
        NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "打开 rxdiff 失败(sc start elenchus?): %lu\n", GetLastError());
        return 1;
    }

    RXDIFF_IN in = { 0 };
    in.TargetVa = target;
    in.Len = len;
    in.ExecCount = execN;
    RXDIFF_OUT out;
    ZeroMemory(&out, sizeof(out));

    DWORD br = 0;
    if (!DeviceIoControl(h, ELEN_IOCTL_RXDIFF_PROBE, &in, sizeof(in), &out, sizeof(out), &br, NULL))
    {
        fprintf(stderr, "IOCTL 失败: %lu\n", GetLastError());
        CloseHandle(h);
        return 1;
    }
    CloseHandle(h);

    printf("目标=%llX ntoskrnl=%llX len=%lu exec=%lu execStatus=0x%lX diskStatus=0x%lX\n",
        out.TargetVa, out.NtBase, out.Len, execN, out.ExecStatus, out.DiskStatus);

    ULONG rva = (ULONG)(out.TargetVa - out.NtBase);
    HexDump("BEFORE:", out.Before, out.Len, rva);
    HexDump("AFTER: ", out.After, out.Len, rva);
    HexDump("DISK:  ", out.Disk, out.Len, rva);

    int bD = (out.DiskStatus == 0) && MemEq(out.Before, out.Disk, out.Len);
    int aD = (out.DiskStatus == 0) && MemEq(out.After, out.Disk, out.Len);
    int bA = MemEq(out.Before, out.After, out.Len);

    printf("\n判定: BEFORE==DISK:%s AFTER==DISK:%s BEFORE==AFTER:%s\n",
        bD ? "Y" : "N", aD ? "Y" : "N", bA ? "Y" : "N");
    if (bD && aD)
    {
        printf("结论: PASS — 读-执行-读全程原始字节(结构性读透明或无hook)\n");
    }
    else if (!bD && bA)
    {
        printf("结论: 可疑 — 执行前已可见非磁盘字节(常驻工件或别名视图)\n");
    }
    else if (bD && !aD)
    {
        printf("结论: FAIL — 执行后字节改变(驻留影子视图无读透明: CodePage型R=1暴露)\n");
    }
    else
    {
        printf("结论: 复杂形态 — 需人工判读(对照三dump)\n");
    }
    return 0;
}
