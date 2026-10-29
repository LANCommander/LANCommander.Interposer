#pragma once

// The HTTP that FastDL needs, and no more: a HEAD that reads back one response
// header, and a GET streamed into a file. WinHTTP on Windows (http and https); a
// small HTTP/1.1 client on Linux (http only).

#include <string>

namespace platform::http
{
    // Open / close the client. Requests fail until Initialize succeeds.
    bool Initialize();
    void Shutdown();
    bool Ready();

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
