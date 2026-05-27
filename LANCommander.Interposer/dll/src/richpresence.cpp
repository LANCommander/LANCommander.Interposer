#include "richpresence.h"
#include "config.h"
#include "discord_ipc.h"

#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Manager state
// ---------------------------------------------------------------------------
static std::mutex                                     g_rpMutex;
static PresenceData                                   g_presence;
static PresenceData                                   g_defaults;  // populated from Config.yml
static std::vector<std::unique_ptr<IRichPresenceBackend>> g_backends;
static bool                                           g_rpInitialized = false;

// Forward declarations for config globals defined in config.cpp
extern bool        g_rpDiscordEnabled;
extern std::string g_rpDiscordAppId;
extern int         g_rpDefaultType;
extern std::string g_rpDefaultName;
extern std::string g_rpDefaultDetails;
extern std::string g_rpDefaultDetailsUrl;
extern std::string g_rpDefaultState;
extern std::string g_rpDefaultStateUrl;
extern std::string g_rpDefaultLargeImageKey;
extern std::string g_rpDefaultLargeImageText;
extern std::string g_rpDefaultSmallImageKey;
extern std::string g_rpDefaultSmallImageText;
extern std::string g_rpDefaultButton1Text;
extern std::string g_rpDefaultButton1Url;
extern std::string g_rpDefaultButton2Text;
extern std::string g_rpDefaultButton2Url;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string SafeStr(const char* s) { return s ? s : ""; }

// Push current g_presence to all connected backends.
// Caller must hold g_rpMutex.
static void FlushToBackends()
{
    for (auto& backend : g_backends)
        backend->UpdatePresence(g_presence);
}

// ---------------------------------------------------------------------------
// InitRichPresence
// ---------------------------------------------------------------------------
void InitRichPresence()
{
    std::lock_guard<std::mutex> lk(g_rpMutex);

    if (g_rpInitialized)
        return;

    // Build defaults from config
    g_defaults.type           = g_rpDefaultType;
    g_defaults.name           = g_rpDefaultName;
    g_defaults.details        = g_rpDefaultDetails;
    g_defaults.detailsUrl     = g_rpDefaultDetailsUrl;
    g_defaults.state          = g_rpDefaultState;
    g_defaults.stateUrl       = g_rpDefaultStateUrl;
    g_defaults.largeImageKey  = g_rpDefaultLargeImageKey;
    g_defaults.largeImageText = g_rpDefaultLargeImageText;
    g_defaults.smallImageKey  = g_rpDefaultSmallImageKey;
    g_defaults.smallImageText = g_rpDefaultSmallImageText;
    g_defaults.button1Text    = g_rpDefaultButton1Text;
    g_defaults.button1Url     = g_rpDefaultButton1Url;
    g_defaults.button2Text    = g_rpDefaultButton2Text;
    g_defaults.button2Url     = g_rpDefaultButton2Url;

    g_presence = g_defaults;

    // Create Discord backend if configured
    if (g_rpDiscordEnabled && !g_rpDiscordAppId.empty())
    {
        auto discord = std::make_unique<DiscordIpcBackend>(g_rpDiscordAppId);

        if (discord->Connect())
        {
            discord->UpdatePresence(g_presence);
            g_backends.push_back(std::move(discord));
        }
        else
        {
            InterposerLog(L"RICH PRESENCE", L"Discord IPC connection failed (is Discord running?)");
        }
    }

    g_rpInitialized = true;
}

// ---------------------------------------------------------------------------
// ShutdownRichPresence
// ---------------------------------------------------------------------------
void ShutdownRichPresence()
{
    std::lock_guard<std::mutex> lk(g_rpMutex);

    for (auto& backend : g_backends)
    {
        backend->ClearPresence();
        backend->Disconnect();
    }

    g_backends.clear();
    g_rpInitialized = false;
}

// ---------------------------------------------------------------------------
// Plugin API exports
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport)
void InterposerSetPresenceDetails(const char* details)
{
    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.details = SafeStr(details);
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceState(const char* state)
{
    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.state = SafeStr(state);
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceTimestamps(int64_t start, int64_t end)
{
    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.timestampStart = start;
    g_presence.timestampEnd   = end;
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceLargeImage(const char* key, const char* text)
{
    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.largeImageKey  = SafeStr(key);
    g_presence.largeImageText = SafeStr(text);
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceSmallImage(const char* key, const char* text)
{
    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.smallImageKey  = SafeStr(key);
    g_presence.smallImageText = SafeStr(text);
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceParty(const char* id, int size, int max)
{
    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.partyId   = SafeStr(id);
    g_presence.partySize = size;
    g_presence.partyMax  = max;
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceButton(int index, const char* text, const char* url)
{
    std::lock_guard<std::mutex> lk(g_rpMutex);

    if (index == 0)
    {
        g_presence.button1Text = SafeStr(text);
        g_presence.button1Url  = SafeStr(url);
    }
    else if (index == 1)
    {
        g_presence.button2Text = SafeStr(text);
        g_presence.button2Url  = SafeStr(url);
    }
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceType(int type)
{
    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.type = type;
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceName(const char* name)
{
    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.name = SafeStr(name);
}

extern "C" __declspec(dllexport)
void InterposerUpdatePresence()
{
    std::lock_guard<std::mutex> lk(g_rpMutex);
    FlushToBackends();
}

extern "C" __declspec(dllexport)
void InterposerClearPresence()
{
    std::lock_guard<std::mutex> lk(g_rpMutex);

    // Reset to config defaults
    g_presence = g_defaults;

    for (auto& backend : g_backends)
        backend->ClearPresence();
}
