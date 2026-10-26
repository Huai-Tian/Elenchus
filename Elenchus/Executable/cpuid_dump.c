// =====================================================================
// cpuid_dump.c — CPUID 全叶 dump（S0 基线 / B1 向量）
//
// 输出 CSV: leaf,subleaf,eax,ebx,ecx,edx (hex)
// 
// 编译: cl /O2 cpuid_dump.c
// 对比: python3 diff_cpuid.py base.csv target.csv
// =====================================================================
#include <windows.h>
#include <stdio.h>
#include <intrin.h>

static void DumpRange(unsigned beg, unsigned end, FILE* out)
{
    int regs[4];
    char line[256];
    for (unsigned leaf = beg; leaf <= end; leaf++)
    {
        // 有子叶语义的 leaf 才枚举 subleaf，其余固定 0（多 dump 无害）
        BOOL hasSub = (leaf == 4 || leaf == 7 || leaf == 0xB ||
                       leaf == 0xF || leaf == 0x10 || leaf == 0x14 ||
                       leaf == 0x17 || leaf == 0x18 || leaf == 0x1D ||
                       leaf == 0x1F || leaf == 0x8000001D ||
                       leaf == 0x80000020 || leaf == 0x80000026);
        unsigned subMax = hasSub ? 8 : 1;
        for (unsigned sub = 0; sub < subMax; sub++)
        {
            __cpuidex(regs, (int)leaf, (int)sub);
            // 无子叶语义的 leaf 读 sub>0 常返回与 sub=0 相同——原样记录
            snprintf(line, sizeof(line), "0x%X,%u,0x%X,0x%X,0x%X,0x%X",
                leaf, sub,
                (unsigned)regs[0], (unsigned)regs[1],
                (unsigned)regs[2], (unsigned)regs[3]);
            fprintf(out, "%s\n", line);
            if (!hasSub)
            {
                break;
            }
        }
    }
}

int RunCpuidDump(int argc, char** argv)
{
    FILE* out = stdout;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
        {
            out = fopen(argv[++i], "w");
            if (!out) { perror("fopen"); return 1; }
        }
    }
    int regs[4];

    __cpuid(regs, 0);
    unsigned maxBasic = (unsigned)regs[0];
    __cpuid(regs, 0x80000000);
    unsigned maxExt = (unsigned)regs[0];

    fprintf(out, "leaf,subleaf,eax,ebx,ecx,edx\n");
    DumpRange(0, maxBasic, out);
    if (maxExt >= 0x80000000)
    {
        DumpRange(0x80000000, maxExt, out);
    }
    // ── 越界/管理程序 leaf 一致性（Ophion 2026 探针）──
    // 裸机语义: Intel 对 >maxBasic 的 basic 区 leaf 回声"最后一个有效
    // basic leaf"的数据; hypervisor leaf 区(0x40000000..0x4000000F)在
    // 裸机上同样受此回声规则支配(AMD 通常回零)。
    // 伪装框架若"签名 leaf 归零"而回声非零 = 可检测差异。
    // 必采: maxBasic+1 / 无效 leaf / 完整管理程序区 / maxExt+1
    {
        const unsigned extra[] = { maxBasic + 1, 0x13371337,
                                   0x40000000, 0x40000001, 0x40000002,
                                   0x40000003, 0x40000004, 0x40000005,
                                   0x40000006, 0x40000007, 0x40000008,
                                   0x40000009, 0x4000000A, 0x4000000B,
                                   0x4000000C, 0x4000000D, 0x4000000E,
                                   0x4000000F };
        for (unsigned k = 0; k < sizeof(extra) / sizeof(extra[0]); k++)
        {
            __cpuid(regs, (int)extra[k]);
            fprintf(out, "0x%X,0,0x%X,0x%X,0x%X,0x%X\n", extra[k],
                (unsigned)regs[0], (unsigned)regs[1],
                (unsigned)regs[2], (unsigned)regs[3]);
        }
        if (maxExt >= 0x80000000)
        {
            __cpuid(regs, (int)(maxExt + 1));
            fprintf(out, "0x%X,0,0x%X,0x%X,0x%X,0x%X\n", maxExt + 1,
                (unsigned)regs[0], (unsigned)regs[1],
                (unsigned)regs[2], (unsigned)regs[3]);
        }
    }
    if (out != stdout)
    {
        fclose(out);
    }
    fprintf(stderr, "dump 完成: basic<=0x%X ext<=0x%X\n", maxBasic, maxExt);
    return 0;
}
