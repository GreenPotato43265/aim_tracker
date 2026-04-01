/*
  valorant_aim_tracker.cpp  –  Win32 API only, no external libraries
  (Scroll sync fixed – standard edit controls now work correctly)
*/

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <string>
#include <sstream>
#include <vector>
#include <iomanip>
#include <fstream>
#include <cctype>
#include <shlwapi.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

/* -- IDs -------------------------------------------------------------------- */
#define ID_EDIT_RAW    101
#define ID_EDIT_DISP   102
#define ID_BTN_OPEN    201
#define ID_BTN_FONT    203
#define ID_BTN_NEWTAB  204
#define ID_BTN_CLOSETAB 205
#define ID_TAB         401
#define ID_STATUS      301

#define TIMER_DISPLAY  1001
#define TIMER_TOOLTIP  1002

/* -- Forward declarations --------------------------------------------------- */
static std::string getRawText();
static void updateDisplay();
static void updateTitle();
static void SaveCurrentTabToFile();
static void UpdateCurrentTabContent();
static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);
static std::string InputBox(const std::string& title, const std::string& prompt, const std::string& defaultValue);
static LRESULT CALLBACK InputBoxProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp);
static void hideTip();
static void showTip(const char* text, POINT screenPt);
static void updateTooltip();
LRESULT CALLBACK TipWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp);
static void registerTipClass(HINSTANCE hInst);
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void UpdateTabText(int idx);
static void AddTab(const std::string& filePath, const std::string& content);
static void RemoveTab(int idx);
static void LoadTab(int idx);
static void SyncVerticalScroll(HWND source, HWND target);
static int getCaretLine();
static void buildIniPath();
static void saveSettings();
static void loadSettings();
static void rebuildFont();
static void RenameCurrentTab();
static bool isNumber(const std::string& s);
struct AvgResult { double avg; int count; };
static AvgResult calcAvg(const std::string& line);
static std::string processLine(const std::string& raw);
static std::vector<std::string> getLines();
static void doOpen();
static void doNewTab();
static void doCloseTab();
static void doChooseFont();
static void layoutControls(HWND hWnd);

/* -- globals ---------------------------------------------------------------- */
static HWND   g_hWnd = nullptr;
static HWND   g_hRaw = nullptr;
static HWND   g_hDisp = nullptr;
static HWND   g_hStatus = nullptr;
static HWND   g_hTab = nullptr;
static HFONT  g_hFont = nullptr;
static HFONT  g_hBtnFont = nullptr;
static HBRUSH g_hDispBrush = nullptr;

static bool   g_displayDirty = false;

/* Settings (global for the whole app) */
static char  g_fontName[LF_FACESIZE] = "Consolas";
static int   g_fontSize = 20;
static char  g_iniPath[MAX_PATH] = {};

/* Tooltip popup */
static HWND  g_hTipWnd = nullptr;
static int   g_lastTooltipLine = -999;
static char  g_tipText[128] = {};

/* Last selection – restored when window is reactivated */
static DWORD g_selStart = 0, g_selEnd = 0;
static bool  g_windowActive = true;

/* ======================== TAB MANAGEMENT ======================== */
struct TabData {
    std::string filePath;
    std::string content;
};
static std::vector<TabData> g_tabs;
static int g_activeTab = -1;

static std::string GetFileNameOnly(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    if (pos != std::string::npos)
        return path.substr(pos + 1);
    return path;
}

static void UpdateTabText(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) return;
    std::string fullName = GetFileNameOnly(g_tabs[idx].filePath);
    size_t dot = fullName.find_last_of('.');
    std::string displayName = (dot != std::string::npos) ? fullName.substr(0, dot) : fullName;
    if (displayName.empty()) displayName = "untitled";

    TCITEMA ti = { 0 };
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<char*>(displayName.c_str());
    TabCtrl_SetItem(g_hTab, idx, &ti);
}

static void AddTab(const std::string& filePath, const std::string& content) {
    TabData td;
    td.filePath = filePath;
    td.content = content;
    g_tabs.push_back(td);
    int idx = (int)g_tabs.size() - 1;

    std::string fullName = GetFileNameOnly(filePath);
    size_t dot = fullName.find_last_of('.');
    std::string displayName = (dot != std::string::npos) ? fullName.substr(0, dot) : fullName;
    if (displayName.empty()) displayName = "untitled";

    TCITEMA ti = { 0 };
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<char*>(displayName.c_str());
    TabCtrl_InsertItem(g_hTab, idx, &ti);
}

static void RemoveTab(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) return;
    g_tabs.erase(g_tabs.begin() + idx);
    TabCtrl_DeleteItem(g_hTab, idx);
    if (g_activeTab == idx) g_activeTab = -1;
    else if (g_activeTab > idx) g_activeTab--;
}

static void LoadTab(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) return;
    g_activeTab = idx;
    SetWindowTextA(g_hRaw, g_tabs[idx].content.c_str());
    updateDisplay();
    updateTitle();
    g_displayDirty = true;
    updateDisplay();

    // Reset scroll positions to top
    SendMessageA(g_hRaw, EM_LINESCROLL, 0, -1000000);
    SendMessageA(g_hDisp, EM_LINESCROLL, 0, -1000000);
}

static void SaveCurrentTabToFile() {
    if (g_activeTab < 0 || g_activeTab >= (int)g_tabs.size()) return;
    TabData& tab = g_tabs[g_activeTab];
    if (tab.filePath.empty()) return;

    std::string raw = getRawText();
    std::string lf;
    lf.reserve(raw.size());
    for (char c : raw) if (c != '\r') lf += c;
    std::ofstream f(tab.filePath, std::ios::binary);
    if (f) f << lf;
}

static void UpdateCurrentTabContent() {
    if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size()) {
        g_tabs[g_activeTab].content = getRawText();
    }
}

/* ======================== SIMPLE INPUT BOX (no resource) ================== */
struct InputBoxData {
    std::string result;
    std::string prompt;
    std::string defaultValue;
    HWND hEdit;
    bool closed;
};

static LRESULT CALLBACK InputBoxProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    InputBoxData* data = (InputBoxData*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtr(hDlg, GWLP_USERDATA, lp);
        data = (InputBoxData*)lp;
        SetWindowTextA(hDlg, data->prompt.c_str());
        SetDlgItemTextA(hDlg, 1001, data->defaultValue.c_str());
        data->hEdit = GetDlgItem(hDlg, 1001);
        SetFocus(data->hEdit);
        SendMessage(data->hEdit, EM_SETSEL, 0, -1);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            char buf[512];
            GetDlgItemTextA(hDlg, 1001, buf, sizeof(buf));
            data->result = buf;
            data->closed = true;
            DestroyWindow(hDlg);
        }
        else if (LOWORD(wp) == IDCANCEL) {
            data->closed = true;
            DestroyWindow(hDlg);
        }
        return TRUE;
    }
    return DefWindowProcA(hDlg, msg, wp, lp);
}

static std::string InputBox(const std::string& title, const std::string& prompt, const std::string& defaultValue) {
    InputBoxData data;
    data.result = "";
    data.prompt = prompt;
    data.defaultValue = defaultValue;
    data.closed = false;

    HWND hDlg = CreateWindowExA(0, "#32770", title.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 300, 150,
        g_hWnd, nullptr, GetModuleHandleA(nullptr), &data);
    if (!hDlg) return "";

    CreateWindowExA(0, "STATIC", prompt.c_str(),
        WS_CHILD | WS_VISIBLE,
        10, 10, 280, 20,
        hDlg, (HMENU)1000, GetModuleHandleA(nullptr), nullptr);

    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", defaultValue.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        10, 40, 280, 25,
        hDlg, (HMENU)1001, GetModuleHandleA(nullptr), nullptr);

    CreateWindowExA(0, "BUTTON", "OK",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        150, 80, 60, 25,
        hDlg, (HMENU)IDOK, GetModuleHandleA(nullptr), nullptr);

    CreateWindowExA(0, "BUTTON", "Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        220, 80, 60, 25,
        hDlg, (HMENU)IDCANCEL, GetModuleHandleA(nullptr), nullptr);

    SetWindowLongPtrA(hDlg, GWLP_WNDPROC, (LONG_PTR)InputBoxProc);
    SetWindowLongPtrA(hDlg, GWLP_USERDATA, (LONG_PTR)&data);

    ShowWindow(hDlg, SW_SHOW);
    MSG msg;
    while (!data.closed && GetMessageA(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return data.result;
}

/* ======================== RENAME CURRENT TAB ============================== */
static void RenameCurrentTab() {
    if (g_activeTab < 0) return;
    TabData& tab = g_tabs[g_activeTab];
    std::string oldPath = tab.filePath;
    std::string oldName = GetFileNameOnly(oldPath);
    size_t dot = oldName.find_last_of('.');
    std::string oldBase = (dot != std::string::npos) ? oldName.substr(0, dot) : oldName;

    std::string newBase = InputBox("Rename Tab", "New name (without extension):", oldBase);
    if (newBase.empty() || newBase == oldBase) return;

    std::string extension;
    if (dot != std::string::npos) extension = oldName.substr(dot);
    std::string newName = newBase + extension;
    std::string newPath = oldPath;
    size_t lastSlash = newPath.find_last_of("\\/");
    if (lastSlash != std::string::npos)
        newPath = newPath.substr(0, lastSlash + 1) + newName;
    else
        newPath = newName;

    if (!MoveFileA(oldPath.c_str(), newPath.c_str())) {
        MessageBoxA(g_hWnd, "Failed to rename file.", "Error", MB_ICONERROR);
        return;
    }

    tab.filePath = newPath;
    UpdateTabText(g_activeTab);
    updateTitle();
    SaveCurrentTabToFile();
}

/* ======================== INI HELPERS ======================== */
static void buildIniPath() {
    GetModuleFileNameA(nullptr, g_iniPath, MAX_PATH);
    char* p = strrchr(g_iniPath, '\\');
    if (p) *(p + 1) = '\0';
    strcat_s(g_iniPath, "avg_editor.ini");
}
static void saveSettings() {
    char buf[32];
    WritePrivateProfileStringA("Font", "Name", g_fontName, g_iniPath);
    sprintf_s(buf, "%d", g_fontSize);
    WritePrivateProfileStringA("Font", "Size", buf, g_iniPath);

    WritePrivateProfileStringA("Tabs", "Count", std::to_string(g_tabs.size()).c_str(), g_iniPath);
    for (size_t i = 0; i < g_tabs.size(); ++i) {
        char key[32];
        sprintf_s(key, "Tab%zu", i);
        WritePrivateProfileStringA("Tabs", key, g_tabs[i].filePath.c_str(), g_iniPath);
    }
    sprintf_s(buf, "%d", g_activeTab);
    WritePrivateProfileStringA("Tabs", "Active", buf, g_iniPath);
}
static void loadSettings() {
    GetPrivateProfileStringA("Font", "Name", "Consolas", g_fontName, LF_FACESIZE, g_iniPath);
    g_fontSize = GetPrivateProfileIntA("Font", "Size", 20, g_iniPath);
    if (g_fontSize < 8)  g_fontSize = 8;
    if (g_fontSize > 72) g_fontSize = 72;

    int count = GetPrivateProfileIntA("Tabs", "Count", 0, g_iniPath);
    if (count > 0) {
        for (int i = 0; i < count; ++i) {
            char key[32];
            sprintf_s(key, "Tab%d", i);
            char path[MAX_PATH] = {};
            GetPrivateProfileStringA("Tabs", key, "", path, MAX_PATH, g_iniPath);
            if (path[0]) {
                std::ifstream f(path, std::ios::binary);
                std::string content;
                if (f) {
                    std::ostringstream ss; ss << f.rdbuf();
                    std::string txt = ss.str();
                    for (size_t j = 0; j < txt.size(); ++j) {
                        if (txt[j] == '\r') continue;
                        if (txt[j] == '\n') content += '\r';
                        content += txt[j];
                    }
                }
                AddTab(path, content);
            }
        }
        int active = GetPrivateProfileIntA("Tabs", "Active", 0, g_iniPath);
        if (active >= 0 && active < (int)g_tabs.size())
            g_activeTab = active;
        else if (!g_tabs.empty())
            g_activeTab = 0;
    }
}

/* ======================== FONT ======================== */
static void rebuildFont() {
    if (g_hFont) DeleteObject(g_hFont);
    HDC hdc = GetDC(nullptr);
    int logH = -MulDiv(g_fontSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, hdc);
    g_hFont = CreateFontA(logH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, g_fontName);
    if (g_hRaw)  SendMessageA(g_hRaw, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    if (g_hDisp) SendMessageA(g_hDisp, WM_SETFONT, (WPARAM)g_hFont, TRUE);
}

static void updateTitle() {
    char t[MAX_PATH + 64];
    if (g_activeTab >= 0 && !g_tabs[g_activeTab].filePath.empty())
        sprintf_s(t, "Avg Editor – %s", g_tabs[g_activeTab].filePath.c_str());
    else
        sprintf_s(t, "Avg Editor – [no file]");
    SetWindowTextA(g_hWnd, t);
}

/* ======================== NUMERIC HELPERS ======================== */
static bool isNumber(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[i] == '-' || s[i] == '+') ++i;
    if (i >= s.size()) return false;
    bool dot = false, hasDigit = false;
    for (; i < s.size(); ++i) {
        if (s[i] == '.') { if (dot) return false; dot = true; }
        else if (isdigit((unsigned char)s[i])) hasDigit = true;
        else return false;
    }
    return hasDigit;
}

static AvgResult calcAvg(const std::string& line) {
    size_t h = line.find('#');
    if (h == std::string::npos) return { 0,0 };
    std::string after = line.substr(h + 1);
    size_t h2 = after.find('#');
    std::string seg = (h2 != std::string::npos) ? after.substr(0, h2) : after;
    std::istringstream ss(seg);
    std::string tok;
    std::vector<double> nums;
    while (ss >> tok)
        if (isNumber(tok)) {
            try { nums.push_back(std::stod(tok)); }
            catch (...) {}
        }
    if (nums.empty()) return { 0,0 };
    double sum = 0;
    for (double n : nums) sum += n;
    return { sum / (double)nums.size(), (int)nums.size() };
}

static std::string processLine(const std::string& raw) {
    auto [avg, cnt] = calcAvg(raw);
    if (cnt == 0) return raw;

    size_t h = raw.find('#');
    std::string prefix = raw.substr(0, h + 1);
    std::string after = raw.substr(h + 1);
    size_t h2 = after.find('#');
    std::string comment = (h2 != std::string::npos) ? "  " + after.substr(h2) : "";

    std::ostringstream out;
    out << prefix << "  [avg=" << std::fixed << std::setprecision(2) << avg
        << " n=" << cnt << "]" << comment;
    return out.str();
}

static std::string getRawText() {
    int len = GetWindowTextLengthA(g_hRaw);
    if (len == 0) return "";
    std::string buf(len + 2, '\0');
    GetWindowTextA(g_hRaw, &buf[0], len + 1);
    buf.resize(len);
    return buf;
}

static std::vector<std::string> getLines() {
    std::string raw = getRawText();
    std::string lf;
    lf.reserve(raw.size());
    for (char c : raw) if (c != '\r') lf += c;
    std::vector<std::string> lines;
    std::istringstream ss(lf);
    std::string l;
    while (std::getline(ss, l)) lines.push_back(l);
    return lines;
}

static void updateDisplay() {
    auto lines = getLines();
    std::ostringstream out;
    int total = 0, withAvg = 0;
    for (int i = 0; i < (int)lines.size(); ++i) {
        if (i > 0) out << "\r\n";
        std::string proc = processLine(lines[i]);
        if (proc != lines[i]) ++withAvg;
        out << proc;
        ++total;
    }
    SetWindowTextA(g_hDisp, out.str().c_str());

    std::string status = "Lines: " + std::to_string(total)
        + "   with avg: " + std::to_string(withAvg)
        + "   |  Ctrl+A = select all   Ctrl+O = open   F5 = date";
    if (g_activeTab >= 0 && !g_tabs[g_activeTab].filePath.empty())
        status += "   |  autosave: ON";
    SetWindowTextA(g_hStatus, status.c_str());
    g_displayDirty = false;
}

/* ======================== SCROLL SYNCHRONIZATION ======================== */
static void SyncVerticalScroll(HWND source, HWND target) {
    int srcLine = (int)SendMessageA(source, EM_GETFIRSTVISIBLELINE, 0, 0);
    int tgtLine = (int)SendMessageA(target, EM_GETFIRSTVISIBLELINE, 0, 0);
    int delta = srcLine - tgtLine;
    if (delta != 0) {
        SendMessageA(target, EM_LINESCROLL, 0, delta);
    }
}

static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_KILLFOCUS:
        if (hwnd == g_hRaw) {
            hideTip();
            g_lastTooltipLine = -999;
        }
        break;

    case WM_HSCROLL:
    case WM_VSCROLL:
    {
        HWND other = (hwnd == g_hRaw) ? g_hDisp : g_hRaw;
        // Forward the scroll message to the other control
        SendMessageA(other, msg, wp, lp);
        // Synchronize vertical positions
        if (msg == WM_VSCROLL) {
            SyncVerticalScroll(hwnd, other);
        }
        return 0; // Prevent default processing to avoid crash
    }
    break;

    case WM_MOUSEWHEEL:
    {
        HWND other = (hwnd == g_hRaw) ? g_hDisp : g_hRaw;
        // Forward mouse wheel to the other control
        SendMessageA(other, msg, wp, lp);
        // Synchronize vertical positions
        SyncVerticalScroll(hwnd, other);
        return 0; // Prevent default processing to avoid crash
    }
    break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ======================== TOOLTIP ======================== */
static int getCaretLine() {
    return (int)SendMessageA(g_hRaw, EM_LINEFROMCHAR, (WPARAM)-1, 0);
}

LRESULT CALLBACK TipWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hw, &ps);
        RECT rc; GetClientRect(hw, &rc);
        HBRUSH br = CreateSolidBrush(RGB(255, 255, 180));
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        FrameRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));
        HFONT old = (HFONT)SelectObject(hdc, g_hFont);
        rc.left += 6; rc.right -= 6; rc.top += 3; rc.bottom -= 3;
        DrawTextA(hdc, g_tipText, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
        EndPaint(hw, &ps);
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static void registerTipClass(HINSTANCE hInst) {
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TipWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "AvgTipClass";
    RegisterClassExA(&wc);
}

static void hideTip() {
    if (g_hTipWnd) ShowWindow(g_hTipWnd, SW_HIDE);
    g_lastTooltipLine = -999;
}

static void showTip(const char* text, POINT screenPt) {
    strcpy_s(g_tipText, text);
    HDC hdc = GetDC(nullptr);
    HFONT old = (HFONT)SelectObject(hdc, g_hFont);
    SIZE sz; GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    SelectObject(hdc, old);
    ReleaseDC(nullptr, hdc);
    int W = sz.cx + 14, H = sz.cy + 8;
    int x = screenPt.x + 8;
    int y = screenPt.y;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    if (x + W > sw) x = sw - W - 4;
    SetWindowPos(g_hTipWnd, HWND_TOPMOST, x, y, W, H,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hTipWnd, nullptr, TRUE);
}

static void updateTooltip() {
    if (GetFocus() != g_hRaw) { hideTip(); return; }
    int lineIdx = getCaretLine();
    auto lines = getLines();
    if (lineIdx < 0 || lineIdx >= (int)lines.size()) { hideTip(); return; }
    auto [avg, cnt] = calcAvg(lines[lineIdx]);
    if (cnt == 0) { hideTip(); return; }
    char buf[128];
    sprintf_s(buf, "avg = %.2f   (%d values)", avg, cnt);

    std::string& line = lines[lineIdx];
    size_t hashPos = line.find('#');
    if (hashPos == std::string::npos) { hideTip(); return; }

    int lineStart = (int)SendMessageA(g_hRaw, EM_LINEINDEX, (WPARAM)lineIdx, 0);
    int charPos = lineStart + (int)hashPos + 1;

    LRESULT lpos = SendMessageA(g_hRaw, EM_POSFROMCHAR, (WPARAM)charPos, 0);
    if (lpos == -1) return;
    POINT pt = { (int)(short)LOWORD(lpos), (int)(short)HIWORD(lpos) };
    ClientToScreen(g_hRaw, &pt);

    showTip(buf, pt);
    g_lastTooltipLine = lineIdx;
}

/* ======================== FILE OPERATIONS ======================== */
static void doOpen() {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = "Text files\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) return;

    std::ifstream f(path, std::ios::binary);
    if (!f) { MessageBoxA(g_hWnd, "Cannot open file.", "Error", MB_ICONERROR); return; }
    std::ostringstream ss; ss << f.rdbuf();
    std::string txt = ss.str();
    std::string crlf;
    crlf.reserve(txt.size() + 512);
    for (size_t i = 0; i < txt.size(); ++i) {
        if (txt[i] == '\r') continue;
        if (txt[i] == '\n') crlf += '\r';
        crlf += txt[i];
    }

    AddTab(path, crlf);
    int newIdx = (int)g_tabs.size() - 1;
    TabCtrl_SetCurSel(g_hTab, newIdx);
    LoadTab(newIdx);
    UpdateTabText(newIdx);
    saveSettings();
}

static void doNewTab() {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = "Text files\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameA(&ofn)) return;

    std::string content;
    if (PathFileExistsA(path)) {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            std::ostringstream ss; ss << f.rdbuf();
            std::string txt = ss.str();
            for (size_t i = 0; i < txt.size(); ++i) {
                if (txt[i] == '\r') continue;
                if (txt[i] == '\n') content += '\r';
                content += txt[i];
            }
        }
    }

    AddTab(path, content);
    int newIdx = (int)g_tabs.size() - 1;
    TabCtrl_SetCurSel(g_hTab, newIdx);
    LoadTab(newIdx);
    UpdateTabText(newIdx);
    if (content.empty()) SaveCurrentTabToFile();
    saveSettings();
}

static void doCloseTab() {
    if (g_activeTab < 0 || g_tabs.empty()) return;
    
    // Save current tab before closing
    if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size()) {
        g_tabs[g_activeTab].content = getRawText();
        SaveCurrentTabToFile();
    }
    
    if (g_tabs.size() == 1) {
        char defaultPath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, defaultPath, MAX_PATH);
        char* p = strrchr(defaultPath, '\\');
        if (p) *(p + 1) = '\0';
        strcat_s(defaultPath, "avg_editor_data.txt");
        AddTab(defaultPath, "");
        int newIdx = (int)g_tabs.size() - 1;
        TabCtrl_SetCurSel(g_hTab, newIdx);
        LoadTab(newIdx);
        UpdateTabText(newIdx);
        saveSettings();
        return;
    }
    
    RemoveTab(g_activeTab);
    if (g_tabs.empty()) {
        char defaultPath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, defaultPath, MAX_PATH);
        char* p = strrchr(defaultPath, '\\');
        if (p) *(p + 1) = '\0';
        strcat_s(defaultPath, "avg_editor_data.txt");
        AddTab(defaultPath, "");
        g_activeTab = 0;
        LoadTab(0);
        UpdateTabText(0);
    }
    else {
        int newActive = (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size()) ? g_activeTab : 0;
        TabCtrl_SetCurSel(g_hTab, newActive);
        LoadTab(newActive);
    }
    saveSettings();
}

static void doChooseFont() {
    LOGFONTA lf = {};
    HDC hdc = GetDC(nullptr);
    lf.lfHeight = -MulDiv(g_fontSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, hdc);
    lf.lfWeight = FW_NORMAL;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    strcpy_s(lf.lfFaceName, g_fontName);
    CHOOSEFONTA cf = {};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_hWnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_FIXEDPITCHONLY | CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS | CF_LIMITSIZE;
    cf.nSizeMin = 8; cf.nSizeMax = 72;
    if (!ChooseFontA(&cf)) return;
    strcpy_s(g_fontName, lf.lfFaceName);
    if (cf.iPointSize > 0) g_fontSize = cf.iPointSize / 10;
    rebuildFont();
    saveSettings();
    char lbl[32]; sprintf_s(lbl, "Font (%dpt)", g_fontSize);
    SetWindowTextA(GetDlgItem(g_hWnd, ID_BTN_FONT), lbl);
}

/* ======================== LAYOUT ======================== */
static void layoutControls(HWND hWnd) {
    RECT rc; GetClientRect(hWnd, &rc);
    int W = rc.right, H = rc.bottom;
    int tabH = 28;
    int btnH = 30;
    int gap = 6;

    SetWindowPos(g_hTab, nullptr, 0, 0, W, tabH, SWP_NOZORDER);

    int y = tabH + gap;
    int x = gap;
    auto place = [&](int id, int w) {
        SetWindowPos(GetDlgItem(hWnd, id), nullptr, x, y, w, btnH, SWP_NOZORDER);
        x += w + gap;
        };
    place(ID_BTN_OPEN, 100);
    place(ID_BTN_NEWTAB, 100);
    place(ID_BTN_CLOSETAB, 100);
    place(ID_BTN_FONT, 130);

    int statH = 22;
    SetWindowPos(g_hStatus, nullptr, 0, H - statH, W, statH, SWP_NOZORDER);

    int editTop = y + btnH + gap;
    int editH = H - editTop - statH - gap;
    int half = (W - gap * 3) / 2;

    SetWindowPos(g_hRaw, nullptr, gap, editTop, half, editH, SWP_NOZORDER);
    SetWindowPos(g_hDisp, nullptr, gap * 2 + half, editTop, half, editH, SWP_NOZORDER);
}

/* ======================== WINDOW PROCEDURE ======================== */
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        InitCommonControls();
        g_hBtnFont = CreateFontA(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        g_hDispBrush = CreateSolidBrush(RGB(235, 245, 255));
        rebuildFont();

        DWORD es = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL
            | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN;

        g_hRaw = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", es,
            0, 0, 0, 0, hWnd, (HMENU)ID_EDIT_RAW, GetModuleHandleA(nullptr), nullptr);
        SendMessageA(g_hRaw, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageA(g_hRaw, EM_SETLIMITTEXT, 0, 0);
        SetWindowSubclass(g_hRaw, EditSubclassProc, 0, 0);

        g_hDisp = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", es | ES_READONLY,
            0, 0, 0, 0, hWnd, (HMENU)ID_EDIT_DISP, GetModuleHandleA(nullptr), nullptr);
        SendMessageA(g_hDisp, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageA(g_hDisp, EM_SETLIMITTEXT, 0, 0);
        SetWindowSubclass(g_hDisp, EditSubclassProc, 0, 0);

        auto mkBtn = [&](const char* lbl, int id) {
            HWND b = CreateWindowA("BUTTON", lbl, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)(UINT_PTR)id, GetModuleHandleA(nullptr), nullptr);
            SendMessageA(b, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);
            };
        mkBtn("Open", ID_BTN_OPEN);
        mkBtn("New Tab", ID_BTN_NEWTAB);
        mkBtn("Close Tab", ID_BTN_CLOSETAB);
        char fontLbl[32]; sprintf_s(fontLbl, "Font (%dpt)", g_fontSize);
        mkBtn(fontLbl, ID_BTN_FONT);

        g_hStatus = CreateWindowA("STATIC", "Ready", WS_CHILD | WS_VISIBLE | SS_SUNKEN,
            0, 0, 0, 0, hWnd, (HMENU)ID_STATUS, GetModuleHandleA(nullptr), nullptr);
        SendMessageA(g_hStatus, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);

        g_hTab = CreateWindowExA(0, WC_TABCONTROL, "",
            WS_CHILD | WS_VISIBLE | TCS_FOCUSNEVER,
            0, 0, 0, 0,
            hWnd, (HMENU)ID_TAB, GetModuleHandleA(nullptr), nullptr);

        loadSettings();
        if (g_tabs.empty()) {
            char defaultPath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, defaultPath, MAX_PATH);
            char* p = strrchr(defaultPath, '\\');
            if (p) *(p + 1) = '\0';
            strcat_s(defaultPath, "avg_editor_data.txt");
            std::string content;
            std::ifstream f(defaultPath, std::ios::binary);
            if (f) {
                std::ostringstream ss; ss << f.rdbuf();
                std::string txt = ss.str();
                for (size_t i = 0; i < txt.size(); ++i) {
                    if (txt[i] == '\r') continue;
                    if (txt[i] == '\n') content += '\r';
                    content += txt[i];
                }
            }
            AddTab(defaultPath, content);
            g_activeTab = 0;
        }

        TabCtrl_SetCurSel(g_hTab, g_activeTab);
        LoadTab(g_activeTab);

        g_hTipWnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            "AvgTipClass", "", WS_POPUP,
            0, 0, 100, 24, hWnd, nullptr, GetModuleHandleA(nullptr), nullptr);

        SetTimer(hWnd, TIMER_DISPLAY, 120, nullptr);
        SetTimer(hWnd, TIMER_TOOLTIP, 200, nullptr);

        updateTitle();
        return 0;
    }

    case WM_SIZE:
        layoutControls(hWnd);
        return 0;

    case WM_CTLCOLOREDIT:
        if ((HWND)lParam == g_hDisp) {
            SetBkColor((HDC)wParam, RGB(235, 245, 255));
            return (LRESULT)g_hDispBrush;
        }
        break;

    case WM_COMMAND: {
        WORD id = LOWORD(wParam), evt = HIWORD(wParam);
        if (id == ID_EDIT_RAW && evt == EN_CHANGE) {
            g_displayDirty = true;
            UpdateCurrentTabContent();
            SaveCurrentTabToFile();
        }
        if (evt == BN_CLICKED) {
            switch (id) {
            case ID_BTN_OPEN:     doOpen(); break;
            case ID_BTN_NEWTAB:   doNewTab(); break;
            case ID_BTN_CLOSETAB: doCloseTab(); break;
            case ID_BTN_FONT:     doChooseFont(); break;
            }
        }
        return 0;
    }

    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lParam;
        if (nm->idFrom == ID_TAB && nm->code == TCN_SELCHANGE) {
            int newIdx = TabCtrl_GetCurSel(g_hTab);
            if (newIdx >= 0 && newIdx < (int)g_tabs.size() && newIdx != g_activeTab) {
                if (g_activeTab >= 0) {
                    g_tabs[g_activeTab].content = getRawText();
                    SaveCurrentTabToFile();
                }
                LoadTab(newIdx);
                saveSettings();
            }
        }
        else if (nm->idFrom == ID_TAB && nm->code == NM_RCLICK) {
            int idx = TabCtrl_GetCurSel(g_hTab);
            if (idx >= 0 && idx < (int)g_tabs.size()) {
                g_activeTab = idx;
                RenameCurrentTab();
            }
        }
        return 0;
    }

    case WM_ACTIVATE: {
        if (LOWORD(wParam) == WA_INACTIVE) {
            g_windowActive = false;
            hideTip();
            SendMessageA(g_hRaw, EM_GETSEL, (WPARAM)&g_selStart, (LPARAM)&g_selEnd);
        }
        else {
            g_windowActive = true;
            SetFocus(g_hRaw);
            SendMessageA(g_hRaw, EM_SETSEL, g_selStart, g_selEnd);
            SendMessageA(g_hRaw, EM_SCROLLCARET, 0, 0);
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_DISPLAY && g_displayDirty) updateDisplay();
        if (wParam == TIMER_TOOLTIP) updateTooltip();
        return 0;

    case WM_DESTROY:
        if (g_activeTab >= 0) {
            g_tabs[g_activeTab].content = getRawText();
            SaveCurrentTabToFile();
        }
        saveSettings();
        KillTimer(hWnd, TIMER_DISPLAY);
        KillTimer(hWnd, TIMER_TOOLTIP);
        hideTip();
        if (g_hFont)      DeleteObject(g_hFont);
        if (g_hBtnFont)   DeleteObject(g_hBtnFont);
        if (g_hDispBrush) DeleteObject(g_hDispBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

/* ======================== ENTRY POINT ======================== */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    buildIniPath();
    registerTipClass(hInst);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "AvgEditorClass";
    RegisterClassExA(&wc);

    g_hWnd = CreateWindowExA(0, "AvgEditorClass", "",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1380, 780,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hWnd, nShow);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F5 && GetFocus() == g_hRaw) {
            SYSTEMTIME st; GetLocalTime(&st);
            char date[12];
            sprintf_s(date, "%02d.%02d.%02d", st.wDay, st.wMonth, st.wYear % 100);
            SendMessageA(g_hRaw, EM_REPLACESEL, TRUE, (LPARAM)date);
            continue;
        }
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (msg.wParam == 'O') { doOpen(); continue; }
            if (msg.wParam == 'A') {
                HWND foc = GetFocus();
                if (foc == g_hRaw || foc == g_hDisp) {
                    SendMessageA(foc, EM_SETSEL, 0, -1);
                    continue;
                }
            }
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}