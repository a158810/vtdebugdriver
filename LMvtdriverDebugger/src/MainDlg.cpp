#include "stdafx.h"
#include "MainDlg.h"
#include "resource.h"
#include "debugger.h"
#include "disasm.h"
#include "utils.h"
#include <afxdialogex.h>
#include <afxdlgs.h>
#include <thread>
#include <functional>
#include <memory>
#include <streambuf>
#include <iostream>
#include <cerrno>

// ============================================================================
// Forward std::cout / std::cerr (used by Debugger diagnostics) into the MFC
// output edit control. The sink is thread-aware: UI-thread output is written
// directly, worker-thread output is posted to the UI thread as WM_APP_LOG.
// ============================================================================
class UILogStreamBuf : public std::streambuf {
public:
    std::function<void(const std::string&)> sink;
protected:
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            m_buf.push_back((char)ch);
            if (ch == (int_type)'\n') flush_line();
        }
        return ch;
    }
    int sync() override { flush_line(); return 0; }
private:
    std::string m_buf;
    void flush_line() {
        if (!m_buf.empty() && sink) {
            while (!m_buf.empty() && (m_buf.back() == '\n' || m_buf.back() == '\r')) m_buf.pop_back();
            if (!m_buf.empty()) sink(m_buf);
            m_buf.clear();
        }
    }
};

Debugger g_debugger;
static CMainDlg* g_pDlg = nullptr;
static UILogStreamBuf g_cout_buf;
static UILogStreamBuf g_cerr_buf;
static std::streambuf* g_old_cout = nullptr;
static std::streambuf* g_old_cerr = nullptr;
static DWORD g_uiThreadId = 0;
static HWND g_logHwnd = nullptr;

static void RouteLog(const std::string& s) {
    if (s.empty()) return;
    if (GetCurrentThreadId() == g_uiThreadId) {
        if (g_pDlg) g_pDlg->Log(CString(s.c_str()));
    } else if (g_logHwnd) {
        // the posted pointer is freed by OnAppLog on the UI thread
        ::PostMessage(g_logHwnd, CMainDlg::WM_APP_LOG,
                      reinterpret_cast<WPARAM>(new std::string(s)), 0);
    }
}

// ============================================================================
// CMainDlg
// ============================================================================
IMPLEMENT_DYNAMIC(CMainDlg, CDialogEx)

CMainDlg::CMainDlg(CWnd* pParent) : CDialogEx(IDD_MAIN_DIALOG, pParent) {
    m_running = false;
    m_debugging = false;
    m_worker = nullptr;
    m_stopWorker = false;
    m_openProcess = NULL;
    m_openThread = NULL;
    InitializeCriticalSection(&m_eventLock);
}

void CMainDlg::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_REGS_EDIT, m_regsEdit);
    DDX_Control(pDX, IDC_DISASM_LIST, m_disasmlist);
    DDX_Control(pDX, IDC_MEMORY_EDIT, m_memoryEdit);
    DDX_Control(pDX, IDC_BREAKPOINT_LIST, m_breakpointList);
    DDX_Control(pDX, IDC_OUTPUT_EDIT, m_outputEdit);
    DDX_Control(pDX, IDC_DISASM_ADDR, m_disasmAddr);
    DDX_Control(pDX, IDC_DISASM_COUNT, m_disasmCount);
    DDX_Control(pDX, IDC_MEMORY_ADDR, m_memoryAddr);
    DDX_Control(pDX, IDC_MEMORY_SIZE, m_memorySize);
    DDX_Control(pDX, IDC_BP_ADDR, m_bpAddr);
    DDX_Control(pDX, IDC_BP_TYPE, m_bpType);
    DDX_Control(pDX, IDC_BP_COND, m_bpCond);
    DDX_Control(pDX, IDC_BP_SIZE, m_bpSize);
    DDX_Control(pDX, IDC_PID_EDIT, m_pidEdit);
    DDX_Control(pDX, IDC_PATH_EDIT, m_pathEdit);
    DDX_Control(pDX, IDC_STATUS_TEXT, m_statusText);
    DDX_Control(pDX, IDC_DRV_STATUS, m_driverStatus);
}

BEGIN_MESSAGE_MAP(CMainDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BTN_OPEN, &CMainDlg::OnBtnOpen)
    ON_BN_CLICKED(IDC_BTN_BROWSE, &CMainDlg::OnBtnBrowse)
    ON_BN_CLICKED(IDC_BTN_ATTACH, &CMainDlg::OnBtnAttach)
    ON_BN_CLICKED(IDC_BTN_DETACH, &CMainDlg::OnBtnDetach)
    ON_BN_CLICKED(IDC_BTN_RUN, &CMainDlg::OnBtnRun)
    ON_BN_CLICKED(IDC_BTN_GO, &CMainDlg::OnBtnGo)
    ON_BN_CLICKED(IDC_BTN_STEP_INTO, &CMainDlg::OnBtnStepInto)
    ON_BN_CLICKED(IDC_BTN_STEP_OVER, &CMainDlg::OnBtnStepOver)
    ON_BN_CLICKED(IDC_BTN_STOP, &CMainDlg::OnBtnStop)
    ON_BN_CLICKED(IDC_BTN_BREAK, &CMainDlg::OnBtnBreak)
    ON_BN_CLICKED(IDC_BTN_PING, &CMainDlg::OnBtnPing)
    ON_BN_CLICKED(IDC_BTN_BP_SET, &CMainDlg::OnBtnBpSet)
    ON_BN_CLICKED(IDC_BTN_BP_DEL, &CMainDlg::OnBtnBpDel)
    ON_BN_CLICKED(IDC_BTN_BP_ENABLE, &CMainDlg::OnBtnBpEnable)
    ON_BN_CLICKED(IDC_BTN_BP_DISABLE, &CMainDlg::OnBtnBpDisable)
    ON_BN_CLICKED(IDC_BTN_MEM_READ, &CMainDlg::OnBtnMemRead)
    ON_BN_CLICKED(IDC_REGS_REFRESH, &CMainDlg::OnRefreshRegs)
    ON_BN_CLICKED(IDC_BTN_REGS_APPLY, &CMainDlg::OnRegsApply)
    ON_BN_CLICKED(IDC_DISASM_REFRESH, &CMainDlg::OnRefreshDisasm)
    ON_MESSAGE(CMainDlg::WM_DEBUG_EVENT, &CMainDlg::OnDebugEvent)
    ON_MESSAGE(CMainDlg::WM_APP_LOG, &CMainDlg::OnAppLog)
    ON_WM_DESTROY()
    ON_WM_CLOSE()
    ON_WM_TIMER()
END_MESSAGE_MAP()

// ============================================================================
// Helpers
// ============================================================================
bool CMainDlg::ParseUnsigned(const CString& text, uint64_t& value, int base, uint64_t maxValue) const {
    CString input(text);
    input.Trim();
    if (input.IsEmpty() || input[0] == _T('-') || input[0] == _T('+')) return false;

    errno = 0;
    TCHAR* end = nullptr;
    unsigned __int64 parsed = _tcstoui64(input, &end, base);
    if (errno == ERANGE || end == static_cast<LPCTSTR>(input) || *end != _T('\0') || parsed > maxValue) {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool CMainDlg::ParseAddr(const CString& text, uint64_t& value) const {
    return ParseUnsigned(text, value, 16, UINT64_MAX);
}

void CMainDlg::Log(const CString& msg) {
    CString text;
    m_outputEdit.GetWindowText(text);
    text += msg + _T("\r\n");
    m_outputEdit.SetWindowText(text);
    int len = m_outputEdit.GetLineCount();
    m_outputEdit.LineScroll(len);
}
// ============================================================================
// Refresh helpers (UI thread only)
// ============================================================================
void CMainDlg::RefreshRegisters() {
    if (!m_debugging) {
        m_regsEdit.SetWindowText(_T("No process attached."));
        m_regsEdit.SetReadOnly(TRUE);
        return;
    }
    DWORD tid = g_debugger.GetCurrentTid();
    if (tid == 0) {
        m_regsEdit.SetWindowText(_T("Attached. Waiting for the first debug event..."));
        m_regsEdit.SetReadOnly(TRUE);
        return;
    }
    RegisterContext rc = {};
    if (!g_debugger.TryGetRegisters(tid, rc)) {
        m_regsEdit.SetWindowText(_T("Register context unavailable until an exception is paused."));
        m_regsEdit.SetReadOnly(TRUE);
        return;
    }
    CString txt;
    txt.Format(_T("RAX=0x%016llX\r\nRBX=0x%016llX\r\nRCX=0x%016llX\r\nRDX=0x%016llX\r\n")
               _T("RSI=0x%016llX\r\nRDI=0x%016llX\r\nRBP=0x%016llX\r\nRSP=0x%016llX\r\n")
               _T("R8=0x%016llX\r\nR9=0x%016llX\r\nR10=0x%016llX\r\nR11=0x%016llX\r\n")
               _T("R12=0x%016llX\r\nR13=0x%016llX\r\nR14=0x%016llX\r\nR15=0x%016llX\r\n")
               _T("RIP=0x%016llX\r\nRFLAGS=0x%016llX\r\n")
               _T("CS=0x%04X\r\nDS=0x%04X\r\nES=0x%04X\r\nFS=0x%04X\r\nGS=0x%04X\r\nSS=0x%04X\r\n")
               _T("DR0=0x%016llX\r\nDR1=0x%016llX\r\nDR2=0x%016llX\r\nDR3=0x%016llX\r\n")
               _T("DR6=0x%016llX\r\nDR7=0x%016llX"),
               rc.rax, rc.rbx, rc.rcx, rc.rdx,
               rc.rsi, rc.rdi, rc.rbp, rc.rsp,
               rc.r8, rc.r9, rc.r10, rc.r11,
               rc.r12, rc.r13, rc.r14, rc.r15,
               rc.rip, rc.rflags,
               rc.cs, rc.ds, rc.es, rc.fs, rc.gs, rc.ss,
               rc.dr0, rc.dr1, rc.dr2, rc.dr3,
               rc.dr6, rc.dr7);
    m_regsEdit.SetWindowText(txt);
    m_regsEdit.SetReadOnly(m_running ? TRUE : FALSE);
}

void CMainDlg::RefreshDisassembly() {
    m_disasmlist.ResetContent();
    if (!m_debugging) return;

    uint64_t addr = 0;
    int count = 16;
    CString sAddr, sCount;
    m_disasmAddr.GetWindowText(sAddr);
    m_disasmCount.GetWindowText(sCount);
    RegisterContext currentContext = {};
    DWORD currentTid = g_debugger.GetCurrentTid();
    bool haveContext = !m_running && currentTid != 0 &&
        g_debugger.TryGetRegisters(currentTid, currentContext);
    if (!sAddr.IsEmpty()) {
        if (!ParseAddr(sAddr, addr)) {
            Log(_T("Invalid disassembly address. Use hexadecimal, for example 7FF700001000."));
            return;
        }
    } else {
        DWORD tid = g_debugger.GetCurrentTid();
        if (tid == 0) {
            m_disasmlist.AddString(_T("Attached. Waiting for a debug event, or enter an address."));
            return;
        }
        if (!haveContext) {
            m_disasmlist.AddString(_T("Register context unavailable. Enter an address or pause on an exception."));
            return;
        }
        addr = currentContext.rip;
    }
    if (!sCount.IsEmpty()) {
        uint64_t parsedCount = 0;
        if (!ParseUnsigned(sCount, parsedCount, 10, 128) || parsedCount == 0) {
            Log(_T("Invalid disassembly count. Enter a decimal value from 1 to 128."));
            return;
        }
        count = static_cast<int>(parsedCount);
    }

    auto insts = g_debugger.Disassemble(addr, count);
    if (insts.empty()) {
        CString message;
        message.Format(_T("Cannot disassemble %016llX (error %lu). Check that the address is readable in the target."),
            addr, g_debugger.GetLastErrorInfo());
        m_disasmlist.AddString(message);
        return;
    }
    for (auto& inst : insts) {
        CString line;
        CString bytes(inst.bytes.c_str());
        CString mnem(inst.mnemonic.c_str());
        CString ops(inst.operands.c_str());
        bool isCurrent = haveContext && inst.address == currentContext.rip;
        line.Format(_T("%s %016llX  %-30s  %s %s"),
                    isCurrent ? _T("->") : _T("  "), inst.address,
                    (LPCTSTR)bytes, (LPCTSTR)mnem, (LPCTSTR)ops);
        int row = m_disasmlist.AddString(line);
        if (isCurrent && row != LB_ERR && row != LB_ERRSPACE)
            m_disasmlist.SetCurSel(row);
    }
    if (insts.size() < static_cast<size_t>(count))
        m_disasmlist.AddString(_T("Disassembly stopped: subsequent bytes could not be read."));
}

void CMainDlg::RefreshBreakpoints() {
    m_breakpointList.ResetContent();
    for (auto& kv : g_debugger.Breakpoints()) {
        const Breakpoint& bp = kv.second;
        const TCHAR* type = _T("SW");
        if (bp.type == BreakpointType::Hardware) type = _T("HW");
        else if (bp.type == BreakpointType::Memory) type = _T("MEM");
        CString line;
        line.Format(_T("%u  %-3s  %-3s  %016llX"),
                    bp.id, type, bp.enabled ? _T("on") : _T("off"), bp.address);
        m_breakpointList.AddString(line);
    }
}

void CMainDlg::RefreshAll() {
    RefreshRegisters();
    RefreshDisassembly();
    RefreshBreakpoints();
}

void CMainDlg::RefreshDriverStatus() {
    ULONG version = 0, flags = 0;
    if (g_debugger.PingDriver(&version, &flags)) {
        CString txt;
        txt.Format(_T("Driver: Online v%u (%s)"), version,
                   (flags & DBGSYS_FLAG_HOOK_ACTIVE) ? _T("hook on") : _T("hook off"));
        m_driverStatus.SetWindowText(txt);
    } else {
        CString txt;
        txt.Format(_T("Driver: Offline (err %u)"), g_debugger.GetLastErrorInfo());
        m_driverStatus.SetWindowText(txt);
    }
}

// ============================================================================
// Event worker thread
// ============================================================================
UINT AFX_CDECL CMainDlg::DebugEventWorker(LPVOID pParam) {
    CMainDlg* dlg = reinterpret_cast<CMainDlg*>(pParam);
    for (;;) {
        if (dlg->WorkerShouldStop()) break;
        if (!g_debugger.IsActive()) {
            Sleep(200); // not attached yet: poll until the user attaches
            continue;
        }
        DBG_EVENT_INFO evt = {};
        if (g_debugger.GetEvent(evt)) {
            dlg->QueueEvent(evt);
        } else {
            // detach wakes us with STATUS_NO_MORE_ENTRIES; anything else is
            // a transient failure, retry unless we are asked to stop.
            if (dlg->WorkerShouldStop()) break;
            Sleep(200);
        }
    }
    g_debugger.StopEventBackend();
    return 0;
}

bool CMainDlg::StartWorkerThread() {
    if (m_worker) {
        if (WaitForSingleObject(m_worker->m_hThread, 0) == WAIT_TIMEOUT) return !m_stopWorker.load();
        delete m_worker;
        m_worker = nullptr;
    }
    m_stopWorker = false;
    m_worker = AfxBeginThread(CMainDlg::DebugEventWorker, this,
                              THREAD_PRIORITY_NORMAL, 0, CREATE_SUSPENDED);
    if (!m_worker) return false;
    m_worker->m_bAutoDelete = FALSE;
    m_worker->ResumeThread();
    return true;
}
void CMainDlg::StopWorkerThread() {
    m_stopWorker = true;
    g_debugger.Detach(); // wakes the worker blocked inside GetEvent
    if (m_worker) {
        HANDLE hThread = m_worker->m_hThread;
        if (hThread) WaitForSingleObject(hThread, 5000);
    }
    if (m_worker && WaitForSingleObject(m_worker->m_hThread, 0) == WAIT_OBJECT_0) {
        delete m_worker;
        m_worker = nullptr;
        m_stopWorker = false;
    }
}

void CMainDlg::QueueEvent(const DBG_EVENT_INFO& evt) {
    EnterCriticalSection(&m_eventLock);
    m_events.push_back(evt);
    LeaveCriticalSection(&m_eventLock);
    if (m_hWnd) ::PostMessage(m_hWnd, WM_DEBUG_EVENT, 0, 0);
}

LRESULT CMainDlg::OnDebugEvent(WPARAM, LPARAM) {
    bool have = false;
    DBG_EVENT_INFO evt = {};
    EnterCriticalSection(&m_eventLock);
    if (!m_events.empty()) {
        evt = m_events.front();
        m_events.pop_front();
        have = true;
    }
    LeaveCriticalSection(&m_eventLock);
    if (!have) return 0;
    if (!m_debugging) return 0; // detached in the meantime: drop the event

    if (evt.EventType == DbgEventExitProcess) {
        StopWorkerThread();
        m_debugging = false;
        m_running = false;
        m_statusText.SetWindowText(_T("Status: Target exited"));
        RefreshAll();
        return 0;
    }

    bool pause = g_debugger.ProcessEvent(evt);
    if (!pause) {
        // internal auto-handling (e.g. software breakpoint hit): keep running
        g_debugger.Continue();
    } else {
        m_running = false;
        m_statusText.SetWindowText(_T("Status: Breakpoint"));
        RegisterContext context = {};
        if (g_debugger.TryGetRegisters(g_debugger.GetCurrentTid(), context)) {
            CString address;
            address.Format(_T("%016llX"), context.rip);
            m_disasmAddr.SetWindowText(address);
        }
        RefreshAll();
    }
    return 0;
}

LRESULT CMainDlg::OnAppLog(WPARAM wParam, LPARAM) {
    std::unique_ptr<std::string> msg(reinterpret_cast<std::string*>(wParam));
    if (msg) Log(CString(msg->c_str()));
    return 0;
}
// ============================================================================
// Button handlers
// ============================================================================
void CMainDlg::OnBtnOpen() {
    if (m_debugging) {
        Log(_T("Already debugging. Detach or stop the current target first."));
        return;
    }

    CString path;
    m_pathEdit.GetWindowText(path);
    path.Trim();
    if (path.IsEmpty()) {
        Log(_T("Choose an executable first (Browse)."));
        return;
    }

    if (!g_debugger.DriverConnect()) {
        AfxMessageBox(_T("Driver not connected. Load LMvtdriverGuest.sys in the VM first."), MB_ICONERROR);
        return;
    }

    // 1. create suspended: the image is mapped but no code has run yet,
    //    which mirrors how Win32 debuggers attach before the initial break
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    wchar_t cmdline[2 * MAX_PATH] = {};
    wcscpy_s(cmdline, static_cast<LPCWSTR>(path)); // lpCommandLine must be writable

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        CString msg;
        msg.Format(_T("CreateProcess failed. Error code: %d"), err);
        AfxMessageBox(msg, MB_ICONERROR);
        Log(_T("Open failed."));
        return;
    }

    // 2. attach the still-suspended process through the driver
    if (!g_debugger.AttachProcess(pi.dwProcessId)) {
        DWORD err = g_debugger.GetLastErrorInfo();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CString msg;
        msg.Format(_T("Driver attach failed. Error code: %d"), err);
        AfxMessageBox(msg, MB_ICONERROR);
        Log(_T("Open failed."));
        return;
    }

    m_debugging = true;
    m_running = true;
    if (!StartWorkerThread()) {
        g_debugger.Detach();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        m_debugging = false;
        m_running = false;
        Log(_T("Cannot start the exception event worker."));
        return;
    }
    if (!g_debugger.WaitForEventBackend(15000)) {
        const DWORD error = g_debugger.GetLastErrorInfo();
        const ULONG phase = g_debugger.GetEventBackendPhase();
        StopWorkerThread();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CString message;
        message.Format(_T("Guest DR3 event backend failed (%u, stage=%u)."), error, phase);
        Log(message);
        m_debugging = false;
        m_running = false;
        return;
    }

    // 3. resume the primary thread only after the exception backend is attached
    ResumeThread(pi.hThread);

    // keep the handles for Stop / dialog teardown
    if (m_openProcess) CloseHandle(m_openProcess);
    if (m_openThread) CloseHandle(m_openThread);
    m_openProcess = pi.hProcess;
    m_openThread = pi.hThread;

    m_statusText.SetWindowText(_T("Status: Debugging"));
    CString msg;
    msg.Format(_T("Opened %s, PID %d"), static_cast<LPCTSTR>(path), pi.dwProcessId);
    Log(msg);
    RefreshAll();
}

void CMainDlg::OnBtnBrowse() {
    CFileDialog dlg(TRUE, _T("exe"), NULL,
        OFN_HIDEREADONLY | OFN_FILEMUSTEXIST,
        _T("Executable Files (*.exe)|*.exe|All Files (*.*)|*.*||"), this);
    dlg.m_ofn.lpstrTitle = _T("Select executable");
    if (dlg.DoModal() == IDOK) {
        m_pathEdit.SetWindowText(dlg.GetPathName());
        Log(_T("Path selected."));
    }
}

void CMainDlg::OnBtnAttach() {
    CString pidStr;
    m_pidEdit.GetWindowText(pidStr);
    uint64_t parsedPid = 0;
    if (!ParseUnsigned(pidStr, parsedPid, 10, MAXDWORD) || parsedPid == 0) {
        Log(_T("Invalid PID. Enter a decimal PID between 1 and 4294967295."));
        return;
    }
    DWORD pid = static_cast<DWORD>(parsedPid);

    HANDLE hCheck = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hCheck) {
        CString err;
        err.Format(_T("Cannot open process PID %d. Error code: %d"), pid, GetLastError());
        AfxMessageBox(err, MB_ICONERROR);
        Log(_T("Attach failed: process not found or access denied."));
        return;
    }
    CloseHandle(hCheck);

    if (!g_debugger.DriverConnect()) {
        AfxMessageBox(_T("Driver not connected. Load LMvtdriverGuest.sys in the VM first."), MB_ICONERROR);
        return;
    }

    if (g_debugger.AttachProcess(pid)) {
        m_debugging = true;
        m_running = true;
        if (!StartWorkerThread()) {
            g_debugger.Detach();
            m_debugging = false;
            m_running = false;
            Log(_T("Cannot start the exception event worker."));
            return;
        }
        if (!g_debugger.WaitForEventBackend(15000)) {
            const DWORD error = g_debugger.GetLastErrorInfo();
            const ULONG phase = g_debugger.GetEventBackendPhase();
            StopWorkerThread();
            m_debugging = false;
            m_running = false;
            CString message;
            message.Format(_T("Guest DR3 event backend failed (%u, stage=%u)."), error, phase);
            Log(message);
            return;
        }
        m_statusText.SetWindowText(_T("Status: Debugging"));
        CString msg;
        msg.Format(_T("Attached to PID %d"), pid);
        Log(msg);
        RefreshAll();
    } else {
        DWORD err = g_debugger.GetLastErrorInfo();
        CString errMsg;
        switch (err) {
        case ERROR_ACCESS_DENIED:
            errMsg = _T("Access denied. Run as Administrator and retry.");
            break;
        case ERROR_INVALID_PARAMETER:
            errMsg = _T("Invalid PID or the process does not exist.");
            break;
        default:
            errMsg.Format(_T("Attach failed. Error code: %d (0x%X)."), err, err);
            break;
        }
        AfxMessageBox(errMsg, MB_ICONERROR);
        Log(_T("Attach failed."));
    }
}

void CMainDlg::OnBtnDetach() {
    if (!m_debugging) { Log(_T("No process attached.")); return; }
    StopWorkerThread();
    EnterCriticalSection(&m_eventLock);
    m_events.clear();
    LeaveCriticalSection(&m_eventLock);
    m_debugging = false;
    m_running = false;
    m_statusText.SetWindowText(_T("Status: Detached"));
    Log(_T("Detached."));
    RefreshAll();
}

void CMainDlg::OnBtnRun() {
    if (!m_debugging) { Log(_T("No process.")); return; }
    if (!g_debugger.Continue()) { Log(_T("Continue failed (no pending event?).")); return; }
    m_running = true;
    m_statusText.SetWindowText(_T("Status: Running"));
    Log(_T("Running..."));
}

void CMainDlg::OnBtnGo() {
    if (!m_debugging) { Log(_T("No process.")); return; }
    if (!g_debugger.Continue()) { Log(_T("Continue failed (no pending event?).")); return; }
    m_running = true;
    m_statusText.SetWindowText(_T("Status: Running"));
    Log(_T("Go..."));
}

void CMainDlg::OnBtnStepInto() {
    if (!m_debugging) { Log(_T("No process.")); return; }
    if (m_running) { Log(_T("Process is running. Pause at an event first.")); return; }
    if (!g_debugger.StepInto(g_debugger.GetCurrentTid())) { Log(_T("Step failed.")); return; }
    m_running = true;
    m_statusText.SetWindowText(_T("Status: Stepping"));
    Log(_T("Step into..."));
}

void CMainDlg::OnBtnStepOver() {
    if (!m_debugging) { Log(_T("No process.")); return; }
    if (m_running) { Log(_T("Process is running. Pause at an event first.")); return; }
    if (!g_debugger.StepOver(g_debugger.GetCurrentTid())) { Log(_T("Step failed.")); return; }
    m_running = true;
    m_statusText.SetWindowText(_T("Status: Stepping"));
    Log(_T("Step over..."));
}

void CMainDlg::OnBtnStop() {
    if (!m_debugging) { Log(_T("No process.")); return; }
    g_debugger.Kill(); // TerminateProcess + detach (wakes the worker)
    StopWorkerThread();
    EnterCriticalSection(&m_eventLock);
    m_events.clear();
    LeaveCriticalSection(&m_eventLock);
    m_debugging = false;
    m_running = false;
    m_statusText.SetWindowText(_T("Status: Stopped"));
    Log(_T("Process killed."));
}

void CMainDlg::OnBtnBreak() {
    if (!m_debugging) { Log(_T("No process.")); return; }
    if (!m_running) { Log(_T("Target is already paused at an event.")); return; }

    // the synthetic breakpoint arrives through the worker thread like any
    // other event and pauses the UI; nothing blocks here
    if (!g_debugger.BreakTarget()) {
        Log(_T("Break failed. Check that the target is running and attached."));
        return;
    }
    m_statusText.SetWindowText(_T("Status: Breaking..."));
    Log(_T("Break requested..."));
}

void CMainDlg::OnBtnPing() {
    ULONG version = 0, flags = 0;
    if (g_debugger.PingDriver(&version, &flags)) {
        CString msg;
        msg.Format(_T("Ping OK. Driver protocol v%u, hook %s."), version,
                   (flags & DBGSYS_FLAG_HOOK_ACTIVE) ? _T("installed") : _T("not installed"));
        Log(msg);
    } else {
        CString msg;
        msg.Format(_T("Ping failed. Error code: %u"), g_debugger.GetLastErrorInfo());
        Log(msg);
    }
    RefreshDriverStatus();
}

void CMainDlg::OnTimer(UINT_PTR nIDEvent) {
    if (nIDEvent == DRV_PING_TIMER) {
        RefreshDriverStatus();
    }
    CDialogEx::OnTimer(nIDEvent);
}

void CMainDlg::OnBtnBpSet() {
    if (!m_debugging) { Log(_T("No process.")); return; }
    CString addrStr;
    m_bpAddr.GetWindowText(addrStr);
    if (addrStr.IsEmpty()) return;
    uint64_t addr = 0;
    if (!ParseAddr(addrStr, addr) || addr == 0) {
        Log(_T("Invalid breakpoint address. Use a non-zero hexadecimal address."));
        return;
    }

    int type = m_bpType.GetCurSel();
    if (type < 0 || type > 2) {
        Log(_T("Select a breakpoint type first."));
        return;
    }
    uint32_t bpId = 0;
    if (type == 0) { // Software
        bpId = g_debugger.SetSoftwareBreakpoint(addr);
    } else if (type == 1) { // Hardware (driver support pending)
        HardwareBreakCondition hc = HardwareBreakCondition::Execute;
        int condSel = m_bpCond.GetCurSel();
        if (condSel == 1) hc = HardwareBreakCondition::Write;
        else if (condSel == 2) hc = HardwareBreakCondition::ReadWrite;
        bpId = g_debugger.SetHardwareBreakpoint(addr, hc);
    } else { // Memory (driver support pending)
        CString sizeStr;
        m_bpSize.GetWindowText(sizeStr);
        uint64_t sz = 1;
        if (!sizeStr.IsEmpty() && (!ParseUnsigned(sizeStr, sz, 10, 0x100000) || sz == 0)) {
            Log(_T("Invalid memory breakpoint size. Enter a decimal value from 1 to 1048576."));
            return;
        }
        if (addr > UINT64_MAX - (sz - 1)) {
            Log(_T("Memory breakpoint range overflows the address space."));
            return;
        }
        MemoryBreakCondition mc = MemoryBreakCondition::ReadWrite;
        int condSel = m_bpCond.GetCurSel();
        if (condSel == 0) mc = MemoryBreakCondition::Read;
        else if (condSel == 1) mc = MemoryBreakCondition::Write;
        else if (condSel == 2) mc = MemoryBreakCondition::Execute;
        bpId = g_debugger.SetMemoryBreakpoint(addr, sz, mc);
    }
    if (bpId == 0) {
        Log(_T("Failed to set breakpoint. Check the address, or that the type is supported (HW/MEM are not wired to the driver yet)."));
        return;
    }
    CString msg;
    msg.Format(_T("BP set at 0x%llX"), addr);
    Log(msg);
    RefreshBreakpoints();
}

// list items start with the decimal id, e.g. "3  SW  on  0000000000401000"
static bool ParseBpIdFromItem(const CString& text, uint32_t& id) {
    int sp = text.Find(_T(' '));
    CString tok = (sp > 0) ? text.Left(sp) : text;
    errno = 0;
    TCHAR* end = nullptr;
    unsigned __int64 parsed = _tcstoui64(tok, &end, 10);
    if (errno == ERANGE || end == static_cast<LPCTSTR>(tok) || *end != _T('\0') || parsed == 0 || parsed > UINT32_MAX) {
        return false;
    }
    id = static_cast<uint32_t>(parsed);
    return true;
}

void CMainDlg::OnBtnBpDel() {
    int sel = m_breakpointList.GetCurSel();
    if (sel == LB_ERR) return;
    CString text;
    m_breakpointList.GetText(sel, text);
    uint32_t id = 0;
    if (!ParseBpIdFromItem(text, id)) return;
    g_debugger.RemoveBreakpoint(id);
    RefreshBreakpoints();
}

void CMainDlg::OnBtnBpEnable() {
    int sel = m_breakpointList.GetCurSel();
    if (sel == LB_ERR) return;
    CString text;
    m_breakpointList.GetText(sel, text);
    uint32_t id = 0;
    if (!ParseBpIdFromItem(text, id)) return;
    g_debugger.EnableBreakpoint(id);
    RefreshBreakpoints();
}

void CMainDlg::OnBtnBpDisable() {
    int sel = m_breakpointList.GetCurSel();
    if (sel == LB_ERR) return;
    CString text;
    m_breakpointList.GetText(sel, text);
    uint32_t id = 0;
    if (!ParseBpIdFromItem(text, id)) return;
    g_debugger.DisableBreakpoint(id);
    RefreshBreakpoints();
}
void CMainDlg::OnBtnMemRead() {
    if (!m_debugging) return;
    CString addrStr, sizeStr;
    m_memoryAddr.GetWindowText(addrStr);
    m_memorySize.GetWindowText(sizeStr);
    if (addrStr.IsEmpty()) return;
    uint64_t addr = 0;
    if (!ParseAddr(addrStr, addr)) {
        Log(_T("Invalid memory address. Use hexadecimal, for example 7FF700001000."));
        return;
    }
    uint64_t parsedSize = 128;
    if (!sizeStr.IsEmpty() && !ParseUnsigned(sizeStr, parsedSize, 10, 256)) {
        Log(_T("Invalid memory size. Enter a decimal value from 1 to 256."));
        return;
    }
    if (parsedSize == 0 || addr > UINT64_MAX - (parsedSize - 1)) {
        Log(_T("Memory read range is invalid."));
        return;
    }
    size_t size = static_cast<size_t>(parsedSize);

    uint8_t buf[256];
    CString out;
    size_t lines = (size + 15) / 16;
    for (size_t line = 0; line < lines && line < 8; line++) {
        uint64_t la = addr + line * 16;
        size_t rs = size - line * 16;
        if (rs > 16) rs = 16;
        if (g_debugger.ReadMemory(la, buf, rs)) {
            CString hex, asc;
            for (size_t i = 0; i < 16; i++) {
                if (i < rs) {
                    CString b;
                    b.Format(_T("%02X "), buf[i]);
                    hex += b;
                    asc += (buf[i] >= 32 && buf[i] < 127) ? (TCHAR)buf[i] : _T('.');
                } else {
                    hex += _T("   ");
                }
            }
            CString lineStr;
            lineStr.Format(_T("%016llX  %s %s\r\n"), la, (LPCTSTR)hex, (LPCTSTR)asc);
            out += lineStr;
        }
    }
    m_memoryEdit.SetWindowText(out);
}

void CMainDlg::OnBtnMemWrite() {
    Log(_T("Memory write not implemented in the UI yet."));
}

void CMainDlg::OnRefreshRegs() {
    RefreshRegisters();
}

void CMainDlg::OnRegsApply() {
    if (!m_debugging) { Log(_T("No process attached.")); return; }
    DWORD tid = g_debugger.GetCurrentTid();
    if (m_running || tid == 0) { Log(_T("Wait for an exception before editing registers.")); return; }
    RegisterContext rc = {};
    if (!g_debugger.TryGetRegisters(tid, rc)) {
        Log(_T("Cannot read the paused thread context; no registers were changed."));
        return;
    }
    CString text;
    m_regsEdit.GetWindowText(text);
    int pos = 0;
    int applied = 0;
    int skippedDr = 0;
    int skippedSegments = 0;
    CString line = text.Tokenize(_T("\r\n"), pos);
    while (!line.IsEmpty()) {
        int eq = line.Find(_T('='));
        if (eq > 0 && eq < line.GetLength() - 1) {
            CString name = line.Left(eq);
            name.Trim(); name.MakeUpper();
            CString val = line.Mid(eq + 1);
            val.Trim();
            if (name.Left(2) == _T("DR")) {
                skippedDr++; // debug registers are not exposed by the driver yet
                line = text.Tokenize(_T("\r\n"), pos);
                continue;
            }
            if (name == _T("CS") || name == _T("SS") || name == _T("DS") ||
                name == _T("ES") || name == _T("FS") || name == _T("GS")) {
                skippedSegments++;
                line = text.Tokenize(_T("\r\n"), pos);
                continue;
            }
            uint64_t v = 0;
            if (!ParseUnsigned(val, v, 0, UINT64_MAX)) {
                CString msg;
                msg.Format(_T("Invalid register value for %s."), (LPCTSTR)name);
                Log(msg);
                line = text.Tokenize(_T("\r\n"), pos);
                continue;
            }
            bool ok = false;
            if (name == _T("RAX")) { rc.rax = v; ok = true; }
            else if (name == _T("RBX")) { rc.rbx = v; ok = true; }
            else if (name == _T("RCX")) { rc.rcx = v; ok = true; }
            else if (name == _T("RDX")) { rc.rdx = v; ok = true; }
            else if (name == _T("RSI")) { rc.rsi = v; ok = true; }
            else if (name == _T("RDI")) { rc.rdi = v; ok = true; }
            else if (name == _T("RBP")) { rc.rbp = v; ok = true; }
            else if (name == _T("RSP")) { rc.rsp = v; ok = true; }
            else if (name == _T("R8"))  { rc.r8  = v; ok = true; }
            else if (name == _T("R9"))  { rc.r9  = v; ok = true; }
            else if (name == _T("R10")) { rc.r10 = v; ok = true; }
            else if (name == _T("R11")) { rc.r11 = v; ok = true; }
            else if (name == _T("R12")) { rc.r12 = v; ok = true; }
            else if (name == _T("R13")) { rc.r13 = v; ok = true; }
            else if (name == _T("R14")) { rc.r14 = v; ok = true; }
            else if (name == _T("R15")) { rc.r15 = v; ok = true; }
            else if (name == _T("RIP")) { rc.rip = v; ok = true; }
            else if (name == _T("RFLAGS")) { rc.rflags = v; ok = true; }
            else if (name == _T("CS")) { rc.cs = (uint16_t)v; ok = true; }
            else if (name == _T("DS")) { rc.ds = (uint16_t)v; ok = true; }
            else if (name == _T("ES")) { rc.es = (uint16_t)v; ok = true; }
            else if (name == _T("FS")) { rc.fs = (uint16_t)v; ok = true; }
            else if (name == _T("GS")) { rc.gs = (uint16_t)v; ok = true; }
            else if (name == _T("SS")) { rc.ss = (uint16_t)v; ok = true; }
            if (ok) applied++;
        }
        line = text.Tokenize(_T("\r\n"), pos);
    }
    if (applied > 0) {
        if (g_debugger.SetRegisters(tid, rc)) {
            if (skippedSegments > 0) Log(_T("Segment selectors are read-only in this backend."));
            CString msg;
            msg.Format(_T("Applied %d register change(s) to thread 0x%X."), applied, tid);
            Log(msg);
            RefreshRegisters();
        } else {
            Log(_T("Failed to write registers to target thread."));
        }
    } else if (skippedDr > 0) {
        Log(_T("DR0-DR7 edits ignored: the driver does not expose debug registers yet."));
    } else {
        Log(_T("No register changes detected."));
    }
}

void CMainDlg::OnRefreshDisasm() {
    RefreshDisassembly();
}
// ============================================================================
// Dialog lifecycle
// ============================================================================
BOOL CMainDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    g_pDlg = this;
    g_uiThreadId = GetCurrentThreadId();
    g_logHwnd = m_hWnd;

    // Route std::cout / std::cerr (used by Debugger diagnostics) into the
    // output edit control, thread-safely.
    g_cout_buf.sink = [](const std::string& s) { RouteLog(s); };
    g_cerr_buf.sink = [](const std::string& s) { RouteLog(s); };
    g_old_cout = std::cout.rdbuf(&g_cout_buf);
    g_old_cerr = std::cerr.rdbuf(&g_cerr_buf);

    // BP type combo
    m_bpType.AddString(_T("Software"));
    m_bpType.AddString(_T("Hardware"));
    m_bpType.AddString(_T("Memory"));
    m_bpType.SetCurSel(0);

    m_bpCond.AddString(_T("Execute"));
    m_bpCond.AddString(_T("Write"));
    m_bpCond.AddString(_T("ReadWrite"));
    m_bpCond.SetCurSel(0);

    m_disasmCount.SetWindowText(_T("16"));
    m_memorySize.SetWindowText(_T("80"));
    m_bpSize.SetWindowText(_T("1"));

    // Fixed-width font for disasm / memory / output
    CFont fixedFont;
    fixedFont.CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, _T("Consolas"));
    m_disasmlist.SetFont(&fixedFont);
    m_memoryEdit.SetFont(&fixedFont);
    m_outputEdit.SetFont(&fixedFont);
    fixedFont.Detach();

    Log(_T("LMvtdriverDebugger MFC Debugger ready."));

    if (!g_debugger.DriverConnect()) {
        AfxMessageBox(_T("Cannot open \\\\.\\LMvtdriverGuest. Load LMvtdriverGuest.sys in the VM first."), MB_ICONWARNING);
    }

    RefreshDriverStatus();
    SetTimer(DRV_PING_TIMER, 2000, NULL);

    return TRUE;
}

void CMainDlg::OnClose() {
    OnCancel();
}

void CMainDlg::OnOK() {
    OnCancel();
}

void CMainDlg::OnCancel() {
    if (m_closeInProgress) return;
    m_closeInProgress = true;
    KillTimer(DRV_PING_TIMER);
    bool ready = g_debugger.Detach();
    if (ready) {
        StopWorkerThread();
        ready = m_worker == nullptr;
        if (!ready) Log(_T("Close blocked: event worker has not exited. Retry after it finishes."));
        m_debugging = false;
        m_running = false;
        m_statusText.SetWindowText(_T("Status: Detached"));
    }
    if (!ready) {
        CString message;
        message.Format(_T("Shutdown cleanup failed (error %lu). The window remains open. Do not force-unload drivers. Retry closing after checking the log."),
            g_debugger.GetLastErrorInfo());
        Log(message);
        AfxMessageBox(message, MB_OK | MB_ICONWARNING);
        SetTimer(DRV_PING_TIMER, 2000, nullptr);
        m_closeInProgress = false;
        return;
    }
    g_debugger.DriverDisconnect();
    CDialogEx::OnCancel();
}

void CMainDlg::OnDestroy() {
    KillTimer(DRV_PING_TIMER);

    m_stopWorker = true;
    if (m_debugging) g_debugger.Detach();

    if (m_openProcess) { CloseHandle(m_openProcess); m_openProcess = NULL; }
    if (m_openThread) { CloseHandle(m_openThread); m_openThread = NULL; }

    if (m_worker) {
        if (WaitForSingleObject(m_worker->m_hThread, 5000) == WAIT_OBJECT_0) {
            delete m_worker;
            m_worker = nullptr;
        }
    }

    if (g_old_cout) { std::cout.rdbuf(g_old_cout); g_old_cout = nullptr; }
    if (g_old_cerr) { std::cerr.rdbuf(g_old_cerr); g_old_cerr = nullptr; }

    // Free log strings still sitting in the message queue.
    MSG msg;
    while (::PeekMessage(&msg, m_hWnd, WM_APP_LOG, WM_APP_LOG, PM_REMOVE)) {
        std::unique_ptr<std::string> p(reinterpret_cast<std::string*>(msg.wParam));
    }

    EnterCriticalSection(&m_eventLock);
    m_events.clear();
    LeaveCriticalSection(&m_eventLock);

    g_debugger.DriverDisconnect();
    g_pDlg = nullptr;
    g_logHwnd = nullptr;
    CDialogEx::OnDestroy();
    DeleteCriticalSection(&m_eventLock);
}

