// Windows Desktop Capture with GDI (MinGW Compatible)
// Captures 1920x1080 from FIRST monitor, scales down to 1440x1080, composites into 1920x1080 buffer with black padding on right
// Displays in fullscreen borderless window on FIRST monitor at 60 Hz
// Uses SetWindowDisplayAffinity to exclude self from capture (Windows 10 2004+)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>  // For timeBeginPeriod/timeEndPeriod
#include <stdio.h>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

// DPI awareness - must be set before any window/GDI calls
// Function pointer types for DPI functions (not in older MinGW headers)
typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);

// DPI awareness constants
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
#define PROCESS_PER_MONITOR_DPI_AWARE 2

void EnableDPIAwareness()
{
    // Try SetProcessDpiAwarenessContext first (Windows 10 1703+)
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
    
    // Try SetProcessDpiAwareness (Windows 8.1+)
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
    
    // Fall back to SetProcessDPIAware (Windows Vista+)
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
constexpr int SOURCE_WIDTH = 1920;   // Full first monitor width to capture
constexpr int SOURCE_HEIGHT = 1080;  // Full first monitor height to capture
constexpr int RENDER_WIDTH = 1440;   // Scaled-down width for display
constexpr int RENDER_HEIGHT = 1080;  // Height stays the same
constexpr int OUTPUT_WIDTH = 1920;
constexpr int OUTPUT_HEIGHT = 1080;
constexpr int TARGET_FPS = 60;
constexpr int FRAME_TIME_MS = 1000 / TARGET_FPS; // ~16.67ms

// Display affinity constant (not in MinGW headers)
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif

// Function pointer for GetCursorFrameInfo (not in MinGW headers)
typedef HCURSOR (WINAPI *PFN_GetCursorFrameInfo)(HCURSOR hCursor, LPCWSTR name, DWORD istep, DWORD *rate, DWORD *steps);
static PFN_GetCursorFrameInfo g_pGetCursorFrameInfo = nullptr;

// Global state
static bool g_Running = true;
static HWND g_hWnd = nullptr;
static bool g_UseExcludeFromCapture = false;
static HHOOK g_KeyboardHook = nullptr;

// SetWindowBand - undocumented API to set window z-order band
// Band 1 = ZBID_DESKTOP, Band 2 = ZBID_IMMERSIVE_BACKGROUND, etc.
// Band 7+ puts window above most system UI
typedef BOOL (WINAPI *PFN_SetWindowBand)(HWND hWnd, HWND hwndInsertAfter, DWORD dwBand);
static PFN_SetWindowBand g_pSetWindowBand = nullptr;

// Monitor positions (hardcoded for performance)
// First monitor at origin (both capture source and output destination) - 1920x1080
constexpr int FIRST_MONITOR_X = 0;
constexpr int FIRST_MONITOR_Y = 0;

// GDI objects
static HDC g_hdcScreen = nullptr;
static HDC g_hdcMemory = nullptr;
static HDC g_hdcWindow = nullptr;  // Cached window DC
static HBITMAP g_hBitmap = nullptr;
static HBITMAP g_hOldBitmap = nullptr;
static void* g_pBitmapBits = nullptr;
static HRGN g_hClipRgn = nullptr;  // Reusable clipping region

// Cursor caching
static HCURSOR g_lastCursor = nullptr;
static DWORD g_cursorFrameCount = 0;
static DWORD g_cursorFrameRate = 0;
static DWORD g_cachedHotspotX = 0;
static DWORD g_cachedHotspotY = 0;

// Function pointer for SetWindowDisplayAffinity (Windows 10+)
typedef BOOL (WINAPI *PFN_SetWindowDisplayAffinity)(HWND, DWORD);
static PFN_SetWindowDisplayAffinity g_pSetWindowDisplayAffinity = nullptr;

// Forward declarations
LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool InitWindow(HINSTANCE hInstance);
bool InitGDI();
void Cleanup();
void CaptureAndRender();

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // Enable DPI awareness FIRST, before any window/GDI operations
    EnableDPIAwareness();
    
    // Initialize window
    if (!InitWindow(hInstance))
    {
        MessageBoxA(nullptr, "Failed to create window", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Initialize GDI resources
    if (!InitGDI())
    {
        MessageBoxA(nullptr, "Failed to initialize GDI", "Error", MB_OK | MB_ICONERROR);
        Cleanup();
        return 1;
    }

    // Register Insert key as global hotkey to exit
    if (!RegisterHotKey(NULL, 1, 0, VK_INSERT))
    {
        MessageBoxA(nullptr, "Failed to register hotkey (Insert). Another app may be using it.", "Warning", MB_OK | MB_ICONWARNING);
    }

    // Request 1ms timer resolution for accurate Sleep()
    timeBeginPeriod(1);

    // Show window
    ShowWindow(g_hWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hWnd);

    // Hide cursor aggressively
    while (ShowCursor(FALSE) >= 0) {}
    
    // Clip cursor to our window to prevent it from showing at edges
    RECT clipRect = { FIRST_MONITOR_X, FIRST_MONITOR_Y, 
                      FIRST_MONITOR_X + OUTPUT_WIDTH, FIRST_MONITOR_Y + OUTPUT_HEIGHT };
    ClipCursor(&clipRect);

    // High-resolution timer for frame timing
    LARGE_INTEGER frequency, lastTime, currentTime;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&lastTime);

    // Main loop
    MSG msg = {};
    while (g_Running)
    {
        // Process all pending messages
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_Running = false;
                break;
            }
            // Check for Insert hotkey
            if (msg.message == WM_HOTKEY && msg.wParam == 1)
            {
                g_Running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (g_Running)
        {
            // Capture and render frame
            CaptureAndRender();

            // Keep window at the very top (above taskbar, tooltips, etc.)
            SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0, 
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

            // Frame timing (target 60 FPS)
            QueryPerformanceCounter(&currentTime);
            double elapsedMs = (double)(currentTime.QuadPart - lastTime.QuadPart) * 1000.0 / frequency.QuadPart;
            
            if (elapsedMs < FRAME_TIME_MS)
            {
                Sleep((DWORD)(FRAME_TIME_MS - elapsedMs));
            }
            
            QueryPerformanceCounter(&lastTime);
        }
    }

    // Unregister hotkey
    UnregisterHotKey(NULL, 1);

    // Restore timer resolution
    timeEndPeriod(1);

    // Show cursor again
    ShowCursor(TRUE);
    while (ShowCursor(TRUE) < 0) {}
    
    // Release cursor clip
    ClipCursor(NULL);

    Cleanup();
    return 0;
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SETCURSOR:
        // Hide the cursor when it's over our window
        SetCursor(NULL);
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

bool InitWindow(HINSTANCE hInstance)
{
    // Try to load SetWindowDisplayAffinity (Windows 10 2004+)
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32)
    {
        g_pSetWindowDisplayAffinity = (PFN_SetWindowDisplayAffinity)GetProcAddress(hUser32, "SetWindowDisplayAffinity");
        g_pGetCursorFrameInfo = (PFN_GetCursorFrameInfo)GetProcAddress(hUser32, "GetCursorFrameInfo");
        g_pSetWindowBand = (PFN_SetWindowBand)GetProcAddress(hUser32, "SetWindowBand");
    }

    // Register window class
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = 0;  // No CS_HREDRAW/CS_VREDRAW - window never resizes
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = NULL;  // No cursor - we hide it over our window
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "DesktopCaptureClass";

    if (!RegisterClassExA(&wc))
    {
        return false;
    }

    // Create borderless fullscreen window covering first monitor (1920x1080)
    // WS_EX_TRANSPARENT + WS_EX_LAYERED makes mouse clicks pass through to desktop
    g_hWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        "DesktopCaptureClass",
        "Desktop Capture",
        WS_POPUP,               // Borderless
        FIRST_MONITOR_X, FIRST_MONITOR_Y,  // Position at first monitor (0,0)
        OUTPUT_WIDTH, OUTPUT_HEIGHT,        // Hardcoded 1920x1080
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_hWnd)
    {
        return false;
    }

    // Set layered window to fully opaque (required for WS_EX_LAYERED)
    SetLayeredWindowAttributes(g_hWnd, 0, 255, LWA_ALPHA);

    // Try to put window in highest z-order band (above taskbar, notifications, etc.)
    if (g_pSetWindowBand)
    {
        // ZBID_UIACCESS (7) or higher puts us above most system UI
        g_pSetWindowBand(g_hWnd, HWND_TOPMOST, 7);
    }

    // Try to exclude window from screen capture (Windows 10 2004+))
    if (g_pSetWindowDisplayAffinity)
    {
        if (g_pSetWindowDisplayAffinity(g_hWnd, WDA_EXCLUDEFROMCAPTURE))
        {
            g_UseExcludeFromCapture = true;
        }
    }

    // Cache window DC for the lifetime of the app (much faster than GetDC/ReleaseDC each frame)
    g_hdcWindow = GetDC(g_hWnd);

    return true;
}

bool InitGDI()
{
    // Get screen DC
    g_hdcScreen = GetDC(NULL);
    if (!g_hdcScreen)
    {
        return false;
    }

    // Create memory DC compatible with screen
    g_hdcMemory = CreateCompatibleDC(g_hdcScreen);
    if (!g_hdcMemory)
    {
        return false;
    }

    // Create DIB section for direct pixel access (1920x1080, 32-bit BGRA)
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = OUTPUT_WIDTH;
    bmi.bmiHeader.biHeight = -OUTPUT_HEIGHT;  // Negative for top-down bitmap
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_hBitmap = CreateDIBSection(g_hdcMemory, &bmi, DIB_RGB_COLORS, &g_pBitmapBits, NULL, 0);
    if (!g_hBitmap)
    {
        return false;
    }

    // Select bitmap into memory DC
    g_hOldBitmap = (HBITMAP)SelectObject(g_hdcMemory, g_hBitmap);

    // Create reusable clipping region for cursor
    g_hClipRgn = CreateRectRgn(0, 0, RENDER_WIDTH, RENDER_HEIGHT);

    // Set stretch mode for high-quality scaling
    SetStretchBltMode(g_hdcMemory, HALFTONE);
    SetBrushOrgEx(g_hdcMemory, 0, 0, NULL);

    // Pre-fill the entire buffer with opaque black
    if (g_pBitmapBits)
    {
        DWORD* pixels = (DWORD*)g_pBitmapBits;
        constexpr int totalPixels = OUTPUT_WIDTH * OUTPUT_HEIGHT;
        for (int i = 0; i < totalPixels; i++)
        {
            pixels[i] = 0xFF000000;  // Opaque black (ARGB format)
        }
    }

    return true;
}

void CaptureAndRender()
{
    // If we have WDA_EXCLUDEFROMCAPTURE support, capture directly
    // Otherwise fall back to hide/show method
    if (!g_UseExcludeFromCapture)
    {
        // Hide our window temporarily so we capture the actual desktop
        SetWindowPos(g_hWnd, NULL, 0, 0, 0, 0, 
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_HIDEWINDOW | SWP_NOACTIVATE);
    }
    
    // Capture the 1920x1080 region from the FIRST monitor and scale it down to 1440x1080
    // on the left side of our buffer
    StretchBlt(
        g_hdcMemory,            // Destination DC (our 1920x1080 buffer)
        0, 0,                   // Destination x, y
        RENDER_WIDTH,           // Destination width (1440)
        RENDER_HEIGHT,          // Destination height (1080)
        g_hdcScreen,            // Source DC (desktop)
        FIRST_MONITOR_X,        // Source x (0)
        FIRST_MONITOR_Y,        // Source y (0)
        SOURCE_WIDTH,           // Source width (1920)
        SOURCE_HEIGHT,          // Source height (1080)
        SRCCOPY                 // Copy operation
    );

    // Draw the mouse cursor onto the captured image
    CURSORINFO ci = {};
    ci.cbSize = sizeof(CURSORINFO);
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING))
    {
        // Cache cursor info when cursor changes (avoid GetIconInfo allocation every frame)
        if (ci.hCursor != g_lastCursor)
        {
            g_lastCursor = ci.hCursor;
            
            // Get hotspot from icon info (only when cursor changes)
            ICONINFO iconInfo = {};
            if (GetIconInfo(ci.hCursor, &iconInfo))
            {
                g_cachedHotspotX = iconInfo.xHotspot;
                g_cachedHotspotY = iconInfo.yHotspot;
                // Clean up allocated bitmaps
                if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
                if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
            }
            
            // Also update animation info
            if (g_pGetCursorFrameInfo)
            {
                DWORD rate = 0;
                DWORD totalSteps = 0;
                g_pGetCursorFrameInfo(ci.hCursor, NULL, 0, &rate, &totalSteps);
                g_cursorFrameCount = totalSteps;
                g_cursorFrameRate = rate;
            }
        }
        
        // Calculate cursor position relative to first monitor, scaled down and using cached hotspot
        // Scale factor: RENDER_WIDTH / SOURCE_WIDTH = 1440 / 1920 = 0.75
        double scaleX = (double)RENDER_WIDTH / SOURCE_WIDTH;
        double scaleY = (double)RENDER_HEIGHT / SOURCE_HEIGHT;
        
        int cursorX = (int)((ci.ptScreenPos.x - FIRST_MONITOR_X) * scaleX) - (int)(g_cachedHotspotX * scaleX);
        int cursorY = (int)((ci.ptScreenPos.y - FIRST_MONITOR_Y) * scaleY) - (int)(g_cachedHotspotY * scaleY);

        // Only draw if cursor hotspot is within the first monitor area
        int hotspotX = ci.ptScreenPos.x - FIRST_MONITOR_X;
        int hotspotY = ci.ptScreenPos.y - FIRST_MONITOR_Y;
        
        if (hotspotX >= 0 && hotspotX < SOURCE_WIDTH && 
            hotspotY >= 0 && hotspotY < SOURCE_HEIGHT)
        {
            // Set clipping region to prevent cursor drawing outside capture area
            SelectClipRgn(g_hdcMemory, g_hClipRgn);
            
            // Handle animated cursors using GetCursorFrameInfo
            HCURSOR hCursorToDraw = ci.hCursor;
            
            if (g_pGetCursorFrameInfo && g_cursorFrameCount > 1 && g_cursorFrameRate > 0)
            {
                // Animated cursor - calculate current frame based on time
                // Rate is in jiffies (1/60th of a second)
                DWORD frameTime = (g_cursorFrameRate * 1000) / 60;
                if (frameTime == 0) frameTime = 60;
                
                DWORD animStep = (GetTickCount() / frameTime) % g_cursorFrameCount;
                
                // Get the actual frame cursor handle
                DWORD dummy1, dummy2;
                HCURSOR hFrame = g_pGetCursorFrameInfo(ci.hCursor, NULL, animStep, &dummy1, &dummy2);
                if (hFrame)
                {
                    hCursorToDraw = hFrame;
                }
            }
            
            DrawIconEx(
                g_hdcMemory,
                cursorX,
                cursorY,
                hCursorToDraw,
                0, 0,
                0,
                NULL,
                DI_NORMAL);
            
            // Remove clipping region
            SelectClipRgn(g_hdcMemory, NULL);
        }
    }

    if (!g_UseExcludeFromCapture)
    {
        // Show the window again
        SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0, 
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }

    // The remaining pixels on the right stay black (pre-filled during init)

    // Present the buffer to the window using cached DC
    BitBlt(
        g_hdcWindow,        // Destination (window) - cached DC
        0, 0,               // Destination x, y
        OUTPUT_WIDTH,       // 1920
        OUTPUT_HEIGHT,      // 1080
        g_hdcMemory,        // Source (our buffer)
        0, 0,               // Source x, y
        SRCCOPY             // Copy operation
    );
}

void Cleanup()
{
    if (g_hdcMemory && g_hOldBitmap)
    {
        SelectObject(g_hdcMemory, g_hOldBitmap);
    }

    if (g_hClipRgn)
    {
        DeleteObject(g_hClipRgn);
        g_hClipRgn = nullptr;
    }

    if (g_hBitmap)
    {
        DeleteObject(g_hBitmap);
        g_hBitmap = nullptr;
    }

    if (g_hdcMemory)
    {
        DeleteDC(g_hdcMemory);
        g_hdcMemory = nullptr;
    }

    if (g_hdcScreen)
    {
        ReleaseDC(NULL, g_hdcScreen);
        g_hdcScreen = nullptr;
    }

    if (g_hdcWindow && g_hWnd)
    {
        ReleaseDC(g_hWnd, g_hdcWindow);
        g_hdcWindow = nullptr;
    }

    if (g_hWnd)
    {
        DestroyWindow(g_hWnd);
        g_hWnd = nullptr;
    }
}
