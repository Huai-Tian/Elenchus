// =====================================================================
// dbgface.c — B3 探测器: 调试子系统保真（纯 R3）
//
// 四项测试（每项 100 次重复）:
//   1. int1 软件陷阱投递     — 经 VEH 捕获 STATUS_SINGLE_STEP 计数
//   2. ICEBP(0xF1) 投递      — 经典 hypervisor 指纹（被吞=int1 永不到达）
//   3. TF 单步陷阱           — pushfq|TF|popfq 后下一条指令应触发陷阱
//   4. DR0/DR7 写读一致性    — 挂起工作线程写 DR, 读回应一致
//                              (DR7 回读恒含 bit10 RA1=裸机形态)
//
// 判读: 裸机 = 四项全 PASS。框架下被吞/丢失 = 调试面指纹。
//       GnptHooks DR租用制下 guest 的写入应忠实生效（影子一致）。
//
// 用法: dbgface.exe [-n 100]
// 编译: cl /O2 dbgface.c
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static int g_delivered = 0;
static void* g_stubBase = NULL;

// RWX 桩区（四种微码序列, 64B 对齐分片）
static void* MakeStubs(void)
{
    UCHAR* p = (UCHAR*)VirtualAlloc(NULL, 256, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!p) return NULL;
    // int1:   CD 01        (int 1)
    p[0] = 0xCD; p[1] = 0x01; p[2] = 0xC3;
    // ICEBP:  F1           (int 1 陷阱, 不置 DR6)
    p[64] = 0xF1; p[65] = 0xC3;
    // TF:     9C                       pushfq
    //         81 4C 24 00 00 01 00 00  or dword ptr [rsp], 0x100
    //         9D                       popfq
    //         C3                       ret  (popfq 后本条即触发 TF 陷阱)
    p[128] = 0x9C;
    p[129] = 0x81; p[130] = 0x4C; p[131] = 0x24; p[132] = 0x00;
    p[133] = 0x00; p[134] = 0x01; p[135] = 0x00; p[136] = 0x00;
    p[137] = 0x9D; p[138] = 0xC3;
    g_stubBase = p;
    return p;
}

// VEH: 单步类异常认领（地址落在桩区 = 我们的探针）
static LONG WINAPI VehHandler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == (DWORD)0x80000004 /*STATUS_SINGLE_STEP*/)
    {
        ULONG64 addr = (ULONG64)ep->ExceptionRecord->ExceptionAddress;
        ULONG64 base = (ULONG64)g_stubBase;
        if (addr >= base && addr < base + 256)
        {
            // TF 陷阱: 清 TF 后继续（陷阱已计为投递）
            ep->ContextRecord->EFlags &= ~0x100;
            InterlockedIncrement(&g_delivered);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

typedef void (*STUB_FN)(void);

int main(int argc, char** argv)
{
    int n = 100;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }

    UCHAR* stubs = (UCHAR*)MakeStubs();
    if (!stubs) { fprintf(stderr, "VirtualAlloc 失败\n"); return 1; }
    PVOID veh = AddVectoredExceptionHandler(1, VehHandler);
    if (!veh) { fprintf(stderr, "VEH 注册失败\n"); return 1; }

    printf("== 调试子系统保真探测 (每项 %d 次) ==\n\n", n);

    // 1. int1
    g_delivered = 0;
    STUB_FN fnInt1 = (STUB_FN)stubs;
    for (int i = 0; i < n; i++) fnInt1();
    printf("int1 软件陷阱:   %3d/%d  %s\n", g_delivered, n,
        g_delivered == n ? "PASS" : (g_delivered == 0 ? "FAIL(吞=指纹)" : "PARTIAL"));

    // 2. ICEBP
    g_delivered = 0;
    STUB_FN fnIcebp = (STUB_FN)(stubs + 64);
    for (int i = 0; i < n; i++) fnIcebp();
    printf("ICEBP (0xF1):    %3d/%d  %s\n", g_delivered, n,
        g_delivered == n ? "PASS" : (g_delivered == 0 ? "FAIL(吞=经典指纹)" : "PARTIAL"));

    // 3. TF 单步
    g_delivered = 0;
    STUB_FN fnTf = (STUB_FN)(stubs + 128);
    for (int i = 0; i < n; i++) fnTf();
    printf("TF 单步陷阱:     %3d/%d  %s\n", g_delivered, n,
        g_delivered == n ? "PASS" : (g_delivered == 0 ? "FAIL(单步被吞)" : "PARTIAL"));

    // 4. DR0/DR7 写读一致性（挂起-写-读回）
    typedef struct { HANDLE ready; volatile LONG* stop; } WORKER_ARG;
    printf("DR 写读一致性:   ");
    static volatile LONG g_stop = 0;
    HANDLE hReady = CreateEventA(NULL, FALSE, FALSE, NULL);
    WORKER_ARG wa = { hReady, &g_stop };
    HANDLE hThread = CreateThread(NULL, 0, [](LPVOID p) -> DWORD
    {
        WORKER_ARG* a = (WORKER_ARG*)p;
        SetEvent(a->ready);
        while (!*a->stop) { Sleep(1); }
        return 0;
    }, &wa, 0, NULL);
    WaitForSingleObject(hReady, INFINITE);
    Sleep(50);

    SuspendThread(hThread);
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    BOOL ok = GetThreadContext(hThread, &ctx);
    ULONG64 saveDr0 = ctx.Dr0, saveDr7 = ctx.Dr7;
    // 写入测试值: DR0=规范地址值, DR7=L0|RA1(bit10)=裸机恒读形态
    ctx.Dr0 = 0x0000414100004141ULL;
    ctx.Dr7 = 0x401;
    BOOL okSet = SetThreadContext(hThread, &ctx);
    ResumeThread(hThread);
    Sleep(50);
    SuspendThread(hThread);
    CONTEXT back = { 0 };
    back.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    BOOL okBack = GetThreadContext(hThread, &back);
    ResumeThread(hThread);
    // 还原
    SuspendThread(hThread);
    CONTEXT rst = { 0 };
    rst.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    rst.Dr0 = saveDr0;
    rst.Dr7 = saveDr7;
    SetThreadContext(hThread, &rst);
    ResumeThread(hThread);
    g_stop = 1;
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    CloseHandle(hReady);

    if (ok && okSet && okBack)
    {
        BOOL dr0ok = back.Dr0 == 0x0000414100004141ULL;
        BOOL dr7ok = (back.Dr7 == 0x401) || ((back.Dr7 & 0x401) == 0x401);
        printf("DR0 写读=%s (回读 0x%016llX) DR7=%s (回读 0x%llX, 期望含 0x401)\n",
            dr0ok ? "一致" : "漂移", (unsigned long long)back.Dr0,
            dr7ok ? "一致" : "漂移", (unsigned long long)back.Dr7);
        printf("                   %s\n", (dr0ok && dr7ok) ? "PASS" : "FAIL(影子不一致)");
    }
    else
    {
        printf("Get/SetThreadContext 失败: %lu %lu %lu (信息性)\n",
            (unsigned long)ok, (unsigned long)okSet, (unsigned long)okBack);
    }

    RemoveVectoredExceptionHandler(veh);
    VirtualFree(stubs, 0, MEM_RELEASE);
    printf("\n判读: 三形态(裸机/VMware/框架)各跑一遍对照; FAIL 项=调试面指纹\n");
    return 0;
}
