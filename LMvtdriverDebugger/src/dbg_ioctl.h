// dbg_ioctl.h - shared protocol header (IOCTL definitions + structs)
// Both sides include this same file to keep control codes and layouts in sync.
#pragma once

#ifdef _KERNEL_MODE
    #include <ntddk.h>
#else
    #include <windows.h>
    #include <winioctl.h>
    #include "..\\..\\lmv_control_shared.h"
    typedef LONG NTSTATUS;
#endif

#define DEVICE_NAME         L"\\\\.\\LMvtdriverGuest"
#define NT_DEVICE_NAME      L"\\Device\\LMdbgsys"
#define SYMBOLIC_LINK_NAME  L"\\DosDevices\\LMdbgsys"

#define FILE_DEVICE_DBGSYS  0x8000

#define IOCTL_ATTACH_PROCESS    CTL_CODE(FILE_DEVICE_DBGSYS, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DETACH_PROCESS    CTL_CODE(FILE_DEVICE_DBGSYS, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GET_EVENT         CTL_CODE(FILE_DEVICE_DBGSYS, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_CONTINUE_EVENT    CTL_CODE(FILE_DEVICE_DBGSYS, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_READ_MEMORY       CTL_CODE(FILE_DEVICE_DBGSYS, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WRITE_MEMORY      CTL_CODE(FILE_DEVICE_DBGSYS, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GET_CONTEXT       CTL_CODE(FILE_DEVICE_DBGSYS, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SET_CONTEXT       CTL_CODE(FILE_DEVICE_DBGSYS, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DUMP_TABLE        CTL_CODE(FILE_DEVICE_DBGSYS, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
// PTE Hook step 1: 2MB large page split / merge
#define IOCTL_SPLIT_LARGE_PAGE  CTL_CODE(FILE_DEVICE_DBGSYS, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MERGE_LARGE_PAGE  CTL_CODE(FILE_DEVICE_DBGSYS, 0x80A, METHOD_BUFFERED, FILE_ANY_ACCESS)
// Inline hook KiDispatchException (manual install / remove)
#define IOCTL_INSTALL_HOOK      CTL_CODE(FILE_DEVICE_DBGSYS, 0x80B, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_REMOVE_HOOK       CTL_CODE(FILE_DEVICE_DBGSYS, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS)
// Debugger-initiated stop: suspend a target thread and push a synthetic break event
#define IOCTL_BREAK_TARGET      CTL_CODE(FILE_DEVICE_DBGSYS, 0x80D, METHOD_BUFFERED, FILE_ANY_ACCESS)
// Driver liveness check: echo a magic value + protocol version so R3 can
// verify the handle really talks to an alive dbgsys.sys.
#define IOCTL_PING              CTL_CODE(FILE_DEVICE_DBGSYS, 0x80E, METHOD_BUFFERED, FILE_ANY_ACCESS)
// Process-private PTE hook of KiDispatchException: copy the target 4KB page,
// patch the copy, and repoint the target process's private PTE chain to it.
// Unlike IOCTL_INSTALL_HOOK (which edits ntoskrnl code directly), this only
// affects one attached process and leaves the original physical page intact.
#define IOCTL_PTE_HOOK_INSTALL  CTL_CODE(FILE_DEVICE_DBGSYS, 0x80F, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PTE_HOOK_REMOVE   CTL_CODE(FILE_DEVICE_DBGSYS, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)

// DBG_EVENT_INFO.Reserved[0] flag: event was synthesized by IOCTL_BREAK_TARGET,
// not raised by real CPU execution. R3 must not rewind RIP or re-arm anything.
#define DBG_FLAG_SYNTHETIC_BREAK 0x1

typedef enum _DBG_EVENT_TYPE {
    DbgEventException      = 1,
    DbgEventCreateThread   = 2,
    DbgEventCreateProcess  = 3,
    DbgEventExitThread     = 4,
    DbgEventExitProcess    = 5,
    DbgEventLoadDll        = 6,
    DbgEventUnloadDll      = 7,
} DBG_EVENT_TYPE;

// context read/write (GET_CONTEXT / SET_CONTEXT)
typedef struct _DBG_CONTEXT {
    ULONG64 Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rbp, Rsp, Rip;
    ULONG64 R8, R9, R10, R11, R12, R13, R14, R15;
    ULONG64 RFlags;
    ULONG64 SegCs, SegSs, SegDs, SegEs, SegFs, SegGs;
} DBG_CONTEXT, *PDBG_CONTEXT;

typedef struct _DBG_EVENT_INFO {
    ULONG        EventType;
    ULONG        ProcessId;
    ULONG        ThreadId;
    NTSTATUS     ExceptionCode;
    ULONG64      ExceptionAddress;
    ULONG        FirstChance;
    ULONG        Reserved[2];
    DBG_CONTEXT  Context;
    BOOLEAN      ContextValid;
} DBG_EVENT_INFO, *PDBG_EVENT_INFO;

typedef struct _ATTACH_REQUEST {
    ULONG        ProcessId;
} ATTACH_REQUEST, *PATTACH_REQUEST;

typedef struct _BREAK_REQUEST {
    ULONG        ProcessId;
    ULONG        ThreadId;      // thread to suspend; R3 picks it (0 = invalid)
} BREAK_REQUEST, *PBREAK_REQUEST;

// IOCTL_PING reply. Magic must match DBGSYS_PING_MAGIC or R3 treats the
// driver as incompatible / stale.
#define DBGSYS_PING_MAGIC       0x4C4D4442  // 'LMDB'
#define DBGSYS_PROTOCOL_VERSION 1

// PING_REPLY.Flags bit: KiDispatchException inline hook is installed
#define DBGSYS_FLAG_HOOK_ACTIVE 0x1

typedef struct _PING_REPLY {
    ULONG        Magic;
    ULONG        Version;
    ULONG        Flags;
    ULONG        Reserved;
} PING_REPLY, *PPING_REPLY;

typedef struct _MEMORY_REQUEST {
    ULONG        ProcessId;
    ULONG64      Address;
    ULONG        Size;
} MEMORY_REQUEST, *PMEMORY_REQUEST;

typedef struct _CONTEXT_REQUEST {
    ULONG        ProcessId;
    ULONG        ThreadId;
} CONTEXT_REQUEST, *PCONTEXT_REQUEST;

// PTE Hook step 1: large page split/merge request and result
typedef struct _SPLIT_PAGE_REQUEST {
    ULONG        ProcessId;   // needed for user-mode addresses; pass 0 for kernel addresses
    ULONG64      Address;     // 4KB aligned; 0 = auto-resolve KiDispatchException
} SPLIT_PAGE_REQUEST, *PSPLIT_PAGE_REQUEST;

typedef struct _SPLIT_PAGE_INFO {
    ULONG64      Address;            // target virtual address (4KB aligned)
    ULONG64      LargePageBase;      // base of the containing 2MB large page
    ULONG64      PdeAddress;         // self-mapping virtual address of the page's PDE
    ULONG64      OldPdeValue;        // original PDE value (used when merging back)
    ULONG64      NewPdeValue;        // PDE value after the split
    ULONG64      PteTableVirtual;    // virtual address of the new 4KB PTE table
    ULONG64      PteTablePhysical;   // physical address of the new 4KB PTE table
    BOOLEAN      WasLargePage;       // TRUE = a large page was actually split/merged
} SPLIT_PAGE_INFO, *PSPLIT_PAGE_INFO;

// Process-private PTE hook install/remove request. Address 0 means "resolve
// KiDispatchException automatically". The target process must already be
// attached; the driver auto-splits the containing large page on install.
typedef struct _PTE_HOOK_REQUEST {
    ULONG        ProcessId;
    ULONG64      Address;            // 4KB aligned; 0 = auto-resolve KiDispatchException
} PTE_HOOK_REQUEST, *PPTE_HOOK_REQUEST;

typedef struct _PTE_HOOK_INFO {
    ULONG64      Address;            // target virtual address (4KB aligned)
    ULONG64      OriginalPte;        // private-chain PTE value before hook
    ULONG64      NewPte;             // private-chain PTE value after hook
    ULONG64      NewPageVirtual;     // kernel virtual address of the patched copy
    ULONG64      NewPagePhysical;    // physical address of the patched copy
} PTE_HOOK_INFO, *PPTE_HOOK_INFO;


