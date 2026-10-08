#include "npt_manager.hpp"

BOOLEAN
SvTranslateNpt2Mb (
    _In_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ UINT64 GuestPhysicalAddress,
    _Out_ PUINT64 HostPhysicalAddress
    )
{
    ULONG64 pml4Index;
    ULONG64 pdptIndex;
    ULONG64 pdIndex;
    PPML4_ENTRY_2MB pml4Entry;
    PPML4E_TREE pml4Tree;
    PD_ENTRY_2MB* pdEntry;
    UINT64 pdEntryValue;
    UINT64 pteValue;
    ULONG pteIndex;

    pml4Index = (GuestPhysicalAddress >> 39) & 0x1ff;
    pdptIndex = (GuestPhysicalAddress >> 30) & 0x1ff;
    pdIndex = (GuestPhysicalAddress >> 21) & 0x1ff;

    if (pml4Index >= RTL_NUMBER_OF(SharedVpData->Pml4eTrees))
    {
        return FALSE;
    }

    pml4Entry = &SharedVpData->Pml4Entries[pml4Index];
    pml4Tree = &SharedVpData->Pml4eTrees[pml4Index];
    if (pml4Entry->Fields.Valid == 0 ||
        pml4Tree->PdptEntries[pdptIndex].Fields.Valid == 0)
    {
        return FALSE;
    }

    pdEntry = &pml4Tree->PdEntries[pdptIndex][pdIndex];
    pdEntryValue = pdEntry->AsUInt64;
    if ((pdEntryValue & 1) == 0)
    {
        return FALSE;
    }

    if ((pdEntryValue & (1ull << 7)) != 0)
    {
        *HostPhysicalAddress =
            (pdEntry->Fields.PageFrameNumber << 21) |
            (GuestPhysicalAddress & ((1ull << 21) - 1));
        return TRUE;
    }

    PNPT_TEST_REGION region = nullptr;
    for (ULONG index = 0; index < RTL_NUMBER_OF(SharedVpData->NptTestRegions); ++index)
    {
        PNPT_TEST_REGION candidate = &SharedVpData->NptTestRegions[index];
        if (candidate->State == NPT_TEST_REGION_ACTIVE &&
            candidate->Base == (GuestPhysicalAddress & ~((1ull << 21) - 1)) &&
            candidate->PtesPhysicalAddress ==
                (pdEntryValue & 0x000ffffffffff000ull))
        {
            region = candidate;
            break;
        }
    }
    if (region == nullptr)
    {
        PNPT_DEBUG_REGION debugRegion = &SharedVpData->DebugRegion;
        if (debugRegion->State == NPT_TEST_REGION_ACTIVE &&
            debugRegion->Base == (GuestPhysicalAddress & ~((1ull << 21) - 1)) &&
            debugRegion->PtesPhysicalAddress ==
                (pdEntryValue & 0x000ffffffffff000ull))
        {
            pteValue = debugRegion->Ptes[(GuestPhysicalAddress >> PAGE_SHIFT) & 0x1ff];
            if ((pteValue & 1) == 0)
                return FALSE;
            *HostPhysicalAddress = (pteValue & 0x000ffffffffff000ull) |
                                   (GuestPhysicalAddress & (PAGE_SIZE - 1));
            return TRUE;
        }
    }
    if (region == nullptr)
    {
        return FALSE;
    }

    pteIndex = static_cast<ULONG>((GuestPhysicalAddress >> PAGE_SHIFT) & 0x1ff);
    pteValue = region->Ptes[pteIndex];
    if ((pteValue & 1) == 0)
    {
        return FALSE;
    }

    *HostPhysicalAddress =
        (pteValue & 0x000ffffffffff000ull) |
        (GuestPhysicalAddress & (PAGE_SIZE - 1));
    return TRUE;
}

PD_ENTRY_2MB*
SvGetNptPde (
    _In_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ UINT64 GuestPhysicalAddress
    )
{
    ULONG64 pml4Index;
    ULONG64 pdptIndex;
    ULONG64 pdIndex;
    PPML4E_TREE pml4Tree;

    pml4Index = (GuestPhysicalAddress >> 39) & 0x1ff;
    pdptIndex = (GuestPhysicalAddress >> 30) & 0x1ff;
    pdIndex = (GuestPhysicalAddress >> 21) & 0x1ff;
    if (pml4Index >= RTL_NUMBER_OF(SharedVpData->Pml4eTrees))
    {
        return nullptr;
    }

    if (SharedVpData->Pml4Entries[pml4Index].Fields.Valid == 0)
    {
        return nullptr;
    }

    pml4Tree = &SharedVpData->Pml4eTrees[pml4Index];
    if (pml4Tree->PdptEntries[pdptIndex].Fields.Valid == 0)
    {
        return nullptr;
    }

    return &pml4Tree->PdEntries[pdptIndex][pdIndex];
}

static
PNPT_TEST_REGION
SvFindNptTestRegion (
    _In_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ UINT64 GuestPhysicalAddress
    )
{
    UINT64 regionBase = GuestPhysicalAddress & ~((1ull << 21) - 1);
    for (ULONG index = 0; index < RTL_NUMBER_OF(SharedVpData->NptTestRegions); ++index)
    {
        PNPT_TEST_REGION region = &SharedVpData->NptTestRegions[index];
        if (region->State != NPT_TEST_REGION_FREE && region->Base == regionBase)
        {
            return region;
        }
    }
    return nullptr;
}

PNPT_TEST_PAGE
SvFindNptTestPage (
    _In_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ UINT64 GuestPhysicalAddress
    )
{
    for (ULONG index = 0; index < RTL_NUMBER_OF(SharedVpData->NptTestPages); ++index)
    {
        PNPT_TEST_PAGE page = &SharedVpData->NptTestPages[index];
        if (InterlockedCompareExchange(&page->State, 0, 0) != NPT_TEST_STATE_UNREGISTERED &&
            static_cast<UINT64>(InterlockedCompareExchange64(&page->Gpa, 0, 0)) ==
                GuestPhysicalAddress)
        {
            return page;
        }
    }
    return nullptr;
}

BOOLEAN
SvRegisterNptTestPage (
    _Inout_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ UINT64 GuestPhysicalAddress
    )
{
    if ((GuestPhysicalAddress & (PAGE_SIZE - 1)) != 0 ||
        InterlockedCompareExchange(&SharedVpData->NptTestControlLock, 1, 0) != 0)
    {
        return FALSE;
    }

    if (SharedVpData->DebugRegion.State != NPT_TEST_REGION_FREE &&
        SharedVpData->DebugRegion.Base ==
            (GuestPhysicalAddress & ~((1ull << 21) - 1)))
    {
        InterlockedExchange(&SharedVpData->NptTestControlLock, 0);
        return FALSE;
    }
    PNPT_TEST_REGION region = SvFindNptTestRegion(SharedVpData, GuestPhysicalAddress);
    PNPT_TEST_PAGE freePage = nullptr;
    BOOLEAN hasFreeRegion = FALSE;
    for (ULONG index = 0; index < RTL_NUMBER_OF(SharedVpData->NptTestRegions); ++index)
    {
        if (SharedVpData->NptTestRegions[index].State == NPT_TEST_REGION_FREE)
        {
            hasFreeRegion = TRUE;
        }
    }
    for (ULONG index = 0; index < RTL_NUMBER_OF(SharedVpData->NptTestPages); ++index)
    {
        PNPT_TEST_PAGE page = &SharedVpData->NptTestPages[index];
        if (InterlockedCompareExchange(&page->State, 0, 0) == NPT_TEST_STATE_UNREGISTERED)
        {
            freePage = page;
        }
        else if (static_cast<UINT64>(InterlockedCompareExchange64(&page->Gpa, 0, 0)) ==
                     GuestPhysicalAddress)
        {
            InterlockedExchange(&SharedVpData->NptTestControlLock, 0);
            return FALSE;
        }
    }

    UINT64 hostPhysicalAddress = 0;
    BOOLEAN success = freePage != nullptr &&
        (region == nullptr ? hasFreeRegion != FALSE :
         region->State == NPT_TEST_REGION_ACTIVE) &&
        SvTranslateNpt2Mb(SharedVpData, GuestPhysicalAddress, &hostPhysicalAddress) &&
        hostPhysicalAddress == GuestPhysicalAddress;
    if (success != FALSE)
    {
        InterlockedExchange(&freePage->State, NPT_TEST_STATE_BUSY);
        InterlockedExchange64(&freePage->Gpa, static_cast<LONG64>(GuestPhysicalAddress));
        InterlockedExchange(&freePage->State, NPT_TEST_STATE_REGISTERED);
    }
    InterlockedExchange(&SharedVpData->NptTestControlLock, 0);
    return success;
}

BOOLEAN
SvSetNptTestPageMapping (
    _Inout_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ UINT64 GuestPhysicalAddress,
    _In_ BOOLEAN MapToTestPage
    )
{
    if (InterlockedCompareExchange(&SharedVpData->NptTestControlLock, 1, 0) != 0)
    {
        return FALSE;
    }

    PNPT_TEST_PAGE page = SvFindNptTestPage(SharedVpData, GuestPhysicalAddress);
    if (page == nullptr)
    {
        InterlockedExchange(&SharedVpData->NptTestControlLock, 0);
        return MapToTestPage == FALSE;
    }
    LONG originalState = MapToTestPage ? NPT_TEST_STATE_REGISTERED :
        InterlockedCompareExchange(&page->State, 0, 0);
    if (originalState != NPT_TEST_STATE_REGISTERED &&
        originalState != NPT_TEST_STATE_MAPPED)
    {
        InterlockedExchange(&SharedVpData->NptTestControlLock, 0);
        return FALSE;
    }
    if (MapToTestPage == FALSE && originalState == NPT_TEST_STATE_MAPPED &&
        InterlockedCompareExchange(&page->WatchState,
                                   NPT_TEST_WATCH_UNARMED,
                                   NPT_TEST_WATCH_UNARMED) != NPT_TEST_WATCH_UNARMED)
    {
        InterlockedExchange(&SharedVpData->NptTestControlLock, 0);
        return FALSE;
    }
    if (InterlockedCompareExchange(&page->State, NPT_TEST_STATE_BUSY,
                                   originalState) != originalState)
    {
        InterlockedExchange(&SharedVpData->NptTestControlLock, 0);
        return FALSE;
    }

    BOOLEAN success = FALSE;
    if (MapToTestPage == FALSE && originalState == NPT_TEST_STATE_REGISTERED)
    {
        success = TRUE;
    }
    else
    {
        PD_ENTRY_2MB* pde = SvGetNptPde(SharedVpData, GuestPhysicalAddress);
        PNPT_TEST_REGION region = SvFindNptTestRegion(SharedVpData, GuestPhysicalAddress);
        ULONG pteIndex = static_cast<ULONG>((GuestPhysicalAddress >> PAGE_SHIFT) & 0x1ff);
        if (pde != nullptr && page->BackingPhysicalAddress != 0)
        {
            if (region == nullptr && MapToTestPage != FALSE &&
                pde->Fields.Valid != 0 && pde->Fields.LargePage != 0)
            {
                for (ULONG index = 0; index < RTL_NUMBER_OF(SharedVpData->NptTestRegions); ++index)
                {
                    if (SharedVpData->NptTestRegions[index].State == NPT_TEST_REGION_FREE)
                    {
                        region = &SharedVpData->NptTestRegions[index];
                        break;
                    }
                }
                if (region != nullptr && region->PtesPhysicalAddress != 0 &&
                    (pde->Fields.PageFrameNumber << 21) ==
                        (GuestPhysicalAddress & ~((1ull << 21) - 1)))
                {
                    UINT64 originalPde = pde->AsUInt64;
                    UINT64 hostRegionBase = pde->Fields.PageFrameNumber << 21;
                    UINT64 flags = originalPde & (0x7full | (1ull << 8) | (1ull << 63));
                    if ((originalPde & (1ull << 12)) != 0)
                    {
                        flags |= (1ull << 7);
                    }
                    for (ULONG index = 0; index < 512; ++index)
                    {
                        UINT64 hostPage = hostRegionBase +
                            (static_cast<UINT64>(index) << PAGE_SHIFT);
                        region->Ptes[index] =
                            (hostPage & 0x000ffffffffff000ull) | flags;
                    }
                    region->Ptes[pteIndex] =
                        (page->BackingPhysicalAddress & 0x000ffffffffff000ull) | flags;
                    region->Base = GuestPhysicalAddress & ~((1ull << 21) - 1);
                    region->OriginalPde = originalPde;
                    region->MappedPageCount = 1;
                    region->State = NPT_TEST_REGION_ACTIVE;
                    KeMemoryBarrier();
                    UINT64 pdeValue =
                        (region->PtesPhysicalAddress & 0x000ffffffffff000ull) | 0x7;
                    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&pde->AsUInt64),
                                          static_cast<LONG64>(pdeValue));
                    KeMemoryBarrier();
                    success = TRUE;
                }
            }
            else if (region != nullptr && region->State == NPT_TEST_REGION_ACTIVE &&
                     (pde->AsUInt64 & 0x000ffffffffff000ull) ==
                         region->PtesPhysicalAddress &&
                     (pde->AsUInt64 & (1ull << 7)) == 0)
            {
                UINT64 originalPde = region->OriginalPde;
                PD_ENTRY_2MB oldEntry = {};
                oldEntry.AsUInt64 = originalPde;
                UINT64 hostRegionBase = oldEntry.Fields.PageFrameNumber << 21;
                UINT64 flags = originalPde & (0x7full | (1ull << 8) | (1ull << 63));
                if ((originalPde & (1ull << 12)) != 0)
                {
                    flags |= (1ull << 7);
                }
                UINT64 identityPte =
                    ((hostRegionBase + (static_cast<UINT64>(pteIndex) << PAGE_SHIFT)) &
                     0x000ffffffffff000ull) | flags;
                UINT64 currentPte = region->Ptes[pteIndex];
                if (MapToTestPage != FALSE &&
                    ((currentPte ^ identityPte) & ~0x60ull) == 0)
                {
                    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(
                        &region->Ptes[pteIndex]),
                        static_cast<LONG64>((page->BackingPhysicalAddress &
                                             0x000ffffffffff000ull) | flags));
                    ++region->MappedPageCount;
                    success = TRUE;
                }
                else if (MapToTestPage == FALSE && region->MappedPageCount > 0 &&
                         (currentPte & 0x000ffffffffff000ull) ==
                             page->BackingPhysicalAddress)
                {
                    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(
                        &region->Ptes[pteIndex]), static_cast<LONG64>(identityPte));
                    KeMemoryBarrier();
                    if (--region->MappedPageCount == 0)
                    {
                        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(
                            &pde->AsUInt64), static_cast<LONG64>(originalPde));
                        KeMemoryBarrier();
                        region->State = NPT_TEST_REGION_RETIRED;
                    }
                    success = TRUE;
                }
            }
        }
    }

    if (success != FALSE && MapToTestPage == FALSE)
    {
        InterlockedExchange64(&page->Gpa, 0);
        InterlockedExchange(&page->State, NPT_TEST_STATE_UNREGISTERED);
    }
    else
    {
        InterlockedExchange(&page->State, success != FALSE ?
            NPT_TEST_STATE_MAPPED : originalState);
    }
    InterlockedExchange(&SharedVpData->NptTestControlLock, 0);
    return success;
}

BOOLEAN
SvReleaseNptTestRegion (
    _Inout_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ UINT64 GuestPhysicalAddress
    )
{
    if (InterlockedCompareExchange(&SharedVpData->NptTestControlLock, 1, 0) != 0)
    {
        return FALSE;
    }

    PNPT_TEST_REGION region = SvFindNptTestRegion(SharedVpData, GuestPhysicalAddress);
    BOOLEAN success = TRUE;
    if (region != nullptr)
    {
        success = region->State == NPT_TEST_REGION_RETIRED &&
                  region->MappedPageCount == 0;
        for (ULONG index = 0; index < RTL_NUMBER_OF(SharedVpData->NptTestPages); ++index)
        {
            PNPT_TEST_PAGE page = &SharedVpData->NptTestPages[index];
            if (InterlockedCompareExchange(&page->State, 0, 0) != NPT_TEST_STATE_UNREGISTERED &&
                (static_cast<UINT64>(InterlockedCompareExchange64(&page->Gpa, 0, 0)) &
                 ~((1ull << 21) - 1)) == region->Base)
            {
                success = FALSE;
            }
        }
        PD_ENTRY_2MB* pde = SvGetNptPde(SharedVpData, region->Base);
        if (pde == nullptr ||
            ((pde->AsUInt64 ^ region->OriginalPde) & ~0x60ull) != 0)
        {
            success = FALSE;
        }
        if (success != FALSE)
        {
            region->State = NPT_TEST_REGION_FREE;
            region->Base = 0;
            region->OriginalPde = 0;
        }
    }
    InterlockedExchange(&SharedVpData->NptTestControlLock, 0);
    return success;
}

volatile LONG64*
SvGetNptTestPte (
    _In_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ UINT64 GuestPhysicalAddress
    )
{
    PNPT_TEST_REGION region = SvFindNptTestRegion(SharedVpData, GuestPhysicalAddress);
    PD_ENTRY_2MB* pde = SvGetNptPde(SharedVpData, GuestPhysicalAddress);
    if (region == nullptr || region->State != NPT_TEST_REGION_ACTIVE ||
        pde == nullptr || (pde->AsUInt64 & (1ull << 7)) != 0 ||
        (pde->AsUInt64 & 0x000ffffffffff000ull) != region->PtesPhysicalAddress)
    {
        return nullptr;
    }

    ULONG pteIndex = static_cast<ULONG>((GuestPhysicalAddress >> PAGE_SHIFT) & 0x1ff);
    return reinterpret_cast<volatile LONG64*>(&region->Ptes[pteIndex]);
}

BOOLEAN
SvArmNptTestWriteWatch (
    _Inout_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ UINT64 GuestPhysicalAddress
    )
{
    PNPT_TEST_PAGE page = SvFindNptTestPage(SharedVpData, GuestPhysicalAddress);
    if (page == nullptr ||
        GuestPhysicalAddress != static_cast<UINT64>(InterlockedCompareExchange64(
            &page->Gpa, 0, 0)) ||
        InterlockedCompareExchange(&page->State,
                                   NPT_TEST_STATE_MAPPED,
                                   NPT_TEST_STATE_MAPPED) != NPT_TEST_STATE_MAPPED ||
        InterlockedCompareExchange(&page->WatchState,
                                   NPT_TEST_WATCH_BUSY,
                                   NPT_TEST_WATCH_UNARMED) != NPT_TEST_WATCH_UNARMED)
    {
        return FALSE;
    }

    volatile LONG64* pte = SvGetNptTestPte(SharedVpData, GuestPhysicalAddress);
    UINT64 pteValue = pte == nullptr ? 0 : static_cast<UINT64>(
        InterlockedCompareExchange64(pte, 0, 0));
    if ((pteValue & 3) != 3 ||
        (pteValue & 0x000ffffffffff000ull) !=
            page->BackingPhysicalAddress)
    {
        InterlockedExchange(&page->WatchState,
                            NPT_TEST_WATCH_UNARMED);
        return FALSE;
    }

    InterlockedExchange64(&page->WriteNpfCount, 0);
    InterlockedExchange64(&page->WriteFaultInfo1, 0);
    InterlockedExchange64(&page->WriteFaultGpa, 0);
    InterlockedExchange64(pte, static_cast<LONG64>(pteValue & ~2ull));
    KeMemoryBarrier();
    InterlockedExchange(&page->WatchState, NPT_TEST_WATCH_ARMED);
    return TRUE;
}

BOOLEAN
SvDisarmNptTestWriteWatch (
    _Inout_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ UINT64 GuestPhysicalAddress
    )
{
    PNPT_TEST_PAGE page = SvFindNptTestPage(SharedVpData, GuestPhysicalAddress);
    if (page == nullptr)
    {
        return TRUE;
    }

    LONG previousState = InterlockedCompareExchange(
        &page->WatchState,
        NPT_TEST_WATCH_BUSY,
        NPT_TEST_WATCH_ARMED);
    if (previousState != NPT_TEST_WATCH_ARMED)
    {
        previousState = InterlockedCompareExchange(
            &page->WatchState,
            NPT_TEST_WATCH_BUSY,
            NPT_TEST_WATCH_FIRED);
        if (previousState != NPT_TEST_WATCH_FIRED)
        {
            return previousState == NPT_TEST_WATCH_UNARMED;
        }
    }

    if (GuestPhysicalAddress != static_cast<UINT64>(InterlockedCompareExchange64(
            &page->Gpa, 0, 0)) ||
        InterlockedCompareExchange(&page->State,
                                   NPT_TEST_STATE_MAPPED,
                                   NPT_TEST_STATE_MAPPED) != NPT_TEST_STATE_MAPPED)
    {
        InterlockedExchange(&page->WatchState, previousState);
        return FALSE;
    }

    volatile LONG64* pte = SvGetNptTestPte(SharedVpData, GuestPhysicalAddress);
    UINT64 pteValue = pte == nullptr ? 0 : static_cast<UINT64>(
        InterlockedCompareExchange64(pte, 0, 0));
    if ((pteValue & 1) == 0 ||
        (pteValue & 0x000ffffffffff000ull) !=
            page->BackingPhysicalAddress)
    {
        InterlockedExchange(&page->WatchState, previousState);
        return FALSE;
    }

    InterlockedExchange64(pte, static_cast<LONG64>(pteValue | 2ull));
    KeMemoryBarrier();
    InterlockedExchange(&page->WatchState,
                        NPT_TEST_WATCH_UNARMED);
    return TRUE;
}


BOOLEAN
SvInstallNptDebugHook(
    _Inout_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ ULONG ProcessId,
    _In_ UINT64 GuestPhysicalAddress,
    _In_ ULONG PatchOffset,
    _In_reads_bytes_(PatchLength) const UCHAR* PatchBytes,
    _In_ ULONG PatchLength,
    _Out_opt_ PVOID Info
    )
{
    UNREFERENCED_PARAMETER(Info);
    if (SharedVpData == nullptr || PatchBytes == nullptr ||
        (GuestPhysicalAddress & (PAGE_SIZE - 1)) != 0 ||
        PatchLength == 0 || PatchLength > 16 ||
        PatchOffset >= PAGE_SIZE || PatchLength > PAGE_SIZE - PatchOffset ||
        ProcessId == 0 ||
        InterlockedCompareExchange(&SharedVpData->DebugHook.State, 2, 0) != 0)
        return FALSE;

    PNPT_DEBUG_REGION region = &SharedVpData->DebugRegion;
    PNPT_DEBUG_HOOK hook = &SharedVpData->DebugHook;
    UINT64 originalHostPhysicalAddress = 0;
    BOOLEAN success = FALSE;
    if (region->State != NPT_TEST_REGION_FREE ||
        region->PtesPhysicalAddress == 0 || hook->ShadowHostPhysicalAddress == 0 ||
        !SvTranslateNpt2Mb(SharedVpData, GuestPhysicalAddress,
                            &originalHostPhysicalAddress) ||
        originalHostPhysicalAddress != GuestPhysicalAddress)
        goto Exit;

    {
        PHYSICAL_ADDRESS originalPa = {};
        originalPa.QuadPart = originalHostPhysicalAddress;
        PVOID originalPage = MmMapIoSpace(originalPa, PAGE_SIZE, MmCached);
        if (originalPage == nullptr)
            goto Exit;
        RtlCopyMemory(hook->ShadowPage, originalPage, PAGE_SIZE);
        MmUnmapIoSpace(originalPage, PAGE_SIZE);
    }
    RtlCopyMemory(hook->ShadowPage + PatchOffset, PatchBytes, PatchLength);

    if (InterlockedCompareExchange(&SharedVpData->NptTestControlLock, 1, 0) != 0)
        goto Exit;
    PD_ENTRY_2MB* pde = SvGetNptPde(SharedVpData, GuestPhysicalAddress);
    if (region->State == NPT_TEST_REGION_FREE && pde != nullptr &&
        pde->Fields.Valid != 0 && pde->Fields.LargePage != 0 &&
        (pde->Fields.PageFrameNumber << 21) ==
            (GuestPhysicalAddress & ~((1ull << 21) - 1)))
    {
        UINT64 originalPde = pde->AsUInt64;
        UINT64 hostRegionBase = pde->Fields.PageFrameNumber << 21;
        UINT64 flags = originalPde & (0x7full | (1ull << 8) | (1ull << 63));
        if ((originalPde & (1ull << 12)) != 0)
            flags |= (1ull << 7);
        for (ULONG index = 0; index < 512; ++index)
        {
            UINT64 hostPage = hostRegionBase +
                (static_cast<UINT64>(index) << PAGE_SHIFT);
            region->Ptes[index] =
                (hostPage & 0x000ffffffffff000ull) | flags;
        }
        region->Ptes[(GuestPhysicalAddress >> PAGE_SHIFT) & 0x1ff] =
            (hook->ShadowHostPhysicalAddress & 0x000ffffffffff000ull) | flags;
        region->Base = GuestPhysicalAddress & ~((1ull << 21) - 1);
        region->OriginalPde = originalPde;
        region->State = NPT_TEST_REGION_ACTIVE;
        hook->ProcessId = ProcessId;
        hook->PatchOffset = PatchOffset;
        hook->PatchLength = PatchLength;
        hook->GuestPhysicalAddress = GuestPhysicalAddress;
        hook->OriginalHostPhysicalAddress = originalHostPhysicalAddress;
        KeMemoryBarrier();
        UINT64 pdeValue = (region->PtesPhysicalAddress & 0x000ffffffffff000ull) |
                          (originalPde & (0x7full | (1ull << 8) | (1ull << 63)));
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&pde->AsUInt64),
                              static_cast<LONG64>(pdeValue));
        KeMemoryBarrier();
        success = TRUE;
    }
    InterlockedExchange(&SharedVpData->NptTestControlLock, 0);

Exit:
    InterlockedExchange(&hook->State, success ? 1 : 0);
    return success;
}

BOOLEAN
SvInstallNptExecProbe(
    _Inout_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ ULONG ProcessId,
    _In_ UINT64 GuestPhysicalAddress,
    _In_ UINT64 EntryAddress,
    _In_ UINT64 MirrorAddress,
    _In_ UINT64 RedirectAddress
    )
{
    if (SharedVpData == nullptr || ProcessId == 0 || GuestPhysicalAddress == 0 ||
        (GuestPhysicalAddress & (PAGE_SIZE - 1)) != 0 ||
        (EntryAddress & ~(static_cast<UINT64>(PAGE_SIZE) - 1)) != EntryAddress ||
        (MirrorAddress & (PAGE_SIZE - 1)) != 0 ||
        RedirectAddress == 0 ||
        InterlockedCompareExchange(&SharedVpData->DebugHook.State, 2, 0) != 0)
        return FALSE;

    PNPT_DEBUG_REGION region = &SharedVpData->DebugRegion;
    PNPT_DEBUG_HOOK hook = &SharedVpData->DebugHook;
    UINT64 originalHpa = 0;
    BOOLEAN success = FALSE;
    if (region->State != NPT_TEST_REGION_FREE ||
        region->PtesPhysicalAddress == 0 ||
        !SvTranslateNpt2Mb(SharedVpData, GuestPhysicalAddress, &originalHpa) ||
        originalHpa != GuestPhysicalAddress ||
        InterlockedCompareExchange(&SharedVpData->NptTestControlLock, 1, 0) != 0)
        goto Exit;

    {
        PD_ENTRY_2MB* pde = SvGetNptPde(SharedVpData, GuestPhysicalAddress);
        if (pde != nullptr && pde->Fields.Valid != 0 && pde->Fields.LargePage != 0 &&
            (pde->Fields.PageFrameNumber << 21) ==
                (GuestPhysicalAddress & ~((1ull << 21) - 1)))
        {
            UINT64 originalPde = pde->AsUInt64;
            UINT64 hostBase = pde->Fields.PageFrameNumber << 21;
            UINT64 flags = originalPde & (0x7full | (1ull << 8) | (1ull << 63));
            if ((originalPde & (1ull << 12)) != 0)
                flags |= (1ull << 7);
            for (ULONG index = 0; index < 512; ++index)
                region->Ptes[index] =
                    ((hostBase + (static_cast<UINT64>(index) << PAGE_SHIFT)) &
                     0x000ffffffffff000ull) | flags;
            region->Ptes[(GuestPhysicalAddress >> PAGE_SHIFT) & 0x1ff] |= 1ull << 63;
            region->Base = GuestPhysicalAddress & ~((1ull << 21) - 1);
            region->OriginalPde = originalPde;
            region->State = NPT_TEST_REGION_ACTIVE;
            hook->ProcessId = ProcessId;
            hook->GuestPhysicalAddress = GuestPhysicalAddress;
            hook->OriginalHostPhysicalAddress = originalHpa;
            hook->EntryAddress = EntryAddress;
            hook->MirrorAddress = MirrorAddress;
            hook->RedirectAddress = RedirectAddress;
            hook->ExecuteProbe = 1;
            KeMemoryBarrier();
            UINT64 pdeValue = (region->PtesPhysicalAddress & 0x000ffffffffff000ull) |
                              (originalPde & (0x7full | (1ull << 8) | (1ull << 63)));
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&pde->AsUInt64),
                                  static_cast<LONG64>(pdeValue));
            KeMemoryBarrier();
            success = TRUE;
        }
    }
    InterlockedExchange(&SharedVpData->NptTestControlLock, 0);
Exit:
    InterlockedExchange(&hook->State, success ? 1 : 0);
    return success;
}

BOOLEAN
SvHandleNptExecProbe(
    _In_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ UINT64 GuestPhysicalAddress,
    _In_ UINT64 ExitInfo1,
    _Inout_ PUINT64 GuestRip
    )
{
    PNPT_DEBUG_HOOK hook = &SharedVpData->DebugHook;
    if (hook->ExecuteProbe != 1 ||
        (GuestPhysicalAddress & ~(static_cast<UINT64>(PAGE_SIZE) - 1)) !=
            hook->GuestPhysicalAddress ||
        (ExitInfo1 & 0x11ull) != 0x11ull || (ExitInfo1 & 0x2ull) != 0)
        return FALSE;
    if (hook->State != 1)
        return TRUE;
    if ((*GuestRip & ~(static_cast<UINT64>(PAGE_SIZE) - 1)) != hook->EntryAddress)
        return FALSE;
    *GuestRip = *GuestRip == hook->EntryAddress ?
        hook->RedirectAddress :
        hook->MirrorAddress + (*GuestRip & (PAGE_SIZE - 1));
    return TRUE;
}

BOOLEAN
SvRemoveNptDebugHook(
    _Inout_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _Out_opt_ PVOID Info
    )
{
    UNREFERENCED_PARAMETER(Info);
    if (SharedVpData == nullptr ||
        InterlockedCompareExchange(&SharedVpData->DebugHook.State, 2, 1) != 1)
        return FALSE;

    PNPT_DEBUG_REGION region = &SharedVpData->DebugRegion;
    BOOLEAN success = FALSE;
    if (InterlockedCompareExchange(&SharedVpData->NptTestControlLock, 1, 0) != 0)
        goto Exit;
    PD_ENTRY_2MB* pde = SvGetNptPde(
        SharedVpData, SharedVpData->DebugHook.GuestPhysicalAddress);
    if (region->State == NPT_TEST_REGION_ACTIVE && pde != nullptr &&
        (pde->AsUInt64 & 0x000ffffffffff000ull) == region->PtesPhysicalAddress &&
        (pde->AsUInt64 & (1ull << 7)) == 0)
    {
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&pde->AsUInt64),
                              static_cast<LONG64>(region->OriginalPde));
        KeMemoryBarrier();
        region->State = NPT_TEST_REGION_RETIRED;
        success = TRUE;
    }
    InterlockedExchange(&SharedVpData->NptTestControlLock, 0);

Exit:
    InterlockedExchange(&SharedVpData->DebugHook.State, success ? 0 : 1);
    return success;
}

BOOLEAN
SvReleaseNptDebugHookRegion(
    _Inout_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData
    )
{
    if (SharedVpData == nullptr ||
        InterlockedCompareExchange(&SharedVpData->NptTestControlLock, 1, 0) != 0)
        return FALSE;
    PNPT_DEBUG_REGION region = &SharedVpData->DebugRegion;
    PD_ENTRY_2MB* pde = SvGetNptPde(SharedVpData, region->Base);
    BOOLEAN success = region->State == NPT_TEST_REGION_RETIRED &&
        SharedVpData->DebugHook.State == 0 && pde != nullptr &&
        ((pde->AsUInt64 ^ region->OriginalPde) & ~0x60ull) == 0;
    if (success)
    {
        region->Base = 0;
        region->OriginalPde = 0;
        region->State = NPT_TEST_REGION_FREE;
        SharedVpData->DebugHook.GuestPhysicalAddress = 0;
        SharedVpData->DebugHook.OriginalHostPhysicalAddress = 0;
        SharedVpData->DebugHook.ExecuteProbe = 0;
        SharedVpData->DebugHook.EntryAddress = 0;
        SharedVpData->DebugHook.MirrorAddress = 0;
        SharedVpData->DebugHook.RedirectAddress = 0;
    }
    InterlockedExchange(&SharedVpData->NptTestControlLock, 0);
    return success;
}