#include "exec_relocator.hpp"
#include "../LMvtdriverDebugger/src/zydis/Zydis.h"

#include <cstring>
#include <limits>

ExecRelocationResult RelocateRipRelativeInstruction(
    const std::uint8_t* source, std::size_t sourceLength,
    std::uintptr_t sourceAddress, std::uintptr_t destinationAddress,
    std::uint8_t* destination, std::size_t destinationLength)
{
    ExecRelocationResult result = { ExecRelocationStatus::InvalidInput, 0, 0 };
    if (source == nullptr || destination == nullptr || sourceLength == 0 ||
        destinationLength == 0 || sourceAddress == 0 || destinationAddress == 0)
        return result;

    ZydisDecoder decoder = {};
    ZydisDecodedInstruction instruction = {};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT] = {};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                        ZYDIS_STACK_WIDTH_64)) ||
        !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, source, sourceLength,
                                              &instruction, operands)) ||
        instruction.length > destinationLength)
        return result;
    result.Length = instruction.length;
    if (instruction.meta.category == ZYDIS_CATEGORY_CALL ||
        instruction.meta.category == ZYDIS_CATEGORY_COND_BR ||
        instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
        instruction.meta.category == ZYDIS_CATEGORY_RET)
    {
        result.Status = ExecRelocationStatus::UnsupportedControlFlow;
        return result;
    }
    if (instruction.encoding != ZYDIS_INSTRUCTION_ENCODING_LEGACY ||
        (instruction.mnemonic != ZYDIS_MNEMONIC_MOV &&
         instruction.mnemonic != ZYDIS_MNEMONIC_LEA))
    {
        result.Status = ExecRelocationStatus::UnsupportedInstruction;
        return result;
    }

    const ZydisDecodedOperand* relativeOperand = nullptr;
    for (std::uint8_t index = 0; index < instruction.operand_count; ++index)
    {
        const ZydisDecodedOperand& operand = operands[index];
        if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative)
        {
            result.Status = ExecRelocationStatus::UnsupportedControlFlow;
            return result;
        }
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
            operand.mem.base == ZYDIS_REGISTER_RIP)
        {
            if (relativeOperand != nullptr)
            {
                result.Status = ExecRelocationStatus::UnsupportedInstruction;
                return result;
            }
            relativeOperand = &operand;
        }
    }
    if (relativeOperand == nullptr || instruction.raw.disp.size != 32 ||
        instruction.raw.disp.offset + sizeof(std::int32_t) > instruction.length ||
        sourceAddress > std::numeric_limits<std::uintptr_t>::max() - instruction.length ||
        destinationAddress > std::numeric_limits<std::uintptr_t>::max() - instruction.length)
    {
        result.Status = ExecRelocationStatus::UnsupportedInstruction;
        return result;
    }
    ZyanU64 target = 0;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, relativeOperand,
                                                sourceAddress, &target)))
    {
        result.Status = ExecRelocationStatus::UnsupportedInstruction;
        return result;
    }
    const std::uintptr_t destinationEnd = destinationAddress + instruction.length;
    std::int32_t displacement = 0;
    if (target >= destinationEnd)
    {
        const std::uint64_t distance = target - destinationEnd;
        if (distance > static_cast<std::uint64_t>(INT32_MAX))
        {
            result.Status = ExecRelocationStatus::OutOfRange;
            return result;
        }
        displacement = static_cast<std::int32_t>(distance);
    }
    else
    {
        const std::uint64_t distance = destinationEnd - target;
        if (distance > static_cast<std::uint64_t>(INT32_MAX) + 1)
        {
            result.Status = ExecRelocationStatus::OutOfRange;
            return result;
        }
        displacement = static_cast<std::int32_t>(-static_cast<std::int64_t>(distance));
    }
    std::memcpy(destination, source, instruction.length);
    std::memcpy(destination + instruction.raw.disp.offset,
                &displacement, sizeof(displacement));
    result.Status = ExecRelocationStatus::Success;
    result.Target = static_cast<std::uintptr_t>(target);
    return result;
}
