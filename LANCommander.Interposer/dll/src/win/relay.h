#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <string>

#include "relay_settings.h"

// ---------------------------------------------------------------------------
// Network relay -- carries a game's LAN traffic through a LANCommander relay
// server instead of a VPN. See relay.cpp for the mechanism and
// LANCommander.Interposer.Documentation\NetworkRelay.md for the user's view.
//
// The hooks in network.cpp never touch the relay connection. They only
// translate addresses: a datagram for a virtual address is sent to a loopback
// "portal" socket the relay thread owns, which forwards it, and a datagram the
// relay delivers arrives from that portal, whose address the receive hooks
// rewrite back to the virtual one. Every game socket therefore stays a real
// socket, so blocking, non-blocking, select, WSAAsyncSelect and overlapped I/O
// keep working without being reimplemented.
// ---------------------------------------------------------------------------

// Connection state, as reported to the status export, the event pipe and the
// .NET RelayState enum. The values are part of that contract.
enum class RelayState : int
{
    Disabled     = 0, // NetworkRelay.Enabled is false, or Disconnect was called
    Connecting   = 1, // first attempt in progress
    Connected    = 2, // Welcome received; a virtual address is assigned
    Reconnecting = 3, // lost the server; retrying with the previous address
    Failed       = 4, // rejected by the server -- not retried until reconfigured
};

// Startup, from InstallHooks after LoadConfig and before InstallNetworkHooks.
// Overlays the launcher's override mapping onto the Config.yml settings and
// starts the relay thread. The thread does not touch the network until the
// loader lock is released, so this is safe from DllMain.
void InitRelay();

// Shutdown. processTerminating is DllMain's lpvReserved != nullptr: the OS has
// already killed our threads, so there is nothing to wait for.
void ShutdownRelay(bool processTerminating);

// True when the socket-level hooks are needed at all: the relay is enabled, or
// a launcher passed relay options (even disabled ones), which means it may
// connect later through the control pipe. Hooks cannot be added after the fact
// without racing the game's own socket calls, so this is decided once.
bool RelayHooksWanted();

// ---- address translation, called from the hooks in network.cpp ------------

// True once a virtual address has been assigned (it is kept across reconnects).
bool RelayActive();

// The assigned virtual address and netmask in host byte order, 0 when none.
uint32_t RelayVirtualIp();
uint32_t RelayNetmask();
uint32_t RelayIpxNetwork();

bool RelayUdpEnabled();
bool RelayTcpEnabled();
bool RelayIpxEnabled();

// Block until an address is assigned or the configured AddressTimeout passes.
// Used by the local-address hooks (gethostbyname of our own name, adapter
// enumeration) so a game that asks before the first Welcome still sees the
// virtual address. Returns RelayActive().
bool RelayWaitForAddress();

// True when ipHost (host byte order) is inside the virtual subnet, including
// its directed broadcast, or is the limited broadcast 255.255.255.255.
bool RelayIsVirtualAddress(uint32_t ipHost);

// The loopback address of the portal that stands in for (proto, vip, port), in
// out. vip may be relay::kBroadcast. Creates the portal on first use. Returns
// false when the relay is not active or the portal could not be created.
bool RelayPortalAddress(uint8_t proto, uint32_t vipHost, uint16_t port, sockaddr_in& out);

// If addr is a portal's loopback address, replace it with the virtual endpoint
// the portal stands for and return true. proto receives the portal's protocol.
bool RelayPortalPeer(const sockaddr_in& addr, uint8_t& proto, uint32_t& vipHost, uint16_t& port);

// UDP: translate a sendto/connect destination. Returns true and fills out with
// the portal address when `to` is a virtual address.
bool RelayMapDatagramDestination(const sockaddr* to, int tolen, sockaddr_in& out);

// UDP: rewrite a portal source address back to the virtual endpoint in place.
bool RelayMapDatagramSource(sockaddr* from, int fromlen);

// TCP: called before connect(). When dst is a virtual address, makes sure the
// socket is bound, records where it was going, and fills out with the address
// to connect to instead (the stream gateway, or loopback for our own address).
bool RelayPrepareStreamConnect(SOCKET s, const sockaddr* dst, int dstlen, sockaddr_in& out);

// TCP: rewrite the peer address of an accepted or connected stream socket in
// place, when it is one of ours.
bool RelayMapStreamPeer(SOCKET s, sockaddr* addr, int addrlen);

// bind(): a game binding to its virtual address is bound to INADDR_ANY instead,
// since no real interface carries that address. Returns true when rewritten.
bool RelayMapBind(SOCKET s, const sockaddr* name, int namelen, sockaddr_in& out);

// getsockname(): report the virtual address for a socket whose bind was
// rewritten, or which the relay bound on its behalf. Rewrites in place.
bool RelayMapLocalName(SOCKET s, sockaddr* name, int namelen);

// Forget per-socket state. Called from the closesocket hook.
void RelayOnSocketClosed(SOCKET s);

// The player name behind a virtual address -- ours, or a peer's -- for the
// reverse-lookup hooks, which answer from it instead of asking real DNS about
// an address it has never heard of. False when nobody has that address.
bool RelayNameForAddress(uint32_t vipHost, std::wstring& name);

// True when vipHost is our own address or a peer's. The ICMP hooks answer an
// echo to such an address themselves, since ICMP is not relayed.
bool RelayAddressReachable(uint32_t vipHost);

// Round trip to the relay server in milliseconds, measured by the control
// connection's pings. A peer is reported as twice that away.
DWORD RelayRoundTripMs();

// ---- launcher control -------------------------------------------------------

// Apply new settings (a NetworkRelay YAML map, as the override mapping holds)
// on top of the current ones and (re)connect. Empty text reconnects as is.
bool RelayConnect(const std::wstring& yamlOverrides);

// Drop the connection and stop retrying. Addresses stay assigned so sockets the
// game already holds keep their meaning if it reconnects.
void RelayDisconnect();

// A JSON snapshot of the state, for the control pipe.
std::wstring RelayStatusJson();

// The in-process control exports (InterposerRelayGetStatus, ...EnumPeers,
// ...Connect, ...Disconnect) and their structs are declared in the public header.
#include "LANCommander.Interposer.h"
