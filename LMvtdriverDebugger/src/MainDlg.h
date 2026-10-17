#pragma once
#include "resource.h"
#include "debugger.h"
#include <atomic>
#include <deque>
#include <vector>

class CDisassemblyDlg;

// ============================================================================
// Transparent mode event record
// ============================================================================
struct TransparentEventRecord {
    UINT64 timestamp;
    UINT64 rip;
    UINT32 exceptionType;
    HOST_EXCEPTION_EVENT fullEvent;  // Complete event data
};

// ============================================================================
// Main dialog: attaches to a running process through dbgsys.sys and shows
// exception events delivered by the driver-side KiDispatchException hook.
//
// Event flow:
//   worker thread : GetEvent()    -> blocks inside the driver (PopEvent)
//   worker thread : QueueEvent()  -> push to m_events + post WM_DEBUG_EVENT
//   UI thread     : OnDebugEvent() -> ProcessEvent() -> Continue() or pause
// ============================================================================
class CMainDlg : public CDialogEx {
    DECLARE_DYNAMIC(CMainDlg)
public:
    CMainDlg(CWnd* pParent = nullptr);

    enum { IDD = IDD_MAIN_DIALOG };
    enum { WM_DEBUG_EVENT = WM_APP + 1, WM_APP_LOG = WM_APP + 2, DRV_PING_TIMER = 1 };

    CEdit m_regsEdit;
    CListBox m_disasmlist;
    CEdit m_memoryEdit;
    CListBox m_breakpointList;
    CEdit m_outputEdit;
    CEdit m_disasmAddr;
    CEdit m_disasmCount;
    CEdit m_memoryAddr;
    CEdit m_memorySize;
    CEdit m_bpAddr;
    CComboBox m_bpType;
    CComboBox m_bpCond;
    CEdit m_bpSize;
    CEdit m_pidEdit;
    CEdit m_pathEdit;
    CStatic m_statusText;
    CStatic m_driverStatus;
    CTabCtrl m_tabControl;  // Tab control for mode switching
    CListCtrl m_transparentEventList;  // New: event list for transparent mode
    CEdit m_eventDetailsEdit;  // Transparent mode: selected event details
    CEdit m_outputEditT;  // Transparent mode: output window
    CEdit m_transparentDr2Address;
    CComboBox m_transparentDr2Length;
    CComboBox m_transparentDr2Condition;
    CButton m_transparentDr2Set;
    CButton m_transparentDr2Stop;
    CStatic m_transparentDr2Status;
    CStatic m_statTotal;  // Statistics: total events
    CStatic m_statDropped;  // Statistics: dropped events
    CStatic m_statErrors;  // Statistics: errors
    CStatic m_statRate;  // Statistics: event rate

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnCancel() override;
    virtual void OnOK() override;
    DECLARE_MESSAGE_MAP()

public:
    afx_msg void OnBtnOpen();
    afx_msg void OnBtnBrowse();
    afx_msg void OnBtnAttach();
    afx_msg void OnBtnDetach();
    afx_msg void OnBtnRun();
    afx_msg void OnBtnGo();
    afx_msg void OnBtnStepInto();
    afx_msg void OnBtnStepOver();
    afx_msg void OnBtnStop();
    afx_msg void OnBtnBreak();
    afx_msg void OnBtnPing();
    afx_msg void OnBtnBpSet();
    afx_msg void OnBtnBpDel();
    afx_msg void OnBtnBpEnable();
    afx_msg void OnBtnBpDisable();
    afx_msg void OnBtnDr2Watch();
    afx_msg void OnBtnDr2Stop();
    afx_msg void OnBtnMemRead();
    afx_msg void OnBtnMemWrite();
    afx_msg void OnRefreshRegs();
    afx_msg void OnRegsApply();
    afx_msg void OnRefreshDisasm();
    afx_msg void OnBtnShowDisasm();
    afx_msg void OnDestroy();
    afx_msg void OnClose();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg LRESULT OnDebugEvent(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnAppLog(WPARAM wParam, LPARAM lParam);
    afx_msg void OnTransparentEventSelected(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnBtnClearEvents();
    afx_msg void OnTabSelChange(NMHDR* pNMHDR, LRESULT* pResult);

    // used from the event worker thread
    bool WorkerShouldStop() const { return m_stopWorker.load(); }
    void QueueEvent(const DBG_EVENT_INFO& evt);
    void Log(const CString& msg);

private:
    void RefreshAll();
    void RefreshDriverStatus();
    void RefreshRegisters();
    void RefreshDisassembly();
    void RefreshBreakpoints();
    void UpdateTransparentDr2Controls();
    void InitTransparentEventList();
    void AddTransparentEvent(const HOST_EXCEPTION_EVENT& evt);
    void DisplayTransparentEvent(int index);
    void ShowTab(int tabIndex);  // Show/hide controls based on tab selection
    bool ParseUnsigned(const CString& text, uint64_t& value, int base, uint64_t maxValue) const;
    bool ParseAddr(const CString& text, uint64_t& value) const;
    bool StartWorkerThread();
    void StopWorkerThread();
    static UINT AFX_CDECL DebugEventWorker(LPVOID pParam);

    CRITICAL_SECTION m_eventLock;
    std::deque<DBG_EVENT_INFO> m_events;
    CWinThread* m_worker;
    std::atomic<bool> m_stopWorker;
    HANDLE m_openProcess;   // process/thread handles from Open (create-suspended)
    HANDLE m_openThread;
    bool m_running;
    bool m_debugging;
    bool m_closeInProgress = false;
    uint32_t m_transparentDr2BreakpointId = 0;
    std::vector<TransparentEventRecord> m_transparentEvents;  // Event history for transparent mode
    CDisassemblyDlg* m_transparentDisasmDlg = nullptr;
    static const int MAX_TRANSPARENT_EVENTS = 10000;  // Limit to prevent memory overflow
};
