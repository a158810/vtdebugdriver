#include "debugger.h"
#include "disasm.h"
#include "driver_comm.h"
#include "utils.h"
#include <iostream>
#include <string>
#include <vector>
#include <iomanip>

static Debugger g_debugger;
static bool g_running = true;
static bool g_debugging = false;

static void PrintHelp() {
    std::cout << R"(
lmdbgr3 - User-mode Debugger
=============================

Process Control:
  open <path> [args]     - Start a new process for debugging
  attach <pid>           - Attach to a running process
  detach                 - Detach from current process
  kill                   - Kill the debugged process
  run / g / go / cont    - Continue execution

Execution:
  stepi                  - Step into (single instruction)
  stepo                  - Step over

Breakpoints:
  bp <addr>              - Set software breakpoint (0xCC)
  bh <addr> [cond]       - Set hardware breakpoint (e/w/rw)
  bm <addr> <size> [c]   - Set memory breakpoint (r/w/x/rw)
  bl                     - List all breakpoints
  bc <id>                - Clear (remove) breakpoint
  be <id>                - Enable breakpoint
  bd <id>                - Disable breakpoint

Registers:
  r                      - Display all registers
  r <reg> <value>        - Set register

Memory:
  d <addr> [count]       - Display memory (hex dump)
  e <addr> <byte>...     - Edit memory

Disassembly:
  u <addr> [count]       - Disassemble
  u                       - Disassemble at RIP

Info:
  lm                     - List modules
  base                   - Show image base
  sym <name>             - Resolve symbol

Driver:
  driver <path>          - Connect to driver
  driver.close            - Disconnect

Other:
  q / quit / exit        - Exit
  help / ?               - Show this help
)";
}

static void CmdOpen(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cerr << "Usage: open <path> [args]" << std::endl; return; }
    std::wstring path = AnsiToWide(args[1]);
    std::wstring proc_args;
    for (size_t i = 2; i < args.size(); i++) {
        if (i > 2) proc_args += L" ";
        proc_args += AnsiToWide(args[i]);
    }
    if (g_debugger.CreateProcess(path, proc_args)) g_debugging = true;
}

static void CmdAttach(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cerr << "Usage: attach <pid>" << std::endl; return; }
    DWORD pid = (DWORD)ParseHex32(args[1]);
    if (pid == 0) pid = (DWORD)std::stoul(args[1]);
    if (g_debugger.AttachProcess(pid)) g_debugging = true;
}

static void CmdDetach() { g_debugger.Detach(); g_debugging = false; }
static void CmdKill() { g_debugger.Kill(); g_debugging = false; }
static void CmdRun() { if (g_debugging) g_debugger.Run(); else std::cerr << "No process." << std::endl; }

static void CmdBreakpoint(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cerr << "Usage: bp <addr>" << std::endl; return; }
    g_debugger.SetSoftwareBreakpoint(ParseHex64(args[1]));
}

static void CmdHardwareBP(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cerr << "Usage: bh <addr> [e/w/rw]" << std::endl; return; }
    uint64_t addr = ParseHex64(args[1]);
    HardwareBreakCondition cond = HardwareBreakCondition::Execute;
    if (args.size() >= 3) {
        std::string c = args[2];
        if (c == "w") cond = HardwareBreakCondition::Write;
        else if (c == "rw") cond = HardwareBreakCondition::ReadWrite;
    }
    g_debugger.SetHardwareBreakpoint(addr, cond);
}

static void CmdMemoryBP(const std::vector<std::string>& args) {
    if (args.size() < 3) { std::cerr << "Usage: bm <addr> <size> [r/w/x/rw]" << std::endl; return; }
    uint64_t addr = ParseHex64(args[1]);
    uint64_t size = ParseHex64(args[2]);
    if (size == 0) size = 1;
    MemoryBreakCondition cond = MemoryBreakCondition::ReadWrite;
    if (args.size() >= 4) {
        std::string c = args[3];
        if (c == "r") cond = MemoryBreakCondition::Read;
        else if (c == "w") cond = MemoryBreakCondition::Write;
        else if (c == "x") cond = MemoryBreakCondition::Execute;
    }
    g_debugger.SetMemoryBreakpoint(addr, size, cond);
}

static void CmdRegisters(const std::vector<std::string>& args) {
    if (!g_debugging) { std::cerr << "No process." << std::endl; return; }
    DWORD tid = g_debugger.GetProcessId();
    if (args.size() == 1) {
        g_debugger.DisplayRegisters(tid);
    } else if (args.size() >= 3) {
        RegisterContext rc = g_debugger.GetRegisters(tid);
        std::string reg = args[1];
        uint64_t val = ParseHex64(args[2]);
        if (reg == "rax") rc.rax = val;
        else if (reg == "rbx") rc.rbx = val;
        else if (reg == "rcx") rc.rcx = val;
        else if (reg == "rdx") rc.rdx = val;
        else if (reg == "rsi") rc.rsi = val;
        else if (reg == "rdi") rc.rdi = val;
        else if (reg == "rbp") rc.rbp = val;
        else if (reg == "rsp") rc.rsp = val;
        else if (reg == "r8") rc.r8 = val;
        else if (reg == "r9") rc.r9 = val;
        else if (reg == "r10") rc.r10 = val;
        else if (reg == "r11") rc.r11 = val;
        else if (reg == "r12") rc.r12 = val;
        else if (reg == "r13") rc.r13 = val;
        else if (reg == "r14") rc.r14 = val;
        else if (reg == "r15") rc.r15 = val;
        else if (reg == "rip") rc.rip = val;
        else if (reg == "rflags") rc.rflags = val;
        else if (reg == "dr0") rc.dr0 = val;
        else if (reg == "dr1") rc.dr1 = val;
        else if (reg == "dr2") rc.dr2 = val;
        else if (reg == "dr3") rc.dr3 = val;
        else if (reg == "dr6") rc.dr6 = val;
        else if (reg == "dr7") rc.dr7 = val;
        else { std::cerr << "Unknown register: " << reg << std::endl; return; }
        g_debugger.SetRegisters(tid, rc);
        std::cout << reg << " = 0x" << FormatHex64(val) << std::endl;
    }
}

static void CmdDisplay(const std::vector<std::string>& args) {
    if (!g_debugging) { std::cerr << "No process." << std::endl; return; }
    if (args.size() < 2) { std::cerr << "Usage: d <addr> [count]" << std::endl; return; }
    uint64_t addr = ParseHex64(args[1]);
    size_t count = 128;
    if (args.size() >= 3) count = (size_t)ParseHex64(args[2]);
    g_debugger.DisplayMemory(addr, count);
}

static void CmdEdit(const std::vector<std::string>& args) {
    if (!g_debugging) { std::cerr << "No process." << std::endl; return; }
    if (args.size() < 3) { std::cerr << "Usage: e <addr> <byte>..." << std::endl; return; }
    uint64_t addr = ParseHex64(args[1]);
    for (size_t i = 2; i < args.size(); i++) {
        uint8_t b = (uint8_t)ParseHex32(args[i]);
        g_debugger.WriteMemory(addr + i - 2, &b, 1);
    }
    std::cout << "Written " << (args.size() - 2) << " bytes at 0x" << FormatHex64(addr) << std::endl;
}

static void CmdDisasm(const std::vector<std::string>& args) {
    if (!g_debugging) { std::cerr << "No process." << std::endl; return; }
    uint64_t addr = 0;
    int count = 16;
    if (args.size() >= 2) {
        addr = ParseHex64(args[1]);
        if (args.size() >= 3) count = (int)std::stoul(args[2]);
    } else {
        RegisterContext rc = g_debugger.GetRegisters(g_debugger.GetProcessId());
        addr = rc.rip;
    }
    auto instructions = g_debugger.Disassemble(addr, count);
    for (auto& inst : instructions) {
        std::cout << "  " << FormatHex64(inst.address) << "  "
                  << std::setw(24) << std::left << inst.bytes
                  << inst.mnemonic;
        if (!inst.operands.empty()) std::cout << " " << inst.operands;
        std::cout << std::endl;
    }
}

static void CmdListModules() {
    if (!g_debugging) { std::cerr << "No process." << std::endl; return; }
    auto mods = g_debugger.GetModules();
    std::cout << "  Base               Size       Name" << std::endl;
    std::cout << "  ------------------------------------------------" << std::endl;
    for (auto& m : mods) {
        std::cout << "  " << FormatHex64(m.base)
                  << "  " << std::hex << std::setw(8) << m.size << std::dec
                  << "  " << m.name << std::endl;
    }
}

static void CmdBase() {
    if (!g_debugging) { std::cerr << "No process." << std::endl; return; }
    std::cout << "Image base: 0x" << FormatHex64(g_debugger.GetImageBase()) << std::endl;
}

static void CmdSym(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cerr << "Usage: sym <name>" << std::endl; return; }
    uint64_t addr = g_debugger.ResolveSymbol(args[1]);
    if (addr) std::cout << args[1] << " = 0x" << FormatHex64(addr) << std::endl;
    else std::cout << "Symbol not found." << std::endl;
}

static void CmdDriver(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cerr << "Usage: driver <path> or driver.close" << std::endl; return; }
    if (args[1] == "close") g_debugger.DriverDisconnect();
    else g_debugger.DriverConnect(AnsiToWide(args[1]));
}

static void ProcessCommand(const std::string& line) {
    auto args = SplitArgs(line);
    if (args.empty()) return;
    std::string cmd = args[0];

    if (cmd == "q" || cmd == "quit" || cmd == "exit") {
        if (g_debugging) g_debugger.Detach();
        g_running = false;
    }
    else if (cmd == "help" || cmd == "?") PrintHelp();
    else if (cmd == "open") CmdOpen(args);
    else if (cmd == "attach") CmdAttach(args);
    else if (cmd == "detach") CmdDetach();
    else if (cmd == "kill") CmdKill();
    else if (cmd == "run" || cmd == "g" || cmd == "go" || cmd == "cont") CmdRun();
    else if (cmd == "stepi") g_debugger.StepInto(g_debugger.GetProcessId());
    else if (cmd == "stepo") g_debugger.StepOver(g_debugger.GetProcessId());
    else if (cmd == "bp") CmdBreakpoint(args);
    else if (cmd == "bh") CmdHardwareBP(args);
    else if (cmd == "bm") CmdMemoryBP(args);
    else if (cmd == "bl") g_debugger.ListBreakpoints();
    else if (cmd == "bc") { if (args.size()>=2) g_debugger.RemoveBreakpoint((uint32_t)std::stoul(args[1])); }
    else if (cmd == "be") { if (args.size()>=2) g_debugger.EnableBreakpoint((uint32_t)std::stoul(args[1])); }
    else if (cmd == "bd") { if (args.size()>=2) g_debugger.DisableBreakpoint((uint32_t)std::stoul(args[1])); }
    else if (cmd == "r") CmdRegisters(args);
    else if (cmd == "d") CmdDisplay(args);
    else if (cmd == "e") CmdEdit(args);
    else if (cmd == "u") CmdDisasm(args);
    else if (cmd == "lm") CmdListModules();
    else if (cmd == "base") CmdBase();
    else if (cmd == "sym") CmdSym(args);
    else if (cmd == "driver") CmdDriver(args);
    else std::cerr << "Unknown command: " << cmd << " (type help)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "lmdbgr3 - User-mode Debugger" << std::endl;
    std::cout << "Type 'help' for commands, 'q' to quit." << std::endl << std::endl;

    if (argc >= 2) {
        std::wstring path = AnsiToWide(argv[1]);
        std::wstring proc_args;
        for (int i = 2; i < argc; i++) {
            if (i > 2) proc_args += L" ";
            proc_args += AnsiToWide(argv[i]);
        }
        if (g_debugger.CreateProcess(path, proc_args)) g_debugging = true;
    }

    while (g_running) {
        std::cout << "lmdbgr3> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) break;

        auto args = SplitArgs(line);
        if (!args.empty()) {
            std::string cmd = args[0];
            bool run_target = (cmd == "run" || cmd == "g" || cmd == "go" || cmd == "cont" ||
                               cmd == "stepi" || cmd == "stepo");

            ProcessCommand(line);

            if (g_debugging && run_target) {
                bool waiting = true;
                while (waiting) {
                    DEBUG_EVENT evt = {};
                    DebuggerAction action = g_debugger.WaitForEvent(evt, INFINITE);

                    switch (action) {
                    case DebuggerAction::Detach:
                        g_debugging = false;
                        waiting = false;
                        std::cout << "[+] Process exited." << std::endl;
                        break;
                    case DebuggerAction::Break:
                        waiting = false;
                        break;
                    case DebuggerAction::Continue:
                        break;
                    }
                }
            }
        }
    }

    return 0;
}
