#define POOL_NX_OPTIN 1
#include <ntddk.h>
#include <wdmsec.h>
#include <intrin.h>
#include <stddef.h>
#include "../src/host/svm_core.hpp"
#include "svmtest_shared.h"

extern "C" VOID SvmTestVmrun(PVOID EntryContext);
extern "C" VOID SvmTestCaptureCpuState(PVOID State);
extern "C" UCHAR SvmTestL2CodeBegin;

extern "C" UCHAR SvmTestL2Vmmcall;
extern "C" UCHAR SvmTestL2NestedCpuidSetup;
extern "C" UCHAR SvmTestL2NestedCpuid;
extern "C" UCHAR SvmTestL2NestedCpuidContinue;
extern "C" UCHAR SvmTestL2NestedCpuidVmmcall;
extern "C" UCHAR SvmTestL2CpuidSetup;
extern "C" UCHAR SvmTestL2Cpuid;
extern "C" UCHAR SvmTestL2MsrSetup;
extern "C" UCHAR SvmTestL2Msr;
extern "C" UCHAR SvmTestL2Breakpoint;
extern "C" UCHAR SvmTestL2NestedVmrun;
extern "C" UCHAR SvmTestL2NestedVmrunInstruction;
extern "C" UCHAR SvmTestL2Halt;
extern "C" UCHAR SvmTestL2BreakpointHandler;
extern "C" UCHAR SvmTestL2GeneralProtectionHandler;
extern "C" UCHAR SvmTestL2MsrWriteSetup;
extern "C" UCHAR SvmTestL2MsrWrite;
extern "C" UCHAR SvmTestL2VmmcallCompletion;
extern "C" UCHAR SvmTestL2Exception;
extern "C" UCHAR SvmTestL2FullNestedEntry;
extern "C" UCHAR SvmTestL2FullNestedCpuid;
extern "C" UCHAR SvmTestL2FullNestedResume;
extern "C" UCHAR SvmTestL2FullNestedComplete;
extern "C" UCHAR SvmTestL2MsrTransparencyEntry;
extern "C" UCHAR SvmTestL2MsrTransparencyReadEfer;
extern "C" UCHAR SvmTestL2MsrTransparencyWriteEfer;
extern "C" UCHAR SvmTestL2MsrTransparencyReadEferBack;
extern "C" UCHAR SvmTestL2MsrTransparencyReadVmCr;
extern "C" UCHAR SvmTestL2MsrTransparencyWriteVmCr;
extern "C" UCHAR SvmTestL2MsrTransparencyReadVmCrBack;
extern "C" UCHAR SvmTestL2MsrTransparencyReadVmHsave;
extern "C" UCHAR SvmTestL2MsrTransparencyWriteVmHsave;
extern "C" UCHAR SvmTestL2MsrTransparencyReadVmHsaveBack;
extern "C" UCHAR SvmTestL2MsrTransparencyComplete;
extern "C" UCHAR SvmTestL2CodeEnd;

static constexpr ULONG kSvmFeatureLeaf = 0x8000000a;
static constexpr ULONG kSvmProcessorFeatureLeaf = 0x80000001;
static constexpr ULONG kHypervisorLeaf = 0x40000000;
static constexpr ULONG kEferMsr = 0xc0000080;
static constexpr ULONG kVmCrMsr = 0xc0010114;
static constexpr ULONG kVmHsavePaMsr = 0xc0010117;
static constexpr ULONG kFsBaseMsr = 0xc0000100;
static constexpr ULONG kGsBaseMsr = 0xc0000101;
static constexpr ULONG kKernelGsBaseMsr = 0xc0000102;
static constexpr ULONG kStarMsr = 0xc0000081;
static constexpr ULONG kLstarMsr = 0xc0000082;
static constexpr ULONG kCstarMsr = 0xc0000083;
static constexpr ULONG kSfmaskMsr = 0xc0000084;
static constexpr ULONG kPatMsr = 0x00000277;
static constexpr ULONG kSysenterCsMsr = 0x00000174;
static constexpr ULONG kSysenterEspMsr = 0x00000175;
static constexpr ULONG kSysenterEipMsr = 0x00000176;
static constexpr UINT64 kEferSvme = 1ull << 12;
static constexpr UINT64 kNestedPhysicalAddressMask = (1ull << 40) - 1;
static constexpr ULONG kMaximumIterations = 8;
static constexpr ULONG kBlockPageCount = 16;
static constexpr ULONG kGuestAsid = 1;
static constexpr ULONG kVmcbTlbControlFlushAll = 1;
static constexpr ULONG kVmexitUd = VMEXIT_EXCEPTION_UD;
static constexpr ULONG kVmexitVmmcall = VMEXIT_VMMCALL;
static constexpr ULONG kVmexitCpuid = VMEXIT_CPUID;
static constexpr ULONG kVmexitMsr = VMEXIT_MSR;
static constexpr ULONG kVmexitVmrun = VMEXIT_VMRUN;
static constexpr ULONG kInterceptUd = 1u << 6;
static constexpr ULONG kMsrPermissionMapSize = PAGE_SIZE * 2;
static constexpr ULONG kIoPermissionMapSize = PAGE_SIZE * 3;
static constexpr UINT64 kExpectedGuestMagic = 0x53564d5445535431ull;
static constexpr ULONG kExpectedMagicMarker = 0x53564d54u;

#pragma pack(push, 1)
typedef struct _SVMTEST_DESCRIPTOR_TABLE
{
    USHORT Limit;
    UINT64 Base;
} SVMTEST_DESCRIPTOR_TABLE;

typedef struct _SVMTEST_CPU_STATE
{
    USHORT Cs;
    USHORT Ds;
    USHORT Es;
    USHORT Ss;
    USHORT Fs;
    USHORT Gs;
    USHORT Tr;
    USHORT Ldtr;
    SVMTEST_DESCRIPTOR_TABLE Gdtr;
    SVMTEST_DESCRIPTOR_TABLE Idtr;
    UINT64 Cr0;
    UINT64 Cr3;
    UINT64 Cr4;
    UINT64 Rflags;
} SVMTEST_CPU_STATE;
#pragma pack(pop)

static_assert(sizeof(SVMTEST_DESCRIPTOR_TABLE) == 10, "descriptor table size");
static_assert(sizeof(SVMTEST_CPU_STATE) == 68, "cpu state size");
static_assert(offsetof(SVMTEST_CPU_STATE, Gdtr) == 16, "gdtr offset");
static_assert(offsetof(SVMTEST_CPU_STATE, Cr0) == 36, "cr0 offset");

typedef struct _SVMTEST_SEGMENT
{
    USHORT Selector;
    USHORT Attributes;
    ULONG Limit;
    UINT64 Base;
} SVMTEST_SEGMENT;

typedef struct _SVMTEST_IDT_GATE
{
    USHORT OffsetLow;
    USHORT Selector;
    UCHAR Ist;
    UCHAR Attributes;
    USHORT OffsetMiddle;
    ULONG OffsetHigh;
    ULONG Reserved;
} SVMTEST_IDT_GATE;

typedef struct _SVMTEST_L2_CONTEXT
{
    volatile ULONG Stage;
    volatile ULONG MagicMarker;
    volatile UINT64 GuestMagic;
    volatile ULONG CpuidMarker;
    volatile ULONG MsrMarker;
    volatile ULONG ExceptionMarker;
    volatile ULONG NestedVmrunMarker;
    volatile ULONG CpuidObservedEax;
    UINT64 VmcbPhysicalAddress;
    UINT64 MsrValueLow;
    UINT64 MsrValueHigh;
    volatile ULONG L1HandlerObservedMarker;
    volatile ULONG L1HandlerObservedRax;
    volatile ULONG L1HandlerObservedRbx;
    volatile ULONG L1HandlerObservedRcx;
    volatile ULONG L2ResumeMarker;
    // MSR transparency test fields (stored at fixed offsets for asm access)
    UINT64 MsrEferInitial;         // Offset 84
    UINT64 MsrEferAfterWrite;      // Offset 92
    UINT64 MsrEferReadBack;        // Offset 100
    UINT64 MsrVmCrInitial;         // Offset 108
    UINT64 MsrVmCrAfterWrite;      // Offset 116
    UINT64 MsrVmCrReadBack;        // Offset 124
    UINT64 MsrVmHsavePaInitial;    // Offset 132
    UINT64 MsrVmHsavePaAfterWrite; // Offset 140
    UINT64 MsrVmHsavePaReadBack;   // Offset 148
    ULONG Padding[1];              // Padding to align
} SVMTEST_L2_CONTEXT;

static_assert(offsetof(SVMTEST_L2_CONTEXT, VmcbPhysicalAddress) == 40, "vmcb offset");
static_assert(offsetof(SVMTEST_L2_CONTEXT, CpuidObservedEax) == 32, "cpuid result offset");
static_assert(offsetof(SVMTEST_L2_CONTEXT, L1HandlerObservedMarker) == 64, "l1 handler marker offset");
static_assert(offsetof(SVMTEST_L2_CONTEXT, MsrEferInitial) == 84, "msr efer initial offset");
static_assert(sizeof(SVMTEST_L2_CONTEXT) == 160, "l2 context size");

typedef struct _SVMTEST_GUEST_GPRS
{
    UINT64 Rbx;
    UINT64 Rcx;
    UINT64 Rdx;
    UINT64 Rbp;
    UINT64 Rsi;
    UINT64 Rdi;
    UINT64 R8;
    UINT64 R9;
    UINT64 R10;
    UINT64 R11;
    UINT64 R12;
    UINT64 R13;
    UINT64 R14;
    UINT64 R15;
} SVMTEST_GUEST_GPRS;

typedef struct _SVMTEST_VMEXIT_CONTEXT
{
    UINT64 GuestVmcbPa;
    UINT64 HostVmcbPa;
    UINT64 HostStackTop;
    UINT64 OriginalRsp;
    UINT64 OriginalRflags;
    NTSTATUS DispatchStatus;
    ULONG Reserved;
    PVOID RunContext;
    SVMTEST_GUEST_GPRS GuestGprs;
} SVMTEST_VMEXIT_CONTEXT;

static_assert(sizeof(SVMTEST_GUEST_GPRS) == 112, "guest GPR context size");
static_assert(sizeof(SVMTEST_VMEXIT_CONTEXT) == 168, "VMEXIT context size");
static_assert(offsetof(SVMTEST_VMEXIT_CONTEXT, GuestVmcbPa) == 0, "entry VMCB offset");
static_assert(offsetof(SVMTEST_VMEXIT_CONTEXT, HostVmcbPa) == 8, "entry host VMCB offset");
static_assert(offsetof(SVMTEST_VMEXIT_CONTEXT, HostStackTop) == 16, "entry stack offset");
static_assert(offsetof(SVMTEST_VMEXIT_CONTEXT, OriginalRsp) == 24, "entry RSP offset");
static_assert(offsetof(SVMTEST_VMEXIT_CONTEXT, OriginalRflags) == 32, "entry RFLAGS offset");
static_assert(offsetof(SVMTEST_VMEXIT_CONTEXT, DispatchStatus) == 40, "entry status offset");
static_assert(offsetof(SVMTEST_VMEXIT_CONTEXT, RunContext) == 48, "entry owner offset");
static_assert(offsetof(SVMTEST_VMEXIT_CONTEXT, GuestGprs) == 56, "entry GPR offset");

typedef struct _SVMTEST_RUN_CONTEXT
{
    PVOID Block;
    PVMCB Vmcb;
    PVMCB HostVmcb;
    PVOID Hsave;
    PVOID Msrpm;
    PVOID Iopm;
    PVOID Stack;
    PVOID HostStack;
    PVOID Idt;
    SVMTEST_L2_CONTEXT* L2;
    SVMTEST_VMEXIT_CONTEXT Entry;
    UINT64 VmcbPa;
    UINT64 HostVmcbPa;
    UINT64 HsavePa;
    UINT64 MsrpmPa;
    UINT64 IopmPa;
    UINT64 SavedEfer;
    UINT64 SavedVmHsavePa;
    PSVMTEST_RESULT ActiveResult;
    UINT64 ExpectedExitRip;
    UINT64 ExpectedInfo1;
    ULONG ExpectedExitCode;
    ULONG ExpectedInstructionLength;
    ULONG ActiveRecordIndex;
    GROUP_AFFINITY PreviousAffinity;
    PROCESSOR_NUMBER Processor;
    BOOLEAN AffinityPinned;
    BOOLEAN EferChanged;
    BOOLEAN HsaveChanged;
} SVMTEST_RUN_CONTEXT, *PSVMTEST_RUN_CONTEXT;

static const GUID g_SvmTestDeviceClassGuid =
{ 0x19d88c63, 0x32e5, 0x4a7d, { 0x9a, 0x25, 0x3a, 0x20, 0x91, 0x4d, 0x6b, 0x01 } };
static PDEVICE_OBJECT g_DeviceObject;
static UNICODE_STRING g_SymbolicLink;
static FAST_MUTEX g_TestMutex;
static SVMTEST_CAPABILITIES g_Capabilities;
static SVMTEST_RESULT g_Result;

static VOID SvmTestInitializeResult(_Out_ PSVMTEST_RESULT Result)
{
    RtlZeroMemory(Result, sizeof(*Result));
    Result->Version = SVMTEST_PROTOCOL_VERSION;
    Result->Size = sizeof(*Result);
    Result->Status = STATUS_DEVICE_NOT_READY;
    Result->Stage = SVMTEST_STAGE_NONE;
}

static VOID SvmTestSetStage(_Inout_ PSVMTEST_RESULT Result, _In_ ULONG Stage)
{
    Result->Stage = Stage;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "svmtest: stage=%lu\n", Stage);
}

static VOID SvmTestCompleteIrp(_In_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static BOOLEAN SvmTestIsLowPhysicalAddress(UINT64 Address, SIZE_T Length)
{
    if (Address == 0 || (Address & (PAGE_SIZE - 1)) != 0 || Length == 0)
        return FALSE;
    return Address <= kNestedPhysicalAddressMask &&
        Address <= kNestedPhysicalAddressMask - (Length - 1);
}

static PVOID SvmTestAllocateLowMemory(SIZE_T Size)
{
    PHYSICAL_ADDRESS lowest = {};
    PHYSICAL_ADDRESS highest = {};
    PHYSICAL_ADDRESS boundary = {};
    highest.QuadPart = kNestedPhysicalAddressMask;
    PVOID address = MmAllocateContiguousMemorySpecifyCache(
        Size, lowest, highest, boundary, MmCached);
    if (address != nullptr)
        RtlZeroMemory(address, Size);
    return address;
}

static BOOLEAN SvmTestGetPhysicalAddress(PVOID Address, SIZE_T Length, _Out_ UINT64* PhysicalAddress)
{
    PHYSICAL_ADDRESS physical = MmGetPhysicalAddress(Address);
    *PhysicalAddress = static_cast<UINT64>(physical.QuadPart);
    return SvmTestIsLowPhysicalAddress(*PhysicalAddress, Length);
}

static VOID SvmTestQueryHypervisorVendor(_Out_writes_(16) PCHAR Vendor)
{
    int registers[4] = {};
    __cpuid(registers, 1);
    if ((registers[2] & (1u << 31)) == 0)
        return;
    __cpuid(registers, kHypervisorLeaf);
    RtlCopyMemory(Vendor + 0, &registers[1], sizeof(registers[1]));
    RtlCopyMemory(Vendor + 4, &registers[2], sizeof(registers[2]));
    RtlCopyMemory(Vendor + 8, &registers[3], sizeof(registers[3]));
}

static VOID SvmTestQueryCapabilities(_Out_ PSVMTEST_CAPABILITIES Capabilities)
{
    int registers[4] = {};
    CHAR vendor[13] = {};
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Version = SVMTEST_PROTOCOL_VERSION;
    Capabilities->Size = sizeof(*Capabilities);

    __cpuid(registers, 0);
    RtlCopyMemory(vendor + 0, &registers[1], sizeof(registers[1]));
    RtlCopyMemory(vendor + 4, &registers[3], sizeof(registers[3]));
    RtlCopyMemory(vendor + 8, &registers[2], sizeof(registers[2]));
    RtlCopyMemory(Capabilities->CpuVendor, vendor, sizeof(vendor));

    __cpuid(registers, 0x80000000);
    if (static_cast<ULONG>(registers[0]) < kSvmFeatureLeaf)
    {
        Capabilities->Status = STATUS_NOT_SUPPORTED;
        return;
    }
    __cpuid(registers, kSvmProcessorFeatureLeaf);
    if ((registers[2] & (1u << 2)) == 0)
    {
        Capabilities->Status = STATUS_NOT_SUPPORTED;
        return;
    }

    Capabilities->Flags |= SVMTEST_FLAG_SVM_SUPPORTED;
    __cpuid(registers, kSvmFeatureLeaf);
    Capabilities->SvmRevision = static_cast<ULONG>(registers[0]);
    Capabilities->MaximumAsid = static_cast<ULONG>(registers[1]);
    Capabilities->SvmFeaturesEdx = static_cast<ULONG>(registers[3]);
    Capabilities->Efer = __readmsr(kEferMsr);
    Capabilities->VmCr = __readmsr(kVmCrMsr);
    Capabilities->VmHsavePa = __readmsr(kVmHsavePaMsr);
    if ((Capabilities->Efer & kEferSvme) != 0)
        Capabilities->Flags |= SVMTEST_FLAG_SVME_ENABLED;

    SvmTestQueryHypervisorVendor(Capabilities->HypervisorVendor);
    if ((Capabilities->SvmFeaturesEdx & (1u << 3)) != 0)
        Capabilities->Flags |= SVMTEST_FLAG_NRIP_SAVE;
    if ((Capabilities->VmCr & SVM_VM_CR_SVMDIS) == 0)
        Capabilities->Flags |= SVMTEST_FLAG_SVMDIS_CLEAR;

    ULONG readyFlags = SVMTEST_FLAG_SVM_SUPPORTED | SVMTEST_FLAG_SVMDIS_CLEAR |
        SVMTEST_FLAG_NRIP_SAVE;
    if ((Capabilities->Flags & readyFlags) == readyFlags &&
        Capabilities->MaximumAsid >= kGuestAsid)
    {
        Capabilities->Flags |= SVMTEST_FLAG_SVM_READY;
        Capabilities->Status = STATUS_SUCCESS;
    }
    else
    {
        Capabilities->Status = STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS SvmTestReadSegment(
    _In_ const SVMTEST_CPU_STATE* Cpu,
    _In_ USHORT Selector,
    _In_ BOOLEAN SystemSegment,
    _Out_ SVMTEST_SEGMENT* Segment)
{
    RtlZeroMemory(Segment, sizeof(*Segment));
    Segment->Selector = Selector;
    if (Selector == 0)
        return STATUS_SUCCESS;
    if ((Selector & 4) != 0)
        return STATUS_NOT_SUPPORTED;

    ULONG descriptorOffset = Selector & ~7u;
    if (descriptorOffset + sizeof(UINT64) > static_cast<ULONG>(Cpu->Gdtr.Limit) + 1)
        return STATUS_INVALID_DEVICE_STATE;

    UINT64 descriptorAddress = Cpu->Gdtr.Base + descriptorOffset;
    UINT64 descriptor = *reinterpret_cast<volatile const UINT64*>(descriptorAddress);
    Segment->Attributes = static_cast<USHORT>(
        ((descriptor >> 40) & 0xff) | (((descriptor >> 52) & 0x0f) << 8));
    Segment->Base = ((descriptor >> 16) & 0xffff) |
        (((descriptor >> 32) & 0xff) << 16) | (((descriptor >> 56) & 0xff) << 24);
    Segment->Limit = static_cast<ULONG>((descriptor & 0xffff) | ((descriptor >> 32) & 0x000f0000));
    if ((descriptor & (1ull << 55)) != 0)
        Segment->Limit = (Segment->Limit << 12) | 0xfff;

    if (SystemSegment)
    {
        if (descriptorOffset + 2 * sizeof(UINT64) > static_cast<ULONG>(Cpu->Gdtr.Limit) + 1)
            return STATUS_INVALID_DEVICE_STATE;
        UINT64 descriptorHigh = *reinterpret_cast<volatile const UINT64*>(descriptorAddress + sizeof(UINT64));
        Segment->Base |= (descriptorHigh & 0xffffffffull) << 32;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS SvmTestSetSegment(
    _In_ const SVMTEST_CPU_STATE* Cpu,
    _In_ USHORT Selector,
    _In_ BOOLEAN SystemSegment,
    _Out_ USHORT* VmcbSelector,
    _Out_ USHORT* VmcbAttributes,
    _Out_ UINT32* VmcbLimit,
    _Out_ UINT64* VmcbBase)
{
    SVMTEST_SEGMENT segment = {};
    NTSTATUS status = SvmTestReadSegment(Cpu, Selector, SystemSegment, &segment);
    if (!NT_SUCCESS(status))
        return status;
    *VmcbSelector = segment.Selector;
    *VmcbAttributes = segment.Attributes;
    *VmcbLimit = segment.Limit;
    *VmcbBase = segment.Base;
    return STATUS_SUCCESS;
}

static VOID SvmTestSetMsrPermission(_Inout_ PUCHAR Msrpm, _In_ ULONG Msr, _In_ BOOLEAN Write)
{
    ULONG rangeBase;
    ULONG byteOffset;
    if (Msr <= 0x1fff)
    {
        rangeBase = 0;
        byteOffset = 0;
    }
    else if (Msr >= 0xc0000000 && Msr <= 0xc0001fff)
    {
        rangeBase = 0xc0000000;
        byteOffset = 0x800;
    }
    else if (Msr >= 0xc0010000 && Msr <= 0xc0011fff)
    {
        rangeBase = 0xc0010000;
        byteOffset = 0x1000;
    }
    else
    {
        return;
    }

    ULONG bitIndex = byteOffset * 8 + (Msr - rangeBase) * 2 + (Write ? 1u : 0u);
    Msrpm[bitIndex / 8] |= static_cast<UCHAR>(1u << (bitIndex % 8));
}
static NTSTATUS SvmTestBuildVmcb(_Inout_ PSVMTEST_RUN_CONTEXT Context)
{
    SVMTEST_CPU_STATE cpu = {};
    SvmTestCaptureCpuState(&cpu);

    PVMCB vmcb = Context->Vmcb;
    RtlZeroMemory(vmcb, sizeof(*vmcb));
    RtlZeroMemory(Context->Idt, PAGE_SIZE);
    SIZE_T idtBytes = min(static_cast<SIZE_T>(cpu.Idtr.Limit) + 1, static_cast<SIZE_T>(PAGE_SIZE));
    if (idtBytes != 0 && cpu.Idtr.Base != 0)
        RtlCopyMemory(Context->Idt, reinterpret_cast<PVOID>(cpu.Idtr.Base), idtBytes);
    RtlZeroMemory(Context->Msrpm, kMsrPermissionMapSize);
    RtlZeroMemory(Context->Iopm, kIoPermissionMapSize);
    SvmTestSetMsrPermission(static_cast<PUCHAR>(Context->Msrpm), kEferMsr, FALSE);
    SvmTestSetMsrPermission(static_cast<PUCHAR>(Context->Msrpm), kEferMsr, TRUE);

    NTSTATUS status = SvmTestSetSegment(&cpu, cpu.Cs, FALSE,
        &vmcb->StateSaveArea.CsSelector, &vmcb->StateSaveArea.CsAttrib,
        &vmcb->StateSaveArea.CsLimit, &vmcb->StateSaveArea.CsBase);
    if (!NT_SUCCESS(status)) return status;
    status = SvmTestSetSegment(&cpu, cpu.Ds, FALSE,
        &vmcb->StateSaveArea.DsSelector, &vmcb->StateSaveArea.DsAttrib,
        &vmcb->StateSaveArea.DsLimit, &vmcb->StateSaveArea.DsBase);
    if (!NT_SUCCESS(status)) return status;
    status = SvmTestSetSegment(&cpu, cpu.Es, FALSE,
        &vmcb->StateSaveArea.EsSelector, &vmcb->StateSaveArea.EsAttrib,
        &vmcb->StateSaveArea.EsLimit, &vmcb->StateSaveArea.EsBase);
    if (!NT_SUCCESS(status)) return status;
    status = SvmTestSetSegment(&cpu, cpu.Ss, FALSE,
        &vmcb->StateSaveArea.SsSelector, &vmcb->StateSaveArea.SsAttrib,
        &vmcb->StateSaveArea.SsLimit, &vmcb->StateSaveArea.SsBase);
    if (!NT_SUCCESS(status)) return status;
    status = SvmTestSetSegment(&cpu, cpu.Fs, FALSE,
        &vmcb->StateSaveArea.FsSelector, &vmcb->StateSaveArea.FsAttrib,
        &vmcb->StateSaveArea.FsLimit, &vmcb->StateSaveArea.FsBase);
    if (!NT_SUCCESS(status)) return status;
    status = SvmTestSetSegment(&cpu, cpu.Gs, FALSE,
        &vmcb->StateSaveArea.GsSelector, &vmcb->StateSaveArea.GsAttrib,
        &vmcb->StateSaveArea.GsLimit, &vmcb->StateSaveArea.GsBase);
    if (!NT_SUCCESS(status)) return status;
    status = SvmTestSetSegment(&cpu, cpu.Ldtr, TRUE,
        &vmcb->StateSaveArea.LdtrSelector, &vmcb->StateSaveArea.LdtrAttrib,
        &vmcb->StateSaveArea.LdtrLimit, &vmcb->StateSaveArea.LdtrBase);
    if (!NT_SUCCESS(status)) return status;
    status = SvmTestSetSegment(&cpu, cpu.Tr, TRUE,
        &vmcb->StateSaveArea.TrSelector, &vmcb->StateSaveArea.TrAttrib,
        &vmcb->StateSaveArea.TrLimit, &vmcb->StateSaveArea.TrBase);
    if (!NT_SUCCESS(status)) return status;

    vmcb->StateSaveArea.GdtrBase = cpu.Gdtr.Base;
    vmcb->StateSaveArea.GdtrLimit = cpu.Gdtr.Limit;
    vmcb->StateSaveArea.IdtrBase = reinterpret_cast<UINT64>(Context->Idt);
    vmcb->StateSaveArea.IdtrLimit = PAGE_SIZE - 1;
    vmcb->StateSaveArea.FsBase = __readmsr(kFsBaseMsr);
    vmcb->StateSaveArea.GsBase = __readmsr(kGsBaseMsr);
    vmcb->StateSaveArea.KernelGsBase = __readmsr(kKernelGsBaseMsr);
    vmcb->StateSaveArea.Star = __readmsr(kStarMsr);
    vmcb->StateSaveArea.LStar = __readmsr(kLstarMsr);
    vmcb->StateSaveArea.CStar = __readmsr(kCstarMsr);
    vmcb->StateSaveArea.SfMask = __readmsr(kSfmaskMsr);
    vmcb->StateSaveArea.SysenterCs = __readmsr(kSysenterCsMsr);
    vmcb->StateSaveArea.SysenterEsp = __readmsr(kSysenterEspMsr);
    vmcb->StateSaveArea.SysenterEip = __readmsr(kSysenterEipMsr);
    vmcb->StateSaveArea.Cr0 = cpu.Cr0;
    vmcb->StateSaveArea.Cr3 = cpu.Cr3;
    vmcb->StateSaveArea.Cr4 = cpu.Cr4;
    vmcb->StateSaveArea.Cr2 = __readcr2();
    vmcb->StateSaveArea.Dr6 = __readdr(6);
    vmcb->StateSaveArea.Dr7 = __readdr(7) & ~0xF00000C0ull;
    vmcb->StateSaveArea.GPat = __readmsr(kPatMsr);
    vmcb->StateSaveArea.Rflags = (cpu.Rflags | 2ull) & ~(0x200ull | 0x100ull | 0x10000ull);
    vmcb->StateSaveArea.Rip = reinterpret_cast<UINT64>(&SvmTestL2CodeBegin);
    vmcb->StateSaveArea.Rsp = (reinterpret_cast<UINT64>(Context->Stack) + PAGE_SIZE - 0x100) & ~0xfull;
    vmcb->StateSaveArea.Rax = 0;
    vmcb->StateSaveArea.Efer = Context->SavedEfer | kEferSvme;
    vmcb->StateSaveArea.GPat = __readmsr(kPatMsr);
    vmcb->StateSaveArea.Cpl = 0;

    vmcb->ControlArea.InterceptException = 0;
    vmcb->ControlArea.InterceptMisc1 = SVM_INTERCEPT_MISC1_CPUID | SVM_INTERCEPT_MISC1_MSR_PROT | SVM_INTERCEPT_MISC1_HLT;
    vmcb->ControlArea.InterceptMisc2 = SVM_INTERCEPT_MISC2_VMRUN | SVM_INTERCEPT_MISC2_VMMCALL;
    vmcb->ControlArea.IopmBasePa = Context->IopmPa;
    vmcb->ControlArea.MsrpmBasePa = Context->MsrpmPa;
    vmcb->ControlArea.GuestAsid = kGuestAsid;
    vmcb->ControlArea.TlbControl = 0;
    vmcb->ControlArea.NpEnable = 0;
    vmcb->ControlArea.EventInj = 0;
    vmcb->ControlArea.VmcbClean = 0;

    PVMCB hostVmcb = Context->HostVmcb;
    RtlZeroMemory(hostVmcb, sizeof(*hostVmcb));
    RtlCopyMemory(&hostVmcb->StateSaveArea,
                  &vmcb->StateSaveArea,
                  sizeof(hostVmcb->StateSaveArea));
    hostVmcb->StateSaveArea.IdtrBase = cpu.Idtr.Base;
    hostVmcb->StateSaveArea.IdtrLimit = cpu.Idtr.Limit;
    hostVmcb->StateSaveArea.Rflags = cpu.Rflags;
    hostVmcb->StateSaveArea.Rip = 0;
    hostVmcb->StateSaveArea.Rsp = 0;
    hostVmcb->StateSaveArea.Rax = 0;
    hostVmcb->StateSaveArea.Efer = Context->SavedEfer;
    hostVmcb->StateSaveArea.Cpl = 0;

    RtlZeroMemory(&Context->Entry, sizeof(Context->Entry));
    Context->Entry.GuestVmcbPa = Context->VmcbPa;
    Context->Entry.HostVmcbPa = Context->HostVmcbPa;
    Context->Entry.HostStackTop =
        (reinterpret_cast<UINT64>(Context->HostStack) + PAGE_SIZE - 0x100) & ~0xfull;
    Context->Entry.RunContext = Context;
    Context->Entry.GuestGprs.Rdi = reinterpret_cast<UINT64>(Context->L2);
    Context->ActiveResult = nullptr;
    Context->L2->VmcbPhysicalAddress = Context->VmcbPa;
    Context->L2->Stage = 0;
    Context->L2->MagicMarker = 0;
    Context->L2->GuestMagic = 0;
    Context->L2->CpuidMarker = 0;
    Context->L2->MsrMarker = 0;
    Context->L2->ExceptionMarker = 0;
    Context->L2->NestedVmrunMarker = 0;
    Context->L2->CpuidObservedEax = 0;
    return STATUS_SUCCESS;
}
static UINT64 SvmTestCodeAddress(
    _In_ const SVMTEST_RUN_CONTEXT* Context,
    _In_ const UCHAR* Label)
{
    UNREFERENCED_PARAMETER(Context);
    return reinterpret_cast<UINT64>(Label);
}

static VOID SvmTestSetIdtGate(
    _Inout_ SVMTEST_IDT_GATE* Gate,
    _In_ USHORT Selector,
    _In_ UINT64 Target)
{
    RtlZeroMemory(Gate, sizeof(*Gate));
    Gate->Selector = Selector;
    Gate->Attributes = 0x8e;
    Gate->OffsetLow = static_cast<USHORT>(Target & 0xffff);
    Gate->OffsetMiddle = static_cast<USHORT>((Target >> 16) & 0xffff);
    Gate->OffsetHigh = static_cast<ULONG>(Target >> 32);
}

static NTSTATUS SvmTestBuildIdt(_Inout_ PSVMTEST_RUN_CONTEXT Context)
{
    if (Context->Vmcb->StateSaveArea.CsSelector == 0)
        return STATUS_INVALID_DEVICE_STATE;

    auto idt = static_cast<SVMTEST_IDT_GATE*>(Context->Idt);
    SvmTestSetIdtGate(&idt[static_cast<UCHAR>(kVmexitUd - VMEXIT_EXCEPTION_DE)],
                      Context->Vmcb->StateSaveArea.CsSelector,
                      SvmTestCodeAddress(Context, &SvmTestL2BreakpointHandler));
    SvmTestSetIdtGate(&idt[13], Context->Vmcb->StateSaveArea.CsSelector,
                      SvmTestCodeAddress(Context, &SvmTestL2GeneralProtectionHandler));
    return STATUS_SUCCESS;
}

static NTSTATUS SvmTestAllocateContext(_Out_ PSVMTEST_RUN_CONTEXT Context)
{
    if (Context->Block != nullptr)
        return STATUS_INVALID_DEVICE_STATE;

    Context->Block = SvmTestAllocateLowMemory(kBlockPageCount * PAGE_SIZE);
    if (Context->Block == nullptr)
        return STATUS_INSUFFICIENT_RESOURCES;

    PUCHAR block = static_cast<PUCHAR>(Context->Block);
    Context->Vmcb = reinterpret_cast<PVMCB>(block + PAGE_SIZE * 0);
    Context->Hsave = block + PAGE_SIZE * 1;
    Context->HostVmcb = reinterpret_cast<PVMCB>(block + PAGE_SIZE * 2);
    Context->HostStack = block + PAGE_SIZE * 3;
    Context->Msrpm = block + PAGE_SIZE * 4;
    Context->Iopm = block + PAGE_SIZE * 6;
    Context->Stack = block + PAGE_SIZE * 9;
    Context->L2 = reinterpret_cast<SVMTEST_L2_CONTEXT*>(block + PAGE_SIZE * 11);
    Context->Idt = block + PAGE_SIZE * 12;

    UINT64 blockPa = 0;
    if (!SvmTestGetPhysicalAddress(Context->Block, kBlockPageCount * PAGE_SIZE, &blockPa) ||
        !SvmTestGetPhysicalAddress(Context->Vmcb, PAGE_SIZE, &Context->VmcbPa) ||
        !SvmTestGetPhysicalAddress(Context->HostVmcb, PAGE_SIZE, &Context->HostVmcbPa) ||
        !SvmTestGetPhysicalAddress(Context->Hsave, PAGE_SIZE, &Context->HsavePa) ||
        !SvmTestGetPhysicalAddress(Context->Msrpm, kMsrPermissionMapSize, &Context->MsrpmPa) ||
        !SvmTestGetPhysicalAddress(Context->Iopm, kIoPermissionMapSize, &Context->IopmPa))
    {
        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}

static VOID SvmTestFreeContext(_Inout_ PSVMTEST_RUN_CONTEXT Context)
{
    if (Context->Block != nullptr)
    {
        RtlZeroMemory(Context->Block, kBlockPageCount * PAGE_SIZE);
        MmFreeContiguousMemory(Context->Block);
    }
    RtlZeroMemory(Context, sizeof(*Context));
}

static NTSTATUS SvmTestPinCurrentProcessor(_Inout_ PSVMTEST_RUN_CONTEXT Context)
{
    KeGetCurrentProcessorNumberEx(&Context->Processor);
    if (Context->Processor.Number >= sizeof(KAFFINITY) * 8)
        return STATUS_NOT_SUPPORTED;

    GROUP_AFFINITY affinity = {};
    affinity.Mask = static_cast<KAFFINITY>(1ull << Context->Processor.Number);
    affinity.Group = Context->Processor.Group;
    KeSetSystemGroupAffinityThread(&affinity, &Context->PreviousAffinity);
    Context->AffinityPinned = TRUE;

    PROCESSOR_NUMBER verify = {};
    KeGetCurrentProcessorNumberEx(&verify);
    if (verify.Group != Context->Processor.Group || verify.Number != Context->Processor.Number)
        return STATUS_INVALID_DEVICE_STATE;
    return STATUS_SUCCESS;
}

static VOID SvmTestUnpinProcessor(_Inout_ PSVMTEST_RUN_CONTEXT Context)
{
    if (Context->AffinityPinned)
    {
        KeRevertToUserGroupAffinityThread(&Context->PreviousAffinity);
        Context->AffinityPinned = FALSE;
    }
}

static NTSTATUS SvmTestPrepareContext(_Inout_ PSVMTEST_RUN_CONTEXT Context, _Inout_ PSVMTEST_RESULT Result)
{
    NTSTATUS status = SvmTestPinCurrentProcessor(Context);
    if (!NT_SUCCESS(status))
        return status;

    Context->SavedEfer = __readmsr(kEferMsr);
    Context->SavedVmHsavePa = __readmsr(kVmHsavePaMsr);
    SvmTestSetStage(Result, SVMTEST_STAGE_SET_HSAVE);
    if ((Context->SavedEfer & kEferSvme) != 0)
        return STATUS_DEVICE_BUSY;
    if ((__readmsr(kVmCrMsr) & SVM_VM_CR_SVMDIS) != 0)
        return STATUS_NOT_SUPPORTED;
    SvmTestSetStage(Result, SVMTEST_STAGE_ALLOCATION);
    status = SvmTestAllocateContext(Context);
    if (!NT_SUCCESS(status))
        return status;

    SvmTestSetStage(Result, SVMTEST_STAGE_GUEST_SETUP);
    status = SvmTestBuildVmcb(Context);
    if (!NT_SUCCESS(status))
        return status;
    status = SvmTestBuildIdt(Context);
    if (!NT_SUCCESS(status))
        return status;

    UINT64 efer = Context->SavedEfer | kEferSvme;
    __writemsr(kEferMsr, efer);
    Context->EferChanged = TRUE;
    if ((__readmsr(kEferMsr) & kEferSvme) == 0)
        return STATUS_NOT_SUPPORTED;
    __writemsr(kVmHsavePaMsr, Context->HsavePa);
    Context->HsaveChanged = TRUE;
    if (__readmsr(kVmHsavePaMsr) != Context->HsavePa)
        return STATUS_NOT_SUPPORTED;
    __svm_vmsave(Context->HostVmcbPa);
    return STATUS_SUCCESS;
}

static VOID SvmTestRestoreContext(_Inout_ PSVMTEST_RUN_CONTEXT Context)
{
    if (Context->HsaveChanged)
    {
        __writemsr(kVmHsavePaMsr, Context->SavedVmHsavePa);
        Context->HsaveChanged = FALSE;
    }
    if (Context->EferChanged)
    {
        __writemsr(kEferMsr, Context->SavedEfer);
        Context->EferChanged = FALSE;
    }
    SvmTestUnpinProcessor(Context);
    SvmTestFreeContext(Context);
}

extern "C" BOOLEAN SvmTestHandleVmExit(
    _Inout_ SVMTEST_VMEXIT_CONTEXT* Entry,
    _Inout_ SVMTEST_GUEST_GPRS* GuestRegisters)
{
    UNREFERENCED_PARAMETER(GuestRegisters);

    PSVMTEST_RUN_CONTEXT Context =
        static_cast<PSVMTEST_RUN_CONTEXT>(Entry->RunContext);
    PVMCB vmcb = Context->Vmcb;
    PSVMTEST_RESULT result = Context->ActiveResult;
    NTSTATUS status = STATUS_SUCCESS;
    if (vmcb->ControlArea.ExitCode != Context->ExpectedExitCode ||
        vmcb->StateSaveArea.Rip != Context->ExpectedExitRip ||
        vmcb->ControlArea.NRip !=
            Context->ExpectedExitRip + Context->ExpectedInstructionLength ||
        (Context->ExpectedInfo1 != MAXUINT64 &&
         vmcb->ControlArea.ExitInfo1 != Context->ExpectedInfo1))
    {
        status = STATUS_DATA_ERROR;
    }

    if (status == STATUS_SUCCESS && vmcb->ControlArea.ExitCode == kVmexitCpuid)
    {
        ULONG leaf = static_cast<ULONG>(vmcb->StateSaveArea.Rax);
        if (result != nullptr)
        {
            result->CpuidExitObservedEax = leaf;
        }
        if (leaf == SVMTEST_CPUID_LEAF)
        {
            vmcb->StateSaveArea.Rax = SVMTEST_CPUID_RETURN_EAX;
        }
        else if (result != nullptr && result->Mode == SVMTEST_MODE_CPUID)
        {
            status = STATUS_DATA_ERROR;
        }
    }

    Entry->DispatchStatus = status;
    return TRUE;
}

static NTSTATUS SvmTestExecuteVmrunEx(
    _Inout_ PSVMTEST_RUN_CONTEXT Context,
    _Inout_ PSVMTEST_RESULT Result,
    _In_ ULONG RecordIndex,
    _In_ UINT64 EntryRip,
    _In_ UINT64 ExpectedExitRip,
    _In_ ULONG ExpectedExitCode,
    _In_ UINT64 ExpectedInfo1,
    _In_ ULONG InstructionLength,
    _In_ UINT64 EventInjection)
{
    PVMCB vmcb = Context->Vmcb;
    vmcb->StateSaveArea.Rip = EntryRip;
    vmcb->ControlArea.ExitCode = 0;
    vmcb->ControlArea.ExitInfo1 = 0;
    vmcb->ControlArea.ExitInfo2 = 0;
    vmcb->ControlArea.NRip = 0;
    vmcb->ControlArea.EventInj = EventInjection;
    vmcb->ControlArea.TlbControl = kVmcbTlbControlFlushAll;
    vmcb->ControlArea.VmcbClean = 0;
    Context->ActiveResult = Result;
    Context->ExpectedExitRip = ExpectedExitRip;
    Context->ExpectedInfo1 = ExpectedInfo1;
    Context->ExpectedExitCode = ExpectedExitCode;
    Context->ExpectedInstructionLength = InstructionLength;
    Context->ActiveRecordIndex = RecordIndex;
    Context->Entry.DispatchStatus = STATUS_DEVICE_NOT_READY;
    __svm_vmsave(Context->HostVmcbPa);
    SvmTestVmrun(&Context->Entry);

    if (RecordIndex < SVMTEST_EXIT_RECORD_COUNT)
    {
        PSVMTEST_EXIT_RECORD record = &Result->Exits[RecordIndex];
        record->ExitCode = vmcb->ControlArea.ExitCode;
        record->ExitInfo1 = vmcb->ControlArea.ExitInfo1;
        record->ExitInfo2 = vmcb->ControlArea.ExitInfo2;
        record->GuestRip = vmcb->StateSaveArea.Rip;
        record->ObservedNrip = vmcb->ControlArea.NRip;
        record->ExpectedNrip = ExpectedExitRip + InstructionLength;
    }

    if (!NT_SUCCESS(Context->Entry.DispatchStatus))
    {
        return Context->Entry.DispatchStatus;
    }
    if (vmcb->ControlArea.ExitCode != ExpectedExitCode ||
        vmcb->StateSaveArea.Rip != ExpectedExitRip ||
        vmcb->ControlArea.NRip != ExpectedExitRip + InstructionLength ||
        (ExpectedInfo1 != MAXUINT64 && vmcb->ControlArea.ExitInfo1 != ExpectedInfo1))
    {
        return STATUS_DATA_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS SvmTestExecuteVmrun(
    _Inout_ PSVMTEST_RUN_CONTEXT Context,
    _Inout_ PSVMTEST_RESULT Result,
    _In_ ULONG RecordIndex,
    _In_ UINT64 EntryRip,
    _In_ UINT64 ExpectedExitRip,
    _In_ ULONG ExpectedExitCode,
    _In_ UINT64 ExpectedInfo1,
    _In_ ULONG InstructionLength)
{
    return SvmTestExecuteVmrunEx(Context, Result, RecordIndex, EntryRip,
                                  ExpectedExitRip, ExpectedExitCode,
                                  ExpectedInfo1, InstructionLength, 0);
}
static NTSTATUS SvmTestRunCpuidIteration(
    _Inout_ PSVMTEST_RUN_CONTEXT Context,
    _Inout_ PSVMTEST_RESULT Result)
{
    NTSTATUS status = SvmTestBuildVmcb(Context);
    if (!NT_SUCCESS(status))
        return status;
    status = SvmTestBuildIdt(Context);
    if (!NT_SUCCESS(status))
        return status;

    Context->L2->CpuidMarker = 0;
    Context->L2->CpuidObservedEax = 0;
    Context->Vmcb->ControlArea.InterceptException = MAXULONG;
    Result->CpuidLeaf = SVMTEST_CPUID_LEAF;
    Result->CpuidExpectedEax = SVMTEST_CPUID_RETURN_EAX;

    SvmTestSetStage(Result, SVMTEST_STAGE_CPUID);
    status = SvmTestExecuteVmrun(Context, Result, 0,
        SvmTestCodeAddress(Context, &SvmTestL2NestedCpuidSetup),
        SvmTestCodeAddress(Context, &SvmTestL2NestedCpuid),
        kVmexitCpuid, MAXUINT64, 2);
    if (!NT_SUCCESS(status))
        return status;

    if (Context->L2->MagicMarker != kExpectedMagicMarker ||
        Context->L2->GuestMagic != kExpectedGuestMagic ||
        Context->L2->CpuidMarker != SVMTEST_CPUID_MARKER ||
        Result->CpuidExitObservedEax != SVMTEST_CPUID_LEAF)
    {
        return STATUS_DATA_ERROR;
    }
    ++Result->CpuidExitCount;

    Context->Vmcb->StateSaveArea.Rax = SVMTEST_CPUID_RETURN_EAX;
    Context->Vmcb->StateSaveArea.Rip = Context->Vmcb->ControlArea.NRip;
    Context->Vmcb->ControlArea.VmcbClean = 0;

    SvmTestSetStage(Result, SVMTEST_STAGE_CPUID_RESUME);
    status = SvmTestExecuteVmrun(Context, Result, 1,
        Context->Vmcb->ControlArea.NRip,
        SvmTestCodeAddress(Context, &SvmTestL2NestedCpuidVmmcall),
        kVmexitVmmcall, MAXUINT64, 3);
    if (!NT_SUCCESS(status))
        return status;

    Result->GuestMagic = Context->L2->GuestMagic;
    Result->CpuidObservedEax = Context->L2->CpuidObservedEax;
    if (Context->L2->MagicMarker != kExpectedMagicMarker ||
        Context->L2->GuestMagic != kExpectedGuestMagic ||
        Context->L2->CpuidMarker != SVMTEST_CPUID_RESULT_MARKER ||
        Result->CpuidObservedEax != SVMTEST_CPUID_RETURN_EAX)
    {
        return STATUS_DATA_ERROR;
    }

    ++Result->VmmcallExitCount;
    Result->PassedMask |= SVMTEST_PASS_CPUID | SVMTEST_PASS_VMMCALL;
    return STATUS_SUCCESS;
}

// Full nested test: L1 handler that processes L2 exit and re-VMRUNs
static NTSTATUS SvmTestRunFullNestedIteration(
    _Inout_ PSVMTEST_RUN_CONTEXT Context,
    _Inout_ PSVMTEST_RESULT Result)
{
    NTSTATUS status = SvmTestBuildVmcb(Context);
    if (!NT_SUCCESS(status))
        return status;
    status = SvmTestBuildIdt(Context);
    if (!NT_SUCCESS(status))
        return status;

    // Initialize L2 context
    Context->L2->L1HandlerObservedMarker = 0;
    Context->L2->L1HandlerObservedRax = 0;
    Context->L2->L1HandlerObservedRbx = 0;
    Context->L2->L1HandlerObservedRcx = 0;
    Context->L2->L2ResumeMarker = 0;
    Context->L2->CpuidObservedEax = 0;

    // Set up VMCB to intercept CPUID so L0 will reflect to L1
    Context->Vmcb->ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_CPUID;

    // Execute L2 which will trigger CPUID VMEXIT
    SvmTestSetStage(Result, SVMTEST_STAGE_NESTED_VMRUN);
    Context->Vmcb->StateSaveArea.Rip = SvmTestCodeAddress(Context, &SvmTestL2FullNestedEntry);
    Context->Vmcb->ControlArea.VmcbClean = 0;

    // First VMRUN: Enter L2
    ++Result->NestedEnterCount;
    SvmTestVmrun(&Context->Entry);

    // At this point, L2 executed CPUID which triggered VMEXIT to L0
    // L0 should reflect this to L1 by calling SvReflectNestedExit()
    // We are now back in L1 (this code is the "L1 handler")

    // Check if we got the expected CPUID exit
    if (Context->Vmcb->ControlArea.ExitCode != kVmexitCpuid)
    {
        return STATUS_UNSUCCESSFUL;
    }

    // Record that L1 handler is processing the exit
    ++Result->NestedReflectCount;

    // L1 handler observes L2's register state from VMCB
    Context->L2->L1HandlerObservedMarker = SVMTEST_L1_HANDLER_MARKER;
    Context->L2->L1HandlerObservedRax = static_cast<ULONG>(Context->Vmcb->StateSaveArea.Rax);
    Context->L2->L1HandlerObservedRbx = static_cast<ULONG>(Context->Entry.GuestGprs.Rbx);
    Context->L2->L1HandlerObservedRcx = static_cast<ULONG>(Context->Entry.GuestGprs.Rcx);

    // L1 handler modifies the result (simulates handling the exit)
    Context->Vmcb->StateSaveArea.Rax = SVMTEST_CPUID_RETURN_EAX;

    // L1 handler advances RIP past the CPUID instruction
    Context->Vmcb->StateSaveArea.Rip = Context->Vmcb->ControlArea.NRip;
    Context->Vmcb->ControlArea.VmcbClean = 0;

    // L1 handler re-executes VMRUN to resume L2
    // L0 should catch this second VMRUN and re-enter L2
    ++Result->NestedReturnCount;
    ++Result->NestedVmrunExitCount;
    SvmTestVmrun(&Context->Entry);

    // L2 should have resumed and completed, triggering VMMCALL
    if (Context->Vmcb->ControlArea.ExitCode != kVmexitVmmcall)
    {
        return STATUS_UNSUCCESSFUL;
    }

    // Verify L2 saw the modified RAX and set the resume marker
    Result->L2ResumeMarker = Context->L2->L2ResumeMarker;
    Result->CpuidObservedEax = Context->L2->CpuidObservedEax;
    Result->L1HandlerObservedMarker = Context->L2->L1HandlerObservedMarker;
    Result->L1HandlerObservedRax = Context->L2->L1HandlerObservedRax;
    Result->L1HandlerObservedRbx = Context->L2->L1HandlerObservedRbx;
    Result->L1HandlerObservedRcx = Context->L2->L1HandlerObservedRcx;

    if (Context->L2->L2ResumeMarker != SVMTEST_L2_RESUME_MARKER ||
        Context->L2->CpuidObservedEax != SVMTEST_CPUID_RETURN_EAX)
    {
        return STATUS_DATA_ERROR;
    }

    ++Result->VmmcallExitCount;
    Result->PassedMask |= SVMTEST_PASS_NESTED_VMRUN;
    return STATUS_SUCCESS;
}

static NTSTATUS SvmTestRunFullNested(
    _In_ ULONG RequestedIterations,
    _Out_ PSVMTEST_RESULT Result)
{
    SvmTestInitializeResult(Result);
    Result->Mode = SVMTEST_MODE_NESTED_FULL;
    Result->IterationsRequested = RequestedIterations;
    SvmTestSetStage(Result, SVMTEST_STAGE_CAPABILITIES);

    SVMTEST_CAPABILITIES capabilities = {};
    SvmTestQueryCapabilities(&capabilities);
    if (!NT_SUCCESS(capabilities.Status))
    {
        Result->Status = capabilities.Status;
        return capabilities.Status;
    }

    SVMTEST_RUN_CONTEXT context = {};
    SvmTestSetStage(Result, SVMTEST_STAGE_PIN_PROCESSOR);
    NTSTATUS status = SvmTestPrepareContext(&context, Result);
    if (!NT_SUCCESS(status))
    {
        ULONG failedStage = Result->Stage;
        Result->Status = status;
        SvmTestSetStage(Result, SVMTEST_STAGE_CLEANUP);
        SvmTestRestoreContext(&context);
        Result->Stage = failedStage;
        return status;
    }

    Result->ProcessorGroup = context.Processor.Group;
    Result->ProcessorNumber = context.Processor.Number;
    Result->VmcbPhysicalAddressLow = static_cast<ULONG>(context.VmcbPa);
    Result->VmcbPhysicalAddressHigh = static_cast<ULONG>(context.VmcbPa >> 32);
    Result->HsavePhysicalAddressLow = static_cast<ULONG>(context.HsavePa);
    Result->HsavePhysicalAddressHigh = static_cast<ULONG>(context.HsavePa >> 32);

    status = STATUS_SUCCESS;
    for (ULONG iteration = 0; iteration < RequestedIterations; ++iteration)
    {
        status = SvmTestRunFullNestedIteration(&context, Result);
        if (!NT_SUCCESS(status))
            break;
        ++Result->IterationsCompleted;
    }

    ULONG finalStage = Result->Stage;
    SvmTestSetStage(Result, SVMTEST_STAGE_CLEANUP);
    SvmTestRestoreContext(&context);
    Result->Status = status;
    if (NT_SUCCESS(status))
        SvmTestSetStage(Result, SVMTEST_STAGE_COMPLETE);
    else
        Result->Stage = finalStage;
    return status;
}

static NTSTATUS SvmTestRunCpuid(
    _In_ ULONG RequestedIterations,
    _Out_ PSVMTEST_RESULT Result)
{
    SvmTestInitializeResult(Result);
    Result->Mode = SVMTEST_MODE_CPUID;
    Result->IterationsRequested = RequestedIterations;
    SvmTestSetStage(Result, SVMTEST_STAGE_CAPABILITIES);

    SVMTEST_CAPABILITIES capabilities = {};
    SvmTestQueryCapabilities(&capabilities);
    if (!NT_SUCCESS(capabilities.Status))
    {
        Result->Status = capabilities.Status;
        return capabilities.Status;
    }
    SVMTEST_RUN_CONTEXT context = {};
    SvmTestSetStage(Result, SVMTEST_STAGE_PIN_PROCESSOR);
    NTSTATUS status = SvmTestPrepareContext(&context, Result);
    if (!NT_SUCCESS(status))
    {
        ULONG failedStage = Result->Stage;
        Result->Status = status;
        SvmTestSetStage(Result, SVMTEST_STAGE_CLEANUP);
        SvmTestRestoreContext(&context);
        Result->Stage = failedStage;
        return status;
    }

    Result->ProcessorGroup = context.Processor.Group;
    Result->ProcessorNumber = context.Processor.Number;
    Result->VmcbPhysicalAddressLow = static_cast<ULONG>(context.VmcbPa);
    Result->VmcbPhysicalAddressHigh = static_cast<ULONG>(context.VmcbPa >> 32);
    Result->HsavePhysicalAddressLow = static_cast<ULONG>(context.HsavePa);
    Result->HsavePhysicalAddressHigh = static_cast<ULONG>(context.HsavePa >> 32);

    status = STATUS_SUCCESS;
    for (ULONG iteration = 0; iteration < RequestedIterations; ++iteration)
    {
        status = SvmTestRunCpuidIteration(&context, Result);
        if (!NT_SUCCESS(status))
            break;
        ++Result->IterationsCompleted;
    }

    ULONG finalStage = Result->Stage;
    SvmTestSetStage(Result, SVMTEST_STAGE_CLEANUP);
    SvmTestRestoreContext(&context);
    Result->Status = status;
    if (NT_SUCCESS(status))
        SvmTestSetStage(Result, SVMTEST_STAGE_COMPLETE);
    else
        Result->Stage = finalStage;
    return status;
}

// MSR Transparency Test: Iteration function
static NTSTATUS SvmTestRunMsrTransparencyIteration(
    _Inout_ PSVMTEST_RUN_CONTEXT Context,
    _Inout_ PSVMTEST_RESULT Result)
{
    NTSTATUS status = SvmTestBuildVmcb(Context);
    if (!NT_SUCCESS(status))
        return status;

    // Initialize MSR test fields
    Context->L2->MsrEferInitial = 0;
    Context->L2->MsrEferAfterWrite = 0;
    Context->L2->MsrEferReadBack = 0;
    Context->L2->MsrVmCrInitial = 0;
    Context->L2->MsrVmCrAfterWrite = 0;
    Context->L2->MsrVmCrReadBack = 0;
    Context->L2->MsrVmHsavePaInitial = 0;
    Context->L2->MsrVmHsavePaAfterWrite = 0;
    Context->L2->MsrVmHsavePaReadBack = 0;
    Result->MsrEferInitial = 0;
    Result->MsrEferAfterWrite = 0;
    Result->MsrEferReadBack = 0;
    Result->MsrVmCrInitial = 0;
    Result->MsrVmCrAfterWrite = 0;
    Result->MsrVmCrReadBack = 0;
    Result->MsrVmHsavePaInitial = 0;
    Result->MsrVmHsavePaAfterWrite = 0;
    Result->MsrVmHsavePaReadBack = 0;
    Result->MsrTestPassed = 0;
    Result->MsrTransparencyDetected = 0;

    // Intercept MSR access
    Context->Vmcb->ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_MSR_PROT;

    // Execute MSR transparency test
    SvmTestSetStage(Result, SVMTEST_STAGE_MSR);
    Context->Vmcb->StateSaveArea.Rip = SvmTestCodeAddress(Context, &SvmTestL2MsrTransparencyEntry);
    Context->Vmcb->ControlArea.VmcbClean = 0;
    SvmTestVmrun(&Context->Entry);

    // After VMMCALL, collect results
    Result->MsrEferInitial = Context->L2->MsrEferInitial;
    Result->MsrEferAfterWrite = Context->L2->MsrEferAfterWrite;
    Result->MsrEferReadBack = Context->L2->MsrEferReadBack;
    Result->MsrVmCrInitial = Context->L2->MsrVmCrInitial;
    Result->MsrVmCrAfterWrite = Context->L2->MsrVmCrAfterWrite;
    Result->MsrVmCrReadBack = Context->L2->MsrVmCrReadBack;
    Result->MsrVmHsavePaInitial = Context->L2->MsrVmHsavePaInitial;
    Result->MsrVmHsavePaAfterWrite = Context->L2->MsrVmHsavePaAfterWrite;
    Result->MsrVmHsavePaReadBack = Context->L2->MsrVmHsavePaReadBack;

    // Validate MSR transparency
    BOOLEAN eferTransparent = (Result->MsrEferReadBack == Result->MsrEferAfterWrite);
    BOOLEAN vmCrTransparent = (Result->MsrVmCrReadBack == Result->MsrVmCrAfterWrite);
    BOOLEAN vmHsaveTransparent = (Result->MsrVmHsavePaReadBack == Result->MsrVmHsavePaAfterWrite);

    if (eferTransparent && vmCrTransparent && vmHsaveTransparent)
    {
        Result->MsrTestPassed = 1;
        Result->MsrTransparencyDetected = 0;  // No anomalies detected
    }
    else
    {
        Result->MsrTestPassed = 0;
        Result->MsrTransparencyDetected = 1;  // Virtualization detected
    }

    ++Result->VmmcallExitCount;
    if (Result->MsrTestPassed)
        Result->PassedMask |= SVMTEST_PASS_MSR;

    return STATUS_SUCCESS;
}

// MSR Transparency Test: Main function
static NTSTATUS SvmTestRunMsrTransparency(
    _In_ ULONG RequestedIterations,
    _Out_ PSVMTEST_RESULT Result)
{
    SvmTestInitializeResult(Result);
    Result->Mode = SVMTEST_MODE_MSR_TRANSPARENCY;
    Result->IterationsRequested = RequestedIterations;
    SvmTestSetStage(Result, SVMTEST_STAGE_CAPABILITIES);

    SVMTEST_CAPABILITIES capabilities = {};
    SvmTestQueryCapabilities(&capabilities);
    if (!NT_SUCCESS(capabilities.Status))
    {
        Result->Status = capabilities.Status;
        return capabilities.Status;
    }

    SVMTEST_RUN_CONTEXT context = {};
    SvmTestSetStage(Result, SVMTEST_STAGE_PIN_PROCESSOR);
    NTSTATUS status = SvmTestPrepareContext(&context, Result);
    if (!NT_SUCCESS(status))
    {
        ULONG failedStage = Result->Stage;
        Result->Status = status;
        SvmTestSetStage(Result, SVMTEST_STAGE_CLEANUP);
        SvmTestRestoreContext(&context);
        Result->Stage = failedStage;
        return status;
    }

    Result->ProcessorGroup = context.Processor.Group;
    Result->ProcessorNumber = context.Processor.Number;
    Result->VmcbPhysicalAddressLow = static_cast<ULONG>(context.VmcbPa);
    Result->VmcbPhysicalAddressHigh = static_cast<ULONG>(context.VmcbPa >> 32);
    Result->HsavePhysicalAddressLow = static_cast<ULONG>(context.HsavePa);
    Result->HsavePhysicalAddressHigh = static_cast<ULONG>(context.HsavePa >> 32);

    status = STATUS_SUCCESS;
    for (ULONG iteration = 0; iteration < RequestedIterations; ++iteration)
    {
        status = SvmTestRunMsrTransparencyIteration(&context, Result);
        if (!NT_SUCCESS(status))
            break;
        ++Result->IterationsCompleted;
    }

    ULONG finalStage = Result->Stage;
    SvmTestSetStage(Result, SVMTEST_STAGE_CLEANUP);
    SvmTestRestoreContext(&context);
    Result->Status = status;
    if (NT_SUCCESS(status))
        SvmTestSetStage(Result, SVMTEST_STAGE_COMPLETE);
    else
        Result->Stage = finalStage;
    return status;
}

static NTSTATUS SvmTestDispatchCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    SvmTestCompleteIrp(Irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS SvmTestDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;

    if (code == IOCTL_SVMTEST_QUERY_CAPABILITIES)
    {
        if (outputLength < sizeof(SVMTEST_CAPABILITIES))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            ExAcquireFastMutex(&g_TestMutex);
            SvmTestQueryCapabilities(&g_Capabilities);
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &g_Capabilities,
                          sizeof(g_Capabilities));
            ExReleaseFastMutex(&g_TestMutex);
            status = STATUS_SUCCESS;
            information = sizeof(g_Capabilities);
        }
    }
    else if (code == IOCTL_SVMTEST_QUERY_RESULT)
    {
        if (outputLength < sizeof(SVMTEST_RESULT))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            ExAcquireFastMutex(&g_TestMutex);
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &g_Result, sizeof(g_Result));
            ExReleaseFastMutex(&g_TestMutex);
            status = STATUS_SUCCESS;
            information = sizeof(g_Result);
        }
    }
    else if (code == IOCTL_SVMTEST_RESET)
    {
        ExAcquireFastMutex(&g_TestMutex);
        SvmTestInitializeResult(&g_Result);
        ExReleaseFastMutex(&g_TestMutex);
        status = STATUS_SUCCESS;
    }
    else if (code == IOCTL_SVMTEST_RUN_SMOKE)
    {
        status = STATUS_NOT_SUPPORTED;
    }
    else if (code == IOCTL_SVMTEST_RUN_CPUID)
    {
        if (outputLength < sizeof(SVMTEST_RESULT))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            ULONG expectedMode = SVMTEST_MODE_CPUID;
            SVMTEST_RUN_REQUEST request = {};
            request.Version = SVMTEST_PROTOCOL_VERSION;
            request.Size = sizeof(request);
            request.Iterations = 1;
            request.Mode = expectedMode;
            if (inputLength >= sizeof(request))
            {
                RtlCopyMemory(&request, Irp->AssociatedIrp.SystemBuffer, sizeof(request));
                if (request.Version != SVMTEST_PROTOCOL_VERSION ||
                    request.Size < sizeof(request))
                {
                    status = STATUS_REVISION_MISMATCH;
                    SvmTestCompleteIrp(Irp, status, 0);
                    return status;
                }
                if (request.Mode != expectedMode)
                {
                    status = STATUS_INVALID_PARAMETER;
                    SvmTestCompleteIrp(Irp, status, 0);
                    return status;
                }
                if (request.Iterations == 0)
                    request.Iterations = 1;
            }
            if (request.Iterations > kMaximumIterations)
                request.Iterations = kMaximumIterations;

            ExAcquireFastMutex(&g_TestMutex);
            SvmTestInitializeResult(&g_Result);
            status = SvmTestRunCpuid(request.Iterations, &g_Result);
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &g_Result, sizeof(g_Result));
            ExReleaseFastMutex(&g_TestMutex);
            information = sizeof(g_Result);
        }
    }
    else if (code == IOCTL_SVMTEST_RUN_FULL_NESTED)
    {
        if (outputLength < sizeof(SVMTEST_RESULT))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            ULONG expectedMode = SVMTEST_MODE_NESTED_FULL;
            SVMTEST_RUN_REQUEST request = {};
            request.Version = SVMTEST_PROTOCOL_VERSION;
            request.Size = sizeof(request);
            request.Iterations = 1;
            request.Mode = expectedMode;
            if (inputLength >= sizeof(request))
            {
                RtlCopyMemory(&request, Irp->AssociatedIrp.SystemBuffer, sizeof(request));
                if (request.Version != SVMTEST_PROTOCOL_VERSION ||
                    request.Size < sizeof(request))
                {
                    status = STATUS_REVISION_MISMATCH;
                    SvmTestCompleteIrp(Irp, status, 0);
                    return status;
                }
                if (request.Mode != expectedMode)
                {
                    status = STATUS_INVALID_PARAMETER;
                    SvmTestCompleteIrp(Irp, status, 0);
                    return status;
                }
                if (request.Iterations == 0)
                    request.Iterations = 1;
            }
            if (request.Iterations > kMaximumIterations)
                request.Iterations = kMaximumIterations;

            ExAcquireFastMutex(&g_TestMutex);
            SvmTestInitializeResult(&g_Result);
            status = SvmTestRunFullNested(request.Iterations, &g_Result);
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &g_Result, sizeof(g_Result));
            ExReleaseFastMutex(&g_TestMutex);
            information = sizeof(g_Result);
        }
    }
    else if (code == IOCTL_SVMTEST_RUN_MSR_TEST)
    {
        if (outputLength < sizeof(SVMTEST_RESULT))
            status = STATUS_BUFFER_TOO_SMALL;
        else
        {
            ULONG expectedMode = SVMTEST_MODE_MSR_TRANSPARENCY;
            SVMTEST_RUN_REQUEST request = {};
            request.Version = SVMTEST_PROTOCOL_VERSION;
            request.Size = sizeof(request);
            request.Iterations = 1;
            request.Mode = expectedMode;
            if (inputLength >= sizeof(request))
            {
                RtlCopyMemory(&request, Irp->AssociatedIrp.SystemBuffer, sizeof(request));
                if (request.Version != SVMTEST_PROTOCOL_VERSION ||
                    request.Size < sizeof(request))
                {
                    status = STATUS_REVISION_MISMATCH;
                    SvmTestCompleteIrp(Irp, status, 0);
                    return status;
                }
                if (request.Mode != expectedMode)
                {
                    status = STATUS_INVALID_PARAMETER;
                    SvmTestCompleteIrp(Irp, status, 0);
                    return status;
                }
                if (request.Iterations == 0)
                    request.Iterations = 1;
            }
            if (request.Iterations > kMaximumIterations)
                request.Iterations = kMaximumIterations;

            ExAcquireFastMutex(&g_TestMutex);
            SvmTestInitializeResult(&g_Result);
            status = SvmTestRunMsrTransparency(request.Iterations, &g_Result);
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &g_Result, sizeof(g_Result));
            ExReleaseFastMutex(&g_TestMutex);
            information = sizeof(g_Result);
        }
    }

    SvmTestCompleteIrp(Irp, status, information);
    return status;
}

static VOID SvmTestUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    ExAcquireFastMutex(&g_TestMutex);
    ExReleaseFastMutex(&g_TestMutex);
    if (g_DeviceObject != nullptr)
    {
        IoDeleteSymbolicLink(&g_SymbolicLink);
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = nullptr;
    }
}

extern "C"
NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    ExInitializeFastMutex(&g_TestMutex);
    SvmTestInitializeResult(&g_Result);
    SvmTestQueryCapabilities(&g_Capabilities);

    UNICODE_STRING deviceName = RTL_CONSTANT_STRING(L"\\Device\\svmtest");
    RtlInitUnicodeString(&g_SymbolicLink, L"\\DosDevices\\svmtest");
    NTSTATUS status = IoCreateDeviceSecure(DriverObject, 0, &deviceName,
                                           FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                           FALSE, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL,
                                           &g_SvmTestDeviceClassGuid, &g_DeviceObject);
    if (!NT_SUCCESS(status))
        return status;

    g_DeviceObject->Flags |= DO_BUFFERED_IO;
    status = IoCreateSymbolicLink(&g_SymbolicLink, &deviceName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = nullptr;
        return status;
    }

    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
        DriverObject->MajorFunction[i] = SvmTestDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SvmTestDispatchDeviceControl;
    DriverObject->DriverUnload = SvmTestUnload;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
