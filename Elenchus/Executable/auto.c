// =====================================================================
// auto.c — 全自动采集编排器: 无参数启动即跑
//
// 行为: 检测环境 → 判定形态(bare/vm/fw) → 按形态执行完整采集清单 →
//       产物自动命名 <向量>_<形态>_<轮次>.csv → 控制台逐步报告 →
//       汇总 auto_report_rN.txt。rc=1(环境缺失)即停防污染。
//
// 形态判定:
//   fw  : Elenchus 驱动可开 AND CPUID.1.ECX[31]=1
//   vm  : 驱动不可开(guest 无驱动) AND hypervisor 位=1(VMware 形态)
//   bare: 其余(裸机; 或驱动在位但框架未装载——见下方"dwell 分段"说明)
//
// 阶段推进: 人工装/卸框架与驱动之间各跑一次本程序, 即得三形态全套;
//           框架装载期间 drv+hv 双真 → fw; 卸载后 → bare(补充轮)。
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include "probes.h"
#include "auto.h"

#define ELENCHUS_DEV "\\\\.\\Elenchus"
#define NSAMP "1000000"

// ---------------- 环境探测 ----------------
static int HypervisorBit(void)
{
    int regs[4];
    __cpuid(regs, 1);
    return (regs[2] >> 31) & 1;
}

static int DriverAvailable(void)
{
    HANDLE h = CreateFileA(ELENCHUS_DEV, GENERIC_READ | GENERIC_WRITE, 0,
        NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    CloseHandle(h);
    return 1;
}

static void DetectForm(const char** form, const char** desc)
{
    int drv = DriverAvailable();
    int hv = HypervisorBit();
    if (drv && hv)       { *form = "fw";   *desc = "框架驻留(驱动+hypervisor位)"; }
    else if (!drv && hv) { *form = "vm";   *desc = "虚拟机正对照(无驱动+hypervisor位)"; }
    else if (drv)        { *form = "bare"; *desc = "裸机/仅探测器驱动(框架未装载)"; }
    else                 { *form = "bare"; *desc = "裸机"; }
}

// ---------------- 轮次号 ----------------
static int NextRound(void)
{
    WIN32_FIND_DATAA fd;
    int maxr = 0;
    HANDLE h = FindFirstFileA("auto_report_r*.txt", &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            int r = atoi(fd.cFileName + strlen("auto_report_r"));
            if (r > maxr) maxr = r;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return maxr + 1;
}

// ---------------- 子探测器调度 ----------------
// 可变参数直接内联进 argv 数组(最多 12 槽), 返回 Run_XXX 的 rc
static FILE* g_rep;

static int Dispatch(const char* sub, const char* const args[], int nargs)
{
    const char* argv[12] = { sub };
    int argc = 1;
    for (int i = 0; i < nargs && argc < 12; i++)
    {
        argv[argc++] = args[i];
    }
    printf("\n>>> elenchus %s", sub);
    for (int i = 0; i < nargs; i++)
    {
        printf(" %s", args[i]);
    }
    printf("\n");
    if (g_rep)
    {
        fprintf(g_rep, ">>> %s", sub);
        for (int i = 0; i < nargs; i++)
        {
            fprintf(g_rep, " %s", args[i]);
        }
        fprintf(g_rep, "\n");
    }

    int rc;
    if      (!strcmp(sub, "cpuid")) rc = RunCpuidDump(argc, (char**)argv);
    else if (!strcmp(sub, "dwell")) rc = RunDwell(argc, (char**)argv);
    else if (!strcmp(sub, "clk"))   rc = RunClk(argc, (char**)argv);
    else if (!strcmp(sub, "tlb"))   rc = RunTlb(argc, (char**)argv);
    else if (!strcmp(sub, "rx"))    rc = RunRx(argc, (char**)argv);
    else if (!strcmp(sub, "id"))    rc = RunId(argc, (char**)argv);
    else if (!strcmp(sub, "dbg"))   rc = RunDbg(argc, (char**)argv);
    else if (!strcmp(sub, "bench")) rc = RunBench(argc, (char**)argv);
    else rc = 2;

    printf("    [%s] rc=%d %s\n", sub, rc,
        rc == 0 ? "OK" : (rc == 1 ? "环境缺失→停止" : "参数/错误"));
    if (g_rep)
    {
        fprintf(g_rep, "    rc=%d\n", rc);
        fflush(g_rep);
    }
    return rc;
}

// 便捷宏: 传静态参数表
#define RUN(sub, ...) do { \
    const char* _a[] = { __VA_ARGS__ }; \
    int _n = (int)(sizeof(_a) / sizeof(_a[0])); \
    int _rc = Dispatch(sub, _a, _n); \
    if (_rc != 0 && _rc != 2) { \
        printf("!!! 停止(auto 阶段), 修复后重新运行本轮\n"); \
        if (g_rep) fprintf(g_rep, "!!! STOP at %s rc=%d\n", sub, _rc); \
        return 1; \
    } } while (0)

int RunAuto(void)
{
    char host[64] = { 0 };
    DWORD sz = sizeof(host) - 1;
    GetComputerNameA(host, &sz);
    for (char* p = host; *p; p++)
    {
        if (*p == '-' || *p == ' ') *p = '_';
    }

    const char* form = NULL, * desc = NULL;
    DetectForm(&form, &desc);
    int rnd = NextRound();

    char rep[128], tag[64];
    snprintf(rep, sizeof(rep), "auto_report_r%d.txt", rnd);
    g_rep = fopen(rep, "w");

    printf("=====================================================================\n");
    printf(" Elenchus 全自动采集   机器=%s  轮次=r%d\n", host, rnd);
    printf(" 形态判定: %s → %s\n", form, desc);
    printf(" 产物: <向量>_%s_%s_r%d.csv(当前目录) + %s\n", form, host, rnd, rep);
    printf("=====================================================================\n");
    if (g_rep) fprintf(g_rep, "host=%s form=%s round=r%d desc=%s\n", host, form, rnd, desc);

    #define TAG(p) (snprintf(tag, sizeof(tag), "%s_%s_%s_r%d", p, form, host, rnd), tag)

    // ---------- 步骤 1: B1 CPUID 全叶(三形态通用) ----------
    RUN("cpuid", "-o", TAG("b1_cpuid"));

    // ---------- 步骤 2: B3 调试保真(纯 R3, 三形态通用) ----------
    RUN("dbg", "-n", "100");

    // ---------- 步骤 3: B2 CPUID+MSR+指令面 ----------
    // bare(无驱动形态): id 的驱动部分自动降级(其内部已处理, 仅打印不可用)
    // vm: guest 无驱动, 同上; fw: 全量
    RUN("id", "-o", TAG("b2_id"));

    // ---------- 步骤 4: A1 dwell 夹逼三形态 ----------
    RUN("dwell", "-n", NSAMP, "-c", "0", "-i", "cpuid", "-o", TAG("a1_cpuid"));
    RUN("dwell", "-n", NSAMP, "-c", "0", "-i", "sgdt",  "-o", TAG("a1_sgdt"));
    RUN("dwell", "-n", NSAMP, "-c", "0", "-i", "rdtscaux");          // PASS/FAIL 直接入报告

    // ---------- 步骤 5: A2 三轴时钟(需驱动; vm 形态无驱动自动降级跳过) ----------
    if (strcmp(form, "vm") != 0)
    {
        RUN("clk", "-h", "-n", NSAMP, "-c", "0", "-o", TAG("a2_hpet"));
        RUN("clk", "-p", "-n", NSAMP, "-c", "0", "-o", TAG("a2_pmtmr"));
        RUN("clk", "-q", "-n", NSAMP, "-c", "0", "-o", TAG("a2_qpc"));
    }

    // ---------- 步骤 6: A4 TLB 扰动 ----------
    if (strcmp(form, "vm") != 0)
    {
        RUN("tlb", "-t", "none",  "-c", "0");
        RUN("tlb", "-t", "cpuid", "-c", "0");
        RUN("tlb", "-t", "clock", "-c", "0");    // 时钟封堵路径 exit(重点)
    }

    // ---------- 步骤 7: C2 读-执行差分(需驱动; 非框架形态跳过——无 hook 可差分) ----------
    if (!strcmp(form, "fw"))
    {
        RUN("rx", "-c", "0", "-e", "10");
    }

    // ---------- 步骤 8: PERF 基准(需驱动; 三形态都测裸机/inline 项) ----------
    if (strcmp(form, "vm") != 0)
    {
        RUN("bench", "k", "-n", "1000000");             // 裸机/当前形态
        RUN("bench", "kaddr");                          // KbTarget VA(报告留档)
    }

    printf("\n=====================================================================\n");
    printf(" [auto 完成] 形态=%s 轮次=r%d\n", form, rnd);
    printf(" 提醒:\n");
    if (!strcmp(form, "fw"))
    {
        printf("  ★ A2 真值交叉验证: 打开框架日志, 统计 'y' 事件数, 应>0\n");
        printf("    (Gept: gept_log.txt | Gnpt: gnpt_log.txt) 附在回传数据里\n");
        printf("  ★ 完整三形态: 卸载框架后再跑一次本程序(自动记为下一轮 bare)\n");
    }
    else if (!strcmp(form, "vm"))
    {
        printf("  ★ vm 形态: 驱动相关项已自动跳过(clk/tlb/rx/bench)\n");
    }
    else
    {
        printf("  ★ bare 形态完成后: 装 VMware 跑 guest 内同程序(vm 轮),\n");
        printf("    再装框架跑 fw 轮 —— 三形态即齐\n");
    }
    printf("=====================================================================\n");

    if (g_rep)
    {
        fprintf(g_rep, "\n[DONE] form=%s round=r%d host=%s\n", form, rnd, host);
        fclose(g_rep);
        g_rep = NULL;
    }
    printf("汇总: %s\n", rep);
    return 0;
}
