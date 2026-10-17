#include <stdio.h>
#include <intrin.h>

int main() {
    int regs[4] = {0};
    char vendor[13] = {0};

    // 查询 CPUID leaf 0x1
    __cpuid(regs, 1);
    printf("CPUID 0x1:\n");
    printf("  ECX = 0x%08X\n", regs[2]);
    printf("  Hypervisor bit (bit 31) = %d\n", (regs[2] >> 31) & 1);
    printf("\n");

    // 查询 CPUID leaf 0x40000000
    __cpuid(regs, 0x40000000);
    printf("CPUID 0x40000000:\n");
    printf("  EAX = 0x%08X\n", regs[0]);
    printf("  EBX = 0x%08X\n", regs[1]);
    printf("  ECX = 0x%08X\n", regs[2]);
    printf("  EDX = 0x%08X\n", regs[3]);

    memcpy(vendor + 0, &regs[1], 4);
    memcpy(vendor + 4, &regs[2], 4);
    memcpy(vendor + 8, &regs[3], 4);
    vendor[12] = '\0';

    printf("  Vendor = \"%s\"\n", vendor);

    return 0;
}
