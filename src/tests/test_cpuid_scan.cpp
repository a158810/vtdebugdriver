// CPUID 枚举检测 - 模拟反作弊检测
// 编译: cl /O2 /EHsc test_cpuid_scan.cpp /Fe:cpuid_scan.exe

#include <Windows.h>
#include <stdio.h>
#include <intrin.h>
#include <vector>

struct SuspiciousLeaf {
    UINT32 leaf;
    int regs[4];
};

int main()
{
    printf("=== CPUID Hypervisor Detection Scan ===\n");
    printf("Scanning CPUID leaves 0x40000000 - 0x50000000\n\n");

    std::vector<SuspiciousLeaf> suspicious;

    // 1. 标准 Hypervisor 接口检测
    printf("[1] Checking standard hypervisor interface (0x40000000)...\n");
    int regs[4];
    __cpuid(regs, 0x40000000);

    if (regs[0] != 0 || regs[1] != 0 || regs[2] != 0 || regs[3] != 0) {
        printf("  [!] DETECTED: Hypervisor present bit or vendor string found!\n");
        printf("      EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
               regs[0], regs[1], regs[2], regs[3]);
        char vendor[13] = {0};
        memcpy(vendor, &regs[1], 12);
        printf("      Vendor: %s\n", vendor);
    } else {
        printf("  [OK] No standard hypervisor interface\n");
    }

    // 2. CPUID leaf 1 - Hypervisor Present 位检测
    printf("\n[2] Checking CPUID.1:ECX bit 31 (Hypervisor Present)...\n");
    __cpuid(regs, 1);
    if (regs[2] & (1 << 31)) {
        printf("  [!] DETECTED: Hypervisor Present bit is SET!\n");
    } else {
        printf("  [OK] Hypervisor Present bit is clear\n");
    }

    // 3. 暴力枚举可疑叶（EasyAntiCheat 的做法）
    printf("\n[3] Brute-force scanning 0x40000000-0x4FFFFFFF (this may take a while)...\n");

    for (UINT32 leaf = 0x40000000; leaf < 0x50000000; leaf += 0x1000) {
        __try {
            __cpuid(regs, leaf);

            // 如果返回非零值，记录
            if (regs[0] != 0 || regs[1] != 0 || regs[2] != 0 || regs[3] != 0) {
                SuspiciousLeaf sl;
                sl.leaf = leaf;
                memcpy(sl.regs, regs, sizeof(regs));
                suspicious.push_back(sl);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // 某些叶可能触发异常，忽略
        }
    }

    // 4. 精确扫描可疑区域
    printf("\n[4] Fine-grained scan around suspicious areas...\n");

    // 扫描 "LMVT" 区域 (0x4C4D0000 - 0x4C4E0000)
    for (UINT32 leaf = 0x4C4D0000; leaf < 0x4C4E0000; leaf++) {
        __try {
            __cpuid(regs, leaf);
            if (regs[0] != 0 || regs[1] != 0 || regs[2] != 0 || regs[3] != 0) {
                SuspiciousLeaf sl;
                sl.leaf = leaf;
                memcpy(sl.regs, regs, sizeof(regs));
                suspicious.push_back(sl);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // 5. 报告结果
    printf("\n[5] RESULTS:\n");
    if (suspicious.empty()) {
        printf("  [OK] No suspicious CPUID leaves detected\n");
    } else {
        printf("  [!] DETECTED %zu suspicious CPUID leaf(s):\n", suspicious.size());
        for (const auto& sl : suspicious) {
            printf("      Leaf 0x%08X: EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
                   sl.leaf, sl.regs[0], sl.regs[1], sl.regs[2], sl.regs[3]);

            // 检查是否是你的魔术值
            if (sl.regs[0] == 0x53564D4C) {  // "SVML"
                printf("      >>> CRITICAL: Detected LMvtdriver signature!\n");
            }
        }
    }

    // 6. AMD SVM 特性检测
    printf("\n[6] Checking AMD SVM features...\n");
    __cpuid(regs, 0x80000001);
    if (regs[2] & (1 << 2)) {  // SVM bit
        printf("  [INFO] SVM feature is present\n");

        __cpuid(regs, 0x8000000A);
        printf("  [INFO] SVM Revision: %d\n", regs[0] & 0xFF);
        printf("  [INFO] Number of ASIDs: %d\n", regs[1]);

        // 检查 NPT 支持
        if (regs[3] & 1) {
            printf("  [!] WARNING: NPT is reported as SUPPORTED (should be hidden!)\n");
        } else {
            printf("  [OK] NPT is hidden\n");
        }
    }

    printf("\n=== Scan Complete ===\n");

    system("pause");
    return 0;
}
