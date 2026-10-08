#include <ntifs.h>
#include <ntimage.h>
#include "../lmv_control_shared.h"

static NTSTATUS LmvReadKernelImage(_In_ ULONGLONG Address, _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ SIZE_T Length)
{
    MM_COPY_ADDRESS source = {};
    SIZE_T copied = 0;
    source.VirtualAddress = reinterpret_cast<PVOID>(Address);
    NTSTATUS status = MmCopyMemory(Buffer, source, Length, MM_COPY_MEMORY_VIRTUAL, &copied);
    return NT_SUCCESS(status) && copied == Length ? STATUS_SUCCESS : STATUS_PARTIAL_COPY;
}

extern "C" NTSTATUS LmvGuestFindKiFunctionBounds(_Inout_ PLMV_GUEST_KI_PAGE_PREFLIGHT Output)
{
    if (Output == nullptr || Output->KernelBase == 0 || Output->KernelImageSize == 0 ||
        Output->RoutineAddress < Output->KernelBase ||
        Output->RoutineAddress - Output->KernelBase >= Output->KernelImageSize)
        return STATUS_INVALID_PARAMETER;

    IMAGE_DOS_HEADER dos = {};
    NTSTATUS status = LmvReadKernelImage(Output->KernelBase, &dos, sizeof(dos));
    if (!NT_SUCCESS(status) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        static_cast<ULONGLONG>(dos.e_lfanew) > Output->KernelImageSize - sizeof(IMAGE_NT_HEADERS64))
        return STATUS_INVALID_IMAGE_FORMAT;
    IMAGE_NT_HEADERS64 nt = {};
    status = LmvReadKernelImage(Output->KernelBase + dos.e_lfanew, &nt, sizeof(nt));
    if (!NT_SUCCESS(status) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.SizeOfImage != Output->KernelImageSize)
        return STATUS_INVALID_IMAGE_FORMAT;
    const IMAGE_DATA_DIRECTORY& directory =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (directory.VirtualAddress == 0 || directory.Size == 0 ||
        directory.Size % sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) != 0 ||
        directory.Size > 2u * 1024u * 1024u ||
        directory.VirtualAddress > Output->KernelImageSize ||
        directory.Size > Output->KernelImageSize - directory.VirtualAddress)
        return STATUS_INVALID_IMAGE_FORMAT;

    PVOID buffer = ExAllocatePoolWithTag(NonPagedPoolNx, directory.Size, 'fKmL');
    if (buffer == nullptr)
        return STATUS_INSUFFICIENT_RESOURCES;
    status = LmvReadKernelImage(Output->KernelBase + directory.VirtualAddress,
        buffer, directory.Size);
    if (NT_SUCCESS(status))
    {
        Output->PageFunctionRecordCapacity = LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS;
        const auto entries = static_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(buffer);
        const ULONG count = directory.Size / sizeof(*entries);
        const ULONGLONG pageRva = Output->PageAddress - Output->KernelBase;
        const ULONGLONG routineRva = Output->RoutineAddress - Output->KernelBase;
        for (ULONG index = 0; index < count; ++index)
        {
            const auto& entry = entries[index];
            if (entry.BeginAddress >= entry.EndAddress ||
                entry.EndAddress > Output->KernelImageSize ||
                entry.UnwindInfoAddress >= Output->KernelImageSize)
                continue;
            if (static_cast<ULONGLONG>(entry.BeginAddress) < pageRva + PAGE_SIZE &&
                static_cast<ULONGLONG>(entry.EndAddress) > pageRva)
            {
                ++Output->SamePageFunctionCount;
                if (entry.BeginAddress != routineRva)
                    ++Output->SamePageOtherFunctionCount;
                if (Output->PageFunctionRecordCount <
                    LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS)
                {
                    PLMV_KI_PAGE_FUNCTION_RECORD record =
                        &Output->PageFunctionRecords[Output->PageFunctionRecordCount++];
                    record->BeginAddress = Output->KernelBase + entry.BeginAddress;
                    record->EndAddress = Output->KernelBase + entry.EndAddress;
                    record->UnwindAddress = Output->KernelBase + entry.UnwindInfoAddress;
                    record->IsTarget = entry.BeginAddress == routineRva ? 1u : 0u;
                }
                else
                    Output->PageFunctionRecordTruncated = 1;
            }
            if (routineRva >= entry.BeginAddress && routineRva < entry.EndAddress)
            {
                if (Output->FunctionRecordFound != 0)
                {
                    status = STATUS_DATA_ERROR;
                    break;
                }
                Output->FunctionRecordFound = 1;
                Output->FunctionBeginAddress = Output->KernelBase + entry.BeginAddress;
                Output->FunctionEndAddress = Output->KernelBase + entry.EndAddress;
                Output->FunctionUnwindAddress = Output->KernelBase + entry.UnwindInfoAddress;
                Output->EntryWithinFunction = entry.BeginAddress == routineRva ? 0u : 1u;
            }
        }
        if (NT_SUCCESS(status) && Output->FunctionRecordFound == 0)
            status = STATUS_NOT_FOUND;
    }
    ExFreePoolWithTag(buffer, 'fKmL');
    return status;
}