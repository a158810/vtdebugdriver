#pragma once

#include "../lmv_control_shared.h"

#include <cstddef>
#include <cstdint>

enum class KiStubPlanStatus
{
    Success,
    InvalidInput,
    UnexpectedPrologue,
    InvalidWindow,
    RelocationFailed,
    InvalidCandidate
};

struct KiStubPlan
{
    static constexpr std::size_t ReplayLength = 32;
    static constexpr std::size_t JumpLength = 14;
    static constexpr std::size_t Length = ReplayLength + JumpLength;
    std::uint8_t Bytes[Length];
    std::uint64_t CandidateAddress;
    std::uint64_t RipTarget;
    std::uint64_t ResumeAddress;
};

KiStubPlanStatus BuildKiStubPlan(const LMV_GUEST_KI_DISPATCH_INFO& info,
                                 std::uint64_t candidateAddress,
                                 KiStubPlan* plan);
