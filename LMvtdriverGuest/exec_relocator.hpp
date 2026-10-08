#pragma once

#include <cstddef>
#include <cstdint>

enum class ExecRelocationStatus
{
    Success,
    InvalidInput,
    UnsupportedInstruction,
    UnsupportedControlFlow,
    OutOfRange
};

struct ExecRelocationResult
{
    ExecRelocationStatus Status;
    std::uint8_t Length;
    std::uintptr_t Target;
};

ExecRelocationResult RelocateRipRelativeInstruction(
    const std::uint8_t* source, std::size_t sourceLength,
    std::uintptr_t sourceAddress, std::uintptr_t destinationAddress,
    std::uint8_t* destination, std::size_t destinationLength);
