#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>
#include <atomic>
#include <stdexcept>
#include "dbg_ioctl.h"
#include "disasm.h"

// ============================================================================
// Breakpoint types
// ============================================================================
enum class BreakpointType {
    Software,       // INT3 (0xCC), written through the driver
    Hardware,       // DR0-DR3 (not supported by driver yet)
    Memory          // DR2 data watchpoint (transparent mode only)
};

enum class HardwareBreakCondition {
    Execute = 0,
    Write   = 1,
    ReadWrite = 3
};

enum class MemoryBreakCondition {
    Read    = 0,
    Write   = 1,
    Execute = 2,
    ReadWrite = 3
};

// ============================================================================
// Breakpoint record
// ============================================================================
struct Breakpoint {
    uint32_t        id;
    BreakpointType  type;
    uint64_t        address;
    uint64_t        size;
    bool            enabled;
    uint8_t         orig_byte;      // software breakpoint: original byte
    DWORD           old_protect;
    uint8_t         hw_slot;        // hardware breakpoint: DR0-DR3 slot (0-3)
    HardwareBreakCondition hw_cond;
    MemoryBreakCondition    mem_cond;

    Breakpoint() : id(0), type(BreakpointType::Software), address(0), size(1),
                   enabled(false), orig_byte(0), old_protect(0),
                   hw_slot(0xFF), hw_cond(HardwareBreakCondition::Execute),
                   mem_cond(MemoryBreakCondition::ReadWrite) {}
};

// ============================================================================
// x64 register context (mirrors DBG_CONTEXT from dbg_ioctl.h)
// ============================================================================
struct RegisterContext {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip;
    uint64_t rflags;
    uint16_t cs, ds, es, fs, gs, ss;
    // the driver does not expose debug registers yet; kept for UI layout
    uint64_t dr0, dr1, dr2, dr3, dr6, dr7;
    uint64_t xmm[16][2];
};

// ============================================================================
// Module info
// ============================================================================
struct ModuleInfo {
    std::string name;
    uint64_t    base;
    uint64_t    size;
    uint64_t    entry;
};

// ============================================================================
// Main debugger class (driver-backed)
// ============================================================================
class Debugger {
public:
    Debugger();
    ~Debugger();

    // ---- process control ----
    bool AttachProcess(DWORD pid);
    bool Detach();
    bool PrepareDriverUnload();
    void Kill();

    // ---- event control ----
    bool GetEvent(DBG_EVENT_INFO& evt);              // Windows debug events run on the worker
    void StopEventBackend();
    bool WaitForEventBackend(DWORD timeoutMilliseconds);
    ULONG GetEventBackendPhase();
    bool ProcessEvent(const DBG_EVENT_INFO& evt);    // true = pause UI
    bool Continue();                                 // 工作线程继续 Windows 调试事件
    bool BreakTarget();                              // IOCTL_BREAK_TARGET (F12)
    bool StepInto(DWORD tid);
    bool StepOver(DWORD tid);

    // ---- transparent mode ----
    bool SetDebugMode(ULONG mode);                   // Set LMV_DEBUG_MODE_INTERACTIVE or LMV_DEBUG_MODE_TRANSPARENT
    bool PollHostEvents(HOST_EXCEPTION_EVENT& evt);  // Non-blocking poll for transparent mode events
    ULONG GetDebugMode() const { return m_debugMode; }

    // ---- breakpoints ----
    uint32_t SetSoftwareBreakpoint(uint64_t addr);
    uint32_t SetHardwareBreakpoint(uint64_t addr, HardwareBreakCondition cond);
    uint32_t SetMemoryBreakpoint(uint64_t addr, uint64_t size, MemoryBreakCondition cond);
    bool     RemoveBreakpoint(uint32_t id);
    bool     EnableBreakpoint(uint32_t id);
    bool     DisableBreakpoint(uint32_t id);
    const std::map<uint32_t, Breakpoint>& Breakpoints() const { return m_breakpoints; }

    // ---- registers ----
    RegisterContext GetRegisters(DWORD tid);
    bool TryGetRegisters(DWORD tid, RegisterContext& result);
    bool SetRegisters(DWORD tid, const RegisterContext& ctx);

    // ---- memory ----
    bool ReadMemory(uint64_t addr, void* buf, size_t size);
    bool WriteMemory(uint64_t addr, const void* buf, size_t size);

    // ---- disassembly ----
    std::vector<DisasmInstruction> Disassemble(uint64_t addr, int count = 10);

    // ---- info ----
    bool IsActive() const { return m_active; }
    DWORD GetLastErrorInfo() const { return m_last_error; }
    DWORD GetProcessId() const { return m_pid; }
    DWORD GetThreadId() const { return m_tid; }
    DWORD GetCurrentTid() const { return m_last_tid ? m_last_tid : m_tid; }
    uint64_t GetImageBase();
    std::vector<ModuleInfo> GetModules();
    uint64_t ResolveSymbol(const std::string& name);

    // ---- driver communication ----
    bool DriverConnect(const std::wstring& device_path = DEVICE_NAME);
    void DriverDisconnect();
    bool DriverIoctl(DWORD code, void* in, size_t in_size,
                     void* out, size_t out_size, DWORD* returned = nullptr);
    bool IsDriverConnected() const { return m_hDevice != INVALID_HANDLE_VALUE; }

    // Liveness check: send IOCTL_PING and validate the magic echo. Works even
    // when the persistent handle is closed (opens a temporary one), so the UI
    // can show "driver loaded but not connected yet".
    bool PingDriver(ULONG* version = nullptr, ULONG* flags = nullptr);

private:
    bool SetRipAndTrap(DWORD tid, uint64_t rip);
    bool HandleSoftwareBreakpoint(const DBG_EVENT_INFO& evt);
    bool HandleSingleStep(const DBG_EVENT_INFO& evt);
    bool ControlDr2Watch(ULONG operation, const Breakpoint& breakpoint, uint64_t* hits = nullptr);

    bool GetDriverContext(DWORD tid, DBG_CONTEXT& dc);
    bool SetDriverContext(DWORD tid, const DBG_CONTEXT& dc);

    std::atomic<bool>   m_active;
    DWORD               m_pid;
    DWORD               m_tid;
    DWORD               m_last_error;
    DWORD               m_last_tid;          // thread that raised the last event
    HANDLE              m_eventResume;
    HANDLE              m_backendReady;
    volatile LONG      m_guestAttached;
    volatile LONG      m_backendPhase;
    volatile LONG      m_eventPending;
    volatile LONG      m_resumeStatus;
    DWORD               m_eventProcessId;
    DWORD               m_eventThreadId;
    bool                m_initialBreak;
    ULONG               m_last_event_sequence; // Guest Stub 事件序号
    std::map<uint32_t, Breakpoint> m_breakpoints;
    uint32_t            m_next_bp_id;

    // StepOver temporary breakpoint
    bool                m_step_over_bp;
    uint64_t            m_step_over_return_addr;
    uint8_t             m_step_over_orig;    // original byte of the temp BP

    // software BP hit: waiting for the follow-up single-step to re-arm
    uint32_t            m_pending_bp_id;
    uint64_t            m_pending_bp_addr;

    HANDLE              m_hDevice;
    HANDLE              m_hEventDevice;    // dedicated handle for the blocking GetEvent worker
    ULONG               m_driverVersion;   // last successful ping reply
    ULONG               m_driverFlags;
    ULONG               m_debugMode;       // LMV_DEBUG_MODE_INTERACTIVE or LMV_DEBUG_MODE_TRANSPARENT
};

// ============================================================================
// Exception helper
// ============================================================================
class DebuggerException : public std::runtime_error {
public:
    DebuggerException(const std::string& msg) : std::runtime_error(msg) {}
    DebuggerException(const std::string& msg, DWORD err)
        : std::runtime_error(msg + " (error " + std::to_string(err) + ")") {}
};


