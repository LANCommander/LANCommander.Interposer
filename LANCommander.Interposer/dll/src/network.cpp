#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <MinHook.h>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "network.h"
#include "config.h"
#include "fastdl.h"

// ---------------------------------------------------------------------------
// Trampoline pointers
// ---------------------------------------------------------------------------
using FnGetAddrInfo  = int(WSAAPI*)(PCSTR,  PCSTR,  const ADDRINFOA*,  PADDRINFOA*);
using FnGetAddrInfoW = int(WSAAPI*)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
using FnConnect      = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using FnWSAConnect   = int(WSAAPI*)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);

static FnGetAddrInfo  g_origGetAddrInfo  = nullptr;
static FnGetAddrInfoW g_origGetAddrInfoW = nullptr;
static FnConnect      g_origConnect      = nullptr;
static FnWSAConnect   g_origWSAConnect   = nullptr;

// ---------------------------------------------------------------------------
// Deduplication — each unique host is only logged/probed once per session
// ---------------------------------------------------------------------------
static std::set<std::wstring> g_seenHosts;
static std::mutex             g_seenHostsMutex;

static bool TryMarkSeen(const std::wstring& host)
{
    std::lock_guard<std::mutex> lk(g_seenHostsMutex);
    return g_seenHosts.insert(host).second;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring AnsiToWide(const char* s)
{
    if (!s || s[0] == '\0') return {};
    int len = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), len);
    return out;
}

// Format a sockaddr as a host string (IPv4 dotted-decimal or compact IPv6 hex).
static std::wstring SockAddrToHost(const sockaddr* sa)
{
    if (!sa) return {};

    wchar_t buf[64]{};

    if (sa->sa_family == AF_INET)
    {
        const auto* a = reinterpret_cast<const sockaddr_in*>(sa);
        const BYTE* b = reinterpret_cast<const BYTE*>(&a->sin_addr);
        wsprintfW(buf, L"%d.%d.%d.%d",
            static_cast<int>(b[0]), static_cast<int>(b[1]),
            static_cast<int>(b[2]), static_cast<int>(b[3]));
    }
    else if (sa->sa_family == AF_INET6)
    {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(sa);
        const BYTE* b  = a6->sin6_addr.u.Byte;
        wsprintfW(buf,
            L"%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
            L"%02x%02x:%02x%02x:%02x%02x:%02x%02x",
            b[0],  b[1],  b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
            b[8],  b[9],  b[10], b[11], b[12], b[13], b[14], b[15]);
    }

    return std::wstring(buf);
}

// Extract the destination port from a sockaddr (host byte order), or -1 if unknown.
// sin_port / sin6_port are in network byte order (big-endian); swap manually.
static int SockAddrToPort(const sockaddr* sa)
{
    if (!sa) return -1;

    if (sa->sa_family == AF_INET)
    {
        const WORD netPort = reinterpret_cast<const sockaddr_in*>(sa)->sin_port;
        return static_cast<int>((netPort >> 8) | ((netPort & 0xFF) << 8));
    }

    if (sa->sa_family == AF_INET6)
    {
        const WORD netPort = reinterpret_cast<const sockaddr_in6*>(sa)->sin6_port;
        return static_cast<int>((netPort >> 8) | ((netPort & 0xFF) << 8));
    }

    return -1;
}

// Skip addresses that are uninteresting for FastDL probing.
static bool IsSkippableHost(const std::wstring& host)
{
    if (host.empty())         return true;
    if (host == L"0.0.0.0")  return true;
    if (host == L"::")        return true;

    // Loopback (127.x.x.x)
    if (host.size() >= 4 && host.substr(0, 4) == L"127.") return true;

    return false;
}

// ---------------------------------------------------------------------------
// OnHostDiscovered — called once per unique host/address
//
// gameServerPort: the port the game connected to (host byte order), or -1 if
//                 unknown (e.g. from a DNS lookup with no associated connect).
// ---------------------------------------------------------------------------
static void OnHostDiscovered(const std::wstring& host, int gameServerPort = -1)
{
    if (IsSkippableHost(host))
        return;

    if (!TryMarkSeen(host))
        return; // already handled this session

    if (gameServerPort > 0)
    {
        wchar_t portBuf[16]{};
        wsprintfW(portBuf, L"%d", gameServerPort);
        LogNetworkAccess(L"CONNECT", host.c_str(), portBuf);
    }
    else
    {
        LogNetworkAccess(L"CONNECT", host.c_str(), nullptr);
    }

    if (g_fastdlProbeConnections && g_fastdlEnabled)
    {
        // Snapshot config values now — they won't change after LoadConfig,
        // but we make copies so the lambda doesn't hold references.
        const int          probePort = g_fastdlProbePort;
        const int          gamePort  = gameServerPort;
        const std::wstring h         = host;

        std::thread([h, probePort, gamePort]()
        {
            ProbeServerForFastDL(h, probePort, gamePort);
        }).detach();
    }
}

// ---------------------------------------------------------------------------
// Hook implementations
// ---------------------------------------------------------------------------
static int WSAAPI HookGetAddrInfo(
    PCSTR            nodename,
    PCSTR            servname,
    const ADDRINFOA* hints,
    PADDRINFOA*      result)
{
    const int ret = g_origGetAddrInfo(nodename, servname, hints, result);

    // Only capture on success and non-empty hostname
    if (ret == 0 && nodename && nodename[0] != '\0')
        OnHostDiscovered(AnsiToWide(nodename));

    return ret;
}

static int WSAAPI HookGetAddrInfoW(
    PCWSTR           nodename,
    PCWSTR           servname,
    const ADDRINFOW* hints,
    PADDRINFOW*      result)
{
    const int ret = g_origGetAddrInfoW(nodename, servname, hints, result);

    if (ret == 0 && nodename && nodename[0] != L'\0')
        OnHostDiscovered(std::wstring(nodename));

    return ret;
}

static int WSAAPI HookConnect(SOCKET s, const sockaddr* name, int namelen)
{
    // Capture address and port before the call (non-blocking connects return
    // SOCKET_ERROR immediately, so we can't rely on the return value).
    const std::wstring host = SockAddrToHost(name);
    const int          port = SockAddrToPort(name);

    const int ret = g_origConnect(s, name, namelen);

    if (!host.empty())
        OnHostDiscovered(host, port);

    return ret;
}

static int WSAAPI HookWSAConnect(
    SOCKET       s,
    const sockaddr* name, int namelen,
    LPWSABUF     lpCallerData,
    LPWSABUF     lpCalleeData,
    LPQOS        lpSQOS,
    LPQOS        lpGQOS)
{
    const std::wstring host = SockAddrToHost(name);
    const int          port = SockAddrToPort(name);

    const int ret = g_origWSAConnect(s, name, namelen,
        lpCallerData, lpCalleeData, lpSQOS, lpGQOS);

    if (!host.empty())
        OnHostDiscovered(host, port);

    return ret;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InstallNetworkHooks()
{
    if (!g_logNetwork && !g_fastdlProbeConnections)
        return;

    MH_CreateHookApi(L"ws2_32", "getaddrinfo",
        reinterpret_cast<LPVOID>(HookGetAddrInfo),
        reinterpret_cast<LPVOID*>(&g_origGetAddrInfo));

    MH_CreateHookApi(L"ws2_32", "GetAddrInfoW",
        reinterpret_cast<LPVOID>(HookGetAddrInfoW),
        reinterpret_cast<LPVOID*>(&g_origGetAddrInfoW));

    MH_CreateHookApi(L"ws2_32", "connect",
        reinterpret_cast<LPVOID>(HookConnect),
        reinterpret_cast<LPVOID*>(&g_origConnect));

    MH_CreateHookApi(L"ws2_32", "WSAConnect",
        reinterpret_cast<LPVOID>(HookWSAConnect),
        reinterpret_cast<LPVOID*>(&g_origWSAConnect));
}

void RemoveNetworkHooks()
{
    std::lock_guard<std::mutex> lk(g_seenHostsMutex);
    g_seenHosts.clear();
}
