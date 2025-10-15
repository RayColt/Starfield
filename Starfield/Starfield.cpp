// MyStarfield.cpp
// Direct2D multi-monitor screensaver with animated preview, robust arg parsing, logging,
// input debounce, smart settings handling with ForcePopup fallback and resource presence check.
// Requires: resource.h + resources.rc (IDD_SETTINGS, control IDs).
// Link: d2d1.lib

#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>
#include <random>
#include <fstream>
#include <shellapi.h>
#include "resource.h"

#pragma comment(lib, "d2d1.lib")

// Registry keys
static LPCWSTR REG_KEY = L"Software\\MyStarfieldScreensaver";
static LPCWSTR REG_STARS = L"StarCount";
static LPCWSTR REG_SPEED = L"SpeedPercent";
static LPCWSTR REG_TWINKLE = L"TwinklePercent";
static LPCWSTR REG_COLOR_R = L"ColorR";
static LPCWSTR REG_COLOR_G = L"ColorG";
static LPCWSTR REG_COLOR_B = L"ColorB";

// Defaults
static int g_starCount = 600;
static int g_speedPercent = 60;
static int g_twinklePercent = 30;
static COLORREF g_color = RGB(255, 255, 240);

// Logging helper for debugging
static void log(const char* s) {
    CreateDirectoryW(L"C:\\Temp", NULL);
    std::ofstream f("C:\\Temp\\MyStarfield_log.txt", std::ios::app);
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        f << t.wYear << "-" << t.wMonth << "-" << t.wDay << " "
            << t.wHour << ":" << t.wMinute << ":" << t.wSecond
            << " pid=" << GetCurrentProcessId() << " : " << s << "\n";
    }
}

// Registry helpers
static int GetRegDWORD(LPCWSTR name, int def) {
    HKEY hKey; DWORD val = def; DWORD size = sizeof(val);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, name, NULL, NULL, (LPBYTE)&val, &size);
        RegCloseKey(hKey);
    }
    return (int)val;
}
static void SetRegDWORD(LPCWSTR name, DWORD v) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}
static void LoadSettings() {
    g_starCount = GetRegDWORD(REG_STARS, g_starCount);
    g_speedPercent = GetRegDWORD(REG_SPEED, g_speedPercent);
    g_twinklePercent = GetRegDWORD(REG_TWINKLE, g_twinklePercent);
    int r = GetRegDWORD(REG_COLOR_R, GetRValue(g_color));
    int g = GetRegDWORD(REG_COLOR_G, GetGValue(g_color));
    int b = GetRegDWORD(REG_COLOR_B, GetBValue(g_color));
    g_color = RGB(r, g, b);
}
static void SaveSettings() {
    SetRegDWORD(REG_STARS, (DWORD)g_starCount);
    SetRegDWORD(REG_SPEED, (DWORD)g_speedPercent);
    SetRegDWORD(REG_TWINKLE, (DWORD)g_twinklePercent);
    SetRegDWORD(REG_COLOR_R, (DWORD)GetRValue(g_color));
    SetRegDWORD(REG_COLOR_G, (DWORD)GetGValue(g_color));
    SetRegDWORD(REG_COLOR_B, (DWORD)GetBValue(g_color));
}

// Star model and render window structure
struct Star { float x, y, z, base, phase; };
struct RenderWindow {
    HWND hwnd = NULL;
    ID2D1Factory* factory = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    std::vector<Star> stars;
    RECT rc = {};
    std::mt19937 rng;
    bool isPreview = false;
};

static HINSTANCE g_hInst = NULL;
static std::vector<RenderWindow*> g_windows;
static bool g_running = true;

// Input debounce
static LARGE_INTEGER g_perfFreq;
static LARGE_INTEGER g_startCounter;
static double g_inputDebounceSeconds = 2.5;

// Utility: robust arg parsing (handles /p:1234 and /p 1234)
static void ParseArgs(int argc, wchar_t** argv, wchar_t& modeOut, HWND& hwndOut) {
    modeOut = 0; hwndOut = NULL;
    if (argc <= 1) return;
    std::wstring a1 = argv[1];
    if (a1.size() >= 2 && (a1[0] == L'/' || a1[0] == L'-')) {
        wchar_t c = towlower(a1[1]);
        modeOut = c;
        size_t colon = a1.find(L':');
        if (colon != std::wstring::npos) {
            std::wstring num = a1.substr(colon + 1);
            if (!num.empty()) hwndOut = (HWND)_wcstoui64(num.c_str(), nullptr, 0);
        }
        else if (argc >= 3) {
            std::wstring a2 = argv[2];
            bool numeric = !a2.empty();
            for (wchar_t ch : a2) if (!iswdigit(ch)) { numeric = false; break; }
            if (numeric) hwndOut = (HWND)_wcstoui64(a2.c_str(), nullptr, 0);
        }
    }
}

// Initialize stars for a RenderWindow
static void InitStars(RenderWindow* rw) {
    RECT r = rw->rc;
    int w = max(1, r.right - r.left), h = max(1, r.bottom - r.top);
    rw->stars.clear();
    rw->stars.reserve(g_starCount);
    std::uniform_real_distribution<float> ux(0.0f, (float)w);
    std::uniform_real_distribution<float> uy(0.0f, (float)h);
    std::uniform_real_distribution<float> uz(0.2f, 1.0f);
    std::uniform_real_distribution<float> ub(0.6f, 1.0f);
    std::uniform_real_distribution<float> uph(0.0f, 6.28318530718f);
    for (int i = 0; i < g_starCount; ++i) {
        Star s{ ux(rw->rng), uy(rw->rng), uz(rw->rng), ub(rw->rng), uph(rw->rng) };
        rw->stars.push_back(s);
    }
}

// Create Direct2D render target for a RenderWindow
static HRESULT CreateRT(RenderWindow* rw) {
    if (!rw->factory) return E_FAIL;
    if (rw->rt) { rw->rt->Release(); rw->rt = nullptr; }
    D2D1_SIZE_U size = D2D1::SizeU(rw->rc.right - rw->rc.left, rw->rc.bottom - rw->rc.top);
    D2D1_HWND_RENDER_TARGET_PROPERTIES props = D2D1::HwndRenderTargetProperties(rw->hwnd, size);
    return rw->factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), props, &rw->rt);
}

// Render a frame for a RenderWindow
static void RenderFrame(RenderWindow* rw, float dt, float totalTime) {
    if (!rw->rt) return;
    if (!rw->brush) rw->rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &rw->brush);
    rw->rt->BeginDraw();
    rw->rt->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    D2D1_SIZE_F size = rw->rt->GetSize();
    float cx = size.width * 0.5f, cy = size.height * 0.5f;
    float speed = g_speedPercent / 100.0f;
    float twinkle = g_twinklePercent / 100.0f;
    for (auto& s : rw->stars) {
        s.z -= 0.5f * speed * dt;
        if (s.z <= 0.05f) {
            std::uniform_real_distribution<float> ux(0.0f, size.width);
            std::uniform_real_distribution<float> uy(0.0f, size.height);
            std::uniform_real_distribution<float> uz(0.5f, 1.0f);
            std::uniform_real_distribution<float> ub(0.6f, 1.0f);
            std::uniform_real_distribution<float> uph(0.0f, 6.28318530718f);
            s.x = ux(rw->rng); s.y = uy(rw->rng); s.z = uz(rw->rng); s.base = ub(rw->rng); s.phase = uph(rw->rng);
        }
        float px = (s.x - cx) / s.z + cx;
        float py = (s.y - cy) / s.z + cy;
        float psz = 2.5f / s.z; if (psz < 1.0f) psz = 1.0f;
        float tw = s.base + (sinf(s.phase + (float)totalTime * 5.0f) * 0.5f + 0.5f) * twinkle;
        float rr = GetRValue(g_color) / 255.0f * tw;
        float gg = GetGValue(g_color) / 255.0f * tw;
        float bb = GetBValue(g_color) / 255.0f * tw;
        rw->brush->SetColor(D2D1::ColorF(rr, gg, bb, 1.0f));
        D2D1_ELLIPSE ell = D2D1::Ellipse(D2D1::Point2F(px, py), psz, psz);
        rw->rt->FillEllipse(ell, rw->brush);
    }
    HRESULT hr = rw->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        if (rw->rt) { rw->rt->Release(); rw->rt = nullptr; }
        CreateRT(rw);
    }
}

// Fullscreen window proc
LRESULT CALLBACK FullWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RenderWindow* rw = (RenderWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: return 0;
    case WM_SIZE:
        if (rw) {
            GetClientRect(hWnd, &rw->rc);
            if (rw->rt) { rw->rt->Release(); rw->rt = nullptr; }
            CreateRT(rw);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_MOUSEMOVE: {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double seconds = double(now.QuadPart - g_startCounter.QuadPart) / double(g_perfFreq.QuadPart);
        if (seconds >= g_inputDebounceSeconds) {
            log("Input after debounce -> exiting");
            g_running = false;
            PostQuitMessage(0);
        }
        else {
            log("Ignored input during debounce");
        }
        return 0;
    }
    case WM_DESTROY:
        log("WM_DESTROY fullscreen window");
        return 0;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// Preview minimal proc for fallback painting
LRESULT CALLBACK PreviewProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hWnd, &ps);
        return 0;
    }
    default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// Enum monitors and create one fullscreen window per monitor
static BOOL CALLBACK MonEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
    RECT r = mi.rcMonitor;
    RenderWindow* rw = new RenderWindow();
    rw->rc = r;
    std::random_device rd; rw->rng.seed(rd());
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &rw->factory);
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = FullWndProc; wc.hInstance = g_hInst; wc.lpszClassName = L"MyStarfieldFullClass"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc); reg = true;
    }
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"MyStarfieldFullClass", L"MyStarfield", WS_POPUP | WS_VISIBLE,
        r.left, r.top, r.right - r.left, r.bottom - r.top, NULL, NULL, g_hInst, NULL);
    if (!hwnd) { if (rw->factory) rw->factory->Release(); delete rw; return TRUE; }
    rw->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)rw);
    ShowWindow(hwnd, SW_SHOW);
    GetClientRect(hwnd, &rw->rc);
    CreateRT(rw);
    InitStars(rw);
    g_windows.push_back(rw);
    log("Created fullscreen window for monitor");
    return TRUE;
}

// Run fullscreen multi-monitor loop
static void RunFull() {
    log("RunFull start");
    EnumDisplayMonitors(NULL, NULL, MonEnumProc, 0);
    QueryPerformanceFrequency(&g_perfFreq);
    QueryPerformanceCounter(&g_startCounter);
    LARGE_INTEGER last; QueryPerformanceCounter(&last);
    double total = 0.0;
    MSG msg;
    while (g_running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - last.QuadPart) / double(g_perfFreq.QuadPart);
        last = now; total += dt;
        for (auto rw : g_windows) RenderFrame(rw, (float)dt, (float)total);
        Sleep(1);
    }
    log("RunFull exiting cleanup");
    for (auto rw : g_windows) {
        if (rw->brush) rw->brush->Release();
        if (rw->rt) rw->rt->Release();
        if (rw->factory) rw->factory->Release();
        if (rw->hwnd) DestroyWindow(rw->hwnd);
        delete rw;
    }
    g_windows.clear();
    log("RunFull end");
}

// Settings dialog proc (resource-based)
INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemInt(hDlg, IDC_EDIT_STARS, g_starCount, FALSE);
        SetDlgItemInt(hDlg, IDC_EDIT_SPEED, g_speedPercent, FALSE);
        SetDlgItemInt(hDlg, IDC_EDIT_TWINKLE, g_twinklePercent, FALSE);
        SendDlgItemMessageW(hDlg, IDC_COMBO_COLOR, CB_ADDSTRING, 0, (LPARAM)L"Warm White");
        SendDlgItemMessageW(hDlg, IDC_COMBO_COLOR, CB_ADDSTRING, 0, (LPARAM)L"Cool White");
        SendDlgItemMessageW(hDlg, IDC_COMBO_COLOR, CB_ADDSTRING, 0, (LPARAM)L"Blue");
        SendDlgItemMessageW(hDlg, IDC_COMBO_COLOR, CB_ADDSTRING, 0, (LPARAM)L"Yellow");
        if (g_color == RGB(255, 255, 240)) SendDlgItemMessageW(hDlg, IDC_COMBO_COLOR, CB_SETCURSEL, 0, 0);
        else if (g_color == RGB(200, 200, 255)) SendDlgItemMessageW(hDlg, IDC_COMBO_COLOR, CB_SETCURSEL, 1, 0);
        else if (g_color == RGB(160, 180, 255)) SendDlgItemMessageW(hDlg, IDC_COMBO_COLOR, CB_SETCURSEL, 2, 0);
        else if (g_color == RGB(255, 240, 180)) SendDlgItemMessageW(hDlg, IDC_COMBO_COLOR, CB_SETCURSEL, 3, 0);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            BOOL ok;
            int stars = GetDlgItemInt(hDlg, IDC_EDIT_STARS, &ok, FALSE); if (!ok) stars = g_starCount;
            stars = max(10, min(5000, stars));
            int speed = GetDlgItemInt(hDlg, IDC_EDIT_SPEED, &ok, FALSE); if (!ok) speed = g_speedPercent;
            speed = max(10, min(300, speed));
            int tw = GetDlgItemInt(hDlg, IDC_EDIT_TWINKLE, &ok, FALSE); if (!ok) tw = g_twinklePercent;
            tw = max(0, min(100, tw));
            int sel = (int)SendDlgItemMessageW(hDlg, IDC_COMBO_COLOR, CB_GETCURSEL, 0, 0);
            COLORREF col = g_color;
    switch (sel) { case 0: col = RGB(255, 255, 240); break; case 1: col = RGB(200, 200, 255); break; case 2: col = RGB(160, 180, 255); break; case 3: col = RGB(255, 240, 180); break; }
                         g_starCount = stars; g_speedPercent = speed; g_twinklePercent = tw; g_color = col;
                         SaveSettings();
                         EndDialog(hDlg, IDOK);
                         return TRUE;
        }
        else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL); return TRUE;
        }
        else if (LOWORD(wParam) == IDC_BUTTON_COLORPICK) {
            CHOOSECOLORW cc = {}; static COLORREF cust[16];
            cc.lStructSize = sizeof(cc); cc.hwndOwner = hDlg; cc.lpCustColors = cust; cc.rgbResult = g_color; cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc)) { g_color = cc.rgbResult; }
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// Helper to judge small preview area
static bool IsRectTooSmall(RECT const& rc, int minW = 220, int minH = 140) {
    return (rc.right - rc.left) < minW || (rc.bottom - rc.top) < minH;
}

// Force-popup fallback that tries to run DialogBoxParam(NULL) and logs resource presence
int RunSettingsForcePopup()
{
    log("RunSettingsForcePopup: starting");

    // log module handle and check for dialog resource presence
    HMODULE hMod = GetModuleHandleW(NULL);
    char buf[256];
    sprintf_s(buf, "RunSettingsForcePopup: module handle = %p", hMod); log(buf);

    // Check resource presence using FindResource
    HRSRC rc = FindResourceW(hMod, MAKEINTRESOURCEW(IDD_SETTINGS), RT_DIALOG);
    if (rc) {
        sprintf_s(buf, "RunSettingsForcePopup: resource IDD_SETTINGS FOUND (HRSRC=%p)", rc); log(buf);
    }
    else {
        sprintf_s(buf, "RunSettingsForcePopup: resource IDD_SETTINGS NOT FOUND (GetLastError=%lu)", GetLastError()); log(buf);
    }

    // Allow our process to set foreground (best-effort)
    AllowSetForegroundWindow(ASFW_ANY);

    INT_PTR res = DialogBoxParamW(g_hInst, MAKEINTRESOURCE(IDD_SETTINGS), NULL, SettingsDlgProc, 0);
    if (res == -1) {
        log("RunSettingsForcePopup: DialogBoxParam(NULL) failed");
        return -1;
    }
    log("RunSettingsForcePopup: DialogBoxParam(NULL) returned OK");
    return (int)res;
}

// Robust settings launcher: try embedding when owner HWND is valid; otherwise show popup.
// If embedding fails we call RunSettingsForcePopup.
static int RunSettingsSmart(HWND ownerFromCmdline) {
    char buf[256];
    sprintf_s(buf, "RunSettingsSmart: owner=%p", ownerFromCmdline); log(buf);

    if (!IsWindow(ownerFromCmdline)) {
        log("RunSettingsSmart: no owner, using modal DialogBoxParam(NULL)");
        INT_PTR res = DialogBoxParamW(g_hInst, MAKEINTRESOURCE(IDD_SETTINGS), NULL, SettingsDlgProc, 0);
        sprintf_s(buf, "RunSettingsSmart: modal returned %d", (int)res); log(buf);
        return (int)res;
    }

    RECT ownerClient = {};
    if (!GetClientRect(ownerFromCmdline, &ownerClient)) {
        log("RunSettingsSmart: GetClientRect failed; fallback to force popup");
        return RunSettingsForcePopup();
    }

    if (IsRectTooSmall(ownerClient)) {
        log("RunSettingsSmart: owner too small; forcing popup");
        return RunSettingsForcePopup();
    }

    log("RunSettingsSmart: attempting embedded modeless dialog");
    HWND dlg = CreateDialogParamW(g_hInst, MAKEINTRESOURCE(IDD_SETTINGS), ownerFromCmdline, SettingsDlgProc, 0);
    if (!dlg) {
        log("RunSettingsSmart: CreateDialogParam failed; forcing popup");
        return RunSettingsForcePopup();
    }

    LONG_PTR style = GetWindowLongPtrW(dlg, GWL_STYLE);
    style &= ~WS_POPUP;
    style |= WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(dlg, GWL_STYLE, style);

    RECT dlgRc; GetWindowRect(dlg, &dlgRc);
    int ownerW = ownerClient.right - ownerClient.left;
    int ownerH = ownerClient.bottom - ownerClient.top;
    int dlgW = dlgRc.right - dlgRc.left;
    int dlgH = dlgRc.bottom - dlgRc.top;
    int x = max(0, (ownerW - dlgW) / 2);
    int y = max(0, (ownerH - dlgH) / 2);
    SetWindowPos(dlg, NULL, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_SHOWWINDOW);

    Sleep(60);

    RECT nowOwnerRc; GetClientRect(ownerFromCmdline, &nowOwnerRc);
    if (IsRectTooSmall(nowOwnerRc)) {
        log("RunSettingsSmart: owner still too small after embedding; destroying and forcing popup");
        DestroyWindow(dlg);
        return RunSettingsForcePopup();
    }

    log("RunSettingsSmart: entering message loop for embedded dialog");
    MSG msg;
    while (IsWindow(dlg) && IsWindow(ownerFromCmdline) && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    log("RunSettingsSmart: embedded dialog loop ended");
    return 0;
}

// Animated Direct2D preview inside supplied parent HWND
static int RunPreview(HWND parent) {
    if (!IsWindow(parent)) return 0;
    log("RunPreview start");
    WNDCLASSW wc = {}; wc.lpfnWndProc = PreviewProc; wc.hInstance = g_hInst; wc.lpszClassName = L"MyStarPre";
    RegisterClassW(&wc);
    RECT pr; GetClientRect(parent, &pr);
    HWND child = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, pr.right - pr.left, pr.bottom - pr.top, parent, NULL, g_hInst, NULL);
    if (!child) { UnregisterClassW(wc.lpszClassName, g_hInst); log("RunPreview: failed to create child"); return 0; }

    RenderWindow* rw = new RenderWindow();
    rw->hwnd = child; rw->isPreview = true; rw->rc = pr;
    std::random_device rd; rw->rng.seed(rd());
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &rw->factory);
    CreateRT(rw); InitStars(rw);

    QueryPerformanceFrequency(&g_perfFreq);
    LARGE_INTEGER last; QueryPerformanceCounter(&last);
    double total = 0.0;
    MSG msg;
    while (IsWindow(child)) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { DestroyWindow(child); break; }
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - last.QuadPart) / double(g_perfFreq.QuadPart);
        last = now; total += dt;
        RenderFrame(rw, (float)dt, (float)total);
        Sleep(15);
    }

    if (rw->brush) rw->brush->Release();
    if (rw->rt) rw->rt->Release();
    if (rw->factory) rw->factory->Release();
    DestroyWindow(child);
    UnregisterClassW(wc.lpszClassName, g_hInst);
    delete rw;
    log("RunPreview end");
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    g_hInst = hInstance;
    LoadSettings();

    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t mode = 0; HWND argH = NULL; ParseArgs(argc, argv, mode, argH);
    {
        char buf[256]; sprintf_s(buf, "Parsed args mode=%c hwnd=%p", mode ? (char)mode : '0', argH); log(buf);
    }

    if (mode == 'c') {
        log("wWinMain: entering settings smart");
        int r = RunSettingsSmart(argH);
        if (r == -1) {
            log("wWinMain: RunSettingsSmart returned -1; calling RunSettingsForcePopup");
            RunSettingsForcePopup();
        }
        log("wWinMain: settings branch finished");
        LocalFree(argv);
        return 0;
    }

    if (mode == 'p') {
        if (argH) {
            log("wWinMain: entering preview with provided parent");
            RunPreview(argH);
            log("wWinMain: preview returned");
            LocalFree(argv);
            return 0;
        }
        else {
            log("wWinMain: preview requested but no HWND; falling back to fullscreen");
        }
    }

    // Default: run fullscreen
    log("wWinMain: entering fullscreen screensaver");
    g_running = true;
    RunFull();
    log("wWinMain: fullscreen screensaver finished");
    LocalFree(argv);
    return 0;
}
