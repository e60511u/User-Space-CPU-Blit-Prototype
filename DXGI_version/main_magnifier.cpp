// Windows Desktop Capture using Magnification API
// The Magnification API renders above EVERYTHING including cursor, taskbar, and Start menu
// Captures 1920x1080 from FIRST monitor, scales down to 1440x1080, displays with black padding

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <magnification.h>
#include <mmsystem.h>
#include <stdio.h>

#pragma comment(lib, "magnification.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")

// DPI awareness
typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
#define PROCESS_PER_MONITOR_DPI_AWARE 2

void EnableDPIAwareness()
{
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32)
    {
        auto pSetProcessDpiAwarenessContext = (PFN_SetProcessDpiAwarenessContext)
            GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (pSetProcessDpiAwarenessContext)
        {
            if (pSetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                return;
        }
    }
    
    HMODULE hShcore = LoadLibraryA("shcore.dll");
    if (hShcore)
    {
        auto pSetProcessDpiAwareness = (PFN_SetProcessDpiAwareness)
            GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (pSetProcessDpiAwareness)
        {
            if (SUCCEEDED(pSetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE)))
            {
                FreeLibrary(hShcore);
                return;
            }
        }
        FreeLibrary(hShcore);
    }
    
    if (hUser32)
    {
        auto pSetProcessDPIAware = (PFN_SetProcessDPIAware)
            GetProcAddress(hUser32, "SetProcessDPIAware");
        if (pSetProcessDPIAware)
        {
            pSetProcessDPIAware();
        }
    }
}

// Constants
constexpr int SOURCE_WIDTH = 1920;
constexpr int SOURCE_HEIGHT = 1080;
constexpr int RENDER_WIDTH = 1440;
constexpr int RENDER_HEIGHT = 1080;
constexpr int OUTPUT_WIDTH = 1920;
constexpr int OUTPUT_HEIGHT = 1080;
constexpr int TARGET_FPS = 60;
constexpr int FRAME_TIME_MS = 1000 / TARGET_FPS;
constexpr int FIRST_MONITOR_X = 0;
constexpr int FIRST_MONITOR_Y = 0;

// Global state
static bool g_Running = true;
static HWND g_hHostWnd = nullptr;      // Host window (magnifier container)
static HWND g_hMagWnd = nullptr;       // Magnifier control window
static HWND g_hBlackWnd = nullptr;     // Black padding window on the right

// Window class name for magnifier host
const wchar_t* MAGNIFIER_HOST_CLASS = L"MagnifierHostClass";
const wchar_t* BLACK_WINDOW_CLASS = L"BlackPaddingClass";

LRESULT CALLBACK HostWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK BlackWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            EndPaint(hWnd, &ps);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool InitMagnifier(HINSTANCE hInstance)
{
    // Initialize the Magnification API
    if (!MagInitialize())
    {
        MessageBoxA(nullptr, "Failed to initialize Magnification API", "Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Register host window class
    WNDCLASSEXW wcHost = {};
    wcHost.cbSize = sizeof(WNDCLASSEXW);
    wcHost.style = 0;
    wcHost.lpfnWndProc = HostWndProc;
    wcHost.hInstance = hInstance;
    wcHost.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcHost.hbrBackground = nullptr;
    wcHost.lpszClassName = MAGNIFIER_HOST_CLASS;
    
    if (!RegisterClassExW(&wcHost))
    {
        char buf[256];
        sprintf(buf, "Failed to register host window class. Error: %lu", GetLastError());
        MessageBoxA(nullptr, buf, "Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Register black padding window class
    WNDCLASSEXW wcBlack = {};
    wcBlack.cbSize = sizeof(WNDCLASSEXW);
    wcBlack.style = 0;
    wcBlack.lpfnWndProc = BlackWndProc;
    wcBlack.hInstance = hInstance;
    wcBlack.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcBlack.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcBlack.lpszClassName = BLACK_WINDOW_CLASS;
    
    if (!RegisterClassExW(&wcBlack))
        return false;

    // Create the host window for the magnifier (covers left 1440 pixels)
    // WS_EX_TOPMOST alone isn't enough, but the magnifier control will be above everything
    g_hHostWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        MAGNIFIER_HOST_CLASS,
        L"Magnifier Host",
        WS_POPUP,
        FIRST_MONITOR_X, FIRST_MONITOR_Y,
        RENDER_WIDTH, OUTPUT_HEIGHT,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_hHostWnd)
    {
        char buf[256];
        sprintf(buf, "Failed to create host window. Error: %lu", GetLastError());
        MessageBoxA(nullptr, buf, "Error", MB_OK | MB_ICONERROR);
        return false;
    }

    SetLayeredWindowAttributes(g_hHostWnd, 0, 255, LWA_ALPHA);

    // Create the magnifier control window as a child of the host
    // The magnifier window class is "Magnifier" - this is the magic window that renders above everything
    g_hMagWnd = CreateWindowW(
        L"Magnifier",  // Magnifier window class
        L"Magnifier",
        WS_CHILD | WS_VISIBLE | MS_SHOWMAGNIFIEDCURSOR,
        0, 0,
        RENDER_WIDTH, OUTPUT_HEIGHT,
        g_hHostWnd, nullptr, hInstance, nullptr
    );

    if (!g_hMagWnd)
    {
        MessageBoxA(nullptr, "Failed to create magnifier window", "Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Create black padding window on the right (480 pixels)
    g_hBlackWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        BLACK_WINDOW_CLASS,
        L"Black Padding",
        WS_POPUP | WS_VISIBLE,
        FIRST_MONITOR_X + RENDER_WIDTH, FIRST_MONITOR_Y,
        OUTPUT_WIDTH - RENDER_WIDTH, OUTPUT_HEIGHT,
        nullptr, nullptr, hInstance, nullptr
    );

    if (g_hBlackWnd)
    {
        SetLayeredWindowAttributes(g_hBlackWnd, 0, 255, LWA_ALPHA);
    }

    // Set up the magnification transformation
    // We want to scale 1920 -> 1440, so factor = 1440/1920 = 0.75
    MAGTRANSFORM transform;
    memset(&transform, 0, sizeof(transform));
    
    // Magnification matrix (3x3)
    // Scale factors on diagonal
    float scaleX = (float)RENDER_WIDTH / (float)SOURCE_WIDTH;  // 0.75
    float scaleY = (float)RENDER_HEIGHT / (float)SOURCE_HEIGHT; // 1.0
    
    transform.v[0][0] = scaleX;
    transform.v[1][1] = scaleY;
    transform.v[2][2] = 1.0f;

    if (!MagSetWindowTransform(g_hMagWnd, &transform))
    {
        // Transform not supported, use default (1:1)
    }

    return true;
}

void UpdateMagnifier()
{
    // Set the source rectangle - the area of the screen to magnify
    // We capture the full first monitor (1920x1080)
    RECT sourceRect;
    sourceRect.left = FIRST_MONITOR_X;
    sourceRect.top = FIRST_MONITOR_Y;
    sourceRect.right = FIRST_MONITOR_X + SOURCE_WIDTH;
    sourceRect.bottom = FIRST_MONITOR_Y + SOURCE_HEIGHT;

    // Update the magnifier source
    MagSetWindowSource(g_hMagWnd, sourceRect);

    // Force redraw
    InvalidateRect(g_hMagWnd, nullptr, FALSE);
}

void Cleanup()
{
    if (g_hMagWnd)
    {
        DestroyWindow(g_hMagWnd);
        g_hMagWnd = nullptr;
    }
    
    if (g_hHostWnd)
    {
        DestroyWindow(g_hHostWnd);
        g_hHostWnd = nullptr;
    }

    if (g_hBlackWnd)
    {
        DestroyWindow(g_hBlackWnd);
        g_hBlackWnd = nullptr;
    }

    MagUninitialize();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    EnableDPIAwareness();

    if (!InitMagnifier(hInstance))
    {
        Cleanup();
        return 1;
    }

    // Register Insert key as global hotkey to exit
    if (!RegisterHotKey(g_hHostWnd, 1, 0, VK_INSERT))
    {
        MessageBoxA(nullptr, "Failed to register hotkey (Insert).", "Warning", MB_OK | MB_ICONWARNING);
    }

    timeBeginPeriod(1);

    ShowWindow(g_hHostWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hHostWnd);

    LARGE_INTEGER frequency, lastTime, currentTime;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&lastTime);

    MSG msg = {};
    while (g_Running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_Running = false;
                break;
            }
            if (msg.message == WM_HOTKEY && msg.wParam == 1)
            {
                g_Running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (g_Running)
        {
            UpdateMagnifier();

            // Keep windows on top
            SetWindowPos(g_hHostWnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (g_hBlackWnd)
            {
                SetWindowPos(g_hBlackWnd, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }

            QueryPerformanceCounter(&currentTime);
            double elapsedMs = (double)(currentTime.QuadPart - lastTime.QuadPart) * 1000.0 / frequency.QuadPart;
            
            if (elapsedMs < FRAME_TIME_MS)
            {
                Sleep((DWORD)(FRAME_TIME_MS - elapsedMs));
            }
            
            QueryPerformanceCounter(&lastTime);
        }
    }

    UnregisterHotKey(g_hHostWnd, 1);
    timeEndPeriod(1);
    Cleanup();
    
    return 0;
}
