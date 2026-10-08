#include "ki_replay.hpp"
#include "exec_relocator.hpp"
#include "../LMvtdriverDebugger/src/zydis/Zydis.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

bool RunKiReplaySmoke(const LMV_GUEST_KI_DISPATCH_INFO& info)
{
    constexpr unsigned char first[] = {
        0x40, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x81, 0xEC
    };
    constexpr unsigned char middle[] = {
        0x48, 0x8D, 0x6C, 0x24, 0x30, 0x48, 0x8B, 0x05
    };
    constexpr unsigned char last[] = { 0x48, 0x33, 0xC5 };
    constexpr unsigned lengths[] = { 2, 1, 1, 2, 2, 2, 7, 5, 7, 3 };
    const bool shape = info.Status == 0 && info.BytesRead == sizeof(info.OriginalBytes) &&
        info.RoutineAddress != 0 &&
        std::memcmp(info.OriginalBytes, first, sizeof(first)) == 0 &&
        std::memcmp(info.OriginalBytes + 17, middle, sizeof(middle)) == 0 &&
        std::memcmp(info.OriginalBytes + 29, last, sizeof(last)) == 0;
    std::uint32_t allocation = 0;
    if (shape)
        std::memcpy(&allocation, info.OriginalBytes + 13, sizeof(allocation));
    if (!shape || allocation != 0x188)
    {
        std::printf("guest ki-replay-smoke: REVIEW reason=unexpected-prologue\n");
        return false;
    }

    ZydisDecoder decoder = {};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                        ZYDIS_STACK_WIDTH_64)))
        return false;
    unsigned offset = 0;
    for (unsigned length : lengths)
    {
        ZydisDecodedInstruction decoded = {};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT] = {};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder,
                info.OriginalBytes + offset, info.BytesRead - offset, &decoded, operands)) ||
            decoded.length != length)
        {
            std::printf("guest ki-replay-smoke: REVIEW reason=decode offset=%x\n", offset);
            return false;
        }
        offset += length;
    }
    if (offset != sizeof(info.OriginalBytes))
        return false;

    auto memory = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (memory == nullptr)
        return false;
    auto source = memory;
    auto replay = memory + 4096;
    constexpr std::uint64_t cookie = 0x1122334455667788ull;
    std::memcpy(source, info.OriginalBytes, sizeof(info.OriginalBytes));
    std::memcpy(source + 0x200, &cookie, sizeof(cookie));
    const auto sourceEnd = reinterpret_cast<std::uintptr_t>(source + 29);
    const auto cookieAddress = reinterpret_cast<std::uintptr_t>(source + 0x200);
    const auto displacement = static_cast<std::int32_t>(cookieAddress - sourceEnd);
    std::memcpy(source + 25, &displacement, sizeof(displacement));
    std::memcpy(replay, source, 22);
    const auto relocated = RelocateRipRelativeInstruction(
        source + 22, 7, reinterpret_cast<std::uintptr_t>(source + 22),
        reinterpret_cast<std::uintptr_t>(replay + 22), replay + 22, 7);
    bool pass = relocated.Status == ExecRelocationStatus::Success &&
                relocated.Length == 7 && relocated.Target == cookieAddress;
    constexpr unsigned char tail[] = {
        0x48, 0x33, 0xC5,
        0x48, 0x33, 0xC5,
        0x48, 0x81, 0xC4, 0x88, 0x01, 0x00, 0x00,
        0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5C, 0x5E, 0x5B, 0x5D, 0xC3
    };
    if (pass)
        std::memcpy(replay + 29, tail, sizeof(tail));
    DWORD previous = 0;
    pass = pass && VirtualProtect(replay, 4096, PAGE_EXECUTE_READ, &previous) != 0;
    if (pass)
    {
        using ReplayFunction = std::uint64_t (*)();
        const auto function = reinterpret_cast<ReplayFunction>(replay);
        for (unsigned attempt = 0; attempt < 16 && pass; ++attempt)
            pass = function() == cookie;
    }
    std::printf("guest ki-replay-smoke: %s instructions=10 replay=32 rip-target=%s stack=%x returns=%s synthetic=1\n",
                pass ? "PASS" : "FAIL", relocated.Target == cookieAddress ? "preserved" : "mismatch",
                allocation, pass ? "ok" : "failed");
    VirtualFree(memory, 0, MEM_RELEASE);
    return pass;
}
