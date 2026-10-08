#include <ntifs.h>
#include "static_probe_sample.hpp"

extern "C" ULONGLONG LmvGuestKernelProbeOriginal(ULONGLONG Token, ULONGLONG Mode,
    LMV_STATIC_PROBE_SAMPLE* Sample);
extern "C" ULONGLONG LmvGuestKernelProbeNonEntry();

struct LMV_KERNEL_PROBE_WORKER
{
    HANDLE ThreadHandle;
    PKEVENT StartEvent;
    volatile LONG* Cancel;
    PROCESSOR_NUMBER Processor;
    ULONGLONG OriginalAddress;
    ULONGLONG OriginalBytes;
    ULONG Completed;
    ULONG Failed;
};

static VOID NTAPI LmvGuestKernelProbeWorker(_In_ PVOID Context)
{
    LMV_KERNEL_PROBE_WORKER* worker = static_cast<LMV_KERNEL_PROBE_WORKER*>(Context);
    KeWaitForSingleObject(worker->StartEvent, Executive, KernelMode, FALSE, nullptr);
    if (InterlockedCompareExchange(worker->Cancel, 0, 0) == 0)
    {
        GROUP_AFFINITY affinity = {};
        GROUP_AFFINITY previous = {};
        affinity.Group = worker->Processor.Group;
        affinity.Mask = static_cast<KAFFINITY>(1) << worker->Processor.Number;
        KeSetSystemGroupAffinityThread(&affinity, &previous);
        PROCESSOR_NUMBER current = {};
        KeGetCurrentProcessorNumberEx(&current);
        if (current.Group != worker->Processor.Group ||
            current.Number != worker->Processor.Number)
        {
            worker->Failed = 1;
        }
        else
        {
            for (ULONG iteration = 0; iteration < 4; ++iteration)
            {
                LMV_STATIC_PROBE_SAMPLE handled = {};
                LMV_STATIC_PROBE_SAMPLE continued = {};
                ULONGLONG readBytes = 0;
                RtlCopyMemory(&readBytes,
                    reinterpret_cast<const VOID*>(worker->OriginalAddress), sizeof(readBytes));
                const ULONGLONG handledResult = LmvGuestKernelProbeOriginal(
                    0x1234, 1, &handled);
                const ULONGLONG continuedResult = LmvGuestKernelProbeOriginal(
                    0x1234, 0, &continued);
                const ULONGLONG nonEntryResult = LmvGuestKernelProbeNonEntry();
                if (readBytes != worker->OriginalBytes ||
                    handledResult != 0x44 || continuedResult != 0x1235 ||
                    nonEntryResult != 0x33 ||
                    handled.HandlerCalls != 1 || handled.ContinueCalls != 0 ||
                    continued.HandlerCalls != 1 || continued.ContinueCalls != 1 ||
                    handled.Token != 0x1234 || continued.Token != 0x1234 ||
                    handled.Mode != 1 || continued.Mode != 0 ||
                    ((handled.EntryFlags ^ handled.ExitFlags) & 0xCD5) != 0 ||
                    ((continued.EntryFlags ^ continued.ExitFlags) & 0xCD5) != 0)
                {
                    worker->Failed = 1;
                    break;
                }
                ++worker->Completed;
            }
        }
        KeRevertToUserGroupAffinityThread(&previous);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

extern "C" NTSTATUS LmvGuestRunKernelWorkers(
    _In_ ULONGLONG OriginalAddress, _In_ ULONGLONG OriginalBytes,
    _Out_ PULONG Executed, _Out_ PULONG Mismatches, _Out_ PULONG Iterations)
{
    *Executed = 0;
    *Mismatches = 0;
    *Iterations = 0;
    const ULONG count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (count == 0 || count > MAXULONG / sizeof(LMV_KERNEL_PROBE_WORKER) ||
        count > MAXULONG / 4)
        return STATUS_NOT_SUPPORTED;
    const SIZE_T bytes = static_cast<SIZE_T>(count) * sizeof(LMV_KERNEL_PROBE_WORKER);
    LMV_KERNEL_PROBE_WORKER* workers = static_cast<LMV_KERNEL_PROBE_WORKER*>(
        ExAllocatePoolWithTag(NonPagedPoolNx, bytes, 'wkML'));
    if (workers == nullptr)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(workers, bytes);
    KEVENT startEvent;
    KeInitializeEvent(&startEvent, NotificationEvent, FALSE);
    volatile LONG cancel = 0;
    ULONG created = 0;
    NTSTATUS status = STATUS_SUCCESS;
    for (ULONG index = 0; index < count; ++index)
    {
        LMV_KERNEL_PROBE_WORKER* worker = &workers[index];
        status = KeGetProcessorNumberFromIndex(index, &worker->Processor);
        if (!NT_SUCCESS(status) ||
            worker->Processor.Number >= sizeof(KAFFINITY) * 8)
        {
            status = STATUS_NOT_SUPPORTED;
            break;
        }
        worker->StartEvent = &startEvent;
        worker->Cancel = &cancel;
        worker->OriginalAddress = OriginalAddress;
        worker->OriginalBytes = OriginalBytes;
        OBJECT_ATTRIBUTES attributes;
        InitializeObjectAttributes(&attributes, nullptr, OBJ_KERNEL_HANDLE, nullptr, nullptr);
        status = PsCreateSystemThread(&worker->ThreadHandle, SYNCHRONIZE,
            &attributes, nullptr, nullptr, LmvGuestKernelProbeWorker, worker);
        if (!NT_SUCCESS(status))
            break;
        ++created;
    }
    if (!NT_SUCCESS(status))
        InterlockedExchange(&cancel, 1);
    KeSetEvent(&startEvent, IO_NO_INCREMENT, FALSE);
    for (ULONG index = 0; index < created; ++index)
    {
        NTSTATUS waitStatus = ZwWaitForSingleObject(workers[index].ThreadHandle,
            FALSE, nullptr);
        if (!NT_SUCCESS(waitStatus))
        {
            status = waitStatus;
            break;
        }
        ZwClose(workers[index].ThreadHandle);
        workers[index].ThreadHandle = nullptr;
        *Iterations += workers[index].Completed;
        if (workers[index].Failed != 0 || workers[index].Completed != 4)
            ++*Mismatches;
        else
            ++*Executed;
    }
    if (created != 0)
    {
        for (ULONG index = 0; index < created; ++index)
        {
            if (workers[index].ThreadHandle != nullptr)
            {
                ZwWaitForSingleObject(workers[index].ThreadHandle, FALSE, nullptr);
                ZwClose(workers[index].ThreadHandle);
            }
        }
    }
    if (NT_SUCCESS(status) &&
        (created != count || *Executed != count || *Mismatches != 0 ||
         *Iterations != count * 4 ||
         KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS) != count))
        status = STATUS_DATA_ERROR;
    ExFreePoolWithTag(workers, 'wkML');
    return status;
}