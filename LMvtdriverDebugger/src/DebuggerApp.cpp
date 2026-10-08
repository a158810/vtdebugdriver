#include "stdafx.h"
#include "DebuggerApp.h"
#include "MainDlg.h"
#include "resource.h"

BEGIN_MESSAGE_MAP(CDebuggerApp, CWinApp)
END_MESSAGE_MAP()

CDebuggerApp theApp;

// Last-resort filter: never let a transient invalid-handle debug/subsystem
// exception terminate the GUI. STATUS_INVALID_HANDLE can be raised as a
// first-chance exception by nested debugging; logging it is enough.
static LONG WINAPI AppUnhandledExceptionFilter(_EXCEPTION_POINTERS* ep) {
    DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;
    if (code == 0xC0000008) return EXCEPTION_EXECUTE_HANDLER;
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL CDebuggerApp::InitInstance() {
    CWinApp::InitInstance();
    AfxEnableControlContainer();
    SetRegistryKey(_T("lmdbgr3"));
    SetUnhandledExceptionFilter(AppUnhandledExceptionFilter);

    CMainDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();
    return FALSE;
}
