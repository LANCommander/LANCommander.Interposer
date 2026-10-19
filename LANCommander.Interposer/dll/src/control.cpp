#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>
#include <vector>

#include "control.h"
#include "config.h"
#include "relay.h"

namespace
{

HANDLE g_pipe    = INVALID_HANDLE_VALUE;
HANDLE g_thread  = nullptr;
HANDLE g_stop    = nullptr;   // manual-reset; set to end the thread
HANDLE g_parked  = nullptr;   // set by the thread once it is safe to terminate

// One overlapped transfer of exactly `size` bytes, abandoned if g_stop fires.
bool Transfer(bool write, void* buffer, DWORD size)
{
    BYTE* p = static_cast<BYTE*>(buffer);

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return false;

    bool ok = true;

    while (size > 0)
    {
        ResetEvent(ov.hEvent);

        DWORD done = 0;
        BOOL  r = write ? WriteFile(g_pipe, p, size, &done, &ov)
                        : ReadFile(g_pipe, p, size, &done, &ov);

        if (!r && GetLastError() == ERROR_IO_PENDING)
        {
            HANDLE waits[2] = { ov.hEvent, g_stop };
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
            {
                CancelIo(g_pipe);
                WaitForSingleObject(ov.hEvent, 1000);
                ok = false;
                break;
            }
            r = GetOverlappedResult(g_pipe, &ov, &done, FALSE);
        }

        if (!r || done == 0)
        {
            ok = false;
            break;
        }

        p    += done;
        size -= done;
    }

    CloseHandle(ov.hEvent);
    return ok;
}

bool ReadU32(DWORD& value)        { return Transfer(false, &value, sizeof(value)); }
bool WriteBytes(const void* p, DWORD n) { return Transfer(true, const_cast<void*>(p), n); }

bool Respond(DWORD requestId, bool ok, const std::wstring& payload)
{
    std::vector<BYTE> message(12 + payload.size() * sizeof(wchar_t));

    const DWORD status = ok ? 1u : 0u;
    const DWORD chars  = static_cast<DWORD>(payload.size());

    memcpy(message.data() + 0, &requestId, 4);
    memcpy(message.data() + 4, &status,    4);
    memcpy(message.data() + 8, &chars,     4);
    if (chars)
        memcpy(message.data() + 12, payload.data(), payload.size() * sizeof(wchar_t));

    return WriteBytes(message.data(), static_cast<DWORD>(message.size()));
}

void Handle(DWORD requestId, DWORD command, const std::wstring& payload)
{
    switch (command)
    {
    case CONTROL_PING:
        Respond(requestId, true, L"pong");
        break;

    case CONTROL_RELAY_CONNECT:
        Respond(requestId, RelayConnect(payload), {});
        break;

    case CONTROL_RELAY_DISCONNECT:
        RelayDisconnect();
        Respond(requestId, true, {});
        break;

    case CONTROL_RELAY_STATUS:
        Respond(requestId, true, RelayStatusJson());
        break;

    case CONTROL_GET_CONFIG:
    {
        wchar_t value[1024]{};
        const bool ok = InterposerGetConfigString(payload.c_str(), value, ARRAYSIZE(value)) != FALSE;
        Respond(requestId, ok, ok ? std::wstring(value) : std::wstring());
        break;
    }

    default:
        Respond(requestId, false, L"unknown command");
        break;
    }
}

DWORD WINAPI ControlThread(LPVOID)
{
    for (;;)
    {
        DWORD requestId = 0, command = 0, chars = 0;

        if (!ReadU32(requestId) || !ReadU32(command) || !ReadU32(chars))
            break;

        // A request bigger than this is not one the host would send.
        if (chars > 1024 * 1024)
            break;

        std::wstring payload(chars, L'\0');
        if (chars && !Transfer(false, payload.data(), chars * sizeof(wchar_t)))
            break;

        Handle(requestId, command, payload);
    }

    // See RelayThread: park rather than return into a module that may be
    // about to be unmapped.
    SetEvent(g_parked);
    Sleep(INFINITE);
    return 0;
}

} // namespace

void StartControlPipe()
{
    wchar_t name[128]{};
    wsprintfW(name, L"\\\\.\\pipe\\InterposerControl_%lu", GetCurrentProcessId());

    HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return; // no launcher listening -- not an error

    g_pipe   = h;
    g_stop   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_parked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
}

void StopControlPipe(bool processTerminating)
{
    if (!g_thread || processTerminating)
        return;

    SetEvent(g_stop);

    const bool parked = WaitForSingleObject(g_parked, 3000) == WAIT_OBJECT_0;
    TerminateThread(g_thread, 0);

    CloseHandle(g_thread);
    g_thread = nullptr;

    if (!parked)
        return;

    CloseHandle(g_pipe);
    CloseHandle(g_stop);
    CloseHandle(g_parked);
    g_pipe   = INVALID_HANDLE_VALUE;
    g_stop   = nullptr;
    g_parked = nullptr;
}
