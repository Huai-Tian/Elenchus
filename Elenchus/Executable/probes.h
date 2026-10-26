// =====================================================================
// probes.h — 各探测器入口声明(统一分发器 elenchus.c 调用)
//
// 每个 Run_XXX 的参数视角: argv[0]=子命令名, 参数从 argv[1] 起
// —— 与原独立 main 的解析逻辑完全一致, 零改动迁移。
// =====================================================================
#pragma once

int RunCpuidDump(int argc, char** argv);   // elenchus cpuid  : B1 CPUID 全叶 dump
int RunDwell(int argc, char** argv);       // elenchus dwell  : A1 夹逼/rdtscaux/窗口
int RunClk(int argc, char** argv);         // elenchus clk    : A2 三轴时钟采样
int RunTlb(int argc, char** argv);         // elenchus tlb    : A4 TLB 扰动
int RunRx(int argc, char** argv);          // elenchus rx     : C2 读-执行差分
int RunId(int argc, char** argv);          // elenchus id     : B1/B2 CPUID+MSR+指令面
int RunDbg(int argc, char** argv);         // elenchus dbg    : B3 调试保真
int RunBench(int argc, char** argv);       // elenchus bench  : PERF hits/batch/flood/ctxsw/k
