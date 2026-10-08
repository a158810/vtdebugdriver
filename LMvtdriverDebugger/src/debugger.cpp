#include "stdafx.h"
#include "debugger.h"
#include "disasm.h"
#include "utils.h"
#include "../../lmv_control_shared.h"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <vector>

// ============================================================================
// Debugger: driver-backed debugger core.
//

// ============================================================================

Debugger::Debugger()
    : m_active(false)
    , m_pid(0)
    , m_tid(0)
    , m_last_error(0)
    , m_last_tid(0)
    , m_eventResume(CreateEventW(NULL, FALSE, FALSE, NULL))
    , m_backendReady(CreateEventW(NULL, TRUE, FALSE, NULL))
    , m_guestAttached(0)
    , m_backendPhase(0)
    , m_eventPending(0)
    , m_resumeStatus(0)
    , m_eventProcessId(0)
    , m_eventThreadId(0)
    , m_initialBreak(true)
    , m_last_event_sequence(0)
    , m_next_bp_id(1)
    , m_step_over_bp(false)
    , m_step_over_return_addr(0)
    , m_step_over_orig(0)
    , m_pending_bp_id(0)
    , m_pending_bp_addr(0)
    , m_hDevice(INVALID_HANDLE_VALUE)
    , m_hEventDevice(INVALID_HANDLE_VALUE)
    , m_driverVersion(0)
    , m_driverFlags(0)
{
}

Debugger::~Debugger()
{
    if (m_active) Detach();
    DriverDisconnect();
    if (m_eventResume) CloseHandle(m_eventResume);
    if (m_backendReady) CloseHandle(m_backendReady);
}

// ============================================================================
// Driver communication
// ============================================================================
bool Debugger::DriverConnect(const std::wstring& device_path)
{
    if (m_hDevice == INVALID_HANDLE_VALUE) {
        m_hDevice = CreateFileW(device_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (m_hDevice == INVALID_HANDLE_VALUE) {
            m_last_error = GetLastError();
            std::cerr << "[!] cannot open " << WideToAnsi(device_path)
                      << ", error " << m_last_error << std::endl;
            return false;
        }
        std::cout << "[+] driver connected" << std::endl;
    }

    if (m_hEventDevice == INVALID_HANDLE_VALUE) {
        m_hEventDevice = CreateFileW(device_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (m_hEventDevice == INVALID_HANDLE_VALUE) {
            m_last_error = GetLastError();
            std::cerr << "[!] cannot open event device " << WideToAnsi(device_path)
                      << ", error " << m_last_error << std::endl;
            if (m_hDevice != INVALID_HANDLE_VALUE) {
                CloseHandle(m_hDevice);
                m_hDevice = INVALID_HANDLE_VALUE;
            }
            return false;
        }
    }


    return true;
}

void Debugger::DriverDisconnect()
{
    if (m_hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
    }
    if (m_hEventDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hEventDevice);
        m_hEventDevice = INVALID_HANDLE_VALUE;
    }
}

bool Debugger::DriverIoctl(DWORD code, void* in, size_t in_size,
                           void* out, size_t out_size, DWORD* returned)
{
    if (m_hDevice == INVALID_HANDLE_VALUE) {
        m_last_error = ERROR_FILE_NOT_FOUND;
        std::cerr << "[!] driver not connected" << std::endl;
        return false;
    }
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(m_hDevice, code,
        in, static_cast<DWORD>(in_size),
        out, static_cast<DWORD>(out_size),
        &bytes, NULL);
    if (returned) *returned = bytes;
    if (!ok) m_last_error = GetLastError();
    return ok != FALSE;
}

bool Debugger::PingDriver(ULONG* version, ULONG* flags)
{
    HANDLE h = m_hDevice;
    bool temporary = false;

    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(DEVICE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            m_last_error = GetLastError();
            return false;
        }
        temporary = true;
    }

    PING_REPLY reply = {};
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_PING,
        NULL, 0, &reply, sizeof(reply), &bytes, NULL);
    m_last_error = ok ? ERROR_SUCCESS : GetLastError();

    if (temporary) CloseHandle(h);

    if (!ok || bytes < sizeof(reply) || reply.Magic != DBGSYS_PING_MAGIC) {
        return false;
    }

    m_driverVersion = reply.Version;
    m_driverFlags   = reply.Flags;
    if (version) *version = reply.Version;
    if (flags)   *flags   = reply.Flags;
    return true;
}

// ============================================================================
// Process control
// ============================================================================
bool Debugger::AttachProcess(DWORD pid)
{
    if (m_hDevice == INVALID_HANDLE_VALUE) {
        m_last_error = ERROR_FILE_NOT_FOUND;
        std::cerr << "[!] AttachProcess: driver not connected" << std::endl;
        return false;
    }
    if (m_active && m_pid == pid) return true; // idempotent
    if (m_active && !Detach()) return false;

    LMV_GUEST_KI_SESSION session = {};
    session.Version = LMV_CPUID_PROTOCOL_VERSION;
    session.Size = sizeof(session);
    session.Operation = LMV_KI_SESSION_START;
    session.ProcessId = pid;
    DWORD returned = 0;
    if (!DriverIoctl(IOCTL_LMV_GUEST_KI_SESSION, &session, sizeof(session),
        &session, sizeof(session), &returned)) {
        std::cerr << "[!] DR3 session start failed, error " << m_last_error
                  << std::endl;
        return false;
    }
    if (returned != sizeof(session) || session.Version != LMV_CPUID_PROTOCOL_VERSION ||
        session.Size != sizeof(session) || session.ProcessId != pid ||
        session.KernelCr3 == 0 || session.KiAddress == 0 ||
        session.ProcessorsExpected == 0 || session.ProcessorsSeen != session.ProcessorsExpected) {
        session.Operation = LMV_KI_SESSION_STOP;
        DriverIoctl(IOCTL_LMV_GUEST_KI_SESSION, &session, sizeof(session), &session, sizeof(session));
        m_last_error = ERROR_INVALID_DATA;
        std::cerr << "[!] invalid DR3 session handshake" << std::endl;
        return false;
    }
    std::cout << "[+] DR3 session active: PID=" << pid
              << " kernel-cr3=0x" << std::hex << session.KernelCr3
              << " ki=0x" << session.KiAddress << std::dec
              << " CPUs=" << session.ProcessorsSeen << "/" << session.ProcessorsExpected << std::endl;


    m_active = true;
    m_pid = pid;
    m_tid = 0;
    m_last_tid = 0;
    m_last_event_sequence = 0;
    m_initialBreak = true;
    InterlockedExchange(&m_eventPending, 0);
    InterlockedExchange(&m_resumeStatus, 0);
    InterlockedExchange(&m_guestAttached, 0);
    if (m_backendReady) ResetEvent(m_backendReady);
    InterlockedExchange(&m_backendPhase, 0);
    if (m_eventResume) ResetEvent(m_eventResume);
    m_breakpoints.clear();
    m_step_over_bp = false;
    m_step_over_return_addr = 0;
    m_step_over_orig = 0;
    m_pending_bp_id = 0;
    m_pending_bp_addr = 0;

    std::cout << "[+] attached to PID " << pid << std::endl;
    return true;
}

bool Debugger::PrepareDriverUnload()
{
    if (!IsDriverConnected()) return true;
    LMV_GUEST_KI_DRAIN drain = {};
    drain.Version = LMV_CPUID_PROTOCOL_VERSION;
    drain.Size = sizeof(drain);
    drain.Operation = LMV_KI_SESSION_PREPARE_UNLOAD;
    DWORD returned = 0;
    if (!DriverIoctl(IOCTL_LMV_GUEST_KI_SESSION, &drain, sizeof(drain),
        &drain, sizeof(drain), &returned)) {
        std::cerr << "[!] prepare-unload failed, error " << m_last_error << std::endl;
        return false;
    }
    if (returned != sizeof(drain) || drain.Version != LMV_CPUID_PROTOCOL_VERSION ||
        drain.Size != sizeof(drain) || drain.Status != 0 || drain.InFlight != 0 ||
        drain.ArmedProcessors != 0 || drain.Flags != LMV_KI_DRAIN_RETIRED ||
        drain.ProcessorsSeen != drain.ProcessorsExpected) {
        m_last_error = ERROR_INVALID_DATA;
        std::cerr << "[!] invalid prepare-unload response" << std::endl;
        return false;
    }
    m_last_error = ERROR_SUCCESS;
    std::cout << "[+] Guest prepared for unload; unload Guest before Host. Reattach requires reloading both drivers."
              << std::endl;
    return true;
}

bool Debugger::Detach()
{
    if (!m_active) return true;

    // Restore everything we modified in the target before releasing it.
    for (auto& kv : m_breakpoints) {
        Breakpoint& bp = kv.second;
        if (bp.type == BreakpointType::Software && bp.enabled) {
            WriteMemory(bp.address, &bp.orig_byte, 1);
        }
    }
    if (m_step_over_bp) {
        WriteMemory(m_step_over_return_addr, &m_step_over_orig, 1);
    }

    LMV_GUEST_KI_SESSION session = {};
    session.Version = LMV_CPUID_PROTOCOL_VERSION;
    session.Size = sizeof(session);
    session.Operation = LMV_KI_SESSION_STOP;
    if (!DriverIoctl(IOCTL_LMV_GUEST_KI_SESSION, &session, sizeof(session),
        &session, sizeof(session))) {
        std::cerr << "[!] DR3 session stop failed, error " << m_last_error << std::endl;
        LMV_GUEST_KI_DRAIN drain = {};
        drain.Version = LMV_CPUID_PROTOCOL_VERSION;
        drain.Size = sizeof(drain);
        drain.Operation = LMV_KI_SESSION_QUERY;
        if (DriverIoctl(IOCTL_LMV_GUEST_KI_SESSION, &drain, sizeof(drain), &drain, sizeof(drain)))
            std::cerr << "[!] drain: in-flight=" << drain.InFlight << " armed CPUs="
                << drain.ArmedProcessors << " flags=" << drain.Flags << std::endl;
        return false;
    }

    m_active = false;
    InterlockedExchange(&m_eventPending, 0);
    InterlockedExchange(&m_resumeStatus, 0);
    InterlockedExchange(&m_guestAttached, 0);
    if (m_eventResume) SetEvent(m_eventResume);
    m_pid = 0;
    m_tid = 0;
    m_last_tid = 0;
    m_last_event_sequence = 0;
    m_breakpoints.clear();
    m_step_over_bp = false;
    m_step_over_return_addr = 0;
    m_step_over_orig = 0;
    m_pending_bp_id = 0;
    m_pending_bp_addr = 0;

    std::cout << "[+] detached" << std::endl;
    return true;
}

void Debugger::Kill()
{
    if (!m_active) return;

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, m_pid);
    if (h) {
        TerminateProcess(h, 1);
        CloseHandle(h);
        std::cout << "[+] target process " << m_pid << " terminated" << std::endl;
    } else {
        std::cerr << "[!] OpenProcess(PROCESS_TERMINATE) failed: "
                  << GetLastError() << std::endl;
    }
    Detach(); // release the hash-table node even if terminate failed
}

// ============================================================================
// Event control
// ============================================================================
bool Debugger::GetEvent(DBG_EVENT_INFO& evt)
{
    if (!m_active || m_hEventDevice == INVALID_HANDLE_VALUE) return false;
    InterlockedExchange(&m_backendPhase, 4);
    InterlockedExchange(&m_guestAttached, 1);
    SetEvent(m_backendReady);
    if (InterlockedCompareExchange(&m_eventPending, 0, 0)) {
        if (WaitForSingleObject(m_eventResume, 100) != WAIT_OBJECT_0 || !m_active) return false;
        LMV_GUEST_STUB_CONTINUE continuation = {};
        continuation.Version = LMV_CPUID_PROTOCOL_VERSION;
        continuation.Size = sizeof(continuation);
        continuation.Sequence = m_last_event_sequence;
        continuation.Disposition = LMV_GUEST_STUB_DISPOSITION_CONTINUE;
        DWORD returned = 0;
        if (!DeviceIoControl(m_hEventDevice, IOCTL_LMV_GUEST_KI_DR3_CONTINUE,
            &continuation, sizeof(continuation), &continuation, sizeof(continuation), &returned, NULL)) {
            m_last_error = GetLastError();
            InterlockedExchange(&m_resumeStatus, 0);
            return false;
        }
        InterlockedExchange(&m_eventPending, 0);
        InterlockedExchange(&m_resumeStatus, 0);
    }
    LMV_GUEST_STUB_WAIT_REQUEST request = {};
    request.Version = LMV_CPUID_PROTOCOL_VERSION;
    request.Size = sizeof(request);
    request.TimeoutMilliseconds = 200;
    LMV_GUEST_KI_DR3_EVENT event = {};
    DWORD returned = 0;
    if (!DeviceIoControl(m_hEventDevice, IOCTL_LMV_GUEST_KI_DR3_WAIT_EVENT,
        &request, sizeof(request), &event, sizeof(event), &returned, NULL)) {
        m_last_error = GetLastError();
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, m_pid);
        bool exited = process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        if (process) CloseHandle(process);
        if (exited && m_active) {
            evt = {};
            evt.EventType = DbgEventExitProcess;
            evt.ProcessId = m_pid;
            Detach();
            return true;
        }
        return false;
    }
    if (!m_active || returned != sizeof(event) || event.Version != LMV_CPUID_PROTOCOL_VERSION ||
        event.Size != sizeof(event) || event.ProcessId != m_pid) return false;
    evt = {};
    evt.EventType = DbgEventException;
    evt.ProcessId = event.ProcessId;
    evt.ThreadId = event.ThreadId;
    evt.ExceptionCode = event.ExceptionCode;
    evt.ExceptionAddress = event.ExceptionAddress;
    evt.FirstChance = 1;
    const auto& frame = event.Frame;
    auto& context = evt.Context;
    context.Rax = frame.Rax; context.Rbx = frame.Rbx; context.Rcx = frame.Rcx; context.Rdx = frame.Rdx;
    context.Rsi = frame.Rsi; context.Rdi = frame.Rdi; context.Rbp = frame.Rbp; context.Rsp = frame.Rsp;
    context.R8 = frame.R8; context.R9 = frame.R9; context.R10 = frame.R10; context.R11 = frame.R11;
    context.R12 = frame.R12; context.R13 = frame.R13; context.R14 = frame.R14; context.R15 = frame.R15;
    context.Rip = frame.Rip; context.RFlags = frame.Rflags;
    evt.ContextValid = TRUE;
    m_last_event_sequence = event.Sequence;
    m_eventProcessId = event.ProcessId;
    m_eventThreadId = event.ThreadId;
    InterlockedExchange(&m_resumeStatus, 0);
    InterlockedExchange(&m_eventPending, 1);
    return true;
}

bool Debugger::ProcessEvent(const DBG_EVENT_INFO& evt)
{
    m_last_tid = evt.ThreadId;
    m_tid = evt.ThreadId;

    if (evt.EventType != DbgEventException) {
        std::cout << "[event] type=" << evt.EventType
                  << " pid=" << evt.ProcessId << " tid=" << evt.ThreadId
                  << std::endl;
        return false;
    }

    // Synthetic break from the Break / Open path: the driver suspended the
    // thread and reported its *current* user RIP. There is no INT3 byte to
    // rewind and nothing to re-arm - just pause for inspection.
    if (evt.Reserved[0] & DBG_FLAG_SYNTHETIC_BREAK) {
        std::cout << "[event] debugger break pid=" << evt.ProcessId
                  << " tid=" << evt.ThreadId << " rip=0x" << std::hex
                  << evt.ExceptionAddress << std::dec << std::endl;
        return true;
    }

    switch (evt.ExceptionCode) {
    case 0x80000003: // STATUS_BREAKPOINT (INT3)
        return HandleSoftwareBreakpoint(evt);
    case 0x80000004: // STATUS_SINGLE_STEP (#DB)
        return HandleSingleStep(evt);
    default:
        // Other first-chance exceptions: freeze and show. Continuing re-runs
        // the faulting instruction, so the user edits RIP / steps to pass it.
        std::cout << "[event] exception code=0x" << std::hex << std::setfill('0')
                  << std::setw(8) << evt.ExceptionCode << std::dec
                  << " addr=0x" << std::hex << evt.ExceptionAddress << std::dec
                  << " pid=" << evt.ProcessId << " tid=" << evt.ThreadId
                  << " first=" << evt.FirstChance << std::endl;
        return true;
    }
}

bool Debugger::Continue()
{
    if (!m_active || !InterlockedCompareExchange(&m_eventPending, 0, 0)) {
        m_last_error = ERROR_INVALID_STATE;
        return false;
    }
    if (InterlockedCompareExchange(&m_resumeStatus, DBG_CONTINUE, 0) != 0) {
        m_last_error = ERROR_BUSY;
        return false;
    }
    SetEvent(m_eventResume);
    return true;
}

bool Debugger::WaitForEventBackend(DWORD timeoutMilliseconds)
{
    if (!m_backendReady || WaitForSingleObject(m_backendReady, timeoutMilliseconds) != WAIT_OBJECT_0) {
        m_last_error = WAIT_TIMEOUT;
        return false;
    }
    return InterlockedCompareExchange(&m_guestAttached, 0, 0) != 0;
}
ULONG Debugger::GetEventBackendPhase()
{
    return static_cast<ULONG>(InterlockedCompareExchange(&m_backendPhase, 0, 0));
}

void Debugger::StopEventBackend()
{
    if (m_active) Detach();
    InterlockedExchange(&m_guestAttached, 0);
    InterlockedExchange(&m_eventPending, 0);
    InterlockedExchange(&m_resumeStatus, 0);
    m_eventProcessId = 0;
    m_eventThreadId = 0;
}

// BreakTarget remains unsupported until the Guest protocol implements it.
bool Debugger::BreakTarget()
{
    m_last_error = ERROR_NOT_SUPPORTED;
    std::cerr << "[!] BreakTarget: current Guest protocol does not expose an async break request"
              << std::endl;
    return false;
}

// ============================================================================
// Exception handling (breakpoint / single-step)
// ============================================================================
bool Debugger::GetDriverContext(DWORD tid, DBG_CONTEXT& dc)
{
    if (!m_active || !InterlockedCompareExchange(&m_eventPending, 0, 0) ||
        tid != m_eventThreadId || InterlockedCompareExchange(&m_resumeStatus, 0, 0)) {
        m_last_error = ERROR_INVALID_STATE;
        return false;
    }
    LMV_GUEST_KI_DR3_CONTEXT request = {};
    request.Version = LMV_CPUID_PROTOCOL_VERSION;
    request.Size = sizeof(request);
    request.Sequence = m_last_event_sequence;
    DWORD returned = 0;
    if (!DeviceIoControl(m_hEventDevice, IOCTL_LMV_GUEST_KI_DR3_GET_CONTEXT,
        &request, sizeof(request), &request, sizeof(request), &returned, NULL)) {
        m_last_error = GetLastError();
        return false;
    }
    if (returned != sizeof(request)) { m_last_error = ERROR_INVALID_DATA; return false; }
    const auto& frame = request.Frame;
    dc = {};
    dc.Rax = frame.Rax; dc.Rbx = frame.Rbx; dc.Rcx = frame.Rcx; dc.Rdx = frame.Rdx;
    dc.Rsi = frame.Rsi; dc.Rdi = frame.Rdi; dc.Rbp = frame.Rbp; dc.Rsp = frame.Rsp;
    dc.R8 = frame.R8; dc.R9 = frame.R9; dc.R10 = frame.R10; dc.R11 = frame.R11;
    dc.R12 = frame.R12; dc.R13 = frame.R13; dc.R14 = frame.R14; dc.R15 = frame.R15;
    dc.Rip = frame.Rip; dc.RFlags = frame.Rflags;
    return true;
}

bool Debugger::SetDriverContext(DWORD tid, const DBG_CONTEXT& dc)
{
    if (!m_active || !InterlockedCompareExchange(&m_eventPending, 0, 0) ||
        tid != m_eventThreadId || InterlockedCompareExchange(&m_resumeStatus, 0, 0)) {
        m_last_error = ERROR_INVALID_STATE;
        return false;
    }
    LMV_GUEST_KI_DR3_CONTEXT request = {};
    request.Version = LMV_CPUID_PROTOCOL_VERSION;
    request.Size = sizeof(request);
    request.Sequence = m_last_event_sequence;
    request.Frame = { dc.Rip, dc.Rsp, dc.RFlags, dc.Rax, dc.Rcx, dc.Rdx,
        dc.R8, dc.R9, dc.R10, dc.R11, dc.Rbx, dc.Rbp, dc.Rsi, dc.Rdi,
        dc.R12, dc.R13, dc.R14, dc.R15 };
    DWORD returned = 0;
    if (!DeviceIoControl(m_hEventDevice, IOCTL_LMV_GUEST_KI_DR3_SET_CONTEXT,
        &request, sizeof(request), &request, sizeof(request), &returned, NULL)) {
        m_last_error = GetLastError();
        return false;
    }
    if (returned != sizeof(request)) { m_last_error = ERROR_INVALID_DATA; return false; }
    return true;
}

bool Debugger::SetRipAndTrap(DWORD tid, uint64_t rip)
{
    DBG_CONTEXT context = {};
    if (!GetDriverContext(tid, context)) return false;
    context.Rip = rip;
    context.RFlags |= 0x100;
    return SetDriverContext(tid, context);
}
// INT3 reached. On x64 the saved RIP already points PAST the INT3 byte, so
// the breakpoint address is ExceptionAddress - 1.
bool Debugger::HandleSoftwareBreakpoint(const DBG_EVENT_INFO& evt)
{
    uint64_t bp_addr = evt.ContextValid ? evt.Context.Rip - 1 : evt.ExceptionAddress;

    // StepOver temporary breakpoint (planted right after a call)?
    if (m_step_over_bp && bp_addr == m_step_over_return_addr) {
        WriteMemory(bp_addr, &m_step_over_orig, 1);
        std::cout << "[+] step-over temp breakpoint at 0x" << std::hex
                  << bp_addr << std::dec << " removed" << std::endl;
        m_step_over_bp = false;
        m_step_over_return_addr = 0;
        m_step_over_orig = 0;
        return true; // pause on the instruction after the call
    }

    // One of our software breakpoints?
    for (auto& kv : m_breakpoints) {
        Breakpoint& bp = kv.second;
        if (bp.type == BreakpointType::Software && bp.enabled &&
            bp.address == bp_addr) {
            if (!WriteMemory(bp_addr, &bp.orig_byte, 1)) {
                std::cerr << "[!] cannot restore original byte at 0x" << std::hex
                          << bp_addr << std::dec << std::endl;
                return true; // pause; let the user sort it out
            }
            if (!SetRipAndTrap(evt.ThreadId, bp_addr)) {
                std::cerr << "[!] cannot rewind RIP for breakpoint" << std::endl;
                return true;
            }
            // Do not pause yet: the target runs the restored instruction and
            // the follow-up single-step re-arms the INT3 and pauses.
            m_pending_bp_id = bp.id;
            m_pending_bp_addr = bp_addr;
            return false;
        }
    }

    // INT3 we did not plant (user code): rewind so the instruction is not
    // skipped, and pause for inspection. But if the byte at bp_addr is a real
    // INT3 instruction (e.g. the initial process breakpoint in ntdll), the
    // trap-frame RIP already points past it; rewinding would re-execute the
    // int3 forever, so pause without touching RIP / TF.
    uint8_t byte = 0;
    bool isRealInt3 = ReadMemory(bp_addr, &byte, 1) && byte == 0xCC;
    if (!isRealInt3) {
        SetRipAndTrap(evt.ThreadId, bp_addr);
    }
    std::cout << "[event] INT3 at 0x" << std::hex << bp_addr << std::dec
              << " not in breakpoint table, pausing"
              << (isRealInt3 ? " (real int3, RIP already past it)" : "")
              << std::endl;
    return true;
}

// Single-step (#DB) reached. Clear TF in the saved context first so the
// target does not re-trap on the next instruction.
bool Debugger::HandleSingleStep(const DBG_EVENT_INFO& evt)
{
    DBG_CONTEXT dc;
    if (!GetDriverContext(evt.ThreadId, dc)) {
        std::cerr << "[!] cannot read context for single-step" << std::endl;
        return true;
    }
    dc.RFlags &= ~0x100ULL;
    if (!SetDriverContext(evt.ThreadId, dc)) {
        std::cerr << "[!] cannot clear TF flag" << std::endl;
        return true;
    }

    if (m_pending_bp_id != 0) {
        // Re-arm the INT3 consumed by a breakpoint hit.
        uint8_t cc = 0xCC;
        if (WriteMemory(m_pending_bp_addr, &cc, 1)) {
            std::cout << "[breakpoint] id=" << m_pending_bp_id << " hit at 0x"
                      << std::hex << m_pending_bp_addr << std::dec << std::endl;
        } else {
            std::cerr << "[!] cannot re-arm breakpoint at 0x" << std::hex
                      << m_pending_bp_addr << std::dec << std::endl;
        }
        m_pending_bp_id = 0;
        m_pending_bp_addr = 0;
        return true; // pause
    }

    // Ordinary single-step (StepInto / StepOver)
    std::cout << "[event] single-step pid=" << evt.ProcessId
              << " tid=" << evt.ThreadId << std::endl;
    return true; // pause
}
// ============================================================================
// Stepping
// ============================================================================
bool Debugger::StepInto(DWORD tid)
{
    DBG_CONTEXT dc;
    if (!GetDriverContext(tid, dc)) {
        std::cerr << "[!] StepInto: cannot read context" << std::endl;
        return false;
    }
    dc.RFlags |= 0x100; // TF
    if (!SetDriverContext(tid, dc)) return false;
    return Continue();
}

bool Debugger::StepOver(DWORD tid)
{
    DBG_CONTEXT dc;
    if (!GetDriverContext(tid, dc)) {
        std::cerr << "[!] StepOver: cannot read context" << std::endl;
        return false;
    }

    uint8_t code[16] = {};
    if (!ReadMemory(dc.Rip, code, sizeof(code))) {
        std::cerr << "[!] StepOver: cannot read instruction bytes" << std::endl;
        return false;
    }
    DisasmInstruction inst = DisasmOne(dc.Rip, code, sizeof(code));
    if (inst.length <= 0) {
        std::cerr << "[!] StepOver: cannot decode instruction at 0x" << std::hex
                  << dc.Rip << std::dec << std::endl;
        return false;
    }

    uint64_t next = dc.Rip + inst.length;
    if (inst.is_call) {
        // A call steps INTO the callee when TF is set, so instead plant a
        // temporary INT3 at the return address and run at full speed.
        uint8_t orig = 0;
        if (!ReadMemory(next, &orig, 1)) return false;
        uint8_t cc = 0xCC;
        if (!WriteMemory(next, &cc, 1)) return false;
        m_step_over_bp = true;
        m_step_over_return_addr = next;
        m_step_over_orig = orig;
        dc.RFlags &= ~0x100ULL; // no trap on the call itself
    } else {
        // jmp / jcc / ret / plain instruction: TF pauses right after the
        // instruction, which is the correct step-over destination.
        dc.RFlags |= 0x100;
    }

    if (!SetDriverContext(tid, dc)) {
        // roll back the temporary breakpoint we just planted
        if (m_step_over_bp) {
            WriteMemory(m_step_over_return_addr, &m_step_over_orig, 1);
            m_step_over_bp = false;
            m_step_over_return_addr = 0;
            m_step_over_orig = 0;
        }
        return false;
    }
    return Continue();
}

// ============================================================================
// Breakpoints
// ============================================================================
uint32_t Debugger::SetSoftwareBreakpoint(uint64_t addr)
{
    if (!m_active || addr == 0) return 0;

    // already set at this address?
    for (auto& kv : m_breakpoints) {
        if (kv.second.type == BreakpointType::Software &&
            kv.second.address == addr) {
            return kv.second.id;
        }
    }

    uint8_t orig = 0;
    if (!ReadMemory(addr, &orig, 1)) {
        std::cerr << "[!] SetSoftwareBreakpoint: cannot read 0x" << std::hex
                  << addr << std::dec << std::endl;
        return 0;
    }
    uint8_t cc = 0xCC;
    if (!WriteMemory(addr, &cc, 1)) {
        std::cerr << "[!] SetSoftwareBreakpoint: cannot write 0xCC at 0x"
                  << std::hex << addr << std::dec << std::endl;
        return 0;
    }

    Breakpoint bp;
    bp.id = m_next_bp_id++;
    bp.type = BreakpointType::Software;
    bp.address = addr;
    bp.size = 1;
    bp.enabled = true;
    bp.orig_byte = orig;
    m_breakpoints[bp.id] = bp;

    std::cout << "[bp] id=" << bp.id << " software at 0x" << std::hex << addr
              << std::dec << " (orig 0x" << std::hex << (int)orig << std::dec
              << ")" << std::endl;
    return bp.id;
}

uint32_t Debugger::SetHardwareBreakpoint(uint64_t addr, HardwareBreakCondition cond)
{
    std::cerr << "[!] hardware breakpoints not supported by driver yet"
              << std::endl;
    return 0;
}

uint32_t Debugger::SetMemoryBreakpoint(uint64_t addr, uint64_t size,
                                       MemoryBreakCondition cond)
{
    std::cerr << "[!] memory breakpoints not supported by driver yet"
              << std::endl;
    return 0;
}

bool Debugger::RemoveBreakpoint(uint32_t id)
{
    auto it = m_breakpoints.find(id);
    if (it == m_breakpoints.end()) return false;
    Breakpoint& bp = it->second;
    if (bp.type == BreakpointType::Software && bp.enabled) {
        WriteMemory(bp.address, &bp.orig_byte, 1);
    }
    m_breakpoints.erase(it);
    std::cout << "[bp] id=" << id << " removed" << std::endl;
    return true;
}

bool Debugger::EnableBreakpoint(uint32_t id)
{
    auto it = m_breakpoints.find(id);
    if (it == m_breakpoints.end()) return false;
    Breakpoint& bp = it->second;
    if (bp.enabled) return true;
    if (bp.type == BreakpointType::Software) {
        uint8_t cc = 0xCC;
        if (!WriteMemory(bp.address, &cc, 1)) return false;
        bp.enabled = true;
        std::cout << "[bp] id=" << id << " enabled" << std::endl;
    }
    return true;
}

bool Debugger::DisableBreakpoint(uint32_t id)
{
    auto it = m_breakpoints.find(id);
    if (it == m_breakpoints.end()) return false;
    Breakpoint& bp = it->second;
    if (!bp.enabled) return true;
    if (bp.type == BreakpointType::Software) {
        WriteMemory(bp.address, &bp.orig_byte, 1);
        bp.enabled = false;
        std::cout << "[bp] id=" << id << " disabled" << std::endl;
    }
    return true;
}

// ============================================================================
// Registers
// ============================================================================
RegisterContext Debugger::GetRegisters(DWORD tid)
{
    RegisterContext context = {};
    TryGetRegisters(tid, context);
    return context;
}

bool Debugger::TryGetRegisters(DWORD tid, RegisterContext& result)
{
    result = {};
    DBG_CONTEXT context = {};
    if (!GetDriverContext(tid, context)) return false;

    result.rax = context.Rax; result.rbx = context.Rbx;
    result.rcx = context.Rcx; result.rdx = context.Rdx;
    result.rsi = context.Rsi; result.rdi = context.Rdi;
    result.rbp = context.Rbp; result.rsp = context.Rsp;
    result.r8 = context.R8; result.r9 = context.R9;
    result.r10 = context.R10; result.r11 = context.R11;
    result.r12 = context.R12; result.r13 = context.R13;
    result.r14 = context.R14; result.r15 = context.R15;
    result.rip = context.Rip;
    result.rflags = context.RFlags;
    result.cs = static_cast<uint16_t>(context.SegCs);
    result.ds = static_cast<uint16_t>(context.SegDs);
    result.es = static_cast<uint16_t>(context.SegEs);
    result.fs = static_cast<uint16_t>(context.SegFs);
    result.gs = static_cast<uint16_t>(context.SegGs);
    result.ss = static_cast<uint16_t>(context.SegSs);
    return true;
}
bool Debugger::SetRegisters(DWORD tid, const RegisterContext& ctx)
{
    DBG_CONTEXT dc = {};
    dc.Rax = ctx.rax; dc.Rbx = ctx.rbx; dc.Rcx = ctx.rcx; dc.Rdx = ctx.rdx;
    dc.Rsi = ctx.rsi; dc.Rdi = ctx.rdi; dc.Rbp = ctx.rbp; dc.Rsp = ctx.rsp;
    dc.R8  = ctx.r8;  dc.R9  = ctx.r9;  dc.R10 = ctx.r10; dc.R11 = ctx.r11;
    dc.R12 = ctx.r12; dc.R13 = ctx.r13; dc.R14 = ctx.r14; dc.R15 = ctx.r15;
    dc.Rip = ctx.rip;
    dc.RFlags = ctx.rflags;
    dc.SegCs = ctx.cs; dc.SegDs = ctx.ds; dc.SegEs = ctx.es;
    dc.SegFs = ctx.fs; dc.SegGs = ctx.gs; dc.SegSs = ctx.ss;
    return SetDriverContext(tid, dc);
}

// ============================================================================
// Memory
// ============================================================================
bool Debugger::ReadMemory(uint64_t addr, void* buf, size_t size)
{
    if (m_hDevice == INVALID_HANDLE_VALUE || buf == nullptr || size == 0 ||
        size > LMV_MEMORY_IO_MAX_SIZE || !m_active) {
        m_last_error = ERROR_INVALID_PARAMETER;
        return false;
    }

    const ULONG requestHeaderSize = FIELD_OFFSET(LMV_GUEST_MEMORY_REQUEST, Data);
    const ULONG responseHeaderSize = FIELD_OFFSET(LMV_GUEST_MEMORY_RESPONSE, Data);
    std::vector<uint8_t> input(requestHeaderSize, 0);
    std::vector<uint8_t> output(responseHeaderSize + size, 0);
    auto* request = reinterpret_cast<LMV_GUEST_MEMORY_REQUEST*>(input.data());
    auto* response = reinterpret_cast<LMV_GUEST_MEMORY_RESPONSE*>(output.data());
    request->Version = LMV_CPUID_PROTOCOL_VERSION;
    request->Size = requestHeaderSize;
    request->ProcessId = m_pid;
    request->Address = addr;
    request->Length = static_cast<ULONG>(size);

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(m_hDevice, IOCTL_LMV_GUEST_READ_PROCESS_MEMORY,
        input.data(), static_cast<DWORD>(input.size()),
        output.data(), static_cast<DWORD>(output.size()), &returned, NULL);
    if (!ok) {
        m_last_error = GetLastError();
        return false;
    }
    if (returned < responseHeaderSize || response->Status != LMV_CPUID_RESULT_SUCCESS ||
        response->BytesTransferred < size) {
        m_last_error = response->Status != LMV_CPUID_RESULT_SUCCESS
            ? response->Status : ERROR_PARTIAL_COPY;
        return false;
    }
    memcpy(buf, response->Data, size);
    return true;
}

bool Debugger::WriteMemory(uint64_t addr, const void* buf, size_t size)
{
    if (m_hDevice == INVALID_HANDLE_VALUE || buf == nullptr || size == 0 ||
        size > LMV_MEMORY_IO_MAX_SIZE || !m_active) {
        m_last_error = ERROR_INVALID_PARAMETER;
        return false;
    }

    const ULONG requestHeaderSize = FIELD_OFFSET(LMV_GUEST_MEMORY_REQUEST, Data);
    const ULONG responseHeaderSize = FIELD_OFFSET(LMV_GUEST_MEMORY_RESPONSE, Data);
    std::vector<uint8_t> input(requestHeaderSize + size, 0);
    std::vector<uint8_t> output(responseHeaderSize + size, 0);
    auto* request = reinterpret_cast<LMV_GUEST_MEMORY_REQUEST*>(input.data());
    auto* response = reinterpret_cast<LMV_GUEST_MEMORY_RESPONSE*>(output.data());
    request->Version = LMV_CPUID_PROTOCOL_VERSION;
    request->Size = requestHeaderSize;
    request->ProcessId = m_pid;
    request->Address = addr;
    request->Length = static_cast<ULONG>(size);
    memcpy(request->Data, buf, size);

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(m_hDevice, IOCTL_LMV_GUEST_WRITE_PROCESS_MEMORY,
        input.data(), static_cast<DWORD>(input.size()),
        output.data(), static_cast<DWORD>(output.size()), &returned, NULL);
    if (!ok) {
        m_last_error = GetLastError();
        return false;
    }
    if (returned < responseHeaderSize || response->Status != LMV_CPUID_RESULT_SUCCESS ||
        response->BytesTransferred < size) {
        m_last_error = response->Status != LMV_CPUID_RESULT_SUCCESS
            ? response->Status : ERROR_PARTIAL_COPY;
        return false;
    }
    return true;
}

// ============================================================================
// Disassembly

// ============================================================================
std::vector<DisasmInstruction> Debugger::Disassemble(uint64_t addr, int count)
{
    std::vector<DisasmInstruction> result;
    if (!m_active) return result;
    if (count < 1) count = 1;
    if (count > 128) count = 128;

    uint64_t cur = addr;
    for (int i = 0; i < count; i++) {
        uint8_t code[16] = {};
        if (!ReadMemory(cur, code, sizeof(code))) break;
        DisasmInstruction inst = DisasmOne(cur, code, sizeof(code));
        if (inst.length <= 0) break;
        result.push_back(inst);
        cur += inst.length;
    }
    return result;
}

// ============================================================================
// Process info (still read-only Win32 handles; only querying, no debugging)
// ============================================================================
uint64_t Debugger::GetImageBase()
{
    if (!m_active) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, m_pid);
    if (!h) return 0;
    HMODULE mods[1]; DWORD needed = 0;
    uint64_t base = 0;
    if (EnumProcessModules(h, mods, sizeof(mods), &needed)) {
        MODULEINFO mi = {};
        if (GetModuleInformation(h, mods[0], &mi, sizeof(mi))) {
            base = reinterpret_cast<uint64_t>(mi.lpBaseOfDll);
        }
    }
    CloseHandle(h);
    return base;
}

std::vector<ModuleInfo> Debugger::GetModules()
{
    std::vector<ModuleInfo> result;
    if (!m_active) return result;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, m_pid);
    if (!h) return result;
    HMODULE mods[256]; DWORD needed = 0;
    if (EnumProcessModules(h, mods, sizeof(mods), &needed)) {
        int count = needed / sizeof(HMODULE);
        for (int i = 0; i < count && i < 256; i++) {
            ModuleInfo mi = {};
            wchar_t name[MAX_PATH];
            if (GetModuleBaseNameW(h, mods[i], name, MAX_PATH)) {
                mi.name = WideToAnsi(name);
            }
            MODULEINFO info = {};
            if (GetModuleInformation(h, mods[i], &info, sizeof(info))) {
                mi.base = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
                mi.size = info.SizeOfImage;
                mi.entry = reinterpret_cast<uint64_t>(info.EntryPoint);
            }
            result.push_back(mi);
        }
    }
    CloseHandle(h);
    return result;
}

uint64_t Debugger::ResolveSymbol(const std::string& name)
{
    auto mods = GetModules();
    for (auto& m : mods) {
        if (m.name.empty()) continue;
        size_t pos = name.find('!');
        if (pos == std::string::npos) continue;
        std::string mod_name = name.substr(0, pos);
        std::string sym_name = name.substr(pos + 1);
        if (mod_name == m.name) {
            HMODULE hLocal = LoadLibraryA(m.name.c_str());
            if (hLocal) {
                FARPROC p = GetProcAddress(hLocal, sym_name.c_str());
                if (p) {
                    uint64_t rva = reinterpret_cast<uint64_t>(p) -
                                   reinterpret_cast<uint64_t>(hLocal);
                    FreeLibrary(hLocal);
                    return m.base + rva;
                }
                FreeLibrary(hLocal);
            }
        }
    }
    return 0;
}





