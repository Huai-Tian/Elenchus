// =====================================================================
// main.c — Elenchus 统一探测器驱动: 入口 + IRP 分发路由
//
// 一个设备对象, 四个探测模块按 IOCTL 段认领:
//   HptMap(A2 时钟轴) / Rxdiff(C2 读-执行差分) /
//   Kbench(PERF 每跳基准) / Idface(B2 MSR+指令面)
//
// 安装: sc create elenchus type= kernel start= demand binPath= "<path>\Driver.sys"
//       sc start elenchus
// 卸载: sc stop elenchus && sc delete elenchus
//
// 驱动本身零探测逻辑、零驻留状态(除各模块显式资源), 纯被动响应 IOCTL。
// =====================================================================
#include "elenchus.h"

static DRIVER_DISPATCH ElenchusCreateClose, ElenchusIoctl;
static DRIVER_UNLOAD ElenchusUnload;

static NTSTATUS ElenchusCreateClose(PDEVICE_OBJECT Dev, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Dev);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS ElenchusIoctl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Dev);
    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = io->Parameters.DeviceIoControl.IoControlCode;
    ULONG outLen = 0;
    NTSTATUS st;

    // 依次询问各模块; STATUS_NOT_IMPLEMENTED = 该模块不认领
    st = HptMapDispatch(code, Irp, io, &outLen);
    if (st == STATUS_NOT_IMPLEMENTED)
    {
        st = RxdiffDispatch(code, Irp, io, &outLen);
    }
    if (st == STATUS_NOT_IMPLEMENTED)
    {
        st = KbenchDispatch(code, Irp, io, &outLen);
    }
    if (st == STATUS_NOT_IMPLEMENTED)
    {
        st = IdfaceDispatch(code, Irp, io, &outLen);
    }
    if (st == STATUS_NOT_IMPLEMENTED)
    {
        st = STATUS_INVALID_PARAMETER;    // 无人认领
    }

    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = outLen;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

static VOID ElenchusUnload(PDRIVER_OBJECT Drv)
{
    // 模块资源清理(顺序: 先还原可变内核状态, 再释放映射)
    KbenchShutdown();     // inline 还原 + trampoline 释放
    HptMapShutdown();     // MMIO 槽全 unmap

    UNICODE_STRING dos = RTL_CONSTANT_STRING(ELENCHUS_DOS_NAME);
    IoDeleteSymbolicLink(&dos);
    if (Drv->DeviceObject != NULL)
    {
        IoDeleteDevice(Drv->DeviceObject);
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING RegPath)
{
    UNREFERENCED_PARAMETER(RegPath);
    UNICODE_STRING dev = RTL_CONSTANT_STRING(ELENCHUS_DEVICE_NAME);
    UNICODE_STRING dos = RTL_CONSTANT_STRING(ELENCHUS_DOS_NAME);
    PDEVICE_OBJECT devObj = NULL;
    NTSTATUS st = IoCreateDevice(Drv, 0, &dev, FILE_DEVICE_UNKNOWN, 0, FALSE, &devObj);
    if (!NT_SUCCESS(st))
    {
        return st;
    }
    st = IoCreateSymbolicLink(&dos, &dev);
    if (!NT_SUCCESS(st))
    {
        IoDeleteDevice(devObj);
        return st;
    }
    Drv->MajorFunction[IRP_MJ_CREATE] = ElenchusCreateClose;
    Drv->MajorFunction[IRP_MJ_CLOSE] = ElenchusCreateClose;
    Drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ElenchusIoctl;
    Drv->DriverUnload = ElenchusUnload;
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Elenchus: unified probe driver loaded (hptmap/rxdiff/kbench/idface)\n");
    return STATUS_SUCCESS;
}
