#include <ntifs.h>
#include <aux_klib.h>
#include "guest_memory.hpp"
#include "guest_layout.hpp"
#include "../lmv_control_shared.h"

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

    if (!LmvGuestReadPageTableEntry(table, pml4Index, &entry) || !(entry & LMV_PAGE_PRESENT))
        return FALSE;
    table = entry & LMV_PAGE_ADDRESS_MASK;
    if (!LmvGuestReadPageTableEntry(table, pdptIndex, &entry) || !(entry & LMV_PAGE_PRESENT))
        return FALSE;
    if (entry & LMV_PAGE_LARGE)
    {
        *PhysicalAddress = (entry & LMV_PAGE_1GB_ADDRESS_MASK) +
                           (VirtualAddress & ((1ull << 30) - 1));
        return TRUE;
    }
    table = entry & LMV_PAGE_ADDRESS_MASK;
    if (!LmvGuestReadPageTableEntry(table, pdIndex, &entry) || !(entry & LMV_PAGE_PRESENT))
        return FALSE;
    if (entry & LMV_PAGE_LARGE)
    {
        *PhysicalAddress = (entry & LMV_PAGE_2MB_ADDRESS_MASK) +
                           (VirtualAddress & ((1ull << 21) - 1));
        return TRUE;
    }
    table = entry & LMV_PAGE_ADDRESS_MASK;
    if (!LmvGuestReadPageTableEntry(table, ptIndex, &entry) || !(entry & LMV_PAGE_PRESENT))
        return FALSE;
    *PhysicalAddress = (entry & LMV_PAGE_ADDRESS_MASK) + (VirtualAddress & (PAGE_SIZE - 1));
    return TRUE;
}

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

static NTSTATUS LmvGuestTransfer(ULONGLONG ProcessId, ULONGLONG Address, PUCHAR Buffer,
                                 ULONG Length, BOOLEAN Write, PULONG BytesTransferred,
                                 PULONG PageCount, PULONGLONG ValidPageBitmap)
{
    PEPROCESS process = nullptr;
    ULONGLONG userCr3 = 0;
    ULONGLONG kernelCr3 = 0;
    ULONG offset = 0;
    ULONG pageIndex = 0;
    NTSTATUS status = STATUS_SUCCESS;
    ULONGLONG highestUserAddress = reinterpret_cast<ULONGLONG>(MmHighestUserAddress);

    *BytesTransferred = 0;
    *PageCount = 0;
    RtlZeroMemory(ValidPageBitmap, sizeof(ULONGLONG) * 4);
    if (Length == 0 || Length > LMV_MEMORY_IO_MAX_SIZE ||
        Address > highestUserAddress ||
        Address > highestUserAddress - (Length - 1))
        return STATUS_INVALID_PARAMETER;

    status = PsLookupProcessByProcessId(reinterpret_cast<HANDLE>(ProcessId), &process);
    if (!NT_SUCCESS(status)) return status;
    LmvGuestGetDirectoryTableBases(process, &userCr3, &kernelCr3);
    if (userCr3 == 0 && kernelCr3 == 0)
    {
        ObDereferenceObject(process);
        return STATUS_INVALID_ADDRESS;
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "[LMvtdriverGuest] transfer pid=%llu va=%llx user-cr3=%llx kernel-cr3=%llx length=%lu\n",
               ProcessId, Address, userCr3, kernelCr3, Length);

    while (offset < Length)
    {
        ULONGLONG physicalAddress = 0;
        ULONGLONG virtualAddress = Address + offset;
        ULONG pageOffset = static_cast<ULONG>(virtualAddress & (PAGE_SIZE - 1));
        ULONG chunk = min(Length - offset, PAGE_SIZE - pageOffset);
        BOOLEAN translated = FALSE;

        ++(*PageCount);
        if (userCr3 != 0)
            translated = LmvGuestTranslateAddress(userCr3, virtualAddress, &physicalAddress);
        if (!translated && kernelCr3 != 0 && kernelCr3 != userCr3)
            translated = LmvGuestTranslateAddress(kernelCr3, virtualAddress, &physicalAddress);

        if (translated)
        {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                       "[LMvtdriverGuest] translated va=%llx gpa=%llx\n",
                       virtualAddress, physicalAddress);
            BOOLEAN transferred = Write ?
                LmvGuestWritePhysical(physicalAddress, Buffer + offset, chunk) :
                LmvGuestReadPhysical(physicalAddress, Buffer + offset, chunk);
            if (transferred)
            {
                *BytesTransferred += chunk;
                ValidPageBitmap[pageIndex / 64] |= 1ull << (pageIndex % 64);
            }
            else if (NT_SUCCESS(status))
            {
                status = STATUS_PARTIAL_COPY;
            }
        }
        else if (NT_SUCCESS(status))
        {
            status = STATUS_PARTIAL_COPY;
        }
        ++pageIndex;
        offset += chunk;
    }

    ObDereferenceObject(process);
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
    ULONGLONG userCr3 = 0;
    ULONGLONG kernelCr3 = 0;
    ULONGLONG physicalAddress = 0;
    NTSTATUS status;

    if (GuestPhysicalAddress == nullptr || Address == 0)
        return STATUS_INVALID_PARAMETER;

    *GuestPhysicalAddress = 0;
    if (Address > reinterpret_cast<ULONGLONG>(MmHighestUserAddress))
    {
        PHYSICAL_ADDRESS physical = MmGetPhysicalAddress(reinterpret_cast<PVOID>(Address));
        if (physical.QuadPart == 0)
            return STATUS_INVALID_ADDRESS;
        *GuestPhysicalAddress = physical.QuadPart;
        return STATUS_SUCCESS;
    }

    status = PsLookupProcessByProcessId(reinterpret_cast<HANDLE>(ProcessId), &process);
    if (!NT_SUCCESS(status))
        return status;

    LmvGuestGetDirectoryTableBases(process, &userCr3, &kernelCr3);
    if (userCr3 != 0)
        LmvGuestTranslateAddress(userCr3, Address, &physicalAddress);
    if (physicalAddress == 0 && kernelCr3 != 0 && kernelCr3 != userCr3)
        LmvGuestTranslateAddress(kernelCr3, Address, &physicalAddress);

    ObDereferenceObject(process);
    if (physicalAddress == 0)
        return STATUS_INVALID_ADDRESS;

    *GuestPhysicalAddress = physicalAddress;
    return STATUS_SUCCESS;
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
