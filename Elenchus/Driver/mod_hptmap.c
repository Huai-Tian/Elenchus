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

// 槽形态: Reg!=NULL → MMIO 映射(HPET 等 8B 计数器)
//          Port!=0   → I/O 口(PM_TMR 等 legacy 固定块, 4B 读)
// 两者皆零 = 空槽。R3 用 MAP 输入的 bit63 标志选择形态。
typedef struct _HPTMAP_SLOT
{
    volatile ULONG64* Reg;   // MMIO 虚拟地址
    ULONG Port;             // I/O 口号(Reg==NULL 时有效)
} HPTMAP_SLOT;

#define HPTMAP_FLAG_PORT 0x8000000000000000ULL   // MAP 输入 bit63 = I/O 口模式

static HPTMAP_SLOT g_hptSlots[HPTMAP_SLOTS];

// 空槽判定
static BOOLEAN SlotFree(const HPTMAP_SLOT* s)
{
    return s->Reg == NULL && s->Port == 0;
}

NTSTATUS HptMapDispatch(ULONG Code, PIRP Irp, PIO_STACK_LOCATION Io, ULONG* OutLen)
{
    NTSTATUS st = STATUS_NOT_IMPLEMENTED;

    if (Code == ELEN_IOCTL_HPTMAP_MAP &&
        Io->Parameters.DeviceIoControl.InputBufferLength >= sizeof(ULONG64) &&
        Io->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG))
    {
        ULONG64 addr = *(ULONG64*)Irp->AssociatedIrp.SystemBuffer;
        ULONG i;
        st = STATUS_UNSUCCESSFUL;
        for (i = 0; i < HPTMAP_SLOTS; i++)
        {
            if (SlotFree(&g_hptSlots[i]))
            {
                break;
            }
        }
        if (i >= HPTMAP_SLOTS)
        {
            st = STATUS_TOO_MANY_NODES;    // 槽满
        }
        else if (addr & HPTMAP_FLAG_PORT)
        {
            // I/O 口槽(PM_TMR 等): 口号 = 低 16 位
            g_hptSlots[i].Reg = NULL;
            g_hptSlots[i].Port = (ULONG)(addr & 0xFFFF);
            *(ULONG*)Irp->AssociatedIrp.SystemBuffer = i;
            st = STATUS_SUCCESS;
            *OutLen = sizeof(ULONG);
        }
        else
        {
            // MMIO 槽(HPET 等): 8字节窗口, 非缓存
            PHYSICAL_ADDRESS pa;
            pa.QuadPart = (LONGLONG)addr;
            PVOID va = MmMapIoSpace(pa, 8, MmNonCached);
            if (va != NULL)
            {
                g_hptSlots[i].Reg = (volatile ULONG64*)va;
                g_hptSlots[i].Port = 0;
                *(ULONG*)Irp->AssociatedIrp.SystemBuffer = i;
                st = STATUS_SUCCESS;
                *OutLen = sizeof(ULONG);
            }
            else
            {
                st = STATUS_INSUFFICIENT_RESOURCES;
            }
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
            // MMIO: 双读稳定(跨位读可能撕裂, 重读至一致)
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
        else if (i < HPTMAP_SLOTS && g_hptSlots[i].Port != 0)
        {
            // I/O 口: 4 字节读, 双读稳定
            ULONG v0 = READ_PORT_ULONG((PULONG)(ULONG_PTR)g_hptSlots[i].Port);
            for (ULONG k = 0; k < 8; k++)
            {
                ULONG v1 = READ_PORT_ULONG((PULONG)(ULONG_PTR)g_hptSlots[i].Port);
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
        else if (i < HPTMAP_SLOTS && g_hptSlots[i].Port != 0)
        {
            g_hptSlots[i].Port = 0;    // 口槽无资源需释放
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
        g_hptSlots[i].Port = 0;
    }
}
