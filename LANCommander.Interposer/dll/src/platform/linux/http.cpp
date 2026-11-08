#include "http.h"
#include "platform.h"
#include "config.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <map>
#include <string>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

// A deliberately small HTTP/1.1 client: one request per connection
// ("Connection: close"), request bodies sized by Content-Length, response
// bodies sized by Content-Length, chunked, or the end of the connection.
// http:// only -- a TLS stack is not worth carrying into every game process for
// FastDL, whose servers are plain HTTP in practice, and LAN LANCommander
// servers usually are too. An https:// URL fails, and says so once in the log.

namespace platform::http
{

namespace
{
    std::atomic<int>  g_users{ 0 };   // Initialize calls not yet matched by Shutdown
    std::atomic<bool> g_warnedHttps{ false };

    // WinHTTP's default send/receive timeout, for requests that name none.
    constexpr int kDefaultTimeoutMs = 30000;

    struct Url
    {
        bool        valid = false;
        std::string host;
        std::string port = "80";
        std::string path = "/";
        std::string hostHeader;
    };

    Url Parse(const std::wstring& wideUrl)
    {
        Url url;
        const std::string text = WideToUtf8(wideUrl);

        std::string lower = text.substr(0, 8);
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lower.rfind("https://", 0) == 0)
        {
            if (!g_warnedHttps.exchange(true))
                InterposerLog(L"HTTP", (L"https is not supported on this platform: " + wideUrl).c_str());

            return url;
        }

        if (lower.rfind("http://", 0) != 0)
            return url;

        const std::string rest      = text.substr(7);
        const size_t      pathStart = rest.find('/');
        const std::string authority = rest.substr(0, pathStart);

        if (pathStart != std::string::npos)
            url.path = rest.substr(pathStart);

        // [v6::address]:port, or host:port
        if (!authority.empty() && authority[0] == '[')
        {
            const size_t close = authority.find(']');
            if (close == std::string::npos)
                return url;

            url.host = authority.substr(1, close - 1);

            if (close + 1 < authority.size() && authority[close + 1] == ':')
                url.port = authority.substr(close + 2);
        }
        else
        {
            const size_t colon = authority.rfind(':');
            url.host = authority.substr(0, colon);

            if (colon != std::string::npos)
                url.port = authority.substr(colon + 1);
        }

        if (url.host.empty() || url.port.empty())
            return url;

        url.hostHeader = authority;
        url.valid      = true;
        return url;
    }

    // Connect with a timeout: non-blocking connect, wait for it, then back to
    // blocking with send/receive timeouts. -1 on failure.
    int Connect(const Url& url, int timeoutMs)
    {
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* results = nullptr;

        if (getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &results) != 0)
            return -1;

        int fd = -1;

        for (addrinfo* ai = results; ai && fd < 0; ai = ai->ai_next)
        {
            fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
            if (fd < 0)
                continue;

            const int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            bool connected = connect(fd, ai->ai_addr, ai->ai_addrlen) == 0;

            if (!connected && errno == EINPROGRESS)
            {
                pollfd p{ fd, POLLOUT, 0 };
                int error = 0;
                socklen_t length = sizeof(error);

                connected = poll(&p, 1, timeoutMs) == 1 &&
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
            }

            if (!connected)
            {
                close(fd);
                fd = -1;
                continue;
            }

            fcntl(fd, F_SETFL, flags);

            timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }

        freeaddrinfo(results);
        return fd;
    }

    bool SendAll(int fd, const std::string& data)
    {
        size_t sent = 0;

        while (sent < data.size())
        {
            const ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);

            if (n < 0 && errno == EINTR)
                continue;

            if (n <= 0)
                return false;

            sent += static_cast<size_t>(n);
        }

        return true;
    }

    // Reads from a socket through a buffer, so the headers and the start of
    // the body can arrive in one recv.
    struct Reader
    {
        int         fd;
        std::string buffer;
        bool        eof = false;

        bool Fill()
        {
            char chunk[16384];

            for (;;)
            {
                const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);

                if (n < 0 && errno == EINTR)
                    continue;

                if (n <= 0)
                {
                    eof = true;
                    return false;
                }

                buffer.append(chunk, static_cast<size_t>(n));
                return true;
            }
        }

        // One line, without its CRLF.
        bool Line(std::string& line)
        {
            size_t end;

            while ((end = buffer.find("\r\n")) == std::string::npos)
            {
                if (buffer.size() > 65536 || !Fill())
                    return false;
            }

            line = buffer.substr(0, end);
            buffer.erase(0, end + 2);
            return true;
        }
    };

    struct ResponseHead
    {
        int                                status = 0;
        std::map<std::string, std::string> headers;   // names lower case
    };

    bool ReadHead(Reader& reader, ResponseHead& response)
    {
        std::string line;

        // "HTTP/1.1 200 OK"
        if (!reader.Line(line) || line.rfind("HTTP/", 0) != 0)
            return false;

        const size_t space = line.find(' ');
        if (space == std::string::npos)
            return false;

        response.status = atoi(line.c_str() + space + 1);

        while (reader.Line(line))
        {
            if (line.empty())
                return true;

            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;

            std::string name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            size_t start = colon + 1;
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
                ++start;

            response.headers[name] = line.substr(start);
        }

        return false;
    }

    bool HasHeader(const std::wstring& block, const char* name)
    {
        std::string lower = WideToUtf8(block);
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        return lower.rfind(std::string(name) + ":", 0) == 0 ||
               lower.find(std::string("\r\n") + name + ":") != std::string::npos;
    }

    // Connect, send the request, read the status line and headers. -1 on failure.
    int Request(const Url& url, const char* method, const std::wstring& headers,
                int timeoutMs, Reader*& reader, ResponseHead& response,
                const void* body = nullptr, size_t bodyBytes = 0)
    {
        const int fd = Connect(url, timeoutMs);

        if (fd < 0)
            return -1;

        std::string request = std::string(method) + " " + url.path + " HTTP/1.1\r\n";
        request += "Host: " + url.hostHeader + "\r\n";

        // A User-Agent in the caller's headers replaces the default, as
        // WINHTTP_ADDREQ_FLAG_REPLACE does on Windows.
        if (!HasHeader(headers, "user-agent"))
            request += "User-Agent: LANCommander.Interposer/1.0\r\n";

        request += "Connection: close\r\n";

        if (bodyBytes || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0)
            request += "Content-Length: " + std::to_string(bodyBytes) + "\r\n";

        request += WideToUtf8(headers);
        request += "\r\n";

        if (bodyBytes)
            request.append(static_cast<const char*>(body), bodyBytes);

        reader = new Reader{ fd, std::string(), false };

        if (!SendAll(fd, request) || !ReadHead(*reader, response))
        {
            close(fd);
            delete reader;
            reader = nullptr;
            return -1;
        }

        return fd;
    }

    // Hand the body to `write`, by Content-Length, chunks, or to end of stream.
    template <typename Write>
    bool CopyBody(Reader& reader, const ResponseHead& response, Write write)
    {
        auto te = response.headers.find("transfer-encoding");
        if (te != response.headers.end() && te->second.find("chunked") != std::string::npos)
        {
            for (;;)
            {
                std::string sizeLine;
                if (!reader.Line(sizeLine))
                    return false;

                const size_t size = strtoul(sizeLine.c_str(), nullptr, 16);

                if (size == 0)
                    return true; // trailers, if any, are not needed

                while (reader.buffer.size() < size + 2)
                {
                    if (!reader.Fill())
                        return false;
                }

                if (!write(reader.buffer.data(), size))
                    return false;

                reader.buffer.erase(0, size + 2);
            }
        }

        auto cl = response.headers.find("content-length");
        const bool sized = cl != response.headers.end();
        unsigned long long remaining = sized ? strtoull(cl->second.c_str(), nullptr, 10) : 0;

        for (;;)
        {
            if (!reader.buffer.empty())
            {
                const size_t take = sized
                    ? static_cast<size_t>(std::min<unsigned long long>(remaining, reader.buffer.size()))
                    : reader.buffer.size();

                if (!write(reader.buffer.data(), take))
                    return false;

                reader.buffer.erase(0, take);
                remaining -= sized ? take : 0;
            }

            if (sized && remaining == 0)
                return true;

            if (!reader.Fill())
                return !sized; // end of stream is the end of an unsized body
        }
    }
}

bool Initialize()
{
    ++g_users;
    return true;
}

void Shutdown()
{
    int users = g_users.load();

    while (users > 0 && !g_users.compare_exchange_weak(users, users - 1))
        ;
}

bool Ready()
{
    return g_users.load() > 0;
}

HeadResult Head(const std::wstring& url, const std::wstring& requestHeaders,
                const wchar_t* responseHeader, int timeoutMs)
{
    HeadResult result;
    const Url parsed = Parse(url);

    if (!Ready() || !parsed.valid)
        return result;

    Reader*  reader = nullptr;
    ResponseHead response;

    const int fd = Request(parsed, "HEAD", requestHeaders,
        timeoutMs > 0 ? timeoutMs : kDefaultTimeoutMs, reader, response);

    if (fd < 0)
        return result;

    result.status = response.status;

    if (responseHeader)
    {
        std::string name = WideToUtf8(responseHeader);
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (auto it = response.headers.find(name); it != response.headers.end())
        {
            result.headerPresent = true;
            result.headerValue   = Utf8ToWide(it->second);
        }
    }

    close(fd);
    delete reader;
    return result;
}

bool GetToFile(const std::wstring& url, const std::wstring& path)
{
    const Url parsed = Parse(url);

    if (!Ready() || !parsed.valid)
        return false;

    Reader*  reader = nullptr;
    ResponseHead response;

    const int fd = Request(parsed, "GET", L"", kDefaultTimeoutMs, reader, response);

    if (fd < 0)
        return false;

    bool ok = false;

    if (response.status == 200)
    {
        const size_t slash = path.find_last_of(L'/');
        if (slash != std::wstring::npos && slash > 0)
            CreateDirectoryTree(path.substr(0, slash));

        const int out = open(WideToUtf8(path).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

        if (out >= 0)
        {
            ok = CopyBody(*reader, response,
                [&](const char* data, size_t bytes) { return WriteToFile(out, data, bytes); });
            close(out);

            if (!ok)
                RemoveFile(path);
        }
    }

    close(fd);
    delete reader;
    return ok;
}

Response Send(const wchar_t* method, const std::wstring& url, const std::wstring& requestHeaders,
              const void* body, size_t bodyBytes, int timeoutMs, size_t maxResponseBytes)
{
    Response result;
    const Url parsed = Parse(url);

    if (!Ready() || !parsed.valid)
        return result;

    Reader*           reader = nullptr;
    ResponseHead      head;
    const std::string verb = WideToUtf8(method);

    const int fd = Request(parsed, verb.c_str(), requestHeaders,
        timeoutMs > 0 ? timeoutMs : kDefaultTimeoutMs, reader, head, body, bodyBytes);

    if (fd < 0)
        return result;

    result.status = head.status;

    if (auto it = head.headers.find("retry-after"); it != head.headers.end() &&
        !it->second.empty() && std::isdigit(static_cast<unsigned char>(it->second[0])))
        result.retryAfterSeconds = atoi(it->second.c_str());

    CopyBody(*reader, head, [&](const char* data, size_t bytes)
    {
        const size_t room = maxResponseBytes - std::min(maxResponseBytes, result.body.size());
        result.body.append(data, std::min(room, bytes));
        return true;
    });

    close(fd);
    delete reader;
    return result;
}

} // namespace platform::http
