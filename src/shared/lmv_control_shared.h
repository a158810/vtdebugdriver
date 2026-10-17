#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <Windows.h>
#include <winioctl.h>
#endif

#define LMV_DEVICE_TYPE 0x8000

// ===== VMEXIT Exception Codes =====
#define VMEXIT_EXCEPTION_DE 0x0040
#define VMEXIT_EXCEPTION_DB 0x0041
#define VMEXIT_EXCEPTION_BP 0x0043

// ===== Debug Mode Definitions =====
// Support for transparent monitoring and interactive debugging modes
#define LMV_DEBUG_MODE_INTERACTIVE 0u    // Interactive mode: suspend thread, support Step/Run
#define LMV_DEBUG_MODE_TRANSPARENT 1u    // Transparent mode: no thread suspension, event logging only

// ===== Session State Machine =====
typedef enum _LMV_SESSION_STATE {
    LMV_SESSION_IDLE = 0,           // No active session
    LMV_SESSION_INTERACTIVE = 1,    // Interactive mode active
    LMV_SESSION_TRANSPARENT = 2,    // Transparent mode active
    LMV_SESSION_DETACHING = 3       // Detaching in progress
} LMV_SESSION_STATE;

// ===== Host Event Queue for Transparent Mode =====
// Used by Host driver to record exception events in transparent mode
#define HOST_EVENT_QUEUE_SIZE 256
#define HOST_EVENT_KIND_EXCEPTION 0u
#define HOST_EVENT_KIND_DR2_WATCH 1u

// Maximum call stack depth to capture
#define HOST_EXCEPTION_MAX_STACK_FRAMES 32

typedef struct _HOST_EXCEPTION_EVENT {
    UINT64 Rip;
    UINT64 Rsp;
    UINT64 Rflags;
    UINT64 Rax;
    UINT64 Rbx;
    UINT64 Rcx;
    UINT64 Rdx;
    UINT64 Rsi;
    UINT64 Rdi;
    UINT64 Rbp;
    UINT64 R8;
    UINT64 R9;
    UINT64 R10;
    UINT64 R11;
    UINT64 R12;
    UINT64 R13;
    UINT64 R14;
    UINT64 R15;
    UINT64 Cr3;
    UINT64 Dr6;
    UINT64 Timestamp;
    ULONG ExceptionType;
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG Reserved;
    UINT64 ExceptionAddress;
    UINT64 ExceptionInformation[15];
    ULONG NumberParameters;
    ULONG FirstChance;
    ULONG CaptureStatus;
    ULONG ContextValid;
    // Call stack capture
    UINT64 StackFrames[HOST_EXCEPTION_MAX_STACK_FRAMES];  // Return addresses
    ULONG StackFrameCount;                                 // Number of valid frames captured
    ULONG StackCaptureStatus;                              // 0=success, non-zero=error
} HOST_EXCEPTION_EVENT, *PHOST_EXCEPTION_EVENT;
static_assert(sizeof(HOST_EXCEPTION_EVENT) == 592, "HOST_EXCEPTION_EVENT layout mismatch");

#define IOCTL_LMV_QUERY_STATUS CTL_CODE(LMV_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_LMV_SET_DEBUG_MODE CTL_CODE(LMV_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_POLL_HOST_EVENTS CTL_CODE(LMV_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_LMV_GUEST_QUERY_STATUS CTL_CODE(LMV_DEVICE_TYPE, 0x810, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_QUERY_NPT_STATS CTL_CODE(LMV_DEVICE_TYPE, 0x811, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_NPT_COMMAND CTL_CODE(LMV_DEVICE_TYPE, 0x812, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_NPT_SMOKE_TEST CTL_CODE(LMV_DEVICE_TYPE, 0x813, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_READ_PROCESS_MEMORY CTL_CODE(LMV_DEVICE_TYPE, 0x814, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_WRITE_PROCESS_MEMORY CTL_CODE(LMV_DEVICE_TYPE, 0x815, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_DEBUG_HOOK_SMOKE CTL_CODE(LMV_DEVICE_TYPE, 0x816, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_RESOLVE_KI_DISPATCH CTL_CODE(LMV_DEVICE_TYPE, 0x817, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_STUB_TRIGGER CTL_CODE(LMV_DEVICE_TYPE, 0x818, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_STUB_GET_EVENT CTL_CODE(LMV_DEVICE_TYPE, 0x819, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_STUB_CONTINUE CTL_CODE(LMV_DEVICE_TYPE, 0x81A, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_STUB_WAIT_EVENT CTL_CODE(LMV_DEVICE_TYPE, 0x81B, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_DEBUG_HOOK_READ_SMOKE CTL_CODE(LMV_DEVICE_TYPE, 0x81C, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_EXEC_PROBE CTL_CODE(LMV_DEVICE_TYPE, 0x81D, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_QUERY_KI_STUB_SITES CTL_CODE(LMV_DEVICE_TYPE, 0x81E, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_STATIC_STUB_SMOKE CTL_CODE(LMV_DEVICE_TYPE, 0x81F, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KERNEL_EXEC_SMOKE CTL_CODE(LMV_DEVICE_TYPE, 0x820, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KERNEL_EXEC_STRESS CTL_CODE(LMV_DEVICE_TYPE, 0x821, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_PAGE_PREFLIGHT CTL_CODE(LMV_DEVICE_TYPE, 0x822, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_NEXT_PAGE CTL_CODE(LMV_DEVICE_TYPE, 0x823, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_WINDOW CTL_CODE(LMV_DEVICE_TYPE, 0x824, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_MIRROR_STORAGE CTL_CODE(LMV_DEVICE_TYPE, 0x825, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_EXEC_MIRROR_SMOKE CTL_CODE(LMV_DEVICE_TYPE, 0x826, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_DR3_SMOKE CTL_CODE(LMV_DEVICE_TYPE, 0x827, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_DR_COEXIST CTL_CODE(LMV_DEVICE_TYPE, 0x828, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_DR3_PROBE CTL_CODE(LMV_DEVICE_TYPE, 0x829, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_DR3_WAIT_EVENT CTL_CODE(LMV_DEVICE_TYPE, 0x82A, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_DR3_CONTINUE CTL_CODE(LMV_DEVICE_TYPE, 0x82B, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_QUEUE_CONTROL CTL_CODE(LMV_DEVICE_TYPE, 0x82C, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_QUEUE_PRODUCE CTL_CODE(LMV_DEVICE_TYPE, 0x82D, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_DR3_SET_RAX CTL_CODE(LMV_DEVICE_TYPE, 0x82E, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_DR3_SET_RIP CTL_CODE(LMV_DEVICE_TYPE, 0x82F, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_DR3_GET_CONTEXT CTL_CODE(LMV_DEVICE_TYPE, 0x830, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_DR3_SET_CONTEXT CTL_CODE(LMV_DEVICE_TYPE, 0x831, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_KI_SESSION CTL_CODE(LMV_DEVICE_TYPE, 0x832, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_DR2_WATCH CTL_CODE(LMV_DEVICE_TYPE, 0x833, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define LMV_CONTROL_VERSION 1
#define LMV_STATUS_FLAG_SVM_RUNNING 0x00000001u

#define LMV_CPUID_CALL_LEAF 0x4C4D0001u
#define LMV_CPUID_CALL_MAGIC 0x4C4D5654434D414Cull
#define LMV_CPUID_PROTOCOL_VERSION 2u

typedef enum _LMV_CPUID_COMMAND
{
    LmvCpuidCommandQueryStatus = 1,
    LmvCpuidCommandNotifySharedCommand = 2,
    LmvCpuidCommandQuerySharedPage = 3,
    LmvCpuidCommandQueryNptStats = 4,
    LmvCpuidCommandFlushNptTlb = 5,
    LmvCpuidCommandSyncKiSession = 6,
    LmvCpuidCommandReleaseKiGuest = 7,
    LmvCpuidCommandReadVirtualMemory = 8,
    LmvCpuidCommandWriteVirtualMemory = 9,
    LmvCpuidCommandPollHostEvents = 10,
    LmvCpuidCommandReadHostEvent = 11,
    LmvCpuidCommandSubmitHostEvent = 12,
    LmvCpuidCommandSyncDr2Watch = 13
} LMV_CPUID_COMMAND;

#define LMV_CPUID_RESULT_SUCCESS 0u
#define LMV_CPUID_RESULT_INVALID_ARGUMENT 0xC000000Du
#define LMV_CPUID_RESULT_UNSUPPORTED 0xC00000BBu
#define LMV_MEMORY_IO_MAX_SIZE (64u * 1024u)
#define LMV_DBG_FILE_DEVICE 0x8000
#define LMV_DBG_IOCTL_PING CTL_CODE(LMV_DBG_FILE_DEVICE, 0x80E, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define LMV_DBG_IOCTL_ATTACH_PROCESS CTL_CODE(LMV_DBG_FILE_DEVICE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define LMV_DBG_IOCTL_DETACH_PROCESS CTL_CODE(LMV_DBG_FILE_DEVICE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define LMV_DBG_IOCTL_PTE_HOOK_INSTALL CTL_CODE(LMV_DBG_FILE_DEVICE, 0x80F, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define LMV_DBG_IOCTL_PTE_HOOK_REMOVE CTL_CODE(LMV_DBG_FILE_DEVICE, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define LMV_DBG_PING_MAGIC 0x4C4D4442u
#define LMV_DBG_PROTOCOL_VERSION 1u
#define LMV_DBG_FLAG_HOOK_ACTIVE 0x1u

typedef struct _LMV_DBG_ATTACH_REQUEST
{
    ULONG ProcessId;
} LMV_DBG_ATTACH_REQUEST, *PLMV_DBG_ATTACH_REQUEST;

typedef struct _LMV_DBG_PING_REPLY
{
    ULONG Magic;
    ULONG Version;
    ULONG Flags;
    ULONG Reserved;
} LMV_DBG_PING_REPLY, *PLMV_DBG_PING_REPLY;


typedef struct _LMV_DBG_NPT_HOOK_REQUEST
{
    ULONG ProcessId;
    ULONG Reserved;
    ULONGLONG Address;
    ULONGLONG GuestPhysicalAddress;
    ULONG PatchOffset;
    ULONG PatchLength;
    UCHAR PatchBytes[16];
} LMV_DBG_NPT_HOOK_REQUEST, *PLMV_DBG_NPT_HOOK_REQUEST;

typedef struct _LMV_DBG_NPT_HOOK_INFO
{
    ULONG Status;
    ULONG Installed;
    ULONG ProcessId;
    ULONG Reserved;
    ULONGLONG Address;
    ULONGLONG GuestPhysicalAddress;
    ULONGLONG OriginalHostPhysicalAddress;
    ULONGLONG ShadowHostPhysicalAddress;
} LMV_DBG_NPT_HOOK_INFO, *PLMV_DBG_NPT_HOOK_INFO;
typedef struct _LMV_GUEST_DEBUG_HOOK_SMOKE_RESULT
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Step;
    ULONGLONG GuestPhysicalAddress;
    ULONGLONG OriginalHostPhysicalAddress;
    ULONGLONG ShadowHostPhysicalAddress;
    ULONGLONG OriginalValue;
    ULONGLONG PatchedValue;
    ULONGLONG RestoredValue;
    ULONG ProcessorsSeen;
    ULONG ProcessorsExpected;
    ULONG Mode;              // LMV_DEBUG_MODE_INTERACTIVE or LMV_DEBUG_MODE_TRANSPARENT
} LMV_GUEST_DEBUG_HOOK_SMOKE_RESULT, *PLMV_GUEST_DEBUG_HOOK_SMOKE_RESULT;

#define LMV_EXEC_PROBE_ARM 1u
#define LMV_EXEC_PROBE_DISARM 2u
typedef struct _LMV_GUEST_EXEC_PROBE
{
    ULONG Version;
    ULONG Size;
    ULONG Operation;
    ULONG Status;
    ULONGLONG OriginalPage;
    ULONGLONG MirrorPage;
    ULONGLONG HookEntry;
    ULONGLONG GuestPhysicalAddress;
    ULONG ProcessorsSeen;
    ULONG ProcessorsExpected;
    ULONG Mode;              // LMV_DEBUG_MODE_INTERACTIVE or LMV_DEBUG_MODE_TRANSPARENT
} LMV_GUEST_EXEC_PROBE, *PLMV_GUEST_EXEC_PROBE;
#define LMV_GUEST_STUB_STATE_IDLE 0u
#define LMV_GUEST_STUB_STATE_PENDING 1u
#define LMV_GUEST_STUB_STATE_CONTINUED 2u
#define LMV_GUEST_STUB_DISPOSITION_CONTINUE 1u
#define LMV_GUEST_STUB_DISPOSITION_HANDLED 2u
#define LMV_GUEST_STUB_QUEUE_CAPACITY 16u
typedef struct _LMV_GUEST_STUB_REQUEST
{
    ULONG Version;
    ULONG Size;
    ULONG ProcessId;
    ULONG ThreadId;
} LMV_GUEST_STUB_REQUEST, *PLMV_GUEST_STUB_REQUEST;

typedef struct _LMV_GUEST_STUB_EVENT
{
    ULONG Version;
    ULONG Size;
    ULONG State;
    ULONG Sequence;
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG ExceptionCode;
    ULONG Reserved;
    ULONGLONG ExceptionAddress;
    ULONGLONG Rax;
    ULONGLONG Rcx;
    ULONGLONG Rdx;
    ULONGLONG R8;
    ULONGLONG R9;
    ULONGLONG R10;
    ULONGLONG R11;
} LMV_GUEST_STUB_EVENT, *PLMV_GUEST_STUB_EVENT;
typedef struct _LMV_GUEST_STUB_WAIT_REQUEST
{
    ULONG Version;
    ULONG Size;
    ULONG TimeoutMilliseconds;
    ULONG Reserved;
} LMV_GUEST_STUB_WAIT_REQUEST, *PLMV_GUEST_STUB_WAIT_REQUEST;

typedef struct _LMV_GUEST_STUB_CONTINUE
{
    ULONG Version;
    ULONG Size;
    ULONG Sequence;
    ULONG Disposition;
    ULONG State;
    ULONG Reserved;
} LMV_GUEST_STUB_CONTINUE, *PLMV_GUEST_STUB_CONTINUE;
typedef struct _LMV_GUEST_KI_DISPATCH_INFO
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Method;
    ULONGLONG RoutineAddress;
    ULONGLONG GuestPhysicalAddress;
    ULONGLONG HostPhysicalAddress;
    ULONGLONG KernelBase;
    ULONG KernelImageSize;
    ULONG PageOffset;
    ULONG BytesRead;
    ULONG PrologueVerified;
    ULONG JumpPatchBytes;
    ULONG ReplayPrologueBytes;
    ULONG SubRspAllocation;
    ULONGLONG ResumeAddress;
    UCHAR OriginalBytes[32];
} LMV_GUEST_KI_DISPATCH_INFO, *PLMV_GUEST_KI_DISPATCH_INFO;

#define LMV_KI_PREFLIGHT_IMAGE_RANGE 0x00000001u
#define LMV_KI_PREFLIGHT_STABLE 0x00000002u
#define LMV_KI_PREFLIGHT_PROLOGUE 0x00000004u
#define LMV_KI_PREFLIGHT_NPT_TRANSLATED 0x00000008u
#define LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS 32u

typedef struct _LMV_KI_PAGE_FUNCTION_RECORD
{
    ULONGLONG BeginAddress;
    ULONGLONG EndAddress;
    ULONGLONG UnwindAddress;
    ULONG IsTarget;
    ULONG Reserved;
} LMV_KI_PAGE_FUNCTION_RECORD, *PLMV_KI_PAGE_FUNCTION_RECORD;

typedef struct _LMV_GUEST_KI_PAGE_PREFLIGHT
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Flags;
    ULONGLONG RoutineAddress;
    ULONGLONG PageAddress;
    ULONGLONG GuestPhysicalPage;
    ULONGLONG HostPhysicalPage;
    ULONGLONG KernelBase;
    ULONG KernelImageSize;
    ULONG EntryOffset;
    ULONG BytesCopied;
    ULONG PrologueVerified;
    ULONGLONG PageHash;
    UCHAR PageBytes[4096];
    ULONGLONG FunctionBeginAddress;
    ULONGLONG FunctionEndAddress;
    ULONGLONG FunctionUnwindAddress;
    ULONG FunctionRecordFound;
    ULONG EntryWithinFunction;
    ULONG SamePageFunctionCount;
    ULONG SamePageOtherFunctionCount;
    ULONG PageFunctionRecordCount;
    ULONG PageFunctionRecordCapacity;
    ULONG PageFunctionRecordTruncated;
    LMV_KI_PAGE_FUNCTION_RECORD PageFunctionRecords[LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS];
} LMV_GUEST_KI_PAGE_PREFLIGHT, *PLMV_GUEST_KI_PAGE_PREFLIGHT;

typedef struct _LMV_GUEST_KI_NEXT_PAGE
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Flags;
    ULONGLONG PageAddress;
    ULONG BytesCopied;
    ULONG Reserved;
    ULONGLONG PageHash;
    UCHAR PageBytes[4096];
    ULONG PageFunctionRecordCount;
    ULONG PageFunctionRecordTruncated;
    LMV_KI_PAGE_FUNCTION_RECORD PageFunctionRecords[LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS];
} LMV_GUEST_KI_NEXT_PAGE, *PLMV_GUEST_KI_NEXT_PAGE;

typedef struct _LMV_GUEST_KI_WINDOW_PAGE
{
    LONG PageOffset;
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Flags;
    ULONGLONG RoutineAddress;
    ULONGLONG KernelBase;
    ULONG KernelImageSize;
    ULONG BytesCopied;
    ULONGLONG PageAddress;
    ULONGLONG PageHash;
    UCHAR PageBytes[4096];
    ULONG PageFunctionRecordCount;
    ULONG PageFunctionRecordTruncated;
    LMV_KI_PAGE_FUNCTION_RECORD PageFunctionRecords[LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS];
} LMV_GUEST_KI_WINDOW_PAGE, *PLMV_GUEST_KI_WINDOW_PAGE;

#define LMV_KI_MIRROR_MAX_PAGES 8u
#define LMV_KI_MIRROR_ALLOCATE 1u
#define LMV_KI_MIRROR_RELEASE 2u
#define LMV_KI_MIRROR_ALLOCATE_MDL 3u
#define LMV_KI_MIRROR_FLAG_STORAGE_ONLY 0x00000001u
#define LMV_KI_MIRROR_FLAG_PHYSICAL_LIST 0x00000002u
#define LMV_KI_MIRROR_FLAG_MDL 0x00000004u
typedef struct _LMV_GUEST_KI_MIRROR_STORAGE
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Operation;
    ULONG Flags;
    ULONG RequestedPages;
    ULONG AllocatedPages;
    ULONG Reserved;
    ULONGLONG VirtualAddress;
    ULONGLONG PhysicalPages[LMV_KI_MIRROR_MAX_PAGES];
    ULONGLONG ContentHash;
} LMV_GUEST_KI_MIRROR_STORAGE, *PLMV_GUEST_KI_MIRROR_STORAGE;
typedef struct _LMV_GUEST_KI_EXEC_MIRROR_SMOKE
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Stage;
    ULONGLONG VirtualAddress;
    ULONGLONG PhysicalAddress;
    ULONGLONG ExpectedValue;
    ULONGLONG ObservedValue;
    ULONG ProtectionApplied;
    ULONG Reserved;
} LMV_GUEST_KI_EXEC_MIRROR_SMOKE, *PLMV_GUEST_KI_EXEC_MIRROR_SMOKE;

#define LMV_KI_SITE_IMAGE_CODE 0x00000001u
#define LMV_KI_SITE_NX_BUFFER 0x00000002u
typedef struct _LMV_GUEST_KI_STUB_SITES
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Flags;
    ULONGLONG ImageCodeAddress;
    ULONGLONG NxBufferAddress;
    ULONGLONG NxBufferSize;
} LMV_GUEST_KI_STUB_SITES, *PLMV_GUEST_KI_STUB_SITES;

typedef struct _LMV_GUEST_STATIC_STUB_SMOKE
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Flags;
    ULONGLONG HandledResult;
    ULONGLONG ContinuedResult;
    ULONGLONG HandledEntryFlags;
    ULONGLONG HandledExitFlags;
    ULONGLONG ContinuedEntryFlags;
    ULONGLONG ContinuedExitFlags;
    ULONG HandledCalls;
    ULONG ContinuedCalls;
} LMV_GUEST_STATIC_STUB_SMOKE, *PLMV_GUEST_STATIC_STUB_SMOKE;

typedef struct _LMV_GUEST_KERNEL_EXEC_SMOKE
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Step;
    ULONGLONG GuestPhysicalAddress;
    ULONGLONG OriginalAddress;
    ULONGLONG MirrorAddress;
    ULONGLONG RedirectAddress;
    ULONGLONG ReadBefore;
    ULONGLONG ReadArmed;
    ULONGLONG ReadRestored;
    ULONGLONG HandledResult;
    ULONGLONG ContinuedResult;
    ULONGLONG NonEntryResult;
    ULONGLONG RestoredResult;
    ULONG HandledCalls;
    ULONG ContinuedCalls;
    ULONG ProcessorsSeen;
    ULONG ProcessorsExpected;
    ULONG Mode;              // LMV_DEBUG_MODE_INTERACTIVE or LMV_DEBUG_MODE_TRANSPARENT
    ULONG RemainingStage;
    ULONG ProcessorsExecuted;
    ULONG WorkerMismatches;
    ULONG Iterations;
} LMV_GUEST_KERNEL_EXEC_SMOKE, *PLMV_GUEST_KERNEL_EXEC_SMOKE;
typedef struct _LMV_GUEST_MEMORY_REQUEST
{
    ULONG Version;
    ULONG Size;
    ULONGLONG ProcessId;
    ULONGLONG Address;
    ULONG Length;
    ULONG Flags;
    UCHAR Data[1];
} LMV_GUEST_MEMORY_REQUEST, *PLMV_GUEST_MEMORY_REQUEST;

typedef struct _LMV_GUEST_MEMORY_RESPONSE
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG BytesTransferred;
    ULONGLONG ProcessId;
    ULONGLONG Address;
    ULONG RequestedLength;
    ULONG PageCount;
    ULONGLONG ValidPageBitmap[4];
    UCHAR Data[1];
} LMV_GUEST_MEMORY_RESPONSE, *PLMV_GUEST_MEMORY_RESPONSE;

// Host-level virtual memory read/write request (via CR3 switch)
typedef struct _LMV_HOST_MEMORY_REQUEST
{
    ULONGLONG VirtualAddress;
    ULONGLONG Cr3;
    ULONG Length;
    ULONG Reserved;
    UCHAR Data[256];  // Inline buffer for small reads/writes
} LMV_HOST_MEMORY_REQUEST, *PLMV_HOST_MEMORY_REQUEST;

typedef struct _LMV_CPUID_RESULT
{
    ULONG Status;
    ULONG Flags;
    ULONG Version;
    ULONG Sequence;
} LMV_CPUID_RESULT, *PLMV_CPUID_RESULT;

typedef struct _LMV_CPUID_SHARED_PAGE_RESULT
{
    ULONG Status;
    ULONG GpaLow;
    ULONG GpaHigh;
    ULONG Magic;
} LMV_CPUID_SHARED_PAGE_RESULT, *PLMV_CPUID_SHARED_PAGE_RESULT;

typedef struct _LMV_SHARED_COMMAND
{
    ULONG State;
    ULONG CommandId;
    ULONG Version;
    ULONG Size;
    ULONG Sequence;
    ULONG Flags;
    ULONG Reserved;
    ULONG Reserved2;
    ULONGLONG Argument0;
    ULONGLONG Argument1;
    ULONGLONG Argument2;
    ULONGLONG Argument3;
} LMV_SHARED_COMMAND, *PLMV_SHARED_COMMAND;

typedef struct _LMV_SHARED_RESULT
{
    ULONG State;
    ULONG Status;
    ULONG Version;
    ULONG Size;
    ULONG Sequence;
    ULONG Flags;
    ULONG Reserved;
    ULONG Reserved2;
    ULONGLONG Value0;
    ULONGLONG Value1;
    ULONGLONG Value2;
    ULONGLONG Value3;
} LMV_SHARED_RESULT, *PLMV_SHARED_RESULT;

typedef enum _LMV_DEBUG_COMMAND_ID
{
    LmvDebugCommandAttachProcess = 30,
    LmvDebugCommandDetachProcess = 31,
    LmvDebugCommandQueryState = 32,
    LmvDebugCommandInstallNptHook = 33,
    LmvDebugCommandRemoveNptHook = 34,
    LmvDebugCommandReleaseNptHookRegion = 35,
    LmvDebugCommandTranslateNpt = 36,
    LmvDebugCommandArmExecProbe = 37,
    LmvDebugCommandDisarmExecProbe = 38,
    LmvDebugCommandArmPrivateDr3 = 39,
    LmvDebugCommandDisarmPrivateDr3 = 40,
    LmvDebugCommandQueryPrivateDr3 = 41,
    LmvDebugCommandArmKiDr3Once = 42,
    LmvDebugCommandQueryKiDr3Once = 43,
    LmvDebugCommandQueryKiDr3Args = 44,
    LmvDebugCommandQueryKiDr3Frame = 45,
    LmvDebugCommandArmKiDr3RedirectOnce = 46,
    LmvDebugCommandStartKiSession = 47,
    LmvDebugCommandStopKiSession = 48,
    LmvDebugCommandRegisterKiExits = 49,
    LmvDebugCommandQueryKiDrain = 50,
    LmvDebugCommandRetireKiGuest = 51,
    LmvDebugCommandSetMemoryBreakpoint = 52,
    LmvDebugCommandRemoveMemoryBreakpoint = 53,
    LmvDebugCommandQueryMemoryBreakpoint = 54,
    LmvDebugCommandSetMode = 55,              // Set debug mode and state
    LmvDebugCommandConfigureDr2Watch = 56,
    LmvDebugCommandRemoveDr2Watch = 57
} LMV_DEBUG_COMMAND_ID;

#define LMV_KI_SESSION_START 1u
#define LMV_KI_SESSION_STOP 2u
#define LMV_KI_SESSION_QUERY 3u
#define LMV_KI_SESSION_PREPARE_UNLOAD 4u
#define LMV_KI_DRAIN_REGISTERED 1u
#define LMV_KI_DRAIN_ENABLED 2u
#define LMV_KI_DRAIN_RETIRED 4u
typedef struct _LMV_GUEST_KI_SESSION
{
    ULONG Version;
    ULONG Size;
    ULONG Operation;
    ULONG ProcessId;
    ULONGLONG KernelCr3;
    ULONGLONG KiAddress;
    ULONG ProcessorsSeen;
    ULONG ProcessorsExpected;
    ULONG Mode;              // LMV_DEBUG_MODE_INTERACTIVE or LMV_DEBUG_MODE_TRANSPARENT
} LMV_GUEST_KI_SESSION, *PLMV_GUEST_KI_SESSION;

#define LMV_DR2_WATCH_START 1u
#define LMV_DR2_WATCH_STOP 2u
#define LMV_DR2_CONDITION_WRITE 1u
#define LMV_DR2_CONDITION_READ_WRITE 3u
typedef struct _LMV_GUEST_DR2_WATCH
{
    ULONG Version;
    ULONG Size;
    ULONG Operation;
    ULONG ProcessId;
    ULONGLONG Address;
    ULONG Length;
    ULONG Condition;
    ULONG Status;
    ULONG ProcessorsSeen;
    ULONG ProcessorsExpected;
    ULONGLONG Hits;
} LMV_GUEST_DR2_WATCH, *PLMV_GUEST_DR2_WATCH;

typedef struct _LMV_GUEST_KI_DRAIN
{
    ULONG Version;
    ULONG Size;
    ULONG Operation;
    ULONG Status;
    ULONGLONG InFlight;
    ULONG ArmedProcessors;
    ULONG Flags;
    ULONG ProcessorsSeen;
    ULONG ProcessorsExpected;
} LMV_GUEST_KI_DRAIN, *PLMV_GUEST_KI_DRAIN;

typedef struct _LMV_GUEST_DR3_SMOKE_RESULT
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Step;
    ULONGLONG Target;
    ULONGLONG OriginalDr3;
    ULONGLONG GuestDr3Readback;
    ULONGLONG OriginalDr7;
    ULONGLONG GuestDr7Readback;
    ULONGLONG HitsBefore;
    ULONGLONG HitsAfter;
    ULONGLONG HitsAfterDisarm;
} LMV_GUEST_DR3_SMOKE_RESULT, *PLMV_GUEST_DR3_SMOKE_RESULT;
typedef struct _LMV_GUEST_DR_COEXIST_RESULT
{
    HANDLE StartEvent;
    HANDLE DoneEvent;
    ULONG Status;
    ULONG Step;
    ULONGLONG Target;
    ULONGLONG HitsBefore;
    ULONGLONG HitsAfterNative;
    ULONGLONG HitsAfterPrivate;
    ULONGLONG HitsAfterDisarm;
    ULONGLONG HitsAfterRestore;
} LMV_GUEST_DR_COEXIST_RESULT, *PLMV_GUEST_DR_COEXIST_RESULT;
#define LMV_KI_EXCEPTION_INTERNAL_INVALID_OP 0x10000002u
typedef struct _LMV_GUEST_KI_FRAME_SNAPSHOT
{
    ULONGLONG Rip;
    ULONGLONG Rsp;
    ULONGLONG Rflags;
    ULONGLONG Rax;
    ULONGLONG Rcx;
    ULONGLONG Rdx;
    ULONGLONG R8;
    ULONGLONG R9;
    ULONGLONG R10;
    ULONGLONG R11;
    ULONGLONG Rbx;
    ULONGLONG Rbp;
    ULONGLONG Rsi;
    ULONGLONG Rdi;
    ULONGLONG R12;
    ULONGLONG R13;
    ULONGLONG R14;
    ULONGLONG R15;
} LMV_GUEST_KI_FRAME_SNAPSHOT, *PLMV_GUEST_KI_FRAME_SNAPSHOT;
typedef struct _LMV_GUEST_KI_DR3_EVENT
{
    ULONG Version;
    ULONG Size;
    ULONG State;
    ULONG Sequence;
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG ExceptionCode;
    ULONG Reserved;
    ULONGLONG ExceptionAddress;
    LMV_GUEST_KI_FRAME_SNAPSHOT Frame;
} LMV_GUEST_KI_DR3_EVENT, *PLMV_GUEST_KI_DR3_EVENT;
typedef struct _LMV_GUEST_KI_DR3_CONTEXT
{
    ULONG Version;
    ULONG Size;
    ULONG Sequence;
    ULONG Reserved;
    LMV_GUEST_KI_FRAME_SNAPSHOT Frame;
} LMV_GUEST_KI_DR3_CONTEXT, *PLMV_GUEST_KI_DR3_CONTEXT;
typedef struct _LMV_GUEST_KI_DR3_SET_RAX
{
    ULONG Version;
    ULONG Size;
    ULONG Sequence;
    ULONG Reserved;
    ULONGLONG Rax;
} LMV_GUEST_KI_DR3_SET_RAX, *PLMV_GUEST_KI_DR3_SET_RAX;
typedef struct _LMV_GUEST_KI_DR3_SET_RIP
{
    ULONG Version;
    ULONG Size;
    ULONG Sequence;
    ULONG Reserved;
    ULONGLONG Rip;
} LMV_GUEST_KI_DR3_SET_RIP, *PLMV_GUEST_KI_DR3_SET_RIP;
#define LMV_GUEST_KI_QUEUE_BEGIN 1u
#define LMV_GUEST_KI_QUEUE_END 2u
typedef struct _LMV_GUEST_KI_QUEUE_CONTROL
{
    ULONG Version;
    ULONG Size;
    ULONG Operation;
    ULONG Status;
} LMV_GUEST_KI_QUEUE_CONTROL, *PLMV_GUEST_KI_QUEUE_CONTROL;
typedef struct _LMV_GUEST_KI_QUEUE_PRODUCE
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Sequence;
    ULONG ThreadId;
} LMV_GUEST_KI_QUEUE_PRODUCE, *PLMV_GUEST_KI_QUEUE_PRODUCE;
typedef struct _LMV_GUEST_KI_DR3_PROBE_RESULT
{
    HANDLE StartEvent;
    HANDLE DoneEvent;
    ULONG Status;
    ULONG Step;
    ULONGLONG KiAddress;
    ULONGLONG HitsBefore;
    ULONGLONG HitsAfter;
    ULONGLONG LastRip;
    ULONGLONG LastDr6;
    ULONGLONG LastCr3;
    ULONGLONG LastRcx;
    ULONGLONG LastRdx;
    ULONGLONG LastR8;
    ULONGLONG LastR9;
    ULONGLONG LastRsp;
    ULONGLONG LastRflags;
    ULONGLONG LastRax;
    ULONGLONG LastRbp;
    USHORT ProcessorGroup;
    USHORT ProcessorNumber;
    ULONGLONG ArmedTargetAfter;
    ULONGLONG ArmedDr7After;
    ULONG PrologueVerified;
    ULONG Reserved;
    ULONGLONG RedirectStub;
    ULONGLONG StubHitsBefore;
    ULONGLONG StubHitsAfter;
    ULONG InspectStatus;
    ULONG InspectReason;
    ULONG InspectProcessId;
    ULONG InspectThreadId;
    ULONG InspectExceptionCode;
    ULONG InspectFirstChance;
    ULONGLONG InspectExceptionAddress;
    ULONG FrameStatus;
    ULONG FrameReason;
    LMV_GUEST_KI_FRAME_SNAPSHOT Frame;
    ULONG EventStatus;
    ULONG EventSequence;
} LMV_GUEST_KI_DR3_PROBE_RESULT, *PLMV_GUEST_KI_DR3_PROBE_RESULT;
typedef struct _LMV_SHARED_PAGE
{
    ULONG Magic;
    ULONG Version;
    ULONG Size;
    volatile ULONG State;
    LMV_SHARED_COMMAND Command;
    LMV_SHARED_RESULT Result;
    LMV_DBG_NPT_HOOK_REQUEST DebugHookRequest;
    UINT8 Data[3072];  // General-purpose data buffer for CPUID data transfer (e.g., HOST_EXCEPTION_EVENT)
} LMV_SHARED_PAGE, *PLMV_SHARED_PAGE;

#define LMV_SHARED_PAGE_MAGIC 0x4C4D5653u
#define LMV_SHARED_STATE_FREE 0u
#define LMV_SHARED_STATE_READY 1u
#define LMV_SHARED_STATE_PROCESSING 2u
#define LMV_SHARED_STATE_COMPLETE 3u

typedef enum _LMV_NPT_COMMAND_ID
{
    LmvNptCommandRegisterPage = 10,
    LmvNptCommandReadQword = 11,
    LmvNptCommandRemapPage = 12,
    LmvNptCommandRestorePage = 13,
    LmvNptCommandFlushTlb = 14,
    LmvNptCommandArmWriteWatch = 15,
    LmvNptCommandQueryWriteWatch = 16,
    LmvNptCommandDisarmWriteWatch = 17,
    LmvNptCommandReleaseRegion = 18
} LMV_NPT_COMMAND_ID;

typedef struct _LMV_GUEST_NPT_COMMAND
{
    ULONG Version;
    ULONG Size;
    ULONG CommandId;
    ULONG Reserved;
    ULONGLONG GuestPhysicalAddress;
} LMV_GUEST_NPT_COMMAND, *PLMV_GUEST_NPT_COMMAND;

typedef struct _LMV_GUEST_NPT_RESULT
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Reserved;
    ULONGLONG Value;
    ULONGLONG HostPhysicalAddress;
} LMV_GUEST_NPT_RESULT, *PLMV_GUEST_NPT_RESULT;

typedef struct _LMV_GUEST_NPT_SMOKE_RESULT
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Step;
    ULONGLONG GuestPhysicalAddress;
    ULONGLONG HostPhysicalAddress;
    ULONGLONG OriginalValue;
    ULONGLONG MappedValue;
    ULONGLONG RestoredValue;
    ULONG ProcessorsSeen;
    ULONG ProcessorsExpected;
} LMV_GUEST_NPT_SMOKE_RESULT, *PLMV_GUEST_NPT_SMOKE_RESULT;

typedef struct _LMV_GUEST_QUERY_STATUS
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Flags;
} LMV_GUEST_QUERY_STATUS, *PLMV_GUEST_QUERY_STATUS;

typedef struct _LMV_GUEST_QUERY_NPT_STATS
{
    ULONG Version;
    ULONG Size;
    ULONG Status;
    ULONG Reserved;
    ULONGLONG VmExitCount;
    ULONGLONG NpfCount;
    ULONGLONG ProcessorCount;
    ULONGLONG SharedPageGpa;
} LMV_GUEST_QUERY_NPT_STATS, *PLMV_GUEST_QUERY_NPT_STATS;

typedef struct _LMV_QUERY_STATUS
{
    ULONG Version;
    ULONG Size;
    ULONG Flags;
    ULONG Reserved;
} LMV_QUERY_STATUS, *PLMV_QUERY_STATUS;

// ============================================================================
// NPT Memory Breakpoint Support
// ============================================================================

// IOCTL codes for memory breakpoints
#define IOCTL_LMV_GUEST_SET_MEMORY_BP    CTL_CODE(LMV_DEVICE_TYPE, 0x840, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_REMOVE_MEMORY_BP CTL_CODE(LMV_DEVICE_TYPE, 0x841, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_LMV_GUEST_QUERY_MEMORY_BP  CTL_CODE(LMV_DEVICE_TYPE, 0x842, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

// Memory breakpoint types (can be combined with bitwise OR)
#define MEMORY_BP_READ    0x00000001u  // Trigger on read access (clears Present bit)
#define MEMORY_BP_WRITE   0x00000002u  // Trigger on write access (clears Write bit)
#define MEMORY_BP_EXECUTE 0x00000004u  // Trigger on execute (sets NX bit)

// Memory breakpoint states
#define MEMORY_BP_STATE_FREE     0u
#define MEMORY_BP_STATE_ARMED    1u
#define MEMORY_BP_STATE_HIT      2u
#define MEMORY_BP_STATE_DISABLED 3u

// Memory breakpoint request structure
typedef struct _LMV_MEMORY_BP_REQUEST
{
    ULONG Version;              // Protocol version
    ULONG Size;                 // sizeof(LMV_MEMORY_BP_REQUEST)
    ULONG ProcessId;            // Target process ID
    ULONG Type;                 // MEMORY_BP_READ | MEMORY_BP_WRITE | MEMORY_BP_EXECUTE
    ULONGLONG Address;          // Virtual address to monitor
    ULONGLONG Length;           // Size of monitored region (must be page-aligned)
} LMV_MEMORY_BP_REQUEST, *PLMV_MEMORY_BP_REQUEST;

// Memory breakpoint result structure
typedef struct _LMV_MEMORY_BP_RESULT
{
    ULONG Version;              // Protocol version
    ULONG Size;                 // sizeof(LMV_MEMORY_BP_RESULT)
    ULONG Status;               // NTSTATUS code
    ULONG BreakpointId;         // Assigned breakpoint ID (0-15)
    ULONG State;                // Current breakpoint state
    ULONG Reserved;
    ULONGLONG HitCount;         // Number of times triggered
    ULONGLONG LastRip;          // RIP of last trigger
} LMV_MEMORY_BP_RESULT, *PLMV_MEMORY_BP_RESULT;

// Memory breakpoint query structure
typedef struct _LMV_MEMORY_BP_QUERY
{
    ULONG Version;              // Protocol version
    ULONG Size;                 // sizeof(LMV_MEMORY_BP_QUERY)
    ULONG BreakpointId;         // Breakpoint ID to query
    ULONG Reserved;
} LMV_MEMORY_BP_QUERY, *PLMV_MEMORY_BP_QUERY;

// Memory breakpoint remove structure
typedef struct _LMV_MEMORY_BP_REMOVE
{
    ULONG Version;              // Protocol version
    ULONG Size;                 // sizeof(LMV_MEMORY_BP_REMOVE)
    ULONG BreakpointId;         // Breakpoint ID to remove
    ULONG Reserved;
} LMV_MEMORY_BP_REMOVE, *PLMV_MEMORY_BP_REMOVE;
