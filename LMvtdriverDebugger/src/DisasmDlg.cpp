#include "stdafx.h"
#include "DisasmDlg.h"

#include <cerrno>

IMPLEMENT_DYNAMIC(CDisassemblyDlg, CDialogEx)

CDisassemblyDlg::CDisassemblyDlg(Debugger* debugger, CWnd* parent)
    : CDialogEx(IDD_DISASM_DIALOG, parent),
      m_debugger(debugger),
      m_address(0),
      m_listLeft(0),
      m_listTop(0),
      m_listRightMargin(0),
      m_listBottomMargin(0),
      m_layoutReady(false) {
}

void CDisassemblyDlg::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_DISASM_WINDOW_ADDR, m_addressEdit);
    DDX_Control(pDX, IDC_DISASM_WINDOW_COUNT, m_countEdit);
    DDX_Control(pDX, IDC_DISASM_WINDOW_LIST, m_instructionList);
}

BEGIN_MESSAGE_MAP(CDisassemblyDlg, CDialogEx)
    ON_BN_CLICKED(IDC_DISASM_WINDOW_REFRESH, &CDisassemblyDlg::OnBnClickedRefresh)
    ON_WM_CLOSE()
    ON_WM_DESTROY()
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
END_MESSAGE_MAP()

BOOL CDisassemblyDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();

    m_countEdit.SetWindowText(_T("16"));
    m_fixedFont.CreateFont(
        16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, _T("Consolas"));
    m_instructionList.SetFont(&m_fixedFont);

    RECT listRect = {};
    RECT clientRect = {};
    m_instructionList.GetWindowRect(&listRect);
    ScreenToClient(&listRect);
    GetClientRect(&clientRect);
    m_listLeft = listRect.left;
    m_listTop = listRect.top;
    m_listRightMargin = clientRect.right - listRect.right;
    m_listBottomMargin = clientRect.bottom - listRect.bottom;
    m_layoutReady = true;

    SetAddress(m_address);
    CenterWindow(GetParent());
    return TRUE;
}

void CDisassemblyDlg::SetAddress(uint64_t address) {
    m_address = address;
    if (m_addressEdit.GetSafeHwnd() == nullptr)
        return;

    if (address == 0) {
        m_addressEdit.SetWindowText(_T(""));
        return;
    }

    CString text;
    text.Format(_T("%016I64X"), address);
    m_addressEdit.SetWindowText(text);
    if (IsWindowVisible())
        RefreshDisassembly();
}

bool CDisassemblyDlg::ParseAddress(const CString& text, uint64_t& value) const {
    CString input(text);
    input.Trim();
    if (input.IsEmpty() || input[0] == _T('-') || input[0] == _T('+'))
        return false;

    errno = 0;
    TCHAR* end = nullptr;
    unsigned __int64 parsed = _tcstoui64(input, &end, 16);
    if (errno == ERANGE || end == static_cast<LPCTSTR>(input) || *end != _T('\0'))
        return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool CDisassemblyDlg::ParseCount(const CString& text, int& value) const {
    CString input(text);
    input.Trim();
    if (input.IsEmpty() || input[0] == _T('-') || input[0] == _T('+'))
        return false;

    errno = 0;
    TCHAR* end = nullptr;
    unsigned __int64 parsed = _tcstoui64(input, &end, 10);
    if (errno == ERANGE || end == static_cast<LPCTSTR>(input) || *end != _T('\0') ||
        parsed < 1 || parsed > 128)
        return false;
    value = static_cast<int>(parsed);
    return true;
}

void CDisassemblyDlg::SetStatus(const CString& text) {
    m_instructionList.ResetContent();
    m_instructionList.AddString(text);
}

void CDisassemblyDlg::RefreshDisassembly() {
    m_instructionList.ResetContent();
    if (m_debugger == nullptr || !m_debugger->IsActive()) {
        SetStatus(_T("Debugger is not attached."));
        return;
    }

    CString addressText;
    CString countText;
    m_addressEdit.GetWindowText(addressText);
    m_countEdit.GetWindowText(countText);

    uint64_t address = 0;
    if (!ParseAddress(addressText, address)) {
        SetStatus(_T("Enter a hexadecimal RIP address."));
        return;
    }

    int count = 16;
    if (!countText.IsEmpty() && !ParseCount(countText, count)) {
        SetStatus(_T("Count must be a decimal value from 1 to 128."));
        return;
    }

    m_address = address;
    const auto instructions = m_debugger->Disassemble(address, count);
    if (instructions.empty()) {
        CString message;
        message.Format(_T("Cannot disassemble %016I64X (error %lu)."),
            address, m_debugger->GetLastErrorInfo());
        SetStatus(message);
        return;
    }

    for (const auto& instruction : instructions) {
        CString bytes(instruction.bytes.c_str());
        CString mnemonic(instruction.mnemonic.c_str());
        CString operands(instruction.operands.c_str());
        CString line;
        line.Format(_T("   %016I64X  %-30s  %s %s"),
            instruction.address,
            static_cast<LPCTSTR>(bytes),
            static_cast<LPCTSTR>(mnemonic),
            static_cast<LPCTSTR>(operands));
        m_instructionList.AddString(line);
    }

    if (instructions.size() < static_cast<size_t>(count))
        m_instructionList.AddString(_T("Disassembly stopped: subsequent bytes could not be read."));
}

void CDisassemblyDlg::OnBnClickedRefresh() {
    RefreshDisassembly();
}

void CDisassemblyDlg::OnCancel() {
    ShowWindow(SW_HIDE);
}

void CDisassemblyDlg::OnClose() {
    ShowWindow(SW_HIDE);
}

void CDisassemblyDlg::OnDestroy() {
    m_layoutReady = false;
    CDialogEx::OnDestroy();
}

void CDisassemblyDlg::OnSize(UINT nType, int cx, int cy) {
    CDialogEx::OnSize(nType, cx, cy);
    if (!m_layoutReady || m_instructionList.GetSafeHwnd() == nullptr)
        return;

    const int width = max(0, cx - m_listLeft - m_listRightMargin);
    const int height = max(0, cy - m_listTop - m_listBottomMargin);
    m_instructionList.MoveWindow(m_listLeft, m_listTop, width, height, TRUE);
}

void CDisassemblyDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI) {
    CDialogEx::OnGetMinMaxInfo(lpMMI);
    lpMMI->ptMinTrackSize.x = 440;
    lpMMI->ptMinTrackSize.y = 220;
}
