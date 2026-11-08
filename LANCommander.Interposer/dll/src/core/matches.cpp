#include "matches.h"
#include "callbacks.h"
#include "config.h"
#include "file_access.h"
#include "http.h"
#include "instance.h"
#include "json.h"
#include "pipe_events.h"
#include "platform.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Match statistics. See matches.h for the shape, Documentation/MatchStatistics.md
// for the contract, and LANCommander's MatchSubmissions.cs for the wire format
// this speaks (camelCase JSON, enums as their names).
//
// Threading: every Match field is read and written under g_mutex. The exports
// take it briefly and fire callbacks after releasing it. The HTTP worker takes
// a copy of what it is about to send under the lock, sends it unlocked, and
// applies the reply under the lock again. Matches are never erased, so a
// Match* stays valid across the unlocked part; their handles stay valid for
// the life of the process.
//
// Sequence numbers. The server numbers each reporter's events from 1 and
// tells a reporter re-creating a match the highest it has written. Events
// recorded before the match is registered carry provisional numbers, and are
// renumbered after the server's at registration -- that is what lets a
// restarted server carry on a match under the same external id. Once
// registered the numbers are final (sequencesFinal), and a later run that
// finds them in the spool resends only the ones above the server's.
// ---------------------------------------------------------------------------

using platform::Utf8ToWide;
using platform::WideToUtf8;

namespace
{

constexpr size_t kBatchSize      = 500;        // LANCommander's default batch limit
constexpr size_t kChunkBytes     = 8u << 20;   // attachment upload chunk
constexpr int    kRequestTimeout = 15000;
constexpr int    kMaxBackoffMs   = 30000;
constexpr int    kStopWaitMs     = 3000;

const char* const kResultStatus[]   = { "Finished", "DidNotFinish", "Disqualified", "Defeated", "Forfeited" };
const char* const kAttachmentKind[] = { "Demo", "Replay", "Log", "Screenshot", "Other" };
const wchar_t* const kSinkNames[]   = { L"none", L"LANCommander server", L"launcher" };

struct Event
{
    std::string id;
    long long   sequence   = 0;
    std::string type;
    long long   gameTimeMs = 0;
    long long   tick       = -1;
    std::string actor;
    std::string subject;
    std::string payload;     // a JSON object, or empty
    std::string clientTime;  // ISO 8601
};

struct Participant
{
    std::string key;
    std::string name;
    std::string team;
    long long   joinedMs = -1;
    long long   leftMs   = -1;
    unsigned    version  = 1;   // bumped on every change
    unsigned    sent     = 0;   // the version the server has
};

struct Statistic
{
    std::string playerKey;
    std::string team;
    std::string key;
    double      value = 0;
};

struct Result
{
    std::string playerKey;
    std::string team;
    int         rank     = 0;
    int         status   = 0;
    bool        hasScore = false;
    double      score    = 0;
    long long   timeMs   = -1;
    std::string payload;
};

struct Attachment
{
    std::wstring path;
    int          kind    = 4;
    long long    startMs = -1;
    long long    endMs   = -1;
    std::string  id;          // LANCommander's, once declared
    long long    size    = -1;
    long long    offset  = 0; // bytes the server has
    bool         done    = false;
    bool         failed  = false;
};

struct Match
{
    DWORD       handle = 0;
    int         sink   = INTERPOSER_MATCH_SINK_NONE;

    std::string externalId, map, mode, gameVersion, serverName, serverAddress;
    std::string settings, metadata;      // JSON objects, or empty
    long long   startedOn = 0;           // Unix ms
    std::string gameId, serverId, reporterKind, reporterName;

    long long          latestGameTimeMs = 0;   // of the last event, for a leave with no time
    long long          lastSequence   = 0;     // the highest number handed out
    bool               sequencesFinal = false; // numbered after the server's
    std::deque<Event>  pending;                // not yet accepted, in sequence order

    std::map<std::string, Participant> participants;
    std::vector<Statistic>             statistics;
    std::vector<Result>                results;

    bool        ended     = false;
    bool        endSent   = false;
    std::string endReason;
    bool        abandoned = false;
    long long   endedOn   = 0;

    std::vector<Attachment> attachments;

    // Delivery (HTTP)
    bool         registered = false;
    std::string  matchId, reporterId;
    DWORD        rejected   = 0;
    std::wstring lastError;
    long long    retryAt    = 0;    // steady-clock ms
    int          backoffMs  = 0;
    bool         gaveUp     = false; // the server refused the match itself
    bool         delivered  = false;
    bool         orphan     = false; // loaded from the spool; no caller holds the handle yet

    // Spool (HTTP)
    std::wstring spoolDir;
    long long    spooledSequence = 0;  // events above this are not on disk yet
    bool         rewriteEvents   = false;
    bool         matchFileDirty  = true;
    bool         stateDirty      = true;
};

std::mutex                              g_mutex;
std::condition_variable                 g_wake;      // worker: something to send
std::condition_variable                 g_progress;  // Flush: the worker made progress
std::map<DWORD, std::unique_ptr<Match>> g_matches;
DWORD                                   g_nextHandle = 1;

platform::Thread  g_thread      = nullptr;
std::atomic<bool> g_stop{ false };
bool              g_flushNow    = false;
bool              g_httpOpen    = false;
bool              g_warnedGame  = false;

long long SteadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Str(const char* s) { return s ? s : ""; }

bool ValidJsonOrNull(const char* s)
{
    return !s || !*s || json::IsObject(s);
}

std::wstring Wide(const std::string& s) { return Utf8ToWide(s); }

void Fire(const wchar_t* kind, DWORD handle, const std::string& body)
{
    const std::wstring wide = Utf8ToWide(body);
    FireMatchCallback(kind, handle, wide.c_str());
}

// ── JSON: the wire format ─────────────────────────────────────────────────────

void AppendOwner(std::string& j, const std::string& playerKey, const std::string& team)
{
    json::AppendKeyStringIfAny(j, "playerKey", playerKey);
    json::AppendKeyStringIfAny(j, "team", team);
}

std::string EventJson(const Event& e)
{
    std::string j = "{";
    json::AppendKeyString(j, "id", e.id);
    json::AppendKeyNumber(j, "sequence", e.sequence);
    json::AppendKeyString(j, "type", e.type);
    json::AppendKeyNumber(j, "gameTimeMilliseconds", e.gameTimeMs);
    if (e.tick >= 0)
        json::AppendKeyNumber(j, "tick", e.tick);
    json::AppendKeyStringIfAny(j, "actorKey", e.actor);
    json::AppendKeyStringIfAny(j, "subjectKey", e.subject);
    json::AppendKeyRaw(j, "payload", e.payload);
    json::AppendKeyString(j, "clientTime", e.clientTime);
    json::CloseObject(j);
    return j;
}

std::string ParticipantJson(const Participant& p)
{
    std::string j = "{";
    json::AppendKeyString(j, "playerKey", p.key);
    json::AppendKeyStringIfAny(j, "displayName", p.name);
    json::AppendKeyStringIfAny(j, "team", p.team);
    if (p.joinedMs >= 0)
        json::AppendKeyNumber(j, "joinedAtMilliseconds", p.joinedMs);
    if (p.leftMs >= 0)
        json::AppendKeyNumber(j, "leftAtMilliseconds", p.leftMs);
    json::CloseObject(j);
    return j;
}

std::string StatisticJson(const Statistic& s)
{
    std::string j = "{";
    AppendOwner(j, s.playerKey, s.team);
    json::AppendKeyString(j, "key", s.key);
    json::AppendKeyDouble(j, "value", s.value);
    json::CloseObject(j);
    return j;
}

std::string ResultJson(const Result& r)
{
    std::string j = "{";
    AppendOwner(j, r.playerKey, r.team);
    json::AppendKeyNumber(j, "rank", r.rank);
    if (r.hasScore)
        json::AppendKeyDouble(j, "score", r.score);
    json::AppendKeyString(j, "status", kResultStatus[r.status]);
    if (r.timeMs >= 0)
        json::AppendKeyNumber(j, "timeMilliseconds", r.timeMs);
    json::AppendKeyRaw(j, "payload", r.payload);
    json::CloseObject(j);
    return j;
}

template <typename T, typename F>
std::string ArrayJson(const T& items, F each)
{
    std::string j = "[";
    for (const auto& item : items)
    {
        j += each(item);
        j += ',';
    }
    json::CloseArray(j);
    return j;
}

// MatchCreateRequest. `forward` is added for the START callback: true when
// the launcher is the one to send it.
std::string CreateJson(const Match& m, const bool* forward)
{
    std::string j = "{";
    json::AppendKeyStringIfAny(j, "gameId", m.gameId);
    json::AppendKeyStringIfAny(j, "serverId", m.serverId);
    json::AppendKeyString(j, "externalId", m.externalId);
    json::AppendKeyStringIfAny(j, "map", m.map);
    json::AppendKeyStringIfAny(j, "mode", m.mode);
    json::AppendKeyStringIfAny(j, "gameVersion", m.gameVersion);
    json::AppendKeyStringIfAny(j, "serverName", m.serverName);
    json::AppendKeyStringIfAny(j, "serverAddress", m.serverAddress);
    json::AppendKeyString(j, "startedOn", json::FormatUtc(m.startedOn));
    json::AppendKeyRaw(j, "settings", m.settings);
    json::AppendKeyRaw(j, "metadata", m.metadata);
    json::AppendKeyString(j, "reporterKind", m.reporterKind);
    json::AppendKeyStringIfAny(j, "reporterName", m.reporterName);
    if (forward)
        json::AppendKeyBool(j, "forward", *forward);
    json::CloseObject(j);
    return j;
}

// MatchEndRequest.
std::string EndJson(const Match& m, bool withParticipants)
{
    std::string j = "{";
    json::AppendKeyString(j, "endedOn", json::FormatUtc(m.endedOn));
    json::AppendKeyStringIfAny(j, "reason", m.endReason);
    json::AppendKeyBool(j, "abandoned", m.abandoned);
    json::AppendKeyRaw(j, "results", ArrayJson(m.results, ResultJson));
    json::AppendKeyRaw(j, "statistics", ArrayJson(m.statistics, StatisticJson));
    if (withParticipants)
        json::AppendKeyRaw(j, "participants", ArrayJson(m.participants,
            [](const auto& entry) { return ParticipantJson(entry.second); }));
    json::CloseObject(j);
    return j;
}

std::wstring FileNameOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::string ContentTypeOf(const std::wstring& path)
{
    std::wstring name = FileNameOf(path);
    const size_t dot  = name.rfind(L'.');
    std::wstring ext  = dot == std::wstring::npos ? L"" : name.substr(dot + 1);

    for (auto& c : ext)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');

    if (ext == L"log" || ext == L"txt") return "text/plain";
    if (ext == L"json")                 return "application/json";
    if (ext == L"png")                  return "image/png";
    if (ext == L"jpg" || ext == L"jpeg") return "image/jpeg";
    if (ext == L"zip")                  return "application/zip";
    return "application/octet-stream";
}

// MatchAttachmentCreateRequest; with `path` for the ATTACH callback.
std::string AttachmentJson(const Attachment& a, const std::string& reporterId, bool withPath)
{
    std::string j = "{";
    json::AppendKeyStringIfAny(j, "reporterId", reporterId);
    if (withPath)
        json::AppendKeyString(j, "path", WideToUtf8(a.path));
    json::AppendKeyString(j, "kind", kAttachmentKind[a.kind]);
    json::AppendKeyString(j, "fileName", WideToUtf8(FileNameOf(a.path)));
    json::AppendKeyString(j, "contentType", ContentTypeOf(a.path));
    json::AppendKeyNumber(j, "size", a.size < 0 ? 0 : a.size);
    if (a.startMs >= 0)
        json::AppendKeyNumber(j, "startMilliseconds", a.startMs);
    if (a.endMs >= 0)
        json::AppendKeyNumber(j, "endMilliseconds", a.endMs);
    json::CloseObject(j);
    return j;
}

// ── JSON: reading replies and the spool ───────────────────────────────────────

YAML::Node ParseJson(const std::string& text)
{
    // JSON is (near enough) a subset of YAML, and yaml-cpp is already linked.
    try
    {
        return YAML::Load(text);
    }
    catch (const YAML::Exception&)
    {
        return YAML::Node();
    }
}

// A field by its camelCase name, or the PascalCase one. Through a const
// reference: the non-const operator[] would insert.
YAML::Node FieldOf(const YAML::Node& node, const char* camel)
{
    if (!node.IsMap())
        return YAML::Node();

    if (const YAML::Node value = node[camel])
        return value;

    // Not "pascal": <windows.h> defines that as a calling convention.
    std::string capitalised = camel;
    capitalised[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(capitalised[0])));
    return node[capitalised];
}

std::string TextOf(const YAML::Node& node, const char* key)
{
    const YAML::Node value = FieldOf(node, key);

    try { return value && value.IsScalar() ? value.as<std::string>() : std::string(); }
    catch (const YAML::Exception&) { return {}; }
}

long long NumberOf(const YAML::Node& node, const char* key, long long fallback)
{
    const YAML::Node value = FieldOf(node, key);

    try { return value && value.IsScalar() ? value.as<long long>() : fallback; }
    catch (const YAML::Exception&) { return fallback; }
}

double DoubleOf(const YAML::Node& node, const char* key)
{
    const YAML::Node value = FieldOf(node, key);

    try { return value && value.IsScalar() ? value.as<double>() : 0; }
    catch (const YAML::Exception&) { return 0; }
}

bool BoolOf(const YAML::Node& node, const char* key)
{
    const YAML::Node value = FieldOf(node, key);

    try { return value && value.IsScalar() && value.as<bool>(); }
    catch (const YAML::Exception&) { return false; }
}

// The spool stores every JSON blob (payloads, settings) as a JSON *string*, so
// reading it back through yaml-cpp gives the original text rather than a tree
// that would have to be re-emitted as JSON.
std::string SpoolEventLine(const Event& e)
{
    std::string j = "{";
    json::AppendKeyString(j, "id", e.id);
    json::AppendKeyNumber(j, "sequence", e.sequence);
    json::AppendKeyString(j, "type", e.type);
    json::AppendKeyNumber(j, "gameTimeMs", e.gameTimeMs);
    json::AppendKeyNumber(j, "tick", e.tick);
    json::AppendKeyString(j, "actor", e.actor);
    json::AppendKeyString(j, "subject", e.subject);
    json::AppendKeyString(j, "payload", e.payload);
    json::AppendKeyString(j, "clientTime", e.clientTime);
    json::CloseObject(j);
    j += '\n';
    return j;
}

std::string SpoolMatchJson(const Match& m)
{
    std::string j = "{";
    json::AppendKeyString(j, "externalId", m.externalId);
    json::AppendKeyString(j, "map", m.map);
    json::AppendKeyString(j, "mode", m.mode);
    json::AppendKeyString(j, "gameVersion", m.gameVersion);
    json::AppendKeyString(j, "serverName", m.serverName);
    json::AppendKeyString(j, "serverAddress", m.serverAddress);
    json::AppendKeyString(j, "settings", m.settings);
    json::AppendKeyString(j, "metadata", m.metadata);
    json::AppendKeyNumber(j, "startedOn", m.startedOn);
    json::AppendKeyString(j, "gameId", m.gameId);
    json::AppendKeyString(j, "serverId", m.serverId);
    json::AppendKeyString(j, "reporterKind", m.reporterKind);
    json::AppendKeyString(j, "reporterName", m.reporterName);
    json::AppendKeyNumber(j, "lastSequence", m.lastSequence);
    json::AppendKeyBool(j, "sequencesFinal", m.sequencesFinal);
    json::AppendKeyString(j, "matchId", m.matchId);
    json::CloseObject(j);
    return j;
}

std::string SpoolStateJson(const Match& m)
{
    std::string j = "{";

    json::AppendKeyRaw(j, "participants", ArrayJson(m.participants, [](const auto& entry)
    {
        const Participant& p = entry.second;
        std::string o = "{";
        json::AppendKeyString(o, "key", p.key);
        json::AppendKeyString(o, "name", p.name);
        json::AppendKeyString(o, "team", p.team);
        json::AppendKeyNumber(o, "joinedMs", p.joinedMs);
        json::AppendKeyNumber(o, "leftMs", p.leftMs);
        json::AppendKeyBool(o, "sent", p.sent == p.version);
        json::CloseObject(o);
        return o;
    }));

    json::AppendKeyRaw(j, "statistics", ArrayJson(m.statistics, [](const Statistic& s)
    {
        std::string o = "{";
        json::AppendKeyString(o, "playerKey", s.playerKey);
        json::AppendKeyString(o, "team", s.team);
        json::AppendKeyString(o, "key", s.key);
        json::AppendKeyDouble(o, "value", s.value);
        json::CloseObject(o);
        return o;
    }));

    json::AppendKeyRaw(j, "results", ArrayJson(m.results, [](const Result& r)
    {
        std::string o = "{";
        json::AppendKeyString(o, "playerKey", r.playerKey);
        json::AppendKeyString(o, "team", r.team);
        json::AppendKeyNumber(o, "rank", r.rank);
        json::AppendKeyNumber(o, "status", r.status);
        json::AppendKeyBool(o, "hasScore", r.hasScore);
        json::AppendKeyDouble(o, "score", r.score);
        json::AppendKeyNumber(o, "timeMs", r.timeMs);
        json::AppendKeyString(o, "payload", r.payload);
        json::CloseObject(o);
        return o;
    }));

    json::AppendKeyBool(j, "ended", m.ended);
    json::AppendKeyBool(j, "endSent", m.endSent);
    json::AppendKeyString(j, "endReason", m.endReason);
    json::AppendKeyBool(j, "abandoned", m.abandoned);
    json::AppendKeyNumber(j, "endedOn", m.endedOn);

    json::AppendKeyRaw(j, "attachments", ArrayJson(m.attachments, [](const Attachment& a)
    {
        std::string o = "{";
        json::AppendKeyString(o, "path", WideToUtf8(a.path));
        json::AppendKeyNumber(o, "kind", a.kind);
        json::AppendKeyNumber(o, "startMs", a.startMs);
        json::AppendKeyNumber(o, "endMs", a.endMs);
        json::AppendKeyString(o, "id", a.id);
        json::AppendKeyNumber(o, "size", a.size);
        json::AppendKeyNumber(o, "offset", a.offset);
        json::AppendKeyBool(o, "done", a.done);
        json::AppendKeyBool(o, "failed", a.failed);
        json::CloseObject(o);
        return o;
    }));

    json::CloseObject(j);
    return j;
}

// .interposer/Matches/<external id, made safe>-<hash>. The hash keeps two ids
// that sanitise alike apart.
std::wstring SpoolDirectoryFor(const std::string& externalId)
{
    std::string safe;

    for (char c : externalId)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '_' || c == '.';
        safe += ok ? c : '_';

        if (safe.size() >= 48)
            break;
    }

    unsigned hash = 2166136261u; // FNV-1a

    for (unsigned char c : externalId)
    {
        hash ^= c;
        hash *= 16777619u;
    }

    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "-%08x", hash);

    return GetInterposerDataDirectory() + L"Matches" + platform::kPathSeparator +
        Utf8ToWide(safe + suffix) + platform::kPathSeparator;
}

// ── Sinks ─────────────────────────────────────────────────────────────────────

bool HttpConfigured()
{
    return g_msEnabled && !g_msServerUrl.empty() && !g_msToken.empty() && !g_msGameId.empty();
}

int ChooseSink()
{
    if (!g_msEnabled)
        return INTERPOSER_MATCH_SINK_NONE;

    if (!g_msServerUrl.empty() && !g_msToken.empty())
    {
        if (!g_msGameId.empty())
            return INTERPOSER_MATCH_SINK_HTTP;

        if (!g_warnedGame)
        {
            g_warnedGame = true;
            InterposerLog(L"MATCH ERROR", L"MatchStatistics.ServerUrl and Token are set but GameId is not; matches are not sent");
        }
    }

    return EventPipeConnected() ? INTERPOSER_MATCH_SINK_LAUNCHER : INTERPOSER_MATCH_SINK_NONE;
}

std::string DefaultReporterName()
{
    if (!g_msReporterName.empty())
        return g_msReporterName;

    if (!g_computername.empty())
        return WideToUtf8(g_computername);

    std::wstring name;

    if (platform::GetEnvironmentValue(L"COMPUTERNAME", name) || platform::GetEnvironmentValue(L"HOSTNAME", name))
        return WideToUtf8(name);

    return {};
}

// ── The HTTP worker ───────────────────────────────────────────────────────────

void WorkerMain(void*);

void StartWorkerLocked()
{
    if (g_thread)
        return;

    if (!g_httpOpen)
        g_httpOpen = platform::http::Initialize();

    g_stop    = false;
    g_thread  = platform::StartThread(WorkerMain, nullptr);

    if (!g_thread)
        InterposerLog(L"MATCH ERROR", L"could not start the match statistics thread");
}

enum class Outcome { Ok, Retry, Fail };

struct Reply
{
    Outcome     outcome = Outcome::Retry;
    int         status  = 0;
    std::string body;
    int         retryAfter = -1;
    std::wstring what;
};

Reply Call(const wchar_t* method, const std::string& path, const void* body, size_t bytes,
           const wchar_t* contentType = L"application/json")
{
    // InterposerMatchConfigure may replace these at any time, under g_mutex;
    // never called with it held.
    std::wstring serverUrl;
    std::string  token;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        serverUrl = g_msServerUrl;
        token     = g_msToken;
    }

    const std::wstring url     = serverUrl + Utf8ToWide(path);
    const std::wstring headers = L"Authorization: Bearer " + Utf8ToWide(token) + L"\r\n" +
                                 L"Content-Type: " + contentType + L"\r\n" +
                                 L"Accept: application/json\r\n";

    const platform::http::Response response =
        platform::http::Send(method, url, headers, body, bytes, kRequestTimeout);

    Reply reply;
    reply.status     = response.status;
    reply.body       = response.body;
    reply.retryAfter = response.retryAfterSeconds;

    const int s = response.status;

    if (s >= 200 && s < 300)
        reply.outcome = Outcome::Ok;
    else if (s == 0 || s >= 500 || s == 401 || s == 403 || s == 408 || s == 429)
        reply.outcome = Outcome::Retry;   // unreachable, busy, or a credential to fix: keep it
    else
        reply.outcome = Outcome::Fail;    // this request will never be accepted

    std::wstring summary = std::wstring(method) + L" " + Utf8ToWide(path) + L": ";

    if (s == 0)
        summary += L"no response from " + serverUrl;
    else
    {
        summary += L"HTTP " + std::to_wstring(s);

        if (s == 401 || s == 403)
            summary += L" (check MatchStatistics.Token and its matches:report scope)";
        else if (s == 404 && path == "/api/Matches")
            summary += L" (match statistics may be turned off on the server)";
    }

    reply.what = summary;
    return reply;
}

Reply Call(const wchar_t* method, const std::string& path, const std::string& body)
{
    return Call(method, path, body.data(), body.size());
}

// Note a failed request: keep the error, back off. Caller holds g_mutex.
void NoteFailureLocked(Match& m, const Reply& reply)
{
    if (m.lastError != reply.what)
        InterposerLog(L"MATCH ERROR", reply.what.c_str());

    m.lastError = reply.what;

    if (reply.outcome != Outcome::Retry)
        return;

    m.backoffMs = std::min(kMaxBackoffMs, std::max(1000, m.backoffMs * 2));

    const int wait = reply.status == 429 && reply.retryAfter >= 0 ? reply.retryAfter * 1000 : m.backoffMs;
    m.retryAt = SteadyMs() + wait;
}

void NoteSuccessLocked(Match& m)
{
    m.backoffMs = 0;
    m.retryAt   = 0;
}

bool AttachmentsSettled(const Match& m)
{
    return std::all_of(m.attachments.begin(), m.attachments.end(),
        [](const Attachment& a) { return a.done || a.failed; });
}

bool ParticipantsSent(const Match& m)
{
    return std::all_of(m.participants.begin(), m.participants.end(),
        [](const auto& entry) { return entry.second.sent == entry.second.version; });
}

// Nothing is left to send. Caller holds g_mutex.
bool IdleLocked(const Match& m)
{
    if (m.sink != INTERPOSER_MATCH_SINK_HTTP || m.gaveUp)
        return true;

    return m.registered && m.pending.empty() && ParticipantsSent(m) && (!m.ended || m.endSent) && AttachmentsSettled(m);
}

// Write what the spool is missing. A failed write is retried on the next pass.
void PersistSpool(Match& m)
{
    std::string  matchJson, stateJson, eventLines;
    bool         rewrite = false;
    std::wstring dir;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (m.delivered || m.gaveUp)
            return;

        if (m.spoolDir.empty())
            m.spoolDir = SpoolDirectoryFor(m.externalId);

        dir = m.spoolDir;

        if (m.matchFileDirty)
            matchJson = SpoolMatchJson(m);

        if (m.stateDirty)
            stateJson = SpoolStateJson(m);

        rewrite = m.rewriteEvents;

        for (const Event& e : m.pending)
            if (rewrite || e.sequence > m.spooledSequence)
                eventLines += SpoolEventLine(e);

        m.matchFileDirty  = false;
        m.stateDirty      = false;
        m.rewriteEvents   = false;
        m.spooledSequence = m.lastSequence;
    }

    if (matchJson.empty() && stateJson.empty() && eventLines.empty() && !rewrite)
        return;

    bool ok = true;

    {
        InternalFileAccess internal;

        platform::CreateDirectoryTree(dir);

        auto replace = [&](const wchar_t* name, const std::string& content)
        {
            const std::wstring target = dir + name;
            const std::wstring temp   = target + L".tmp";

            return platform::WriteWholeFile(temp, content) && platform::ReplaceFile(temp, target);
        };

        if (!matchJson.empty())
            ok &= replace(L"match.json", matchJson);

        if (!stateJson.empty())
            ok &= replace(L"state.json", stateJson);

        if (rewrite)
            ok &= replace(L"events.jsonl", eventLines);
        else if (!eventLines.empty())
        {
            const platform::FileHandle file = platform::OpenForAppend(dir + L"events.jsonl");

            if (file == platform::kInvalidFile)
                ok = false;
            else
            {
                ok &= platform::WriteToFile(file, eventLines.data(), eventLines.size());
                platform::CloseFile(file);
            }
        }
    }

    if (!ok)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        m.matchFileDirty = m.stateDirty = m.rewriteEvents = true;

        const std::wstring error = L"could not write " + dir;

        if (m.lastError != error)
            InterposerLog(L"MATCH ERROR", error.c_str());

        m.lastError = error;
    }
}

void RemoveSpool(const std::wstring& dir)
{
    if (dir.empty())
        return;

    InternalFileAccess internal;

    for (const wchar_t* name : { L"match.json", L"state.json", L"events.jsonl",
                                 L"match.json.tmp", L"state.json.tmp", L"events.jsonl.tmp" })
        platform::RemoveFile(dir + name);

    std::wstring trimmed = dir;
    platform::TrimTrailingSeparators(trimmed);
    platform::RemoveEmptyDirectory(trimmed);
}

// One request for one match. Returns true when it made progress and there may
// be more to do; false when the match is idle, waiting out a retry, or failed.
bool Step(Match& m)
{
    // ── Register ─────────────────────────────────────────────────────────────
    std::string body, path;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (m.delivered || m.gaveUp || (m.retryAt && SteadyMs() < m.retryAt))
            return false;

        if (!m.registered)
            body = CreateJson(m, nullptr);
    }

    if (!body.empty())
    {
        const Reply reply = Call(L"POST", "/api/Matches", body);
        const YAML::Node parsed = ParseJson(reply.body);

        std::lock_guard<std::mutex> lock(g_mutex);

        if (reply.outcome == Outcome::Ok && !TextOf(parsed, "matchId").empty())
        {
            m.registered = true;
            m.matchId    = TextOf(parsed, "matchId");
            m.reporterId = TextOf(parsed, "reporterId");

            const long long serverLast = NumberOf(parsed, "lastSequence", 0);

            if (m.sequencesFinal)
            {
                // Numbered in an earlier run: the server has everything up to
                // its last, and nothing after.
                while (!m.pending.empty() && m.pending.front().sequence <= serverLast)
                    m.pending.pop_front();

                m.lastSequence = std::max(m.lastSequence, serverLast);
            }
            else
            {
                long long next = serverLast;

                for (Event& e : m.pending)
                    e.sequence = ++next;

                m.lastSequence   = next;
                m.sequencesFinal = true;
                m.rewriteEvents  = true;
            }

            m.matchFileDirty = true;
            m.lastError.clear();
            NoteSuccessLocked(m);

            LogMatch(L"MATCH SEND", (L"registered " + Wide(m.externalId)).c_str(),
                (L"match " + Wide(m.matchId) + L", resuming after " + std::to_wstring(serverLast)).c_str());
            return true;
        }

        if (reply.outcome == Outcome::Ok)
        {
            Reply bad = reply;
            bad.outcome = Outcome::Retry;
            bad.what    = L"POST /api/Matches: unreadable reply";
            NoteFailureLocked(m, bad);
            return false;
        }

        NoteFailureLocked(m, reply);

        if (reply.outcome == Outcome::Fail)
        {
            // The server will never take this match: drop it, spool and all.
            m.gaveUp = true;
            g_progress.notify_all();
        }

        return false;
    }

    // ── Participants ─────────────────────────────────────────────────────────
    std::vector<std::pair<std::string, unsigned>> sentVersions;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        std::string array = "[";

        for (const auto& [key, p] : m.participants)
        {
            if (p.sent == p.version)
                continue;

            array += ParticipantJson(p);
            array += ',';
            sentVersions.emplace_back(key, p.version);
        }

        json::CloseArray(array);

        if (!sentVersions.empty())
        {
            body = array;
            path = "/api/Matches/" + m.matchId + "/Participants";
        }
    }

    if (!sentVersions.empty())
    {
        const Reply reply = Call(L"POST", path, body);

        std::lock_guard<std::mutex> lock(g_mutex);

        if (reply.outcome == Outcome::Retry)
        {
            NoteFailureLocked(m, reply);
            return false;
        }

        if (reply.outcome == Outcome::Fail)
            NoteFailureLocked(m, reply);   // dropped: they go again with the end
        else
            NoteSuccessLocked(m);

        for (const auto& [key, version] : sentVersions)
            m.participants[key].sent = version;

        m.stateDirty = true;
        return true;
    }

    // ── Events ───────────────────────────────────────────────────────────────
    size_t    batchCount   = 0;
    long long lastInBatch  = 0;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (!m.pending.empty())
        {
            std::string events = "[";

            for (const Event& e : m.pending)
            {
                if (batchCount == kBatchSize)
                    break;

                events += EventJson(e);
                events += ',';
                lastInBatch = e.sequence;
                ++batchCount;
            }

            json::CloseArray(events);

            body = "{";
            json::AppendKeyString(body, "reporterId", m.reporterId);
            json::AppendKeyRaw(body, "events", events);
            json::CloseObject(body);

            path = "/api/Matches/" + m.matchId + "/Events";
        }
    }

    if (batchCount)
    {
        const Reply reply = Call(L"POST", path, body);
        const YAML::Node parsed = ParseJson(reply.body);

        std::lock_guard<std::mutex> lock(g_mutex);

        if (reply.outcome == Outcome::Retry)
        {
            NoteFailureLocked(m, reply);
            return false;
        }

        size_t rejected = 0;

        if (reply.outcome == Outcome::Ok)
        {
            const YAML::Node list = FieldOf(parsed, "rejected");
            rejected = list && list.IsSequence() ? list.size() : 0;
            NoteSuccessLocked(m);
        }
        else
        {
            // The whole batch was refused (a match the server has closed, say).
            rejected = batchCount;
            NoteFailureLocked(m, reply);
        }

        while (!m.pending.empty() && m.pending.front().sequence <= lastInBatch)
            m.pending.pop_front();

        m.rejected += static_cast<DWORD>(rejected);

        LogMatchDiag(L"MATCH SEND", (std::to_wstring(batchCount) + L" events for " + Wide(m.externalId)).c_str(),
            rejected ? (std::to_wstring(rejected) + L" rejected").c_str() : nullptr);

        g_progress.notify_all();
        return true;
    }

    // ── End ──────────────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (m.ended && !m.endSent)
        {
            body = EndJson(m, true);
            path = "/api/Matches/" + m.matchId + "/End";
        }
        else
            path.clear();
    }

    if (!path.empty())
    {
        const Reply reply = Call(L"POST", path, body);

        std::lock_guard<std::mutex> lock(g_mutex);

        if (reply.outcome == Outcome::Retry)
        {
            NoteFailureLocked(m, reply);
            return false;
        }

        if (reply.outcome == Outcome::Fail)
            NoteFailureLocked(m, reply);
        else
        {
            NoteSuccessLocked(m);
            LogMatch(L"MATCH SEND", (L"ended " + Wide(m.externalId)).c_str(), Wide(m.matchId).c_str());
        }

        m.endSent    = true;
        m.stateDirty = true;
        g_progress.notify_all();
        return true;
    }

    // ── Attachments: one request per step ────────────────────────────────────
    Attachment* attachment = nullptr;
    Attachment  copy;
    std::string reporterId, matchId;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        for (Attachment& a : m.attachments)
        {
            if (!a.done && !a.failed)
            {
                attachment = &a;
                copy       = a;
                break;
            }
        }

        reporterId = m.reporterId;
        matchId    = m.matchId;
    }

    if (attachment)
    {
        const std::string base = "/api/Matches/" + matchId + "/Attachments";
        Reply reply;

        // Declare it, with the size as it is now
        if (copy.id.empty())
        {
            long long size;

            {
                InternalFileAccess internal;
                size = platform::FileSize(copy.path);
            }

            if (size < 0)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                attachment->failed = true;
                m.stateDirty       = true;
                m.lastError        = L"cannot read " + copy.path;
                InterposerLog(L"MATCH ERROR", m.lastError.c_str());
                g_progress.notify_all();
                return true;
            }

            copy.size = size;
            reply = Call(L"POST", base, AttachmentJson(copy, reporterId, false));

            const YAML::Node parsed = ParseJson(reply.body);
            const std::string id = TextOf(parsed, "id");

            std::lock_guard<std::mutex> lock(g_mutex);

            if (reply.outcome == Outcome::Ok && !id.empty())
            {
                attachment->id     = id;
                attachment->size   = size;
                attachment->offset = 0;
                m.stateDirty       = true;
                NoteSuccessLocked(m);
                return true;
            }

            if (reply.outcome == Outcome::Ok)
            {
                reply.outcome = Outcome::Retry;
                reply.what    = L"POST " + Utf8ToWide(base) + L": unreadable reply";
            }
        }
        // Send the next chunk
        else if (copy.offset < copy.size)
        {
            std::vector<unsigned char> chunk;
            bool readOk;

            {
                InternalFileAccess internal;
                readOk = platform::ReadFileRange(copy.path, copy.offset,
                    static_cast<size_t>(std::min<long long>(kChunkBytes, copy.size - copy.offset)), chunk);
            }

            if (!readOk || chunk.empty())
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                attachment->failed = true;
                m.stateDirty       = true;
                m.lastError        = L"cannot read " + copy.path;
                InterposerLog(L"MATCH ERROR", m.lastError.c_str());
                g_progress.notify_all();
                return true;
            }

            reply = Call(L"PUT", base + "/" + copy.id + "/Content?offset=" + std::to_string(copy.offset),
                chunk.data(), chunk.size(), L"application/octet-stream");

            std::lock_guard<std::mutex> lock(g_mutex);

            if (reply.outcome == Outcome::Ok)
            {
                attachment->offset += static_cast<long long>(chunk.size());
                m.stateDirty = true;
                NoteSuccessLocked(m);
                return true;
            }
        }
        // Finish it
        else
        {
            reply = Call(L"POST", base + "/" + copy.id + "/Complete", nullptr, 0);

            std::lock_guard<std::mutex> lock(g_mutex);

            if (reply.outcome == Outcome::Ok)
            {
                attachment->done = true;
                m.stateDirty     = true;
                NoteSuccessLocked(m);
                LogMatch(L"MATCH ATTACH", (L"uploaded " + copy.path).c_str(), Wide(m.matchId).c_str());
                g_progress.notify_all();
                return true;
            }
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        NoteFailureLocked(m, reply);

        if (reply.outcome == Outcome::Fail)
        {
            attachment->failed = true;
            m.stateDirty       = true;
            g_progress.notify_all();
            return true;
        }

        return false;
    }

    return false;
}

// Drain one match as far as the server allows, then retire it if done.
void Process(Match& m)
{
    for (int steps = 0; steps < 64 && !g_stop; ++steps)
    {
        PersistSpool(m);

        if (!Step(m))
            break;
    }

    std::wstring spoolToRemove;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (m.delivered)
            return;

        // Done: the end and every attachment are in -- or, for a match a
        // previous run left open and no caller has picked up, everything
        // recorded so far is. Or the server refused the match outright.
        const bool done = m.gaveUp ||
            (IdleLocked(m) && (m.ended || m.orphan));

        if (!done)
            return;

        m.delivered = true;
        spoolToRemove = m.spoolDir;

        if (!m.gaveUp)
            LogMatch(L"MATCH SENT", Wide(m.externalId).c_str(), Wide(m.matchId).c_str());

        g_progress.notify_all();
    }

    RemoveSpool(spoolToRemove);
}

void WorkerMain(void*)
{
    while (!g_stop)
    {
        std::vector<Match*> work;
        long long           nextRetry = 0;

        {
            std::lock_guard<std::mutex> lock(g_mutex);

            for (auto& [handle, match] : g_matches)
                if (match->sink == INTERPOSER_MATCH_SINK_HTTP && !match->delivered)
                    work.push_back(match.get());
        }

        for (Match* m : work)
        {
            if (g_stop)
                break;

            Process(*m);
        }

        std::unique_lock<std::mutex> lock(g_mutex);

        for (Match* m : work)
            if (!m->delivered && m->retryAt && (!nextRetry || m->retryAt < nextRetry))
                nextRetry = m->retryAt;

        long long wait = g_msFlushInterval;

        if (nextRetry)
            wait = std::min<long long>(wait, std::max<long long>(50, nextRetry - SteadyMs()));

        if (!g_flushNow)
            g_wake.wait_for(lock, std::chrono::milliseconds(wait), [] { return g_stop.load() || g_flushNow; });

        g_flushNow = false;
    }
}

void WakeWorkerLocked()
{
    g_flushNow = true;
    g_wake.notify_all();
}

// ── Spool loading ─────────────────────────────────────────────────────────────

void LoadSpooled(const std::wstring& dir)
{
    std::vector<unsigned char> raw;
    InternalFileAccess internal;

    if (!platform::ReadWholeFile(dir + L"match.json", 1 << 20, raw))
        return;

    const YAML::Node meta = ParseJson(std::string(raw.begin(), raw.end()));
    const std::string externalId = TextOf(meta, "externalId");

    if (externalId.empty())
        return;

    auto m = std::make_unique<Match>();
    m->sink           = INTERPOSER_MATCH_SINK_HTTP;
    m->orphan         = true;
    m->spoolDir       = dir;
    m->externalId     = externalId;
    m->map            = TextOf(meta, "map");
    m->mode           = TextOf(meta, "mode");
    m->gameVersion    = TextOf(meta, "gameVersion");
    m->serverName     = TextOf(meta, "serverName");
    m->serverAddress  = TextOf(meta, "serverAddress");
    m->settings       = TextOf(meta, "settings");
    m->metadata       = TextOf(meta, "metadata");
    m->startedOn      = NumberOf(meta, "startedOn", json::NowUnixMilliseconds());
    m->gameId         = TextOf(meta, "gameId");
    m->serverId       = TextOf(meta, "serverId");
    m->reporterKind   = TextOf(meta, "reporterKind");
    m->reporterName   = TextOf(meta, "reporterName");
    m->lastSequence   = NumberOf(meta, "lastSequence", 0);
    m->sequencesFinal = BoolOf(meta, "sequencesFinal");

    // Events: one per line. A line cut short by a crash fails to parse and is
    // dropped; so is any repeat a rewrite left behind.
    if (platform::ReadWholeFile(dir + L"events.jsonl", 512u << 20, raw))
    {
        std::string text(raw.begin(), raw.end());
        size_t start = 0;
        std::map<long long, Event> bySequence;

        while (start < text.size())
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();

            const YAML::Node line = ParseJson(text.substr(start, end - start));
            start = end + 1;

            Event e;
            e.id         = TextOf(line, "id");
            e.sequence   = NumberOf(line, "sequence", 0);
            e.type       = TextOf(line, "type");
            e.gameTimeMs = NumberOf(line, "gameTimeMs", 0);
            e.tick       = NumberOf(line, "tick", -1);
            e.actor      = TextOf(line, "actor");
            e.subject    = TextOf(line, "subject");
            e.payload    = TextOf(line, "payload");
            e.clientTime = TextOf(line, "clientTime");

            if (e.id.empty() || e.type.empty() || e.sequence <= 0)
                continue;

            bySequence[e.sequence] = std::move(e);
        }

        for (auto& [sequence, e] : bySequence)
        {
            m->lastSequence = std::max(m->lastSequence, sequence);
            m->pending.push_back(std::move(e));
        }
    }

    m->spooledSequence = m->lastSequence;
    m->rewriteEvents   = true;   // compact what was read

    if (platform::ReadWholeFile(dir + L"state.json", 64u << 20, raw))
    {
        const YAML::Node state = ParseJson(std::string(raw.begin(), raw.end()));

        if (const YAML::Node list = FieldOf(state, "participants"); list && list.IsSequence())
        {
            for (const YAML::Node& item : list)
            {
                Participant p;
                p.key      = TextOf(item, "key");
                p.name     = TextOf(item, "name");
                p.team     = TextOf(item, "team");
                p.joinedMs = NumberOf(item, "joinedMs", -1);
                p.leftMs   = NumberOf(item, "leftMs", -1);
                p.sent     = BoolOf(item, "sent") ? p.version : 0;

                if (!p.key.empty())
                    m->participants[p.key] = p;
            }
        }

        if (const YAML::Node list = FieldOf(state, "statistics"); list && list.IsSequence())
        {
            for (const YAML::Node& item : list)
                m->statistics.push_back({ TextOf(item, "playerKey"), TextOf(item, "team"),
                    TextOf(item, "key"), DoubleOf(item, "value") });
        }

        if (const YAML::Node list = FieldOf(state, "results"); list && list.IsSequence())
        {
            for (const YAML::Node& item : list)
            {
                Result r;
                r.playerKey = TextOf(item, "playerKey");
                r.team      = TextOf(item, "team");
                r.rank      = static_cast<int>(NumberOf(item, "rank", 0));
                r.status    = std::clamp(static_cast<int>(NumberOf(item, "status", 0)), 0, 4);
                r.hasScore  = BoolOf(item, "hasScore");
                r.score     = DoubleOf(item, "score");
                r.timeMs    = NumberOf(item, "timeMs", -1);
                r.payload   = TextOf(item, "payload");
                m->results.push_back(r);
            }
        }

        m->ended     = BoolOf(state, "ended");
        m->endSent   = BoolOf(state, "endSent");
        m->endReason = TextOf(state, "endReason");
        m->abandoned = BoolOf(state, "abandoned");
        m->endedOn   = NumberOf(state, "endedOn", 0);

        if (const YAML::Node list = FieldOf(state, "attachments"); list && list.IsSequence())
        {
            for (const YAML::Node& item : list)
            {
                Attachment a;
                a.path    = Utf8ToWide(TextOf(item, "path"));
                a.kind    = std::clamp(static_cast<int>(NumberOf(item, "kind", 4)), 0, 4);
                a.startMs = NumberOf(item, "startMs", -1);
                a.endMs   = NumberOf(item, "endMs", -1);
                a.id      = TextOf(item, "id");
                a.size    = NumberOf(item, "size", -1);
                a.offset  = NumberOf(item, "offset", 0);
                a.done    = BoolOf(item, "done");
                a.failed  = BoolOf(item, "failed");

                if (!a.path.empty())
                    m->attachments.push_back(a);
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    if (m->gameId.empty())
        m->gameId = g_msGameId;

    if (m->reporterKind.empty())
        m->reporterKind = g_msReporterKind;

    m->handle = g_nextHandle++;

    LogMatch(L"MATCH START", (L"resuming " + Wide(m->externalId) + L" from " + dir).c_str(),
        (std::to_wstring(m->pending.size()) + L" events unsent").c_str());

    g_matches[m->handle] = std::move(m);
}

// ── Exports: shared checks ────────────────────────────────────────────────────

// The match for a handle, or null. Caller holds g_mutex.
Match* FindLocked(DWORD handle)
{
    auto it = g_matches.find(handle);
    return it == g_matches.end() ? nullptr : it->second.get();
}

// The match, if it may still be changed. Caller holds g_mutex.
Match* OpenLocked(DWORD handle)
{
    Match* m = FindLocked(handle);
    return m && !m->ended && !m->orphan ? m : nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

namespace
{

std::atomic<bool> g_spoolLoaded{ false };

// Load what a previous run left in .interposer/Matches/, once, as soon as the
// HTTP settings are complete -- from InitMatches when Config.yml has them, or
// from the InterposerMatchConfigure call that completes them. reportWaiting
// logs the unsent count when they are not complete yet (at startup only, so a
// host configuring piecemeal does not repeat it).
void LoadSpoolOnce(bool reportWaiting)
{
    if (g_spoolLoaded)
        return;

    bool configured;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        configured = HttpConfigured();
    }

    const std::wstring root = GetInterposerDataDirectory() + L"Matches" + platform::kPathSeparator;

    std::vector<platform::DirectoryEntry> entries;

    {
        InternalFileAccess internal;
        entries = platform::ListDirectory(root);
    }

    size_t waiting = 0;

    for (const auto& entry : entries)
        if (entry.isDirectory)
            ++waiting;

    if (!waiting)
    {
        if (configured)
            g_spoolLoaded = true;
        return;
    }

    if (!configured)
    {
        if (reportWaiting)
            InterposerLog(L"MATCH ERROR", (std::to_wstring(waiting) +
                L" unsent matches are waiting in " + root +
                L"; MatchStatistics.ServerUrl, Token and GameId are needed to send them").c_str());
        return;
    }

    if (g_spoolLoaded.exchange(true))
        return;   // another thread got here first

    for (const auto& entry : entries)
        if (entry.isDirectory)
            LoadSpooled(root + entry.name + platform::kPathSeparator);

    std::lock_guard<std::mutex> lock(g_mutex);

    if (!g_matches.empty())
        StartWorkerLocked();
}

} // namespace

void InitMatches()
{
    LoadSpoolOnce(true);
}

void ShutdownMatches(bool processTerminating)
{
    if (processTerminating || !g_thread)
        return;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
        g_wake.notify_all();
        g_progress.notify_all();
    }

    const bool parked = platform::StopThread(g_thread, kStopWaitMs);
    g_thread = nullptr;

    // A thread terminated mid-request may hold WinHTTP's locks; leave the
    // client alone rather than risk a hang closing it.
    if (parked && g_httpOpen)
        platform::http::Shutdown();

    g_httpOpen = false;
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

extern "C" INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchConfigure(const InterposerMatchConfig* config)
{
    FORWARD_TO_PRIMARY(InterposerMatchConfigure, config);

    if (!config || config->structSize < sizeof(InterposerMatchConfig))
        return FALSE;

    // Check everything before changing anything.
    std::wstring url;
    std::string  token, kind;

    if (config->serverUrl && !NormalizeMatchServerUrl(Utf8ToWide(config->serverUrl), url))
    {
        InterposerLog(L"MATCH ERROR", (L"InterposerMatchConfigure: serverUrl is not an http:// or https:// URL: " + url).c_str());
        return FALSE;
    }

    if (config->token)
        token = WideToUtf8(ExpandEnvVars(Utf8ToWide(config->token)));

    if (config->gameId && *config->gameId && !IsGuidText(config->gameId))
    {
        InterposerLog(L"MATCH ERROR", (L"InterposerMatchConfigure: gameId is not a GUID: " + Utf8ToWide(config->gameId)).c_str());
        return FALSE;
    }

    if (config->serverId && *config->serverId && !IsGuidText(config->serverId))
    {
        InterposerLog(L"MATCH ERROR", (L"InterposerMatchConfigure: serverId is not a GUID: " + Utf8ToWide(config->serverId)).c_str());
        return FALSE;
    }

    if (config->reporterKind && !NormalizeReporterKind(config->reporterKind, kind))
    {
        InterposerLog(L"MATCH ERROR", (L"InterposerMatchConfigure: reporterKind is not Server, Host or Client: " +
            Utf8ToWide(config->reporterKind)).c_str());
        return FALSE;
    }

    if (config->flushIntervalMs != 0 &&
        (config->flushIntervalMs < kMinMatchFlushInterval || config->flushIntervalMs > kMaxMatchFlushInterval))
    {
        InterposerLog(L"MATCH ERROR", L"InterposerMatchConfigure: flushIntervalMs must be between 100 and 600000");
        return FALSE;
    }

    bool configured;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (config->enabled >= 0)     g_msEnabled       = config->enabled != 0;
        if (config->serverUrl)        g_msServerUrl     = url;
        if (config->token)            g_msToken         = token;
        if (config->gameId)           g_msGameId        = config->gameId;
        if (config->serverId)         g_msServerId      = config->serverId;
        if (config->reporterKind)     g_msReporterKind  = kind;
        if (config->reporterName)     g_msReporterName  = config->reporterName;
        if (config->flushIntervalMs)  g_msFlushInterval = config->flushIntervalMs;

        g_warnedGame = false;   // a new configuration earns a new warning
        configured   = HttpConfigured();

        // The token is never logged.
        LogMatch(L"MATCH CONFIG", configured ? (L"sending to " + g_msServerUrl).c_str() : L"HTTP not configured",
            g_msEnabled ? nullptr : L"disabled");

        // A request backing off may have failed on the old settings (a wrong
        // token, an unreachable URL): retry everything now.
        for (auto& [h, m] : g_matches)
        {
            m->retryAt   = 0;
            m->backoffMs = 0;
        }

        if (g_thread)
            WakeWorkerLocked();
    }

    if (configured)
        LoadSpoolOnce(false);

    return TRUE;
}

extern "C" INTERPOSER_API DWORD INTERPOSER_CALL InterposerMatchStart(const InterposerMatchInfo* info)
{
    FORWARD_TO_PRIMARY(InterposerMatchStart, info);

    if (!info || info->structSize < sizeof(InterposerMatchInfo) ||
        !ValidJsonOrNull(info->settingsJson) || !ValidJsonOrNull(info->metadataJson))
        return 0;

    const std::string externalId = info->externalId && *info->externalId ? info->externalId : json::NewGuid();

    DWORD       handle;
    std::string startJson;
    int         sink;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        // The same id again carries on with the match: one this process is
        // still recording, or one a previous run left in the spool.
        for (auto& [h, existing] : g_matches)
        {
            if (existing->externalId != externalId || existing->ended || existing->gaveUp)
                continue;

            if (existing->orphan && existing->delivered)
                continue;

            existing->orphan = false;
            LogMatch(L"MATCH START", (L"continuing " + Wide(externalId)).c_str());
            return h;
        }

        auto m = std::make_unique<Match>();
        m->handle        = g_nextHandle++;
        m->sink          = ChooseSink();
        m->externalId    = externalId;
        m->map           = Str(info->map);
        m->mode          = Str(info->mode);
        m->gameVersion   = Str(info->gameVersion);
        m->serverName    = Str(info->serverName);
        m->serverAddress = Str(info->serverAddress);
        m->settings      = info->settingsJson && *info->settingsJson ? info->settingsJson : "";
        m->metadata      = info->metadataJson && *info->metadataJson ? info->metadataJson : "";
        m->startedOn     = info->startedOn > 0 ? info->startedOn : json::NowUnixMilliseconds();
        m->gameId        = g_msGameId;
        m->serverId      = g_msServerId;
        m->reporterKind  = g_msReporterKind;
        m->reporterName  = DefaultReporterName();

        handle = m->handle;
        sink   = m->sink;

        const bool forward = sink == INTERPOSER_MATCH_SINK_LAUNCHER;
        startJson = CreateJson(*m, &forward);

        LogMatch(L"MATCH START", Wide(externalId).c_str(),
            (L"reported to: " + std::wstring(kSinkNames[sink])).c_str());

        g_matches[handle] = std::move(m);

        if (sink == INTERPOSER_MATCH_SINK_HTTP)
        {
            StartWorkerLocked();
            WakeWorkerLocked();
        }
    }

    Fire(L"START", handle, startJson);
    return handle;
}

extern "C" INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchPlayerJoined(DWORD match, const char* playerKey,
    const char* displayName, const char* team, long long gameTimeMs)
{
    FORWARD_TO_PRIMARY(InterposerMatchPlayerJoined, match, playerKey, displayName, team, gameTimeMs);

    if (!playerKey || !*playerKey)
        return FALSE;

    std::string body;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        Match* m = OpenLocked(match);
        if (!m)
            return FALSE;

        Participant& p = m->participants[playerKey];
        p.key  = playerKey;

        if (displayName)
            p.name = displayName;
        if (team)
            p.team = team;
        if (gameTimeMs >= 0 && p.joinedMs < 0)
            p.joinedMs = gameTimeMs;

        p.leftMs = -1;   // back again
        ++p.version;
        m->stateDirty = true;

        body = ParticipantJson(p);

        LogMatchDiag(L"MATCH PLAYER", (Wide(m->externalId) + L": " + Wide(p.key)).c_str(),
            (L"joined as " + Wide(p.name)).c_str());
    }

    Fire(L"PARTICIPANT", match, body);
    return TRUE;
}

extern "C" INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchPlayerLeft(DWORD match, const char* playerKey,
    long long gameTimeMs)
{
    FORWARD_TO_PRIMARY(InterposerMatchPlayerLeft, match, playerKey, gameTimeMs);

    if (!playerKey || !*playerKey)
        return FALSE;

    std::string body;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        Match* m = OpenLocked(match);
        if (!m)
            return FALSE;

        Participant& p = m->participants[playerKey];
        p.key    = playerKey;
        p.leftMs = gameTimeMs >= 0 ? gameTimeMs : m->latestGameTimeMs;
        ++p.version;
        m->stateDirty = true;

        body = ParticipantJson(p);

        LogMatchDiag(L"MATCH PLAYER", (Wide(m->externalId) + L": " + Wide(p.key)).c_str(), L"left");
    }

    Fire(L"PARTICIPANT", match, body);
    return TRUE;
}

extern "C" INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchEvent(DWORD match, const char* type,
    long long gameTimeMs, long long tick, const char* actorKey, const char* subjectKey, const char* payloadJson)
{
    FORWARD_TO_PRIMARY(InterposerMatchEvent, match, type, gameTimeMs, tick, actorKey, subjectKey, payloadJson);

    if (!type || !*type || gameTimeMs < 0 || !ValidJsonOrNull(payloadJson))
        return FALSE;

    Event e;
    e.id         = json::NewGuid();
    e.type       = type;
    e.gameTimeMs = gameTimeMs;
    e.tick       = tick >= 0 ? tick : -1;
    e.actor      = Str(actorKey);
    e.subject    = Str(subjectKey);
    e.payload    = payloadJson && *payloadJson ? payloadJson : "";
    e.clientTime = json::FormatUtc(json::NowUnixMilliseconds());

    std::string body;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        Match* m = OpenLocked(match);
        if (!m)
            return FALSE;

        e.sequence = ++m->lastSequence;
        m->latestGameTimeMs = std::max(m->latestGameTimeMs, e.gameTimeMs);
        body = EventJson(e);

        LogMatchDiag(L"MATCH EVENT", (Wide(m->externalId) + L" #" + std::to_wstring(e.sequence)).c_str(),
            Wide(e.type).c_str());

        if (m->sink == INTERPOSER_MATCH_SINK_HTTP)
            m->pending.push_back(std::move(e));
    }

    Fire(L"EVENT", match, body);
    return TRUE;
}

// Set or add a statistic. Caller validates.
static BOOL ApplyStatistic(DWORD match, const char* playerKey, const char* team, const char* key,
                           double value, bool add)
{
    if (!key || !*key || !std::isfinite(value) || ((!playerKey || !*playerKey) && (!team || !*team)))
        return FALSE;

    std::lock_guard<std::mutex> lock(g_mutex);

    Match* m = OpenLocked(match);
    if (!m)
        return FALSE;

    const std::string player = Str(playerKey), group = Str(team);

    for (Statistic& s : m->statistics)
    {
        if (s.playerKey == player && s.team == group && s.key == key)
        {
            const double next = add ? s.value + value : value;

            if (!std::isfinite(next))
                return FALSE;

            s.value = next;
            m->stateDirty = true;
            return TRUE;
        }
    }

    m->statistics.push_back({ player, group, key, value });
    m->stateDirty = true;
    return TRUE;
}

extern "C" INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchSetStatistic(DWORD match, const char* playerKey,
    const char* team, const char* key, double value)
{
    FORWARD_TO_PRIMARY(InterposerMatchSetStatistic, match, playerKey, team, key, value);

    return ApplyStatistic(match, playerKey, team, key, value, false);
}

extern "C" INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchAddStatistic(DWORD match, const char* playerKey,
    const char* team, const char* key, double delta)
{
    FORWARD_TO_PRIMARY(InterposerMatchAddStatistic, match, playerKey, team, key, delta);

    return ApplyStatistic(match, playerKey, team, key, delta, true);
}

extern "C" INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchSetResult(DWORD match, const InterposerMatchResult* result)
{
    FORWARD_TO_PRIMARY(InterposerMatchSetResult, match, result);

    if (!result || result->structSize < sizeof(InterposerMatchResult) ||
        result->status < 0 || result->status > 4 ||
        ((!result->playerKey || !*result->playerKey) && (!result->team || !*result->team)) ||
        (result->hasScore && !std::isfinite(result->score)) ||
        !ValidJsonOrNull(result->payloadJson))
        return FALSE;

    Result r;
    r.playerKey = Str(result->playerKey);
    r.team      = Str(result->team);
    r.rank      = result->rank;
    r.status    = result->status;
    r.hasScore  = result->hasScore != 0;
    r.score     = r.hasScore ? result->score : 0;
    r.timeMs    = result->timeMs >= 0 ? result->timeMs : -1;
    r.payload   = result->payloadJson && *result->payloadJson ? result->payloadJson : "";

    std::lock_guard<std::mutex> lock(g_mutex);

    Match* m = OpenLocked(match);
    if (!m)
        return FALSE;

    for (Result& existing : m->results)
    {
        if (existing.playerKey == r.playerKey && existing.team == r.team)
        {
            existing = r;
            m->stateDirty = true;
            return TRUE;
        }
    }

    m->results.push_back(r);
    m->stateDirty = true;
    return TRUE;
}

extern "C" INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchEnd(DWORD match, const char* reason, BOOL abandoned)
{
    FORWARD_TO_PRIMARY(InterposerMatchEnd, match, reason, abandoned);

    std::string body;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        Match* m = OpenLocked(match);
        if (!m)
            return FALSE;

        m->ended      = true;
        m->endReason  = Str(reason);
        m->abandoned  = abandoned != FALSE;
        m->endedOn    = json::NowUnixMilliseconds();
        m->stateDirty = true;

        body = EndJson(*m, false);

        LogMatch(L"MATCH END", Wide(m->externalId).c_str(),
            (Wide(m->endReason.empty() ? std::string("ended") : m->endReason) +
             (m->abandoned ? L", abandoned" : L"")).c_str());

        if (m->sink == INTERPOSER_MATCH_SINK_HTTP)
            WakeWorkerLocked();
    }

    Fire(L"END", match, body);
    return TRUE;
}

extern "C" INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchAttachFile(DWORD match, const wchar_t* path, int kind,
    long long startMs, long long endMs)
{
    FORWARD_TO_PRIMARY(InterposerMatchAttachFile, match, path, kind, startMs, endMs);

    if (!path || !*path || kind < 0 || kind > 4)
        return FALSE;

    Attachment a;
    a.kind    = kind;
    a.startMs = startMs >= 0 ? startMs : -1;
    a.endMs   = endMs >= 0 ? endMs : -1;

    {
        InternalFileAccess internal;

        // Absolute, so it still names the file after the game changes directory.
        std::wstring full;
        a.path = platform::GetFullPath(path, full) ? full : path;
        a.size = platform::FileSize(a.path);
    }

    if (a.size < 0)
        return FALSE;

    std::string body;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        Match* m = FindLocked(match);
        if (!m || m->orphan)
            return FALSE;

        body = AttachmentJson(a, std::string(), true);

        LogMatch(L"MATCH ATTACH", (Wide(m->externalId) + L": " + a.path).c_str(),
            m->sink == INTERPOSER_MATCH_SINK_NONE ? L"not uploaded: no LANCommander server or launcher" : nullptr);

        if (m->sink == INTERPOSER_MATCH_SINK_HTTP)
        {
            m->attachments.push_back(a);
            m->stateDirty = true;

            // A match already delivered goes back in the queue for this file.
            if (m->delivered && !m->gaveUp)
            {
                m->delivered      = false;
                m->matchFileDirty = true;  // its spool was removed with it
                m->rewriteEvents  = true;
            }

            WakeWorkerLocked();
        }
    }

    Fire(L"ATTACH", match, body);
    return TRUE;
}

extern "C" INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchFlush(DWORD match, DWORD timeoutMs)
{
    FORWARD_TO_PRIMARY(InterposerMatchFlush, match, timeoutMs);

    std::unique_lock<std::mutex> lock(g_mutex);

    Match* m = FindLocked(match);
    if (!m)
        return FALSE;

    if (m->sink != INTERPOSER_MATCH_SINK_HTTP)
        return TRUE;

    WakeWorkerLocked();

    return g_progress.wait_for(lock, std::chrono::milliseconds(timeoutMs),
        [m] { return g_stop.load() || IdleLocked(*m); }) && IdleLocked(*m) ? TRUE : FALSE;
}

extern "C" INTERPOSER_API BOOL INTERPOSER_CALL InterposerMatchGetStatus(DWORD match, InterposerMatchStatus* status)
{
    FORWARD_TO_PRIMARY(InterposerMatchGetStatus, match, status);

    if (!status || status->size < sizeof(InterposerMatchStatus))
        return FALSE;

    std::lock_guard<std::mutex> lock(g_mutex);

    const Match* m = FindLocked(match);
    if (!m)
        return FALSE;

    const DWORD size = status->size;
    std::memset(status, 0, sizeof(InterposerMatchStatus));
    status->size = size;

    status->sink = m->sink;
    std::snprintf(status->matchId, sizeof(status->matchId), "%s", m->matchId.c_str());
    status->pendingEvents  = static_cast<DWORD>(m->pending.size());
    status->rejectedEvents = m->rejected;

    for (const Attachment& a : m->attachments)
        if (!a.done && !a.failed)
            ++status->pendingAttachments;

    status->ended     = m->ended ? 1 : 0;
    status->delivered = m->sink == INTERPOSER_MATCH_SINK_HTTP ? (m->delivered && !m->gaveUp ? 1 : 0)
                                                              : (m->ended ? 1 : 0);

    const size_t capacity = sizeof(status->lastError) / sizeof(status->lastError[0]);
    const size_t length   = std::min(m->lastError.size(), capacity - 1);
    std::copy(m->lastError.begin(), m->lastError.begin() + static_cast<std::ptrdiff_t>(length), status->lastError);
    status->lastError[length] = L'\0';

    return TRUE;
}
