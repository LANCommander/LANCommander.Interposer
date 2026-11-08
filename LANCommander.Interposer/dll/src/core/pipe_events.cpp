#include "pipe_events.h"
#include "platform.h"

#include <atomic>
#include <cwchar>
#include <vector>

// ---------------------------------------------------------------------------
// Client for sending hook events back to the launcher, over the channel the
// platform provides (a named pipe on Windows, a Unix socket elsewhere).
//
// Wire format per message (all little-endian):
//   int32   eventType      (PipeEventType)
//   int32   field1Len      (UTF-16 code units, excluding NUL)
//   char16[] field1        (UTF-16 LE)
//   int32   field2Len
//   char16[] field2
//   int32   field3Len
//   char16[] field3
//   int32   intField       (port for NETWORK, 0 otherwise)
//
// UTF-16 everywhere, whatever wchar_t is, because the launcher reads it that
// way on every platform.
//
// Thread safety: g_eventChannel is an atomic so that multiple hook threads can
// call SendPipeEvent concurrently. On a broken channel only the first thread to
// swap the handle out closes it; the rest see the swap and stop writing.
// ---------------------------------------------------------------------------

static std::atomic<platform::FileHandle> g_eventChannel{ platform::kInvalidFile };

// ---------------------------------------------------------------------------
void ConnectEventPipe()
{
    platform::FileHandle channel = platform::ConnectEventChannel();

    if (channel == platform::kInvalidFile)
        return; // the launcher offered no channel -- not an error

    g_eventChannel.exchange(channel);
}

// ---------------------------------------------------------------------------
void DisconnectEventPipe()
{
    platform::FileHandle channel = g_eventChannel.exchange(platform::kInvalidFile);

    if (channel != platform::kInvalidFile)
        platform::CloseFile(channel);
}

// ---------------------------------------------------------------------------
bool EventPipeConnected()
{
    return g_eventChannel.load() != platform::kInvalidFile;
}

// ---------------------------------------------------------------------------
static void WriteInt32(std::vector<unsigned char>& buf, int value)
{
    const auto v = static_cast<uint32_t>(value);

    for (int shift = 0; shift < 32; shift += 8)
        buf.push_back(static_cast<unsigned char>(v >> shift));
}

static void WriteUnit(std::vector<unsigned char>& buf, uint32_t unit)
{
    buf.push_back(static_cast<unsigned char>(unit & 0xFF));
    buf.push_back(static_cast<unsigned char>(unit >> 8));
}

static void WriteWString(std::vector<unsigned char>& buf, const wchar_t* s)
{
    const size_t lengthAt = buf.size();
    WriteInt32(buf, 0);

    if (!s)
        return;

    int units = 0;

    for (; *s; ++s)
    {
        const auto cp = static_cast<uint32_t>(*s);

        // Only reachable where wchar_t is 32-bit: a code point outside the
        // BMP becomes a surrogate pair.
        if (cp > 0xFFFF && cp <= 0x10FFFF)
        {
            WriteUnit(buf, 0xD800 + ((cp - 0x10000) >> 10));
            WriteUnit(buf, 0xDC00 + ((cp - 0x10000) & 0x3FF));
            units += 2;
        }
        else
        {
            WriteUnit(buf, cp > 0x10FFFF ? 0xFFFD : cp);
            ++units;
        }
    }

    for (int shift = 0; shift < 32; shift += 8)
        buf[lengthAt + shift / 8] = static_cast<unsigned char>(static_cast<uint32_t>(units) >> shift);
}

// ---------------------------------------------------------------------------
void SendPipeEvent(PipeEventType type,
                   const wchar_t* field1,
                   const wchar_t* field2,
                   const wchar_t* field3,
                   int intField)
{
    // Fast path: skip serialization if the channel isn't connected.
    platform::FileHandle channel = g_eventChannel.load();
    if (channel == platform::kInvalidFile)
        return;

    // Serialize message.
    std::vector<unsigned char> buf;
    buf.reserve(256);

    WriteInt32(buf, static_cast<int>(type));
    WriteWString(buf, field1);
    WriteWString(buf, field2);
    WriteWString(buf, field3);
    WriteInt32(buf, intField);

    if (!platform::WriteEventChannel(channel, buf.data(), buf.size()))
    {
        // Channel broken — the launcher disconnected. Swap the handle out;
        // only the thread that wins the exchange closes it.
        platform::FileHandle expected = channel;

        if (g_eventChannel.compare_exchange_strong(expected, platform::kInvalidFile))
            platform::CloseFile(channel);
    }
}
