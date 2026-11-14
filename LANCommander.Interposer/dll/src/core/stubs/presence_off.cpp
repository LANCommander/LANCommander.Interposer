// Rich presence's exports, for a build without it: always on Linux, until it
// has a Discord IPC of its own, and in an embedded build with
// INTERPOSER_FEATURE_RICH_PRESENCE off. See core/interposer_features.h.

#include "instance.h"

extern "C" {

INTERPOSER_API void InterposerSetPresenceDetails(const char* details)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceDetails, details);
}

INTERPOSER_API void InterposerSetPresenceState(const char* state)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceState, state);
}

INTERPOSER_API void InterposerSetPresenceTimestamps(long long start, long long end)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceTimestamps, start, end);
}

INTERPOSER_API void InterposerSetPresenceLargeImage(const char* key, const char* text)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceLargeImage, key, text);
}

INTERPOSER_API void InterposerSetPresenceSmallImage(const char* key, const char* text)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceSmallImage, key, text);
}

INTERPOSER_API void InterposerSetPresenceParty(const char* id, int size, int max)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceParty, id, size, max);
}

INTERPOSER_API void InterposerSetPresenceButton(int index, const char* text, const char* url)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceButton, index, text, url);
}

INTERPOSER_API void InterposerSetPresenceType(int type)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceType, type);
}

INTERPOSER_API void InterposerSetPresenceName(const char* name)
{
    FORWARD_TO_PRIMARY(InterposerSetPresenceName, name);
}

INTERPOSER_API void InterposerUpdatePresence(void)
{
    FORWARD_TO_PRIMARY(InterposerUpdatePresence);
}

INTERPOSER_API void InterposerClearPresence(void)
{
    FORWARD_TO_PRIMARY(InterposerClearPresence);
}

} // extern "C"
