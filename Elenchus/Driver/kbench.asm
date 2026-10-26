; =====================================================================
; kbench.asm — kbench.c 配套（MASM x64）
;
; KbTarget: 手写靶函数。**布局契约**: 前 12 字节恰为 4 条 3B 寄存器
; 指令（可整体拷贝为 trampoline 前缀, +12 为严格指令边界）, 之后
; not/ret。r11 在全函数中未用 → 跳转桥安全。
;
; KbDetour: inline 基线的替换体, 尾跳 trampoline（trampoline 执行
; 原 12B 前缀后跳回 KbTarget+12 完成原函数, ret 直返原调用者）。
; =====================================================================
.code
EXTERN KbTrampFn:QWORD
PUBLIC KbTarget, KbDetour

KbTarget PROC
    mov     rax, rcx        ; 48 89 C8   (+3)
    add     rax, rdx        ; 48 01 D0   (+6)
    xor     rax, r8         ; 4C 31 C0   (+9)
    sub     rax, r9         ; 4C 29 C8   (+12) ← trampoline 拷贝边界
    not     rax             ; 48 F7 D0
    ret                     ; C3
KbTarget ENDP

KbDetour PROC
    jmp     QWORD PTR [KbTrampFn]
KbDetour ENDP

END
