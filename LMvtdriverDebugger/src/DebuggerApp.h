#pragma once
#include "debugger.h"

class CDebuggerApp : public CWinApp {
public:
    virtual BOOL InitInstance();
    DECLARE_MESSAGE_MAP()
};

extern CDebuggerApp theApp;
extern Debugger g_debugger;
