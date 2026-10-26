// =====================================================================
// mod_hptmap.c — A2 探测模块: 物理地址映射读取 (HPET/PM_TMR)
//
// 目的: 把 MMIO 时钟计数器暴露给 R3 采样器做跨时钟域相干性探测。
//       模块本身零探测逻辑——只做 MmMapIoSpace 映射与读取。
//
// IOCTL (elenchus.h 段 [0x800]):
//   ELEN_IOCTL_HPTMAP_MAP    in : ULONG64 PhysAddr   out: ULONG Slot
//   ELEN_IOCTL_HPTMAP_READ   in : ULONG Slot         out: ULONG64 Value
//                                 (双读至稳定, 防跨 clk 边沿撕裂)
//   ELEN_IOCTL_HPTMAP_UNMAP  in : ULONG Slot
// =====================================================================
#include "elenchus.h"

#define HPTMAP_SLOTS 8

typedef struct _HPTMAP_SLOT
{
    volatile ULONG64* Reg;   // 映射后的计数器虚拟地址(8B对齐读)
} HPTMAP_SLOT;

static HPTMAP_SLOT g_hptSlots[HPTMAP_SLOTS];

NTSTATUS HptMapDispatch(ULONG Code, PIRP Irp, PIO_STACK_LOCATION Io, ULONG* OutLen)
{
    NTSTATUS st = STATUS_NOT_IMPLEMENTED;

    if (Code == ELEN_IOCTL_HPTMAP_MAP &&
        Io->Parameters.DeviceIoControl.InputBufferLength >= sizeof(ULONG64) &&
        Io->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG))
    {
        ULONG64 phys = *(ULONG64*)Irp->AssociatedIrp.SystemBuffer;
        ULONG i;
        st = STATUS_UNSUCCESSFUL;
        for (i = 0; i < HPTMAP_SLOTS; i++)
        {
            if (g_hptSlots[i].Reg == NULL)
            {
                break;
            }
        }
        if (i < HPTMAP_SLOTS)
        {
            // 8字节窗口, 非缓存(计数器是设备内存)
            PHYSICAL_ADDRESS pa;
            pa.QuadPart = (LONGLONG)phys;
            PVOID va = MmMapIoSpace(pa, 8, MmNonCached);
            if (va != NULL)
            {
                g_hptSlots[i].Reg = (volatile ULONG64*)va;
                *(ULONG*)Irp->AssociatedIrp.SystemBuffer = i;
                st = STATUS_SUCCESS;
                *OutLen = sizeof(ULONG);
            }
            else
            {
                st = STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        else
        {
            st = STATUS_TOO_MANY_NODES;    // 槽满
        }
    }
    else if (Code == ELEN_IOCTL_HPTMAP_READ &&
             Io->Parameters.DeviceIoControl.InputBufferLength >= sizeof(ULONG) &&
             Io->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG64))
    {
        ULONG i = *(ULONG*)Irp->AssociatedIrp.SystemBuffer;
        st = STATUS_INVALID_PARAMETER;
        if (i < HPTMAP_SLOTS && g_hptSlots[i].Reg != NULL)
        {
            // 双读稳定: MMIO 计数器跨位读可能撕裂, 重读至一致
            ULONG64 v0 = *g_hptSlots[i].Reg;
            for (ULONG k = 0; k < 8; k++)
            {
                ULONG64 v1 = *g_hptSlots[i].Reg;
                if (v1 == v0)
                {
                    break;
                }
                v0 = v1;
            }
            *(ULONG64*)Irp->AssociatedIrp.SystemBuffer = v0;
            st = STATUS_SUCCESS;
            *OutLen = sizeof(ULONG64);
        }
    }
    else if (Code == ELEN_IOCTL_HPTMAP_UNMAP &&
             Io->Parameters.DeviceIoControl.InputBufferLength >= sizeof(ULONG))
    {
        ULONG i = *(ULONG*)Irp->AssociatedIrp.SystemBuffer;
        st = STATUS_INVALID_PARAMETER;
        if (i < HPTMAP_SLOTS && g_hptSlots[i].Reg != NULL)
        {
            MmUnmapIoSpace((PVOID)g_hptSlots[i].Reg, 8);
            g_hptSlots[i].Reg = NULL;
            st = STATUS_SUCCESS;
        }
    }
    return st;
}

VOID HptMapShutdown(VOID)
{
    for (ULONG i = 0; i < HPTMAP_SLOTS; i++)
    {
        if (g_hptSlots[i].Reg != NULL)
        {
            MmUnmapIoSpace((PVOID)g_hptSlots[i].Reg, 8);
            g_hptSlots[i].Reg = NULL;
        }
    }
}
