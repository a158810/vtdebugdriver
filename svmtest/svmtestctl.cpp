#include <Windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cwchar>
#include "svmtest_shared.h"

static bool IsNtSuccess(LONG status)
{
    return status >= 0;
}

static const wchar_t* StageName(ULONG stage)
{
    switch (stage)
    {
    case SVMTEST_STAGE_NONE: return L"none";
    case SVMTEST_STAGE_CAPABILITIES: return L"capabilities";
    case SVMTEST_STAGE_ALLOCATION: return L"allocation";
    case SVMTEST_STAGE_GUEST_SETUP: return L"guest-setup";
    case SVMTEST_STAGE_PIN_PROCESSOR: return L"pin-processor";
    case SVMTEST_STAGE_SET_HSAVE: return L"set-hsave";
    case SVMTEST_STAGE_VMMCALL: return L"vmmcall";
    case SVMTEST_STAGE_CPUID: return L"cpuid";
    case SVMTEST_STAGE_MSR: return L"msr";
    case SVMTEST_STAGE_EXCEPTION: return L"exception";
    case SVMTEST_STAGE_NESTED_VMRUN: return L"nested-vmrun";
    case SVMTEST_STAGE_CLEANUP: return L"cleanup";
    case SVMTEST_STAGE_COMPLETE: return L"complete";
    case SVMTEST_STAGE_CPUID_RESUME: return L"cpuid-resume";
    default: return L"unknown";
    }
}

static void PrintCapabilities(const SVMTEST_CAPABILITIES& capabilities)
{
    std::wprintf(L"status=0x%08lx flags=0x%08lx\n",
                 static_cast<unsigned long>(capabilities.Status), capabilities.Flags);
    std::wprintf(L"flags: svm=%u svm_ready=%u svmdis_clear=%u nrip_save=%u svme_current=%u\n",
                 (capabilities.Flags & SVMTEST_FLAG_SVM_SUPPORTED) != 0,
                 (capabilities.Flags & SVMTEST_FLAG_SVM_READY) != 0,
                 (capabilities.Flags & SVMTEST_FLAG_SVMDIS_CLEAR) != 0,
                 (capabilities.Flags & SVMTEST_FLAG_NRIP_SAVE) != 0,
                 (capabilities.Flags & SVMTEST_FLAG_SVME_ENABLED) != 0);
    std::wprintf(L"cpu=%S hypervisor=%S revision=%lu max_asid=%lu features_edx=0x%08lx\n",
                 capabilities.CpuVendor, capabilities.HypervisorVendor,
                 capabilities.SvmRevision, capabilities.MaximumAsid,
                 capabilities.SvmFeaturesEdx);
    std::wprintf(L"efer=0x%016llx vm_cr=0x%016llx vm_hsave_pa=0x%016llx\n",
                 static_cast<unsigned long long>(capabilities.Efer),
                 static_cast<unsigned long long>(capabilities.VmCr),
                 static_cast<unsigned long long>(capabilities.VmHsavePa));
}
static const wchar_t* ModeName(ULONG mode)
{
    switch (mode)
    {
    case SVMTEST_MODE_CPUID: return L"cpuid-roundtrip";
    case SVMTEST_MODE_FULL_SMOKE: return L"full-smoke";
    case SVMTEST_MODE_NESTED_FULL: return L"nested-full";
    case SVMTEST_MODE_MSR_TRANSPARENCY: return L"msr-transparency";
    default: return L"unknown";
    }
}

static void PrintResult(const SVMTEST_RESULT& result)
{
    std::wprintf(L"status=0x%08lx stage=%lu(%s) mode=%s passed=0x%08lx iterations=%lu/%lu\n",
                 static_cast<unsigned long>(result.Status), result.Stage,
                 StageName(result.Stage), ModeName(result.Mode), result.PassedMask,
                 result.IterationsCompleted, result.IterationsRequested);
    std::wprintf(L"processor=%lu:%lu vmcb_pa=0x%08lx%08lx hsave_pa=0x%08lx%08lx magic=0x%016llx\n",
                 result.ProcessorGroup, result.ProcessorNumber,
                 result.VmcbPhysicalAddressHigh, result.VmcbPhysicalAddressLow,
                 result.HsavePhysicalAddressHigh, result.HsavePhysicalAddressLow,
                 static_cast<unsigned long long>(result.GuestMagic));
    if (result.Mode == SVMTEST_MODE_CPUID)
    {
        std::wprintf(L"cpuid: leaf=0x%08lx exit_eax=0x%08lx expected_eax=0x%08lx observed_eax=0x%08lx\n",
                     result.CpuidLeaf, result.CpuidExitObservedEax,
                     result.CpuidExpectedEax, result.CpuidObservedEax);
    }
    else if (result.Mode == SVMTEST_MODE_NESTED_FULL)
    {
        std::wprintf(L"nested: enter=%lu reflect=%lu return=%lu\n",
                     result.NestedEnterCount, result.NestedReflectCount, result.NestedReturnCount);
        std::wprintf(L"l1_handler: marker=0x%08lx rax=0x%08lx rbx=0x%08lx rcx=0x%08lx\n",
                     result.L1HandlerObservedMarker, result.L1HandlerObservedRax,
                     result.L1HandlerObservedRbx, result.L1HandlerObservedRcx);
        std::wprintf(L"l2_resume: marker=0x%08lx observed_eax=0x%08lx\n",
                     result.L2ResumeMarker, result.CpuidObservedEax);
    }
    else if (result.Mode == SVMTEST_MODE_MSR_TRANSPARENCY)
    {
        std::wprintf(L"msr_test: passed=%lu transparency_detected=%lu\n",
                     result.MsrTestPassed, result.MsrTransparencyDetected);
        std::wprintf(L"efer: initial=0x%016llx after_write=0x%016llx read_back=0x%016llx\n",
                     static_cast<unsigned long long>(result.MsrEferInitial),
                     static_cast<unsigned long long>(result.MsrEferAfterWrite),
                     static_cast<unsigned long long>(result.MsrEferReadBack));
        std::wprintf(L"vm_cr: initial=0x%016llx after_write=0x%016llx read_back=0x%016llx\n",
                     static_cast<unsigned long long>(result.MsrVmCrInitial),
                     static_cast<unsigned long long>(result.MsrVmCrAfterWrite),
                     static_cast<unsigned long long>(result.MsrVmCrReadBack));
        std::wprintf(L"vm_hsave_pa: initial=0x%016llx after_write=0x%016llx read_back=0x%016llx\n",
                     static_cast<unsigned long long>(result.MsrVmHsavePaInitial),
                     static_cast<unsigned long long>(result.MsrVmHsavePaAfterWrite),
                     static_cast<unsigned long long>(result.MsrVmHsavePaReadBack));
        if (result.MsrTestPassed && !result.MsrTransparencyDetected)
            std::wprintf(L"[+] MSR virtualization is TRANSPARENT\n");
        else if (result.MsrTransparencyDetected)
            std::wprintf(L"[!] MSR virtualization DETECTED (not transparent)\n");
    }
    std::wprintf(L"exits: vmmcall=%lu cpuid=%lu msr=%lu exception=%lu injected=%lu nested_vmrun=%lu nested_gp=%lu\n",
                 result.VmmcallExitCount, result.CpuidExitCount, result.MsrExitCount,
                 result.ExceptionExitCount, result.ExceptionInjectionCount,
                 result.NestedVmrunExitCount, result.NestedVmrunGpCount);
    for (ULONG index = 0; index < SVMTEST_EXIT_RECORD_COUNT; ++index)
    {
        const SVMTEST_EXIT_RECORD& exit = result.Exits[index];
        if (exit.ExitCode == 0 && exit.GuestRip == 0 && exit.ObservedNrip == 0)
            continue;
        std::wprintf(L"exit[%lu]: code=0x%llx info1=0x%llx info2=0x%llx rip=0x%llx nrip=0x%llx expected=0x%llx\n",
                     index,
                     static_cast<unsigned long long>(exit.ExitCode),
                     static_cast<unsigned long long>(exit.ExitInfo1),
                     static_cast<unsigned long long>(exit.ExitInfo2),
                     static_cast<unsigned long long>(exit.GuestRip),
                     static_cast<unsigned long long>(exit.ObservedNrip),
                     static_cast<unsigned long long>(exit.ExpectedNrip));
    }
}
static HANDLE OpenDevice()
{
    return CreateFileW(L"\\\\.\\svmtest", GENERIC_READ | GENERIC_WRITE,
                       0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static bool QueryCapabilities(HANDLE device, SVMTEST_CAPABILITIES* capabilities)
{
    DWORD returned = 0;
    ZeroMemory(capabilities, sizeof(*capabilities));
    return DeviceIoControl(device, static_cast<DWORD>(IOCTL_SVMTEST_QUERY_CAPABILITIES),
                           nullptr, static_cast<DWORD>(0), capabilities, static_cast<DWORD>(sizeof(*capabilities)),
                           &returned, nullptr) != FALSE &&
           returned >= static_cast<DWORD>(sizeof(*capabilities));
}

static bool QueryResult(HANDLE device, SVMTEST_RESULT* result)
{
    DWORD returned = 0;
    ZeroMemory(result, sizeof(*result));
    return DeviceIoControl(device, static_cast<DWORD>(IOCTL_SVMTEST_QUERY_RESULT),
                           nullptr, static_cast<DWORD>(0), result, static_cast<DWORD>(sizeof(*result)),
                           &returned, nullptr) != FALSE &&
           returned >= static_cast<DWORD>(sizeof(*result));
}

static void PrintUsage()
{
    std::wprintf(L"Usage: svmtestctl.exe capabilities | run [iterations] | cpuid [iterations] | nested [iterations] | msr [iterations] | result | reset\n");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        PrintUsage();
        return 2;
    }

    HANDLE device = OpenDevice();
    if (device == INVALID_HANDLE_VALUE)
    {
        std::fwprintf(stderr, L"Cannot open \\\\.\\svmtest (error %lu). Run elevated and load the driver first.\n",
                      GetLastError());
        return 1;
    }

    int exitCode = 0;
    if (_wcsicmp(argv[1], L"capabilities") == 0)
    {
        SVMTEST_CAPABILITIES capabilities = {};
        if (!QueryCapabilities(device, &capabilities))
        {
            std::fwprintf(stderr, L"Capability query failed (error %lu).\n", GetLastError());
            exitCode = 1;
        }
        else
        {
            PrintCapabilities(capabilities);
            exitCode = IsNtSuccess(capabilities.Status) ? 0 : 1;
        }
    }
    else if (_wcsicmp(argv[1], L"run") == 0 ||
             _wcsicmp(argv[1], L"cpuid") == 0)
    {
        ULONG iterations = 1;
        if (argc > 3)
        {
            PrintUsage();
            CloseHandle(device);
            return 2;
        }
        if (argc > 2)
        {
            wchar_t* end = nullptr;
            unsigned long parsed = std::wcstoul(argv[2], &end, 10);
            if (end == argv[2] || *end != L'\0' || parsed == 0 || parsed > 8)
            {
                std::fwprintf(stderr, L"iterations must be between 1 and 8.\n");
                CloseHandle(device);
                return 2;
            }
            iterations = static_cast<ULONG>(parsed);
        }

        SVMTEST_RUN_REQUEST request = {};
        request.Version = SVMTEST_PROTOCOL_VERSION;
        request.Size = static_cast<ULONG>(sizeof(request));
        request.Iterations = iterations;
        request.Mode = SVMTEST_MODE_CPUID;
        SVMTEST_RESULT result = {};
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(device, static_cast<DWORD>(IOCTL_SVMTEST_RUN_CPUID),
                                  &request, static_cast<DWORD>(sizeof(request)),
                                  &result, static_cast<DWORD>(sizeof(result)), &returned, nullptr);
        DWORD requestError = ok ? ERROR_SUCCESS : GetLastError();
        BOOL resultReceived = returned >= static_cast<DWORD>(sizeof(result));
        if (!resultReceived)
        {
            SVMTEST_RESULT queried = {};
            if (QueryResult(device, &queried))
            {
                result = queried;
                resultReceived = TRUE;
            }
            else
                std::fwprintf(stderr, L"SVM test request returned error %lu.\n", requestError);
        }
        PrintResult(result);
        exitCode = resultReceived && IsNtSuccess(result.Status) ? 0 : 1;
    }
    else if (_wcsicmp(argv[1], L"nested") == 0)
    {
        ULONG iterations = 1;
        if (argc > 3)
        {
            PrintUsage();
            CloseHandle(device);
            return 2;
        }
        if (argc > 2)
        {
            wchar_t* end = nullptr;
            unsigned long parsed = std::wcstoul(argv[2], &end, 10);
            if (end == argv[2] || *end != L'\0' || parsed == 0 || parsed > 8)
            {
                std::fwprintf(stderr, L"iterations must be between 1 and 8.\n");
                CloseHandle(device);
                return 2;
            }
            iterations = static_cast<ULONG>(parsed);
        }

        SVMTEST_RUN_REQUEST request = {};
        request.Version = SVMTEST_PROTOCOL_VERSION;
        request.Size = static_cast<ULONG>(sizeof(request));
        request.Iterations = iterations;
        request.Mode = SVMTEST_MODE_NESTED_FULL;
        SVMTEST_RESULT result = {};
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(device, static_cast<DWORD>(IOCTL_SVMTEST_RUN_FULL_NESTED),
                                  &request, static_cast<DWORD>(sizeof(request)),
                                  &result, static_cast<DWORD>(sizeof(result)), &returned, nullptr);
        DWORD requestError = ok ? ERROR_SUCCESS : GetLastError();
        BOOL resultReceived = returned >= static_cast<DWORD>(sizeof(result));
        if (!resultReceived)
        {
            SVMTEST_RESULT queried = {};
            if (QueryResult(device, &queried))
            {
                result = queried;
                resultReceived = TRUE;
            }
            else
                std::fwprintf(stderr, L"Full nested test request returned error %lu.\n", requestError);
        }
        PrintResult(result);
        exitCode = resultReceived && IsNtSuccess(result.Status) ? 0 : 1;
    }
    else if (_wcsicmp(argv[1], L"msr") == 0)
    {
        ULONG iterations = 1;
        if (argc > 3)
        {
            PrintUsage();
            CloseHandle(device);
            return 2;
        }
        if (argc > 2)
        {
            wchar_t* end = nullptr;
            unsigned long parsed = std::wcstoul(argv[2], &end, 10);
            if (end == argv[2] || *end != L'\0' || parsed == 0 || parsed > 8)
            {
                std::fwprintf(stderr, L"iterations must be between 1 and 8.\n");
                CloseHandle(device);
                return 2;
            }
            iterations = static_cast<ULONG>(parsed);
        }

        SVMTEST_RUN_REQUEST request = {};
        request.Version = SVMTEST_PROTOCOL_VERSION;
        request.Size = static_cast<ULONG>(sizeof(request));
        request.Iterations = iterations;
        request.Mode = SVMTEST_MODE_MSR_TRANSPARENCY;
        SVMTEST_RESULT result = {};
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(device, static_cast<DWORD>(IOCTL_SVMTEST_RUN_MSR_TEST),
                                  &request, static_cast<DWORD>(sizeof(request)),
                                  &result, static_cast<DWORD>(sizeof(result)), &returned, nullptr);
        DWORD requestError = ok ? ERROR_SUCCESS : GetLastError();
        BOOL resultReceived = returned >= static_cast<DWORD>(sizeof(result));
        if (!resultReceived)
        {
            SVMTEST_RESULT queried = {};
            if (QueryResult(device, &queried))
            {
                result = queried;
                resultReceived = TRUE;
            }
            else
                std::fwprintf(stderr, L"MSR transparency test request returned error %lu.\n", requestError);
        }
        PrintResult(result);
        exitCode = resultReceived && IsNtSuccess(result.Status) ? 0 : 1;
    }
    else if (_wcsicmp(argv[1], L"result") == 0)
    {
        SVMTEST_RESULT result = {};
        if (!QueryResult(device, &result))
        {
            std::fwprintf(stderr, L"Result query failed (error %lu).\n", GetLastError());
            exitCode = 1;
        }
        else
        {
            PrintResult(result);
            exitCode = IsNtSuccess(result.Status) ? 0 : 1;
        }
    }
    else if (_wcsicmp(argv[1], L"reset") == 0)
    {
        DWORD returned = 0;
        if (!DeviceIoControl(device, static_cast<DWORD>(IOCTL_SVMTEST_RESET), nullptr, static_cast<DWORD>(0),
                             nullptr, static_cast<DWORD>(0), &returned, nullptr))
        {
            std::fwprintf(stderr, L"Reset failed (error %lu).\n", GetLastError());
            exitCode = 1;
        }
    }
    else
    {
        PrintUsage();
        exitCode = 2;
    }

    CloseHandle(device);
    return exitCode;
}
