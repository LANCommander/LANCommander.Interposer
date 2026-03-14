#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

struct Rect {
    int left;
    int top;
    int width;
    int height;
};

// Returns the work area (usable desktop area) of the monitor that contains the given window.
Rect GetMonitorRect(HMONITOR hMonitor);
