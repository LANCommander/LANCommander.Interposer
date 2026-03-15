#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <cstdint>
#include <string>
#include <vector>

#include "fastdl.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
static HINTERNET g_session = nullptr;

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

// Finds the first FastDLPath whose localPrefix matches the beginning of localPath
// (case-insensitive) and returns the full HTTP URL, or an empty string if no
// mapping applies.
static std::wstring BuildFastDLUrl(const std::wstring& localPath)
{
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
        std::wstring url = g_fastdlBaseUrl;
        
        if (!mapping.remoteSubPath.empty())
            url += mapping.remoteSubPath + L"/";
        
        url += relative;

        return url;
    }

    return {};
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

    // Determine whether we need to download
    bool needDownload = true;
    bool hasCRC = (crcBuffer[0] != L'\0');

    if (hasCRC)
    {
        // Compare CRC against local file (if it exists)
        HANDLE fileHandler = CreateFileW(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
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

    // GET download
    bool ok = DoGetDownload(connectHandler, urlPath, flags, localPath);
    WinHttpCloseHandle(connectHandler);

    if (ok)
        LogFastDLAccess(L"[FASTDL]       ", url.c_str(), localPath.c_str());

    return ok;
}

bool TryFastDLDownload(const std::wstring& localPath)
{
    return FastDLImpl(localPath, false);
}

bool FastDLFileExists(const std::wstring& localPath)
{
    return FastDLImpl(localPath, true);
}
