OPTION CASEMAP:NONE

LMVPROBE SEGMENT ALIGN(4096) READ EXECUTE 'CODE'

PUBLIC LmvGuestKernelProbeOriginal
PUBLIC LmvGuestKernelProbeNonEntry
PUBLIC LmvGuestKernelProbeMirror

LmvGuestKernelProbeOriginal PROC
    lea rax, [rcx + 1]
    ret
LmvGuestKernelProbeOriginal ENDP

DB (100h - ($ - LmvGuestKernelProbeOriginal)) DUP (0C3h)
LmvGuestKernelProbeNonEntry PROC
    mov eax, 33h
    ret
LmvGuestKernelProbeNonEntry ENDP
DB (1000h - ($ - LmvGuestKernelProbeOriginal)) DUP (0C3h)

LmvGuestKernelProbeMirror PROC
    lea rax, [rcx + 1]
    ret
LmvGuestKernelProbeMirror ENDP
DB (100h - ($ - LmvGuestKernelProbeMirror)) DUP (0C3h)
    mov eax, 33h
    ret
DB (1000h - ($ - LmvGuestKernelProbeMirror)) DUP (0C3h)

LMVPROBE ENDS

END