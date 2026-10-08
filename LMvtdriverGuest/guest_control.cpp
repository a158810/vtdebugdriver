#include <Windows.h>
#include <intrin.h>
#include <cstdio>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include "../lmv_control_shared.h"
#include "../ki_session_lifecycle.hpp"
#include "exec_relocator.hpp"
#include "ki_replay.hpp"
#include "ki_stub_plan.hpp"
#include "../LMvtdriverDebugger/src/zydis/Zydis.h"

static HANDLE OpenGuestDevice(DWORD shareMode = 0)
{
    return CreateFileW(L"\\\\.\\LMvtdriverGuest", GENERIC_READ | GENERIC_WRITE,
                       shareMode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static int RunKiPageRelocationAnalysis()
{
    HANDLE device = OpenGuestDevice();
    if (device == INVALID_HANDLE_VALUE) { std::printf("guest ki-page-reloc: FAIL open=%lu\n", GetLastError()); return 5; }
    LMV_GUEST_KI_PAGE_PREFLIGHT snapshot = {};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_KI_PAGE_PREFLIGHT), nullptr, 0, &snapshot, sizeof(snapshot), &returned, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(device);
    if (!ok || returned != sizeof(snapshot) || snapshot.Status != 0 || snapshot.BytesCopied != sizeof(snapshot.PageBytes) || snapshot.PageFunctionRecordTruncated || snapshot.PageFunctionRecordCount != snapshot.SamePageFunctionCount || snapshot.PageFunctionRecordCount > LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS) {
        std::printf("guest ki-page-reloc: FAIL status=%08lx win32=%lu records=%lu\n", snapshot.Status, error, snapshot.PageFunctionRecordCount); return 5;
    }
    ZydisDecoder decoder = {};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) return 5;
    const ULONGLONG page = snapshot.PageAddress, pageEnd = page + sizeof(snapshot.PageBytes);
    unsigned decoded=0, relativeData=0, relativeControl=0, unresolved=0, decodeFailures=0, externalTargets=0, crossPage=0, printed=0;
    for (unsigned index=0; index<snapshot.PageFunctionRecordCount; ++index) {
        const auto& record=snapshot.PageFunctionRecords[index];
        if (record.BeginAddress < page || record.BeginAddress >= pageEnd || record.EndAddress <= record.BeginAddress) { ++unresolved; std::printf("guest ki-page-reloc-function: index=%u role=%s REVIEW boundary=%llx:%llx\n", index, record.IsTarget ? "ki" : "other", record.BeginAddress, record.EndAddress); continue; }
        const bool functionCrossesPage = record.EndAddress > pageEnd;
        if (functionCrossesPage) ++crossPage;
        const ULONGLONG limit=record.EndAddress < pageEnd ? record.EndAddress : pageEnd;
        ULONGLONG address=record.BeginAddress; unsigned functionInstructions=0, functionIssues=functionCrossesPage ? 1u : 0u;
        while(address<limit) {
            ZydisDecodedInstruction instruction={}; ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]={};
            const auto offset=static_cast<size_t>(address-page);
            if(!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder,snapshot.PageBytes+offset,sizeof(snapshot.PageBytes)-offset,&instruction,operands)) || instruction.length==0 || address+instruction.length>limit) { ++decodeFailures; ++unresolved; ++functionIssues; if(printed++<40) std::printf("guest ki-page-reloc-issue: index=%u address=%llx reason=decode-or-boundary\n",index,address); break; }
            ++decoded; ++functionInstructions;
            for(unsigned oi=0;oi<instruction.operand_count;++oi) {
                const auto& operand=operands[oi]; const bool data=operand.type==ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base==ZYDIS_REGISTER_RIP; const bool control=operand.type==ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative; if(!data&&!control) continue;
                if(data) ++relativeData; if(control) ++relativeControl; ZyanU64 target=0; const bool resolved=ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction,&operand,address,&target)); const bool internal=resolved && target>=page && target<pageEnd;
                if(!resolved) { ++unresolved; ++functionIssues; } else if(control&&!internal) { ++externalTargets; ++functionIssues; }
                if(printed++<40) std::printf("guest ki-page-reloc-site: index=%u address=%llx kind=%s target=%llx action=%s\n",index,address,data?"rip-data":"relative-control",target,!resolved?"unresolved":data?"preserve-original":internal?"mirror-target":"external-review");
            }
            address+=instruction.length;
        }
        std::printf("guest ki-page-reloc-function: index=%u role=%s status=%s start=%llx end=%llx instructions=%u issues=%u\n",index,record.IsTarget?"ki":"other",functionIssues==0 ? "PASS" : "REVIEW",record.BeginAddress,record.EndAddress,functionInstructions,functionIssues);
    }
    std::printf("guest ki-page-reloc: REVIEW page=%llx records=%lu decoded=%u rip-data=%u relative-control=%u unresolved=%u decode-failures=%u external-targets=%u cross-page=%u install=0 execute=0\n",page,snapshot.PageFunctionRecordCount,decoded,relativeData,relativeControl,unresolved,decodeFailures,externalTargets,crossPage);
    return 0;
}

static bool EncodeMirrorDisplacement(ULONGLONG target, ULONGLONG instructionEnd,
                                     unsigned bits, unsigned char* output)
{
    if (bits != 8 && bits != 32)
        return false;
    const bool positive = target >= instructionEnd;
    const ULONGLONG distance = positive ? target - instructionEnd : instructionEnd - target;
    const ULONGLONG maximum = bits == 8 ? (positive ? 127ull : 128ull) :
        (positive ? static_cast<ULONGLONG>(INT32_MAX) : static_cast<ULONGLONG>(INT32_MAX) + 1ull);
    if (distance > maximum)
        return false;
    const std::int64_t displacement = positive ? static_cast<std::int64_t>(distance) :
        -static_cast<std::int64_t>(distance);
    if (bits == 8)
    {
        const std::int8_t value = static_cast<std::int8_t>(displacement);
        std::memcpy(output, &value, sizeof(value));
    }
    else
    {
        const std::int32_t value = static_cast<std::int32_t>(displacement);
        std::memcpy(output, &value, sizeof(value));
    }
    return true;
}

struct KiPageByteDifference
{
    unsigned Count;
    unsigned FirstOffset;
    unsigned char Expected;
    unsigned char Observed;
};

static KiPageByteDifference CompareKiPageBytes(const unsigned char* expected,
                                              const unsigned char* observed, size_t length)
{
    KiPageByteDifference result = {};
    result.FirstOffset = static_cast<unsigned>(length);
    for (size_t offset = 0; offset < length; ++offset)
    {
        if (expected[offset] == observed[offset])
            continue;
        if (result.Count == 0)
        {
            result.FirstOffset = static_cast<unsigned>(offset);
            result.Expected = expected[offset];
            result.Observed = observed[offset];
        }
        ++result.Count;
    }
    return result;
}

static void PrintKiPageByteDifferences(const char* page,
                                       const unsigned char* disk,
                                       const unsigned char* guest, size_t length)
{
    unsigned printed = 0;
    for (size_t offset = 0; offset < length && printed < 8; ++offset)
    {
        if (disk[offset] == guest[offset])
            continue;
        std::printf("guest ki-page-mirror-byte: page=%s offset=%04zx disk=%02x guest=%02x\n",
            page, offset, static_cast<unsigned>(disk[offset]),
            static_cast<unsigned>(guest[offset]));
        ++printed;
    }
}

static int RunKiPageMirrorSelftest()
{
    unsigned char data[4] = {};
    bool pass = EncodeMirrorDisplacement(0x101020, 0x101010, 8, data) &&
        data[0] == 0x10;
    pass = EncodeMirrorDisplacement(0x100120, 0x101010, 32, data) && pass;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, data, sizeof(displacement));
    pass = displacement == -0xEF0 && pass;
    pass = !EncodeMirrorDisplacement(0x100120, 0x101010, 8, data) && pass;
    pass = !EncodeMirrorDisplacement(0x1000000000ull, 0x101010, 32, data) && pass;
    pass = EncodeMirrorDisplacement(0x101038, 0x100FE7, 8, data) &&
        data[0] == 0x51 && pass;
    const unsigned char expected[] = { 0x11, 0x22, 0x33, 0x44 };
    const unsigned char observed[] = { 0x11, 0x20, 0x33, 0x40 };
    const auto differences = CompareKiPageBytes(expected, observed, sizeof(expected));
    pass = differences.Count == 2 && differences.FirstOffset == 1 &&
        differences.Expected == 0x22 && differences.Observed == 0x20 && pass;
    std::printf("guest ki-page-mirror-selftest: %s internal-rel8=1 external-rel32=1 short-range-rejected=1 far-range-rejected=1 cross-page-rel8=1 byte-diff=1\n",
        pass ? "PASS" : "FAIL");
    return pass ? 0 : 5;
}

static int RunKiPageMirrorPlan()
{
    HANDLE device = OpenGuestDevice();
    if (device == INVALID_HANDLE_VALUE) { std::printf("guest ki-page-mirror-plan: FAIL open=%lu\n", GetLastError()); return 5; }
    LMV_GUEST_KI_PAGE_PREFLIGHT snapshot = {};
    LMV_GUEST_KI_NEXT_PAGE nextPage = {};
    LMV_GUEST_KI_NEXT_PAGE nextPageAgain = {};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_KI_PAGE_PREFLIGHT), nullptr, 0, &snapshot, sizeof(snapshot), &returned, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    DWORD nextReturned = 0;
    const BOOL nextOk = ok && DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_KI_NEXT_PAGE), nullptr, 0, &nextPage, sizeof(nextPage), &nextReturned, nullptr);
    const DWORD nextError = nextOk ? 0 : GetLastError();
    DWORD nextAgainReturned = 0;
    const BOOL nextAgainOk = nextOk && DeviceIoControl(device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_NEXT_PAGE), nullptr, 0,
        &nextPageAgain, sizeof(nextPageAgain), &nextAgainReturned, nullptr);
    const DWORD nextAgainError = nextAgainOk ? 0 : GetLastError();
    CloseHandle(device);
    if (!ok || returned != sizeof(snapshot) || snapshot.Status != 0 || snapshot.BytesCopied != sizeof(snapshot.PageBytes) || !nextOk || nextReturned != sizeof(nextPage) || nextPage.Status != 0 || nextPage.BytesCopied != sizeof(nextPage.PageBytes) || nextPage.PageAddress != snapshot.PageAddress + 4096 || !nextAgainOk || nextAgainReturned != sizeof(nextPageAgain) || nextPageAgain.Status != 0 || nextPageAgain.BytesCopied != sizeof(nextPageAgain.PageBytes) || nextPageAgain.PageAddress != nextPage.PageAddress || nextPage.PageFunctionRecordTruncated != 0 || nextPage.PageFunctionRecordCount > LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS || snapshot.PageFunctionRecordTruncated != 0 || snapshot.PageFunctionRecordCount > LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS) { std::printf("guest ki-page-mirror-plan: FAIL status=%08lx next-status=%08lx win32=%lu next-win32=%lu repeat-win32=%lu\n", snapshot.Status, nextPage.Status, error, nextError, nextAgainError); return 5; }

    HANDLE file = CreateFileW(L"C:\\Windows\\System32\\ntoskrnl.exe", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { std::printf("guest ki-page-mirror-plan: REVIEW vm-page-only file-open=%lu\n", GetLastError()); return 0; }
    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > 64 * 1024 * 1024) { CloseHandle(file); return 5; }
    std::vector<unsigned char> image(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;
    const BOOL fileRead = ReadFile(file, image.data(), static_cast<DWORD>(image.size()), &bytesRead, nullptr);
    CloseHandle(file);
    if (!fileRead || bytesRead != image.size()) { std::printf("guest ki-page-mirror-plan: REVIEW file-read=%lu\n", GetLastError()); return 0; }
    if (image.size() < sizeof(IMAGE_DOS_HEADER) || snapshot.KernelBase == 0 || snapshot.PageAddress < snapshot.KernelBase || snapshot.PageAddress - snapshot.KernelBase >= snapshot.KernelImageSize) { std::printf("guest ki-page-mirror-plan: REVIEW invalid-image-or-snapshot\n"); return 0; }
    IMAGE_DOS_HEADER dos = {};
    std::memcpy(&dos, image.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || static_cast<size_t>(dos.e_lfanew) > image.size() - sizeof(IMAGE_NT_HEADERS64)) { std::printf("guest ki-page-mirror-plan: REVIEW invalid-dos-header\n"); return 0; }
    IMAGE_NT_HEADERS64 nt = {};
    std::memcpy(&nt, image.data() + dos.e_lfanew, sizeof(nt));
    const size_t sectionBase = static_cast<size_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage != snapshot.KernelImageSize || nt.FileHeader.NumberOfSections == 0 || sectionBase > image.size() || nt.FileHeader.NumberOfSections > (image.size() - sectionBase) / sizeof(IMAGE_SECTION_HEADER)) { std::printf("guest ki-page-mirror-plan: REVIEW invalid-pe-headers\n"); return 0; }
    const unsigned pageRva = static_cast<unsigned>(snapshot.PageAddress - snapshot.KernelBase);
    size_t rawOffset = 0, rawSize = 0;
    for (unsigned index = 0; index < nt.FileHeader.NumberOfSections; ++index) { IMAGE_SECTION_HEADER section = {}; std::memcpy(&section, image.data() + sectionBase + index * sizeof(section), sizeof(section)); const unsigned span = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData); if (pageRva >= section.VirtualAddress && static_cast<ULONGLONG>(pageRva - section.VirtualAddress) < span && pageRva - section.VirtualAddress < section.SizeOfRawData) { const unsigned delta = pageRva - section.VirtualAddress; rawOffset = static_cast<size_t>(section.PointerToRawData) + delta; rawSize = section.SizeOfRawData - delta; break; } }
    if (!rawOffset || rawOffset > image.size() || image.size() - rawOffset < 8192 || rawSize < 8192) { std::printf("guest ki-page-mirror-plan: REVIEW page-rva=%x raw=%zu adjacent-page-unavailable=1\n", pageRva, rawOffset); return 0; }
    const auto firstDifference = CompareKiPageBytes(image.data() + rawOffset, snapshot.PageBytes, 4096);
    const auto nextDifference = CompareKiPageBytes(image.data() + rawOffset + 4096, nextPage.PageBytes, 4096);
    const auto nextDrift = CompareKiPageBytes(nextPage.PageBytes, nextPageAgain.PageBytes, 4096);
    const bool firstMatch = firstDifference.Count == 0;
    const bool nextMatch = nextDifference.Count == 0;
    const bool bytesMatch = firstMatch && nextMatch;
    std::printf("guest ki-page-mirror-bytes: first={differences=%u first-offset=%04x disk=%02x guest=%02x hash=%llx stable=%u} next={differences=%u first-offset=%04x disk=%02x guest=%02x hash=%llx} next-repeat={differences=%u first-offset=%04x}\n",
        firstDifference.Count, firstDifference.FirstOffset, firstDifference.Expected, firstDifference.Observed, snapshot.PageHash,
        (snapshot.Flags & LMV_KI_PREFLIGHT_STABLE) ? 1u : 0u, nextDifference.Count, nextDifference.FirstOffset,
        nextDifference.Expected, nextDifference.Observed, nextPage.PageHash, nextDrift.Count, nextDrift.FirstOffset);
    PrintKiPageByteDifferences("first", image.data() + rawOffset, snapshot.PageBytes, 4096);
    PrintKiPageByteDifferences("next", image.data() + rawOffset + 4096, nextPage.PageBytes, 4096);
    const bool entryPage = snapshot.FunctionBeginAddress >= snapshot.PageAddress && snapshot.FunctionBeginAddress < snapshot.PageAddress + 4096;
    const bool entryFits = snapshot.FunctionEndAddress <= snapshot.PageAddress + 4096;
    bool functionsFit = true;
    for (unsigned index = 0; index < snapshot.PageFunctionRecordCount; ++index) functionsFit = functionsFit && snapshot.PageFunctionRecords[index].BeginAddress >= snapshot.PageAddress && snapshot.PageFunctionRecords[index].EndAddress <= snapshot.PageAddress + 8192;

    std::vector<unsigned char> original(8192);
    std::memcpy(original.data(), snapshot.PageBytes, 4096);
    std::memcpy(original.data() + 4096, nextPage.PageBytes, 4096);
    std::vector<unsigned char> mirror = original;
    const ULONGLONG originalPage = snapshot.PageAddress, mirrorPage = originalPage + 0x100000;
    ZydisDecoder decoder = {};
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    unsigned decoded = 0, patched = 0, internal = 0, external = 0, decodeFailures = 0, addressFailures = 0, rangeFailures = 0, unsupportedWidths = 0, crossPageFunctions = 0;
    unsigned failureDetails = 0;
    unsigned nextFunctionsAnalyzed = 0;
    unsigned beyondMirror = 0;
    const unsigned totalFunctions = snapshot.PageFunctionRecordCount + nextPage.PageFunctionRecordCount;
    for (unsigned fi = 0; fi < totalFunctions; ++fi) {
        const bool fromNextPage = fi >= snapshot.PageFunctionRecordCount;
        const auto& record = fromNextPage ? nextPage.PageFunctionRecords[fi - snapshot.PageFunctionRecordCount] : snapshot.PageFunctionRecords[fi];
        if (fromNextPage && record.BeginAddress < originalPage + 4096) continue;
        if (record.BeginAddress < originalPage || record.BeginAddress >= originalPage + 8192) continue;
        if (fromNextPage) ++nextFunctionsAnalyzed;
        if (record.EndAddress > originalPage + 8192)
        {
            ++beyondMirror;
            std::printf("guest ki-page-mirror-function: page=%s start=%llx end=%llx status=extends-beyond-mirror\n",
                fromNextPage ? "next" : "first", record.BeginAddress, record.EndAddress);
        }
        if (!fromNextPage && record.EndAddress > originalPage + 4096) ++crossPageFunctions;
        ULONGLONG address = record.BeginAddress;
        const ULONGLONG limit = record.EndAddress < originalPage + 8192 ? record.EndAddress : originalPage + 8192;
        while (address < limit) {
            ZydisDecodedInstruction instruction = {};
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT] = {};
            const size_t offset = static_cast<size_t>(address - originalPage);
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, original.data() + offset, original.size() - offset, &instruction, operands)) || instruction.length == 0 || address + instruction.length > limit)
            {
                ++decodeFailures;
                std::printf("guest ki-page-mirror-function: page=%s address=%llx start=%llx end=%llx status=decode-or-boundary\n",
                    fromNextPage ? "next" : "first", address, record.BeginAddress, record.EndAddress);
                break;
            }
            ++decoded;
            std::memcpy(mirror.data() + offset, original.data() + offset, instruction.length);
            for (unsigned oi = 0; oi < instruction.operand_count; ++oi) {
                const auto& operand = operands[oi];
                const bool ripData = operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_RIP;
                const bool relativeControl = operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative;
                if (!ripData && !relativeControl) continue;
                ZyanU64 target = 0;
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target))) { ++addressFailures; continue; }
                const bool targetInternal = relativeControl && target >= originalPage && target < originalPage + 8192;
                if (relativeControl) { if (targetInternal) ++internal; else ++external; }
                const ULONGLONG relocatedTarget = targetInternal ? mirrorPage + (target - originalPage) : target;
                const unsigned bits = relativeControl ? instruction.raw.imm[0].size : instruction.raw.disp.size;
                const unsigned fieldOffset = relativeControl ? instruction.raw.imm[0].offset : instruction.raw.disp.offset;
                if ((bits != 8 && bits != 32) || (!relativeControl && bits != 32)) { ++unsupportedWidths; continue; }
                const unsigned width = bits / 8;
                if (fieldOffset + width > instruction.length) { ++addressFailures; continue; }
                if (!EncodeMirrorDisplacement(relocatedTarget, mirrorPage + offset + instruction.length,
                                              bits, mirror.data() + offset + fieldOffset))
                {
                    ++rangeFailures;
                    if (failureDetails++ < 16)
                    {
                        const long long displacement = static_cast<long long>(relocatedTarget) -
                            static_cast<long long>(mirrorPage + offset + instruction.length);
                        std::printf("guest ki-page-mirror-failure: address=%llx kind=%s bits=%u target=%llx relocated-target=%llx displacement=%lld reason=range\n",
                            address, relativeControl ? "relative-control" : "rip-data", bits,
                            static_cast<ULONGLONG>(target), relocatedTarget, displacement);
                    }
                    continue;
                }
                ++patched;
            }
            address += instruction.length;
        }
    }
    const unsigned failures = decodeFailures + addressFailures + rangeFailures + unsupportedWidths;
    const bool firstPagePlanComplete = firstMatch && nextDrift.Count == 0 && entryPage && entryFits && failures == 0;
    std::printf("guest ki-page-mirror-plan: REVIEW vm-page=%llx page-rva=%x disk-bytes=%s mirror=%llx decoded=%u patched=%u internal-targets=%u external-targets=%u failures=%u decode-failures=%u address-failures=%u range-failures=%u unsupported-widths=%u cross-page-functions=%u entry-page=%u entry-fits=%u functions-fit=%u first-page-plan=%s next-page-functions=%u/%lu next-page-beyond-mirror=%u page-before-entry=unanalyzed installed=0 executed=0\n", snapshot.PageAddress, pageRva, bytesMatch ? "match" : "mismatch", mirrorPage, decoded, patched, internal, external, failures, decodeFailures, addressFailures, rangeFailures, unsupportedWidths, crossPageFunctions, entryPage ? 1 : 0, entryFits ? 1 : 0, functionsFit ? 1 : 0, firstPagePlanComplete ? "complete" : "incomplete", nextFunctionsAnalyzed, nextPage.PageFunctionRecordCount, beyondMirror);
    return 0;
}

struct KiWindowAudit
{
    unsigned Targets;
    unsigned Failures;
    unsigned Boundary;
    unsigned Patched;
    unsigned ControlRangeFailures;
    unsigned RipDataRangeFailures;
    unsigned CallRelays;
    unsigned JumpRelays;
    unsigned ConditionalRelays;
    unsigned UnsupportedControl;
    ULONGLONG AllowedMirrorMin;
    ULONGLONG AllowedMirrorMax;
    unsigned AllowedRangeReferences;
    unsigned AllowedRangeEmpty;
};

static int AnalyzeKiWindow(const std::vector<LMV_GUEST_KI_WINDOW_PAGE>& pages,
    KiWindowAudit* audit = nullptr, ULONGLONG requestedMirrorBegin = 0,
    size_t reservedMirrorPages = 0)
{
    const ULONGLONG begin = pages[0].PageAddress;
    const ULONGLONG end = begin + pages.size() * 4096;
    const ULONGLONG mirrorBegin = requestedMirrorBegin != 0 ?
        requestedMirrorBegin : begin + 0x100000;
    std::vector<unsigned char> original(pages.size() * 4096);
    for (unsigned index = 0; index < pages.size(); ++index)
        std::memcpy(original.data() + index * 4096, pages[index].PageBytes, 4096);
    std::vector<unsigned char> mirror = original;
    std::vector<LMV_KI_PAGE_FUNCTION_RECORD> functions;
    for (const auto& page : pages)
        for (ULONG index = 0; index < page.PageFunctionRecordCount; ++index)
        {
            const auto& record = page.PageFunctionRecords[index];
            bool duplicate = false;
            for (const auto& previous : functions)
                if (record.BeginAddress == previous.BeginAddress &&
                    record.EndAddress == previous.EndAddress &&
                    record.UnwindAddress == previous.UnwindAddress)
                    duplicate = true;
            if (!duplicate) functions.push_back(record);
        }
    ZydisDecoder decoder = {};
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    unsigned decoded = 0, patched = 0, internal = 0, external = 0;
    unsigned failures = 0, boundary = 0, targets = 0;
    unsigned controlRangeFailures = 0, ripDataRangeFailures = 0;
    unsigned callRelays = 0, jumpRelays = 0, conditionalRelays = 0;
    unsigned unsupportedControl = 0, printedFailures = 0;
    ULONGLONG allowedMirrorMin = 0;
    ULONGLONG allowedMirrorMax = UINT64_MAX;
    unsigned allowedRangeReferences = 0;
    bool allowedRangeEmpty = false;
    for (const auto& record : functions)
    {
        if (record.IsTarget) ++targets;
        if (record.BeginAddress < begin || record.EndAddress > end ||
            record.BeginAddress >= record.EndAddress)
        {
            ++boundary;
            std::printf("guest ki-window-function: start=%llx end=%llx status=extends-beyond-window\n",
                record.BeginAddress, record.EndAddress);
            continue;
        }
        ULONGLONG address = record.BeginAddress;
        while (address < record.EndAddress)
        {
            const size_t offset = static_cast<size_t>(address - begin);
            ZydisDecodedInstruction instruction = {};
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT] = {};
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, original.data() + offset,
                original.size() - offset, &instruction, operands)) ||
                instruction.length == 0 || address + instruction.length > record.EndAddress)
            {
                ++failures;
                std::printf("guest ki-window-failure: address=%llx reason=decode-or-boundary\n", address);
                break;
            }
            ++decoded;
            for (unsigned operandIndex = 0; operandIndex < instruction.operand_count; ++operandIndex)
            {
                const auto& operand = operands[operandIndex];
                const bool ripData = operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                    operand.mem.base == ZYDIS_REGISTER_RIP;
                const bool relativeControl = operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                    operand.imm.is_relative;
                if (!ripData && !relativeControl) continue;
                ZyanU64 target = 0;
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target)))
                {
                    ++failures;
                    continue;
                }
                const bool inside = relativeControl && target >= begin && target < end;
                if (relativeControl) { if (inside) ++internal; else ++external; }
                if ((relativeControl && !inside) || ripData)
                {
                    const unsigned rangeBits = relativeControl ? instruction.raw.imm[0].size :
                        instruction.raw.disp.size;
                    if (rangeBits == 8 || rangeBits == 32)
                    {
                        const ULONGLONG siteOffset = static_cast<ULONGLONG>(offset + instruction.length);
                        const ULONGLONG positiveMax = rangeBits == 8 ? 127ull :
                            static_cast<ULONGLONG>(INT32_MAX);
                        const ULONGLONG negativeMax = rangeBits == 8 ? 128ull :
                            static_cast<ULONGLONG>(INT32_MAX) + 1ull;
                        const ULONGLONG targetAddress = static_cast<ULONGLONG>(target);
                        const ULONGLONG lowerMargin = siteOffset + positiveMax;
                        const ULONGLONG referenceMin = targetAddress < lowerMargin ? 0 :
                            targetAddress - lowerMargin;
                        const ULONGLONG upperWithMargin = targetAddress > UINT64_MAX - negativeMax ?
                            UINT64_MAX : targetAddress + negativeMax;
                        const ULONGLONG referenceMax = upperWithMargin < siteOffset ? 0 :
                            upperWithMargin - siteOffset;
                        ++allowedRangeReferences;
                        if (referenceMin > allowedMirrorMin)
                            allowedMirrorMin = referenceMin;
                        if (referenceMax < allowedMirrorMax)
                            allowedMirrorMax = referenceMax;
                        if (allowedMirrorMin > allowedMirrorMax)
                            allowedRangeEmpty = true;
                    }
                }
                const ULONGLONG relocated = inside ? mirrorBegin + (target - begin) : target;
                const unsigned bits = relativeControl ? instruction.raw.imm[0].size : instruction.raw.disp.size;
                const unsigned field = relativeControl ? instruction.raw.imm[0].offset : instruction.raw.disp.offset;
                const bool widthValid = (bits == 8 || bits == 32) &&
                    (!ripData || bits == 32) && field + bits / 8 <= instruction.length;
                const bool fits = widthValid && EncodeMirrorDisplacement(relocated,
                    mirrorBegin + offset + instruction.length, bits,
                    mirror.data() + offset + field);
                if (!fits)
                {
                    ++failures;
                    const char* kind = "unsupported";
                    if (widthValid && ripData)
                    {
                        ++ripDataRangeFailures;
                        kind = "rip-data";
                    }
                    else if (widthValid && relativeControl)
                    {
                        ++controlRangeFailures;
                        if (bits == 32 && instruction.mnemonic == ZYDIS_MNEMONIC_CALL)
                        {
                            ++callRelays;
                            kind = "call-relay-candidate";
                        }
                        else if (bits == 32 && instruction.mnemonic == ZYDIS_MNEMONIC_JMP)
                        {
                            ++jumpRelays;
                            kind = "jump-relay-candidate";
                        }
                        else if (bits == 32 &&
                            instruction.meta.category == ZYDIS_CATEGORY_COND_BR)
                        {
                            ++conditionalRelays;
                            kind = "conditional-relay-candidate";
                        }
                        else
                            ++unsupportedControl;
                    }
                    else if (relativeControl)
                        ++unsupportedControl;
                    if (printedFailures++ < 16)
                        std::printf("guest ki-window-failure: address=%llx target=%llx bits=%u kind=%s reason=%s\n",
                            address, static_cast<ULONGLONG>(target), bits, kind,
                            widthValid ? "range" : "width-or-field");
                }
                else ++patched;
            }
            address += instruction.length;
        }
    }
    if (audit != nullptr)
        *audit = { targets, failures, boundary, patched, controlRangeFailures,
            ripDataRangeFailures, callRelays, jumpRelays, conditionalRelays,
            unsupportedControl, allowedMirrorMin, allowedMirrorMax,
            allowedRangeReferences, allowedRangeEmpty ? 1u : 0u };
    std::printf("guest ki-window-plan: REVIEW begin=%llx end=%llx ki=%llx functions=%zu targets=%u decoded=%u patched=%u internal=%u external=%u failures=%u boundary=%u other-entries=unproven guest-bytes=authoritative installed=0 executed=0\n",
        begin, end, pages[2].RoutineAddress, functions.size(), targets,
        decoded, patched, internal, external, failures, boundary);
    std::printf("guest ki-window-relay-plan: control-range=%u rip-data-range=%u calls=%u jumps=%u conditional=%u unsupported-control=%u installed=0 executed=0\n",
        controlRangeFailures, ripDataRangeFailures, callRelays, jumpRelays,
        conditionalRelays, unsupportedControl);
    const size_t mirrorPages = reservedMirrorPages != 0 ? reservedMirrorPages : pages.size();
    const ULONGLONG mirrorBytes = static_cast<ULONGLONG>(mirrorPages) * 4096ull;
    const ULONGLONG alignedMin = allowedMirrorMin <= UINT64_MAX - 0xfffull ?
        (allowedMirrorMin + 0xfffull) & ~0xfffull : UINT64_MAX;
    const ULONGLONG alignedMax = allowedMirrorMax >= mirrorBytes - 1 ?
        (allowedMirrorMax - (mirrorBytes - 1)) & ~0xfffull : 0;
    const bool rangeUsable = allowedRangeReferences != 0 && !allowedRangeEmpty &&
        alignedMin <= alignedMax;
    std::printf("guest ki-window-mirror-range: references=%u empty=%u usable=%u min=%llx max=%llx aligned-min=%llx aligned-max=%llx window-pages=%zu mirror-pages=%zu installed=0 executed=0\n",
        allowedRangeReferences, allowedRangeEmpty ? 1u : 0u, rangeUsable ? 1u : 0u,
        allowedMirrorMin, allowedMirrorMax, alignedMin, alignedMax, pages.size(), mirrorPages);
    return 0;
}

struct KiEntryPathStats
{
    unsigned Instructions;
    unsigned Internal;
    unsigned OutsideWindow;
    unsigned Indirect;
    unsigned Returns;
    unsigned Failures;
    unsigned OtherFunctions;
    unsigned FallthroughExits;
};

struct KiReachableCall
{
    ULONGLONG Source;
    ULONGLONG Target;
    ULONGLONG ReturnAddress;
};


static int RunKiRealMirrorSelftest()
{
    std::vector<LMV_GUEST_KI_WINDOW_PAGE> pages(6);
    const ULONGLONG begin = 0x10000000ull;
    for (unsigned index = 0; index < pages.size(); ++index)
        pages[index].PageAddress = begin + static_cast<ULONGLONG>(index) * 4096;
    const ULONGLONG entry = pages[2].PageAddress + 0x100;
    const ULONGLONG data = pages[2].PageAddress + 0x200;
    const ULONGLONG external = begin + 0x80000;
    pages[2].RoutineAddress = entry;
    pages[2].PageFunctionRecordCount = 1;
    auto& record = pages[2].PageFunctionRecords[0];
    record.BeginAddress = entry;
    record.EndAddress = entry + 24;
    record.IsTarget = 1;
    unsigned char* code = pages[2].PageBytes + 0x100;
    code[0] = 0x48;
    code[1] = 0x8b;
    code[2] = 0x05;
    const std::int32_t ripDisplacement = static_cast<std::int32_t>(data - (entry + 7));
    std::memcpy(code + 3, &ripDisplacement, sizeof(ripDisplacement));
    code[7] = 0xe8;
    const std::int32_t callDisplacement = static_cast<std::int32_t>(external - (entry + 12));
    std::memcpy(code + 8, &callDisplacement, sizeof(callDisplacement));
    code[12] = 0xe9;
    const std::int32_t jumpDisplacement = static_cast<std::int32_t>(external - (entry + 17));
    std::memcpy(code + 13, &jumpDisplacement, sizeof(jumpDisplacement));
    code[17] = 0x0f;
    code[18] = 0x85;
    const std::int32_t conditionalDisplacement =
        static_cast<std::int32_t>(external - (entry + 23));
    std::memcpy(code + 19, &conditionalDisplacement, sizeof(conditionalDisplacement));
    code[23] = 0xc3;
    KiWindowAudit nearAudit = {};
    KiWindowAudit farAudit = {};
    const int nearStatus = AnalyzeKiWindow(pages, &nearAudit, begin + 0x100000);
    const int farStatus = AnalyzeKiWindow(pages, &farAudit, begin + 0x100000000ull);
    const bool pass = nearStatus == 0 && farStatus == 0 &&
        nearAudit.Targets == 1 && nearAudit.Failures == 0 && nearAudit.Boundary == 0 &&
        nearAudit.AllowedRangeReferences == 4 && nearAudit.AllowedRangeEmpty == 0 &&
        nearAudit.AllowedMirrorMin <= begin + 0x100000 &&
        nearAudit.AllowedMirrorMax >= begin + 0x100000 + 7 * 4096 - 1 &&
        farAudit.AllowedMirrorMax < begin + 0x100000000ull &&
        farAudit.Targets == 1 && farAudit.Failures == 4 &&
        farAudit.RipDataRangeFailures == 1 && farAudit.ControlRangeFailures == 3 &&
        farAudit.CallRelays == 1 && farAudit.JumpRelays == 1 &&
        farAudit.ConditionalRelays == 1 && farAudit.UnsupportedControl == 0;
    std::printf("guest ki-real-mirror-selftest: %s near-failures=%u far-failures=%u call=%u jump=%u conditional=%u rip-data=%u no-install=1\n",
        pass ? "PASS" : "FAIL", nearAudit.Failures, farAudit.Failures,
        farAudit.CallRelays, farAudit.JumpRelays, farAudit.ConditionalRelays,
        farAudit.RipDataRangeFailures);
    return pass ? 0 : 5;
}

static int AnalyzeKiEntryPaths(const std::vector<LMV_GUEST_KI_WINDOW_PAGE>& pages,
    KiEntryPathStats* result = nullptr, std::vector<KiReachableCall>* reachableCalls = nullptr)
{
    const ULONGLONG windowBegin = pages.front().PageAddress;
    const ULONGLONG windowEnd = windowBegin + pages.size() * 4096;
    const ULONGLONG entry = pages[2].RoutineAddress;
    const LMV_KI_PAGE_FUNCTION_RECORD* function = nullptr;
    unsigned matches = 0;
    for (ULONG index = 0; index < pages[2].PageFunctionRecordCount; ++index)
    {
        const auto& record = pages[2].PageFunctionRecords[index];
        if (record.IsTarget && record.BeginAddress == entry)
        {
            ++matches;
            function = &record;
        }
    }
    if (matches != 1 || function->EndAddress > windowEnd ||
        function->BeginAddress < windowBegin || function->EndAddress <= entry)
    {
        std::printf("guest ki-entry-path: REVIEW target-record=%u bounds=unavailable installed=0 executed=0\n", matches);
        return 0;
    }
    std::vector<unsigned char> bytes(pages.size() * 4096);
    for (size_t index = 0; index < pages.size(); ++index)
        std::memcpy(bytes.data() + index * 4096, pages[index].PageBytes, 4096);
    std::vector<unsigned char> boundaries(static_cast<size_t>(function->EndAddress - entry));
    std::vector<ULONGLONG> pending(1, entry);
    ZydisDecoder decoder = {};
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    unsigned instructions = 0, calls = 0, returns = 0, internal = 0;
    unsigned otherFunctions = 0, outsideWindow = 0, indirect = 0;
    unsigned ripData = 0, decodeFailures = 0, overlapping = 0, fallthroughExits = 0;
    while (!pending.empty())
    {
        const ULONGLONG address = pending.back();
        pending.pop_back();
        if (address < entry || address >= function->EndAddress)
        {
            ++fallthroughExits;
            continue;
        }
        const size_t index = static_cast<size_t>(address - entry);
        if (boundaries[index] == 1) continue;
        if (boundaries[index] == 2)
        {
            ++overlapping;
            continue;
        }
        const size_t offset = static_cast<size_t>(address - windowBegin);
        ZydisDecodedInstruction instruction = {};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT] = {};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data() + offset,
                bytes.size() - offset, &instruction, operands)) ||
            instruction.length == 0 || address + instruction.length > function->EndAddress)
        {
            ++decodeFailures;
            std::printf("guest ki-entry-path-issue: at=%llx type=decode-or-boundary\n", address);
            continue;
        }
        bool conflict = false;
        for (unsigned byte = 1; byte < instruction.length; ++byte)
            conflict = conflict || boundaries[index + byte] != 0;
        if (conflict)
        {
            ++overlapping;
            std::printf("guest ki-entry-path-issue: at=%llx type=overlapping-instruction\n", address);
            continue;
        }
        boundaries[index] = 1;
        for (unsigned byte = 1; byte < instruction.length; ++byte)
            boundaries[index + byte] = 2;
        ++instructions;
        const bool call = instruction.meta.category == ZYDIS_CATEGORY_CALL;
        const bool branch = call || instruction.meta.category == ZYDIS_CATEGORY_COND_BR ||
            instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR;
        const bool conditional = instruction.meta.category == ZYDIS_CATEGORY_COND_BR;
        const bool unconditional = instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR;
        if (call) ++calls;
        if (instruction.meta.category == ZYDIS_CATEGORY_RET) { ++returns; continue; }
        if (instruction.meta.category == ZYDIS_CATEGORY_INTERRUPT ||
            instruction.meta.category == ZYDIS_CATEGORY_SYSRET)
        {
            ++indirect;
            continue;
        }
        bool direct = false;
        for (unsigned operandIndex = 0; operandIndex < instruction.operand_count; ++operandIndex)
        {
            const auto& operand = operands[operandIndex];
            if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                operand.mem.base == ZYDIS_REGISTER_RIP) ++ripData;
            if (!branch || operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                !operand.imm.is_relative) continue;
            ZyanU64 target = 0;
            if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target)))
            {
                ++decodeFailures;
                continue;
            }
            direct = true;
            if (target >= entry && target < function->EndAddress)
            {
                ++internal;
                pending.push_back(target);
            }
            else
            {
                if (target >= windowBegin && target < windowEnd) ++otherFunctions;
                else ++outsideWindow;
                if (call && reachableCalls != nullptr)
                    reachableCalls->push_back({ address, target, address + instruction.length });
                if (otherFunctions + outsideWindow <= 16)
                    std::printf("guest ki-entry-path-exit: from=%llx to=%llx kind=%s area=%s\n",
                        address, static_cast<ULONGLONG>(target), call ? "call" : "branch",
                        target >= windowBegin && target < windowEnd ? "other-function" : "outside-window");
            }
        }
        if (branch && !direct)
        {
            ++indirect;
            if (indirect <= 16)
                std::printf("guest ki-entry-path-issue: at=%llx type=indirect-control\n", address);
        }
        if (!unconditional && (!branch || call || conditional))
            pending.push_back(address + instruction.length);
    }
    if (result != nullptr)
        *result = { instructions, internal, outsideWindow, indirect, returns,
            decodeFailures + overlapping, otherFunctions, fallthroughExits };
    std::printf("guest ki-entry-path: REVIEW entry=%llx function=[%llx,%llx) reachable=%u calls=%u returns=%u direct-internal=%u other-function=%u outside-window=%u indirect=%u rip-data=%u decode-failures=%u overlaps=%u fallthrough-exits=%u indirect-targets=unproven exception-edges=unproven installed=0 executed=0\n",
        entry, function->BeginAddress, function->EndAddress, instructions, calls,
        returns, internal, otherFunctions, outsideWindow, indirect, ripData,
        decodeFailures, overlapping, fallthroughExits);
    return 0;
}

static int RunKiEntryPathSelftest()
{
    std::vector<LMV_GUEST_KI_WINDOW_PAGE> pages(6);
    const ULONGLONG begin = 0x10000000;
    for (unsigned index = 0; index < pages.size(); ++index)
        pages[index].PageAddress = begin + index * 4096;
    const ULONGLONG entry = pages[2].PageAddress + 0x100;
    pages[2].RoutineAddress = entry;
    pages[2].PageFunctionRecordCount = 1;
    auto& record = pages[2].PageFunctionRecords[0];
    record.BeginAddress = entry;
    record.EndAddress = entry + 10;
    record.IsTarget = 1;
    auto* code = pages[2].PageBytes + 0x100;
    code[0] = 0x74; code[1] = 0x07;
    code[2] = 0xE8;
    const ULONGLONG outside = begin + pages.size() * 4096 + 0x100;
    const std::int32_t displacement = static_cast<std::int32_t>(outside - (entry + 7));
    std::memcpy(code + 3, &displacement, sizeof(displacement));
    code[7] = 0xFF; code[8] = 0xE0; code[9] = 0xC3;
    KiEntryPathStats stats = {};
    AnalyzeKiEntryPaths(pages, &stats);
    const bool pass = stats.Instructions == 4 && stats.Internal == 1 &&
        stats.OutsideWindow == 1 && stats.Indirect == 1 &&
        stats.Returns == 1 && stats.Failures == 0;
    std::printf("guest ki-entry-path-selftest: %s reachable=%u internal=%u outside=%u indirect=%u returns=%u failures=%u\n",
        pass ? "PASS" : "FAIL", stats.Instructions, stats.Internal,
        stats.OutsideWindow, stats.Indirect, stats.Returns, stats.Failures);
    return pass ? 0 : 5;
}

struct KiTailReview
{
    unsigned Instructions;
    unsigned Patched;
    unsigned Returns;
    unsigned Issues;
};

static KiTailReview ReviewKiReachableTail(
    const std::vector<LMV_GUEST_KI_WINDOW_PAGE>& pages,
    const LMV_GUEST_KI_WINDOW_PAGE& tail, const KiReachableCall& call,
    const LMV_KI_PAGE_FUNCTION_RECORD& function)
{
    KiTailReview review = {};
    const ULONGLONG begin = pages.front().PageAddress;
    const ULONGLONG end = tail.PageAddress + 4096;
    const ULONGLONG mirrorBegin = begin + 0x100000;
    std::vector<unsigned char> original(pages.size() * 4096 + 4096);
    for (size_t index = 0; index < pages.size(); ++index)
        std::memcpy(original.data() + index * 4096, pages[index].PageBytes, 4096);
    std::memcpy(original.data() + pages.size() * 4096, tail.PageBytes, 4096);
    ZydisDecoder decoder = {};
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisDecodedInstruction source = {};
    ZydisDecodedOperand sourceOperands[ZYDIS_MAX_OPERAND_COUNT] = {};
    const size_t sourceOffset = static_cast<size_t>(call.Source - begin);
    bool callRelocates = false;
    if (call.Source >= begin && call.Source < tail.PageAddress &&
        ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, original.data() + sourceOffset,
            original.size() - sourceOffset, &source, sourceOperands)) &&
        source.meta.category == ZYDIS_CATEGORY_CALL &&
        call.Source + source.length == call.ReturnAddress)
    {
        for (unsigned index = 0; index < source.operand_count; ++index)
        {
            if (sourceOperands[index].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                !sourceOperands[index].imm.is_relative) continue;
            ZyanU64 target = 0;
            unsigned char displacement[4] = {};
            callRelocates = ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(
                &source, &sourceOperands[index], call.Source, &target)) &&
                target == call.Target &&
                EncodeMirrorDisplacement(mirrorBegin + (call.Target - begin),
                    mirrorBegin + (call.ReturnAddress - begin),
                    source.raw.imm[0].size, displacement);
        }
    }
    if (!callRelocates) ++review.Issues;
    const ULONGLONG mirrorReturn = mirrorBegin + (call.ReturnAddress - begin);
    std::printf("guest ki-tail-call: source=%llx target=%llx original-return=%llx mirror-return=%llx direct-relocation=%s return-path=unproven\n",
        call.Source, call.Target, call.ReturnAddress, mirrorReturn,
        callRelocates ? "fits" : "unsupported");
    for (ULONGLONG address = function.BeginAddress; address < function.EndAddress;)
    {
        const size_t offset = static_cast<size_t>(address - begin);
        ZydisDecodedInstruction instruction = {};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT] = {};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, original.data() + offset,
                original.size() - offset, &instruction, operands)) ||
            instruction.length == 0 || address + instruction.length > function.EndAddress)
        {
            ++review.Issues;
            std::printf("guest ki-tail-issue: at=%llx kind=decode-or-boundary\n", address);
            break;
        }
        ++review.Instructions;
        if (instruction.meta.category == ZYDIS_CATEGORY_RET) ++review.Returns;
        for (unsigned index = 0; index < instruction.operand_count; ++index)
        {
            const auto& operand = operands[index];
            const bool ripData = operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                operand.mem.base == ZYDIS_REGISTER_RIP;
            const bool relative = operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                operand.imm.is_relative;
            if (!ripData && !relative) continue;
            ZyanU64 target = 0;
            if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand,
                address, &target)))
            {
                ++review.Issues;
                continue;
            }
            const ULONGLONG relocated = relative && target >= begin && target < end ?
                mirrorBegin + (target - begin) : target;
            const unsigned bits = relative ? instruction.raw.imm[0].size : instruction.raw.disp.size;
            unsigned char displacement[4] = {};
            if ((bits != 8 && bits != 32) || (ripData && bits != 32) ||
                !EncodeMirrorDisplacement(relocated, mirrorBegin + offset + instruction.length,
                    bits, displacement))
            {
                ++review.Issues;
                std::printf("guest ki-tail-issue: at=%llx target=%llx kind=relocation\n",
                    address, static_cast<ULONGLONG>(target));
            }
            else ++review.Patched;
        }
        address += instruction.length;
    }
    std::printf("guest ki-tail-plan: REVIEW function=[%llx,%llx) decoded=%u patched=%u returns=%u issues=%u linear-only=1 dynamic-returns=unproven installed=0 executed=0\n",
        function.BeginAddress, function.EndAddress, review.Instructions,
        review.Patched, review.Returns, review.Issues);
    return review;
}

struct KiTailAudit
{
    unsigned Crossing;
    unsigned Reviewed;
    unsigned Issues;
    unsigned Uncovered;
    ULONGLONG ContentHash;
};

static int ReviewKiReachableCalls(const std::vector<LMV_GUEST_KI_WINDOW_PAGE>& pages,
    const std::vector<KiReachableCall>& calls, KiTailAudit* audit = nullptr)
{
    const ULONGLONG windowEnd = pages.back().PageAddress + 4096;
    std::vector<KiReachableCall> crossing;
    std::vector<LMV_KI_PAGE_FUNCTION_RECORD> functions;
    for (const auto& call : calls)
        for (ULONG index = 0; index < pages.back().PageFunctionRecordCount; ++index)
        {
            const auto& record = pages.back().PageFunctionRecords[index];
            if (record.BeginAddress != call.Target || record.EndAddress <= windowEnd)
                continue;
            bool known = false;
            for (const auto& previous : functions)
                if (previous.BeginAddress == record.BeginAddress &&
                    previous.EndAddress == record.EndAddress) known = true;
            if (!known)
            {
                crossing.push_back(call);
                functions.push_back(record);
            }
        }
    if (crossing.empty())
    {
        std::printf("guest ki-tail-plan: REVIEW reachable-crossing=0 installed=0 executed=0\n");
        return 0;
    }
    if (audit != nullptr)
        audit->Crossing = static_cast<unsigned>(crossing.size());
    LMV_GUEST_KI_WINDOW_PAGE tail = {};
    LMV_GUEST_KI_WINDOW_PAGE repeat = {};
    tail.PageOffset = 4;
    repeat.PageOffset = 4;
    HANDLE device = OpenGuestDevice();
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("guest ki-tail-plan: FAIL open=%lu\n", GetLastError());
        return 5;
    }
    DWORD firstBytes = 0, secondBytes = 0;
    const BOOL first = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_KI_WINDOW),
        &tail, sizeof(tail), &tail, sizeof(tail), &firstBytes, nullptr);
    const BOOL second = first && DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_KI_WINDOW),
        &repeat, sizeof(repeat), &repeat, sizeof(repeat), &secondBytes, nullptr);
    const DWORD error = second ? 0 : GetLastError();
    CloseHandle(device);
    const bool stable = first && second && firstBytes == sizeof(tail) &&
        secondBytes == sizeof(repeat) && tail.Status == 0 && repeat.Status == 0 &&
        tail.Version == LMV_CPUID_PROTOCOL_VERSION && tail.Size == sizeof(tail) &&
        repeat.Version == tail.Version && repeat.Size == tail.Size &&
        tail.PageOffset == 4 && repeat.PageOffset == 4 &&
        tail.PageAddress == windowEnd && repeat.PageAddress == windowEnd &&
        tail.KernelBase == pages[2].KernelBase &&
        tail.KernelImageSize == pages[2].KernelImageSize &&
        tail.RoutineAddress == pages[2].RoutineAddress &&
        repeat.KernelBase == tail.KernelBase &&
        repeat.KernelImageSize == tail.KernelImageSize &&
        repeat.RoutineAddress == tail.RoutineAddress &&
        tail.BytesCopied == sizeof(tail.PageBytes) &&
        repeat.BytesCopied == sizeof(repeat.PageBytes) &&
        (tail.Flags & LMV_KI_PREFLIGHT_STABLE) != 0 &&
        (repeat.Flags & LMV_KI_PREFLIGHT_STABLE) != 0 &&
        tail.PageHash == repeat.PageHash &&
        tail.PageFunctionRecordTruncated == 0 &&
        repeat.PageFunctionRecordTruncated == 0 &&
        tail.PageFunctionRecordCount == repeat.PageFunctionRecordCount &&
        tail.PageFunctionRecordCount <= LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS &&
        std::memcmp(tail.PageBytes, repeat.PageBytes, sizeof(tail.PageBytes)) == 0 &&
        std::memcmp(tail.PageFunctionRecords, repeat.PageFunctionRecords,
            tail.PageFunctionRecordCount * sizeof(LMV_KI_PAGE_FUNCTION_RECORD)) == 0;
    std::printf("guest ki-tail-page: %s address=%llx hash=%llx records=%lu status=%08lx win32=%lu\n",
        stable ? "PASS" : "FAIL", tail.PageAddress, tail.PageHash,
        tail.PageFunctionRecordCount, tail.Status, error);
    if (!stable) return 5;
    if (audit != nullptr)
    {
        ULONGLONG hash = 14695981039346656037ull;
        for (const auto& page : pages)
            for (unsigned char value : page.PageBytes)
                hash = (hash ^ value) * 1099511628211ull;
        for (unsigned char value : tail.PageBytes)
            hash = (hash ^ value) * 1099511628211ull;
        audit->ContentHash = hash;
    }
    for (size_t index = 0; index < crossing.size(); ++index)
    {
        const auto& record = functions[index];
        bool matchesTail = false;
        for (ULONG entry = 0; entry < tail.PageFunctionRecordCount; ++entry)
            if (tail.PageFunctionRecords[entry].BeginAddress == record.BeginAddress &&
                tail.PageFunctionRecords[entry].EndAddress == record.EndAddress &&
                tail.PageFunctionRecords[entry].UnwindAddress == record.UnwindAddress)
                matchesTail = true;
        if (!matchesTail || record.EndAddress > tail.PageAddress + 4096)
        {
            if (audit != nullptr) ++audit->Uncovered;
            std::printf("guest ki-tail-plan: REVIEW function=[%llx,%llx) status=%s installed=0 executed=0\n",
                record.BeginAddress, record.EndAddress,
                matchesTail ? "extends-beyond-tail" : "metadata-changed");
            continue;
        }
        const KiTailReview review = ReviewKiReachableTail(pages, tail, crossing[index], record);
        if (audit != nullptr)
        {
            ++audit->Reviewed;
            audit->Issues += review.Issues;
        }
    }
    return 0;
}

static int RunKiTailSelftest()
{
    std::vector<LMV_GUEST_KI_WINDOW_PAGE> pages(6);
    const ULONGLONG begin = 0x10000000;
    for (size_t index = 0; index < pages.size(); ++index)
        pages[index].PageAddress = begin + index * 4096;
    LMV_GUEST_KI_WINDOW_PAGE tail = {};
    tail.PageAddress = begin + pages.size() * 4096;
    const ULONGLONG target = tail.PageAddress - 8;
    const ULONGLONG source = pages[2].PageAddress + 0x120;
    unsigned char* caller = pages[2].PageBytes + 0x120;
    caller[0] = 0xE8;
    const std::int32_t callDisplacement = static_cast<std::int32_t>(target - (source + 5));
    std::memcpy(caller + 1, &callDisplacement, sizeof(callDisplacement));
    std::memset(pages.back().PageBytes + 4096 - 8, 0x90, 8);
    std::memset(tail.PageBytes, 0x90, 16);
    tail.PageBytes[0] = 0xE9;
    const std::int32_t jumpDisplacement = 5;
    std::memcpy(tail.PageBytes + 1, &jumpDisplacement, sizeof(jumpDisplacement));
    tail.PageBytes[15] = 0xC3;
    LMV_KI_PAGE_FUNCTION_RECORD function = {};
    function.BeginAddress = target;
    function.EndAddress = tail.PageAddress + 16;
    KiReachableCall call = { source, target, source + 5 };
    const KiTailReview good = ReviewKiReachableTail(pages, tail, call, function);
    ++call.ReturnAddress;
    const KiTailReview bad = ReviewKiReachableTail(pages, tail, call, function);
    const bool pass = good.Instructions == 20 && good.Patched == 1 &&
        good.Returns == 1 && good.Issues == 0 && bad.Issues != 0;
    std::printf("guest ki-tail-selftest: %s instructions=%u patched=%u returns=%u issues=%u invalid-return-issues=%u\n",
        pass ? "PASS" : "FAIL", good.Instructions, good.Patched,
        good.Returns, good.Issues, bad.Issues);
    return pass ? 0 : 5;
}

enum KiInstallBlocker : unsigned
{
    KiInstallBlockerSnapshot = 1u << 0,
    KiInstallBlockerRelocation = 1u << 1,
    KiInstallBlockerTail = 1u << 2,
    KiInstallBlockerRouting = 1u << 3,
    KiInstallBlockerMirror = 1u << 4,
    KiInstallBlockerReturn = 1u << 5
};

static unsigned EvaluateKiInstallBlockers(bool snapshotValid,
    const KiWindowAudit& window, const KiEntryPathStats& entry,
    const KiTailAudit& tail)
{
    unsigned blockers = 0;
    if (!snapshotValid) blockers |= KiInstallBlockerSnapshot;
    if (window.Targets != 1 || window.Failures != 0 ||
        entry.Instructions == 0 || entry.Failures != 0)
        blockers |= KiInstallBlockerRelocation;
    if (tail.Issues != 0 || tail.Uncovered != 0 || tail.Reviewed != tail.Crossing)
        blockers |= KiInstallBlockerTail;
    if (entry.OtherFunctions != 0 || entry.OutsideWindow != 0 ||
        entry.Indirect != 0 || entry.FallthroughExits != 0 || window.Boundary != 0)
        blockers |= KiInstallBlockerRouting;
    blockers |= KiInstallBlockerMirror | KiInstallBlockerReturn;
    return blockers;
}

static bool CheckKiMirrorStorage(const KiTailAudit& tail)
{
    if (tail.ContentHash == 0)
    {
        std::printf("guest ki-mirror-storage: REVIEW missing-seventh-page installed=0\n");
        return false;
    }
    HANDLE device = OpenGuestDevice();
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("guest ki-mirror-storage: FAIL open=%lu\n", GetLastError());
        return false;
    }
    LMV_GUEST_KI_MIRROR_STORAGE request = {};
    request.Operation = LMV_KI_MIRROR_ALLOCATE;
    request.RequestedPages = 7;
    DWORD returned = 0;
    const BOOL allocated = DeviceIoControl(device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_MIRROR_STORAGE),
        &request, sizeof(request), &request, sizeof(request), &returned, nullptr);
    const DWORD allocateError = allocated ? 0 : GetLastError();
    bool valid = allocated && returned == sizeof(request) && request.Status == 0 &&
        request.Version == LMV_CPUID_PROTOCOL_VERSION && request.Size == sizeof(request) &&
        request.Operation == LMV_KI_MIRROR_ALLOCATE && request.RequestedPages == 7 &&
        request.AllocatedPages == 7 && request.VirtualAddress != 0 &&
        (request.VirtualAddress & 0xfff) == 0 &&
        request.Flags == (LMV_KI_MIRROR_FLAG_STORAGE_ONLY |
            LMV_KI_MIRROR_FLAG_PHYSICAL_LIST) && request.ContentHash == tail.ContentHash;
    for (unsigned index = 0; index < 7; ++index)
    {
        valid = valid && request.PhysicalPages[index] != 0 &&
            (request.PhysicalPages[index] & 0xfff) == 0;
        for (unsigned previous = 0; previous < index; ++previous)
            valid = valid && request.PhysicalPages[index] != request.PhysicalPages[previous];
    }
    std::printf("guest ki-mirror-storage: %s pages=%lu va=%llx hash=%llx expected=%llx flags=%08lx status=%08lx win32=%lu executable=0 npt-installed=0\n",
        valid ? "PASS" : "REVIEW", request.AllocatedPages, request.VirtualAddress,
        request.ContentHash, tail.ContentHash, request.Flags, request.Status, allocateError);
    LMV_GUEST_KI_MIRROR_STORAGE release = {};
    release.Operation = LMV_KI_MIRROR_RELEASE;
    returned = 0;
    const BOOL released = DeviceIoControl(device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_MIRROR_STORAGE),
        &release, sizeof(release), &release, sizeof(release), &returned, nullptr);
    const DWORD releaseError = released ? 0 : GetLastError();
    CloseHandle(device);
    const bool freed = released && returned == sizeof(release) && release.Status == 0 &&
        release.VirtualAddress == 0 && release.AllocatedPages == 0;
    std::printf("guest ki-mirror-release: %s status=%08lx win32=%lu\n",
        freed ? "PASS" : "FAIL", release.Status, releaseError);
    return valid && freed;
}

static bool CheckKiExecutableMirror(const std::vector<LMV_GUEST_KI_WINDOW_PAGE>& pages,
    const KiTailAudit& tail)
{
    if (tail.ContentHash == 0) return false;
    HANDLE device = OpenGuestDevice();
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("guest ki-real-mirror: FAIL open=%lu\n", GetLastError());
        return false;
    }
    LMV_GUEST_KI_MIRROR_STORAGE request = {};
    request.Operation = LMV_KI_MIRROR_ALLOCATE_MDL;
    request.RequestedPages = 7;
    DWORD returned = 0;
    const BOOL allocated = DeviceIoControl(device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_MIRROR_STORAGE),
        &request, sizeof(request), &request, sizeof(request), &returned, nullptr);
    const DWORD allocateError = allocated ? 0 : GetLastError();
    const bool shape = allocated && returned == sizeof(request) && request.Status == 0 &&
        request.Version == LMV_CPUID_PROTOCOL_VERSION && request.Size == sizeof(request) &&
        request.Operation == LMV_KI_MIRROR_ALLOCATE_MDL && request.RequestedPages == 7 &&
        request.AllocatedPages == 7 && request.VirtualAddress != 0 &&
        (request.VirtualAddress & 0xfff) == 0 &&
        request.Flags == (LMV_KI_MIRROR_FLAG_MDL | LMV_KI_MIRROR_FLAG_PHYSICAL_LIST) &&
        request.ContentHash == tail.ContentHash;
    bool physicalPagesValid = shape;
    for (unsigned index = 0; index < 7; ++index)
    {
        physicalPagesValid = physicalPagesValid && request.PhysicalPages[index] != 0 &&
            (request.PhysicalPages[index] & 0xfff) == 0;
        for (unsigned previous = 0; previous < index; ++previous)
            physicalPagesValid = physicalPagesValid &&
                request.PhysicalPages[index] != request.PhysicalPages[previous];
    }
    KiWindowAudit audit = {};
    const int analysis = physicalPagesValid &&
        request.VirtualAddress <= UINT64_MAX - pages.size() * 4096 ?
        AnalyzeKiWindow(pages, &audit, request.VirtualAddress, 7) : 5;
    LMV_GUEST_KI_MIRROR_STORAGE release = {};
    release.Operation = LMV_KI_MIRROR_RELEASE;
    returned = 0;
    const BOOL released = DeviceIoControl(device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_MIRROR_STORAGE),
        &release, sizeof(release), &release, sizeof(release), &returned, nullptr);
    const DWORD releaseError = released ? 0 : GetLastError();
    CloseHandle(device);
    const bool freed = released && returned == sizeof(release) && release.Status == 0 &&
        release.VirtualAddress == 0 && release.AllocatedPages == 0;
    const bool relocationFits = analysis == 0 && audit.Failures == 0;
    std::printf("guest ki-real-mirror-plan: %s status=%08lx va=%llx pages=%lu hash=%llx expected-hash=%llx flags=%08lx patched=%u range-or-decode-failures=%u boundary=%u call-relays=%u jump-relays=%u conditional-relays=%u rip-data-range=%u unsupported-control=%u allocate=%u release=%u win32=%lu/%lu executable=1 installed=0 executed-ki=0\n",
        physicalPagesValid && relocationFits && freed ? "REVIEW" : "BLOCKED",
        request.Status, request.VirtualAddress, request.AllocatedPages,
        request.ContentHash, tail.ContentHash, request.Flags, audit.Patched,
        audit.Failures, audit.Boundary, audit.CallRelays, audit.JumpRelays,
        audit.ConditionalRelays, audit.RipDataRangeFailures, audit.UnsupportedControl,
        physicalPagesValid ? 1u : 0u,
        freed ? 1u : 0u, allocateError, releaseError);
    return physicalPagesValid && relocationFits && freed;
}

static int RunKiInstallPreflightSelftest()
{
    KiWindowAudit window = { 1, 0, 2, 1257 };
    KiEntryPathStats entry = {};
    entry.Instructions = 284;
    entry.OtherFunctions = 8;
    entry.OutsideWindow = 26;
    KiTailAudit tail = { 1, 1, 0, 0, 0 };
    const unsigned expected = KiInstallBlockerRouting |
        KiInstallBlockerMirror | KiInstallBlockerReturn;
    const bool baseline = EvaluateKiInstallBlockers(true, window, entry, tail) == expected;
    ++window.Failures;
    const bool relocation = (EvaluateKiInstallBlockers(true, window, entry, tail) &
        KiInstallBlockerRelocation) != 0;
    window.Failures = 0;
    ++tail.Uncovered;
    const bool incomplete = (EvaluateKiInstallBlockers(true, window, entry, tail) &
        KiInstallBlockerTail) != 0;
    tail.Uncovered = 0;
    const bool invalidSnapshot = (EvaluateKiInstallBlockers(false, window, entry, tail) &
        KiInstallBlockerSnapshot) != 0;
    const bool pass = baseline && relocation && incomplete && invalidSnapshot;
    std::printf("guest ki-install-preflight-selftest: %s baseline-blockers=%08x relocation=%u tail=%u snapshot=%u no-install=1\n",
        pass ? "PASS" : "FAIL", expected, relocation ? 1u : 0u,
        incomplete ? 1u : 0u, invalidSnapshot ? 1u : 0u);
    return pass ? 0 : 5;
}

static int RunKiWindowPlan(bool installPreflight = false, bool realMirrorPlan = false)
{
    HANDLE device = OpenGuestDevice();
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("guest ki-window-plan: FAIL open=%lu\n", GetLastError());
        return 5;
    }
    std::vector<LMV_GUEST_KI_WINDOW_PAGE> pages(6);
    bool pass = true;
    for (unsigned index = 0; index < pages.size(); ++index)
    {
        auto& page = pages[index];
        LMV_GUEST_KI_WINDOW_PAGE repeat = {};
        page.PageOffset = static_cast<LONG>(index) - 2;
        repeat.PageOffset = page.PageOffset;
        DWORD returned = 0, repeated = 0;
        const BOOL first = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_KI_WINDOW),
            &page, sizeof(page), &page, sizeof(page), &returned, nullptr);
        const BOOL second = first && DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_KI_WINDOW),
            &repeat, sizeof(repeat), &repeat, sizeof(repeat), &repeated, nullptr);
        const bool stable = first && second && returned == sizeof(page) &&
            repeated == sizeof(repeat) && page.Status == 0 && repeat.Status == 0 &&
            page.Version == LMV_CPUID_PROTOCOL_VERSION && page.Size == sizeof(page) &&
            repeat.Version == page.Version && repeat.Size == page.Size &&
            page.PageOffset == static_cast<LONG>(index) - 2 && repeat.PageOffset == page.PageOffset &&
            page.BytesCopied == sizeof(page.PageBytes) &&
            repeat.BytesCopied == sizeof(repeat.PageBytes) &&
            (page.Flags & LMV_KI_PREFLIGHT_STABLE) != 0 &&
            (repeat.Flags & LMV_KI_PREFLIGHT_STABLE) != 0 &&
            page.PageFunctionRecordTruncated == 0 && repeat.PageFunctionRecordTruncated == 0 &&
            page.PageFunctionRecordCount <= LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS &&
            page.PageFunctionRecordCount == repeat.PageFunctionRecordCount &&
            page.RoutineAddress == repeat.RoutineAddress &&
            page.KernelBase == repeat.KernelBase &&
            page.KernelImageSize == repeat.KernelImageSize &&
            page.PageAddress == repeat.PageAddress && page.PageHash == repeat.PageHash &&
            std::memcmp(page.PageBytes, repeat.PageBytes, sizeof(page.PageBytes)) == 0 &&
            std::memcmp(page.PageFunctionRecords, repeat.PageFunctionRecords,
                page.PageFunctionRecordCount * sizeof(LMV_KI_PAGE_FUNCTION_RECORD)) == 0;
        std::printf("guest ki-window-page: offset=%ld %s address=%llx records=%lu\n",
            page.PageOffset, stable ? "PASS" : "FAIL", page.PageAddress,
            page.PageFunctionRecordCount);
        pass = pass && stable;
        if (!stable) break;
    }
    LMV_GUEST_KI_PAGE_PREFLIGHT snapshot = {};
    bool snapshotValid = false;
    if (installPreflight && pass)
    {
        DWORD snapshotBytes = 0;
        const BOOL snapshotOk = DeviceIoControl(device,
            static_cast<DWORD>(IOCTL_LMV_GUEST_KI_PAGE_PREFLIGHT), nullptr, 0,
            &snapshot, sizeof(snapshot), &snapshotBytes, nullptr);
        const DWORD snapshotError = snapshotOk ? 0 : GetLastError();
        const ULONG requiredFlags = LMV_KI_PREFLIGHT_IMAGE_RANGE |
            LMV_KI_PREFLIGHT_STABLE | LMV_KI_PREFLIGHT_PROLOGUE |
            LMV_KI_PREFLIGHT_NPT_TRANSLATED;
        snapshotValid = snapshotOk && snapshotBytes == sizeof(snapshot) &&
            snapshot.Status == 0 && snapshot.Version == LMV_CPUID_PROTOCOL_VERSION &&
            snapshot.Size == sizeof(snapshot) &&
            (snapshot.Flags & requiredFlags) == requiredFlags &&
            snapshot.PageAddress == pages[2].PageAddress &&
            snapshot.RoutineAddress == pages[2].RoutineAddress &&
            snapshot.PageHash == pages[2].PageHash &&
            snapshot.BytesCopied == sizeof(snapshot.PageBytes) &&
            snapshot.PrologueVerified == 1 &&
            snapshot.GuestPhysicalPage != 0 && snapshot.HostPhysicalPage != 0 &&
            snapshot.FunctionRecordFound == 1 &&
            snapshot.FunctionBeginAddress == snapshot.RoutineAddress &&
            snapshot.PageFunctionRecordTruncated == 0 &&
            snapshot.PageFunctionRecordCount == pages[2].PageFunctionRecordCount &&
            std::memcmp(snapshot.PageBytes, pages[2].PageBytes,
                sizeof(snapshot.PageBytes)) == 0 &&
            std::memcmp(snapshot.PageFunctionRecords, pages[2].PageFunctionRecords,
                snapshot.PageFunctionRecordCount * sizeof(LMV_KI_PAGE_FUNCTION_RECORD)) == 0;
        std::printf("guest ki-install-snapshot: %s status=%08lx flags=%08lx ki=%llx gpa=%llx hpa=%llx win32=%lu installed=0\n",
            snapshotValid ? "PASS" : "REVIEW", snapshot.Status, snapshot.Flags,
            snapshot.RoutineAddress, snapshot.GuestPhysicalPage,
            snapshot.HostPhysicalPage, snapshotError);
    }
    CloseHandle(device);
    if (!pass) return 5;
    const ULONGLONG begin = pages[0].PageAddress;
    pass = begin <= UINT64_MAX - 0x100000 - pages.size() * 4096;
    for (unsigned index = 0; index < pages.size(); ++index)
        pass = pass && pages[index].PageAddress == begin + index * 4096 &&
            pages[index].RoutineAddress == pages[2].RoutineAddress &&
            pages[index].KernelBase == pages[2].KernelBase &&
            pages[index].KernelImageSize == pages[2].KernelImageSize;
    pass = pass && pages[2].RoutineAddress >= pages[2].PageAddress &&
        pages[2].RoutineAddress < pages[2].PageAddress + 4096;
    if (!pass)
    {
        std::printf("guest ki-window-plan: FAIL inconsistent-window\n");
        return 5;
    }
    KiWindowAudit window = {};
    const int windowStatus = AnalyzeKiWindow(pages, &window);
    if (windowStatus != 0) return windowStatus;
    std::vector<KiReachableCall> calls;
    KiEntryPathStats entry = {};
    const int entryStatus = AnalyzeKiEntryPaths(pages, &entry, &calls);
    if (entryStatus != 0) return entryStatus;
    KiTailAudit tail = {};
    const int tailStatus = ReviewKiReachableCalls(pages, calls, &tail);
    if (tailStatus != 0) return tailStatus;
    if (realMirrorPlan)
        return CheckKiExecutableMirror(pages, tail) ? 0 : 5;
    if (!installPreflight) return 0;
    const bool storageReady = CheckKiMirrorStorage(tail);
    const ULONGLONG candidate = begin + 0x100000;
    const size_t requiredPages = pages.size() + (tail.Crossing != 0 ? 1u : 0u);
    const bool overlapsImage = candidate >= pages[2].KernelBase &&
        candidate - pages[2].KernelBase < pages[2].KernelImageSize &&
        requiredPages * 4096 <= pages[2].KernelImageSize -
            (candidate - pages[2].KernelBase);
    const unsigned blockers = EvaluateKiInstallBlockers(snapshotValid, window, entry, tail);
    std::printf("guest ki-install-preflight: BLOCKED blockers=%08x snapshot=%u targets=%u relocation-failures=%u window-boundary=%u entry-issues=%u external-calls-and-branches=%u other-functions=%u tail=%u/%u tail-issues=%u required-mirror-pages=%zu nx-storage=%u candidate=%llx candidate-overlaps-image=%u executable-mirror=absent return-runtime=unverified routing=unverified npt-installed=0 ki-executed=0\n",
        blockers, snapshotValid ? 1u : 0u, window.Targets, window.Failures,
        window.Boundary, entry.Failures + entry.Indirect + entry.FallthroughExits,
        entry.OutsideWindow, entry.OtherFunctions, tail.Reviewed,
        tail.Crossing, tail.Issues + tail.Uncovered, requiredPages,
        storageReady ? 1u : 0u, candidate,
        overlapsImage ? 1u : 0u);
    return 5;
}

static int RunPrivateDr3Smoke(HANDLE device)
{
    LMV_GUEST_DR3_SMOKE_RESULT result = {};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_DR3_SMOKE),
        nullptr, 0, &result, sizeof(result), &returned, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    const bool pass = ok && returned == sizeof(result) && result.Status == 0 &&
        result.Step == 7 && result.HitsAfter == result.HitsBefore + 1 &&
        result.HitsAfterDisarm == result.HitsAfter &&
        result.GuestDr3Readback == result.Target &&
        result.GuestDr7Readback == (result.OriginalDr7 | 0x40);
    std::printf("guest dr3-smoke: %s status=%08lx step=%lu target=%llx dr3={original=%llx readback=%llx} "
        "dr7={original=%llx readback=%llx} hits=%llu:%llu:%llu win32=%lu\n",
        pass ? "PASS" : "FAIL", result.Status, result.Step, result.Target,
        result.OriginalDr3, result.GuestDr3Readback, result.OriginalDr7,
        result.GuestDr7Readback, result.HitsBefore, result.HitsAfter,
        result.HitsAfterDisarm, error);
    return pass ? 0 : 5;
}
static int RunPrivateDr3AllCpusSmoke(HANDLE device)
{
    const WORD groupCount = GetActiveProcessorGroupCount();
    DWORD expected = 0;
    for (WORD group = 0; group < groupCount; ++group)
        expected += GetActiveProcessorCount(group);
    if (expected == 0)
        return 5;

    GROUP_AFFINITY previous = {};
    bool pinned = false;
    DWORD tested = 0;
    int result = 0;
    for (WORD group = 0; group < groupCount && result == 0; ++group)
    {
        const DWORD count = GetActiveProcessorCount(group);
        for (WORD number = 0; number < count; ++number)
        {
            GROUP_AFFINITY affinity = {};
            affinity.Group = group;
            affinity.Mask = static_cast<KAFFINITY>(1) << number;
            if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity,
                pinned ? nullptr : &previous))
            {
                result = 5;
                break;
            }
            pinned = true;
            if (RunPrivateDr3Smoke(device) != 0)
            {
                std::printf("guest dr3-all-cpus: FAIL cpu=%hu:%hu\n", group, number);
                result = 5;
                break;
            }
            ++tested;
        }
    }
    if (pinned && !SetThreadGroupAffinity(GetCurrentThread(), &previous, nullptr))
        result = 5;
    std::printf("guest dr3-all-cpus: %s tested=%lu expected=%lu\n",
        result == 0 && tested == expected ? "PASS" : "FAIL", tested, expected);
    return result == 0 && tested == expected ? 0 : 5;
}
static volatile LONG g_NativeDrHits;
static volatile LONG g_NativeDrUnexpected;
static DWORD g_NativeDrThreadId;
static unsigned g_NativeDrSlot;
static DWORD64 g_NativeDrStatus;

__declspec(noinline) static DWORD WINAPI NativeDrTarget(LPVOID)
{
    return 0x12345678;
}

static LONG CALLBACK NativeDrExceptionHandler(PEXCEPTION_POINTERS exception)
{
    if (GetCurrentThreadId() != g_NativeDrThreadId ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD64 slotBit = 1ull << g_NativeDrSlot;
    const DWORD64 enableBits = 3ull << (g_NativeDrSlot * 2);
    const DWORD64 conditionBits = 0xFull << (16 + g_NativeDrSlot * 4);
    g_NativeDrStatus = exception->ContextRecord->Dr6;
    exception->ContextRecord->Dr7 &= ~(enableBits | conditionBits);
    exception->ContextRecord->Dr6 = 0;
    if (exception->ExceptionRecord->ExceptionAddress !=
            reinterpret_cast<void*>(&NativeDrTarget) ||
        (g_NativeDrStatus & slotBit) == 0)
        InterlockedIncrement(&g_NativeDrUnexpected);
    else
        InterlockedIncrement(&g_NativeDrHits);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int RunNativeDrSmoke()
{
    PVOID handler = AddVectoredExceptionHandler(1, NativeDrExceptionHandler);
    if (handler == nullptr)
    {
        std::printf("guest native-dr-smoke: FAIL handler win32=%lu\n", GetLastError());
        return 5;
    }
    int result = 0;
    for (unsigned slot = 0; slot < 3; ++slot)
    {
        DWORD threadId = 0;
        HANDLE thread = CreateThread(nullptr, 0, NativeDrTarget, nullptr,
            CREATE_SUSPENDED, &threadId);
        if (thread == nullptr)
        {
            std::printf("guest native-dr-smoke: FAIL slot=%u create=%lu\n",
                slot, GetLastError());
            result = 5;
            break;
        }

        CONTEXT context = {};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        bool started = false;
        DWORD waitStatus = WAIT_FAILED;
        DWORD exitCode = 0;
        InterlockedExchange(&g_NativeDrHits, 0);
        InterlockedExchange(&g_NativeDrUnexpected, 0);
        g_NativeDrThreadId = threadId;
        g_NativeDrSlot = slot;
        g_NativeDrStatus = 0;
        if (GetThreadContext(thread, &context) && (context.Dr7 & 0xff) == 0)
        {
            const DWORD64 enableBits = 3ull << (slot * 2);
            const DWORD64 conditionBits = 0xFull << (16 + slot * 4);
            const DWORD64 address = reinterpret_cast<DWORD64>(&NativeDrTarget);
            if (slot == 0) context.Dr0 = address;
            if (slot == 1) context.Dr1 = address;
            if (slot == 2) context.Dr2 = address;
            context.Dr6 = 0;
            context.Dr7 = (context.Dr7 & ~(enableBits | conditionBits)) |
                (1ull << (slot * 2));
            if (SetThreadContext(thread, &context) &&
                GetThreadContext(thread, &context) &&
                (context.Dr7 & (1ull << (slot * 2))) != 0 &&
                ResumeThread(thread) != static_cast<DWORD>(-1))
            {
                started = true;
                waitStatus = WaitForSingleObject(thread, 5000);
                if (waitStatus == WAIT_OBJECT_0)
                    GetExitCodeThread(thread, &exitCode);
            }
        }
        if (!started || waitStatus != WAIT_OBJECT_0)
        {
            TerminateThread(thread, 5);
            if (WaitForSingleObject(thread, 1000) != WAIT_OBJECT_0)
                ExitProcess(5);
        }
        CloseHandle(thread);
        const bool pass = started && waitStatus == WAIT_OBJECT_0 &&
            exitCode == 0x12345678 && g_NativeDrHits == 1 &&
            g_NativeDrUnexpected == 0 && (g_NativeDrStatus & (1ull << slot)) != 0;
        std::printf("guest native-dr-smoke: slot=%u %s hits=%ld unexpected=%ld dr6=%llx wait=%lu\n",
            slot, pass ? "PASS" : "FAIL", g_NativeDrHits,
            g_NativeDrUnexpected, g_NativeDrStatus, waitStatus);
        if (!pass)
        {
            result = 5;
            break;
        }
    }
    RemoveVectoredExceptionHandler(handler);
    std::printf("guest native-dr-smoke: %s\n", result == 0 ? "PASS" : "FAIL");
    return result;
}
struct NativeDrCoexistWorker
{
    HANDLE StartEvent;
    HANDLE DoneEvent;
    DWORD WaitStatus;
    DWORD Observed;
};

static DWORD WINAPI NativeDrCoexistTarget(LPVOID context)
{
    NativeDrCoexistWorker* worker = static_cast<NativeDrCoexistWorker*>(context);
    worker->WaitStatus = WaitForSingleObject(worker->StartEvent, 5000);
    if (worker->WaitStatus == WAIT_OBJECT_0)
        worker->Observed = NativeDrTarget(nullptr);
    SetEvent(worker->DoneEvent);
    return worker->WaitStatus == WAIT_OBJECT_0 ? worker->Observed : 5;
}

static int RunNativeDrCoexistSmoke(HANDLE device)
{
    HANDLE startEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (startEvent == nullptr || doneEvent == nullptr)
    {
        std::printf("guest dr-coexist: FAIL create-event=%lu\n", GetLastError());
        if (startEvent != nullptr) CloseHandle(startEvent);
        if (doneEvent != nullptr) CloseHandle(doneEvent);
        return 5;
    }

    NativeDrCoexistWorker worker = {startEvent, doneEvent, WAIT_FAILED, 0};
    DWORD workerId = 0;
    HANDLE thread = CreateThread(nullptr, 0, NativeDrCoexistTarget,
        &worker, CREATE_SUSPENDED, &workerId);
    if (thread == nullptr)
    {
        std::printf("guest dr-coexist: FAIL create-thread=%lu\n", GetLastError());
        CloseHandle(startEvent);
        CloseHandle(doneEvent);
        return 5;
    }

    int result = 5;
    GROUP_AFFINITY previous = {};
    bool pinned = false;
    bool started = false;
    BOOL ok = FALSE;
    DWORD ioctlError = 0;
    PVOID handler = nullptr;
    LMV_GUEST_DR_COEXIST_RESULT request = {};
    DWORD returned = 0;
    DWORD workerExit = 0;
    DWORD waitStatus = WAIT_FAILED;
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    PROCESSOR_NUMBER current = {};
    GetCurrentProcessorNumberEx(&current);
    GROUP_AFFINITY affinity = {};
    affinity.Group = current.Group;
    affinity.Mask = static_cast<KAFFINITY>(1) << current.Number;
    if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, &previous))
        goto Cleanup;
    pinned = true;
    if (!SetThreadGroupAffinity(thread, &affinity, nullptr))
        goto Cleanup;
    handler = AddVectoredExceptionHandler(1, NativeDrExceptionHandler);
    if (handler == nullptr)
        goto Cleanup;
    InterlockedExchange(&g_NativeDrHits, 0);
    InterlockedExchange(&g_NativeDrUnexpected, 0);
    g_NativeDrThreadId = workerId;
    g_NativeDrSlot = 0;
    g_NativeDrStatus = 0;
    if (!GetThreadContext(thread, &context) || (context.Dr7 & 0xff) != 0)
        goto Cleanup;
    context.Dr0 = reinterpret_cast<DWORD64>(&NativeDrTarget);
    context.Dr6 = 0;
    context.Dr7 = (context.Dr7 & ~(3ull | (0xFull << 16))) | 1;
    if (!SetThreadContext(thread, &context) ||
        ResumeThread(thread) == static_cast<DWORD>(-1))
        goto Cleanup;
    started = true;
    request.StartEvent = startEvent;
    request.DoneEvent = doneEvent;
    ok = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_DR_COEXIST),
        &request, static_cast<DWORD>(sizeof(request)), &request,
        static_cast<DWORD>(sizeof(request)), &returned, nullptr);
    ioctlError = ok ? 0 : GetLastError();
    waitStatus = WaitForSingleObject(thread, 5000);
    if (waitStatus == WAIT_OBJECT_0)
        GetExitCodeThread(thread, &workerExit);
    if (ok && returned == sizeof(request) && request.Status == 0 &&
        request.Step == 6 && request.HitsAfterNative == request.HitsBefore &&
        request.HitsAfterPrivate == request.HitsBefore + 1 &&
        request.HitsAfterDisarm == request.HitsAfterPrivate &&
        request.HitsAfterRestore == request.HitsAfterDisarm &&
        waitStatus == WAIT_OBJECT_0 && workerExit == 0x12345678 &&
        worker.WaitStatus == WAIT_OBJECT_0 && worker.Observed == 0x12345678 &&
        g_NativeDrHits == 1 && g_NativeDrUnexpected == 0 &&
        (g_NativeDrStatus & 1) != 0)
        result = 0;
    std::printf("guest dr-coexist: %s status=%08lx step=%lu dr0={hits=%ld unexpected=%ld dr6=%llx} dr3=%llu:%llu:%llu:%llu:%llu wait=%lu win32=%lu\n",
        result == 0 ? "PASS" : "FAIL", request.Status, request.Step,
        g_NativeDrHits, g_NativeDrUnexpected, g_NativeDrStatus,
        request.HitsBefore, request.HitsAfterNative, request.HitsAfterPrivate,
        request.HitsAfterDisarm, request.HitsAfterRestore, waitStatus, ioctlError);

Cleanup:
    if (!started)
        TerminateThread(thread, 5);
    if (WaitForSingleObject(thread, 1000) != WAIT_OBJECT_0)
        ExitProcess(5);
    if (handler != nullptr)
        RemoveVectoredExceptionHandler(handler);
    if (pinned)
        SetThreadGroupAffinity(GetCurrentThread(), &previous, nullptr);
    CloseHandle(thread);
    CloseHandle(startEvent);
    CloseHandle(doneEvent);
    return result;
}
static int KiProbeExceptionFilter(EXCEPTION_POINTERS* exception,
    DWORD expectedCode, ULONGLONG* observedAddress, DWORD* observedCode,
    LMV_GUEST_KI_FRAME_SNAPSHOT* observedFrame)
{
    if (exception == nullptr || exception->ExceptionRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;
    if (observedAddress != nullptr)
        *observedAddress = reinterpret_cast<ULONGLONG>(
            exception->ExceptionRecord->ExceptionAddress);
    if (observedCode != nullptr)
        *observedCode = exception->ExceptionRecord->ExceptionCode;
    if (observedFrame != nullptr && exception->ContextRecord != nullptr)
    {
        const CONTEXT* context = exception->ContextRecord;
        observedFrame->Rip = context->Rip;
        observedFrame->Rsp = context->Rsp;
        observedFrame->Rflags = context->EFlags;
        observedFrame->Rax = context->Rax;
        observedFrame->Rcx = context->Rcx;
        observedFrame->Rdx = context->Rdx;
        observedFrame->R8 = context->R8;
        observedFrame->R9 = context->R9;
        observedFrame->R10 = context->R10;
        observedFrame->R11 = context->R11;
        observedFrame->Rbx = context->Rbx;
        observedFrame->Rbp = context->Rbp;
        observedFrame->Rsi = context->Rsi;
        observedFrame->Rdi = context->Rdi;
        observedFrame->R12 = context->R12;
        observedFrame->R13 = context->R13;
        observedFrame->R14 = context->R14;
        observedFrame->R15 = context->R15;
    }
    return exception->ExceptionRecord->ExceptionCode == expectedCode ?
        EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static bool TriggerKiProbeException(bool hardware,
    ULONGLONG* observedAddress = nullptr, DWORD* observedCode = nullptr,
    LMV_GUEST_KI_FRAME_SNAPSHOT* observedFrame = nullptr)
{
    bool caught = false;
    __try
    {
        if (hardware)
            __ud2();
        else
            RaiseException(0xE0424242, 0, 0, nullptr);
    }
    __except (KiProbeExceptionFilter(GetExceptionInformation(),
        hardware ? EXCEPTION_ILLEGAL_INSTRUCTION : 0xE0424242,
        observedAddress, observedCode, observedFrame))
    {
        caught = true;
    }
    return caught;
}
static bool TriggerKiHandledProbe(bool* unexpectedException)
{
    bool resumed = false;
    *unexpectedException = false;
    __try
    {
        __ud2();
        resumed = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *unexpectedException = true;
    }
    return resumed && !*unexpectedException;
}
struct KiDr3ProbeWorker
{
    HANDLE Device;
    LMV_GUEST_KI_DR3_PROBE_RESULT Result;
    DWORD Returned;
    DWORD Error;
    BOOL Success;
};

static DWORD WINAPI KiDr3ProbeWorkerEntry(LPVOID context)
{
    KiDr3ProbeWorker* worker = static_cast<KiDr3ProbeWorker*>(context);
    worker->Success = DeviceIoControl(worker->Device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_PROBE),
        &worker->Result, static_cast<DWORD>(sizeof(worker->Result)),
        &worker->Result, static_cast<DWORD>(sizeof(worker->Result)),
        &worker->Returned, nullptr);
    worker->Error = worker->Success ? 0 : GetLastError();
    return 0;
}

struct KiDr3EventWorker
{
    HANDLE Device;
    HANDLE ForeignDevice;
    HANDLE StartEvent;
    LMV_GUEST_KI_DR3_EVENT Event;
    LMV_GUEST_STUB_CONTINUE Continuation;
    DWORD EventBytes;
    DWORD ContinueBytes;
    DWORD Error;
    BOOL Received;
    BOOL Continued;
    BOOL WrongSequenceRejected;
    BOOL DuplicateDeliveryRejected;
    BOOL ForeignHandleRejected;
    BOOL RaxWriteRequested;
    BOOL RaxWriteSucceeded;
    BOOL HandleUd2Requested;
    BOOL RipWriteSucceeded;
    BOOL HandledWithoutRipRejected;
    DWORD RipWriteBytes;
    BOOL RaxWrongSequenceRejected;
    BOOL RaxForeignHandleRejected;
    DWORD RaxWriteBytes;
    ULONGLONG RequestedRax;
};

static DWORD WINAPI KiDr3EventWorkerEntry(LPVOID context)
{
    KiDr3EventWorker* worker = static_cast<KiDr3EventWorker*>(context);
    if (WaitForSingleObject(worker->StartEvent, 5000) != WAIT_OBJECT_0)
        return 0;
    LMV_GUEST_STUB_WAIT_REQUEST request = {};
    request.Version = LMV_CPUID_PROTOCOL_VERSION;
    request.Size = sizeof(request);
    request.TimeoutMilliseconds = 4000;
    worker->Received = DeviceIoControl(worker->Device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_WAIT_EVENT),
        &request, sizeof(request), &worker->Event, sizeof(worker->Event),
        &worker->EventBytes, nullptr);
    if (!worker->Received || worker->EventBytes != sizeof(worker->Event))
    {
        worker->Error = GetLastError();
        return 0;
    }
    LMV_GUEST_STUB_CONTINUE wrongSequence = {};
    wrongSequence.Version = LMV_CPUID_PROTOCOL_VERSION;
    wrongSequence.Size = sizeof(wrongSequence);
    wrongSequence.Sequence = worker->Event.Sequence + 1;
    wrongSequence.Disposition = LMV_GUEST_STUB_DISPOSITION_CONTINUE;
    DWORD ignoredBytes = 0;
    worker->WrongSequenceRejected = !DeviceIoControl(worker->Device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_CONTINUE),
        &wrongSequence, sizeof(wrongSequence), &wrongSequence,
        sizeof(wrongSequence), &ignoredBytes, nullptr);
    request.TimeoutMilliseconds = 0;
    LMV_GUEST_KI_DR3_EVENT duplicate = {};
    worker->DuplicateDeliveryRejected = !DeviceIoControl(worker->Device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_WAIT_EVENT),
        &request, sizeof(request), &duplicate, sizeof(duplicate),
        &ignoredBytes, nullptr);
    LMV_GUEST_STUB_CONTINUE foreignContinue = {};
    foreignContinue.Version = LMV_CPUID_PROTOCOL_VERSION;
    foreignContinue.Size = sizeof(foreignContinue);
    foreignContinue.Sequence = worker->Event.Sequence;
    foreignContinue.Disposition = LMV_GUEST_STUB_DISPOSITION_CONTINUE;
    worker->ForeignHandleRejected = !DeviceIoControl(worker->ForeignDevice,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_CONTINUE),
        &foreignContinue, sizeof(foreignContinue), &foreignContinue,
        sizeof(foreignContinue), &ignoredBytes, nullptr);
    if (worker->HandleUd2Requested)
    {
        LMV_GUEST_STUB_CONTINUE premature = {};
        premature.Version = LMV_CPUID_PROTOCOL_VERSION;
        premature.Size = sizeof(premature);
        premature.Sequence = worker->Event.Sequence;
        premature.Disposition = LMV_GUEST_STUB_DISPOSITION_HANDLED;
        worker->HandledWithoutRipRejected = !DeviceIoControl(worker->Device,
            static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_CONTINUE),
            &premature, sizeof(premature), &premature, sizeof(premature),
            &ignoredBytes, nullptr);
        LMV_GUEST_KI_DR3_SET_RIP setRip = {};
        setRip.Version = LMV_CPUID_PROTOCOL_VERSION;
        setRip.Size = sizeof(setRip);
        setRip.Sequence = worker->Event.Sequence;
        setRip.Rip = worker->Event.Frame.Rip + 2;
        worker->RipWriteSucceeded = DeviceIoControl(worker->Device,
            static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_SET_RIP),
            &setRip, sizeof(setRip), &setRip, sizeof(setRip),
            &worker->RipWriteBytes, nullptr);
    }
    if (worker->RaxWriteRequested)
    {
        LMV_GUEST_KI_DR3_SET_RAX wrongRax = {};
        wrongRax.Version = LMV_CPUID_PROTOCOL_VERSION;
        wrongRax.Size = sizeof(wrongRax);
        wrongRax.Sequence = worker->Event.Sequence + 1;
        wrongRax.Rax = worker->RequestedRax;
        worker->RaxWrongSequenceRejected = !DeviceIoControl(worker->Device,
            static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_SET_RAX),
            &wrongRax, sizeof(wrongRax), &wrongRax, sizeof(wrongRax),
            &ignoredBytes, nullptr);

        LMV_GUEST_KI_DR3_SET_RAX foreignRax = {};
        foreignRax.Version = LMV_CPUID_PROTOCOL_VERSION;
        foreignRax.Size = sizeof(foreignRax);
        foreignRax.Sequence = worker->Event.Sequence;
        foreignRax.Rax = worker->RequestedRax;
        worker->RaxForeignHandleRejected = !DeviceIoControl(worker->ForeignDevice,
            static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_SET_RAX),
            &foreignRax, sizeof(foreignRax), &foreignRax, sizeof(foreignRax),
            &ignoredBytes, nullptr);

        LMV_GUEST_KI_DR3_SET_RAX setRax = {};
        setRax.Version = LMV_CPUID_PROTOCOL_VERSION;
        setRax.Size = sizeof(setRax);
        setRax.Sequence = worker->Event.Sequence;
        setRax.Rax = worker->RequestedRax;
        worker->RaxWriteSucceeded = DeviceIoControl(worker->Device,
            static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_SET_RAX),
            &setRax, sizeof(setRax), &setRax, sizeof(setRax),
            &ignoredBytes, nullptr);
        worker->RaxWriteBytes = ignoredBytes;
    }
    worker->Continuation.Version = LMV_CPUID_PROTOCOL_VERSION;
    worker->Continuation.Size = sizeof(worker->Continuation);
    worker->Continuation.Sequence = worker->Event.Sequence;
    worker->Continuation.Disposition = worker->HandleUd2Requested &&
        worker->RipWriteSucceeded ? LMV_GUEST_STUB_DISPOSITION_HANDLED :
        LMV_GUEST_STUB_DISPOSITION_CONTINUE;
    worker->Continued = DeviceIoControl(worker->Device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_CONTINUE),
        &worker->Continuation, sizeof(worker->Continuation),
        &worker->Continuation, sizeof(worker->Continuation),
        &worker->ContinueBytes, nullptr);
    if (!worker->Continued)
        worker->Error = GetLastError();
    return 0;
}

static void PrintKiDr3StackCandidate(const char* registerName,
    ULONGLONG value, ULONGLONG stackPointer)
{
    if (stackPointer >= 0xffff800000000000ull &&
        value >= stackPointer && value - stackPointer < 0x1000)
        std::printf(" %s=+%llx", registerName, value - stackPointer);
    else
        std::printf(" %s=outside", registerName);
}
static int RunKiDr3Probe(HANDLE device, bool hardware, bool redirect = false,
    bool inspect = false, bool captureFrame = false, bool waitForContinue = false,
    bool writeRax = false, bool handleUd2 = false)
{
    HANDLE startEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (startEvent == nullptr || doneEvent == nullptr)
    {
        std::printf("guest ki-dr3-probe: FAIL create-event=%lu\n", GetLastError());
        if (startEvent != nullptr) CloseHandle(startEvent);
        if (doneEvent != nullptr) CloseHandle(doneEvent);
        return 5;
    }
    KiDr3ProbeWorker worker = {};
    worker.Device = device;
    worker.Result.StartEvent = startEvent;
    worker.Result.DoneEvent = doneEvent;
    worker.Result.Reserved = (redirect ? 1u : 0u) |
        (inspect ? 2u : 0u) | (captureFrame ? 4u : 0u) |
        (waitForContinue ? 8u : 0u) | (writeRax ? 16u : 0u) |
        (handleUd2 ? 32u : 0u);
    DWORD workerId = 0;
    HANDLE thread = CreateThread(nullptr, 0, KiDr3ProbeWorkerEntry,
        &worker, CREATE_SUSPENDED, &workerId);
    if (thread == nullptr)
    {
        std::printf("guest ki-dr3-probe: FAIL create-thread=%lu\n", GetLastError());
        CloseHandle(startEvent);
        CloseHandle(doneEvent);
        return 5;
    }

    int result = 5;
    GROUP_AFFINITY previous = {};
    GROUP_AFFINITY affinity = {};
    PROCESSOR_NUMBER current = {};
    DWORD waitStatus = WAIT_FAILED;
    DWORD brokerWaitStatus = WAIT_FAILED;
    HANDLE brokerDevice = INVALID_HANDLE_VALUE;
    HANDLE foreignBrokerDevice = INVALID_HANDLE_VALUE;
    HANDLE brokerThread = nullptr;
    KiDr3EventWorker broker = {};
    broker.RaxWriteRequested = writeRax ? TRUE : FALSE;
    broker.HandleUd2Requested = handleUd2 ? TRUE : FALSE;
    broker.RequestedRax = 0xA55A112233445566ull;
    bool pinned = false;
    bool started = false;
    bool caught = false;
    ULONGLONG observedAddress = 0;
    DWORD observedCode = 0;
    LMV_GUEST_KI_FRAME_SNAPSHOT observedFrame = {};
    GetCurrentProcessorNumberEx(&current);
    affinity.Group = current.Group;
    affinity.Mask = static_cast<KAFFINITY>(1) << current.Number;
    if (waitForContinue)
    {
        brokerDevice = OpenGuestDevice(FILE_SHARE_READ | FILE_SHARE_WRITE);
        if (brokerDevice == INVALID_HANDLE_VALUE)
            goto Cleanup;
        broker.Device = brokerDevice;
        foreignBrokerDevice = OpenGuestDevice(FILE_SHARE_READ | FILE_SHARE_WRITE);
        if (foreignBrokerDevice == INVALID_HANDLE_VALUE)
            goto Cleanup;
        broker.ForeignDevice = foreignBrokerDevice;
        broker.StartEvent = startEvent;
        brokerThread = CreateThread(nullptr, 0, KiDr3EventWorkerEntry,
            &broker, 0, nullptr);
        if (brokerThread == nullptr)
            goto Cleanup;
    }
    if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, &previous))
        goto Cleanup;
    pinned = true;
    if (!SetThreadGroupAffinity(thread, &affinity, nullptr) ||
        ResumeThread(thread) == static_cast<DWORD>(-1))
        goto Cleanup;
    started = true;
    if (WaitForSingleObject(startEvent, 5000) == WAIT_OBJECT_0)
    {
        if (handleUd2)
        {
            bool unexpectedException = false;
            caught = TriggerKiHandledProbe(&unexpectedException);
            if (unexpectedException)
                observedCode = EXCEPTION_ILLEGAL_INSTRUCTION;
        }
        else
            caught = TriggerKiProbeException(hardware, &observedAddress, &observedCode,
                captureFrame ? &observedFrame : nullptr);
    }
    SetEvent(doneEvent);
    if (waitForContinue)
    {
        brokerWaitStatus = WaitForSingleObject(brokerThread, 7000);
        if (brokerWaitStatus != WAIT_OBJECT_0)
            ExitProcess(5);
    }
    waitStatus = WaitForSingleObject(thread, 8000);
    if (waitStatus != WAIT_OBJECT_0)
        ExitProcess(5);
    if (worker.Success && worker.Returned == sizeof(worker.Result) &&
        worker.Result.Status == 0 && worker.Result.Step == 4 && caught &&
        worker.Result.KiAddress != 0 && worker.Result.PrologueVerified == 1 &&
        worker.Result.HitsAfter == worker.Result.HitsBefore + 1 &&
        worker.Result.LastRip == worker.Result.KiAddress &&
        (worker.Result.LastDr6 & 8) != 0 && worker.Result.LastCr3 != 0 &&
        (!redirect || (worker.Result.RedirectStub != 0 &&
         worker.Result.StubHitsAfter == worker.Result.StubHitsBefore + 1)) &&
        (!inspect || (worker.Result.InspectStatus == 0 &&
         worker.Result.InspectReason == 7 &&
         worker.Result.InspectProcessId == GetCurrentProcessId() &&
         worker.Result.InspectThreadId == GetCurrentThreadId() &&
         (worker.Result.InspectExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
          worker.Result.InspectExceptionCode == LMV_KI_EXCEPTION_INTERNAL_INVALID_OP) &&
         worker.Result.InspectFirstChance == 1 &&
         (handleUd2 ? (observedCode == 0 &&
          worker.Result.InspectExceptionAddress == broker.Event.Frame.Rip) :
          (observedCode == EXCEPTION_ILLEGAL_INSTRUCTION &&
           worker.Result.InspectExceptionAddress == observedAddress &&
           observedAddress != 0 &&
           observedAddress < 0x0000800000000000ull)))) &&
        (!captureFrame || (worker.Result.FrameStatus == 0 &&
         worker.Result.FrameReason == 12 &&
         (handleUd2 ? (worker.Result.Frame.Rip == broker.Event.Frame.Rip + 2 &&
          worker.Result.Frame.Rsp == broker.Event.Frame.Rsp &&
          worker.Result.Frame.Rsp != 0) :
          (worker.Result.Frame.Rip == observedFrame.Rip &&
           worker.Result.Frame.Rip == observedAddress &&
           worker.Result.Frame.Rsp == observedFrame.Rsp &&
           worker.Result.Frame.Rsp != 0 &&
           worker.Result.Frame.Rbp == observedFrame.Rbp)))))
    {
        if (!waitForContinue || (brokerWaitStatus == WAIT_OBJECT_0 &&
            broker.Received && broker.Continued &&
            broker.WrongSequenceRejected && broker.DuplicateDeliveryRejected &&
            broker.ForeignHandleRejected &&
            (!writeRax || (broker.RaxWrongSequenceRejected &&
             broker.RaxForeignHandleRejected && broker.RaxWriteBytes ==
             sizeof(LMV_GUEST_KI_DR3_SET_RAX))) &&
            broker.EventBytes == sizeof(broker.Event) &&
            broker.ContinueBytes == sizeof(broker.Continuation) &&
            broker.Event.State == LMV_GUEST_STUB_STATE_PENDING &&
            broker.Continuation.State == LMV_GUEST_STUB_STATE_CONTINUED &&
            broker.Event.ProcessId == GetCurrentProcessId() &&
            broker.Event.ThreadId == GetCurrentThreadId() &&
            (handleUd2 ? (broker.Event.ExceptionAddress == broker.Event.Frame.Rip &&
             broker.HandledWithoutRipRejected && broker.RipWriteSucceeded &&
             broker.RipWriteBytes == sizeof(LMV_GUEST_KI_DR3_SET_RIP) &&
             broker.Continuation.Disposition == LMV_GUEST_STUB_DISPOSITION_HANDLED) :
             (broker.Event.ExceptionAddress == observedAddress &&
              broker.Event.Frame.Rip == observedFrame.Rip &&
              broker.Event.Frame.Rsp == observedFrame.Rsp &&
              broker.Event.Frame.Rbp == observedFrame.Rbp)) &&
            broker.Event.Sequence == worker.Result.EventSequence &&
            worker.Result.EventStatus == 0))
            result = 0;
        if (result == 0 && writeRax &&
            (!broker.RaxWriteSucceeded ||
             worker.Result.Frame.Rax != broker.RequestedRax ||
             observedFrame.Rax != broker.RequestedRax))
            result = 5;
    }
    std::printf("guest ki-dr3-probe: %s mode=%s status=%08lx step=%lu ki=%llx hit=%llu:%llu rip=%llx dr6=%llx cr3=%llx armed-after=%llx dr7-after=%llx caught=%u wait=%lu win32=%lu\n",
        result == 0 ? "PASS" : "FAIL", hardware ? "ud2" : "raise", worker.Result.Status,
        worker.Result.Step, worker.Result.KiAddress,
        worker.Result.HitsBefore, worker.Result.HitsAfter,
        worker.Result.LastRip, worker.Result.LastDr6,
        worker.Result.LastCr3, worker.Result.ArmedTargetAfter,
        worker.Result.ArmedDr7After, caught ? 1u : 0u,
        waitStatus, worker.Error);
    if (redirect)
        std::printf("guest ki-dr3-redirect: %s stub=%llx calls=%llu:%llu event-wait=%u\n",
            worker.Result.RedirectStub != 0 &&
            worker.Result.HitsAfter == worker.Result.HitsBefore + 1 &&
            worker.Result.StubHitsAfter == worker.Result.StubHitsBefore + 1 ?
                "PASS" : "FAIL", worker.Result.RedirectStub,
            worker.Result.StubHitsBefore, worker.Result.StubHitsAfter,
            waitForContinue ? 1u : 0u);
    if (inspect)
        std::printf("guest ki-dr3-inspect: %s copy-status=%08lx reason=%lu pid=%lu tid=%lu raw-code=%08lx raw-address=%llx first=%lu user-code=%08lx user-address=%llx expected-pid=%lu expected-tid=%lu (read-only)\n",
            result == 0 ? "PASS" : "FAIL", worker.Result.InspectStatus,
            worker.Result.InspectReason, worker.Result.InspectProcessId,
            worker.Result.InspectThreadId, worker.Result.InspectExceptionCode,
            worker.Result.InspectExceptionAddress, worker.Result.InspectFirstChance,
            observedCode, observedAddress, GetCurrentProcessId(), GetCurrentThreadId());
    if (captureFrame)
    {
        const LMV_GUEST_KI_FRAME_SNAPSHOT& frame = worker.Result.Frame;
        std::printf("guest ki-dr3-frame: %s status=%08lx reason=%lu rip={guest=%llx user=%llx} rsp={guest=%llx user=%llx} rflags={guest=%llx user=%llx} (read-only)\n",
            result == 0 ? "PASS" : "FAIL", worker.Result.FrameStatus,
            worker.Result.FrameReason, frame.Rip, observedFrame.Rip,
            frame.Rsp, observedFrame.Rsp, frame.Rflags, observedFrame.Rflags);
        std::printf("guest ki-dr3-frame-gpr: guest={rax=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx r10=%llx r11=%llx rbx=%llx rbp=%llx rsi=%llx rdi=%llx r12=%llx r13=%llx r14=%llx r15=%llx}\n",
            frame.Rax, frame.Rcx, frame.Rdx, frame.R8, frame.R9, frame.R10,
            frame.R11, frame.Rbx, frame.Rbp, frame.Rsi, frame.Rdi,
            frame.R12, frame.R13, frame.R14, frame.R15);
        std::printf("guest ki-dr3-frame-gpr: user={rax=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx r10=%llx r11=%llx rbx=%llx rbp=%llx rsi=%llx rdi=%llx r12=%llx r13=%llx r14=%llx r15=%llx}\n",
            observedFrame.Rax, observedFrame.Rcx, observedFrame.Rdx,
            observedFrame.R8, observedFrame.R9, observedFrame.R10,
            observedFrame.R11, observedFrame.Rbx, observedFrame.Rbp,
            observedFrame.Rsi, observedFrame.Rdi, observedFrame.R12,
            observedFrame.R13, observedFrame.R14, observedFrame.R15);
    }
    if (waitForContinue)
        std::printf("guest ki-dr3-event: %s status=%08lx sequence=%lu broker={wait=%lu received=%u continued=%u wrong-sequence-rejected=%u duplicate-rejected=%u foreign-handle-rejected=%u bytes=%lu/%lu win32=%lu} event={state=%lu pid=%lu tid=%lu code=%08lx address=%llx}\n",
            result == 0 ? "PASS" : "FAIL", worker.Result.EventStatus,
            worker.Result.EventSequence, brokerWaitStatus,
            broker.Received ? 1u : 0u, broker.Continued ? 1u : 0u,
            broker.WrongSequenceRejected ? 1u : 0u,
            broker.DuplicateDeliveryRejected ? 1u : 0u,
            broker.ForeignHandleRejected ? 1u : 0u,
            broker.EventBytes, broker.ContinueBytes, broker.Error,
            broker.Event.State, broker.Event.ProcessId, broker.Event.ThreadId,
            broker.Event.ExceptionCode, broker.Event.ExceptionAddress);
    if (handleUd2)
        std::printf("guest ki-dr3-handled: %s set-rip=%u premature-rejected=%u disposition=%lu entry=%llx resumed=%llx user-filter=%u bytes=%lu (one-shot)\n",
            result == 0 ? "PASS" : "FAIL", broker.RipWriteSucceeded ? 1u : 0u,
            broker.HandledWithoutRipRejected ? 1u : 0u,
            broker.Continuation.Disposition, broker.Event.Frame.Rip,
            worker.Result.Frame.Rip, observedCode != 0 ? 1u : 0u,
            broker.RipWriteBytes);
    if (writeRax)
        std::printf("guest ki-dr3-rax: %s write=%u original=%llx requested=%llx frame=%llx user=%llx (original Ki dispatch continues)\n",
            result == 0 ? "PASS" : "FAIL", broker.RaxWriteSucceeded ? 1u : 0u,
            broker.Event.Frame.Rax, broker.RequestedRax,
            worker.Result.Frame.Rax, observedFrame.Rax);
    if (result == 0)
        std::printf("guest ki-dr3-snapshot: cpu=%hu:%hu rcx=%llx rdx=%llx r8=%llx r9=%llx rsp=%llx rflags=%llx rax=%llx rbp=%llx\n",
            worker.Result.ProcessorGroup, worker.Result.ProcessorNumber,
            worker.Result.LastRcx, worker.Result.LastRdx,
            worker.Result.LastR8, worker.Result.LastR9,
            worker.Result.LastRsp, worker.Result.LastRflags,
            worker.Result.LastRax, worker.Result.LastRbp);
    if (result == 0)
    {
        std::printf("guest ki-dr3-stack-candidates: rsp=%llx", worker.Result.LastRsp);
        PrintKiDr3StackCandidate("rcx", worker.Result.LastRcx, worker.Result.LastRsp);
        PrintKiDr3StackCandidate("rdx", worker.Result.LastRdx, worker.Result.LastRsp);
        PrintKiDr3StackCandidate("r8", worker.Result.LastR8, worker.Result.LastRsp);
        PrintKiDr3StackCandidate("r9", worker.Result.LastR9, worker.Result.LastRsp);
        PrintKiDr3StackCandidate("rax", worker.Result.LastRax, worker.Result.LastRsp);
        PrintKiDr3StackCandidate("rbp", worker.Result.LastRbp, worker.Result.LastRsp);
        std::printf(" (offsets only; no stack bytes captured)\n");
    }

Cleanup:
    if (brokerThread != nullptr)
    {
        SetEvent(startEvent);
        if (WaitForSingleObject(brokerThread, 7000) != WAIT_OBJECT_0)
            ExitProcess(5);
        CloseHandle(brokerThread);
    }
    if (brokerDevice != INVALID_HANDLE_VALUE)
        CloseHandle(brokerDevice);
    if (foreignBrokerDevice != INVALID_HANDLE_VALUE)
        CloseHandle(foreignBrokerDevice);
    if (!started)
        TerminateThread(thread, 5);
    if (WaitForSingleObject(thread, 1000) != WAIT_OBJECT_0)
        ExitProcess(5);
    if (pinned)
        SetThreadGroupAffinity(GetCurrentThread(), &previous, nullptr);
    CloseHandle(thread);
    CloseHandle(startEvent);
    CloseHandle(doneEvent);
    return result;
}
static int RunKiDr3AllCpusEventSmoke(HANDLE device)
{
    const WORD groupCount = GetActiveProcessorGroupCount();
    DWORD expected = 0;
    for (WORD group = 0; group < groupCount; ++group)
        expected += GetActiveProcessorCount(group);
    if (expected == 0)
        return 5;

    GROUP_AFFINITY previous = {};
    bool pinned = false;
    DWORD tested = 0;
    int result = 0;
    for (WORD group = 0; group < groupCount && result == 0; ++group)
    {
        const DWORD count = GetActiveProcessorCount(group);
        for (WORD number = 0; number < count && result == 0; ++number)
        {
            GROUP_AFFINITY affinity = {};
            affinity.Group = group;
            affinity.Mask = static_cast<KAFFINITY>(1) << number;
            if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity,
                pinned ? nullptr : &previous))
            {
                result = 5;
                break;
            }
            pinned = true;
            for (unsigned round = 1; round <= 2; ++round)
            {
                if (RunKiDr3Probe(device, true, true, true, true, true) != 0)
                {
                    std::printf("guest ki-dr3-all-cpus-event: FAIL cpu=%hu:%hu round=%u\n",
                        group, number, round);
                    result = 5;
                    break;
                }
                ++tested;
                std::printf("guest ki-dr3-all-cpus-event: cpu=%hu:%hu round=%u PASS\n",
                    group, number, round);
            }
        }
    }
    if (pinned && !SetThreadGroupAffinity(GetCurrentThread(), &previous, nullptr))
        result = 5;
    std::printf("guest ki-dr3-all-cpus-event: %s tested=%lu expected=%lu rounds=2 mode=serial-one-shot\n",
        result == 0 && tested == expected * 2 ? "PASS" : "FAIL",
        tested, expected * 2);
    return result == 0 && tested == expected * 2 ? 0 : 5;
}
struct KiQueueProducer
{
    HANDLE Device;
    LMV_GUEST_KI_QUEUE_PRODUCE Result;
    DWORD Returned;
    DWORD Error;
    BOOL Success;
};

static DWORD WINAPI KiQueueProducerEntry(LPVOID context)
{
    KiQueueProducer* producer = static_cast<KiQueueProducer*>(context);
    producer->Result.Version = LMV_CPUID_PROTOCOL_VERSION;
    producer->Result.Size = sizeof(producer->Result);
    producer->Success = DeviceIoControl(producer->Device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_QUEUE_PRODUCE),
        &producer->Result, sizeof(producer->Result),
        &producer->Result, sizeof(producer->Result),
        &producer->Returned, nullptr);
    producer->Error = producer->Success ? 0 : GetLastError();
    return 0;
}

static int RunKiQueueConcurrencySmoke(HANDLE owner)
{
    constexpr DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
    HANDLE producerDevices[2] = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };
    HANDLE consumerDevices[2] = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };
    HANDLE producerThreads[2] = {};
    KiQueueProducer producers[2] = {};
    LMV_GUEST_KI_DR3_EVENT events[2] = {};
    LMV_GUEST_KI_QUEUE_CONTROL session = {};
    session.Version = LMV_CPUID_PROTOCOL_VERSION;
    session.Size = sizeof(session);
    session.Operation = LMV_GUEST_KI_QUEUE_BEGIN;
    DWORD returned = 0;
    bool active = DeviceIoControl(owner,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_QUEUE_CONTROL),
        &session, sizeof(session), &session, sizeof(session), &returned, nullptr) &&
        returned == sizeof(session) && session.Status == 0;
    bool delivered = active;
    bool crossRejected = false;
    bool syntheticRaxRejected = false;
    bool syntheticHandledRejected = false;
    bool continued = false;
    bool producersFinished = false;
    bool ended = false;
    DWORD producerIds[2] = {};
    DWORD deliveries = 0;
    if (active)
    {
        for (unsigned index = 0; index < 2; ++index)
        {
            producerDevices[index] = OpenGuestDevice(shareMode);
            consumerDevices[index] = OpenGuestDevice(shareMode);
            if (producerDevices[index] == INVALID_HANDLE_VALUE ||
                consumerDevices[index] == INVALID_HANDLE_VALUE)
            {
                delivered = false;
                break;
            }
            producers[index].Device = producerDevices[index];
            producerThreads[index] = CreateThread(nullptr, 0, KiQueueProducerEntry,
                &producers[index], 0, &producerIds[index]);
            if (producerThreads[index] == nullptr)
            {
                delivered = false;
                break;
            }
        }
        for (unsigned index = 0; index < 2 && delivered; ++index)
        {
            LMV_GUEST_STUB_WAIT_REQUEST request = {};
            request.Version = LMV_CPUID_PROTOCOL_VERSION;
            request.Size = sizeof(request);
            request.TimeoutMilliseconds = 1500;
            delivered = DeviceIoControl(consumerDevices[index],
                static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_WAIT_EVENT),
                &request, sizeof(request), &events[index], sizeof(events[index]),
                &returned, nullptr) && returned == sizeof(events[index]) &&
                events[index].State == LMV_GUEST_STUB_STATE_PENDING &&
                events[index].ProcessId == GetCurrentProcessId() &&
                events[index].ExceptionCode == 0xE0424D56u;
            if (delivered)
                ++deliveries;
        }
        if (delivered)
        {
            delivered = events[0].Sequence != events[1].Sequence &&
                events[0].ThreadId != events[1].ThreadId &&
                ((events[0].ThreadId == producerIds[0] &&
                  events[1].ThreadId == producerIds[1]) ||
                 (events[0].ThreadId == producerIds[1] &&
                  events[1].ThreadId == producerIds[0]));
        }
        if (delivered)
        {
            LMV_GUEST_KI_DR3_SET_RAX setRax = {};
            setRax.Version = LMV_CPUID_PROTOCOL_VERSION;
            setRax.Size = sizeof(setRax);
            setRax.Sequence = events[0].Sequence;
            setRax.Rax = 0xA55A112233445566ull;
            syntheticRaxRejected = !DeviceIoControl(consumerDevices[0],
                static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_SET_RAX),
                &setRax, sizeof(setRax), &setRax, sizeof(setRax),
                &returned, nullptr);
            LMV_GUEST_STUB_CONTINUE handled = {};
            handled.Version = LMV_CPUID_PROTOCOL_VERSION;
            handled.Size = sizeof(handled);
            handled.Sequence = events[0].Sequence;
            handled.Disposition = LMV_GUEST_STUB_DISPOSITION_HANDLED;
            syntheticHandledRejected = !DeviceIoControl(consumerDevices[0],
                static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_CONTINUE),
                &handled, sizeof(handled), &handled, sizeof(handled),
                &returned, nullptr);
            LMV_GUEST_STUB_CONTINUE cross = {};
            cross.Version = LMV_CPUID_PROTOCOL_VERSION;
            cross.Size = sizeof(cross);
            cross.Sequence = events[0].Sequence;
            cross.Disposition = LMV_GUEST_STUB_DISPOSITION_CONTINUE;
            crossRejected = !DeviceIoControl(consumerDevices[1],
                static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_CONTINUE),
                &cross, sizeof(cross), &cross, sizeof(cross), &returned, nullptr);
            continued = crossRejected;
            for (unsigned index = 0; index < 2 && continued; ++index)
            {
                LMV_GUEST_STUB_CONTINUE request = {};
                request.Version = LMV_CPUID_PROTOCOL_VERSION;
                request.Size = sizeof(request);
                request.Sequence = events[index].Sequence;
                request.Disposition = LMV_GUEST_STUB_DISPOSITION_CONTINUE;
                continued = DeviceIoControl(consumerDevices[index],
                    static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_CONTINUE),
                    &request, sizeof(request), &request, sizeof(request),
                    &returned, nullptr) && returned == sizeof(request) &&
                    request.State == LMV_GUEST_STUB_STATE_CONTINUED;
            }
        }
        session.Operation = LMV_GUEST_KI_QUEUE_END;
        ended = DeviceIoControl(owner,
            static_cast<DWORD>(IOCTL_LMV_GUEST_KI_QUEUE_CONTROL),
            &session, sizeof(session), &session, sizeof(session), &returned, nullptr) &&
            returned == sizeof(session) && session.Status == 0;
    }
    for (unsigned index = 0; index < 2; ++index)
    {
        if (producerThreads[index] != nullptr)
        {
            if (WaitForSingleObject(producerThreads[index], 5000) != WAIT_OBJECT_0)
                ExitProcess(5);
            CloseHandle(producerThreads[index]);
        }
        if (producerDevices[index] != INVALID_HANDLE_VALUE)
            CloseHandle(producerDevices[index]);
        if (consumerDevices[index] != INVALID_HANDLE_VALUE)
            CloseHandle(consumerDevices[index]);
    }
    producersFinished = producerThreads[0] != nullptr && producerThreads[1] != nullptr &&
        producers[0].Success && producers[1].Success &&
        producers[0].Returned == sizeof(producers[0].Result) &&
        producers[1].Returned == sizeof(producers[1].Result) &&
        producers[0].Result.Status == 0 && producers[1].Result.Status == 0 &&
        producers[0].Result.ThreadId == producerIds[0] &&
        producers[1].Result.ThreadId == producerIds[1] &&
        producers[0].Result.Sequence != producers[1].Result.Sequence;
    const bool passed = ended && delivered && deliveries == 2 &&
        crossRejected && syntheticRaxRejected && syntheticHandledRejected &&
        continued && producersFinished;
    std::printf("guest ki-queue-concurrency: %s delivered=%lu cross-rejected=%u continued=%u producers=%u sequences=%lu,%lu threads=%lu,%lu mode=synthetic-no-host-hook\n",
        passed ? "PASS" : "FAIL", deliveries, crossRejected ? 1u : 0u,
        continued ? 1u : 0u, producersFinished ? 1u : 0u,
        events[0].Sequence, events[1].Sequence,
        events[0].ThreadId, events[1].ThreadId);
    return passed ? 0 : 5;
}

static int RunKiQueueCancelSmoke(HANDLE owner)
{
    constexpr DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
    HANDLE producerDevice = INVALID_HANDLE_VALUE;
    HANDLE consumerDevice = INVALID_HANDLE_VALUE;
    HANDLE producerThread = nullptr;
    KiQueueProducer producer = {};
    LMV_GUEST_KI_QUEUE_CONTROL session = {};
    session.Version = LMV_CPUID_PROTOCOL_VERSION;
    session.Size = sizeof(session);
    session.Operation = LMV_GUEST_KI_QUEUE_BEGIN;
    DWORD returned = 0;
    const bool started = DeviceIoControl(owner,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_QUEUE_CONTROL),
        &session, sizeof(session), &session, sizeof(session), &returned, nullptr) &&
        returned == sizeof(session) && session.Status == 0;
    bool received = false;
    bool ended = false;
    bool cancelled = false;
    LMV_GUEST_KI_DR3_EVENT event = {};
    DWORD producerId = 0;
    if (started)
    {
        producerDevice = OpenGuestDevice(shareMode);
        consumerDevice = OpenGuestDevice(shareMode);
        if (producerDevice != INVALID_HANDLE_VALUE &&
            consumerDevice != INVALID_HANDLE_VALUE)
        {
            producer.Device = producerDevice;
            producerThread = CreateThread(nullptr, 0, KiQueueProducerEntry,
                &producer, 0, &producerId);
            if (producerThread != nullptr)
            {
                LMV_GUEST_STUB_WAIT_REQUEST request = {};
                request.Version = LMV_CPUID_PROTOCOL_VERSION;
                request.Size = sizeof(request);
                request.TimeoutMilliseconds = 1500;
                received = DeviceIoControl(consumerDevice,
                    static_cast<DWORD>(IOCTL_LMV_GUEST_KI_DR3_WAIT_EVENT),
                    &request, sizeof(request), &event, sizeof(event),
                    &returned, nullptr) && returned == sizeof(event) &&
                    event.State == LMV_GUEST_STUB_STATE_PENDING &&
                    event.ThreadId == producerId;
            }
        }
        if (consumerDevice != INVALID_HANDLE_VALUE)
        {
            CloseHandle(consumerDevice);
            consumerDevice = INVALID_HANDLE_VALUE;
        }
        if (producerThread != nullptr)
        {
            const DWORD wait = WaitForSingleObject(producerThread, 4000);
            if (wait != WAIT_OBJECT_0)
            {
                session.Operation = LMV_GUEST_KI_QUEUE_END;
                DeviceIoControl(owner,
                    static_cast<DWORD>(IOCTL_LMV_GUEST_KI_QUEUE_CONTROL),
                    &session, sizeof(session), &session, sizeof(session), &returned, nullptr);
                if (WaitForSingleObject(producerThread, 4000) != WAIT_OBJECT_0)
                    ExitProcess(5);
            }
            cancelled = wait == WAIT_OBJECT_0 && producer.Success &&
                producer.Returned == sizeof(producer.Result) &&
                producer.Result.Status == static_cast<ULONG>(0xC0000120L) &&
                producer.Result.Sequence == event.Sequence &&
                producer.Result.ThreadId == producerId;
            CloseHandle(producerThread);
        }
        session.Operation = LMV_GUEST_KI_QUEUE_END;
        ended = DeviceIoControl(owner,
            static_cast<DWORD>(IOCTL_LMV_GUEST_KI_QUEUE_CONTROL),
            &session, sizeof(session), &session, sizeof(session), &returned, nullptr) &&
            returned == sizeof(session) && session.Status == 0;
    }
    if (producerDevice != INVALID_HANDLE_VALUE)
        CloseHandle(producerDevice);
    if (consumerDevice != INVALID_HANDLE_VALUE)
        CloseHandle(consumerDevice);
    const bool passed = started && received && cancelled && ended;
    std::printf("guest ki-queue-cancel: %s received=%u cancelled=%u ended=%u sequence=%lu thread=%lu status=%08lx mode=synthetic-no-host-hook\n",
        passed ? "PASS" : "FAIL", received ? 1u : 0u, cancelled ? 1u : 0u,
        ended ? 1u : 0u, event.Sequence, producerId, producer.Result.Status);
    return passed ? 0 : 5;
}

static int RunKiExecMirrorSmoke(HANDLE device)
{
    LMV_GUEST_KI_EXEC_MIRROR_SMOKE result = {};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_EXEC_MIRROR_SMOKE),
        nullptr, 0, &result, sizeof(result), &returned, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    const bool pass = ok && returned == sizeof(result) && result.Status == 0 &&
        result.Stage == 4 && result.ProtectionApplied == 1 &&
        result.ObservedValue == result.ExpectedValue &&
        result.VirtualAddress != 0 && result.PhysicalAddress != 0;
    std::printf("guest ki-exec-mirror-smoke: %s status=%08lx stage=%lu va=%llx gpa=%llx expected=%llx observed=%llx executable=%lu win32=%lu\n",
        pass ? "PASS" : "FAIL", result.Status, result.Stage,
        result.VirtualAddress, result.PhysicalAddress, result.ExpectedValue,
        result.ObservedValue, result.ProtectionApplied, error);
    return pass ? 0 : 5;
}
static int RunExecProbeSmoke(HANDLE device)
{
    unsigned char* original = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    unsigned char* mirror = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    unsigned char* hook = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (original == nullptr || mirror == nullptr || hook == nullptr)
    {
        std::printf("guest exec-probe: FAIL allocation=%lu\n", GetLastError());
        if (original != nullptr) VirtualFree(original, 0, MEM_RELEASE);
        if (mirror != nullptr) VirtualFree(mirror, 0, MEM_RELEASE);
        if (hook != nullptr) VirtualFree(hook, 0, MEM_RELEASE);
        return 5;
    }

    original[0] = 0xB8; original[1] = 0x11; original[5] = 0xC3;
    original[0x100] = 0xB8; original[0x101] = 0x22; original[0x105] = 0xC3;
    std::memcpy(mirror, original, 4096);
    hook[0] = 0xB8; hook[1] = 0x33; hook[5] = 0xC3;
    DWORD protection = 0;
    if (!VirtualProtect(original, 4096, PAGE_EXECUTE_READ, &protection) ||
        !VirtualProtect(mirror, 4096, PAGE_EXECUTE_READ, &protection) ||
        !VirtualProtect(hook, 4096, PAGE_EXECUTE_READ, &protection))
    {
        std::printf("guest exec-probe: FAIL protect=%lu\n", GetLastError());
        VirtualFree(original, 0, MEM_RELEASE);
        VirtualFree(mirror, 0, MEM_RELEASE);
        VirtualFree(hook, 0, MEM_RELEASE);
        return 5;
    }

    LMV_GUEST_EXEC_PROBE probe = {};
    probe.Version = LMV_CPUID_PROTOCOL_VERSION;
    probe.Size = sizeof(probe);
    probe.Operation = LMV_EXEC_PROBE_ARM;
    probe.OriginalPage = reinterpret_cast<ULONGLONG>(original);
    probe.MirrorPage = reinterpret_cast<ULONGLONG>(mirror);
    probe.HookEntry = reinterpret_cast<ULONGLONG>(hook);
    DWORD returned = 0;
    BOOL armed = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_EXEC_PROBE),
                                 &probe, sizeof(probe), &probe, sizeof(probe),
                                 &returned, nullptr);
    DWORD armError = armed ? 0 : GetLastError();
    unsigned entryValue = 0, otherValue = 0, restoredValue = 0;
    bool readOriginal = false;
    if (armed && probe.Status == 0)
    {
        readOriginal = original[1] == 0x11 && original[0x101] == 0x22;
        using TestFunction = unsigned (*)();
        entryValue = reinterpret_cast<TestFunction>(original)();
        otherValue = reinterpret_cast<TestFunction>(original + 0x100)();
        probe.Operation = LMV_EXEC_PROBE_DISARM;
        returned = 0;
        BOOL disarmed = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_EXEC_PROBE),
                                        &probe, sizeof(probe), &probe, sizeof(probe),
                                        &returned, nullptr);
        if (disarmed && probe.Status == 0)
        {
            restoredValue = reinterpret_cast<TestFunction>(original)();
            std::printf("guest exec-probe: %s read=%s entry=%x non-entry=%x restored=%x processors=%lu/%lu GPA=%llx\n",
                        readOriginal && entryValue == 0x33 && otherValue == 0x22 &&
                        restoredValue == 0x11 ? "PASS" : "FAIL",
                        readOriginal ? "original" : "modified", entryValue,
                        otherValue, restoredValue, probe.ProcessorsSeen,
                        probe.ProcessorsExpected, probe.GuestPhysicalAddress);
            VirtualFree(original, 0, MEM_RELEASE);
            VirtualFree(mirror, 0, MEM_RELEASE);
            VirtualFree(hook, 0, MEM_RELEASE);
            return readOriginal && entryValue == 0x33 && otherValue == 0x22 &&
                   restoredValue == 0x11 ? 0 : 5;
        }
        std::printf("guest exec-probe: FAIL disarm status=%08lx win32=%lu; pages retained, revert VM snapshot\n",
                    probe.Status, GetLastError());
        return 5;
    }
    if (probe.GuestPhysicalAddress != 0)
    {
        probe.Operation = LMV_EXEC_PROBE_DISARM;
        returned = 0;
        BOOL disarmed = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_EXEC_PROBE),
                                        &probe, sizeof(probe), &probe, sizeof(probe),
                                        &returned, nullptr);
        if (!disarmed && probe.Status != static_cast<ULONG>(0xC000000Du))
        {
            std::printf("guest exec-probe: FAIL arm=%lu disarm=%08lx; pages retained, revert VM snapshot\n",
                        armError, probe.Status);
            return 5;
        }
    }
    std::printf("guest exec-probe: FAIL arm status=%08lx win32=%lu\n", probe.Status, armError);
    VirtualFree(original, 0, MEM_RELEASE);
    VirtualFree(mirror, 0, MEM_RELEASE);
    VirtualFree(hook, 0, MEM_RELEASE);
    return 5;
}

struct ExecProbeWorker
{
    const unsigned char* Original;
    HANDLE StartEvent;
    WORD Group;
    BYTE Number;
    bool Armed;
    bool Relocated;
    unsigned Mismatches;
    unsigned RelativeValue;
    unsigned CrossValue;
};

static DWORD WINAPI RunExecProbeWorker(void* parameter)
{
    ExecProbeWorker* worker = static_cast<ExecProbeWorker*>(parameter);
    if (WaitForSingleObject(worker->StartEvent, INFINITE) != WAIT_OBJECT_0)
    {
        ++worker->Mismatches;
        return 1;
    }
    PROCESSOR_NUMBER processor = {};
    GetCurrentProcessorNumberEx(&processor);
    if (processor.Group != worker->Group || processor.Number != worker->Number)
        ++worker->Mismatches;

    using TestFunction = unsigned (*)();
    const auto entry = reinterpret_cast<TestFunction>(worker->Original);
    const auto other = reinterpret_cast<TestFunction>(worker->Original + 0x100);
    const auto relative = reinterpret_cast<TestFunction>(worker->Original + 0x200);
    const auto boundary = reinterpret_cast<TestFunction>(worker->Original + 0xffd);
    for (unsigned iteration = 0; iteration < 8; ++iteration)
    {
        if (worker->Original[1] != 0x11 || worker->Original[0x300] != 0x55 ||
            worker->Original[0xffd] != 0xB8 || worker->Original[0x1002] != 0xC3)
            ++worker->Mismatches;
        unsigned entryValue = entry();
        unsigned otherValue = other();
        worker->RelativeValue = relative();
        worker->CrossValue = boundary();
        if (entryValue != (worker->Armed ? 0x33u : 0x11u) ||
            otherValue != 0x22u || worker->CrossValue != 0x44u ||
            worker->RelativeValue != (worker->Armed && !worker->Relocated ? 0x66u : 0x55u))
            ++worker->Mismatches;
    }
    return 0;
}

static bool RunExecProbeWorkers(const unsigned char* original, bool armed,
                                unsigned* processors, unsigned* mismatches,
                                unsigned* relativeValue, unsigned* crossValue,
                                bool relocated = false)
{
    unsigned count = 0;
    const WORD groupCount = GetActiveProcessorGroupCount();
    for (WORD group = 0; group < groupCount; ++group)
        count += GetActiveProcessorCount(group);
    *processors = count;
    *mismatches = 0;
    if (count == 0)
        return false;

    std::vector<ExecProbeWorker> workers(count);
    std::vector<HANDLE> threads;
    threads.reserve(count);
    HANDLE startEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (startEvent == nullptr)
        return false;
    bool created = true;
    unsigned index = 0;
    for (WORD group = 0; group < groupCount && created; ++group)
    {
        const DWORD active = GetActiveProcessorCount(group);
        for (DWORD number = 0; number < active; ++number)
        {
            ExecProbeWorker& worker = workers[index++];
            worker.Original = original;
            worker.StartEvent = startEvent;
            worker.Group = group;
            worker.Number = static_cast<BYTE>(number);
            worker.Armed = armed;
            worker.Relocated = relocated;
            HANDLE thread = CreateThread(nullptr, 0, RunExecProbeWorker,
                                         &worker, 0, nullptr);
            if (thread == nullptr)
            {
                created = false;
                break;
            }
            threads.push_back(thread);
            GROUP_AFFINITY affinity = {};
            affinity.Group = group;
            affinity.Mask = static_cast<KAFFINITY>(1) << number;
            if (!SetThreadGroupAffinity(thread, &affinity, nullptr))
                ++worker.Mismatches;
        }
    }
    SetEvent(startEvent);
    bool joined = true;
    for (HANDLE thread : threads)
    {
        if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0)
            joined = false;
        CloseHandle(thread);
    }
    for (const ExecProbeWorker& worker : workers)
        *mismatches += worker.Mismatches;
    *relativeValue = workers[0].RelativeValue;
    *crossValue = workers[0].CrossValue;
    CloseHandle(startEvent);
    return created && joined && threads.size() == count && *mismatches == 0;
}

static int RunExecProbeStress(HANDLE device)
{
    constexpr SIZE_T regionSize = 8192;
    auto original = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, regionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto mirror = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, regionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto hook = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (original == nullptr || mirror == nullptr || hook == nullptr)
    {
        std::printf("guest exec-probe-stress: FAIL allocation=%lu\n", GetLastError());
        if (original != nullptr) VirtualFree(original, 0, MEM_RELEASE);
        if (mirror != nullptr) VirtualFree(mirror, 0, MEM_RELEASE);
        if (hook != nullptr) VirtualFree(hook, 0, MEM_RELEASE);
        return 5;
    }
    original[0] = 0xB8; original[1] = 0x11; original[5] = 0xC3;
    original[0x100] = 0xB8; original[0x101] = 0x22; original[0x105] = 0xC3;
    original[0x200] = 0x8B; original[0x201] = 0x05;
    original[0x202] = 0xFA; original[0x206] = 0xC3;
    original[0x300] = 0x55;
    original[0xffd] = 0xB8; original[0xffe] = 0x44; original[0x1002] = 0xC3;
    std::memcpy(mirror, original, regionSize);
    mirror[0x300] = 0x66;
    hook[0] = 0xB8; hook[1] = 0x33; hook[5] = 0xC3;
    DWORD oldProtection = 0;
    if (!VirtualProtect(original, regionSize, PAGE_EXECUTE_READ, &oldProtection) ||
        !VirtualProtect(mirror, regionSize, PAGE_EXECUTE_READ, &oldProtection) ||
        !VirtualProtect(hook, 4096, PAGE_EXECUTE_READ, &oldProtection))
    {
        std::printf("guest exec-probe-stress: FAIL protect=%lu\n", GetLastError());
        VirtualFree(original, 0, MEM_RELEASE);
        VirtualFree(mirror, 0, MEM_RELEASE);
        VirtualFree(hook, 0, MEM_RELEASE);
        return 5;
    }

    unsigned processorCount = 0;
    unsigned totalMismatches = 0;
    unsigned relativeValue = 0;
    unsigned boundaryValue = 0;
    bool success = true;
    for (unsigned round = 1; round <= 3; ++round)
    {
        LMV_GUEST_EXEC_PROBE probe = {};
        probe.Version = LMV_CPUID_PROTOCOL_VERSION;
        probe.Size = sizeof(probe);
        probe.Operation = LMV_EXEC_PROBE_ARM;
        probe.OriginalPage = reinterpret_cast<ULONGLONG>(original);
        probe.MirrorPage = reinterpret_cast<ULONGLONG>(mirror);
        probe.HookEntry = reinterpret_cast<ULONGLONG>(hook);
        DWORD returned = 0;
        BOOL armed = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_EXEC_PROBE),
                                     &probe, sizeof(probe), &probe, sizeof(probe),
                                     &returned, nullptr);
        DWORD armError = armed ? 0 : GetLastError();
        if (!armed || probe.Status != 0 || returned != sizeof(probe))
        {
            std::printf("guest exec-probe-stress: FAIL round=%u arm=%08lx win32=%lu\n",
                        round, probe.Status, armError);
            if (probe.GuestPhysicalAddress != 0)
            {
                probe.Operation = LMV_EXEC_PROBE_DISARM;
                returned = 0;
                if (!DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_EXEC_PROBE),
                                     &probe, sizeof(probe), &probe, sizeof(probe),
                                     &returned, nullptr) && probe.Status != 0xC000000Du)
                {
                    std::printf("guest exec-probe-stress: disarm failed; pages retained, revert VM snapshot\n");
                    return 5;
                }
            }
            success = false;
            break;
        }
        unsigned mismatches = 0;
        bool activeOk = RunExecProbeWorkers(original, true, &processorCount,
                                             &mismatches, &relativeValue, &boundaryValue);
        totalMismatches += mismatches;
        probe.Operation = LMV_EXEC_PROBE_DISARM;
        returned = 0;
        BOOL disarmed = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_EXEC_PROBE),
                                        &probe, sizeof(probe), &probe, sizeof(probe),
                                        &returned, nullptr);
        if (!disarmed || probe.Status != 0 || returned != sizeof(probe))
        {
            std::printf("guest exec-probe-stress: FAIL round=%u disarm=%08lx win32=%lu; pages retained, revert VM snapshot\n",
                        round, probe.Status, disarmed ? 0 : GetLastError());
            return 5;
        }
        unsigned restoredMismatches = 0;
        unsigned restoredCount = 0;
        unsigned restoredRelative = 0;
        unsigned restoredBoundary = 0;
        bool restoreOk = RunExecProbeWorkers(original, false, &restoredCount,
                                             &restoredMismatches, &restoredRelative,
                                             &restoredBoundary);
        totalMismatches += restoredMismatches;
        bool roundOk = activeOk && restoreOk && restoredCount == processorCount &&
                       probe.ProcessorsSeen == probe.ProcessorsExpected &&
                       probe.ProcessorsSeen == processorCount;
        std::printf("guest exec-probe-stress: round=%u %s processors=%u/%lu mismatches=%u boundary=%x relative={armed=%x restored=%x}\n",
                    round, roundOk ? "PASS" : "FAIL", processorCount,
                    probe.ProcessorsSeen, mismatches + restoredMismatches,
                    boundaryValue, relativeValue, restoredRelative);
        if (!roundOk)
        {
            success = false;
            break;
        }
    }
    VirtualFree(original, 0, MEM_RELEASE);
    VirtualFree(mirror, 0, MEM_RELEASE);
    VirtualFree(hook, 0, MEM_RELEASE);
    std::printf("guest exec-probe-stress: %s rounds=3 mismatches=%u relative=LIMITED (mirror data, not original data)\n",
                success ? "PASS" : "FAIL", totalMismatches);
    return success ? 0 : 5;
}

static int RunRelocatorSmoke()
{
    auto memory = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 16384, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (memory == nullptr)
    {
        std::printf("guest relocator-smoke: FAIL allocation=%lu\n", GetLastError());
        return 5;
    }
    auto original = memory;
    auto relocated = memory + 8192;
    const unsigned char mov[] = {0x8B, 0x05, 0xFA, 0x00, 0x00, 0x00, 0xC3};
    const unsigned char lea[] = {0x48, 0x8D, 0x05, 0xF9, 0x00, 0x00, 0x00, 0xC3};
    const unsigned char branch[] = {0xEB, 0x00};
    const unsigned char indirect[] = {0x8B, 0x00};
    std::memcpy(original + 0x200, mov, sizeof(mov));
    std::memcpy(original + 0x210, lea, sizeof(lea));
    original[0x300] = 0x55;
    relocated[0x300] = 0x66;
    const auto moveResult = RelocateRipRelativeInstruction(
        original + 0x200, 6, reinterpret_cast<std::uintptr_t>(original + 0x200),
        reinterpret_cast<std::uintptr_t>(relocated + 0x200), relocated + 0x200, 6);
    const auto leaResult = RelocateRipRelativeInstruction(
        original + 0x210, 7, reinterpret_cast<std::uintptr_t>(original + 0x210),
        reinterpret_cast<std::uintptr_t>(relocated + 0x210), relocated + 0x210, 7);
    relocated[0x206] = 0xC3;
    relocated[0x217] = 0xC3;
    unsigned char rejected[8] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
    const auto branchResult = RelocateRipRelativeInstruction(branch, sizeof(branch),
        reinterpret_cast<std::uintptr_t>(original),
        reinterpret_cast<std::uintptr_t>(relocated), rejected, sizeof(rejected));
    const unsigned char call[] = {0xE8, 0x00, 0x00, 0x00, 0x00};
    const auto callResult = RelocateRipRelativeInstruction(call, sizeof(call),
        reinterpret_cast<std::uintptr_t>(original),
        reinterpret_cast<std::uintptr_t>(relocated), rejected, sizeof(rejected));
    const auto truncatedResult = RelocateRipRelativeInstruction(mov, 2,
        reinterpret_cast<std::uintptr_t>(original),
        reinterpret_cast<std::uintptr_t>(relocated), rejected, sizeof(rejected));
    const auto indirectResult = RelocateRipRelativeInstruction(indirect, sizeof(indirect),
        reinterpret_cast<std::uintptr_t>(original),
        reinterpret_cast<std::uintptr_t>(relocated), rejected, sizeof(rejected));
    const auto farResult = RelocateRipRelativeInstruction(
        original + 0x200, 6, reinterpret_cast<std::uintptr_t>(original + 0x200),
        reinterpret_cast<std::uintptr_t>(original) + (1ull << 33), rejected,
        sizeof(rejected));
    bool pass = moveResult.Status == ExecRelocationStatus::Success &&
                leaResult.Status == ExecRelocationStatus::Success &&
                moveResult.Target == reinterpret_cast<std::uintptr_t>(original + 0x300) &&
                leaResult.Target == reinterpret_cast<std::uintptr_t>(original + 0x310) &&
                branchResult.Status == ExecRelocationStatus::UnsupportedControlFlow &&
                callResult.Status == ExecRelocationStatus::UnsupportedControlFlow &&
                truncatedResult.Status == ExecRelocationStatus::InvalidInput &&
                indirectResult.Status == ExecRelocationStatus::UnsupportedInstruction &&
                farResult.Status == ExecRelocationStatus::OutOfRange &&
                rejected[0] == 0xAA;
    DWORD oldProtection = 0;
    if (pass && VirtualProtect(memory, 16384, PAGE_EXECUTE_READ, &oldProtection))
    {
        using ValueFunction = unsigned (*)();
        using AddressFunction = std::uintptr_t (*)();
        pass = reinterpret_cast<ValueFunction>(relocated + 0x200)() == 0x55 &&
               reinterpret_cast<AddressFunction>(relocated + 0x210)() ==
                   reinterpret_cast<std::uintptr_t>(original + 0x310);
    }
    else
        pass = false;
    std::printf("guest relocator-smoke: %s mov=%u lea=%u branch=%u indirect=%u far=%u\n",
                pass ? "PASS" : "FAIL", static_cast<unsigned>(moveResult.Status),
                static_cast<unsigned>(leaResult.Status),
                static_cast<unsigned>(branchResult.Status),
                static_cast<unsigned>(indirectResult.Status),
                static_cast<unsigned>(farResult.Status));
    VirtualFree(memory, 0, MEM_RELEASE);
    return pass ? 0 : 5;
}

static int RunExecProbeRelocSmoke(HANDLE device)
{
    auto memory = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 20480, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (memory == nullptr)
    {
        std::printf("guest exec-probe-reloc-smoke: FAIL allocation=%lu\n", GetLastError());
        return 5;
    }
    auto original = memory;
    auto mirror = memory + 8192;
    auto hook = memory + 16384;
    original[0] = 0xB8; original[1] = 0x11; original[5] = 0xC3;
    original[0x100] = 0xB8; original[0x101] = 0x22; original[0x105] = 0xC3;
    original[0x200] = 0x8B; original[0x201] = 0x05;
    original[0x202] = 0xFA; original[0x206] = 0xC3;
    original[0x300] = 0x55;
    original[0xffd] = 0xB8; original[0xffe] = 0x44; original[0x1002] = 0xC3;
    std::memcpy(mirror, original, 8192);
    mirror[0x300] = 0x66;
    hook[0] = 0xB8; hook[1] = 0x33; hook[5] = 0xC3;
    auto relocated = RelocateRipRelativeInstruction(
        original + 0x200, 6, reinterpret_cast<std::uintptr_t>(original + 0x200),
        reinterpret_cast<std::uintptr_t>(mirror + 0x200), mirror + 0x200, 6);
    bool prepared = relocated.Status == ExecRelocationStatus::Success &&
                    relocated.Target == reinterpret_cast<std::uintptr_t>(original + 0x300);
    DWORD oldProtection = 0;
    prepared = prepared && VirtualProtect(memory, 20480, PAGE_EXECUTE_READ, &oldProtection);
    if (!prepared)
    {
        std::printf("guest exec-probe-reloc-smoke: FAIL prepare=%u win32=%lu\n",
                    static_cast<unsigned>(relocated.Status), GetLastError());
        VirtualFree(memory, 0, MEM_RELEASE);
        return 5;
    }
    LMV_GUEST_EXEC_PROBE probe = {};
    probe.Version = LMV_CPUID_PROTOCOL_VERSION;
    probe.Size = sizeof(probe);
    probe.Operation = LMV_EXEC_PROBE_ARM;
    probe.OriginalPage = reinterpret_cast<ULONGLONG>(original);
    probe.MirrorPage = reinterpret_cast<ULONGLONG>(mirror);
    probe.HookEntry = reinterpret_cast<ULONGLONG>(hook);
    DWORD returned = 0;
    BOOL armed = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_EXEC_PROBE),
                                 &probe, sizeof(probe), &probe, sizeof(probe),
                                 &returned, nullptr);
    if (!armed || probe.Status != 0 || returned != sizeof(probe))
    {
        std::printf("guest exec-probe-reloc-smoke: FAIL arm=%08lx win32=%lu\n",
                    probe.Status, armed ? 0 : GetLastError());
        if (probe.GuestPhysicalAddress != 0)
        {
            probe.Operation = LMV_EXEC_PROBE_DISARM;
            returned = 0;
            if (!DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_EXEC_PROBE),
                                 &probe, sizeof(probe), &probe, sizeof(probe),
                                 &returned, nullptr) && probe.Status != 0xC000000Du)
            {
                std::printf("guest exec-probe-reloc-smoke: disarm failed; page retained, restore VM snapshot\n");
                return 5;
            }
        }
        VirtualFree(memory, 0, MEM_RELEASE);
        return 5;
    }
    unsigned processors = 0;
    unsigned mismatches = 0;
    unsigned relativeValue = 0;
    unsigned crossValue = 0;
    const bool activeOk = RunExecProbeWorkers(original, true, &processors,
                                               &mismatches, &relativeValue, &crossValue,
                                               true);
    probe.Operation = LMV_EXEC_PROBE_DISARM;
    returned = 0;
    BOOL disarmed = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_EXEC_PROBE),
                                    &probe, sizeof(probe), &probe, sizeof(probe),
                                    &returned, nullptr);
    if (!disarmed || probe.Status != 0 || returned != sizeof(probe))
    {
        std::printf("guest exec-probe-reloc-smoke: FAIL disarm=%08lx win32=%lu; page retained, restore VM snapshot\n",
                    probe.Status, disarmed ? 0 : GetLastError());
        return 5;
    }
    unsigned restoredProcessors = 0;
    unsigned restoredMismatches = 0;
    unsigned restoredRelative = 0;
    unsigned restoredCross = 0;
    const bool restoredOk = RunExecProbeWorkers(original, false, &restoredProcessors,
                                                 &restoredMismatches, &restoredRelative,
                                                 &restoredCross, true);
    const bool pass = activeOk && restoredOk && mismatches + restoredMismatches == 0 &&
                      processors == restoredProcessors &&
                      probe.ProcessorsSeen == probe.ProcessorsExpected &&
                      processors == probe.ProcessorsSeen &&
                      relativeValue == 0x55 && restoredRelative == 0x55 &&
                      crossValue == 0x44 && restoredCross == 0x44;
    std::printf("guest exec-probe-reloc-smoke: %s processors=%u/%lu mismatches=%u relative={armed=%x restored=%x} cross=%x\n",
                pass ? "PASS" : "FAIL", processors, probe.ProcessorsSeen,
                mismatches + restoredMismatches, relativeValue, restoredRelative,
                crossValue);
    VirtualFree(memory, 0, MEM_RELEASE);
    return pass ? 0 : 5;
}

static const char* RelocationStatusName(ExecRelocationStatus status)
{
    switch (status)
    {
    case ExecRelocationStatus::Success: return "relocatable";
    case ExecRelocationStatus::InvalidInput: return "invalid";
    case ExecRelocationStatus::UnsupportedInstruction: return "unsupported";
    case ExecRelocationStatus::UnsupportedControlFlow: return "control-flow";
    case ExecRelocationStatus::OutOfRange: return "out-of-range";
    default: return "unknown";
    }
}

static int RunKiPagePreflight()
{
    HANDLE device = OpenGuestDevice();
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("guest ki-page-preflight: FAIL open=%lu\n", GetLastError());
        return 5;
    }
    LMV_GUEST_KI_PAGE_PREFLIGHT result = {};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_KI_PAGE_PREFLIGHT), nullptr, 0,
        &result, sizeof(result), &returned, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(device);
    if (!ok || returned != sizeof(result) || result.Status != 0 ||
        result.Version != LMV_CPUID_PROTOCOL_VERSION || result.Size != sizeof(result) ||
        result.BytesCopied != sizeof(result.PageBytes) ||
        result.EntryOffset >= sizeof(result.PageBytes) ||
        result.RoutineAddress != result.PageAddress + result.EntryOffset)
    {
        std::printf("guest ki-page-preflight: FAIL status=%08lx win32=%lu flags=%08lx bytes=%lu\n",
            result.Status, error, result.Flags, result.BytesCopied);
        return 5;
    }
    ZydisDecoder decoder = {};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                       ZYDIS_STACK_WIDTH_64)))
    {
        std::printf("guest ki-page-preflight: FAIL decoder-init\n");
        return 5;
    }
    constexpr unsigned windowBytes = 64;
    const unsigned available = static_cast<unsigned>(sizeof(result.PageBytes)) - result.EntryOffset;
    unsigned consumed = 0;
    bool truncated = available < windowBytes;
    unsigned entryInstructions = 0;
    unsigned entryControlFlow = 0;
    unsigned entryRipRelative = 0;
    unsigned entryDecodeFailures = 0;
    while (consumed < windowBytes && consumed < available)
    {
        ZydisDecodedInstruction instruction = {};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT] = {};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder,
                result.PageBytes + result.EntryOffset + consumed, available - consumed,
                &instruction, operands)) || instruction.length == 0)
        {
            ++entryDecodeFailures;
            if (available - consumed < ZYDIS_MAX_INSTRUCTION_LENGTH)
                truncated = true;
            break;
        }
        if (instruction.meta.category == ZYDIS_CATEGORY_CALL ||
            instruction.meta.category == ZYDIS_CATEGORY_COND_BR ||
            instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
            instruction.meta.category == ZYDIS_CATEGORY_RET ||
            instruction.meta.category == ZYDIS_CATEGORY_INTERRUPT)
            ++entryControlFlow;
        for (unsigned index = 0; index < instruction.operand_count; ++index)
        {
            if (operands[index].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                operands[index].mem.base == ZYDIS_REGISTER_RIP)
                ++entryRipRelative;
        }
        consumed += instruction.length;
        ++entryInstructions;
    }
    const bool snapshot = (result.Flags & (LMV_KI_PREFLIGHT_IMAGE_RANGE |
        LMV_KI_PREFLIGHT_STABLE | LMV_KI_PREFLIGHT_PROLOGUE |
        LMV_KI_PREFLIGHT_NPT_TRANSLATED)) ==
        (LMV_KI_PREFLIGHT_IMAGE_RANGE | LMV_KI_PREFLIGHT_STABLE |
         LMV_KI_PREFLIGHT_PROLOGUE | LMV_KI_PREFLIGHT_NPT_TRANSLATED);
    const bool functionRecord = result.FunctionRecordFound == 1 &&
        result.EntryWithinFunction == 0 &&
        result.FunctionBeginAddress == result.RoutineAddress &&
        result.FunctionEndAddress >= result.RoutineAddress + 32 &&
        result.FunctionEndAddress <= result.KernelBase + result.KernelImageSize &&
        result.SamePageFunctionCount >= 1 &&
        result.SamePageOtherFunctionCount < result.SamePageFunctionCount;
    bool recordsValid = result.PageFunctionRecordTruncated == 0 &&
        result.PageFunctionRecordCapacity == LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS &&
        result.PageFunctionRecordCount == result.SamePageFunctionCount &&
        result.PageFunctionRecordCount <= LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS;
    unsigned targetRecords = 0;
    if (recordsValid)
    {
        for (unsigned index = 0; index < result.PageFunctionRecordCount; ++index)
        {
            const LMV_KI_PAGE_FUNCTION_RECORD& record = result.PageFunctionRecords[index];
            const bool overlaps = record.BeginAddress < result.PageAddress + 4096 &&
                record.EndAddress > result.PageAddress &&
                record.BeginAddress < record.EndAddress;
            if (!overlaps || (record.IsTarget != 0 && record.IsTarget != 1))
                recordsValid = false;
            if (record.IsTarget != 0)
            {
                ++targetRecords;
                if (record.BeginAddress != result.FunctionBeginAddress ||
                    record.EndAddress != result.FunctionEndAddress ||
                    record.UnwindAddress != result.FunctionUnwindAddress)
                    recordsValid = false;
            }
        }
    }
    recordsValid = recordsValid && targetRecords == 1;
    const bool pass = snapshot && consumed >= windowBytes &&
        entryDecodeFailures == 0 && !truncated && functionRecord && recordsValid;
    std::printf("guest ki-page-preflight: %s snapshot=%s status=%08lx flags=%08lx routine=%llx page=%llx GPA=%llx HPA=%llx entry-offset=%lu hash=%llx entry-window={bytes=%lu instructions=%lu control-flow=%lu rip-relative=%lu decode-failures=%lu} page-tail=unknown other-entries=unknown reachable=unproven install=0 execute=0 win32=%lu\n",
        pass ? "PASS" : "REVIEW", snapshot ? "stable" : "review",
        result.Status, result.Flags, result.RoutineAddress, result.PageAddress,
        result.GuestPhysicalPage, result.HostPhysicalPage, result.EntryOffset,
        result.PageHash, consumed, entryInstructions,
        entryControlFlow, entryRipRelative,
        entryDecodeFailures, error);
    std::printf("guest ki-function-bounds: %s record=%lu start=%llx end=%llx unwind=%llx entry-at-start=%u replay32-within=%u same-page-records=%lu other-records=%lu other-entries=unproven install=0\n",
        functionRecord ? "PASS" : "REVIEW", result.FunctionRecordFound,
        result.FunctionBeginAddress, result.FunctionEndAddress,
        result.FunctionUnwindAddress, result.EntryWithinFunction == 0 ? 1u : 0u,
        result.FunctionEndAddress >= result.RoutineAddress + 32 ? 1u : 0u,
        result.SamePageFunctionCount, result.SamePageOtherFunctionCount);
    std::printf("guest ki-page-functions: %s listed=%lu total=%lu capacity=%lu truncated=%lu targets=%u\n",
        recordsValid ? "PASS" : "REVIEW", result.PageFunctionRecordCount,
        result.SamePageFunctionCount, result.PageFunctionRecordCapacity,
        result.PageFunctionRecordTruncated, targetRecords);
    if (result.PageFunctionRecordCount <= LMV_KI_PREFLIGHT_MAX_PAGE_FUNCTIONS)
    {
        for (unsigned index = 0; index < result.PageFunctionRecordCount; ++index)
        {
            const LMV_KI_PAGE_FUNCTION_RECORD& record = result.PageFunctionRecords[index];
            if (record.BeginAddress < result.PageAddress)
                std::printf("guest ki-page-function: index=%u role=%s start=%llx end=%llx unwind=%llx offset=before-page\n",
                    index, record.IsTarget ? "ki" : "other", record.BeginAddress,
                    record.EndAddress, record.UnwindAddress);
            else
                std::printf("guest ki-page-function: index=%u role=%s start=%llx end=%llx unwind=%llx offset=%03llx\n",
                    index, record.IsTarget ? "ki" : "other", record.BeginAddress,
                    record.EndAddress, record.UnwindAddress,
                    record.BeginAddress - result.PageAddress);
        }
    }
    return pass ? 0 : 5;
}
static int RunKiPrologueAnalysis(HANDLE device)
{
    LMV_GUEST_KI_DISPATCH_INFO info = {};
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_RESOLVE_KI_DISPATCH),
                               nullptr, 0, &info, sizeof(info), &returned, nullptr);
    if (!ok || info.Status != 0 || info.RoutineAddress == 0 || info.BytesRead == 0)
    {
        std::printf("guest ki-analyze: FAIL status=%08lx win32=%lu bytes=%lu\n",
                    info.Status, ok ? 0 : GetLastError(), info.BytesRead);
        return 5;
    }

    ZydisDecoder decoder = {};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                       ZYDIS_STACK_WIDTH_64)))
    {
        std::printf("guest ki-analyze: FAIL decoder-init\n");
        return 5;
    }
    unsigned offset = 0;
    unsigned instructionCount = 0;
    unsigned unsupportedCount = 0;
    unsigned ripRelativeCount = 0;
    unsigned relocatableCount = 0;
    unsigned copiedBytes = 0;
    bool complete = true;
    while (offset < info.BytesRead)
    {
        ZydisDecodedInstruction instruction = {};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT] = {};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(
                &decoder, info.OriginalBytes + offset, info.BytesRead - offset,
                &instruction, operands)) || instruction.length == 0)
        {
            std::printf("guest ki-analyze: offset=%02x decode=FAIL\n", offset);
            complete = false;
            break;
        }
        bool hasRipRelative = false;
        for (unsigned operandIndex = 0; operandIndex < instruction.operand_count; ++operandIndex)
        {
            if (operands[operandIndex].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                operands[operandIndex].mem.base == ZYDIS_REGISTER_RIP)
                hasRipRelative = true;
        }
        ExecRelocationStatus relocationStatus = ExecRelocationStatus::Success;
        if (hasRipRelative)
        {
            ++ripRelativeCount;
            std::uint8_t relocated[ZYDIS_MAX_INSTRUCTION_LENGTH] = {};
            const auto relocation = RelocateRipRelativeInstruction(
                info.OriginalBytes + offset, info.BytesRead - offset,
                static_cast<std::uintptr_t>(info.RoutineAddress + offset),
                static_cast<std::uintptr_t>(info.RoutineAddress + 0x100000 + offset),
                relocated, sizeof(relocated));
            relocationStatus = relocation.Status;
            if (relocation.Status == ExecRelocationStatus::Success)
                ++relocatableCount;
            else
                ++unsupportedCount;
        }
        else if (instruction.meta.category == ZYDIS_CATEGORY_CALL ||
                 instruction.meta.category == ZYDIS_CATEGORY_COND_BR ||
                 instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
                 instruction.meta.category == ZYDIS_CATEGORY_RET)
        {
            relocationStatus = ExecRelocationStatus::UnsupportedControlFlow;
            ++unsupportedCount;
        }
        const char* mnemonic = ZydisMnemonicGetString(instruction.mnemonic);
        std::printf("guest ki-analyze: offset=%02x address=%llx length=%u mnemonic=%s rip=%u status=%s\n",
                    offset, info.RoutineAddress + offset, instruction.length,
                    mnemonic != nullptr ? mnemonic : "?", hasRipRelative ? 1 : 0,
                    RelocationStatusName(relocationStatus));
        offset += instruction.length;
        copiedBytes = offset;
        ++instructionCount;
        if (instructionCount >= 16)
            break;
    }
    const bool windowComplete = complete && copiedBytes == info.BytesRead;
    const bool safeWindow = windowComplete && unsupportedCount == 0;
    std::printf("guest ki-analyze: %s routine=%llx bytes=%lu instructions=%u rip-relative=%u relocatable=%u unsupported=%u window=%s\n",
                safeWindow ? "PASS" : "REVIEW", info.RoutineAddress, info.BytesRead,
                instructionCount, ripRelativeCount, relocatableCount, unsupportedCount,
                windowComplete ? "complete" : "partial");
    return safeWindow ? 0 : 5;
}

static LMV_GUEST_KI_DISPATCH_INFO KiStubFixture()
{
    LMV_GUEST_KI_DISPATCH_INFO fixture = {};
    constexpr std::uint8_t bytes[32] = {
        0x40, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x56,
        0x41, 0x57, 0x48, 0x81, 0xEC, 0x88, 0x01, 0x00,
        0x00, 0x48, 0x8D, 0x6C, 0x24, 0x30, 0x48, 0x8B,
        0x05, 0x73, 0x61, 0x8D, 0x00, 0x48, 0x33, 0xC5
    };
    std::memcpy(fixture.OriginalBytes, bytes, sizeof(bytes));
    fixture.RoutineAddress = 0xFFFFF80347F3C330ull;
    fixture.BytesRead = sizeof(bytes);
    return fixture;
}

static int RunKiStubSelftest()
{
    auto fixture = KiStubFixture();
    KiStubPlan plan = {};
    const std::uint64_t candidate = fixture.RoutineAddress + 0x100000;
    const auto success = BuildKiStubPlan(fixture, candidate, &plan);
    const std::uint64_t expectedTarget = fixture.RoutineAddress + 29 + 0x8D6173;
    std::uint64_t encodedResume = 0;
    std::memcpy(&encodedResume, plan.Bytes + 38, sizeof(encodedResume));
    const bool constructed = success == KiStubPlanStatus::Success &&
        plan.CandidateAddress == candidate &&
        plan.RipTarget == expectedTarget &&
        plan.ResumeAddress == fixture.RoutineAddress + 32 &&
        encodedResume == plan.ResumeAddress &&
        plan.Bytes[32] == 0xFF && plan.Bytes[33] == 0x25 &&
        plan.Bytes[34] == 0 && plan.Bytes[35] == 0 &&
        plan.Bytes[36] == 0 && plan.Bytes[37] == 0 &&
        plan.Bytes[22] == 0x48 && plan.Bytes[23] == 0x8B;
    KiStubPlan unchanged = {};
    std::memset(&unchanged, 0xA5, sizeof(unchanged));
    KiStubPlan sentinel = unchanged;
    auto mutated = fixture;
    mutated.OriginalBytes[10] = 0x90;
    const bool mutationRejected = BuildKiStubPlan(mutated, candidate, &unchanged) ==
        KiStubPlanStatus::UnexpectedPrologue &&
        std::memcmp(&unchanged, &sentinel, sizeof(sentinel)) == 0;
    const bool farRejected = BuildKiStubPlan(fixture,
        fixture.RoutineAddress + (1ull << 33), &unchanged) ==
        KiStubPlanStatus::RelocationFailed &&
        std::memcmp(&unchanged, &sentinel, sizeof(sentinel)) == 0;
    const bool addressRejected = BuildKiStubPlan(fixture, 0x100000,
        &unchanged) == KiStubPlanStatus::InvalidCandidate &&
        std::memcmp(&unchanged, &sentinel, sizeof(sentinel)) == 0;
    fixture.BytesRead = 31;
    const bool truncatedRejected = BuildKiStubPlan(fixture, candidate, &unchanged) ==
        KiStubPlanStatus::InvalidInput &&
        std::memcmp(&unchanged, &sentinel, sizeof(sentinel)) == 0;
    const bool pass = constructed && mutationRejected && farRejected &&
                      addressRejected && truncatedRejected;
    std::printf("guest ki-stub-selftest: %s generated=%u mutation=%u far=%u invalid-address=%u truncated=%u unchanged=%u\n",
                pass ? "PASS" : "FAIL", constructed ? 1 : 0, mutationRejected ? 1 : 0,
                farRejected ? 1 : 0, addressRejected ? 1 : 0,
                truncatedRejected ? 1 : 0,
                std::memcmp(&unchanged, &sentinel, sizeof(sentinel)) == 0 ? 1 : 0);
    return pass ? 0 : 5;
}

static int RunKiStubDryRun()
{
    HANDLE device = OpenGuestDevice();
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("guest ki-stub-dry-run: FAIL open=%lu\n", GetLastError());
        return 5;
    }
    LMV_GUEST_KI_DISPATCH_INFO info = {};
    DWORD returned = 0;
    const BOOL read = DeviceIoControl(device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_RESOLVE_KI_DISPATCH),
        nullptr, 0, &info, sizeof(info), &returned, nullptr);
    const DWORD error = read ? 0 : GetLastError();
    CloseHandle(device);
    if (!read || returned != sizeof(info) || info.Status != 0 ||
        info.RoutineAddress > UINT64_MAX - 0x100000)
    {
        std::printf("guest ki-stub-dry-run: FAIL query=%08lx win32=%lu\n",
                    info.Status, error);
        return 5;
    }
    KiStubPlan plan = {};
    const std::uint64_t hypothetical = info.RoutineAddress + 0x100000;
    const auto result = BuildKiStubPlan(info, hypothetical, &plan);
    if (result != KiStubPlanStatus::Success)
    {
        std::printf("guest ki-stub-dry-run: REVIEW result=%u candidate=hypothetical-%llx installed=0\n",
                    static_cast<unsigned>(result), hypothetical);
        return 5;
    }
    std::uint64_t jumpTarget = 0;
    std::memcpy(&jumpTarget, plan.Bytes + 38, sizeof(jumpTarget));
    const bool pass = jumpTarget == plan.ResumeAddress &&
                      plan.ResumeAddress == info.RoutineAddress + 32;
    std::printf("guest ki-stub-dry-run: %s source=%llx candidate=hypothetical-%llx rip-target=%llx resume=%llx size=%zu allocated=0 installed=0 executed=0\n",
                pass ? "PASS" : "FAIL", info.RoutineAddress, hypothetical,
                plan.RipTarget, plan.ResumeAddress, KiStubPlan::Length);
    return pass ? 0 : 5;
}

static int RunKernelExecSmoke(bool stress, unsigned round)
{
    HANDLE device = OpenGuestDevice();
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("guest kernel-exec-smoke: FAIL open=%lu\n", GetLastError());
        return 5;
    }
    LMV_GUEST_KERNEL_EXEC_SMOKE result = {};
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(device,
        static_cast<DWORD>(stress ? IOCTL_LMV_GUEST_KERNEL_EXEC_STRESS :
            IOCTL_LMV_GUEST_KERNEL_EXEC_SMOKE),
        nullptr, 0, &result, sizeof(result), &bytes, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(device);
    const bool pass = ok && bytes == sizeof(result) && result.Status == 0 &&
        result.Version == LMV_CPUID_PROTOCOL_VERSION && result.Size == sizeof(result) &&
        result.Step == 5 && result.RemainingStage == 0 &&
        result.GuestPhysicalAddress != 0 &&
        result.ReadBefore == result.ReadArmed &&
        result.ReadBefore == result.ReadRestored &&
        result.HandledResult == 0x44 && result.ContinuedResult == 0x1235 &&
        result.NonEntryResult == 0x33 && result.RestoredResult == 0x1235 &&
        result.HandledCalls == 1 && result.ContinuedCalls == 1 &&
        result.ProcessorsSeen == result.ProcessorsExpected &&
        result.ProcessorsExpected != 0 &&
        (!stress || (result.ProcessorsExecuted == result.ProcessorsExpected &&
         result.WorkerMismatches == 0 &&
         result.Iterations == result.ProcessorsExpected * 4));
    std::printf("guest kernel-exec-%s: round=%u %s status=%08lx step=%lu GPA=%llx addresses={original=%llx mirror=%llx stub=%llx} read={%llx,%llx,%llx} results={handled=%llx continued=%llx non-entry=%llx restored=%llx} calls=%lu/%lu processors=%lu/%lu executed=%lu mismatches=%lu iterations=%lu remaining-stage=%lu executed-ki=0 win32=%lu\n",
        stress ? "stress" : "smoke", round, pass ? "PASS" : "FAIL", result.Status, result.Step,
        result.GuestPhysicalAddress, result.OriginalAddress,
        result.MirrorAddress, result.RedirectAddress, result.ReadBefore,
        result.ReadArmed, result.ReadRestored, result.HandledResult,
        result.ContinuedResult, result.NonEntryResult, result.RestoredResult,
        result.HandledCalls, result.ContinuedCalls, result.ProcessorsSeen,
        result.ProcessorsExpected, result.ProcessorsExecuted,
        result.WorkerMismatches, result.Iterations, result.RemainingStage, error);
    return pass ? 0 : 5;
}
static int RunStaticStubSmoke()
{
    HANDLE device = OpenGuestDevice();
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("guest static-stub-smoke: FAIL open=%lu\n", GetLastError());
        return 5;
    }
    LMV_GUEST_STATIC_STUB_SMOKE result = {};
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_STATIC_STUB_SMOKE),
        nullptr, 0, &result, sizeof(result), &bytes, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(device);
    const bool pass = ok && bytes == sizeof(result) && result.Status == 0 &&
        result.Version == LMV_CPUID_PROTOCOL_VERSION && result.Size == sizeof(result) &&
        result.Flags == 3 && result.HandledResult == 0x44 &&
        result.ContinuedResult == 0x1235 && result.HandledCalls == 1 &&
        result.ContinuedCalls == 1;
    std::printf("guest static-stub-smoke: %s status=%08lx handled=%llx continued=%llx calls=%lu/%lu flags=%llx:%llx,%llx:%llx installed=0 executed-ki=0 win32=%lu\n",
        pass ? "PASS" : "FAIL", result.Status, result.HandledResult,
        result.ContinuedResult, result.HandledCalls, result.ContinuedCalls,
        result.HandledEntryFlags, result.HandledExitFlags,
        result.ContinuedEntryFlags, result.ContinuedExitFlags, error);
    return pass ? 0 : 5;
}
static int RunKiStubSites()
{
    HANDLE device = OpenGuestDevice();
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("guest ki-stub-sites: FAIL open=%lu\n", GetLastError());
        return 5;
    }
    LMV_GUEST_KI_DISPATCH_INFO info = {};
    LMV_GUEST_KI_STUB_SITES sites = {};
    DWORD infoBytes = 0;
    DWORD siteBytes = 0;
    const BOOL queriedKi = DeviceIoControl(device,
        static_cast<DWORD>(IOCTL_LMV_GUEST_RESOLVE_KI_DISPATCH),
        nullptr, 0, &info, sizeof(info), &infoBytes, nullptr);
    const DWORD kiError = queriedKi ? 0 : GetLastError();
    BOOL queriedSites = FALSE;
    DWORD siteError = 0;
    if (queriedKi && infoBytes == sizeof(info) && info.Status == 0)
    {
        queriedSites = DeviceIoControl(device,
            static_cast<DWORD>(IOCTL_LMV_GUEST_QUERY_KI_STUB_SITES),
            nullptr, 0, &sites, sizeof(sites), &siteBytes, nullptr);
        siteError = queriedSites ? 0 : GetLastError();
    }
    CloseHandle(device);
    if (!queriedKi || !queriedSites || infoBytes != sizeof(info) ||
        siteBytes != sizeof(sites) || info.Status != 0 || sites.Status != 0 ||
        sites.Version != LMV_CPUID_PROTOCOL_VERSION || sites.Size != sizeof(sites) ||
        sites.Flags != (LMV_KI_SITE_IMAGE_CODE | LMV_KI_SITE_NX_BUFFER) ||
        sites.ImageCodeAddress == 0 || sites.NxBufferAddress == 0 ||
        sites.ImageCodeAddress == sites.NxBufferAddress || sites.NxBufferSize < KiStubPlan::Length)
    {
        std::printf("guest ki-stub-sites: FAIL ki=%08lx site=%08lx win32={ki=%lu sites=%lu} flags=%08lx\n",
                    info.Status, sites.Status, kiError, siteError, sites.Flags);
        return 5;
    }
    KiStubPlan imagePlan = {};
    KiStubPlan nxPlan = {};
    const auto imageResult = BuildKiStubPlan(info, sites.ImageCodeAddress, &imagePlan);
    const auto nxResult = BuildKiStubPlan(info, sites.NxBufferAddress, &nxPlan);
    std::printf("guest ki-stub-sites: PASS ki=%llx image-code=%llx image-plan=%u image-patchable=unknown nx-buffer=%llx nx-bytes=%llu nx-plan=%u nx-executable=0 eligible=0 installed=0 executed=0\n",
                info.RoutineAddress, sites.ImageCodeAddress,
                static_cast<unsigned>(imageResult), sites.NxBufferAddress,
                sites.NxBufferSize, static_cast<unsigned>(nxResult));
    return 0;
}

static int RunMemoryCommand(HANDLE device, wchar_t** argv)
{
    const bool write = wcscmp(argv[1], L"--write") == 0;
    const unsigned long long processId = _wcstoui64(argv[2], nullptr, 0);
    const unsigned long long address = _wcstoui64(argv[3], nullptr, 0);
    const ULONG length = write ? static_cast<ULONG>(sizeof(unsigned long long))
                               : static_cast<ULONG>(_wcstoui64(argv[4], nullptr, 0));
    if (processId == 0 || address == 0 || length == 0 || length > LMV_MEMORY_IO_MAX_SIZE)
    {
        std::printf("memory argument invalid\n");
        return 4;
    }

    std::vector<unsigned char> input(FIELD_OFFSET(LMV_GUEST_MEMORY_REQUEST, Data) + length, 0);
    std::vector<unsigned char> output(FIELD_OFFSET(LMV_GUEST_MEMORY_RESPONSE, Data) + length, 0);
    auto* request = reinterpret_cast<LMV_GUEST_MEMORY_REQUEST*>(input.data());
    request->Version = LMV_CPUID_PROTOCOL_VERSION;
    request->Size = FIELD_OFFSET(LMV_GUEST_MEMORY_REQUEST, Data);
    request->ProcessId = processId;
    request->Address = address;
    request->Length = length;
    if (write)
    {
        const unsigned long long value = _wcstoui64(argv[4], nullptr, 0);
        memcpy(request->Data, &value, sizeof(value));
    }

    DWORD returned = 0;
    const DWORD ioctl = write ? static_cast<DWORD>(IOCTL_LMV_GUEST_WRITE_PROCESS_MEMORY)
                              : static_cast<DWORD>(IOCTL_LMV_GUEST_READ_PROCESS_MEMORY);
    BOOL ok = DeviceIoControl(device, ioctl, input.data(), static_cast<DWORD>(input.size()),
                              output.data(), static_cast<DWORD>(output.size()), &returned, nullptr);
    auto* response = reinterpret_cast<LMV_GUEST_MEMORY_RESPONSE*>(output.data());
    std::printf("guest memory-%s: ioctl=%s status=%08lx bytes=%lu pages=%lu bitmap=%llx\n",
                write ? "write" : "read", ok ? "ok" : "failed", response->Status,
                response->BytesTransferred, response->PageCount,
                static_cast<unsigned long long>(response->ValidPageBitmap[0]));
    if (ok && response->Status == 0 && !write)
    {
        std::printf("data:");
        for (ULONG i = 0; i < response->BytesTransferred; ++i)
            std::printf(" %02x", response->Data[i]);
        std::printf("\n");
    }
    return ok && response->Status == 0 ? 0 : 5;
}

static bool ParseUnsigned(const wchar_t* text, unsigned long long* value)
{
    if (text == nullptr || *text == L'\0' || *text == L'-')
        return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long parsed = _wcstoui64(text, &end, 0);
    if (errno == ERANGE || end == text || *end != L'\0')
        return false;
    *value = parsed;
    return true;
}

static int ParseHexDigit(wchar_t digit)
{
    if (digit >= L'0' && digit <= L'9') return digit - L'0';
    if (digit >= L'a' && digit <= L'f') return digit - L'a' + 10;
    if (digit >= L'A' && digit <= L'F') return digit - L'A' + 10;
    return -1;
}

static int RunDebugHookCommand(HANDLE device, int argc, wchar_t** argv)
{
    const bool install = wcscmp(argv[1], L"--hook-install") == 0;
    const bool remove = wcscmp(argv[1], L"--hook-remove") == 0;
    const bool attach = wcscmp(argv[1], L"--dbg-attach") == 0;
    const bool detach = wcscmp(argv[1], L"--dbg-detach") == 0;
    if ((install && (argc != 8 || wcscmp(argv[7], L"--unsafe-global") != 0)) ||
        (!install && argc != 3))
    {
        std::printf("usage: --dbg-attach <pid> | --dbg-detach <pid> | --hook-remove <pid>\n"
                    "       --hook-install <pid> <page-va> <offset> <length> <hexbytes> --unsafe-global\n");
        return 4;
    }

    unsigned long long pid = 0;
    if (!ParseUnsigned(argv[2], &pid) || pid == 0 || pid > 0xffffffffull)
    {
        std::printf("invalid PID\n");
        return 4;
    }

    DWORD returned = 0;
    if (attach || detach)
    {
        LMV_DBG_ATTACH_REQUEST request = { static_cast<ULONG>(pid) };
        const DWORD control = attach ? LMV_DBG_IOCTL_ATTACH_PROCESS : LMV_DBG_IOCTL_DETACH_PROCESS;
        const BOOL ok = DeviceIoControl(device, control, &request, sizeof(request),
                                        nullptr, 0, &returned, nullptr);
        std::printf("debug %s: %s win32=%lu\n", attach ? "attach" : "detach",
                    ok ? "PASS" : "FAIL", ok ? 0 : GetLastError());
        return ok ? 0 : 5;
    }

    LMV_DBG_NPT_HOOK_REQUEST request = {};
    request.ProcessId = static_cast<ULONG>(pid);
    if (install)
    {
        unsigned long long address = 0;
        unsigned long long offset = 0;
        unsigned long long length = 0;
        if (!ParseUnsigned(argv[3], &address) || address == 0 || (address & 0xfffull) != 0 ||
            !ParseUnsigned(argv[4], &offset) || offset >= 4096 ||
            !ParseUnsigned(argv[5], &length) || length == 0 || length > sizeof(request.PatchBytes) ||
            length > 4096 - offset || wcslen(argv[6]) != length * 2)
        {
            std::printf("invalid page VA, offset, length or patch bytes\n");
            return 4;
        }
        request.Address = address;
        request.PatchOffset = static_cast<ULONG>(offset);
        request.PatchLength = static_cast<ULONG>(length);
        for (ULONG index = 0; index < request.PatchLength; ++index)
        {
            const int upper = ParseHexDigit(argv[6][index * 2]);
            const int lower = ParseHexDigit(argv[6][index * 2 + 1]);
            if (upper < 0 || lower < 0)
            {
                std::printf("patch must contain only hexadecimal bytes\n");
                return 4;
            }
            request.PatchBytes[index] = static_cast<UCHAR>((upper << 4) | lower);
        }
        std::printf("WARNING: NPT remap affects every Guest CPU/CR3 sharing this GPA; use only a disposable VM data page.\n");
    }

    LMV_DBG_NPT_HOOK_INFO info = {};
    const DWORD control = install ? LMV_DBG_IOCTL_PTE_HOOK_INSTALL : LMV_DBG_IOCTL_PTE_HOOK_REMOVE;
    const BOOL ok = DeviceIoControl(device, control, &request, sizeof(request),
                                    &info, sizeof(info), &returned, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    std::printf("debug hook-%s: %s win32=%lu status=%08lx installed=%lu pid=%lu VA=%llx GPA=%llx original-HPA=%llx shadow-HPA=%llx bytes=%lu\n",
                install ? "install" : "remove", ok ? "PASS" : "FAIL", error,
                info.Status, info.Installed, info.ProcessId, info.Address,
                info.GuestPhysicalAddress, info.OriginalHostPhysicalAddress,
                info.ShadowHostPhysicalAddress, returned);
    if (ok && returned != sizeof(info))
        return 5;
    return ok ? 0 : 5;
}
static int RunKiDrainSelftest()
{
    bool pass = LmvKiDrainComplete(0, 0, 0) &&
        LmvKiDrainComplete(0, 0, LMV_KI_DRAIN_REGISTERED) &&
        LmvKiDrainComplete(0, 0, LMV_KI_DRAIN_RETIRED) &&
        !LmvKiDrainComplete(1, 0, 0) && !LmvKiDrainComplete(~0ull, 0, 0) &&
        !LmvKiDrainComplete(0, 1, 0) && !LmvKiDrainComplete(0, 0, LMV_KI_DRAIN_ENABLED);
    const ULONGLONG base = 0xfffff80010000000ull;
    const ULONGLONG end = base + 0x10000;
    pass = pass && !LmvKiExitTargetValid(base, base, end) &&
        !LmvKiExitTargetValid(end - 1, base, end) &&
        !LmvKiExitTargetValid(0x1234, base, end) &&
        LmvKiExitTargetValid(base - 1, base, end) && LmvKiExitTargetValid(end, base, end);
    std::printf("guest ki-drain-selftest: %s drain-predicates=checked exit-image-boundaries=checked synthetic=1\n",
        pass ? "PASS" : "FAIL");
    return pass ? 0 : 5;
}

static int RunKiDrain(bool prepareUnload)
{
    HANDLE device = OpenGuestDevice(FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("guest ki-drain: FAIL open=%lu\n", GetLastError());
        return 5;
    }
    LMV_GUEST_KI_DRAIN drain = {};
    drain.Version = LMV_CPUID_PROTOCOL_VERSION;
    drain.Size = sizeof(drain);
    drain.Operation = prepareUnload ? LMV_KI_SESSION_PREPARE_UNLOAD : LMV_KI_SESSION_QUERY;
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_KI_SESSION), &drain, sizeof(drain),
        &drain, sizeof(drain), &returned, nullptr);
    DWORD error = ok ? 0 : GetLastError();
    CloseHandle(device);
    std::printf("guest ki-%s: %s win32=%lu status=%08lx inflight=%llu armed=%lu flags=%08lx CPUs=%lu/%lu bytes=%lu\n",
        prepareUnload ? "prepare-unload" : "drain", ok ? "PASS" : "FAIL", error, drain.Status,
        drain.InFlight, drain.ArmedProcessors, drain.Flags, drain.ProcessorsSeen,
        drain.ProcessorsExpected, returned);
    if (!ok || returned != sizeof(drain))
        return 5;
    if (prepareUnload)
    {
        if (drain.InFlight != 0 || drain.ArmedProcessors != 0 ||
            !(drain.Flags & LMV_KI_DRAIN_RETIRED))
            return 5;
        std::printf("Guest is retired: close all debugger handles, unload Guest, then unload Host. Reattach requires reloading both drivers.\n");
    }
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && wcscmp(argv[1], L"--ki-drain-selftest") == 0)
        return RunKiDrainSelftest();
    if (argc == 2 && wcscmp(argv[1], L"--ki-drain") == 0)
        return RunKiDrain(false);
    if (argc == 2 && wcscmp(argv[1], L"--ki-prepare-unload") == 0)
        return RunKiDrain(true);
    if (argc == 2 && wcscmp(argv[1], L"--ki-page-preflight") == 0)
        return RunKiPagePreflight();
    if (argc == 2 && wcscmp(argv[1], L"--ki-page-reloc-analysis") == 0)
        return RunKiPageRelocationAnalysis();
    if (argc == 2 && wcscmp(argv[1], L"--ki-page-mirror-plan") == 0)
        return RunKiPageMirrorPlan();
    if (argc == 2 && wcscmp(argv[1], L"--ki-window-plan") == 0)
        return RunKiWindowPlan();
    if (argc == 2 && wcscmp(argv[1], L"--ki-real-mirror-plan") == 0)
        return RunKiWindowPlan(false, true);
    if (argc == 2 && wcscmp(argv[1], L"--ki-real-mirror-selftest") == 0)
        return RunKiRealMirrorSelftest();
    if (argc == 2 && wcscmp(argv[1], L"--ki-install-preflight") == 0)
        return RunKiWindowPlan(true);
    if (argc == 2 && wcscmp(argv[1], L"--ki-install-preflight-selftest") == 0)
        return RunKiInstallPreflightSelftest();
    if (argc == 2 && wcscmp(argv[1], L"--ki-entry-path-selftest") == 0)
        return RunKiEntryPathSelftest();
    if (argc == 2 && wcscmp(argv[1], L"--ki-tail-selftest") == 0)
        return RunKiTailSelftest();
    if (argc == 2 && wcscmp(argv[1], L"--ki-page-mirror-selftest") == 0)
        return RunKiPageMirrorSelftest();
    if (argc == 2 && wcscmp(argv[1], L"--kernel-exec-smoke") == 0)
        return RunKernelExecSmoke(false, 1);
    if (argc == 2 && wcscmp(argv[1], L"--kernel-exec-stress") == 0)
    {
        for (unsigned round = 1; round <= 3; ++round)
        {
            if (RunKernelExecSmoke(true, round) != 0)
                return 5;
        }
        std::printf("guest kernel-exec-stress: PASS rounds=3\n");
        return 0;
    }
    if (argc == 2 && wcscmp(argv[1], L"--static-stub-smoke") == 0)
        return RunStaticStubSmoke();
    if (argc == 2 && wcscmp(argv[1], L"--ki-stub-selftest") == 0)
        return RunKiStubSelftest();
    if (argc == 2 && wcscmp(argv[1], L"--ki-stub-sites") == 0)
        return RunKiStubSites();
    if (argc == 2 && wcscmp(argv[1], L"--ki-stub-dry-run") == 0)
        return RunKiStubDryRun();
    if (argc == 2 && wcscmp(argv[1], L"--ki-replay-selftest") == 0)
    {
        LMV_GUEST_KI_DISPATCH_INFO fixture = {};
        const unsigned char bytes[32] = {
            0x40, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x56,
            0x41, 0x57, 0x48, 0x81, 0xEC, 0x88, 0x01, 0x00,
            0x00, 0x48, 0x8D, 0x6C, 0x24, 0x30, 0x48, 0x8B,
            0x05, 0x73, 0x61, 0x8D, 0x00, 0x48, 0x33, 0xC5
        };
        std::memcpy(fixture.OriginalBytes, bytes, sizeof(bytes));
        fixture.RoutineAddress = 0xFFFFF80347F3C330ull;
        fixture.BytesRead = sizeof(bytes);
        bool pass = RunKiReplaySmoke(fixture);
        fixture.OriginalBytes[10] = 0x90;
        pass = !RunKiReplaySmoke(fixture) && pass;
        std::printf("guest ki-replay-selftest: %s mutation=rejected\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 5;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-replay-smoke") == 0)
    {
        HANDLE replayDevice = OpenGuestDevice();
        if (replayDevice == INVALID_HANDLE_VALUE)
        {
            std::printf("open guest driver failed: %lu\n", GetLastError());
            return 1;
        }
        LMV_GUEST_KI_DISPATCH_INFO info = {};
        DWORD returned = 0;
        const BOOL read = DeviceIoControl(replayDevice,
            static_cast<DWORD>(IOCTL_LMV_GUEST_RESOLVE_KI_DISPATCH),
            nullptr, 0, &info, sizeof(info), &returned, nullptr);
        const DWORD error = read ? 0 : GetLastError();
        CloseHandle(replayDevice);
        if (!read || returned != sizeof(info) || info.Status != 0)
        {
            std::printf("guest ki-replay-smoke: FAIL read=%08lx win32=%lu\n",
                        info.Status, error);
            return 5;
        }
        return RunKiReplaySmoke(info) ? 0 : 5;
    }
    if (argc == 2 && wcscmp(argv[1], L"--analyze-ki") == 0)
    {
        HANDLE device = OpenGuestDevice();
        if (device == INVALID_HANDLE_VALUE)
        {
            std::printf("open guest driver failed: %lu\n", GetLastError());
            return 1;
        }
        const int result = RunKiPrologueAnalysis(device);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--relocator-smoke") == 0)
        return RunRelocatorSmoke();
    HANDLE device = OpenGuestDevice(argc == 2 &&
        (wcscmp(argv[1], L"--ki-dr3-rax-smoke") == 0 ||
         wcscmp(argv[1], L"--ki-dr3-handled-smoke") == 0 ||
         wcscmp(argv[1], L"--ki-queue-cancel-smoke") == 0 ||
         wcscmp(argv[1], L"--ki-queue-concurrency-smoke") == 0 ||
         wcscmp(argv[1], L"--ki-dr3-event-smoke") == 0 ||
         wcscmp(argv[1], L"--ki-dr3-all-cpus-event-smoke") == 0) ?
        FILE_SHARE_READ | FILE_SHARE_WRITE : 0);
    if (device == INVALID_HANDLE_VALUE)
    {
        std::printf("open guest driver failed: %lu\n", GetLastError());
        return 1;
    }

    if (argc == 2 && wcscmp(argv[1], L"--dr3-smoke") == 0)
    {
        const int result = RunPrivateDr3Smoke(device);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--dr3-all-cpus-smoke") == 0)
    {
        const int result = RunPrivateDr3AllCpusSmoke(device);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-queue-cancel-smoke") == 0)
    {
        const int result = RunKiQueueCancelSmoke(device);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-queue-concurrency-smoke") == 0)
    {
        const int result = RunKiQueueConcurrencySmoke(device);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-dr3-handled-smoke") == 0)
    {
        const int result = RunKiDr3Probe(device, true, true, true, true, true, false, true);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-dr3-rax-smoke") == 0)
    {
        const int result = RunKiDr3Probe(device, true, true, true, true, true, true);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-dr3-all-cpus-event-smoke") == 0)
    {
        const int result = RunKiDr3AllCpusEventSmoke(device);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-dr3-event-smoke") == 0)
    {
        const int result = RunKiDr3Probe(device, true, true, true, true, true);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-dr3-frame-smoke") == 0)
    {
        const int result = RunKiDr3Probe(device, true, true, true, true);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-dr3-inspect-smoke") == 0)
    {
        const int result = RunKiDr3Probe(device, true, true, true);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-dr3-redirect-smoke") == 0)
    {
        const int result = RunKiDr3Probe(device, true, true);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-dr3-probe-ud2") == 0)
    {
        const int result = RunKiDr3Probe(device, true);
        CloseHandle(device);
        return result;
    }    if (argc == 2 && wcscmp(argv[1], L"--ki-dr3-probe") == 0)
    {
        const int result = RunKiDr3Probe(device, false);
        CloseHandle(device);
        return result;
    }    if (argc == 2 && wcscmp(argv[1], L"--dr-coexist-smoke") == 0)
    {
        const int result = RunNativeDrCoexistSmoke(device);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--native-dr-smoke") == 0)
    {
        const int result = RunNativeDrSmoke();
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--ki-exec-mirror-smoke") == 0)
    {
        const int result = RunKiExecMirrorSmoke(device);
        CloseHandle(device);
        return result;
    }

    if ((argc == 5 && wcscmp(argv[1], L"--read") == 0) ||
        (argc == 5 && wcscmp(argv[1], L"--write") == 0))
    {
        int result = RunMemoryCommand(device, argv);
        CloseHandle(device);
        return result;
    }

    if (argc == 2 && wcscmp(argv[1], L"--stub-smoke") == 0)
    {
        LMV_GUEST_STUB_REQUEST request = {};
        request.Version = LMV_CPUID_PROTOCOL_VERSION;
        request.Size = sizeof(request);
        DWORD returned = 0;
        BOOL triggerOk = DeviceIoControl(
            device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_TRIGGER),
            &request, sizeof(request), nullptr, 0, &returned, nullptr);
        LMV_GUEST_STUB_EVENT event = {};
        BOOL eventOk = triggerOk && DeviceIoControl(
            device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_GET_EVENT),
            nullptr, 0, &event, sizeof(event), &returned, nullptr);
        LMV_GUEST_STUB_CONTINUE continueRequest = {};
        continueRequest.Version = LMV_CPUID_PROTOCOL_VERSION;
        continueRequest.Size = sizeof(continueRequest);
        continueRequest.Sequence = event.Sequence;
        continueRequest.Disposition = LMV_GUEST_STUB_DISPOSITION_CONTINUE;
        BOOL continueOk = eventOk && DeviceIoControl(
            device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_CONTINUE),
            &continueRequest, sizeof(continueRequest),
            &continueRequest, sizeof(continueRequest), &returned, nullptr);
        const DWORD error = (triggerOk && eventOk && continueOk) ? 0 : GetLastError();
        std::printf("guest stub-smoke: %s trigger=%s event=%s continue=%s sequence=%lu pid=%lu tid=%lu code=%08lx address=%llx state=%lu win32=%lu\n",
                    triggerOk && eventOk && continueOk &&
                    event.State == LMV_GUEST_STUB_STATE_PENDING &&
                    continueRequest.State == LMV_GUEST_STUB_STATE_CONTINUED ? "PASS" : "FAIL",
                    triggerOk ? "ok" : "fail", eventOk ? "ok" : "fail",
                    continueOk ? "ok" : "fail", event.Sequence, event.ProcessId,
                    event.ThreadId, event.ExceptionCode, event.ExceptionAddress,
                    continueRequest.State, error);
        CloseHandle(device);
        return triggerOk && eventOk && continueOk ? 0 : 5;
    }
    if (argc == 2 && wcscmp(argv[1], L"--stub-queue-smoke") == 0)
    {
        constexpr ULONG eventCount = 3;
        BOOL triggerOk = TRUE;
        DWORD returned = 0;
        for (ULONG index = 0; index < eventCount; ++index)
        {
            LMV_GUEST_STUB_REQUEST request = {};
            request.Version = LMV_CPUID_PROTOCOL_VERSION;
            request.Size = sizeof(request);
            request.ProcessId = 4000 + index;
            request.ThreadId = 8000 + index;
            if (!DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_TRIGGER),
                                  &request, sizeof(request), nullptr, 0, &returned, nullptr))
            {
                triggerOk = FALSE;
                break;
            }
        }

        BOOL orderOk = triggerOk;
        BOOL continueOk = triggerOk;
        ULONG previousSequence = 0;
        ULONG received = 0;
        DWORD error = triggerOk ? 0 : GetLastError();
        for (ULONG index = 0; triggerOk && index < eventCount; ++index)
        {
            LMV_GUEST_STUB_WAIT_REQUEST waitRequest = {};
            waitRequest.Version = LMV_CPUID_PROTOCOL_VERSION;
            waitRequest.Size = sizeof(waitRequest);
            waitRequest.TimeoutMilliseconds = 1000;
            LMV_GUEST_STUB_EVENT event = {};
            returned = 0;
            const BOOL waitOk = DeviceIoControl(
                device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_WAIT_EVENT),
                &waitRequest, sizeof(waitRequest), &event, sizeof(event), &returned, nullptr);
            if (!waitOk)
            {
                orderOk = FALSE;
                error = GetLastError();
                break;
            }
            if (event.State != LMV_GUEST_STUB_STATE_PENDING ||
                event.ProcessId != 4000 + index ||
                event.ThreadId != 8000 + index ||
                (index != 0 && event.Sequence <= previousSequence))
                orderOk = FALSE;
            previousSequence = event.Sequence;
            ++received;

            LMV_GUEST_STUB_CONTINUE continueRequest = {};
            continueRequest.Version = LMV_CPUID_PROTOCOL_VERSION;
            continueRequest.Size = sizeof(continueRequest);
            continueRequest.Sequence = event.Sequence;
            continueRequest.Disposition = LMV_GUEST_STUB_DISPOSITION_CONTINUE;
            returned = 0;
            if (!DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_CONTINUE),
                                 &continueRequest, sizeof(continueRequest),
                                 &continueRequest, sizeof(continueRequest), &returned, nullptr) ||
                continueRequest.State != LMV_GUEST_STUB_STATE_CONTINUED)
            {
                continueOk = FALSE;
                error = GetLastError();
                break;
            }
        }
        const BOOL pass = triggerOk && orderOk && continueOk && received == eventCount;
        std::printf("guest stub-queue-smoke: %s trigger=%s order=%s continue=%s received=%lu capacity=%u win32=%lu\n",
                    pass ? "PASS" : "FAIL", triggerOk ? "ok" : "fail",
                    orderOk ? "ok" : "fail", continueOk ? "ok" : "fail",
                    received, LMV_GUEST_STUB_QUEUE_CAPACITY, error);
        CloseHandle(device);
        return pass ? 0 : 5;
    }
    if (argc == 2 && wcscmp(argv[1], L"--stub-queue-full-smoke") == 0)
    {
        const ULONG eventCount = LMV_GUEST_STUB_QUEUE_CAPACITY;
        BOOL fillOk = TRUE;
        DWORD returned = 0;
        DWORD error = 0;
        for (ULONG index = 0; index < eventCount; ++index)
        {
            LMV_GUEST_STUB_REQUEST request = {};
            request.Version = LMV_CPUID_PROTOCOL_VERSION;
            request.Size = sizeof(request);
            request.ProcessId = 5000 + index;
            request.ThreadId = 9000 + index;
            if (!DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_TRIGGER),
                                  &request, sizeof(request), nullptr, 0, &returned, nullptr))
            {
                fillOk = FALSE;
                error = GetLastError();
                break;
            }
        }

        LMV_GUEST_STUB_REQUEST overflowRequest = {};
        overflowRequest.Version = LMV_CPUID_PROTOCOL_VERSION;
        overflowRequest.Size = sizeof(overflowRequest);
        overflowRequest.ProcessId = 6000;
        overflowRequest.ThreadId = 10000;
        const BOOL overflowRejected = !DeviceIoControl(
            device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_TRIGGER),
            &overflowRequest, sizeof(overflowRequest), nullptr, 0, &returned, nullptr);
        if (overflowRejected && error == 0)
            error = GetLastError();

        BOOL orderOk = fillOk;
        BOOL continueOk = fillOk;
        ULONG received = 0;
        ULONG previousSequence = 0;
        for (ULONG index = 0; fillOk && index < eventCount; ++index)
        {
            LMV_GUEST_STUB_WAIT_REQUEST waitRequest = {};
            waitRequest.Version = LMV_CPUID_PROTOCOL_VERSION;
            waitRequest.Size = sizeof(waitRequest);
            waitRequest.TimeoutMilliseconds = 1000;
            LMV_GUEST_STUB_EVENT event = {};
            returned = 0;
            if (!DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_WAIT_EVENT),
                                 &waitRequest, sizeof(waitRequest), &event, sizeof(event),
                                 &returned, nullptr))
            {
                orderOk = FALSE;
                error = GetLastError();
                break;
            }
            if (event.State != LMV_GUEST_STUB_STATE_PENDING ||
                event.ProcessId != 5000 + index ||
                event.ThreadId != 9000 + index ||
                (index != 0 && event.Sequence <= previousSequence))
            {
                orderOk = FALSE;
            }
            previousSequence = event.Sequence;
            ++received;

            LMV_GUEST_STUB_CONTINUE continueRequest = {};
            continueRequest.Version = LMV_CPUID_PROTOCOL_VERSION;
            continueRequest.Size = sizeof(continueRequest);
            continueRequest.Sequence = event.Sequence;
            continueRequest.Disposition = LMV_GUEST_STUB_DISPOSITION_CONTINUE;
            returned = 0;
            if (!DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_CONTINUE),
                                 &continueRequest, sizeof(continueRequest),
                                 &continueRequest, sizeof(continueRequest), &returned, nullptr) ||
                continueRequest.State != LMV_GUEST_STUB_STATE_CONTINUED)
            {
                continueOk = FALSE;
                error = GetLastError();
                break;
            }
        }

        LMV_GUEST_STUB_REQUEST recoveryRequest = {};
        recoveryRequest.Version = LMV_CPUID_PROTOCOL_VERSION;
        recoveryRequest.Size = sizeof(recoveryRequest);
        recoveryRequest.ProcessId = 7000;
        recoveryRequest.ThreadId = 11000;
        const BOOL recoveryTriggerOk = DeviceIoControl(
            device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_TRIGGER),
            &recoveryRequest, sizeof(recoveryRequest), nullptr, 0, &returned, nullptr);
        LMV_GUEST_STUB_WAIT_REQUEST recoveryWait = {};
        recoveryWait.Version = LMV_CPUID_PROTOCOL_VERSION;
        recoveryWait.Size = sizeof(recoveryWait);
        recoveryWait.TimeoutMilliseconds = 1000;
        LMV_GUEST_STUB_EVENT recoveryEvent = {};
        const BOOL recoveryWaitOk = recoveryTriggerOk && DeviceIoControl(
            device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_WAIT_EVENT),
            &recoveryWait, sizeof(recoveryWait), &recoveryEvent, sizeof(recoveryEvent),
            &returned, nullptr);
        LMV_GUEST_STUB_CONTINUE recoveryContinue = {};
        recoveryContinue.Version = LMV_CPUID_PROTOCOL_VERSION;
        recoveryContinue.Size = sizeof(recoveryContinue);
        recoveryContinue.Sequence = recoveryEvent.Sequence;
        recoveryContinue.Disposition = LMV_GUEST_STUB_DISPOSITION_CONTINUE;
        const BOOL recoveryContinueOk = recoveryWaitOk && DeviceIoControl(
            device, static_cast<DWORD>(IOCTL_LMV_GUEST_STUB_CONTINUE),
            &recoveryContinue, sizeof(recoveryContinue), &recoveryContinue,
            sizeof(recoveryContinue), &returned, nullptr) &&
            recoveryContinue.State == LMV_GUEST_STUB_STATE_CONTINUED;

        const BOOL pass = fillOk && overflowRejected && orderOk && continueOk &&
                          received == eventCount && recoveryTriggerOk &&
                          recoveryWaitOk && recoveryEvent.ProcessId == 7000 &&
                          recoveryEvent.ThreadId == 11000 && recoveryContinueOk;
        std::printf("guest stub-queue-full-smoke: %s fill=%s overflow=%s order=%s "
                    "continue=%s received=%lu capacity=%u recovery=%s overflow-win32=%lu\n",
                    pass ? "PASS" : "FAIL", fillOk ? "ok" : "fail",
                    overflowRejected ? "rejected" : "accepted", orderOk ? "ok" : "fail",
                    continueOk ? "ok" : "fail", received, LMV_GUEST_STUB_QUEUE_CAPACITY,
                    recoveryContinueOk ? "ok" : "fail", error);
        CloseHandle(device);
        return pass ? 0 : 5;
    }
    if (argc == 2 && wcscmp(argv[1], L"--exec-probe-reloc-smoke") == 0)
    {
        const int result = RunExecProbeRelocSmoke(device);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--exec-probe-stress") == 0)
    {
        const int result = RunExecProbeStress(device);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--exec-probe-smoke") == 0)
    {
        const int result = RunExecProbeSmoke(device);
        CloseHandle(device);
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--resolve-ki") == 0)
    {
        LMV_GUEST_KI_DISPATCH_INFO info = {};
        DWORD returned = 0;
        returned = 0;
        const BOOL ok = DeviceIoControl(
            device, static_cast<DWORD>(IOCTL_LMV_GUEST_RESOLVE_KI_DISPATCH),
            nullptr, 0, &info, sizeof(info), &returned, nullptr);
        const DWORD error = ok ? 0 : GetLastError();
        std::printf("ki-dispatch: %s status=%08lx method=%lu VA=%llx GPA=%llx HPA=%llx kernel-base=%llx kernel-size=%lx page-offset=%lx bytes=%lu win32=%lu\n",
                    ok && info.Status == 0 && info.RoutineAddress != 0 &&
                    info.GuestPhysicalAddress != 0 && info.HostPhysicalAddress != 0 &&
                    info.BytesRead != 0 && info.PrologueVerified != 0 ? "PASS" : "FAIL",
                    info.Status, info.Method, info.RoutineAddress,
                    info.GuestPhysicalAddress, info.HostPhysicalAddress,
                    info.KernelBase, info.KernelImageSize, info.PageOffset,
                    info.BytesRead, error);
        std::printf("ki-dispatch prologue: verified=%lu jump-patch=%lu replay-window=%lu sub-rsp=%lx resume=%llx\n",
                    info.PrologueVerified, info.JumpPatchBytes,
                    info.ReplayPrologueBytes, info.SubRspAllocation,
                    info.ResumeAddress);
        std::printf("ki-dispatch bytes:");
        for (ULONG index = 0; index < info.BytesRead && index < sizeof(info.OriginalBytes); ++index)
            std::printf(" %02x", info.OriginalBytes[index]);
        std::printf("\n");
        CloseHandle(device);
        return ok && info.Status == 0 ? 0 : 5;
    }
    if (argc >= 2 && (wcscmp(argv[1], L"--hook-install") == 0 ||
                      wcscmp(argv[1], L"--hook-remove") == 0 ||
                      wcscmp(argv[1], L"--dbg-attach") == 0 ||
                      wcscmp(argv[1], L"--dbg-detach") == 0))
    {
        const int result = RunDebugHookCommand(device, argc, argv);
        CloseHandle(device);
        return result;
    }
    LMV_GUEST_QUERY_STATUS status = {};
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_QUERY_STATUS),
                              nullptr, 0, &status, sizeof(status), &returned, nullptr);
    if (!ok)
    {
        std::printf("query status failed: %lu\n", GetLastError());
        CloseHandle(device);
        return 1;
    }
    std::printf("guest query-status: status=%08lx flags=%08lx version=%lu size=%lu\n",
                status.Status, status.Flags, status.Version, status.Size);

    LMV_GUEST_QUERY_NPT_STATS stats = {};
    returned = 0;
    ok = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_QUERY_NPT_STATS),
                         nullptr, 0, &stats, sizeof(stats), &returned, nullptr);
    if (!ok)
    {
        std::printf("query npt stats failed: %lu\n", GetLastError());
        CloseHandle(device);
        return 1;
    }
    std::printf("guest npt-stats: status=%08lx vmexit=%llu npf=%llu processors=%llu shared-page=%llx\n",
                stats.Status, stats.VmExitCount, stats.NpfCount,
                stats.ProcessorCount, stats.SharedPageGpa);

    if (argc == 2 && (wcscmp(argv[1], L"--hook-smoke") == 0 ||
                      wcscmp(argv[1], L"--hook-read-smoke") == 0))
    {
        const bool requireOriginalRead = wcscmp(argv[1], L"--hook-read-smoke") == 0;
        LMV_GUEST_DEBUG_HOOK_SMOKE_RESULT test = {};
        returned = 0;
        const BOOL testOk = DeviceIoControl(
            device, static_cast<DWORD>(requireOriginalRead ?
                IOCTL_LMV_GUEST_DEBUG_HOOK_READ_SMOKE : IOCTL_LMV_GUEST_DEBUG_HOOK_SMOKE),
            nullptr, 0, &test, sizeof(test), &returned, nullptr);
        const DWORD error = testOk ? 0 : GetLastError();
        if (requireOriginalRead)
        {
            const bool pass = testOk && test.Status == 0 && test.Step == 5 &&
                              returned == sizeof(test) && test.PatchedValue == test.OriginalValue;
            std::printf("guest hook-read-smoke: %s status=%08lx step=%lu observed=%llx original=%llx "
                        "restored=%llx processors=%lu/%lu win32=%lu\n",
                        pass ? "PASS" : "FAIL", test.Status, test.Step,
                        test.PatchedValue, test.OriginalValue, test.RestoredValue,
                        test.ProcessorsSeen, test.ProcessorsExpected, error);
            CloseHandle(device);
            return pass ? 0 : 5;
        }        std::printf("guest hook-smoke: %s status=%08lx step=%lu GPA=%llx original-HPA=%llx shadow-HPA=%llx original=%llx patched=%llx restored=%llx processors=%lu/%lu win32=%lu\n",
                    testOk && test.Status == 0 && test.Step == 5 &&
                    returned == sizeof(test) ? "PASS" : "FAIL",
                    test.Status, test.Step, test.GuestPhysicalAddress,
                    test.OriginalHostPhysicalAddress, test.ShadowHostPhysicalAddress,
                    test.OriginalValue, test.PatchedValue, test.RestoredValue,
                    test.ProcessorsSeen, test.ProcessorsExpected, error);
        CloseHandle(device);
        return testOk && test.Status == 0 && test.Step == 5 &&
               returned == sizeof(test) ? 0 : 5;
    }
    if (argc == 2 && wcscmp(argv[1], L"--npt-smoke") == 0)
    {
        LMV_GUEST_NPT_SMOKE_RESULT test = {};
        returned = 0;
        BOOL testOk = DeviceIoControl(device, static_cast<DWORD>(IOCTL_LMV_GUEST_NPT_SMOKE_TEST),
                                      nullptr, 0, &test, sizeof(test), &returned, nullptr);
        std::printf("guest npt-smoke: %s status=%08lx step=%lu GPA=%llx HPA=%llx original=%llx mapped=%llx restored=%llx processors=%lu/%lu\n",
                    testOk && test.Status == 0 && test.Step == 6 ? "PASS" : "FAIL",
                    test.Status, test.Step, test.GuestPhysicalAddress,
                    test.HostPhysicalAddress, test.OriginalValue,
                    test.MappedValue, test.RestoredValue,
                    test.ProcessorsSeen, test.ProcessorsExpected);
        if (!testOk || test.Status != 0 || test.Step != 6)
        {
            std::printf("npt smoke IOCTL error: %lu\n", GetLastError());
            CloseHandle(device);
            return 3;
        }
    }
    CloseHandle(device);
    return status.Status == 0 && stats.Status == 0 ? 0 : 2;
}
