// 综合反虚拟化检测 - 模拟 EasyAntiCheat/BattlEye 的检测方法
// 编译: cl /O2 /EHsc test_anticheat_suite.cpp /Fe:anticheat_suite.exe

#include <Windows.h>
#include <stdio.h>
#include <intrin.h>
#include <vector>

#pragma intrinsic(__rdtsc)

struct DetectionResult {
    const char* test_name;
    BOOL detected;
    const char* reason;
};

std::vector<DetectionResult> g_results;

void AddResult(const char* name, BOOL detected, const char* reason) {
    DetectionResult result;
    result.test_name = name;
    result.detected = detected;
    result.reason = reason;
    g_results.push_back(result);
}

// ===== 检测 1: CPUID 功能位检测 =====
void Test_CPUID_Features() {
    printf("[1] CPUID Feature Detection\n");

    int regs[4];

    // 检查 Hypervisor Present 位
    __cpuid(regs, 1);
    if (regs[2] & (1 << 31)) {
        AddResult("CPUID.1:ECX[31]", TRUE, "Hypervisor Present bit is SET");
        printf("  [!] DETECTED: Hypervisor Present bit\n");
    } else {
        AddResult("CPUID.1:ECX[31]", FALSE, NULL);
        printf("  [OK] No hypervisor bit\n");
    }

    // 检查 SVM NPT 隐藏
    __cpuid(regs, 0x8000000A);
    if (regs[3] & 1) {
        AddResult("AMD NPT Hide", TRUE, "NPT feature is visible (should be hidden)");
        printf("  [!] DETECTED: NPT not hidden\n");
    } else {
        AddResult("AMD NPT Hide", FALSE, NULL);
        printf("  [OK] NPT hidden\n");
    }
}

// ===== 检测 2: 魔术 CPUID 叶扫描 =====
void Test_CPUID_Magic_Leaves() {
    printf("\n[2] Magic CPUID Leaf Scanning\n");

    int regs[4];
    BOOL found = FALSE;

    // 重点扫描可能的魔术叶区域
    UINT32 suspicious_ranges[] = {
        0x4C4D0000, 0x4C4E0000,  // "LM" prefix
        0x53564D00, 0x53564D100, // "SVM" prefix
        0x41414100, 0x41414200,  // SimpleSVM unload
    };

    for (int i = 0; i < sizeof(suspicious_ranges) / sizeof(UINT32); i += 2) {
        for (UINT32 leaf = suspicious_ranges[i]; leaf < suspicious_ranges[i + 1]; leaf++) {
            __try {
                __cpuid(regs, leaf);
                if (regs[0] != 0 || regs[1] != 0 || regs[2] != 0 || regs[3] != 0) {
                    printf("  [!] Found response at leaf 0x%08X: EAX=0x%08X\n", leaf, regs[0]);

                    // 检查特定签名
                    if (regs[0] == 0x53564D4C) {  // "SVML"
                        AddResult("Magic CPUID Leaf", TRUE, "Found LMvtdriver signature 'SVML'");
                        printf("      >>> CRITICAL: LMvtdriver detected!\n");
                        found = TRUE;
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    if (!found) {
        AddResult("Magic CPUID Leaf", FALSE, NULL);
        printf("  [OK] No magic CPUID leaves found\n");
    }
}

// ===== 检测 3: MSR 访问检测 =====
void Test_MSR_Access() {
    printf("\n[3] MSR Access Detection\n");
    printf("  [SKIP] MSR access requires kernel mode\n");
    printf("  (Would check VM_CR, VM_HSAVE_PA if in kernel)\n");
    AddResult("MSR Access", FALSE, "Skipped (requires kernel mode)");
}

// ===== 检测 4: 异常处理延迟（快速版）=====
void Test_Exception_Timing_Quick() {
    printf("\n[4] Exception Timing (Quick)\n");

    const int SAMPLES = 50;
    std::vector<UINT64> timings;

    for (int i = 0; i < SAMPLES; i++) {
        volatile int* null_ptr = nullptr;
        UINT64 start = __rdtsc();

        __try {
            *null_ptr = 0;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            UINT64 end = __rdtsc();
            timings.push_back(end - start);
        }
    }

    // 计算中位数
    std::sort(timings.begin(), timings.end());
    UINT64 median = timings[SAMPLES / 2];

    printf("  Median exception time: %llu cycles\n", median);

    if (median > 10000) {
        AddResult("Exception Timing", TRUE, "Very high delay (>10000 cycles)");
        printf("  [!] DETECTED: Abnormally high exception delay\n");
    } else if (median > 5000) {
        AddResult("Exception Timing", TRUE, "Elevated delay (>5000 cycles)");
        printf("  [!] WARNING: Elevated exception delay\n");
    } else {
        AddResult("Exception Timing", FALSE, NULL);
        printf("  [OK] Normal exception timing\n");
    }
}

// ===== 检测 5: 内存扫描检测 =====
void Test_Memory_Scanning() {
    printf("\n[5] Memory Scanning for Signatures\n");

    // 扫描当前进程内存找可疑字符串
    MEMORY_BASIC_INFORMATION mbi;
    UINT64 address = 0;
    BOOL found = FALSE;

    const char* signatures[] = {
        "LMvtdriver",
        "SimpleSvm",
        "SVML",
        "KiDispatchException",
        NULL
    };

    printf("  Scanning process memory for hypervisor signatures...\n");

    // 只扫描前 10GB（避免太慢）
    while (address < 0x280000000ULL) {
        if (VirtualQuery((LPCVOID)address, &mbi, sizeof(mbi)) == 0) {
            break;
        }

        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect == PAGE_READONLY || mbi.Protect == PAGE_READWRITE)) {

            __try {
                char* mem = (char*)mbi.BaseAddress;
                for (SIZE_T i = 0; i < mbi.RegionSize - 16; i++) {
                    for (int sig = 0; signatures[sig] != NULL; sig++) {
                        if (memcmp(&mem[i], signatures[sig], strlen(signatures[sig])) == 0) {
                            printf("  [!] Found signature '%s' at 0x%p\n",
                                   signatures[sig], (void*)(address + i));
                            found = TRUE;
                        }
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }

        address += mbi.RegionSize;

        // 限制扫描时间
        static int scan_count = 0;
        if (++scan_count > 100) break;
    }

    if (found) {
        AddResult("Memory Signatures", TRUE, "Found hypervisor strings in memory");
    } else {
        AddResult("Memory Signatures", FALSE, NULL);
        printf("  [OK] No suspicious signatures found\n");
    }
}

// ===== 检测 6: 进程/模块检测 =====
void Test_Process_Module_Detection() {
    printf("\n[6] Process/Module Detection\n");

    const wchar_t* suspicious_drivers[] = {
        L"\\\\.\\LMvtdriver",
        L"\\\\.\\LMvtdriverGuest",
        L"\\\\.\\SimpleSvm",
        NULL
    };

    BOOL found = FALSE;
    for (int i = 0; suspicious_drivers[i] != NULL; i++) {
        HANDLE h = CreateFileW(suspicious_drivers[i], GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            printf("  [!] DETECTED: Driver accessible: %S\n", suspicious_drivers[i]);
            CloseHandle(h);
            found = TRUE;
        }
    }

    if (found) {
        AddResult("Driver Detection", TRUE, "Found hypervisor driver");
    } else {
        AddResult("Driver Detection", FALSE, NULL);
        printf("  [OK] No suspicious drivers found\n");
    }
}

// ===== 检测 7: INT3 指令特征 =====
void Test_INT3_Behavior() {
    printf("\n[7] INT3 Breakpoint Behavior\n");

    // 测试 INT3 是否被拦截
    UINT64 start = __rdtsc();
    __try {
        __debugbreak();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        UINT64 end = __rdtsc();

        if (end - start > 5000) {
            AddResult("INT3 Timing", TRUE, "INT3 exception has high delay");
            printf("  [!] WARNING: INT3 delay is high (%llu cycles)\n", end - start);
        } else {
            AddResult("INT3 Timing", FALSE, NULL);
            printf("  [OK] INT3 timing normal (%llu cycles)\n", end - start);
        }
    }
}

// ===== 检测 8: 系统调用钩子检测 =====
void Test_Syscall_Hooks() {
    printf("\n[8] System Call Hook Detection\n");

    // 快速测试几个关键 API
    const char* apis[] = {
        "NtQuerySystemInformation",
        "NtSetInformationThread",
        "NtQueryInformationProcess",
        NULL
    };

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    BOOL suspicious = FALSE;

    for (int i = 0; apis[i] != NULL; i++) {
        FARPROC proc = GetProcAddress(ntdll, apis[i]);
        if (proc) {
            BYTE* code = (BYTE*)proc;

            // 检查是否以 JMP 或 CALL 开头（inline hook）
            if (code[0] == 0xE9 || code[0] == 0xE8 || code[0] == 0xFF) {
                printf("  [!] WARNING: %s may be hooked (opcode: 0x%02X)\n",
                       apis[i], code[0]);
                suspicious = TRUE;
            }
        }
    }

    if (suspicious) {
        AddResult("Syscall Hooks", TRUE, "Detected potential inline hooks");
    } else {
        AddResult("Syscall Hooks", FALSE, NULL);
        printf("  [OK] No obvious syscall hooks\n");
    }
}

// ===== 打印最终报告 =====
void PrintFinalReport() {
    printf("\n");
    printf("==============================================\n");
    printf("       FINAL DETECTION REPORT\n");
    printf("==============================================\n\n");

    int total = g_results.size();
    int detected = 0;

    for (const auto& result : g_results) {
        if (result.detected) {
            detected++;
            printf("  [!] %s\n", result.test_name);
            if (result.reason) {
                printf("      → %s\n", result.reason);
            }
        }
    }

    printf("\n----------------------------------------------\n");
    printf("  Detection Score: %d / %d tests flagged\n", detected, total);
    printf("----------------------------------------------\n\n");

    if (detected >= 3) {
        printf("  🚨 VERDICT: HYPERVISOR DETECTED\n");
        printf("     Confidence: HIGH\n");
        printf("     Multiple detection methods agree.\n");
    } else if (detected >= 1) {
        printf("  ⚠️  VERDICT: SUSPICIOUS\n");
        printf("     Confidence: MEDIUM\n");
        printf("     Some indicators present.\n");
    } else {
        printf("  ✅ VERDICT: CLEAN\n");
        printf("     No hypervisor detected.\n");
    }

    printf("\n==============================================\n");
}

int main()
{
    printf("==============================================\n");
    printf("   Anti-Cheat Detection Suite v1.0\n");
    printf("   Simulating EasyAntiCheat/BattlEye\n");
    printf("==============================================\n\n");

    Test_CPUID_Features();
    Test_CPUID_Magic_Leaves();
    Test_MSR_Access();
    Test_Exception_Timing_Quick();
    Test_Memory_Scanning();
    Test_Process_Module_Detection();
    Test_INT3_Behavior();
    Test_Syscall_Hooks();

    PrintFinalReport();

    printf("\nPress any key to exit...\n");
    system("pause >nul");
    return 0;
}
