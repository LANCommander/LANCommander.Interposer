#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <MinHook.h>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "network.h"
#include "config.h"
#include "fastdl.h"

// ---------------------------------------------------------------------------
// Trampoline pointers
// ---------------------------------------------------------------------------
using FnGetAddrInfo    = int(WSAAPI*)(PCSTR,  PCSTR,  const ADDRINFOA*,  PADDRINFOA*);
using FnGetAddrInfoW   = int(WSAAPI*)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
using FnGetAddrInfoExW = int(WSAAPI*)(PCWSTR, PCWSTR, DWORD, LPGUID, const ADDRINFOEXW*, PADDRINFOEXW*, struct timeval*, LPOVERLAPPED, LPLOOKUPSERVICE_COMPLETION_ROUTINE, LPHANDLE);
using FnGetAddrInfoExA = int(WSAAPI*)(PCSTR,  PCSTR,  DWORD, LPGUID, const ADDRINFOEXA*, PADDRINFOEXA*, struct timeval*, LPOVERLAPPED, LPLOOKUPSERVICE_COMPLETION_ROUTINE, LPHANDLE);
using FnGetHostByName  = hostent*(WSAAPI*)(const char*);
using FnConnect        = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using FnWSAConnect     = int(WSAAPI*)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
using FnSendTo         = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using FnWSASendTo      = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using FnRecvFrom       = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using FnWSARecvFrom    = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using FnSend           = int(WSAAPI*)(SOCKET, const char*, int, int);
using FnWSASend        = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using FnRecv           = int(WSAAPI*)(SOCKET, char*, int, int);
using FnWSARecv        = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

// ws2_32 trampolines
static FnGetAddrInfo   g_origGetAddrInfo    = nullptr;
static FnGetAddrInfoW  g_origGetAddrInfoW   = nullptr;
static FnGetAddrInfoExW g_origGetAddrInfoExW = nullptr;
static FnGetAddrInfoExA g_origGetAddrInfoExA = nullptr;
static FnGetHostByName g_origGetHostByName2 = nullptr;  // ws2_32 (separate from wsock32 export)
static FnConnect       g_origConnect       = nullptr;
static FnWSAConnect    g_origWSAConnect    = nullptr;
static FnSendTo        g_origSendTo        = nullptr;
static FnWSASendTo     g_origWSASendTo     = nullptr;
static FnRecvFrom      g_origRecvFrom      = nullptr;
static FnWSARecvFrom   g_origWSARecvFrom   = nullptr;
static FnSend          g_origSend          = nullptr;
static FnWSASend       g_origWSASend       = nullptr;
static FnRecv          g_origRecv          = nullptr;
static FnWSARecv       g_origWSARecv       = nullptr;

// wsock32 trampolines (separate entries; on modern Windows wsock32 forwards to
// ws2_32 internally, but games linked directly against wsock32 call these
// exports and MinHook must patch them independently)
static FnGetHostByName g_origGetHostByName = nullptr;  // wsock32 only
static FnConnect       g_origConnect32     = nullptr;
static FnSendTo        g_origSendTo32      = nullptr;
static FnRecvFrom      g_origRecvFrom32    = nullptr;
static FnSend          g_origSend32        = nullptr;
static FnRecv          g_origRecv32        = nullptr;

// ---------------------------------------------------------------------------
// Address collection — each unique host is logged once; addresses are stored
// for deferred FastDL probing at download time (not probed immediately).
// ---------------------------------------------------------------------------
struct DiscoveredAddress { std::wstring host; int gamePort; };

static std::set<std::wstring>        g_seenHosts;
static std::vector<DiscoveredAddress> g_discoveredAddresses;
static size_t                        g_probedCount = 0; // index of next address to probe
static std::mutex                    g_discoveredMutex;

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

static std::string WideToAnsi(const std::wstring& s)
{
    if (s.empty()) return {};
    int len = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

// Pool a substituted DNS name so that the returned pointer remains valid for
// the lifetime of the process. Required by GetAddrInfoExW/A whose `pName`
// argument may be retained until an asynchronous lookup completes — passing a
// stack-local string would dangle. std::set guarantees pointer/iterator
// stability across insertions, so existing entries are never invalidated.
static const wchar_t* StableWide(std::wstring s)
{
    static std::mutex                  mtx;
    static std::set<std::wstring>      pool;
    std::lock_guard<std::mutex> lk(mtx);
    return pool.insert(std::move(s)).first->c_str();
}

static const char* StableAnsi(std::string s)
{
    static std::mutex                  mtx;
    static std::set<std::string>       pool;
    std::lock_guard<std::mutex> lk(mtx);
    return pool.insert(std::move(s)).first->c_str();
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

// Returns true if port falls within any configured filtered range (e.g. server
// browser ports that should not be treated as game server addresses).
static bool IsPortFiltered(int port)
{
    if (port <= 0)
        return false;

    for (const auto& range : g_fastdlFilteredPorts)
        if (port >= range.min && port <= range.max)
            return true;

    return false;
}

// ---------------------------------------------------------------------------
// OnHostDiscovered — called once per unique host/address
//
// gameServerPort: the port the game connected to (host byte order), or -1 if
//                 unknown (e.g. from a DNS lookup with no associated connect).
// Stores the address for deferred FastDL probing; does NOT probe immediately.
// ---------------------------------------------------------------------------
static void OnHostDiscovered(const std::wstring& host, int gameServerPort = -1)
{
    if (IsSkippableHost(host))
        return;

    // Skip addresses on server browser / non-game-server port ranges
    if (IsPortFiltered(gameServerPort))
        return;

    {
        std::lock_guard<std::mutex> lk(g_discoveredMutex);
        if (!g_seenHosts.insert(host).second)
            return; // already recorded this session

        if (g_fastdlProbeConnections && g_fastdlEnabled)
            g_discoveredAddresses.push_back({ host, gameServerPort });
    }

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
    if (nodename && nodename[0] != '\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname      = AnsiToWide(nodename);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogNetworkAccess(L"DNS REDIRECT", wname.c_str(), redirected.c_str());

            const std::string ansi = WideToAnsi(redirected);
            const int ret = g_origGetAddrInfo(ansi.c_str(), servname, hints, result);

            if (ret == 0)
                OnHostDiscovered(redirected);

            return ret;
        }
    }

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
    if (nodename && nodename[0] != L'\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname(nodename);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogNetworkAccess(L"DNS REDIRECT", wname.c_str(), redirected.c_str());

            const int ret = g_origGetAddrInfoW(redirected.c_str(), servname, hints, result);

            if (ret == 0)
                OnHostDiscovered(redirected);

            return ret;
        }
    }

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

// Capture the peer address of a connected socket (TCP send/recv path).
static void OnSocketActivity(SOCKET s)
{
    sockaddr_storage ss{};
    int sslen = static_cast<int>(sizeof(ss));
    if (getpeername(s, reinterpret_cast<sockaddr*>(&ss), &sslen) == 0)
    {
        const std::wstring host = SockAddrToHost(reinterpret_cast<sockaddr*>(&ss));
        const int          port = SockAddrToPort(reinterpret_cast<sockaddr*>(&ss));
        if (!host.empty())
            OnHostDiscovered(host, port);
    }
}

static int WSAAPI HookSendTo(
    SOCKET s, const char* buf, int len, int flags,
    const sockaddr* to, int tolen)
{
    if (to)
    {
        const std::wstring host = SockAddrToHost(to);
        const int          port = SockAddrToPort(to);
        if (!host.empty())
            OnHostDiscovered(host, port);
    }
    return g_origSendTo(s, buf, len, flags, to, tolen);
}

static int WSAAPI HookWSASendTo(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesSent, DWORD dwFlags,
    const sockaddr* lpTo, int iTolen,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    if (lpTo)
    {
        const std::wstring host = SockAddrToHost(lpTo);
        const int          port = SockAddrToPort(lpTo);
        if (!host.empty())
            OnHostDiscovered(host, port);
    }
    return g_origWSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent,
        dwFlags, lpTo, iTolen, lpOverlapped, lpCompletionRoutine);
}

static int WSAAPI HookRecvFrom(
    SOCKET s, char* buf, int len, int flags,
    sockaddr* from, int* fromlen)
{
    const int ret = g_origRecvFrom(s, buf, len, flags, from, fromlen);
    if (ret != SOCKET_ERROR && from)
    {
        const std::wstring host = SockAddrToHost(from);
        const int          port = SockAddrToPort(from);
        if (!host.empty())
            OnHostDiscovered(host, port);
    }
    return ret;
}

static int WSAAPI HookWSARecvFrom(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
    sockaddr* lpFrom, LPINT lpFromlen,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    const int ret = g_origWSARecvFrom(s, lpBuffers, dwBufferCount,
        lpNumberOfBytesRecvd, lpFlags, lpFrom, lpFromlen,
        lpOverlapped, lpCompletionRoutine);
    // ret == 0 means data received synchronously; lpFrom is valid then.
    if (ret == 0 && lpFrom)
    {
        const std::wstring host = SockAddrToHost(lpFrom);
        const int          port = SockAddrToPort(lpFrom);
        if (!host.empty())
            OnHostDiscovered(host, port);
    }
    return ret;
}

static int WSAAPI HookSend(SOCKET s, const char* buf, int len, int flags)
{
    OnSocketActivity(s);
    return g_origSend(s, buf, len, flags);
}

static int WSAAPI HookWSASend(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesSent, DWORD dwFlags,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    OnSocketActivity(s);
    return g_origWSASend(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent,
        dwFlags, lpOverlapped, lpCompletionRoutine);
}

static int WSAAPI HookRecv(SOCKET s, char* buf, int len, int flags)
{
    OnSocketActivity(s);
    return g_origRecv(s, buf, len, flags);
}

static int WSAAPI HookWSARecv(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    OnSocketActivity(s);
    return g_origWSARecv(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd,
        lpFlags, lpOverlapped, lpCompletionRoutine);
}

// ---------------------------------------------------------------------------
// wsock32 hook implementations
// ---------------------------------------------------------------------------

// ws2_32!gethostbyname — modern apps that link directly against ws2_32 use
// this entry point. The wsock32 export of the same name forwards here on
// modern Windows, so when both hooks are installed the wsock32 path will
// re-enter through this hook on its way down. That double-invocation is
// harmless: by the time the inner call runs the name has already been
// substituted, so ApplyDnsRedirect returns it unchanged.
static hostent* WSAAPI HookGetHostByName2(const char* name)
{
    if (name && name[0] != '\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname      = AnsiToWide(name);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogNetworkAccess(L"DNS REDIRECT", wname.c_str(), redirected.c_str());

            const std::string ansi = WideToAnsi(redirected);
            hostent* ret = g_origGetHostByName2(ansi.c_str());

            if (ret)
                OnHostDiscovered(redirected);

            return ret;
        }
    }

    hostent* ret = g_origGetHostByName2(name);

    if (ret && name && name[0] != '\0')
        OnHostDiscovered(AnsiToWide(name));

    return ret;
}

// GetAddrInfoExW — async-capable wide resolver. When `lpOverlapped` is non-
// null the OS may keep `pName` referenced until the operation completes, so a
// substituted name must come from the StableWide pool rather than the stack.
static int WSAAPI HookGetAddrInfoExW(
    PCWSTR             pName,
    PCWSTR             pServiceName,
    DWORD              dwNameSpace,
    LPGUID             lpNspId,
    const ADDRINFOEXW* hints,
    PADDRINFOEXW*      ppResult,
    struct timeval*    timeout,
    LPOVERLAPPED       lpOverlapped,
    LPLOOKUPSERVICE_COMPLETION_ROUTINE lpCompletionRoutine,
    LPHANDLE           lpHandle)
{
    if (pName && pName[0] != L'\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname(pName);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogNetworkAccess(L"DNS REDIRECT", wname.c_str(), redirected.c_str());

            const wchar_t* stableName = StableWide(redirected);
            const int ret = g_origGetAddrInfoExW(
                stableName, pServiceName, dwNameSpace, lpNspId, hints, ppResult,
                timeout, lpOverlapped, lpCompletionRoutine, lpHandle);

            if (ret == 0 || ret == WSA_IO_PENDING)
                OnHostDiscovered(redirected);

            return ret;
        }
    }

    const int ret = g_origGetAddrInfoExW(
        pName, pServiceName, dwNameSpace, lpNspId, hints, ppResult,
        timeout, lpOverlapped, lpCompletionRoutine, lpHandle);

    if ((ret == 0 || ret == WSA_IO_PENDING) && pName && pName[0] != L'\0')
        OnHostDiscovered(std::wstring(pName));

    return ret;
}

// GetAddrInfoExA — ANSI counterpart of GetAddrInfoExW. Same async caveat
// applies, hence the StableAnsi pool for the substituted name.
static int WSAAPI HookGetAddrInfoExA(
    PCSTR              pName,
    PCSTR              pServiceName,
    DWORD              dwNameSpace,
    LPGUID             lpNspId,
    const ADDRINFOEXA* hints,
    PADDRINFOEXA*      ppResult,
    struct timeval*    timeout,
    LPOVERLAPPED       lpOverlapped,
    LPLOOKUPSERVICE_COMPLETION_ROUTINE lpCompletionRoutine,
    LPHANDLE           lpHandle)
{
    if (pName && pName[0] != '\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname      = AnsiToWide(pName);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogNetworkAccess(L"DNS REDIRECT", wname.c_str(), redirected.c_str());

            const char* stableName = StableAnsi(WideToAnsi(redirected));
            const int ret = g_origGetAddrInfoExA(
                stableName, pServiceName, dwNameSpace, lpNspId, hints, ppResult,
                timeout, lpOverlapped, lpCompletionRoutine, lpHandle);

            if (ret == 0 || ret == WSA_IO_PENDING)
                OnHostDiscovered(redirected);

            return ret;
        }
    }

    const int ret = g_origGetAddrInfoExA(
        pName, pServiceName, dwNameSpace, lpNspId, hints, ppResult,
        timeout, lpOverlapped, lpCompletionRoutine, lpHandle);

    if ((ret == 0 || ret == WSA_IO_PENDING) && pName && pName[0] != '\0')
        OnHostDiscovered(AnsiToWide(pName));

    return ret;
}

// gethostbyname — Winsock 1 DNS resolution (wsock32 only; no ws2_32 equivalent)
static hostent* WSAAPI HookGetHostByName(const char* name)
{
    if (name && name[0] != '\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname      = AnsiToWide(name);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogNetworkAccess(L"DNS REDIRECT", wname.c_str(), redirected.c_str());

            const std::string ansi = WideToAnsi(redirected);
            hostent* ret = g_origGetHostByName(ansi.c_str());

            if (ret)
                OnHostDiscovered(redirected);

            return ret;
        }
    }

    hostent* ret = g_origGetHostByName(name);

    if (ret && name && name[0] != '\0')
        OnHostDiscovered(AnsiToWide(name));

    return ret;
}

// The remaining wsock32 functions share signatures with their ws2_32 counterparts;
// delegate to the same OnHostDiscovered/OnSocketActivity logic but call the
// wsock32 trampolines so we don't cross DLL entry points.

static int WSAAPI HookConnect32(SOCKET s, const sockaddr* name, int namelen)
{
    const std::wstring host = SockAddrToHost(name);
    const int          port = SockAddrToPort(name);
    const int ret = g_origConnect32(s, name, namelen);
    if (!host.empty())
        OnHostDiscovered(host, port);
    return ret;
}

static int WSAAPI HookSendTo32(
    SOCKET s, const char* buf, int len, int flags,
    const sockaddr* to, int tolen)
{
    if (to)
    {
        const std::wstring host = SockAddrToHost(to);
        const int          port = SockAddrToPort(to);
        if (!host.empty())
            OnHostDiscovered(host, port);
    }
    return g_origSendTo32(s, buf, len, flags, to, tolen);
}

static int WSAAPI HookRecvFrom32(
    SOCKET s, char* buf, int len, int flags,
    sockaddr* from, int* fromlen)
{
    const int ret = g_origRecvFrom32(s, buf, len, flags, from, fromlen);
    if (ret != SOCKET_ERROR && from)
    {
        const std::wstring host = SockAddrToHost(from);
        const int          port = SockAddrToPort(from);
        if (!host.empty())
            OnHostDiscovered(host, port);
    }
    return ret;
}

static int WSAAPI HookSend32(SOCKET s, const char* buf, int len, int flags)
{
    OnSocketActivity(s);
    return g_origSend32(s, buf, len, flags);
}

static int WSAAPI HookRecv32(SOCKET s, char* buf, int len, int flags)
{
    OnSocketActivity(s);
    return g_origRecv32(s, buf, len, flags);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void InstallNetworkHooks()
{
    if (!g_logNetwork && !g_fastdlProbeConnections && g_dnsRedirects.empty())
    {
        InterposerLog(L"HOOK INIT", L"network hooks skipped (Logging.Network, FastDL.ProbeConnections, DnsRedirects all unset)");
        return;
    }

    LogHookInit(L"ws2_32", "getaddrinfo",
        MH_CreateHookApi(L"ws2_32", "getaddrinfo",
            reinterpret_cast<LPVOID>(HookGetAddrInfo),
            reinterpret_cast<LPVOID*>(&g_origGetAddrInfo)));

    LogHookInit(L"ws2_32", "GetAddrInfoW",
        MH_CreateHookApi(L"ws2_32", "GetAddrInfoW",
            reinterpret_cast<LPVOID>(HookGetAddrInfoW),
            reinterpret_cast<LPVOID*>(&g_origGetAddrInfoW)));

    LogHookInit(L"ws2_32", "GetAddrInfoExW",
        MH_CreateHookApi(L"ws2_32", "GetAddrInfoExW",
            reinterpret_cast<LPVOID>(HookGetAddrInfoExW),
            reinterpret_cast<LPVOID*>(&g_origGetAddrInfoExW)));

    LogHookInit(L"ws2_32", "GetAddrInfoExA",
        MH_CreateHookApi(L"ws2_32", "GetAddrInfoExA",
            reinterpret_cast<LPVOID>(HookGetAddrInfoExA),
            reinterpret_cast<LPVOID*>(&g_origGetAddrInfoExA)));

    LogHookInit(L"ws2_32", "gethostbyname",
        MH_CreateHookApi(L"ws2_32", "gethostbyname",
            reinterpret_cast<LPVOID>(HookGetHostByName2),
            reinterpret_cast<LPVOID*>(&g_origGetHostByName2)));

    LogHookInit(L"ws2_32", "connect",
        MH_CreateHookApi(L"ws2_32", "connect",
            reinterpret_cast<LPVOID>(HookConnect),
            reinterpret_cast<LPVOID*>(&g_origConnect)));

    LogHookInit(L"ws2_32", "WSAConnect",
        MH_CreateHookApi(L"ws2_32", "WSAConnect",
            reinterpret_cast<LPVOID>(HookWSAConnect),
            reinterpret_cast<LPVOID*>(&g_origWSAConnect)));

    LogHookInit(L"ws2_32", "sendto",
        MH_CreateHookApi(L"ws2_32", "sendto",
            reinterpret_cast<LPVOID>(HookSendTo),
            reinterpret_cast<LPVOID*>(&g_origSendTo)));

    LogHookInit(L"ws2_32", "WSASendTo",
        MH_CreateHookApi(L"ws2_32", "WSASendTo",
            reinterpret_cast<LPVOID>(HookWSASendTo),
            reinterpret_cast<LPVOID*>(&g_origWSASendTo)));

    LogHookInit(L"ws2_32", "recvfrom",
        MH_CreateHookApi(L"ws2_32", "recvfrom",
            reinterpret_cast<LPVOID>(HookRecvFrom),
            reinterpret_cast<LPVOID*>(&g_origRecvFrom)));

    LogHookInit(L"ws2_32", "WSARecvFrom",
        MH_CreateHookApi(L"ws2_32", "WSARecvFrom",
            reinterpret_cast<LPVOID>(HookWSARecvFrom),
            reinterpret_cast<LPVOID*>(&g_origWSARecvFrom)));

    LogHookInit(L"ws2_32", "send",
        MH_CreateHookApi(L"ws2_32", "send",
            reinterpret_cast<LPVOID>(HookSend),
            reinterpret_cast<LPVOID*>(&g_origSend)));

    LogHookInit(L"ws2_32", "WSASend",
        MH_CreateHookApi(L"ws2_32", "WSASend",
            reinterpret_cast<LPVOID>(HookWSASend),
            reinterpret_cast<LPVOID*>(&g_origWSASend)));

    LogHookInit(L"ws2_32", "recv",
        MH_CreateHookApi(L"ws2_32", "recv",
            reinterpret_cast<LPVOID>(HookRecv),
            reinterpret_cast<LPVOID*>(&g_origRecv)));

    LogHookInit(L"ws2_32", "WSARecv",
        MH_CreateHookApi(L"ws2_32", "WSARecv",
            reinterpret_cast<LPVOID>(HookWSARecv),
            reinterpret_cast<LPVOID*>(&g_origWSARecv)));

    // wsock32 — older games (Winsock 1) link against this DLL directly.
    // gethostbyname is wsock32-only; connect/send*/recv* share signatures with
    // ws2_32 but need separate trampolines so MinHook patches the right exports.
    // MH_ERROR_MODULE_NOT_FOUND here means wsock32.dll was not yet loaded at inject time.
    LogHookInit(L"wsock32", "gethostbyname",
        MH_CreateHookApi(L"wsock32", "gethostbyname",
            reinterpret_cast<LPVOID>(HookGetHostByName),
            reinterpret_cast<LPVOID*>(&g_origGetHostByName)));

    LogHookInit(L"wsock32", "connect",
        MH_CreateHookApi(L"wsock32", "connect",
            reinterpret_cast<LPVOID>(HookConnect32),
            reinterpret_cast<LPVOID*>(&g_origConnect32)));

    LogHookInit(L"wsock32", "sendto",
        MH_CreateHookApi(L"wsock32", "sendto",
            reinterpret_cast<LPVOID>(HookSendTo32),
            reinterpret_cast<LPVOID*>(&g_origSendTo32)));

    LogHookInit(L"wsock32", "recvfrom",
        MH_CreateHookApi(L"wsock32", "recvfrom",
            reinterpret_cast<LPVOID>(HookRecvFrom32),
            reinterpret_cast<LPVOID*>(&g_origRecvFrom32)));

    LogHookInit(L"wsock32", "send",
        MH_CreateHookApi(L"wsock32", "send",
            reinterpret_cast<LPVOID>(HookSend32),
            reinterpret_cast<LPVOID*>(&g_origSend32)));

    LogHookInit(L"wsock32", "recv",
        MH_CreateHookApi(L"wsock32", "recv",
            reinterpret_cast<LPVOID>(HookRecv32),
            reinterpret_cast<LPVOID*>(&g_origRecv32)));
}

void LateInstallNetworkHooks(const wchar_t* moduleName)
{
    if (!g_logNetwork && !g_fastdlProbeConnections && g_dnsRedirects.empty())
        return;

    if (_wcsicmp(moduleName, L"wsock32.dll") != 0)
        return;

    LogHookInit(L"wsock32", "gethostbyname",
        MH_CreateHookApi(L"wsock32", "gethostbyname",
            reinterpret_cast<LPVOID>(HookGetHostByName),
            reinterpret_cast<LPVOID*>(&g_origGetHostByName)));

    LogHookInit(L"wsock32", "connect",
        MH_CreateHookApi(L"wsock32", "connect",
            reinterpret_cast<LPVOID>(HookConnect32),
            reinterpret_cast<LPVOID*>(&g_origConnect32)));

    LogHookInit(L"wsock32", "sendto",
        MH_CreateHookApi(L"wsock32", "sendto",
            reinterpret_cast<LPVOID>(HookSendTo32),
            reinterpret_cast<LPVOID*>(&g_origSendTo32)));

    LogHookInit(L"wsock32", "recvfrom",
        MH_CreateHookApi(L"wsock32", "recvfrom",
            reinterpret_cast<LPVOID>(HookRecvFrom32),
            reinterpret_cast<LPVOID*>(&g_origRecvFrom32)));

    LogHookInit(L"wsock32", "send",
        MH_CreateHookApi(L"wsock32", "send",
            reinterpret_cast<LPVOID>(HookSend32),
            reinterpret_cast<LPVOID*>(&g_origSend32)));

    LogHookInit(L"wsock32", "recv",
        MH_CreateHookApi(L"wsock32", "recv",
            reinterpret_cast<LPVOID>(HookRecv32),
            reinterpret_cast<LPVOID*>(&g_origRecv32)));

    MH_EnableHook(MH_ALL_HOOKS);
}

void ProbeAllDiscoveredAddresses()
{
    if (!g_fastdlProbeConnections || !g_fastdlEnabled)
        return;

    // Snapshot addresses that have not yet been probed, advancing g_probedCount
    // atomically so concurrent callers don't double-probe the same entry.
    std::vector<DiscoveredAddress> toProbe;
    {
        std::lock_guard<std::mutex> lk(g_discoveredMutex);
        const size_t total = g_discoveredAddresses.size();
        if (g_probedCount >= total)
            return;
        toProbe.assign(g_discoveredAddresses.begin() + static_cast<ptrdiff_t>(g_probedCount),
                        g_discoveredAddresses.end());
        g_probedCount = total;
    }

    for (const auto& addr : toProbe)
        ProbeServerForFastDL(addr.host, g_fastdlProbePort, addr.gamePort);
}

void RemoveNetworkHooks()
{
    std::lock_guard<std::mutex> lk(g_discoveredMutex);
    g_seenHosts.clear();
    g_discoveredAddresses.clear();
    g_probedCount = 0;
}
