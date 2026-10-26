// =====================================================================
// elenchus.c — 统一探测器入口: 一个 exe, 八个子命令
//
// 用法: Executable.exe <子命令> [参数...]
//
//   cpuid  [-o out.csv]                    CPUID 全叶 dump(含越界/管理程序 leaf)
//   dwell  -i cpuid|sgdt|rdtscaux [-n N] [-c CPU] [-o FILE] [-w]
//   clk    -h|-p|-q [-n N] [-c CPU] [-o FILE]   (HPET/PM_TMR/QPC 三轴)
//   tlb    -t none|cpuid|clock [-p 页数] [-r 轮数] [-c CPU]
//   rx     [-c CPU] [-e 次数]              读-执行差分(经 elenchus 驱动)
//   id     [-o 前缀] [-m msr列表] [-c CPU] CPUID+MSR+指令面(经 elenchus 驱动)
//   dbg    [-n 次数]                       调试子系统保真(int1/ICEBP/TF/DR)
//   bench  hits|batch|flood|ctxsw|k|kaddr [参数...]  性能基准(经 elenchus 驱动)
//
// 各子命令参数与原独立程序完全一致(参数平移, 内部解析零改动)。
// =====================================================================
#include <stdio.h>
#include <string.h>
#include "probes.h"
#include "auto.h"

static void Help(void)
{
    printf(
        "Elenchus — 统一探测器套件 (一个 exe, 八个子命令)\n"
        "\n"
        "用法: Executable.exe <子命令> [参数...]\n"
        "\n"
        "  cpuid  [-o out.csv]                 CPUID 全叶 dump(含越界/管理程序 leaf) [B1]\n"
        "  dwell  -i cpuid|sgdt|rdtscaux [...] RDTSC 夹逼驻留 / TSC_AUX / 安装窗口 [A1/C3]\n"
        "        -n N -c CPU -o FILE -w(窗口模式)\n"
        "  clk    -h|-p|-q [-n N -c CPU -o F]  三轴时钟采样 HPET/PM_TMR/QPC [A2 签名实验]\n"
        "  tlb    -t none|cpuid|clock [...]    exit 诱发 TLB 扰动 [A4]\n"
        "        -p 页数 -r 轮数 -c CPU\n"
        "  rx     [-c CPU] [-e 执行次数]       读-执行差分 [C2 三模式实证]\n"
        "  id     [-o 前缀] [-m MSR列表]       CPUID+MSR+指令面 [B1/B2]\n"
        "  dbg    [-n 次数]                    调试子系统保真 int1/ICEBP/TF/DR [B3]\n"
        "  bench  hits|batch|flood|ctxsw|k|kaddr  性能基准 [PERF]\n"
        "\n"
        "驱动依赖(rx/id/bench/clk -h/tlb -t clock 需先: sc start elenchus)\n"
        "完整协议: Script/PROTOCOL.md\n");
}

int main(int argc, char** argv)
{
    // ── 全自动模式: 无参数直接回车 ──
    // 检测环境(bare/vm/fw) → 按形态跑完整清单 → 自动命名/轮次/汇总
    if (argc < 2)
    {
        return RunAuto();
    }
    const char* sub = argv[1];
    if (!strcmp(sub, "cpuid"))       return RunCpuidDump(argc - 1, argv + 1);
    if (!strcmp(sub, "dwell"))       return RunDwell(argc - 1, argv + 1);
    if (!strcmp(sub, "clk"))         return RunClk(argc - 1, argv + 1);
    if (!strcmp(sub, "tlb"))         return RunTlb(argc - 1, argv + 1);
    if (!strcmp(sub, "rx"))          return RunRx(argc - 1, argv + 1);
    if (!strcmp(sub, "id"))          return RunId(argc - 1, argv + 1);
    if (!strcmp(sub, "dbg"))         return RunDbg(argc - 1, argv + 1);
    if (!strcmp(sub, "bench"))       return RunBench(argc - 1, argv + 1);
    if (!strcmp(sub, "help") || !strcmp(sub, "-h") || !strcmp(sub, "/?"))
    {
        Help();
        return 0;
    }
    fprintf(stderr, "未知子命令: %s\n\n", sub);
    Help();
    return 2;
}
