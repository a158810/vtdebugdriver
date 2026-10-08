OPTION CASEMAP:NONE

EXTERN LmvGuestStaticProbeDecision:PROC
PUBLIC LmvGuestStaticProbe

.code

LmvGuestStaticProbe PROC
    pushfq
    pop qword ptr [r8]
    pushfq
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    sub rsp, 28h
    call LmvGuestStaticProbeDecision
    test al, al
    lea rsp, [rsp + 28h]
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    jz LmvGuestStaticProbeContinue
    popfq
    pushfq
    pop qword ptr [r8 + 8]
    mov eax, 44h
    ret
LmvGuestStaticProbeContinue:
    popfq
    pushfq
    pop qword ptr [r8 + 8]
    mov dword ptr [r8 + 24h], 1
    lea rax, [rcx + 1]
    ret
LmvGuestStaticProbe ENDP

EXTERN LmvGuestKernelProbeMirror:PROC
PUBLIC LmvGuestKernelProbeRedirect

LmvGuestKernelProbeRedirect PROC
    pushfq
    pop qword ptr [r8]
    pushfq
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    sub rsp, 28h
    call LmvGuestStaticProbeDecision
    test al, al
    lea rsp, [rsp + 28h]
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    jz LmvGuestKernelProbeContinue
    popfq
    pushfq
    pop qword ptr [r8 + 8]
    mov eax, 44h
    ret
LmvGuestKernelProbeContinue:
    popfq
    pushfq
    pop qword ptr [r8 + 8]
    mov dword ptr [r8 + 24h], 1
    jmp LmvGuestKernelProbeMirror
LmvGuestKernelProbeRedirect ENDP
END