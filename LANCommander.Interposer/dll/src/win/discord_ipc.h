#pragma once

#include "richpresence.h"

// Discord rich presence backend using raw named-pipe IPC.
// No external dependencies — communicates directly with the Discord client
// via \\.\pipe\discord-ipc-{0..9} using length-prefixed JSON frames.
class DiscordIpcBackend : public IRichPresenceBackend
{
public:
    explicit DiscordIpcBackend(const std::string& applicationId);
    ~DiscordIpcBackend() override;

    bool Connect() override;
    void Disconnect() override;
    bool IsConnected() override;
    void UpdatePresence(const PresenceData& data) override;
    void ClearPresence() override;

private:
    // IPC frame opcodes
    enum Opcode : uint32_t
    {
        OP_HANDSHAKE = 0,
        OP_FRAME     = 1,
        OP_CLOSE     = 2,
        OP_PING      = 3,
        OP_PONG      = 4,
    };

    // Send a framed message (opcode + length + payload).
    bool SendFrame(Opcode opcode, const std::string& json);

    // Read one frame from the pipe. Returns false on error.
    bool ReadFrame(Opcode& outOpcode, std::string& outJson);

    // Build the SET_ACTIVITY JSON payload.
    static std::string BuildActivityJson(const PresenceData& data, DWORD pid);

    // Escape a string for safe embedding in JSON.
    static std::string JsonEscape(const std::string& s);

    std::string m_applicationId;
    HANDLE      m_pipe      = INVALID_HANDLE_VALUE;
    int         m_nonce     = 1;
};
