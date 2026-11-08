/*
 * LANCommander.Interposer.h -- the Interposer's public API.
 *
 * One header for every way of reaching the Interposer from native code:
 *
 *   - A game patch that LINKS it. Add LANCommander.Interposer.lib (one per
 *     architecture, in the release archive's SDK\lib\) to the patch's linker
 *     inputs and ship LANCommander.Interposer.dll and its .interposer\ folder
 *     beside the game executable. The loader then brings the Interposer in
 *     with the patch, its hooks are in place before the patch runs, and the
 *     functions below are called directly.
 *
 *   - A plugin in .interposer\Plugins\, which resolves the same functions with
 *     GetProcAddress on the HMODULE handed to InterposerPluginInit. The types
 *     here describe those exports; a plugin does not need the import library.
 *
 *   - A patch that EMBEDS it: the patch's own DLL (a dinput.dll proxy, an
 *     .asi) with LANCommander.Interposer.Static linked in, so one file carries
 *     both. See LANCommander.Interposer.Embedded.h.
 *
 * Plain C, so it can be included from C or C++. Every function uses __cdecl --
 * written out below, so a project compiled with /Gz or /Gr still calls them
 * correctly. Outside Windows the Win32 type names used here (BOOL, DWORD,
 * LONG) are defined with their Win32 sizes, and wchar_t is whatever the
 * platform's is: UTF-32 on Linux. Every buffer is owned by the caller; the DLL allocates nothing
 * that crosses this boundary, which is what makes it safe to call from a
 * module built with a different compiler or C runtime.
 *
 * Voice chat -- the interposer_comms_* API -- has headers of its own:
 * LANCommander.Interposer.Comms.Client.h and LANCommander.Interposer.Comms.Server.h.
 * The same import library carries both.
 *
 * If a second copy of the Interposer is loaded into a process that already
 * has one -- a patch that links it, in a game LANCommander has also deployed
 * it to -- the second copy installs no hooks of its own and forwards every
 * function below to the first. Calling through either copy is equivalent.
 */
#ifndef LANCOMMANDER_INTERPOSER_H
#define LANCOMMANDER_INTERPOSER_H

#ifdef _WIN32
#  include <windows.h>
#else
/* The Win32 names the API is written in, with their Win32 sizes: BOOL and
 * LONG are 32-bit, and DWORD is a 32-bit unsigned int rather than unsigned
 * long, which is 64-bit on LP64. */
#  include <stdint.h>
#  include <wchar.h>
typedef int      BOOL;
typedef uint32_t DWORD;
typedef int32_t  LONG;
#  ifndef TRUE
#    define TRUE  1
#  endif
#  ifndef FALSE
#    define FALSE 0
#  endif
#endif

#include "LANCommander.Interposer.Export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * Configuration and logging
 * ======================================================================== */

/* Return the effective username (the configured override, or the real Windows
 * account name). bufferSize is in wchar_t units including the terminator. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerGetUsername(wchar_t* buffer, DWORD bufferSize);

/* Write a line to the session log regardless of the Logging flags. verb is
 * normalised: brackets stripped, truncated to 16 characters. */
INTERPOSER_API void INTERPOSER_CALL InterposerLog(const wchar_t* verb, const wchar_t* message);

/* Read a scalar from Config.yml by dot-separated path, e.g.
 * L"FastDL.BaseUrl". Returns FALSE if the key is missing, is not a scalar, or
 * the buffer (wchar_t units, including the terminator) is too small. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerGetConfigString(const wchar_t* dotPath, wchar_t* buffer, DWORD bufferSize);

/* Register default configuration under Plugins.<pluginName>. yamlDefaults is
 * a YAML map body. A section that already exists in Config.yml is left alone;
 * otherwise the defaults are merged in memory and appended to Config.yml.
 * Returns FALSE on bad YAML or a failed write. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerRegisterPluginConfig(const wchar_t* pluginName, const wchar_t* yamlDefaults);

/* Write a scalar back to Config.yml. The path must lie inside a plugin
 * section -- Plugins.<Name>.<Key>[.<Key>...] -- because every other key is
 * read once at startup. Missing keys and maps are created; comments are kept.
 * Returns FALSE outside Plugins.<Name>, on a shape collision, or if the file
 * could not be written (the in-memory value is updated regardless). */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerSetConfigString(const wchar_t* dotPath, const wchar_t* value);

/* ===========================================================================
 * FastDL
 * ======================================================================== */

/* Point FastDL at a server found at run time -- the one the game server a
 * player joins advertises, say. baseUrl is http:// or https://; a trailing
 * slash is added if missing. NULL or L"" forgets it. While set, it wins over
 * a FastDL.BaseUrl in Config.yml (or the launcher's --fastdl-url), which is
 * used again once it is forgotten, and over the servers
 * FastDL.ProbeConnections finds. Config.yml decides the rest (FastDL.Enabled,
 * FastDL.Paths). Returns FALSE when FastDL is off
 * or the URL is not http(s); the URL is not kept then. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerSetFastDLBaseUrl(const wchar_t* baseUrl);

/* ===========================================================================
 * Virtual registry
 * ======================================================================== */

/* Inject a transient REG_SZ into the virtual store by exact key path (not
 * persisted). valueName may be L"@", L"" or NULL for the default value. */
INTERPOSER_API void INTERPOSER_CALL InterposerSetRegistryValue(const wchar_t* keyPath, const wchar_t* valueName, const wchar_t* value);

/* As above, for every virtual key whose path ends with \keySuffix
 * (case-insensitive, on a backslash boundary). Returns the number of keys
 * updated. */
INTERPOSER_API DWORD INTERPOSER_CALL InterposerSetRegistryValueBySuffix(const wchar_t* keySuffix, const wchar_t* valueName, const wchar_t* value);

/* ===========================================================================
 * Notification callbacks
 *
 * Observe, not modify. Fired on the thread that triggered the hook, regardless
 * of the Logging flags. One of each at a time; pass NULL to unregister.
 * ======================================================================== */

/* File operations: CreateFile, GetFileAttributes, FindFirstFile, Delete, Move,
 * Copy, LoadLibrary. verb is e.g. "FILE READ", "FILE REDIRECT";
 * secondaryPath is the redirect/destination path, or NULL. */
typedef void (INTERPOSER_CALL* InterposerFileCallback)(
    const wchar_t* verb, const wchar_t* path, const wchar_t* secondaryPath);

/* Registry operations. verb is e.g. "REG OPEN", "REG READ"; valueName is NULL
 * for key-level operations (a key delete included). */
typedef void (INTERPOSER_CALL* InterposerRegistryCallback)(
    const wchar_t* verb, const wchar_t* keyPath, const wchar_t* valueName);

/* DNS resolution. redirectedHostname equals hostname when nothing matched. */
typedef void (INTERPOSER_CALL* InterposerDnsCallback)(
    const wchar_t* hostname, const wchar_t* redirectedHostname);

/* Network connections. port is 0 if unknown. */
typedef void (INTERPOSER_CALL* InterposerNetworkCallback)(
    const wchar_t* address, int port);

/* Identity. type is "USERNAME" or "COMPUTERNAME". */
typedef void (INTERPOSER_CALL* InterposerIdentityCallback)(
    const wchar_t* type, const wchar_t* value);

/* Network relay.
 *   kind "STATE"       address = our virtual address (empty before the first
 *                      Welcome), detail = state name, value = RelayState
 *        "PEER JOIN"   address = the peer's virtual address, detail = its
 *        "PEER LEAVE"  player name, value = its relay client id */
typedef void (INTERPOSER_CALL* InterposerRelayCallback)(
    const wchar_t* kind, const wchar_t* address, const wchar_t* detail, int value);

/* Rich presence, fired on every InterposerUpdatePresence / InterposerClearPresence
 * and once with the Config.yml defaults at startup.
 *   kind "UPDATE"   json = the presence now shown (see RichPresence.md for its
 *                   fields; strings are empty and numbers 0 when unset)
 *        "CLEAR"    json = NULL: the game shows no presence
 * Fired after the presence lock is released, so the callback may call the
 * InterposerSetPresence* functions. */
typedef void (INTERPOSER_CALL* InterposerPresenceCallback)(
    const wchar_t* kind, const wchar_t* json);

INTERPOSER_API void INTERPOSER_CALL InterposerRegisterFileCallback(InterposerFileCallback cb);
INTERPOSER_API void INTERPOSER_CALL InterposerRegisterRegistryCallback(InterposerRegistryCallback cb);
INTERPOSER_API void INTERPOSER_CALL InterposerRegisterDnsCallback(InterposerDnsCallback cb);
INTERPOSER_API void INTERPOSER_CALL InterposerRegisterNetworkCallback(InterposerNetworkCallback cb);
INTERPOSER_API void INTERPOSER_CALL InterposerRegisterIdentityCallback(InterposerIdentityCallback cb);
INTERPOSER_API void INTERPOSER_CALL InterposerRegisterRelayCallback(InterposerRelayCallback cb);
INTERPOSER_API void INTERPOSER_CALL InterposerRegisterPresenceCallback(InterposerPresenceCallback cb);

/* ===========================================================================
 * Network adapters
 * ======================================================================== */

typedef struct InterposerNetworkAdapter {
    wchar_t friendlyName[256];
    wchar_t description[256];
    wchar_t macAddress[24];   /* "00:11:22:33:44:55", empty if none */
    wchar_t ipv4Address[16];  /* first IPv4 (dotted-decimal), empty if none */
    wchar_t ipv6Address[46];  /* first IPv6, empty if none */
    int     allowed;          /* 1 = passes the NetworkAdapters filter (or no filter), 0 = hidden */
} InterposerNetworkAdapter;

/* Fills up to capacity entries and writes the number written to *outCount.
 * Returns the TOTAL number available -- call with capacity 0 to size the
 * buffer. */
INTERPOSER_API DWORD INTERPOSER_CALL InterposerEnumNetworkAdapters(
    InterposerNetworkAdapter* buffer, DWORD capacity, DWORD* outCount);

/* ===========================================================================
 * Audio devices
 * ======================================================================== */

typedef struct InterposerAudioDevice {
    wchar_t name[256];        /* friendly name, e.g. "Speakers (Realtek High Definition Audio)" */
    wchar_t description[256]; /* device description, e.g. "Speakers" */
    wchar_t id[256];          /* Core Audio endpoint ID */
    wchar_t instanceId[256];  /* device instance path, e.g. SWD\MMDEVAPI\{0.0.0...} */
    wchar_t hardwareId[256];  /* most specific hardware ID, e.g. USB\VID_0965&PID_9001, empty if none */
    int     direction;        /* 1 = output (render), 2 = input (capture), 0 = unknown */
    int     isDefault;        /* 1 = the system default endpoint for its direction */
    int     allowed;          /* 1 = passes the AudioDevices filter (or no filter), 0 = hidden */
} InterposerAudioDevice;

/* Same contract as InterposerEnumNetworkAdapters. Reports the real device set
 * whatever the filter, with every string an AudioDevices entry matches on. */
INTERPOSER_API DWORD INTERPOSER_CALL InterposerEnumAudioDevices(
    InterposerAudioDevice* buffer, DWORD capacity, DWORD* outCount);

/* ===========================================================================
 * DirectInput mouse transform
 *
 * Only the system mouse of a game bridged by
 * DirectInput.FixLegacyDeviceEnumeration is routed through this.
 * ======================================================================== */

/* One buffered event: the DirectInput 8 record minus its pointer-sized
 * uAppData, so the layout is identical on x86 and x64. */
typedef struct InterposerInputEvent {
    DWORD dwOfs;      /* offset of the object within the game's data format */
    LONG  data;       /* axis delta, or button state */
    DWORD timeStamp;  /* milliseconds, as DirectInput reported it */
    DWORD sequence;   /* DirectInput sequence number */
} InterposerInputEvent;

/* An axis the game's data format did not declare. */
#define INTERPOSER_MOUSE_AXIS_NONE 0xFFFFFFFFu

typedef struct InterposerMouseBatch {
    DWORD                 structSize;   /* sizeof(InterposerMouseBatch) -- version guard */
    DWORD                 axisOffsetX;  /* dwOfs of each axis in the game's own data */
    DWORD                 axisOffsetY;  /* format, or INTERPOSER_MOUSE_AXIS_NONE */
    DWORD                 axisOffsetZ;  /* (wheel) */
    InterposerInputEvent* events;       /* events to deliver, in order */
    DWORD                 count;        /* in: retrieved. out: to deliver. */
    DWORD                 capacity;     /* events[] capacity; count must not exceed it */
} InterposerMouseBatch;

/* Rewrite batch->events in place and set batch->count, up to capacity. Runs on
 * the game's input thread inside GetDeviceData: must not block. Note WINAPI
 * (__stdcall) on Windows, not INTERPOSER_CALL. */
typedef void (INTERPOSER_STDCALL* InterposerMouseTransform)(InterposerMouseBatch* batch, void* userData);

/* One at a time; registering again replaces. NULL unregisters. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerRegisterMouseTransform(
    InterposerMouseTransform callback, void* userData);

#ifdef _WIN32
/* ===========================================================================
 * DirectInput entry points, for a patch deployed as dinput.dll or dinput8.dll
 *
 * Such a patch serves the game's DirectInputCreateA/Ex or DirectInput8Create
 * itself, so no Interposer detours them. Calling these from that export is
 * what keeps DirectInput.FixLegacyDeviceEnumeration and
 * DirectInput.DeviceFilter in effect: each does what the Interposer's own
 * proxy export of that name does, and falls through to the real System32
 * DLL when neither is configured. The object comes back in *ppvOut, ready for
 * the patch to wrap. Exporting these is also how another Interposer in the
 * process recognises the module and leaves its exports alone.
 *
 * hinst is an HINSTANCE, riid a REFIID; the return value is an HRESULT.
 * ======================================================================== */

INTERPOSER_API LONG INTERPOSER_CALL InterposerDirectInputCreateA(
    HINSTANCE hinst, DWORD version, void** ppvOut, void* punkOuter);
INTERPOSER_API LONG INTERPOSER_CALL InterposerDirectInputCreateEx(
    HINSTANCE hinst, DWORD version, const void* riid, void** ppvOut, void* punkOuter);
INTERPOSER_API LONG INTERPOSER_CALL InterposerDirectInput8Create(
    HINSTANCE hinst, DWORD version, const void* riid, void** ppvOut, void* punkOuter);
#endif

/* ===========================================================================
 * Network relay
 * ======================================================================== */

typedef struct InterposerRelayStatus {
    DWORD              size;               /* caller sets to sizeof(InterposerRelayStatus) */
    int                state;              /* RelayState */
    DWORD              clientId;
    wchar_t            virtualAddress[16];
    wchar_t            netmask[16];
    wchar_t            network[128];
    wchar_t            server[256];
    int                udpActive;          /* 1 = datagrams over UDP, 0 = TCP fallback */
    DWORD              peerCount;
    unsigned long long bytesSent;
    unsigned long long bytesReceived;
    unsigned long long datagramsSent;
    unsigned long long datagramsReceived;
    DWORD              streamsOpen;
    wchar_t            lastError[256];
} InterposerRelayStatus;

typedef struct InterposerRelayPeer {
    DWORD   clientId;
    wchar_t virtualAddress[16];
    wchar_t name[128];
} InterposerRelayPeer;

/* Returns FALSE if status is NULL or status->size is too small. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerRelayGetStatus(InterposerRelayStatus* status);

/* Same contract as InterposerEnumNetworkAdapters. */
INTERPOSER_API DWORD INTERPOSER_CALL InterposerRelayEnumPeers(
    InterposerRelayPeer* buffer, DWORD capacity, DWORD* outCount);

/* Apply a NetworkRelay YAML map over the current settings and (re)connect.
 * yamlOverrides may be NULL to reconnect as is. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerRelayConnect(const wchar_t* yamlOverrides);

INTERPOSER_API void INTERPOSER_CALL InterposerRelayDisconnect(void);

/* ===========================================================================
 * Rich presence -- all strings UTF-8
 * ======================================================================== */

INTERPOSER_API void INTERPOSER_CALL InterposerSetPresenceDetails(const char* details);
INTERPOSER_API void INTERPOSER_CALL InterposerSetPresenceState(const char* state);
INTERPOSER_API void INTERPOSER_CALL InterposerSetPresenceTimestamps(long long start, long long end);
INTERPOSER_API void INTERPOSER_CALL InterposerSetPresenceLargeImage(const char* key, const char* text);
INTERPOSER_API void INTERPOSER_CALL InterposerSetPresenceSmallImage(const char* key, const char* text);
INTERPOSER_API void INTERPOSER_CALL InterposerSetPresenceParty(const char* id, int size, int max);
INTERPOSER_API void INTERPOSER_CALL InterposerSetPresenceButton(int index, const char* text, const char* url);
INTERPOSER_API void INTERPOSER_CALL InterposerSetPresenceType(int type);
INTERPOSER_API void INTERPOSER_CALL InterposerSetPresenceName(const char* name);
INTERPOSER_API void INTERPOSER_CALL InterposerUpdatePresence(void);
INTERPOSER_API void INTERPOSER_CALL InterposerClearPresence(void);

/* ===========================================================================
 * Match statistics -- all strings UTF-8
 *
 * Report a multiplayer match to LANCommander: who played, what happened, and
 * how it ended. A dedicated server and a client patch call the same functions;
 * where the reports go is decided by configuration, not by the caller:
 *
 *   - MatchStatistics.ServerUrl and .Token set (a dedicated server, with a
 *     token carrying the matches:report scope): the Interposer sends them to
 *     the LANCommander server itself, batching events, retrying when the
 *     server is unreachable, and keeping unsent reports on disk
 *     (.interposer\Matches\) so a crash or restart loses nothing.
 *   - Otherwise, when LANCommander's launcher started the game: the launcher
 *     sends them, as the signed-in player.
 *   - Otherwise they are only logged, and passed to the callback below.
 *
 * A match is identified by the handle InterposerMatchStart returns. Several
 * may be open at once. Calls return FALSE only for a mistake by the caller --
 * an unknown handle, a missing key, a payload that is not a JSON object, a
 * change after InterposerMatchEnd -- never because LANCommander cannot be
 * reached; InterposerMatchGetStatus says how delivery is going.
 *
 * Player keys name a player throughout, prefixed by the kind of id:
 * "steam:STEAM_0:1:123", "name:Pat", "bot:3". Event types and statistic keys
 * are lowercase with underscores: "kill", "lap_completed", "best_lap_ms".
 * Game time is milliseconds since the match started by the game's own clock,
 * so pauses do not count.
 * ======================================================================== */

/* InterposerMatchResult.status */
#define INTERPOSER_MATCH_RESULT_FINISHED      0
#define INTERPOSER_MATCH_RESULT_DID_NOT_FINISH 1
#define INTERPOSER_MATCH_RESULT_DISQUALIFIED  2
#define INTERPOSER_MATCH_RESULT_DEFEATED      3
#define INTERPOSER_MATCH_RESULT_FORFEITED     4

/* InterposerMatchAttachFile kind */
#define INTERPOSER_MATCH_ATTACHMENT_DEMO       0
#define INTERPOSER_MATCH_ATTACHMENT_REPLAY     1
#define INTERPOSER_MATCH_ATTACHMENT_LOG        2
#define INTERPOSER_MATCH_ATTACHMENT_SCREENSHOT 3
#define INTERPOSER_MATCH_ATTACHMENT_OTHER      4

/* InterposerMatchStatus.sink */
#define INTERPOSER_MATCH_SINK_NONE     0   /* logged and passed to the callback only */
#define INTERPOSER_MATCH_SINK_HTTP     1   /* sent to the LANCommander server by the Interposer */
#define INTERPOSER_MATCH_SINK_LAUNCHER 2   /* sent by LANCommander's launcher */

typedef struct InterposerMatchInfo {
    DWORD       structSize;     /* sizeof(InterposerMatchInfo) -- version guard */
    const char* externalId;     /* the game's own id for the match (a round or session id), or
                                   NULL for a generated one. Starting a match again under the same
                                   id carries on with it -- after a restart, too. */
    const char* map;
    const char* mode;
    const char* gameVersion;
    const char* serverName;     /* for a server LANCommander does not manage, else NULL */
    const char* serverAddress;  /* likewise: the address players connect to */
    const char* settingsJson;   /* the rules played with (time limit, cvars), a JSON object, or NULL */
    const char* metadataJson;   /* anything else to keep, a JSON object, or NULL */
    long long   startedOn;      /* Unix milliseconds UTC, 0 = now */
} InterposerMatchInfo;

typedef struct InterposerMatchResult {
    DWORD       structSize;     /* sizeof(InterposerMatchResult) */
    const char* playerKey;      /* a player key, a team, or both */
    const char* team;
    int         rank;           /* 1 for the winner; tied entries share a rank */
    int         status;         /* INTERPOSER_MATCH_RESULT_* */
    int         hasScore;       /* 0 = no score */
    double      score;
    long long   timeMs;         /* a finishing time, for races; negative = none */
    const char* payloadJson;    /* anything else for the entry, a JSON object, or NULL */
} InterposerMatchResult;

typedef struct InterposerMatchStatus {
    DWORD   size;               /* caller sets to sizeof(InterposerMatchStatus) */
    int     sink;               /* INTERPOSER_MATCH_SINK_* */
    char    matchId[40];        /* LANCommander's id for the match, empty until it is known */
    DWORD   pendingEvents;      /* recorded and not yet accepted by the server */
    DWORD   rejectedEvents;     /* refused by the server; never retried */
    DWORD   pendingAttachments; /* attached and not yet uploaded */
    int     ended;              /* InterposerMatchEnd was called */
    int     delivered;          /* everything, the end and attachments included, has been accepted */
    wchar_t lastError[256];     /* the last delivery problem, empty when none */
} InterposerMatchStatus;

/* Start (or carry on with) a match. Returns its handle, 0 on bad arguments. */
INTERPOSER_API DWORD INTERPOSER_CALL InterposerMatchStart(const InterposerMatchInfo* info);

/* A player joined, or changed name or team. gameTimeMs negative = unknown. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchPlayerJoined(DWORD match, const char* playerKey,
    const char* displayName, const char* team, long long gameTimeMs);

/* A player left. gameTimeMs negative = unknown. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchPlayerLeft(DWORD match, const char* playerKey, long long gameTimeMs);

/* Something happened. actorKey is who caused it, subjectKey who it happened
 * to (a kill's victim); either may be NULL. payloadJson is the detail as a JSON
 * object, or NULL: refer to players by key, give durations in whole
 * milliseconds with an _ms suffix, and leave out derived values such as rates.
 * tick is the game's simulation tick, negative when it has none. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchEvent(DWORD match, const char* type, long long gameTimeMs,
    long long tick, const char* actorKey, const char* subjectKey, const char* payloadJson);

/* An end-of-match total for a player (playerKey) or a team (team, playerKey
 * NULL). Set replaces the value; Add adds to the running total the Interposer
 * keeps, so a patch can count kills as they happen. Sent with the end. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchSetStatistic(DWORD match, const char* playerKey,
    const char* team, const char* key, double value);
INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchAddStatistic(DWORD match, const char* playerKey,
    const char* team, const char* key, double delta);

/* A player's or team's place in the final standings. Setting the same player
 * or team again replaces it. Sent with the end. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchSetResult(DWORD match, const InterposerMatchResult* result);

/* End the match. reason is e.g. "completed", "time_limit" or "aborted" (may be
 * NULL); abandoned means it was cut short and its results should not count. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchEnd(DWORD match, const char* reason, BOOL abandoned);

/* Attach a file -- a demo, a log -- to the match, uploaded in the background.
 * Allowed before or after the end. startMs/endMs are the game time the file
 * covers, negative when unknown. The file must stay in place until
 * InterposerMatchGetStatus reports it uploaded. Needs the HTTP sink (or the
 * launcher's): with neither it is only logged. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchAttachFile(DWORD match, const wchar_t* path, int kind,
    long long startMs, long long endMs);

/* Send everything recorded so far, and wait up to timeoutMs for the server to
 * take it -- for a server about to shut down. TRUE when nothing is left
 * waiting. Returns at once for a match that is not sent over HTTP. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchFlush(DWORD match, DWORD timeoutMs);

/* Returns FALSE for an unknown handle, or if status->size is too small. */
INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchGetStatus(DWORD match, InterposerMatchStatus* status);

/* Every step of every match, whatever the sink: kind is "START",
 * "PARTICIPANT", "EVENT", "END" or "ATTACH", and json describes it (see
 * MatchStatistics.md). Fired on the calling thread, outside any lock. */
typedef void (INTERPOSER_CALL* InterposerMatchCallback)(const wchar_t* kind, DWORD match, const wchar_t* json);

INTERPOSER_API void INTERPOSER_CALL InterposerRegisterMatchCallback(InterposerMatchCallback cb);

#ifdef __cplusplus
}
#endif

#endif /* LANCOMMANDER_INTERPOSER_H */
