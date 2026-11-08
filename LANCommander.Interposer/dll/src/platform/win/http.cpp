#include "http.h"
#include "platform.h"

#include <winhttp.h>
#include <mutex>
#include <vector>

namespace platform::http
{

namespace
{
    HINTERNET  g_session = nullptr;
    int        g_sessionUsers = 0;    // Initialize calls not yet matched by Shutdown
    std::mutex g_sessionMutex;

    // One request's connection: the URL cracked, the server connected.
    struct Connection
    {
        HINTERNET connect = nullptr;
        wchar_t   path[2048]{};
        DWORD     flags = 0;

        explicit Connection(const std::wstring& url)
        {
            URL_COMPONENTS components{};
            components.dwStructSize = sizeof(components);

            wchar_t host[256]{};
            components.lpszHostName = host; components.dwHostNameLength = 256;
            components.lpszUrlPath  = path; components.dwUrlPathLength  = 2048;

            if (!g_session || !WinHttpCrackUrl(url.c_str(), 0, 0, &components))
                return;

            flags   = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
            connect = WinHttpConnect(g_session, host, components.nPort, 0);
        }

        ~Connection()
        {
            if (connect)
                WinHttpCloseHandle(connect);
        }

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        // Send `verb` (with `body`, if any) and wait for the response. Null on failure.
        HINTERNET Send(const wchar_t* verb, const std::wstring& headers, int timeoutMs,
                       const void* body = nullptr, size_t bodyBytes = 0)
        {
            if (!connect)
                return nullptr;

            HINTERNET request = WinHttpOpenRequest(connect, verb, path, nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

            if (!request)
                return nullptr;

            if (timeoutMs > 0)
                WinHttpSetTimeouts(request, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

            if (!headers.empty())
                WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1),
                    WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

            const DWORD length = static_cast<DWORD>(bodyBytes);

            if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    bodyBytes ? const_cast<void*>(body) : WINHTTP_NO_REQUEST_DATA, length, length, 0) ||
                !WinHttpReceiveResponse(request, nullptr))
            {
                WinHttpCloseHandle(request);
                return nullptr;
            }

            return request;
        }
    };

    int StatusOf(HINTERNET request)
    {
        DWORD status = 0;
        DWORD size   = sizeof(status);

        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            nullptr, &status, &size, nullptr);

        return static_cast<int>(status);
    }

    // Creates all parent directories needed for filePath to exist.
    void EnsureParentDirectoriesExist(const std::wstring& filePath)
    {
        size_t slash = filePath.find_last_of(L"\\/");

        if (slash == std::wstring::npos)
            return;

        std::wstring dir = filePath.substr(0, slash);

        // Walk each prefix at each separator and call CreateDirectoryW.
        // Failures are ignored (e.g. already exists, root drive).
        for (size_t i = 0; i <= dir.size(); ++i)
        {
            if ((i == dir.size() || dir[i] == L'\\' || dir[i] == L'/') && i > 0)
                CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
        }
    }
}

bool Initialize()
{
    std::lock_guard<std::mutex> lock(g_sessionMutex);

    if (!g_session)
    {
        g_session = WinHttpOpen(
            L"LANCommander.Interposer/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
    }

    if (!g_session)
        return false;

    ++g_sessionUsers;
    return true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_sessionMutex);

    if (g_sessionUsers > 0 && --g_sessionUsers > 0)
        return;

    if (g_session)
    {
        WinHttpCloseHandle(g_session);
        g_session = nullptr;
    }
}

bool Ready()
{
    return g_session != nullptr;
}

HeadResult Head(const std::wstring& url, const std::wstring& requestHeaders,
                const wchar_t* responseHeader, int timeoutMs)
{
    HeadResult result;
    Connection connection(url);

    HINTERNET request = connection.Send(L"HEAD", requestHeaders, timeoutMs);

    if (!request)
        return result;

    result.status = StatusOf(request);

    if (responseHeader)
    {
        wchar_t value[64]{};
        DWORD   size = sizeof(value);

        result.headerPresent = WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM,
            responseHeader, value, &size, nullptr) == TRUE;

        if (result.headerPresent)
            result.headerValue = value;
    }

    WinHttpCloseHandle(request);
    return result;
}

Response Send(const wchar_t* method, const std::wstring& url, const std::wstring& requestHeaders,
              const void* body, size_t bodyBytes, int timeoutMs, size_t maxResponseBytes)
{
    Response   result;
    Connection connection(url);

    HINTERNET request = connection.Send(method, requestHeaders, timeoutMs, body, bodyBytes);

    if (!request)
        return result;

    result.status = StatusOf(request);

    // Retry-After is either seconds or an HTTP date; only the seconds form is
    // used by the servers this talks to.
    {
        wchar_t value[64]{};
        DWORD   size = sizeof(value);

        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX,
                value, &size, WINHTTP_NO_HEADER_INDEX) && value[0] >= L'0' && value[0] <= L'9')
            result.retryAfterSeconds = _wtoi(value);
    }

    DWORD available = 0;

    while (WinHttpQueryDataAvailable(request, &available) && available > 0)
    {
        std::vector<char> buf(available);
        DWORD read = 0;

        if (!WinHttpReadData(request, buf.data(), available, &read) || read == 0)
            break;

        if (result.body.size() + read > maxResponseBytes)
        {
            result.body.append(buf.data(), maxResponseBytes - result.body.size());
            break;
        }

        result.body.append(buf.data(), read);
    }

    WinHttpCloseHandle(request);
    return result;
}

bool GetToFile(const std::wstring& url, const std::wstring& path)
{
    Connection connection(url);

    HINTERNET request = connection.Send(L"GET", L"", 0);

    if (!request)
        return false;

    if (StatusOf(request) != 200)
    {
        WinHttpCloseHandle(request);
        return false;
    }

    EnsureParentDirectoriesExist(path);

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE)
    {
        WinHttpCloseHandle(request);
        return false;
    }

    bool  writeOk        = true;
    DWORD bytesAvailable = 0;

    while (WinHttpQueryDataAvailable(request, &bytesAvailable) && bytesAvailable > 0)
    {
        std::vector<BYTE> buf(bytesAvailable);
        DWORD bytesRead = 0;

        if (!WinHttpReadData(request, buf.data(), bytesAvailable, &bytesRead))
        {
            writeOk = false;
            break;
        }

        if (bytesRead > 0)
        {
            DWORD written = 0;
            if (!WriteFile(file, buf.data(), bytesRead, &written, nullptr) || written != bytesRead)
            {
                writeOk = false;
                break;
            }
        }
    }

    CloseHandle(file);
    WinHttpCloseHandle(request);

    if (!writeOk)
        DeleteFileW(path.c_str());

    return writeOk;
}

} // namespace platform::http
