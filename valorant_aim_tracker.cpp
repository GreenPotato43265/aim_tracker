/*
  valorant_aim_tracker.cpp  �  Win32 API only, no external libraries
  Visual Studio: Windows Desktop Application (C++) project, add this file, F5.

  Features:
�     LEFT pane  = editable raw text
�     RIGHT pane = live display with [avg=X.XX] appended to every # line
�     Tooltip popup appears whenever you have a line selected (cursor on it)
      showing the average for that line  (e.g.  "avg = 22.00  (4 values)")
�     Every keystroke immediately saved to file (no debounce delay)
�     Ctrl+A  � select all text in focused pane
�     Ctrl+O  � Open file
�     Font� button  � Windows font picker
�     Settings (font name/size, last file) saved to avg_editor.ini
�     Title bar shows current file + [saved] status
*/

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

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")

/* -- IDs -------------------------------------------------------------------- */
#define ID_EDIT_RAW    101
#define ID_EDIT_DISP   102
#define ID_BTN_OPEN    201
#define ID_BTN_CLEAR   202
#define ID_BTN_FONT    203
#define ID_STATUS      301
#define TIMER_DISPLAY  1001   // display refresh (120 ms debounce � visual only)
#define TIMER_TOOLTIP  1002   // tooltip update poll (200 ms)

/* -- globals ---------------------------------------------------------------- */
static HWND   g_hWnd = nullptr;
static HWND   g_hRaw = nullptr;
static HWND   g_hDisp = nullptr;
static HWND   g_hStatus = nullptr;
static HFONT  g_hFont = nullptr;   // editor font (monospace)
static HFONT  g_hBtnFont = nullptr;   // small UI font
static HBRUSH g_hDispBrush = nullptr;

static bool   g_displayDirty = false;

/* Settings */
static char  g_fontName[LF_FACESIZE] = "Consolas";
static int   g_fontSize = 20;
static char  g_curFile[MAX_PATH] = {};
static char  g_iniPath[MAX_PATH] = {};

/* Tooltip � simple owner-drawn popup, avoids Win32 tooltip control quirks */
static HWND  g_hTipWnd = nullptr;
static int   g_lastTooltipLine = -999;
static char  g_tipText[128] = {};

/* Last selection in raw edit � restored when window is re-activated */
static DWORD g_selStart = 0, g_selEnd = 0;

/* Set to false when our window loses activation � tooltip must stay hidden */
static bool  g_windowActive = true;

/* Original WNDPROC of g_hRaw before subclassing */
static WNDPROC g_origRawProc = nullptr;

/* -- INI helpers ------------------------------------------------------------ */
static void buildIniPath()
{
    GetModuleFileNameA(nullptr, g_iniPath, MAX_PATH);
    char* p = strrchr(g_iniPath, '\\');
    if (p) *(p + 1) = '\0';
    strcat_s(g_iniPath, "avg_editor.ini");
}
static void saveSettings()
{
    char buf[32];
    WritePrivateProfileStringA("Font", "Name", g_fontName, g_iniPath);
    sprintf_s(buf, "%d", g_fontSize);
    WritePrivateProfileStringA("Font", "Size", buf, g_iniPath);
    WritePrivateProfileStringA("File", "LastPath", g_curFile, g_iniPath);
}
static void loadSettings()
{
    GetPrivateProfileStringA("Font", "Name", "Consolas", g_fontName, LF_FACESIZE, g_iniPath);
    g_fontSize = GetPrivateProfileIntA("Font", "Size", 20, g_iniPath);
    if (g_fontSize < 8)  g_fontSize = 8;
    if (g_fontSize > 72) g_fontSize = 72;
    GetPrivateProfileStringA("File", "LastPath", "", g_curFile, MAX_PATH, g_iniPath);
}

/* -- Font ------------------------------------------------------------------- */
static void rebuildFont()
{
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

/* -- Title bar -------------------------------------------------------------- */
static void updateTitle()
{
    char t[MAX_PATH + 64];
    if (g_curFile[0])
        sprintf_s(t, "# Avg Editor  �  %s", g_curFile);
    else
        sprintf_s(t, "# Avg Editor  �  [no file � Ctrl+O to open]");
    SetWindowTextA(g_hWnd, t);
}

/* -- Numeric helpers -------------------------------------------------------- */
static bool isNumber(const std::string& s)
{
    if (s.empty()) return false;
    size_t i = 0;
    if (s[i] == '-' || s[i] == '+') ++i;
    if (i >= s.size()) return false;
    bool dot = false, hasDigit = false;
    for (; i < s.size(); ++i) {
        if (s[i] == '.') { if (dot)return false;dot = true; }
        else if (isdigit((unsigned char)s[i])) { hasDigit = true; }
        else return false;
    }
    return hasDigit;
}

/* Returns {avg, count} for numbers after the first # on a line.
   count==0 means no numbers found. */
struct AvgResult { double avg; int count; };
static AvgResult calcAvg(const std::string& line)
{
    size_t h = line.find('#');
    if (h == std::string::npos) return { 0,0 };
    std::string after = line.substr(h + 1);
    size_t h2 = after.find('#');
    std::string seg = (h2 != std::string::npos) ? after.substr(0, h2) : after;
    std::istringstream ss(seg);
    std::string tok;
    std::vector<double> nums;
    while (ss >> tok)
        if (isNumber(tok)) { try { nums.push_back(std::stod(tok)); } catch (...) {}; }
    if (nums.empty()) return { 0,0 };
    double sum = 0; for (double n : nums) sum += n;
    return { sum / (double)nums.size(), (int)nums.size() };
}

static std::string processLine(const std::string& raw)
{
    auto [avg, cnt] = calcAvg(raw);
    if (cnt == 0) return raw;

    // Keep everything up to and including the '#', hide the numbers, show avg
    size_t h = raw.find('#');
    std::string prefix = raw.substr(0, h + 1);  // e.g. "0.75 trans 5#"

    // If there is a second # (comment), keep it and everything after
    std::string after = raw.substr(h + 1);
    size_t h2 = after.find('#');
    std::string comment = (h2 != std::string::npos) ? "  " + after.substr(h2) : "";

    std::ostringstream out;
    out << prefix << "  [avg=" << std::fixed << std::setprecision(2) << avg << " n=" << cnt << "]" << comment;
    return out.str();
}

/* -- Get raw text ----------------------------------------------------------- */
static std::string getRawText()
{
    int len = GetWindowTextLengthA(g_hRaw);
    if (len == 0) return "";
    std::string buf(len + 2, '\0');
    GetWindowTextA(g_hRaw, &buf[0], len + 1);
    buf.resize(len);
    return buf;
}

/* Split raw (CRLF) into lines (strip CR) */
static std::vector<std::string> getLines()
{
    std::string raw = getRawText();
    std::string lf; lf.reserve(raw.size());
    for (char c : raw) if (c != '\r') lf += c;
    std::vector<std::string> lines;
    std::istringstream ss(lf);
    std::string l;
    while (std::getline(ss, l)) lines.push_back(l);
    return lines;
}

/* -- Auto-derive save path from exe folder if none set --------------------- */
static void ensureCurFile()
{
    if (g_curFile[0]) return;
    // Use same folder as the ini/exe, filename = avg_editor_data.txt
    GetModuleFileNameA(nullptr, g_curFile, MAX_PATH);
    char* p = strrchr(g_curFile, '\\');
    if (p) *(p + 1) = '\0';
    strcat_s(g_curFile, "avg_editor_data.txt");
    saveSettings();
    updateTitle();
}

/* -- Write raw to file immediately, no dialog ever ------------------------- */
static void saveToFile()
{
    ensureCurFile();
    std::string raw = getRawText();
    std::string lf; lf.reserve(raw.size());
    for (char c : raw) if (c != '\r') lf += c;
    std::ofstream f(g_curFile, std::ios::binary);
    if (f) f << lf;
}

/* -- Update display pane ---------------------------------------------------- */
static void updateDisplay()
{
    auto lines = getLines();
    std::ostringstream out;
    int total = 0, withAvg = 0;
    for (int i = 0;i < (int)lines.size();++i) {
        if (i > 0) out << "\r\n";
        std::string proc = processLine(lines[i]);
        if (proc != lines[i]) ++withAvg;
        out << proc;
        ++total;
    }
    SetWindowTextA(g_hDisp, out.str().c_str());

    std::string status = "Lines: " + std::to_string(total)
        + "   with avg: " + std::to_string(withAvg)
        + "   |  Ctrl+A = select all   Ctrl+O = open";
    if (g_curFile[0]) status += "   |  autosave: ON";
    SetWindowTextA(g_hStatus, status.c_str());
    g_displayDirty = false;
}

/* -- Subclass proc for g_hRaw � catches WM_KILLFOCUS to hide tip ------------ */
LRESULT CALLBACK RawEditProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KILLFOCUS) {
        if (g_hTipWnd) ShowWindow(g_hTipWnd, SW_HIDE);
        g_lastTooltipLine = -999;
    }
    return CallWindowProcA(g_origRawProc, hw, msg, wp, lp);
}

/* -- Tooltip: simple owner-drawn popup window ------------------------------ */

static int getCaretLine()
{
    return (int)SendMessageA(g_hRaw, EM_LINEFROMCHAR, (WPARAM)-1, 0);
}

// WndProc for the popup tip window � just paints text on a yellow background
LRESULT CALLBACK TipWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hw, &ps);
        RECT rc; GetClientRect(hw, &rc);
        // Background
        HBRUSH br = CreateSolidBrush(RGB(255, 255, 180));
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        // Border
        FrameRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        // Text � use the same font as the editor (g_hFont)
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

static void registerTipClass(HINSTANCE hInst)
{
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TipWndProc;
    wc.hInstance = hInst;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = "AvgTipClass";
    RegisterClassExA(&wc);
}

static void hideTip()
{
    if (g_hTipWnd) ShowWindow(g_hTipWnd, SW_HIDE);
    g_lastTooltipLine = -999;
}

static void showTip(const char* text, POINT screenPt)
{
    strcpy_s(g_tipText, text);

    // Measure text using the same font as the editor
    HDC hdc = GetDC(nullptr);
    HFONT old = (HFONT)SelectObject(hdc, g_hFont);
    SIZE sz; GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    SelectObject(hdc, old);
    ReleaseDC(nullptr, hdc);

    int W = sz.cx + 14, H = sz.cy + 8;
    // Place to the right of screenPt (end of line), vertically centred on it
    int x = screenPt.x + 8;
    int y = screenPt.y;

    // Keep on screen
    int sw = GetSystemMetrics(SM_CXSCREEN);
    if (x + W > sw) x = sw - W - 4;

    SetWindowPos(g_hTipWnd, HWND_TOPMOST, x, y, W, H,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hTipWnd, nullptr, TRUE);
}

/* Called every TIMER_TOOLTIP ms */
static void updateTooltip()
{
    // If the raw edit doesn't have focus, we shouldn't show a tip
    if (GetFocus() != g_hRaw) {
        hideTip();
        return;
    }

    int lineIdx = getCaretLine();
    auto lines = getLines();

    if (lineIdx < 0 || lineIdx >= (int)lines.size()) {
        hideTip();
        return;
    }

    auto [avg, cnt] = calcAvg(lines[lineIdx]);
    if (cnt == 0) {
        hideTip();
        return;
    }

    char buf[128];
    sprintf_s(buf, "avg = %.2f   (%d values)", avg, cnt);

    int lineCharIdx = (int)SendMessageA(g_hRaw, EM_LINEINDEX, (WPARAM)lineIdx, 0);
    int lineLen = (int)SendMessageA(g_hRaw, EM_LINELENGTH, (WPARAM)lineCharIdx, 0);
    int lineEndChar = lineCharIdx + lineLen;

    LRESULT lpos = SendMessageA(g_hRaw, EM_POSFROMCHAR, (WPARAM)lineEndChar, 0);
    if (lpos == -1) return; // Character not visible

    POINT pt = { (int)(short)LOWORD(lpos), (int)(short)HIWORD(lpos) };
    ClientToScreen(g_hRaw, &pt);

    showTip(buf, pt);
    g_lastTooltipLine = lineIdx;
}
/* -- Open ------------------------------------------------------------------- */
static void doOpen()
{
    char path[MAX_PATH] = {};
    if (g_curFile[0]) strcpy_s(path, g_curFile);
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = "Text files\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) return;

    std::ifstream f(path, std::ios::binary);
    if (!f) { MessageBoxA(g_hWnd, "Cannot open file.", "Error", MB_ICONERROR);return; }
    std::ostringstream ss; ss << f.rdbuf();
    std::string txt = ss.str();

    // Normalise to CRLF
    std::string crlf; crlf.reserve(txt.size() + 512);
    for (size_t i = 0;i < txt.size();++i) {
        if (txt[i] == '\r') continue;
        if (txt[i] == '\n') crlf += '\r';
        crlf += txt[i];
    }
    SetWindowTextA(g_hRaw, crlf.c_str());
    strcpy_s(g_curFile, path);
    saveSettings();
    updateTitle();
    updateDisplay();
}

/* -- Font picker ------------------------------------------------------------ */
static void doChooseFont()
{
    LOGFONTA lf = {};
    HDC hdc = GetDC(nullptr);
    lf.lfHeight = -MulDiv(g_fontSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, hdc);
    lf.lfWeight = FW_NORMAL;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    strcpy_s(lf.lfFaceName, g_fontName);

    CHOOSEFONTA cf = {};
    cf.lStructSize = sizeof(cf); cf.hwndOwner = g_hWnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_FIXEDPITCHONLY | CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS | CF_LIMITSIZE;
    cf.nSizeMin = 8; cf.nSizeMax = 72;
    if (!ChooseFontA(&cf)) return;

    strcpy_s(g_fontName, lf.lfFaceName);
    if (cf.iPointSize > 0) g_fontSize = cf.iPointSize / 10;
    rebuildFont();
    saveSettings();

    // Update button label
    char lbl[32]; sprintf_s(lbl, "Font (%dpt)", g_fontSize);
    SetWindowTextA(GetDlgItem(g_hWnd, ID_BTN_FONT), lbl);
}

/* -- Layout ----------------------------------------------------------------- */
static void layoutControls(HWND hWnd)
{
    RECT rc; GetClientRect(hWnd, &rc);
    int W = rc.right, H = rc.bottom;
    const int btnH = 30, btnY = 7, gap = 6, statH = 22;

    int x = gap;
    auto place = [&](int id, int w) {
        SetWindowPos(GetDlgItem(hWnd, id), nullptr, x, btnY, w, btnH, SWP_NOZORDER);
        x += w + gap;
        };
    place(ID_BTN_OPEN, 100);
    place(ID_BTN_CLEAR, 80);
    place(ID_BTN_FONT, 130);

    SetWindowPos(g_hStatus, nullptr, 0, H - statH, W, statH, SWP_NOZORDER);

    int editTop = btnY + btnH + gap * 2;
    int editH = H - editTop - statH - gap;
    int half = (W - gap * 3) / 2;

    SetWindowPos(g_hRaw, nullptr, gap, editTop, half, editH, SWP_NOZORDER);
    SetWindowPos(g_hDisp, nullptr, gap * 2 + half, editTop, half, editH, SWP_NOZORDER);
}

/* -- WndProc ---------------------------------------------------------------- */
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
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
        // Subclass to intercept WM_KILLFOCUS for tooltip hiding
        g_origRawProc = (WNDPROC)SetWindowLongPtrA(g_hRaw, GWLP_WNDPROC,
            (LONG_PTR)RawEditProc);

        g_hDisp = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", es | ES_READONLY,
            0, 0, 0, 0, hWnd, (HMENU)ID_EDIT_DISP, GetModuleHandleA(nullptr), nullptr);
        SendMessageA(g_hDisp, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageA(g_hDisp, EM_SETLIMITTEXT, 0, 0);

        auto mkBtn = [&](const char* lbl, int id) {
            HWND b = CreateWindowA("BUTTON", lbl, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)(UINT_PTR)id, GetModuleHandleA(nullptr), nullptr);
            SendMessageA(b, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);
            };
        mkBtn("Open", ID_BTN_OPEN);
        mkBtn("Clear", ID_BTN_CLEAR);

        char fontLbl[32]; sprintf_s(fontLbl, "Font (%dpt)", g_fontSize);
        mkBtn(fontLbl, ID_BTN_FONT);

        g_hStatus = CreateWindowA("STATIC", "Ready", WS_CHILD | WS_VISIBLE | SS_SUNKEN,
            0, 0, 0, 0, hWnd, (HMENU)ID_STATUS, GetModuleHandleA(nullptr), nullptr);
        SendMessageA(g_hStatus, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);

        /* -- Custom tip popup window -- */
        g_hTipWnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            "AvgTipClass", "",
            WS_POPUP,
            0, 0, 100, 24,
            hWnd, nullptr, GetModuleHandleA(nullptr), nullptr);

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

    case WM_COMMAND:
    {
        WORD id = LOWORD(wParam), evt = HIWORD(wParam);

        if (id == ID_EDIT_RAW && evt == EN_CHANGE) {
            g_displayDirty = true;
            // Immediate save on every change
            saveToFile();
        }

        if (evt == BN_CLICKED) {
            switch (id) {
            case ID_BTN_OPEN:  doOpen(); break;
            case ID_BTN_CLEAR:
                SetWindowTextA(g_hRaw, "");
                saveToFile();
                updateDisplay();
                break;
            case ID_BTN_FONT:  doChooseFont(); break;
            }
        }
        return 0;
    }

case WM_ACTIVATE:
    {
        if (LOWORD(wParam) == WA_INACTIVE) {
            g_windowActive = false;
            hideTip();
            // Store selection before we lose focus
            SendMessageA(g_hRaw, EM_GETSEL, (WPARAM)&g_selStart, (LPARAM)&g_selEnd);
        }
        else {
            g_windowActive = true;
            SetFocus(g_hRaw);
            // Restore selection and scroll to it
            SendMessageA(g_hRaw, EM_SETSEL, g_selStart, g_selEnd);
            SendMessageA(g_hRaw, EM_SCROLLCARET, 0, 0);
        }
        return 0;
    }
    // WM_ACTIVATEAPP is no longer needed; WM_ACTIVATE handles it.

    case WM_TIMER:
        if (wParam == TIMER_DISPLAY && g_displayDirty) updateDisplay();
        if (wParam == TIMER_TOOLTIP) updateTooltip();
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_DISPLAY);
        KillTimer(hWnd, TIMER_TOOLTIP);
        hideTip();
        saveSettings();
        if (g_hFont)      DeleteObject(g_hFont);
        if (g_hBtnFont)   DeleteObject(g_hBtnFont);
        if (g_hDispBrush) DeleteObject(g_hDispBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

/* -- WinMain ---------------------------------------------------------------- */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow)
{
    buildIniPath();
    loadSettings();

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

    // Restore last file
    if (g_curFile[0]) {
        std::ifstream f(g_curFile, std::ios::binary);
        if (f) {
            std::ostringstream ss; ss << f.rdbuf();
            std::string txt = ss.str();
            std::string crlf;
            for (size_t i = 0;i < txt.size();++i) {
                if (txt[i] == '\r') continue;
                if (txt[i] == '\n') crlf += '\r';
                crlf += txt[i];
            }
            SetWindowTextA(g_hRaw, crlf.c_str());
            updateDisplay();
        }
        else {
            g_curFile[0] = '\0';
        }
    }
    updateTitle();

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0)) {
        // F5 � insert today's date as DD.MM.YY at caret in raw edit
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F5) {
            if (GetFocus() == g_hRaw) {
                SYSTEMTIME st; GetLocalTime(&st);
                char date[12];
                sprintf_s(date, "%02d.%02d.%02d",
                    st.wDay, st.wMonth, st.wYear % 100);
                // Replace current selection (or insert at caret) with date
                SendMessageA(g_hRaw, EM_REPLACESEL, TRUE, (LPARAM)date);
                continue;
            }
        }
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (msg.wParam == 'O') { doOpen();continue; }
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