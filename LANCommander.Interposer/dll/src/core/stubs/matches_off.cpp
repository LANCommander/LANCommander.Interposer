// Match statistics' exports, for an embedded build with
// INTERPOSER_FEATURE_MATCHES off (see core/interposer_features.h). No match can be
// started, so every call that takes a handle fails as it does for an unknown
// one.

#include "instance.h"

extern "C" {

INTERPOSER_API BOOL InterposerMatchConfigure(const InterposerMatchConfig* config)
{
    FORWARD_TO_PRIMARY(InterposerMatchConfigure, config);
    return FALSE;
}

INTERPOSER_API DWORD InterposerMatchStart(const InterposerMatchInfo* info)
{
    FORWARD_TO_PRIMARY(InterposerMatchStart, info);
    return 0;
}

INTERPOSER_API BOOL InterposerMatchPlayerJoined(DWORD match, const char* playerKey, const char* displayName,
    const char* team, long long gameTimeMs)
{
    FORWARD_TO_PRIMARY(InterposerMatchPlayerJoined, match, playerKey, displayName, team, gameTimeMs);
    return FALSE;
}

INTERPOSER_API BOOL InterposerMatchPlayerLeft(DWORD match, const char* playerKey, long long gameTimeMs)
{
    FORWARD_TO_PRIMARY(InterposerMatchPlayerLeft, match, playerKey, gameTimeMs);
    return FALSE;
}

INTERPOSER_API BOOL InterposerMatchEvent(DWORD match, const char* type, long long gameTimeMs, long long tick,
    const char* actorKey, const char* subjectKey, const char* payloadJson)
{
    FORWARD_TO_PRIMARY(InterposerMatchEvent, match, type, gameTimeMs, tick, actorKey, subjectKey, payloadJson);
    return FALSE;
}

INTERPOSER_API BOOL InterposerMatchSetStatistic(DWORD match, const char* playerKey, const char* team,
    const char* key, double value)
{
    FORWARD_TO_PRIMARY(InterposerMatchSetStatistic, match, playerKey, team, key, value);
    return FALSE;
}

INTERPOSER_API BOOL InterposerMatchAddStatistic(DWORD match, const char* playerKey, const char* team,
    const char* key, double delta)
{
    FORWARD_TO_PRIMARY(InterposerMatchAddStatistic, match, playerKey, team, key, delta);
    return FALSE;
}

INTERPOSER_API BOOL InterposerMatchSetResult(DWORD match, const InterposerMatchResult* result)
{
    FORWARD_TO_PRIMARY(InterposerMatchSetResult, match, result);
    return FALSE;
}

INTERPOSER_API BOOL InterposerMatchEnd(DWORD match, const char* reason, BOOL abandoned)
{
    FORWARD_TO_PRIMARY(InterposerMatchEnd, match, reason, abandoned);
    return FALSE;
}

INTERPOSER_API BOOL InterposerMatchAttachFile(DWORD match, const wchar_t* path, int kind,
    long long startMs, long long endMs)
{
    FORWARD_TO_PRIMARY(InterposerMatchAttachFile, match, path, kind, startMs, endMs);
    return FALSE;
}

INTERPOSER_API BOOL InterposerMatchFlush(DWORD match, DWORD timeoutMs)
{
    FORWARD_TO_PRIMARY(InterposerMatchFlush, match, timeoutMs);
    return FALSE;
}

INTERPOSER_API BOOL InterposerMatchGetStatus(DWORD match, InterposerMatchStatus* status)
{
    FORWARD_TO_PRIMARY(InterposerMatchGetStatus, match, status);
    return FALSE;
}

} // extern "C"
