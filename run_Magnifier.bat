@echo off
cd /d "%~dp0"
echo Running Magnification API version...
echo This uses the Windows Magnification API which renders above everything.
echo Press Insert to exit.
echo.
DXGI_version\bin\DesktopCaptureMag.exe
