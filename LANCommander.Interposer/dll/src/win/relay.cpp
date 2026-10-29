// Must precede every Winsock include in this translation unit: the relay thread
// selects over one socket per remote endpoint the game talks to, and the
// default of 64 would cap a game at a handful of peers.
#define FD_SETSIZE 1024

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>

// mstcpip.h only declares this for a Vista+ _WIN32_WINNT, but XP SP2 already
// honours it.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "relay.h"
#include "instance.h"
#include "relay_protocol.h"
#include "callbacks.h"
#include "config.h"
#include "ipx.h"
#include "network.h"

// ---------------------------------------------------------------------------
// Network relay client.
//
// One background thread owns the connection to the relay server: a TCP
// connection for control messages and tunnelled game streams, and a UDP socket
// for game datagrams (which fall back to the TCP connection until the server
// confirms it can reach us over UDP).
//
// The game never talks to that thread directly. Every remote endpoint it sends
// to -- a virtual address and port, or the broadcast address and a port -- is
// stood in for by a "portal": a UDP socket on 127.0.0.1 owned by the relay
// thread. The send hooks rewrite the destination to the portal's address and
// call the real sendto, so the datagram reaches the portal over loopback; the
// relay thread reads it there, learns the game's source port from the loopback
// source address, and forwards it to the server. Inbound, the relay thread
// sends the payload *from* the portal of the sender's endpoint to the game's
// port on loopback, and the receive hooks rewrite the portal address back to
// the virtual one. The game's sockets are never anything but real sockets.
//
// TCP is the same idea with a listening "gateway" in place of a portal. A
// connect() to a virtual address is redirected to the gateway after the hook
// has recorded, by the game socket's local port, where it was really going; the
// relay thread accepts it and opens a stream through the server. On the far
// side the relay thread connects to the game's listening port on loopback from
// a socket whose local port it records, so accept() and getpeername() can
// report the remote virtual address instead of 127.0.0.1.
// ---------------------------------------------------------------------------

namespace
{

// ---- settings and published state -------------------------------------------

std::mutex    g_settingsMutex;
RelaySettings g_settings;              // guarded by g_settingsMutex
bool          g_overridePresent = false; // the launcher passed Local\InterposerRelay_<pid>
bool          g_hooksWanted     = false;

std::atomic<int>      g_state{static_cast<int>(RelayState::Disabled)};
std::atomic<uint32_t> g_vip{0};
std::atomic<uint32_t> g_mask{0};
std::atomic<uint32_t> g_ipxNet{0};
std::atomic<uint32_t> g_clientId{0};
std::atomic<bool>     g_udpOn{true};
std::atomic<bool>     g_tcpOn{true};
std::atomic<bool>     g_ipxOn{true};
std::atomic<bool>     g_loopbackBroadcast{true};
std::atomic<bool>     g_udpActive{false};
std::atomic<int>      g_addressTimeout{5000};

// Set the first time an address is assigned and never reset: a reconnect asks
// for the same address, and sockets the game holds keep their meaning.
HANDLE g_addressReady = nullptr;

std::atomic<uint64_t> g_bytesSent{0};
std::atomic<uint64_t> g_bytesReceived{0};
std::atomic<uint64_t> g_datagramsSent{0};
std::atomic<uint64_t> g_datagramsReceived{0};
std::atomic<uint32_t> g_streamsOpen{0};
std::atomic<DWORD>    g_serverRtt{1};   // ms, from the control connection's pings

struct PeerInfo
{
    uint32_t     clientId;
    uint32_t     vip;
    std::wstring name;
};

std::mutex            g_statusMutex;    // guards the four below
std::wstring          g_lastError;
std::wstring          g_serverName;
std::wstring          g_networkName;
std::wstring          g_ownName;        // the player name we joined with
std::vector<PeerInfo> g_peers;

// ---- thread control -----------------------------------------------------------

HANDLE              g_thread = nullptr;
HANDLE              g_parked = nullptr;   // set by the thread once it is safe to terminate
std::atomic<bool>   g_stop{false};
std::mutex          g_commandMutex;
bool                g_commandReconnect  = false;
bool                g_commandDisconnect = false;
std::atomic<SOCKET> g_wakeSocket{INVALID_SOCKET};
sockaddr_in         g_wakeAddress{};
std::atomic<uint16_t> g_gatewayPort{0};

// ---- portals ------------------------------------------------------------------

struct Portal
{
    SOCKET   sock;
    uint8_t  proto;
    uint32_t vip;        // relay::kBroadcast for a broadcast portal
    uint16_t port;
    uint16_t localPort;  // the portal's own port on 127.0.0.1
};

constexpr size_t kMaxPortals = 900; // leaves FD_SETSIZE room for streams

std::mutex                              g_portalMutex;
std::unordered_map<uint64_t, Portal*>   g_portalsByKey;
std::unordered_map<uint16_t, Portal*>   g_portalsByLocalPort;
std::vector<Portal*>                    g_portals;     // never shrinks; freed at shutdown
bool                                    g_portalLimitLogged = false;

// ---- stream bookkeeping shared with the hooks ----------------------------------

struct PendingConnect
{
    SOCKET   game;
    uint32_t vip;
    uint16_t port;
};

std::mutex                                   g_streamMapMutex;
std::unordered_map<uint16_t, PendingConnect> g_pendingConnects; // by the game socket's local port
std::unordered_map<SOCKET, sockaddr_in>      g_socketPeers;     // game socket -> virtual peer
std::unordered_map<uint16_t, sockaddr_in>    g_inboundPeers;    // relay connector's local port -> virtual peer
std::unordered_set<SOCKET>                   g_virtualLocal;    // report the virtual address from getsockname

// ---- helpers --------------------------------------------------------------------

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring IpToString(uint32_t ipHost)
{
    wchar_t buf[16]{};
    swprintf_s(buf, L"%u.%u.%u.%u",
        (ipHost >> 24) & 0xFF, (ipHost >> 16) & 0xFF, (ipHost >> 8) & 0xFF, ipHost & 0xFF);
    return buf;
}

std::wstring EndpointToString(uint32_t ipHost, uint16_t port)
{
    std::wstring s = (ipHost == relay::kBroadcast) ? std::wstring(L"broadcast") : IpToString(ipHost);
    return s + L":" + std::to_wstring(port);
}

// Dotted-quad only. Hand-rolled because it is called while parsing settings,
// and keeps InetPton off the XP import list.
bool ParseIpv4(const std::wstring& text, uint32_t& ipHost)
{
    uint32_t parts[4]{};
    int      part = 0;
    bool     digit = false;

    for (wchar_t c : text)
    {
        if (c >= L'0' && c <= L'9')
        {
            parts[part] = parts[part] * 10 + static_cast<uint32_t>(c - L'0');
            if (parts[part] > 255) return false;
            digit = true;
        }
        else if (c == L'.' && digit && part < 3)
        {
            ++part;
            digit = false;
        }
        else
            return false;
    }

    if (part != 3 || !digit)
        return false;

    ipHost = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

bool Elapsed(DWORD since, DWORD interval)
{
    return GetTickCount() - since >= interval;
}

uint64_t PortalKey(uint8_t proto, uint32_t vip, uint16_t port)
{
    return (static_cast<uint64_t>(proto) << 48) | (static_cast<uint64_t>(vip) << 16) | port;
}

sockaddr_in Loopback(uint16_t port)
{
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

sockaddr_in Endpoint(uint32_t ipHost, uint16_t port)
{
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(ipHost);
    return a;
}

void SetNonBlocking(SOCKET s)
{
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}

void SetNoDelay(SOCKET s)
{
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

// Without this a datagram to a port nobody has bound comes back as an ICMP
// port-unreachable, and the next recvfrom on the sending socket fails with
// WSAECONNRESET -- which on a portal would look like the remote peer vanished.
void DisableConnReset(SOCKET s)
{
    BOOL  off = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &returned, nullptr, nullptr);
}

void AbortiveClose(SOCKET s)
{
    linger l{};
    l.l_onoff  = 1;
    l.l_linger = 0;
    setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
    closesocket(s);
}

uint16_t LocalPortOf(SOCKET s)
{
    sockaddr_in a{};
    int len = sizeof(a);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0)
        return 0;
    return ntohs(a.sin_port);
}

const wchar_t* StateName(RelayState state)
{
    switch (state)
    {
    case RelayState::Disabled:     return L"Disabled";
    case RelayState::Connecting:   return L"Connecting";
    case RelayState::Connected:    return L"Connected";
    case RelayState::Reconnecting: return L"Reconnecting";
    case RelayState::Failed:       return L"Failed";
    }
    return L"Unknown";
}

void SetLastErrorText(const std::wstring& text)
{
    std::lock_guard<std::mutex> lk(g_statusMutex);
    g_lastError = text;
}

void SetState(RelayState state, const wchar_t* detail = nullptr)
{
    const int previous = g_state.exchange(static_cast<int>(state));

    if (previous == static_cast<int>(state))
        return;

    const uint32_t vip = g_vip.load();
    const std::wstring address = vip ? IpToString(vip) : std::wstring();

    LogRelay(L"RELAY STATE", StateName(state), detail);
    FireRelayCallback(L"STATE", address.c_str(), StateName(state), static_cast<int>(state));
}

void Wake()
{
    const SOCKET s = g_wakeSocket.load();
    if (s == INVALID_SOCKET)
        return;

    NetHookScope scope;
    const char byte = 0;
    sendto(s, &byte, 1, 0, reinterpret_cast<const sockaddr*>(&g_wakeAddress), sizeof(g_wakeAddress));
}

void ApplyFeatureFlags(const RelaySettings& s)
{
    g_udpOn             = s.udp;
    g_tcpOn             = s.tcp;
    g_ipxOn             = s.ipx;
    g_loopbackBroadcast = s.loopbackBroadcast;
    g_addressTimeout    = s.addressTimeout < 0 ? 0 : s.addressTimeout;
}

RelaySettings SnapshotSettings()
{
    std::lock_guard<std::mutex> lk(g_settingsMutex);
    return g_settings;
}

// ---- portals ------------------------------------------------------------------

Portal* CreatePortalLocked(uint8_t proto, uint32_t vip, uint16_t port)
{
    if (g_portals.size() >= kMaxPortals)
    {
        if (!g_portalLimitLogged)
        {
            g_portalLimitLogged = true;
            InterposerLog(L"RELAY", L"portal limit reached; datagrams to further endpoints are dropped");
        }
        return nullptr;
    }

    NetHookScope scope;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return nullptr;

    sockaddr_in local = Loopback(0);
    if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
    {
        closesocket(s);
        return nullptr;
    }

    DisableConnReset(s);
    SetNonBlocking(s);

    int buffer = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));

    auto* p = new Portal{ s, proto, vip, port, LocalPortOf(s) };

    g_portalsByKey[PortalKey(proto, vip, port)] = p;
    g_portalsByLocalPort[p->localPort]          = p;
    g_portals.push_back(p);

    LogRelayDiag(L"RELAY PORTAL", EndpointToString(vip, port).c_str(),
        (std::wstring(proto == relay::ProtoIpx ? L"IPX via 127.0.0.1:" : L"UDP via 127.0.0.1:")
            + std::to_wstring(p->localPort)).c_str());

    return p;
}

Portal* GetPortal(uint8_t proto, uint32_t vip, uint16_t port)
{
    bool created = false;
    Portal* p = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_portalMutex);
        auto it = g_portalsByKey.find(PortalKey(proto, vip, port));
        if (it != g_portalsByKey.end())
            return it->second;

        p = CreatePortalLocked(proto, vip, port);
        created = p != nullptr;
    }

    // The relay thread has to add it to its select set before it can forward
    // anything the game sends there.
    if (created)
        Wake();

    return p;
}

// ---- connection (relay thread only) ---------------------------------------------

struct Connection
{
    SOCKET      tcp = INVALID_SOCKET;
    SOCKET      udp = INVALID_SOCKET;
    sockaddr_in server{};      // TCP endpoint
    sockaddr_in serverUdp{};   // UDP endpoint, from Welcome
    std::string rx;
    std::string tx;
    size_t      txOffset = 0;
    uint32_t    clientId = 0;
    uint32_t    token    = 0;
    uint16_t    udpPort  = 0;
    bool        welcomed = false;
    bool        rejected = false;
    bool        dead     = false;   // lost or refused; the thread loop closes it
    bool        udpConfirmed = false;
    DWORD       connectedAt  = 0;
    DWORD       lastRx       = 0;
    DWORD       lastPing     = 0;
    DWORD       lastUdpHello = 0;
    DWORD       lastUdpAck   = 0;
    int         udpHellos    = 0;
};

Connection s_conn;

struct Stream
{
    enum Phase { Opening, Connecting, Open };

    uint32_t    id;
    SOCKET      sock;
    bool        outbound;
    Phase       phase;
    uint32_t    peerVip;
    uint16_t    peerPort;
    uint16_t    connectorPort;   // inbound: the local port the game's accept() sees
    std::string out;
    size_t      outOffset = 0;
    bool        localEof  = false; // we sent StreamClose
    bool        remoteEof = false; // we received StreamClose
    bool        shutdown  = false; // shutdown(SD_SEND) done
};

std::map<uint32_t, Stream> s_streams;
uint32_t                   s_nextStreamId = 1;
SOCKET                     s_gateway = INVALID_SOCKET;
char                       s_buffer[65536];

void FlushTcp()
{
    while (s_conn.tcp != INVALID_SOCKET && s_conn.txOffset < s_conn.tx.size())
    {
        const int n = send(s_conn.tcp, s_conn.tx.data() + s_conn.txOffset,
            static_cast<int>(std::min<size_t>(s_conn.tx.size() - s_conn.txOffset, 64 * 1024)), 0);

        if (n <= 0)
            break; // WSAEWOULDBLOCK, or an error the read side will see

        s_conn.txOffset += static_cast<size_t>(n);
        g_bytesSent += static_cast<uint64_t>(n);
    }

    if (s_conn.txOffset == s_conn.tx.size())
    {
        s_conn.tx.clear();
        s_conn.txOffset = 0;
    }
    else if (s_conn.txOffset > 1024 * 1024)
    {
        s_conn.tx.erase(0, s_conn.txOffset);
        s_conn.txOffset = 0;
    }
}

void SendFrame(relay::FrameType type, const std::string& body)
{
    if (s_conn.tcp == INVALID_SOCKET)
        return;

    s_conn.tx += relay::Frame(type, body);
    FlushTcp();
}

size_t TcpBacklog()
{
    return s_conn.tx.size() - s_conn.txOffset;
}

void SendUdpPacket(relay::FrameType type, const std::string& body)
{
    relay::Writer w;
    w.U32(s_conn.clientId);
    w.U32(s_conn.token);
    w.U8(type);
    w.buf += body;

    const int n = sendto(s_conn.udp, w.buf.data(), static_cast<int>(w.buf.size()), 0,
        reinterpret_cast<const sockaddr*>(&s_conn.serverUdp), sizeof(s_conn.serverUdp));

    if (n > 0)
        g_bytesSent += static_cast<uint64_t>(n);
}

void SendDatagramBody(const std::string& body)
{
    if (s_conn.udpConfirmed && s_conn.udp != INVALID_SOCKET)
        SendUdpPacket(relay::Datagram, body);
    else
        SendFrame(relay::Datagram, body);

    ++g_datagramsSent;
}

std::string DatagramBody(uint8_t proto, uint32_t peerVip, uint16_t peerPort, uint16_t localPort,
                         uint8_t ipxType, const char* payload, size_t length)
{
    relay::Writer w;
    w.buf.reserve(relay::kDatagramHeader + length);
    w.U8(proto);
    w.U32(peerVip);
    w.U16(peerPort);
    w.U16(localPort);
    w.U8(ipxType);
    w.Bytes(payload, length);
    return w.buf;
}

// ---- datagram delivery ----------------------------------------------------------

// Hand a datagram that arrived for us to the game: send it from the portal of
// the sender's endpoint to the game's socket on loopback.
void DeliverInbound(uint8_t proto, uint32_t peerVip, uint16_t peerPort, uint16_t localPort,
                    uint8_t ipxType, const char* payload, size_t length)
{
    sockaddr_in to{};
    std::string framed;
    const char* data = payload;
    size_t      size = length;

    if (proto == relay::ProtoUdp)
    {
        if (!g_udpOn)
            return;
        to = Loopback(localPort);
    }
    else if (proto == relay::ProtoIpx)
    {
        if (!g_ipxOn)
            return;

        const uint16_t udpPort = IpxUdpPortForSocketNumber(localPort);
        if (!udpPort)
            return; // nothing in this process is bound to that IPX socket

        // The loopback hop carries the IPX packet type in front of the payload;
        // the IPX receive path strips it.
        framed.reserve(length + 1);
        framed.push_back(static_cast<char>(ipxType));
        framed.append(payload, length);
        data = framed.data();
        size = framed.size();
        to   = Loopback(udpPort);
    }
    else
        return;

    Portal* p = GetPortal(proto, peerVip, peerPort);
    if (!p)
        return;

    sendto(p->sock, data, static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    ++g_datagramsReceived;

    if (g_logLevel >= LogLevel::Trace)
        LogRelayDiag(L"RELAY RECV", EndpointToString(peerVip, peerPort).c_str(),
            (std::to_wstring(length) + L" bytes to port " + std::to_wstring(localPort)).c_str());
}

// A datagram the game sent to a portal: forward it to the server, or deliver it
// straight back when it was addressed to ourselves.
void ForwardOutbound(uint8_t proto, uint32_t dstVip, uint16_t dstPort, uint16_t srcPort,
                     uint8_t ipxType, const char* payload, size_t length)
{
    if (length > relay::kMaxDatagram)
        return;

    const uint32_t own       = g_vip.load();
    const bool     broadcast = dstVip == relay::kBroadcast;

    if (own && (dstVip == own || (broadcast && g_loopbackBroadcast)))
        DeliverInbound(proto, own, srcPort, dstPort, ipxType, payload, length);

    if (dstVip == own || !s_conn.welcomed)
        return;

    SendDatagramBody(DatagramBody(proto, dstVip, dstPort, srcPort, ipxType, payload, length));

    if (g_logLevel >= LogLevel::Trace)
        LogRelayDiag(L"RELAY SEND", EndpointToString(dstVip, dstPort).c_str(),
            (std::to_wstring(length) + L" bytes from port " + std::to_wstring(srcPort)).c_str());
}

void HandleDatagram(relay::Reader& r)
{
    const uint8_t  proto     = r.U8();
    const uint32_t peerVip   = r.U32();
    const uint16_t peerPort  = r.U16();
    const uint16_t localPort = r.U16();
    const uint8_t  ipxType   = r.U8();

    if (!r.Ok())
        return;

    DeliverInbound(proto, peerVip, peerPort, localPort, ipxType,
        reinterpret_cast<const char*>(r.Cursor()), r.Remaining());
}

void ReadPortal(Portal* p)
{
    for (int i = 0; i < 64; ++i)
    {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(p->sock, s_buffer, sizeof(s_buffer), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLen);

        if (n < 0)
            return;

        // Only the game, over loopback, has any business sending here.
        if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
            continue;

        const uint16_t srcPort = ntohs(from.sin_port);

        if (p->proto == relay::ProtoUdp)
        {
            if (g_udpOn)
                ForwardOutbound(relay::ProtoUdp, p->vip, p->port, srcPort, 0, s_buffer, static_cast<size_t>(n));
        }
        else if (n >= 1)
        {
            const uint16_t socketNumber = IpxSocketNumberForUdpPort(srcPort);
            if (socketNumber && g_ipxOn)
                ForwardOutbound(relay::ProtoIpx, p->vip, p->port, socketNumber,
                    static_cast<uint8_t>(s_buffer[0]), s_buffer + 1, static_cast<size_t>(n - 1));
        }
    }
}

// ---- streams (relay thread only) --------------------------------------------------

void DestroyStream(std::map<uint32_t, Stream>::iterator it, bool abortive)
{
    Stream& st = it->second;

    if (st.sock != INVALID_SOCKET)
    {
        if (abortive) AbortiveClose(st.sock);
        else          closesocket(st.sock);
    }

    if (!st.outbound && st.connectorPort)
    {
        std::lock_guard<std::mutex> lk(g_streamMapMutex);
        g_inboundPeers.erase(st.connectorPort);
    }

    LogRelayDiag(L"RELAY STREAM", EndpointToString(st.peerVip, st.peerPort).c_str(),
        abortive ? L"aborted" : L"closed");

    s_streams.erase(it);
    g_streamsOpen = static_cast<uint32_t>(s_streams.size());
}

void SendStreamId(relay::FrameType type, uint32_t id)
{
    relay::Writer w;
    w.U32(id);
    SendFrame(type, w.buf);
}

void FlushStream(Stream& st)
{
    while (st.outOffset < st.out.size())
    {
        const int n = send(st.sock, st.out.data() + st.outOffset,
            static_cast<int>(std::min<size_t>(st.out.size() - st.outOffset, 64 * 1024)), 0);

        if (n <= 0)
            break;

        st.outOffset += static_cast<size_t>(n);
    }

    if (st.outOffset == st.out.size())
    {
        st.out.clear();
        st.outOffset = 0;

        if (st.remoteEof && !st.shutdown)
        {
            ::shutdown(st.sock, SD_SEND);
            st.shutdown = true;
        }
    }
}

// Both directions finished: tear the stream down.
bool MaybeFinishStream(std::map<uint32_t, Stream>::iterator it)
{
    Stream& st = it->second;
    if (st.localEof && st.remoteEof && st.out.empty())
    {
        DestroyStream(it, false);
        return true;
    }
    return false;
}

void AbortAllStreams()
{
    while (!s_streams.empty())
        DestroyStream(s_streams.begin(), true);

    std::lock_guard<std::mutex> lk(g_streamMapMutex);
    g_inboundPeers.clear();
}

void AcceptGateway()
{
    for (;;)
    {
        sockaddr_in peer{};
        int peerLen = sizeof(peer);
        SOCKET c = accept(s_gateway, reinterpret_cast<sockaddr*>(&peer), &peerLen);

        if (c == INVALID_SOCKET)
            return;

        const uint16_t gamePort = ntohs(peer.sin_port);

        PendingConnect pending{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_streamMapMutex);
            auto it = g_pendingConnects.find(gamePort);
            if (it != g_pendingConnects.end())
            {
                pending = it->second;
                found   = true;
                g_pendingConnects.erase(it);
            }
        }

        if (!found || !s_conn.welcomed || !g_tcpOn)
        {
            AbortiveClose(c);
            continue;
        }

        SetNonBlocking(c);
        SetNoDelay(c);

        uint32_t id = s_nextStreamId++;
        if (s_nextStreamId >= relay::kServerStreamBit)
            s_nextStreamId = 1;

        Stream st{};
        st.id            = id;
        st.sock          = c;
        st.outbound      = true;
        st.phase         = Stream::Opening;
        st.peerVip       = pending.vip;
        st.peerPort      = pending.port;
        st.connectorPort = gamePort;
        s_streams.emplace(id, std::move(st));
        g_streamsOpen = static_cast<uint32_t>(s_streams.size());

        relay::Writer w;
        w.U32(id);
        w.U32(pending.vip);
        w.U16(pending.port);
        w.U16(gamePort);
        SendFrame(relay::StreamOpen, w.buf);

        LogRelay(L"RELAY STREAM", EndpointToString(pending.vip, pending.port).c_str(), L"connecting");
    }
}

void HandleStreamOpen(relay::Reader& r)
{
    const uint32_t id        = r.U32();
    const uint32_t peerVip   = r.U32();
    const uint16_t peerPort  = r.U16();
    const uint16_t localPort = r.U16();

    if (!r.Ok())
        return;

    auto reject = [&](uint8_t reason)
    {
        relay::Writer w;
        w.U32(id);
        w.U8(reason);
        SendFrame(relay::StreamReject, w.buf);
    };

    if (!g_tcpOn || s_streams.count(id))
    {
        reject(relay::StreamRefused);
        return;
    }

    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c == INVALID_SOCKET)
    {
        reject(relay::StreamRefused);
        return;
    }

    sockaddr_in local = Loopback(0);
    if (bind(c, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
    {
        closesocket(c);
        reject(relay::StreamRefused);
        return;
    }

    const uint16_t connectorPort = LocalPortOf(c);
    {
        std::lock_guard<std::mutex> lk(g_streamMapMutex);
        g_inboundPeers[connectorPort] = Endpoint(peerVip, peerPort);
    }

    SetNonBlocking(c);
    SetNoDelay(c);

    sockaddr_in target = Loopback(localPort);
    connect(c, reinterpret_cast<const sockaddr*>(&target), sizeof(target));

    Stream st{};
    st.id            = id;
    st.sock          = c;
    st.outbound      = false;
    st.phase         = Stream::Connecting;
    st.peerVip       = peerVip;
    st.peerPort      = peerPort;
    st.connectorPort = connectorPort;
    s_streams.emplace(id, std::move(st));
    g_streamsOpen = static_cast<uint32_t>(s_streams.size());

    LogRelay(L"RELAY STREAM", EndpointToString(peerVip, peerPort).c_str(),
        (L"incoming to port " + std::to_wstring(localPort)).c_str());
}

void HandleStreamFrame(relay::FrameType type, relay::Reader& r)
{
    const uint32_t id = r.U32();
    if (!r.Ok())
        return;

    auto it = s_streams.find(id);
    if (it == s_streams.end())
        return;

    Stream& st = it->second;

    switch (type)
    {
    case relay::StreamAccept:
        if (st.phase == Stream::Opening)
        {
            st.phase = Stream::Open;
            LogRelay(L"RELAY STREAM", EndpointToString(st.peerVip, st.peerPort).c_str(), L"connected");
        }
        break;

    case relay::StreamReject:
        // The game's connect already succeeded locally, against the gateway,
        // so the closest it can see to "refused" is an immediate reset.
        LogRelay(L"RELAY STREAM", EndpointToString(st.peerVip, st.peerPort).c_str(), L"refused");
        DestroyStream(it, true);
        break;

    case relay::StreamData:
        st.out.append(reinterpret_cast<const char*>(r.Cursor()), r.Remaining());
        if (st.phase != Stream::Connecting)
            FlushStream(st);
        break;

    case relay::StreamClose:
        st.remoteEof = true;
        if (st.phase != Stream::Connecting)
            FlushStream(st);
        MaybeFinishStream(it);
        break;

    default:
        break;
    }
}

void ReadStream(std::map<uint32_t, Stream>::iterator it)
{
    Stream& st = it->second;

    const int n = recv(st.sock, s_buffer, static_cast<int>(relay::kMaxStreamChunk), 0);

    if (n > 0)
    {
        relay::Writer w;
        w.buf.reserve(4 + static_cast<size_t>(n));
        w.U32(st.id);
        w.Bytes(s_buffer, static_cast<size_t>(n));
        SendFrame(relay::StreamData, w.buf);
        return;
    }

    if (n == 0)
    {
        st.localEof = true;
        SendStreamId(relay::StreamClose, st.id);
        MaybeFinishStream(it);
        return;
    }

    if (WSAGetLastError() == WSAEWOULDBLOCK)
        return;

    SendStreamId(relay::StreamClose, st.id);
    DestroyStream(it, true);
}

void CompleteInboundConnect(std::map<uint32_t, Stream>::iterator it, bool failed)
{
    Stream& st = it->second;

    int error = 0;
    int len   = sizeof(error);
    getsockopt(st.sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);

    if (failed || error != 0)
    {
        relay::Writer w;
        w.U32(st.id);
        w.U8(relay::StreamRefused);
        SendFrame(relay::StreamReject, w.buf);
        LogRelay(L"RELAY STREAM", EndpointToString(st.peerVip, st.peerPort).c_str(),
            L"nothing listening on the local port");
        DestroyStream(it, true);
        return;
    }

    st.phase = Stream::Open;
    SendStreamId(relay::StreamAccept, st.id);
    FlushStream(st);
    MaybeFinishStream(it);
}

// ---- control frames ---------------------------------------------------------------

void ClearPeers()
{
    std::vector<PeerInfo> gone;
    {
        std::lock_guard<std::mutex> lk(g_statusMutex);
        gone.swap(g_peers);
    }

    for (const PeerInfo& p : gone)
        FireRelayCallback(L"PEER LEAVE", IpToString(p.vip).c_str(), p.name.c_str(), static_cast<int>(p.clientId));
}

void HandleWelcome(relay::Reader& r)
{
    const uint32_t clientId = r.U32();
    const uint32_t token    = r.U32();
    const uint32_t vip      = r.U32();
    const uint32_t mask     = r.U32();
    const uint16_t udpPort  = r.U16();
    const uint32_t ipxNet   = r.U32();

    if (!r.Ok() || vip == 0)
        return;

    const uint32_t previous = g_vip.exchange(vip);

    s_conn.clientId     = clientId;
    s_conn.token        = token;
    s_conn.udpPort      = udpPort;
    s_conn.welcomed     = true;
    s_conn.lastPing     = GetTickCount() - 10000; // ping now: the ICMP hooks want a round trip early
    s_conn.udpConfirmed = false;
    s_conn.udpHellos    = 0;
    s_conn.lastUdpHello = GetTickCount() - 60000;
    s_conn.serverUdp    = s_conn.server;
    s_conn.serverUdp.sin_port = htons(udpPort);

    g_mask     = mask;
    g_ipxNet   = ipxNet;
    g_clientId = clientId;
    g_udpActive = false;

    SetEvent(g_addressReady);

    if (previous && previous != vip)
        InterposerLog(L"RELAY", (L"virtual address changed from " + IpToString(previous) + L" to "
            + IpToString(vip) + L"; addresses the game already advertised are stale").c_str());

    uint32_t prefix = 0;
    for (uint32_t m = mask; m & 0x80000000u; m <<= 1)
        ++prefix;

    std::wstring network;
    {
        std::lock_guard<std::mutex> lk(g_statusMutex);
        network = g_networkName;
        g_lastError.clear();
    }

    LogRelay(L"RELAY", (IpToString(vip) + L"/" + std::to_wstring(prefix)).c_str(),
        (L"assigned in network '" + network + L"'").c_str());

    SetState(RelayState::Connected);
}

void HandleControlFrame(relay::FrameType type, const uint8_t* body, size_t length)
{
    relay::Reader r(body, length);

    switch (type)
    {
    case relay::Welcome:
        HandleWelcome(r);
        break;

    case relay::Reject:
    {
        const uint8_t code = r.U8();
        const std::wstring message = Utf8ToWide(r.Str());
        const std::wstring text = L"rejected by the relay server (" + std::to_wstring(code) + L"): " + message;

        InterposerLog(L"RELAY", text.c_str());
        SetLastErrorText(text);
        s_conn.rejected = true;
        s_conn.dead     = true;
        break;
    }

    case relay::PeerJoined:
    {
        PeerInfo p{};
        p.clientId = r.U32();
        p.vip      = r.U32();
        p.name     = Utf8ToWide(r.Str());
        if (!r.Ok())
            break;

        {
            std::lock_guard<std::mutex> lk(g_statusMutex);
            g_peers.erase(std::remove_if(g_peers.begin(), g_peers.end(),
                [&](const PeerInfo& e) { return e.clientId == p.clientId; }), g_peers.end());
            g_peers.push_back(p);
        }

        LogRelay(L"RELAY PEER", IpToString(p.vip).c_str(), (L"joined: " + p.name).c_str());
        FireRelayCallback(L"PEER JOIN", IpToString(p.vip).c_str(), p.name.c_str(), static_cast<int>(p.clientId));
        break;
    }

    case relay::PeerLeft:
    {
        const uint32_t clientId = r.U32();
        if (!r.Ok())
            break;

        PeerInfo gone{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_statusMutex);
            for (auto it = g_peers.begin(); it != g_peers.end(); ++it)
            {
                if (it->clientId == clientId)
                {
                    gone  = *it;
                    found = true;
                    g_peers.erase(it);
                    break;
                }
            }
        }

        if (found)
        {
            LogRelay(L"RELAY PEER", IpToString(gone.vip).c_str(), (L"left: " + gone.name).c_str());
            FireRelayCallback(L"PEER LEAVE", IpToString(gone.vip).c_str(), gone.name.c_str(),
                static_cast<int>(gone.clientId));
        }
        break;
    }

    case relay::Ping:
    {
        relay::Writer w;
        w.U32(r.U32());
        SendFrame(relay::Pong, w.buf);
        break;
    }

    case relay::Pong:
    {
        // The nonce is the tick count the ping went out at.
        const DWORD rtt = GetTickCount() - r.U32();
        if (r.Ok() && rtt < 60000)
            g_serverRtt = rtt ? rtt : 1;
        break;
    }

    case relay::Datagram:
        HandleDatagram(r);
        break;

    case relay::StreamOpen:
        HandleStreamOpen(r);
        break;

    case relay::StreamAccept:
    case relay::StreamReject:
    case relay::StreamData:
    case relay::StreamClose:
        HandleStreamFrame(type, r);
        break;

    default:
        break;
    }
}

// ---- connection lifecycle ----------------------------------------------------------

void CloseConnection(bool sendLeave)
{
    if (s_conn.tcp != INVALID_SOCKET)
    {
        if (sendLeave && s_conn.welcomed)
        {
            SendFrame(relay::Leave, {});
            FlushTcp();
        }
        closesocket(s_conn.tcp);
    }

    if (s_conn.udp != INVALID_SOCKET)
        closesocket(s_conn.udp);

    s_conn = Connection{};
    g_udpActive = false;

    AbortAllStreams();
    ClearPeers();
}

// "host" or "host:port"; a bare IPv6 literal is not supported.
void SplitServer(const std::wstring& server, int defaultPort, std::wstring& host, std::wstring& port)
{
    host = server;
    port = std::to_wstring(defaultPort);

    const size_t colon = server.rfind(L':');
    if (colon != std::wstring::npos && server.find(L':') == colon && colon + 1 < server.size())
    {
        host = server.substr(0, colon);
        port = server.substr(colon + 1);
    }
}

// Wait for a non-blocking connect, waking early for a stop request.
bool WaitForConnect(SOCKET s, DWORD timeoutMs)
{
    const DWORD start = GetTickCount();

    while (!g_stop && !Elapsed(start, timeoutMs))
    {
        fd_set wr, ex, rd;
        FD_ZERO(&wr); FD_ZERO(&ex); FD_ZERO(&rd);
        FD_SET(s, &wr);
        FD_SET(s, &ex);

        const SOCKET wake = g_wakeSocket.load();
        if (wake != INVALID_SOCKET)
            FD_SET(wake, &rd);

        timeval tv{ 0, 250 * 1000 };
        if (select(0, &rd, &wr, &ex, &tv) <= 0)
            continue;

        if (wake != INVALID_SOCKET && FD_ISSET(wake, &rd))
        {
            char drain[16];
            while (recv(wake, drain, sizeof(drain), 0) > 0) {}
        }

        if (FD_ISSET(s, &ex))
            return false;

        if (FD_ISSET(s, &wr))
        {
            int error = 0;
            int len = sizeof(error);
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);
            return error == 0;
        }
    }

    return false;
}

bool OpenConnection(const RelaySettings& cfg)
{
    std::wstring host, port;
    SplitServer(cfg.server, cfg.port, host, port);

    if (host.empty())
    {
        SetLastErrorText(L"no relay server configured (NetworkRelay.Server)");
        return false;
    }

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    if (getaddrinfo(WideToUtf8(host).c_str(), WideToUtf8(port).c_str(), &hints, &result) != 0 || !result)
    {
        SetLastErrorText(L"could not resolve relay server " + host);
        LogRelay(L"RELAY", cfg.server.c_str(), L"could not resolve");
        return false;
    }

    sockaddr_in server = *reinterpret_cast<const sockaddr_in*>(result->ai_addr);
    freeaddrinfo(result);

    SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp == INVALID_SOCKET)
        return false;

    SetNonBlocking(tcp);
    SetNoDelay(tcp);

    connect(tcp, reinterpret_cast<const sockaddr*>(&server), sizeof(server));

    if (!WaitForConnect(tcp, 5000))
    {
        closesocket(tcp);
        SetLastErrorText(L"could not connect to relay server " + cfg.server);
        LogRelay(L"RELAY", cfg.server.c_str(), L"connection failed");
        return false;
    }

    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp != INVALID_SOCKET)
    {
        sockaddr_in any{};
        any.sin_family = AF_INET;
        bind(udp, reinterpret_cast<const sockaddr*>(&any), sizeof(any));
        SetNonBlocking(udp);
        DisableConnReset(udp);
    }

    s_conn = Connection{};
    s_conn.tcp         = tcp;
    s_conn.udp         = udp;
    s_conn.server      = server;
    s_conn.connectedAt = GetTickCount();
    s_conn.lastRx      = s_conn.connectedAt;
    s_conn.lastPing    = s_conn.connectedAt;

    // Ask for the address we already had, so a reconnect does not renumber us
    // under a game that has already told its peers where to find it.
    uint32_t requested = g_vip.load();
    if (!requested && !cfg.address.empty())
        ParseIpv4(cfg.address, requested);

    // Peers see us under the username the game itself is told -- the launcher's
    // override, Player.Username, or the Windows account -- so the two never differ.
    std::wstring player;
    {
        wchar_t name[256]{};
        if (InterposerGetUsername(name, ARRAYSIZE(name)))
            player = name;
    }

    relay::Writer w;
    w.U8(relay::kProtocolVersion);
    w.Str(WideToUtf8(cfg.network));
    w.Str(WideToUtf8(cfg.ticket));
    w.Str(WideToUtf8(player));
    w.U32(requested);
    SendFrame(relay::Hello, w.buf);

    {
        std::lock_guard<std::mutex> lk(g_statusMutex);
        g_serverName  = cfg.server;
        g_networkName = cfg.network;
        g_ownName     = player;
    }

    LogRelay(L"RELAY", cfg.server.c_str(), (L"connected; joining '" + cfg.network + L"'").c_str());
    return true;
}

void ReadControl()
{
    for (;;)
    {
        const int n = recv(s_conn.tcp, s_buffer, sizeof(s_buffer), 0);

        if (n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK))
        {
            SetLastErrorText(L"the relay server closed the connection");
            s_conn.dead = true;
            return;
        }

        if (n < 0)
            return;

        s_conn.rx.append(s_buffer, static_cast<size_t>(n));
        s_conn.lastRx = GetTickCount();
        g_bytesReceived += static_cast<uint64_t>(n);

        size_t offset = 0;
        while (s_conn.rx.size() - offset >= 2)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(s_conn.rx.data() + offset);
            const size_t length = (static_cast<size_t>(p[0]) << 8) | p[1];

            if (length == 0)
            {
                s_conn.dead = true; // protocol violation
                return;
            }

            if (s_conn.rx.size() - offset < 2 + length)
                break;

            HandleControlFrame(static_cast<relay::FrameType>(p[2]), p + 3, length - 1);
            offset += 2 + length;

            if (s_conn.dead)
                return;
        }

        s_conn.rx.erase(0, offset);
    }
}

void ReadUdp()
{
    for (int i = 0; i < 256; ++i)
    {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(s_conn.udp, s_buffer, sizeof(s_buffer), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLen);

        if (n < 0)
            return;

        if (from.sin_addr.s_addr != s_conn.serverUdp.sin_addr.s_addr ||
            from.sin_port != s_conn.serverUdp.sin_port)
            continue;

        g_bytesReceived += static_cast<uint64_t>(n);

        relay::Reader r(reinterpret_cast<const uint8_t*>(s_buffer), static_cast<size_t>(n));
        const uint32_t clientId = r.U32();
        const uint32_t token    = r.U32();
        const uint8_t  type     = r.U8();

        if (!r.Ok() || clientId != s_conn.clientId || token != s_conn.token)
            continue;

        if (type == relay::UdpHelloAck)
        {
            s_conn.lastUdpAck = GetTickCount();
            if (!s_conn.udpConfirmed)
            {
                s_conn.udpConfirmed = true;
                g_udpActive = true;
                LogRelay(L"RELAY", L"UDP path confirmed", nullptr);
            }
        }
        else if (type == relay::Datagram)
            HandleDatagram(r);
    }
}

void RunTimers()
{
    if (s_conn.tcp == INVALID_SOCKET || s_conn.dead)
        return;

    if (!s_conn.welcomed)
    {
        if (Elapsed(s_conn.connectedAt, 10000))
        {
            SetLastErrorText(L"the relay server did not answer the handshake");
            s_conn.dead = true;
        }
        return;
    }

    if (Elapsed(s_conn.lastRx, 30000))
    {
        SetLastErrorText(L"lost contact with the relay server");
        s_conn.dead = true;
        return;
    }

    if (Elapsed(s_conn.lastPing, 10000))
    {
        s_conn.lastPing = GetTickCount();
        relay::Writer w;
        w.U32(s_conn.lastPing);
        SendFrame(relay::Ping, w.buf);
    }

    if (s_conn.udpPort && s_conn.udp != INVALID_SOCKET)
    {
        // Probe quickly until the server answers, then refresh the NAT mapping
        // every 15 s. If the answers stop, fall back to TCP rather than send
        // datagrams into a hole.
        const DWORD interval = s_conn.udpConfirmed ? 15000 : 500;
        if ((s_conn.udpConfirmed || s_conn.udpHellos < 20) && Elapsed(s_conn.lastUdpHello, interval))
        {
            s_conn.lastUdpHello = GetTickCount();
            ++s_conn.udpHellos;
            SendUdpPacket(relay::UdpHello, {});
        }

        if (s_conn.udpConfirmed && Elapsed(s_conn.lastUdpAck, 50000))
        {
            s_conn.udpConfirmed = false;
            s_conn.udpHellos    = 0;
            g_udpActive = false;
            LogRelay(L"RELAY", L"UDP path lost; datagrams fall back to TCP", nullptr);
        }
    }
}

// One select() round over every socket the thread owns. A lost control
// connection is flagged in s_conn.dead for the thread loop to deal with.
void Pump(DWORD timeoutMs)
{
    fd_set rd, wr, ex;
    FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);

    const SOCKET wake = g_wakeSocket.load();
    if (wake != INVALID_SOCKET)  FD_SET(wake, &rd);
    if (s_gateway != INVALID_SOCKET) FD_SET(s_gateway, &rd);

    if (s_conn.tcp != INVALID_SOCKET)
    {
        FD_SET(s_conn.tcp, &rd);
        if (TcpBacklog())
            FD_SET(s_conn.tcp, &wr);
    }

    if (s_conn.udp != INVALID_SOCKET)
        FD_SET(s_conn.udp, &rd);

    // Stop reading game streams while the control connection is backed up, so
    // a fast sender cannot grow the queue without bound.
    const bool readStreams = TcpBacklog() < 2 * 1024 * 1024;

    for (auto& [id, st] : s_streams)
    {
        if (st.phase == Stream::Connecting)
        {
            FD_SET(st.sock, &wr);
            FD_SET(st.sock, &ex);
            continue;
        }

        if (readStreams && !st.localEof)
            FD_SET(st.sock, &rd);
        if (!st.out.empty())
            FD_SET(st.sock, &wr);
    }

    std::vector<Portal*> portals;
    {
        std::lock_guard<std::mutex> lk(g_portalMutex);
        portals = g_portals;
    }

    for (Portal* p : portals)
        FD_SET(p->sock, &rd);

    timeval tv{ 0, static_cast<long>(timeoutMs) * 1000 };
    const int ready = select(0, &rd, &wr, &ex, &tv);

    if (ready <= 0)
        return;

    if (wake != INVALID_SOCKET && FD_ISSET(wake, &rd))
    {
        char drain[64];
        while (recv(wake, drain, sizeof(drain), 0) > 0) {}
    }

    if (s_conn.tcp != INVALID_SOCKET)
    {
        if (FD_ISSET(s_conn.tcp, &rd))
            ReadControl();

        if (!s_conn.dead && FD_ISSET(s_conn.tcp, &wr))
            FlushTcp();
    }

    if (s_conn.udp != INVALID_SOCKET && FD_ISSET(s_conn.udp, &rd))
        ReadUdp();

    if (s_gateway != INVALID_SOCKET && FD_ISSET(s_gateway, &rd))
        AcceptGateway();

    for (Portal* p : portals)
        if (FD_ISSET(p->sock, &rd))
            ReadPortal(p);

    for (auto it = s_streams.begin(); it != s_streams.end();)
    {
        auto current = it++;
        Stream& st = current->second;

        if (st.phase == Stream::Connecting)
        {
            if (FD_ISSET(st.sock, &ex))
                CompleteInboundConnect(current, true);
            else if (FD_ISSET(st.sock, &wr))
                CompleteInboundConnect(current, false);
            continue;
        }

        if (FD_ISSET(st.sock, &wr))
        {
            FlushStream(st);
            if (MaybeFinishStream(current))
                continue;
        }

        if (FD_ISSET(st.sock, &rd))
            ReadStream(current);
    }
}

void CreateLocalSockets()
{
    SOCKET wake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (wake != INVALID_SOCKET)
    {
        sockaddr_in a = Loopback(0);
        bind(wake, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
        SetNonBlocking(wake);
        DisableConnReset(wake);
        g_wakeAddress = Loopback(LocalPortOf(wake));
        g_wakeSocket  = wake;
    }

    s_gateway = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_gateway != INVALID_SOCKET)
    {
        sockaddr_in a = Loopback(0);
        if (bind(s_gateway, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0 &&
            listen(s_gateway, SOMAXCONN) == 0)
        {
            SetNonBlocking(s_gateway);
            g_gatewayPort = LocalPortOf(s_gateway);
        }
        else
        {
            closesocket(s_gateway);
            s_gateway = INVALID_SOCKET;
        }
    }
}

DWORD WINAPI RelayThread(LPVOID)
{
    // Every socket call this thread makes is its own business, not the game's:
    // the hooks pass it straight through for the life of the thread.
    ++t_netHookDepth;

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    CreateLocalSockets();

    RelaySettings cfg     = SnapshotSettings();
    bool  wantConnected   = cfg.enabled;
    bool  failed          = false;
    DWORD nextAttempt     = GetTickCount();
    DWORD backoff         = 1000;

    while (!g_stop)
    {
        {
            std::lock_guard<std::mutex> lk(g_commandMutex);

            if (g_commandReconnect)
            {
                g_commandReconnect = false;
                CloseConnection(true);
                cfg           = SnapshotSettings();
                wantConnected = cfg.enabled;
                failed        = false;
                backoff       = 1000;
                nextAttempt   = GetTickCount();
            }

            if (g_commandDisconnect)
            {
                g_commandDisconnect = false;
                CloseConnection(true);
                wantConnected = false;
                SetState(RelayState::Disabled);
            }
        }

        if (wantConnected && !failed && s_conn.tcp == INVALID_SOCKET &&
            static_cast<int>(GetTickCount() - nextAttempt) >= 0)
        {
            SetState(g_vip.load() ? RelayState::Reconnecting : RelayState::Connecting);

            if (!OpenConnection(cfg))
            {
                nextAttempt = GetTickCount() + backoff;
                backoff     = std::min<DWORD>(backoff * 2, 30000);
            }
            else
                backoff = 1000;
        }

        Pump(250);
        RunTimers();

        if (s_conn.dead)
        {
            const bool rejected = s_conn.rejected;
            CloseConnection(false);

            if (rejected)
            {
                failed = true;
                SetState(RelayState::Failed);
            }
            else if (wantConnected)
            {
                LogRelay(L"RELAY", L"connection lost; retrying", nullptr);
                nextAttempt = GetTickCount() + backoff;
                backoff     = std::min<DWORD>(backoff * 2, 30000);
                SetState(g_vip.load() ? RelayState::Reconnecting : RelayState::Connecting);
            }
        }
    }

    CloseConnection(true);

    // Park rather than return. ShutdownRelay may be about to unmap this module
    // (FreeLibrary), and a thread on its way out still has return addresses in
    // it; a thread blocked in the kernel never runs another instruction of ours
    // and is terminated from there.
    SetEvent(g_parked);
    Sleep(INFINITE);
    return 0;
}

void StartThread()
{
    if (g_thread)
        return;

    g_parked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, RelayThread, nullptr, 0, nullptr);
}

std::wstring JsonEscape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 2);
    for (wchar_t c : s)
    {
        switch (c)
        {
        case L'"':  out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n";  break;
        case L'\r': out += L"\\r";  break;
        case L'\t': out += L"\\t";  break;
        default:
            if (c < 0x20)
            {
                wchar_t buf[8];
                swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
                out += buf;
            }
            else
                out += c;
        }
    }
    return out;
}

std::wstring ReadOverrideMapping()
{
    wchar_t name[64]{};
    swprintf_s(name, L"Local\\InterposerRelay_%lu", GetCurrentProcessId());

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!mapping)
        return {};

    std::wstring text;
    constexpr SIZE_T kSize = 8192;

    if (const char* view = static_cast<const char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, kSize)))
    {
        const size_t length = strnlen(view, kSize);
        text = Utf8ToWide(std::string(view, length));
        UnmapViewOfFile(view);
    }

    CloseHandle(mapping);

    g_overridePresent = true;
    return text.empty() ? std::wstring(L"{}") : text;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void InitRelay()
{
    g_addressReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    RelaySettings settings = g_relayConfig;

    const std::wstring overrides = ReadOverrideMapping();
    if (g_overridePresent)
    {
        std::wstring error;
        if (!ParseRelaySettings(WideToUtf8(overrides), settings, &error))
            InterposerLog(L"RELAY", (L"ignoring the launcher's relay options: " + error).c_str());
    }

    {
        std::lock_guard<std::mutex> lk(g_settingsMutex);
        g_settings = settings;
    }

    ApplyFeatureFlags(settings);
    g_hooksWanted = settings.enabled || g_overridePresent;

    if (!g_hooksWanted)
        return;

    if (settings.enabled)
        LogRelay(L"RELAY", settings.server.c_str(), (L"network '" + settings.network + L"'").c_str());
    else
        LogRelay(L"RELAY", L"disabled; waiting for the launcher", nullptr);

    StartThread();
}

void ShutdownRelay(bool processTerminating)
{
    if (!g_thread || processTerminating)
        return;

    g_stop = true;
    Wake();

    // The thread parks itself once its sockets are closed. Waiting on the
    // thread handle instead would deadlock: its exit needs the loader lock,
    // which DLL_PROCESS_DETACH is holding.
    const bool parked = WaitForSingleObject(g_parked, 3000) == WAIT_OBJECT_0;

    // Terminated either way -- left running, it would fault the moment this
    // module is unmapped. One that never parked may hold a lock, though, so its
    // resources are leaked rather than touched.
    TerminateThread(g_thread, 0);

    CloseHandle(g_thread);
    CloseHandle(g_parked);
    g_thread = nullptr;
    g_parked = nullptr;

    if (!parked)
        return;

    const SOCKET wake = g_wakeSocket.exchange(INVALID_SOCKET);
    if (wake != INVALID_SOCKET)
        closesocket(wake);

    if (s_gateway != INVALID_SOCKET)
    {
        closesocket(s_gateway);
        s_gateway = INVALID_SOCKET;
    }

    std::lock_guard<std::mutex> lk(g_portalMutex);
    for (Portal* p : g_portals)
    {
        closesocket(p->sock);
        delete p;
    }
    g_portals.clear();
    g_portalsByKey.clear();
    g_portalsByLocalPort.clear();
}

bool RelayHooksWanted()
{
    return g_hooksWanted;
}

// ---------------------------------------------------------------------------
// Address translation
// ---------------------------------------------------------------------------

bool RelayActive()        { return g_vip.load() != 0; }
uint32_t RelayVirtualIp() { return g_vip.load(); }
uint32_t RelayNetmask()   { return g_mask.load(); }
uint32_t RelayIpxNetwork(){ return g_ipxNet.load(); }
bool RelayUdpEnabled()    { return g_hooksWanted && g_udpOn; }
bool RelayTcpEnabled()    { return g_hooksWanted && g_tcpOn; }
bool RelayIpxEnabled()    { return g_hooksWanted && g_ipxOn; }

bool RelayWaitForAddress()
{
    if (RelayActive())
        return true;

    if (!g_hooksWanted || !g_addressReady)
        return false;

    const auto state = static_cast<RelayState>(g_state.load());
    if (state != RelayState::Connecting && state != RelayState::Reconnecting)
        return false;

    WaitForSingleObject(g_addressReady, static_cast<DWORD>(g_addressTimeout.load()));
    return RelayActive();
}

bool RelayIsVirtualAddress(uint32_t ipHost)
{
    if (ipHost == relay::kBroadcast)
        return true;

    const uint32_t vip  = g_vip.load();
    const uint32_t mask = g_mask.load();

    return vip && mask && (ipHost & mask) == (vip & mask);
}

static bool IsBroadcast(uint32_t ipHost)
{
    if (ipHost == relay::kBroadcast)
        return true;

    const uint32_t mask = g_mask.load();
    return mask && (ipHost & ~mask) == ~mask;
}

bool RelayPortalAddress(uint8_t proto, uint32_t vipHost, uint16_t port, sockaddr_in& out)
{
    if (!RelayActive())
        return false;

    Portal* p = GetPortal(proto, vipHost, port);
    if (!p)
        return false;

    out = Loopback(p->localPort);
    return true;
}

bool RelayPortalPeer(const sockaddr_in& addr, uint8_t& proto, uint32_t& vipHost, uint16_t& port)
{
    if (addr.sin_family != AF_INET || addr.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        return false;

    std::lock_guard<std::mutex> lk(g_portalMutex);
    auto it = g_portalsByLocalPort.find(ntohs(addr.sin_port));
    if (it == g_portalsByLocalPort.end())
        return false;

    proto   = it->second->proto;
    vipHost = it->second->vip;
    port    = it->second->port;
    return true;
}

bool RelayMapDatagramDestination(const sockaddr* to, int tolen, sockaddr_in& out)
{
    if (!to || tolen < static_cast<int>(sizeof(sockaddr_in)) || to->sa_family != AF_INET)
        return false;

    if (!g_udpOn || !RelayActive())
        return false;

    const auto*    in = reinterpret_cast<const sockaddr_in*>(to);
    const uint32_t ip = ntohl(in->sin_addr.s_addr);

    if (!RelayIsVirtualAddress(ip))
        return false;

    return RelayPortalAddress(relay::ProtoUdp, IsBroadcast(ip) ? relay::kBroadcast : ip,
        ntohs(in->sin_port), out);
}

bool RelayMapDatagramSource(sockaddr* from, int fromlen)
{
    if (!from || fromlen < static_cast<int>(sizeof(sockaddr_in)) || from->sa_family != AF_INET)
        return false;

    auto* in = reinterpret_cast<sockaddr_in*>(from);

    uint8_t  proto = 0;
    uint32_t vip   = 0;
    uint16_t port  = 0;

    if (!RelayPortalPeer(*in, proto, vip, port) || proto != relay::ProtoUdp || vip == relay::kBroadcast)
        return false;

    in->sin_addr.s_addr = htonl(vip);
    in->sin_port        = htons(port);
    return true;
}

bool RelayPrepareStreamConnect(SOCKET s, const sockaddr* dst, int dstlen, sockaddr_in& out)
{
    if (!dst || dstlen < static_cast<int>(sizeof(sockaddr_in)) || dst->sa_family != AF_INET)
        return false;

    if (!g_tcpOn || !RelayActive())
        return false;

    const auto*    in   = reinterpret_cast<const sockaddr_in*>(dst);
    const uint32_t ip   = ntohl(in->sin_addr.s_addr);
    const uint16_t port = ntohs(in->sin_port);

    if (!RelayIsVirtualAddress(ip) || IsBroadcast(ip))
        return false;

    if (ip == g_vip.load())
    {
        // Our own address: no relay involved, just loopback.
        std::lock_guard<std::mutex> lk(g_streamMapMutex);
        g_socketPeers[s] = *in;
        g_virtualLocal.insert(s);
        out = Loopback(port);
        return true;
    }

    const uint16_t gateway = g_gatewayPort.load();
    if (!gateway)
        return false;

    // The gateway identifies the connection by the game socket's local port,
    // so the socket must have one before connect() runs.
    uint16_t localPort = LocalPortOf(s);
    if (!localPort)
    {
        sockaddr_in local = Loopback(0);
        if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
            return false;
        localPort = LocalPortOf(s);
        if (!localPort)
            return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_streamMapMutex);
        g_pendingConnects[localPort] = PendingConnect{ s, ip, port };
        g_socketPeers[s] = *in;
        g_virtualLocal.insert(s);
    }

    out = Loopback(gateway);
    return true;
}

bool RelayMapStreamPeer(SOCKET s, sockaddr* addr, int addrlen)
{
    if (!addr || addrlen < static_cast<int>(sizeof(sockaddr_in)) || addr->sa_family != AF_INET)
        return false;

    auto* in = reinterpret_cast<sockaddr_in*>(addr);

    std::lock_guard<std::mutex> lk(g_streamMapMutex);

    auto peer = g_socketPeers.find(s);
    if (peer != g_socketPeers.end())
    {
        *in = peer->second;
        return true;
    }

    if (in->sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        return false;

    auto inbound = g_inboundPeers.find(ntohs(in->sin_port));
    if (inbound == g_inboundPeers.end())
        return false;

    *in = inbound->second;

    // Remember it against the accepted socket: the stream entry, and with it
    // the port mapping, goes away when the stream closes, but the game may
    // still ask the socket who it was talking to.
    if (s != INVALID_SOCKET)
    {
        g_socketPeers[s] = inbound->second;
        g_virtualLocal.insert(s);
    }
    return true;
}

bool RelayMapBind(SOCKET s, const sockaddr* name, int namelen, sockaddr_in& out)
{
    if (!name || namelen < static_cast<int>(sizeof(sockaddr_in)) || name->sa_family != AF_INET)
        return false;

    const uint32_t vip = g_vip.load();
    const auto*    in  = reinterpret_cast<const sockaddr_in*>(name);

    if (!vip || ntohl(in->sin_addr.s_addr) != vip)
        return false;

    out = *in;
    out.sin_addr.s_addr = htonl(INADDR_ANY);

    std::lock_guard<std::mutex> lk(g_streamMapMutex);
    g_virtualLocal.insert(s);
    return true;
}

bool RelayMapLocalName(SOCKET s, sockaddr* name, int namelen)
{
    if (!name || namelen < static_cast<int>(sizeof(sockaddr_in)) || name->sa_family != AF_INET)
        return false;

    const uint32_t vip = g_vip.load();
    if (!vip)
        return false;

    {
        std::lock_guard<std::mutex> lk(g_streamMapMutex);
        if (!g_virtualLocal.count(s))
            return false;
    }

    reinterpret_cast<sockaddr_in*>(name)->sin_addr.s_addr = htonl(vip);
    return true;
}

void RelayOnSocketClosed(SOCKET s)
{
    std::lock_guard<std::mutex> lk(g_streamMapMutex);

    g_socketPeers.erase(s);
    g_virtualLocal.erase(s);

    for (auto it = g_pendingConnects.begin(); it != g_pendingConnects.end();)
    {
        if (it->second.game == s) it = g_pendingConnects.erase(it);
        else                      ++it;
    }
}

bool RelayNameForAddress(uint32_t vipHost, std::wstring& name)
{
    if (!vipHost)
        return false;

    std::lock_guard<std::mutex> lk(g_statusMutex);

    if (vipHost == g_vip.load())
    {
        name = g_ownName;
        return true;
    }

    for (const PeerInfo& p : g_peers)
    {
        if (p.vip == vipHost)
        {
            name = p.name;
            return true;
        }
    }

    return false;
}

bool RelayAddressReachable(uint32_t vipHost)
{
    std::wstring ignored;
    return RelayActive() && RelayNameForAddress(vipHost, ignored);
}

DWORD RelayRoundTripMs()
{
    return g_serverRtt.load();
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

bool RelayConnect(const std::wstring& yamlOverrides)
{
    {
        std::lock_guard<std::mutex> lk(g_settingsMutex);

        RelaySettings next = g_settings;

        if (!yamlOverrides.empty())
        {
            std::wstring error;
            if (!ParseRelaySettings(WideToUtf8(yamlOverrides), next, &error))
            {
                SetLastErrorText(L"invalid relay options: " + error);
                return false;
            }
        }

        next.enabled = true;
        g_settings   = next;
        ApplyFeatureFlags(next);
    }

    if (!g_hooksWanted)
        InterposerLog(L"RELAY", L"connecting, but the socket hooks were not installed at startup; "
                                L"the game's traffic will not be relayed");

    {
        std::lock_guard<std::mutex> lk(g_commandMutex);
        g_commandReconnect  = true;
        g_commandDisconnect = false;
    }

    StartThread();
    Wake();
    return true;
}

void RelayDisconnect()
{
    {
        std::lock_guard<std::mutex> lk(g_settingsMutex);
        g_settings.enabled = false;
    }

    {
        std::lock_guard<std::mutex> lk(g_commandMutex);
        g_commandDisconnect = true;
        g_commandReconnect  = false;
    }

    Wake();
}

std::wstring RelayStatusJson()
{
    const auto     state = static_cast<RelayState>(g_state.load());
    const uint32_t vip   = g_vip.load();
    const uint32_t mask  = g_mask.load();

    std::wstring json = L"{";
    json += L"\"state\":" + std::to_wstring(static_cast<int>(state));
    json += L",\"stateName\":\"" + std::wstring(StateName(state)) + L"\"";
    json += L",\"clientId\":" + std::to_wstring(g_clientId.load());
    json += L",\"virtualAddress\":\"" + (vip ? IpToString(vip) : std::wstring()) + L"\"";
    json += L",\"netmask\":\"" + (mask ? IpToString(mask) : std::wstring()) + L"\"";
    json += L",\"udpActive\":" + std::wstring(g_udpActive ? L"true" : L"false");
    json += L",\"bytesSent\":" + std::to_wstring(g_bytesSent.load());
    json += L",\"bytesReceived\":" + std::to_wstring(g_bytesReceived.load());
    json += L",\"datagramsSent\":" + std::to_wstring(g_datagramsSent.load());
    json += L",\"datagramsReceived\":" + std::to_wstring(g_datagramsReceived.load());
    json += L",\"streamsOpen\":" + std::to_wstring(g_streamsOpen.load());

    std::lock_guard<std::mutex> lk(g_statusMutex);

    json += L",\"network\":\"" + JsonEscape(g_networkName) + L"\"";
    json += L",\"server\":\"" + JsonEscape(g_serverName) + L"\"";
    json += L",\"lastError\":\"" + JsonEscape(g_lastError) + L"\"";
    json += L",\"peers\":[";

    for (size_t i = 0; i < g_peers.size(); ++i)
    {
        if (i) json += L",";
        json += L"{\"clientId\":" + std::to_wstring(g_peers[i].clientId);
        json += L",\"virtualAddress\":\"" + IpToString(g_peers[i].vip) + L"\"";
        json += L",\"name\":\"" + JsonEscape(g_peers[i].name) + L"\"}";
    }

    json += L"]}";
    return json;
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) BOOL InterposerRelayGetStatus(InterposerRelayStatus* status)
{
    FORWARD_TO_PRIMARY(InterposerRelayGetStatus, status);

    if (!status || status->size < sizeof(InterposerRelayStatus))
        return FALSE;

    ZeroMemory(status, sizeof(*status));
    status->size = sizeof(InterposerRelayStatus);

    const uint32_t vip  = g_vip.load();
    const uint32_t mask = g_mask.load();

    status->state             = g_state.load();
    status->clientId          = g_clientId.load();
    status->udpActive         = g_udpActive ? 1 : 0;
    status->bytesSent         = g_bytesSent.load();
    status->bytesReceived     = g_bytesReceived.load();
    status->datagramsSent     = g_datagramsSent.load();
    status->datagramsReceived = g_datagramsReceived.load();
    status->streamsOpen       = g_streamsOpen.load();

    if (vip)  wcsncpy_s(status->virtualAddress, IpToString(vip).c_str(), _TRUNCATE);
    if (mask) wcsncpy_s(status->netmask, IpToString(mask).c_str(), _TRUNCATE);

    std::lock_guard<std::mutex> lk(g_statusMutex);
    wcsncpy_s(status->network, g_networkName.c_str(), _TRUNCATE);
    wcsncpy_s(status->server, g_serverName.c_str(), _TRUNCATE);
    wcsncpy_s(status->lastError, g_lastError.c_str(), _TRUNCATE);
    status->peerCount = static_cast<DWORD>(g_peers.size());
    return TRUE;
}

extern "C" __declspec(dllexport) DWORD InterposerRelayEnumPeers(
    InterposerRelayPeer* buffer, DWORD capacity, DWORD* outCount)
{
    FORWARD_TO_PRIMARY(InterposerRelayEnumPeers, buffer, capacity, outCount);

    std::lock_guard<std::mutex> lk(g_statusMutex);

    const DWORD total   = static_cast<DWORD>(g_peers.size());
    DWORD       written = 0;

    for (; buffer && written < capacity && written < total; ++written)
    {
        InterposerRelayPeer& out = buffer[written];
        ZeroMemory(&out, sizeof(out));
        out.clientId = g_peers[written].clientId;
        wcsncpy_s(out.virtualAddress, IpToString(g_peers[written].vip).c_str(), _TRUNCATE);
        wcsncpy_s(out.name, g_peers[written].name.c_str(), _TRUNCATE);
    }

    if (outCount)
        *outCount = written;

    return total;
}

extern "C" __declspec(dllexport) BOOL InterposerRelayConnect(const wchar_t* yamlOverrides)
{
    FORWARD_TO_PRIMARY(InterposerRelayConnect, yamlOverrides);

    return RelayConnect(yamlOverrides ? std::wstring(yamlOverrides) : std::wstring()) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) void InterposerRelayDisconnect()
{
    FORWARD_TO_PRIMARY(InterposerRelayDisconnect);

    RelayDisconnect();
}
