// 异常处理延迟检测 - 检测 DR3 断点和 VMEXIT 延迟
// 编译: cl /O2 /EHsc test_exception_timing.cpp /Fe:exception_timing.exe

#include <Windows.h>
#include <stdio.h>
#include <intrin.h>
#include <vector>

#pragma intrinsic(__rdtsc)

// 测试结果
struct TimingResult {
    UINT64 min_cycles;
    UINT64 max_cycles;
    UINT64 avg_cycles;
    UINT64 median_cycles;
};

// 计算中位数
UINT64 GetMedian(std::vector<UINT64>& values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

// 测试 1：访问违例异常延迟
TimingResult TestAccessViolation() {
    const int ITERATIONS = 100;
    std::vector<UINT64> samples;

    printf("  Testing Access Violation (#PF) timing...\n");

    for (int i = 0; i < ITERATIONS; i++) {
        volatile int* null_ptr = nullptr;
        UINT64 start = __rdtsc();

        __try {
            *null_ptr = 42;  // 触发 #PF
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            UINT64 end = __rdtsc();
            samples.push_back(end - start);
        }

        Sleep(1);  // 避免缓存效应
    }

    TimingResult result;
    result.min_cycles = *std::min_element(samples.begin(), samples.end());
    result.max_cycles = *std::max_element(samples.begin(), samples.end());
    result.avg_cycles = 0;
    for (auto v : samples) result.avg_cycles += v;
    result.avg_cycles /= samples.size();
    result.median_cycles = GetMedian(samples);

    return result;
}

// 测试 2：除零异常延迟
TimingResult TestDivideByZero() {
    const int ITERATIONS = 100;
    std::vector<UINT64> samples;

    printf("  Testing Divide by Zero (#DE) timing...\n");

    for (int i = 0; i < ITERATIONS; i++) {
        volatile int divisor = 0;
        UINT64 start = __rdtsc();

        __try {
            int result = 42 / divisor;  // 触发 #DE
            (void)result;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            UINT64 end = __rdtsc();
            samples.push_back(end - start);
        }

        Sleep(1);
    }

    TimingResult result;
    result.min_cycles = *std::min_element(samples.begin(), samples.end());
    result.max_cycles = *std::max_element(samples.begin(), samples.end());
    result.avg_cycles = 0;
    for (auto v : samples) result.avg_cycles += v;
    result.avg_cycles /= samples.size();
    result.median_cycles = GetMedian(samples);

    return result;
}

// 测试 3：INT3 断点异常延迟
TimingResult TestBreakpoint() {
    const int ITERATIONS = 100;
    std::vector<UINT64> samples;

    printf("  Testing INT3 Breakpoint (#BP) timing...\n");

    for (int i = 0; i < ITERATIONS; i++) {
        UINT64 start = __rdtsc();

        __try {
            __debugbreak();  // INT3
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            UINT64 end = __rdtsc();
            samples.push_back(end - start);
        }

        Sleep(1);
    }

    TimingResult result;
    result.min_cycles = *std::min_element(samples.begin(), samples.end());
    result.max_cycles = *std::max_element(samples.begin(), samples.end());
    result.avg_cycles = 0;
    for (auto v : samples) result.avg_cycles += v;
    result.avg_cycles /= samples.size();
    result.median_cycles = GetMedian(samples);

    return result;
}

// 测试 4：大量连续异常（压力测试）
TimingResult TestContinuousExceptions() {
    const int ITERATIONS = 1000;
    std::vector<UINT64> samples;

    printf("  Testing continuous exceptions (stress test)...\n");

    UINT64 total_start = __rdtsc();

    for (int i = 0; i < ITERATIONS; i++) {
        volatile int* null_ptr = nullptr;
        UINT64 start = __rdtsc();

        __try {
            *null_ptr = i;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            UINT64 end = __rdtsc();
            samples.push_back(end - start);
        }
    }

    UINT64 total_end = __rdtsc();

    TimingResult result;
    result.min_cycles = *std::min_element(samples.begin(), samples.end());
    result.max_cycles = *std::max_element(samples.begin(), samples.end());
    result.avg_cycles = 0;
    for (auto v : samples) result.avg_cycles += v;
    result.avg_cycles /= samples.size();
    result.median_cycles = GetMedian(samples);

    printf("    Total time for %d exceptions: %llu cycles\n", ITERATIONS, total_end - total_start);
    printf("    Average time per exception: %llu cycles\n", (total_end - total_start) / ITERATIONS);

    return result;
}

void PrintResult(const char* test_name, const TimingResult& result) {
    printf("\n  %s:\n", test_name);
    printf("    Min:    %6llu cycles\n", result.min_cycles);
    printf("    Median: %6llu cycles\n", result.median_cycles);
    printf("    Avg:    %6llu cycles\n", result.avg_cycles);
    printf("    Max:    %6llu cycles\n", result.max_cycles);

    // 检测阈值
    const UINT64 NORMAL_THRESHOLD = 5000;    // 正常异常 < 5000 cycles
    const UINT64 SUSPICIOUS_THRESHOLD = 10000;  // 可疑

    if (result.median_cycles > SUSPICIOUS_THRESHOLD) {
        printf("    [!] CRITICAL: Exception delay is VERY HIGH (likely virtualized!)\n");
    } else if (result.median_cycles > NORMAL_THRESHOLD) {
        printf("    [!] WARNING: Exception delay is elevated (suspicious)\n");
    } else {
        printf("    [OK] Exception delay is within normal range\n");
    }
}

int main()
{
    printf("=== Exception Handling Timing Detection ===\n");
    printf("This test measures exception handling delays\n");
    printf("to detect hypervisor/debugger presence.\n\n");

    // 基准测试：正常代码执行
    printf("[Baseline] Measuring normal execution overhead...\n");
    UINT64 baseline_start = __rdtsc();
    for (int i = 0; i < 100; i++) {
        volatile int x = i * 2;
        (void)x;
    }
    UINT64 baseline_end = __rdtsc();
    printf("  Normal execution: ~%llu cycles per iteration\n\n",
           (baseline_end - baseline_start) / 100);

    // 测试 1: #PF
    printf("[Test 1] Access Violation Exception\n");
    TimingResult result1 = TestAccessViolation();
    PrintResult("Access Violation (#PF)", result1);

    // 测试 2: #DE
    printf("\n[Test 2] Divide by Zero Exception\n");
    TimingResult result2 = TestDivideByZero();
    PrintResult("Divide by Zero (#DE)", result2);

    // 测试 3: #BP
    printf("\n[Test 3] Breakpoint Exception\n");
    TimingResult result3 = TestBreakpoint();
    PrintResult("Breakpoint (#BP)", result3);

    // 测试 4: 连续异常压力测试
    printf("\n[Test 4] Continuous Exception Stress Test\n");
    TimingResult result4 = TestContinuousExceptions();
    PrintResult("Stress Test (1000 exceptions)", result4);

    // 综合评估
    printf("\n=== FINAL ASSESSMENT ===\n");
    int suspicious_count = 0;

    if (result1.median_cycles > 5000) suspicious_count++;
    if (result2.median_cycles > 5000) suspicious_count++;
    if (result3.median_cycles > 5000) suspicious_count++;
    if (result4.median_cycles > 5000) suspicious_count++;

    if (suspicious_count >= 3) {
        printf("[!] DETECTED: High probability of hypervisor/debugger!\n");
        printf("    %d out of 4 tests show elevated delays\n", suspicious_count);
        printf("    Likely cause: DR3 breakpoint on KiDispatchException\n");
    } else if (suspicious_count >= 1) {
        printf("[?] SUSPICIOUS: Some tests show elevated delays\n");
        printf("    %d out of 4 tests flagged\n", suspicious_count);
    } else {
        printf("[OK] All tests passed - no obvious virtualization detected\n");
    }

    printf("\n=== Test Complete ===\n");
    system("pause");
    return 0;
}
