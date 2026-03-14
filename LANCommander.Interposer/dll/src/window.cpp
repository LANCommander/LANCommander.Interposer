#include "window.h"

Rect GetMonitorRect(HMONITOR hMonitor)
{
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (hMonitor && GetMonitorInfoW(hMonitor, &monitorInfo))
    {
        return {
            monitorInfo.rcWork.left,
            monitorInfo.rcWork.top,
            monitorInfo.rcWork.right  - monitorInfo.rcWork.left,
            monitorInfo.rcWork.bottom - monitorInfo.rcWork.top
        };
    }

    // Fallback: primary monitor via SystemMetrics
    return {
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN)
    };
}
