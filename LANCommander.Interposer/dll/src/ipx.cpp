#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <wsipx.h>
#include <wsnwlink.h>
#include <nspapi.h>
#include <windows.h>

// mstcpip.h only declares this for a Vista+ _WIN32_WINNT, but XP SP2 already
// honours it.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

// WSAPROTOCOL_INFOA is deprecated in favour of the W form, but the ANSI
// entry points this file answers for hand the game exactly that struct.
#pragma warning(disable: 4996)

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ipx.h"
#include "relay.h"
#include "relay_protocol.h"
#include "config.h"
#include "network.h"

// ---------------------------------------------------------------------------
// IPX emulation.
//
// An emulated IPX socket is a real UDP socket bound to 127.0.0.1, so select,
// WSAAsyncSelect, WSAEventSelect and blocking reads all keep working without
// help. What this file adds is the IPX addressing on top:
//
//   node     02:00:a.b.c.d -- the virtual IPv4 address, locally administered
//   network  the relay network's IPX number, from Welcome
//   socket   an IPX socket number, mapped to the UDP port the socket is
//            really bound to; the relay thread looks it up both ways
//
// Datagrams cross the loopback hop to and from the relay thread's portals with
// a one-byte prefix carrying the IPX packet type, which the send path adds and
// the receive path strips. That is why an overlapped receive cannot be
// emulated -- the prefix would land in the game's buffer after we returned --
// and why FIONREAD is adjusted.
//
// SPX (the connection-oriented half) is not emulated: socket() refuses it with
// WSAEPROTONOSUPPORT, as a machine without IPX would.
// ---------------------------------------------------------------------------

namespace
{

constexpr int      kIpxMaxPayload = 1467;   // what IPX_MAXSIZE reports on Ethernet II
constexpr size_t   kIpxHeader     = 30;
constexpr uint16_t kDynamicFirst  = 0x4000;
constexpr uint16_t kDynamicLast   = 0x7FFF;
constexpr DWORD    kCatalogEntry  = 0x4C430001; // "LC", arbitrary but stable

// {6C1F6B4E-3D2A-4F8B-9C7E-1A2B3C4D5E60}
constexpr GUID kProviderId = { 0x6c1f6b4e, 0x3d2a, 0x4f8b, { 0x9c, 0x7e, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x60 } };

struct IpxSocketInfo
{
    uint16_t     socketNumber = 0;  // host order; 0 = not bound yet
    uint16_t     udpPort      = 0;
    uint8_t      packetType   = 0;
    int          filterType   = -1; // IPX_FILTERPTYPE, -1 = off
    int          dsType       = 0;
    bool         recvHeader   = false;
    bool         extended     = false;
    bool         broadcast    = true;
    bool         connected    = false;
    SOCKADDR_IPX peer{};
};

std::mutex                                g_ipxMutex;
std::unordered_map<SOCKET, IpxSocketInfo> g_sockets;
std::unordered_map<uint16_t, SOCKET>      g_byUdpPort;
std::unordered_map<uint16_t, uint16_t>    g_udpPortBySocket;
uint16_t                                  g_nextDynamic = kDynamicFirst;

thread_local std::vector<char> t_buffer;

int Fail(int error)
{
    WSASetLastError(error);
    return SOCKET_ERROR;
}

void NodeFromVip(uint32_t vip, unsigned char node[6])
{
    node[0] = 0x02;
    node[1] = 0x00;
    node[2] = static_cast<unsigned char>(vip >> 24);
    node[3] = static_cast<unsigned char>(vip >> 16);
    node[4] = static_cast<unsigned char>(vip >> 8);
    node[5] = static_cast<unsigned char>(vip);
}

uint32_t VipFromNode(const char nodeChars[6])
{
    const auto* node = reinterpret_cast<const unsigned char*>(nodeChars);

    bool broadcast = true;
    for (int i = 0; i < 6; ++i)
        if (node[i] != 0xFF)
            broadcast = false;

    if (broadcast)
        return relay::kBroadcast;

    return (static_cast<uint32_t>(node[2]) << 24) | (static_cast<uint32_t>(node[3]) << 16) |
           (static_cast<uint32_t>(node[4]) << 8)  |  static_cast<uint32_t>(node[5]);
}

void FillAddress(SOCKADDR_IPX& out, uint32_t vip, uint16_t socketNumber)
{
    ZeroMemory(&out, sizeof(out));
    out.sa_family = AF_IPX;

    const uint32_t net = htonl(RelayIpxNetwork());
    memcpy(out.sa_netnum, &net, 4);

    if (vip == relay::kBroadcast)
        memset(out.sa_nodenum, 0xFF, 6);
    else
        NodeFromVip(vip, reinterpret_cast<unsigned char*>(out.sa_nodenum));

    out.sa_socket = htons(socketNumber);
}

uint16_t LocalUdpPort(SOCKET s)
{
    sockaddr_in a{};
    int len = sizeof(a);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0)
        return 0;
    return ntohs(a.sin_port);
}

bool SocketNumberInUseLocked(uint16_t number)
{
    return g_udpPortBySocket.count(number) != 0;
}

int BindLocked(SOCKET s, IpxSocketInfo& info, uint16_t requested,
               std::unique_lock<std::mutex>& lock)
{
    uint16_t number = requested;

    if (number == 0)
    {
        for (int tries = 0; tries <= kDynamicLast - kDynamicFirst; ++tries)
        {
            const uint16_t candidate = g_nextDynamic;
            g_nextDynamic = (g_nextDynamic >= kDynamicLast) ? kDynamicFirst : g_nextDynamic + 1;

            if (!SocketNumberInUseLocked(candidate))
            {
                number = candidate;
                break;
            }
        }

        if (number == 0)
            return Fail(WSAEADDRINUSE);
    }
    else if (SocketNumberInUseLocked(number))
        return Fail(WSAEADDRINUSE);

    // Reserve the number before dropping the lock for the real bind, so two
    // threads cannot claim it at once.
    g_udpPortBySocket[number] = 0;
    lock.unlock();

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const int r = bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
    const int error = WSAGetLastError();
    const uint16_t udpPort = (r == 0) ? LocalUdpPort(s) : 0;

    lock.lock();

    if (r != 0 || !udpPort)
    {
        g_udpPortBySocket.erase(number);
        return Fail(r != 0 ? error : WSAEINVAL);
    }

    auto it = g_sockets.find(s);
    if (it == g_sockets.end())
    {
        g_udpPortBySocket.erase(number);
        return Fail(WSAENOTSOCK);
    }

    it->second.socketNumber   = number;
    it->second.udpPort        = udpPort;
    g_udpPortBySocket[number] = udpPort;
    g_byUdpPort[udpPort]      = s;

    if (g_logLevel >= LogLevel::Debug)
    {
        wchar_t text[64];
        swprintf_s(text, L"socket 0x%04X on 127.0.0.1:%u", number, udpPort);
        LogRelayDiag(L"IPX BIND", text, nullptr);
    }

    return 0;
}

int EnsureBound(SOCKET s)
{
    std::unique_lock<std::mutex> lock(g_ipxMutex);

    auto it = g_sockets.find(s);
    if (it == g_sockets.end())
        return Fail(WSAENOTSOCK);

    if (it->second.socketNumber)
        return 0;

    return BindLocked(s, it->second, 0, lock);
}

bool Snapshot(SOCKET s, IpxSocketInfo& out)
{
    std::lock_guard<std::mutex> lk(g_ipxMutex);
    auto it = g_sockets.find(s);
    if (it == g_sockets.end())
        return false;
    out = it->second;
    return true;
}

void PutU16(unsigned char* p, uint16_t v) { p[0] = static_cast<unsigned char>(v >> 8); p[1] = static_cast<unsigned char>(v); }

// The 30-byte header IPX_RECVHDR asks for, in front of the payload.
void BuildHeader(unsigned char* h, size_t payload, uint8_t type,
                 uint32_t dstVip, uint16_t dstSocket, uint32_t srcVip, uint16_t srcSocket)
{
    const uint32_t net = htonl(RelayIpxNetwork());

    PutU16(h + 0, 0xFFFF);                                       // checksum: none
    PutU16(h + 2, static_cast<uint16_t>(kIpxHeader + payload));  // length
    h[4] = 0;                                                    // transport control
    h[5] = type;
    memcpy(h + 6, &net, 4);
    NodeFromVip(dstVip, h + 10);
    PutU16(h + 16, dstSocket);
    memcpy(h + 18, &net, 4);
    NodeFromVip(srcVip, h + 22);
    PutU16(h + 28, srcSocket);
}

template <typename Info, typename Char>
void FillProtocol(Info& p, const Char* name)
{
    ZeroMemory(&p, sizeof(p));
    p.dwServiceFlags1    = XP1_CONNECTIONLESS | XP1_MESSAGE_ORIENTED | XP1_SUPPORT_BROADCAST |
                           XP1_SUPPORT_MULTIPOINT | XP1_IFS_HANDLES;
    p.dwProviderFlags    = PFL_MATCHES_PROTOCOL_ZERO;
    p.ProviderId         = kProviderId;
    p.dwCatalogEntryId   = kCatalogEntry;
    p.ProtocolChain.ChainLen = BASE_PROTOCOL;
    p.iVersion           = 2;
    p.iAddressFamily     = AF_IPX;
    p.iMaxSockAddr       = sizeof(SOCKADDR_IPX);
    p.iMinSockAddr       = sizeof(SOCKADDR_IPX);
    p.iSocketType        = SOCK_DGRAM;
    p.iProtocol          = NSPROTO_IPX;
    p.iProtocolMaxOffset = 255;
    p.iNetworkByteOrder  = BIGENDIAN;
    p.iSecurityScheme    = SECURITY_PROTOCOL_NONE;
    p.dwMessageSize      = kIpxMaxPayload;

    size_t i = 0;
    for (; name[i] && i + 1 < ARRAYSIZE(p.szProtocol); ++i)
        p.szProtocol[i] = name[i];
    p.szProtocol[i] = 0;
}

void FillProtocolW(WSAPROTOCOL_INFOW& p) { FillProtocol(p, L"MSAFD nwlnkipx [IPX]"); }
void FillProtocolA(WSAPROTOCOL_INFOA& p) { FillProtocol(p, "MSAFD nwlnkipx [IPX]"); }

// A zero-terminated lpiProtocols list asks for IPX if it names it; null asks for everything.
bool WantsIpx(const INT* protocols)
{
    if (!protocols)
        return true;

    for (const INT* p = protocols; *p; ++p)
        if (*p == NSPROTO_IPX)
            return true;

    return false;
}

template <typename Info, typename Fn>
int EnumWithIpx(Fn orig, LPINT protocols, Info* buffer, LPDWORD length, void (*fill)(Info&))
{
    if (!length || !IpxWanted() || !WantsIpx(protocols))
        return orig(protocols, buffer, length);

    // What the real catalog has to say, into a buffer of our own.
    DWORD realLength = 0;
    std::vector<BYTE> real;
    int count = orig(protocols, nullptr, &realLength);

    if (count == SOCKET_ERROR && realLength)
    {
        real.resize(realLength);
        count = orig(protocols, reinterpret_cast<Info*>(real.data()), &realLength);
    }

    if (count == SOCKET_ERROR || count < 0)
        count = 0;

    const auto* entries = reinterpret_cast<const Info*>(real.data());

    // A machine that still has a real IPX stack keeps it.
    for (int i = 0; i < count; ++i)
        if (entries[i].iAddressFamily == AF_IPX)
            return orig(protocols, buffer, length);

    const DWORD need = static_cast<DWORD>((count + 1) * sizeof(Info));

    if (!buffer || *length < need)
    {
        *length = need;
        return Fail(WSAENOBUFS);
    }

    if (count)
        memcpy(buffer, entries, count * sizeof(Info));

    fill(buffer[count]);
    return count + 1;
}

// wsock32's PROTOCOL_INFO, the Winsock 1.1 name-space form. A and W differ
// only in the character type of the string the entry points at.
template <typename Char>
struct LegacyProtocolInfo
{
    DWORD dwServiceFlags;
    INT   iAddressFamily;
    INT   iMaxSockAddr;
    INT   iMinSockAddr;
    INT   iSocketType;
    INT   iProtocol;
    DWORD dwMessageSize;
    Char* lpProtocol;
};

template <typename Char>
int LegacyEnum(void* origPtr, LPINT protocols, LPVOID buffer, LPDWORD length)
{
    using Info = LegacyProtocolInfo<Char>;
    auto orig = reinterpret_cast<int (WSAAPI*)(LPINT, LPVOID, LPDWORD)>(origPtr);

    if (!length || !IpxWanted() || !WantsIpx(protocols))
        return orig(protocols, buffer, length);

    DWORD realLength = 16 * 1024;
    std::vector<BYTE> real(realLength);
    int count = orig(protocols, real.data(), &realLength);

    if (count == SOCKET_ERROR && GetLastError() == ERROR_INSUFFICIENT_BUFFER && realLength > real.size())
    {
        real.resize(realLength);
        count = orig(protocols, real.data(), &realLength);
    }

    if (count == SOCKET_ERROR || count < 0)
        count = 0;

    const auto* entries = reinterpret_cast<const Info*>(real.data());

    for (int i = 0; i < count; ++i)
        if (entries[i].iAddressFamily == AF_IPX)
            return orig(protocols, buffer, length);

    // Pack: the entry array first, then every string after it.
    static const Char kName[] = { 'I','P','X',0 };

    std::vector<std::basic_string<Char>> names;
    for (int i = 0; i < count; ++i)
        names.emplace_back(entries[i].lpProtocol ? entries[i].lpProtocol : kName + 3);
    names.emplace_back(kName);

    DWORD need = static_cast<DWORD>((count + 1) * sizeof(Info));
    for (const auto& n : names)
        need += static_cast<DWORD>((n.size() + 1) * sizeof(Char));

    if (!buffer || *length < need)
    {
        *length = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return SOCKET_ERROR;
    }

    auto* out     = static_cast<Info*>(buffer);
    auto* strings = reinterpret_cast<Char*>(out + count + 1);

    for (int i = 0; i <= count; ++i)
    {
        if (i < count)
            out[i] = entries[i];
        else
        {
            ZeroMemory(&out[i], sizeof(Info));
            out[i].dwServiceFlags = XP_CONNECTIONLESS | XP_MESSAGE_ORIENTED |
                                    XP_SUPPORTS_BROADCAST | XP_SUPPORTS_MULTICAST;
            out[i].iAddressFamily = AF_IPX;
            out[i].iMaxSockAddr   = sizeof(SOCKADDR_IPX);
            out[i].iMinSockAddr   = sizeof(SOCKADDR_IPX);
            out[i].iSocketType    = SOCK_DGRAM;
            out[i].iProtocol      = NSPROTO_IPX;
            out[i].dwMessageSize  = kIpxMaxPayload;
        }

        const auto& name = names[static_cast<size_t>(i)];
        memcpy(strings, name.c_str(), (name.size() + 1) * sizeof(Char));
        out[i].lpProtocol = strings;
        strings += name.size() + 1;
    }

    return count + 1;
}

struct CompletionContext
{
    LPWSAOVERLAPPED_COMPLETION_ROUTINE routine;
    DWORD                              bytes;
    LPWSAOVERLAPPED                    overlapped;
};

VOID CALLBACK RunCompletion(ULONG_PTR param)
{
    auto* c = reinterpret_cast<CompletionContext*>(param);
    c->routine(0, c->bytes, c->overlapped, 0);
    delete c;
}

} // namespace

// ---------------------------------------------------------------------------

bool IpxWanted()
{
    return RelayIpxEnabled();
}

bool IpxIsSocket(SOCKET s)
{
    std::lock_guard<std::mutex> lk(g_ipxMutex);
    return g_sockets.count(s) != 0;
}

SOCKET IpxSocket(int af, int type, int protocol, const WSAPROTOCOL_INFOW* protocolInfo,
                 DWORD wsaFlags, bool useWsaSocket, bool& handled)
{
    handled = false;

    if (!IpxWanted())
        return INVALID_SOCKET;

    const bool fromInfo = protocolInfo && (af == FROM_PROTOCOL_INFO || af == 0);
    const int  family   = fromInfo ? protocolInfo->iAddressFamily : af;

    if (family != AF_IPX)
        return INVALID_SOCKET;

    handled = true;

    int socketType = type;
    int proto      = protocol;

    if (protocolInfo)
    {
        if (socketType == FROM_PROTOCOL_INFO || socketType == 0) socketType = protocolInfo->iSocketType;
        if (proto == FROM_PROTOCOL_INFO || proto == 0)           proto      = protocolInfo->iProtocol;
    }

    if (socketType == 0)
        socketType = SOCK_DGRAM;

    if (socketType != SOCK_DGRAM || (proto != 0 && (proto < NSPROTO_IPX || proto > NSPROTO_IPX + 255)))
    {
        LogRelay(L"IPX", L"SPX requested; only IPX datagrams are emulated", nullptr);
        WSASetLastError(WSAEPROTONOSUPPORT);
        return INVALID_SOCKET;
    }

    SOCKET s = useWsaSocket
        ? WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, wsaFlags)
        : socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (s == INVALID_SOCKET)
        return s;

    BOOL  off = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &returned, nullptr, nullptr);

    IpxSocketInfo info{};
    info.packetType = static_cast<uint8_t>(proto ? proto - NSPROTO_IPX : 0);

    {
        std::lock_guard<std::mutex> lk(g_ipxMutex);
        g_sockets[s] = info;
    }

    LogRelay(L"IPX", L"socket created", nullptr);
    return s;
}

int IpxBind(SOCKET s, const sockaddr* name, int namelen)
{
    if (!name || namelen < static_cast<int>(sizeof(SOCKADDR_IPX)))
        return Fail(WSAEFAULT);

    if (name->sa_family != AF_IPX)
        return Fail(WSAEAFNOSUPPORT);

    std::unique_lock<std::mutex> lock(g_ipxMutex);

    auto it = g_sockets.find(s);
    if (it == g_sockets.end())
        return Fail(WSAENOTSOCK);

    if (it->second.socketNumber)
        return Fail(WSAEINVAL);

    const auto* ipx = reinterpret_cast<const SOCKADDR_IPX*>(name);
    return BindLocked(s, it->second, ntohs(ipx->sa_socket), lock);
}

int IpxConnect(SOCKET s, const sockaddr* name, int namelen)
{
    if (!name || namelen < static_cast<int>(sizeof(SOCKADDR_IPX)))
        return Fail(WSAEFAULT);

    if (name->sa_family != AF_IPX)
        return Fail(WSAEAFNOSUPPORT);

    if (EnsureBound(s) != 0)
        return SOCKET_ERROR;

    std::lock_guard<std::mutex> lk(g_ipxMutex);

    auto it = g_sockets.find(s);
    if (it == g_sockets.end())
        return Fail(WSAENOTSOCK);

    it->second.peer      = *reinterpret_cast<const SOCKADDR_IPX*>(name);
    it->second.connected = true;
    return 0;
}

int IpxGetSockName(SOCKET s, sockaddr* name, int* namelen)
{
    if (!name || !namelen || *namelen < static_cast<int>(sizeof(SOCKADDR_IPX)))
        return Fail(WSAEFAULT);

    IpxSocketInfo info{};
    if (!Snapshot(s, info))
        return Fail(WSAENOTSOCK);

    if (!info.socketNumber)
        return Fail(WSAEINVAL);

    RelayWaitForAddress();

    FillAddress(*reinterpret_cast<SOCKADDR_IPX*>(name), RelayVirtualIp(), info.socketNumber);
    *namelen = sizeof(SOCKADDR_IPX);
    return 0;
}

int IpxGetPeerName(SOCKET s, sockaddr* name, int* namelen)
{
    if (!name || !namelen || *namelen < static_cast<int>(sizeof(SOCKADDR_IPX)))
        return Fail(WSAEFAULT);

    IpxSocketInfo info{};
    if (!Snapshot(s, info))
        return Fail(WSAENOTSOCK);

    if (!info.connected)
        return Fail(WSAENOTCONN);

    *reinterpret_cast<SOCKADDR_IPX*>(name) = info.peer;
    *namelen = sizeof(SOCKADDR_IPX);
    return 0;
}

int IpxSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen)
{
    if (len < 0 || (len > 0 && !buf))
        return Fail(WSAEFAULT);

    if (static_cast<size_t>(len) > relay::kMaxDatagram)
        return Fail(WSAEMSGSIZE);

    if (EnsureBound(s) != 0)
        return SOCKET_ERROR;

    IpxSocketInfo info{};
    if (!Snapshot(s, info))
        return Fail(WSAENOTSOCK);

    SOCKADDR_IPX dst{};

    if (to)
    {
        if (tolen < static_cast<int>(sizeof(SOCKADDR_IPX)))
            return Fail(WSAEFAULT);
        if (to->sa_family != AF_IPX)
            return Fail(WSAEAFNOSUPPORT);
        dst = *reinterpret_cast<const SOCKADDR_IPX*>(to);
    }
    else if (info.connected)
        dst = info.peer;
    else
        return Fail(WSAENOTCONN);

    if (!RelayActive() && !RelayWaitForAddress())
        return Fail(WSAENETDOWN);

    sockaddr_in portal{};
    if (!RelayPortalAddress(relay::ProtoIpx, VipFromNode(dst.sa_nodenum), ntohs(dst.sa_socket), portal))
        return Fail(WSAENETUNREACH);

    std::vector<char>& packet = t_buffer;
    packet.resize(static_cast<size_t>(len) + 1);
    packet[0] = static_cast<char>(info.packetType);
    if (len)
        memcpy(packet.data() + 1, buf, static_cast<size_t>(len));

    const int r = sendto(s, packet.data(), len + 1, flags & ~MSG_DONTROUTE,
        reinterpret_cast<const sockaddr*>(&portal), sizeof(portal));

    return r == SOCKET_ERROR ? r : len;
}

int IpxRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen)
{
    if (from && (!fromlen || *fromlen < static_cast<int>(sizeof(SOCKADDR_IPX))))
        return Fail(WSAEFAULT);

    if (len < 0 || (len > 0 && !buf))
        return Fail(WSAEFAULT);

    IpxSocketInfo info{};
    if (!Snapshot(s, info))
        return Fail(WSAENOTSOCK);

    if (!info.socketNumber)
        return Fail(WSAEINVAL);

    std::vector<char>& raw = t_buffer;
    raw.resize(65536);

    for (;;)
    {
        sockaddr_in src{};
        int srcLen = sizeof(src);

        const int n = recvfrom(s, raw.data(), static_cast<int>(raw.size()), flags,
            reinterpret_cast<sockaddr*>(&src), &srcLen);

        if (n == SOCKET_ERROR)
            return SOCKET_ERROR;

        if (n < 1)
            continue; // not one of ours: every relayed packet carries the type byte

        const uint8_t type = static_cast<uint8_t>(raw[0]);

        if (info.filterType >= 0 && type != info.filterType)
        {
            // A peeked packet would be peeked again forever; consume it.
            if (flags & MSG_PEEK)
                recvfrom(s, raw.data(), static_cast<int>(raw.size()), flags & ~MSG_PEEK, nullptr, nullptr);
            continue;
        }

        uint8_t  proto   = 0;
        uint32_t srcVip  = 0;
        uint16_t srcSock = 0;
        RelayPortalPeer(src, proto, srcVip, srcSock);

        const size_t payload = static_cast<size_t>(n - 1);
        const size_t total   = payload + (info.recvHeader ? kIpxHeader : 0);
        size_t       copied  = 0;

        if (info.recvHeader)
        {
            unsigned char header[kIpxHeader];
            BuildHeader(header, payload, type, RelayVirtualIp(), info.socketNumber, srcVip, srcSock);

            copied = std::min<size_t>(kIpxHeader, static_cast<size_t>(len));
            memcpy(buf, header, copied);
        }

        const size_t room = static_cast<size_t>(len) - copied;
        const size_t body = std::min(room, payload);
        if (body)
            memcpy(buf + copied, raw.data() + 1, body);
        copied += body;

        if (from)
        {
            FillAddress(*reinterpret_cast<SOCKADDR_IPX*>(from), srcVip, srcSock);
            *fromlen = sizeof(SOCKADDR_IPX);
        }

        if (total > static_cast<size_t>(len))
            return Fail(WSAEMSGSIZE);

        return static_cast<int>(copied);
    }
}

int IpxWSASendTo(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD sent, DWORD flags,
                 const sockaddr* to, int tolen, LPWSAOVERLAPPED overlapped,
                 LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
    std::string gathered;
    for (DWORD i = 0; i < count; ++i)
        gathered.append(buffers[i].buf, buffers[i].len);

    const int n = IpxSendTo(s, gathered.data(), static_cast<int>(gathered.size()),
        static_cast<int>(flags), to, tolen);

    if (n == SOCKET_ERROR)
        return SOCKET_ERROR;

    if (sent)
        *sent = static_cast<DWORD>(n);

    // A loopback datagram send completes on the spot, so an overlapped request
    // is finished before it returns. The event and the completion routine are
    // honoured; an I/O completion port is not, because no packet is queued.
    if (overlapped)
    {
        overlapped->Internal     = 0;
        overlapped->InternalHigh = static_cast<ULONG_PTR>(n);

        if (completion)
            QueueUserAPC(RunCompletion, GetCurrentThread(),
                reinterpret_cast<ULONG_PTR>(new CompletionContext{ completion, static_cast<DWORD>(n), overlapped }));
        else if (overlapped->hEvent)
            SetEvent(overlapped->hEvent);
    }

    return 0;
}

int IpxWSARecvFrom(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD received, LPDWORD flags,
                   sockaddr* from, LPINT fromlen, LPWSAOVERLAPPED overlapped,
                   LPWSAOVERLAPPED_COMPLETION_ROUTINE /*completion*/)
{
    if (overlapped)
    {
        LogRelay(L"IPX", L"overlapped receive refused; not supported on emulated IPX sockets", nullptr);
        return Fail(WSAEOPNOTSUPP);
    }

    size_t capacity = 0;
    for (DWORD i = 0; i < count; ++i)
        capacity += buffers[i].len;

    std::string gathered(capacity, '\0');

    const int inFlags = flags ? static_cast<int>(*flags) : 0;
    int n = IpxRecvFrom(s, gathered.data(), static_cast<int>(capacity), inFlags, from, fromlen);

    const bool truncated = n == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE;
    if (n == SOCKET_ERROR && !truncated)
        return SOCKET_ERROR;

    if (truncated)
        n = static_cast<int>(capacity);

    size_t offset = 0;
    for (DWORD i = 0; i < count && offset < static_cast<size_t>(n); ++i)
    {
        const size_t take = std::min<size_t>(buffers[i].len, static_cast<size_t>(n) - offset);
        memcpy(buffers[i].buf, gathered.data() + offset, take);
        offset += take;
    }

    if (received)
        *received = static_cast<DWORD>(n);

    if (flags)
        *flags = truncated ? MSG_PARTIAL : 0;

    return truncated ? Fail(WSAEMSGSIZE) : 0;
}

int IpxGetSockOpt(SOCKET s, int level, int optname, char* optval, int* optlen, bool& handled)
{
    handled = false;

    IpxSocketInfo info{};
    if (!Snapshot(s, info))
        return 0;

    auto putInt = [&](int value) -> int
    {
        if (!optval || !optlen || *optlen < static_cast<int>(sizeof(int)))
            return Fail(WSAEFAULT);
        *reinterpret_cast<int*>(optval) = value;
        *optlen = sizeof(int);
        return 0;
    };

    if (level == SOL_SOCKET)
    {
        if (optname == SO_PROTOCOL_INFOW || optname == SO_PROTOCOL_INFOA)
        {
            handled = true;
            const bool wide = optname == SO_PROTOCOL_INFOW;
            const int  need = wide ? sizeof(WSAPROTOCOL_INFOW) : sizeof(WSAPROTOCOL_INFOA);

            if (!optval || !optlen || *optlen < need)
                return Fail(WSAEFAULT);

            if (wide) FillProtocolW(*reinterpret_cast<WSAPROTOCOL_INFOW*>(optval));
            else      FillProtocolA(*reinterpret_cast<WSAPROTOCOL_INFOA*>(optval));

            *optlen = need;
            return 0;
        }

        if (optname == SO_MAX_MSG_SIZE)
        {
            handled = true;
            return putInt(kIpxMaxPayload);
        }

        return 0;
    }

    if (level != NSPROTO_IPX)
        return 0;

    handled = true;

    switch (optname)
    {
    case IPX_PTYPE:            return putInt(info.packetType);
    case IPX_FILTERPTYPE:      return putInt(info.filterType < 0 ? 0 : info.filterType);
    case IPX_DSTYPE:           return putInt(info.dsType);
    case IPX_EXTENDED_ADDRESS: return putInt(info.extended ? TRUE : FALSE);
    case IPX_RECVHDR:          return putInt(info.recvHeader ? TRUE : FALSE);
    case IPX_RECEIVE_BROADCAST:return putInt(info.broadcast ? TRUE : FALSE);
    case IPX_MAXSIZE:          return putInt(kIpxMaxPayload);
    case IPX_MAX_ADAPTER_NUM:  return putInt(1);

    case IPX_ADDRESS:
    {
        if (!optval || !optlen || *optlen < static_cast<int>(sizeof(IPX_ADDRESS_DATA)))
            return Fail(WSAEFAULT);

        auto* data = reinterpret_cast<IPX_ADDRESS_DATA*>(optval);
        if (data->adapternum != 0)
            return Fail(WSAEINVAL);

        RelayWaitForAddress();

        const uint32_t net = htonl(RelayIpxNetwork());
        memcpy(data->netnum, &net, 4);
        NodeFromVip(RelayVirtualIp(), data->nodenum);
        data->wan       = FALSE;
        data->status    = RelayActive() ? TRUE : FALSE;
        data->maxpkt    = kIpxMaxPayload;
        data->linkspeed = 100000; // in 100 bps units: 10 Mbit, the classic Ethernet IPX ran on
        *optlen = sizeof(IPX_ADDRESS_DATA);
        return 0;
    }

    case IPX_GETNETINFO:
    case IPX_GETNETINFO_NORIP:
    {
        if (!optval || !optlen || *optlen < static_cast<int>(sizeof(IPX_NETNUM_DATA)))
            return Fail(WSAEFAULT);

        // Every network is one hop away: the relay is the whole internetwork.
        auto* data = reinterpret_cast<IPX_NETNUM_DATA*>(optval);
        data->hopcount = 1;
        data->netdelay = 1;
        data->cardnum  = 0;
        memset(data->router, 0xFF, sizeof(data->router));
        *optlen = sizeof(IPX_NETNUM_DATA);
        return 0;
    }

    default:
        return Fail(WSAENOPROTOOPT);
    }
}

int IpxSetSockOpt(SOCKET s, int level, int optname, const char* optval, int optlen, bool& handled)
{
    handled = false;

    if (level != NSPROTO_IPX || !IpxIsSocket(s))
        return 0;

    handled = true;

    int value = 0;
    if (optval && optlen >= static_cast<int>(sizeof(int)))
        value = *reinterpret_cast<const int*>(optval);
    else if (optval && optlen >= 1)
        value = static_cast<unsigned char>(optval[0]);

    std::lock_guard<std::mutex> lk(g_ipxMutex);

    auto it = g_sockets.find(s);
    if (it == g_sockets.end())
        return Fail(WSAENOTSOCK);

    IpxSocketInfo& info = it->second;

    switch (optname)
    {
    case IPX_PTYPE:             info.packetType = static_cast<uint8_t>(value); break;
    case IPX_FILTERPTYPE:       info.filterType = value & 0xFF;                break;
    case IPX_STOPFILTERPTYPE:   info.filterType = -1;                          break;
    case IPX_DSTYPE:            info.dsType     = value;                       break;
    case IPX_EXTENDED_ADDRESS:  info.extended   = value != 0;                  break;
    case IPX_RECVHDR:           info.recvHeader = value != 0;                  break;
    case IPX_RECEIVE_BROADCAST: info.broadcast  = value != 0;                  break;
    default:
        // Accepted and ignored: an option a real stack would honour but that
        // has no meaning here should not make the game give up on IPX.
        break;
    }

    return 0;
}

void IpxAdjustReadable(SOCKET s, long cmd, u_long* argp)
{
    if (cmd == FIONREAD && argp && *argp > 0 && IpxIsSocket(s))
        --*argp;
}

void IpxOnSocketClosed(SOCKET s)
{
    std::lock_guard<std::mutex> lk(g_ipxMutex);

    auto it = g_sockets.find(s);
    if (it == g_sockets.end())
        return;

    if (it->second.socketNumber)
        g_udpPortBySocket.erase(it->second.socketNumber);
    if (it->second.udpPort)
        g_byUdpPort.erase(it->second.udpPort);

    g_sockets.erase(it);
}

uint16_t IpxSocketNumberForUdpPort(uint16_t udpPort)
{
    std::lock_guard<std::mutex> lk(g_ipxMutex);

    auto it = g_byUdpPort.find(udpPort);
    if (it == g_byUdpPort.end())
        return 0;

    auto info = g_sockets.find(it->second);
    return info == g_sockets.end() ? 0 : info->second.socketNumber;
}

uint16_t IpxUdpPortForSocketNumber(uint16_t socketNumber)
{
    std::lock_guard<std::mutex> lk(g_ipxMutex);

    auto it = g_udpPortBySocket.find(socketNumber);
    return it == g_udpPortBySocket.end() ? 0 : it->second;
}

int IpxEnumProtocolsW(FnWSAEnumProtocolsW orig, LPINT protocols, LPWSAPROTOCOL_INFOW buffer, LPDWORD length)
{
    return EnumWithIpx<WSAPROTOCOL_INFOW>(orig, protocols, buffer, length, FillProtocolW);
}

int IpxEnumProtocolsA(FnWSAEnumProtocolsA orig, LPINT protocols, LPWSAPROTOCOL_INFOA buffer, LPDWORD length)
{
    return EnumWithIpx<WSAPROTOCOL_INFOA>(orig, protocols, buffer, length, FillProtocolA);
}

int IpxLegacyEnumProtocols(void* orig, bool wide, LPINT protocols, LPVOID buffer, LPDWORD length)
{
    return wide ? LegacyEnum<wchar_t>(orig, protocols, buffer, length)
                : LegacyEnum<char>(orig, protocols, buffer, length);
}
