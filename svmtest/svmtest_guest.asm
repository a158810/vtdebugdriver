OPTION CASEMAP:NONE

SVMTEST_MSR_EFER EQU 0C0000080h
SVMTEST_MSR_VM_CR EQU 0C0010114h
SVMTEST_MSR_VM_HSAVE_PA EQU 0C0010117h
SVMTEST_CPUID_LEAF EQU 53564D54h
SVMTEST_CPUID_MARKER EQU 43505549h
SVMTEST_CPUID_RESULT_MARKER EQU 43505254h
SVMTEST_L1_HANDLER_MARKER EQU 4C314841h
SVMTEST_L2_RESUME_MARKER EQU 4C325253h

.code

PUBLIC SvmTestL2CodeBegin
PUBLIC SvmTestL2NestedCpuidSetup
PUBLIC SvmTestL2NestedCpuid
PUBLIC SvmTestL2NestedCpuidContinue
PUBLIC SvmTestL2NestedCpuidVmmcall
PUBLIC SvmTestL2Vmmcall
PUBLIC SvmTestL2CpuidSetup
PUBLIC SvmTestL2Cpuid
PUBLIC SvmTestL2MsrSetup
PUBLIC SvmTestL2Msr
PUBLIC SvmTestL2MsrWriteSetup
PUBLIC SvmTestL2MsrWrite
PUBLIC SvmTestL2Breakpoint
PUBLIC SvmTestL2Exception
PUBLIC SvmTestL2NestedVmrunSetup
PUBLIC SvmTestL2NestedVmrun
PUBLIC SvmTestL2NestedVmrunInstruction
PUBLIC SvmTestL2BreakpointHandler
PUBLIC SvmTestL2GeneralProtectionHandler
PUBLIC SvmTestL2VmmcallCompletion
PUBLIC SvmTestL2Halt
PUBLIC SvmTestL2CodeEnd
PUBLIC SvmTestL2FullNestedEntry
PUBLIC SvmTestL2FullNestedCpuid
PUBLIC SvmTestL2FullNestedResume
PUBLIC SvmTestL2FullNestedComplete

SvmTestL2CodeBegin LABEL BYTE


    mov dword ptr [rdi], 1
    mov dword ptr [rdi + 4], 053564D54h
    mov rax, 053564D5445535431h
    mov qword ptr [rdi + 8], rax

SvmTestL2Vmmcall LABEL BYTE
    vmmcall

SvmTestL2CpuidSetup LABEL BYTE
    mov dword ptr [rdi], 2
    mov dword ptr [rdi + 16], 1
    mov eax, 1
    xor ecx, ecx
SvmTestL2Cpuid LABEL BYTE
    cpuid

SvmTestL2MsrSetup LABEL BYTE
    mov dword ptr [rdi], 3
    mov dword ptr [rdi + 20], 1
    mov ecx, SVMTEST_MSR_EFER
SvmTestL2Msr LABEL BYTE
    rdmsr
SvmTestL2MsrWriteSetup LABEL BYTE
    mov dword ptr [rdi], 4
    mov ecx, SVMTEST_MSR_EFER
    mov rax, qword ptr [rdi + 48]
    mov rdx, qword ptr [rdi + 56]
SvmTestL2MsrWrite LABEL BYTE
    wrmsr

SvmTestL2Breakpoint LABEL BYTE
    mov dword ptr [rdi], 4
SvmTestL2Exception LABEL BYTE
    ud2
SvmTestL2VmmcallCompletion LABEL BYTE
    vmmcall

SvmTestL2NestedVmrunSetup LABEL BYTE
SvmTestL2NestedVmrun LABEL BYTE
    mov dword ptr [rdi], 5
    mov rax, qword ptr [rdi + 40]
SvmTestL2NestedVmrunInstruction LABEL BYTE
    vmrun rax
    vmmcall

SvmTestL2NestedCpuidSetup LABEL BYTE
    mov dword ptr [rdi], 1
    mov dword ptr [rdi + 4], 053564D54h
    mov rax, 053564D5445535431h
    mov qword ptr [rdi + 8], rax
    mov dword ptr [rdi], 6
    mov dword ptr [rdi + 16], SVMTEST_CPUID_MARKER
    mov dword ptr [rdi + 32], 0
    mov eax, SVMTEST_CPUID_LEAF
    xor ecx, ecx
SvmTestL2NestedCpuid LABEL BYTE
    cpuid
SvmTestL2NestedCpuidContinue LABEL BYTE
    mov dword ptr [rdi + 16], SVMTEST_CPUID_RESULT_MARKER
    mov dword ptr [rdi + 32], eax
SvmTestL2NestedCpuidVmmcall LABEL BYTE
    vmmcall

SvmTestL2Halt PROC
    cli
SvmTestL2HaltLoop:
    jmp short SvmTestL2HaltLoop
SvmTestL2Halt ENDP

SvmTestL2BreakpointHandler PROC
    mov dword ptr [rdi + 24], 1
    add qword ptr [rsp], 2
    iretq
SvmTestL2BreakpointHandler ENDP

SvmTestL2GeneralProtectionHandler PROC
    mov dword ptr [rdi + 28], 1
    add qword ptr [rsp + 8], 3
    add rsp, 8
    iretq
SvmTestL2GeneralProtectionHandler ENDP

; Full nested test: L2 code that will trigger reflection to L1
SvmTestL2FullNestedEntry LABEL BYTE
    ; Setup initial state
    mov dword ptr [rdi], 1
    mov dword ptr [rdi + 4], 053564D54h
    mov rax, 053564D5445535431h
    mov qword ptr [rdi + 8], rax

    ; Prepare for CPUID that will be reflected to L1
    mov dword ptr [rdi + 64], 0  ; L1HandlerObservedMarker offset
    mov dword ptr [rdi + 68], 0  ; L1HandlerObservedRax offset
    mov dword ptr [rdi + 72], 0  ; L1HandlerObservedRbx offset
    mov dword ptr [rdi + 76], 0  ; L1HandlerObservedRcx offset
    mov dword ptr [rdi + 80], 0  ; L2ResumeMarker offset

    ; Set marker registers for L1 to observe
    mov ebx, 012345678h
    mov ecx, 087654321h
    mov eax, SVMTEST_CPUID_LEAF

SvmTestL2FullNestedCpuid LABEL BYTE
    ; This CPUID will trigger VMEXIT that L0 reflects to L1
    cpuid

SvmTestL2FullNestedResume LABEL BYTE
    ; After L1 handler processes and re-VMRUNs, we resume here
    ; Check if we see the modified EAX from L1 handler
    mov dword ptr [rdi + 80], SVMTEST_L2_RESUME_MARKER
    mov dword ptr [rdi + 32], eax  ; Store the EAX we received

SvmTestL2FullNestedComplete LABEL BYTE
    ; Signal completion
    vmmcall

; MSR Transparency Test: Tests if MSR virtualization is transparent
PUBLIC SvmTestL2MsrTransparencyEntry
PUBLIC SvmTestL2MsrTransparencyReadEfer
PUBLIC SvmTestL2MsrTransparencyWriteEfer
PUBLIC SvmTestL2MsrTransparencyReadEferBack
PUBLIC SvmTestL2MsrTransparencyReadVmCr
PUBLIC SvmTestL2MsrTransparencyWriteVmCr
PUBLIC SvmTestL2MsrTransparencyReadVmCrBack
PUBLIC SvmTestL2MsrTransparencyReadVmHsave
PUBLIC SvmTestL2MsrTransparencyWriteVmHsave
PUBLIC SvmTestL2MsrTransparencyReadVmHsaveBack
PUBLIC SvmTestL2MsrTransparencyComplete

SVMTEST_MSR_VM_CR EQU 0C0010114h
SVMTEST_MSR_VM_HSAVE_PA EQU 0C0010117h

SvmTestL2MsrTransparencyEntry LABEL BYTE
    ; Initialize test markers
    mov dword ptr [rdi], 1
    mov dword ptr [rdi + 4], 053564D54h

    ; Test 1: Read EFER
SvmTestL2MsrTransparencyReadEfer LABEL BYTE
    mov ecx, SVMTEST_MSR_EFER
    rdmsr
    ; Store initial EFER value at offset 84 (MsrEferInitial)
    mov dword ptr [rdi + 84], eax
    mov dword ptr [rdi + 88], edx

    ; Test 2: Write EFER (clear a non-critical bit if set, or set SCE)
SvmTestL2MsrTransparencyWriteEfer LABEL BYTE
    mov ecx, SVMTEST_MSR_EFER
    mov eax, dword ptr [rdi + 84]
    mov edx, dword ptr [rdi + 88]
    xor eax, 1  ; Toggle SCE bit
    wrmsr
    ; Store written value at offset 92 (MsrEferAfterWrite)
    mov dword ptr [rdi + 92], eax
    mov dword ptr [rdi + 96], edx

    ; Test 3: Read EFER back
SvmTestL2MsrTransparencyReadEferBack LABEL BYTE
    mov ecx, SVMTEST_MSR_EFER
    rdmsr
    ; Store read-back value at offset 100 (MsrEferReadBack)
    mov dword ptr [rdi + 100], eax
    mov dword ptr [rdi + 104], edx

    ; Test 4: Read VM_CR
SvmTestL2MsrTransparencyReadVmCr LABEL BYTE
    mov ecx, SVMTEST_MSR_VM_CR
    rdmsr
    ; Store initial VM_CR at offset 108 (MsrVmCrInitial)
    mov dword ptr [rdi + 108], eax
    mov dword ptr [rdi + 112], edx

    ; Test 5: Write VM_CR (should allow writing legal values)
SvmTestL2MsrTransparencyWriteVmCr LABEL BYTE
    mov ecx, SVMTEST_MSR_VM_CR
    xor eax, eax  ; Write 0
    xor edx, edx
    wrmsr
    ; Store written value at offset 116 (MsrVmCrAfterWrite)
    mov dword ptr [rdi + 116], eax
    mov dword ptr [rdi + 120], edx

    ; Test 6: Read VM_CR back
SvmTestL2MsrTransparencyReadVmCrBack LABEL BYTE
    mov ecx, SVMTEST_MSR_VM_CR
    rdmsr
    ; Store read-back value at offset 124 (MsrVmCrReadBack)
    mov dword ptr [rdi + 124], eax
    mov dword ptr [rdi + 128], edx

    ; Test 7: Read VM_HSAVE_PA
SvmTestL2MsrTransparencyReadVmHsave LABEL BYTE
    mov ecx, SVMTEST_MSR_VM_HSAVE_PA
    rdmsr
    ; Store initial VM_HSAVE_PA at offset 132 (MsrVmHsavePaInitial)
    mov dword ptr [rdi + 132], eax
    mov dword ptr [rdi + 136], edx

    ; Test 8: Write VM_HSAVE_PA (write a test page-aligned address)
SvmTestL2MsrTransparencyWriteVmHsave LABEL BYTE
    mov ecx, SVMTEST_MSR_VM_HSAVE_PA
    mov eax, 012340000h  ; Page-aligned test address
    xor edx, edx
    wrmsr
    ; Store written value at offset 140 (MsrVmHsavePaAfterWrite)
    mov dword ptr [rdi + 140], eax
    mov dword ptr [rdi + 144], edx

    ; Test 9: Read VM_HSAVE_PA back
SvmTestL2MsrTransparencyReadVmHsaveBack LABEL BYTE
    mov ecx, SVMTEST_MSR_VM_HSAVE_PA
    rdmsr
    ; Store read-back value at offset 148 (MsrVmHsavePaReadBack)
    mov dword ptr [rdi + 148], eax
    mov dword ptr [rdi + 152], edx

SvmTestL2MsrTransparencyComplete LABEL BYTE
    ; Signal completion
    vmmcall

SvmTestL2CodeEnd LABEL BYTE

END
