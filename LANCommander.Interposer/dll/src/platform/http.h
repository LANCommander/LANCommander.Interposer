#pragma once

// The HTTP the Interposer needs, and no more: for FastDL a HEAD that reads back
// one response header and a GET streamed into a file; for match statistics a
// request with a body whose response body is read back. WinHTTP on Windows
// (http and https); a small HTTP/1.1 client on Linux (http only).

#include <string>

namespace platform::http
{
    // Open / close the client. Requests fail until Initialize succeeds. Counted:
    // every successful Initialize needs its own Shutdown, so FastDL and match
    // reporting each hold the client open independently.
    bool Initialize();
    void Shutdown();
    bool Ready();

    struct Response
    {
        int         status = 0;          // 0 = no response at all
        std::string body;                // the response body, as sent
        int         retryAfterSeconds = -1; // Retry-After in seconds, -1 when absent
    };

    // Send `method` to `url` with `body` (may be empty). `requestHeaders` is a
    // block of "Name: value\r\n" lines. Reads at most maxResponseBytes of the
    // response body. `timeoutMs` 0 uses the client's defaults.
    Response Send(const wchar_t* method, const std::wstring& url, const std::wstring& requestHeaders,
                  const void* body, size_t bodyBytes, int timeoutMs, size_t maxResponseBytes = 1 << 20);

    struct HeadResult
    {
        int          status = 0;      // 0 = no response at all
        bool         headerPresent = false;
        std::wstring headerValue;     // the requested response header, when present
    };

    // HEAD `url`. `requestHeaders` is a block of "Name: value\r\n" lines (may be
    // empty). `responseHeader` names the header to read back (may be null).
    // `timeoutMs` 0 uses the client's defaults.
    HeadResult Head(const std::wstring& url, const std::wstring& requestHeaders,
                    const wchar_t* responseHeader, int timeoutMs);

    // GET `url` into `path`. Only once the server answers 200 are the parent
    // directories and the file created; a failed transfer deletes what it wrote.
    // True when the whole body was written.
    bool GetToFile(const std::wstring& url, const std::wstring& path);
}
