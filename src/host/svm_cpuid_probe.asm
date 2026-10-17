; svm_cpuid_probe.asm - Assembly helper for detecting Hypervisor with magic CPUID

OPTION CASEMAP:NONE

LMV_CPUID_CALL_LEAF EQU 04C4D0001h
LMV_CPUID_CALL_MAGIC EQU 04C4D5654434D414Ch

.code

;
; SvCpuidProbeWithMagic - Call CPUID with magic value to detect Hypervisor
;
; Returns EAX register value from CPUID call
;
SvCpuidProbeWithMagic PROC
    push r15
    push rbx
    mov r15, LMV_CPUID_CALL_MAGIC
    mov eax, LMV_CPUID_CALL_LEAF
    xor ecx, ecx
    cpuid
    ; EAX already contains return value
    pop rbx
    pop r15
    ret
SvCpuidProbeWithMagic ENDP

END
