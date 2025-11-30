// Windows Desktop Capture with DXGI Desktop Duplication
// Captures 1920x1080 from FIRST monitor, scales down to 1440x1080, composites into 1920x1080 buffer with black padding on right
// Displays in fullscreen borderless window on FIRST monitor using D3D11
// Uses SetWindowDisplayAffinity to exclude self from capture (Windows 10 2004+)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <mmsystem.h>
#include <stdio.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "winmm.lib")

// DPI awareness - must be set before any window/GDI calls
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

// Display affinity constant
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// Global state
static bool g_Running = true;
static HWND g_hWnd = nullptr;
static bool g_UseExcludeFromCapture = false;

// Shell/Taskbar handles for hiding
static HWND g_hTaskbar = nullptr;
static HWND g_hStartButton = nullptr;
static bool g_ShellHidden = false;

// Hide Windows shell UI (taskbar, Start button)
void HideWindowsShell()
{
    // Find and hide the taskbar
    g_hTaskbar = FindWindowA("Shell_TrayWnd", nullptr);
    if (g_hTaskbar)
    {
        ShowWindow(g_hTaskbar, SW_HIDE);
    }
    
    // Find and hide the Start button (Windows 10/11)
    g_hStartButton = FindWindowA("Button", "Start");
    if (!g_hStartButton)
    {
        g_hStartButton = FindWindowExA(nullptr, nullptr, "Button", "Start");
    }
    if (g_hStartButton)
    {
        ShowWindow(g_hStartButton, SW_HIDE);
    }
    
    g_ShellHidden = true;
}

// Restore Windows shell UI
void ShowWindowsShell()
{
    if (g_hTaskbar)
    {
        ShowWindow(g_hTaskbar, SW_SHOW);
        g_hTaskbar = nullptr;
    }
    
    if (g_hStartButton)
    {
        ShowWindow(g_hStartButton, SW_SHOW);
        g_hStartButton = nullptr;
    }
    
    g_ShellHidden = false;
}

// Cursor rendering
static ID3D11Texture2D* g_CursorTexture = nullptr;
static ID3D11ShaderResourceView* g_CursorSRV = nullptr;
static BYTE* g_CursorBuffer = nullptr;
static int g_CursorWidth = 0;
static int g_CursorHeight = 0;
static int g_CursorHotspotX = 0;
static int g_CursorHotspotY = 0;
static bool g_CursorVisible = true;
static POINT g_CursorPosition = {0, 0};

// D3D11/DXGI objects
static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static IDXGISwapChain1* g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RenderTargetView = nullptr;
static ID3D11Texture2D* g_BackBuffer = nullptr;
static IDXGIOutputDuplication* g_DeskDupl = nullptr;
static ID3D11Texture2D* g_StagingTexture = nullptr;
static ID3D11Texture2D* g_ScaledTexture = nullptr;

// Shader for scaling
static ID3D11VertexShader* g_VertexShader = nullptr;
static ID3D11PixelShader* g_PixelShader = nullptr;
static ID3D11SamplerState* g_SamplerState = nullptr;
static ID3D11ShaderResourceView* g_DesktopSRV = nullptr;
static ID3D11InputLayout* g_InputLayout = nullptr;
static ID3D11Buffer* g_VertexBuffer = nullptr;

// Monitor position
constexpr int FIRST_MONITOR_X = 0;
constexpr int FIRST_MONITOR_Y = 0;

// SetWindowBand API and window band constants
// These are undocumented Windows z-order bands
#define ZBID_DEFAULT 0
#define ZBID_DESKTOP 1
#define ZBID_UIACCESS 2
#define ZBID_IMMERSIVE_IHM 3
#define ZBID_IMMERSIVE_NOTIFICATION 4
#define ZBID_IMMERSIVE_APPCHROME 5
#define ZBID_IMMERSIVE_MOGO 6
#define ZBID_IMMERSIVE_EDGY 7
#define ZBID_IMMERSIVE_INACTIVEMOBODY 8
#define ZBID_IMMERSIVE_INACTIVEDOCK 9
#define ZBID_IMMERSIVE_ACTIVEMOBODY 10
#define ZBID_IMMERSIVE_ACTIVEDOCK 11
#define ZBID_IMMERSIVE_BACKGROUND 12
#define ZBID_IMMERSIVE_SEARCH 13
#define ZBID_GENUINE_WINDOWS 14
#define ZBID_IMMERSIVE_RESTRICTED 15
#define ZBID_SYSTEM_TOOLS 16
#define ZBID_LOCK 17
#define ZBID_ABOVELOCK_UX 18

typedef BOOL (WINAPI *PFN_SetWindowBand)(HWND hWnd, HWND hwndInsertAfter, DWORD dwBand);
typedef HWND (WINAPI *PFN_CreateWindowInBand)(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam, DWORD dwBand);
static PFN_SetWindowBand g_pSetWindowBand = nullptr;
static PFN_CreateWindowInBand g_pCreateWindowInBand = nullptr;

// SetWindowDisplayAffinity
typedef BOOL (WINAPI *PFN_SetWindowDisplayAffinity)(HWND, DWORD);
static PFN_SetWindowDisplayAffinity g_pSetWindowDisplayAffinity = nullptr;

// Vertex structure for fullscreen quad
struct Vertex
{
    float x, y;
    float u, v;
};

// Shader source code
const char* g_ShaderSource = R"(
struct VS_INPUT
{
    float2 pos : POSITION;
    float2 tex : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

Texture2D desktopTex : register(t0);
SamplerState samplerState : register(s0);

PS_INPUT VS(VS_INPUT input)
{
    PS_INPUT output;
    output.pos = float4(input.pos, 0.0f, 1.0f);
    output.tex = input.tex;
    return output;
}

float4 PS(PS_INPUT input) : SV_TARGET
{
    // Only sample from the scaled region (left 1440 pixels of 1920)
    // Map UV from [0, 0.75] x [0, 1] for the left side
    float2 scaledUV = input.tex;
    
    // Check if we're in the render area (left 1440 pixels = 75% of width)
    if (input.tex.x <= 0.75f)
    {
        // Remap UV.x from [0, 0.75] to [0, 1] for sampling full source
        scaledUV.x = input.tex.x / 0.75f;
        return desktopTex.Sample(samplerState, scaledUV);
    }
    else
    {
        // Black padding on the right
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
}
)";

// Forward declarations
LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool InitWindow(HINSTANCE hInstance);
bool InitD3D();
bool InitDesktopDuplication();
bool InitShaders();
void Cleanup();
void CaptureAndRender();

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    EnableDPIAwareness();
    
    if (!InitWindow(hInstance))
    {
        MessageBoxA(nullptr, "Failed to create window", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!InitD3D())
    {
        MessageBoxA(nullptr, "Failed to initialize Direct3D 11", "Error", MB_OK | MB_ICONERROR);
        Cleanup();
        return 1;
    }

    if (!InitDesktopDuplication())
    {
        MessageBoxA(nullptr, "Failed to initialize Desktop Duplication.\nMake sure you're running Windows 8 or later.", "Error", MB_OK | MB_ICONERROR);
        Cleanup();
        return 1;
    }

    if (!InitShaders())
    {
        MessageBoxA(nullptr, "Failed to initialize shaders", "Error", MB_OK | MB_ICONERROR);
        Cleanup();
        return 1;
    }

    // Register Insert key as global hotkey to exit (use window handle)
    if (!RegisterHotKey(g_hWnd, 1, 0, VK_INSERT))
    {
        MessageBoxA(nullptr, "Failed to register hotkey (Insert).", "Warning", MB_OK | MB_ICONWARNING);
    }

    timeBeginPeriod(1);

    ShowWindow(g_hWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hWnd);

    // Hide Windows shell elements (taskbar, Start) so our window is truly on top
    HideWindowsShell();

    LARGE_INTEGER frequency, lastTime, currentTime;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&lastTime);

    MSG msg = {};
    while (g_Running)
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
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
            DispatchMessageA(&msg);
        }

        if (g_Running)
        {
            CaptureAndRender();

            // Aggressively maintain topmost status
            SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0, 
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
            
            // Reapply highest window band every frame
            if (g_pSetWindowBand)
            {
                g_pSetWindowBand(g_hWnd, HWND_TOPMOST, ZBID_ABOVELOCK_UX);
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

    UnregisterHotKey(g_hWnd, 1);
    timeEndPeriod(1);

    // Restore Windows shell elements
    ShowWindowsShell();

    Cleanup();
    return 0;
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

bool InitWindow(HINSTANCE hInstance)
{
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32)
    {
        g_pSetWindowDisplayAffinity = (PFN_SetWindowDisplayAffinity)GetProcAddress(hUser32, "SetWindowDisplayAffinity");
        g_pSetWindowBand = (PFN_SetWindowBand)GetProcAddress(hUser32, "SetWindowBand");
        g_pCreateWindowInBand = (PFN_CreateWindowInBand)GetProcAddress(hUser32, "CreateWindowInBand");
    }

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = 0;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = NULL;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "DesktopCaptureDXGIClass";

    if (!RegisterClassExA(&wc))
        return false;

    // Try to create window in the highest possible band using undocumented API
    if (g_pCreateWindowInBand)
    {
        // Try ZBID_ABOVELOCK_UX (18) - highest band, used by lock screen UI
        g_hWnd = g_pCreateWindowInBand(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"DesktopCaptureDXGIClass",
            L"Desktop Capture DXGI",
            WS_POPUP,
            FIRST_MONITOR_X, FIRST_MONITOR_Y,
            OUTPUT_WIDTH, OUTPUT_HEIGHT,
            nullptr, nullptr, hInstance, nullptr,
            ZBID_ABOVELOCK_UX
        );
        
        // If highest band fails, try ZBID_SYSTEM_TOOLS (16)
        if (!g_hWnd)
        {
            g_hWnd = g_pCreateWindowInBand(
                WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                L"DesktopCaptureDXGIClass",
                L"Desktop Capture DXGI",
                WS_POPUP,
                FIRST_MONITOR_X, FIRST_MONITOR_Y,
                OUTPUT_WIDTH, OUTPUT_HEIGHT,
                nullptr, nullptr, hInstance, nullptr,
                ZBID_SYSTEM_TOOLS
            );
        }
    }
    
    // Fallback to regular CreateWindow
    if (!g_hWnd)
    {
        g_hWnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            "DesktopCaptureDXGIClass",
            "Desktop Capture DXGI",
            WS_POPUP,
            FIRST_MONITOR_X, FIRST_MONITOR_Y,
            OUTPUT_WIDTH, OUTPUT_HEIGHT,
            nullptr, nullptr, hInstance, nullptr
        );
    }

    if (!g_hWnd)
        return false;

    // Make window fully opaque but click-through
    SetLayeredWindowAttributes(g_hWnd, 0, 255, LWA_ALPHA);

    // Apply SetWindowBand with highest possible band
    if (g_pSetWindowBand)
    {
        // Try bands from highest to lowest until one works
        // These require different privilege levels
        DWORD bands[] = { ZBID_ABOVELOCK_UX, ZBID_LOCK, ZBID_SYSTEM_TOOLS, ZBID_GENUINE_WINDOWS, 
                          ZBID_IMMERSIVE_RESTRICTED, ZBID_IMMERSIVE_SEARCH, ZBID_IMMERSIVE_ACTIVEDOCK };
        
        for (int i = 0; i < sizeof(bands)/sizeof(bands[0]); i++)
        {
            if (g_pSetWindowBand(g_hWnd, HWND_TOPMOST, bands[i]))
                break;
        }
    }

    if (g_pSetWindowDisplayAffinity)
    {
        if (g_pSetWindowDisplayAffinity(g_hWnd, WDA_EXCLUDEFROMCAPTURE))
            g_UseExcludeFromCapture = true;
    }

    return true;
}

bool InitD3D()
{
    HRESULT hr;

    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL featureLevel;
    
    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &g_Device,
        &featureLevel,
        &g_Context
    );

    if (FAILED(hr))
        return false;

    // Get DXGI factory
    IDXGIDevice* dxgiDevice = nullptr;
    hr = g_Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr))
        return false;

    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    dxgiDevice->Release();
    if (FAILED(hr))
        return false;

    IDXGIFactory2* dxgiFactory = nullptr;
    hr = dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&dxgiFactory);
    dxgiAdapter->Release();
    if (FAILED(hr))
        return false;

    // Create swap chain (windowed mode for desktop duplication compatibility)
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = OUTPUT_WIDTH;
    swapChainDesc.Height = OUTPUT_HEIGHT;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Flags = 0;

    hr = dxgiFactory->CreateSwapChainForHwnd(
        g_Device,
        g_hWnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &g_SwapChain
    );
    
    // Disable Alt+Enter fullscreen toggle
    dxgiFactory->MakeWindowAssociation(g_hWnd, DXGI_MWA_NO_ALT_ENTER);
    dxgiFactory->Release();
    
    if (FAILED(hr))
        return false;

    // Get back buffer and create render target view
    hr = g_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&g_BackBuffer);
    if (FAILED(hr))
        return false;

    hr = g_Device->CreateRenderTargetView(g_BackBuffer, nullptr, &g_RenderTargetView);
    if (FAILED(hr))
        return false;

    // Set viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = (float)OUTPUT_WIDTH;
    viewport.Height = (float)OUTPUT_HEIGHT;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    g_Context->RSSetViewports(1, &viewport);

    return true;
}

bool InitDesktopDuplication()
{
    HRESULT hr;

    // Get DXGI device and adapter
    IDXGIDevice* dxgiDevice = nullptr;
    hr = g_Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr))
        return false;

    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    dxgiDevice->Release();
    if (FAILED(hr))
        return false;

    // Get primary output (monitor)
    IDXGIOutput* dxgiOutput = nullptr;
    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    dxgiAdapter->Release();
    if (FAILED(hr))
        return false;

    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOutput1);
    dxgiOutput->Release();
    if (FAILED(hr))
        return false;

    // Create desktop duplication
    hr = dxgiOutput1->DuplicateOutput(g_Device, &g_DeskDupl);
    dxgiOutput1->Release();
    if (FAILED(hr))
        return false;

    // Create staging texture for CPU access if needed
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = SOURCE_WIDTH;
    stagingDesc.Height = SOURCE_HEIGHT;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_DEFAULT;
    stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    hr = g_Device->CreateTexture2D(&stagingDesc, nullptr, &g_StagingTexture);
    if (FAILED(hr))
        return false;

    // Create shader resource view for the staging texture
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    
    hr = g_Device->CreateShaderResourceView(g_StagingTexture, &srvDesc, &g_DesktopSRV);
    if (FAILED(hr))
        return false;

    return true;
}

bool InitShaders()
{
    HRESULT hr;

    // Compile vertex shader
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    
    hr = D3DCompile(
        g_ShaderSource,
        strlen(g_ShaderSource),
        "shader",
        nullptr,
        nullptr,
        "VS",
        "vs_4_0",
        0, 0,
        &vsBlob,
        &errorBlob
    );
    
    if (FAILED(hr))
    {
        if (errorBlob)
        {
            MessageBoxA(nullptr, (char*)errorBlob->GetBufferPointer(), "VS Compile Error", MB_OK);
            errorBlob->Release();
        }
        return false;
    }

    hr = g_Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_VertexShader);
    if (FAILED(hr))
    {
        vsBlob->Release();
        return false;
    }

    // Create input layout
    D3D11_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = g_Device->CreateInputLayout(inputLayout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_InputLayout);
    vsBlob->Release();
    if (FAILED(hr))
        return false;

    // Compile pixel shader
    ID3DBlob* psBlob = nullptr;
    hr = D3DCompile(
        g_ShaderSource,
        strlen(g_ShaderSource),
        "shader",
        nullptr,
        nullptr,
        "PS",
        "ps_4_0",
        0, 0,
        &psBlob,
        &errorBlob
    );
    
    if (FAILED(hr))
    {
        if (errorBlob)
        {
            MessageBoxA(nullptr, (char*)errorBlob->GetBufferPointer(), "PS Compile Error", MB_OK);
            errorBlob->Release();
        }
        return false;
    }

    hr = g_Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_PixelShader);
    psBlob->Release();
    if (FAILED(hr))
        return false;

    // Create sampler state (bilinear filtering for smooth scaling)
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = g_Device->CreateSamplerState(&samplerDesc, &g_SamplerState);
    if (FAILED(hr))
        return false;

    // Create fullscreen quad vertex buffer
    Vertex vertices[] = {
        { -1.0f,  1.0f, 0.0f, 0.0f },  // Top-left
        {  1.0f,  1.0f, 1.0f, 0.0f },  // Top-right
        { -1.0f, -1.0f, 0.0f, 1.0f },  // Bottom-left
        {  1.0f, -1.0f, 1.0f, 1.0f }   // Bottom-right
    };

    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.ByteWidth = sizeof(vertices);
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = vertices;

    hr = g_Device->CreateBuffer(&bufferDesc, &initData, &g_VertexBuffer);
    if (FAILED(hr))
        return false;

    return true;
}

void DrawCursorOnTexture(ID3D11Texture2D* destTexture, int cursorX, int cursorY)
{
    if (!g_CursorBuffer || g_CursorWidth == 0 || g_CursorHeight == 0)
        return;
    
    // Adjust cursor position by hotspot
    int drawX = cursorX - g_CursorHotspotX;
    int drawY = cursorY - g_CursorHotspotY;
    
    // Create a staging texture for CPU write
    D3D11_TEXTURE2D_DESC desc;
    destTexture->GetDesc(&desc);
    
    ID3D11Texture2D* stagingTex = nullptr;
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    
    if (FAILED(g_Device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex)))
        return;
    
    g_Context->CopyResource(stagingTex, destTexture);
    
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(g_Context->Map(stagingTex, 0, D3D11_MAP_READ_WRITE, 0, &mapped)))
    {
        BYTE* destData = (BYTE*)mapped.pData;
        
        for (int y = 0; y < g_CursorHeight; y++)
        {
            int destY = drawY + y;
            if (destY < 0 || destY >= (int)desc.Height)
                continue;
            
            for (int x = 0; x < g_CursorWidth; x++)
            {
                int destX = drawX + x;
                if (destX < 0 || destX >= (int)desc.Width)
                    continue;
                
                int srcIdx = (y * g_CursorWidth + x) * 4;
                int destIdx = destY * mapped.RowPitch + destX * 4;
                
                BYTE srcA = g_CursorBuffer[srcIdx + 3];
                if (srcA > 0)
                {
                    BYTE srcB = g_CursorBuffer[srcIdx + 0];
                    BYTE srcG = g_CursorBuffer[srcIdx + 1];
                    BYTE srcR = g_CursorBuffer[srcIdx + 2];
                    
                    if (srcA == 255)
                    {
                        destData[destIdx + 0] = srcB;
                        destData[destIdx + 1] = srcG;
                        destData[destIdx + 2] = srcR;
                        destData[destIdx + 3] = 255;
                    }
                    else
                    {
                        // Alpha blend
                        BYTE destB = destData[destIdx + 0];
                        BYTE destG = destData[destIdx + 1];
                        BYTE destR = destData[destIdx + 2];
                        
                        destData[destIdx + 0] = (srcB * srcA + destB * (255 - srcA)) / 255;
                        destData[destIdx + 1] = (srcG * srcA + destG * (255 - srcA)) / 255;
                        destData[destIdx + 2] = (srcR * srcA + destR * (255 - srcA)) / 255;
                        destData[destIdx + 3] = 255;
                    }
                }
            }
        }
        
        g_Context->Unmap(stagingTex, 0);
        g_Context->CopyResource(destTexture, stagingTex);
    }
    
    stagingTex->Release();
}

void UpdateCursorShape(DXGI_OUTDUPL_POINTER_SHAPE_INFO* shapeInfo, BYTE* shapeBuffer)
{
    if (g_CursorBuffer)
    {
        delete[] g_CursorBuffer;
        g_CursorBuffer = nullptr;
    }
    
    g_CursorWidth = shapeInfo->Width;
    g_CursorHeight = shapeInfo->Height;
    g_CursorHotspotX = shapeInfo->HotSpot.x;
    g_CursorHotspotY = shapeInfo->HotSpot.y;
    
    if (shapeInfo->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME)
    {
        // Monochrome cursor: height is 2x (AND mask + XOR mask)
        g_CursorHeight = shapeInfo->Height / 2;
        g_CursorBuffer = new BYTE[g_CursorWidth * g_CursorHeight * 4];
        
        int widthBytes = (g_CursorWidth + 7) / 8;
        
        for (int y = 0; y < g_CursorHeight; y++)
        {
            for (int x = 0; x < g_CursorWidth; x++)
            {
                int byteIdx = y * shapeInfo->Pitch + x / 8;
                int bitIdx = 7 - (x % 8);
                
                int andBit = (shapeBuffer[byteIdx] >> bitIdx) & 1;
                int xorBit = (shapeBuffer[byteIdx + g_CursorHeight * shapeInfo->Pitch] >> bitIdx) & 1;
                
                int destIdx = (y * g_CursorWidth + x) * 4;
                
                if (andBit == 0 && xorBit == 0)
                {
                    // Black
                    g_CursorBuffer[destIdx + 0] = 0;
                    g_CursorBuffer[destIdx + 1] = 0;
                    g_CursorBuffer[destIdx + 2] = 0;
                    g_CursorBuffer[destIdx + 3] = 255;
                }
                else if (andBit == 0 && xorBit == 1)
                {
                    // White
                    g_CursorBuffer[destIdx + 0] = 255;
                    g_CursorBuffer[destIdx + 1] = 255;
                    g_CursorBuffer[destIdx + 2] = 255;
                    g_CursorBuffer[destIdx + 3] = 255;
                }
                else if (andBit == 1 && xorBit == 0)
                {
                    // Transparent
                    g_CursorBuffer[destIdx + 0] = 0;
                    g_CursorBuffer[destIdx + 1] = 0;
                    g_CursorBuffer[destIdx + 2] = 0;
                    g_CursorBuffer[destIdx + 3] = 0;
                }
                else
                {
                    // Inverse (render as semi-transparent white)
                    g_CursorBuffer[destIdx + 0] = 255;
                    g_CursorBuffer[destIdx + 1] = 255;
                    g_CursorBuffer[destIdx + 2] = 255;
                    g_CursorBuffer[destIdx + 3] = 128;
                }
            }
        }
    }
    else if (shapeInfo->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR)
    {
        // 32-bit BGRA cursor
        g_CursorBuffer = new BYTE[g_CursorWidth * g_CursorHeight * 4];
        
        for (int y = 0; y < g_CursorHeight; y++)
        {
            memcpy(g_CursorBuffer + y * g_CursorWidth * 4,
                   shapeBuffer + y * shapeInfo->Pitch,
                   g_CursorWidth * 4);
        }
    }
    else if (shapeInfo->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR)
    {
        // Masked color cursor
        g_CursorBuffer = new BYTE[g_CursorWidth * g_CursorHeight * 4];
        
        for (int y = 0; y < g_CursorHeight; y++)
        {
            for (int x = 0; x < g_CursorWidth; x++)
            {
                int srcIdx = y * shapeInfo->Pitch + x * 4;
                int destIdx = (y * g_CursorWidth + x) * 4;
                
                BYTE mask = shapeBuffer[srcIdx + 3];
                if (mask)
                {
                    // XOR with screen - render as semi-transparent
                    g_CursorBuffer[destIdx + 0] = shapeBuffer[srcIdx + 0];
                    g_CursorBuffer[destIdx + 1] = shapeBuffer[srcIdx + 1];
                    g_CursorBuffer[destIdx + 2] = shapeBuffer[srcIdx + 2];
                    g_CursorBuffer[destIdx + 3] = 128;
                }
                else
                {
                    // Opaque
                    g_CursorBuffer[destIdx + 0] = shapeBuffer[srcIdx + 0];
                    g_CursorBuffer[destIdx + 1] = shapeBuffer[srcIdx + 1];
                    g_CursorBuffer[destIdx + 2] = shapeBuffer[srcIdx + 2];
                    g_CursorBuffer[destIdx + 3] = 255;
                }
            }
        }
    }
}

void CaptureAndRender()
{
    HRESULT hr;

    // Acquire next frame from desktop duplication
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    IDXGIResource* desktopResource = nullptr;
    
    hr = g_DeskDupl->AcquireNextFrame(0, &frameInfo, &desktopResource);
    
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        // No new frame, just present the previous one
    }
    else if (SUCCEEDED(hr))
    {
        // Get the desktop texture
        ID3D11Texture2D* desktopTexture = nullptr;
        hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture);
        
        if (SUCCEEDED(hr))
        {
            // Copy desktop to our staging texture
            g_Context->CopyResource(g_StagingTexture, desktopTexture);
            desktopTexture->Release();
        }
        
        // Update cursor shape if changed
        if (frameInfo.PointerShapeBufferSize > 0)
        {
            BYTE* shapeBuffer = new BYTE[frameInfo.PointerShapeBufferSize];
            UINT bufferSizeRequired;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
            
            hr = g_DeskDupl->GetFramePointerShape(frameInfo.PointerShapeBufferSize,
                                                   shapeBuffer, &bufferSizeRequired, &shapeInfo);
            if (SUCCEEDED(hr))
            {
                UpdateCursorShape(&shapeInfo, shapeBuffer);
            }
            
            delete[] shapeBuffer;
        }
        
        desktopResource->Release();
        g_DeskDupl->ReleaseFrame();
    }
    else if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        // Desktop duplication was lost, need to recreate
        g_DeskDupl->Release();
        g_DeskDupl = nullptr;
        InitDesktopDuplication();
        return;
    }
    
    // Clear the render target to black
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_Context->ClearRenderTargetView(g_RenderTargetView, clearColor);

    // Set up pipeline
    g_Context->OMSetRenderTargets(1, &g_RenderTargetView, nullptr);
    g_Context->VSSetShader(g_VertexShader, nullptr, 0);
    g_Context->PSSetShader(g_PixelShader, nullptr, 0);
    g_Context->PSSetShaderResources(0, 1, &g_DesktopSRV);
    g_Context->PSSetSamplers(0, 1, &g_SamplerState);
    g_Context->IASetInputLayout(g_InputLayout);
    
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    g_Context->IASetVertexBuffers(0, 1, &g_VertexBuffer, &stride, &offset);
    g_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // Draw fullscreen quad (desktop)
    g_Context->Draw(4, 0);

    // Unbind shader resources before drawing cursor on back buffer
    ID3D11ShaderResourceView* nullSRV = nullptr;
    g_Context->PSSetShaderResources(0, 1, &nullSRV);

    // Draw cursor on the BACK BUFFER (after desktop render) using real-time cursor position
    // This avoids feedback loop since we're drawing on output, not source
    POINT cursorPos;
    if (GetCursorPos(&cursorPos))
    {
        // Adjust for monitor position (cursor is in virtual screen coordinates)
        int cursorX = cursorPos.x - FIRST_MONITOR_X;
        int cursorY = cursorPos.y - FIRST_MONITOR_Y;
        
        // Only draw if cursor is within our capture area
        if (cursorX >= 0 && cursorX < SOURCE_WIDTH && cursorY >= 0 && cursorY < SOURCE_HEIGHT)
        {
            if (g_CursorBuffer)
            {
                // Scale cursor position: source 1920 -> output 1440 (left 75% of screen)
                int scaledCursorX = (cursorX * RENDER_WIDTH) / SOURCE_WIDTH;
                int scaledCursorY = cursorY; // Y doesn't change
                
                DrawCursorOnTexture(g_BackBuffer, scaledCursorX, scaledCursorY);
            }
        }
    }

    // Present
    g_SwapChain->Present(1, 0);  // VSync enabled
}

void Cleanup()
{
    if (g_CursorBuffer) { delete[] g_CursorBuffer; g_CursorBuffer = nullptr; }
    if (g_CursorSRV) { g_CursorSRV->Release(); g_CursorSRV = nullptr; }
    if (g_CursorTexture) { g_CursorTexture->Release(); g_CursorTexture = nullptr; }
    if (g_VertexBuffer) { g_VertexBuffer->Release(); g_VertexBuffer = nullptr; }
    if (g_InputLayout) { g_InputLayout->Release(); g_InputLayout = nullptr; }
    if (g_SamplerState) { g_SamplerState->Release(); g_SamplerState = nullptr; }
    if (g_PixelShader) { g_PixelShader->Release(); g_PixelShader = nullptr; }
    if (g_VertexShader) { g_VertexShader->Release(); g_VertexShader = nullptr; }
    if (g_DesktopSRV) { g_DesktopSRV->Release(); g_DesktopSRV = nullptr; }
    if (g_StagingTexture) { g_StagingTexture->Release(); g_StagingTexture = nullptr; }
    if (g_ScaledTexture) { g_ScaledTexture->Release(); g_ScaledTexture = nullptr; }
    if (g_DeskDupl) { g_DeskDupl->Release(); g_DeskDupl = nullptr; }
    if (g_RenderTargetView) { g_RenderTargetView->Release(); g_RenderTargetView = nullptr; }
    if (g_BackBuffer) { g_BackBuffer->Release(); g_BackBuffer = nullptr; }
    if (g_SwapChain) { g_SwapChain->Release(); g_SwapChain = nullptr; }
    if (g_Context) { g_Context->Release(); g_Context = nullptr; }
    if (g_Device) { g_Device->Release(); g_Device = nullptr; }
    
    if (g_hWnd)
    {
        DestroyWindow(g_hWnd);
        g_hWnd = nullptr;
    }
}
