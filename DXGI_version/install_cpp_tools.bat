@echo off
echo Installing Visual Studio Build Tools C++ workload...
echo This requires administrator privileges.
echo.

"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe" modify --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --passive --wait

echo.
echo Installation complete! You may need to restart your terminal.
pause
