// =====================================================================
// mod_kbench.c — PERF 探测模块: 内核侧每跳开销基准
//
// 提供:
//   1. KbTarget 手写靶函数(kbench.asm, 12B 安全前缀布局)
//   2. ELEN_IOCTL_KBENCH_RUN: 内核态 rdtsc 夹逼 N 次调用 → cycles/call
//      (当前函数处于什么形态就测什么形态: 裸/inline/框架hook)
//   3. ELEN_IOCTL_KBENCH_SETINLINE: 装/卸 12B inline jmp(自产基线)
//      12B = 48 B8 imm64(KbDetour) + FF E0 (mov rax,imm; jmp rax)
//   4. ELEN_IOCTL_KBENCH_GETADDR: 返回 KbTarget VA(框架 demo 挂钩用)
//
// WP 位切换装桩; 本核装本核测(R3 钉核), 无跨核问题。
// =====================================================================
#include "elenchus.h"

// ExAllocatePoolWithTag 弃用警告静音(兼容 2004 以前的 Win10)
#pragma warning(disable : 4996)

typedef ULONG64 (*KB_FN)(ULONG64, ULONG64, ULONG64, ULONG64);

extern ULONG64 KbTarget(ULONG64, ULONG64, ULONG64, ULONG64);
extern ULONG64 KbDetour(ULONG64, ULONG64, ULONG64, ULONG64);

// trampoline 函数指针(KbDetour 经 [KbTrampFn] 尾跳)
KB_FN KbTrampFn = NULL;

typedef struct _KB_RUN_IN  { ULONG64 Iterations; } KB_RUN_IN;
typedef struct _KB_RUN_OUT { ULONG64 CyclesPerCall; ULONG64 Checksum; ULONG64 TargetVa; } KB_RUN_OUT;

static UCHAR* g_trampoline = NULL;   // NonPaged 池: 12B前缀+12B跳回
static UCHAR g_origBytes[12];        // 卸载还原
static BOOLEAN g_inlineOn = FALSE;

// 装桩: 关 WP → 写 12B jmp → 开 WP
static NTSTATUS KbenchSetInline(BOOLEAN On)
{
    // trampoline 一次性构建
    if (g_trampoline == NULL)
    {
        g_trampoline = (UCHAR*)ExAllocatePoolWithTag(NonPagedPool, 32, 'cBk');
        if (g_trampoline == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        // [0..11] 原 12B 前缀(与 KbTarget 逐字节相同——asm 布局契约)
        RtlCopyMemory(g_trampoline, (PVOID)KbTarget, 12);
        // [12..23] mov r11, KbTarget+12; jmp r11
        ULONG64 cont = (ULONG64)KbTarget + 12;
        g_trampoline[12] = 0x49; g_trampoline[13] = 0xBB;
        *(ULONG64*)(g_trampoline + 14) = cont;
        g_trampoline[22] = 0x41; g_trampoline[23] = 0xFF;
        g_trampoline[24] = 0xE3;
        KbTrampFn = (KB_FN)g_trampoline;
    }

    ULONG64 cr0 = __readcr0();
    __writecr0(cr0 & ~0x10000ULL);              // WP off
    _disable();
    if (On && !g_inlineOn)
    {
        RtlCopyMemory(g_origBytes, (PVOID)KbTarget, 12);
        UCHAR jmp12[12] = { 0 };
        jmp12[0] = 0x48; jmp12[1] = 0xB8;                        // mov rax, imm64
        *(ULONG64*)(jmp12 + 2) = (ULONG64)KbDetour;
        jmp12[10] = 0xFF; jmp12[11] = 0xE0;                      // jmp rax
        RtlCopyMemory((PVOID)KbTarget, jmp12, 12);
        g_inlineOn = TRUE;
    }
    else if (!On && g_inlineOn)
    {
        RtlCopyMemory((PVOID)KbTarget, g_origBytes, 12);
        g_inlineOn = FALSE;
    }
    _enable();
    __writecr0(cr0);                            // WP 恢复
    KeFlushWriteBuffer();
    return STATUS_SUCCESS;
}

NTSTATUS KbenchDispatch(ULONG Code, PIRP Irp, PIO_STACK_LOCATION Io, ULONG* OutLen)
{
    if (Code == ELEN_IOCTL_KBENCH_RUN &&
        Io->Parameters.DeviceIoControl.InputBufferLength >= sizeof(KB_RUN_IN) &&
        Io->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(KB_RUN_OUT))
    {
        KB_RUN_IN* in = (KB_RUN_IN*)Irp->AssociatedIrp.SystemBuffer;
        KB_RUN_OUT* out = (KB_RUN_OUT*)Irp->AssociatedIrp.SystemBuffer;
        ULONG64 iters = in->Iterations ? in->Iterations : 1000000ULL;

        // 预热(训练分支/缓存; 亦使框架侧视图驻留稳定)
        ULONG64 warm = 0;
        for (ULONG64 i = 0; i < 10000; i++)
        {
            warm += KbTarget(i, i + 1, i + 2, i + 3);
        }

        KB_FN fn = (KB_FN)KbTarget;
        ULONG64 check = 0;
        ULONG64 t0 = __rdtsc();
        for (ULONG64 i = 0; i < iters; i++)
        {
            check += fn(i, i + 1, i + 2, i + 3);
        }
        ULONG64 t1 = __rdtsc();

        out->CyclesPerCall = (t1 - t0) / iters;
        out->Checksum = check ^ warm;
        out->TargetVa = (ULONG64)KbTarget;
        *OutLen = sizeof(KB_RUN_OUT);
        return STATUS_SUCCESS;
    }
    if (Code == ELEN_IOCTL_KBENCH_SETINLINE &&
        Io->Parameters.DeviceIoControl.InputBufferLength >= sizeof(ULONG) &&
        Io->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG))
    {
        ULONG on = *(ULONG*)Irp->AssociatedIrp.SystemBuffer;
        NTSTATUS st = KbenchSetInline(on ? TRUE : FALSE);
        if (NT_SUCCESS(st))
        {
            *(ULONG*)Irp->AssociatedIrp.SystemBuffer = g_inlineOn ? 1 : 0;
            *OutLen = sizeof(ULONG);
        }
        return st;
    }
    if (Code == ELEN_IOCTL_KBENCH_GETADDR &&
        Io->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG64))
    {
        *(ULONG64*)Irp->AssociatedIrp.SystemBuffer = (ULONG64)KbTarget;
        *OutLen = sizeof(ULONG64);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_IMPLEMENTED;
}

VOID KbenchShutdown(VOID)
{
    if (g_inlineOn)
    {
        KbenchSetInline(FALSE);
    }
    if (g_trampoline != NULL)
    {
        RtlZeroMemory(g_trampoline, 32);
        ExFreePoolWithTag(g_trampoline, 'cBk');
        g_trampoline = NULL;
    }
}
