#include "file_access.h"
#include "config.h"
#include "fastdl.h"

// ---------------------------------------------------------------------------
// InternalFileAccess
// ---------------------------------------------------------------------------

namespace
{
    thread_local int t_internalDepth = 0;
}

InternalFileAccess::InternalFileAccess()  { ++t_internalDepth; }
InternalFileAccess::~InternalFileAccess() { --t_internalDepth; }

bool InternalFileAccess::Active()
{
    return t_internalDepth > 0;
}

// ---------------------------------------------------------------------------
// Redirects
// ---------------------------------------------------------------------------

// Applies file redirects and, at Debug level or above, records whether a rule
// actually fired. The [FILE REDIRECT] / plain-verb lines are the callers'.
std::wstring RedirectFilePath(const std::wstring& path)
{
    // Below Debug there is nothing to report, so skip the bookkeeping entirely.
    if (!g_logFiles || g_logLevel < LogLevel::Debug)
        return ApplyFileRedirects(path);

    FileRedirectMatch match;
    std::wstring result = ApplyFileRedirects(path, &match);

    if (match.matched)
    {
        std::wstring detail = L"rule #" + std::to_wstring(match.ruleIndex + 1) + L"  " + match.pattern;

        // Only worth showing when %TOKEN% substitution rewrote the rule: the
        // author needs to see both what they wrote and what the regex compiled.
        if (!match.source.empty() && match.source != match.pattern)
            detail += L"  (as written: " + match.source + L")";

        LogFileDiag(L"REDIRECT HIT", path.c_str(), detail.c_str());
    }
    else
    {
        std::wstring detail = match.ruleCount == 0
            ? std::wstring(L"no rules configured")
            : std::to_wstring(match.ruleCount)
                + (match.ruleCount == 1 ? L" rule, none matched" : L" rules, none matched");

        LogFileDiag(L"REDIRECT MISS", path.c_str(), detail.c_str());

        // Trace only — `tried` is left empty at Debug.
        for (const auto& pattern : match.tried)
            LogFileDiag(L"REDIRECT RULE", path.c_str(), pattern.c_str());
    }

    return result;
}

void EnsureParentDirectoryExists(const std::wstring& filePath)
{
    const size_t pos = filePath.find_last_of(L"\\/");

    if (pos == std::wstring::npos || pos == 0)
        return;

    // Through the platform layer, which on Linux reaches mkdir and stat -- both
    // hooked there, and neither to be redirected for this.
    InternalFileAccess internal;
    platform::CreateDirectoryTree(filePath.substr(0, pos));
}

static const wchar_t* AccessVerb(FileAccessMode mode)
{
    switch (mode)
    {
    case FileAccessMode::ReadWrite: return L"FILE R/W";
    case FileAccessMode::Write:     return L"FILE WRITE";
    default:                        return L"FILE READ";
    }
}

std::wstring ResolveOpenPath(const std::wstring& path, FileAccessMode mode)
{
    const bool writes = mode != FileAccessMode::Read;

    std::wstring redirected = RedirectFilePath(path);

    if (redirected != path)
    {
        LogFileAccess(L"FILE REDIRECT", path.c_str(), redirected.c_str());

        // Ensure the destination directory tree exists for write operations
        if (writes)
            EnsureParentDirectoryExists(redirected);
    }
    else
        LogFileAccess(AccessVerb(mode), path.c_str());

    // FastDL: for read-only opens, ensure the file is downloaded (or cached) if eligible.
    if (!writes)
    {
        std::wstring overlayPath;

        {
            InternalFileAccess internal;
            TryFastDLDownload(redirected);

            // Serve from the overlay directory if a cached copy exists there
            overlayPath = GetExistingOverlayPath(redirected);
        }

        if (!overlayPath.empty())
        {
            LogFileAccess(L"FILE OVERLAY", redirected.c_str(), overlayPath.c_str());
            return overlayPath;
        }
    }

    return redirected;
}

std::wstring ResolvePath(const std::wstring& path, const wchar_t* verb)
{
    std::wstring redirected = RedirectFilePath(path);

    if (redirected != path)
        LogFileAccess(L"FILE REDIRECT", path.c_str(), redirected.c_str());
    else
        LogFileAccess(verb, path.c_str());

    return redirected;
}

std::wstring AttributeFallbackPath(const std::wstring& path)
{
    InternalFileAccess internal;
    return GetExistingOverlayPath(path);
}

void ResolveTwoPaths(const std::wstring& source, const std::wstring& destination,
                     const wchar_t* verb, std::wstring& outSource, std::wstring& outDestination)
{
    outSource      = RedirectFilePath(source);
    outDestination = RedirectFilePath(destination);

    if (outSource != source)
        LogFileAccess(L"FILE REDIRECT", source.c_str(), outSource.c_str());

    if (outDestination != destination)
    {
        LogFileAccess(L"FILE REDIRECT", destination.c_str(), outDestination.c_str());
        EnsureParentDirectoryExists(outDestination);
    }

    if (outSource == source && outDestination == destination)
        LogFileAccess(verb, source.c_str(), destination.c_str());
}
