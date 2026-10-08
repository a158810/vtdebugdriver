OPTION CASEMAP:NONE

LMV_CPUID_CALL_LEAF EQU 04C4D0001h
LMV_CPUID_CALL_MAGIC EQU 04C4D5654434D414Ch

.code

LmvGuestCpuidCall PROC
    push rbx
    push rsi
    push r13
    push r14
    push r15
    mov rsi, r8
    mov r13d, edx
    mov r14d, ecx
    mov r15, LMV_CPUID_CALL_MAGIC
    mov eax, LMV_CPUID_CALL_LEAF
    xor ecx, ecx
    cpuid
    test rsi, rsi
    jz short LmvGuestCpuidCallDone
    mov dword ptr [rsi], eax
    mov dword ptr [rsi + 4], ebx
    mov dword ptr [rsi + 8], ecx
    mov dword ptr [rsi + 0Ch], edx
LmvGuestCpuidCallDone:
    pop r15
    pop r14
    pop r13
    pop rsi
    pop rbx
    ret
LmvGuestCpuidCall ENDP

LmvGuestQuerySharedPage PROC
    push rbx
    push rsi
    push r13
    push r14
    push r15
    mov rsi, rcx
    mov r13d, 2
    mov r14d, 3
    mov r15, LMV_CPUID_CALL_MAGIC
    mov eax, LMV_CPUID_CALL_LEAF
    xor ecx, ecx
    cpuid
    test rsi, rsi
    jz short LmvGuestQuerySharedPageDone
    mov dword ptr [rsi], eax
    mov dword ptr [rsi + 4], ebx
    mov dword ptr [rsi + 8], ecx
    mov dword ptr [rsi + 0Ch], edx
LmvGuestQuerySharedPageDone:
    pop r15
    pop r14
    pop r13
    pop rsi
    pop rbx
    ret
LmvGuestQuerySharedPage ENDP

END
