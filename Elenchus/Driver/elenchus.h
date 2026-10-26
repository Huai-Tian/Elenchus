// =====================================================================
// elenchus.h — 统一探测器驱动公共头
//
// Elenchus.sys = 四个探测模块共用一个设备对象与 IRP 分发:
//   \\Device\\Elenchus / \\DosDevices\\Elenchus
//
// IOCTL 段分配 (CTL_CODE(FILE_DEVICE_UNKNOWN, fn, METHOD_BUFFERED, FILE_ANY_ACCESS)):
//   [0x800] HptMap  : 0x800 MAP / 0x801 READ / 0x802 UNMAP   (A2 时钟轴)
//   [0x810] Rxdiff  : 0x810 PROBE                            (C2 读-执行差分)
//   [0x820] Kbench  : 0x820 RUN / 0x821 SETINLINE / 0x822 GETADDR (PERF)
//   [0x830] Idface  : 0x830 MSR / 0x831 INSTR                (B2 身份面)
//
// 分发契约: 各模块 Dispatch 若不认领该 IOCTL 码, 返回
//   STATUS_NOT_IMPLEMENTED 且不碰 Irp; 认领则完成填充并返回最终状态。
// =====================================================================
#pragma once
#include <ntddk.h>

#define ELENCHUS_DEVICE_NAME L"\\Device\\Elenchus"
#define ELENCHUS_DOS_NAME    L"\\DosDevices\\Elenchus"

// ---- HptMap (A2) ----
#define ELEN_IOCTL_HPTMAP_MAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ELEN_IOCTL_HPTMAP_READ  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ELEN_IOCTL_HPTMAP_UNMAP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ---- Rxdiff (C2) ----
#define ELEN_IOCTL_RXDIFF_PROBE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ---- Kbench (PERF) ----
#define ELEN_IOCTL_KBENCH_RUN       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ELEN_IOCTL_KBENCH_SETINLINE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ELEN_IOCTL_KBENCH_GETADDR   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x822, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ---- Idface (B2) ----
#define ELEN_IOCTL_IDFACE_MSR   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x830, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ELEN_IOCTL_IDFACE_INSTR CTL_CODE(FILE_DEVICE_UNKNOWN, 0x831, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 各模块分发入口: 认领→处理并返回最终 NTSTATUS; 不认领→STATUS_NOT_IMPLEMENTED
NTSTATUS HptMapDispatch(ULONG Code, PIRP Irp, PIO_STACK_LOCATION Io, ULONG* OutLen);
NTSTATUS RxdiffDispatch(ULONG Code, PIRP Irp, PIO_STACK_LOCATION Io, ULONG* OutLen);
NTSTATUS KbenchDispatch(ULONG Code, PIRP Irp, PIO_STACK_LOCATION Io, ULONG* OutLen);
NTSTATUS IdfaceDispatch(ULONG Code, PIRP Irp, PIO_STACK_LOCATION Io, ULONG* OutLen);

// 卸载期清理(kbench: inline 还原+trampoline 释放; hptmap: MMIO 全 unmap)
VOID HptMapShutdown(VOID);
VOID KbenchShutdown(VOID);
