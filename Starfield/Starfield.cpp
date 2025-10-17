// Starfield.cpp
// Build as Windows GUI (/SUBSYSTEM:WINDOWS)
// Copy generated Starfield.scr in Debug directory to C:\Windows\System32

#include <windows.h>
#include <string>
#include <vector>
#include <random>
#include <fstream>
#include <shellapi.h>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")

// ---- Config / registry keys
static LPCWSTR REG_KEY = L"Software\\StarfieldScreensaver";
static LPCWSTR REG_STARS = L"StarCount";
static LPCWSTR REG_SPEED = L"SpeedPercent";

// Defaults
static int g_starCount = 600;
static int g_speed = 60;

static COLORREF g_color = RGB(255, 255, 255);

// Registry helpers
static int GetRegDWORD(LPCWSTR name, int def)
{
    HKEY hKey; DWORD val = def; DWORD size = sizeof(val);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        RegQueryValueExW(hKey, name, NULL, NULL, (LPBYTE)&val, &size);
        RegCloseKey(hKey);
    }
    return (int)val;
}
static void SetRegDWORD(LPCWSTR name, DWORD v)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}
static void LoadSettings()
{
    g_starCount = GetRegDWORD(REG_STARS, g_starCount);
    g_speed = GetRegDWORD(REG_SPEED, g_speed);
}
static void SaveSettings()
{
    SetRegDWORD(REG_STARS, (DWORD)g_starCount);
    SetRegDWORD(REG_SPEED, (DWORD)g_speed);
}

// Star model
struct Star
{
    float x;     // world X (centered)
    float y;     // world Y (centered)
    float z;     // depth
    float speed; // per-star speed (depth units / second or arbitrary units)
};

// RenderWindow
struct RenderWindow
{
    HWND hwnd = NULL;
    HDC backHdc = NULL;
    HBITMAP backBmp = NULL;
    HBITMAP oldBackBmp = NULL;
    RECT rc = {};
    std::vector<Star> stars;
    std::mt19937 rng;
    bool isPreview = false;
};

// Globals
static HINSTANCE g_hInst = NULL;
static std::vector<RenderWindow*> g_windows;
static bool g_running = true;

// Input filtering
static LARGE_INTEGER g_perfFreq;
static LARGE_INTEGER g_startCounter;
static double g_inputDebounceSeconds = 0.66; // mouse movement speed to stop screensaver from running
static POINT g_startMouse = { 0,0 };
static bool g_startMouseInit = false;
static const int g_mouseMoveThreshold = 12; // pixels

// Simple arg parsing
static void ParseArgs(int argc, wchar_t** argv, wchar_t& modeOut, HWND& hwndOut)
{
    modeOut = 0; hwndOut = NULL;
    if (argc <= 1) return;
    std::wstring a1 = argv[1];
    if (a1.size() >= 2 && (a1[0] == L'/' || a1[0] == L'-'))
    {
        wchar_t c = towlower(a1[1]);
        modeOut = c;
        size_t colon = a1.find(L':');
        if (colon != std::wstring::npos)
        {
            std::wstring num = a1.substr(colon + 1);
            if (!num.empty()) hwndOut = (HWND)_wcstoui64(num.c_str(), nullptr, 0);
        }
        else if (argc >= 3)
        {
            std::wstring a2 = argv[2];
            bool numeric = !a2.empty();
            for (wchar_t ch : a2) if (!iswdigit(ch)) { numeric = false; break; }
            if (numeric) hwndOut = (HWND)_wcstoui64(a2.c_str(), nullptr, 0);
        }
    }
}

// ---- backbuffer helpers
static bool CreateBackbuffer(RenderWindow* rw)
{
    if (!rw || !rw->hwnd) return false;
    HDC wnd = GetDC(rw->hwnd);
    if (!wnd) return false;
    // release existing
    if (rw->backHdc)
    {
        SelectObject(rw->backHdc, rw->oldBackBmp);
        DeleteObject(rw->backBmp);
        DeleteDC(rw->backHdc);
        rw->backHdc = NULL; rw->backBmp = NULL; rw->oldBackBmp = NULL;
    }
    int w = max(1, rw->rc.right - rw->rc.left);
    int h = max(1, rw->rc.bottom - rw->rc.top);
    HDC mem = CreateCompatibleDC(wnd);
    HBITMAP bmp = CreateCompatibleBitmap(wnd, w, h);
    if (!mem || !bmp)
    {
        if (mem) DeleteDC(mem);
        ReleaseDC(rw->hwnd, wnd);
        return false;
    }
    rw->oldBackBmp = (HBITMAP)SelectObject(mem, bmp);
    rw->backHdc = mem;
    rw->backBmp = bmp;
    // init black background
    HBRUSH b = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT rc = { 0,0,w,h };
    FillRect(rw->backHdc, &rc, b);
    ReleaseDC(rw->hwnd, wnd);
    return true;
}

static void DestroyBackbuffer(RenderWindow* rw)
{
    if (!rw) return;
    if (rw->backHdc)
    {
        SelectObject(rw->backHdc, rw->oldBackBmp);
        DeleteObject(rw->backBmp);
        DeleteDC(rw->backHdc);
        rw->backHdc = NULL; rw->backBmp = NULL; rw->oldBackBmp = NULL;
    }
}

// InitStars: centered world coords (so projection works predictably)
// Smaller Z_MIN(closer to 0) makes stars appear larger and move faster
// as they approach because projection uses 1 / z.
static const float Z_MIN = 2.0f;
// decrease Z_MAX (e.g., 800) to bring more stars visually forward
static const float Z_MAX = 33.0f;
// FOCAL ~ 1.0 is appropriate for your sample values (x ~ [-1600..1600], z ~ [10..100])
static const float FOCAL = 9.0f;
// multiplier that controls drawn core size; increase for larger stars
static const float SIZE_SCALE = 1.0f;
static void InitStars(RenderWindow* rw)
{
    if (!rw) return;
    RECT r = rw->rc;
    int width = max(1, r.right - r.left);
    int height = max(1, r.bottom - r.top);
    rw->stars.clear();
    rw->stars.resize(g_starCount);
    int jitterMax = max(1, g_speed / 2 + 1);
    std::uniform_real_distribution<float> ud01(0.0f, 1.0f);

    for (int i = 0; i < g_starCount; ++i)
    {
        float fx = ud01(rw->rng);
        float fy = ud01(rw->rng);
        float fz = ud01(rw->rng);

        // centered world coords as in your samples
        rw->stars[i].x = (fx - 0.5f) * (float)width * 2.0f;
        rw->stars[i].y = (fy - 0.5f) * (float)height * 2.0f;

        // deep range (classic)
        rw->stars[i].z = fz * (Z_MAX - Z_MIN) + Z_MIN;
        rw->stars[i].speed = (float)g_speed + float(rw->rng() % jitterMax);
    }
}

// RenderFrame tuned to the sample values (uses totalTime)
static void RenderFrame(RenderWindow* rw, float dt, float totalTime)
{
    if (!rw || !rw->backHdc) return;
    int w = max(1, rw->rc.right - rw->rc.left);
    int h = max(1, rw->rc.bottom - rw->rc.top);
    float cx = w * 0.5f, cy = h * 0.5f;

    // clear
    RECT fill = { 0,0,w,h };
    FillRect(rw->backHdc, &fill, (HBRUSH)GetStockObject(BLACK_BRUSH));

    HPEN oldPen = (HPEN)SelectObject(rw->backHdc, GetStockObject(NULL_PEN));
    int baseR = GetRValue(g_color), baseG = GetGValue(g_color), baseB = GetBValue(g_color);

    // subtle pulse
    float pulse = 1.0f + 0.05f * sinf(totalTime * 1.5f);

    // small brush cache (few buckets)
    const int BUCKETS = 6;
    HBRUSH brushes[BUCKETS] = { 0 };

    for (auto& s : rw->stars)
    {
        // advance depth
        s.z -= s.speed * dt * 0.5f;
        if (s.z <= Z_MIN) {
            // respawn centered and far
            std::uniform_real_distribution<float> ud01(0.0f, 1.0f);
            float fx = ud01(rw->rng), fy = ud01(rw->rng), fz = ud01(rw->rng);
            s.x = (fx - 0.5f) * (float)w * 2.0f;
            s.y = (fy - 0.5f) * (float)h * 2.0f;
            s.z = fz * (Z_MAX - Z_MIN) + Z_MIN;
            int jitterMax = max(1, g_speed / 2 + 1);
            s.speed = (float)g_speed + float(rw->rng() % jitterMax);
        }

        // projection using small focal factor
        float px = cx + s.x * (FOCAL / s.z);
        float py = cy + s.y * (FOCAL / s.z);

        // size scales with inverse depth; near -> larger
        float inv = (Z_MIN / s.z); // near => closer to 1
        int psz = (int)ceilf(max(1.0f, SIZE_SCALE * inv));
        if (psz > 128) psz = 128;

        // intensity from depth (near -> brighter) then pulsate
        float t = (s.z - Z_MIN) / (Z_MAX - Z_MIN); // 0..1
        float depthIntensity = 1.0f - t;
        int intensity = (int)lroundf(100.0f + depthIntensity * 155.0f * pulse);
        intensity = max(0, min(255, intensity));

        // map to bucket
        int bucket = (int)((intensity) / (256.0f / BUCKETS));
        bucket = max(0, min(BUCKETS - 1, bucket));
        if (!brushes[bucket])
        {
            int br = (baseR * intensity) / 255;
            int bg = (baseG * intensity) / 255;
            int bb = (baseB * intensity) / 255;
            // slightly move nearer buckets toward white for pop
            float whiten = 0.5f + 0.5f * (bucket / (float)(BUCKETS - 1));
            br = min(255, (int)lroundf(br * whiten + 255 * (1.0f - whiten)));
            bg = min(255, (int)lroundf(bg * whiten + 255 * (1.0f - whiten)));
            bb = min(255, (int)lroundf(bb * whiten + 255 * (1.0f - whiten)));
            brushes[bucket] = CreateSolidBrush(RGB(br, bg, bb));
        }
        // skip if offscreen
        if (px + psz < 0 || px - psz > w || py + psz < 0 || py - psz > h) continue;
        HBRUSH oldBrush = (HBRUSH)SelectObject(rw->backHdc, brushes[bucket]);
        Ellipse(rw->backHdc,
            (int)floorf(px - psz), (int)floorf(py - psz),
            (int)ceilf(px + psz + 1), (int)ceilf(py + psz + 1));
        SelectObject(rw->backHdc, oldBrush);
    }
    // cleanup
    for (int i = 0; i < BUCKETS; ++i) if (brushes[i])
    {
        DeleteObject(brushes[i]); brushes[i] = NULL;
    }
    SelectObject(rw->backHdc, oldPen);
    // blit
    HDC wnd = GetDC(rw->hwnd);
    BitBlt(wnd, 0, 0, w, h, rw->backHdc, 0, 0, SRCCOPY);
    ReleaseDC(rw->hwnd, wnd);
}

// ---- Foreground check and window procs
static bool ForegroundIsOurWindow()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return (fgPid == GetCurrentProcessId());
}

// Fullscreen window proc (uses input filtering)
LRESULT CALLBACK FullWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    RenderWindow* rw = (RenderWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (msg)
    {
        case WM_CREATE:
            g_startMouseInit = false;
            return 0;
        case WM_SIZE:
            if (rw)
            {
                GetClientRect(hWnd, &rw->rc);
                DestroyBackbuffer(rw);
                CreateBackbuffer(rw);
                g_startMouseInit = false;
            }
            return 0;
        case WM_KEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_MOUSEMOVE:
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double seconds = double(now.QuadPart - g_startCounter.QuadPart) / double(g_perfFreq.QuadPart);
            if (seconds < g_inputDebounceSeconds) { return 0; }
            if (!ForegroundIsOurWindow()) { return 0; }
            if (msg == WM_MOUSEMOVE)
            {
                POINT cur; GetCursorPos(&cur);
                if (!g_startMouseInit)
                {
                    g_startMouse = cur;
                    g_startMouseInit = true;
                    return 0;
                }
                int dx = abs(cur.x - g_startMouse.x), dy = abs(cur.y - g_startMouse.y);
                if (dx < g_mouseMoveThreshold && dy < g_mouseMoveThreshold) { return 0; }
            }
            g_running = false; PostQuitMessage(0);
            return 0;
        }
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// Preview proc
LRESULT CALLBACK PreviewProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
            EndPaint(hWnd, &ps);
            return 0;
        }
        default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// Monitor enumeration -> create fullscreen windows
static BOOL CALLBACK MonEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
    RECT r = mi.rcMonitor;
    RenderWindow* rw = new RenderWindow();
    rw->rc = r;
    std::random_device rd;
    rw->rng.seed(rd());
    static bool reg = false;
    if (!reg)
    {
        WNDCLASSW wc = {}; wc.lpfnWndProc = FullWndProc; wc.hInstance = g_hInst; wc.lpszClassName = L"StarfieldFullClass"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc); reg = true;
    }
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"StarfieldFullClass", L"Starfield", WS_POPUP | WS_VISIBLE,
        r.left, r.top, r.right - r.left, r.bottom - r.top, NULL, NULL, g_hInst, NULL);
    if (!hwnd)
    {
        delete rw;
        return TRUE;
    }
    rw->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)rw);
    ShowWindow(hwnd, SW_SHOW);
    GetClientRect(hwnd, &rw->rc);
    CreateBackbuffer(rw);
    InitStars(rw);
    g_windows.push_back(rw);
    return TRUE;
}

// Run fullscreen
static void RunFull()
{
    EnumDisplayMonitors(NULL, NULL, MonEnumProc, 0);
    QueryPerformanceFrequency(&g_perfFreq);
    QueryPerformanceCounter(&g_startCounter);
    POINT p;
    GetCursorPos(&p);
    g_startMouse = p;
    g_startMouseInit = true;
    LARGE_INTEGER last;
    QueryPerformanceCounter(&last);
    double total = 0.0;
    MSG msg;
    while (g_running)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - last.QuadPart) / double(g_perfFreq.QuadPart);
        last = now; total += dt;
        for (auto rw : g_windows) RenderFrame(rw, (float)dt, (float)total);
        Sleep(8);
    }
    for (auto rw : g_windows)
    {
        DestroyBackbuffer(rw);
        if (rw->hwnd) DestroyWindow(rw->hwnd);
        delete rw;
    }
    g_windows.clear();
}

// ---------------- Settings dialog programmatic UI ----------------
// IDs
enum { CID_OK = 100, CID_CANCEL = 101, CID_EDIT_STARS = 110, CID_EDIT_SPEED = 111, CID_PREVIEW = 112 };

// Create child controls on given window
static void CreateSettingsControls(HWND dlg)
{
    CreateWindowExW(0, L"STATIC", L"Star count:", WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 10, 80, 18, dlg, NULL, g_hInst, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT, 100, 8, 80, 20, dlg, (HMENU)CID_EDIT_STARS, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"Speed:", WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 40, 80, 18, dlg, NULL, g_hInst, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT, 100, 38, 80, 20, dlg, (HMENU)CID_EDIT_SPEED, g_hInst, NULL);
    CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 80, 70, 80, 26, dlg, (HMENU)CID_OK, g_hInst, NULL);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 168, 70, 80, 26, dlg, (HMENU)CID_CANCEL, g_hInst, NULL);
}

// Settings window proc handles control actions and closes window
LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            CreateSettingsControls(hWnd);
            SetDlgItemInt(hWnd, CID_EDIT_STARS, g_starCount, FALSE);
            SetDlgItemInt(hWnd, CID_EDIT_SPEED, g_speed, FALSE);
            return 0;
        }
        case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            if (id == CID_OK)
            {
                BOOL ok;
                int stars = GetDlgItemInt(hWnd, CID_EDIT_STARS, &ok, FALSE); if (!ok) stars = g_starCount;
                stars = std::fmax(10, std::fmin(5000, stars));
                int speed = GetDlgItemInt(hWnd, CID_EDIT_SPEED, &ok, FALSE); if (!ok) speed = g_speed;
                speed = std::fmax(10, std::fmin(300, speed));
                g_starCount = stars; g_speed = speed;
                SaveSettings();
                DestroyWindow(hWnd);
                return 0;
            }
            else if (id == CID_CANCEL)
            {
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        }
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// Register settings class once
static void EnsureSettingsClassRegistered()
{
    static bool reg = false;
    if (reg) return;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"StarfieldSettingsClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    reg = true;
}

// Show modal popup (centered) and block until closed
static int ShowSettingsModalPopup()
{
    EnsureSettingsClassRegistered();
    int w = 360, h = 144;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - w) / 2, y = (sh - h) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"StarfieldSettingsClass", L"Starfield Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, w, h,
        NULL, NULL, g_hInst, NULL);
    if (!dlg) { return -1; }
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    // Modal message loop: run until dlg destroyed
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// Simple preview runner
static int RunPreview(HWND parent)
{
    if (!IsWindow(parent)) return 0;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = PreviewProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"MyStarPre";
    RegisterClassW(&wc);
    RECT pr; GetClientRect(parent, &pr);
    HWND child = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, pr.right - pr.left, pr.bottom - pr.top, parent, NULL, g_hInst, NULL);
    if (!child)
    {
        UnregisterClassW(wc.lpszClassName, g_hInst);
        return 0;
    }
    RenderWindow* rw = new RenderWindow();
    rw->hwnd = child; rw->isPreview = true; rw->rc = pr;
    std::random_device rd; rw->rng.seed(rd());
    CreateBackbuffer(rw);
    InitStars(rw);
    QueryPerformanceFrequency(&g_perfFreq);
    LARGE_INTEGER last;
    QueryPerformanceCounter(&last);
    double total = 0.0; MSG msg;
    while (IsWindow(child)) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { DestroyWindow(child); break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - last.QuadPart) / double(g_perfFreq.QuadPart);
        last = now; total += dt;
        RenderFrame(rw, (float)dt, (float)total);
        Sleep(15);
    }
    DestroyBackbuffer(rw);
    DestroyWindow(child);
    UnregisterClassW(wc.lpszClassName, g_hInst);
    delete rw;
    return 0;
}

// Entry point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    g_hInst = hInstance;
    LoadSettings();
    // log path for verification
    wchar_t modPath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, modPath, MAX_PATH);
    char pathLog[512];
    sprintf_s(pathLog, "Running from: %ws", modPath);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t mode = 0;
    HWND argH = NULL;
    ParseArgs(argc, argv, mode, argH);
    {
        char buf[256];
        sprintf_s(buf, "Parsed args mode=%c hwnd=%p", mode ? (char)mode : '0', argH);
    }
    if (mode == 'c')
    {
        ShowSettingsModalPopup();
        LocalFree(argv); return 0;
    }
    if (mode == 'p')
    {
        if (argH)
        {
            RunPreview(argH);
            LocalFree(argv); return 0;
        }
    }
    // Default: fullscreen
    g_running = true;
    RunFull();
    LocalFree(argv);
    return 0;
}