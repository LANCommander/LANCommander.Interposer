#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <cstdint>

// ---------------------------------------------------------------------------
// IPX emulation over the network relay. See ipx.cpp.
//
// Windows has shipped without an IPX stack since Vista, so socket(AF_IPX, ...)
// fails and an IPX game finds no network at all. These functions stand in for
// it: an IPX socket is really a UDP socket bound to loopback, and every call
// that carries a SOCKADDR_IPX or an NSPROTO_IPX option is answered here, called
// from the hooks in network.cpp while they hold a NetHookScope.
//
// Every function returns what the Winsock call it replaces would, and sets
// WSAGetLastError on failure.
// ---------------------------------------------------------------------------

// True when IPX emulation is configured. Decided once, at hook installation.
bool IpxWanted();

// True when s was created by IpxSocket.
bool IpxIsSocket(SOCKET s);

// socket() / WSASocket(). handled is false when the request is not for IPX, in
// which case the caller passes it through untouched. protocolInfo may be null.
SOCKET IpxSocket(int af, int type, int protocol, const WSAPROTOCOL_INFOW* protocolInfo,
                 DWORD wsaFlags, bool useWsaSocket, bool& handled);

int IpxBind(SOCKET s, const sockaddr* name, int namelen);
int IpxConnect(SOCKET s, const sockaddr* name, int namelen);
int IpxGetSockName(SOCKET s, sockaddr* name, int* namelen);
int IpxGetPeerName(SOCKET s, sockaddr* name, int* namelen);

int IpxSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen);
int IpxRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen);

// The scatter/gather forms. Overlapped receives are refused with WSAEOPNOTSUPP:
// the loopback hop carries a one-byte packet-type prefix that has to be
// stripped, and an overlapped receive completes after we have returned.
int IpxWSASendTo(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD sent, DWORD flags,
                 const sockaddr* to, int tolen, LPWSAOVERLAPPED overlapped,
                 LPWSAOVERLAPPED_COMPLETION_ROUTINE completion);
int IpxWSARecvFrom(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD received, LPDWORD flags,
                   sockaddr* from, LPINT fromlen, LPWSAOVERLAPPED overlapped,
                   LPWSAOVERLAPPED_COMPLETION_ROUTINE completion);

// NSPROTO_IPX-level options, plus SO_PROTOCOL_INFO. handled is false for an
// option the real UDP socket can answer itself.
int IpxGetSockOpt(SOCKET s, int level, int optname, char* optval, int* optlen, bool& handled);
int IpxSetSockOpt(SOCKET s, int level, int optname, const char* optval, int optlen, bool& handled);

// FIONREAD counts the packet-type prefix; this takes it back off.
void IpxAdjustReadable(SOCKET s, long cmd, u_long* argp);

void IpxOnSocketClosed(SOCKET s);

// For the relay thread. The IPX socket number bound to a loopback UDP port and
// the other way round, 0 when there is none.
uint16_t IpxSocketNumberForUdpPort(uint16_t udpPort);
uint16_t IpxUdpPortForSocketNumber(uint16_t socketNumber);

// WSAEnumProtocols / wsock32 EnumProtocols with the emulated IPX protocol
// appended, so a game that checks for IPX before creating a socket finds it.
// orig is the real function; the signature matches the export.
using FnWSAEnumProtocolsW = int (WSAAPI*)(LPINT, LPWSAPROTOCOL_INFOW, LPDWORD);
using FnWSAEnumProtocolsA = int (WSAAPI*)(LPINT, LPWSAPROTOCOL_INFOA, LPDWORD);
using FnEnumProtocolsW    = int (WSAAPI*)(LPINT, LPVOID, LPDWORD);
using FnEnumProtocolsA    = int (WSAAPI*)(LPINT, LPVOID, LPDWORD);

int IpxEnumProtocolsW(FnWSAEnumProtocolsW orig, LPINT protocols, LPWSAPROTOCOL_INFOW buffer, LPDWORD length);
int IpxEnumProtocolsA(FnWSAEnumProtocolsA orig, LPINT protocols, LPWSAPROTOCOL_INFOA buffer, LPDWORD length);
int IpxLegacyEnumProtocols(void* orig, bool wide, LPINT protocols, LPVOID buffer, LPDWORD length);
