#pragma once
#include "resource.h"
#include "debugger.h"
#include <atomic>
#include <deque>

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
    afx_msg void OnBtnMemRead();
    afx_msg void OnBtnMemWrite();
    afx_msg void OnRefreshRegs();
    afx_msg void OnRegsApply();
    afx_msg void OnRefreshDisasm();
    afx_msg void OnDestroy();
    afx_msg void OnClose();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg LRESULT OnDebugEvent(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnAppLog(WPARAM wParam, LPARAM lParam);

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
};
