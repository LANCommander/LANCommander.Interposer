#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// All fields that a rich presence backend may consume.
// Not every backend supports every field — backends ignore what they don't use.
struct PresenceData
{
    int         type            = 0;    // 0 Playing, 1 Streaming, 2 Listening, 3 Watching, 5 Competing
    std::string name;                   // Display name (shown as "Playing <name>")
    std::string details;                // First detail line
    std::string detailsUrl;             // URL associated with details (backend-dependent)
    std::string state;                  // Second detail line
    std::string stateUrl;               // URL associated with state (backend-dependent)
    int64_t     timestampStart  = 0;    // Unix epoch seconds; 0 = not set
    int64_t     timestampEnd    = 0;    // Unix epoch seconds; 0 = not set
    std::string largeImageKey;          // Asset key or URL
    std::string largeImageText;         // Tooltip on hover
    std::string smallImageKey;          // Asset key or URL
    std::string smallImageText;         // Tooltip on hover
    std::string partyId;                // Arbitrary party identifier
    int         partySize       = 0;    // Current party member count
    int         partyMax        = 0;    // Maximum party size
    std::string button1Text;            // Label for button 1 (max 32 chars on Discord)
    std::string button1Url;             // URL opened when button 1 is clicked
    std::string button2Text;            // Label for button 2
    std::string button2Url;             // URL opened when button 2 is clicked
};

// Abstract backend interface. Implement this for each rich presence service.
class IRichPresenceBackend
{
public:
    virtual ~IRichPresenceBackend() = default;

    // Attempt to connect to the service. Returns true on success.
    virtual bool Connect() = 0;

    // Disconnect gracefully.
    virtual void Disconnect() = 0;

    // Returns true if currently connected.
    virtual bool IsConnected() = 0;

    // Push the current presence state to the service.
    virtual void UpdatePresence(const PresenceData& data) = 0;

    // Clear the presence (remove activity from the service).
    virtual void ClearPresence() = 0;
};

// Initialize the rich presence subsystem. Reads config, connects backends,
// and sends the default presence from Config.yml.
void InitRichPresence();

// Disconnect all backends and release resources.
void ShutdownRichPresence();

// ---------------------------------------------------------------------------
// Plugin API — exported by name, resolved by plugins via GetProcAddress.
// All strings are UTF-8.
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) void InterposerSetPresenceDetails(const char* details);
extern "C" __declspec(dllexport) void InterposerSetPresenceState(const char* state);
extern "C" __declspec(dllexport) void InterposerSetPresenceTimestamps(int64_t start, int64_t end);
extern "C" __declspec(dllexport) void InterposerSetPresenceLargeImage(const char* key, const char* text);
extern "C" __declspec(dllexport) void InterposerSetPresenceSmallImage(const char* key, const char* text);
extern "C" __declspec(dllexport) void InterposerSetPresenceParty(const char* id, int size, int max);
extern "C" __declspec(dllexport) void InterposerSetPresenceButton(int index, const char* text, const char* url);
extern "C" __declspec(dllexport) void InterposerSetPresenceType(int type);
extern "C" __declspec(dllexport) void InterposerSetPresenceName(const char* name);
extern "C" __declspec(dllexport) void InterposerUpdatePresence();
extern "C" __declspec(dllexport) void InterposerClearPresence();
