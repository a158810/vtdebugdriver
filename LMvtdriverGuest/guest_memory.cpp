#include <ntifs.h>
#include <aux_klib.h>
#include "guest_memory.hpp"
#include "guest_layout.hpp"
#include "../src/shared/lmv_control_shared.h"

// External CPUID call functions from guest_probe.asm
extern "C" VOID LmvGuestCpuidCall(ULONG Command, ULONG Argument, PVOID Result);
extern "C" VOID LmvGuestQuerySharedPage(PLMV_CPUID_SHARED_PAGE_RESULT Result);

#if 0  // Disabled: now using Host-based CR3 switch instead of physical memory mapping
static BOOLEAN LmvGuestReadPhysical(ULONGLONG PhysicalAddress, PUCHAR Buffer, SIZE_T Length)
{
    while (Length != 0)
    {
        ULONGLONG pageAddress = PhysicalAddress & ~(static_cast<ULONGLONG>(PAGE_SIZE) - 1);
        SIZE_T pageOffset = static_cast<SIZE_T>(PhysicalAddress & (PAGE_SIZE - 1));
        SIZE_T chunk = min(Length, static_cast<SIZE_T>(PAGE_SIZE) - pageOffset);
        PHYSICAL_ADDRESS physical = {};
        PVOID mapped = nullptr;
        physical.QuadPart = pageAddress;
        mapped = MmMapIoSpace(physical, PAGE_SIZE, MmCached);
        if (mapped == nullptr) return FALSE;
        RtlCopyMemory(Buffer, static_cast<PUCHAR>(mapped) + pageOffset, chunk);
        MmUnmapIoSpace(mapped, PAGE_SIZE);
        PhysicalAddress += chunk;
        Buffer += chunk;
        Length -= chunk;
    }
    return TRUE;
}

static BOOLEAN LmvGuestWritePhysical(ULONGLONG PhysicalAddress, const UCHAR* Buffer, SIZE_T Length)
{
    while (Length != 0)
    {
        ULONGLONG pageAddress = PhysicalAddress & ~(static_cast<ULONGLONG>(PAGE_SIZE) - 1);
        SIZE_T pageOffset = static_cast<SIZE_T>(PhysicalAddress & (PAGE_SIZE - 1));
        SIZE_T chunk = min(Length, static_cast<SIZE_T>(PAGE_SIZE) - pageOffset);
        PHYSICAL_ADDRESS physical = {};
        PVOID mapped = nullptr;
        physical.QuadPart = pageAddress;
        mapped = MmMapIoSpace(physical, PAGE_SIZE, MmCached);
        if (mapped == nullptr) return FALSE;
        RtlCopyMemory(static_cast<PUCHAR>(mapped) + pageOffset, Buffer, chunk);
        KeMemoryBarrier();
        MmUnmapIoSpace(mapped, PAGE_SIZE);
        PhysicalAddress += chunk;
        Buffer += chunk;
        Length -= chunk;
    }
    return TRUE;
}

static BOOLEAN LmvGuestReadPageTableEntry(ULONGLONG Table, ULONG Index, PULONGLONG Entry)
{
    return LmvGuestReadPhysical(Table + static_cast<ULONGLONG>(Index) * sizeof(ULONGLONG),
                                 reinterpret_cast<PUCHAR>(Entry), sizeof(*Entry));
}

static BOOLEAN LmvGuestTranslateAddress(ULONGLONG DirectoryTableBase,
                                         ULONGLONG VirtualAddress,
                                         PULONGLONG PhysicalAddress)
{
    ULONGLONG entry = 0;
    ULONGLONG table = DirectoryTableBase & LMV_PAGE_ADDRESS_MASK;
    ULONG pml4Index = static_cast<ULONG>((VirtualAddress >> 39) & 0x1FF);
    ULONG pdptIndex = static_cast<ULONG>((VirtualAddress >> 30) & 0x1FF);
    ULONG pdIndex = static_cast<ULONG>((VirtualAddress >> 21) & 0x1FF);
    ULONG ptIndex = static_cast<ULONG>((VirtualAddress >> 12) & 0x1FF);

    if (!LmvGuestReadPageTableEntry(table, pml4Index, &entry))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverGuest] PML4 read failed: pml4=%llx idx=%u\n", table, pml4Index);
        return FALSE;
    }
    if (!(entry & LMV_PAGE_PRESENT))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverGuest] PML4 not present: entry=%llx\n", entry);
        return FALSE;
    }
    table = entry & LMV_PAGE_ADDRESS_MASK;

    if (!LmvGuestReadPageTableEntry(table, pdptIndex, &entry))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverGuest] PDPT read failed: pdpt=%llx idx=%u\n", table, pdptIndex);
        return FALSE;
    }
    if (!(entry & LMV_PAGE_PRESENT))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverGuest] PDPT not present: entry=%llx\n", entry);
        return FALSE;
    }
    if (entry & LMV_PAGE_LARGE)
    {
        *PhysicalAddress = (entry & LMV_PAGE_1GB_ADDRESS_MASK) +
                           (VirtualAddress & ((1ull << 30) - 1));
        return TRUE;
    }
    table = entry & LMV_PAGE_ADDRESS_MASK;

    if (!LmvGuestReadPageTableEntry(table, pdIndex, &entry))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverGuest] PD read failed: pd=%llx idx=%u\n", table, pdIndex);
        return FALSE;
    }
    if (!(entry & LMV_PAGE_PRESENT))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverGuest] PD not present: entry=%llx\n", entry);
        return FALSE;
    }
    if (entry & LMV_PAGE_LARGE)
    {
        *PhysicalAddress = (entry & LMV_PAGE_2MB_ADDRESS_MASK) +
                           (VirtualAddress & ((1ull << 21) - 1));
        return TRUE;
    }
    table = entry & LMV_PAGE_ADDRESS_MASK;
    if (!LmvGuestReadPageTableEntry(table, ptIndex, &entry))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverGuest] PT read failed: pt=%llx idx=%u\n", table, ptIndex);
        return FALSE;
    }
    if (!(entry & LMV_PAGE_PRESENT))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[LMvtdriverGuest] PT not present: entry=%llx\n", entry);
        return FALSE;
    }
    *PhysicalAddress = (entry & LMV_PAGE_ADDRESS_MASK) + (VirtualAddress & (PAGE_SIZE - 1));
    return TRUE;
}
#endif  // Disabled physical memory mapping functions

#if 0  // Disabled: no longer needed with KeStackAttachProcess method
static VOID LmvGuestGetDirectoryTableBases(PEPROCESS Process,
                                            PULONGLONG UserDirectoryTableBase,
                                            PULONGLONG KernelDirectoryTableBase)
{
    PUCHAR processBytes = reinterpret_cast<PUCHAR>(Process);
    volatile ULONGLONG* kernelDirectoryTableBase = reinterpret_cast<volatile ULONGLONG*>(
        processBytes + LMV_KPROCESS_DIRECTORY_TABLE_BASE_OFFSET);
    volatile ULONGLONG* userDirectoryTableBase = reinterpret_cast<volatile ULONGLONG*>(
        processBytes + LMV_KPROCESS_USER_DIRECTORY_TABLE_BASE_OFFSET);
    *UserDirectoryTableBase = *userDirectoryTableBase & LMV_PAGE_ADDRESS_MASK;
    *KernelDirectoryTableBase = *kernelDirectoryTableBase & LMV_PAGE_ADDRESS_MASK;
}
#endif

// Shared page pointer (initialized on first use)
static PLMV_SHARED_PAGE g_SharedPage = nullptr;
static PHYSICAL_ADDRESS g_SharedPagePa = {};

static NTSTATUS LmvGuestTransfer(ULONGLONG ProcessId, ULONGLONG Address, PUCHAR Buffer,
                                 ULONG Length, BOOLEAN Write, PULONG BytesTransferred,
                                 PULONG PageCount, PULONGLONG ValidPageBitmap)
{
    PEPROCESS process = nullptr;
    NTSTATUS status = STATUS_SUCCESS;
    ULONGLONG cr3 = 0;
    ULONG offset = 0;
    ULONGLONG highestUserAddress = reinterpret_cast<ULONGLONG>(MmHighestUserAddress);

    *BytesTransferred = 0;
    *PageCount = 0;
    RtlZeroMemory(ValidPageBitmap, sizeof(ULONGLONG) * 4);

    if (Length == 0 || Length > LMV_MEMORY_IO_MAX_SIZE ||
        Address > highestUserAddress ||
        Address > highestUserAddress - (Length - 1))
        return STATUS_INVALID_PARAMETER;

    // Get target process and CR3
    status = PsLookupProcessByProcessId(reinterpret_cast<HANDLE>(ProcessId), &process);
    if (!NT_SUCCESS(status))
        return status;

    // Read CR3 from EPROCESS
    PUCHAR processBytes = reinterpret_cast<PUCHAR>(process);
    volatile ULONGLONG* kernelCr3Ptr = reinterpret_cast<volatile ULONGLONG*>(
        processBytes + LMV_KPROCESS_DIRECTORY_TABLE_BASE_OFFSET);
    cr3 = *kernelCr3Ptr & 0x000FFFFFFFFFF000ULL;

    ObDereferenceObject(process);

    if (cr3 == 0)
        return STATUS_INVALID_ADDRESS;

    // Query shared page on first use
    if (g_SharedPage == nullptr)
    {
        LMV_CPUID_SHARED_PAGE_RESULT pageResult = {};
        LmvGuestQuerySharedPage(&pageResult);

        if (pageResult.Status != LMV_CPUID_RESULT_SUCCESS || pageResult.Magic != LMV_SHARED_PAGE_MAGIC)
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "[LMvtdriverGuest] Failed to query shared page: status=%08X magic=%08X\n",
                       pageResult.Status, pageResult.Magic);
            return STATUS_UNSUCCESSFUL;
        }

        g_SharedPagePa.LowPart = pageResult.GpaLow;
        g_SharedPagePa.HighPart = pageResult.GpaHigh;
        g_SharedPage = static_cast<PLMV_SHARED_PAGE>(
            MmMapIoSpace(g_SharedPagePa, PAGE_SIZE, MmCached));

        if (g_SharedPage == nullptr)
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "[LMvtdriverGuest] Failed to map shared page\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                   "[LMvtdriverGuest] Shared page mapped: VA=%p PA=%llx\n",
                   g_SharedPage, g_SharedPagePa.QuadPart);
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "[LMvtdriverGuest] transfer pid=%llu va=%llx cr3=%llx length=%lu (via Host CR3 switch)\n",
               ProcessId, Address, cr3, Length);

    // Transfer in chunks (max 256 bytes per request)
    while (offset < Length)
    {
        ULONG chunk = min(Length - offset, sizeof(LMV_HOST_MEMORY_REQUEST::Data));
        PLMV_HOST_MEMORY_REQUEST request =
            reinterpret_cast<PLMV_HOST_MEMORY_REQUEST>(&g_SharedPage->Command.Argument0);

        // Prepare request in shared page
        request->VirtualAddress = Address + offset;
        request->Cr3 = cr3;
        request->Length = chunk;
        request->Reserved = 0;

        if (Write)
        {
            RtlCopyMemory(request->Data, Buffer + offset, chunk);
        }

        KeMemoryBarrier();

        // Call Host via CPUID
        LMV_CPUID_RESULT result = {};
        ULONG command = Write ? LmvCpuidCommandWriteVirtualMemory : LmvCpuidCommandReadVirtualMemory;
        LmvGuestCpuidCall(command, 0, &result);

        if (result.Status == LMV_CPUID_RESULT_SUCCESS)
        {
            if (!Write)
            {
                // Copy data back from Host response
                RtlCopyMemory(Buffer + offset, request->Data, chunk);
            }

            *BytesTransferred += chunk;
            ValidPageBitmap[(*PageCount) / 64] |= 1ull << ((*PageCount) % 64);
        }
        else
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "[LMvtdriverGuest] Host transfer failed: va=%llx status=%08X\n",
                       request->VirtualAddress, result.Status);
            if (NT_SUCCESS(status))
                status = STATUS_PARTIAL_COPY;
        }

        ++(*PageCount);
        offset += chunk;
    }

    return *BytesTransferred == Length ? STATUS_SUCCESS : status;
}

NTSTATUS LmvGuestReadProcessMemory(ULONGLONG ProcessId, ULONGLONG Address, PUCHAR Buffer,
                                   ULONG Length, PULONG BytesTransferred, PULONG PageCount,
                                   PULONGLONG ValidPageBitmap)
{
    return LmvGuestTransfer(ProcessId, Address, Buffer, Length, FALSE,
                             BytesTransferred, PageCount, ValidPageBitmap);
}

NTSTATUS LmvGuestWriteProcessMemory(ULONGLONG ProcessId, ULONGLONG Address, const UCHAR* Buffer,
                                    ULONG Length, PULONG BytesTransferred, PULONG PageCount,
                                    PULONGLONG ValidPageBitmap)
{
    return LmvGuestTransfer(ProcessId, Address, const_cast<PUCHAR>(Buffer), Length, TRUE,
                             BytesTransferred, PageCount, ValidPageBitmap);
}

NTSTATUS
LmvGuestResolveProcessAddress(
    ULONGLONG ProcessId,
    ULONGLONG Address,
    PULONGLONG GuestPhysicalAddress
    )
{
    PEPROCESS process = nullptr;
    KAPC_STATE apcState;
    NTSTATUS status;

    if (GuestPhysicalAddress == nullptr || Address == 0)
        return STATUS_INVALID_PARAMETER;

    *GuestPhysicalAddress = 0;

    // Kernel address: use MmGetPhysicalAddress directly
    if (Address > reinterpret_cast<ULONGLONG>(MmHighestUserAddress))
    {
        PHYSICAL_ADDRESS physical = MmGetPhysicalAddress(reinterpret_cast<PVOID>(Address));
        if (physical.QuadPart == 0)
            return STATUS_INVALID_ADDRESS;
        *GuestPhysicalAddress = physical.QuadPart;
        return STATUS_SUCCESS;
    }

    // User address: attach to process context then use MmGetPhysicalAddress
    status = PsLookupProcessByProcessId(reinterpret_cast<HANDLE>(ProcessId), &process);
    if (!NT_SUCCESS(status))
        return status;

    KeStackAttachProcess(reinterpret_cast<PKPROCESS>(process), &apcState);

    __try
    {
        ProbeForRead(reinterpret_cast<PVOID>(Address), 1, 1);
        PHYSICAL_ADDRESS physical = MmGetPhysicalAddress(reinterpret_cast<PVOID>(Address));
        if (physical.QuadPart != 0)
        {
            *GuestPhysicalAddress = physical.QuadPart;
            status = STATUS_SUCCESS;
        }
        else
        {
            status = STATUS_INVALID_ADDRESS;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = STATUS_ACCESS_VIOLATION;
    }

    KeUnstackDetachProcess(&apcState);
    ObDereferenceObject(process);

    return status;
}

static NTSTATUS
LmvGuestFindKernelImage(_Out_ PULONGLONG Base, _Out_ PULONG Size)
{
    ULONG needed = 0;
    NTSTATUS status = AuxKlibInitialize();
    if (!NT_SUCCESS(status))
        return status;

    status = AuxKlibQueryModuleInformation(
        &needed, sizeof(AUX_MODULE_EXTENDED_INFO), nullptr);
    if ((status != STATUS_BUFFER_TOO_SMALL && !NT_SUCCESS(status)) ||
        needed < sizeof(AUX_MODULE_EXTENDED_INFO))
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;

    auto* modules = static_cast<PAUX_MODULE_EXTENDED_INFO>(
        ExAllocatePoolZero(NonPagedPoolNx, needed, 'iVmL'));
    if (modules == nullptr)
        return STATUS_INSUFFICIENT_RESOURCES;

    status = AuxKlibQueryModuleInformation(
        &needed, sizeof(AUX_MODULE_EXTENDED_INFO), modules);
    if (NT_SUCCESS(status))
    {
        const ULONG count = needed / sizeof(AUX_MODULE_EXTENDED_INFO);
        for (ULONG index = 0; index < count; ++index)
        {
            const char* name = reinterpret_cast<const char*>(
                modules[index].FullPathName + modules[index].FileNameOffset);
            if (_stricmp(name, "ntoskrnl.exe") == 0 ||
                _stricmp(name, "ntkrnlmp.exe") == 0 ||
                _stricmp(name, "ntkrnlpa.exe") == 0)
            {
                *Base = reinterpret_cast<ULONGLONG>(modules[index].BasicInfo.ImageBase);
                *Size = modules[index].ImageSize;
                ExFreePoolWithTag(modules, 'iVmL');
                return STATUS_SUCCESS;
            }
        }
        status = STATUS_NOT_FOUND;
    }
    ExFreePoolWithTag(modules, 'iVmL');
    return status;
}
static const UCHAR g_KiDispatchPattern[] = {
    0x40, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x81, 0xEC, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x6C, 0x24, 0x30
};
static const UCHAR g_KiDispatchMask[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static ULONGLONG
LmvGuestScanKernelForKiDispatch(ULONGLONG Base, ULONG Size)
{
    const ULONG patternSize = RTL_NUMBER_OF(g_KiDispatchPattern);
    if (Base == 0 || Size < patternSize)
        return 0;
    const ULONG bufferSize = PAGE_SIZE + patternSize - 1;
    auto* buffer = static_cast<PUCHAR>(ExAllocatePool2(POOL_FLAG_NON_PAGED, bufferSize, 'sVmL'));
    if (buffer == nullptr)
        return 0;
    ULONGLONG matchAddress = 0;
    for (ULONG offset = 0; offset < Size; )
    {
        ULONG remaining = Size - offset;
        ULONG length = remaining < bufferSize ? remaining : bufferSize;
        MM_COPY_ADDRESS source = {};
        source.VirtualAddress = reinterpret_cast<PVOID>(Base + offset);
        SIZE_T copied = 0;
        MmCopyMemory(buffer, source, length, MM_COPY_MEMORY_VIRTUAL, &copied);
        if (copied >= patternSize)
        {
            for (ULONG candidate = 0; candidate < PAGE_SIZE && candidate <= copied - patternSize; ++candidate)
            {
                BOOLEAN match = TRUE;
                for (ULONG index = 0; index < patternSize; ++index)
                {
                    if (g_KiDispatchMask[index] != 0 &&
                        buffer[candidate + index] != g_KiDispatchPattern[index])
                    {
                        match = FALSE;
                        break;
                    }
                }
                if (match)
                {
                    if (matchAddress != 0)
                    {
                        ExFreePoolWithTag(buffer, 'sVmL');
                        return 0;
                    }
                    matchAddress = Base + offset + candidate;
                }
            }
        }
        if (remaining <= PAGE_SIZE)
            break;
        offset += PAGE_SIZE;
    }
    ExFreePoolWithTag(buffer, 'sVmL');
    return matchAddress;
}

NTSTATUS
LmvGuestResolveKiDispatchException(
    PULONGLONG RoutineAddress,
    PULONGLONG GuestPhysicalAddress,
    PULONGLONG KernelBase,
    PULONG KernelImageSize,
    PUCHAR OriginalBytes,
    ULONG MaximumBytes,
    PULONG BytesRead,
    PULONG Method)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"KiDispatchException");
    PVOID routine = MmGetSystemRoutineAddress(&name);
    ULONGLONG base = 0;
    ULONG size = 0;
    NTSTATUS status;
    SIZE_T copied = 0;
    MM_COPY_ADDRESS source = {};

    if (RoutineAddress == nullptr || GuestPhysicalAddress == nullptr ||
        KernelBase == nullptr || KernelImageSize == nullptr ||
        OriginalBytes == nullptr || BytesRead == nullptr || Method == nullptr ||
        MaximumBytes == 0)
        return STATUS_INVALID_PARAMETER;

    *RoutineAddress = 0;
    *GuestPhysicalAddress = 0;
    *KernelBase = 0;
    *KernelImageSize = 0;
    *BytesRead = 0;
    *Method = 0;
    RtlZeroMemory(OriginalBytes, MaximumBytes);

    status = LmvGuestFindKernelImage(&base, &size);
    if (!NT_SUCCESS(status))
        return status;

    if (routine != nullptr)
    {
        *RoutineAddress = reinterpret_cast<ULONGLONG>(routine);
        *Method = 1;
    }
    else
    {
        *RoutineAddress = LmvGuestScanKernelForKiDispatch(base, size);
        if (*RoutineAddress == 0)
            return STATUS_NOT_FOUND;
        *Method = 2;
    }

    *GuestPhysicalAddress = MmGetPhysicalAddress(
        reinterpret_cast<PVOID>(*RoutineAddress)).QuadPart;
    if (*GuestPhysicalAddress == 0)
        return STATUS_INVALID_ADDRESS;

    source.VirtualAddress = reinterpret_cast<PVOID>(*RoutineAddress);
    status = MmCopyMemory(OriginalBytes, source, MaximumBytes,
                          MM_COPY_MEMORY_VIRTUAL, &copied);
    if (!NT_SUCCESS(status) && copied == 0)
        return status;

    *KernelBase = base;
    *KernelImageSize = size;
    *BytesRead = static_cast<ULONG>(copied);
    return copied == MaximumBytes ? STATUS_SUCCESS : STATUS_PARTIAL_COPY;
}
