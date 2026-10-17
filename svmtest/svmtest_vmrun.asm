OPTION CASEMAP:NONE

.code

PUBLIC SvmTestVmrun
PUBLIC SvmTestCaptureCpuState

extern SvmTestHandleVmExit : proc

SVMTEST_ENTRY_GUEST_VMCB_PA    EQU 000h
SVMTEST_ENTRY_HOST_VMCB_PA     EQU 008h
SVMTEST_ENTRY_HOST_STACK_TOP   EQU 010h
SVMTEST_ENTRY_ORIGINAL_RSP     EQU 018h
SVMTEST_ENTRY_ORIGINAL_RFLAGS  EQU 020h
SVMTEST_ENTRY_RUN_CONTEXT      EQU 030h
SVMTEST_ENTRY_GUEST_GPRS       EQU 038h

SVMTEST_GPR_RBX                EQU 000h
SVMTEST_GPR_RCX                EQU 008h
SVMTEST_GPR_RDX                EQU 010h
SVMTEST_GPR_RBP                EQU 018h
SVMTEST_GPR_RSI                EQU 020h
SVMTEST_GPR_RDI                EQU 028h
SVMTEST_GPR_R8                 EQU 030h
SVMTEST_GPR_R9                 EQU 038h
SVMTEST_GPR_R10                EQU 040h
SVMTEST_GPR_R11                EQU 048h
SVMTEST_GPR_R12                EQU 050h
SVMTEST_GPR_R13                EQU 058h
SVMTEST_GPR_R14                EQU 060h
SVMTEST_GPR_R15                EQU 068h

SvmTestVmrun PROC
    mov r11, rcx
    mov r10, rsp
    mov qword ptr [r11 + SVMTEST_ENTRY_ORIGINAL_RSP], r10
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    pushfq
    pop rax
    mov qword ptr [r11 + SVMTEST_ENTRY_ORIGINAL_RFLAGS], rax
    cli

    mov rax, qword ptr [r11 + SVMTEST_ENTRY_HOST_STACK_TOP]
    sub rax, 40h
    mov qword ptr [rax + 20h], r11
    mov rsp, rax
    mov qword ptr [rsp + 20h], r11

    mov rbx, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RBX]
    mov rcx, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RCX]
    mov rdx, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RDX]
    mov rbp, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RBP]
    mov rsi, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RSI]
    mov rdi, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RDI]
    mov r8, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R8]
    mov r9, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R9]
    mov r10, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R10]
    mov r12, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R12]
    mov r13, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R13]
    mov r14, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R14]
    mov r15, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R15]
    mov rax, qword ptr [r11 + SVMTEST_ENTRY_HOST_VMCB_PA]
    vmsave rax
    mov rax, qword ptr [r11 + SVMTEST_ENTRY_GUEST_VMCB_PA]
    vmload rax
    mov r11, qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R11]
    mov rax, qword ptr [rsp + 20h]
    mov rax, qword ptr [rax + SVMTEST_ENTRY_GUEST_VMCB_PA]
    vmrun rax

    mov qword ptr [rsp + 00h], r10
    mov qword ptr [rsp + 08h], r11
    mov r11, qword ptr [rsp + 20h]
    mov rax, qword ptr [r11 + SVMTEST_ENTRY_GUEST_VMCB_PA]
    vmsave rax
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RBX], rbx
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RCX], rcx
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RDX], rdx
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RBP], rbp
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RSI], rsi
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_RDI], rdi
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R8], r8
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R9], r9
    mov rax, qword ptr [rsp + 00h]
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R10], rax
    mov rax, qword ptr [rsp + 08h]
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R11], rax
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R12], r12
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R13], r13
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R14], r14
    mov qword ptr [r11 + SVMTEST_ENTRY_GUEST_GPRS + SVMTEST_GPR_R15], r15
    mov rax, qword ptr [r11 + SVMTEST_ENTRY_HOST_VMCB_PA]
    vmload rax
    mov rcx, r11
    lea rdx, [r11 + SVMTEST_ENTRY_GUEST_GPRS]
    call SvmTestHandleVmExit
    mov r11, qword ptr [rsp + 20h]
    mov r10, qword ptr [r11 + SVMTEST_ENTRY_ORIGINAL_RSP]
    mov rax, qword ptr [r11 + SVMTEST_ENTRY_ORIGINAL_RFLAGS]
    lea rsp, [r10 - 40h]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    push rax
    popfq
    stgi
    ret
SvmTestVmrun ENDP

SvmTestCaptureCpuState PROC
    mov r10, rcx
    mov ax, cs
    mov word ptr [r10 + 00h], ax
    mov ax, ds
    mov word ptr [r10 + 02h], ax
    mov ax, es
    mov word ptr [r10 + 04h], ax
    mov ax, ss
    mov word ptr [r10 + 06h], ax
    mov ax, fs
    mov word ptr [r10 + 08h], ax
    mov ax, gs
    mov word ptr [r10 + 0Ah], ax
    str ax
    mov word ptr [r10 + 0Ch], ax
    sldt ax
    mov word ptr [r10 + 0Eh], ax
    sgdt fword ptr [r10 + 10h]
    sidt fword ptr [r10 + 1Ah]
    mov rax, cr0
    mov qword ptr [r10 + 24h], rax
    mov rax, cr3
    mov qword ptr [r10 + 2Ch], rax
    mov rax, cr4
    mov qword ptr [r10 + 34h], rax
    pushfq
    pop rax
    mov qword ptr [r10 + 3Ch], rax
    ret
SvmTestCaptureCpuState ENDP

END
