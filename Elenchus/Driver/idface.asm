; =====================================================================
; idface.asm — 指令面探测助手（idface.c 配套, MASM x64）
;
; 每个桩只执行一条目标指令后返回；异常由 C 侧 __try/__except 捕获。
; 裸机语义基准:
;   VMXOFF/VMREAD: 非VMX操作 → #UD (0xC000001D)
;   STGI/CLGI:     非SVM(GIF无效) → #UD
; VMM 下若注入 #GP (0xC0000096) 或吞异常 = 身份面差异（B2 行数据）
; =====================================================================
.code
PUBLIC IdfVmxoff, IdfVmread, IdfStgi, IdfClgi

IdfVmxoff PROC
    vmxoff
    xor rax, rax
    ret
IdfVmxoff ENDP

IdfVmread PROC
    xor rbx, rbx
    vmread rbx, rax        ; VMREAD r/m64, r64: 读字段号rax → rbx
    mov rax, rbx
    ret
IdfVmread ENDP

IdfStgi PROC
    stgi
    xor rax, rax
    ret
IdfStgi ENDP

IdfClgi PROC
    clgi
    xor rax, rax
    ret
IdfClgi ENDP

END
