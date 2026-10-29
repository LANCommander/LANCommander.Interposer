#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "fastdl.h"
#include "config.h"
#include "http.h"

// Response header that a LANCommander FastDL endpoint must return.
// Any value is accepted — its mere presence confirms the endpoint is ours.
static constexpr const wchar_t* FASTDL_PROBE_HEADER = L"X-FastDL";

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
static std::wstring           g_fastdlDownloadBase;           // resolved overlay directory (trailing separator)
static std::vector<std::wstring> g_blockedPaths;              // absolute lowercase paths that may not be overwritten

// URLs discovered at runtime via ProbeServerForFastDL.
// Protected by g_probedUrlMutex. Used as BaseUrl fallback when g_fastdlBaseUrl is empty.
// Verified   = server returned FASTDL_PROBE_HEADER (preferred).
// Unverified = server returned HTTP 200 without the header (fallback).
static std::wstring      g_probedBaseUrlVerified;
static std::wstring      g_probedBaseUrl;
static std::shared_mutex g_probedUrlMutex;

// ---------------------------------------------------------------------------
// CRC32 (ISO 3309 / ITU-T V.42, polynomial 0xEDB88320)
// ---------------------------------------------------------------------------
static uint32_t ComputeCRC32(const uint8_t* data, size_t len)
{
    // Build lookup table once
    static uint32_t table[256]{};
    static bool     tableReady = false;

    if (!tableReady)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        tableReady = true;
    }

    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);

    return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring ToLowerFastDL(std::wstring input)
{
    for (auto& character : input)
        character = static_cast<wchar_t>(std::towlower(character));

    return input;
}

static bool EqualsNoCase(const std::wstring& a, const std::wstring& b)
{
    return ToLowerFastDL(a) == ToLowerFastDL(b);
}

// Returns the lowercase file extension including the dot (e.g. L".pk3"),
// or an empty string if there is no extension.
static std::wstring FileExtension(const std::wstring& path)
{
    size_t dot = path.rfind(L'.');

    if (dot == std::wstring::npos)
        return {};

    // Make sure the dot isn't part of a directory name
    size_t slash = path.find_last_of(L"\\/");

    if (slash != std::wstring::npos && dot < slash)
        return {};

    return ToLowerFastDL(path.substr(dot));
}

// Returns true if the file extension is in the allowed list (or the list is empty).
static bool IsExtensionAllowed(const std::wstring& path)
{
    if (g_fastdlAllowedExtensions.empty())
        return true;

    std::wstring extensions = FileExtension(path);

    if (extensions.empty())
        return false;

    for (const auto& allowed : g_fastdlAllowedExtensions)
        if (extensions == allowed) return true;

    return false;
}

// Result of a FastDL probe request.
enum class ProbeResult { NotFound, Unverified, Verified };

// HEAD `url` with custom headers.
// Returns Verified if HTTP 200 and FASTDL_PROBE_HEADER is present,
// Unverified if HTTP 200 without the header, NotFound otherwise.
static ProbeResult DoProbeRequest(const std::wstring& url, const std::wstring& additionalHeaders)
{
    // Use a short timeout so probing doesn't stall the file-load hook for long.
    const int timeout = g_fastdlProbeTimeout > 0 ? g_fastdlProbeTimeout : 2000;

    const platform::http::HeadResult result =
        platform::http::Head(url, additionalHeaders, FASTDL_PROBE_HEADER, timeout);

    if (result.status != 200)
        return ProbeResult::NotFound;

    // Check for the LANCommander verification header (any value is accepted).
    return result.headerPresent ? ProbeResult::Verified : ProbeResult::Unverified;
}

// Returns the effective FastDL base URL.
// Priority: configured BaseUrl > verified probe URL > unverified probe URL.
static std::wstring GetEffectiveBaseUrl()
{
    if (!g_fastdlBaseUrl.empty())
        return g_fastdlBaseUrl;

    std::shared_lock<std::shared_mutex> lk(g_probedUrlMutex);
    return g_probedBaseUrlVerified.empty() ? g_probedBaseUrl : g_probedBaseUrlVerified;
}

// The FastDLPath whose Local prefix covers localPath, with the part of the path
// after the prefix in `relative` (leading separators stripped). Tried first
// against the path as the game passed it, which is what a relative Local
// matches, then against its full path -- so an absolute Local, including one
// built from %GAMEDIR%, also covers a game that opens files relative to its
// working directory.
static const FastDLPath* MatchFastDLPath(const std::wstring& localPath, std::wstring& relative)
{
    auto match = [&](const std::wstring& candidate) -> const FastDLPath*
    {
        const std::wstring lower = ToLowerFastDL(candidate);

        for (const auto& mapping : g_fastdlPaths)
        {
            if (!lower.starts_with(ToLowerFastDL(mapping.localPrefix)))
                continue;

            relative = candidate.substr(mapping.localPrefix.size());

            while (!relative.empty() && (relative[0] == L'\\' || relative[0] == L'/'))
                relative.erase(relative.begin());

            return &mapping;
        }

        return nullptr;
    };

    if (const FastDLPath* mapping = match(localPath))
        return mapping;

    std::wstring full;

    if (!platform::GetFullPath(localPath, full) || EqualsNoCase(full, localPath))
        return nullptr;

    return match(full);
}

// Finds the first FastDLPath whose localPrefix matches the beginning of localPath
// (case-insensitive) and returns the full HTTP URL, or an empty string if no
// mapping applies.
static std::wstring BuildFastDLUrl(const std::wstring& localPath)
{
    const std::wstring baseUrl = GetEffectiveBaseUrl();
    if (baseUrl.empty())
        return {};

    std::wstring relative;
    const FastDLPath* mapping = MatchFastDLPath(localPath, relative);

    if (!mapping)
        return {};

    // Convert all backslashes to forward slashes
    for (auto& character : relative)
        if (character == L'\\') character = L'/';

    // Compose: baseUrl + remoteSubPath + "/" + relative
    std::wstring url = baseUrl;

    if (!mapping->remoteSubPath.empty())
        url += mapping->remoteSubPath + L"/";

    url += relative;

    return url;
}

// Computes the overlay (download cache) path for localPath.
// Returns empty string if: overlay is disabled, no FastDLPath prefix matches,
// or the computed path escapes the download base (path traversal attempt).
static std::wstring ComputeOverlayPath(const std::wstring& localPath)
{
    if (!g_fastdlUseDownloadDir || g_fastdlDownloadBase.empty())
        return {};

    std::wstring relative;
    const FastDLPath* mapping = MatchFastDLPath(localPath, relative);

    if (!mapping)
        return {};

    const std::wstring& remoteSubPath = mapping->remoteSubPath;

    // Build: <downloadBase>/<remoteSubPath>/<relative>
    std::wstring overlay = g_fastdlDownloadBase;

    if (!remoteSubPath.empty())
        overlay += remoteSubPath + platform::kPathSeparator;

    overlay += relative;

    // Normalize every separator to the platform's
    for (auto& c : overlay)
        if (c == L'/' || c == L'\\')
            c = platform::kPathSeparator;

    // Path traversal check: resolved path must stay within the download base
    std::wstring resolved;

    if (!platform::GetFullPath(overlay, resolved))
        return {};

    std::wstring resolvedLower = ToLowerFastDL(resolved);
    std::wstring baseLower     = ToLowerFastDL(g_fastdlDownloadBase);

    if (!resolvedLower.starts_with(baseLower))
        return {}; // path traversal attempt — reject

    return resolved;
}

// Returns true if downloadTarget is on the blocked-paths list.
static bool IsPathBlocked(const std::wstring& downloadTarget)
{
    if (!g_fastdlBlockSensitiveFiles || g_blockedPaths.empty())
        return false;

    std::wstring lower = ToLowerFastDL(downloadTarget);

    for (const auto& blocked : g_blockedPaths)
        if (lower == blocked) return true;

    return false;
}

// Downloads `url` to localPath via a GET request. Streams through a temp file,
// then renames it into place. Returns true on success.
static bool DoGetDownload(const std::wstring& url, const std::wstring& localPath)
{
    // Stream body to a temp file in the same directory
    const std::wstring tempPath = localPath + L".fastdl_tmp";

    if (!platform::http::GetToFile(url, tempPath))
        return false;

    // Rename to the final path
    if (!platform::ReplaceFile(tempPath, localPath))
    {
        platform::RemoveFile(tempPath);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InitFastDL()
{
    if (!g_fastdlEnabled)
        return;

    // A URL override from the launcher (Local\InterposerFastDL_<pid> on
    // Windows, INTERPOSER_FAST_DL elsewhere).
    if (std::string url = platform::ReadLauncherValue(L"InterposerFastDL"); !url.empty())
        g_fastdlBaseUrl = platform::Utf8ToWide(url);

    platform::http::Initialize();

    // The DLL's own path and directory (needed below)
    const std::wstring dllPath = platform::ModulePath();
    const std::wstring dllDir  = platform::ModuleDirectory();

    // Where .interposer\ lives: the DLL's directory unless the launcher named
    // another. The injector, by contrast, is always beside the DLL.
    const std::wstring& baseDir = GetInterposerBaseDirectory();
    const std::wstring  dataDir = GetInterposerDataDirectory();

    // Resolve overlay download directory
    if (g_fastdlUseDownloadDir)
    {
        std::wstring candidate;

        if (!g_fastdlDownloadDir.empty())
        {
            // Treat as absolute if it begins with a drive letter or UNC prefix,
            // or with the root on a platform whose separator is '/'
            const std::wstring& dir = g_fastdlDownloadDir;
            bool isAbsolute =
                (dir.size() >= 3 && dir[1] == L':') ||
                (dir.size() >= 2 && dir[0] == L'\\' && dir[1] == L'\\') ||
                (platform::kPathSeparator == L'/' && !dir.empty() && dir[0] == L'/');

            candidate = isAbsolute ? dir : (baseDir + dir);
        }
        else
        {
            candidate = dataDir + L"Downloads";
        }

        std::wstring resolved;
        g_fastdlDownloadBase = platform::GetFullPath(candidate, resolved) ? resolved : candidate;

        // Ensure a trailing separator
        if (!g_fastdlDownloadBase.empty() && g_fastdlDownloadBase.back() != platform::kPathSeparator)
            g_fastdlDownloadBase += platform::kPathSeparator;
    }

    // Build list of paths that FastDL must never overwrite
    if (g_fastdlBlockSensitiveFiles)
    {
        auto addBlocked = [](const std::wstring& path)
        {
            std::wstring resolved;
            if (!path.empty() && platform::GetFullPath(path, resolved))
                g_blockedPaths.push_back(ToLowerFastDL(resolved));
        };

        addBlocked(dllPath);                                    // interposer DLL itself
        addBlocked(dataDir + L"Config.yml");                    // config file

        // The virtual registry store: the default location, plus wherever
        // Registry.Files moved the overlay to.
        addBlocked(dataDir + L"Registry.reg");

        for (const std::wstring& registryFile : g_registryFiles)
            addBlocked(registryFile);

        addBlocked(dllDir + L"LANCommander.Interposer.Injector.exe");
        addBlocked(dllDir + L"Injector.exe");

        // The game executable
        addBlocked(platform::ExecutablePath());
    }
}

void ShutdownFastDL()
{
    platform::http::Shutdown();
}

// Shared implementation for TryFastDLDownload and FastDLFileExists.
// headOnly=true  → only do the HEAD check; returns true if server returns 200.
// headOnly=false → full download logic.
static bool FastDLImpl(const std::wstring& localPath, bool headOnly)
{
    if (!g_fastdlEnabled || !platform::http::Ready() || g_fastdlPaths.empty())
        return false;

    if (!IsExtensionAllowed(localPath))
        return false;

    // If no base URL is configured, probe any server addresses collected by the
    // network hooks that haven't been probed yet. This runs synchronously so that
    // a discovered FastDL endpoint is available immediately for this download.
    if (g_fastdlBaseUrl.empty())
        ProbeAllDiscoveredAddresses();

    std::wstring url = BuildFastDLUrl(localPath);

    if (url.empty())
        return false;

    // HEAD request
    const platform::http::HeadResult head = platform::http::Head(url, L"", L"X-Checksum-CRC32", 0);

    if (head.status != 200)
        return false;

    if (headOnly)
        return true;

    // Compute effective download target (overlay path or localPath if overlay disabled)
    std::wstring downloadPath;
    {
        std::wstring overlay = ComputeOverlayPath(localPath);
        downloadPath = overlay.empty() ? localPath : overlay;
    }

    // Refuse to overwrite sensitive files
    if (IsPathBlocked(downloadPath))
        return false;

    // Determine whether we need to download
    // CRC comparison is performed against the existing download (overlay file),
    // not the original game file, so the overlay stays in sync with the server.
    bool needDownload = true;

    if (head.headerPresent && !head.headerValue.empty())
    {
        // Compare CRC against the current download (if it exists).
        // Only read files up to 512 MB for CRC comparison.
        std::vector<unsigned char> data;

        if (platform::ReadWholeFile(downloadPath, 512ull * 1024 * 1024, data))
        {
            uint32_t crc = ComputeCRC32(data.data(), data.size());
            wchar_t localCrcStr[16]{};
            swprintf(localCrcStr, std::size(localCrcStr), L"%08x", crc);
            needDownload = !EqualsNoCase(localCrcStr, head.headerValue);
        }
    }

    if (!needDownload)
        return true; // file is already current

    // GET download to the effective download target
    bool ok = DoGetDownload(url, downloadPath);

    if (ok)
        LogFastDLAccess(L"FASTDL", url.c_str(), localPath.c_str());

    return ok;
}

bool TryFastDLDownload(const std::wstring& localPath)
{
    return FastDLImpl(localPath, false);
}

void ProbeServerForFastDL(const std::wstring& host, int probePort, int gameServerPort)
{
    if (!platform::http::Ready())
        return;

    // Skip immediately if we already have a verified URL — nothing better exists.
    {
        std::shared_lock<std::shared_mutex> lk(g_probedUrlMutex);
        if (!g_probedBaseUrlVerified.empty())
            return;
    }

    std::wstring probePath = g_fastdlProbePath;
    if (probePath.empty())
        probePath = L"/";

    // Build the header block for this probe request.
    // User-Agent identifies the interposer version.
    // X-Server-Host / Port let the FastDL server route the response to the
    // correct game-server instance when one HTTP port serves multiple game
    // servers.
    std::wstring headers;
    headers += L"User-Agent: LANCommander.Interposer.FastDL/";
    headers += platform::ModuleVersion();
    headers += L"\r\n";
    headers += L"X-Server-Host: ";
    headers += host;
    headers += L"\r\n";
    if (gameServerPort > 0)
    {
        headers += L"X-Server-Port: ";
        headers += std::to_wstring(gameServerPort);
        headers += L"\r\n";
    }

    // An IPv6 literal needs brackets in a URL.
    const std::wstring authority = host.find(L':') != std::wstring::npos ? L"[" + host + L"]" : host;
    const std::wstring probeUrl  = L"http://" + authority + L":" + std::to_wstring(probePort) + probePath;

    const ProbeResult result = DoProbeRequest(probeUrl, headers);

    if (result == ProbeResult::NotFound)
        return;

    // Build the discovered base URL
    std::wstring discovered = L"http://" + authority;
    if (probePort != 80)
        discovered += L":" + std::to_wstring(probePort);
    discovered += probePath;
    if (!discovered.empty() && discovered.back() != L'/')
        discovered += L'/';

    {
        std::unique_lock<std::shared_mutex> lk(g_probedUrlMutex);

        if (result == ProbeResult::Verified)
        {
            // Verified always wins — overwrite any previous unverified result.
            if (g_probedBaseUrlVerified.empty())
                g_probedBaseUrlVerified = discovered;
        }
        else // Unverified
        {
            // Only store if we have nothing yet (verified or unverified).
            if (g_probedBaseUrlVerified.empty() && g_probedBaseUrl.empty())
                g_probedBaseUrl = discovered;
        }
    }

    const wchar_t* verb = (result == ProbeResult::Verified)
        ? L"FASTDL PROBE OK" : L"FASTDL PROBE";
    LogFastDLAccess(verb, discovered.c_str(), host.c_str());
}

std::wstring GetExistingOverlayPath(const std::wstring& localPath)
{
    if (!g_fastdlEnabled || !g_fastdlUseDownloadDir)
        return {};

    std::wstring overlay = ComputeOverlayPath(localPath);

    if (overlay.empty())
        return {};

    // Check whether the file actually exists in the overlay directory
    if (!platform::PathExists(overlay))
        return {};

    return overlay;
}

bool FastDLFileExists(const std::wstring& localPath)
{
    return FastDLImpl(localPath, true);
}
