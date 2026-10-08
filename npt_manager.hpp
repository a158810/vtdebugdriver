#pragma once

#include <ntifs.h>
#include "lmv_control_shared.h"

//
// x86-64 defined structures.
//

//
// See "2-Mbyte PML4E-Long Mode" and "2-Mbyte PDPE-Long Mode".
//
typedef struct _PML4_ENTRY_2MB
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT64 Valid : 1;               // [0]
            UINT64 Write : 1;               // [1]
            UINT64 User : 1;                // [2]
            UINT64 WriteThrough : 1;        // [3]
            UINT64 CacheDisable : 1;        // [4]
            UINT64 Accessed : 1;            // [5]
            UINT64 Reserved1 : 3;           // [6:8]
            UINT64 Avl : 3;                 // [9:11]
            UINT64 PageFrameNumber : 40;    // [12:51]
            UINT64 Reserved2 : 11;          // [52:62]
            UINT64 NoExecute : 1;           // [63]
        } Fields;
    };
} PML4_ENTRY_2MB, *PPML4_ENTRY_2MB,
  PDPT_ENTRY_2MB, *PPDPT_ENTRY_2MB;
static_assert(sizeof(PML4_ENTRY_2MB) == 8,
              "PML4_ENTRY_1GB Size Mismatch");

//
// See "2-Mbyte PDE-Long Mode".
//
typedef struct _PD_ENTRY_2MB
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT64 Valid : 1;               // [0]
            UINT64 Write : 1;               // [1]
            UINT64 User : 1;                // [2]
            UINT64 WriteThrough : 1;        // [3]
            UINT64 CacheDisable : 1;        // [4]
            UINT64 Accessed : 1;            // [5]
            UINT64 Dirty : 1;               // [6]
            UINT64 LargePage : 1;           // [7]
            UINT64 Global : 1;              // [8]
            UINT64 Avl : 3;                 // [9:11]
            UINT64 Pat : 1;                 // [12]
            UINT64 Reserved1 : 8;           // [13:20]
            UINT64 PageFrameNumber : 31;    // [21:51]
            UINT64 Reserved2 : 11;          // [52:62]
            UINT64 NoExecute : 1;           // [63]
        } Fields;
    };
} PD_ENTRY_2MB, *PPD_ENTRY_2MB;
static_assert(sizeof(PD_ENTRY_2MB) == 8,
              "PDE_ENTRY_2MB Size Mismatch");

//
// SimpleSVM specific structures.
//

typedef struct _PML4E_TREE
{
    DECLSPEC_ALIGN(PAGE_SIZE) PDPT_ENTRY_2MB PdptEntries[512];
    DECLSPEC_ALIGN(PAGE_SIZE) PD_ENTRY_2MB PdEntries[512][512];
} PML4E_TREE, *PPML4E_TREE;

typedef struct _NPT_TEST_PAGE
{
    volatile LONG64 Gpa;
    volatile LONG State;
    volatile LONG WatchState;
    volatile LONG64 WriteNpfCount;
    volatile LONG64 WriteFaultInfo1;
    volatile LONG64 WriteFaultGpa;
    UINT64 BackingPhysicalAddress;
    DECLSPEC_ALIGN(PAGE_SIZE) UINT8 BackingPage[PAGE_SIZE];
} NPT_TEST_PAGE, *PNPT_TEST_PAGE;

typedef struct _NPT_TEST_REGION
{
    UINT64 Base;
    UINT64 OriginalPde;
    UINT64 PtesPhysicalAddress;
    LONG MappedPageCount;
    LONG State;
    DECLSPEC_ALIGN(PAGE_SIZE) UINT64 Ptes[512];
} NPT_TEST_REGION, *PNPT_TEST_REGION;


typedef struct _NPT_DEBUG_REGION
{
    UINT64 Base;
    UINT64 OriginalPde;
    UINT64 PtesPhysicalAddress;
    volatile LONG State;
    DECLSPEC_ALIGN(PAGE_SIZE) UINT64 Ptes[512];
} NPT_DEBUG_REGION, *PNPT_DEBUG_REGION;
typedef struct _NPT_DEBUG_HOOK
{
    volatile LONG State;
    ULONG ProcessId;
    ULONG PatchOffset;
    ULONG PatchLength;
    UINT64 GuestPhysicalAddress;
    UINT64 OriginalHostPhysicalAddress;
    UINT64 ShadowHostPhysicalAddress;
    UINT64 EntryAddress;
    UINT64 MirrorAddress;
    UINT64 RedirectAddress;
    volatile LONG ExecuteProbe;
    DECLSPEC_ALIGN(PAGE_SIZE) UINT8 ShadowPage[PAGE_SIZE];
} NPT_DEBUG_HOOK, *PNPT_DEBUG_HOOK;
typedef struct _SHARED_VIRTUAL_PROCESSOR_DATA
{
    PVOID MsrPermissionsMap;
    DECLSPEC_ALIGN(PAGE_SIZE) LMV_SHARED_PAGE SharedPage;
    volatile LONG NptTestControlLock;
    volatile LONG64 VmExitTotal;
    volatile LONG64 NpfTotal;
    ULONG ProcessorCount;
    volatile LONG DebugAttached;
    volatile ULONG DebugProcessId;
    volatile LONG KiSessionEnabled;
    volatile LONG KiGuestRegistered;
    volatile LONG KiGuestRetired;
    volatile LONG KiGuestReleased;
    volatile LONG KiArmedProcessors;
    volatile LONG64 KiInFlight;
    UINT64 KiGuestBase;
    UINT64 KiGuestEnd;
    UINT64 KiCaptureExit;
    UINT64 KiCommitExit;
    UINT64 KiPassExit;
    UINT64 KiSessionCr3Mask;
    UINT64 KiSessionCr3;
    UINT64 KiSessionEntry;
    UINT64 KiSessionEventStub;
    UINT64 KiSessionPassStub;
    NPT_TEST_REGION NptTestRegions[4];
    NPT_TEST_PAGE NptTestPages[4];
    NPT_DEBUG_REGION DebugRegion;
    NPT_DEBUG_HOOK DebugHook;
    DECLSPEC_ALIGN(PAGE_SIZE) PML4_ENTRY_2MB Pml4Entries[512];
    DECLSPEC_ALIGN(PAGE_SIZE) PML4E_TREE Pml4eTrees[2];    // For 1TB
} SHARED_VIRTUAL_PROCESSOR_DATA, *PSHARED_VIRTUAL_PROCESSOR_DATA;

#define NPT_TEST_STATE_UNREGISTERED 0
#define NPT_TEST_STATE_REGISTERED   1
#define NPT_TEST_STATE_MAPPED       2
#define NPT_TEST_STATE_BUSY         3
#define NPT_TEST_WATCH_UNARMED       0
#define NPT_TEST_WATCH_ARMED         1
#define NPT_TEST_WATCH_FIRED         2
#define NPT_TEST_WATCH_BUSY          3
#define NPT_TEST_REGION_FREE          0
#define NPT_TEST_REGION_ACTIVE        1
#define NPT_TEST_REGION_RETIRED       2

BOOLEAN SvTranslateNpt2Mb(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, UINT64 GuestPhysicalAddress, PUINT64 HostPhysicalAddress);
PD_ENTRY_2MB* SvGetNptPde(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, UINT64 GuestPhysicalAddress);
PNPT_TEST_PAGE SvFindNptTestPage(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, UINT64 GuestPhysicalAddress);
BOOLEAN SvRegisterNptTestPage(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, UINT64 GuestPhysicalAddress);
BOOLEAN SvSetNptTestPageMapping(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, UINT64 GuestPhysicalAddress, BOOLEAN MapToTestPage);
BOOLEAN SvReleaseNptTestRegion(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, UINT64 GuestPhysicalAddress);
volatile LONG64* SvGetNptTestPte(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, UINT64 GuestPhysicalAddress);
BOOLEAN SvArmNptTestWriteWatch(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, UINT64 GuestPhysicalAddress);
BOOLEAN SvDisarmNptTestWriteWatch(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, UINT64 GuestPhysicalAddress);

BOOLEAN SvInstallNptDebugHook(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, ULONG ProcessId, UINT64 GuestPhysicalAddress, ULONG PatchOffset, const UCHAR* PatchBytes, ULONG PatchLength, PVOID Info);
BOOLEAN SvRemoveNptDebugHook(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, PVOID Info);
BOOLEAN SvInstallNptExecProbe(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, ULONG ProcessId, UINT64 GuestPhysicalAddress, UINT64 EntryAddress, UINT64 MirrorAddress, UINT64 RedirectAddress);
BOOLEAN SvHandleNptExecProbe(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, UINT64 GuestPhysicalAddress, UINT64 ExitInfo1, PUINT64 GuestRip);

BOOLEAN SvReleaseNptDebugHookRegion(PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData);
