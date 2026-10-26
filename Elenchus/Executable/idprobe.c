// =====================================================================
// idprobe.c — B 族 R3 前端: CPUID dump + MSR 面 + 指令面 一键采集
//
// 用法(管理员): Executable.exe id [-o 前缀] [-m msr,msr,...] [-c CPU]
//   产物: <前缀>_cpuid.csv + 控制台 MSR/指令面表
//   -m 自定义 MSR 列表(hex, 逗号分隔)
//   默认 MSR 列表: 0x3A(特征控制,框架伪造点) 0xFE(MTRRcap)
//                 0x10(TSC) 0x1A0(MISC)
//                 0x1234/0x1235(保留→应#GP) 0xC0000082(LSTAR)
//
// 三形态各跑一次, 对照表 = 论文 B1/B2 行
// 编译: cl /O2 idprobe.c
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>

#define ELENCHUS_DEV "\\\\.\\Elenchus"
#define ELEN_IOCTL_IDFACE_MSR   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x830, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ELEN_IOCTL_IDFACE_INSTR CTL_CODE(FILE_DEVICE_UNKNOWN, 0x831, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _MSR_OUT { unsigned long long Value; unsigned long Code; unsigned long Pad; } MSR_OUT;
typedef struct _INSTR_OUT { unsigned long Code; unsigned long Pad; } INSTR_OUT;

static const char* ExcName(ULONG c)
{
    switch (c)
    {
    case 0: return "OK";
    case 0xC000001D: return "#UD(illegal)";
    case 0xC0000096: return "#GP(privileged)";
    default: return "OTHER";
    }
}

int RunId(int argc, char** argv)
{
    const char* prefix = "idprobe";
    const char* extraMsrs = NULL;
    int cpu = 0;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) extraMsrs = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cpu = atoi(argv[++i]);
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);

    // ---- CPUID dump ----
    char path[512];
    snprintf(path, sizeof(path), "%s_cpuid.csv", prefix);
    int regs[4];
    __cpuid(regs, 0);
    unsigned maxBasic = (unsigned)regs[0];
    __cpuid(regs, 0x80000000);
    unsigned maxExt = (unsigned)regs[0];
    FILE* f = fopen(path, "w");
    if (!f) { perror("fopen"); return 1; }
    fprintf(f, "leaf,subleaf,eax,ebx,ecx,edx\n");
    for (unsigned leaf = 0; leaf <= maxBasic; leaf++)
    {
        BOOL hasSub = (leaf == 4 || leaf == 7 || leaf == 0xB || leaf == 0xF ||
                       leaf == 0x10 || leaf == 0x14 || leaf == 0x17 || leaf == 0x18 ||
                       leaf == 0x1D || leaf == 0x1F);
        for (unsigned sub = 0; sub < (hasSub ? 8 : 1); sub++)
        {
            __cpuidex(regs, (int)leaf, (int)sub);
            fprintf(f, "0x%X,%u,0x%X,0x%X,0x%X,0x%X\n", leaf, sub,
                (unsigned)regs[0], (unsigned)regs[1], (unsigned)regs[2], (unsigned)regs[3]);
        }
    }
    for (unsigned leaf = 0x80000000; leaf <= maxExt && leaf >= 0x80000000; leaf++)
    {
        for (unsigned sub = 0; sub < (leaf == 0x8000001D ? 8 : 1); sub++)
        {
            __cpuidex(regs, (int)leaf, (int)sub);
            fprintf(f, "0x%X,%u,0x%X,0x%X,0x%X,0x%X\n", leaf, sub,
                (unsigned)regs[0], (unsigned)regs[1], (unsigned)regs[2], (unsigned)regs[3]);
        }
    }
    // 越界/管理程序 leaf 一致性（同 cpuid_dump.c 的 Ophion 2026 探针集）
    {
        const unsigned extra[] = { maxBasic + 1, 0x13371337,
                                   0x40000000, 0x40000001, 0x40000002,
                                   0x40000003, 0x40000004, 0x40000005,
                                   0x40000006, 0x40000007, 0x40000008,
                                   0x40000009, 0x4000000A, 0x4000000B,
                                   0x4000000C, 0x4000000D, 0x4000000E,
                                   0x4000000F, maxExt + 1 };
        for (unsigned k = 0; k < sizeof(extra) / sizeof(extra[0]); k++)
        {
            __cpuid(regs, (int)extra[k]);
            fprintf(f, "0x%X,0,0x%X,0x%X,0x%X,0x%X\n", extra[k],
                (unsigned)regs[0], (unsigned)regs[1],
                (unsigned)regs[2], (unsigned)regs[3]);
        }
    }
    fclose(f);
    printf("[CPUID] %s 完成 (basic<=0x%X ext<=0x%X)\n", path, maxBasic, maxExt);

    // ---- MSR 面 + 指令面（经 idface 驱动）----
    HANDLE h = CreateFileA(ELENCHUS_DEV, GENERIC_READ | GENERIC_WRITE, 0,
        NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "[idface] 驱动不可用(sc start elenchus?): %lu — 仅CPUID完成\n",
            GetLastError());
        return 0;
    }

    // 默认 MSR 列表
    // VMX 能力区(0x480+): Intel 裸机=可读真值; AMD 裸机=保留→#GP
    // —— 各平台对照自身裸机形态即可(框架应与所在平台裸机一致)
    // VM_CR(0xC0010114, APM §15.30.1): AMD 裸机=可读(SVME位真实状态);
    //   GnptHooks 有 VM_CR 读伪造(SVME=0 故事) → AMD 侧关键对照格。
    //   Intel 上读它=#GP(保留区), 同样是数据。
    unsigned msrs[16];
    int nMsrs = 0;
    int def[] = { 0x3A, 0xFE, 0x10, 0x1A0, 0x1234, 0x1235, 0xC0000082,
                  0x480, 0x481, 0x482, 0x486, 0x489, 0xC0010114 };
    for (unsigned i = 0; i < sizeof(def) / sizeof(def[0]); i++)
    {
        if (nMsrs < 16) msrs[nMsrs++] = (unsigned)def[i];
    }
    if (extraMsrs)
    {
        char* buf = _strdup(extraMsrs);
        for (char* tok = strtok(buf, ","); tok && nMsrs < 16; tok = strtok(NULL, ","))
        {
            msrs[nMsrs++] = (unsigned)strtoul(tok, NULL, 0);
        }
        free(buf);
    }

    printf("\n[MSR 面] (三形态对照: 同表裸机/VMware/框架各跑一次)\n");
    printf("%-12s %-14s %-18s %s\n", "MSR", "结果", "值", "异常");
    DWORD br;
    for (int i = 0; i < (int)nMsrs; i++)
    {
        ULONG32 in = msrs[i];
        MSR_OUT out = { 0 };
        if (DeviceIoControl(h, ELEN_IOCTL_IDFACE_MSR, &in, sizeof(in), &out, sizeof(out), &br, NULL))
        {
            printf("0x%-10X %-14s 0x%016llX %s(0x%lX)\n",
                msrs[i], out.Code == 0 ? "READ" : "EXC", out.Value,
                ExcName(out.Code), out.Code);
        }
        else
        {
            printf("0x%-10X IOCTL失败 %lu\n", msrs[i], GetLastError());
        }
    }

    printf("\n[指令面] (判定基准: APM/SDM 原文语义 + 本机裸机实测)\n");
    const char* names[] = { "VMXOFF", "VMREAD", "STGI", "CLGI" };
    printf("%-8s %-18s\n", "指令", "结果");
    for (ULONG w = 0; w < 4; w++)
    {
        ULONG in = w;
        INSTR_OUT out = { 0 };
        if (DeviceIoControl(h, ELEN_IOCTL_IDFACE_INSTR, &in, sizeof(in), &out, sizeof(out), &br, NULL))
        {
            printf("%-8s %s(0x%lX)\n", names[w], ExcName(out.Code), out.Code);
        }
        else
        {
            printf("%-8s IOCTL失败 %lu\n", names[w], GetLastError());
        }
    }
    CloseHandle(h);
    printf("\n判读(对照本机裸机同名输出, 架构语义基准):\n");
    printf("  VMXOFF/VMREAD: SDM Vol.3C—VMX operation 之外执行恒 #UD;\n");
    printf("    框架下 #GP 或吞 = FAIL(Intel)\n");
    printf("  CLGI: APM—EFER.SVME=0 时 #UD(裸机即此形态); 框架下异常类改变 = FAIL\n");
    printf("  STGI: APM §15.4—SVME=1 或 CPUID Fn8000_0001_ECX[SKINIT]=1 时合法!\n");
    printf("    多数现代 AMD 裸机 SKINIT=1 → STGI 合法执行(OK, 非 #UD);\n");
    printf("    框架若笼统封成 #UD = 与裸机不一致(FAIL)——GnptHooks 的\n");
    printf("    'STGI passthrough 按 SKINIT 位门控'(M15.4)正在此处受检验\n");
    printf("  保留MSR(0x1234/35)应#GP; 0x3A框架侧=伪造值(对照裸机真值);\n");
    printf("  VM_CR(0xC0010114): AMD 侧对照裸机 SVME 位读数(Intel 侧=保留#GP)\n");
    return 0;
}
