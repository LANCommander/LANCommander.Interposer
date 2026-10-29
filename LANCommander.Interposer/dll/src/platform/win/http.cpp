#include "http.h"
#include "platform.h"

#include <winhttp.h>
#include <vector>

namespace platform::http
{

namespace
{
    HINTERNET g_session = nullptr;

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

        // Send `verb` and wait for the response. Null on failure.
        HINTERNET Send(const wchar_t* verb, const std::wstring& headers, int timeoutMs)
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

            if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
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
    if (!g_session)
    {
        g_session = WinHttpOpen(
            L"LANCommander.Interposer/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
    }

    return g_session != nullptr;
}

void Shutdown()
{
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
