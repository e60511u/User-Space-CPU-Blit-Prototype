// Monitor detection utility
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
    static int monitorNum = 0;
    monitorNum++;
    
    MONITORINFOEXA mi;
    mi.cbSize = sizeof(MONITORINFOEXA);
    
    if (GetMonitorInfoA(hMonitor, (MONITORINFO*)&mi))
    {
        DEVMODEA dm;
        dm.dmSize = sizeof(DEVMODEA);
        
        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        {
            printf("Monitor %d: %s\n", monitorNum, mi.szDevice);
            printf("  Position: (%ld, %ld)\n", dm.dmPosition.x, dm.dmPosition.y);
            printf("  Resolution: %lu x %lu\n", dm.dmPelsWidth, dm.dmPelsHeight);
            printf("  Refresh Rate: %lu Hz\n", dm.dmDisplayFrequency);
            printf("  Primary: %s\n", (mi.dwFlags & MONITORINFOF_PRIMARY) ? "Yes" : "No");
            printf("\n");
        }
    }
    
    return TRUE;
}

int main()
{
    printf("=== Monitor Configuration ===\n\n");
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
    
    printf("Press Enter to exit...");
    getchar();
    return 0;
}
