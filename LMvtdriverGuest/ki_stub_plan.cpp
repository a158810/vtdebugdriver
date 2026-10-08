#include "ki_stub_plan.hpp"
#include "exec_relocator.hpp"
#include "../LMvtdriverDebugger/src/zydis/Zydis.h"

#include <cstring>
#include <limits>

KiStubPlanStatus BuildKiStubPlan(const LMV_GUEST_KI_DISPATCH_INFO& info,
                                 std::uint64_t candidateAddress,
                                 KiStubPlan* plan)
{
    if (plan == nullptr || info.Status != 0 || info.BytesRead != KiStubPlan::ReplayLength ||
        info.RoutineAddress == 0 || info.RoutineAddress >
            (std::numeric_limits<std::uint64_t>::max)() - KiStubPlan::ReplayLength)
        return KiStubPlanStatus::InvalidInput;
    if ((candidateAddress >> 48) != 0xffff ||
        candidateAddress > (std::numeric_limits<std::uint64_t>::max)() - KiStubPlan::Length)
        return KiStubPlanStatus::InvalidCandidate;

    constexpr std::uint8_t prefix[] = {
        0x40, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x81, 0xEC, 0x88, 0x01, 0x00, 0x00,
        0x48, 0x8D, 0x6C, 0x24, 0x30, 0x48, 0x8B, 0x05
    };
    constexpr std::uint8_t suffix[] = { 0x48, 0x33, 0xC5 };
    constexpr unsigned instructionLengths[] = { 2, 1, 1, 2, 2, 2, 7, 5, 7, 3 };
    if (std::memcmp(info.OriginalBytes, prefix, sizeof(prefix)) != 0 ||
        std::memcmp(info.OriginalBytes + 29, suffix, sizeof(suffix)) != 0)
        return KiStubPlanStatus::UnexpectedPrologue;

    ZydisDecoder decoder = {};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                        ZYDIS_STACK_WIDTH_64)))
        return KiStubPlanStatus::InvalidWindow;
    std::size_t offset = 0;
    for (unsigned expectedLength : instructionLengths)
    {
        ZydisDecodedInstruction instruction = {};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT] = {};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, info.OriginalBytes + offset,
                KiStubPlan::ReplayLength - offset, &instruction, operands)) ||
            instruction.length != expectedLength ||
            instruction.meta.category == ZYDIS_CATEGORY_CALL ||
            instruction.meta.category == ZYDIS_CATEGORY_COND_BR ||
            instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
            instruction.meta.category == ZYDIS_CATEGORY_RET)
            return KiStubPlanStatus::InvalidWindow;
        offset += instruction.length;
    }
    if (offset != KiStubPlan::ReplayLength)
        return KiStubPlanStatus::InvalidWindow;

    KiStubPlan next = {};
    next.CandidateAddress = candidateAddress;
    next.ResumeAddress = info.RoutineAddress + KiStubPlan::ReplayLength;
    std::memcpy(next.Bytes, info.OriginalBytes, KiStubPlan::ReplayLength);
    const auto relocation = RelocateRipRelativeInstruction(
        info.OriginalBytes + 22, KiStubPlan::ReplayLength - 22,
        static_cast<std::uintptr_t>(info.RoutineAddress + 22),
        static_cast<std::uintptr_t>(candidateAddress + 22), next.Bytes + 22,
        KiStubPlan::ReplayLength - 22);
    if (relocation.Status != ExecRelocationStatus::Success || relocation.Length != 7)
        return KiStubPlanStatus::RelocationFailed;
    next.RipTarget = relocation.Target;

    constexpr std::uint8_t jump[] = { 0xFF, 0x25, 0, 0, 0, 0 };
    std::memcpy(next.Bytes + KiStubPlan::ReplayLength, jump, sizeof(jump));
    std::memcpy(next.Bytes + KiStubPlan::ReplayLength + sizeof(jump),
                &next.ResumeAddress, sizeof(next.ResumeAddress));
    *plan = next;
    return KiStubPlanStatus::Success;
}
