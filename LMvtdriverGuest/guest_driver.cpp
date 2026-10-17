#include <ntifs.h>
#include <intrin.h>
#include "../src/shared/lmv_control_shared.h"
#include "../src/shared/ki_session_lifecycle.hpp"
#include "guest_memory.hpp"
#include "guest_layout.hpp"
#include "static_probe_sample.hpp"
extern "C" NTSTATUS LmvGuestFindKiFunctionBounds(
    PLMV_GUEST_KI_PAGE_PREFLIGHT Output);

extern "C" VOID LmvGuestCpuidCall(ULONG CommandId, ULONG Sequence, PLMV_CPUID_RESULT Result);
extern "C" VOID LmvGuestQuerySharedPage(PLMV_CPUID_SHARED_PAGE_RESULT Result);
extern "C" VOID LmvGuestKiDr3RedirectStub();
extern "C" VOID LmvGuestKiDr3InspectStub();
extern "C" VOID LmvGuestKiSessionEventStub();
extern "C" VOID LmvGuestKiSessionPassStub();
extern "C" VOID LmvGuestKiCaptureExit();
extern "C" VOID LmvGuestKiCommitExit();
static VOID LmvGuestUnload(PDRIVER_OBJECT DriverObject);

typedef struct _LMV_KI_INSPECT_STATE
{
    volatile LONG Valid;
    ULONG Status;
    ULONG Reason;
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG ExceptionCode;
    ULONG FirstChance;
    ULONGLONG ExceptionAddress;
    ULONG FrameStatus;
    ULONG FrameReason;
    LMV_GUEST_KI_FRAME_SNAPSHOT Frame;
    ULONG EventStatus;
    ULONG EventSequence;
} LMV_KI_INSPECT_STATE;

static volatile LONG g_KiInspectEnabled;
static volatile LONG g_KiInspectCaptureFrame;
static volatile LONG g_KiDr3WaitEnabled;
static volatile LONG g_KiDr3RaxWriteEnabled;
static volatile LONG g_KiDr3HandleEnabled;
static ULONG g_KiInspectTargetProcessId;
static LMV_KI_INSPECT_STATE g_KiInspectState;
static FAST_MUTEX g_KiDr3EventMutex;
static KEVENT g_KiDr3EventAvailable;
static constexpr ULONG kKiDr3EventCapacity = 16;
static constexpr ULONG kKiDr3EventCancelled = 3;
typedef struct _LMV_KI_DR3_EVENT_SLOT
{
    KEVENT Completed;
    LMV_GUEST_KI_DR3_EVENT Event;
    PFILE_OBJECT Consumer;
    BOOLEAN RealFrame;
    BOOLEAN WriteRax;
    BOOLEAN Handled;
    BOOLEAN WriteRip;
    BOOLEAN WriteContext;
    BOOLEAN Persistent;
    ULONG State;
} LMV_KI_DR3_EVENT_SLOT;
static LMV_KI_DR3_EVENT_SLOT g_KiDr3Events[kKiDr3EventCapacity];
static volatile LONG g_KiDr3EventSequence;
static PFILE_OBJECT g_KiDr3EventOwner;
static ULONG g_KiDr3ControllerProcessId;
static volatile ULONG g_KiSessionMode = LMV_DEBUG_MODE_INTERACTIVE;  // Default: Interactive mode
static ULONG g_KiDr3EventGeneration;
static BOOLEAN g_KiDr3QueueSmokeActive;
extern "C" volatile LONG64 g_LmvGuestKiDr3StubHits = 0;
extern "C" volatile ULONGLONG g_LmvGuestKiDr3ReturnRip = 0;
extern "C" volatile ULONGLONG g_LmvGuestKiSessionResumeRip = 0;
static PDRIVER_OBJECT g_GuestDriverObject;
static PEPROCESS g_KiSessionProcess;
static PFILE_OBJECT g_KiSessionOwner;
static volatile LONG g_KiSessionActive;
static volatile LONG g_KiDr2WatchActive;
static KEVENT g_KiSessionGate;
static BOOLEAN g_KiGuestRegistered;
static BOOLEAN g_KiGuestRetired;
static volatile LONG g_KiUnloadPreparing;
static volatile LONG g_KiPersistentOnly;
static LONG g_KiActiveRequests;

static PDEVICE_OBJECT g_DeviceObject;
static UNICODE_STRING g_SymbolicLink;
static UNICODE_STRING g_DebugSymbolicLink;
static FAST_MUTEX g_CommandMutex;
static FAST_MUTEX g_SmokeMutex;
static FAST_MUTEX g_KiSiteMutex;
static PVOID g_KiNxSiteBuffer;
static FAST_MUTEX g_KiMirrorMutex;
static PVOID g_KiMirrorAllocation;
static PMDL g_KiMirrorMdl;
static PVOID g_KiMirrorPages;
static PFILE_OBJECT g_KiMirrorOwner;
static volatile LONG g_Sequence;
static PMDL g_ExecProbeMdl;
static ULONG g_ExecProbeProcessId;
static ULONG g_ExecProbeStage;
static ULONG g_KernelProbeStage;
static ULONG g_KernelProbeProcessId;
static PFILE_OBJECT g_ExecProbeFileObject;
static FAST_MUTEX g_StubMutex;
static KEVENT g_StubEventAvailable;
static LMV_GUEST_STUB_EVENT g_StubQueue[LMV_GUEST_STUB_QUEUE_CAPACITY];
static ULONG g_StubQueueHead;
static ULONG g_StubQueueTail;
static ULONG g_StubQueueCount;
static LMV_GUEST_STUB_EVENT g_StubActiveEvent;
static BOOLEAN g_StubActive;
static volatile ULONG g_StubSequence;
static NTSTATUS
LmvGuestIssueSharedCommand(
    _In_ ULONG CommandId,
    _In_ ULONGLONG GuestPhysicalAddress,
    _Out_ PLMV_SHARED_RESULT Result,
    _In_ ULONGLONG ExtraArgument = 0,
    _In_ ULONGLONG Argument2 = 0,
    _In_ ULONGLONG Argument3 = 0,
    _In_ ULONG Flags = 0
    )
{
    LMV_CPUID_SHARED_PAGE_RESULT pageResult = {};
    LMV_CPUID_RESULT notifyResult = {};
    PHYSICAL_ADDRESS pagePa = {};
    PLMV_SHARED_PAGE page = nullptr;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    ULONG sequence = static_cast<ULONG>(InterlockedIncrement(&g_Sequence));

    RtlZeroMemory(Result, sizeof(*Result));
    ExAcquireFastMutex(&g_CommandMutex);
    LmvGuestQuerySharedPage(&pageResult);
    if (pageResult.Status != LMV_CPUID_RESULT_SUCCESS ||
        pageResult.Magic != LMV_SHARED_PAGE_MAGIC)
    {
        goto Exit;
    }

    pagePa.LowPart = pageResult.GpaLow;
    pagePa.HighPart = pageResult.GpaHigh;
    if ((pagePa.QuadPart & (PAGE_SIZE - 1)) != 0)
    {
        status = STATUS_DATATYPE_MISALIGNMENT;
        goto Exit;
    }

    page = static_cast<PLMV_SHARED_PAGE>(MmMapIoSpace(pagePa, PAGE_SIZE, MmCached));
    if (page == nullptr)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    if (page->Magic != LMV_SHARED_PAGE_MAGIC ||
        page->Version != LMV_CPUID_PROTOCOL_VERSION ||
        page->Size != sizeof(LMV_SHARED_PAGE) ||
        (page->State != LMV_SHARED_STATE_FREE &&
         page->State != LMV_SHARED_STATE_COMPLETE))
    {
        status = STATUS_REVISION_MISMATCH;
        goto Exit;
    }

    RtlZeroMemory(&page->Command, sizeof(page->Command));
    RtlZeroMemory(&page->Result, sizeof(page->Result));
    page->Command.CommandId = CommandId;
    page->Command.Version = LMV_CPUID_PROTOCOL_VERSION;
    page->Command.Size = sizeof(LMV_SHARED_COMMAND);
    page->Command.Sequence = sequence;
    page->Command.Argument0 = GuestPhysicalAddress;
    page->Command.Argument1 = ExtraArgument;
    page->Command.Argument2 = Argument2;
    page->Command.Argument3 = Argument3;
    page->Command.Flags = Flags;
    KeMemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&page->State), LMV_SHARED_STATE_READY);

    LmvGuestCpuidCall(LmvCpuidCommandNotifySharedCommand, sequence, &notifyResult);
    KeMemoryBarrier();
    if (page->State != LMV_SHARED_STATE_COMPLETE ||
        page->Result.State != LMV_SHARED_STATE_COMPLETE ||
        page->Result.Version != LMV_CPUID_PROTOCOL_VERSION ||
        page->Result.Size != sizeof(LMV_SHARED_RESULT) ||
        page->Result.Sequence != sequence ||
        notifyResult.Sequence != sequence)
    {
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    RtlCopyMemory(Result, &page->Result, sizeof(*Result));
    status = page->Result.Status == LMV_CPUID_RESULT_SUCCESS &&
             notifyResult.Status == LMV_CPUID_RESULT_SUCCESS ?
        STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&page->State), LMV_SHARED_STATE_FREE);

Exit:
    if (page != nullptr)
    {
        MmUnmapIoSpace(page, PAGE_SIZE);
    }
    ExReleaseFastMutex(&g_CommandMutex);
    return status;
}

static NTSTATUS
LmvGuestIssueDebugHookCommand(
    _In_ ULONG CommandId,
    _In_ PLMV_DBG_NPT_HOOK_REQUEST HookRequest,
    _Out_ PLMV_SHARED_RESULT Result
    )
{
    LMV_CPUID_SHARED_PAGE_RESULT pageResult = {};
    LMV_CPUID_RESULT notifyResult = {};
    PHYSICAL_ADDRESS pagePa = {};
    PLMV_SHARED_PAGE page = nullptr;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    ULONG sequence = static_cast<ULONG>(InterlockedIncrement(&g_Sequence));

    if (HookRequest == nullptr || Result == nullptr)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Result, sizeof(*Result));
    ExAcquireFastMutex(&g_CommandMutex);
    LmvGuestQuerySharedPage(&pageResult);
    if (pageResult.Status != LMV_CPUID_RESULT_SUCCESS ||
        pageResult.Magic != LMV_SHARED_PAGE_MAGIC)
        goto Exit;

    pagePa.LowPart = pageResult.GpaLow;
    pagePa.HighPart = pageResult.GpaHigh;
    if ((pagePa.QuadPart & (PAGE_SIZE - 1)) != 0)
    {
        status = STATUS_DATATYPE_MISALIGNMENT;
        goto Exit;
    }

    page = static_cast<PLMV_SHARED_PAGE>(MmMapIoSpace(pagePa, PAGE_SIZE, MmCached));
    if (page == nullptr)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    if (page->Magic != LMV_SHARED_PAGE_MAGIC ||
        page->Version != LMV_CPUID_PROTOCOL_VERSION ||
        page->Size != sizeof(LMV_SHARED_PAGE) ||
        (page->State != LMV_SHARED_STATE_FREE &&
         page->State != LMV_SHARED_STATE_COMPLETE))
    {
        status = STATUS_REVISION_MISMATCH;
        goto Exit;
    }

    RtlCopyMemory(&page->DebugHookRequest, HookRequest, sizeof(*HookRequest));
    RtlZeroMemory(&page->Command, sizeof(page->Command));
    RtlZeroMemory(&page->Result, sizeof(page->Result));
    page->Command.CommandId = CommandId;
    page->Command.Version = LMV_CPUID_PROTOCOL_VERSION;
    page->Command.Size = sizeof(LMV_SHARED_COMMAND);
    page->Command.Sequence = sequence;
    page->Command.Argument0 = HookRequest->ProcessId;
    page->Command.Argument1 = HookRequest->GuestPhysicalAddress;
    if (CommandId == LmvDebugCommandArmExecProbe)
    {
        RtlCopyMemory(&page->Command.Argument2, HookRequest->PatchBytes, sizeof(UINT64));
        RtlCopyMemory(&page->Command.Argument3, HookRequest->PatchBytes + sizeof(UINT64), sizeof(UINT64));
    }
    KeMemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&page->State), LMV_SHARED_STATE_READY);

    LmvGuestCpuidCall(LmvCpuidCommandNotifySharedCommand, sequence, &notifyResult);
    KeMemoryBarrier();
    if (page->State != LMV_SHARED_STATE_COMPLETE ||
        page->Result.State != LMV_SHARED_STATE_COMPLETE ||
        page->Result.Version != LMV_CPUID_PROTOCOL_VERSION ||
        page->Result.Size != sizeof(LMV_SHARED_RESULT) ||
        page->Result.Sequence != sequence ||
        notifyResult.Sequence != sequence)
    {
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    RtlCopyMemory(Result, &page->Result, sizeof(*Result));
    status = page->Result.Status == LMV_CPUID_RESULT_SUCCESS &&
             notifyResult.Status == LMV_CPUID_RESULT_SUCCESS ?
        STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&page->State), LMV_SHARED_STATE_FREE);

Exit:
    if (page != nullptr)
        MmUnmapIoSpace(page, PAGE_SIZE);
    ExReleaseFastMutex(&g_CommandMutex);
    return status;
}
typedef struct _LMV_FLUSH_CONTEXT
{
    volatile LONG Seen;
    volatile LONG Failed;
} LMV_FLUSH_CONTEXT, *PLMV_FLUSH_CONTEXT;

static ULONG_PTR NTAPI
LmvGuestFlushProcessor(_In_ ULONG_PTR Context)
{
    PLMV_FLUSH_CONTEXT flush = reinterpret_cast<PLMV_FLUSH_CONTEXT>(Context);
    LMV_CPUID_RESULT result = {};
    LmvGuestCpuidCall(LmvCpuidCommandFlushNptTlb, 0, &result);
    if (result.Status != LMV_CPUID_RESULT_SUCCESS)
    {
        InterlockedIncrement(&flush->Failed);
    }
    InterlockedIncrement(&flush->Seen);
    return 0;
}

static BOOLEAN
LmvGuestFlushAllProcessors(_Out_ PULONG Seen, _Out_ PULONG Expected)
{
    LMV_FLUSH_CONTEXT flush = {};
    *Expected = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    KeIpiGenericCall(LmvGuestFlushProcessor, reinterpret_cast<ULONG_PTR>(&flush));
    *Seen = static_cast<ULONG>(flush.Seen);
    return flush.Failed == 0 && *Seen == *Expected;
}

static NTSTATUS
LmvGuestRunNptSmokeTest(_Out_ PLMV_GUEST_NPT_SMOKE_RESULT Output)
{
    PHYSICAL_ADDRESS lowest = {};
    PHYSICAL_ADDRESS highest = {};
    PHYSICAL_ADDRESS boundary = {};
    LMV_SHARED_RESULT result = {};
    const UINT64 originalValue = 0x1122334455667788ull;
    const UINT64 mappedValue = 0x8877665544332211ull;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    BOOLEAN registered = FALSE;
    BOOLEAN mapped = FALSE;
    BOOLEAN safeToFree = TRUE;
    volatile UINT64* testPage = nullptr;

    Output->Version = LMV_CPUID_PROTOCOL_VERSION;
    Output->Size = sizeof(*Output);
    Output->Status = static_cast<ULONG>(STATUS_UNSUCCESSFUL);
    ExAcquireFastMutex(&g_SmokeMutex);
    highest.QuadPart = 0x000000ffffffffffll;
    testPage = static_cast<volatile UINT64*>(
        MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lowest, highest,
                                               boundary, MmCached));
    if (testPage == nullptr)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    *testPage = originalValue;
    Output->OriginalValue = originalValue;
    Output->GuestPhysicalAddress = MmGetPhysicalAddress(
        const_cast<UINT64*>(testPage)).QuadPart;
    if ((Output->GuestPhysicalAddress & (PAGE_SIZE - 1)) != 0)
    {
        status = STATUS_INVALID_ADDRESS;
        goto Exit;
    }

    Output->Step = 1;
    status = LmvGuestIssueSharedCommand(LmvNptCommandRegisterPage,
                                        Output->GuestPhysicalAddress, &result);
    if (!NT_SUCCESS(status)) goto Exit;
    registered = TRUE;

    Output->Step = 2;
    status = LmvGuestIssueSharedCommand(LmvNptCommandRemapPage,
                                        Output->GuestPhysicalAddress, &result);
    if (!NT_SUCCESS(status)) goto Exit;
    mapped = TRUE;
    Output->HostPhysicalAddress = result.Value1;
    if (Output->HostPhysicalAddress == 0 ||
        Output->HostPhysicalAddress == Output->GuestPhysicalAddress)
    {
        status = STATUS_DATA_ERROR;
        goto Exit;
    }

    Output->Step = 3;
    if (!LmvGuestFlushAllProcessors(&Output->ProcessorsSeen,
                                    &Output->ProcessorsExpected))
    {
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }
    *testPage = mappedValue;
    status = LmvGuestIssueSharedCommand(LmvNptCommandReadQword,
                                        Output->GuestPhysicalAddress, &result);
    Output->MappedValue = result.Value0;
    if (!NT_SUCCESS(status) || Output->MappedValue != mappedValue ||
        result.Value1 != Output->HostPhysicalAddress)
    {
        status = STATUS_DATA_ERROR;
        goto Exit;
    }

    Output->Step = 4;
    status = LmvGuestIssueSharedCommand(LmvNptCommandRestorePage,
                                        Output->GuestPhysicalAddress, &result);
    if (!NT_SUCCESS(status)) goto Exit;
    mapped = FALSE;
    registered = FALSE;
    if (!LmvGuestFlushAllProcessors(&Output->ProcessorsSeen,
                                    &Output->ProcessorsExpected))
    {
        safeToFree = FALSE;
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }
    Output->RestoredValue = *testPage;
    if (Output->RestoredValue != originalValue)
    {
        status = STATUS_DATA_ERROR;
        goto Exit;
    }

    Output->Step = 5;
    status = LmvGuestIssueSharedCommand(LmvNptCommandReleaseRegion,
                                        Output->GuestPhysicalAddress, &result);
    if (NT_SUCCESS(status))
    {
        Output->Step = 6;
    }

Exit:
    if (mapped || registered)
    {
        NTSTATUS restoreStatus = LmvGuestIssueSharedCommand(
            LmvNptCommandRestorePage, Output->GuestPhysicalAddress, &result);
        if (NT_SUCCESS(restoreStatus))
        {
            mapped = FALSE;
            registered = FALSE;
            if (!LmvGuestFlushAllProcessors(&Output->ProcessorsSeen,
                                            &Output->ProcessorsExpected))
            {
                safeToFree = FALSE;
            }
            LmvGuestIssueSharedCommand(LmvNptCommandReleaseRegion,
                                       Output->GuestPhysicalAddress, &result);
        }
        else
        {
            safeToFree = FALSE;
        }
    }
    if (testPage != nullptr && safeToFree)
    {
        MmFreeContiguousMemory(const_cast<UINT64*>(testPage));
    }
    if (!safeToFree)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverGuest] NPT restore/flush failed; test page retained, revert VM snapshot before unloading Host.\n");
    }
    Output->Status = status;
    ExReleaseFastMutex(&g_SmokeMutex);
    return status;
}

typedef struct _LMV_HOOK_READ_CONTEXT
{
    volatile ULONGLONG* Page;
    ULONGLONG ExpectedValue;
    ULONG Offset;
    volatile LONG Seen;
    volatile LONG Mismatches;
} LMV_HOOK_READ_CONTEXT, *PLMV_HOOK_READ_CONTEXT;

static ULONG_PTR NTAPI
LmvGuestCheckHookReadOnProcessor(_In_ ULONG_PTR Context)
{
    PLMV_HOOK_READ_CONTEXT check = reinterpret_cast<PLMV_HOOK_READ_CONTEXT>(Context);
    const ULONGLONG observed = check->Page[check->Offset];
    if (observed != check->ExpectedValue)
        InterlockedIncrement(&check->Mismatches);
    InterlockedIncrement(&check->Seen);
    return 0;
}
static NTSTATUS
LmvGuestRunDebugHookSmoke(_Out_ PLMV_GUEST_DEBUG_HOOK_SMOKE_RESULT Output,
                          _In_ BOOLEAN RequireOriginalRead)
{
    PHYSICAL_ADDRESS lowest = {};
    PHYSICAL_ADDRESS highest = {};
    PHYSICAL_ADDRESS boundary = {};
    LMV_DBG_NPT_HOOK_REQUEST request = {};
    LMV_SHARED_RESULT result = {};
    const ULONGLONG original = 0x1122334455667788ull;
    const ULONGLONG patched = 0x8877665544332211ull;
    const ULONG patchOffset = 32;
    volatile ULONGLONG* testPage = nullptr;
    BOOLEAN attached = FALSE;
    BOOLEAN hooked = FALSE;
    BOOLEAN safeToFree = TRUE;
    BOOLEAN originalReadFailed = FALSE;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    LMV_HOOK_READ_CONTEXT check = {};

    Output->Version = LMV_CPUID_PROTOCOL_VERSION;
    Output->Size = sizeof(*Output);
    Output->OriginalValue = original;
    ExAcquireFastMutex(&g_SmokeMutex);
    lowest.QuadPart = 0;
    highest.QuadPart = MAXLONGLONG;
    boundary.QuadPart = 0;
    testPage = static_cast<volatile ULONGLONG*>(MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, lowest, highest, boundary, MmCached));
    if (testPage == nullptr)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    RtlZeroMemory(const_cast<ULONGLONG*>(testPage), PAGE_SIZE);
    testPage[patchOffset / sizeof(ULONGLONG)] = original;
    Output->GuestPhysicalAddress = MmGetPhysicalAddress(const_cast<ULONGLONG*>(testPage)).QuadPart;
    if ((Output->GuestPhysicalAddress & (PAGE_SIZE - 1)) != 0)
    {
        status = STATUS_DATATYPE_MISALIGNMENT;
        goto Exit;
    }
    Output->Step = 1;
    request.ProcessId = HandleToULong(PsGetCurrentProcessId());
    request.Address = reinterpret_cast<ULONGLONG>(testPage);
    request.GuestPhysicalAddress = Output->GuestPhysicalAddress;
    request.PatchOffset = patchOffset;
    request.PatchLength = sizeof(patched);
    RtlCopyMemory(request.PatchBytes, &patched, sizeof(patched));
    status = LmvGuestIssueSharedCommand(LmvDebugCommandAttachProcess, request.ProcessId, &result);
    if (!NT_SUCCESS(status))
        goto Exit;
    attached = TRUE;
    Output->Step = 2;

    status = LmvGuestIssueDebugHookCommand(LmvDebugCommandInstallNptHook, &request, &result);
    if (!NT_SUCCESS(status))
        goto Exit;
    hooked = TRUE;
    Output->OriginalHostPhysicalAddress = result.Value2;
    Output->ShadowHostPhysicalAddress = result.Value3;
    if (Output->ShadowHostPhysicalAddress == 0 ||
        Output->ShadowHostPhysicalAddress == Output->OriginalHostPhysicalAddress)
    {
        status = STATUS_DATA_ERROR;
        goto Exit;
    }
    Output->Step = 3;
    if (!LmvGuestFlushAllProcessors(&Output->ProcessorsSeen, &Output->ProcessorsExpected))
    {
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }
    Output->PatchedValue = testPage[patchOffset / sizeof(ULONGLONG)];
    if (RequireOriginalRead)
    {
        check.Page = testPage;
        check.Offset = patchOffset / sizeof(ULONGLONG);
        check.ExpectedValue = original;
        KeIpiGenericCall(LmvGuestCheckHookReadOnProcessor,
                         reinterpret_cast<ULONG_PTR>(&check));
        if (check.Mismatches != 0 ||
            static_cast<ULONG>(check.Seen) != Output->ProcessorsExpected ||
            Output->PatchedValue != original)
        {
            originalReadFailed = TRUE;
        }
    }    else if (Output->PatchedValue != patched)
    {
        status = STATUS_DATA_ERROR;
        goto Exit;
    }
    Output->Step = 4;

    status = LmvGuestIssueDebugHookCommand(LmvDebugCommandRemoveNptHook, &request, &result);
    if (!NT_SUCCESS(status))
        goto Exit;
    hooked = FALSE;
    status = LmvGuestIssueSharedCommand(LmvDebugCommandReleaseNptHookRegion, 0, &result);
    if (!NT_SUCCESS(status))
        goto Exit;
    if (!LmvGuestFlushAllProcessors(&Output->ProcessorsSeen, &Output->ProcessorsExpected))
    {
        safeToFree = FALSE;
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }
    Output->RestoredValue = testPage[patchOffset / sizeof(ULONGLONG)];
    if (Output->RestoredValue != original)
    {
        status = STATUS_DATA_ERROR;
        goto Exit;
    }
    Output->Step = 5;

Exit:
    if (hooked)
    {
        NTSTATUS removeStatus = LmvGuestIssueDebugHookCommand(
            LmvDebugCommandRemoveNptHook, &request, &result);
        if (NT_SUCCESS(removeStatus))
            removeStatus = LmvGuestIssueSharedCommand(
                LmvDebugCommandReleaseNptHookRegion, 0, &result);
        if (!NT_SUCCESS(removeStatus) ||
            !LmvGuestFlushAllProcessors(&Output->ProcessorsSeen, &Output->ProcessorsExpected))
        {
            safeToFree = FALSE;
            status = NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
        }
    }
    if (attached && safeToFree)
    {
        NTSTATUS detachStatus = LmvGuestIssueSharedCommand(
            LmvDebugCommandDetachProcess, request.ProcessId, &result);
        if (!NT_SUCCESS(detachStatus) && NT_SUCCESS(status))
            status = detachStatus;
    }
    if (testPage != nullptr && safeToFree)
        MmFreeContiguousMemory(const_cast<ULONGLONG*>(testPage));
    if (!safeToFree)
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverGuest] debug hook restore/flush failed; test page retained. Revert VM snapshot.\n");
    Output->Status = originalReadFailed && NT_SUCCESS(status) ? STATUS_DATA_ERROR : status;
    ExReleaseFastMutex(&g_SmokeMutex);
    return status;
}
static NTSTATUS
LmvGuestRunExecProbe(_Inout_ PLMV_GUEST_EXEC_PROBE Probe, _In_ BOOLEAN Forced)
{
    LMV_SHARED_RESULT result = {};
    LMV_DBG_NPT_HOOK_REQUEST request = {};
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    ULONG processId = Forced && Probe->Operation == LMV_EXEC_PROBE_DISARM ?
        g_ExecProbeProcessId : HandleToULong(PsGetCurrentProcessId());
    ExAcquireFastMutex(&g_SmokeMutex);
    if (Probe->Operation == LMV_EXEC_PROBE_ARM &&
        g_KernelProbeStage == 0 && g_ExecProbeMdl == nullptr &&
        Probe->Version == LMV_CPUID_PROTOCOL_VERSION && Probe->Size == sizeof(*Probe) &&
        (Probe->OriginalPage & (PAGE_SIZE - 1)) == 0 &&
        (Probe->MirrorPage & (PAGE_SIZE - 1)) == 0 &&
        Probe->OriginalPage != Probe->MirrorPage && Probe->HookEntry != 0)
    {
        PMDL mdl = IoAllocateMdl(reinterpret_cast<PVOID>(Probe->OriginalPage), PAGE_SIZE,
                                 FALSE, FALSE, nullptr);
        if (mdl == nullptr)
            status = STATUS_INSUFFICIENT_RESOURCES;
        else
        {
            __try
            {
                MmProbeAndLockPages(mdl, UserMode, IoReadAccess);
                status = STATUS_SUCCESS;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                status = GetExceptionCode();
            }
            if (NT_SUCCESS(status))
            {
                Probe->GuestPhysicalAddress =
                    static_cast<ULONGLONG>(MmGetMdlPfnArray(mdl)[0]) << PAGE_SHIFT;
                request.ProcessId = processId;
                request.GuestPhysicalAddress = Probe->GuestPhysicalAddress;
                request.Address = Probe->HookEntry;
                RtlCopyMemory(request.PatchBytes, &Probe->OriginalPage, sizeof(UINT64));
                RtlCopyMemory(request.PatchBytes + sizeof(UINT64),
                              &Probe->MirrorPage, sizeof(UINT64));
                status = LmvGuestIssueSharedCommand(LmvDebugCommandAttachProcess,
                                                    processId, &result);
                if (NT_SUCCESS(status))
                {
                    status = LmvGuestIssueDebugHookCommand(
                        LmvDebugCommandArmExecProbe, &request, &result);
                    if (NT_SUCCESS(status) && result.Status != LMV_CPUID_RESULT_SUCCESS)
                        status = static_cast<NTSTATUS>(result.Status);
                    if (NT_SUCCESS(status))
                    {
                        g_ExecProbeMdl = mdl;
                        g_ExecProbeProcessId = processId;
                        g_ExecProbeStage = 1;
                        mdl = nullptr;
                        if (!LmvGuestFlushAllProcessors(&Probe->ProcessorsSeen,
                                                        &Probe->ProcessorsExpected))
                            status = STATUS_UNSUCCESSFUL;
                    }
                    else
                        LmvGuestIssueSharedCommand(LmvDebugCommandDetachProcess,
                                                   processId, &result);
                }
                if (mdl != nullptr)
                    MmUnlockPages(mdl);
            }
            if (mdl != nullptr)
                IoFreeMdl(mdl);
        }
    }
    else if (Probe->Operation == LMV_EXEC_PROBE_DISARM &&
             g_ExecProbeMdl != nullptr && g_ExecProbeProcessId == processId)
    {
        request.ProcessId = processId;
        status = STATUS_SUCCESS;
        if (g_ExecProbeStage == 1)
        {
            status = LmvGuestIssueDebugHookCommand(
                LmvDebugCommandDisarmExecProbe, &request, &result);
            if (NT_SUCCESS(status) && result.Status == LMV_CPUID_RESULT_SUCCESS)
                g_ExecProbeStage = 2;
        }
        if (NT_SUCCESS(status) && result.Status != LMV_CPUID_RESULT_SUCCESS)
            status = static_cast<NTSTATUS>(result.Status);
        if (NT_SUCCESS(status) && g_ExecProbeStage == 2)
        {
            if (LmvGuestFlushAllProcessors(&Probe->ProcessorsSeen,
                                           &Probe->ProcessorsExpected))
                g_ExecProbeStage = 3;
            else
                status = STATUS_UNSUCCESSFUL;
        }
        if (NT_SUCCESS(status) && g_ExecProbeStage == 3)
        {
            status = LmvGuestIssueSharedCommand(
                LmvDebugCommandReleaseNptHookRegion, 0, &result);
            if (NT_SUCCESS(status) && result.Status == LMV_CPUID_RESULT_SUCCESS)
                g_ExecProbeStage = 4;
            else if (NT_SUCCESS(status))
                status = static_cast<NTSTATUS>(result.Status);
        }
        if (NT_SUCCESS(status) && g_ExecProbeStage == 4)
        {
            MmUnlockPages(g_ExecProbeMdl);
            IoFreeMdl(g_ExecProbeMdl);
            g_ExecProbeMdl = nullptr;
            g_ExecProbeProcessId = 0;
            g_ExecProbeStage = 0;
            LmvGuestIssueSharedCommand(LmvDebugCommandDetachProcess,
                                       processId, &result);
        }
    }
    Probe->Status = status;
    ExReleaseFastMutex(&g_SmokeMutex);
    return status;
}

static BOOLEAN
LmvGuestVerifyKiDispatchPrologue(_Inout_ PLMV_GUEST_KI_DISPATCH_INFO Output)
{
    static const UCHAR prefix[] = {
        0x40, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x81, 0xEC
    };
    static const UCHAR leaRbp[] = { 0x48, 0x8D, 0x6C, 0x24, 0x30 };
    if (Output->BytesRead < 22 ||
        RtlCompareMemory(Output->OriginalBytes, prefix, sizeof(prefix)) != sizeof(prefix) ||
        RtlCompareMemory(Output->OriginalBytes + 17, leaRbp, sizeof(leaRbp)) != sizeof(leaRbp))
        return FALSE;

    ULONG allocation = 0;
    RtlCopyMemory(&allocation, Output->OriginalBytes + 13, sizeof(allocation));
    if (allocation == 0 || allocation > 0x100000)
        return FALSE;

    Output->PrologueVerified = 1;
    Output->JumpPatchBytes = 12;
    Output->ReplayPrologueBytes = 17;
    Output->SubRspAllocation = allocation;
    Output->ResumeAddress = Output->RoutineAddress + Output->ReplayPrologueBytes;
    return TRUE;
}
static NTSTATUS
LmvGuestQueryKiDispatchInfo(_Out_ PLMV_GUEST_KI_DISPATCH_INFO Output)
{
    LMV_SHARED_RESULT result = {};
    NTSTATUS status = LmvGuestResolveKiDispatchException(
        &Output->RoutineAddress, &Output->GuestPhysicalAddress,
        &Output->KernelBase, &Output->KernelImageSize,
        Output->OriginalBytes, RTL_NUMBER_OF(Output->OriginalBytes),
        &Output->BytesRead, &Output->Method);
    Output->PageOffset = static_cast<ULONG>(Output->RoutineAddress & (PAGE_SIZE - 1));
    LmvGuestVerifyKiDispatchPrologue(Output);
    if (!NT_SUCCESS(status) && Output->BytesRead == 0)
    {
        Output->Status = status;
        return status;
    }

    status = LmvGuestIssueSharedCommand(
        LmvDebugCommandTranslateNpt, Output->GuestPhysicalAddress, &result);
    Output->HostPhysicalAddress = result.Value0;
    if (!NT_SUCCESS(status) || result.Status != LMV_CPUID_RESULT_SUCCESS)
    {
        Output->Status = NT_SUCCESS(status) ?
            static_cast<NTSTATUS>(result.Status) : status;
        return Output->Status;
    }
    Output->Status = status;
    return status;
}
static NTSTATUS LmvGuestQueryKiNextPage(_Out_ PLMV_GUEST_KI_NEXT_PAGE Output)
{
    LMV_GUEST_KI_DISPATCH_INFO info = {};
    NTSTATUS status = LmvGuestQueryKiDispatchInfo(&info);
    if (!NT_SUCCESS(status) || info.Status != STATUS_SUCCESS || info.RoutineAddress == 0)
    {
        Output->Status = static_cast<ULONG>(NT_SUCCESS(status) ? STATUS_DATA_ERROR : status);
        return static_cast<NTSTATUS>(Output->Status);
    }
    Output->Version = LMV_CPUID_PROTOCOL_VERSION;
    Output->Size = sizeof(*Output);
    Output->PageAddress = (info.RoutineAddress & ~(static_cast<ULONGLONG>(PAGE_SIZE) - 1)) + PAGE_SIZE;
    if (info.KernelImageSize < PAGE_SIZE ||
        Output->PageAddress < info.KernelBase ||
        Output->PageAddress - info.KernelBase > info.KernelImageSize - PAGE_SIZE)
    {
        Output->Status = static_cast<ULONG>(STATUS_NOT_SUPPORTED);
        return STATUS_NOT_SUPPORTED;
    }
    MM_COPY_ADDRESS source = {};
    source.VirtualAddress = reinterpret_cast<PVOID>(Output->PageAddress);
    SIZE_T copied = 0;
    status = MmCopyMemory(Output->PageBytes, source, PAGE_SIZE, MM_COPY_MEMORY_VIRTUAL, &copied);
    Output->BytesCopied = static_cast<ULONG>(copied);
    if (NT_SUCCESS(status) && copied == PAGE_SIZE)
    {
        Output->Flags = 1;
        ULONGLONG hash = 14695981039346656037ull;
        for (ULONG index = 0; index < PAGE_SIZE; ++index)
            hash = (hash ^ Output->PageBytes[index]) * 1099511628211ull;
        Output->PageHash = hash;
        PLMV_GUEST_KI_PAGE_PREFLIGHT records = static_cast<PLMV_GUEST_KI_PAGE_PREFLIGHT>(
            ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(LMV_GUEST_KI_PAGE_PREFLIGHT), 'rKmL'));
        if (records == nullptr)
            status = STATUS_INSUFFICIENT_RESOURCES;
        else
        {
            RtlZeroMemory(records, sizeof(*records));
            records->KernelBase = info.KernelBase;
            records->KernelImageSize = info.KernelImageSize;
            records->RoutineAddress = info.RoutineAddress;
            records->PageAddress = Output->PageAddress;
            status = LmvGuestFindKiFunctionBounds(records);
            if (NT_SUCCESS(status))
            {
                Output->PageFunctionRecordCount = records->PageFunctionRecordCount;
                Output->PageFunctionRecordTruncated = records->PageFunctionRecordTruncated;
                RtlCopyMemory(Output->PageFunctionRecords, records->PageFunctionRecords,
                    records->PageFunctionRecordCount * sizeof(LMV_KI_PAGE_FUNCTION_RECORD));
                if (Output->PageFunctionRecordTruncated != 0)
                    status = STATUS_BUFFER_OVERFLOW;
            }
            ExFreePoolWithTag(records, 'rKmL');
        }
    }
    else if (NT_SUCCESS(status))
        status = STATUS_PARTIAL_COPY;
    Output->Status = static_cast<ULONG>(status);
    return status;
}

static NTSTATUS LmvGuestQueryKiWindowPage(_Inout_ PLMV_GUEST_KI_WINDOW_PAGE Output)
{
    const LONG offset = Output->PageOffset;
    Output->Version = LMV_CPUID_PROTOCOL_VERSION;
    Output->Size = sizeof(*Output);
    if (offset < -2 || offset > 4)
    {
        Output->Status = static_cast<ULONG>(STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }
    LMV_GUEST_KI_DISPATCH_INFO info = {};
    NTSTATUS status = LmvGuestQueryKiDispatchInfo(&info);
    if (!NT_SUCCESS(status) || info.Status != STATUS_SUCCESS || info.RoutineAddress == 0)
    {
        Output->Status = static_cast<ULONG>(NT_SUCCESS(status) ? STATUS_DATA_ERROR : status);
        return static_cast<NTSTATUS>(Output->Status);
    }
    Output->RoutineAddress = info.RoutineAddress;
    Output->KernelBase = info.KernelBase;
    Output->KernelImageSize = info.KernelImageSize;
    if (info.RoutineAddress < info.KernelBase ||
        info.RoutineAddress - info.KernelBase >= info.KernelImageSize)
    {
        Output->Status = static_cast<ULONG>(STATUS_INVALID_IMAGE_FORMAT);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    const LONGLONG pageRva = static_cast<LONGLONG>(
        (info.RoutineAddress - info.KernelBase) & ~(static_cast<ULONGLONG>(PAGE_SIZE) - 1)) +
        static_cast<LONGLONG>(offset) * PAGE_SIZE;
    if (pageRva < 0 || info.KernelImageSize < PAGE_SIZE ||
        pageRva > static_cast<LONGLONG>(info.KernelImageSize - PAGE_SIZE))
    {
        Output->Status = static_cast<ULONG>(STATUS_NOT_SUPPORTED);
        return STATUS_NOT_SUPPORTED;
    }
    Output->PageAddress = info.KernelBase + pageRva;
    PUCHAR repeat = static_cast<PUCHAR>(
        ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE, 'wKmL'));
    PLMV_GUEST_KI_PAGE_PREFLIGHT records = static_cast<PLMV_GUEST_KI_PAGE_PREFLIGHT>(
        ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(LMV_GUEST_KI_PAGE_PREFLIGHT), 'rKmL'));
    if (repeat == nullptr || records == nullptr)
        status = STATUS_INSUFFICIENT_RESOURCES;
    else
    {
        MM_COPY_ADDRESS source = {};
        source.VirtualAddress = reinterpret_cast<PVOID>(Output->PageAddress);
        SIZE_T copied = 0;
        status = MmCopyMemory(Output->PageBytes, source, PAGE_SIZE,
            MM_COPY_MEMORY_VIRTUAL, &copied);
        Output->BytesCopied = static_cast<ULONG>(copied);
        if (NT_SUCCESS(status) && copied == PAGE_SIZE)
        {
            SIZE_T repeated = 0;
            status = MmCopyMemory(repeat, source, PAGE_SIZE,
                MM_COPY_MEMORY_VIRTUAL, &repeated);
            if (NT_SUCCESS(status) && repeated == PAGE_SIZE)
            {
                if (RtlCompareMemory(repeat, Output->PageBytes, PAGE_SIZE) != PAGE_SIZE)
                    status = STATUS_RETRY;
                else
                {
                    Output->Flags = LMV_KI_PREFLIGHT_STABLE;
                    ULONGLONG hash = 14695981039346656037ull;
                    for (ULONG index = 0; index < PAGE_SIZE; ++index)
                        hash = (hash ^ Output->PageBytes[index]) * 1099511628211ull;
                    Output->PageHash = hash;
                    RtlZeroMemory(records, sizeof(*records));
                    records->KernelBase = info.KernelBase;
                    records->KernelImageSize = info.KernelImageSize;
                    records->RoutineAddress = info.RoutineAddress;
                    records->PageAddress = Output->PageAddress;
                    status = LmvGuestFindKiFunctionBounds(records);
                    if (NT_SUCCESS(status))
                    {
                        Output->PageFunctionRecordCount = records->PageFunctionRecordCount;
                        Output->PageFunctionRecordTruncated = records->PageFunctionRecordTruncated;
                        RtlCopyMemory(Output->PageFunctionRecords, records->PageFunctionRecords,
                            records->PageFunctionRecordCount * sizeof(LMV_KI_PAGE_FUNCTION_RECORD));
                        if (records->PageFunctionRecordTruncated != 0)
                            status = STATUS_BUFFER_OVERFLOW;
                    }
                }
            }
            else if (NT_SUCCESS(status))
                status = STATUS_PARTIAL_COPY;
        }
        else if (NT_SUCCESS(status))
            status = STATUS_PARTIAL_COPY;
    }
    if (repeat != nullptr) ExFreePoolWithTag(repeat, 'wKmL');
    if (records != nullptr) ExFreePoolWithTag(records, 'rKmL');
    Output->Status = static_cast<ULONG>(status);
    return status;
}

static NTSTATUS LmvGuestQueryKiPagePreflight(_Out_ PLMV_GUEST_KI_PAGE_PREFLIGHT Output)
{
    LMV_GUEST_KI_DISPATCH_INFO info = {};
    Output->Version = LMV_CPUID_PROTOCOL_VERSION;
    Output->Size = sizeof(*Output);
    NTSTATUS status = LmvGuestQueryKiDispatchInfo(&info);
    if (!NT_SUCCESS(status) || info.Status != STATUS_SUCCESS ||
        info.RoutineAddress == 0 || info.BytesRead != sizeof(info.OriginalBytes))
    {
        Output->Status = NT_SUCCESS(status) ? STATUS_DATA_ERROR : status;
        return static_cast<NTSTATUS>(Output->Status);
    }
    Output->RoutineAddress = info.RoutineAddress;
    Output->PageAddress = info.RoutineAddress & ~(static_cast<ULONGLONG>(PAGE_SIZE) - 1);
    Output->EntryOffset = static_cast<ULONG>(info.RoutineAddress & (PAGE_SIZE - 1));
    Output->KernelBase = info.KernelBase;
    Output->KernelImageSize = info.KernelImageSize;
    if (info.KernelImageSize < PAGE_SIZE ||
        Output->PageAddress < info.KernelBase ||
        Output->PageAddress - info.KernelBase > info.KernelImageSize - PAGE_SIZE ||
        Output->EntryOffset > PAGE_SIZE - sizeof(info.OriginalBytes))
    {
        Output->Status = static_cast<ULONG>(STATUS_NOT_SUPPORTED);
        return STATUS_NOT_SUPPORTED;
    }
    Output->Flags |= LMV_KI_PREFLIGHT_IMAGE_RANGE;
    Output->GuestPhysicalPage = MmGetPhysicalAddress(
        reinterpret_cast<PVOID>(Output->PageAddress)).QuadPart;
    if (Output->GuestPhysicalPage == 0 ||
        Output->GuestPhysicalPage !=
            (info.GuestPhysicalAddress & ~(static_cast<ULONGLONG>(PAGE_SIZE) - 1)))
    {
        Output->Status = static_cast<ULONG>(STATUS_DATA_ERROR);
        return STATUS_DATA_ERROR;
    }
    Output->HostPhysicalPage =
        info.HostPhysicalAddress & ~(static_cast<ULONGLONG>(PAGE_SIZE) - 1);
    status = LmvGuestFindKiFunctionBounds(Output);
    if (!NT_SUCCESS(status))
    {
        Output->Status = static_cast<ULONG>(status);
        return status;
    }
    if (Output->HostPhysicalPage != 0)
        Output->Flags |= LMV_KI_PREFLIGHT_NPT_TRANSLATED;
    PUCHAR second = static_cast<PUCHAR>(
        ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE, 'pKmL'));
    if (second == nullptr)
    {
        Output->Status = static_cast<ULONG>(STATUS_INSUFFICIENT_RESOURCES);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MM_COPY_ADDRESS source = {};
    source.VirtualAddress = reinterpret_cast<PVOID>(Output->PageAddress);
    SIZE_T firstBytes = 0;
    SIZE_T secondBytes = 0;
    status = MmCopyMemory(Output->PageBytes, source, PAGE_SIZE,
        MM_COPY_MEMORY_VIRTUAL, &firstBytes);
    Output->BytesCopied = static_cast<ULONG>(firstBytes);
    if (NT_SUCCESS(status) && firstBytes == PAGE_SIZE)
    {
        status = MmCopyMemory(second, source, PAGE_SIZE,
            MM_COPY_MEMORY_VIRTUAL, &secondBytes);
    }
    if (NT_SUCCESS(status) && firstBytes == PAGE_SIZE && secondBytes == PAGE_SIZE)
    {
        if (RtlCompareMemory(Output->PageBytes, second, PAGE_SIZE) == PAGE_SIZE)
            Output->Flags |= LMV_KI_PREFLIGHT_STABLE;
        if (info.PrologueVerified != 0 &&
            RtlCompareMemory(Output->PageBytes + Output->EntryOffset,
                             info.OriginalBytes, sizeof(info.OriginalBytes)) ==
                sizeof(info.OriginalBytes))
        {
            Output->Flags |= LMV_KI_PREFLIGHT_PROLOGUE;
            Output->PrologueVerified = 1;
        }
        ULONGLONG hash = 14695981039346656037ull;
        for (ULONG index = 0; index < PAGE_SIZE; ++index)
            hash = (hash ^ Output->PageBytes[index]) * 1099511628211ull;
        Output->PageHash = hash;
        if ((Output->Flags & (LMV_KI_PREFLIGHT_STABLE |
                              LMV_KI_PREFLIGHT_PROLOGUE)) !=
            (LMV_KI_PREFLIGHT_STABLE | LMV_KI_PREFLIGHT_PROLOGUE))
            status = STATUS_RETRY;
    }
    else if (NT_SUCCESS(status))
    {
        status = STATUS_PARTIAL_COPY;
    }
    ExFreePoolWithTag(second, 'pKmL');
    Output->Status = static_cast<ULONG>(status);
    return status;
}

static NTSTATUS
LmvGuestStubTrigger(
    _In_ PLMV_GUEST_STUB_REQUEST Request
    )
{
    if (Request == nullptr || Request->Version != LMV_CPUID_PROTOCOL_VERSION ||
        Request->Size != sizeof(*Request))
        return STATUS_INVALID_PARAMETER;

    LMV_GUEST_STUB_EVENT event = {};
    event.Version = LMV_CPUID_PROTOCOL_VERSION;
    event.Size = sizeof(event);
    event.State = LMV_GUEST_STUB_STATE_PENDING;
    event.ProcessId = Request->ProcessId != 0 ? Request->ProcessId :
        HandleToULong(PsGetCurrentProcessId());
    event.ThreadId = Request->ThreadId != 0 ? Request->ThreadId :
        HandleToULong(PsGetCurrentThreadId());
    event.ExceptionCode = static_cast<ULONG>(STATUS_BREAKPOINT);
    event.ExceptionAddress = reinterpret_cast<ULONGLONG>(_ReturnAddress());
    event.Rax = __readgsqword(0x188);

    ExAcquireFastMutex(&g_StubMutex);
    if (g_StubQueueCount >= LMV_GUEST_STUB_QUEUE_CAPACITY)
    {
        ExReleaseFastMutex(&g_StubMutex);
        return STATUS_BUFFER_OVERFLOW;
    }

    event.Sequence = static_cast<ULONG>(InterlockedIncrement(
        reinterpret_cast<volatile LONG*>(&g_StubSequence)));
    event.Rcx = event.Sequence;
    event.Rdx = event.ProcessId;
    event.R8 = event.ThreadId;
    event.R9 = event.ExceptionAddress;
    event.R10 = g_StubQueueCount;
    event.R11 = KeQueryInterruptTime();
    g_StubQueue[g_StubQueueTail] = event;
    g_StubQueueTail = (g_StubQueueTail + 1) % LMV_GUEST_STUB_QUEUE_CAPACITY;
    ++g_StubQueueCount;
    KeMemoryBarrier();
    ExReleaseFastMutex(&g_StubMutex);
    KeSetEvent(&g_StubEventAvailable, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

static NTSTATUS
LmvGuestStubGetEvent(
    _Out_ PLMV_GUEST_STUB_EVENT Event
    )
{
    if (Event == nullptr)
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&g_StubMutex);
    if (g_StubActive)
    {
        RtlCopyMemory(Event, &g_StubActiveEvent, sizeof(*Event));
        ExReleaseFastMutex(&g_StubMutex);
        return STATUS_SUCCESS;
    }
    if (g_StubQueueCount == 0)
    {
        RtlZeroMemory(Event, sizeof(*Event));
        ExReleaseFastMutex(&g_StubMutex);
        return STATUS_NO_MORE_ENTRIES;
    }

    RtlCopyMemory(&g_StubActiveEvent, &g_StubQueue[g_StubQueueHead],
                  sizeof(g_StubActiveEvent));
    g_StubQueueHead = (g_StubQueueHead + 1) % LMV_GUEST_STUB_QUEUE_CAPACITY;
    --g_StubQueueCount;
    g_StubActive = TRUE;
    RtlCopyMemory(Event, &g_StubActiveEvent, sizeof(*Event));
    if (g_StubQueueCount == 0)
        KeResetEvent(&g_StubEventAvailable);
    ExReleaseFastMutex(&g_StubMutex);
    return STATUS_SUCCESS;
}

static NTSTATUS
LmvGuestStubWaitEvent(
    _In_ PLMV_GUEST_STUB_WAIT_REQUEST Request,
    _Out_ PLMV_GUEST_STUB_EVENT Event
    )
{
    if (Request == nullptr || Event == nullptr ||
        Request->Version != LMV_CPUID_PROTOCOL_VERSION ||
        Request->Size != sizeof(*Request) || Request->TimeoutMilliseconds > 60000)
        return STATUS_INVALID_PARAMETER;

    LARGE_INTEGER timeout = {};
    timeout.QuadPart = -static_cast<LONGLONG>(Request->TimeoutMilliseconds) * 10000LL;
    NTSTATUS status = KeWaitForSingleObject(
        &g_StubEventAvailable, Executive, KernelMode, FALSE, &timeout);
    if (status == STATUS_TIMEOUT)
        return status;
    if (!NT_SUCCESS(status))
        return status;
    return LmvGuestStubGetEvent(Event);
}

static NTSTATUS
LmvGuestStubContinue(
    _Inout_ PLMV_GUEST_STUB_CONTINUE Continue
    )
{
    if (Continue == nullptr || Continue->Version != LMV_CPUID_PROTOCOL_VERSION ||
        Continue->Size != sizeof(*Continue) ||
        Continue->Disposition != LMV_GUEST_STUB_DISPOSITION_CONTINUE)
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&g_StubMutex);
    if (!g_StubActive || Continue->Sequence != g_StubActiveEvent.Sequence)
    {
        ExReleaseFastMutex(&g_StubMutex);
        return STATUS_NOT_FOUND;
    }
    Continue->State = LMV_GUEST_STUB_STATE_CONTINUED;
    g_StubActive = FALSE;
    RtlZeroMemory(&g_StubActiveEvent, sizeof(g_StubActiveEvent));
    if (g_StubQueueCount == 0)
        KeResetEvent(&g_StubEventAvailable);
    ExReleaseFastMutex(&g_StubMutex);
    return STATUS_SUCCESS;
}

static VOID
LmvGuestStubReset()
{
    ExAcquireFastMutex(&g_StubMutex);
    RtlZeroMemory(g_StubQueue, sizeof(g_StubQueue));
    RtlZeroMemory(&g_StubActiveEvent, sizeof(g_StubActiveEvent));
    g_StubQueueHead = 0;
    g_StubQueueTail = 0;
    g_StubQueueCount = 0;
    g_StubActive = FALSE;
    KeResetEvent(&g_StubEventAvailable);
    ExReleaseFastMutex(&g_StubMutex);
}
static NTSTATUS
LmvGuestCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

extern "C" __declspec(noinline) ULONG_PTR LmvGuestKiImageCodeAnchor()
{
    return 0x4c4d5349u;
}

extern "C" ULONGLONG LmvGuestStaticProbe(ULONGLONG Token, ULONGLONG Mode,
    LMV_STATIC_PROBE_SAMPLE* Sample);

extern "C" BOOLEAN LmvGuestStaticProbeDecision(ULONGLONG Token, ULONGLONG Mode,
    LMV_STATIC_PROBE_SAMPLE* Sample)
{
    Sample->Token = Token;
    Sample->Mode = Mode;
    ++Sample->HandlerCalls;
    return Mode == 1;
}

static NTSTATUS LmvGuestRunStaticStubSmoke(_Out_ PLMV_GUEST_STATIC_STUB_SMOKE Output)
{
    constexpr ULONGLONG token = 0x1234;
    constexpr ULONGLONG flagMask = 0xCD5;
    LMV_STATIC_PROBE_SAMPLE handled = {};
    LMV_STATIC_PROBE_SAMPLE continued = {};
    Output->Version = LMV_CPUID_PROTOCOL_VERSION;
    Output->Size = sizeof(*Output);
    Output->HandledResult = LmvGuestStaticProbe(token, 1, &handled);
    Output->ContinuedResult = LmvGuestStaticProbe(token, 0, &continued);
    Output->HandledEntryFlags = handled.EntryFlags;
    Output->HandledExitFlags = handled.ExitFlags;
    Output->ContinuedEntryFlags = continued.EntryFlags;
    Output->ContinuedExitFlags = continued.ExitFlags;
    Output->HandledCalls = handled.HandlerCalls;
    Output->ContinuedCalls = continued.ContinueCalls;
    if (Output->HandledResult != 0x44 || Output->ContinuedResult != token + 1 ||
        handled.Token != token || continued.Token != token ||
        handled.Mode != 1 || continued.Mode != 0 ||
        handled.HandlerCalls != 1 || continued.HandlerCalls != 1 ||
        handled.ContinueCalls != 0 || continued.ContinueCalls != 1 ||
        ((handled.EntryFlags ^ handled.ExitFlags) & flagMask) != 0 ||
        ((continued.EntryFlags ^ continued.ExitFlags) & flagMask) != 0)
    {
        Output->Status = static_cast<ULONG>(STATUS_DATA_ERROR);
        return STATUS_DATA_ERROR;
    }
    Output->Flags = 3;
    Output->Status = static_cast<ULONG>(STATUS_SUCCESS);
    return STATUS_SUCCESS;
}
extern "C" ULONGLONG LmvGuestKernelProbeOriginal(ULONGLONG Token, ULONGLONG Mode,
    LMV_STATIC_PROBE_SAMPLE* Sample);
extern "C" ULONGLONG LmvGuestKernelProbeNonEntry();
extern "C" ULONGLONG LmvGuestKernelProbeMirror();
extern "C" ULONGLONG LmvGuestKernelProbeRedirect(ULONGLONG Token, ULONGLONG Mode,
    LMV_STATIC_PROBE_SAMPLE* Sample);
extern "C" NTSTATUS LmvGuestRunKernelWorkers(ULONGLONG OriginalAddress,
    ULONGLONG OriginalBytes, PULONG Executed, PULONG Mismatches, PULONG Iterations);

static NTSTATUS LmvGuestCleanupKernelProbe(_Out_opt_ PULONG Seen,
    _Out_opt_ PULONG Expected)
{
    LMV_DBG_NPT_HOOK_REQUEST request = {};
    LMV_SHARED_RESULT result = {};
    NTSTATUS status = STATUS_SUCCESS;
    ULONG seen = 0;
    ULONG expected = 0;
    request.ProcessId = g_KernelProbeProcessId;
    if (g_KernelProbeStage == 2)
    {
        status = LmvGuestIssueDebugHookCommand(
            LmvDebugCommandDisarmExecProbe, &request, &result);
        if (!NT_SUCCESS(status))
            return status;
        g_KernelProbeStage = 3;
    }
    if (g_KernelProbeStage == 3)
    {
        if (!LmvGuestFlushAllProcessors(&seen, &expected))
            return STATUS_UNSUCCESSFUL;
        g_KernelProbeStage = 4;
    }
    if (g_KernelProbeStage == 4)
    {
        status = LmvGuestIssueSharedCommand(
            LmvDebugCommandReleaseNptHookRegion, 0, &result);
        if (!NT_SUCCESS(status))
            return status;
        g_KernelProbeStage = 1;
    }
    if (g_KernelProbeStage == 1)
    {
        status = LmvGuestIssueSharedCommand(
            LmvDebugCommandDetachProcess, g_KernelProbeProcessId, &result);
        if (!NT_SUCCESS(status))
            return status;
        g_KernelProbeStage = 0;
        g_KernelProbeProcessId = 0;
    }
    if (Seen != nullptr)
        *Seen = seen;
    if (Expected != nullptr)
        *Expected = expected;
    return STATUS_SUCCESS;
}

static NTSTATUS LmvGuestRunKernelExecSmoke(_Out_ PLMV_GUEST_KERNEL_EXEC_SMOKE Output,
    _In_ BOOLEAN Stress)
{
    constexpr ULONGLONG token = 0x1234;
    LMV_DBG_NPT_HOOK_REQUEST request = {};
    LMV_SHARED_RESULT result = {};
    LMV_STATIC_PROBE_SAMPLE handled = {};
    LMV_STATIC_PROBE_SAMPLE continued = {};
    LMV_STATIC_PROBE_SAMPLE restored = {};
    NTSTATUS status = STATUS_SUCCESS;
    const KIRQL initialIrql = KeGetCurrentIrql();
    ULONG cleanupSeen = 0;
    ULONG cleanupExpected = 0;
    BOOLEAN ownsSession = FALSE;
    Output->Version = LMV_CPUID_PROTOCOL_VERSION;
    Output->Size = sizeof(*Output);
    Output->OriginalAddress = reinterpret_cast<ULONGLONG>(&LmvGuestKernelProbeOriginal);
    Output->MirrorAddress = reinterpret_cast<ULONGLONG>(&LmvGuestKernelProbeMirror);
    Output->RedirectAddress = reinterpret_cast<ULONGLONG>(&LmvGuestKernelProbeRedirect);
    ExAcquireFastMutex(&g_SmokeMutex);
    if (g_KernelProbeStage != 0 || g_ExecProbeMdl != nullptr)
    {
        status = STATUS_DEVICE_BUSY;
        goto Done;
    }
    if (Stress && initialIrql != PASSIVE_LEVEL)
    {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }
    if ((Output->OriginalAddress & (PAGE_SIZE - 1)) != 0 ||
        Output->MirrorAddress != Output->OriginalAddress + PAGE_SIZE ||
        reinterpret_cast<ULONGLONG>(&LmvGuestKernelProbeNonEntry) !=
            Output->OriginalAddress + 0x100 ||
        (Output->RedirectAddress & ~(static_cast<ULONGLONG>(PAGE_SIZE) - 1)) ==
            Output->OriginalAddress ||
        !MmIsAddressValid(reinterpret_cast<PVOID>(Output->OriginalAddress)) ||
        !MmIsAddressValid(reinterpret_cast<PVOID>(Output->MirrorAddress)))
    {
        status = STATUS_NOT_SUPPORTED;
        goto Done;
    }
    Output->GuestPhysicalAddress =
        MmGetPhysicalAddress(reinterpret_cast<PVOID>(Output->OriginalAddress)).QuadPart;
    if (Output->GuestPhysicalAddress == 0 ||
        (Output->GuestPhysicalAddress & (PAGE_SIZE - 1)) != 0 ||
        static_cast<ULONGLONG>(MmGetPhysicalAddress(
            reinterpret_cast<PVOID>(Output->MirrorAddress)).QuadPart) ==
            Output->GuestPhysicalAddress)
    {
        status = STATUS_NOT_SUPPORTED;
        goto Done;
    }
    RtlCopyMemory(&Output->ReadBefore,
        reinterpret_cast<const VOID*>(Output->OriginalAddress), sizeof(Output->ReadBefore));
    if (LmvGuestKernelProbeOriginal(token, 0, &restored) != token + 1 ||
        LmvGuestKernelProbeNonEntry() != 0x33 || restored.HandlerCalls != 0)
    {
        status = STATUS_DATA_ERROR;
        goto Done;
    }
    Output->Step = 1;
    request.ProcessId = HandleToULong(PsGetCurrentProcessId());
    request.GuestPhysicalAddress = Output->GuestPhysicalAddress;
    request.Address = Output->RedirectAddress;
    RtlCopyMemory(request.PatchBytes, &Output->OriginalAddress, sizeof(UINT64));
    RtlCopyMemory(request.PatchBytes + sizeof(UINT64),
                  &Output->MirrorAddress, sizeof(UINT64));
    status = LmvGuestIssueSharedCommand(
        LmvDebugCommandAttachProcess, request.ProcessId, &result);
    if (!NT_SUCCESS(status))
        goto Done;
    g_KernelProbeProcessId = request.ProcessId;
    g_KernelProbeStage = 1;
    ownsSession = TRUE;
    Output->Step = 2;
    status = LmvGuestIssueDebugHookCommand(
        LmvDebugCommandArmExecProbe, &request, &result);
    if (!NT_SUCCESS(status))
        goto Done;
    g_KernelProbeStage = 2;
    Output->Step = 3;
    if (!LmvGuestFlushAllProcessors(&Output->ProcessorsSeen,
                                    &Output->ProcessorsExpected))
    {
        status = STATUS_UNSUCCESSFUL;
        goto Done;
    }
    RtlCopyMemory(&Output->ReadArmed,
        reinterpret_cast<const VOID*>(Output->OriginalAddress), sizeof(Output->ReadArmed));
    Output->HandledResult = LmvGuestKernelProbeOriginal(token, 1, &handled);
    Output->ContinuedResult = LmvGuestKernelProbeOriginal(token, 0, &continued);
    Output->NonEntryResult = LmvGuestKernelProbeNonEntry();
    Output->HandledCalls = handled.HandlerCalls;
    Output->ContinuedCalls = continued.ContinueCalls;
    Output->Step = 4;
    if (Output->ReadArmed != Output->ReadBefore ||
        Output->HandledResult != 0x44 || Output->ContinuedResult != token + 1 ||
        Output->NonEntryResult != 0x33 ||
        handled.HandlerCalls != 1 || handled.ContinueCalls != 0 ||
        continued.HandlerCalls != 1 || continued.ContinueCalls != 1 ||
        handled.Token != token || continued.Token != token ||
        handled.Mode != 1 || continued.Mode != 0 ||
        ((handled.EntryFlags ^ handled.ExitFlags) & 0xCD5) != 0 ||
        ((continued.EntryFlags ^ continued.ExitFlags) & 0xCD5) != 0)
        status = STATUS_DATA_ERROR;
    if (NT_SUCCESS(status) && Stress)
    {
        ExReleaseFastMutex(&g_SmokeMutex);
        status = LmvGuestRunKernelWorkers(Output->OriginalAddress,
            Output->ReadBefore, &Output->ProcessorsExecuted,
            &Output->WorkerMismatches, &Output->Iterations);
        ExAcquireFastMutex(&g_SmokeMutex);
        if (NT_SUCCESS(status) &&
            Output->ProcessorsExecuted != Output->ProcessorsExpected)
            status = STATUS_DATA_ERROR;
    }
Done:
    if (ownsSession && g_KernelProbeStage != 0)
    {
        NTSTATUS cleanupStatus = LmvGuestCleanupKernelProbe(
            &cleanupSeen, &cleanupExpected);
        if (!NT_SUCCESS(cleanupStatus))
            status = cleanupStatus;
        else if (NT_SUCCESS(status) && Output->Step >= 4)
        {
            Output->RestoredResult = LmvGuestKernelProbeOriginal(token, 1, &restored);
            RtlCopyMemory(&Output->ReadRestored,
                reinterpret_cast<const VOID*>(Output->OriginalAddress),
                sizeof(Output->ReadRestored));
            if (Output->RestoredResult != token + 1 || restored.HandlerCalls != 0 ||
                Output->ReadRestored != Output->ReadBefore)
                status = STATUS_DATA_ERROR;
            else
                Output->Step = 5;
        }
    }
    if (Output->ProcessorsExpected != 0 &&
        (cleanupSeen != Output->ProcessorsExpected ||
         cleanupExpected != Output->ProcessorsExpected))
        status = STATUS_UNSUCCESSFUL;
    Output->Status = status;
    Output->RemainingStage = g_KernelProbeStage;
    ExReleaseFastMutex(&g_SmokeMutex);
    return status;
}

static VOID LmvGuestReleaseMirrorStorage()
{
    if (g_KiMirrorMdl != nullptr)
    {
        MmUnmapLockedPages(g_KiMirrorPages, g_KiMirrorMdl);
        MmFreePagesFromMdl(g_KiMirrorMdl);
        ExFreePool(g_KiMirrorMdl);
        g_KiMirrorMdl = nullptr;
    }
    else if (g_KiMirrorAllocation != nullptr)
        ExFreePoolWithTag(g_KiMirrorAllocation, 'mKmL');
    g_KiMirrorAllocation = nullptr;
    g_KiMirrorPages = nullptr;
    g_KiMirrorOwner = nullptr;
}

static NTSTATUS LmvGuestMirrorStorage(_Inout_ PLMV_GUEST_KI_MIRROR_STORAGE Output,
    _In_ PFILE_OBJECT FileObject)
{
    const ULONG operation = Output->Operation;
    const ULONG requested = Output->RequestedPages;
    RtlZeroMemory(Output, sizeof(*Output));
    Output->Version = LMV_CPUID_PROTOCOL_VERSION;
    Output->Size = sizeof(*Output);
    Output->Operation = operation;
    Output->RequestedPages = requested;
    if ((operation != LMV_KI_MIRROR_ALLOCATE &&
         operation != LMV_KI_MIRROR_ALLOCATE_MDL &&
         operation != LMV_KI_MIRROR_RELEASE) ||
        (operation != LMV_KI_MIRROR_RELEASE && requested != 7))
    {
        Output->Status = static_cast<ULONG>(STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }
    NTSTATUS status = STATUS_SUCCESS;
    ExAcquireFastMutex(&g_KiMirrorMutex);
    if (operation == LMV_KI_MIRROR_RELEASE)
    {
        if (g_KiMirrorAllocation != nullptr && g_KiMirrorOwner != FileObject)
            status = STATUS_ACCESS_DENIED;
        else if (g_KiMirrorAllocation != nullptr)
        {
            LmvGuestReleaseMirrorStorage();
        }
    }
    else if (g_KiMirrorAllocation != nullptr)
        status = STATUS_DEVICE_BUSY;
    else
    {
        LMV_GUEST_KI_DISPATCH_INFO info = {};
        status = LmvGuestQueryKiDispatchInfo(&info);
        if (NT_SUCCESS(status) && info.Status == STATUS_SUCCESS &&
            info.RoutineAddress >= info.KernelBase &&
            info.RoutineAddress - info.KernelBase < info.KernelImageSize)
        {
            const ULONGLONG pageRva =
                (info.RoutineAddress - info.KernelBase) & ~(static_cast<ULONGLONG>(PAGE_SIZE) - 1);
            const SIZE_T bytes = 7 * PAGE_SIZE;
            if (pageRva < 2 * PAGE_SIZE ||
                info.KernelImageSize < bytes ||
                pageRva - 2 * PAGE_SIZE > info.KernelImageSize - bytes)
                status = STATUS_NOT_SUPPORTED;
            else
            {
                PMDL mdl = nullptr;
                PVOID allocation = nullptr;
                if (operation == LMV_KI_MIRROR_ALLOCATE_MDL)
                {
                    PHYSICAL_ADDRESS lowest = {};
                    PHYSICAL_ADDRESS highest = {};
                    PHYSICAL_ADDRESS boundary = {};
                    highest.QuadPart = MAXULONG64;
                    mdl = MmAllocatePagesForMdlEx(lowest, highest, boundary, bytes,
                        MmCached, MM_ALLOCATE_FULLY_REQUIRED);
                    if (mdl != nullptr)
                        allocation = MmMapLockedPagesSpecifyCache(mdl, KernelMode,
                            MmCached, nullptr, FALSE,
                            NormalPagePriority);
                }
                else
                    allocation = ExAllocatePoolWithTag(NonPagedPoolNx,
                        bytes + PAGE_SIZE - 1, 'mKmL');
                if (allocation == nullptr)
                {
                    if (mdl != nullptr)
                    {
                        MmFreePagesFromMdl(mdl);
                        ExFreePool(mdl);
                    }
                    status = STATUS_INSUFFICIENT_RESOURCES;
                }
                else
                {
                    PUCHAR destination = reinterpret_cast<PUCHAR>(
                        (reinterpret_cast<ULONG_PTR>(allocation) + PAGE_SIZE - 1) &
                        ~(static_cast<ULONG_PTR>(PAGE_SIZE) - 1));
                    if (mdl != nullptr && destination != allocation)
                        status = STATUS_DATATYPE_MISALIGNMENT;
                    PUCHAR repeat = static_cast<PUCHAR>(
                        ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE, 'vKmL'));
                    if (repeat == nullptr && NT_SUCCESS(status))
                        status = STATUS_INSUFFICIENT_RESOURCES;
                    if (NT_SUCCESS(status))
                    {
                        const ULONGLONG sourceBase = info.KernelBase + pageRva - 2 * PAGE_SIZE;
                        ULONGLONG hash = 14695981039346656037ull;
                        for (ULONG page = 0; page < 7 && NT_SUCCESS(status); ++page)
                        {
                            MM_COPY_ADDRESS source = {};
                            source.VirtualAddress = reinterpret_cast<PVOID>(
                                sourceBase + static_cast<ULONGLONG>(page) * PAGE_SIZE);
                            SIZE_T first = 0, second = 0;
                            status = MmCopyMemory(destination + page * PAGE_SIZE,
                                source, PAGE_SIZE, MM_COPY_MEMORY_VIRTUAL, &first);
                            if (NT_SUCCESS(status) && first == PAGE_SIZE)
                                status = MmCopyMemory(repeat, source, PAGE_SIZE,
                                    MM_COPY_MEMORY_VIRTUAL, &second);
                            if (!NT_SUCCESS(status) || first != PAGE_SIZE || second != PAGE_SIZE)
                                status = STATUS_PARTIAL_COPY;
                            else if (RtlCompareMemory(repeat, destination + page * PAGE_SIZE,
                                PAGE_SIZE) != PAGE_SIZE)
                                status = STATUS_RETRY;
                            else
                            {
                                Output->PhysicalPages[page] = MmGetPhysicalAddress(
                                    destination + page * PAGE_SIZE).QuadPart;
                                if (Output->PhysicalPages[page] == 0 ||
                                    (Output->PhysicalPages[page] & (PAGE_SIZE - 1)) != 0)
                                    status = STATUS_DATA_ERROR;
                                for (ULONG byte = 0; byte < PAGE_SIZE && NT_SUCCESS(status); ++byte)
                                    hash = (hash ^ destination[page * PAGE_SIZE + byte]) *
                                        1099511628211ull;
                            }
                        }
                        if (NT_SUCCESS(status) && mdl != nullptr)
                            status = MmProtectMdlSystemAddress(mdl, PAGE_EXECUTE_READ);
                        if (NT_SUCCESS(status))
                        {
                            g_KiMirrorAllocation = allocation;
                            g_KiMirrorMdl = mdl;
                            g_KiMirrorPages = destination;
                            g_KiMirrorOwner = FileObject;
                            Output->Flags = LMV_KI_MIRROR_FLAG_PHYSICAL_LIST |
                                (mdl != nullptr ? LMV_KI_MIRROR_FLAG_MDL :
                                 LMV_KI_MIRROR_FLAG_STORAGE_ONLY);
                            Output->AllocatedPages = 7;
                            Output->VirtualAddress = reinterpret_cast<ULONGLONG>(destination);
                            Output->ContentHash = hash;
                        }
                    }
                    if (repeat != nullptr)
                        ExFreePoolWithTag(repeat, 'vKmL');
                    if (!NT_SUCCESS(status))
                    {
                        if (mdl != nullptr)
                        {
                            MmUnmapLockedPages(allocation, mdl);
                            MmFreePagesFromMdl(mdl);
                            ExFreePool(mdl);
                        }
                        else
                            ExFreePoolWithTag(allocation, 'mKmL');
                    }
                }
            }
        }
        else if (NT_SUCCESS(status)) status = STATUS_DATA_ERROR;
    }
    ExReleaseFastMutex(&g_KiMirrorMutex);
    Output->Status = static_cast<ULONG>(status);
    return status;
}

__declspec(noinline) static ULONGLONG LmvGuestPrivateDr3Target()
{
    return 0x13579BDF2468ACE0ULL;
}

static VOID LmvGuestPrivateDr3Smoke(PLMV_GUEST_DR3_SMOKE_RESULT output)
{
    PROCESSOR_NUMBER processor = {};
    GROUP_AFFINITY affinity = {};
    GROUP_AFFINITY previous = {};
    LMV_SHARED_RESULT command = {};
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    BOOLEAN armed = FALSE;
    ULONGLONG observed = 0;

    output->Version = LMV_CPUID_PROTOCOL_VERSION;
    output->Size = sizeof(*output);
    output->Status = static_cast<ULONG>(STATUS_UNSUCCESSFUL);
    output->Target = reinterpret_cast<ULONGLONG>(&LmvGuestPrivateDr3Target);
    KeGetCurrentProcessorNumberEx(&processor);
    affinity.Group = processor.Group;
    affinity.Mask = 1ULL << processor.Number;
    KeSetSystemGroupAffinityThread(&affinity, &previous);

    output->OriginalDr3 = __readdr(3);
    output->OriginalDr7 = __readdr(7);
    __writedr(3, output->Target);
    output->GuestDr3Readback = __readdr(3);
    __writedr(3, output->OriginalDr3);
    if (output->GuestDr3Readback != output->Target)
        goto Cleanup;
    output->Step = 1;

    status = LmvGuestIssueSharedCommand(LmvDebugCommandArmPrivateDr3, output->Target, &command);
    if (!NT_SUCCESS(status))
    {
        output->Status = command.Status != 0 ? command.Status : static_cast<ULONG>(status);
        goto Cleanup;
    }
    armed = TRUE;
    output->HitsBefore = command.Value0;
    output->Step = 2;

    __writedr(7, output->OriginalDr7 | 0x40);
    output->GuestDr7Readback = __readdr(7);
    __writedr(7, output->OriginalDr7);
    if (output->GuestDr7Readback != (output->OriginalDr7 | 0x40))
        goto Cleanup;
    output->Step = 3;

    observed = LmvGuestPrivateDr3Target();
    if (observed != 0x13579BDF2468ACE0ULL)
        goto Cleanup;
    output->Step = 4;

    status = LmvGuestIssueSharedCommand(LmvDebugCommandQueryPrivateDr3, 0, &command);
    output->HitsAfter = command.Value0;
    if (!NT_SUCCESS(status) || command.Value1 != output->Target ||
        command.Value0 != output->HitsBefore + 1)
        goto Cleanup;
    output->Step = 5;

Cleanup:
    if (armed)
    {
        status = LmvGuestIssueSharedCommand(LmvDebugCommandDisarmPrivateDr3, 0, &command);
        if (NT_SUCCESS(status))
        {
            output->HitsAfterDisarm = command.Value0;
            armed = FALSE;
        }
    }
    __writedr(7, output->OriginalDr7);
    __writedr(3, output->OriginalDr3);
    if (!armed && output->Step == 5 && output->HitsAfterDisarm == output->HitsAfter)
    {
        observed = LmvGuestPrivateDr3Target();
        status = LmvGuestIssueSharedCommand(LmvDebugCommandQueryPrivateDr3, 0, &command);
        if (NT_SUCCESS(status) && command.Value0 == output->HitsAfter &&
            command.Value1 == 0 && observed == 0x13579BDF2468ACE0ULL)
        {
            output->Step = 7;
            output->Status = LMV_CPUID_RESULT_SUCCESS;
        }
    }
    KeRevertToUserGroupAffinityThread(&previous);
}
static VOID LmvGuestDrCoexistSmoke(
    _Inout_ PLMV_GUEST_DR_COEXIST_RESULT output,
    _In_ KPROCESSOR_MODE requestorMode)
{
    PKEVENT startEvent = nullptr;
    PKEVENT doneEvent = nullptr;
    GROUP_AFFINITY affinity = {};
    GROUP_AFFINITY previous = {};
    PROCESSOR_NUMBER processor = {};
    LMV_SHARED_RESULT command = {};
    BOOLEAN pinned = FALSE;
    BOOLEAN armed = FALSE;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    LARGE_INTEGER timeout = {};
    timeout.QuadPart = -30000000LL;
    HANDLE startHandle = output->StartEvent;
    HANDLE doneHandle = output->DoneEvent;
    output->Status = static_cast<ULONG>(status);
    output->Target = reinterpret_cast<ULONGLONG>(&LmvGuestPrivateDr3Target);
    if (requestorMode != UserMode || startHandle == nullptr || doneHandle == nullptr)
    {
        output->Status = static_cast<ULONG>(STATUS_INVALID_PARAMETER);
        return;
    }
    status = ObReferenceObjectByHandle(startHandle, EVENT_MODIFY_STATE,
        *ExEventObjectType, UserMode, reinterpret_cast<PVOID*>(&startEvent), nullptr);
    if (!NT_SUCCESS(status))
        goto Cleanup;
    status = ObReferenceObjectByHandle(doneHandle, SYNCHRONIZE,
        *ExEventObjectType, UserMode, reinterpret_cast<PVOID*>(&doneEvent), nullptr);
    if (!NT_SUCCESS(status))
        goto Cleanup;

    KeGetCurrentProcessorNumberEx(&processor);
    affinity.Group = processor.Group;
    affinity.Mask = 1ULL << processor.Number;
    KeSetSystemGroupAffinityThread(&affinity, &previous);
    pinned = TRUE;
    status = LmvGuestIssueSharedCommand(LmvDebugCommandArmPrivateDr3, output->Target, &command);
    if (!NT_SUCCESS(status))
    {
        if (command.Status != 0)
            status = static_cast<NTSTATUS>(command.Status);
        goto Cleanup;
    }
    armed = TRUE;
    output->HitsBefore = command.Value0;
    output->Step = 1;
    KeSetEvent(startEvent, IO_NO_INCREMENT, FALSE);
    status = KeWaitForSingleObject(doneEvent, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
        goto Cleanup;
    output->Step = 2;
    status = LmvGuestIssueSharedCommand(LmvDebugCommandQueryPrivateDr3, 0, &command);
    if (!NT_SUCCESS(status) || command.Value1 != output->Target)
    {
        if (NT_SUCCESS(status)) status = STATUS_DATA_ERROR;
        goto Cleanup;
    }
    output->HitsAfterNative = command.Value0;
    if (output->HitsAfterNative != output->HitsBefore)
    {
        status = STATUS_DATA_ERROR;
        goto Cleanup;
    }
    output->Step = 3;
    if (LmvGuestPrivateDr3Target() != 0x13579BDF2468ACE0ULL)
    {
        status = STATUS_DATA_ERROR;
        goto Cleanup;
    }
    status = LmvGuestIssueSharedCommand(LmvDebugCommandQueryPrivateDr3, 0, &command);
    if (!NT_SUCCESS(status) || command.Value1 != output->Target)
    {
        if (NT_SUCCESS(status)) status = STATUS_DATA_ERROR;
        goto Cleanup;
    }
    output->HitsAfterPrivate = command.Value0;
    if (output->HitsAfterPrivate != output->HitsBefore + 1)
    {
        status = STATUS_DATA_ERROR;
        goto Cleanup;
    }
    output->Step = 4;

Cleanup:
    if (armed)
    {
        LMV_SHARED_RESULT disarm = {};
        NTSTATUS disarmStatus = LmvGuestIssueSharedCommand(
            LmvDebugCommandDisarmPrivateDr3, 0, &disarm);
        output->HitsAfterDisarm = disarm.Value0;
        if (!NT_SUCCESS(disarmStatus))
            status = disarm.Status != 0 ? static_cast<NTSTATUS>(disarm.Status) : disarmStatus;
        else if (NT_SUCCESS(status) && output->HitsAfterDisarm == output->HitsAfterPrivate &&
            output->Step == 4)
        {
            if (LmvGuestPrivateDr3Target() == 0x13579BDF2468ACE0ULL)
            {
                LMV_SHARED_RESULT query = {};
                NTSTATUS queryStatus = LmvGuestIssueSharedCommand(
                    LmvDebugCommandQueryPrivateDr3, 0, &query);
                output->HitsAfterRestore = query.Value0;
                if (NT_SUCCESS(queryStatus) && query.Value1 == 0 &&
                    output->HitsAfterRestore == output->HitsAfterDisarm)
                    output->Step = 6;
                else
                    status = STATUS_DATA_ERROR;
            }
            else
                status = STATUS_DATA_ERROR;
        }
        else if (NT_SUCCESS(status))
            status = STATUS_DATA_ERROR;
    }
    if (pinned)
        KeRevertToUserGroupAffinityThread(&previous);
    if (doneEvent != nullptr)
        ObDereferenceObject(doneEvent);
    if (startEvent != nullptr)
        ObDereferenceObject(startEvent);
    output->Status = static_cast<ULONG>(status);
}
static VOID LmvGuestKiDr3RefreshAvailable()
{
    for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
    {
        if (g_KiDr3Events[index].State == LMV_GUEST_STUB_STATE_PENDING &&
            g_KiDr3Events[index].Consumer == nullptr)
        {
            KeSetEvent(&g_KiDr3EventAvailable, IO_NO_INCREMENT, FALSE);
            return;
        }
    }
    KeClearEvent(&g_KiDr3EventAvailable);
}

static VOID LmvGuestKiDr3CancelEvents(_In_opt_ PFILE_OBJECT Consumer)
{
    for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
    {
        LMV_KI_DR3_EVENT_SLOT& slot = g_KiDr3Events[index];
        if (slot.State == LMV_GUEST_STUB_STATE_PENDING &&
            (Consumer == nullptr || slot.Consumer == Consumer))
        {
            slot.State = kKiDr3EventCancelled;
            KeSetEvent(&slot.Completed, IO_NO_INCREMENT, FALSE);
        }
    }
    LmvGuestKiDr3RefreshAvailable();
}

static NTSTATUS LmvGuestKiDr3WaitForContinue(
    _Inout_ LMV_KI_INSPECT_STATE* Captured,
    _Out_ PULONG Sequence,
    _Out_opt_ PBOOLEAN RaxModified,
    _Out_opt_ PBOOLEAN ContextModified,
    _Out_opt_ PBOOLEAN Handled,
    _In_ BOOLEAN Persistent = FALSE)
{
    if (RaxModified != nullptr)
        *RaxModified = FALSE;
    if (ContextModified != nullptr)
        *ContextModified = FALSE;
    if (Handled != nullptr)
        *Handled = FALSE;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || (__readeflags() & 0x200) == 0)
        return STATUS_INVALID_DEVICE_STATE;

    ExAcquireFastMutex(&g_KiDr3EventMutex);
    if (InterlockedCompareExchange(&g_KiDr3WaitEnabled, 0, 0) == 0 ||
        (Persistent ?
            (InterlockedCompareExchange(&g_KiSessionActive, 0, 0) == 0 ||
             g_KiSessionProcess != PsGetCurrentProcess()) :
            g_KiInspectTargetProcessId != HandleToULong(PsGetCurrentProcessId())))
    {
        ExReleaseFastMutex(&g_KiDr3EventMutex);
        return STATUS_DEVICE_NOT_READY;
    }
    LMV_KI_DR3_EVENT_SLOT* slot = nullptr;
    if (Persistent)
    {
        for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
        {
            if (g_KiDr3Events[index].State != LMV_GUEST_STUB_STATE_IDLE &&
                g_KiDr3Events[index].Event.ThreadId == Captured->ThreadId)
            {
                ExReleaseFastMutex(&g_KiDr3EventMutex);
                return STATUS_DEVICE_BUSY;
            }
        }
    }
    for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
    {
        if (g_KiDr3Events[index].State == LMV_GUEST_STUB_STATE_IDLE)
        {
            slot = &g_KiDr3Events[index];
            break;
        }
    }
    if (slot == nullptr)
    {
        ExReleaseFastMutex(&g_KiDr3EventMutex);
        return STATUS_BUFFER_OVERFLOW;
    }
    RtlZeroMemory(&slot->Event, sizeof(slot->Event));
    slot->Event.Version = LMV_CPUID_PROTOCOL_VERSION;
    slot->Event.Size = sizeof(slot->Event);
    slot->Event.State = LMV_GUEST_STUB_STATE_PENDING;
    slot->Event.Sequence = static_cast<ULONG>(InterlockedIncrement(&g_KiDr3EventSequence));
    slot->Event.ProcessId = Captured->ProcessId;
    slot->Event.ThreadId = Captured->ThreadId;
    slot->Event.ExceptionCode = Captured->ExceptionCode;
    slot->Event.ExceptionAddress = Captured->ExceptionAddress;
    slot->Event.Frame = Captured->Frame;
    slot->Consumer = nullptr;
    slot->RealFrame = RaxModified != nullptr &&
        Captured->FrameStatus == static_cast<ULONG>(STATUS_SUCCESS);
    slot->WriteRax = FALSE;
    slot->Handled = FALSE;
    slot->WriteRip = FALSE;
    slot->WriteContext = FALSE;
    slot->Persistent = Persistent;
    slot->State = LMV_GUEST_STUB_STATE_PENDING;
    *Sequence = slot->Event.Sequence;
    KeClearEvent(&slot->Completed);
    LmvGuestKiDr3RefreshAvailable();
    ExReleaseFastMutex(&g_KiDr3EventMutex);

    LARGE_INTEGER timeout = {};
    timeout.QuadPart = -20000000LL;
    NTSTATUS status = KeWaitForSingleObject(&slot->Completed,
        Executive, KernelMode, FALSE, Persistent ? nullptr : &timeout);
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    if (slot->State == LMV_GUEST_STUB_STATE_CONTINUED)
    {
        if (slot->WriteRax && RaxModified != nullptr)
        {
            Captured->Frame.Rax = slot->Event.Frame.Rax;
            *RaxModified = TRUE;
        }
        if (slot->WriteContext && ContextModified != nullptr)
        {
            Captured->Frame = slot->Event.Frame;
            *ContextModified = TRUE;
        }
        if (slot->Handled && Handled != nullptr && (slot->Persistent || slot->WriteRip))
        {
            Captured->Frame.Rip = slot->Event.Frame.Rip;
            *Handled = TRUE;
        }
        status = STATUS_SUCCESS;
    }
    else if (Persistent || status == STATUS_SUCCESS)
        status = STATUS_CANCELLED;
    slot->State = LMV_GUEST_STUB_STATE_IDLE;
    slot->Consumer = nullptr;
    slot->RealFrame = FALSE;
    slot->WriteRax = FALSE;
    slot->Handled = FALSE;
    slot->WriteRip = FALSE;
    slot->WriteContext = FALSE;
    slot->Persistent = FALSE;
    RtlZeroMemory(&slot->Event, sizeof(slot->Event));
    LmvGuestKiDr3RefreshAvailable();
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    return status;
}

static NTSTATUS LmvGuestKiDr3WaitEvent(
    _In_ PLMV_GUEST_STUB_WAIT_REQUEST Request,
    _Out_ PLMV_GUEST_KI_DR3_EVENT Event,
    _In_ PFILE_OBJECT FileObject)
{
    if (Request->Version != LMV_CPUID_PROTOCOL_VERSION ||
        Request->Size != sizeof(*Request) || Request->TimeoutMilliseconds > 5000)
        return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    const BOOLEAN owned = g_KiDr3EventOwner != nullptr &&
        g_KiDr3ControllerProcessId == HandleToULong(PsGetCurrentProcessId());
    const ULONG generation = g_KiDr3EventGeneration;
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    if (!owned)
        return STATUS_ACCESS_DENIED;
    LARGE_INTEGER timeout = {};
    timeout.QuadPart = -static_cast<LONGLONG>(Request->TimeoutMilliseconds) * 10000LL;
    NTSTATUS status = KeWaitForSingleObject(&g_KiDr3EventAvailable,
        Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
        return status == STATUS_TIMEOUT ? STATUS_IO_TIMEOUT : status;
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    if (g_KiDr3EventOwner == nullptr ||
        g_KiDr3EventGeneration != generation ||
        g_KiDr3ControllerProcessId != HandleToULong(PsGetCurrentProcessId()))
        status = STATUS_CANCELLED;
    else
    {
        LMV_KI_DR3_EVENT_SLOT* pending = nullptr;
        BOOLEAN busy = FALSE;
        for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
        {
            LMV_KI_DR3_EVENT_SLOT& slot = g_KiDr3Events[index];
            if (slot.State != LMV_GUEST_STUB_STATE_PENDING)
                continue;
            if (slot.Consumer != nullptr)
                busy = TRUE;
            else if (pending == nullptr || slot.Event.Sequence < pending->Event.Sequence)
                pending = &slot;
        }
        if (pending != nullptr)
        {
            pending->Consumer = FileObject;
            *Event = pending->Event;
            LmvGuestKiDr3RefreshAvailable();
        }
        else
            status = busy ? STATUS_SHARING_VIOLATION : STATUS_NOT_FOUND;
    }
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    return status;
}

static NTSTATUS LmvGuestKiDr3Continue(
    _Inout_ PLMV_GUEST_STUB_CONTINUE Continue,
    _In_ PFILE_OBJECT FileObject)
{
    if (Continue->Version != LMV_CPUID_PROTOCOL_VERSION ||
        Continue->Size != sizeof(*Continue) ||
        (Continue->Disposition != LMV_GUEST_STUB_DISPOSITION_CONTINUE &&
         Continue->Disposition != LMV_GUEST_STUB_DISPOSITION_HANDLED))
        return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    LMV_KI_DR3_EVENT_SLOT* slot = nullptr;
    if (g_KiDr3EventOwner != nullptr &&
        g_KiDr3ControllerProcessId == HandleToULong(PsGetCurrentProcessId()))
    {
        for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
        {
            if (g_KiDr3Events[index].State == LMV_GUEST_STUB_STATE_PENDING &&
                g_KiDr3Events[index].Consumer == FileObject &&
                g_KiDr3Events[index].Event.Sequence == Continue->Sequence)
            {
                slot = &g_KiDr3Events[index];
                break;
            }
        }
    }
    const BOOLEAN valid = slot != nullptr &&
        (slot->Persistent || Continue->Disposition != LMV_GUEST_STUB_DISPOSITION_HANDLED ||
         (InterlockedCompareExchange(&g_KiDr3HandleEnabled, 0, 0) != 0 &&
          slot->RealFrame && slot->WriteRip));
    if (valid)
    {
        slot->Handled = slot->Persistent || Continue->Disposition == LMV_GUEST_STUB_DISPOSITION_HANDLED;
        slot->State = LMV_GUEST_STUB_STATE_CONTINUED;
        Continue->State = LMV_GUEST_STUB_STATE_CONTINUED;
        KeSetEvent(&slot->Completed, IO_NO_INCREMENT, FALSE);
    }
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    return valid ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

static NTSTATUS LmvGuestKiDr3SetRax(
    _In_ PLMV_GUEST_KI_DR3_SET_RAX Request,
    _In_ PFILE_OBJECT FileObject)
{
    if (Request->Version != LMV_CPUID_PROTOCOL_VERSION ||
        Request->Size != sizeof(*Request))
        return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    LMV_KI_DR3_EVENT_SLOT* slot = nullptr;
    if (InterlockedCompareExchange(&g_KiDr3RaxWriteEnabled, 0, 0) != 0 &&
        g_KiDr3EventOwner != nullptr &&
        g_KiDr3ControllerProcessId == HandleToULong(PsGetCurrentProcessId()))
    {
        for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
        {
            if (g_KiDr3Events[index].State == LMV_GUEST_STUB_STATE_PENDING &&
                g_KiDr3Events[index].RealFrame &&
                g_KiDr3Events[index].Consumer == FileObject &&
                g_KiDr3Events[index].Event.Sequence == Request->Sequence)
            {
                slot = &g_KiDr3Events[index];
                break;
            }
        }
    }
    if (slot != nullptr)
    {
        slot->Event.Frame.Rax = Request->Rax;
        slot->WriteRax = TRUE;
    }
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    return slot != nullptr ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

static NTSTATUS LmvGuestKiDr3SetRip(
    _In_ PLMV_GUEST_KI_DR3_SET_RIP Request,
    _In_ PFILE_OBJECT FileObject)
{
    if (Request->Version != LMV_CPUID_PROTOCOL_VERSION ||
        Request->Size != sizeof(*Request) || Request->Reserved != 0)
        return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    LMV_KI_DR3_EVENT_SLOT* slot = nullptr;
    if (InterlockedCompareExchange(&g_KiDr3HandleEnabled, 0, 0) != 0 &&
        g_KiDr3EventOwner != nullptr &&
        g_KiDr3ControllerProcessId == HandleToULong(PsGetCurrentProcessId()))
    {
        for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
        {
            LMV_KI_DR3_EVENT_SLOT& candidate = g_KiDr3Events[index];
            if (candidate.State == LMV_GUEST_STUB_STATE_PENDING &&
                candidate.RealFrame && candidate.Consumer == FileObject &&
                candidate.Event.Sequence == Request->Sequence &&
                candidate.Event.ExceptionCode == 0x10000002u &&
                candidate.Event.Frame.Rip < 0x0000800000000000ull - 2 &&
                Request->Rip == candidate.Event.Frame.Rip + 2)
            {
                slot = &candidate;
                break;
            }
        }
    }
    if (slot != nullptr)
    {
        MM_COPY_ADDRESS address = {};
        UCHAR opcode[2] = {};
        SIZE_T copied = 0;
        address.VirtualAddress = reinterpret_cast<PVOID>(slot->Event.Frame.Rip);
        NTSTATUS status = MmCopyMemory(opcode, address, sizeof(opcode),
            MM_COPY_MEMORY_VIRTUAL, &copied);
        if (NT_SUCCESS(status) && copied == sizeof(opcode) &&
            opcode[0] == 0x0f && opcode[1] == 0x0b)
        {
            slot->Event.Frame.Rip = Request->Rip;
            slot->WriteRip = TRUE;
        }
        else
            slot = nullptr;
    }
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    return slot != nullptr ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

static NTSTATUS LmvGuestKiQueueControl(
    _In_ ULONG Operation, _In_ PFILE_OBJECT FileObject)
{
    NTSTATUS status = STATUS_DEVICE_BUSY;
    ExAcquireFastMutex(&g_SmokeMutex);
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    if (Operation == LMV_GUEST_KI_QUEUE_BEGIN)
    {
        BOOLEAN available = g_KiDr3EventOwner == nullptr &&
            InterlockedCompareExchange(&g_KiInspectEnabled, 0, 0) == 0;
        for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
            available = available && g_KiDr3Events[index].State == LMV_GUEST_STUB_STATE_IDLE;
        if (available)
        {
            ++g_KiDr3EventGeneration;
            g_KiDr3EventOwner = FileObject;
            g_KiDr3ControllerProcessId = HandleToULong(PsGetCurrentProcessId());
            g_KiInspectTargetProcessId = g_KiDr3ControllerProcessId;
            g_KiDr3QueueSmokeActive = TRUE;
            InterlockedExchange(&g_KiDr3WaitEnabled, 1);
            status = STATUS_SUCCESS;
        }
    }
    else if (Operation == LMV_GUEST_KI_QUEUE_END)
    {
        status = STATUS_ACCESS_DENIED;
        if (g_KiDr3QueueSmokeActive && g_KiDr3EventOwner == FileObject &&
            g_KiDr3ControllerProcessId == HandleToULong(PsGetCurrentProcessId()))
        {
            InterlockedExchange(&g_KiDr3WaitEnabled, 0);
            g_KiDr3EventOwner = nullptr;
            g_KiDr3ControllerProcessId = 0;
            g_KiDr3QueueSmokeActive = FALSE;
            LmvGuestKiDr3CancelEvents(nullptr);
            KeSetEvent(&g_KiDr3EventAvailable, IO_NO_INCREMENT, FALSE);
            status = STATUS_SUCCESS;
        }
    }
    else
        status = STATUS_INVALID_PARAMETER;
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    ExReleaseFastMutex(&g_SmokeMutex);
    return status;
}

static NTSTATUS LmvGuestKiQueueProduce(
    _Inout_ PLMV_GUEST_KI_QUEUE_PRODUCE Output)
{
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    const BOOLEAN allowed = g_KiDr3QueueSmokeActive &&
        g_KiDr3EventOwner != nullptr &&
        g_KiDr3ControllerProcessId == HandleToULong(PsGetCurrentProcessId());
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    if (!allowed)
        return STATUS_ACCESS_DENIED;

    LMV_KI_INSPECT_STATE captured = {};
    captured.ProcessId = HandleToULong(PsGetCurrentProcessId());
    captured.ThreadId = HandleToULong(PsGetCurrentThreadId());
    captured.ExceptionCode = 0xE0424D56u;
    Output->ThreadId = captured.ThreadId;
    Output->Status = static_cast<ULONG>(
        LmvGuestKiDr3WaitForContinue(&captured, &Output->Sequence,
            nullptr, nullptr, nullptr));
    return STATUS_SUCCESS;
}

static NTSTATUS LmvGuestKiSessionContext(PLMV_GUEST_KI_DR3_CONTEXT Request,
    PFILE_OBJECT FileObject, BOOLEAN Write)
{
    if (Request->Version != LMV_CPUID_PROTOCOL_VERSION || Request->Size != sizeof(*Request))
        return STATUS_REVISION_MISMATCH;
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    NTSTATUS status = STATUS_NOT_FOUND;
    if (g_KiSessionOwner != nullptr &&
        g_KiDr3ControllerProcessId == HandleToULong(PsGetCurrentProcessId()))
    {
        for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
        {
            LMV_KI_DR3_EVENT_SLOT& slot = g_KiDr3Events[index];
            if (!slot.Persistent || !slot.RealFrame || slot.State != LMV_GUEST_STUB_STATE_PENDING ||
                slot.Consumer != FileObject || slot.Event.Sequence != Request->Sequence)
                continue;
            if (!Write)
            {
                Request->Frame = slot.Event.Frame;
                status = STATUS_SUCCESS;
            }
            else if (Request->Frame.Rip == 0 || Request->Frame.Rip >= 0x0000800000000000ull ||
                Request->Frame.Rsp == 0 || Request->Frame.Rsp >= 0x0000800000000000ull ||
                ((Request->Frame.Rflags ^ slot.Event.Frame.Rflags) & ~0x0000000000250dd5ull) != 0)
                status = STATUS_INVALID_PARAMETER;
            else
            {
                slot.Event.Frame = Request->Frame;
                slot.WriteContext = TRUE;
                status = STATUS_SUCCESS;
            }
            break;
        }
    }
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    return status;
}

static NTSTATUS LmvGuestSyncKiSession(PLMV_GUEST_KI_SESSION Session)
{
    Session->ProcessorsSeen = 0;
    Session->ProcessorsExpected = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    NTSTATUS status = STATUS_SUCCESS;
    for (ULONG index = 0; index < Session->ProcessorsExpected; ++index)
    {
        PROCESSOR_NUMBER processor = {};
        NTSTATUS current = KeGetProcessorNumberFromIndex(index, &processor);
        if (!NT_SUCCESS(current))
        {
            status = current;
            continue;
        }
        GROUP_AFFINITY affinity = {};
        GROUP_AFFINITY previous = {};
        affinity.Group = processor.Group;
        affinity.Mask = 1ull << processor.Number;
        KeSetSystemGroupAffinityThread(&affinity, &previous);
        LMV_CPUID_RESULT result = {};
        LmvGuestCpuidCall(LmvCpuidCommandSyncKiSession, Session->Mode, &result);
        KeRevertToUserGroupAffinityThread(&previous);
        if (result.Status == LMV_CPUID_RESULT_SUCCESS)
            ++Session->ProcessorsSeen;
        else
            status = STATUS_UNSUCCESSFUL;
    }
    return status;
}

typedef struct _LMV_DR2_SYNC_CONTEXT
{
    volatile LONG ProcessorsSeen;
    volatile LONG Failed;
    volatile LONG Status;
} LMV_DR2_SYNC_CONTEXT, *PLMV_DR2_SYNC_CONTEXT;

static ULONG_PTR NTAPI LmvGuestSyncDr2WatchProcessor(ULONG_PTR Context)
{
    PLMV_DR2_SYNC_CONTEXT sync = reinterpret_cast<PLMV_DR2_SYNC_CONTEXT>(Context);
    LMV_CPUID_RESULT result = {};
    LmvGuestCpuidCall(LmvCpuidCommandSyncDr2Watch, 0, &result);
    if (result.Status == LMV_CPUID_RESULT_SUCCESS)
        InterlockedIncrement(&sync->ProcessorsSeen);
    else
    {
        InterlockedExchange(&sync->Failed, 1);
        InterlockedCompareExchange(&sync->Status, static_cast<LONG>(result.Status), 0);
    }
    return 0;
}

static NTSTATUS LmvGuestSyncDr2WatchProcessors(PLMV_GUEST_DR2_WATCH Watch)
{
    LMV_DR2_SYNC_CONTEXT sync = {};
    Watch->ProcessorsExpected = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    KeIpiGenericCall(LmvGuestSyncDr2WatchProcessor, reinterpret_cast<ULONG_PTR>(&sync));
    Watch->ProcessorsSeen = static_cast<ULONG>(sync.ProcessorsSeen);
    if (sync.Failed != 0)
        return sync.Status != 0 ? static_cast<NTSTATUS>(sync.Status) : STATUS_UNSUCCESSFUL;
    if (Watch->ProcessorsSeen != Watch->ProcessorsExpected)
        return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

static NTSTATUS LmvGuestControlDr2Watch(PLMV_GUEST_DR2_WATCH Watch, PFILE_OBJECT FileObject)
{
    if (Watch->Version != LMV_CPUID_PROTOCOL_VERSION || Watch->Size != sizeof(*Watch))
        return STATUS_REVISION_MISMATCH;
    if (Watch->Operation != LMV_DR2_WATCH_START && Watch->Operation != LMV_DR2_WATCH_STOP)
        return STATUS_INVALID_PARAMETER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;

    NTSTATUS status = KeWaitForSingleObject(&g_KiSessionGate, Executive, KernelMode, FALSE, nullptr);
    if (!NT_SUCCESS(status))
        return status;

    if (g_KiSessionOwner != FileObject)
        status = STATUS_ACCESS_DENIED;
    else if (g_KiSessionProcess == nullptr || InterlockedCompareExchange(&g_KiSessionActive, 0, 0) == 0)
        status = STATUS_INVALID_DEVICE_STATE;
    else if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_KiSessionMode), 0, 0) !=
        LMV_DEBUG_MODE_TRANSPARENT)
        status = STATUS_INVALID_DEVICE_STATE;
    else if (Watch->ProcessId != HandleToULong(PsGetProcessId(g_KiSessionProcess)))
        status = STATUS_ACCESS_DENIED;
    else if (Watch->Operation == LMV_DR2_WATCH_START &&
        (InterlockedCompareExchange(&g_KiDr2WatchActive, 0, 0) != 0 ||
         Watch->ProcessId == 0 || Watch->Address == 0 ||
         Watch->Address >= 0x0000800000000000ull ||
         (Watch->Length != 1 && Watch->Length != 2 && Watch->Length != 4 && Watch->Length != 8) ||
         Watch->Address > 0x00007FFFFFFFFFFFull - (Watch->Length - 1) ||
         (Watch->Address & (Watch->Length - 1)) != 0 ||
         (Watch->Condition != LMV_DR2_CONDITION_WRITE &&
          Watch->Condition != LMV_DR2_CONDITION_READ_WRITE)))
        status = InterlockedCompareExchange(&g_KiDr2WatchActive, 0, 0) != 0 ?
            STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER;
    else if (Watch->Operation == LMV_DR2_WATCH_START)
    {
        PUCHAR processBytes = reinterpret_cast<PUCHAR>(g_KiSessionProcess);
        ULONGLONG userCr3 = *reinterpret_cast<volatile ULONGLONG*>(
            processBytes + LMV_KPROCESS_USER_DIRECTORY_TABLE_BASE_OFFSET);
        userCr3 &= LMV_PAGE_ADDRESS_MASK;

        LMV_SHARED_RESULT result = {};
        status = LmvGuestIssueSharedCommand(LmvDebugCommandConfigureDr2Watch,
            Watch->Address, &result, Watch->Length, Watch->Condition, userCr3, Watch->ProcessId);
        if (NT_SUCCESS(status))
        {
            status = LmvGuestSyncDr2WatchProcessors(Watch);
            if (NT_SUCCESS(status))
            {
                InterlockedExchange(&g_KiDr2WatchActive, 1);
                Watch->Hits = result.Value0;
            }
            else
            {
                LMV_SHARED_RESULT rollback = {};
                NTSTATUS rollbackStatus = LmvGuestIssueSharedCommand(
                    LmvDebugCommandRemoveDr2Watch, 0, &rollback);
                if (NT_SUCCESS(rollbackStatus))
                {
                    LMV_GUEST_DR2_WATCH syncResult = {};
                    rollbackStatus = LmvGuestSyncDr2WatchProcessors(&syncResult);
                }
                if (!NT_SUCCESS(rollbackStatus))
                    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                        "[LMvtdriverGuest] DR2 watch rollback failed: status=0x%08X\n",
                        rollbackStatus);
            }
        }
    }
    else if (InterlockedCompareExchange(&g_KiDr2WatchActive, 0, 0) == 0)
    {
        Watch->Hits = 0;
        Watch->ProcessorsSeen = 0;
        Watch->ProcessorsExpected = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
        status = STATUS_SUCCESS;
    }
    else
    {
        LMV_SHARED_RESULT result = {};
        status = LmvGuestIssueSharedCommand(LmvDebugCommandRemoveDr2Watch, 0, &result);
        if (NT_SUCCESS(status))
        {
            Watch->Hits = result.Value0;
            status = LmvGuestSyncDr2WatchProcessors(Watch);
            if (NT_SUCCESS(status))
                InterlockedExchange(&g_KiDr2WatchActive, 0);
        }
    }

    Watch->Status = static_cast<ULONG>(status);
    KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
    return status;
}

static NTSTATUS LmvGuestQueryKiDrain(PLMV_GUEST_KI_DRAIN Drain)
{
    LMV_SHARED_RESULT result = {};
    NTSTATUS status = LmvGuestIssueSharedCommand(LmvDebugCommandQueryKiDrain, 0, &result);
    Drain->InFlight = result.Value0;
    Drain->ArmedProcessors = static_cast<ULONG>(result.Value1);
    Drain->Flags = static_cast<ULONG>(result.Value2);
    Drain->Status = status;
    return status;
}

static NTSTATUS LmvGuestWaitKiDrain(PLMV_GUEST_KI_DRAIN Drain)
{
    KEVENT delay = {};
    KeInitializeEvent(&delay, NotificationEvent, FALSE);
    LARGE_INTEGER interval = {};
    interval.QuadPart = -100000LL;
    for (ULONG attempt = 0; attempt < 500; ++attempt)
    {
        NTSTATUS status = LmvGuestQueryKiDrain(Drain);
        if (!NT_SUCCESS(status))
            return status;
        if (LmvKiDrainComplete(Drain->InFlight, Drain->ArmedProcessors, Drain->Flags))
            return STATUS_SUCCESS;
        KeWaitForSingleObject(&delay, Executive, KernelMode, FALSE, &interval);
    }
    Drain->Status = static_cast<ULONG>(STATUS_IO_TIMEOUT);
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS LmvGuestStopKiSession(PLMV_GUEST_KI_SESSION Session)
{
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    InterlockedExchange(&g_KiSessionActive, 0);
    InterlockedExchange(&g_KiDr3WaitEnabled, 0);
    LmvGuestKiDr3CancelEvents(nullptr);
    ++g_KiDr3EventGeneration;
    KeSetEvent(&g_KiDr3EventAvailable, IO_NO_INCREMENT, FALSE);
    ExReleaseFastMutex(&g_KiDr3EventMutex);

    LMV_SHARED_RESULT result = {};
    NTSTATUS status = LmvGuestIssueSharedCommand(LmvDebugCommandStopKiSession, 0, &result);
    if (NT_SUCCESS(status))
        status = LmvGuestSyncKiSession(Session);
    if (!NT_SUCCESS(status))
        return status;
    LMV_GUEST_KI_DRAIN drain = {};
    status = LmvGuestWaitKiDrain(&drain);
    if (!NT_SUCCESS(status))
        return status;
    InterlockedExchange(&g_KiDr2WatchActive, 0);
    LMV_SHARED_RESULT modeResult = {};
    status = LmvGuestIssueSharedCommand(LmvDebugCommandSetMode, LMV_DEBUG_MODE_INTERACTIVE,
        &modeResult, LMV_SESSION_IDLE);
    if (!NT_SUCCESS(status))
        return status;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_KiSessionMode), LMV_DEBUG_MODE_INTERACTIVE);
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    PEPROCESS process = g_KiSessionProcess;
    g_KiSessionProcess = nullptr;
    g_KiSessionOwner = nullptr;
    g_KiDr3EventOwner = nullptr;
    g_KiDr3ControllerProcessId = 0;
    InterlockedExchange(&g_KiDr3RaxWriteEnabled, 0);
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    if (process != nullptr)
        ObDereferenceObject(process);
    return STATUS_SUCCESS;
}


static NTSTATUS LmvGuestControlKiSession(PLMV_GUEST_KI_SESSION Session, PFILE_OBJECT FileObject)
{
    if (Session->Version != LMV_CPUID_PROTOCOL_VERSION || Session->Size != sizeof(*Session))
        return STATUS_REVISION_MISMATCH;
    if (Session->Operation != LMV_KI_SESSION_START && Session->Operation != LMV_KI_SESSION_STOP)
        return STATUS_INVALID_PARAMETER;
    if (Session->Operation == LMV_KI_SESSION_START &&
        Session->Mode != LMV_DEBUG_MODE_INTERACTIVE && Session->Mode != LMV_DEBUG_MODE_TRANSPARENT)
        return STATUS_INVALID_PARAMETER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;

    NTSTATUS status = KeWaitForSingleObject(&g_KiSessionGate, Executive, KernelMode, FALSE, nullptr);
    if (!NT_SUCCESS(status))
        return status;

    if (g_KiGuestRetired)
    {
        KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
        return STATUS_DELETE_PENDING;
    }
    if (Session->Operation == LMV_KI_SESSION_STOP)
    {
        status = g_KiSessionOwner != nullptr && g_KiSessionOwner != FileObject ? STATUS_ACCESS_DENIED :
            g_KiSessionProcess != nullptr ? LmvGuestStopKiSession(Session) : STATUS_SUCCESS;
        KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
        return status;
    }

    if (Session->ProcessId == 0 || Session->ProcessId == HandleToULong(PsGetCurrentProcessId()))
    {
        KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
        return STATUS_INVALID_PARAMETER;
    }

    LMV_GUEST_KI_DISPATCH_INFO info = {};
    status = LmvGuestQueryKiDispatchInfo(&info);
    if (!NT_SUCCESS(status) || info.BytesRead < 2 ||
        info.OriginalBytes[0] != 0x40 || info.OriginalBytes[1] != 0x55)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "[LMvtdriverGuest] KiDispatchException query failed: status=0x%08X bytes=%u prologue=%02X%02X\n",
            status, info.BytesRead,
            info.BytesRead > 0 ? info.OriginalBytes[0] : 0,
            info.BytesRead > 1 ? info.OriginalBytes[1] : 0);
        KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
        return NT_SUCCESS(status) ? STATUS_NOT_SUPPORTED : status;
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "[LMvtdriverGuest] KiDispatchException found at 0x%p\n", (PVOID)info.RoutineAddress);

    PEPROCESS process = nullptr;
    status = PsLookupProcessByProcessId(ULongToHandle(Session->ProcessId), &process);
    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "[LMvtdriverGuest] PsLookupProcessByProcessId failed for PID %u: status=0x%08X\n",
            Session->ProcessId, status);
        KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
        return status;
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "[LMvtdriverGuest] Process lookup succeeded for PID %u\n", Session->ProcessId);

    ExAcquireFastMutex(&g_KiDr3EventMutex);
    BOOLEAN busy = g_KiSessionProcess != nullptr || g_KiSessionOwner != nullptr || g_KiDr3EventOwner != nullptr ||
        g_KiActiveRequests != 1 || g_KiDr3QueueSmokeActive || g_KiInspectEnabled != 0 ||
        g_KernelProbeStage != 0 || g_ExecProbeMdl != nullptr || g_StubActive;
    for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
        busy = busy || g_KiDr3Events[index].State != LMV_GUEST_STUB_STATE_IDLE;
    if (!busy)
        InterlockedExchange(&g_KiPersistentOnly, 1);
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    if (busy)
    {
        ObDereferenceObject(process);
        KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
        return STATUS_DEVICE_BUSY;
    }

    PUCHAR processBytes = reinterpret_cast<PUCHAR>(process);
    ULONGLONG rawCr3 = *reinterpret_cast<volatile ULONGLONG*>(
        processBytes + LMV_KPROCESS_DIRECTORY_TABLE_BASE_OFFSET);
    Session->KernelCr3 = rawCr3 & LMV_PAGE_ADDRESS_MASK;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "[LMvtdriverGuest] Read CR3 from offset 0x%X: raw=0x%016llX masked=0x%016llX\n",
        LMV_KPROCESS_DIRECTORY_TABLE_BASE_OFFSET, rawCr3, Session->KernelCr3);
    if (Session->KernelCr3 == 0)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "[LMvtdriverGuest] Invalid CR3 (zero after masking) - possible wrong EPROCESS offset!\n");
        ObDereferenceObject(process);
        KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
        return STATUS_INVALID_ADDRESS;
    }

    Session->KiAddress = info.RoutineAddress;
    if (!g_KiGuestRegistered)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "[LMvtdriverGuest] Registering Guest exits with Host...\n");
        g_GuestDriverObject->DriverUnload = nullptr;
        LMV_SHARED_RESULT registration = {};
        ULONGLONG base = reinterpret_cast<ULONGLONG>(g_GuestDriverObject->DriverStart);
        status = LmvGuestIssueSharedCommand(LmvDebugCommandRegisterKiExits, base, &registration,
            base + g_GuestDriverObject->DriverSize,
            reinterpret_cast<ULONGLONG>(&LmvGuestKiCaptureExit),
            reinterpret_cast<ULONGLONG>(&LmvGuestKiCommitExit));
        if (!NT_SUCCESS(status))
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "[LMvtdriverGuest] RegisterKiExits failed: status=0x%08X\n", status);
            ObDereferenceObject(process);
            KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
            return status;
        }
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "[LMvtdriverGuest] Guest exits registered successfully\n");
        g_KiGuestRegistered = TRUE;
    }
    g_LmvGuestKiSessionResumeRip = info.RoutineAddress + 2;
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    g_KiSessionProcess = process;
    g_KiSessionOwner = FileObject;
    g_KiDr3EventOwner = FileObject;
    g_KiDr3ControllerProcessId = HandleToULong(PsGetCurrentProcessId());
    ++g_KiDr3EventGeneration;
    InterlockedExchange(&g_KiDr3RaxWriteEnabled, 1);
    InterlockedExchange(&g_KiDr3WaitEnabled, 1);
    InterlockedExchange(&g_KiSessionActive, 1);
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    g_GuestDriverObject->DriverUnload = nullptr;

    // Set session mode and state (immutable once set)
    ULONG sessionMode = Session->Mode; // From R3: LMV_DEBUG_MODE_INTERACTIVE or TRANSPARENT
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_KiSessionMode), static_cast<LONG>(sessionMode));
    ULONG sessionState = (sessionMode == LMV_DEBUG_MODE_TRANSPARENT)
                         ? LMV_SESSION_TRANSPARENT
                         : LMV_SESSION_INTERACTIVE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "[LMvtdriverGuest] Starting Ki session: CR3=0x%016llX Ki=0x%p Mode=%s\n",
        Session->KernelCr3, (PVOID)info.RoutineAddress,
        sessionMode == LMV_DEBUG_MODE_TRANSPARENT ? "TRANSPARENT" : "INTERACTIVE");

    LMV_SHARED_RESULT result = {};
    status = LmvGuestIssueSharedCommand(LmvDebugCommandStartKiSession,
        Session->KernelCr3, &result, info.RoutineAddress,
        reinterpret_cast<ULONGLONG>(&LmvGuestKiSessionEventStub),
        reinterpret_cast<ULONGLONG>(&LmvGuestKiSessionPassStub));

    if (NT_SUCCESS(status))
    {
        // Notify Host of the mode and state
        LMV_SHARED_RESULT modeResult = {};
        NTSTATUS modeStatus = LmvGuestIssueSharedCommand(
            LmvDebugCommandSetMode, sessionMode, &modeResult, sessionState);
        if (!NT_SUCCESS(modeStatus))
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "[LMvtdriverGuest] Failed to set mode in Host: status=0x%08X\n", modeStatus);
        }

        // Continue with DR3 sync
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "[LMvtdriverGuest] Host session started, syncing DR3 across all CPUs...\n");
        status = NT_SUCCESS(modeStatus) ? LmvGuestSyncKiSession(Session) : modeStatus;
        if (!NT_SUCCESS(status))
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "[LMvtdriverGuest] DR3 sync failed: status=0x%08X\n", status);
        }
        else
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                "[LMvtdriverGuest] Ki session fully active: %u CPUs synced\n",
                Session->ProcessorsSeen);
        }
    }
    else
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "[LMvtdriverGuest] Host StartKiSession failed: status=0x%08X\n", status);
    }
    if (!NT_SUCCESS(status))
        LmvGuestStopKiSession(Session);
    KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
    return status;
}

static NTSTATUS LmvGuestControlKiDrain(PLMV_GUEST_KI_DRAIN Drain, PFILE_OBJECT FileObject)
{
    if (Drain->Version != LMV_CPUID_PROTOCOL_VERSION || Drain->Size != sizeof(*Drain))
        return STATUS_REVISION_MISMATCH;
    if (Drain->Operation != LMV_KI_SESSION_QUERY && Drain->Operation != LMV_KI_SESSION_PREPARE_UNLOAD)
        return STATUS_INVALID_PARAMETER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = KeWaitForSingleObject(&g_KiSessionGate, Executive, KernelMode, FALSE, nullptr);
    if (!NT_SUCCESS(status))
        return status;
    if (g_KiGuestRetired)
    {
        Drain->InFlight = 0;
        Drain->ArmedProcessors = 0;
        Drain->Flags = LMV_KI_DRAIN_RETIRED;
        status = STATUS_SUCCESS;
    }
    else if (Drain->Operation == LMV_KI_SESSION_QUERY)
        status = LmvGuestQueryKiDrain(Drain);
    else if (g_KiSessionOwner != nullptr && g_KiSessionOwner != FileObject)
        status = STATUS_ACCESS_DENIED;
    else
    {
        ExAcquireFastMutex(&g_KiDr3EventMutex);
        BOOLEAN busy = g_KiActiveRequests != 1 || g_KiDr3QueueSmokeActive ||
            g_KiInspectEnabled != 0 || g_KiDr3EventOwner != g_KiSessionOwner;
        if (!busy)
            InterlockedExchange(&g_KiUnloadPreparing, 1);
        ExReleaseFastMutex(&g_KiDr3EventMutex);
        if (busy || g_KernelProbeStage != 0 || g_ExecProbeMdl != nullptr || g_StubActive)
            status = STATUS_DEVICE_BUSY;
        else
        {
            LMV_GUEST_KI_SESSION session = {};
            status = LmvGuestStopKiSession(&session);
            Drain->ProcessorsSeen = session.ProcessorsSeen;
            Drain->ProcessorsExpected = session.ProcessorsExpected;
            if (NT_SUCCESS(status))
                status = LmvGuestWaitKiDrain(Drain);
            if (NT_SUCCESS(status))
            {
                LMV_SHARED_RESULT result = {};
                status = LmvGuestIssueSharedCommand(LmvDebugCommandRetireKiGuest, 0, &result);
            }
            if (NT_SUCCESS(status))
            {
                g_KiGuestRetired = TRUE;
                Drain->Flags = LMV_KI_DRAIN_RETIRED;
                g_GuestDriverObject->DriverUnload = LmvGuestUnload;
            }
        }
        if (!NT_SUCCESS(status))
            InterlockedExchange(&g_KiUnloadPreparing, 0);
    }
    Drain->Status = status;
    KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
    return status;
}

static VOID LmvGuestCaptureTransparentException(
    PEXCEPTION_RECORD ExceptionRecord, PVOID ExceptionFrame, PVOID TrapFrame, ULONG FirstChance)
{
    HOST_EXCEPTION_EVENT event = {};
    LARGE_INTEGER systemTime = {};
    KeQuerySystemTimePrecise(&systemTime);
    event.Timestamp = static_cast<UINT64>(systemTime.QuadPart);
    event.ProcessId = HandleToULong(PsGetCurrentProcessId());
    event.ThreadId = HandleToULong(PsGetCurrentThreadId());
    event.Cr3 = __readcr3();
    event.FirstChance = FirstChance;
    event.CaptureStatus = static_cast<ULONG>(STATUS_INVALID_ADDRESS);
    KTRAP_FRAME trap = {};
    KEXCEPTION_FRAME frame = {};
    EXCEPTION_RECORD record = {};
    MM_COPY_ADDRESS source = {};
    SIZE_T copied = 0;
    if (ExceptionRecord != nullptr && reinterpret_cast<ULONG_PTR>(ExceptionRecord) >= 0xffff800000000000ull)
    {
        source.VirtualAddress = ExceptionRecord;
        NTSTATUS status = MmCopyMemory(&record, source, sizeof(record), MM_COPY_MEMORY_VIRTUAL, &copied);
        event.CaptureStatus = static_cast<ULONG>(status);
        if (NT_SUCCESS(status) && copied == sizeof(record) && record.NumberParameters <= 15)
        {
            event.ExceptionType = static_cast<ULONG>(record.ExceptionCode);
            event.ExceptionAddress = reinterpret_cast<UINT64>(record.ExceptionAddress);
            event.NumberParameters = record.NumberParameters;
            RtlCopyMemory(event.ExceptionInformation, record.ExceptionInformation,
                record.NumberParameters * sizeof(UINT64));
            if (TrapFrame != nullptr && ExceptionFrame != nullptr &&
                reinterpret_cast<ULONG_PTR>(TrapFrame) >= 0xffff800000000000ull &&
                reinterpret_cast<ULONG_PTR>(ExceptionFrame) >= 0xffff800000000000ull)
            {
                source.VirtualAddress = TrapFrame;
                copied = 0;
                status = MmCopyMemory(&trap, source, sizeof(trap), MM_COPY_MEMORY_VIRTUAL, &copied);
                if (NT_SUCCESS(status) && copied == sizeof(trap))
                {
                    source.VirtualAddress = ExceptionFrame;
                    copied = 0;
                    status = MmCopyMemory(&frame, source, sizeof(frame), MM_COPY_MEMORY_VIRTUAL, &copied);
                    if (NT_SUCCESS(status) && copied == sizeof(frame) && (trap.SegCs & 3) == 3 &&
                        trap.Rip != 0 && trap.Rip < 0x0000800000000000ull &&
                        trap.Rsp != 0 && trap.Rsp < 0x0000800000000000ull)
                    {
                        event.Rip = trap.Rip; event.Rsp = trap.Rsp; event.Rflags = trap.EFlags;
                        event.Rax = trap.Rax; event.Rcx = trap.Rcx; event.Rdx = trap.Rdx;
                        event.R8 = trap.R8; event.R9 = trap.R9; event.R10 = trap.R10; event.R11 = trap.R11;
                        event.Rbx = frame.Rbx; event.Rbp = frame.Rbp; event.Rsi = frame.Rsi; event.Rdi = frame.Rdi;
                        event.R12 = frame.R12; event.R13 = frame.R13; event.R14 = frame.R14; event.R15 = frame.R15;
                        event.Dr6 = trap.Dr6;
                        event.ContextValid = 1;
                        event.CaptureStatus = STATUS_SUCCESS;

                        // Capture user-mode call stack using direct pointer access
                        event.StackFrameCount = 0;
                        event.StackCaptureStatus = 2; // Default: no frames found
                        if (trap.Rsp != 0 && trap.Rsp < 0x0000800000000000ull)
                        {
                            __try
                            {
                                const ULONG MAX_STACK_SCAN = 0x2000; // 8KB
                                PUINT64 stackPtr = reinterpret_cast<PUINT64>(trap.Rsp);
                                PUINT64 stackEnd = reinterpret_cast<PUINT64>(trap.Rsp + MAX_STACK_SCAN);

                                // Scan stack for user-mode return addresses
                                while (stackPtr < stackEnd && event.StackFrameCount < HOST_EXCEPTION_MAX_STACK_FRAMES)
                                {
                                    UINT64 value = *stackPtr; // Direct pointer read
                                    if (value > 0x10000 && value < 0x0000800000000000ull)
                                    {
                                        event.StackFrames[event.StackFrameCount++] = value;
                                    }
                                    stackPtr++;
                                }

                                event.StackCaptureStatus = (event.StackFrameCount > 0) ? 0 : 2;
                            }
                            __except (EXCEPTION_EXECUTE_HANDLER)
                            {
                                event.StackCaptureStatus = 1; // Exception during scan
                            }
                        }
                    }
                    else
                        event.CaptureStatus = static_cast<ULONG>(NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status);
                }
                else
                    event.CaptureStatus = static_cast<ULONG>(NT_SUCCESS(status) ? STATUS_PARTIAL_COPY : status);
            }
            else
                event.CaptureStatus = static_cast<ULONG>(STATUS_INVALID_ADDRESS);
        }
        else if (NT_SUCCESS(status))
            event.CaptureStatus = static_cast<ULONG>(STATUS_PARTIAL_COPY);
    }
    ExAcquireFastMutex(&g_CommandMutex);
    LMV_CPUID_SHARED_PAGE_RESULT pageResult = {};
    LmvGuestQuerySharedPage(&pageResult);
    if (pageResult.Status == LMV_CPUID_RESULT_SUCCESS && pageResult.Magic == LMV_SHARED_PAGE_MAGIC)
    {
        PHYSICAL_ADDRESS pa = {};
        pa.LowPart = pageResult.GpaLow;
        pa.HighPart = pageResult.GpaHigh;
        PLMV_SHARED_PAGE page = static_cast<PLMV_SHARED_PAGE>(MmMapIoSpace(pa, PAGE_SIZE, MmCached));
        if (page != nullptr)
        {
            if (page->Magic == LMV_SHARED_PAGE_MAGIC && page->Version == LMV_CPUID_PROTOCOL_VERSION &&
                page->Size == sizeof(LMV_SHARED_PAGE))
            {
                RtlCopyMemory(page->Data, &event, sizeof(event));
                KeMemoryBarrier();
                LMV_CPUID_RESULT result = {};
                LmvGuestCpuidCall(LmvCpuidCommandSubmitHostEvent, 0, &result);
            }
            MmUnmapIoSpace(page, PAGE_SIZE);
        }
    }
    ExReleaseFastMutex(&g_CommandMutex);
}

extern "C" BOOLEAN LmvGuestKiSessionException(
    PEXCEPTION_RECORD ExceptionRecord, PVOID ExceptionFrame, PVOID TrapFrame,
    ULONG PreviousMode, ULONG FirstChance)
{
    if (PreviousMode == UserMode && g_KiSessionMode == LMV_DEBUG_MODE_TRANSPARENT)
    {
        if (KeGetCurrentIrql() == PASSIVE_LEVEL && (__readeflags() & 0x200) != 0 &&
            g_KiSessionActive && g_KiSessionProcess == PsGetCurrentProcess())
            LmvGuestCaptureTransparentException(ExceptionRecord, ExceptionFrame, TrapFrame, FirstChance);
        return FALSE;
    }
    if (PreviousMode != UserMode || FirstChance == 0 ||
        KeGetCurrentIrql() != PASSIVE_LEVEL || (__readeflags() & 0x200) == 0 ||
        ExceptionRecord == nullptr || ExceptionFrame == nullptr || TrapFrame == nullptr ||
        reinterpret_cast<ULONG_PTR>(ExceptionRecord) < 0xffff800000000000ull ||
        reinterpret_cast<ULONG_PTR>(ExceptionFrame) < 0xffff800000000000ull ||
        reinterpret_cast<ULONG_PTR>(TrapFrame) < 0xffff800000000000ull)
        return FALSE;
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    BOOLEAN target = InterlockedCompareExchange(&g_KiSessionActive, 0, 0) != 0 &&
        g_KiSessionProcess == PsGetCurrentProcess();
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    if (!target)
        return FALSE;
    KTRAP_FRAME trap = {};
    KEXCEPTION_FRAME frame = {};
    EXCEPTION_RECORD record = {};
    MM_COPY_ADDRESS source = {};
    SIZE_T copied = 0;
    source.VirtualAddress = TrapFrame;
    if (!NT_SUCCESS(MmCopyMemory(&trap, source, sizeof(trap), MM_COPY_MEMORY_VIRTUAL, &copied)) || copied != sizeof(trap))
        return FALSE;
    source.VirtualAddress = ExceptionFrame;
    if (!NT_SUCCESS(MmCopyMemory(&frame, source, sizeof(frame), MM_COPY_MEMORY_VIRTUAL, &copied)) || copied != sizeof(frame))
        return FALSE;
    source.VirtualAddress = ExceptionRecord;
    if (!NT_SUCCESS(MmCopyMemory(&record, source, sizeof(record), MM_COPY_MEMORY_VIRTUAL, &copied)) || copied != sizeof(record) ||
        (trap.SegCs & 3) != 3 || trap.Rip == 0 || trap.Rip >= 0x0000800000000000ull ||
        trap.Rsp == 0 || trap.Rsp >= 0x0000800000000000ull)
        return FALSE;
    LMV_KI_INSPECT_STATE captured = {};
    captured.ProcessId = HandleToULong(PsGetCurrentProcessId());
    captured.ThreadId = HandleToULong(PsGetCurrentThreadId());
    captured.ExceptionCode = static_cast<ULONG>(record.ExceptionCode);
    captured.ExceptionAddress = reinterpret_cast<ULONGLONG>(record.ExceptionAddress);
    captured.FrameStatus = STATUS_SUCCESS;
    captured.Frame = { trap.Rip, trap.Rsp, trap.EFlags, trap.Rax, trap.Rcx, trap.Rdx,
        trap.R8, trap.R9, trap.R10, trap.R11, frame.Rbx, frame.Rbp, frame.Rsi, frame.Rdi,
        frame.R12, frame.R13, frame.R14, frame.R15 };
    BOOLEAN raxModified = FALSE;
    BOOLEAN contextModified = FALSE;
    BOOLEAN handled = FALSE;
    NTSTATUS status = LmvGuestKiDr3WaitForContinue(&captured, &captured.EventSequence,
        &raxModified, &contextModified, &handled, TRUE);
    if (!NT_SUCCESS(status) || !handled)
        return FALSE;
    PKTRAP_FRAME targetTrap = static_cast<PKTRAP_FRAME>(TrapFrame);
    PKEXCEPTION_FRAME targetFrame = static_cast<PKEXCEPTION_FRAME>(ExceptionFrame);
    if (contextModified)
    {
        const LMV_GUEST_KI_FRAME_SNAPSHOT& context = captured.Frame;
        targetTrap->Rax = context.Rax;
        targetTrap->Rcx = context.Rcx;
        targetTrap->Rdx = context.Rdx;
        targetTrap->R8 = context.R8;
        targetTrap->R9 = context.R9;
        targetTrap->R10 = context.R10;
        targetTrap->R11 = context.R11;
        targetTrap->Rip = context.Rip;
        targetTrap->Rsp = context.Rsp;
        targetTrap->EFlags = static_cast<ULONG>(context.Rflags);
        targetFrame->Rbx = context.Rbx;
        targetFrame->Rbp = context.Rbp;
        targetFrame->Rsi = context.Rsi;
        targetFrame->Rdi = context.Rdi;
        targetFrame->R12 = context.R12;
        targetFrame->R13 = context.R13;
        targetFrame->R14 = context.R14;
        targetFrame->R15 = context.R15;
    }
    else if (raxModified)
        targetTrap->Rax = captured.Frame.Rax;
    return TRUE;
}

extern "C" BOOLEAN LmvGuestKiDr3InspectException(
    _In_opt_ PEXCEPTION_RECORD ExceptionRecord,
    _In_opt_ PVOID ExceptionFrame,
    _In_opt_ PVOID TrapFrame,
    _In_ ULONG PreviousMode,
    _In_ ULONG FirstChance)
{
    if (InterlockedCompareExchange(&g_KiInspectEnabled, 0, 0) == 0)
        return FALSE;
    if (KeGetCurrentIrql() > APC_LEVEL)
    {
        g_KiInspectState.Reason = 1;
        return FALSE;
    }
    if (PreviousMode != UserMode)
    {
        g_KiInspectState.Reason = 2;
        return FALSE;
    }
    if (FirstChance == 0)
    {
        g_KiInspectState.Reason = 3;
        return FALSE;
    }
    ULONG processId = static_cast<ULONG>(reinterpret_cast<ULONG_PTR>(PsGetCurrentProcessId()));
    if (processId != g_KiInspectTargetProcessId)
    {
        g_KiInspectState.Reason = 4;
        return FALSE;
    }
    if (ExceptionRecord == nullptr ||
        reinterpret_cast<ULONG_PTR>(ExceptionRecord) < 0xffff800000000000ull)
    {
        g_KiInspectState.Reason = 5;
        return FALSE;
    }

    EXCEPTION_RECORD record = {};
    LMV_KI_INSPECT_STATE captured = {};
    captured.EventStatus = static_cast<ULONG>(STATUS_NOT_FOUND);
    MM_COPY_ADDRESS source = {};
    SIZE_T copied = 0;
    source.VirtualAddress = ExceptionRecord;
    NTSTATUS status = MmCopyMemory(&record, source,
        FIELD_OFFSET(EXCEPTION_RECORD, ExceptionInformation),
        MM_COPY_MEMORY_VIRTUAL, &copied);
    captured.Status = static_cast<ULONG>(status);
    if (!NT_SUCCESS(status) ||
        copied != FIELD_OFFSET(EXCEPTION_RECORD, ExceptionInformation))
    {
        g_KiInspectState.Reason = 6;
        g_KiInspectState.Status = captured.Status;
        return FALSE;
    }

    if (InterlockedCompareExchange(&g_KiInspectCaptureFrame, 0, 0) != 0)
    {
        if (ExceptionFrame == nullptr || TrapFrame == nullptr ||
            (reinterpret_cast<ULONG_PTR>(ExceptionFrame) & 7u) != 0 ||
            (reinterpret_cast<ULONG_PTR>(TrapFrame) & 7u) != 0 ||
            reinterpret_cast<ULONG_PTR>(ExceptionFrame) < 0xffff800000000000ull ||
            reinterpret_cast<ULONG_PTR>(TrapFrame) < 0xffff800000000000ull)
        {
            g_KiInspectState.FrameStatus = static_cast<ULONG>(STATUS_INVALID_PARAMETER);
            g_KiInspectState.FrameReason = 8;
            return FALSE;
        }
        KTRAP_FRAME trap = {};
        KEXCEPTION_FRAME frame = {};
        source.VirtualAddress = TrapFrame;
        copied = 0;
        status = MmCopyMemory(&trap, source, sizeof(trap), MM_COPY_MEMORY_VIRTUAL, &copied);
        if (!NT_SUCCESS(status) || copied != sizeof(trap))
        {
            g_KiInspectState.FrameStatus = static_cast<ULONG>(status);
            g_KiInspectState.FrameReason = 9;
            return FALSE;
        }
        source.VirtualAddress = ExceptionFrame;
        copied = 0;
        status = MmCopyMemory(&frame, source, sizeof(frame), MM_COPY_MEMORY_VIRTUAL, &copied);
        if (!NT_SUCCESS(status) || copied != sizeof(frame))
        {
            g_KiInspectState.FrameStatus = static_cast<ULONG>(status);
            g_KiInspectState.FrameReason = 10;
            return FALSE;
        }
        if ((trap.SegCs & 3u) != 3u ||
            trap.Rip >= 0x0000800000000000ull ||
            trap.Rsp >= 0x0000800000000000ull ||
            trap.Rip == 0 || trap.Rsp == 0)
        {
            g_KiInspectState.FrameStatus = static_cast<ULONG>(STATUS_DATA_ERROR);
            g_KiInspectState.FrameReason = 11;
            return FALSE;
        }
        LMV_GUEST_KI_FRAME_SNAPSHOT& snapshot = captured.Frame;
        snapshot.Rip = trap.Rip;
        snapshot.Rsp = trap.Rsp;
        snapshot.Rflags = trap.EFlags;
        snapshot.Rax = trap.Rax;
        snapshot.Rcx = trap.Rcx;
        snapshot.Rdx = trap.Rdx;
        snapshot.R8 = trap.R8;
        snapshot.R9 = trap.R9;
        snapshot.R10 = trap.R10;
        snapshot.R11 = trap.R11;
        snapshot.Rbx = frame.Rbx;
        snapshot.Rbp = frame.Rbp;
        snapshot.Rsi = frame.Rsi;
        snapshot.Rdi = frame.Rdi;
        snapshot.R12 = frame.R12;
        snapshot.R13 = frame.R13;
        snapshot.R14 = frame.R14;
        snapshot.R15 = frame.R15;
        captured.FrameStatus = static_cast<ULONG>(STATUS_SUCCESS);
        captured.FrameReason = 12;
    }

    captured.ProcessId = processId;
    captured.ThreadId =
        static_cast<ULONG>(reinterpret_cast<ULONG_PTR>(PsGetCurrentThreadId()));
    captured.ExceptionCode = static_cast<ULONG>(record.ExceptionCode);
    captured.ExceptionAddress =
        reinterpret_cast<ULONGLONG>(record.ExceptionAddress);
    captured.FirstChance = 1;
    captured.Reason = 7;
    BOOLEAN handled = FALSE;
    if (InterlockedCompareExchange(&g_KiDr3WaitEnabled, 0, 0) != 0)
    {
        BOOLEAN raxModified = FALSE;
        BOOLEAN contextModified = FALSE;
        captured.EventStatus = static_cast<ULONG>(
            LmvGuestKiDr3WaitForContinue(&captured, &captured.EventSequence,
                &raxModified, &contextModified, &handled));
        if (raxModified && NT_SUCCESS(static_cast<NTSTATUS>(captured.EventStatus)) &&
            TrapFrame != nullptr && captured.FrameStatus == static_cast<ULONG>(STATUS_SUCCESS))
        {
            static_cast<PKTRAP_FRAME>(TrapFrame)->Rax = captured.Frame.Rax;
        }
        if (contextModified && NT_SUCCESS(static_cast<NTSTATUS>(captured.EventStatus)) &&
            TrapFrame != nullptr && ExceptionFrame != nullptr &&
            captured.FrameStatus == static_cast<ULONG>(STATUS_SUCCESS))
        {
            PKTRAP_FRAME trapFrame = static_cast<PKTRAP_FRAME>(TrapFrame);
            PKEXCEPTION_FRAME exceptionFrame = static_cast<PKEXCEPTION_FRAME>(ExceptionFrame);
            const LMV_GUEST_KI_FRAME_SNAPSHOT& frame = captured.Frame;
            trapFrame->Rax = frame.Rax;
            trapFrame->Rcx = frame.Rcx;
            trapFrame->Rdx = frame.Rdx;
            trapFrame->R8 = frame.R8;
            trapFrame->R9 = frame.R9;
            trapFrame->R10 = frame.R10;
            trapFrame->R11 = frame.R11;
            trapFrame->Rip = frame.Rip;
            trapFrame->Rsp = frame.Rsp;
            trapFrame->EFlags = static_cast<ULONG>(frame.Rflags);
            exceptionFrame->Rbx = frame.Rbx;
            exceptionFrame->Rbp = frame.Rbp;
            exceptionFrame->Rsi = frame.Rsi;
            exceptionFrame->Rdi = frame.Rdi;
            exceptionFrame->R12 = frame.R12;
            exceptionFrame->R13 = frame.R13;
            exceptionFrame->R14 = frame.R14;
            exceptionFrame->R15 = frame.R15;
        }
        if (handled && NT_SUCCESS(static_cast<NTSTATUS>(captured.EventStatus)) &&
            TrapFrame != nullptr && captured.FrameStatus == static_cast<ULONG>(STATUS_SUCCESS))
            static_cast<PKTRAP_FRAME>(TrapFrame)->Rip = captured.Frame.Rip;
        else
            handled = FALSE;
    }
    RtlCopyMemory(&g_KiInspectState, &captured, sizeof(captured));
    InterlockedExchange(&g_KiInspectState.Valid, 1);
    return handled;
}
static VOID LmvGuestKiDr3Probe(
    _Inout_ PLMV_GUEST_KI_DR3_PROBE_RESULT output,
    _In_ KPROCESSOR_MODE requestorMode,
    _In_ PFILE_OBJECT owner)
{
    PKEVENT startEvent = nullptr;
    PKEVENT doneEvent = nullptr;
    GROUP_AFFINITY affinity = {};
    GROUP_AFFINITY previous = {};
    PROCESSOR_NUMBER processor = {};
    LMV_GUEST_KI_DISPATCH_INFO info = {};
    LMV_SHARED_RESULT command = {};
    LARGE_INTEGER timeout = {};
    timeout.QuadPart = -30000000LL;
    BOOLEAN pinned = FALSE;
    BOOLEAN armed = FALSE;
    BOOLEAN sessionOwned = FALSE;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    HANDLE startHandle = output->StartEvent;
    HANDLE doneHandle = output->DoneEvent;
    const BOOLEAN redirect = (output->Reserved & 1u) != 0;
    const BOOLEAN inspect = (output->Reserved & 2u) != 0;
    const BOOLEAN captureFrame = (output->Reserved & 4u) != 0;
    const BOOLEAN waitForContinue = (output->Reserved & 8u) != 0;
    const BOOLEAN writeRax = (output->Reserved & 16u) != 0;
    const BOOLEAN handleUd2 = (output->Reserved & 32u) != 0;
    if (waitForContinue)
        timeout.QuadPart = -80000000LL;
    output->Status = static_cast<ULONG>(status);
    output->RedirectStub = 0;
    output->StubHitsBefore = 0;
    output->StubHitsAfter = 0;
    output->InspectStatus = static_cast<ULONG>(STATUS_NOT_FOUND);
    output->InspectReason = 0;
    output->InspectProcessId = 0;
    output->InspectThreadId = 0;
    output->InspectExceptionCode = 0;
    output->InspectFirstChance = 0;
    output->InspectExceptionAddress = 0;
    output->FrameStatus = static_cast<ULONG>(STATUS_NOT_FOUND);
    output->FrameReason = 0;
    RtlZeroMemory(&output->Frame, sizeof(output->Frame));
    output->EventStatus = static_cast<ULONG>(STATUS_NOT_FOUND);
    output->EventSequence = 0;
    if (requestorMode != UserMode || startHandle == nullptr || doneHandle == nullptr ||
        (output->Reserved & ~63u) != 0 ||
        (writeRax && output->Reserved != 31u) || (inspect && !redirect) ||
        (handleUd2 && output->Reserved != 47u) ||
        (captureFrame && !inspect) || (waitForContinue && !captureFrame))
    {
        output->Status = static_cast<ULONG>(STATUS_INVALID_PARAMETER);
        return;
    }
    status = ObReferenceObjectByHandle(startHandle, EVENT_MODIFY_STATE,
        *ExEventObjectType, UserMode, reinterpret_cast<PVOID*>(&startEvent), nullptr);
    if (!NT_SUCCESS(status))
        goto Cleanup;
    status = ObReferenceObjectByHandle(doneHandle, SYNCHRONIZE,
        *ExEventObjectType, UserMode, reinterpret_cast<PVOID*>(&doneEvent), nullptr);
    if (!NT_SUCCESS(status))
        goto Cleanup;
    status = LmvGuestQueryKiDispatchInfo(&info);
    output->KiAddress = info.RoutineAddress;
    output->PrologueVerified = info.PrologueVerified;
    if (!NT_SUCCESS(status))
        goto Cleanup;
    if (info.PrologueVerified == 0 || info.RoutineAddress < info.KernelBase ||
        info.RoutineAddress - info.KernelBase >= info.KernelImageSize ||
        (info.RoutineAddress & (PAGE_SIZE - 1)) !=
            (info.GuestPhysicalAddress & (PAGE_SIZE - 1)))
    {
        status = STATUS_NOT_SUPPORTED;
        goto Cleanup;
    }
    KeGetCurrentProcessorNumberEx(&processor);
    affinity.Group = processor.Group;
    affinity.Mask = 1ULL << processor.Number;
    KeSetSystemGroupAffinityThread(&affinity, &previous);
    pinned = TRUE;
    if (waitForContinue)
    {
        ExAcquireFastMutex(&g_KiDr3EventMutex);
        BOOLEAN available = g_KiDr3EventOwner == nullptr;
        for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
            available = available && g_KiDr3Events[index].State == LMV_GUEST_STUB_STATE_IDLE;
        ExReleaseFastMutex(&g_KiDr3EventMutex);
        if (!available)
        {
            status = STATUS_DEVICE_BUSY;
            goto Cleanup;
        }
    }
    if (redirect)
    {
        if (inspect)
        {
            RtlZeroMemory(&g_KiInspectState, sizeof(g_KiInspectState));
            g_KiInspectState.Status = static_cast<ULONG>(STATUS_NOT_FOUND);
            g_KiInspectState.EventStatus = static_cast<ULONG>(STATUS_NOT_FOUND);
            g_KiInspectTargetProcessId = static_cast<ULONG>(
                reinterpret_cast<ULONG_PTR>(PsGetCurrentProcessId()));
            if (waitForContinue)
            {
                ExAcquireFastMutex(&g_KiDr3EventMutex);
                ++g_KiDr3EventGeneration;
                g_KiDr3EventOwner = owner;
                g_KiDr3ControllerProcessId = HandleToULong(PsGetCurrentProcessId());
                sessionOwned = TRUE;
                LmvGuestKiDr3RefreshAvailable();
                ExReleaseFastMutex(&g_KiDr3EventMutex);
                InterlockedExchange(&g_KiDr3RaxWriteEnabled, writeRax ? 1 : 0);
                InterlockedExchange(&g_KiDr3HandleEnabled, handleUd2 ? 1 : 0);
                InterlockedExchange(&g_KiDr3WaitEnabled, 1);
            }
            InterlockedExchange(&g_KiInspectCaptureFrame, captureFrame ? 1 : 0);
            InterlockedExchange(&g_KiInspectEnabled, 1);
        }
        output->RedirectStub = inspect ?
            reinterpret_cast<ULONGLONG>(&LmvGuestKiDr3InspectStub) :
            reinterpret_cast<ULONGLONG>(&LmvGuestKiDr3RedirectStub);
        output->StubHitsBefore = static_cast<ULONGLONG>(
            InterlockedCompareExchange64(&g_LmvGuestKiDr3StubHits, 0, 0));
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(
            &g_LmvGuestKiDr3ReturnRip), static_cast<LONG64>(output->KiAddress));
    }
    status = LmvGuestIssueSharedCommand(
        redirect ? LmvDebugCommandArmKiDr3RedirectOnce : LmvDebugCommandArmKiDr3Once,
        output->KiAddress, &command, output->RedirectStub);
    if (!NT_SUCCESS(status))
    {
        if (command.Status != 0)
            status = static_cast<NTSTATUS>(command.Status);
        goto Cleanup;
    }
    armed = TRUE;
    output->HitsBefore = command.Value0;
    output->Step = 1;
    KeSetEvent(startEvent, IO_NO_INCREMENT, FALSE);
    status = KeWaitForSingleObject(doneEvent, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
        goto Cleanup;
    output->Step = 2;
    status = LmvGuestIssueSharedCommand(LmvDebugCommandQueryKiDr3Once, 0, &command);
    if (!NT_SUCCESS(status))
        goto Cleanup;
    output->HitsAfter = command.Value0;
    output->LastRip = command.Value1;
    output->LastDr6 = command.Value2;
    output->LastCr3 = command.Value3;
    output->ArmedDr7After = command.Flags;
    {
        LMV_SHARED_RESULT privateState = {};
        status = LmvGuestIssueSharedCommand(
            LmvDebugCommandQueryPrivateDr3, 0, &privateState);
        if (!NT_SUCCESS(status))
            goto Cleanup;
        output->ArmedTargetAfter = privateState.Value1;
    }
    if (output->HitsAfter != output->HitsBefore + 1 ||
        output->LastRip != output->KiAddress ||
        (output->LastDr6 & 8) == 0 || output->LastCr3 == 0)
    {
        status = STATUS_DATA_ERROR;
        goto Cleanup;
    }
    if (output->ArmedTargetAfter != 0 || (output->ArmedDr7After & 0x40) != 0)
    {
        status = STATUS_DATA_ERROR;
        goto Cleanup;
    }
    status = LmvGuestIssueSharedCommand(LmvDebugCommandQueryKiDr3Args, 0, &command);
    if (!NT_SUCCESS(status))
        goto Cleanup;
    output->LastRcx = command.Value0;
    output->LastRdx = command.Value1;
    output->LastR8 = command.Value2;
    output->LastR9 = command.Value3;
    output->ProcessorGroup = static_cast<USHORT>(command.Flags >> 16);
    output->ProcessorNumber = static_cast<USHORT>(command.Flags);
    status = LmvGuestIssueSharedCommand(LmvDebugCommandQueryKiDr3Frame, 0, &command);
    if (!NT_SUCCESS(status))
        goto Cleanup;
    output->LastRsp = command.Value0;
    output->LastRflags = command.Value1;
    output->LastRax = command.Value2;
    output->LastRbp = command.Value3;
    if (output->ProcessorGroup != processor.Group ||
        output->ProcessorNumber != processor.Number)
    {
        status = STATUS_DATA_ERROR;
        goto Cleanup;
    }
    if (redirect)
    {
        output->StubHitsAfter = static_cast<ULONGLONG>(
            InterlockedCompareExchange64(&g_LmvGuestKiDr3StubHits, 0, 0));
        if (output->StubHitsAfter != output->StubHitsBefore + 1)
        {
            status = STATUS_DATA_ERROR;
            goto Cleanup;
        }
    }
    if (inspect && (InterlockedCompareExchange(&g_KiInspectState.Valid, 0, 0) == 0 ||
        g_KiInspectState.Status != static_cast<ULONG>(STATUS_SUCCESS) ||
        g_KiInspectState.ProcessId != g_KiInspectTargetProcessId ||
        (g_KiInspectState.ExceptionCode != static_cast<ULONG>(STATUS_ILLEGAL_INSTRUCTION) &&
         g_KiInspectState.ExceptionCode != LMV_KI_EXCEPTION_INTERNAL_INVALID_OP) ||
         g_KiInspectState.FirstChance != 1 ||
          g_KiInspectState.ThreadId == 0 ||
          (captureFrame && (g_KiInspectState.FrameStatus != static_cast<ULONG>(STATUS_SUCCESS) ||
                            g_KiInspectState.FrameReason != 12)) ||
          (waitForContinue && (g_KiInspectState.EventStatus != static_cast<ULONG>(STATUS_SUCCESS) ||
                               g_KiInspectState.EventSequence == 0))))
    {
        status = STATUS_DATA_ERROR;
        goto Cleanup;
    }
    output->Step = 3;


Cleanup:

    if (inspect && pinned)
    {
        output->InspectStatus = g_KiInspectState.Status;
        output->InspectReason = g_KiInspectState.Reason;
        output->InspectProcessId = g_KiInspectState.ProcessId;
        output->InspectThreadId = g_KiInspectState.ThreadId;
        output->InspectExceptionCode = g_KiInspectState.ExceptionCode;
        output->InspectFirstChance = g_KiInspectState.FirstChance;
        output->InspectExceptionAddress = g_KiInspectState.ExceptionAddress;
        output->FrameStatus = g_KiInspectState.FrameStatus;
        output->FrameReason = g_KiInspectState.FrameReason;
        output->Frame = g_KiInspectState.Frame;
        output->EventStatus = g_KiInspectState.EventStatus;
        output->EventSequence = g_KiInspectState.EventSequence;
    }
    if (armed)
    {
        LMV_SHARED_RESULT disarm = {};
        NTSTATUS disarmStatus = LmvGuestIssueSharedCommand(
            LmvDebugCommandDisarmPrivateDr3, 0, &disarm);
        if (!NT_SUCCESS(disarmStatus))
            status = disarm.Status != 0 ? static_cast<NTSTATUS>(disarm.Status) : disarmStatus;
        else if (NT_SUCCESS(status) && disarm.Value0 == output->HitsAfter &&
            output->Step == 3)
            output->Step = 4;
        else if (NT_SUCCESS(status))
            status = STATUS_DATA_ERROR;
    }
    if (inspect && pinned)
    {
        InterlockedExchange(&g_KiInspectEnabled, 0);
        InterlockedExchange(&g_KiInspectCaptureFrame, 0);
    }
    if (waitForContinue && sessionOwned)
    {
        InterlockedExchange(&g_KiDr3RaxWriteEnabled, 0);
        InterlockedExchange(&g_KiDr3HandleEnabled, 0);
        InterlockedExchange(&g_KiDr3WaitEnabled, 0);
        ExAcquireFastMutex(&g_KiDr3EventMutex);
        g_KiDr3EventOwner = nullptr;
        g_KiDr3ControllerProcessId = 0;
        LmvGuestKiDr3CancelEvents(nullptr);
        KeSetEvent(&g_KiDr3EventAvailable, IO_NO_INCREMENT, FALSE);
        ExReleaseFastMutex(&g_KiDr3EventMutex);
    }
    if (pinned)
        KeRevertToUserGroupAffinityThread(&previous);
    if (doneEvent != nullptr)
        ObDereferenceObject(doneEvent);
    if (startEvent != nullptr)
        ObDereferenceObject(startEvent);
    output->Status = static_cast<ULONG>(status);
}
static NTSTATUS LmvGuestKiExecMirrorSmoke(
    _Out_ PLMV_GUEST_KI_EXEC_MIRROR_SMOKE Output)
{
    constexpr ULONGLONG expected = 0x13579BDF2468ACE0ull;
    PMDL mdl = nullptr;
    PVOID mapping = nullptr;
    NTSTATUS status = STATUS_SUCCESS;
    PHYSICAL_ADDRESS lowest = {};
    PHYSICAL_ADDRESS highest = {};
    PHYSICAL_ADDRESS boundary = {};
    Output->Version = LMV_CPUID_PROTOCOL_VERSION;
    Output->Size = sizeof(*Output);
    Output->ExpectedValue = expected;
    Output->Stage = 1;
    highest.QuadPart = MAXULONG64;
    mdl = MmAllocatePagesForMdlEx(lowest, highest, boundary, PAGE_SIZE,
        MmCached, MM_ALLOCATE_FULLY_REQUIRED);
    if (mdl == nullptr)
        status = STATUS_INSUFFICIENT_RESOURCES;
    else
    {
        mapping = MmMapLockedPagesSpecifyCache(mdl, KernelMode, MmCached,
            nullptr, FALSE, NormalPagePriority);
        if (mapping == nullptr)
            status = STATUS_INSUFFICIENT_RESOURCES;
        else
        {
            Output->VirtualAddress = reinterpret_cast<ULONGLONG>(mapping);
            Output->PhysicalAddress =
                static_cast<ULONGLONG>(MmGetMdlPfnArray(mdl)[0]) << PAGE_SHIFT;
            if (Output->PhysicalAddress == 0)
                status = STATUS_DATA_ERROR;
            else
            {
                PUCHAR code = static_cast<PUCHAR>(mapping);
                RtlZeroMemory(code, PAGE_SIZE);
                code[0] = 0x48;
                code[1] = 0xB8;
                RtlCopyMemory(code + 2, &expected, sizeof(expected));
                code[10] = 0xC3;
                KeMemoryBarrier();
                status = MmProtectMdlSystemAddress(mdl, PAGE_EXECUTE_READ);
                if (NT_SUCCESS(status))
                {
                    Output->ProtectionApplied = 1;
                    Output->Stage = 2;
                    using EXECUTE_MIRROR_FUNCTION = ULONGLONG(*)();
                    __try
                    {
                        Output->ObservedValue =
                            reinterpret_cast<EXECUTE_MIRROR_FUNCTION>(mapping)();
                        Output->Stage = 3;
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        status = GetExceptionCode();
                    }
                    if (NT_SUCCESS(status) &&
                        Output->ObservedValue != Output->ExpectedValue)
                        status = STATUS_DATA_ERROR;
                    MmProtectMdlSystemAddress(mdl, PAGE_READWRITE);
                }
            }
            MmUnmapLockedPages(mapping, mdl);
        }
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
    }
    Output->Status = static_cast<ULONG>(status);
    if (NT_SUCCESS(status))
        Output->Stage = 4;
    return status;
}
static NTSTATUS LmvGuestQueryKiStubSites(_Out_ PLMV_GUEST_KI_STUB_SITES Output)
{
    Output->Version = LMV_CPUID_PROTOCOL_VERSION;
    Output->Size = sizeof(*Output);
    ExAcquireFastMutex(&g_KiSiteMutex);
    if (g_KiNxSiteBuffer == nullptr)
        g_KiNxSiteBuffer = ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE, 'iSmL');
    if (g_KiNxSiteBuffer == nullptr)
    {
        Output->Status = static_cast<ULONG>(STATUS_INSUFFICIENT_RESOURCES);
        ExReleaseFastMutex(&g_KiSiteMutex);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Output->Flags = LMV_KI_SITE_IMAGE_CODE | LMV_KI_SITE_NX_BUFFER;
    Output->ImageCodeAddress = reinterpret_cast<ULONGLONG>(&LmvGuestKiImageCodeAnchor);
    Output->NxBufferAddress = reinterpret_cast<ULONGLONG>(g_KiNxSiteBuffer);
    Output->NxBufferSize = PAGE_SIZE;
    Output->Status = static_cast<ULONG>(STATUS_SUCCESS);
    ExReleaseFastMutex(&g_KiSiteMutex);
    return STATUS_SUCCESS;
}

static NTSTATUS
LmvGuestDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;

    ExAcquireFastMutex(&g_KiDr3EventMutex);
    ++g_KiActiveRequests;
    BOOLEAN blocked = g_KiUnloadPreparing != 0;
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    ULONG sessionOperation = 0;
    if (code == IOCTL_LMV_GUEST_KI_SESSION &&
        stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(ULONG) * 3)
        sessionOperation = static_cast<PULONG>(Irp->AssociatedIrp.SystemBuffer)[2];
    BOOLEAN drainRequest = code == IOCTL_LMV_GUEST_KI_SESSION &&
        (sessionOperation == LMV_KI_SESSION_QUERY || sessionOperation == LMV_KI_SESSION_PREPARE_UNLOAD);
    BOOLEAN persistentAllowed = code == IOCTL_LMV_GUEST_KI_SESSION ||
        code == IOCTL_LMV_GUEST_KI_DR3_GET_CONTEXT || code == IOCTL_LMV_GUEST_KI_DR3_SET_CONTEXT ||
        code == IOCTL_LMV_GUEST_KI_DR3_WAIT_EVENT || code == IOCTL_LMV_GUEST_KI_DR3_CONTINUE ||
        code == LMV_DBG_IOCTL_PING || code == IOCTL_LMV_GUEST_POLL_HOST_EVENTS ||
        code == IOCTL_LMV_GUEST_READ_PROCESS_MEMORY ||
        code == IOCTL_LMV_GUEST_WRITE_PROCESS_MEMORY ||
        code == IOCTL_LMV_GUEST_DR2_WATCH;
    if (blocked && !drainRequest)
        status = STATUS_DELETE_PENDING;
    else if (g_KiPersistentOnly && !persistentAllowed)
        status = STATUS_DEVICE_BUSY;
    else if (drainRequest)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_KI_DRAIN) ||
            outputLength < sizeof(LMV_GUEST_KI_DRAIN))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            PLMV_GUEST_KI_DRAIN drain = static_cast<PLMV_GUEST_KI_DRAIN>(Irp->AssociatedIrp.SystemBuffer);
            status = LmvGuestControlKiDrain(drain, stack->FileObject);
            information = sizeof(*drain);
        }
    }    else if (code == IOCTL_LMV_SET_DEBUG_MODE)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(ULONG))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            ULONG mode = *static_cast<PULONG>(Irp->AssociatedIrp.SystemBuffer);
            if (mode == LMV_DEBUG_MODE_INTERACTIVE || mode == LMV_DEBUG_MODE_TRANSPARENT)
            {
                // Update local mode
                InterlockedExchange((volatile LONG*)&g_KiSessionMode, mode);

                // Notify Host driver via simple CPUID command (old method)
                LMV_CPUID_RESULT result = {};
                LmvGuestCpuidCall(LmvCpuidCommandSyncKiSession, mode, &result);

                if (result.Status != LMV_CPUID_RESULT_SUCCESS)
                {
                    status = STATUS_UNSUCCESSFUL;
                }
                else
                {
                    status = STATUS_SUCCESS;
                    information = 0;
                }
            }
            else
            {
                status = STATUS_INVALID_PARAMETER;
            }
        }
    }    else if (code == IOCTL_LMV_GUEST_POLL_HOST_EVENTS)
    {
        // Poll Host event queue for transparent mode
        if (outputLength < sizeof(HOST_EXCEPTION_EVENT))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            information = 0;
        }
        else
        {
            KeWaitForSingleObject(&g_KiSessionGate, Executive, KernelMode, FALSE, nullptr);
            if (g_KiSessionOwner != stack->FileObject)
                status = STATUS_ACCESS_DENIED;
            else if (!g_KiSessionActive || g_KiSessionMode != LMV_DEBUG_MODE_TRANSPARENT)
                status = STATUS_INVALID_DEVICE_STATE;
            else
            {
                ExAcquireFastMutex(&g_CommandMutex);
                LMV_CPUID_SHARED_PAGE_RESULT pageResult = {};
                LmvGuestQuerySharedPage(&pageResult);
                if (pageResult.Status != LMV_CPUID_RESULT_SUCCESS)
                    status = static_cast<NTSTATUS>(pageResult.Status);
                else if (pageResult.Magic != LMV_SHARED_PAGE_MAGIC)
                    status = STATUS_REVISION_MISMATCH;
                else
                {
                    PHYSICAL_ADDRESS pa = {};
                    pa.LowPart = pageResult.GpaLow;
                    pa.HighPart = pageResult.GpaHigh;
                    PLMV_SHARED_PAGE page = static_cast<PLMV_SHARED_PAGE>(
                        MmMapIoSpace(pa, PAGE_SIZE, MmCached));
                    if (page == nullptr)
                        status = STATUS_INSUFFICIENT_RESOURCES;
                    else
                    {
                        if (page->Magic != LMV_SHARED_PAGE_MAGIC ||
                            page->Version != LMV_CPUID_PROTOCOL_VERSION ||
                            page->Size != sizeof(LMV_SHARED_PAGE))
                            status = STATUS_REVISION_MISMATCH;
                        else
                        {
                            LMV_CPUID_RESULT result = {};
                            LmvGuestCpuidCall(LmvCpuidCommandReadHostEvent, 0, &result);
                            status = static_cast<NTSTATUS>(result.Status);
                            if (result.Status == LMV_CPUID_RESULT_SUCCESS && result.Flags == 1)
                            {
                                RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, page->Data,
                                    sizeof(HOST_EXCEPTION_EVENT));
                                information = sizeof(HOST_EXCEPTION_EVENT);
                            }
                            else if (result.Status == LMV_CPUID_RESULT_SUCCESS)
                                status = STATUS_DATA_ERROR;
                        }
                        MmUnmapIoSpace(page, PAGE_SIZE);
                    }
                }
                ExReleaseFastMutex(&g_CommandMutex);
            }
            KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
        }
    }


    else if (code == IOCTL_LMV_GUEST_KI_SESSION)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_KI_SESSION) ||
            outputLength < sizeof(LMV_GUEST_KI_SESSION))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            PLMV_GUEST_KI_SESSION session = static_cast<PLMV_GUEST_KI_SESSION>(Irp->AssociatedIrp.SystemBuffer);
            status = LmvGuestControlKiSession(session, stack->FileObject);
            if (NT_SUCCESS(status))
                information = sizeof(*session);
        }
    }
    else if (code == IOCTL_LMV_GUEST_DR2_WATCH)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_DR2_WATCH) ||
            outputLength < sizeof(LMV_GUEST_DR2_WATCH))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            PLMV_GUEST_DR2_WATCH watch =
                static_cast<PLMV_GUEST_DR2_WATCH>(Irp->AssociatedIrp.SystemBuffer);
            status = LmvGuestControlDr2Watch(watch, stack->FileObject);
            if (NT_SUCCESS(status))
                information = sizeof(*watch);
        }
    }
    else if (code == IOCTL_LMV_GUEST_KI_DR3_GET_CONTEXT || code == IOCTL_LMV_GUEST_KI_DR3_SET_CONTEXT)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_KI_DR3_CONTEXT) ||
            outputLength < sizeof(LMV_GUEST_KI_DR3_CONTEXT))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            PLMV_GUEST_KI_DR3_CONTEXT context = static_cast<PLMV_GUEST_KI_DR3_CONTEXT>(Irp->AssociatedIrp.SystemBuffer);
            status = LmvGuestKiSessionContext(context, stack->FileObject, code == IOCTL_LMV_GUEST_KI_DR3_SET_CONTEXT);
            if (NT_SUCCESS(status))
                information = sizeof(*context);
        }
    }
    else if (g_KiSessionOwner != nullptr &&
        code != IOCTL_LMV_GUEST_KI_DR3_WAIT_EVENT && code != IOCTL_LMV_GUEST_KI_DR3_CONTINUE &&
        code != LMV_DBG_IOCTL_PING &&
        code != IOCTL_LMV_GUEST_READ_PROCESS_MEMORY && code != IOCTL_LMV_GUEST_WRITE_PROCESS_MEMORY &&
        code != IOCTL_LMV_GUEST_DR2_WATCH)
        status = STATUS_DEVICE_BUSY;
    else if ((code == IOCTL_LMV_GUEST_KERNEL_EXEC_SMOKE ||
         code == IOCTL_LMV_GUEST_KERNEL_EXEC_STRESS) &&
        outputLength >= sizeof(LMV_GUEST_KERNEL_EXEC_SMOKE))
    {
        PLMV_GUEST_KERNEL_EXEC_SMOKE output =
            static_cast<PLMV_GUEST_KERNEL_EXEC_SMOKE>(Irp->AssociatedIrp.SystemBuffer);
        RtlZeroMemory(output, sizeof(*output));
        LmvGuestRunKernelExecSmoke(output,
            code == IOCTL_LMV_GUEST_KERNEL_EXEC_STRESS);
        status = STATUS_SUCCESS;
        information = sizeof(*output);
    }
    else if (code == IOCTL_LMV_GUEST_STATIC_STUB_SMOKE &&
        outputLength >= sizeof(LMV_GUEST_STATIC_STUB_SMOKE))
    {
        PLMV_GUEST_STATIC_STUB_SMOKE result =
            static_cast<PLMV_GUEST_STATIC_STUB_SMOKE>(Irp->AssociatedIrp.SystemBuffer);
        RtlZeroMemory(result, sizeof(*result));
        status = LmvGuestRunStaticStubSmoke(result);
        information = sizeof(*result);
    }
    else if (code == IOCTL_LMV_GUEST_QUERY_KI_STUB_SITES &&
        outputLength >= sizeof(LMV_GUEST_KI_STUB_SITES))
    {
        PLMV_GUEST_KI_STUB_SITES sites =
            static_cast<PLMV_GUEST_KI_STUB_SITES>(Irp->AssociatedIrp.SystemBuffer);
        RtlZeroMemory(sites, sizeof(*sites));
        status = LmvGuestQueryKiStubSites(sites);
        information = sizeof(*sites);
    }
    else if (code == IOCTL_LMV_GUEST_EXEC_PROBE &&
        stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(LMV_GUEST_EXEC_PROBE) &&
        outputLength >= sizeof(LMV_GUEST_EXEC_PROBE))
    {
        PLMV_GUEST_EXEC_PROBE probe =
            static_cast<PLMV_GUEST_EXEC_PROBE>(Irp->AssociatedIrp.SystemBuffer);
        status = LmvGuestRunExecProbe(probe, FALSE);
        if (g_ExecProbeMdl != nullptr &&
            g_ExecProbeProcessId == HandleToULong(PsGetCurrentProcessId()) &&
            g_ExecProbeFileObject == nullptr)
            g_ExecProbeFileObject = stack->FileObject;
        if (g_ExecProbeMdl == nullptr && g_ExecProbeFileObject == stack->FileObject)
            g_ExecProbeFileObject = nullptr;
        information = sizeof(*probe);
    }
    else if (code == IOCTL_LMV_GUEST_STUB_TRIGGER)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_STUB_REQUEST))
            status = STATUS_BUFFER_TOO_SMALL;
        else
            status = LmvGuestStubTrigger(static_cast<PLMV_GUEST_STUB_REQUEST>(Irp->AssociatedIrp.SystemBuffer));
    }
    else if (code == IOCTL_LMV_GUEST_STUB_GET_EVENT)
    {
        if (outputLength < sizeof(LMV_GUEST_STUB_EVENT))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            status = LmvGuestStubGetEvent(static_cast<PLMV_GUEST_STUB_EVENT>(Irp->AssociatedIrp.SystemBuffer));
            information = sizeof(LMV_GUEST_STUB_EVENT);
        }
    }
    else if (code == IOCTL_LMV_GUEST_STUB_WAIT_EVENT)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_STUB_WAIT_REQUEST) ||
            outputLength < sizeof(LMV_GUEST_STUB_EVENT))
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            LMV_GUEST_STUB_WAIT_REQUEST request = {};
            RtlCopyMemory(&request, Irp->AssociatedIrp.SystemBuffer, sizeof(request));
            status = LmvGuestStubWaitEvent(
                &request,
                static_cast<PLMV_GUEST_STUB_EVENT>(Irp->AssociatedIrp.SystemBuffer));
            if (NT_SUCCESS(status))
                information = sizeof(LMV_GUEST_STUB_EVENT);
        }
    }    else if (code == IOCTL_LMV_GUEST_STUB_CONTINUE)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_STUB_CONTINUE) ||
            outputLength < sizeof(LMV_GUEST_STUB_CONTINUE))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            PLMV_GUEST_STUB_CONTINUE continueRequest =
                static_cast<PLMV_GUEST_STUB_CONTINUE>(Irp->AssociatedIrp.SystemBuffer);
            status = LmvGuestStubContinue(continueRequest);
            information = sizeof(*continueRequest);
        }
    }
    else if (code == IOCTL_LMV_GUEST_KI_QUEUE_CONTROL)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_KI_QUEUE_CONTROL) ||
            outputLength < sizeof(LMV_GUEST_KI_QUEUE_CONTROL))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            PLMV_GUEST_KI_QUEUE_CONTROL request =
                static_cast<PLMV_GUEST_KI_QUEUE_CONTROL>(Irp->AssociatedIrp.SystemBuffer);
            if (Irp->RequestorMode != UserMode ||
                request->Version != LMV_CPUID_PROTOCOL_VERSION ||
                request->Size != sizeof(*request))
                status = STATUS_INVALID_PARAMETER;
            else
            {
                status = LmvGuestKiQueueControl(request->Operation, stack->FileObject);
                request->Status = static_cast<ULONG>(status);
                if (NT_SUCCESS(status))
                    information = sizeof(*request);
            }
        }
    }
    else if (code == IOCTL_LMV_GUEST_KI_QUEUE_PRODUCE)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_KI_QUEUE_PRODUCE) ||
            outputLength < sizeof(LMV_GUEST_KI_QUEUE_PRODUCE))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            PLMV_GUEST_KI_QUEUE_PRODUCE request =
                static_cast<PLMV_GUEST_KI_QUEUE_PRODUCE>(Irp->AssociatedIrp.SystemBuffer);
            if (Irp->RequestorMode != UserMode ||
                request->Version != LMV_CPUID_PROTOCOL_VERSION ||
                request->Size != sizeof(*request))
                status = STATUS_INVALID_PARAMETER;
            else
            {
                request->Sequence = 0;
                request->ThreadId = 0;
                status = LmvGuestKiQueueProduce(request);
                if (NT_SUCCESS(status))
                    information = sizeof(*request);
            }
        }
    }
    else if (code == IOCTL_LMV_GUEST_KI_DR3_WAIT_EVENT)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_STUB_WAIT_REQUEST) ||
            outputLength < sizeof(LMV_GUEST_KI_DR3_EVENT))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            LMV_GUEST_STUB_WAIT_REQUEST request = {};
            RtlCopyMemory(&request, Irp->AssociatedIrp.SystemBuffer, sizeof(request));
            status = LmvGuestKiDr3WaitEvent(&request,
                static_cast<PLMV_GUEST_KI_DR3_EVENT>(Irp->AssociatedIrp.SystemBuffer),
                stack->FileObject);
            if (status == STATUS_SUCCESS)
                information = sizeof(LMV_GUEST_KI_DR3_EVENT);
        }
    }
    else if (code == IOCTL_LMV_GUEST_KI_DR3_CONTINUE)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_STUB_CONTINUE) ||
            outputLength < sizeof(LMV_GUEST_STUB_CONTINUE))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            PLMV_GUEST_STUB_CONTINUE continuation =
                static_cast<PLMV_GUEST_STUB_CONTINUE>(Irp->AssociatedIrp.SystemBuffer);
            status = LmvGuestKiDr3Continue(continuation, stack->FileObject);
            if (status == STATUS_SUCCESS)
                information = sizeof(*continuation);
        }
    }
    else if (code == IOCTL_LMV_GUEST_KI_DR3_SET_RAX)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_KI_DR3_SET_RAX) ||
            outputLength < sizeof(LMV_GUEST_KI_DR3_SET_RAX))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            PLMV_GUEST_KI_DR3_SET_RAX request =
                static_cast<PLMV_GUEST_KI_DR3_SET_RAX>(Irp->AssociatedIrp.SystemBuffer);
            status = LmvGuestKiDr3SetRax(request, stack->FileObject);
            if (status == STATUS_SUCCESS)
                information = sizeof(*request);
        }
    }
    else if (code == IOCTL_LMV_GUEST_KI_DR3_SET_RIP)
    {
        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(LMV_GUEST_KI_DR3_SET_RIP) ||
            outputLength < sizeof(LMV_GUEST_KI_DR3_SET_RIP))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            PLMV_GUEST_KI_DR3_SET_RIP request =
                static_cast<PLMV_GUEST_KI_DR3_SET_RIP>(Irp->AssociatedIrp.SystemBuffer);
            status = LmvGuestKiDr3SetRip(request, stack->FileObject);
            if (status == STATUS_SUCCESS)
                information = sizeof(*request);
        }
    }
    else if ((code == LMV_DBG_IOCTL_ATTACH_PROCESS || code == LMV_DBG_IOCTL_DETACH_PROCESS) &&
        stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(LMV_DBG_ATTACH_REQUEST))
    {
        PLMV_DBG_ATTACH_REQUEST request = static_cast<PLMV_DBG_ATTACH_REQUEST>(Irp->AssociatedIrp.SystemBuffer);
        LMV_SHARED_RESULT result = {};
        LmvGuestStubReset();
        ULONG command = code == LMV_DBG_IOCTL_ATTACH_PROCESS ? LmvDebugCommandAttachProcess : LmvDebugCommandDetachProcess;
        status = LmvGuestIssueSharedCommand(command, request->ProcessId, &result);
        if (NT_SUCCESS(status) && result.Status != LMV_CPUID_RESULT_SUCCESS)
            status = static_cast<NTSTATUS>(result.Status);
    }
    else if (code == IOCTL_LMV_GUEST_KI_WINDOW &&
             stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(LMV_GUEST_KI_WINDOW_PAGE) &&
             outputLength >= sizeof(LMV_GUEST_KI_WINDOW_PAGE))
    {
        PLMV_GUEST_KI_WINDOW_PAGE output =
            static_cast<PLMV_GUEST_KI_WINDOW_PAGE>(Irp->AssociatedIrp.SystemBuffer);
        const LONG offset = output->PageOffset;
        RtlZeroMemory(output, sizeof(*output));
        output->PageOffset = offset;
        LmvGuestQueryKiWindowPage(output);
        status = STATUS_SUCCESS;
        information = sizeof(*output);
    }
    else if (code == IOCTL_LMV_GUEST_KI_NEXT_PAGE &&
             outputLength >= sizeof(LMV_GUEST_KI_NEXT_PAGE))
    {
        PLMV_GUEST_KI_NEXT_PAGE output =
            static_cast<PLMV_GUEST_KI_NEXT_PAGE>(Irp->AssociatedIrp.SystemBuffer);
        RtlZeroMemory(output, sizeof(*output));
        LmvGuestQueryKiNextPage(output);
        status = STATUS_SUCCESS;
        information = sizeof(*output);
    }
    else if (code == IOCTL_LMV_GUEST_KI_MIRROR_STORAGE &&
             stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(LMV_GUEST_KI_MIRROR_STORAGE) &&
             outputLength >= sizeof(LMV_GUEST_KI_MIRROR_STORAGE))
    {
        PLMV_GUEST_KI_MIRROR_STORAGE output =
            static_cast<PLMV_GUEST_KI_MIRROR_STORAGE>(Irp->AssociatedIrp.SystemBuffer);
        const ULONG operation = output->Operation;
        const ULONG requested = output->RequestedPages;
        RtlZeroMemory(output, sizeof(*output));
        output->Operation = operation;
        output->RequestedPages = requested;
        LmvGuestMirrorStorage(output, stack->FileObject);
        status = STATUS_SUCCESS;
        information = sizeof(*output);
    }
    else if (code == IOCTL_LMV_GUEST_KI_DR3_PROBE &&
             stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(LMV_GUEST_KI_DR3_PROBE_RESULT) &&
             outputLength >= sizeof(LMV_GUEST_KI_DR3_PROBE_RESULT))
    {
        PLMV_GUEST_KI_DR3_PROBE_RESULT output =
            static_cast<PLMV_GUEST_KI_DR3_PROBE_RESULT>(Irp->AssociatedIrp.SystemBuffer);
        ExAcquireFastMutex(&g_SmokeMutex);
        LmvGuestKiDr3Probe(output, Irp->RequestorMode, stack->FileObject);
        ExReleaseFastMutex(&g_SmokeMutex);
        status = STATUS_SUCCESS;
        information = sizeof(*output);
    }    else if (code == IOCTL_LMV_GUEST_DR_COEXIST &&
             stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(LMV_GUEST_DR_COEXIST_RESULT) &&
             outputLength >= sizeof(LMV_GUEST_DR_COEXIST_RESULT))
    {
        PLMV_GUEST_DR_COEXIST_RESULT output =
            static_cast<PLMV_GUEST_DR_COEXIST_RESULT>(Irp->AssociatedIrp.SystemBuffer);
        ExAcquireFastMutex(&g_SmokeMutex);
        LmvGuestDrCoexistSmoke(output, Irp->RequestorMode);
        ExReleaseFastMutex(&g_SmokeMutex);
        status = STATUS_SUCCESS;
        information = sizeof(*output);
    }
    else if (code == IOCTL_LMV_GUEST_DR3_SMOKE &&
             outputLength >= sizeof(LMV_GUEST_DR3_SMOKE_RESULT))
    {
        PLMV_GUEST_DR3_SMOKE_RESULT output =
            static_cast<PLMV_GUEST_DR3_SMOKE_RESULT>(Irp->AssociatedIrp.SystemBuffer);
        RtlZeroMemory(output, sizeof(*output));
        ExAcquireFastMutex(&g_SmokeMutex);
        LmvGuestPrivateDr3Smoke(output);
        ExReleaseFastMutex(&g_SmokeMutex);
        status = STATUS_SUCCESS;
        information = sizeof(*output);
    }    else if (code == IOCTL_LMV_GUEST_KI_EXEC_MIRROR_SMOKE &&
             outputLength >= sizeof(LMV_GUEST_KI_EXEC_MIRROR_SMOKE))
    {
        PLMV_GUEST_KI_EXEC_MIRROR_SMOKE output =
            static_cast<PLMV_GUEST_KI_EXEC_MIRROR_SMOKE>(Irp->AssociatedIrp.SystemBuffer);
        RtlZeroMemory(output, sizeof(*output));
        LmvGuestKiExecMirrorSmoke(output);
        status = STATUS_SUCCESS;
        information = sizeof(*output);
    }    else if (code == IOCTL_LMV_GUEST_KI_PAGE_PREFLIGHT &&
             outputLength >= sizeof(LMV_GUEST_KI_PAGE_PREFLIGHT))
    {
        PLMV_GUEST_KI_PAGE_PREFLIGHT output =
            static_cast<PLMV_GUEST_KI_PAGE_PREFLIGHT>(Irp->AssociatedIrp.SystemBuffer);
        RtlZeroMemory(output, sizeof(*output));
        LmvGuestQueryKiPagePreflight(output);
        status = STATUS_SUCCESS;
        information = sizeof(*output);
    }    else if (code == IOCTL_LMV_GUEST_RESOLVE_KI_DISPATCH &&
             outputLength >= sizeof(LMV_GUEST_KI_DISPATCH_INFO))
    {
        PLMV_GUEST_KI_DISPATCH_INFO output =
            static_cast<PLMV_GUEST_KI_DISPATCH_INFO>(Irp->AssociatedIrp.SystemBuffer);
        RtlZeroMemory(output, sizeof(*output));
        output->Version = LMV_CPUID_PROTOCOL_VERSION;
        output->Size = sizeof(*output);
        status = LmvGuestQueryKiDispatchInfo(output);
        information = sizeof(*output);
    }    else if ((code == IOCTL_LMV_GUEST_DEBUG_HOOK_SMOKE ||
                  code == IOCTL_LMV_GUEST_DEBUG_HOOK_READ_SMOKE) &&
             outputLength >= sizeof(LMV_GUEST_DEBUG_HOOK_SMOKE_RESULT))
    {
        PLMV_GUEST_DEBUG_HOOK_SMOKE_RESULT output =
            static_cast<PLMV_GUEST_DEBUG_HOOK_SMOKE_RESULT>(Irp->AssociatedIrp.SystemBuffer);
        RtlZeroMemory(output, sizeof(*output));
        status = LmvGuestRunDebugHookSmoke(
            output, code == IOCTL_LMV_GUEST_DEBUG_HOOK_READ_SMOKE);
        information = sizeof(*output);
    }    else if (code == LMV_DBG_IOCTL_PTE_HOOK_INSTALL)
    {
        LMV_DBG_NPT_HOOK_REQUEST request = {};
        LMV_DBG_NPT_HOOK_INFO info = {};
        LMV_SHARED_RESULT result = {};
        ULONGLONG guestPhysicalAddress = 0;

        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(request) ||
            outputLength < sizeof(info))
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            RtlCopyMemory(&request, Irp->AssociatedIrp.SystemBuffer, sizeof(request));
            if (request.ProcessId == 0 || request.Address == 0 || request.PatchLength == 0 ||
                request.PatchLength > RTL_NUMBER_OF(request.PatchBytes) ||
                request.PatchOffset >= PAGE_SIZE ||
                request.PatchLength > PAGE_SIZE - request.PatchOffset)
            {
                status = STATUS_INVALID_PARAMETER;
            }
            else
            {
                status = LmvGuestResolveProcessAddress(request.ProcessId, request.Address,
                                                        &guestPhysicalAddress);
                if (NT_SUCCESS(status))
                {
                    request.GuestPhysicalAddress = guestPhysicalAddress & ~(static_cast<ULONGLONG>(PAGE_SIZE) - 1);
                    status = LmvGuestIssueDebugHookCommand(
                        LmvDebugCommandInstallNptHook, &request, &result);
                    info.Status = result.Status;
                    info.Installed = result.Status == LMV_CPUID_RESULT_SUCCESS ? 1u : 0u;
                    info.ProcessId = request.ProcessId;
                    info.Address = request.Address;
                    info.GuestPhysicalAddress = request.GuestPhysicalAddress;
                    info.OriginalHostPhysicalAddress = result.Value2;
                    info.ShadowHostPhysicalAddress = result.Value3;
                    if (NT_SUCCESS(status) && result.Status != LMV_CPUID_RESULT_SUCCESS)
                        status = static_cast<NTSTATUS>(result.Status);
                    if (NT_SUCCESS(status))
                    {
                        ULONG seen = 0;
                        ULONG expected = 0;
                        if (!LmvGuestFlushAllProcessors(&seen, &expected))
                            status = STATUS_UNSUCCESSFUL;
                    }
                }
            }
        }
        if (outputLength >= sizeof(info))
        {
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &info, sizeof(info));
            information = sizeof(info);
        }
    }
    else if (code == LMV_DBG_IOCTL_PTE_HOOK_REMOVE)
    {
        LMV_DBG_ATTACH_REQUEST attach = {};
        LMV_DBG_NPT_HOOK_REQUEST request = {};
        LMV_DBG_NPT_HOOK_INFO info = {};
        LMV_SHARED_RESULT result = {};

        if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(attach) ||
            outputLength < sizeof(info))
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            RtlCopyMemory(&attach, Irp->AssociatedIrp.SystemBuffer, sizeof(attach));
            request.ProcessId = attach.ProcessId;
            status = LmvGuestIssueDebugHookCommand(
                LmvDebugCommandRemoveNptHook, &request, &result);
            info.Status = result.Status;
            info.Installed = 0;
            info.ProcessId = attach.ProcessId;
            if (NT_SUCCESS(status) && result.Status != LMV_CPUID_RESULT_SUCCESS)
                status = static_cast<NTSTATUS>(result.Status);
            if (NT_SUCCESS(status))
            {
                ULONG seen = 0;
                ULONG expected = 0;
                if (!LmvGuestFlushAllProcessors(&seen, &expected))
                    status = STATUS_UNSUCCESSFUL;
            }
        }
        if (outputLength >= sizeof(info))
        {
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &info, sizeof(info));
            information = sizeof(info);
        }
    }
    else if (code == LMV_DBG_IOCTL_PING &&
             outputLength >= sizeof(LMV_DBG_PING_REPLY))
    {
        PLMV_DBG_PING_REPLY reply = static_cast<PLMV_DBG_PING_REPLY>(Irp->AssociatedIrp.SystemBuffer);
        RtlZeroMemory(reply, sizeof(*reply));
        reply->Magic = LMV_DBG_PING_MAGIC;
        reply->Version = LMV_DBG_PROTOCOL_VERSION;
        status = STATUS_SUCCESS;
        information = sizeof(*reply);
    }
    else if (code == IOCTL_LMV_GUEST_QUERY_STATUS &&
        outputLength >= sizeof(LMV_GUEST_QUERY_STATUS))
    {
        PLMV_GUEST_QUERY_STATUS output =
            static_cast<PLMV_GUEST_QUERY_STATUS>(Irp->AssociatedIrp.SystemBuffer);
        LMV_SHARED_RESULT result = {};
        status = LmvGuestIssueSharedCommand(
            LmvCpuidCommandQueryStatus, 0, &result);
        output->Version = LMV_CPUID_PROTOCOL_VERSION;
        output->Size = sizeof(*output);
        output->Status = NT_SUCCESS(status) ? result.Status : status;
        output->Flags = result.Flags;
        information = sizeof(*output);
    }
    else if (code == IOCTL_LMV_GUEST_QUERY_NPT_STATS &&
             outputLength >= sizeof(LMV_GUEST_QUERY_NPT_STATS))
    {
        PLMV_GUEST_QUERY_NPT_STATS output =
            static_cast<PLMV_GUEST_QUERY_NPT_STATS>(Irp->AssociatedIrp.SystemBuffer);
        LMV_SHARED_RESULT result = {};
        status = LmvGuestIssueSharedCommand(
            LmvCpuidCommandQueryNptStats, 0, &result);
        output->Version = LMV_CPUID_PROTOCOL_VERSION;
        output->Size = sizeof(*output);
        output->Status = NT_SUCCESS(status) ? result.Status : status;
        output->VmExitCount = result.Value0;
        output->NpfCount = result.Value1;
        output->ProcessorCount = result.Value2;
        output->SharedPageGpa = result.Value3;
        information = sizeof(*output);
    }
    else if (code == IOCTL_LMV_GUEST_NPT_SMOKE_TEST &&
             outputLength >= sizeof(LMV_GUEST_NPT_SMOKE_RESULT))
    {
        PLMV_GUEST_NPT_SMOKE_RESULT output =
            static_cast<PLMV_GUEST_NPT_SMOKE_RESULT>(Irp->AssociatedIrp.SystemBuffer);
        RtlZeroMemory(output, sizeof(*output));
        status = LmvGuestRunNptSmokeTest(output);
        information = sizeof(*output);
    }
    else if (code == IOCTL_LMV_GUEST_READ_PROCESS_MEMORY &&
             stack->Parameters.DeviceIoControl.InputBufferLength >= FIELD_OFFSET(LMV_GUEST_MEMORY_REQUEST, Data) &&
             outputLength >= FIELD_OFFSET(LMV_GUEST_MEMORY_RESPONSE, Data))
    {
        LMV_GUEST_MEMORY_REQUEST request =
            *static_cast<PLMV_GUEST_MEMORY_REQUEST>(Irp->AssociatedIrp.SystemBuffer);
        PLMV_GUEST_MEMORY_RESPONSE response =
            static_cast<PLMV_GUEST_MEMORY_RESPONSE>(Irp->AssociatedIrp.SystemBuffer);
        ULONG bytes = 0;
        ULONG pages = 0;
        RtlZeroMemory(response, outputLength);
        response->Version = LMV_CPUID_PROTOCOL_VERSION;
        response->Size = FIELD_OFFSET(LMV_GUEST_MEMORY_RESPONSE, Data);
        response->ProcessId = request.ProcessId;
        response->Address = request.Address;
        response->RequestedLength = request.Length;
        if (request.Version != LMV_CPUID_PROTOCOL_VERSION || request.Size != FIELD_OFFSET(LMV_GUEST_MEMORY_REQUEST, Data) ||
            request.Length == 0 || request.Length > LMV_MEMORY_IO_MAX_SIZE ||
            outputLength < FIELD_OFFSET(LMV_GUEST_MEMORY_RESPONSE, Data) + request.Length)
            status = STATUS_INVALID_PARAMETER;
        else
            status = LmvGuestReadProcessMemory(request.ProcessId, request.Address, response->Data,
                                                request.Length, &bytes, &pages, response->ValidPageBitmap);
        response->Status = status;
        response->BytesTransferred = bytes;
        response->PageCount = pages;
        information = FIELD_OFFSET(LMV_GUEST_MEMORY_RESPONSE, Data) + request.Length;
    }
    else if (code == IOCTL_LMV_GUEST_WRITE_PROCESS_MEMORY &&
             stack->Parameters.DeviceIoControl.InputBufferLength >= FIELD_OFFSET(LMV_GUEST_MEMORY_REQUEST, Data) &&
             outputLength >= FIELD_OFFSET(LMV_GUEST_MEMORY_RESPONSE, Data))
    {
        LMV_GUEST_MEMORY_REQUEST request =
            *static_cast<PLMV_GUEST_MEMORY_REQUEST>(Irp->AssociatedIrp.SystemBuffer);
        PUCHAR data = nullptr;
        PLMV_GUEST_MEMORY_RESPONSE response =
            static_cast<PLMV_GUEST_MEMORY_RESPONSE>(Irp->AssociatedIrp.SystemBuffer);
        ULONG bytes = 0;
        ULONG pages = 0;
        if (request.Length != 0 && request.Length <= LMV_MEMORY_IO_MAX_SIZE &&
            stack->Parameters.DeviceIoControl.InputBufferLength >= FIELD_OFFSET(LMV_GUEST_MEMORY_REQUEST, Data) + request.Length)
        {
            data = static_cast<PUCHAR>(ExAllocatePoolZero(NonPagedPoolNx, request.Length, 'mVmL'));
            if (data != nullptr) RtlCopyMemory(data, request.Data, request.Length);
        }
        RtlZeroMemory(response, outputLength);
        response->Version = LMV_CPUID_PROTOCOL_VERSION;
        response->Size = FIELD_OFFSET(LMV_GUEST_MEMORY_RESPONSE, Data);
        response->ProcessId = request.ProcessId;
        response->Address = request.Address;
        response->RequestedLength = request.Length;
        if (request.Version != LMV_CPUID_PROTOCOL_VERSION || request.Size != FIELD_OFFSET(LMV_GUEST_MEMORY_REQUEST, Data) ||
            request.Length == 0 || request.Length > LMV_MEMORY_IO_MAX_SIZE ||
            outputLength < FIELD_OFFSET(LMV_GUEST_MEMORY_RESPONSE, Data) + request.Length)
            status = STATUS_INVALID_PARAMETER;
        else
            status = data == nullptr ? STATUS_INSUFFICIENT_RESOURCES :
                LmvGuestWriteProcessMemory(request.ProcessId, request.Address, data,
                                           request.Length, &bytes, &pages, response->ValidPageBitmap);
        if (data != nullptr) ExFreePoolWithTag(data, 'mVmL');
        response->Status = status;
        response->BytesTransferred = bytes;
        response->PageCount = pages;
        information = FIELD_OFFSET(LMV_GUEST_MEMORY_RESPONSE, Data) + request.Length;
    }    else if (code == IOCTL_LMV_GUEST_QUERY_STATUS ||
             code == IOCTL_LMV_GUEST_QUERY_NPT_STATS ||
             code == IOCTL_LMV_GUEST_NPT_SMOKE_TEST ||
             code == IOCTL_LMV_GUEST_READ_PROCESS_MEMORY ||
             code == IOCTL_LMV_GUEST_WRITE_PROCESS_MEMORY)
    {
        status = STATUS_BUFFER_TOO_SMALL;
    }

    ExAcquireFastMutex(&g_KiDr3EventMutex);
    --g_KiActiveRequests;
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS
LmvGuestCleanup(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    KeWaitForSingleObject(&g_KiSessionGate, Executive, KernelMode, FALSE, nullptr);
    if (g_KiSessionOwner == IoGetCurrentIrpStackLocation(Irp)->FileObject)
    {
        LMV_GUEST_KI_SESSION session = {};
        NTSTATUS stopped = LmvGuestStopKiSession(&session);
        if (!NT_SUCCESS(stopped))
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "[LMvtdriverGuest] persistent Ki session stop failed; restore VM snapshot.\n");
            ExAcquireFastMutex(&g_KiDr3EventMutex);
            g_KiSessionOwner = nullptr;
            ExReleaseFastMutex(&g_KiDr3EventMutex);
        }
    }
    KeSetEvent(&g_KiSessionGate, IO_NO_INCREMENT, FALSE);
    ExAcquireFastMutex(&g_KiDr3EventMutex);
    if (g_KiDr3EventOwner == IoGetCurrentIrpStackLocation(Irp)->FileObject)
    {
        InterlockedExchange(&g_KiDr3RaxWriteEnabled, 0);
        InterlockedExchange(&g_KiDr3HandleEnabled, 0);
        InterlockedExchange(&g_KiDr3WaitEnabled, 0);
        g_KiDr3QueueSmokeActive = FALSE;
        g_KiDr3EventOwner = nullptr;
        g_KiDr3ControllerProcessId = 0;
        LmvGuestKiDr3CancelEvents(nullptr);
        KeSetEvent(&g_KiDr3EventAvailable, IO_NO_INCREMENT, FALSE);
    }
    else
        LmvGuestKiDr3CancelEvents(IoGetCurrentIrpStackLocation(Irp)->FileObject);
    ExReleaseFastMutex(&g_KiDr3EventMutex);
    ExAcquireFastMutex(&g_KiMirrorMutex);
    if (g_KiMirrorAllocation != nullptr &&
        g_KiMirrorOwner == IoGetCurrentIrpStackLocation(Irp)->FileObject)
    {
        LmvGuestReleaseMirrorStorage();
    }
    ExReleaseFastMutex(&g_KiMirrorMutex);
    if (IoGetCurrentIrpStackLocation(Irp)->FileObject == g_ExecProbeFileObject)
    {
        LMV_GUEST_EXEC_PROBE probe = {};
        probe.Operation = LMV_EXEC_PROBE_DISARM;
        if (NT_SUCCESS(LmvGuestRunExecProbe(&probe, TRUE)))
            g_ExecProbeFileObject = nullptr;
        else
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "[LMvtdriverGuest] execution probe cleanup failed; revert VM snapshot.\n");
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static VOID
LmvGuestUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    LmvGuestStubReset();
    if (g_KernelProbeStage != 0 && !NT_SUCCESS(LmvGuestCleanupKernelProbe(nullptr, nullptr)))
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverGuest] kernel execution probe cleanup failed; revert VM snapshot.\n");
    if (g_ExecProbeMdl != nullptr)
    {
        LMV_GUEST_EXEC_PROBE probe = {};
        probe.Operation = LMV_EXEC_PROBE_DISARM;
        if (!NT_SUCCESS(LmvGuestRunExecProbe(&probe, TRUE)))
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "[LMvtdriverGuest] execution probe disarm failed; revert VM snapshot.\n");
    }
    if (g_KiNxSiteBuffer != nullptr)
    {
        ExFreePoolWithTag(g_KiNxSiteBuffer, 'iSmL');
        g_KiNxSiteBuffer = nullptr;
    }
    ExAcquireFastMutex(&g_KiMirrorMutex);
    if (g_KiMirrorAllocation != nullptr)
    {
        LmvGuestReleaseMirrorStorage();
    }
    ExReleaseFastMutex(&g_KiMirrorMutex);
    IoDeleteSymbolicLink(&g_SymbolicLink);
    IoDeleteSymbolicLink(&g_DebugSymbolicLink);
    if (g_DeviceObject != nullptr)
    {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = nullptr;
    }
    if (g_KiGuestRetired)
    {
        LMV_CPUID_RESULT result = {};
        LmvGuestCpuidCall(LmvCpuidCommandReleaseKiGuest, 0, &result);
        if (result.Status != LMV_CPUID_RESULT_SUCCESS)
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "[LMvtdriverGuest] Host release failed; do not force unload Host.\n");
    }
}

extern "C"
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    UNREFERENCED_PARAMETER(RegistryPath);
    UNICODE_STRING deviceName;
    RtlInitUnicodeString(&deviceName, L"\\Device\\LMvtdriverGuest");
    RtlInitUnicodeString(&g_SymbolicLink, L"\\DosDevices\\LMvtdriverGuest");
    RtlInitUnicodeString(&g_DebugSymbolicLink, L"\\DosDevices\\LMdbgsys");

    NTSTATUS status = IoCreateDevice(
        DriverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN, FALSE, &g_DeviceObject);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = IoCreateSymbolicLink(&g_SymbolicLink, &deviceName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = nullptr;
        return status;
    }

    status = IoCreateSymbolicLink(&g_DebugSymbolicLink, &deviceName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteSymbolicLink(&g_SymbolicLink);
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = nullptr;
        return status;
    }

    ExInitializeFastMutex(&g_CommandMutex);
    ExInitializeFastMutex(&g_SmokeMutex);
    ExInitializeFastMutex(&g_KiSiteMutex);
    ExInitializeFastMutex(&g_KiMirrorMutex);
    ExInitializeFastMutex(&g_StubMutex);
    ExInitializeFastMutex(&g_KiDr3EventMutex);
    KeInitializeEvent(&g_KiSessionGate, SynchronizationEvent, TRUE);
    g_GuestDriverObject = DriverObject;
    KeInitializeEvent(&g_StubEventAvailable, NotificationEvent, FALSE);
    KeInitializeEvent(&g_KiDr3EventAvailable, NotificationEvent, FALSE);
    for (ULONG index = 0; index < kKiDr3EventCapacity; ++index)
        KeInitializeEvent(&g_KiDr3Events[index].Completed, NotificationEvent, FALSE);
    g_StubQueueHead = 0;
    g_StubQueueTail = 0;
    g_StubQueueCount = 0;
    g_StubActive = FALSE;
    DriverObject->DriverUnload = LmvGuestUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = LmvGuestCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = LmvGuestCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = LmvGuestCleanup;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = LmvGuestDeviceControl;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
