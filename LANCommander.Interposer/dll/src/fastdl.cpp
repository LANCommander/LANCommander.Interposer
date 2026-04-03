#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

#include "fastdl.h"
#include "config.h"
#include "network.h"

// Response header that a LANCommander FastDL endpoint must return.
// Any value is accepted — its mere presence confirms the endpoint is ours.
static constexpr LPCWSTR FASTDL_PROBE_HEADER = L"X-FastDL";

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
static HINTERNET              g_session           = nullptr;
static std::wstring           g_fastdlDownloadBase;           // resolved overlay directory (trailing \)
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
        character = towlower(character);
    
    return input;
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

// Returns the DLL's own file version as a string (e.g. "1.2.0.0").
// Falls back to "1.0" if no version resource is embedded.
// Result is cached after the first call.
static const wchar_t* GetDllVersion()
{
    static wchar_t s_version[32] = L"1.0";
    static bool    s_ready       = false;

    if (s_ready)
        return s_version;

    s_ready = true;

    HMODULE hSelf = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetDllVersion), &hSelf);

    wchar_t dllPath[MAX_PATH]{};
    if (!GetModuleFileNameW(hSelf, dllPath, MAX_PATH))
        return s_version;

    DWORD dummy = 0;
    DWORD size  = GetFileVersionInfoSizeW(dllPath, &dummy);
    if (size == 0)
        return s_version;

    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(dllPath, 0, size, buf.data()))
        return s_version;

    VS_FIXEDFILEINFO* info    = nullptr;
    UINT              infoLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\",
            reinterpret_cast<LPVOID*>(&info), &infoLen) || !info)
        return s_version;

    if (info->dwFileVersionMS == 0 && info->dwFileVersionLS == 0)
        return s_version;

    wsprintfW(s_version, L"%d.%d.%d.%d",
        HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
        HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));

    return s_version;
}

// Result of a FastDL probe request.
enum class ProbeResult { NotFound, Unverified, Verified };

// Makes a HEAD request on hConnect at path with custom headers.
// Returns Verified if HTTP 200 and FASTDL_PROBE_HEADER is present,
// Unverified if HTTP 200 without the header, NotFound otherwise.
static ProbeResult DoProbeRequest(HINTERNET hConnect, LPCWSTR path, DWORD flags,
                                  LPCWSTR additionalHeaders)
{
    HINTERNET hReq = WinHttpOpenRequest(
        hConnect, L"HEAD", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!hReq)
        return ProbeResult::NotFound;

    // Use a short timeout so probing doesn't stall the file-load hook for long.
    const DWORD timeout = static_cast<DWORD>(g_fastdlProbeTimeout > 0 ? g_fastdlProbeTimeout : 2000);
    WinHttpSetTimeouts(hReq, timeout, timeout, timeout, timeout);

    if (additionalHeaders && additionalHeaders[0] != L'\0')
        WinHttpAddRequestHeaders(hReq, additionalHeaders, static_cast<DWORD>(-1),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr))
    {
        WinHttpCloseHandle(hReq);
        return ProbeResult::NotFound;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &statusCode, &statusSize, nullptr);

    if (statusCode != 200)
    {
        WinHttpCloseHandle(hReq);
        return ProbeResult::NotFound;
    }

    // Check for the LANCommander verification header (any value is accepted).
    wchar_t headerVal[32]{};
    DWORD   headerSize = sizeof(headerVal);
    bool    verified   = WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CUSTOM,
        FASTDL_PROBE_HEADER, headerVal, &headerSize, nullptr) == TRUE;

    WinHttpCloseHandle(hReq);
    return verified ? ProbeResult::Verified : ProbeResult::Unverified;
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

// Finds the first FastDLPath whose localPrefix matches the beginning of localPath
// (case-insensitive) and returns the full HTTP URL, or an empty string if no
// mapping applies.
static std::wstring BuildFastDLUrl(const std::wstring& localPath)
{
    const std::wstring baseUrl = GetEffectiveBaseUrl();
    if (baseUrl.empty())
        return {};

    std::wstring lowerLocalPath = ToLowerFastDL(localPath);

    for (const auto& mapping : g_fastdlPaths)
    {
        std::wstring prefix = ToLowerFastDL(mapping.localPrefix);

        if (lowerLocalPath.size() < prefix.size())
            continue;

        if (lowerLocalPath.substr(0, prefix.size()) != prefix)
            continue;

        // Extract the relative portion after the prefix
        std::wstring relative = localPath.substr(mapping.localPrefix.size());

        // Strip any leading separator
        while (!relative.empty() && (relative[0] == L'\\' || relative[0] == L'/'))
            relative.erase(relative.begin());

        // Convert all backslashes to forward slashes
        for (auto& character : relative)
            if (character == L'\\') character = L'/';

        // Compose: baseUrl + remoteSubPath + "/" + relative
        std::wstring url = baseUrl;

        if (!mapping.remoteSubPath.empty())
            url += mapping.remoteSubPath + L"/";

        url += relative;

        return url;
    }

    return {};
}

// Computes the overlay (download cache) path for localPath.
// Returns empty string if: overlay is disabled, no FastDLPath prefix matches,
// or the computed path escapes the download base (path traversal attempt).
static std::wstring ComputeOverlayPath(const std::wstring& localPath)
{
    if (!g_fastdlUseDownloadDir || g_fastdlDownloadBase.empty())
        return {};

    std::wstring lowerLocal = ToLowerFastDL(localPath);

    for (const auto& [localPrefix, remoteSubPath] : g_fastdlPaths)
    {
        std::wstring prefix = ToLowerFastDL(localPrefix);

        if (lowerLocal.size() < prefix.size())
            continue;

        if (!lowerLocal.starts_with(prefix))
            continue;

        // Relative portion after the prefix
        std::wstring relative = localPath.substr(localPrefix.size());

        // Strip any leading separator
        while (!relative.empty() && (relative[0] == L'\\' || relative[0] == L'/'))
            relative.erase(relative.begin());

        // Build: <downloadBase>\<remoteSubPath>\<relative>
        std::wstring overlay = g_fastdlDownloadBase;

        if (!remoteSubPath.empty())
            overlay += remoteSubPath + L"\\";

        overlay += relative;

        // Normalize all forward slashes to backslashes
        for (auto& c : overlay)
            if (c == L'/')
                c = L'\\';

        // Path traversal check: resolved path must stay within the download base
        wchar_t resolved[MAX_PATH]{};

        if (GetFullPathNameW(overlay.c_str(), MAX_PATH, resolved, nullptr) == 0)
            return {};

        std::wstring resolvedLower = ToLowerFastDL(resolved);
        std::wstring baseLower     = ToLowerFastDL(g_fastdlDownloadBase);

        if (!resolvedLower.starts_with(baseLower))
            return {}; // path traversal attempt — reject

        return std::wstring(resolved);
    }

    return {};
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

// Creates all parent directories needed for filePath to exist.
static void EnsureParentDirectoriesExist(const std::wstring& filePath)
{
    size_t slash = filePath.find_last_of(L"\\/");
    
    if (slash == std::wstring::npos)
        return;

    std::wstring dir = filePath.substr(0, slash);

    // Walk each prefix at each separator and call CreateDirectoryW.
    // Failures are ignored (e.g. already exists, root drive).
    for (size_t i = 0; i <= dir.size(); ++i)
    {
        if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/')
        {
            if (i > 0)
            {
                std::wstring partial = dir.substr(0, i);
                CreateDirectoryW(partial.c_str(), nullptr);
            }
        }
    }
}

// Performs a HEAD request on the given URL via hConnect.
// Returns the HTTP status code, or 0 on WinHTTP error.
// If outCRC is non-null and the server sent X-Checksum-CRC32, copies the value
// (up to 63 wchars).
static DWORD DoHeadRequest(HINTERNET hConnect, LPCWSTR path, DWORD flags,
                            wchar_t* outCRC, DWORD outCRCCharCount)
{
    HINTERNET hReq = WinHttpOpenRequest(
        hConnect, L"HEAD", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!hReq)
        return 0;

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr))
    {
        WinHttpCloseHandle(hReq);
        return 0;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &statusCode, &statusSize, nullptr);

    if (outCRC && outCRCCharCount > 0)
    {
        outCRC[0] = L'\0';
        
        DWORD crcSize = outCRCCharCount * sizeof(wchar_t);
        
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CUSTOM,
            L"X-Checksum-CRC32", outCRC, &crcSize, nullptr);
    }

    WinHttpCloseHandle(hReq);
    
    return statusCode;
}

// Downloads the file at path (on hConnect) to localPath via a GET request.
// Streams through a temp file, then renames atomically.
// Returns true on success.
static bool DoGetDownload(HINTERNET hConnect, LPCWSTR path, DWORD flags,
                          const std::wstring& localPath)
{
    HINTERNET requestHandler = WinHttpOpenRequest(
        hConnect, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!requestHandler)
        return false;

    if (!WinHttpSendRequest(requestHandler, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(requestHandler, nullptr))
    {
        WinHttpCloseHandle(requestHandler);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    
    WinHttpQueryHeaders(requestHandler,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &statusCode, &statusSize, nullptr);

    if (statusCode != 200)
    {
        WinHttpCloseHandle(requestHandler);
        return false;
    }

    // Ensure parent directories exist
    EnsureParentDirectoriesExist(localPath);

    // Stream body to a temp file in the same directory
    std::wstring tempPath = localPath + L".fastdl_tmp";

    HANDLE tempFileHandler = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (tempFileHandler == INVALID_HANDLE_VALUE)
    {
        WinHttpCloseHandle(requestHandler);
        return false;
    }

    bool writeOk = true;
    DWORD bytesAvailable = 0;

    while (WinHttpQueryDataAvailable(requestHandler, &bytesAvailable) && bytesAvailable > 0)
    {
        std::vector<BYTE> buf(bytesAvailable);
        DWORD bytesRead = 0;

        if (!WinHttpReadData(requestHandler, buf.data(), bytesAvailable, &bytesRead))
        {
            writeOk = false;
            break;
        }

        if (bytesRead > 0)
        {
            DWORD written = 0;
            if (!WriteFile(tempFileHandler, buf.data(), bytesRead, &written, nullptr) ||
                written != bytesRead)
            {
                writeOk = false;
                break;
            }
        }
    }

    CloseHandle(tempFileHandler);
    WinHttpCloseHandle(requestHandler);

    if (!writeOk)
    {
        DeleteFileW(tempPath.c_str());
        return false;
    }

    // Atomic rename to final path
    if (!MoveFileExW(tempPath.c_str(), localPath.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tempPath.c_str());
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

    // Check for a named MMF URL override written by the injector.
    // The MMF name is Local\InterposerFastDL_<pid>.
    DWORD pid = GetCurrentProcessId();
    
    wchar_t mmfName[64]{};
    
    wsprintfW(mmfName, L"Local\\InterposerFastDL_%lu", pid);

    HANDLE hMMF = OpenFileMappingW(FILE_MAP_READ, FALSE, mmfName);
    
    if (hMMF)
    {
        const char* view = static_cast<const char*>(
            MapViewOfFile(hMMF, FILE_MAP_READ, 0, 0, 2048));

        if (view)
        {
            int wideLength = MultiByteToWideChar(CP_UTF8, 0, view, -1, nullptr, 0);
            
            if (wideLength > 1)
            {
                std::wstring url(static_cast<size_t>(wideLength - 1), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, view, -1, url.data(), wideLength);
                g_fastdlBaseUrl = url;
            }
            
            UnmapViewOfFile(view);
        }

        CloseHandle(hMMF);
    }

    // Open a persistent WinHTTP session
    g_session = WinHttpOpen(
        L"LANCommander.Interposer/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    // Locate the DLL's own path and directory (needed below)
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&InitFastDL),
        &hSelf);

    wchar_t dllPath[MAX_PATH]{};
    GetModuleFileNameW(hSelf, dllPath, MAX_PATH);

    std::wstring dllDir(dllPath);
    {
        size_t slash = dllDir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            dllDir.resize(slash + 1);
    }

    // Resolve overlay download directory
    if (g_fastdlUseDownloadDir)
    {
        std::wstring candidate;

        if (!g_fastdlDownloadDir.empty())
        {
            // Treat as absolute if it begins with a drive letter or UNC prefix
            bool isAbsolute =
                (g_fastdlDownloadDir.size() >= 3 && g_fastdlDownloadDir[1] == L':') ||
                (g_fastdlDownloadDir.size() >= 2 &&
                 g_fastdlDownloadDir[0] == L'\\' && g_fastdlDownloadDir[1] == L'\\');

            candidate = isAbsolute ? g_fastdlDownloadDir : (dllDir + g_fastdlDownloadDir);
        }
        else
        {
            candidate = dllDir + L".interposer\\Downloads";
        }

        wchar_t resolved[MAX_PATH]{};
        if (GetFullPathNameW(candidate.c_str(), MAX_PATH, resolved, nullptr) > 0)
            g_fastdlDownloadBase = resolved;
        else
            g_fastdlDownloadBase = candidate;

        // Ensure trailing backslash
        if (!g_fastdlDownloadBase.empty() && g_fastdlDownloadBase.back() != L'\\')
            g_fastdlDownloadBase += L'\\';
    }

    // Build list of paths that FastDL must never overwrite
    if (g_fastdlBlockSensitiveFiles)
    {
        auto addBlocked = [](const std::wstring& path)
        {
            wchar_t resolved[MAX_PATH]{};
            if (GetFullPathNameW(path.c_str(), MAX_PATH, resolved, nullptr) > 0)
                g_blockedPaths.push_back(ToLowerFastDL(resolved));
        };

        addBlocked(dllPath);                                               // interposer DLL itself
        addBlocked(dllDir + L".interposer\\Config.yml");                   // config file
        addBlocked(dllDir + L".interposer\\Registry.reg");                 // virtual registry file
        addBlocked(dllDir + L"LANCommander.Interposer.Injector.exe");
        addBlocked(dllDir + L"Injector.exe");

        // The game executable
        wchar_t gameExe[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, gameExe, MAX_PATH) > 0)
            addBlocked(gameExe);
    }
}

void ShutdownFastDL()
{
    if (g_session)
    {
        WinHttpCloseHandle(g_session);
        g_session = nullptr;
    }
}

// Shared implementation for TryFastDLDownload and FastDLFileExists.
// headOnly=true  → only do the HEAD check; returns true if server returns 200.
// headOnly=false → full download logic.
static bool FastDLImpl(const std::wstring& localPath, bool headOnly)
{
    if (!g_fastdlEnabled || !g_session || g_fastdlPaths.empty())
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

    // Crack the URL into components
    URL_COMPONENTS urlComponents{};
    urlComponents.dwStructSize = sizeof(urlComponents);

    wchar_t host[256]{};
    wchar_t urlPath[2048]{};
    urlComponents.lpszHostName    = host;    urlComponents.dwHostNameLength    = 256;
    urlComponents.lpszUrlPath     = urlPath; urlComponents.dwUrlPathLength     = 2048;

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &urlComponents))
        return false;

    bool  https = (urlComponents.nScheme == INTERNET_SCHEME_HTTPS);
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET connectHandler = WinHttpConnect(g_session, host, urlComponents.nPort, 0);
    if (!connectHandler)
        return false;

    // HEAD request
    wchar_t crcBuffer[64]{};
    DWORD status = DoHeadRequest(connectHandler, urlPath, flags, crcBuffer, 64);

    if (status != 200)
    {
        WinHttpCloseHandle(connectHandler);
        return false;
    }

    if (headOnly)
    {
        WinHttpCloseHandle(connectHandler);
        return true;
    }

    // Compute effective download target (overlay path or localPath if overlay disabled)
    std::wstring downloadPath;
    {
        std::wstring overlay = ComputeOverlayPath(localPath);
        downloadPath = overlay.empty() ? localPath : overlay;
    }

    // Refuse to overwrite sensitive files
    if (IsPathBlocked(downloadPath))
    {
        WinHttpCloseHandle(connectHandler);
        return false;
    }

    // Determine whether we need to download
    // CRC comparison is performed against the existing download (overlay file),
    // not the original game file, so the overlay stays in sync with the server.
    bool needDownload = true;
    bool hasCRC = crcBuffer[0] != L'\0';

    if (hasCRC)
    {
        // Compare CRC against the current download (if it exists)
        HANDLE fileHandler = CreateFileW(downloadPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (fileHandler != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER fileSize{};
            GetFileSizeEx(fileHandler, &fileSize);

            // Only read files up to 512 MB for CRC comparison
            if (fileSize.QuadPart > 0 && fileSize.QuadPart <= 512LL * 1024 * 1024)
            {
                std::vector<uint8_t> data(static_cast<size_t>(fileSize.QuadPart));
                DWORD bytesRead = 0;

                if (ReadFile(fileHandler, data.data(), static_cast<DWORD>(data.size()),
                             &bytesRead, nullptr))
                {
                    uint32_t crc = ComputeCRC32(data.data(), bytesRead);
                    wchar_t localCrcStr[16]{};
                    wsprintfW(localCrcStr, L"%08x", crc);
                    needDownload = (_wcsicmp(localCrcStr, crcBuffer) != 0);
                }
            }

            CloseHandle(fileHandler);
        }
    }

    if (!needDownload)
    {
        WinHttpCloseHandle(connectHandler);
        return true; // file is already current
    }

    // GET download to the effective download target
    bool ok = DoGetDownload(connectHandler, urlPath, flags, downloadPath);
    WinHttpCloseHandle(connectHandler);

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
    if (!g_session)
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
    // X-LANCommander-GameServer-Host / Port let the FastDL server route the
    // response to the correct game-server instance when one HTTP port serves
    // multiple game servers.
    std::wstring headers;
    headers += L"User-Agent: LANCommander.Interposer.FastDL/";
    headers += GetDllVersion();
    headers += L"\r\n";
    headers += L"X-Server-Host: ";
    headers += host;
    headers += L"\r\n";
    if (gameServerPort > 0)
    {
        wchar_t portBuf[16]{};
        wsprintfW(portBuf, L"%d", gameServerPort);
        headers += L"X-Server-Port: ";
        headers += portBuf;
        headers += L"\r\n";
    }

    HINTERNET hConnect = WinHttpConnect(g_session, host.c_str(),
        static_cast<INTERNET_PORT>(probePort), 0);
    if (!hConnect)
        return;

    const ProbeResult result = DoProbeRequest(hConnect, probePath.c_str(), 0, headers.c_str());
    WinHttpCloseHandle(hConnect);

    if (result == ProbeResult::NotFound)
        return;

    // Build the discovered base URL
    std::wstring discovered = L"http://" + host;
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
    if (GetFileAttributesW(overlay.c_str()) == INVALID_FILE_ATTRIBUTES)
        return {};

    return overlay;
}

bool FastDLFileExists(const std::wstring& localPath)
{
    return FastDLImpl(localPath, true);
}
