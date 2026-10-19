#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <set>
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwchar>

#include <mswsock.h>

#include "network.h"
#include "callbacks.h"
#include "config.h"
#include "fastdl.h"
#include "ipx.h"
#include "relay.h"
#include "relay_protocol.h"

thread_local int t_netHookDepth = 0;

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
using FnWSAIoctl       = int(WSAAPI*)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

// Socket lifecycle and addressing -- installed only for the network relay and
// IPX emulation (RelayHooksWanted), which have to see every socket the game
// creates, binds, names and closes.
using FnSocket         = SOCKET(WSAAPI*)(int, int, int);
using FnWSASocketW     = SOCKET(WSAAPI*)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
using FnWSASocketA     = SOCKET(WSAAPI*)(int, int, int, LPWSAPROTOCOL_INFOA, GROUP, DWORD);
using FnBind           = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using FnCloseSocket    = int(WSAAPI*)(SOCKET);
using FnGetSockName    = int(WSAAPI*)(SOCKET, sockaddr*, int*);
using FnAccept         = SOCKET(WSAAPI*)(SOCKET, sockaddr*, int*);
using FnWSAAccept      = SOCKET(WSAAPI*)(SOCKET, sockaddr*, LPINT, LPCONDITIONPROC, DWORD_PTR);
using FnGetSockOpt     = int(WSAAPI*)(SOCKET, int, int, char*, int*);
using FnSetSockOpt     = int(WSAAPI*)(SOCKET, int, int, const char*, int);
using FnIoctlSocket    = int(WSAAPI*)(SOCKET, long, u_long*);

static FnSocket            g_origSocket           = nullptr;
static FnWSASocketW        g_origWSASocketW       = nullptr;
static FnWSASocketA        g_origWSASocketA       = nullptr;
static FnBind              g_origBind             = nullptr;
static FnCloseSocket       g_origCloseSocket      = nullptr;
static FnGetSockName       g_origGetSockName      = nullptr;
static FnGetSockName       g_origGetPeerName      = nullptr;
static FnAccept            g_origAccept           = nullptr;
static FnWSAAccept         g_origWSAAccept        = nullptr;
static FnGetSockOpt        g_origGetSockOpt       = nullptr;
static FnSetSockOpt        g_origSetSockOpt       = nullptr;
static FnIoctlSocket       g_origIoctlSocket      = nullptr;
static FnWSAEnumProtocolsW g_origWSAEnumProtocolsW = nullptr;
static FnWSAEnumProtocolsA g_origWSAEnumProtocolsA = nullptr;
static FnEnumProtocolsW    g_origEnumProtocolsW   = nullptr; // wsock32 (Winsock 1.1 name space)
static FnEnumProtocolsA    g_origEnumProtocolsA   = nullptr;

// The mswsock extension functions a game gets from WSAIoctl rather than an
// export, wrapped so the relay sees their addresses too.
static LPFN_CONNECTEX             g_realConnectEx             = nullptr;
static LPFN_GETACCEPTEXSOCKADDRS  g_realGetAcceptExSockaddrs  = nullptr;

// iphlpapi adapter-enumeration trampolines (resolved by name; iphlpapi.lib is
// intentionally NOT linked).
using FnGetAdaptersInfo      = DWORD(WINAPI*)(PIP_ADAPTER_INFO, PULONG);
using FnGetAdaptersAddresses = ULONG(WINAPI*)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);

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
static FnWSAIoctl      g_origWSAIoctl      = nullptr;

static FnGetAdaptersInfo      g_origGetAdaptersInfo      = nullptr;
static FnGetAdaptersAddresses g_origGetAdaptersAddresses = nullptr;

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
// Network adapter filtering (NetworkAdapters config) — allow-list helpers.
// An adapter is KEPT if it matches ANY populated criterion. When no filter is
// configured the feature is inert.
// ---------------------------------------------------------------------------
static inline bool NaActive() { return g_naEnabled && g_naAnyFilters; }

// The adapter-enumeration hooks are needed when filtering is active, when
// network logging is on (so every enumeration request can be logged even with
// no filters configured), or when the relay has a virtual adapter to show.
static inline bool AdapterHooksWanted() { return NaActive() || g_logNetwork || RelayHooksWanted(); }

// Any reason at all to hook Winsock.
static inline bool NetworkHooksWanted()
{
    return g_logNetwork || g_fastdlProbeConnections || !g_dnsRedirects.empty() || NaActive() ||
           RelayHooksWanted();
}

// ipHostOrder: an IPv4 address already in HOST byte order.
static bool SubnetMatch(uint32_t ipHostOrder)
{
    for (const auto& s : g_naSubnets)
        if ((ipHostOrder & s.mask) == s.network)
            return true;
    return false;
}

// in_addr / sin_addr store IPv4 in network byte order; convert before comparing.
static bool SubnetMatchInAddr(const in_addr& a)
{
    return SubnetMatch(ntohl(a.s_addr));
}

static bool NameMatch(const wchar_t* name)
{
    if (!name || !name[0]) return false;
    try
    {
        const std::wstring s(name);
        for (const auto& re : g_naNames)
            if (std::regex_search(s, re))
                return true;
    }
    catch (const std::regex_error&) { /* pathological input — treat as no match */ }
    return false;
}

static bool MacMatch(const BYTE* addr, size_t len)
{
    if (!addr || len != 6) return false;
    for (const auto& m : g_naMacs)
        if (memcmp(m.data(), addr, 6) == 0)
            return true;
    return false;
}

// Returns true when a gethostbyname argument refers to the local machine:
// null/empty, or case-insensitively equal to the gethostname() result.
static bool IsLocalHostName(const char* name)
{
    if (!name || name[0] == '\0') return true;
    char host[256]{};
    if (gethostname(host, sizeof(host)) != 0) return false;
    return _stricmp(name, host) == 0;
}

// Compact a hostent's IPv4 h_addr_list in place, keeping only addresses that
// match a configured subnet. The hostent lives in Winsock per-thread storage, so
// rewriting the pointer array is thread-safe. To avoid crashing callers that
// dereference h_addr_list[0] without a null check, the list is left untouched
// when zero addresses would survive. Only the subnet filter applies here.
static void FilterHostentAddrs(hostent* he)
{
    if (!he || !he->h_addr_list) return;
    if (he->h_addrtype != AF_INET || he->h_length != 4) return;

    char** list = he->h_addr_list;

    int kept = 0;
    for (int r = 0; list[r] != nullptr; ++r)
        if (SubnetMatchInAddr(*reinterpret_cast<const in_addr*>(list[r])))
            ++kept;

    if (kept == 0) return; // emptying the list risks crashing naive callers

    int w = 0;
    for (int r = 0; list[r] != nullptr; ++r)
        if (SubnetMatchInAddr(*reinterpret_cast<const in_addr*>(list[r])))
            list[w++] = list[r];
    list[w] = nullptr;
}

// ---------------------------------------------------------------------------
// Network relay helpers
// ---------------------------------------------------------------------------

// True when a translating hook should do its work: the relay hooks are wanted
// and this is not a call the relay or IPX code is making on the game's behalf.
static inline bool Translating()
{
    return t_netHookDepth == 0 && RelayHooksWanted();
}

// SOCK_STREAM or SOCK_DGRAM; 0 when the socket cannot say.
static int SocketType(SOCKET s)
{
    int type = 0;
    int len  = sizeof(type);
    if (getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &len) != 0)
        return 0;
    return type;
}

// Our own host name, resolved to the virtual address alone. The hostent lives
// in thread-local storage, as Winsock's own does, so the usual contract -- valid
// until this thread's next resolver call -- holds.
struct VirtualHostent
{
    hostent he;
    char    name[256];
    char*   aliases[1];
    char*   addresses[2];
    in_addr address;
};

static thread_local VirtualHostent t_virtualHost;

static hostent* VirtualLocalHostent()
{
    VirtualHostent& v = t_virtualHost;

    if (gethostname(v.name, sizeof(v.name)) != 0)
        strcpy_s(v.name, "localhost");

    v.address.s_addr = htonl(RelayVirtualIp());
    v.aliases[0]     = nullptr;
    v.addresses[0]   = reinterpret_cast<char*>(&v.address);
    v.addresses[1]   = nullptr;

    v.he.h_name      = v.name;
    v.he.h_aliases   = v.aliases;
    v.he.h_addrtype  = AF_INET;
    v.he.h_length    = 4;
    v.he.h_addr_list = v.addresses;
    return &v.he;
}

// A game asking for its own address is answered with the virtual one, so it
// advertises an address its peers can actually reach through the relay.
static bool WantsVirtualLocalAddress(const char* name)
{
    return Translating() && IsLocalHostName(name) && RelayWaitForAddress();
}

static std::string VirtualAddressText()
{
    const uint32_t ip = RelayVirtualIp();
    char buf[16]{};
    sprintf_s(buf, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

// The address family a getaddrinfo hint asks for, AF_UNSPEC when there is none.
template <typename Hints>
static int HintFamily(const Hints* hints)
{
#pragma warning(suppress: 4996) // ADDRINFOEXA is deprecated, and it is what GetAddrInfoExA's caller passed
    return hints ? hints->ai_family : AF_UNSPEC;
}

// ---------------------------------------------------------------------------
// Overlapped receives. A WSARecvFrom that is still pending when it returns
// fills in its source address later, from a portal on loopback. It is recorded
// here, by its OVERLAPPED, and the address is translated by whichever call
// tells the game the receive finished: WSAGetOverlappedResult,
// GetOverlappedResult(Ex), GetQueuedCompletionStatus(Ex), or the completion
// routine, which the hook wraps. A game that only polls HasOverlappedIoCompleted
// makes none of those calls and still sees 127.0.0.1 -- though a reply sent
// there reaches the right peer all the same.
// ---------------------------------------------------------------------------
struct PendingReceive
{
    SOCKET                             s;
    sockaddr*                          from;
    LPINT                              fromlen;
    LPWSAOVERLAPPED_COMPLETION_ROUTINE routine;  // the game's, when we wrapped it
};

static std::mutex                                          g_pendingMutex;
static std::unordered_map<LPWSAOVERLAPPED, PendingReceive> g_pendingReceives;
static std::atomic<size_t>                                 g_pendingCount{0};

static void TrackReceive(LPWSAOVERLAPPED ov, const PendingReceive& p)
{
    std::lock_guard<std::mutex> lk(g_pendingMutex);
    g_pendingReceives[ov] = p;
    g_pendingCount = g_pendingReceives.size();
}

static void ForgetReceive(LPWSAOVERLAPPED ov)
{
    std::lock_guard<std::mutex> lk(g_pendingMutex);
    g_pendingReceives.erase(ov);
    g_pendingCount = g_pendingReceives.size();
}

// Translate a finished receive's source address. The record is kept when the
// game gave a completion routine: the wrapper still has to find the routine
// to call, and translating twice is harmless.
static void CompleteReceive(const OVERLAPPED* ov)
{
    if (!ov || g_pendingCount.load() == 0)
        return;

    PendingReceive p{};
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        auto it = g_pendingReceives.find(const_cast<LPWSAOVERLAPPED>(ov));
        if (it == g_pendingReceives.end())
            return;

        p = it->second;
        if (!p.routine)
        {
            g_pendingReceives.erase(it);
            g_pendingCount = g_pendingReceives.size();
        }
    }

    if (p.from && p.fromlen)
        RelayMapDatagramSource(p.from, *p.fromlen);
}

static void CALLBACK RelayReceiveCompletion(DWORD error, DWORD bytes, LPWSAOVERLAPPED ov, DWORD flags)
{
    PendingReceive p{};
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        auto it = g_pendingReceives.find(ov);
        if (it != g_pendingReceives.end())
        {
            p = it->second;
            g_pendingReceives.erase(it);
            g_pendingCount = g_pendingReceives.size();
        }
    }

    if (error == 0 && p.from && p.fromlen)
        RelayMapDatagramSource(p.from, *p.fromlen);

    if (p.routine)
        p.routine(error, bytes, ov, flags);
}

// A closed socket's receives will never complete through anything we hook.
static void ForgetReceivesOf(SOCKET s)
{
    if (g_pendingCount.load() == 0)
        return;

    std::lock_guard<std::mutex> lk(g_pendingMutex);
    for (auto it = g_pendingReceives.begin(); it != g_pendingReceives.end();)
    {
        // One with a wrapped routine stays: its routine still runs, with an
        // abort status, and needs the record to find the game's.
        if (it->second.s == s && !it->second.routine) it = g_pendingReceives.erase(it);
        else                                           ++it;
    }
    g_pendingCount = g_pendingReceives.size();
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

    FireNetworkCallback(host.c_str(), gameServerPort > 0 ? gameServerPort : 0);

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
    // Our own host name resolves to the virtual address while the relay is up.
    if (nodename && nodename[0] != '\0' && HintFamily(hints) != AF_INET6 &&
        WantsVirtualLocalAddress(nodename))
        return g_origGetAddrInfo(VirtualAddressText().c_str(), servname, hints, result);

    if (nodename && nodename[0] != '\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname      = AnsiToWide(nodename);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogDnsRedirect( wname.c_str(), redirected.c_str());

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
    if (nodename && nodename[0] != L'\0' && HintFamily(hints) != AF_INET6 &&
        WantsVirtualLocalAddress(WideToAnsi(nodename).c_str()))
    {
        const std::string text = VirtualAddressText();
        return g_origGetAddrInfoW(std::wstring(text.begin(), text.end()).c_str(), servname, hints, result);
    }

    if (nodename && nodename[0] != L'\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname(nodename);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogDnsRedirect( wname.c_str(), redirected.c_str());

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

// connect()/WSAConnect() on a virtual address: a stream socket goes to the
// relay's gateway, a datagram socket to the portal for that endpoint. Returns
// true with the address to connect to instead in `mapped`.
static bool MapConnect(SOCKET s, const sockaddr* name, int namelen, sockaddr_in& mapped)
{
    switch (SocketType(s))
    {
    case SOCK_STREAM: return RelayPrepareStreamConnect(s, name, namelen, mapped);
    case SOCK_DGRAM:  return RelayMapDatagramDestination(name, namelen, mapped);
    default:          return false;
    }
}

static int WSAAPI HookConnect(SOCKET s, const sockaddr* name, int namelen)
{
    if (t_netHookDepth)
        return g_origConnect(s, name, namelen);

    // Capture address and port before the call (non-blocking connects return
    // SOCKET_ERROR immediately, so we can't rely on the return value).
    const std::wstring host = SockAddrToHost(name);
    const int          port = SockAddrToPort(name);

    int ret = 0;

    if (Translating())
    {
        NetHookScope scope;
        sockaddr_in  mapped{};

        if (IpxIsSocket(s))
            return IpxConnect(s, name, namelen);

        if (MapConnect(s, name, namelen, mapped))
            ret = g_origConnect(s, reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped));
        else
            ret = g_origConnect(s, name, namelen);
    }
    else
        ret = g_origConnect(s, name, namelen);

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
    if (t_netHookDepth)
        return g_origWSAConnect(s, name, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);

    const std::wstring host = SockAddrToHost(name);
    const int          port = SockAddrToPort(name);

    if (Translating())
    {
        NetHookScope scope;
        sockaddr_in  mapped{};

        if (IpxIsSocket(s))
            return IpxConnect(s, name, namelen);

        if (MapConnect(s, name, namelen, mapped))
        {
            const int ret = g_origWSAConnect(s, reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped),
                lpCallerData, lpCalleeData, lpSQOS, lpGQOS);

            if (!host.empty())
                OnHostDiscovered(host, port);

            return ret;
        }
    }

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
    if (t_netHookDepth)
        return g_origSendTo(s, buf, len, flags, to, tolen);

    if (Translating())
    {
        NetHookScope scope;
        sockaddr_in  mapped{};

        if (IpxIsSocket(s))
            return IpxSendTo(s, buf, len, flags, to, tolen);

        if (RelayMapDatagramDestination(to, tolen, mapped))
        {
            OnHostDiscovered(SockAddrToHost(to), SockAddrToPort(to));
            return g_origSendTo(s, buf, len, flags, reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped));
        }
    }

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
    if (t_netHookDepth)
        return g_origWSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent,
            dwFlags, lpTo, iTolen, lpOverlapped, lpCompletionRoutine);

    if (Translating())
    {
        NetHookScope scope;
        sockaddr_in  mapped{};

        if (IpxIsSocket(s))
            return IpxWSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent,
                dwFlags, lpTo, iTolen, lpOverlapped, lpCompletionRoutine);

        // The address is captured when the call is made, so a stack copy is
        // fine even for an overlapped send.
        if (RelayMapDatagramDestination(lpTo, iTolen, mapped))
        {
            OnHostDiscovered(SockAddrToHost(lpTo), SockAddrToPort(lpTo));
            return g_origWSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags,
                reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped), lpOverlapped, lpCompletionRoutine);
        }
    }

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
    if (t_netHookDepth)
        return g_origRecvFrom(s, buf, len, flags, from, fromlen);

    if (Translating())
    {
        NetHookScope scope;
        if (IpxIsSocket(s))
            return IpxRecvFrom(s, buf, len, flags, from, fromlen);
    }

    const int ret = g_origRecvFrom(s, buf, len, flags, from, fromlen);

    // A datagram the relay delivered arrives from a portal on loopback; the
    // game is shown the virtual endpoint it stands for. A truncated datagram
    // (WSAEMSGSIZE) still carries a valid source.
    if (from && fromlen && Translating() &&
        (ret != SOCKET_ERROR || WSAGetLastError() == WSAEMSGSIZE))
    {
        const int error = WSAGetLastError();
        RelayMapDatagramSource(from, *fromlen);
        WSASetLastError(error);
    }

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
    if (t_netHookDepth)
        return g_origWSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
            lpFrom, lpFromlen, lpOverlapped, lpCompletionRoutine);

    if (Translating())
    {
        NetHookScope scope;
        if (IpxIsSocket(s))
            return IpxWSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags,
                lpFrom, lpFromlen, lpOverlapped, lpCompletionRoutine);
    }

    // An overlapped receive is recorded before the call, because it can
    // complete -- and be reaped on another thread -- before the call returns.
    const bool track = lpOverlapped && lpFrom && lpFromlen && Translating() && RelayActive();

    if (track)
        TrackReceive(lpOverlapped, PendingReceive{ s, lpFrom, lpFromlen, lpCompletionRoutine });

    const int ret = g_origWSARecvFrom(s, lpBuffers, dwBufferCount,
        lpNumberOfBytesRecvd, lpFlags, lpFrom, lpFromlen, lpOverlapped,
        (track && lpCompletionRoutine) ? RelayReceiveCompletion : lpCompletionRoutine);

    const int error = WSAGetLastError();

    // A receive that completed synchronously has filled lpFrom already. One
    // still pending is translated when it completes (see PendingReceive).
    if (ret == 0 && lpFrom && lpFromlen && Translating())
        RelayMapDatagramSource(lpFrom, *lpFromlen);

    // Done with the record unless something will still report the completion:
    // a pending receive, or the routine, which is queued even on success.
    if (track && !lpCompletionRoutine && (ret == 0 || error != WSA_IO_PENDING))
        ForgetReceive(lpOverlapped);
    else if (track && ret != 0 && error != WSA_IO_PENDING)
        ForgetReceive(lpOverlapped);

    WSASetLastError(error);

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
    if (t_netHookDepth)
        return g_origSend(s, buf, len, flags);

    if (Translating())
    {
        NetHookScope scope;
        if (IpxIsSocket(s))
            return IpxSendTo(s, buf, len, flags, nullptr, 0);
    }

    OnSocketActivity(s);
    return g_origSend(s, buf, len, flags);
}

static int WSAAPI HookWSASend(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesSent, DWORD dwFlags,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    if (t_netHookDepth)
        return g_origWSASend(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent,
            dwFlags, lpOverlapped, lpCompletionRoutine);

    if (Translating())
    {
        NetHookScope scope;
        if (IpxIsSocket(s))
            return IpxWSASendTo(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent,
                dwFlags, nullptr, 0, lpOverlapped, lpCompletionRoutine);
    }

    OnSocketActivity(s);
    return g_origWSASend(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent,
        dwFlags, lpOverlapped, lpCompletionRoutine);
}

static int WSAAPI HookRecv(SOCKET s, char* buf, int len, int flags)
{
    if (t_netHookDepth)
        return g_origRecv(s, buf, len, flags);

    if (Translating())
    {
        NetHookScope scope;
        if (IpxIsSocket(s))
            return IpxRecvFrom(s, buf, len, flags, nullptr, nullptr);
    }

    OnSocketActivity(s);
    return g_origRecv(s, buf, len, flags);
}

static int WSAAPI HookWSARecv(
    SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    if (t_netHookDepth)
        return g_origWSARecv(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd,
            lpFlags, lpOverlapped, lpCompletionRoutine);

    if (Translating())
    {
        NetHookScope scope;
        if (IpxIsSocket(s))
            return IpxWSARecvFrom(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd,
                lpFlags, nullptr, nullptr, lpOverlapped, lpCompletionRoutine);
    }

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
    if (WantsVirtualLocalAddress(name))
        return VirtualLocalHostent();

    if (name && name[0] != '\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname      = AnsiToWide(name);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogDnsRedirect( wname.c_str(), redirected.c_str());

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

    if (ret && NaActive() && IsLocalHostName(name))
        FilterHostentAddrs(ret);

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
    // Our own host name: a numeric name, which an asynchronous lookup may keep
    // referring to after we return -- hence the stable pool.
    if (pName && pName[0] != L'\0' && HintFamily(hints) != AF_INET6 &&
        WantsVirtualLocalAddress(WideToAnsi(pName).c_str()))
    {
        const std::string text = VirtualAddressText();
        return g_origGetAddrInfoExW(StableWide(std::wstring(text.begin(), text.end())), pServiceName,
            dwNameSpace, lpNspId, hints, ppResult, timeout, lpOverlapped, lpCompletionRoutine, lpHandle);
    }

    if (pName && pName[0] != L'\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname(pName);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogDnsRedirect( wname.c_str(), redirected.c_str());

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
    if (pName && pName[0] != '\0' && HintFamily(hints) != AF_INET6 && WantsVirtualLocalAddress(pName))
        return g_origGetAddrInfoExA(StableAnsi(VirtualAddressText()), pServiceName, dwNameSpace, lpNspId,
            hints, ppResult, timeout, lpOverlapped, lpCompletionRoutine, lpHandle);

    if (pName && pName[0] != '\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname      = AnsiToWide(pName);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogDnsRedirect( wname.c_str(), redirected.c_str());

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
    if (WantsVirtualLocalAddress(name))
        return VirtualLocalHostent();

    if (name && name[0] != '\0' && !g_dnsRedirects.empty())
    {
        const std::wstring wname      = AnsiToWide(name);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogDnsRedirect( wname.c_str(), redirected.c_str());

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

    if (ret && NaActive() && IsLocalHostName(name))
        FilterHostentAddrs(ret);

    return ret;
}

// The remaining wsock32 functions share signatures with their ws2_32 counterparts;
// delegate to the same OnHostDiscovered/OnSocketActivity logic but call the
// wsock32 trampolines so we don't cross DLL entry points.

// None of these translate anything. Where wsock32 implements an export itself
// (XP) it does so on top of the ws2_32 one, whose hook does the translating;
// on later Windows they are forwarders and never get installed at all.

static int WSAAPI HookConnect32(SOCKET s, const sockaddr* name, int namelen)
{
    if (t_netHookDepth)
        return g_origConnect32(s, name, namelen);

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
    if (t_netHookDepth)
        return g_origSendTo32(s, buf, len, flags, to, tolen);

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
    if (t_netHookDepth)
        return g_origRecvFrom32(s, buf, len, flags, from, fromlen);

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
    if (t_netHookDepth)
        return g_origSend32(s, buf, len, flags);

    OnSocketActivity(s);
    return g_origSend32(s, buf, len, flags);
}

static int WSAAPI HookRecv32(SOCKET s, char* buf, int len, int flags)
{
    if (t_netHookDepth)
        return g_origRecv32(s, buf, len, flags);

    OnSocketActivity(s);
    return g_origRecv32(s, buf, len, flags);
}

// ---------------------------------------------------------------------------
// Socket lifecycle hooks — network relay and IPX emulation only. Each passes
// straight through at depth > 0 (a call the relay or IPX code is making) and
// otherwise holds a NetHookScope for whatever socket calls it makes itself.
// ---------------------------------------------------------------------------

static SOCKET WSAAPI HookSocket(int af, int type, int protocol)
{
    if (!Translating())
        return g_origSocket(af, type, protocol);

    NetHookScope scope;
    bool handled = false;
    SOCKET s = IpxSocket(af, type, protocol, nullptr, 0, false, handled);
    return handled ? s : g_origSocket(af, type, protocol);
}

static SOCKET WSAAPI HookWSASocketW(int af, int type, int protocol,
    LPWSAPROTOCOL_INFOW info, GROUP g, DWORD flags)
{
    if (!Translating())
        return g_origWSASocketW(af, type, protocol, info, g, flags);

    NetHookScope scope;
    bool handled = false;
    SOCKET s = IpxSocket(af, type, protocol, info, flags, true, handled);
    return handled ? s : g_origWSASocketW(af, type, protocol, info, g, flags);
}

static SOCKET WSAAPI HookWSASocketA(int af, int type, int protocol,
    LPWSAPROTOCOL_INFOA info, GROUP g, DWORD flags)
{
    if (!Translating())
        return g_origWSASocketA(af, type, protocol, info, g, flags);

    NetHookScope scope;

    // IpxSocket only reads the family, type and protocol of the info block.
    WSAPROTOCOL_INFOW wide{};
    if (info)
    {
#pragma warning(push)
#pragma warning(disable: 4996) // the A struct is deprecated, and it is what the game passed
        wide.iAddressFamily = info->iAddressFamily;
        wide.iSocketType    = info->iSocketType;
        wide.iProtocol      = info->iProtocol;
#pragma warning(pop)
    }

    bool handled = false;
    SOCKET s = IpxSocket(af, type, protocol, info ? &wide : nullptr, flags, true, handled);
    return handled ? s : g_origWSASocketA(af, type, protocol, info, g, flags);
}

static int WSAAPI HookBind(SOCKET s, const sockaddr* name, int namelen)
{
    if (!Translating())
        return g_origBind(s, name, namelen);

    NetHookScope scope;

    if (IpxIsSocket(s))
        return IpxBind(s, name, namelen);

    sockaddr_in mapped{};
    if (RelayMapBind(s, name, namelen, mapped))
        return g_origBind(s, reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped));

    return g_origBind(s, name, namelen);
}

static int WSAAPI HookCloseSocket(SOCKET s)
{
    // Forget the socket before the handle is released: once it is, another
    // thread can be handed the same value for a brand-new socket.
    if (RelayHooksWanted())
    {
        IpxOnSocketClosed(s);
        RelayOnSocketClosed(s);
        ForgetReceivesOf(s);
    }

    return g_origCloseSocket(s);
}

static int WSAAPI HookGetSockName(SOCKET s, sockaddr* name, int* namelen)
{
    if (!Translating())
        return g_origGetSockName(s, name, namelen);

    NetHookScope scope;

    if (IpxIsSocket(s))
        return IpxGetSockName(s, name, namelen);

    const int ret = g_origGetSockName(s, name, namelen);
    if (ret == 0 && namelen)
        RelayMapLocalName(s, name, *namelen);
    return ret;
}

static int WSAAPI HookGetPeerName(SOCKET s, sockaddr* name, int* namelen)
{
    if (!Translating())
        return g_origGetPeerName(s, name, namelen);

    NetHookScope scope;

    if (IpxIsSocket(s))
        return IpxGetPeerName(s, name, namelen);

    const int ret = g_origGetPeerName(s, name, namelen);
    if (ret == 0 && namelen)
    {
        if (SocketType(s) == SOCK_STREAM)
            RelayMapStreamPeer(s, name, *namelen);
        else
            RelayMapDatagramSource(name, *namelen);
    }
    return ret;
}

// An accepted relay stream comes from the relay's connector on loopback; the
// game is shown the remote virtual endpoint instead. Called even when the game
// passed no address buffer, so the mapping is recorded for getpeername.
static void MapAccepted(SOCKET accepted, sockaddr* addr, int* addrlen)
{
    if (accepted == INVALID_SOCKET || !RelayActive())
        return;

    if (addr && addrlen)
    {
        RelayMapStreamPeer(accepted, addr, *addrlen);
        return;
    }

    sockaddr_in peer{};
    int peerLen = sizeof(peer);
    if (g_origGetPeerName(accepted, reinterpret_cast<sockaddr*>(&peer), &peerLen) == 0)
        RelayMapStreamPeer(accepted, reinterpret_cast<sockaddr*>(&peer), peerLen);
}

static SOCKET WSAAPI HookAccept(SOCKET s, sockaddr* addr, int* addrlen)
{
    if (!Translating())
        return g_origAccept(s, addr, addrlen);

    NetHookScope scope;
    const SOCKET accepted = g_origAccept(s, addr, addrlen);
    const int error = WSAGetLastError();
    MapAccepted(accepted, addr, addrlen);
    WSASetLastError(error);
    return accepted;
}

static SOCKET WSAAPI HookWSAAccept(SOCKET s, sockaddr* addr, LPINT addrlen,
    LPCONDITIONPROC condition, DWORD_PTR callbackData)
{
    if (!Translating())
        return g_origWSAAccept(s, addr, addrlen, condition, callbackData);

    // The condition callback still sees the loopback caller ID: rewriting it
    // would mean wrapping the game's callback, for a feature games rarely use.
    NetHookScope scope;
    const SOCKET accepted = g_origWSAAccept(s, addr, addrlen, condition, callbackData);
    const int error = WSAGetLastError();
    MapAccepted(accepted, addr, addrlen);
    WSASetLastError(error);
    return accepted;
}

static int WSAAPI HookGetSockOpt(SOCKET s, int level, int optname, char* optval, int* optlen)
{
    if (!Translating())
        return g_origGetSockOpt(s, level, optname, optval, optlen);

    NetHookScope scope;
    bool handled = false;
    const int ret = IpxGetSockOpt(s, level, optname, optval, optlen, handled);
    return handled ? ret : g_origGetSockOpt(s, level, optname, optval, optlen);
}

static int WSAAPI HookSetSockOpt(SOCKET s, int level, int optname, const char* optval, int optlen)
{
    if (!Translating())
        return g_origSetSockOpt(s, level, optname, optval, optlen);

    NetHookScope scope;
    bool handled = false;
    const int ret = IpxSetSockOpt(s, level, optname, optval, optlen, handled);
    return handled ? ret : g_origSetSockOpt(s, level, optname, optval, optlen);
}

static int WSAAPI HookIoctlSocket(SOCKET s, long cmd, u_long* argp)
{
    const int ret = g_origIoctlSocket(s, cmd, argp);

    if (ret == 0 && Translating())
    {
        NetHookScope scope;
        IpxAdjustReadable(s, cmd, argp);
    }

    return ret;
}

static int WSAAPI HookWSAEnumProtocolsW(LPINT protocols, LPWSAPROTOCOL_INFOW buffer, LPDWORD length)
{
    if (!Translating())
        return g_origWSAEnumProtocolsW(protocols, buffer, length);

    NetHookScope scope;
    return IpxEnumProtocolsW(g_origWSAEnumProtocolsW, protocols, buffer, length);
}

static int WSAAPI HookWSAEnumProtocolsA(LPINT protocols, LPWSAPROTOCOL_INFOA buffer, LPDWORD length)
{
    if (!Translating())
        return g_origWSAEnumProtocolsA(protocols, buffer, length);

    NetHookScope scope;
    return IpxEnumProtocolsA(g_origWSAEnumProtocolsA, protocols, buffer, length);
}

static int WSAAPI HookEnumProtocolsW(LPINT protocols, LPVOID buffer, LPDWORD length)
{
    if (!Translating())
        return g_origEnumProtocolsW(protocols, buffer, length);

    NetHookScope scope;
    return IpxLegacyEnumProtocols(reinterpret_cast<void*>(g_origEnumProtocolsW), true, protocols, buffer, length);
}

static int WSAAPI HookEnumProtocolsA(LPINT protocols, LPVOID buffer, LPDWORD length)
{
    if (!Translating())
        return g_origEnumProtocolsA(protocols, buffer, length);

    NetHookScope scope;
    return IpxLegacyEnumProtocols(reinterpret_cast<void*>(g_origEnumProtocolsA), false, protocols, buffer, length);
}

// ConnectEx and GetAcceptExSockaddrs are handed out by WSAIoctl, not exported,
// so the hook there returns these wrappers in their place.
static BOOL PASCAL RelayConnectEx(SOCKET s, const sockaddr* name, int namelen,
    PVOID sendBuffer, DWORD sendLength, LPDWORD sent, LPOVERLAPPED overlapped)
{
    if (Translating())
    {
        NetHookScope scope;
        sockaddr_in  mapped{};

        if (RelayPrepareStreamConnect(s, name, namelen, mapped))
            return g_realConnectEx(s, reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped),
                sendBuffer, sendLength, sent, overlapped);
    }

    return g_realConnectEx(s, name, namelen, sendBuffer, sendLength, sent, overlapped);
}

static void PASCAL RelayGetAcceptExSockaddrs(PVOID buffer, DWORD receiveLength,
    DWORD localLength, DWORD remoteLength, sockaddr** local, LPINT localLen,
    sockaddr** remote, LPINT remoteLen)
{
    g_realGetAcceptExSockaddrs(buffer, receiveLength, localLength, remoteLength,
        local, localLen, remote, remoteLen);

    // No socket handle reaches this function, so only the port-keyed inbound
    // mapping can apply.
    if (Translating() && remote && *remote && remoteLen)
    {
        NetHookScope scope;
        RelayMapStreamPeer(INVALID_SOCKET, *remote, *remoteLen);
    }
}

// ---------------------------------------------------------------------------
// Lookups, tables and echoes the relay answers itself. Each of these either
// reveals our own address or asks about a virtual one, and the real
// implementation knows nothing about either: it would report the real local
// address, ask real DNS about 10.13.37.x, or send ICMP that no relay carries.
// ---------------------------------------------------------------------------

using FnWSAAsyncGetHostByName = HANDLE(WSAAPI*)(HWND, u_int, const char*, char*, int);
using FnWSAAsyncGetHostByAddr = HANDLE(WSAAPI*)(HWND, u_int, const char*, int, int, char*, int);
using FnGetHostByAddr         = hostent*(WSAAPI*)(const char*, int, int);
using FnGetNameInfo           = int(WSAAPI*)(const SOCKADDR*, socklen_t, PCHAR, DWORD, PCHAR, DWORD, INT);
using FnGetNameInfoW          = int(WSAAPI*)(const SOCKADDR*, socklen_t, PWCHAR, DWORD, PWCHAR, DWORD, INT);
using FnGetIpAddrTable        = DWORD(WINAPI*)(PMIB_IPADDRTABLE, PULONG, BOOL);
using FnGetIfTable            = DWORD(WINAPI*)(PMIB_IFTABLE, PULONG, BOOL);
using FnGetIfEntry            = DWORD(WINAPI*)(PMIB_IFROW);
using FnIcmpSendEcho          = DWORD(WINAPI*)(HANDLE, IPAddr, LPVOID, WORD, PIP_OPTION_INFORMATION, LPVOID, DWORD, DWORD);
using FnIcmpSendEcho2         = DWORD(WINAPI*)(HANDLE, HANDLE, PVOID, PVOID, IPAddr, LPVOID, WORD, PIP_OPTION_INFORMATION, LPVOID, DWORD, DWORD);
using FnIcmpSendEcho2Ex       = DWORD(WINAPI*)(HANDLE, HANDLE, PVOID, PVOID, IPAddr, IPAddr, LPVOID, WORD, PIP_OPTION_INFORMATION, LPVOID, DWORD, DWORD);
using FnWSAGetOverlappedResult = BOOL(WSAAPI*)(SOCKET, LPWSAOVERLAPPED, LPDWORD, BOOL, LPDWORD);
using FnGetOverlappedResult   = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
using FnGetOverlappedResultEx = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
using FnGetQueuedCompletionStatus   = BOOL(WINAPI*)(HANDLE, LPDWORD, PULONG_PTR, LPOVERLAPPED*, DWORD);
using FnGetQueuedCompletionStatusEx = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED_ENTRY, ULONG, PULONG, DWORD, BOOL);

static FnWSAAsyncGetHostByName g_origWSAAsyncGetHostByName = nullptr;
static FnWSAAsyncGetHostByAddr g_origWSAAsyncGetHostByAddr = nullptr;
static FnGetHostByAddr         g_origGetHostByAddr         = nullptr;
static FnGetNameInfo           g_origGetNameInfo           = nullptr;
static FnGetNameInfoW          g_origGetNameInfoW          = nullptr;
static FnGetIpAddrTable        g_origGetIpAddrTable        = nullptr;
static FnGetIfTable            g_origGetIfTable            = nullptr;
static FnGetIfEntry            g_origGetIfEntry            = nullptr;
static FnIcmpSendEcho          g_origIcmpSendEcho          = nullptr;
static FnIcmpSendEcho2         g_origIcmpSendEcho2         = nullptr;
static FnIcmpSendEcho2Ex       g_origIcmpSendEcho2Ex       = nullptr;
static FnWSAGetOverlappedResult g_origWSAGetOverlappedResult = nullptr;
static FnGetOverlappedResult   g_origGetOverlappedResult   = nullptr;
static FnGetOverlappedResultEx g_origGetOverlappedResultEx = nullptr;
static FnGetQueuedCompletionStatus   g_origGetQueuedCompletionStatus   = nullptr;
static FnGetQueuedCompletionStatusEx g_origGetQueuedCompletionStatusEx = nullptr;
static LPFN_GETACCEPTEXSOCKADDRS     g_origGetAcceptExSockaddrsExport  = nullptr; // mswsock's export

// The virtual address a sockaddr / raw IPv4 buffer names, or 0.
static uint32_t VirtualAddressOf(const sockaddr* sa, int len)
{
    if (!sa || len < static_cast<int>(sizeof(sockaddr_in)) || sa->sa_family != AF_INET || !RelayActive())
        return 0;

    const uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr);
    return RelayIsVirtualAddress(ip) && ip != 0xFFFFFFFFu ? ip : 0;
}

// The host name a reverse lookup of a virtual address answers with: the
// player's name, which is what a game listing its peers would want to show.
static bool VirtualHostName(uint32_t ip, std::string& out)
{
    std::wstring name;
    if (!RelayNameForAddress(ip, name))
        return false;

    out = WideToAnsi(name);
    if (out.empty())
    {
        char buf[16];
        sprintf_s(buf, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        out = buf;
    }
    return true;
}

static hostent* FillThreadHostent(const char* name, uint32_t ipHost)
{
    VirtualHostent& v = t_virtualHost;

    strncpy_s(v.name, name, _TRUNCATE);
    v.address.s_addr = htonl(ipHost);
    v.aliases[0]     = nullptr;
    v.addresses[0]   = reinterpret_cast<char*>(&v.address);
    v.addresses[1]   = nullptr;

    v.he.h_name      = v.name;
    v.he.h_aliases   = v.aliases;
    v.he.h_addrtype  = AF_INET;
    v.he.h_length    = 4;
    v.he.h_addr_list = v.addresses;
    return &v.he;
}

// --- WSAAsyncGetHostBy* ----------------------------------------------------------

// Pack a one-address hostent into the caller's buffer the way the async
// lookups do: the struct first, then everything it points at. Returns the
// bytes used, or the negated bytes needed when the buffer is too small.
static int PackHostent(char* buf, int buflen, const char* name, uint32_t ipHost)
{
    struct Layout
    {
        hostent he;
        char*   aliases[1];
        char*   addresses[2];
        in_addr address;
    };

    const size_t nameLength = strlen(name) + 1;
    const size_t need       = sizeof(Layout) + nameLength;

    if (!buf || buflen < 0 || static_cast<size_t>(buflen) < need)
        return -static_cast<int>(need);

    auto* l = reinterpret_cast<Layout*>(buf);
    char* nameCopy = buf + sizeof(Layout);
    memcpy(nameCopy, name, nameLength);

    l->address.s_addr = htonl(ipHost);
    l->aliases[0]     = nullptr;
    l->addresses[0]   = reinterpret_cast<char*>(&l->address);
    l->addresses[1]   = nullptr;
    l->he.h_name      = nameCopy;
    l->he.h_aliases   = l->aliases;
    l->he.h_addrtype  = AF_INET;
    l->he.h_length    = 4;
    l->he.h_addr_list = l->addresses;
    return static_cast<int>(need);
}

// Answer an async lookup ourselves: post the completion message the game is
// waiting for, under a handle of our own making.
static HANDLE AnswerAsyncLookup(HWND hWnd, u_int wMsg, char* buf, int buflen, const char* name, uint32_t ipHost)
{
    static std::atomic<uintptr_t> nextHandle{0x4C430000};

    const int    packed = PackHostent(buf, buflen, name, ipHost);
    const HANDLE handle = reinterpret_cast<HANDLE>(nextHandle.fetch_add(1));

    const LPARAM reply = packed > 0
        ? WSAMAKEASYNCREPLY(packed, 0)
        : WSAMAKEASYNCREPLY(-packed, WSAENOBUFS);

    PostMessageA(hWnd, wMsg, reinterpret_cast<WPARAM>(handle), reply);
    return handle;
}

static HANDLE WSAAPI HookWSAAsyncGetHostByName(HWND hWnd, u_int wMsg, const char* name, char* buf, int buflen)
{
    if (name && name[0] && WantsVirtualLocalAddress(name))
    {
        char host[256]{};
        gethostname(host, sizeof(host));
        return AnswerAsyncLookup(hWnd, wMsg, buf, buflen, host, RelayVirtualIp());
    }

    // DnsRedirects apply here as they do to the synchronous lookups. Winsock
    // copies the name before returning, so a temporary is enough.
    if (name && name[0] && !g_dnsRedirects.empty())
    {
        const std::wstring wname      = AnsiToWide(name);
        const std::wstring redirected = ApplyDnsRedirect(wname);

        if (redirected != wname)
        {
            LogDnsRedirect(wname.c_str(), redirected.c_str());
            return g_origWSAAsyncGetHostByName(hWnd, wMsg, WideToAnsi(redirected).c_str(), buf, buflen);
        }
    }

    return g_origWSAAsyncGetHostByName(hWnd, wMsg, name, buf, buflen);
}

static HANDLE WSAAPI HookWSAAsyncGetHostByAddr(HWND hWnd, u_int wMsg, const char* addr, int len, int type,
    char* buf, int buflen)
{
    if (Translating() && addr && type == AF_INET && len >= 4 && RelayActive())
    {
        const uint32_t ip = ntohl(*reinterpret_cast<const u_long*>(addr));
        std::string name;

        if (RelayIsVirtualAddress(ip) && VirtualHostName(ip, name))
            return AnswerAsyncLookup(hWnd, wMsg, buf, buflen, name.c_str(), ip);
    }

    return g_origWSAAsyncGetHostByAddr(hWnd, wMsg, addr, len, type, buf, buflen);
}

// --- reverse lookups ----------------------------------------------------------------

static hostent* WSAAPI HookGetHostByAddr(const char* addr, int len, int type)
{
    if (Translating() && addr && type == AF_INET && len >= 4 && RelayActive())
    {
        const uint32_t ip = ntohl(*reinterpret_cast<const u_long*>(addr));
        std::string name;

        if (RelayIsVirtualAddress(ip) && VirtualHostName(ip, name))
            return FillThreadHostent(name.c_str(), ip);
    }

    return g_origGetHostByAddr(addr, len, type);
}

// getnameinfo for a virtual address: the service half is still the real
// function's business; the host half is answered here. NI_NUMERICHOST asks
// for no name at all, so it is left alone.
static int WSAAPI HookGetNameInfo(const SOCKADDR* sa, socklen_t salen, PCHAR host, DWORD hostlen,
    PCHAR serv, DWORD servlen, INT flags)
{
    std::string name;
    const uint32_t ip = Translating() ? VirtualAddressOf(sa, salen) : 0;

    if (!ip || !host || !hostlen || (flags & NI_NUMERICHOST) || !VirtualHostName(ip, name))
        return g_origGetNameInfo(sa, salen, host, hostlen, serv, servlen, flags);

    if (serv && servlen)
    {
        const int r = g_origGetNameInfo(sa, salen, nullptr, 0, serv, servlen, flags);
        if (r != 0)
            return r;
    }

    if (name.size() + 1 > hostlen)
    {
        WSASetLastError(WSAEFAULT);
        return EAI_FAIL;
    }

    memcpy(host, name.c_str(), name.size() + 1);
    return 0;
}

static int WSAAPI HookGetNameInfoW(const SOCKADDR* sa, socklen_t salen, PWCHAR host, DWORD hostlen,
    PWCHAR serv, DWORD servlen, INT flags)
{
    std::string name;
    const uint32_t ip = Translating() ? VirtualAddressOf(sa, salen) : 0;

    if (!ip || !host || !hostlen || (flags & NI_NUMERICHOST) || !VirtualHostName(ip, name))
        return g_origGetNameInfoW(sa, salen, host, hostlen, serv, servlen, flags);

    if (serv && servlen)
    {
        const int r = g_origGetNameInfoW(sa, salen, nullptr, 0, serv, servlen, flags);
        if (r != 0)
            return r;
    }

    const std::wstring wide = AnsiToWide(name.c_str());
    if (wide.size() + 1 > hostlen)
    {
        WSASetLastError(WSAEFAULT);
        return EAI_FAIL;
    }

    memcpy(host, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
    return 0;
}

// --- ICMP echo ---------------------------------------------------------------------
//
// ICMP is not relayed, so an echo to a virtual address is answered here: from
// a member of the network, with twice our measured round trip to the relay
// server (ours plus, presumably, theirs); from nobody, with a timeout. Server
// browsers ping every server they list, and some drop the ones that never
// answer. Only the synchronous form is answered -- an echo with an event or an
// APC routine is passed through, because IcmpParseReplies would then have to
// understand a reply buffer we made up.

static DWORD AnswerEcho(IPAddr destination, LPVOID request, WORD requestSize, LPVOID reply, DWORD replySize)
{
    const uint32_t ip = ntohl(destination);

    if (!RelayAddressReachable(ip))
    {
        SetLastError(IP_REQ_TIMED_OUT);
        return 0;
    }

    // What the documentation asks callers to provide: room for the reply, the
    // echoed data, and an ICMP error message.
    const DWORD need = sizeof(ICMP_ECHO_REPLY) + requestSize + 8;
    if (!reply || replySize < need)
    {
        SetLastError(IP_BUF_TOO_SMALL);
        return 0;
    }

    auto* r = static_cast<ICMP_ECHO_REPLY*>(reply);
    ZeroMemory(r, sizeof(*r));

    r->Address       = destination;
    r->Status        = IP_SUCCESS;
    r->RoundTripTime = (ip == RelayVirtualIp()) ? 0 : 2 * RelayRoundTripMs();
    r->DataSize      = requestSize;
    r->Data          = static_cast<BYTE*>(reply) + sizeof(ICMP_ECHO_REPLY);
    r->Options.Ttl   = 128;

    if (requestSize && request)
        memcpy(r->Data, request, requestSize);

    return 1;
}

static bool EchoToVirtual(IPAddr destination)
{
    if (!Translating() || !RelayActive())
        return false;

    const uint32_t ip = ntohl(destination);
    return RelayIsVirtualAddress(ip) && ip != 0xFFFFFFFFu;
}

static DWORD WINAPI HookIcmpSendEcho(HANDLE icmp, IPAddr destination, LPVOID request, WORD requestSize,
    PIP_OPTION_INFORMATION options, LPVOID reply, DWORD replySize, DWORD timeout)
{
    if (EchoToVirtual(destination))
        return AnswerEcho(destination, request, requestSize, reply, replySize);

    return g_origIcmpSendEcho(icmp, destination, request, requestSize, options, reply, replySize, timeout);
}

static DWORD WINAPI HookIcmpSendEcho2(HANDLE icmp, HANDLE event, PVOID apc, PVOID apcContext,
    IPAddr destination, LPVOID request, WORD requestSize, PIP_OPTION_INFORMATION options,
    LPVOID reply, DWORD replySize, DWORD timeout)
{
    if (!event && !apc && EchoToVirtual(destination))
        return AnswerEcho(destination, request, requestSize, reply, replySize);

    return g_origIcmpSendEcho2(icmp, event, apc, apcContext, destination, request, requestSize,
        options, reply, replySize, timeout);
}

static DWORD WINAPI HookIcmpSendEcho2Ex(HANDLE icmp, HANDLE event, PVOID apc, PVOID apcContext,
    IPAddr source, IPAddr destination, LPVOID request, WORD requestSize, PIP_OPTION_INFORMATION options,
    LPVOID reply, DWORD replySize, DWORD timeout)
{
    if (!event && !apc && EchoToVirtual(destination))
        return AnswerEcho(destination, request, requestSize, reply, replySize);

    return g_origIcmpSendEcho2Ex(icmp, event, apc, apcContext, source, destination, request, requestSize,
        options, reply, replySize, timeout);
}

// --- overlapped completion ------------------------------------------------------------

static BOOL WSAAPI HookWSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED ov, LPDWORD transferred,
    BOOL wait, LPDWORD flags)
{
    const BOOL ok = g_origWSAGetOverlappedResult(s, ov, transferred, wait, flags);
    if (ok)
    {
        const DWORD error = GetLastError();
        CompleteReceive(ov);
        SetLastError(error);
    }
    return ok;
}

static BOOL WINAPI HookGetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD transferred, BOOL wait)
{
    const BOOL ok = g_origGetOverlappedResult(h, ov, transferred, wait);
    if (ok && g_pendingCount.load())
    {
        const DWORD error = GetLastError();
        CompleteReceive(ov);
        SetLastError(error);
    }
    return ok;
}

static BOOL WINAPI HookGetOverlappedResultEx(HANDLE h, LPOVERLAPPED ov, LPDWORD transferred,
    DWORD timeout, BOOL alertable)
{
    const BOOL ok = g_origGetOverlappedResultEx(h, ov, transferred, timeout, alertable);
    if (ok && g_pendingCount.load())
    {
        const DWORD error = GetLastError();
        CompleteReceive(ov);
        SetLastError(error);
    }
    return ok;
}

static BOOL WINAPI HookGetQueuedCompletionStatus(HANDLE port, LPDWORD transferred, PULONG_PTR key,
    LPOVERLAPPED* ov, DWORD timeout)
{
    const BOOL ok = g_origGetQueuedCompletionStatus(port, transferred, key, ov, timeout);

    // A failed dequeue can still hand back a packet -- a receive that failed --
    // but there is no address to translate in one of those.
    if (ok && ov && *ov && g_pendingCount.load())
    {
        const DWORD error = GetLastError();
        CompleteReceive(*ov);
        SetLastError(error);
    }
    return ok;
}

static BOOL WINAPI HookGetQueuedCompletionStatusEx(HANDLE port, LPOVERLAPPED_ENTRY entries, ULONG count,
    PULONG removed, DWORD timeout, BOOL alertable)
{
    const BOOL ok = g_origGetQueuedCompletionStatusEx(port, entries, count, removed, timeout, alertable);

    if (ok && entries && removed && g_pendingCount.load())
    {
        const DWORD error = GetLastError();
        for (ULONG i = 0; i < *removed; ++i)
            CompleteReceive(entries[i].lpOverlapped);
        SetLastError(error);
    }
    return ok;
}

// mswsock's own GetAcceptExSockaddrs export, for games that import it rather
// than asking WSAIoctl. Mapping is idempotent, so a call that comes in through
// the WSAIoctl wrapper as well does no harm.
static void PASCAL HookGetAcceptExSockaddrs(PVOID buffer, DWORD receiveLength,
    DWORD localLength, DWORD remoteLength, sockaddr** local, LPINT localLen,
    sockaddr** remote, LPINT remoteLen)
{
    g_origGetAcceptExSockaddrsExport(buffer, receiveLength, localLength, remoteLength,
        local, localLen, remote, remoteLen);

    if (Translating() && remote && *remote && remoteLen)
    {
        NetHookScope scope;
        RelayMapStreamPeer(INVALID_SOCKET, *remote, *remoteLen);
    }
}

// ---------------------------------------------------------------------------
// The virtual adapter — the relay's address, shown to the game as a network
// adapter of its own and placed first, where a game that takes the first
// adapter will find it. Real adapters follow, filtered as usual.
// ---------------------------------------------------------------------------

static bool ShowVirtualAdapter()
{
    return Translating() && RelayWaitForAddress();
}

static void Ipv4Text(uint32_t ipHost, char* out, size_t size)
{
    sprintf_s(out, size, "%u.%u.%u.%u",
        (ipHost >> 24) & 0xFF, (ipHost >> 16) & 0xFF, (ipHost >> 8) & 0xFF, ipHost & 0xFF);
}

static void VirtualMac(BYTE mac[6])
{
    const uint32_t ip = RelayVirtualIp();
    mac[0] = 0x02;
    mac[1] = 0x00;
    mac[2] = static_cast<BYTE>(ip >> 24);
    mac[3] = static_cast<BYTE>(ip >> 16);
    mac[4] = static_cast<BYTE>(ip >> 8);
    mac[5] = static_cast<BYTE>(ip);
}

// The interface index the virtual adapter reports everywhere -- adapters, the
// IP address table, the interface table -- so they agree with each other.
static constexpr DWORD kVirtualIfIndex = 0x4C43;

static const char    kVirtualAdapterName[]   = "{4C414E43-4F4D-4D41-4E44-52454C415900}";
static const wchar_t kVirtualAdapterFriendly[] = L"LANCommander Relay";

static void FillVirtualAdapterInfo(IP_ADAPTER_INFO* info)
{
    ZeroMemory(info, sizeof(*info));

    strcpy_s(info->AdapterName, kVirtualAdapterName);
    strcpy_s(info->Description, "LANCommander Relay");
    info->AddressLength = 6;
    VirtualMac(info->Address);
    info->Index = kVirtualIfIndex;
    info->Type  = MIB_IF_TYPE_ETHERNET;

    Ipv4Text(RelayVirtualIp(), info->IpAddressList.IpAddress.String, sizeof(info->IpAddressList.IpAddress.String));
    Ipv4Text(RelayNetmask(),   info->IpAddressList.IpMask.String,    sizeof(info->IpAddressList.IpMask.String));
    strcpy_s(info->GatewayList.IpAddress.String, "0.0.0.0");
    strcpy_s(info->GatewayList.IpMask.String,    "0.0.0.0");
}

// Everything the virtual IP_ADAPTER_ADDRESSES points at, laid out as one block
// at the front of the caller's buffer.
struct VirtualAddressesBlock
{
    IP_ADAPTER_ADDRESSES         adapter;
    IP_ADAPTER_UNICAST_ADDRESS   unicast;
    sockaddr_in                  address;
    char                         name[64];
    wchar_t                      friendlyName[32];
    wchar_t                      dnsSuffix[1];
};

static void FillVirtualAdapterAddresses(VirtualAddressesBlock* block, ULONG flags)
{
    ZeroMemory(block, sizeof(*block));

    block->address.sin_family      = AF_INET;
    block->address.sin_addr.s_addr = htonl(RelayVirtualIp());

    strcpy_s(block->name, kVirtualAdapterName);
    wcscpy_s(block->friendlyName, kVirtualAdapterFriendly);

    IP_ADAPTER_UNICAST_ADDRESS& u = block->unicast;
    u.Length                     = sizeof(u);
    u.Address.lpSockaddr         = reinterpret_cast<sockaddr*>(&block->address);
    u.Address.iSockaddrLength    = sizeof(block->address);
    u.PrefixOrigin               = IpPrefixOriginManual;
    u.SuffixOrigin               = IpSuffixOriginManual;
    u.DadState                   = IpDadStatePreferred;
    u.ValidLifetime              = 0xFFFFFFFF;
    u.PreferredLifetime          = 0xFFFFFFFF;
    u.LeaseLifetime              = 0xFFFFFFFF;
#if (_WIN32_WINNT >= 0x0600)
    UINT8 prefix = 0;
    for (uint32_t m = RelayNetmask(); m & 0x80000000u; m <<= 1)
        ++prefix;
    u.OnLinkPrefixLength = prefix;
#endif

    IP_ADAPTER_ADDRESSES& a = block->adapter;
    a.Length                 = sizeof(a);
    a.IfIndex                = kVirtualIfIndex;
    a.AdapterName            = block->name;
    a.FirstUnicastAddress    = (flags & GAA_FLAG_SKIP_UNICAST) ? nullptr : &u;
    a.DnsSuffix              = block->dnsSuffix;
    a.Description            = block->friendlyName;
    a.FriendlyName           = block->friendlyName;
    VirtualMac(a.PhysicalAddress);
    a.PhysicalAddressLength  = 6;
    a.Flags                  = IP_ADAPTER_IPV4_ENABLED;
    a.Mtu                    = 1400;
    a.IfType                 = IF_TYPE_ETHERNET_CSMACD;
    a.OperStatus             = IfOperStatusUp;
}

// Prepend the virtual adapter to an INTERFACE_INFO array in place, when there
// is room. Returns false when the caller's buffer is too small.
static bool PrependVirtualInterface(INTERFACE_INFO* array, DWORD capacity, DWORD& count)
{
    if (count + 1 > capacity)
        return false;

    memmove(array + 1, array, count * sizeof(INTERFACE_INFO));

    INTERFACE_INFO& v = array[0];
    ZeroMemory(&v, sizeof(v));
    v.iiFlags = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
    v.iiAddress.AddressIn.sin_family           = AF_INET;
    v.iiAddress.AddressIn.sin_addr.s_addr      = htonl(RelayVirtualIp());
    v.iiNetmask.AddressIn.sin_family           = AF_INET;
    v.iiNetmask.AddressIn.sin_addr.s_addr      = htonl(RelayNetmask());
    v.iiBroadcastAddress.AddressIn.sin_family  = AF_INET;
    v.iiBroadcastAddress.AddressIn.sin_addr.s_addr = htonl(RelayVirtualIp() | ~RelayNetmask());

    ++count;
    return true;
}

// --- the IP address and interface tables -------------------------------------------
//
// GetIpAddrTable is how a good many games -- and Java's NetworkInterface --
// find their own address and the broadcast address to search on;
// GetIfTable/GetIfEntry are what an address's dwIndex is looked up in. The
// virtual interface goes first in both, under the same index the adapter
// enumeration reports, so the three agree.

// Call a MIB table getter into a buffer of our own, retrying while the table
// grows between the size query and the read. Fills tmp; returns the result.
template <typename Table, typename Fn>
static DWORD ReadTable(Fn real, std::vector<BYTE>& tmp, BOOL order)
{
    ULONG size = 0;
    DWORD r = real(nullptr, &size, order);

    for (int attempt = 0; attempt < 4 && r == ERROR_INSUFFICIENT_BUFFER; ++attempt)
    {
        tmp.resize(size);
        r = real(reinterpret_cast<Table*>(tmp.data()), &size, order);
    }

    return r;
}

// Prepend one row to a dwNumEntries + table[] MIB table in the caller's buffer.
template <typename Table, typename Row, typename Fn>
static DWORD PrependTableRow(Fn real, Table* buffer, PULONG size, BOOL order, const Row& row)
{
    if (!size)
        return ERROR_INVALID_PARAMETER;

    std::vector<BYTE> tmp;
    const DWORD r = ReadTable<Table>(real, tmp, order);

    DWORD count = 0;
    if (r == NO_ERROR && !tmp.empty())
        count = reinterpret_cast<Table*>(tmp.data())->dwNumEntries;
    else if (r != NO_ERROR && r != ERROR_NO_DATA)
        return r;

    const ULONG need = static_cast<ULONG>(offsetof(Table, table) + (count + 1) * sizeof(Row));

    if (!buffer || *size < need)
    {
        *size = need;
        return ERROR_INSUFFICIENT_BUFFER;
    }

    buffer->dwNumEntries = count + 1;
    buffer->table[0]     = row;
    if (count)
        memcpy(&buffer->table[1], reinterpret_cast<Table*>(tmp.data())->table, count * sizeof(Row));

    return NO_ERROR;
}

static void FillVirtualIfRow(MIB_IFROW& row)
{
    ZeroMemory(&row, sizeof(row));

    wcsncpy_s(row.wszName, kVirtualAdapterFriendly, _TRUNCATE);
    row.dwIndex       = kVirtualIfIndex;
    row.dwType        = IF_TYPE_ETHERNET_CSMACD;
    row.dwMtu         = 1400;
    row.dwSpeed       = 1000000000;
    row.dwPhysAddrLen = 6;
    VirtualMac(row.bPhysAddr);
    row.dwAdminStatus = 1;
    row.dwOperStatus  = IF_OPER_STATUS_OPERATIONAL;

    const char description[] = "LANCommander Relay";
    memcpy(row.bDescr, description, sizeof(description));
    row.dwDescrLen = sizeof(description);
}

static DWORD WINAPI HookGetIpAddrTable(PMIB_IPADDRTABLE table, PULONG size, BOOL order)
{
    if (!ShowVirtualAdapter())
        return g_origGetIpAddrTable(table, size, order);

    MIB_IPADDRROW row{};
    row.dwAddr      = htonl(RelayVirtualIp());
    row.dwIndex     = kVirtualIfIndex;
    row.dwMask      = htonl(RelayNetmask());
    row.dwBCastAddr = 1;       // what Windows itself reports here: the low bit of the broadcast address
    row.dwReasmSize = 0xFFFF;
    row.wType       = MIB_IPADDR_PRIMARY;

    return PrependTableRow(g_origGetIpAddrTable, table, size, order, row);
}

static DWORD WINAPI HookGetIfTable(PMIB_IFTABLE table, PULONG size, BOOL order)
{
    if (!ShowVirtualAdapter())
        return g_origGetIfTable(table, size, order);

    MIB_IFROW row;
    FillVirtualIfRow(row);
    return PrependTableRow(g_origGetIfTable, table, size, order, row);
}

// Routing questions about a virtual address -- "which interface would reach
// this?", asked to pick the local address to advertise -- are answered with
// the virtual interface, which GetIpAddrTable and GetIfEntry then describe.
using FnGetBestInterface   = DWORD(WINAPI*)(IPAddr, PDWORD);
using FnGetBestInterfaceEx = DWORD(WINAPI*)(struct sockaddr*, PDWORD);
using FnGetBestRoute       = DWORD(WINAPI*)(DWORD, DWORD, PMIB_IPFORWARDROW);

static FnGetBestInterface   g_origGetBestInterface   = nullptr;
static FnGetBestInterfaceEx g_origGetBestInterfaceEx = nullptr;
static FnGetBestRoute       g_origGetBestRoute       = nullptr;

static bool RoutesThroughRelay(DWORD destinationNetworkOrder)
{
    const uint32_t ip = ntohl(destinationNetworkOrder);
    return Translating() && RelayActive() && RelayIsVirtualAddress(ip) && ShowVirtualAdapter();
}

static DWORD WINAPI HookGetBestInterface(IPAddr destination, PDWORD index)
{
    if (index && RoutesThroughRelay(destination))
    {
        *index = kVirtualIfIndex;
        return NO_ERROR;
    }
    return g_origGetBestInterface(destination, index);
}

static DWORD WINAPI HookGetBestInterfaceEx(struct sockaddr* destination, PDWORD index)
{
    if (index && destination && destination->sa_family == AF_INET &&
        RoutesThroughRelay(reinterpret_cast<sockaddr_in*>(destination)->sin_addr.s_addr))
    {
        *index = kVirtualIfIndex;
        return NO_ERROR;
    }
    return g_origGetBestInterfaceEx(destination, index);
}

static DWORD WINAPI HookGetBestRoute(DWORD destination, DWORD source, PMIB_IPFORWARDROW route)
{
    if (route && RoutesThroughRelay(destination))
    {
        // An on-link route: the virtual subnet, reached directly.
        ZeroMemory(route, sizeof(*route));
        route->dwForwardMask    = htonl(RelayNetmask());
        route->dwForwardDest    = htonl(RelayVirtualIp() & RelayNetmask());
        route->dwForwardNextHop = destination;
        route->dwForwardIfIndex = kVirtualIfIndex;
        route->dwForwardType    = MIB_IPROUTE_TYPE_DIRECT;
        route->dwForwardProto   = MIB_IPPROTO_LOCAL;
        route->dwForwardMetric1 = 1;
        return NO_ERROR;
    }
    return g_origGetBestRoute(destination, source, route);
}

static DWORD WINAPI HookGetIfEntry(PMIB_IFROW row)
{
    if (row && row->dwIndex == kVirtualIfIndex && ShowVirtualAdapter())
    {
        FillVirtualIfRow(*row);
        return NO_ERROR;
    }

    return g_origGetIfEntry(row);
}

// ---------------------------------------------------------------------------
// Adapter enumeration filtering — iphlpapi (GetAdaptersInfo,
// GetAdaptersAddresses) and ws2_32 (WSAIoctl SIO_GET_INTERFACE_LIST).
//
// The original APIs fill a caller-owned buffer as one contiguous block. We only
// rewrite pointers/compact arrays in place — never free or allocate. For the
// linked-list APIs the head pointer is passed by value and cannot be redirected,
// so when the first node is hidden we promote the first survivor's contents into
// the head slot via a shallow struct copy (embedded/pointed-to sub-structures
// live elsewhere in the same buffer and stay valid).
// ---------------------------------------------------------------------------

// KEEP test for one IP_ADAPTER_INFO node (IPv4 only).
static bool KeepAdapterInfo(const IP_ADAPTER_INFO* p)
{
    for (const IP_ADDR_STRING* a = &p->IpAddressList; a; a = a->Next)
    {
        in_addr v{};
        if (InetPtonA(AF_INET, a->IpAddress.String, &v) == 1 && v.s_addr != 0)
            if (SubnetMatchInAddr(v))
                return true;
    }
    if (MacMatch(p->Address, p->AddressLength))
        return true;
    if (NameMatch(AnsiToWide(p->Description).c_str()))
        return true;
    if (NameMatch(AnsiToWide(p->AdapterName).c_str()))
        return true;
    return false;
}

// KEEP test for one IP_ADAPTER_ADDRESSES node (IPv4 subnet; name/MAC).
static bool KeepAdapterAddresses(const IP_ADAPTER_ADDRESSES* p)
{
    for (const IP_ADAPTER_UNICAST_ADDRESS* u = p->FirstUnicastAddress; u; u = u->Next)
    {
        const SOCKET_ADDRESS& sa = u->Address;
        if (sa.lpSockaddr && sa.lpSockaddr->sa_family == AF_INET)
        {
            const auto* in4 = reinterpret_cast<const sockaddr_in*>(sa.lpSockaddr);
            if (SubnetMatchInAddr(in4->sin_addr))
                return true;
        }
        // AF_INET6 ignored for subnet matching (IPv4 subnets only).
    }
    if (MacMatch(p->PhysicalAddress, p->PhysicalAddressLength))
        return true;
    if (NameMatch(p->FriendlyName))
        return true;
    if (NameMatch(p->Description))
        return true;
    return false;
}

// Drop every adapter the NetworkAdapters filter hides from a linked list and
// return the new head -- null when nothing survives. Nothing is moved: the
// caller decides whether the head slot needs the survivor promoted into it.
static IP_ADAPTER_INFO* FilterAdapterInfoList(IP_ADAPTER_INFO* head)
{
    IP_ADAPTER_INFO* prev = nullptr;
    IP_ADAPTER_INFO* cur  = head;

    while (cur)
    {
        IP_ADAPTER_INFO* next = cur->Next;
        if (KeepAdapterInfo(cur))
        {
            prev = cur;
        }
        else
        {
            if (prev) prev->Next = next;
            else      head = next;
            if (g_logNetwork)
                LogNetworkAccess(L"ADAPTER HIDE", AnsiToWide(cur->Description).c_str(), nullptr);
        }
        cur = next;
    }

    return head;
}

static IP_ADAPTER_ADDRESSES* FilterAdapterAddressesList(IP_ADAPTER_ADDRESSES* head)
{
    IP_ADAPTER_ADDRESSES* prev = nullptr;
    IP_ADAPTER_ADDRESSES* cur  = head;

    while (cur)
    {
        IP_ADAPTER_ADDRESSES* next = cur->Next;
        if (KeepAdapterAddresses(cur))
        {
            prev = cur;
        }
        else
        {
            if (prev) prev->Next = next;
            else      head = next;
            if (g_logNetwork)
                LogNetworkAccess(L"ADAPTER HIDE", cur->FriendlyName ? cur->FriendlyName : L"", nullptr);
        }
        cur = next;
    }

    return head;
}

// GetAdaptersInfo / GetAdaptersAddresses with the virtual adapter in front.
// The real call fills the caller's buffer *after* the space the virtual entry
// takes, so every pointer it writes is already correct, and the virtual entry
// is then written at the start with its Next pointing at the real list. The
// sizes reported on ERROR_BUFFER_OVERFLOW include that space.
static DWORD GetAdaptersInfoWithVirtual(PIP_ADAPTER_INFO buffer, PULONG length)
{
    constexpr ULONG extra = sizeof(IP_ADAPTER_INFO);

    const ULONG given      = *length;
    ULONG       realLength = given > extra ? given - extra : 0;
    auto*       real       = (buffer && given > extra)
        ? reinterpret_cast<PIP_ADAPTER_INFO>(reinterpret_cast<BYTE*>(buffer) + extra)
        : nullptr;

    const DWORD ret = g_origGetAdaptersInfo(real, &realLength);

    if (g_logNetwork)
        LogNetworkAccess(L"ADAPTER ENUM", L"GetAdaptersInfo", nullptr);

    if (ret == ERROR_NO_DATA)
        realLength = 0;
    else if (ret == ERROR_BUFFER_OVERFLOW)
    {
        *length = realLength + extra;
        return ERROR_BUFFER_OVERFLOW;
    }
    else if (ret != ERROR_SUCCESS)
        return ret;

    if (!buffer || given < extra)
    {
        *length = realLength + extra;
        return ERROR_BUFFER_OVERFLOW;
    }

    IP_ADAPTER_INFO* rest = nullptr;
    if (ret == ERROR_SUCCESS && real)
        rest = NaActive() ? FilterAdapterInfoList(real) : real;

    FillVirtualAdapterInfo(buffer);
    buffer->Next = rest;
    return ERROR_SUCCESS;
}

// Rounded up so the real list after it keeps the caller's alignment.
static constexpr ULONG kVirtualAddressesSize =
    static_cast<ULONG>((sizeof(VirtualAddressesBlock) + 15) & ~static_cast<size_t>(15));

static ULONG GetAdaptersAddressesWithVirtual(ULONG family, ULONG flags, PVOID reserved,
    PIP_ADAPTER_ADDRESSES buffer, PULONG length)
{
    constexpr ULONG extra = kVirtualAddressesSize;

    const ULONG given      = *length;
    ULONG       realLength = given > extra ? given - extra : 0;
    auto*       real       = (buffer && given > extra)
        ? reinterpret_cast<PIP_ADAPTER_ADDRESSES>(reinterpret_cast<BYTE*>(buffer) + extra)
        : nullptr;

    const ULONG ret = g_origGetAdaptersAddresses(family, flags, reserved, real, &realLength);

    if (g_logNetwork)
        LogNetworkAccess(L"ADAPTER ENUM", L"GetAdaptersAddresses", nullptr);

    if (ret == ERROR_NO_DATA)
        realLength = 0;
    else if (ret == ERROR_BUFFER_OVERFLOW)
    {
        *length = realLength + extra;
        return ERROR_BUFFER_OVERFLOW;
    }
    else if (ret != ERROR_SUCCESS)
        return ret;

    if (!buffer || given < extra)
    {
        *length = realLength + extra;
        return ERROR_BUFFER_OVERFLOW;
    }

    IP_ADAPTER_ADDRESSES* rest = nullptr;
    if (ret == ERROR_SUCCESS && real)
        rest = NaActive() ? FilterAdapterAddressesList(real) : real;

    auto* block = reinterpret_cast<VirtualAddressesBlock*>(buffer);
    FillVirtualAdapterAddresses(block, flags);
    block->adapter.Next = rest;
    return ERROR_SUCCESS;
}

static DWORD WINAPI HookGetAdaptersInfo(PIP_ADAPTER_INFO pAdapterInfo, PULONG pOutBufLen)
{
    if (pOutBufLen && ShowVirtualAdapter())
        return GetAdaptersInfoWithVirtual(pAdapterInfo, pOutBufLen);

    const DWORD ret = g_origGetAdaptersInfo(pAdapterInfo, pOutBufLen);

    if (g_logNetwork)
        LogNetworkAccess(L"ADAPTER ENUM", L"GetAdaptersInfo", nullptr);

    if (!NaActive() || ret != ERROR_SUCCESS || !pAdapterInfo)
        return ret;

    IP_ADAPTER_INFO* head = FilterAdapterInfoList(pAdapterInfo);

    if (!head)
        return ERROR_NO_DATA;        // all hidden — the API's documented empty result
    if (head != pAdapterInfo)
        *pAdapterInfo = *head;       // promote first survivor into the head slot

    return ret;
}

static ULONG WINAPI HookGetAdaptersAddresses(
    ULONG Family, ULONG Flags, PVOID Reserved,
    PIP_ADAPTER_ADDRESSES pAddresses, PULONG pOutBufLen)
{
    if (pOutBufLen && Family != AF_INET6 && ShowVirtualAdapter())
        return GetAdaptersAddressesWithVirtual(Family, Flags, Reserved, pAddresses, pOutBufLen);

    const ULONG ret = g_origGetAdaptersAddresses(Family, Flags, Reserved, pAddresses, pOutBufLen);

    if (g_logNetwork)
        LogNetworkAccess(L"ADAPTER ENUM", L"GetAdaptersAddresses", nullptr);

    if (!NaActive() || ret != ERROR_SUCCESS || !pAddresses)
        return ret;

    IP_ADAPTER_ADDRESSES* head = FilterAdapterAddressesList(pAddresses);

    if (!head)
        return ERROR_NO_DATA;
    if (head != pAddresses)
        *pAddresses = *head;

    return ret;
}

// True for an IPv4 socket. The mswsock extension pointers are per provider, so
// only the IPv4 TCP provider's are swapped for the relay's wrappers.
static bool IsIpv4Socket(SOCKET s)
{
    WSAPROTOCOL_INFOW info{};
    int len = sizeof(info);
    return getsockopt(s, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&info), &len) == 0 &&
           info.iAddressFamily == AF_INET;
}

// WSAIoctl:
//   SIO_GET_INTERFACE_LIST             compact the INTERFACE_INFO[] array in place
//                                      and put the virtual adapter in front; only on
//                                      a synchronous, successful call
//   SIO_GET_EXTENSION_FUNCTION_POINTER hand out the relay's ConnectEx and
//                                      GetAcceptExSockaddrs wrappers
//   FIONREAD                           discount the IPX packet-type prefix
static int WSAAPI HookWSAIoctl(
    SOCKET s, DWORD dwIoControlCode,
    LPVOID lpvInBuffer, DWORD cbInBuffer,
    LPVOID lpvOutBuffer, DWORD cbOutBuffer,
    LPDWORD lpcbBytesReturned,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    if (t_netHookDepth)
        return g_origWSAIoctl(s, dwIoControlCode, lpvInBuffer, cbInBuffer,
            lpvOutBuffer, cbOutBuffer, lpcbBytesReturned, lpOverlapped, lpCompletionRoutine);

    const int ret = g_origWSAIoctl(s, dwIoControlCode, lpvInBuffer, cbInBuffer,
        lpvOutBuffer, cbOutBuffer, lpcbBytesReturned, lpOverlapped, lpCompletionRoutine);

    if (dwIoControlCode == SIO_GET_INTERFACE_LIST && g_logNetwork)
        LogNetworkAccess(L"ADAPTER ENUM", L"WSAIoctl SIO_GET_INTERFACE_LIST", nullptr);

    if (ret == 0 && Translating())
    {
        NetHookScope scope;

        if (dwIoControlCode == SIO_GET_EXTENSION_FUNCTION_POINTER &&
            lpvInBuffer && cbInBuffer >= sizeof(GUID) &&
            lpvOutBuffer && cbOutBuffer >= sizeof(void*) && IsIpv4Socket(s))
        {
            const GUID& id = *static_cast<const GUID*>(lpvInBuffer);

            if (id == GUID(WSAID_CONNECTEX))
            {
                auto* slot = static_cast<LPFN_CONNECTEX*>(lpvOutBuffer);
                if (*slot && *slot != RelayConnectEx)
                {
                    g_realConnectEx = *slot;
                    *slot = RelayConnectEx;
                }
            }
            else if (id == GUID(WSAID_GETACCEPTEXSOCKADDRS))
            {
                auto* slot = static_cast<LPFN_GETACCEPTEXSOCKADDRS*>(lpvOutBuffer);
                if (*slot && *slot != RelayGetAcceptExSockaddrs)
                {
                    g_realGetAcceptExSockaddrs = *slot;
                    *slot = RelayGetAcceptExSockaddrs;
                }
            }
        }
        else if (dwIoControlCode == FIONREAD && lpvOutBuffer && cbOutBuffer >= sizeof(u_long))
            IpxAdjustReadable(s, FIONREAD, static_cast<u_long*>(lpvOutBuffer));
    }

    if (ret != 0 || dwIoControlCode != SIO_GET_INTERFACE_LIST ||
        lpOverlapped != nullptr || !lpvOutBuffer || !lpcbBytesReturned)
        return ret;

    INTERFACE_INFO* arr = reinterpret_cast<INTERFACE_INFO*>(lpvOutBuffer);
    DWORD count = *lpcbBytesReturned / static_cast<DWORD>(sizeof(INTERFACE_INFO));

    if (NaActive())
    {
        DWORD w = 0;
        for (DWORD r = 0; r < count; ++r)
        {
            const sockaddr_in& a = arr[r].iiAddress.AddressIn;
            if (a.sin_family == AF_INET && SubnetMatchInAddr(a.sin_addr))
            {
                if (w != r) arr[w] = arr[r];
                ++w;
            }
        }
        count = w;
    }

    if (ShowVirtualAdapter() &&
        !PrependVirtualInterface(arr, cbOutBuffer / static_cast<DWORD>(sizeof(INTERFACE_INFO)), count))
    {
        // The caller sized its buffer for the real list; one more entry does
        // not fit, which the real call reports the same way.
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    *lpcbBytesReturned = count * static_cast<DWORD>(sizeof(INTERFACE_INFO));
    return ret;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Install the iphlpapi adapter-enumeration hooks. Safe to call repeatedly
// (MH_ERROR_ALREADY_CREATED is ignored by LogHookInit). MH_ERROR_MODULE_NOT_FOUND
// at startup means iphlpapi.dll wasn't loaded yet — OnLibraryLoaded retries later
// via LateInstallNetworkHooks.
static void InstallAdapterHooks()
{
    LogHookInit(L"iphlpapi.dll", "GetAdaptersInfo",
        MH_CreateHookApi(L"iphlpapi.dll", "GetAdaptersInfo",
            reinterpret_cast<LPVOID>(HookGetAdaptersInfo),
            reinterpret_cast<LPVOID*>(&g_origGetAdaptersInfo)));

    LogHookInit(L"iphlpapi.dll", "GetAdaptersAddresses",
        MH_CreateHookApi(L"iphlpapi.dll", "GetAdaptersAddresses",
            reinterpret_cast<LPVOID>(HookGetAdaptersAddresses),
            reinterpret_cast<LPVOID*>(&g_origGetAdaptersAddresses)));

    if (!RelayHooksWanted())
        return;

    // The relay's virtual interface in the address and interface tables, and
    // echoes to virtual addresses. icmp.dll's exports forward to these.
    struct HookSpec { const char* name; LPVOID detour; LPVOID* original; };

    const HookSpec hooks[] =
    {
        { "GetIpAddrTable",  reinterpret_cast<LPVOID>(HookGetIpAddrTable),  reinterpret_cast<LPVOID*>(&g_origGetIpAddrTable) },
        { "GetIfTable",      reinterpret_cast<LPVOID>(HookGetIfTable),      reinterpret_cast<LPVOID*>(&g_origGetIfTable) },
        { "GetIfEntry",      reinterpret_cast<LPVOID>(HookGetIfEntry),      reinterpret_cast<LPVOID*>(&g_origGetIfEntry) },
        { "GetBestInterface",   reinterpret_cast<LPVOID>(HookGetBestInterface),   reinterpret_cast<LPVOID*>(&g_origGetBestInterface) },
        { "GetBestInterfaceEx", reinterpret_cast<LPVOID>(HookGetBestInterfaceEx), reinterpret_cast<LPVOID*>(&g_origGetBestInterfaceEx) },
        { "GetBestRoute",       reinterpret_cast<LPVOID>(HookGetBestRoute),       reinterpret_cast<LPVOID*>(&g_origGetBestRoute) },
        { "IcmpSendEcho",    reinterpret_cast<LPVOID>(HookIcmpSendEcho),    reinterpret_cast<LPVOID*>(&g_origIcmpSendEcho) },
        { "IcmpSendEcho2",   reinterpret_cast<LPVOID>(HookIcmpSendEcho2),   reinterpret_cast<LPVOID*>(&g_origIcmpSendEcho2) },
        { "IcmpSendEcho2Ex", reinterpret_cast<LPVOID>(HookIcmpSendEcho2Ex), reinterpret_cast<LPVOID*>(&g_origIcmpSendEcho2Ex) },
    };

    for (const HookSpec& h : hooks)
        LogHookInit(L"iphlpapi.dll", h.name, MH_CreateHookApi(L"iphlpapi.dll", h.name, h.detour, h.original));
}

// wsock32's Winsock 1.1 name-space enumeration, which an IPX game may consult
// before it ever creates a socket. Retried from LateInstallNetworkHooks.
static void InstallWsock32ProtocolHooks()
{
    if (!RelayHooksWanted())
        return;

    LogHookInit(L"wsock32", "EnumProtocolsA",
        MH_CreateHookApi(L"wsock32", "EnumProtocolsA",
            reinterpret_cast<LPVOID>(HookEnumProtocolsA),
            reinterpret_cast<LPVOID*>(&g_origEnumProtocolsA)));

    LogHookInit(L"wsock32", "EnumProtocolsW",
        MH_CreateHookApi(L"wsock32", "EnumProtocolsW",
            reinterpret_cast<LPVOID>(HookEnumProtocolsW),
            reinterpret_cast<LPVOID*>(&g_origEnumProtocolsW)));
}

// mswsock's GetAcceptExSockaddrs, for the games that import it directly.
// Retried from LateInstallNetworkHooks when mswsock arrives later.
static void InstallMswsockHooks()
{
    if (!RelayHooksWanted())
        return;

    LogHookInit(L"mswsock", "GetAcceptExSockaddrs",
        MH_CreateHookApi(L"mswsock", "GetAcceptExSockaddrs",
            reinterpret_cast<LPVOID>(HookGetAcceptExSockaddrs),
            reinterpret_cast<LPVOID*>(&g_origGetAcceptExSockaddrsExport)));
}

// The socket-lifecycle hooks the relay and IPX emulation need on top of the
// address-carrying ones every network feature shares.
static void InstallRelayHooks()
{
    struct HookSpec { const char* name; LPVOID detour; LPVOID* original; };

    const HookSpec hooks[] =
    {
        { "socket",            reinterpret_cast<LPVOID>(HookSocket),            reinterpret_cast<LPVOID*>(&g_origSocket) },
        { "WSASocketW",        reinterpret_cast<LPVOID>(HookWSASocketW),        reinterpret_cast<LPVOID*>(&g_origWSASocketW) },
        { "WSASocketA",        reinterpret_cast<LPVOID>(HookWSASocketA),        reinterpret_cast<LPVOID*>(&g_origWSASocketA) },
        { "bind",              reinterpret_cast<LPVOID>(HookBind),              reinterpret_cast<LPVOID*>(&g_origBind) },
        { "closesocket",       reinterpret_cast<LPVOID>(HookCloseSocket),       reinterpret_cast<LPVOID*>(&g_origCloseSocket) },
        { "getsockname",       reinterpret_cast<LPVOID>(HookGetSockName),       reinterpret_cast<LPVOID*>(&g_origGetSockName) },
        { "getpeername",       reinterpret_cast<LPVOID>(HookGetPeerName),       reinterpret_cast<LPVOID*>(&g_origGetPeerName) },
        { "accept",            reinterpret_cast<LPVOID>(HookAccept),            reinterpret_cast<LPVOID*>(&g_origAccept) },
        { "WSAAccept",         reinterpret_cast<LPVOID>(HookWSAAccept),         reinterpret_cast<LPVOID*>(&g_origWSAAccept) },
        { "getsockopt",        reinterpret_cast<LPVOID>(HookGetSockOpt),        reinterpret_cast<LPVOID*>(&g_origGetSockOpt) },
        { "setsockopt",        reinterpret_cast<LPVOID>(HookSetSockOpt),        reinterpret_cast<LPVOID*>(&g_origSetSockOpt) },
        { "ioctlsocket",       reinterpret_cast<LPVOID>(HookIoctlSocket),       reinterpret_cast<LPVOID*>(&g_origIoctlSocket) },
        { "WSAEnumProtocolsW", reinterpret_cast<LPVOID>(HookWSAEnumProtocolsW), reinterpret_cast<LPVOID*>(&g_origWSAEnumProtocolsW) },
        { "WSAEnumProtocolsA", reinterpret_cast<LPVOID>(HookWSAEnumProtocolsA), reinterpret_cast<LPVOID*>(&g_origWSAEnumProtocolsA) },
        { "WSAAsyncGetHostByAddr",  reinterpret_cast<LPVOID>(HookWSAAsyncGetHostByAddr),  reinterpret_cast<LPVOID*>(&g_origWSAAsyncGetHostByAddr) },
        { "gethostbyaddr",          reinterpret_cast<LPVOID>(HookGetHostByAddr),          reinterpret_cast<LPVOID*>(&g_origGetHostByAddr) },
        { "getnameinfo",            reinterpret_cast<LPVOID>(HookGetNameInfo),            reinterpret_cast<LPVOID*>(&g_origGetNameInfo) },
        { "GetNameInfoW",           reinterpret_cast<LPVOID>(HookGetNameInfoW),           reinterpret_cast<LPVOID*>(&g_origGetNameInfoW) },
        { "WSAGetOverlappedResult", reinterpret_cast<LPVOID>(HookWSAGetOverlappedResult), reinterpret_cast<LPVOID*>(&g_origWSAGetOverlappedResult) },
    };

    for (const HookSpec& h : hooks)
        LogHookInit(L"ws2_32", h.name, MH_CreateHookApi(L"ws2_32", h.name, h.detour, h.original));

    // The other ways a game learns an overlapped receive has finished. These
    // live in KernelBase (kernel32 on XP and Vista) and see every overlapped
    // completion in the process, so they do nothing until a relayed receive is
    // actually pending.
    const HookSpec kernel[] =
    {
        { "GetOverlappedResult",         reinterpret_cast<LPVOID>(HookGetOverlappedResult),         reinterpret_cast<LPVOID*>(&g_origGetOverlappedResult) },
        { "GetOverlappedResultEx",       reinterpret_cast<LPVOID>(HookGetOverlappedResultEx),       reinterpret_cast<LPVOID*>(&g_origGetOverlappedResultEx) },
        { "GetQueuedCompletionStatus",   reinterpret_cast<LPVOID>(HookGetQueuedCompletionStatus),   reinterpret_cast<LPVOID*>(&g_origGetQueuedCompletionStatus) },
        { "GetQueuedCompletionStatusEx", reinterpret_cast<LPVOID>(HookGetQueuedCompletionStatusEx), reinterpret_cast<LPVOID*>(&g_origGetQueuedCompletionStatusEx) },
    };

    const bool haveKernelBase = GetModuleHandleW(L"kernelbase.dll") != nullptr;

    for (const HookSpec& h : kernel)
    {
        const wchar_t* module = haveKernelBase ? L"kernelbase" : L"kernel32";
        MH_STATUS status = MH_CreateHookApi(module, h.name, h.detour, h.original);

        if (status == MH_ERROR_FUNCTION_NOT_FOUND && haveKernelBase)
        {
            module = L"kernel32";
            status = MH_CreateHookApi(module, h.name, h.detour, h.original);
        }

        LogHookInit(module, h.name, status);
    }

    InstallWsock32ProtocolHooks();
    InstallMswsockHooks();
}

void InstallNetworkHooks()
{
    if (!NetworkHooksWanted())
    {
        InterposerLog(L"HOOK INIT", L"network hooks skipped (Logging.Network, FastDL.ProbeConnections, DnsRedirects, NetworkAdapters, NetworkRelay all unset)");
        return;
    }

    if (RelayHooksWanted())
        InstallRelayHooks();

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

    LogHookInit(L"ws2_32", "WSAAsyncGetHostByName",
        MH_CreateHookApi(L"ws2_32", "WSAAsyncGetHostByName",
            reinterpret_cast<LPVOID>(HookWSAAsyncGetHostByName),
            reinterpret_cast<LPVOID*>(&g_origWSAAsyncGetHostByName)));

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

    // WSAIoctl — used by SIO_GET_INTERFACE_LIST adapter enumeration.
    LogHookInit(L"ws2_32", "WSAIoctl",
        MH_CreateHookApi(L"ws2_32", "WSAIoctl",
            reinterpret_cast<LPVOID>(HookWSAIoctl),
            reinterpret_cast<LPVOID*>(&g_origWSAIoctl)));

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

    // iphlpapi adapter-enumeration hooks — installed when adapter filtering is
    // active OR network logging is on (to log every enumeration request).
    // iphlpapi.dll may not be loaded yet at inject time; OnLibraryLoaded retries.
    if (AdapterHooksWanted())
        InstallAdapterHooks();
}

void LateInstallNetworkHooks(const wchar_t* moduleName)
{
    if (!NetworkHooksWanted())
        return;

    if (AdapterHooksWanted() && _wcsicmp(moduleName, L"iphlpapi.dll") == 0)
    {
        InstallAdapterHooks();
        MH_EnableHook(MH_ALL_HOOKS);
        return;
    }

    if (_wcsicmp(moduleName, L"mswsock.dll") == 0)
    {
        InstallMswsockHooks();
        MH_EnableHook(MH_ALL_HOOKS);
        return;
    }

    if (_wcsicmp(moduleName, L"wsock32.dll") != 0)
        return;

    InstallWsock32ProtocolHooks();

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

// ---------------------------------------------------------------------------
// Unified adapter enumeration export — see network.h for the contract. Returns
// the same allowed/hidden decision the hooks use (KeepAdapterAddresses) so the
// enumeration and the filtering can never disagree.
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) DWORD InterposerEnumNetworkAdapters(
    InterposerNetworkAdapter* buffer, DWORD capacity, DWORD* outCount)
{
    if (outCount)
        *outCount = 0;

    // Prefer the installed trampoline; otherwise resolve dynamically so this
    // works even when adapter filtering is disabled (no hook installed) without
    // ever statically importing iphlpapi.lib.
    FnGetAdaptersAddresses getAddrs = g_origGetAdaptersAddresses;
    HMODULE iphlp = nullptr;
    if (!getAddrs)
    {
        iphlp = LoadLibraryW(L"iphlpapi.dll");
        if (iphlp)
            getAddrs = reinterpret_cast<FnGetAdaptersAddresses>(
                reinterpret_cast<void*>(GetProcAddress(iphlp, "GetAdaptersAddresses")));
    }
    if (!getAddrs)
    {
        if (iphlp) FreeLibrary(iphlp);
        return 0;
    }

    // Query into a buffer that grows on ERROR_BUFFER_OVERFLOW.
    std::vector<BYTE> buf(16 * 1024);
    ULONG size = static_cast<ULONG>(buf.size());
    ULONG ret = 0;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        size = static_cast<ULONG>(buf.size());
        ret = getAddrs(AF_UNSPEC, 0, nullptr,
            reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &size);
        if (ret == ERROR_BUFFER_OVERFLOW)
        {
            buf.resize(size);
            continue;
        }
        break;
    }

    DWORD total = 0;
    if (ret == ERROR_SUCCESS)
    {
        const auto* head = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buf.data());
        for (const IP_ADAPTER_ADDRESSES* p = head; p; p = p->Next)
        {
            if (total < capacity && buffer)
            {
                InterposerNetworkAdapter& out = buffer[total];
                ZeroMemory(&out, sizeof(out));

                if (p->FriendlyName)
                    wcsncpy_s(out.friendlyName, p->FriendlyName, _TRUNCATE);
                if (p->Description)
                    wcsncpy_s(out.description, p->Description, _TRUNCATE);

                if (p->PhysicalAddressLength == 6)
                    swprintf_s(out.macAddress, L"%02X:%02X:%02X:%02X:%02X:%02X",
                        p->PhysicalAddress[0], p->PhysicalAddress[1], p->PhysicalAddress[2],
                        p->PhysicalAddress[3], p->PhysicalAddress[4], p->PhysicalAddress[5]);

                for (const IP_ADAPTER_UNICAST_ADDRESS* u = p->FirstUnicastAddress; u; u = u->Next)
                {
                    const SOCKET_ADDRESS& sa = u->Address;
                    if (!sa.lpSockaddr) continue;
                    if (sa.lpSockaddr->sa_family == AF_INET && out.ipv4Address[0] == L'\0')
                    {
                        const auto* in4 = reinterpret_cast<const sockaddr_in*>(sa.lpSockaddr);
                        InetNtopW(AF_INET, const_cast<IN_ADDR*>(&in4->sin_addr),
                            out.ipv4Address, ARRAYSIZE(out.ipv4Address));
                    }
                    else if (sa.lpSockaddr->sa_family == AF_INET6 && out.ipv6Address[0] == L'\0')
                    {
                        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa.lpSockaddr);
                        InetNtopW(AF_INET6, const_cast<IN6_ADDR*>(&in6->sin6_addr),
                            out.ipv6Address, ARRAYSIZE(out.ipv6Address));
                    }
                }

                out.allowed = (NaActive() && !KeepAdapterAddresses(p)) ? 0 : 1;
            }
            ++total;
        }
    }

    if (outCount)
        *outCount = (total < capacity) ? total : capacity;
    if (iphlp)
        FreeLibrary(iphlp);
    return total;
}
