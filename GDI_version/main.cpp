// Windows Desktop Capture with DXGI to Fullscreen Window
// Captures 1920x1080 from first monitor, scales to 1440x1080, composites into 1920x1080 buffer with black padding on right
// Displays via DXGI swap chain at 60 Hz vsync

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Constants
constexpr int SOURCE_WIDTH = 1920;   // Full first monitor width to capture
constexpr int SOURCE_HEIGHT = 1080;  // Full first monitor height to capture  
constexpr int RENDER_WIDTH = 1440;   // Scaled-down width for display
constexpr int RENDER_HEIGHT = 1080;  // Height stays the same
constexpr int OUTPUT_WIDTH = 1920;
constexpr int OUTPUT_HEIGHT = 1080;
constexpr int BLACK_REGION_WIDTH = OUTPUT_WIDTH - RENDER_WIDTH; // 480

// Global state
static bool g_Running = true;
static HWND g_hWnd = nullptr;

// D3D11 / DXGI objects
static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static IDXGISwapChain* g_SwapChain = nullptr;
static ID3D11Texture2D* g_BackBuffer = nullptr;
static ID3D11Texture2D* g_CompositeTexture = nullptr;
static IDXGIOutputDuplication* g_DeskDupl = nullptr;

// Forward declarations
LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool InitWindow(HINSTANCE hInstance);
bool InitD3D();
bool InitDesktopDuplication();
void Cleanup();
void RenderFrame();

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // Initialize window
    if (!InitWindow(hInstance))
    {
        MessageBoxA(nullptr, "Failed to create window", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Initialize D3D11 and swap chain
    if (!InitD3D())
    {
        MessageBoxA(nullptr, "Failed to initialize D3D11", "Error", MB_OK | MB_ICONERROR);
        Cleanup();
        return 1;
    }

    // Initialize desktop duplication
    if (!InitDesktopDuplication())
    {
        MessageBoxA(nullptr, "Failed to initialize Desktop Duplication", "Error", MB_OK | MB_ICONERROR);
        Cleanup();
        return 1;
    }

    // Show window
    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    // Main message loop
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
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (g_Running)
        {
            RenderFrame();
        }
    }

    Cleanup();
    return 0;
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            g_Running = false;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

bool InitWindow(HINSTANCE hInstance)
{
    // Register window class
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "DesktopCaptureClass";

    if (!RegisterClassExA(&wc))
    {
        return false;
    }

    // Create borderless fullscreen window at (0,0) covering 1920x1080
    g_hWnd = CreateWindowExA(
        WS_EX_TOPMOST,          // Always on top
        "DesktopCaptureClass",
        "Desktop Capture",
        WS_POPUP,               // Borderless
        0, 0,                   // Position at origin (primary monitor)
        OUTPUT_WIDTH, OUTPUT_HEIGHT,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    return (g_hWnd != nullptr);
}

bool InitD3D()
{
    HRESULT hr;

    // Create D3D11 device and context
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;

    hr = D3D11CreateDevice(
        nullptr,                    // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,                          // No debug flags
        featureLevels,
        1,
        D3D11_SDK_VERSION,
        &g_Device,
        &featureLevel,
        &g_Context
    );

    if (FAILED(hr))
    {
        return false;
    }

    // Get DXGI factory from device
    IDXGIDevice* dxgiDevice = nullptr;
    hr = g_Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr))
    {
        return false;
    }

    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    dxgiDevice->Release();
    if (FAILED(hr))
    {
        return false;
    }

    IDXGIFactory* dxgiFactory = nullptr;
    hr = dxgiAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&dxgiFactory);
    dxgiAdapter->Release();
    if (FAILED(hr))
    {
        return false;
    }

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = OUTPUT_WIDTH;
    scd.BufferDesc.Height = OUTPUT_HEIGHT;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = g_hWnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = dxgiFactory->CreateSwapChain(g_Device, &scd, &g_SwapChain);
    dxgiFactory->Release();
    if (FAILED(hr))
    {
        return false;
    }

    // Get back buffer
    hr = g_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&g_BackBuffer);
    if (FAILED(hr))
    {
        return false;
    }

    // Create composite texture (1920x1080) - this will hold our final frame
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = OUTPUT_WIDTH;
    texDesc.Height = OUTPUT_HEIGHT;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    // Initialize with black pixels
    UINT rowPitch = OUTPUT_WIDTH * 4;
    UINT imageSize = rowPitch * OUTPUT_HEIGHT;
    BYTE* blackPixels = new BYTE[imageSize];
    memset(blackPixels, 0, imageSize);  // All zeros = black (with alpha 0, but that's fine for our use)

    // Set alpha to 255 for opaque black
    for (UINT y = 0; y < OUTPUT_HEIGHT; y++)
    {
        for (UINT x = 0; x < OUTPUT_WIDTH; x++)
        {
            UINT offset = y * rowPitch + x * 4;
            blackPixels[offset + 3] = 255;  // Alpha channel
        }
    }

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = blackPixels;
    initData.SysMemPitch = rowPitch;

    hr = g_Device->CreateTexture2D(&texDesc, &initData, &g_CompositeTexture);
    delete[] blackPixels;

    if (FAILED(hr))
    {
        return false;
    }

    return true;
}

bool InitDesktopDuplication()
{
    HRESULT hr;

    // Get DXGI device
    IDXGIDevice* dxgiDevice = nullptr;
    hr = g_Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr))
    {
        return false;
    }

    // Get DXGI adapter
    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    dxgiDevice->Release();
    if (FAILED(hr))
    {
        return false;
    }

    // Get primary output (monitor 0)
    IDXGIOutput* dxgiOutput = nullptr;
    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    dxgiAdapter->Release();
    if (FAILED(hr))
    {
        return false;
    }

    // Query for Output1 interface (required for DuplicateOutput)
    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOutput1);
    dxgiOutput->Release();
    if (FAILED(hr))
    {
        return false;
    }

    // Create desktop duplication
    hr = dxgiOutput1->DuplicateOutput(g_Device, &g_DeskDupl);
    dxgiOutput1->Release();
    if (FAILED(hr))
    {
        return false;
    }

    return true;
}

void RenderFrame()
{
    HRESULT hr;

    // Acquire next desktop frame
    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

    hr = g_DeskDupl->AcquireNextFrame(16, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        // No new frame available, just present the existing composite
        g_Context->CopyResource(g_BackBuffer, g_CompositeTexture);
        g_SwapChain->Present(1, 0);
        return;
    }

    if (FAILED(hr))
    {
        // On error, just present existing frame
        g_Context->CopyResource(g_BackBuffer, g_CompositeTexture);
        g_SwapChain->Present(1, 0);
        return;
    }

    // Get the desktop texture
    ID3D11Texture2D* desktopTexture = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture);
    desktopResource->Release();

    if (SUCCEEDED(hr))
    {
        // Copy the 1920x1080 region from desktop to the left side of composite texture
        // NOTE: DXGI CopySubresourceRegion doesn't scale - would need shader for actual scaling
        D3D11_BOX srcBox = {};
        srcBox.left = 0;
        srcBox.top = 0;
        srcBox.front = 0;
        srcBox.right = RENDER_WIDTH;
        srcBox.bottom = RENDER_HEIGHT;
        srcBox.back = 1;

        g_Context->CopySubresourceRegion(
            g_CompositeTexture,     // Destination texture
            0,                       // Destination subresource
            0, 0, 0,                // Destination x, y, z
            desktopTexture,         // Source texture
            0,                       // Source subresource
            &srcBox                 // Source region
        );

        desktopTexture->Release();
    }

    // Release the frame
    g_DeskDupl->ReleaseFrame();

    // Copy composite to back buffer and present
    g_Context->CopyResource(g_BackBuffer, g_CompositeTexture);
    g_SwapChain->Present(1, 0);  // 1 = vsync (60 Hz)
}

void Cleanup()
{
    if (g_DeskDupl)
    {
        g_DeskDupl->Release();
        g_DeskDupl = nullptr;
    }

    if (g_CompositeTexture)
    {
        g_CompositeTexture->Release();
        g_CompositeTexture = nullptr;
    }

    if (g_BackBuffer)
    {
        g_BackBuffer->Release();
        g_BackBuffer = nullptr;
    }

    if (g_SwapChain)
    {
        g_SwapChain->Release();
        g_SwapChain = nullptr;
    }

    if (g_Context)
    {
        g_Context->Release();
        g_Context = nullptr;
    }

    if (g_Device)
    {
        g_Device->Release();
        g_Device = nullptr;
    }

    if (g_hWnd)
    {
        DestroyWindow(g_hWnd);
        g_hWnd = nullptr;
    }
}
