#pragma once

#include <ntddk.h>

NTSTATUS LmvGuestReadProcessMemory(
    _In_ ULONGLONG ProcessId,
    _In_ ULONGLONG Address,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _Out_ PULONG BytesTransferred,
    _Out_ PULONG PageCount,
    _Out_writes_(4) PULONGLONG ValidPageBitmap);

NTSTATUS LmvGuestWriteProcessMemory(
    _In_ ULONGLONG ProcessId,
    _In_ ULONGLONG Address,
    _In_reads_bytes_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_ PULONG BytesTransferred,
    _Out_ PULONG PageCount,
    _Out_writes_(4) PULONGLONG ValidPageBitmap);
NTSTATUS LmvGuestResolveProcessAddress(
    _In_ ULONGLONG ProcessId,
    _In_ ULONGLONG Address,
    _Out_ PULONGLONG GuestPhysicalAddress);

NTSTATUS LmvGuestResolveKiDispatchException(
    _Out_ PULONGLONG RoutineAddress,
    _Out_ PULONGLONG GuestPhysicalAddress,
    _Out_ PULONGLONG KernelBase,
    _Out_ PULONG KernelImageSize,
    _Out_writes_bytes_(MaximumBytes) PUCHAR OriginalBytes,
    _In_ ULONG MaximumBytes,
    _Out_ PULONG BytesRead,
    _Out_ PULONG Method);
