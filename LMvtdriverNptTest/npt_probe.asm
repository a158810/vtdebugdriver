OPTION CASEMAP:NONE

LMV_NPT_TEST_CPUID EQU 04C4D4E50h
LMV_CPUID_CALL_LEAF EQU 04C4D0001h
LMV_CPUID_CALL_MAGIC EQU 04C4D5654434D414Ch

.code

LmvNptProbe PROC
    push rbx
    push rsi

    mov rsi, r8
    mov r9d, edx
    mov r8, rcx
    mov eax, LMV_NPT_TEST_CPUID
    mov rbx, r8
    shr r8, 32
    mov ecx, r8d
    mov edx, r9d
    cpuid

    mov dword ptr [rsi], eax
    mov dword ptr [rsi + 4], ebx
    mov dword ptr [rsi + 8], ecx
    mov dword ptr [rsi + 0Ch], edx

    pop rsi
    pop rbx
    ret
LmvNptProbe ENDP

.code

LmvCpuidCall PROC
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
    jz short LmvCpuidCallDone
    mov dword ptr [rsi], eax
    mov dword ptr [rsi + 4], ebx
    mov dword ptr [rsi + 8], ecx
    mov dword ptr [rsi + 0Ch], edx

LmvCpuidCallDone:
    pop r15
    pop r14
    pop r13
    pop rsi
    pop rbx
    ret
LmvCpuidCall ENDP

.code

LmvCpuidQuerySharedPage PROC
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
    jz short LmvCpuidQuerySharedPageDone
    mov dword ptr [rsi], eax
    mov dword ptr [rsi + 4], ebx
    mov dword ptr [rsi + 8], ecx
    mov dword ptr [rsi + 0Ch], edx

LmvCpuidQuerySharedPageDone:
    pop r15
    pop r14
    pop r13
    pop rsi
    pop rbx
    ret
LmvCpuidQuerySharedPage ENDP

END
