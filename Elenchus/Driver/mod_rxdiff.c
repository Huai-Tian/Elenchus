// =====================================================================
// mod_rxdiff.c — C2 探测模块: 读-执行差分
//
// 原理: 对疑似被 hook 的内核函数执行 "读-执行-读" 序列, 并与磁盘上
//       ntoskrnl.exe 权威副本比对:
//         读1 (执行前)   — 若 hook 页有读可见工件(如驻留视图可读的影子
//                          跳转码), 此处即暴露
//         执行 NtClose   — 触发拦截; 若被双视图 hook, 本核驻留 hooked 视图
//         读2 (执行后)   — 驻留后同核再读: 影子驻留核若无读透明(R=1
//                          影子可读)即暴露 14B 跳转
//         磁盘权威字节   — 读1/读2 与之逐字节 diff
//
//       三模式预期(GnptHooks):
//         普通模式(CodePage) 驻留 Secondary 的核: 读2 可见跳转 = FAIL
//         TRANSPARENT(DR) / NX-FENCE: 两读均 == 磁盘 = PASS(结构性)
//       GeptHooks: HideRead=1(exec-only+MTF) = PASS; HideRead=0 视核形态
//
// IOCTL (elenchus.h 段 [0x810]):
//   in : { ULONG64 TargetVa; ULONG Len; ULONG ExecCount; }
//        TargetVa=0 → 驱动自解析 NtClose
//   out: { ULONG64 TargetVa; ULONG64 NtBase; ULONG Len;
//          UCHAR Before[64]; UCHAR After[64]; UCHAR Disk[64];
//          NTSTATUS ExecStatus; NTSTATUS DiskStatus; }
//
// 注意: IOCTL 在调用者线程上下文执行(METHOD_BUFFERED),
//       R3 端钉核 = "同核读-执行-读" 语义成立。
// =====================================================================
#include "elenchus.h"

// ---- ZwQuerySystemInformation / SystemModuleInformation 不在 wdm.h(ntifs.h 专有),
//      WDM 驱动手动声明原型(ntoskrnl 导出, 链接无问题) ----
typedef enum _RXD_SYSINFO_CLASS { SystemModuleInformation = 11 } RXD_SYSINFO_CLASS;
NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(
    _In_ RXD_SYSINFO_CLASS SystemInformationClass,
    _Inout_updates_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength);

// ExAllocatePoolWithTag 弃用警告静音(保留旧 API: 兼容 2004 以前的 Win10,
// ExAllocatePool2 需 2004+, Intel 测试机版本未知)
#pragma warning(disable : 4996)

#define RXDIFF_MAX 64

typedef struct _RXDIFF_IN
{
    ULONG64 TargetVa;    // 0 = 自动解析 NtClose
    ULONG   Len;         // 比对长度(<=64)
    ULONG   ExecCount;   // 执行次数(默认1; >1 提高驻留概率)
} RXDIFF_IN;

typedef struct _RXDIFF_OUT
{
    ULONG64 TargetVa;    // 实际目标
    ULONG64 NtBase;      // ntoskrnl 基址
    ULONG   Len;
    UCHAR   Before[RXDIFF_MAX];   // 执行前读
    UCHAR   After[RXDIFF_MAX];    // 执行后读
    UCHAR   Disk[RXDIFF_MAX];     // 磁盘权威字节
    NTSTATUS ExecStatus;          // NtClose(-1) 返回值(STATUS_INVALID_HANDLE=正常)
    NTSTATUS DiskStatus;          // 磁盘读取状态
} RXDIFF_OUT;

// 解析 ntoskrnl 基址(SystemModuleInformation[0] = 内核本体)
static BOOLEAN RxdiffNtBase(PULONG64 Base)
{
    ULONG need = 0;
    ULONG_PTR* info = NULL;
    ULONG i;
    ZwQuerySystemInformation(SystemModuleInformation, NULL, 0, &need);
    if (need == 0)
    {
        return FALSE;
    }
    info = ExAllocatePoolWithTag(NonPagedPool, need, 'fxdR');
    if (info == NULL)
    {
        return FALSE;
    }
    if (!NT_SUCCESS(ZwQuerySystemInformation(SystemModuleInformation, info, need, &need)))
    {
        ExFreePoolWithTag(info, 'fxdR');
        return FALSE;
    }
    // RTL_PROCESS_MODULE_INFORMATION 布局(Win10 x64):
    //   PVOID Reserved[2]; PVOID ImageBase; ULONG ImageSize; ...
    typedef struct _MODULE_ENTRY {
        PVOID Reserved[2];
        PVOID ImageBase;
        ULONG ImageSize;
        ULONG Flags;
        USHORT LoadOrderIndex, InitOrderIndex, LoadCount;
        USHORT OffsetToFileName;
        CHAR  FullPathName[256];
    } MODULE_ENTRY;
    ULONG count = (ULONG)info[0];
    MODULE_ENTRY* e = (MODULE_ENTRY*)((PUCHAR)info + sizeof(ULONG_PTR));
    BOOLEAN ok = FALSE;
    for (i = 0; i < count && i == 0; i++)
    {
        *Base = (ULONG64)e[i].ImageBase;
        ok = TRUE;
    }
    ExFreePoolWithTag(info, 'fxdR');
    return ok;
}

// 从磁盘 ntoskrnl.exe 读 RVA 处权威字节(解析 PE 节表 RVA→文件偏移)
static NTSTATUS RxdiffDiskBytes(ULONG64 NtBase, ULONG64 TargetVa,
    ULONG Len, UCHAR* Out)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\SystemRoot\\System32\\ntoskrnl.exe");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    NTSTATUS st;
    UCHAR hdr[PAGE_SIZE];
    ULONG64 rva = TargetVa - NtBase;

    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL, NULL);
    st = ZwCreateFile(&h, GENERIC_READ, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL, 0);
    if (!NT_SUCCESS(st))
    {
        return st;
    }
    st = ZwReadFile(h, NULL, NULL, NULL, &iosb, hdr, sizeof(hdr), NULL, NULL);
    if (!NT_SUCCESS(st))
    {
        ZwClose(h);
        return st;
    }
    // DOS header → NT headers → 节表
    if (*(USHORT*)hdr != 0x5A4D)
    {
        ZwClose(h);
        return STATUS_INVALID_IMAGE_NOT_MZ;
    }
    LONG e_lfanew = *(LONG*)(hdr + 0x3C);
    PUCHAR nth = hdr + e_lfanew;
    if (*(ULONG*)nth != 0x00004550)    // 'PE\0\0'
    {
        ZwClose(h);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    USHORT nsec = *(USHORT*)(nth + 6);
    USHORT optSize = *(USHORT*)(nth + 20);
    PUCHAR sec0 = nth + 24 + optSize;    // 第一节头
    // IMAGE_SECTION_HEADER: Name[8] VirtualSize(4) VirtualAddress(4)
    //   SizeOfRawData(4) PointerToRawData(4) ...
    BOOLEAN found = FALSE;
    ULONG fileOff = 0;
    for (USHORT s = 0; s < nsec; s++)
    {
        PUCHAR sh = sec0 + s * 40;
        ULONG vsz = *(ULONG*)(sh + 8);
        ULONG va = *(ULONG*)(sh + 12);
        ULONG rawSz = *(ULONG*)(sh + 16);
        ULONG rawPtr = *(ULONG*)(sh + 20);
        if (rva >= va && rva < (ULONG64)va + (vsz ? vsz : rawSz))
        {
            fileOff = (ULONG)(rva - va) + rawPtr;
            found = TRUE;
            break;
        }
    }
    if (!found)
    {
        ZwClose(h);
        return STATUS_NOT_FOUND;
    }
    LARGE_INTEGER off;
    off.QuadPart = fileOff;
    st = ZwReadFile(h, NULL, NULL, NULL, &iosb, Out, Len, &off, NULL);
    ZwClose(h);
    return st;
}

NTSTATUS RxdiffDispatch(ULONG Code, PIRP Irp, PIO_STACK_LOCATION Io, ULONG* OutLen)
{
    if (Code != ELEN_IOCTL_RXDIFF_PROBE ||
        Io->Parameters.DeviceIoControl.InputBufferLength < sizeof(RXDIFF_IN) ||
        Io->Parameters.DeviceIoControl.OutputBufferLength < sizeof(RXDIFF_OUT))
    {
        return STATUS_NOT_IMPLEMENTED;
    }
    {
        RXDIFF_IN* in = (RXDIFF_IN*)Irp->AssociatedIrp.SystemBuffer;
        RXDIFF_OUT* out = (RXDIFF_OUT*)Irp->AssociatedIrp.SystemBuffer;
        ULONG64 ntBase = 0;
        UNICODE_STRING ntCloseName = RTL_CONSTANT_STRING(L"NtClose");
        PVOID target = (PVOID)in->TargetVa;
        NTSTATUS st;

        RtlZeroMemory(out, sizeof(RXDIFF_OUT));
        out->Len = (in->Len == 0 || in->Len > RXDIFF_MAX) ? 32 : in->Len;

        if (!RxdiffNtBase(&ntBase))
        {
            out->DiskStatus = STATUS_NOT_FOUND;
            return STATUS_UNSUCCESSFUL;
        }
        out->NtBase = ntBase;
        if (target == NULL)
        {
            target = MmGetSystemRoutineAddress(&ntCloseName);
        }
        if (target == NULL)
        {
            return STATUS_NOT_FOUND;
        }
        out->TargetVa = (ULONG64)target;

        // 读1: 执行前(此刻本核处于任意视图形态)
        __try
        {
            RtlCopyMemory(out->Before, target, out->Len);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return GetExceptionCode();
        }

        // 执行: 触发拦截(若被双视图 hook, 本核自此驻留 hooked 视图)
        {
            NTSTATUS(NTAPI * pNtClose)(HANDLE) = (PVOID)target;
            for (ULONG k = 0; k < (in->ExecCount ? in->ExecCount : 1); k++)
            {
                out->ExecStatus = pNtClose((HANDLE)-1);    // 无副作用安全调用
            }
        }

        // 读2: 同核执行后
        __try
        {
            RtlCopyMemory(out->After, target, out->Len);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return GetExceptionCode();
        }

        // 磁盘权威字节
        out->DiskStatus = RxdiffDiskBytes(ntBase, (ULONG64)target, out->Len, out->Disk);
        *OutLen = sizeof(RXDIFF_OUT);
        return STATUS_SUCCESS;
    }
}
