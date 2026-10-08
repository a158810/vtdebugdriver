#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// ============================================================================
// 反汇编指令结构 (声明在此也 OK，debugger.h 已定义)
// 此处给出独立声明以防单独使用
// ============================================================================

// ============================================================================
// 反汇编指令结构
// ============================================================================
struct DisasmInstruction {
    uint64_t    address;
    std::string bytes;
    std::string mnemonic;
    std::string operands;
    int         length;
    bool        is_branch;
    bool        is_call;
};

// ============================================================================
// 反汇编器：基于 Zydis 风格接口的简化实现
// 当前用简单的表驱动解码 (x64)，后续可替换为 capstone/zydis
// ============================================================================

// 单条反汇编
DisasmInstruction DisasmOne(uint64_t address, const uint8_t* code, size_t max_len);

// 批量反汇编
std::vector<DisasmInstruction> DisasmRange(uint64_t address, const uint8_t* code, size_t max_len, int max_count = 10);

// 从进程内存读取并反汇编
std::vector<DisasmInstruction> DisasmProcessMemory(HANDLE hProcess, uint64_t address, int count = 10);

// x86-64 instruction length decoder (returns 0 if it cannot decode safely)
int X64InstructionLength(const uint8_t* code, size_t max_len);

