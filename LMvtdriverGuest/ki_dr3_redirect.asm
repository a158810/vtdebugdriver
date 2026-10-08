OPTION CASEMAP:NONE

EXTERN g_LmvGuestKiDr3ReturnRip:QWORD
EXTERN g_LmvGuestKiDr3StubHits:QWORD
EXTERN LmvGuestKiDr3InspectException:PROC
EXTERN g_LmvGuestKiSessionResumeRip:QWORD
EXTERN LmvGuestKiSessionException:PROC
PUBLIC LmvGuestKiDr3RedirectStub
PUBLIC LmvGuestKiDr3InspectStub
PUBLIC LmvGuestKiSessionEventStub
PUBLIC LmvGuestKiSessionPassStub
PUBLIC LmvGuestKiCaptureExit
PUBLIC LmvGuestKiCommitExit
PUBLIC LmvGuestKiPassExit

.code

LmvGuestKiDr3RedirectStub PROC
    pushfq
    lock inc qword ptr [g_LmvGuestKiDr3StubHits]
    popfq
    jmp qword ptr [g_LmvGuestKiDr3ReturnRip]
LmvGuestKiDr3RedirectStub ENDP
KI_INSPECT_BODY MACRO handler, handledLabel, persistent
    pushfq
    .allocstack 8
    push rax
    .allocstack 8
    push rcx
    .allocstack 8
    push rdx
    .allocstack 8
    push rbx
    .pushreg rbx
    push rbp
    .pushreg rbp
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r8
    .allocstack 8
    push r9
    .allocstack 8
    push r10
    .allocstack 8
    push r11
    .allocstack 8
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    sub rsp, 98h
    .allocstack 98h
    .endprolog
    movaps xmmword ptr [rsp + 30h], xmm0
    movaps xmmword ptr [rsp + 40h], xmm1
    movaps xmmword ptr [rsp + 50h], xmm2
    movaps xmmword ptr [rsp + 60h], xmm3
    movaps xmmword ptr [rsp + 70h], xmm4
    movaps xmmword ptr [rsp + 80h], xmm5
    lock inc qword ptr [g_LmvGuestKiDr3StubHits]
    movzx eax, byte ptr [rsp + 140h]
    mov [rsp + 20h], rax
    mov rcx, [rsp + 100h]
    mov rdx, [rsp + 0F8h]
    mov r8, [rsp + 0D0h]
    movzx r9d, byte ptr [rsp + 0C8h]
    call handler
    movaps xmm0, xmmword ptr [rsp + 30h]
    movaps xmm1, xmmword ptr [rsp + 40h]
    movaps xmm2, xmmword ptr [rsp + 50h]
    movaps xmm3, xmmword ptr [rsp + 60h]
    movaps xmm4, xmmword ptr [rsp + 70h]
    movaps xmm5, xmmword ptr [rsp + 80h]
    add rsp, 98h
    test al, al
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
    jnz handledLabel
    popfq
    IF persistent
    jmp LmvGuestKiSessionPassStub
    ELSE
    jmp qword ptr [g_LmvGuestKiDr3ReturnRip]
    ENDIF
handledLabel:
    popfq
    IF persistent
    jmp LmvGuestKiCaptureExit
    ELSE
    ret
    ENDIF
ENDM

LmvGuestKiDr3InspectStub PROC FRAME
    KI_INSPECT_BODY LmvGuestKiDr3InspectException, ki_dr3_handled, 0
LmvGuestKiDr3InspectStub ENDP

LmvGuestKiSessionEventStub PROC FRAME
    KI_INSPECT_BODY LmvGuestKiSessionException, ki_session_handled, 1
LmvGuestKiSessionEventStub ENDP

LmvGuestKiSessionPassStub PROC
    push rbp
LmvGuestKiPassExit LABEL BYTE
    vmmcall
    ud2
LmvGuestKiSessionPassStub ENDP

LmvGuestKiCaptureExit PROC
    vmmcall
    pop rax
LmvGuestKiCommitExit LABEL BYTE
    vmmcall
    ud2
LmvGuestKiCaptureExit ENDP

END
