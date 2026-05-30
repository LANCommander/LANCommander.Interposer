#include "pipe_events.h"
#include <vector>

// ---------------------------------------------------------------------------
// Named-pipe client for sending hook events back to the host process.
//
// Wire format per message (all little-endian):
//   int32   eventType      (PipeEventType)
//   int32   field1Len      (wchar_t count, excluding NUL)
//   wchar[] field1
//   int32   field2Len
//   wchar[] field2
//   int32   field3Len
//   wchar[] field3
//   int32   intField       (port for NETWORK, 0 otherwise)
//
// Thread safety: g_eventPipe is accessed via Interlocked* functions so that
// multiple hook threads can call SendPipeEvent concurrently.  On a broken
// pipe only the first thread to CAS the handle to INVALID_HANDLE_VALUE
// will close it; the rest see the swap and stop writing.
// ---------------------------------------------------------------------------

static volatile HANDLE g_eventPipe = INVALID_HANDLE_VALUE;

// ---------------------------------------------------------------------------
void ConnectEventPipe()
{
    DWORD pid = GetCurrentProcessId();

    wchar_t pipeName[128]{};
    wsprintfW(pipeName, L"\\\\.\\pipe\\InterposerEvents_%lu", pid);

    HANDLE h = CreateFileW(pipeName,
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           OPEN_EXISTING,
                           0,
                           nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return; // host didn't create a pipe — not an error

    InterlockedExchangePointer(
        reinterpret_cast<PVOID*>(const_cast<HANDLE*>(&g_eventPipe)), h);
}

// ---------------------------------------------------------------------------
void DisconnectEventPipe()
{
    HANDLE h = static_cast<HANDLE>(InterlockedExchangePointer(
        reinterpret_cast<PVOID*>(const_cast<HANDLE*>(&g_eventPipe)),
        INVALID_HANDLE_VALUE));

    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

// ---------------------------------------------------------------------------
static void WriteInt32(std::vector<BYTE>& buf, int value)
{
    const BYTE* p = reinterpret_cast<const BYTE*>(&value);
    buf.insert(buf.end(), p, p + sizeof(int));
}

static void WriteWString(std::vector<BYTE>& buf, const wchar_t* s)
{
    int len = s ? static_cast<int>(wcslen(s)) : 0;
    WriteInt32(buf, len);

    if (len > 0)
    {
        const BYTE* p = reinterpret_cast<const BYTE*>(s);
        buf.insert(buf.end(), p, p + len * sizeof(wchar_t));
    }
}

// ---------------------------------------------------------------------------
void SendPipeEvent(PipeEventType type,
                   const wchar_t* field1,
                   const wchar_t* field2,
                   const wchar_t* field3,
                   int intField)
{
    // Fast path: skip serialization if pipe isn't connected.
    HANDLE h = g_eventPipe;
    if (h == INVALID_HANDLE_VALUE)
        return;

    // Serialize message.
    std::vector<BYTE> buf;
    buf.reserve(256);

    WriteInt32(buf, static_cast<int>(type));
    WriteWString(buf, field1);
    WriteWString(buf, field2);
    WriteWString(buf, field3);
    WriteInt32(buf, intField);

    DWORD written = 0;
    if (!WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr))
    {
        // Pipe broken — host disconnected. Atomically swap the handle out;
        // only the thread that wins the CAS closes it.
        HANDLE old = static_cast<HANDLE>(InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID*>(const_cast<HANDLE*>(&g_eventPipe)),
            INVALID_HANDLE_VALUE, h));

        if (old == h)
            CloseHandle(h);
    }
}
