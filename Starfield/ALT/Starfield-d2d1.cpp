// Starfield.cpp
// Build as Windows GUI (/SUBSYSTEM:WINDOWS)
// Rename output .exe -> .scr to register with Windows Screensaver dialog.
// Link: d2d1.lib

#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>
#include <random>
#include <fstream>
#include <shellapi.h>

#pragma comment(lib, "d2d1.lib")

template <typename T>
T clamp(T val, T minVal, T maxVal) {
    return (val < minVal) ? minVal : (val > maxVal) ? maxVal : val;
}

// ---- Config / registry keys
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
static float g_starSizeMultiplier = 1.0f;
static float g_starBase = 1.0f;   // base numerator
static float g_starMin = 0.5f;
static float g_starMax = 8.0f;

// Logging helper
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

// ---- Starfield rendering structures
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

// Input filtering
static LARGE_INTEGER g_perfFreq;
static LARGE_INTEGER g_startCounter;
static double g_inputDebounceSeconds = 2.5;
static POINT g_startMouse = { 0,0 };
static bool g_startMouseInit = false;
static const int g_mouseMoveThreshold = 12; // pixels

// Simple arg parsing
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
// Forward declarations
LRESULT CALLBACK FullWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK PreviewProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SettingsWndProc(HWND, UINT, WPARAM, LPARAM);

// Direct2D helpers
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
    for (int i = 0; i < g_starCount; ++i) rw->stars.push_back({ ux(rw->rng), uy(rw->rng), uz(rw->rng), ub(rw->rng), uph(rw->rng) });
}
static HRESULT CreateRT(RenderWindow* rw) {
    if (!rw->factory) return E_FAIL;
    if (rw->rt) { rw->rt->Release(); rw->rt = nullptr; }
    D2D1_SIZE_U size = D2D1::SizeU(rw->rc.right - rw->rc.left, rw->rc.bottom - rw->rc.top);
    D2D1_HWND_RENDER_TARGET_PROPERTIES props = D2D1::HwndRenderTargetProperties(rw->hwnd, size);
    return rw->factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), props, &rw->rt);
}
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
        //float psz = 1.0f / s.z; if (psz < 1.0f) psz = 1.0f;
        float psz = (g_starBase / s.z) * (0.6f + 0.8f * s.base) * g_starSizeMultiplier;
        psz = clamp(psz, g_starMin, g_starMax);
        FLOAT dpiX = 96.0f, dpiY = 96.0f;
        rw->rt->GetDpi(&dpiX, &dpiY);
        float dpiScale = dpiX / 96.0f;
        psz *= dpiScale;

        float tw = s.base + (sinf(s.phase + (float)totalTime * 5.0f) * 0.5f + 0.5f) * twinkle;
        float rr = GetRValue(g_color) / 255.0f * tw;
        float gg = GetGValue(g_color) / 255.0f * tw;
        float bb = GetBValue(g_color) / 255.0f * tw;
        rw->brush->SetColor(D2D1::ColorF(rr, gg, bb, 1.0f));
        D2D1_ELLIPSE ell = D2D1::Ellipse(D2D1::Point2F(px, py), psz, psz);
        rw->rt->FillEllipse(ell, rw->brush);
    }
    HRESULT hr = rw->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { if (rw->rt) { rw->rt->Release(); rw->rt = nullptr; } CreateRT(rw); }
}

// Foreground check
static bool ForegroundIsOurWindow() {
    HWND fg = GetForegroundWindow(); if (!fg) return false;
    DWORD fgPid = 0; GetWindowThreadProcessId(fg, &fgPid); return (fgPid == GetCurrentProcessId());
}

// Fullscreen window proc (uses strict input filtering)
LRESULT CALLBACK FullWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RenderWindow* rw = (RenderWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: g_startMouseInit = false; return 0;
    case WM_SIZE:
        if (rw) { GetClientRect(hWnd, &rw->rc); if (rw->rt) { rw->rt->Release(); rw->rt = nullptr; } CreateRT(rw); g_startMouseInit = false; }
        return 0;
    case WM_KEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_MOUSEMOVE: {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double seconds = double(now.QuadPart - g_startCounter.QuadPart) / double(g_perfFreq.QuadPart);
        if (seconds < g_inputDebounceSeconds) { log("Ignored input during debounce"); return 0; }
        if (!ForegroundIsOurWindow()) { log("Ignored input because foreground window is not ours"); return 0; }
        if (msg == WM_MOUSEMOVE) {
            POINT cur; GetCursorPos(&cur);
            if (!g_startMouseInit) { g_startMouse = cur; g_startMouseInit = true; log("initialized start mouse pos"); return 0; }
            int dx = abs(cur.x - g_startMouse.x), dy = abs(cur.y - g_startMouse.y);
            if (dx < g_mouseMoveThreshold && dy < g_mouseMoveThreshold) { log("Ignored small mouse jitter"); return 0; }
        }
        log("Input considered deliberate -> exiting");
        g_running = false; PostQuitMessage(0);
        return 0;
    }
    case WM_DESTROY: log("WM_DESTROY fullscreen window"); return 0;
    default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// Preview proc
LRESULT CALLBACK PreviewProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps); FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH)); EndPaint(hWnd, &ps); return 0; }
    default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// Enum monitors -> create fullscreen windows
static BOOL CALLBACK MonEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
    RECT r = mi.rcMonitor;
    RenderWindow* rw = new RenderWindow();
    rw->rc = r; std::random_device rd; rw->rng.seed(rd());
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &rw->factory);
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = FullWndProc; wc.hInstance = g_hInst; wc.lpszClassName = L"MyStarfieldFullClass"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc); reg = true;
    }
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"MyStarfieldFullClass", L"MyStarfield", WS_POPUP | WS_VISIBLE,
        r.left, r.top, r.right - r.left, r.bottom - r.top, NULL, NULL, g_hInst, NULL);
    if (!hwnd) { if (rw->factory) rw->factory->Release(); delete rw; return TRUE; }
    rw->hwnd = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)rw); ShowWindow(hwnd, SW_SHOW);
    GetClientRect(hwnd, &rw->rc); CreateRT(rw); InitStars(rw); g_windows.push_back(rw);
    log("Created fullscreen window for monitor");
    return TRUE;
}

// Run fullscreen
static void RunFull() {
    log("RunFull start");
    EnumDisplayMonitors(NULL, NULL, MonEnumProc, 0);
    QueryPerformanceFrequency(&g_perfFreq);
    QueryPerformanceCounter(&g_startCounter);
    POINT p; GetCursorPos(&p); g_startMouse = p; g_startMouseInit = true;
    LARGE_INTEGER last; QueryPerformanceCounter(&last);
    double total = 0.0; MSG msg;
    while (g_running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) { g_running = false; break; } TranslateMessage(&msg); DispatchMessage(&msg); }
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

// ---------------- Settings dialog programmatic UI ----------------
// IDs
enum { CID_OK = 100, CID_CANCEL = 101, CID_EDIT_STARS = 110, CID_EDIT_SPEED = 111, CID_EDIT_TWINKLE = 112, CID_COMBO_COLOR = 113, CID_BUTTON_COLOR = 114, CID_PREVIEW = 115 };

// Create child controls on given window
static void CreateSettingsControls(HWND dlg) {
    CreateWindowExW(0, L"STATIC", L"Star count:", WS_CHILD | WS_VISIBLE, 10, 10, 80, 18, dlg, NULL, g_hInst, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_LEFT, 100, 8, 80, 20, dlg, (HMENU)CID_EDIT_STARS, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"Speed (%) :", WS_CHILD | WS_VISIBLE, 10, 40, 80, 18, dlg, NULL, g_hInst, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_LEFT, 100, 38, 80, 20, dlg, (HMENU)CID_EDIT_SPEED, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"Twinkle (%) :", WS_CHILD | WS_VISIBLE, 10, 70, 80, 18, dlg, NULL, g_hInst, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_LEFT, 100, 68, 80, 20, dlg, (HMENU)CID_EDIT_TWINKLE, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"Color preset:", WS_CHILD | WS_VISIBLE, 10, 100, 80, 18, dlg, NULL, g_hInst, NULL);
    HWND hCombo = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 100, 98, 140, 120, dlg, (HMENU)CID_COMBO_COLOR, g_hInst, NULL);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Warm White");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Cool White");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Blue");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Yellow");
    CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE, 80, 170, 80, 26, dlg, (HMENU)CID_OK, g_hInst, NULL);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 180, 170, 80, 26, dlg, (HMENU)CID_CANCEL, g_hInst, NULL);
}

// Settings window proc handles control actions and closes window
LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CreateSettingsControls(hWnd);
        SetDlgItemInt(hWnd, CID_EDIT_STARS, g_starCount, FALSE);
        SetDlgItemInt(hWnd, CID_EDIT_SPEED, g_speedPercent, FALSE);
        SetDlgItemInt(hWnd, CID_EDIT_TWINKLE, g_twinklePercent, FALSE);
        HWND hCombo = GetDlgItem(hWnd, CID_COMBO_COLOR);
        int sel = 0;
        if (g_color == RGB(255, 255, 240)) sel = 0;
        else if (g_color == RGB(200, 200, 255)) sel = 1;
        else if (g_color == RGB(160, 180, 255)) sel = 2;
        else if (g_color == RGB(255, 240, 180)) sel = 3;
        SendMessageW(hCombo, CB_SETCURSEL, sel, 0);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == CID_OK) {
            BOOL ok;
            int stars = GetDlgItemInt(hWnd, CID_EDIT_STARS, &ok, FALSE); if (!ok) stars = g_starCount;
            stars = max(10, min(5000, stars));
            int speed = GetDlgItemInt(hWnd, CID_EDIT_SPEED, &ok, FALSE); if (!ok) speed = g_speedPercent;
            speed = max(10, min(300, speed));
            int tw = GetDlgItemInt(hWnd, CID_EDIT_TWINKLE, &ok, FALSE); if (!ok) tw = g_twinklePercent;
            tw = max(0, min(100, tw));
            HWND hCombo = GetDlgItem(hWnd, CID_COMBO_COLOR);
            int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
            COLORREF col = g_color;
    switch (sel) { case 0: col = RGB(255, 255, 240); break; case 1: col = RGB(200, 200, 255); break; case 2: col = RGB(160, 180, 255); break; case 3: col = RGB(255, 240, 180); break; }
                         g_starCount = stars; g_speedPercent = speed; g_twinklePercent = tw; g_color = col;
                         SaveSettings();
                         DestroyWindow(hWnd);
                         return 0;
        }
        else if (id == CID_CANCEL) {
            DestroyWindow(hWnd);
            return 0;
        }
        else if (id == CID_BUTTON_COLOR) {
            CHOOSECOLORW cc = {}; static COLORREF cust[16];
            cc.lStructSize = sizeof(cc); cc.hwndOwner = hWnd; cc.lpCustColors = cust; cc.rgbResult = g_color; cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc)) { g_color = cc.rgbResult; }
            return 0;
        }
        break;
    }
    case WM_DESTROY:
        // If used in modal loop, breaking GetMessage happens because window is gone; do not PostQuitMessage here.
        return 0;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// Register settings class once
static void EnsureSettingsClassRegistered() {
    static bool reg = false;
    if (reg) return;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"MyStarfieldSettingsClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    reg = true;
}

// Show modal popup (centered) and block until closed
static int ShowSettingsModalPopup() {
    EnsureSettingsClassRegistered();
    int w = 360, h = 220;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - w) / 2, y = (sh - h) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MyStarfieldSettingsClass", L"Starfield Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, w, h,
        NULL, NULL, g_hInst, NULL);
    if (!dlg) { log("ShowSettingsModalPopup: CreateWindowEx failed"); return -1; }
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    // Modal message loop: run until dlg destroyed
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    log("ShowSettingsModalPopup: dialog closed");
    return 0;
}

// Simple preview runner
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
    double total = 0.0; MSG msg;
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

// Entry point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    g_hInst = hInstance;
    LoadSettings();

    // log path for verification
    wchar_t modPath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, modPath, MAX_PATH);
    char pathLog[512]; sprintf_s(pathLog, "Running from: %ws", modPath); log(pathLog);

    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t mode = 0; HWND argH = NULL; ParseArgs(argc, argv, mode, argH);
    { char buf[256]; sprintf_s(buf, "Parsed args mode=%c hwnd=%p", mode ? (char)mode : '0', argH); log(buf); }

    if (mode == 'c') {
        log("wWinMain: entering settings (modal popup forced)");
        ShowSettingsModalPopup();
        log("wWinMain: settings branch finished");
        LocalFree(argv); return 0;
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

    // Default: fullscreen
    log("wWinMain: entering fullscreen screensaver");
    g_running = true;
    // Enumerate monitors and run full-screen animation
    RunFull();
    log("wWinMain: fullscreen screensaver finished");
    LocalFree(argv);
    return 0;
}



