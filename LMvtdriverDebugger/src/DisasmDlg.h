#pragma once

#include "resource.h"
#include "debugger.h"

class CDisassemblyDlg : public CDialogEx {
    DECLARE_DYNAMIC(CDisassemblyDlg)

public:
    CDisassemblyDlg(Debugger* debugger, CWnd* parent = nullptr);

    enum { IDD = IDD_DISASM_DIALOG };

    void SetAddress(uint64_t address);
    void RefreshDisassembly();

protected:
    virtual void DoDataExchange(CDataExchange* pDX) override;
    virtual BOOL OnInitDialog() override;
    virtual void OnCancel() override;

    afx_msg void OnBnClickedRefresh();
    afx_msg void OnClose();
    afx_msg void OnDestroy();
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);

    DECLARE_MESSAGE_MAP()

private:
    bool ParseAddress(const CString& text, uint64_t& value) const;
    bool ParseCount(const CString& text, int& value) const;
    void SetStatus(const CString& text);

    Debugger* m_debugger;
    CEdit m_addressEdit;
    CEdit m_countEdit;
    CListBox m_instructionList;
    CFont m_fixedFont;
    uint64_t m_address;
    int m_listLeft;
    int m_listTop;
    int m_listRightMargin;
    int m_listBottomMargin;
    bool m_layoutReady;
};
