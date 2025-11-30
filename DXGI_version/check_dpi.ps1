Add-Type -AssemblyName System.Windows.Forms

Write-Host "=== Monitor Information ===" -ForegroundColor Cyan
$screens = [System.Windows.Forms.Screen]::AllScreens
foreach ($s in $screens) {
    Write-Host "`nDevice: $($s.DeviceName)"
    Write-Host "  Bounds: X=$($s.Bounds.X), Y=$($s.Bounds.Y), Width=$($s.Bounds.Width), Height=$($s.Bounds.Height)"
    Write-Host "  WorkingArea: X=$($s.WorkingArea.X), Y=$($s.WorkingArea.Y), Width=$($s.WorkingArea.Width), Height=$($s.WorkingArea.Height)"
    Write-Host "  Primary: $($s.Primary)"
}

Write-Host "`n=== DPI Information ===" -ForegroundColor Cyan

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class DPIHelper {
    [DllImport("user32.dll")]
    public static extern int GetDpiForSystem();
    
    [DllImport("user32.dll")]
    public static extern bool SetProcessDPIAware();
    
    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int nIndex);
}
"@

$sysDpi = [DPIHelper]::GetDpiForSystem()
$scalingPercent = [math]::Round(($sysDpi / 96.0) * 100)
Write-Host "System DPI: $sysDpi ($scalingPercent% scaling)"

# Get actual screen resolution via GetSystemMetrics
$SM_CXSCREEN = 0
$SM_CYSCREEN = 1
$realWidth = [DPIHelper]::GetSystemMetrics($SM_CXSCREEN)
$realHeight = [DPIHelper]::GetSystemMetrics($SM_CYSCREEN)
Write-Host "Primary Screen (GetSystemMetrics): ${realWidth}x${realHeight}"

Write-Host "`n=== Recommendation ===" -ForegroundColor Yellow
if ($scalingPercent -gt 100) {
    Write-Host "DPI scaling is enabled at $scalingPercent%."
    Write-Host "This may cause coordinate mismatches in screen capture."
    Write-Host "Consider adding DPI awareness to the application."
} else {
    Write-Host "DPI scaling is at 100% (no scaling)."
}

Write-Host ""
Read-Host "Press Enter to exit"
