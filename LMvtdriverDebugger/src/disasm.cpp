#include "stdafx.h"
#include "disasm.h"
#include "zydis/Zydis.h"
#include <cstring>
#include <cstdio>
#include <string>

// ============================================================================
// Zydis-backed x86-64 disassembler (ported from the LMX64Dbg/x64dbg approach).
// ============================================================================

namespace {
    struct ZydisState {
        ZydisDecoder   decoder;
        ZydisFormatter formatter;
        bool           init = false;
        void Init() {
            if (init) return;
            ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
            ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SIZE, ZYAN_TRUE);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_TRUE);
            init = true;
        }
    };
    ZydisState& State() {
        static ZydisState s;
        s.Init();
        return s;
    }
}

DisasmInstruction DisasmOne(uint64_t address, const uint8_t* code, size_t max_len) {
    DisasmInstruction inst = {};
    inst.address = address;
    inst.length = 0;
    inst.is_branch = false;
    inst.is_call = false;
    if (!code || max_len == 0) return inst;

    ZydisDecodedInstruction info;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZydisState& st = State();
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&st.decoder, code, max_len, &info, operands))) {
        // Could not decode: emit a single "db 0x??" placeholder byte so the
        // caller can still advance past this byte instead of looping forever.
        inst.length = 1;
        inst.mnemonic = "db";
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%02X", code[0]);
        inst.operands = buf;
        snprintf(buf, sizeof(buf), "%02X", code[0]);
        inst.bytes = buf;
        return inst;
    }

    // Bytes column.
    char bytebuf[64];
    size_t boff = 0;
    for (int i = 0; i < info.length && i < (int)max_len; i++) {
        int w = snprintf(bytebuf + boff, sizeof(bytebuf) - boff, "%02X", code[i]);
        if (w <= 0) break;
        boff += w;
    }
    inst.bytes.assign(bytebuf, boff);
    inst.length = info.length;

    // Mnemonic + operands. Format a full line and split at the first space for
    // maximal fidelity with the reference (INTEL syntax, forced size/segment).
    char text[256];
    if (ZYAN_SUCCESS(ZydisFormatterFormatInstruction(
            &st.formatter, &info, operands, info.operand_count_visible,
            text, sizeof(text), address, nullptr))) {
        std::string full(text);
        size_t sp = full.find(' ');
        if (sp == std::string::npos) {
            inst.mnemonic = full;
        } else {
            inst.mnemonic = full.substr(0, sp);
            inst.operands = full.substr(sp + 1);
        }
    } else {
        inst.mnemonic = ZydisMnemonicGetString(info.mnemonic);
    }

    // Branch / call classification for callers that rely on it (e.g. StepOver).
    switch (info.mnemonic) {
        case ZYDIS_MNEMONIC_CALL:
        case ZYDIS_MNEMONIC_SYSCALL:
        case ZYDIS_MNEMONIC_SYSENTER:
            inst.is_call = true;
            inst.is_branch = true;
            break;
        case ZYDIS_MNEMONIC_RET:
        case ZYDIS_MNEMONIC_IRET:
        case ZYDIS_MNEMONIC_IRETD:
        case ZYDIS_MNEMONIC_IRETQ:
        case ZYDIS_MNEMONIC_SYSRET:
        case ZYDIS_MNEMONIC_SYSEXIT:
        case ZYDIS_MNEMONIC_JMP:
        case ZYDIS_MNEMONIC_LOOP:
        case ZYDIS_MNEMONIC_LOOPE:
        case ZYDIS_MNEMONIC_LOOPNE:
        case ZYDIS_MNEMONIC_INT:
        case ZYDIS_MNEMONIC_INT1:
        case ZYDIS_MNEMONIC_INT3:
            inst.is_branch = true;
            break;
        default:
            if (info.meta.category == ZYDIS_CATEGORY_COND_BR ||
                info.meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
                inst.is_branch = true;
            }
            break;
    }

    return inst;
}

std::vector<DisasmInstruction> DisasmRange(uint64_t address, const uint8_t* code, size_t max_len, int max_count) {
    std::vector<DisasmInstruction> result;
    size_t offset = 0;
    for (int i = 0; i < max_count && offset < max_len; i++) {
        DisasmInstruction inst = DisasmOne(address + offset, code + offset, max_len - offset);
        if (inst.length <= 0) break;
        result.push_back(inst);
        offset += inst.length;
    }
    return result;
}

std::vector<DisasmInstruction> DisasmProcessMemory(HANDLE hProcess, uint64_t address, int count) {
    uint8_t buf[256];
    SIZE_T nread;
    if (!ReadProcessMemory(hProcess, (LPCVOID)address, buf, sizeof(buf), &nread)) {
        return {};
    }
    return DisasmRange(address, buf, nread, count);
}


// ============================================================================
// x86-64 instruction length decoder.
// Used only for skipping a faulting instruction safely. Returns 0 on an
// encoding we do not understand so callers can fall back.
// ============================================================================


// ============================================================================
// x86-64 instruction length decoder.
// Used to advance RIP past a faulting instruction. Returns 0 when the encoding
// is not understood; callers must fall back instead of advancing blindly.
// ============================================================================
int X64InstructionLength(const uint8_t* code, size_t max_len) {
    if (!code || max_len == 0) return 0;
    size_t i = 0;
    bool rex = false;
    bool w = false;

    while (i < max_len) {
        uint8_t b = code[i];
        if (b >= 0x40 && b <= 0x4F) { rex = true; w = (b & 0x08) != 0; i++; }
        else if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                 b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) i++;
        else break;
    }
    if (i >= max_len) return 0;
    uint8_t op = code[i++];

    // imm64 mov (B8-FF range: mov r64, imm64 with REX.W; mov r32, imm32 otherwise)
    if (op >= 0xB8 && op <= 0xBF) {
        int n = w ? 8 : 4;
        if (i + n > max_len) return 0;
        return (int)(i + n);
    }

    // simple single-byte instructions
    if (op == 0x90 || op == 0xC3 || op == 0xCB || op == 0xCC || op == 0xCD || op == 0xCE ||
        op == 0x9C || op == 0x9D || op == 0xFA || op == 0xFB || op == 0xFC || op == 0xFD ||
        op == 0xC9 || op == 0xCA || op == 0xF4) {
        return (int)i;
    }

    // push/pop regs
    if ((op >= 0x50 && op <= 0x57) || (op >= 0x58 && op <= 0x5F)) return (int)i;

    // push imm8 (6A), push imm32 (68)
    if (op == 0x6A) return (i < max_len) ? (int)(i + 1) : 0;
    if (op == 0x68) return (i + 4 <= max_len) ? (int)(i + 4) : 0;

    // jcc rel8, jmp rel8
    if ((op >= 0x70 && op <= 0x7F) || op == 0xEB) return (i < max_len) ? (int)(i + 1) : 0;

    // call/jmp rel32
    if (op == 0xE8 || op == 0xE9) return (i + 4 <= max_len) ? (int)(i + 4) : 0;

    // 0F map: near jcc only
    if (op == 0x0F) {
        if (i >= max_len) return 0;
        uint8_t op2 = code[i++];
        if (op2 >= 0x80 && op2 <= 0x8F) return (i + 4 <= max_len) ? (int)(i + 4) : 0;
        return 0;
    }

    // moffs A0-A3
    if (op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA3) {
        int n = w ? 8 : 4;
        return (i + n <= max_len) ? (int)(i + n) : 0;
    }

    // Parse ModR/M + SIB + displacement.
    std::vector<uint8_t> no; // (placeholder unused)
    size_t pos = i;
    // (a proper decoder would need SIB; for our skip use-case we still need it)
    if (pos >= max_len) return 0;
    uint8_t modrm = code[pos++];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm = modrm & 7;
    uint8_t reg = (modrm >> 3) & 7;
    int disp_len = 0;
    if (mod != 3 && rm == 4) {
        if (pos >= max_len) return 0;
        uint8_t sib = code[pos++];
        if ((sib & 7) == 5 && mod == 0) disp_len = 4;
    }
    if (mod == 1) disp_len = 1;
    else if (mod == 2) disp_len = 4;
    else if (mod == 0 && rm == 5) disp_len = 4;
    if (pos + disp_len > max_len) return 0;
    pos += disp_len;

    // opcode families that carry a ModR/M
    bool has_modrm =
        (op >= 0x88 && op <= 0x8B) || (op >= 0x8C && op <= 0x8F) ||
        (op == 0x01 || op == 0x03 || op == 0x09 || op == 0x0B || op == 0x11 || op == 0x13 ||
         op == 0x19 || op == 0x1B || op == 0x21 || op == 0x23 || op == 0x29 || op == 0x2B ||
         op == 0x31 || op == 0x33 || op == 0x39 || op == 0x3B || op == 0x84 || op == 0x85 ||
         op == 0x86 || op == 0x87 || op == 0xF6 || op == 0xF7 || op == 0xFE || op == 0xFF ||
         op == 0x80 || op == 0x81 || op == 0x83 || op == 0xC6 || op == 0xC7);
    if (!has_modrm) return 0;

    // immediates
    int imm_len = 0;
    if (op == 0x80 || op == 0x83 || op == 0xC6) imm_len = 1;
    else if (op == 0x81 || op == 0xC7) imm_len = 4;
    else if (op == 0xF6) imm_len = (reg <= 1) ? 1 : 0;
    else if (op == 0xF7) imm_len = (reg <= 1) ? 4 : 0;
    else if (op == 0xFE || op == 0xFF) imm_len = 0;

    if (pos + imm_len > max_len) return 0;
    return (int)(pos + imm_len);
}