#include "discord_ipc.h"
#include "config.h"

#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
DiscordIpcBackend::DiscordIpcBackend(const std::string& applicationId)
    : m_applicationId(applicationId)
{
}

DiscordIpcBackend::~DiscordIpcBackend()
{
    Disconnect();
}

// ---------------------------------------------------------------------------
// Connect — try discord-ipc-0 through discord-ipc-9
// ---------------------------------------------------------------------------
bool DiscordIpcBackend::Connect()
{
    if (m_pipe != INVALID_HANDLE_VALUE)
        return true;

    for (int i = 0; i < 10; ++i)
    {
        char pipeName[64];
        std::snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\discord-ipc-%d", i);

        HANDLE h = CreateFileA(
            pipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (h != INVALID_HANDLE_VALUE)
        {
            m_pipe = h;
            break;
        }
    }

    if (m_pipe == INVALID_HANDLE_VALUE)
        return false;

    // Send handshake: { "v": 1, "client_id": "<app_id>" }
    std::string handshake = "{\"v\":1,\"client_id\":\"" + JsonEscape(m_applicationId) + "\"}";

    if (!SendFrame(OP_HANDSHAKE, handshake))
    {
        Disconnect();
        return false;
    }

    // Read the READY response (or any dispatch). We don't need the payload,
    // but we must drain the pipe so subsequent writes don't stall.
    Opcode op{};
    std::string response;

    if (!ReadFrame(op, response))
    {
        Disconnect();
        return false;
    }

    InterposerLog(L"RICH PRESENCE", L"Discord IPC connected");

    return true;
}

// ---------------------------------------------------------------------------
// Disconnect
// ---------------------------------------------------------------------------
void DiscordIpcBackend::Disconnect()
{
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        // Send a close frame (best-effort)
        SendFrame(OP_CLOSE, "{}");
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

// ---------------------------------------------------------------------------
// IsConnected
// ---------------------------------------------------------------------------
bool DiscordIpcBackend::IsConnected()
{
    return m_pipe != INVALID_HANDLE_VALUE;
}

// ---------------------------------------------------------------------------
// UpdatePresence
// ---------------------------------------------------------------------------
void DiscordIpcBackend::UpdatePresence(const PresenceData& data)
{
    if (m_pipe == INVALID_HANDLE_VALUE)
    {
        // Try to reconnect (Discord may have restarted)
        if (!Connect())
            return;
    }

    std::string json = BuildActivityJson(data, GetCurrentProcessId());

    if (!SendFrame(OP_FRAME, json))
    {
        // Pipe broken — close and try once more
        Disconnect();

        if (!Connect())
            return;

        if (!SendFrame(OP_FRAME, json))
            Disconnect();
    }
}

// ---------------------------------------------------------------------------
// ClearPresence
// ---------------------------------------------------------------------------
void DiscordIpcBackend::ClearPresence()
{
    if (m_pipe == INVALID_HANDLE_VALUE)
        return;

    char nonceBuf[32];
    std::snprintf(nonceBuf, sizeof(nonceBuf), "%d", m_nonce++);

    std::string json =
        "{\"cmd\":\"SET_ACTIVITY\","
        "\"nonce\":\"" + std::string(nonceBuf) + "\","
        "\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) + "}}";

    if (!SendFrame(OP_FRAME, json))
        Disconnect();
}

// ---------------------------------------------------------------------------
// SendFrame — 4-byte LE opcode + 4-byte LE length + payload
// ---------------------------------------------------------------------------
bool DiscordIpcBackend::SendFrame(Opcode opcode, const std::string& json)
{
    if (m_pipe == INVALID_HANDLE_VALUE)
        return false;

    uint32_t header[2];
    header[0] = static_cast<uint32_t>(opcode);
    header[1] = static_cast<uint32_t>(json.size());

    DWORD written = 0;

    if (!WriteFile(m_pipe, header, sizeof(header), &written, nullptr) || written != sizeof(header))
        return false;

    if (!json.empty())
    {
        if (!WriteFile(m_pipe, json.c_str(), static_cast<DWORD>(json.size()), &written, nullptr)
            || written != static_cast<DWORD>(json.size()))
            return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// ReadFrame — read one framed response from the pipe
// ---------------------------------------------------------------------------
bool DiscordIpcBackend::ReadFrame(Opcode& outOpcode, std::string& outJson)
{
    uint32_t header[2]{};
    DWORD bytesRead = 0;

    if (!ReadFile(m_pipe, header, sizeof(header), &bytesRead, nullptr) || bytesRead != sizeof(header))
        return false;

    outOpcode = static_cast<Opcode>(header[0]);
    uint32_t length = header[1];

    if (length == 0)
    {
        outJson.clear();
        return true;
    }

    // Sanity check — Discord responses are typically small
    if (length > 64 * 1024)
        return false;

    outJson.resize(length);

    if (!ReadFile(m_pipe, outJson.data(), length, &bytesRead, nullptr) || bytesRead != length)
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// BuildActivityJson — assemble the SET_ACTIVITY command
// ---------------------------------------------------------------------------
std::string DiscordIpcBackend::BuildActivityJson(const PresenceData& data, DWORD pid)
{
    // We build JSON by hand because the schema is fixed and small.
    // This avoids pulling in a JSON library for a single use case.
    std::string activity;
    activity.reserve(1024);
    activity += "{";

    // Type
    if (data.type != 0)
    {
        char typeBuf[16];
        std::snprintf(typeBuf, sizeof(typeBuf), "%d", data.type);
        activity += "\"type\":";
        activity += typeBuf;
        activity += ",";
    }

    // Details
    if (!data.details.empty())
    {
        activity += "\"details\":\"" + JsonEscape(data.details) + "\",";
    }

    // State
    if (!data.state.empty())
    {
        activity += "\"state\":\"" + JsonEscape(data.state) + "\",";
    }

    // Timestamps
    if (data.timestampStart != 0 || data.timestampEnd != 0)
    {
        activity += "\"timestamps\":{";
        bool needComma = false;
        if (data.timestampStart != 0)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(data.timestampStart));
            activity += "\"start\":";
            activity += buf;
            needComma = true;
        }
        if (data.timestampEnd != 0)
        {
            if (needComma) activity += ",";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(data.timestampEnd));
            activity += "\"end\":";
            activity += buf;
        }
        activity += "},";
    }

    // Assets (images)
    if (!data.largeImageKey.empty() || !data.smallImageKey.empty())
    {
        activity += "\"assets\":{";
        bool needComma = false;

        if (!data.largeImageKey.empty())
        {
            activity += "\"large_image\":\"" + JsonEscape(data.largeImageKey) + "\"";
            needComma = true;
            if (!data.largeImageText.empty())
            {
                activity += ",\"large_text\":\"" + JsonEscape(data.largeImageText) + "\"";
            }
        }

        if (!data.smallImageKey.empty())
        {
            if (needComma) activity += ",";
            activity += "\"small_image\":\"" + JsonEscape(data.smallImageKey) + "\"";
            if (!data.smallImageText.empty())
            {
                activity += ",\"small_text\":\"" + JsonEscape(data.smallImageText) + "\"";
            }
        }

        activity += "},";
    }

    // Party
    if (!data.partyId.empty() || data.partySize > 0)
    {
        activity += "\"party\":{";
        bool needComma = false;

        if (!data.partyId.empty())
        {
            activity += "\"id\":\"" + JsonEscape(data.partyId) + "\"";
            needComma = true;
        }

        if (data.partySize > 0 && data.partyMax > 0)
        {
            if (needComma) activity += ",";
            char buf[64];
            std::snprintf(buf, sizeof(buf), "\"size\":[%d,%d]", data.partySize, data.partyMax);
            activity += buf;
        }

        activity += "},";
    }

    // Buttons (max 2)
    if (!data.button1Text.empty() && !data.button1Url.empty())
    {
        activity += "\"buttons\":[";
        activity += "{\"label\":\"" + JsonEscape(data.button1Text)
                  + "\",\"url\":\"" + JsonEscape(data.button1Url) + "\"}";

        if (!data.button2Text.empty() && !data.button2Url.empty())
        {
            activity += ",{\"label\":\"" + JsonEscape(data.button2Text)
                      + "\",\"url\":\"" + JsonEscape(data.button2Url) + "\"}";
        }

        activity += "],";
    }

    // Remove trailing comma
    if (!activity.empty() && activity.back() == ',')
        activity.pop_back();

    activity += "}";

    // Wrap in SET_ACTIVITY command
    // Use a simple incrementing nonce
    static int s_nonce = 1;
    char nonceBuf[32];
    std::snprintf(nonceBuf, sizeof(nonceBuf), "%d", s_nonce++);

    std::string cmd;
    cmd.reserve(activity.size() + 128);
    cmd += "{\"cmd\":\"SET_ACTIVITY\",";
    cmd += "\"nonce\":\"";
    cmd += nonceBuf;
    cmd += "\",\"args\":{\"pid\":";
    cmd += std::to_string(pid);
    cmd += ",\"activity\":";
    cmd += activity;
    cmd += "}}";

    return cmd;
}

// ---------------------------------------------------------------------------
// JsonEscape — escape a UTF-8 string for embedding in a JSON string literal
// ---------------------------------------------------------------------------
std::string DiscordIpcBackend::JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);

    for (char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                }
                else
                {
                    out += c;
                }
                break;
        }
    }

    return out;
}
