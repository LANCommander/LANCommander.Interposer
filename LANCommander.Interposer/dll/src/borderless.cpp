#include "borderless.h"
#include "config.h"
#include "window.h"

#include <MinHook.h>
#include <atomic>

// ---------------------------------------------------------------------------
// Tracked state
// ---------------------------------------------------------------------------
static std::atomic<HWND> g_mainWindow{ nullptr };

// ---------------------------------------------------------------------------
// Trampoline (original function) pointers
// ---------------------------------------------------------------------------
using FnCreateWindowExW = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using FnCreateWindowExA = HWND(WINAPI*)(DWORD, LPCSTR,  LPCSTR,  DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using FnSetWindowPos    = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using FnSetWindowLongW  = LONG(WINAPI*)(HWND, int, LONG);
using FnSetWindowLongA  = LONG(WINAPI*)(HWND, int, LONG);

static FnCreateWindowExW g_origCreateWindowExW = nullptr;
static FnCreateWindowExA g_origCreateWindowExA = nullptr;
static FnSetWindowPos    g_origSetWindowPos    = nullptr;
static FnSetWindowLongW  g_origSetWindowLongW  = nullptr;
static FnSetWindowLongA  g_origSetWindowLongA  = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool IsTopLevelGameWindow(HWND parent, int cx, int cy, DWORD style)
{
    return (parent == nullptr)
        && (cx > 0 && cy > 0)
        && (style & (WS_OVERLAPPEDWINDOW | WS_POPUP));
}

// Strip border bits and center the window on its monitor.
// Always calls through trampolines so we never re-enter our own hooks.
static void ApplyBorderless(HWND hwnd, int width, int height)
{
    // --- Strip style border bits ---
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    style &= ~(WS_THICKFRAME | WS_DLGFRAME | WS_BORDER);

    if (g_origSetWindowLongW)
        g_origSetWindowLongW(hwnd, GWL_STYLE, style);
    else
        SetWindowLongW(hwnd, GWL_STYLE, style);

    // --- Strip extended style border bits ---
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);

    if (g_origSetWindowLongW)
        g_origSetWindowLongW(hwnd, GWL_EXSTYLE, exStyle);
    else
        SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle);

    // --- Center on current monitor ---
    HMONITOR monitorHandle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    Rect monitor = GetMonitorRect(monitorHandle);

    int x = monitor.left + (monitor.width  - width)  / 2;
    int y = monitor.top  + (monitor.height - height) / 2;

    constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;

    if (g_origSetWindowPos)
        g_origSetWindowPos(hwnd, nullptr, x, y, width, height, flags);
    else
        SetWindowPos(hwnd, nullptr, x, y, width, height, flags);

    // Prompt game to redraw client area
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
}

// ---------------------------------------------------------------------------
// Hook implementations
// ---------------------------------------------------------------------------
static HWND WINAPI HookCreateWindowExW(
    DWORD     dwExStyle,
    LPCWSTR   lpClassName,
    LPCWSTR   lpWindowName,
    DWORD     dwStyle,
    int       X, int Y,
    int       nWidth, int nHeight,
    HWND      hWndParent,
    HMENU     hMenu,
    HINSTANCE hInstance,
    LPVOID    lpParam)
{
    if (IsTopLevelGameWindow(hWndParent, nWidth, nHeight, dwStyle))
    {
        // Strip border flags before the window is even created
        dwStyle   &= ~(WS_THICKFRAME | WS_DLGFRAME | WS_BORDER);
        dwExStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    }

    HWND hwnd = g_origCreateWindowExW(
        dwExStyle, lpClassName, lpWindowName, dwStyle,
        X, Y, nWidth, nHeight,
        hWndParent, hMenu, hInstance, lpParam);

    if (hwnd && IsTopLevelGameWindow(hWndParent, nWidth, nHeight, dwStyle))
    {
        g_mainWindow.store(hwnd);
        ApplyBorderless(hwnd, nWidth, nHeight);
    }

    return hwnd;
}

static HWND WINAPI HookCreateWindowExA(
    DWORD     dwExStyle,
    LPCSTR    lpClassName,
    LPCSTR    lpWindowName,
    DWORD     dwStyle,
    int       X, int Y,
    int       nWidth, int nHeight,
    HWND      hWndParent,
    HMENU     hMenu,
    HINSTANCE hInstance,
    LPVOID    lpParam)
{
    if (IsTopLevelGameWindow(hWndParent, nWidth, nHeight, dwStyle))
    {
        dwStyle   &= ~(WS_THICKFRAME | WS_DLGFRAME | WS_BORDER);
        dwExStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    }

    HWND hwnd = g_origCreateWindowExA(
        dwExStyle, lpClassName, lpWindowName, dwStyle,
        X, Y, nWidth, nHeight,
        hWndParent, hMenu, hInstance, lpParam);

    if (hwnd && IsTopLevelGameWindow(hWndParent, nWidth, nHeight, dwStyle))
    {
        g_mainWindow.store(hwnd);
        ApplyBorderless(hwnd, nWidth, nHeight);
    }

    return hwnd;
}

static BOOL WINAPI HookSetWindowPos(
    HWND hwnd, HWND hWndInsertAfter,
    int X, int Y, int cx, int cy,
    UINT uFlags)
{
    HWND tracked = g_mainWindow.load();
    if (hwnd == tracked && tracked != nullptr)
    {
        if (!(uFlags & SWP_NOSIZE) && cx > 0 && cy > 0)
        {
            // Re-center whenever the game requests a resize
            HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            Rect mon = GetMonitorRect(hMon);
            X = mon.left + (mon.width  - cx) / 2;
            Y = mon.top  + (mon.height - cy) / 2;
            uFlags &= ~SWP_NOMOVE;  // ensure our recalculated position is applied
        }
        else
        {
            // Suppress arbitrary moves when size isn't changing
            uFlags |= SWP_NOMOVE;
        }
    }

    return g_origSetWindowPos(hwnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

static LONG WINAPI HookSetWindowLongW(HWND hwnd, int nIndex, LONG dwNewLong)
{
    HWND tracked = g_mainWindow.load();
    if (hwnd == tracked && tracked != nullptr)
    {
        if (nIndex == GWL_STYLE)
        {
            // Prevent game re-adding border bits
            dwNewLong &= ~(WS_THICKFRAME | WS_DLGFRAME | WS_BORDER);
        }
        else if (nIndex == GWL_EXSTYLE)
        {
            dwNewLong &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        }
    }

    return g_origSetWindowLongW(hwnd, nIndex, dwNewLong);
}

static LONG WINAPI HookSetWindowLongA(HWND hwnd, int nIndex, LONG dwNewLong)
{
    HWND tracked = g_mainWindow.load();
    if (hwnd == tracked && tracked != nullptr)
    {
        if (nIndex == GWL_STYLE)
            dwNewLong &= ~(WS_THICKFRAME | WS_DLGFRAME | WS_BORDER);
        else if (nIndex == GWL_EXSTYLE)
            dwNewLong &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    }

    return g_origSetWindowLongA(hwnd, nIndex, dwNewLong);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InstallBorderlessHooks()
{
    LogHookInit(L"user32", "CreateWindowExW",
        MH_CreateHookApi(L"user32", "CreateWindowExW",
            reinterpret_cast<LPVOID>(HookCreateWindowExW),
            reinterpret_cast<LPVOID*>(&g_origCreateWindowExW)));

    LogHookInit(L"user32", "CreateWindowExA",
        MH_CreateHookApi(L"user32", "CreateWindowExA",
            reinterpret_cast<LPVOID>(HookCreateWindowExA),
            reinterpret_cast<LPVOID*>(&g_origCreateWindowExA)));

    LogHookInit(L"user32", "SetWindowPos",
        MH_CreateHookApi(L"user32", "SetWindowPos",
            reinterpret_cast<LPVOID>(HookSetWindowPos),
            reinterpret_cast<LPVOID*>(&g_origSetWindowPos)));

    LogHookInit(L"user32", "SetWindowLongW",
        MH_CreateHookApi(L"user32", "SetWindowLongW",
            reinterpret_cast<LPVOID>(HookSetWindowLongW),
            reinterpret_cast<LPVOID*>(&g_origSetWindowLongW)));

    LogHookInit(L"user32", "SetWindowLongA",
        MH_CreateHookApi(L"user32", "SetWindowLongA",
            reinterpret_cast<LPVOID>(HookSetWindowLongA),
            reinterpret_cast<LPVOID*>(&g_origSetWindowLongA)));
}

void ForceBorderless(HWND hwnd)
{
    if (!hwnd) return;

    RECT wr{};
    GetWindowRect(hwnd, &wr);
    int w = wr.right  - wr.left;
    int h = wr.bottom - wr.top;

    g_mainWindow.store(hwnd);
    ApplyBorderless(hwnd, w, h);
}
