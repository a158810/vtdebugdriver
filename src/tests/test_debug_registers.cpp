// DR 寄存器劫持检测 - 检测调试寄存器是否被 Hypervisor 控制
// 编译: cl /O2 /EHsc test_debug_registers.cpp /Fe:debug_registers.exe
// 注意: 需要管理员权限运行

#include <Windows.h>
#include <stdio.h>
#include <intrin.h>

// 用于内核驱动通信（如果需要读取真实 DR 值）
typedef struct _DEBUG_REGISTERS {
    UINT64 Dr0;
    UINT64 Dr1;
    UINT64 Dr2;
    UINT64 Dr3;
    UINT64 Dr6;
    UINT64 Dr7;
} DEBUG_REGISTERS;

// 测试 1：用户态 DR 读写测试
BOOL TestUserModeDR() {
    printf("[Test 1] User-mode Debug Register Access\n");

    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    HANDLE hThread = GetCurrentThread();
    if (!GetThreadContext(hThread, &ctx)) {
        printf("  [ERROR] GetThreadContext failed: %d\n", GetLastError());
        return FALSE;
    }

    printf("  Current DR values:\n");
    printf("    DR0 = 0x%016llX\n", ctx.Dr0);
    printf("    DR1 = 0x%016llX\n", ctx.Dr1);
    printf("    DR2 = 0x%016llX\n", ctx.Dr2);
    printf("    DR3 = 0x%016llX\n", ctx.Dr3);
    printf("    DR6 = 0x%016llX\n", ctx.Dr6);
    printf("    DR7 = 0x%016llX\n", ctx.Dr7);

    // 检查 DR7 的 enable 位
    BOOL dr0_enabled = (ctx.Dr7 & 0x01) != 0;
    BOOL dr1_enabled = (ctx.Dr7 & 0x04) != 0;
    BOOL dr2_enabled = (ctx.Dr7 & 0x10) != 0;
    BOOL dr3_enabled = (ctx.Dr7 & 0x40) != 0;

    printf("\n  DR Enable Status:\n");
    printf("    DR0: %s\n", dr0_enabled ? "ENABLED" : "Disabled");
    printf("    DR1: %s\n", dr1_enabled ? "ENABLED" : "Disabled");
    printf("    DR2: %s\n", dr2_enabled ? "ENABLED" : "Disabled");
    printf("    DR3: %s\n", dr3_enabled ? "ENABLED" : "Disabled");

    // 检测异常
    if (dr3_enabled && ctx.Dr3 != 0) {
        printf("\n  [!] WARNING: DR3 is enabled and non-zero!\n");
        printf("      DR3 = 0x%016llX\n", ctx.Dr3);
        printf("      This could indicate a kernel debugger or hypervisor hook!\n");
        return TRUE;
    }

    if (dr2_enabled && ctx.Dr2 != 0) {
        printf("\n  [!] WARNING: DR2 is enabled and non-zero!\n");
        printf("      DR2 = 0x%016llX\n", ctx.Dr2);
        return TRUE;
    }

    printf("\n  [OK] No suspicious DR usage detected\n");
    return FALSE;
}

// 测试 2：DR 写入一致性测试
BOOL TestDRWriteConsistency() {
    printf("\n[Test 2] Debug Register Write Consistency\n");

    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE hThread = GetCurrentThread();

    // 读取原始值
    if (!GetThreadContext(hThread, &ctx)) {
        printf("  [ERROR] GetThreadContext failed\n");
        return FALSE;
    }

    UINT64 original_dr3 = ctx.Dr3;
    UINT64 original_dr7 = ctx.Dr7;

    // 尝试写入测试值
    UINT64 test_value = 0x12345678ABCDEF00ULL;
    printf("  Attempting to write DR3 = 0x%016llX\n", test_value);

    ctx.Dr3 = test_value;
    ctx.Dr7 = original_dr7 & ~0x40ULL;  // 确保 DR3 禁用

    if (!SetThreadContext(hThread, &ctx)) {
        printf("  [ERROR] SetThreadContext failed: %d\n", GetLastError());
        return FALSE;
    }

    // 重新读取验证
    ctx.Dr3 = 0;
    if (!GetThreadContext(hThread, &ctx)) {
        printf("  [ERROR] GetThreadContext failed\n");
        return FALSE;
    }

    printf("  Read back DR3 = 0x%016llX\n", ctx.Dr3);

    BOOL mismatch = FALSE;
    if (ctx.Dr3 != test_value) {
        printf("  [!] WARNING: DR3 write/read mismatch!\n");
        printf("      Written: 0x%016llX\n", test_value);
        printf("      Read:    0x%016llX\n", ctx.Dr3);
        printf("      This indicates DR3 is being intercepted!\n");
        mismatch = TRUE;
    } else {
        printf("  [OK] DR3 write/read consistent\n");
    }

    // 恢复原始值
    ctx.Dr3 = original_dr3;
    ctx.Dr7 = original_dr7;
    SetThreadContext(hThread, &ctx);

    return mismatch;
}

// 测试 3：触发硬件断点测试 DR 响应
BOOL TestHardwareBreakpointTrigger() {
    printf("\n[Test 3] Hardware Breakpoint Trigger Test\n");

    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE hThread = GetCurrentThread();

    if (!GetThreadContext(hThread, &ctx)) {
        printf("  [ERROR] GetThreadContext failed\n");
        return FALSE;
    }

    // 设置 DR0 断点在一个测试变量上
    volatile int test_var = 0;
    ctx.Dr0 = (UINT64)&test_var;
    ctx.Dr7 = 0x00000001;  // DR0 enabled, write breakpoint, 4 bytes

    printf("  Setting DR0 breakpoint at address 0x%p\n", &test_var);

    if (!SetThreadContext(hThread, &ctx)) {
        printf("  [ERROR] SetThreadContext failed\n");
        return FALSE;
    }

    // 尝试触发断点
    BOOL triggered = FALSE;
    __try {
        test_var = 42;  // 应该触发 DR0 断点
        printf("  [?] Write did not trigger breakpoint (unexpected)\n");
    }
    __except (GetExceptionCode() == EXCEPTION_SINGLE_STEP ?
              EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        printf("  [OK] Hardware breakpoint triggered as expected\n");
        triggered = TRUE;
    }

    // 清除断点
    ctx.Dr0 = 0;
    ctx.Dr7 = 0;
    SetThreadContext(hThread, &ctx);

    return !triggered;  // 如果没触发，说明 DR 被拦截
}

// 测试 4：尝试读取内核态 DR（需要驱动）
void TestKernelDR() {
    printf("\n[Test 4] Kernel Debug Register Detection\n");
    printf("  This test requires a kernel driver to read real DR values.\n");
    printf("  User-mode can only see virtualized values.\n");
    printf("  [SKIP] Not implemented in user-mode test\n");
}

// 测试 5：DR6 状态检查
BOOL TestDR6Status() {
    printf("\n[Test 5] DR6 Debug Status Register Check\n");

    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE hThread = GetCurrentThread();

    if (!GetThreadContext(hThread, &ctx)) {
        printf("  [ERROR] GetThreadContext failed\n");
        return FALSE;
    }

    printf("  DR6 = 0x%016llX\n", ctx.Dr6);

    // 检查 DR6 的各个位
    BOOL b0 = (ctx.Dr6 & 0x01) != 0;
    BOOL b1 = (ctx.Dr6 & 0x02) != 0;
    BOOL b2 = (ctx.Dr6 & 0x04) != 0;
    BOOL b3 = (ctx.Dr6 & 0x08) != 0;
    BOOL bd = (ctx.Dr6 & 0x2000) != 0;
    BOOL bs = (ctx.Dr6 & 0x4000) != 0;
    BOOL bt = (ctx.Dr6 & 0x8000) != 0;

    printf("  Breakpoint hit flags:\n");
    printf("    B0 (DR0 hit): %d\n", b0);
    printf("    B1 (DR1 hit): %d\n", b1);
    printf("    B2 (DR2 hit): %d\n", b2);
    printf("    B3 (DR3 hit): %d\n", b3);
    printf("    BD (Debug register access): %d\n", bd);
    printf("    BS (Single step): %d\n", bs);
    printf("    BT (Task switch): %d\n", bt);

    if (b3) {
        printf("\n  [!] CRITICAL: DR3 breakpoint was hit recently!\n");
        printf("      This strongly indicates kernel debugging or hypervisor hook!\n");
        return TRUE;
    }

    printf("\n  [OK] No suspicious DR6 flags\n");
    return FALSE;
}

int main()
{
    printf("=== Debug Register Hijacking Detection ===\n");
    printf("This test checks if DR2/DR3 are hijacked by a hypervisor.\n\n");

    int detection_count = 0;

    // 测试 1: 读取 DR 值
    if (TestUserModeDR()) {
        detection_count++;
    }

    // 测试 2: DR 写入一致性
    if (TestDRWriteConsistency()) {
        detection_count++;
    }

    // 测试 3: 硬件断点触发
    if (TestHardwareBreakpointTrigger()) {
        detection_count++;
    }

    // 测试 4: 内核 DR（跳过）
    TestKernelDR();

    // 测试 5: DR6 状态
    if (TestDR6Status()) {
        detection_count++;
    }

    // 最终评估
    printf("\n=== FINAL ASSESSMENT ===\n");
    if (detection_count >= 2) {
        printf("[!] DETECTED: High confidence of DR hijacking!\n");
        printf("    %d suspicious indicators found\n", detection_count);
        printf("    Likely cause: Hypervisor intercepting DR3 for KiDispatchException hook\n");
    } else if (detection_count == 1) {
        printf("[?] SUSPICIOUS: Some indicators found\n");
        printf("    %d suspicious indicator\n", detection_count);
    } else {
        printf("[OK] No DR hijacking detected\n");
    }

    printf("\n=== Test Complete ===\n");
    system("pause");
    return 0;
}
