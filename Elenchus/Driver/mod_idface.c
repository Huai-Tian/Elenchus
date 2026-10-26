// =====================================================================
// mod_idface.c — B2 探测模块: MSR 读 + VMX/SVM 指令面
//
// IOCTL (elenchus.h 段 [0x830]):
//   ELEN_IOCTL_IDFACE_MSR   in : ULONG32 Msr → out { ULONG64 Value; ULONG Code; }
//     Code: 0=成功; 0xC000001D=#UD; 0xC0000096=#GP; 其他=异常码
//   ELEN_IOCTL_IDFACE_INSTR in : ULONG Which (0=VMXOFF 1=VMREAD 2=STGI 3=CLGI)
//                             → out { ULONG Code; }
//
// 架构语义基准(SDM/APM 原文, 详见 Elenchus 仓库 Script/PROTOCOL.md):
//   VMXOFF/VMREAD: VMX operation 之外恒 #UD (SDM Vol.3C 33-27)
//   CLGI: EFER.SVME=0 → #UD (APM 指令章)
//   STGI: SVME=1 或 CPUID Fn8000_0001_ECX[SKINIT]=1 时合法! (APM §15.4)
//   判读一律以"本机裸机同名输出"为参照。
// =====================================================================
#include "elenchus.h"

// idface.asm 桩
typedef ULONG64 (*IDF_FN)(ULONG64);
extern ULONG64 IdfVmxoff(ULONG64);
extern ULONG64 IdfVmread(ULONG64);
extern ULONG64 IdfStgi(ULONG64);
extern ULONG64 IdfClgi(ULONG64);

NTSTATUS IdfaceDispatch(ULONG Code, PIRP Irp, PIO_STACK_LOCATION Io, ULONG* OutLen)
{
    if (Code == ELEN_IOCTL_IDFACE_MSR &&
        Io->Parameters.DeviceIoControl.InputBufferLength >= sizeof(ULONG32) &&
        Io->Parameters.DeviceIoControl.OutputBufferLength >= 8 + sizeof(ULONG) * 2)
    {
        typedef struct _MSR_OUT { ULONG64 Value; ULONG Code; ULONG Pad; } MSR_OUT;
        ULONG32 msr = *(ULONG32*)Irp->AssociatedIrp.SystemBuffer;
        MSR_OUT* out = (MSR_OUT*)Irp->AssociatedIrp.SystemBuffer;
        out->Value = 0;
        out->Code = 0;
        __try
        {
            out->Value = __readmsr((ULONG)msr);    // #GP 由 except 捕获
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out->Code = (ULONG)GetExceptionCode();
        }
        *OutLen = sizeof(MSR_OUT);
        return STATUS_SUCCESS;
    }
    if (Code == ELEN_IOCTL_IDFACE_INSTR &&
        Io->Parameters.DeviceIoControl.InputBufferLength >= sizeof(ULONG) &&
        Io->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG) * 2)
    {
        typedef struct _INSTR_OUT { ULONG Code; ULONG Pad; } INSTR_OUT;
        ULONG which = *(ULONG*)Irp->AssociatedIrp.SystemBuffer;
        INSTR_OUT* out = (INSTR_OUT*)Irp->AssociatedIrp.SystemBuffer;
        IDF_FN fn = NULL;
        switch (which)
        {
        case 0: fn = (IDF_FN)IdfVmxoff; break;   // rax 随意
        case 1: fn = (IDF_FN)IdfVmread; break;   // rax=字段号
        case 2: fn = (IDF_FN)IdfStgi; break;
        case 3: fn = (IDF_FN)IdfClgi; break;
        default: return STATUS_INVALID_PARAMETER;
        }
        out->Code = 0;
        __try
        {
            fn(0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out->Code = (ULONG)GetExceptionCode();
        }
        *OutLen = sizeof(INSTR_OUT);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_IMPLEMENTED;
}
