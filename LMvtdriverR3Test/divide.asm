OPTION CASEMAP:NONE
PUBLIC LmvTestDivide
PUBLIC LmvTestDivideFault
.code
LmvTestDivide PROC
    mov eax, ecx
    mov ecx, edx
    xor edx, edx
LmvTestDivideFault LABEL BYTE
    div ecx
    ret
LmvTestDivide ENDP
END
