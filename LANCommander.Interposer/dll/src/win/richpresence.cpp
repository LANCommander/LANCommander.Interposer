#include "richpresence.h"
#include "instance.h"
#include "config.h"
#include "discord_ipc.h"
#include "callbacks.h"

#include <cstdio>

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

static std::wstring ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring out(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

// Push current g_presence to all connected backends.
// Caller must hold g_rpMutex.
static void FlushToBackends()
{
    for (auto& backend : g_backends)
        backend->UpdatePresence(g_presence);
}

static void AppendJsonString(std::string& out, const char* key, const std::string& value)
{
    out += '"';
    out += key;
    out += "\":\"";

    for (char c : value)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
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
        }
    }

    out += "\",";
}

static void AppendJsonNumber(std::string& out, const char* key, long long value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\"%s\":%lld,", key, value);
    out += buf;
}

// The presence as the JSON the launcher reads (PIPE_EVENT_PRESENCE "UPDATE").
// Image fields carry whatever was set -- a Discord asset key or a URL -- and
// the reader decides what it can show. Caller must hold g_rpMutex.
static std::wstring PresenceToJson(const PresenceData& data)
{
    std::string json = "{";

    AppendJsonNumber(json, "type",           data.type);
    AppendJsonString(json, "name",           data.name);
    AppendJsonString(json, "details",        data.details);
    AppendJsonString(json, "detailsUrl",     data.detailsUrl);
    AppendJsonString(json, "state",          data.state);
    AppendJsonString(json, "stateUrl",       data.stateUrl);
    AppendJsonNumber(json, "timestampStart", data.timestampStart);
    AppendJsonNumber(json, "timestampEnd",   data.timestampEnd);
    AppendJsonString(json, "largeImage",     data.largeImageKey);
    AppendJsonString(json, "largeImageText", data.largeImageText);
    AppendJsonString(json, "smallImage",     data.smallImageKey);
    AppendJsonString(json, "smallImageText", data.smallImageText);
    AppendJsonString(json, "partyId",        data.partyId);
    AppendJsonNumber(json, "partySize",      data.partySize);
    AppendJsonNumber(json, "partyMax",       data.partyMax);
    AppendJsonString(json, "button1Text",    data.button1Text);
    AppendJsonString(json, "button1Url",     data.button1Url);
    AppendJsonString(json, "button2Text",    data.button2Text);
    AppendJsonString(json, "button2Url",     data.button2Url);

    json.back() = '}'; // replaces the trailing comma

    return ToWide(json);
}

// ---------------------------------------------------------------------------
// InitRichPresence
// ---------------------------------------------------------------------------
void InitRichPresence()
{
    std::unique_lock<std::mutex> lk(g_rpMutex);

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
            LogRichPresence(L"RP INIT", L"Discord IPC connected");
            discord->UpdatePresence(g_presence);
            g_backends.push_back(std::move(discord));
        }
        else
        {
            InterposerLog(L"RICH PRESENCE", L"Discord IPC connection failed (is Discord running?)");
        }
    }

    g_rpInitialized = true;

    const std::wstring json = PresenceToJson(g_presence);

    lk.unlock();

    // The launcher hears the defaults whether or not any backend connected. Outside
    // the lock: an in-process callback may set presence fields itself.
    FirePresenceCallback(L"UPDATE", json.c_str());
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
    FORWARD_TO_PRIMARY(InterposerSetPresenceDetails, details);

    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.details = SafeStr(details);
    LogRichPresence(L"RP SET", (L"Details: " + ToWide(g_presence.details)).c_str());
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceState(const char* state)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceState, state);

    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.state = SafeStr(state);
    LogRichPresence(L"RP SET", (L"State: " + ToWide(g_presence.state)).c_str());
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceTimestamps(long long start, long long end)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceTimestamps, start, end);

    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.timestampStart = start;
    g_presence.timestampEnd   = end;
    wchar_t buf[80];
    wsprintfW(buf, L"Timestamps: start=%lld end=%lld", static_cast<long long>(start), static_cast<long long>(end));
    LogRichPresence(L"RP SET", buf);
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceLargeImage(const char* key, const char* text)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceLargeImage, key, text);

    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.largeImageKey  = SafeStr(key);
    g_presence.largeImageText = SafeStr(text);
    LogRichPresence(L"RP SET", (L"LargeImage: " + ToWide(g_presence.largeImageKey) + L" (" + ToWide(g_presence.largeImageText) + L")").c_str());
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceSmallImage(const char* key, const char* text)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceSmallImage, key, text);

    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.smallImageKey  = SafeStr(key);
    g_presence.smallImageText = SafeStr(text);
    LogRichPresence(L"RP SET", (L"SmallImage: " + ToWide(g_presence.smallImageKey) + L" (" + ToWide(g_presence.smallImageText) + L")").c_str());
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceParty(const char* id, int size, int max)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceParty, id, size, max);

    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.partyId   = SafeStr(id);
    g_presence.partySize = size;
    g_presence.partyMax  = max;
    wchar_t buf[256];
    wsprintfW(buf, L"Party: %S (%d/%d)", g_presence.partyId.c_str(), size, max);
    LogRichPresence(L"RP SET", buf);
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceButton(int index, const char* text, const char* url)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceButton, index, text, url);

    std::lock_guard<std::mutex> lk(g_rpMutex);

    if (index == 0)
    {
        g_presence.button1Text = SafeStr(text);
        g_presence.button1Url  = SafeStr(url);
        LogRichPresence(L"RP SET", (L"Button1: " + ToWide(g_presence.button1Text) + L" -> " + ToWide(g_presence.button1Url)).c_str());
    }
    else if (index == 1)
    {
        g_presence.button2Text = SafeStr(text);
        g_presence.button2Url  = SafeStr(url);
        LogRichPresence(L"RP SET", (L"Button2: " + ToWide(g_presence.button2Text) + L" -> " + ToWide(g_presence.button2Url)).c_str());
    }
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceType(int type)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceType, type);

    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.type = type;
    wchar_t buf[32];
    wsprintfW(buf, L"Type: %d", type);
    LogRichPresence(L"RP SET", buf);
}

extern "C" __declspec(dllexport)
void InterposerSetPresenceName(const char* name)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceName, name);

    std::lock_guard<std::mutex> lk(g_rpMutex);
    g_presence.name = SafeStr(name);
    LogRichPresence(L"RP SET", (L"Name: " + ToWide(g_presence.name)).c_str());
}

extern "C" __declspec(dllexport)
void InterposerUpdatePresence()
{
    FORWARD_TO_PRIMARY(InterposerUpdatePresence);

    std::wstring json;

    {
        std::lock_guard<std::mutex> lk(g_rpMutex);
        LogRichPresence(L"RP UPDATE", (L"Details=" + ToWide(g_presence.details) + L"  State=" + ToWide(g_presence.state)).c_str());
        FlushToBackends();
        json = PresenceToJson(g_presence);
    }

    FirePresenceCallback(L"UPDATE", json.c_str());
}

extern "C" __declspec(dllexport)
void InterposerClearPresence()
{
    FORWARD_TO_PRIMARY(InterposerClearPresence);

    {
        std::lock_guard<std::mutex> lk(g_rpMutex);

        LogRichPresence(L"RP CLEAR", L"Reset to defaults");

        // Reset to config defaults
        g_presence = g_defaults;

        for (auto& backend : g_backends)
            backend->ClearPresence();
    }

    FirePresenceCallback(L"CLEAR", nullptr);
}
