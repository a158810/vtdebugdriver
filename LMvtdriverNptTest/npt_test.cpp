#include <ntddk.h>
#include "../src/shared/lmv_npt_test_shared.h"
#include "../src/shared/lmv_control_shared.h"

extern "C"
VOID
LmvCpuidCall(
    ULONG CommandId,
    ULONG Sequence,
    PLMV_CPUID_RESULT Result
    );

extern "C"
VOID
LmvCpuidQuerySharedPage(
    PLMV_CPUID_SHARED_PAGE_RESULT Result
    );

extern "C"
VOID
LmvNptProbe(
    unsigned __int64 GuestPhysicalAddress,
    unsigned int Operation,
    PLMV_NPT_TEST_RESULT Result
    );

static const ULONG kTestPageCount = 3;
static const UINT64 kOriginalValues[3] = {
    0xA55A1234FEDC9876ull, 0x5AA5FEDC12349876ull, 0xAA55AA55CC33CC33ull
};
static const UINT64 kMappedValues[3] = {
    0x1122334455667788ull, 0x8877665544332211ull, 0x1122AABB3344CCDDull
};
static const UINT64 kWatchedValues[3] = {
    0x13579BDF2468ACE0ull, 0x02468ACE13579BDFull, 0x0F1E2D3C4B5A6978ull
};

typedef struct _LMV_TEST_PAGE
{
    volatile UINT64* Value;
    PHYSICAL_ADDRESS Address;
    BOOLEAN Registered;
    BOOLEAN MayBeMapped;
} LMV_TEST_PAGE, *PLMV_TEST_PAGE;

typedef struct _LMV_NPT_READ_CONTEXT
{
    volatile UINT64* Value;
    UINT64 Expected;
    volatile LONG ProcessorsSeen;
    volatile LONG Mismatches;
} LMV_NPT_READ_CONTEXT, *PLMV_NPT_READ_CONTEXT;

static PVOID g_TestBuffer;
static PVOID g_CrossRegionBuffer;
static LMV_TEST_PAGE g_TestPages[3];
static volatile LONG g_NptFlushFailureCount;
static volatile LONG g_NptFlushProcessorCount;

static
ULONG_PTR
NTAPI
LmvNptReadProcessor(
    _In_ ULONG_PTR Context
    )
{
    PLMV_NPT_READ_CONTEXT readContext =
        reinterpret_cast<PLMV_NPT_READ_CONTEXT>(Context);
    UINT64 value = *readContext->Value;
    if (value != readContext->Expected)
    {
        InterlockedIncrement(&readContext->Mismatches);
    }
    InterlockedIncrement(&readContext->ProcessorsSeen);
    return 0;
}

static
BOOLEAN
LmvNptCheckAllProcessors(
    _In_ PCSTR Name,
    _In_ PLMV_TEST_PAGE Page,
    _In_ UINT64 Expected
    )
{
    LMV_NPT_READ_CONTEXT readContext = {};
    ULONG activeProcessors = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    readContext.Value = Page->Value;
    readContext.Expected = Expected;
    KeIpiGenericCall(LmvNptReadProcessor,
                     reinterpret_cast<ULONG_PTR>(&readContext));
    BOOLEAN passed = readContext.Mismatches == 0 &&
                     readContext.ProcessorsSeen == static_cast<LONG>(activeProcessors);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID,
               passed ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "[LMvtdriverNptTest] %s: %s processors=%ld expected=%lu mismatches=%ld GPA=%llx\n",
               Name, passed ? "PASS" : "FAIL", readContext.ProcessorsSeen,
               activeProcessors, readContext.Mismatches, Page->Address.QuadPart);
    return passed;
}

static
ULONG_PTR
NTAPI
LmvNptFlushProcessor(
    _In_ ULONG_PTR Context
    )
{
    LMV_NPT_TEST_RESULT result = {};
    UNREFERENCED_PARAMETER(Context);
    LmvNptProbe(0, LMV_NPT_TEST_OPERATION_FLUSH_TLB, &result);
    if (result.Status != LMV_NPT_TEST_STATUS_SUCCESS)
    {
        InterlockedIncrement(&g_NptFlushFailureCount);
    }
    InterlockedIncrement(&g_NptFlushProcessorCount);
    return 0;
}

static
BOOLEAN
LmvNptFlushAllProcessors(
    VOID
    )
{
    ULONG expectedProcessors = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    InterlockedExchange(&g_NptFlushFailureCount, 0);
    InterlockedExchange(&g_NptFlushProcessorCount, 0);
    KeIpiGenericCall(LmvNptFlushProcessor, 0);
    LONG seenProcessors = InterlockedCompareExchange(&g_NptFlushProcessorCount, 0, 0);
    LONG failures = InterlockedCompareExchange(&g_NptFlushFailureCount, 0, 0);
    if (failures != 0 || seenProcessors != static_cast<LONG>(expectedProcessors))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverNptTest] TLB flush: FAIL processors=%ld expected=%lu failures=%ld\n",
                   seenProcessors, expectedProcessors, failures);
        return FALSE;
    }
    return TRUE;
}

static
BOOLEAN
LmvNptRestorePage(
    _In_ ULONG Index
    )
{
    PLMV_TEST_PAGE page = &g_TestPages[Index];
    if (page->Registered == FALSE && page->MayBeMapped == FALSE)
    {
        return TRUE;
    }

    LMV_NPT_TEST_RESULT result = {};
    LmvNptProbe(page->Address.QuadPart,
                LMV_NPT_TEST_OPERATION_DISARM_WRITE_WATCH, &result);
    BOOLEAN disarmSucceeded = result.Status == LMV_NPT_TEST_STATUS_SUCCESS;
    BOOLEAN restoreSucceeded = FALSE;
    if (disarmSucceeded != FALSE)
    {
        LmvNptProbe(page->Address.QuadPart,
                    LMV_NPT_TEST_OPERATION_RESTORE_PAGE, &result);
        restoreSucceeded = result.Status == LMV_NPT_TEST_STATUS_SUCCESS;
    }
    BOOLEAN flushSucceeded = LmvNptFlushAllProcessors();
    BOOLEAN releaseSucceeded = FALSE;
    if (disarmSucceeded != FALSE && restoreSucceeded != FALSE &&
        flushSucceeded != FALSE)
    {
        BOOLEAN siblingActive = FALSE;
        for (ULONG index = 0; index < kTestPageCount; ++index)
        {
            if (index != Index &&
                (g_TestPages[index].Registered != FALSE ||
                 g_TestPages[index].MayBeMapped != FALSE) &&
                (g_TestPages[index].Address.QuadPart >> 21) ==
                    (page->Address.QuadPart >> 21))
            {
                siblingActive = TRUE;
            }
        }
        if (siblingActive == FALSE)
        {
            LmvNptProbe(page->Address.QuadPart,
                        LMV_NPT_TEST_OPERATION_RELEASE_REGION, &result);
            releaseSucceeded = result.Status == LMV_NPT_TEST_STATUS_SUCCESS;
            if (releaseSucceeded != FALSE)
            {
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                           "[LMvtdriverNptTest] region-released: PASS base=%llx\n",
                           page->Address.QuadPart & ~((1ull << 21) - 1));
            }
        }
        else
        {
            releaseSucceeded = TRUE;
        }
    }
    if (releaseSucceeded != FALSE)
    {
        page->Registered = FALSE;
        page->MayBeMapped = FALSE;
        return TRUE;
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[LMvtdriverNptTest] cleanup[%lu]: FAIL disarm=%u restore=%u flush=%u release=%u GPA=%llx\n",
               Index, disarmSucceeded, restoreSucceeded, flushSucceeded,
               releaseSucceeded, page->Address.QuadPart);
    return FALSE;
}

static
BOOLEAN
LmvNptRestoreAll(
    VOID
    )
{
    BOOLEAN success = TRUE;
    for (ULONG index = 0; index < kTestPageCount; ++index)
    {
        if (LmvNptRestorePage(index) == FALSE)
        {
            success = FALSE;
        }
    }
    return success;
}

static
VOID
LmvNptTestUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
    if (g_TestBuffer != nullptr && LmvNptRestoreAll() != FALSE)
    {
        MmFreeContiguousMemory(g_TestBuffer);
        g_TestBuffer = nullptr;
        if (g_CrossRegionBuffer != nullptr)
        {
            MmFreeContiguousMemory(g_CrossRegionBuffer);
            g_CrossRegionBuffer = nullptr;
        }
    }
}

static
BOOLEAN
LmvNptCheck(
    _In_ PCSTR Name,
    _In_ PLMV_TEST_PAGE Page,
    _In_ UINT64 Expected
    )
{
    LMV_NPT_TEST_RESULT result = {};
    LmvNptProbe(Page->Address.QuadPart,
                LMV_NPT_TEST_OPERATION_READ_QWORD, &result);
    BOOLEAN passed = result.Status == LMV_NPT_TEST_STATUS_SUCCESS &&
                     result.Value == Expected;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID,
               passed ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "[LMvtdriverNptTest] %s: %s status=%u value=%llx expected=%llx GPA=%llx\n",
               Name, passed ? "PASS" : "FAIL", result.Status,
               result.Value, Expected, Page->Address.QuadPart);
    return passed;
}

static
BOOLEAN
LmvNptCheckWatchCount(
    _In_ PCSTR Name,
    _In_ PLMV_TEST_PAGE Page,
    _In_ UINT64 ExpectedCount
    )
{
    LMV_NPT_TEST_RESULT result = {};
    LmvNptProbe(Page->Address.QuadPart,
                LMV_NPT_TEST_OPERATION_QUERY_WRITE_WATCH, &result);
    BOOLEAN passed = result.Status == LMV_NPT_TEST_STATUS_SUCCESS &&
                     result.Value == ExpectedCount;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID,
               passed ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "[LMvtdriverNptTest] %s: %s status=%u NPF=%llx expected=%llx GPA=%llx\n",
               Name, passed ? "PASS" : "FAIL", result.Status,
               result.Value, ExpectedCount, Page->Address.QuadPart);
    return passed;
}

extern "C"
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    UNREFERENCED_PARAMETER(RegistryPath);
    DriverObject->DriverUnload = LmvNptTestUnload;

    LMV_CPUID_RESULT cpuidResult = {};
    LmvCpuidCall(LmvCpuidCommandQueryStatus, 1, &cpuidResult);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID,
               cpuidResult.Status == LMV_CPUID_RESULT_SUCCESS ?
                   DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "[LMvtdriverNptTest] CPUID call: status=%08x flags=%08x version=%lu sequence=%lu\n",
               cpuidResult.Status,
               cpuidResult.Flags,
               cpuidResult.Version,
               cpuidResult.Sequence);

    LMV_CPUID_SHARED_PAGE_RESULT sharedPageResult = {};
    LmvCpuidQuerySharedPage(&sharedPageResult);
    PHYSICAL_ADDRESS sharedPagePa = {};
    sharedPagePa.LowPart = sharedPageResult.GpaLow;
    sharedPagePa.HighPart = sharedPageResult.GpaHigh;
    PLMV_SHARED_PAGE sharedPage = sharedPageResult.Status == LMV_CPUID_RESULT_SUCCESS &&
        (sharedPagePa.QuadPart & (PAGE_SIZE - 1)) == 0 ?
        static_cast<PLMV_SHARED_PAGE>(MmMapIoSpace(sharedPagePa, PAGE_SIZE, MmCached)) : nullptr;
    ULONG observedMagic = sharedPage != nullptr ? sharedPage->Magic : 0;
    ULONG observedVersion = sharedPage != nullptr ? sharedPage->Version : 0;
    ULONG observedSize = sharedPage != nullptr ? sharedPage->Size : 0;
    ULONG notifyStatus = LMV_CPUID_RESULT_UNSUPPORTED;
    ULONG statsNotifyStatus0 = LMV_CPUID_RESULT_UNSUPPORTED;
    ULONG statsNotifyStatus1 = LMV_CPUID_RESULT_UNSUPPORTED;
    UINT64 statsVmExit0 = 0;
    UINT64 statsNpf0 = 0;
    UINT64 statsProcessors0 = 0;
    UINT64 statsSharedPage0 = 0;
    UINT64 statsVmExit1 = 0;
    UINT64 statsNpf1 = 0;
    UINT64 statsProcessors1 = 0;
    UINT64 statsSharedPage1 = 0;
    BOOLEAN statsPassed = FALSE;
    BOOLEAN sharedPassed = sharedPage != nullptr &&
                           observedMagic == LMV_SHARED_PAGE_MAGIC &&
                           observedVersion == LMV_CPUID_PROTOCOL_VERSION &&
                           observedSize == sizeof(LMV_SHARED_PAGE) &&
                           sharedPageResult.Magic == LMV_SHARED_PAGE_MAGIC;
    if (sharedPassed)
    {
        RtlZeroMemory(&sharedPage->Command, sizeof(sharedPage->Command));
        RtlZeroMemory(&sharedPage->Result, sizeof(sharedPage->Result));
        sharedPage->Command.CommandId = LmvCpuidCommandQueryStatus;
        sharedPage->Command.Version = LMV_CPUID_PROTOCOL_VERSION;
        sharedPage->Command.Size = sizeof(LMV_SHARED_COMMAND);
        sharedPage->Command.Sequence = 2;
        KeMemoryBarrier();
        sharedPage->State = LMV_SHARED_STATE_READY;
        LMV_CPUID_RESULT notifyResult = {};
        LmvCpuidCall(LmvCpuidCommandNotifySharedCommand, 2, &notifyResult);
        notifyStatus = notifyResult.Status;
        sharedPassed = notifyResult.Status == LMV_CPUID_RESULT_SUCCESS &&
                       sharedPage->State == LMV_SHARED_STATE_COMPLETE &&
                       sharedPage->Result.Status == LMV_CPUID_RESULT_SUCCESS &&
                       sharedPage->Result.Sequence == 2 &&
                       sharedPage->Result.Flags == LMV_STATUS_FLAG_SVM_RUNNING;
        if (sharedPassed)
        {
            RtlZeroMemory(&sharedPage->Command, sizeof(sharedPage->Command));
            RtlZeroMemory(&sharedPage->Result, sizeof(sharedPage->Result));
            sharedPage->Command.CommandId = LmvCpuidCommandQueryNptStats;
            sharedPage->Command.Version = LMV_CPUID_PROTOCOL_VERSION;
            sharedPage->Command.Size = sizeof(LMV_SHARED_COMMAND);
            sharedPage->Command.Sequence = 3;
            KeMemoryBarrier();
            sharedPage->State = LMV_SHARED_STATE_READY;
            LmvCpuidCall(LmvCpuidCommandNotifySharedCommand, 3, &notifyResult);
            statsNotifyStatus0 = notifyResult.Status;
            statsVmExit0 = sharedPage->Result.Value0;
            statsNpf0 = sharedPage->Result.Value1;
            statsProcessors0 = sharedPage->Result.Value2;
            statsSharedPage0 = sharedPage->Result.Value3;

            RtlZeroMemory(&sharedPage->Command, sizeof(sharedPage->Command));
            RtlZeroMemory(&sharedPage->Result, sizeof(sharedPage->Result));
            sharedPage->Command.CommandId = LmvCpuidCommandQueryNptStats;
            sharedPage->Command.Version = LMV_CPUID_PROTOCOL_VERSION;
            sharedPage->Command.Size = sizeof(LMV_SHARED_COMMAND);
            sharedPage->Command.Sequence = 4;
            KeMemoryBarrier();
            sharedPage->State = LMV_SHARED_STATE_READY;
            LmvCpuidCall(LmvCpuidCommandNotifySharedCommand, 4, &notifyResult);
            statsNotifyStatus1 = notifyResult.Status;
            statsVmExit1 = sharedPage->Result.Value0;
            statsNpf1 = sharedPage->Result.Value1;
            statsProcessors1 = sharedPage->Result.Value2;
            statsSharedPage1 = sharedPage->Result.Value3;
            statsPassed = statsNotifyStatus0 == LMV_CPUID_RESULT_SUCCESS &&
                          statsNotifyStatus1 == LMV_CPUID_RESULT_SUCCESS &&
                          sharedPage->Result.State == LMV_SHARED_STATE_COMPLETE &&
                          sharedPage->Result.Status == LMV_CPUID_RESULT_SUCCESS &&
                          sharedPage->Result.Sequence == 4 &&
                          statsProcessors0 != 0 &&
                          statsProcessors0 == statsProcessors1 &&
                          statsSharedPage0 == static_cast<UINT64>(sharedPagePa.QuadPart) &&
                          statsSharedPage1 == static_cast<UINT64>(sharedPagePa.QuadPart) &&
                          statsVmExit1 >= statsVmExit0 &&
                          statsNpf1 >= statsNpf0;
        }
    }
    if (sharedPage != nullptr)
    {
        MmUnmapIoSpace(sharedPage, PAGE_SIZE);
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID,
               sharedPassed ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "[LMvtdriverNptTest] shared-page query-status: %s query=%08x notify=%08x gpa=%llx magic=%08x version=%u size=%u expected=%08x\n",
               sharedPassed ? "PASS" : "FAIL",
               sharedPageResult.Status, notifyStatus, sharedPagePa.QuadPart,
               observedMagic, observedVersion, observedSize,
               LMV_SHARED_PAGE_MAGIC);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID,
               statsPassed ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "[LMvtdriverNptTest] shared-page npt-stats: %s first-status=%08x second-status=%08x first={vmexit=%llu npf=%llu processors=%llu page=%llx} second={vmexit=%llu npf=%llu processors=%llu page=%llx}\n",
               statsPassed ? "PASS" : "FAIL",
               statsNotifyStatus0, statsNotifyStatus1,
               statsVmExit0, statsNpf0, statsProcessors0, statsSharedPage0,
               statsVmExit1, statsNpf1, statsProcessors1, statsSharedPage1);

    PHYSICAL_ADDRESS lowest = {};
    PHYSICAL_ADDRESS highest = {};
    PHYSICAL_ADDRESS boundary = {};
    highest.QuadPart = MAXLONGLONG;
    g_TestBuffer = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE * 4,
                         lowest, highest, boundary, MmCached);
    if (g_TestBuffer == nullptr)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverNptTest] allocation failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(g_TestBuffer, PAGE_SIZE * 4);

    ULONG firstPageIndex = 4;
    for (ULONG index = 0; index < 3; ++index)
    {
        PUINT8 candidate = static_cast<PUINT8>(g_TestBuffer) + index * PAGE_SIZE;
        PHYSICAL_ADDRESS first = MmGetPhysicalAddress(candidate);
        PHYSICAL_ADDRESS second = MmGetPhysicalAddress(candidate + PAGE_SIZE);
        if ((first.QuadPart >> 21) == (second.QuadPart >> 21) &&
            second.QuadPart == first.QuadPart + PAGE_SIZE)
        {
            firstPageIndex = index;
            break;
        }
    }
    if (firstPageIndex == 4)
    {
        MmFreeContiguousMemory(g_TestBuffer);
        g_TestBuffer = nullptr;
        return STATUS_UNSUCCESSFUL;
    }

    for (ULONG index = 0; index < 2; ++index)
    {
        PUINT8 page = static_cast<PUINT8>(g_TestBuffer) +
                      (firstPageIndex + index) * PAGE_SIZE;
        g_TestPages[index].Value = reinterpret_cast<volatile UINT64*>(page);
        g_TestPages[index].Address = MmGetPhysicalAddress(page);
        *g_TestPages[index].Value = kOriginalValues[index];
    }

    UINT64 regionEnd = (g_TestPages[0].Address.QuadPart & ~((1ull << 21) - 1)) +
                       (1ull << 21);
    lowest.QuadPart = regionEnd;
    highest.QuadPart = MAXLONGLONG;
    g_CrossRegionBuffer = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowest, highest, boundary, MmCached);
    if (g_CrossRegionBuffer == nullptr && regionEnd > (1ull << 21))
    {
        lowest.QuadPart = 0;
        highest.QuadPart = regionEnd - (1ull << 21) - 1;
        g_CrossRegionBuffer = MmAllocateContiguousMemorySpecifyCache(
            PAGE_SIZE, lowest, highest, boundary, MmCached);
    }
    if (g_CrossRegionBuffer == nullptr)
    {
        MmFreeContiguousMemory(g_TestBuffer);
        g_TestBuffer = nullptr;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverNptTest] cross-region allocation failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(g_CrossRegionBuffer, PAGE_SIZE);
    g_TestPages[2].Value = static_cast<volatile UINT64*>(g_CrossRegionBuffer);
    g_TestPages[2].Address = MmGetPhysicalAddress(g_CrossRegionBuffer);
    *g_TestPages[2].Value = kOriginalValues[2];
    if ((g_TestPages[2].Address.QuadPart >> 21) ==
        (g_TestPages[0].Address.QuadPart >> 21))
    {
        MmFreeContiguousMemory(g_CrossRegionBuffer);
        g_CrossRegionBuffer = nullptr;
        MmFreeContiguousMemory(g_TestBuffer);
        g_TestBuffer = nullptr;
        return STATUS_INVALID_ADDRESS;
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "[LMvtdriverNptTest] selected three pages GPA0=%llx GPA1=%llx GPA2=%llx\n",
               g_TestPages[0].Address.QuadPart,
               g_TestPages[1].Address.QuadPart,
               g_TestPages[2].Address.QuadPart);

    BOOLEAN passed = TRUE;
    LMV_NPT_TEST_RESULT result = {};
    for (ULONG index = 0; index < kTestPageCount && passed != FALSE; ++index)
    {
        LmvNptProbe(g_TestPages[index].Address.QuadPart,
                    LMV_NPT_TEST_OPERATION_REGISTER_PAGE, &result);
        passed = result.Status == LMV_NPT_TEST_STATUS_SUCCESS;
        if (passed != FALSE)
        {
            g_TestPages[index].Registered = TRUE;
        }
        else
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "[LMvtdriverNptTest] register[%lu]: FAIL status=%u\n",
                       index, result.Status);
        }
    }
    for (ULONG index = 0; index < kTestPageCount && passed != FALSE; ++index)
    {
        LmvNptProbe(g_TestPages[index].Address.QuadPart,
                    LMV_NPT_TEST_OPERATION_REMAP_PAGE, &result);
        passed = result.Status == LMV_NPT_TEST_STATUS_SUCCESS;
        if (passed != FALSE)
        {
            g_TestPages[index].MayBeMapped = TRUE;
        }
        else
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "[LMvtdriverNptTest] remap[%lu]: FAIL status=%u\n",
                       index, result.Status);
        }
    }
    if (passed != FALSE)
    {
        passed = LmvNptFlushAllProcessors();
    }
    if (passed != FALSE)
    {
        for (ULONG index = 0; index < kTestPageCount; ++index)
        {
            PLMV_TEST_PAGE page = &g_TestPages[index];
            *page->Value = kMappedValues[index];
            if (LmvNptCheck("mapped-read", page, kMappedValues[index]) == FALSE ||
                LmvNptCheckAllProcessors("mapped-all-cpus", page,
                                         kMappedValues[index]) == FALSE)
            {
                passed = FALSE;
            }
        }
    }
    if (passed != FALSE)
    {
        for (ULONG index = 0; index < kTestPageCount; ++index)
        {
            LmvNptProbe(g_TestPages[index].Address.QuadPart,
                        LMV_NPT_TEST_OPERATION_ARM_WRITE_WATCH, &result);
            if (result.Status != LMV_NPT_TEST_STATUS_SUCCESS)
            {
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "[LMvtdriverNptTest] arm[%lu]: FAIL status=%u\n",
                           index, result.Status);
                passed = FALSE;
                break;
            }
        }
    }
    if (passed != FALSE)
    {
        passed = LmvNptFlushAllProcessors();
    }
    if (passed != FALSE)
    {
        for (ULONG index = 0; index < kTestPageCount; ++index)
        {
            if (LmvNptCheckAllProcessors("armed-readable", &g_TestPages[index],
                                         kMappedValues[index]) == FALSE)
            {
                passed = FALSE;
            }
        }
    }
    if (passed != FALSE)
    {
        *g_TestPages[0].Value = kWatchedValues[0];
        if (LmvNptCheckWatchCount("page0-fired", &g_TestPages[0], 1) == FALSE ||
            LmvNptCheckWatchCount("page1-not-fired", &g_TestPages[1], 0) == FALSE ||
            LmvNptCheckWatchCount("page2-not-fired", &g_TestPages[2], 0) == FALSE ||
            LmvNptCheck("page0-write", &g_TestPages[0], kWatchedValues[0]) == FALSE)
        {
            passed = FALSE;
        }
    }
    if (passed != FALSE)
    {
        passed = LmvNptRestorePage(0);
    }
    if (passed != FALSE)
    {
        passed = LmvNptCheckAllProcessors("page0-restored", &g_TestPages[0],
                                            kOriginalValues[0]) &&
                 LmvNptCheckAllProcessors("page1-still-armed", &g_TestPages[1],
                                            kMappedValues[1]) &&
                 LmvNptCheckAllProcessors("page2-still-armed", &g_TestPages[2],
                                            kMappedValues[2]);
    }
    if (passed != FALSE)
    {
        *g_TestPages[2].Value = kWatchedValues[2];
        passed = LmvNptCheckWatchCount("page2-fired", &g_TestPages[2], 1) &&
                 LmvNptCheckWatchCount("page1-not-fired", &g_TestPages[1], 0) &&
                 LmvNptCheck("page2-write", &g_TestPages[2], kWatchedValues[2]) &&
                 LmvNptCheckAllProcessors("page2-write-all-cpus", &g_TestPages[2],
                                          kWatchedValues[2]);
    }
    if (passed != FALSE)
    {
        passed = LmvNptRestorePage(2);
    }
    if (passed != FALSE)
    {
        passed = LmvNptCheckAllProcessors("page2-restored", &g_TestPages[2],
                                            kOriginalValues[2]) &&
                 LmvNptCheckAllProcessors("page1-still-armed", &g_TestPages[1],
                                            kMappedValues[1]);
    }
    if (passed != FALSE)
    {
        *g_TestPages[1].Value = kWatchedValues[1];
        passed = LmvNptCheckWatchCount("page1-fired", &g_TestPages[1], 1) &&
                 LmvNptCheck("page1-write", &g_TestPages[1], kWatchedValues[1]) &&
                 LmvNptCheckAllProcessors("page1-write-all-cpus", &g_TestPages[1],
                                          kWatchedValues[1]);
    }
    BOOLEAN cleaned = LmvNptRestoreAll();
    if (cleaned != FALSE)
    {
        for (ULONG index = 0; index < kTestPageCount; ++index)
        {
            if (LmvNptCheckAllProcessors("restored-original-page", &g_TestPages[index],
                                         kOriginalValues[index]) == FALSE)
            {
                passed = FALSE;
            }
        }
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "[LMvtdriverNptTest] summary: %s\n",
               passed != FALSE && cleaned != FALSE ? "PASS" : "FAIL");
    return STATUS_SUCCESS;
}
