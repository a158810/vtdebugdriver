#define POOL_NX_OPTIN 1
#include <ntddk.h>
#include <intrin.h>
#include "svmtest_shared.h"

static constexpr ULONG LMV_CPUID_CALL_LEAF = 0x4C4D0001;
static constexpr ULONG EFER_MSR = 0xc0000080;
static constexpr ULONG VM_CR_MSR = 0xc0010114;

NTSTATUS VmDetectRun(_Out_ VMDETECT_RESULT* Result)
{
    int cpuidRegs[4];
    UINT64 tsc1, tsc2;

    RtlZeroMemory(Result, sizeof(VMDETECT_RESULT));

    // Test 1: Hypervisor Present bit (CPUID.1 ECX bit 31)
    __cpuid(cpuidRegs, 0x00000001);
    if (cpuidRegs[2] & (1 << 31))
    {
        Result->DetectionFlags |= VMDETECT_FLAG_HYPERVISOR_PRESENT;
        Result->HypervisorPresent = 1;

        // Read hypervisor vendor string
        __cpuid(cpuidRegs, 0x40000000);
        RtlCopyMemory(Result->HypervisorVendor + 0, &cpuidRegs[1], 4);
        RtlCopyMemory(Result->HypervisorVendor + 4, &cpuidRegs[2], 4);
        RtlCopyMemory(Result->HypervisorVendor + 8, &cpuidRegs[3], 4);
        Result->HypervisorVendor[12] = '\0';
    }

    // Test 2: Custom CPUID leaf without magic (should be transparent)
    __cpuid(cpuidRegs, LMV_CPUID_CALL_LEAF);
    Result->CustomLeafResponse = cpuidRegs[0];
    if (cpuidRegs[0] != 0 || cpuidRegs[1] != 0)
    {
        Result->DetectionFlags |= VMDETECT_FLAG_CUSTOM_CPUID_LEAF;
    }

    // Test 3: RDTSC timing - CPUID causes VMEXIT
    tsc1 = __rdtsc();
    __cpuid(cpuidRegs, 0x00000001);
    tsc2 = __rdtsc();
    Result->RdtscDelta = tsc2 - tsc1;
    if (Result->RdtscDelta > 10000)  // Threshold: 10k cycles suspicious
    {
        Result->DetectionFlags |= VMDETECT_FLAG_TIMING_ANOMALY;
    }

    // Test 4: DR register read (if intercepted, may return fake values)
    __try
    {
        Result->Dr0Value = __readdr(0);
        Result->Dr1Value = __readdr(1);
        Result->Dr2Value = __readdr(2);
        Result->Dr3Value = __readdr(3);
        Result->Dr6Value = __readdr(6);
        Result->Dr7Value = __readdr(7);

        // Check if DR3 is set (LMvtdriver uses DR3 for KiDispatchException)
        if (Result->Dr3Value != 0 || (Result->Dr7Value & 0x500) != 0)
        {
            Result->DetectionFlags |= VMDETECT_FLAG_DR_ANOMALY;
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Result->DrReadIntercepted = 1;
        Result->DetectionFlags |= VMDETECT_FLAG_DR_ANOMALY;
    }

    // Test 5: MSR read (EFER, VM_CR)
    __try
    {
        Result->EferValue = __readmsr(EFER_MSR);

        // Try to read VM_CR (AMD-specific)
        Result->VmCrValue = __readmsr(VM_CR_MSR);

        // If EFER.SVME is set, another hypervisor is active
        if (Result->EferValue & (1ull << 12))
        {
            Result->DetectionFlags |= VMDETECT_FLAG_MSR_INTERCEPT;
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Result->MsrIntercepted = 1;
        Result->DetectionFlags |= VMDETECT_FLAG_MSR_INTERCEPT;
    }

    return STATUS_SUCCESS;
}
