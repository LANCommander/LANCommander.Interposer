---
sidebar_label: Network Relay
sidebar_position: 12
---

# Network Relay

LAN games expect their players to share a LAN. They find each other by broadcasting, connect to the private addresses they find, and an old enough game does it all over IPX — a protocol Windows has not shipped since XP. Players in different houses usually reach for a VPN to fake that LAN, which means installing a virtual adapter on every machine and running a VPN server.

The network relay does it at the API level instead. Every player's game is given an address on a **virtual LAN**, and its broadcasts, datagrams and TCP connections to other players' virtual addresses travel through a **relay server** — hosted by a LANCommander server, or the standalone test server in this repository. Nothing is installed on the player's machine, no adapter is added, and the game's other traffic (master servers, updates, the internet) is untouched.

```yaml
NetworkRelay:
  Enabled: true
  Server: 'lancommander.example.com'
  Network: 'friday-night'
```

## How It Works

When the relay is enabled the DLL connects to the server, joins the named **network**, and is handed a virtual address — `10.13.37.5`, say. Players in the same network see each other; different networks are isolated, even on one server.

From then on the game sees itself as `10.13.37.5`:

| The game asks | It is told |
|---|---|
| `gethostbyname`, `getaddrinfo`, `GetAddrInfoEx` or `WSAAsyncGetHostByName` of its own host name | the virtual address |
| `GetAdaptersInfo`, `GetAdaptersAddresses`, `SIO_GET_INTERFACE_LIST`, `GetIpAddrTable`, `GetIfTable` | a **LANCommander Relay** adapter carrying the virtual address, listed first, ahead of the real adapters |
| `GetBestInterface` / `GetBestRoute` for another player's address | the virtual adapter |
| `getsockname` on a socket it bound to the virtual address | the virtual address |
| `gethostbyaddr` / `getnameinfo` / `WSAAsyncGetHostByAddr` of another player's address | that player's name — answered locally, so a reverse lookup never stalls on real DNS |
| `IcmpSendEcho` (a ping) to another player's address | a reply, with twice the measured round trip to the relay server, since ICMP itself is not relayed |

And everything it sends to the virtual network goes through the relay:

| The game does | What happens |
|---|---|
| `sendto` a virtual address, the subnet broadcast, or `255.255.255.255` | the datagram is relayed to that player, or to every player in the network |
| `recvfrom` | datagrams from other players arrive from their virtual address and port |
| `connect` to a virtual address | the connection is tunnelled to that player's game, which sees it arrive from the caller's virtual address in `accept` |
| `bind` to its own virtual address | the socket is bound, though no real adapter has that address |
| `socket(AF_IPX, SOCK_DGRAM, NSPROTO_IPX)` | an emulated IPX socket, whose datagrams — broadcasts included — are relayed between players |

Traffic to any other address is not touched. A game can talk to its master server and its players at the same time.

### Under the hood

Each game socket stays a real Windows socket, so blocking, non-blocking, `select`, `WSAAsyncSelect`, `WSAEventSelect` and overlapped I/O all behave exactly as they always did. The hooks only translate addresses:

- Every remote endpoint the game sends to is stood in for by a **portal** — a UDP socket on `127.0.0.1` owned by the DLL. A datagram for `10.13.37.7:27960` is sent to that endpoint's portal over loopback; the relay thread picks it up there and forwards it to the server. Datagrams arriving from `10.13.37.7:27960` are delivered to the game *from* that portal, and the receive hooks show the game the virtual address instead of `127.0.0.1`.
- A TCP `connect` to a virtual address is redirected to a local **gateway** the relay thread listens on, which opens a stream through the server to the other player; there the relay thread connects to the game's listening port on loopback.
- An emulated IPX socket is a UDP socket on loopback. Its IPX node is derived from the virtual address (`02:00:0a:0d:25:05` for `10.13.37.5`), and its IPX socket number is mapped to the UDP port behind it.

Datagrams travel over UDP to the server once the server has answered the DLL over UDP — over the TCP connection until then, and again if UDP stops getting through. TCP tunnels share the TCP connection.

## Options

| Option | Default | Meaning |
|---|---|---|
| `Enabled` | `false` | Connect to the relay. |
| `Server` | | The relay server, as `host` or `host:port`. |
| `Port` | `41300` | The port when `Server` does not name one. The server uses it for TCP and UDP. |
| `Network` | | The network to join. |
| `Ticket` | | An authentication ticket, when the server requires one. A launcher normally supplies it per session. |
| `Address` | | A virtual IPv4 address to ask for. Granted when it is free. |
| `UDP` | `true` | Relay UDP datagrams and broadcasts. |
| `TCP` | `true` | Tunnel TCP connections. |
| `IPX` | `true` | Emulate IPX. |
| `LoopbackBroadcasts` | `true` | Deliver the game's own broadcasts back to it, as a real LAN does. |
| `AddressTimeout` | `5000` | Milliseconds a game asking for its own address waits for the relay to assign one. After that it is told its real address. |

Other players see this player under their username: `Player.Username`, or the Windows account name when that is empty (a launcher sets it per launch with `InterposerService.Username`). There is deliberately no separate relay name, so the name in the player list always matches the one the game itself uses.

The DLL keeps its address across reconnects: if the server goes away it retries, with backoff, and asks for the same address again, so addresses a game has already advertised stay good.

:::tip Choosing addresses
The server hands addresses out of a private range (`10.13.37.0/24` by default) on purpose: many games only list LAN servers whose address is private. Addresses ending in `.0` or `.255` are never assigned, because plenty of old games treat them as special whatever the netmask says.
:::

## Launcher Integration

A launcher drives the relay through `LANCommander.Interposer.NET`. Options set on `InterposerService.Relay` override `Config.yml` key by key for that launch, and the control methods work on the running game:

```csharp
using var interposer = new InterposerService
{
    Username = profile.Alias,   // what the game is told, and what other players see
    Relay    = new RelayOptions
    {
        Enabled    = true,
        Server     = "lancommander.example.com",
        Network    = session.Id,
        Ticket     = session.RelayTicket,
    },
};

interposer.RelayStateChanged += (_, e) => Log($"relay {e.State} {e.VirtualAddress}");
interposer.RelayPeerJoined   += (_, e) => Log($"{e.Name} joined at {e.VirtualAddress}");
interposer.RelayPeerLeft     += (_, e) => Log($"{e.Name} left");

var game = interposer.Start(new ProcessStartInfo(gameExe));

RelayStatus status = await interposer.GetRelayStatusAsync();   // state, address, peers, counters
await interposer.ConnectRelayAsync(new RelayOptions { Network = "another-lobby" });
await interposer.DisconnectRelayAsync();
```

| Member | Purpose |
|---|---|
| `Relay` | Options for the next `Start` / `Inject`. Setting it at all — even with `Enabled = false` — installs the socket hooks, which the DLL can only do at startup. Set it whenever the launcher may want to connect later. |
| `ConnectRelayAsync(options)` | Connect or reconnect, applying new options on top of the current ones. |
| `DisconnectRelayAsync()` | Leave the network and stop retrying. The address stays assigned for the next connect. |
| `GetRelayStatusAsync()` | State, virtual address, netmask, network, server, peers, UDP/TCP path, traffic counters and the last error. |
| `RelayStateChanged` | `Connecting`, `Connected`, `Reconnecting`, `Failed` (refused by the server) or `Disabled`. |
| `RelayPeerJoined` / `RelayPeerLeft` | Other players arriving and leaving. On connecting, `RelayPeerJoined` fires once for each player already present. |
| `PingAsync()` / `QueryConfigStringAsync(path)` | Check the control pipe, and read the game's `Config.yml` from outside the process. |

The control methods reach the game over a named pipe the service creates before injecting, so they work for processes the same `InterposerService` started or injected. Inside the game process — a plugin, or managed code loaded there — `GetRelayStatus()`, `ConnectRelay()` and `DisconnectRelay()` call the DLL directly, and native plugins can use the `InterposerRelay*` exports declared in `relay.h`.

## Hosting the Server

The server is the `LANCommander.Interposer.Relay` library. A LANCommander server hosts a `RelayServer` from a background service and supplies an `IRelayAuthenticator` that checks the ticket its launcher handed out — and can override the network and player name, so the ticket rather than the client decides who sees whom:

```csharp
var relay = new RelayServer(
    new RelayServerOptions { Port = 41300, VirtualSubnet = "10.13.37.0/24" },
    new SessionTicketAuthenticator(sessions),
    logger);

relay.ClientJoined += (_, e) => ...;
relay.ClientLeft   += (_, e) => ...;

await relay.StartAsync();
```

| Option | Default | Meaning |
|---|---|---|
| `BindAddress` | `0.0.0.0` | Address to listen on. |
| `Port` | `41300` | TCP port, and UDP port unless `UdpPort` says otherwise. |
| `UdpPort` | `Port` | UDP port for datagrams. Clients learn it from the server. |
| `DisableUdp` | `false` | Carry datagrams over TCP only, for hosts that cannot open a UDP port. |
| `VirtualSubnet` | `10.13.37.0/24` | The range addresses are handed out of. Every network shares it; networks are isolated from each other. |
| `IpxNetwork` | `1` | The IPX network number emulated IPX sockets report. |
| `MaxClientsPerNetwork` | `64` | Members per network. |
| `MaxStreamsPerClient` | `256` | Tunnelled TCP connections per client. |
| `ClientTimeout` | 45 s | A client silent for this long is dropped. Clients ping every 10 s. |

`GetNetworks()`, `GetClients()` and `Disconnect(clientId)` give a host an admin view.

### The test server

`LANCommander.Interposer.Relay.Server` is a standalone host for testing without a LANCommander server:

```
LANCommander.Interposer.Relay.Server.exe --port 41300 --secret letmein
```

| Flag | Meaning |
|---|---|
| `--port <n>` | TCP port, and UDP unless `--udp-port` is given. `0` picks a free one. |
| `--udp-port <n>` | A separate UDP port. |
| `--no-udp` | Datagrams over TCP only. |
| `--bind <address>` | Address to listen on. |
| `--subnet <cidr>` | Virtual subnet. |
| `--secret <text>` | Require `NetworkRelay.Ticket` to equal this. Without it everyone is let in. |
| `--status <s>` | Seconds between member listings (`0` = never). |

The first line it prints is `LISTENING <tcp-port> <udp-port>`, for scripts.

## Logging

Set `Logging.Relay: true`:

```
2026-10-19 20:11:47  [RELAY]           lancommander.example.com  ->  connected; joining 'friday-night'
2026-10-19 20:11:47  [RELAY]           10.13.37.5/24  ->  assigned in network 'friday-night'
2026-10-19 20:11:47  [RELAY STATE]     Connected
2026-10-19 20:11:47  [RELAY PEER]      10.13.37.7  ->  joined: Alice
2026-10-19 20:11:48  [RELAY]           UDP path confirmed
2026-10-19 20:12:02  [RELAY STREAM]    10.13.37.7:27970  ->  connecting
2026-10-19 20:12:02  [RELAY STREAM]    10.13.37.7:27970  ->  connected
```

`Level: Debug` adds `[RELAY PORTAL]` and `[IPX BIND]`; `Level: Trace` adds a line per datagram. See [Logging](/Interposer/Logging#network-relay-operations).

## Limitations

- **IPv4 only.** A game that talks IPv6 to its peers is not relayed.
- **Multicast is not relayed.** Only unicast and broadcast datagrams are.
- **SPX is not emulated** — only IPX datagrams. `socket()` refuses SPX the way a machine without IPX would.
- **Emulated IPX sockets refuse overlapped receives.** Era-appropriate games use blocking or `select`-driven receives, which work.
- **An overlapped `WSARecvFrom` that completes later** has its source address translated when the game learns it completed — through `WSAGetOverlappedResult`, `GetOverlappedResult(Ex)`, `GetQueuedCompletionStatus(Ex)` or a completion routine. A game that only polls `HasOverlappedIoCompleted` calls none of them and sees the portal's `127.0.0.1` address instead; replies sent there still reach the right player.
- **Pings are answered only when synchronous.** An `IcmpSendEcho2` with an event or an APC routine is passed through, and a virtual address then never answers.
- **`WSALookupServiceBegin` name queries** are not mapped. Of the games surveyed only embedded Chromium (`libcef.dll`) used them, for web content.
- **A refused tunnelled connection looks like a reset,** not `WSAECONNREFUSED`: the game's `connect` completes against the local gateway before the other end has answered.
- **Two copies of a game on one machine** cannot both use the same port, and a broadcast one of them loops back to itself can reach the other from an address it does not recognise. Set `LoopbackBroadcasts: false` for that kind of testing.
- **Traffic is not encrypted.** The ticket authenticates a client to the server; the game traffic itself travels as the game sent it.

## Protocol

For anyone implementing a client or server of their own. Version 1; every integer is big-endian, a string is a `u16` byte count followed by UTF-8. The reference implementations are `dll\src\relay_protocol.h` and `LANCommander.Interposer.Relay\Protocol\`.

**TCP** carries frames: `u16 length` (counting the type byte), `u8 type`, body. **UDP** carries packets: `u32 clientId`, `u32 udpToken`, `u8 type`, body.

| Type | Name | Direction | Body |
|---|---|---|---|
| `0x01` | Hello | C→S | `u8 version`, `str network`, `str ticket`, `str player`, `u32 requestedAddress` |
| `0x02` | Welcome | S→C | `u32 clientId`, `u32 udpToken`, `u32 address`, `u32 netmask`, `u16 udpPort` (0 = no UDP), `u32 ipxNetwork` |
| `0x03` | Reject | S→C | `u8 code` (1 protocol, 2 unauthorized, 3 network full, 4 bad request), `str message` |
| `0x04` | PeerJoined | S→C | `u32 clientId`, `u32 address`, `str name` |
| `0x05` | PeerLeft | S→C | `u32 clientId` |
| `0x06` / `0x07` | Ping / Pong | both | `u32 nonce` |
| `0x10` | Datagram | both, TCP or UDP | `u8 proto` (0 UDP, 1 IPX), `u32 peerAddress`, `u16 peerPort`, `u16 localPort`, `u8 ipxType`, payload |
| `0x20` | StreamOpen | both | `u32 streamId`, `u32 peerAddress`, `u16 peerPort`, `u16 localPort` |
| `0x21` | StreamAccept | both | `u32 streamId` |
| `0x22` | StreamReject | both | `u32 streamId`, `u8 reason` (1 no such peer, 2 refused, 3 limit) |
| `0x23` | StreamData | both | `u32 streamId`, data |
| `0x24` | StreamClose | both | `u32 streamId` — the sender has finished sending |
| `0x30` | Leave | C→S | |
| `0x41` | UdpHello | C→S, UDP | |
| `0x42` | UdpHelloAck | S→C, UDP | |

"Peer" and "local" are from the reader's point of view. A client sending a datagram names the destination as the peer (`0xFFFFFFFF` to broadcast) and its own port as local; the server rewrites the body so the receiver sees the sender as the peer and its own port as local. Stream ids work the same way: a client numbers the streams it opens below `0x80000000`, the server numbers the ones it delivers from `0x80000000` up, and translates between the two.

A client sends `UdpHello` until it is acknowledged, switching its datagrams to UDP when it is, and every 15 s after that to keep a NAT mapping open. The server delivers datagrams to a client over UDP once that client has sent one over UDP — proof the client heard the acknowledgement — and goes back to TCP if the client does.
