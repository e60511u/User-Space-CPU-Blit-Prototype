@echo off
REM CMake build script for Desktop Capture (DXGI/D3D11 version)
REM Captures first monitor using DXGI Desktop Duplication and scales it down for display
REM CMake auto-detects Visual Studio without needing Developer Command Prompt

cd /d "%~dp0"

echo.
echo === Desktop Capture Build Script (DXGI) ===
echo.
echo Current configuration:
echo   Capture source: First monitor (1920x1080) via DXGI Desktop Duplication
echo   Scaled output: 1440x1080 on left side (GPU-accelerated scaling)
echo   Buffer size: 1920x1080 (480px black padding on right)
echo.

echo Configuring with CMake...
cmake -B build -G "Visual Studio 17 2022" -A x64

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo CMake configuration failed!
    echo.
    pause
    exit /b 1
)

echo.
echo Building...
cmake --build build --config Release

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build failed!
    echo.
    pause
    exit /b 1
)

echo.
if not exist bin mkdir bin
copy /Y "build\bin\Release\DesktopCapture.exe" "bin\DesktopCapture.exe" >nul

echo Build successful! Executable: bin\DesktopCapture.exe
echo Press Insert to exit the application when running.
echo.
pause
exit /b 0
