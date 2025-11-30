@echo off
REM Build script for Magnifier API version
REM The Magnification API renders above EVERYTHING including cursor, taskbar, Start menu

cd /d "%~dp0"

echo.
echo === Desktop Capture Build Script (Magnification API) ===
echo.
echo This version uses the Windows Magnification API which renders
echo above ALL Windows UI elements including the cursor and taskbar.
echo.

if not exist bin mkdir bin

echo Building with MSVC...
echo.

REM Try to find Visual Studio
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set VSINSTALL=%%i

if defined VSINSTALL (
    call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)

cl /std:c++17 /O2 /EHsc /W3 main_magnifier.cpp /Fe:bin\DesktopCaptureMag.exe /link magnification.lib user32.lib gdi32.lib winmm.lib /SUBSYSTEM:WINDOWS

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build failed!
    echo.
    pause
    exit /b 1
)

echo.
echo Build successful! Executable: bin\DesktopCaptureMag.exe
echo Press Insert to exit the application when running.
echo.
pause
exit /b 0
