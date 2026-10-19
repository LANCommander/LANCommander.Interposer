using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using LANCommander.Interposer.Relay.Internal;
using LANCommander.Interposer.Relay.Protocol;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace LANCommander.Interposer.Relay
{
    /// <summary>
    /// The relay server the Interposer's network relay connects to. Each client
    /// joins a named network, is handed a virtual LAN address in it, and from then
    /// on its game's broadcasts, datagrams and TCP connections reach the other
    /// members of that network as though they shared a LAN.
    /// <para>
    /// Clients hold one TCP connection, for control messages and tunnelled game
    /// streams, and send datagrams over UDP once both directions are proven to work
    /// (over the TCP connection until then).
    /// </para>
    /// <para>
    /// Host it by calling <see cref="StartAsync"/> and <see cref="StopAsync"/> --
    /// from an <c>IHostedService</c> in a LANCommander server, or a console app.
    /// </para>
    /// </summary>
    public sealed class RelayServer : IAsyncDisposable
    {
        // SIO_UDP_CONNRESET: without it, an ICMP port-unreachable from a client that
        // went away fails the next ReceiveFrom on the shared socket.
        private const int SioUdpConnReset = unchecked((int)0x9800000C);

        private readonly RelayServerOptions _options;
        private readonly IRelayAuthenticator _authenticator;
        private readonly ILogger _logger;
        private readonly uint _subnet;
        private readonly uint _mask;

        private readonly ConcurrentDictionary<string, RelayNetwork> _networks = new(StringComparer.OrdinalIgnoreCase);
        private readonly ConcurrentDictionary<uint, RelayClient> _clients = new();
        private readonly ConcurrentDictionary<Task, byte> _connections = new();

        private TcpListener? _listener;
        private Socket? _udp;
        private CancellationTokenSource? _cts;
        private Task? _acceptLoop;
        private Task? _udpLoop;
        private Task? _sweeper;
        private int _nextClientId;

        /// <summary>Creates a server. Nothing listens until <see cref="StartAsync"/>.</summary>
        /// <param name="options">Server configuration.</param>
        /// <param name="authenticator">Decides who may join; defaults to letting everyone in.</param>
        /// <param name="logger">Optional logger.</param>
        public RelayServer(RelayServerOptions options, IRelayAuthenticator? authenticator = null, ILogger<RelayServer>? logger = null)
        {
            _options       = options ?? throw new ArgumentNullException(nameof(options));
            _authenticator = authenticator ?? new AllowAllRelayAuthenticator();
            _logger        = (ILogger?)logger ?? NullLogger.Instance;

            (_subnet, _mask) = Addressing.ParseCidr(options.VirtualSubnet);
        }

        /// <summary>The TCP endpoint actually bound, once started.</summary>
        public IPEndPoint? TcpEndPoint { get; private set; }

        /// <summary>The UDP endpoint actually bound, once started; null when UDP is disabled.</summary>
        public IPEndPoint? UdpEndPoint { get; private set; }

        /// <summary>True between <see cref="StartAsync"/> and <see cref="StopAsync"/>.</summary>
        public bool IsRunning => _cts is { IsCancellationRequested: false };

        /// <summary>Raised after a client has joined a network and been told its address.</summary>
        public event EventHandler<RelayClientEventArgs>? ClientJoined;

        /// <summary>Raised after a client has left, for whatever reason.</summary>
        public event EventHandler<RelayClientEventArgs>? ClientLeft;

        /// <summary>Bind the ports and start accepting clients.</summary>
        public Task StartAsync(CancellationToken cancellationToken = default)
        {
            if (_cts != null)
                throw new InvalidOperationException("The relay server is already running.");

            _cts = new CancellationTokenSource();

            _listener = new TcpListener(_options.BindAddress, _options.Port);
            _listener.Start();
            TcpEndPoint = (IPEndPoint)_listener.LocalEndpoint;

            if (!_options.DisableUdp)
            {
                _udp = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);

                if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                    _udp.IOControl(SioUdpConnReset, new byte[4], null);

                // The configured port, not the TCP port actually bound: with Port 0
                // that number is only known to be free for TCP, and UDP may find it
                // taken or inside a range Windows reserves. Clients learn the UDP
                // port from Welcome, so the two need not match.
                _udp.Bind(new IPEndPoint(_options.BindAddress, _options.UdpPort ?? _options.Port));
                UdpEndPoint = (IPEndPoint)_udp.LocalEndPoint!;
                _udpLoop = Task.Run(() => RunUdpAsync(_cts.Token));
            }

            _acceptLoop = Task.Run(() => RunAcceptAsync(_cts.Token));
            _sweeper    = Task.Run(() => RunSweeperAsync(_cts.Token));

            _logger.LogInformation("Relay listening on TCP {Tcp}, UDP {Udp}, subnet {Subnet}",
                TcpEndPoint, UdpEndPoint?.ToString() ?? "disabled", _options.VirtualSubnet);

            return Task.CompletedTask;
        }

        /// <summary>Disconnect every client and release the ports.</summary>
        public async Task StopAsync(CancellationToken cancellationToken = default)
        {
            if (_cts == null)
                return;

            _cts.Cancel();
            _listener?.Stop();
            _udp?.Dispose();

            foreach (var client in _clients.Values)
                client.Close();

            var pending = new List<Task>(_connections.Keys);
            if (_acceptLoop != null) pending.Add(_acceptLoop);
            if (_udpLoop != null)    pending.Add(_udpLoop);
            if (_sweeper != null)    pending.Add(_sweeper);

            try
            {
                await Task.WhenAll(pending).WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) { }

            _cts.Dispose();
            _cts = null;
            _listener = null;
            _udp = null;
        }

        /// <inheritdoc />
        public async ValueTask DisposeAsync() => await StopAsync().ConfigureAwait(false);

        /// <summary>Every network that currently has members.</summary>
        public IReadOnlyList<RelayNetworkInfo> GetNetworks()
            => _networks.Values
                .Select(n => new RelayNetworkInfo(n.Name, n.Members().Select(c => c.Snapshot()).ToArray()))
                .Where(n => n.Clients.Count > 0)
                .ToArray();

        /// <summary>Every connected client, across all networks.</summary>
        public IReadOnlyList<RelayClientInfo> GetClients()
            => _clients.Values.Select(c => c.Snapshot()).ToArray();

        /// <summary>Drop a client. Returns false when no such client is connected.</summary>
        public bool Disconnect(uint clientId)
        {
            if (!_clients.TryGetValue(clientId, out var client))
                return false;

            client.Close();
            return true;
        }

        // ---- TCP ---------------------------------------------------------------------

        private async Task RunAcceptAsync(CancellationToken ct)
        {
            while (!ct.IsCancellationRequested)
            {
                TcpClient tcp;

                try
                {
                    tcp = await _listener!.AcceptTcpClientAsync(ct).ConfigureAwait(false);
                }
                catch (OperationCanceledException) { break; }
                catch (ObjectDisposedException) { break; }
                catch (SocketException ex)
                {
                    _logger.LogDebug(ex, "Accept failed");
                    continue;
                }

                var task = Task.Run(() => HandleConnectionAsync(tcp, ct));
                _connections.TryAdd(task, 0);
                _ = task.ContinueWith(t => _connections.TryRemove(t, out _), TaskScheduler.Default);
            }
        }

        private async Task HandleConnectionAsync(TcpClient tcp, CancellationToken ct)
        {
            RelayClient? client = null;
            var reason = "disconnected";

            try
            {
                tcp.NoDelay = true;
                var stream = tcp.GetStream();
                var remote = (IPEndPoint)tcp.Client.RemoteEndPoint!;

                using (var handshake = CancellationTokenSource.CreateLinkedTokenSource(ct))
                {
                    handshake.CancelAfter(_options.HandshakeTimeout);
                    client = await HandshakeAsync(stream, remote, handshake.Token).ConfigureAwait(false);
                }

                if (client == null)
                    return;

                using var closing = client.Closing.Register(tcp.Dispose);
                var writer = client.RunWriterAsync(stream);

                ClientJoined?.Invoke(this, new RelayClientEventArgs(client.Snapshot()));

                while (!client.Closing.IsCancellationRequested)
                {
                    var frame = await ReadFrameAsync(stream, client.Closing).ConfigureAwait(false);
                    if (frame == null)
                        break;

                    client.Touch();
                    client.CountReceived(frame.Value.Body.Length + 3);

                    if (!await DispatchAsync(client, frame.Value.Type, frame.Value.Body).ConfigureAwait(false))
                    {
                        reason = "left";
                        break;
                    }
                }

                if (client.Idle > _options.ClientTimeout)
                    reason = "timed out";

                client.Close();
                await writer.ConfigureAwait(false);
            }
            catch (OperationCanceledException) { }
            catch (IOException) { }
            catch (SocketException) { }
            catch (ObjectDisposedException) { }
            catch (Exception ex)
            {
                reason = "error: " + ex.Message;
                _logger.LogWarning(ex, "Relay connection failed");
            }
            finally
            {
                if (client != null)
                {
                    client.Close();
                    _clients.TryRemove(client.Id, out _);

                    if (client.Network.Leave(client))
                        _networks.TryRemove(new KeyValuePair<string, RelayNetwork>(client.Network.Name, client.Network));

                    _logger.LogInformation("{Player} ({Address}) left network {Network}: {Reason}",
                        client.PlayerName, Addressing.ToIPAddress(client.Address), client.Network.Name, reason);

                    ClientLeft?.Invoke(this, new RelayClientEventArgs(client.Snapshot(), reason));
                }

                tcp.Dispose();
            }
        }

        private async Task<RelayClient?> HandshakeAsync(NetworkStream stream, IPEndPoint remote, CancellationToken ct)
        {
            var hello = await ReadFrameAsync(stream, ct).ConfigureAwait(false);
            if (hello == null || hello.Value.Type != FrameType.Hello)
                return null;

            byte version;
            string network, ticket, player;
            uint requested;

            {
                var reader = new PacketReader(hello.Value.Body.Span);
                version   = reader.U8();
                network   = reader.Str();
                ticket    = reader.Str();
                player    = reader.Str();
                requested = reader.U32();

                if (!reader.Ok)
                {
                    await RejectAsync(stream, RejectCode.BadRequest, "malformed Hello", ct).ConfigureAwait(false);
                    return null;
                }
            }

            if (version != RelayProtocol.Version)
            {
                await RejectAsync(stream, RejectCode.Protocol,
                    $"protocol version {version} is not supported; this server speaks {RelayProtocol.Version}", ct)
                    .ConfigureAwait(false);
                return null;
            }

            var auth = await _authenticator
                .AuthenticateAsync(new RelayAuthenticationRequest(network, ticket, player, remote), ct)
                .ConfigureAwait(false);

            if (!auth.Allowed)
            {
                _logger.LogInformation("Refused {Remote} for network {Network}: {Message}", remote, network, auth.Message);
                await RejectAsync(stream, RejectCode.Unauthorized, auth.Message ?? "unauthorized", ct).ConfigureAwait(false);
                return null;
            }

            var networkName = auth.Network ?? network;
            var playerName  = (auth.PlayerName ?? player).Trim();
            if (playerName.Length > 64)
                playerName = playerName[..64];

            var id    = (uint)Interlocked.Increment(ref _nextClientId);
            var token = BinaryPrimitives.ReadUInt32BigEndian(RandomNumberGenerator.GetBytes(4));

            if (playerName.Length == 0)
                playerName = $"Player {id}";

            RelayClient? client;

            for (;;)
            {
                var net = _networks.GetOrAdd(networkName, name => new RelayNetwork(name, _subnet, _mask,
                    _options.MaxClientsPerNetwork, _options.MaxStreamsPerClient, _udp != null ? SendUdpDatagram : null));

                var status = net.Join(requested,
                    address => new RelayClient(id, token, address, playerName, net, remote, auth.State,
                        _options.SendQueueLimit, _cts!.Token),
                    Welcome,
                    out client);

                if (status == RelayNetwork.JoinStatus.Retired)
                {
                    // Emptied and removed while we were looking it up; make a new one.
                    _networks.TryRemove(new KeyValuePair<string, RelayNetwork>(networkName, net));
                    continue;
                }

                if (status == RelayNetwork.JoinStatus.Full)
                {
                    await RejectAsync(stream, RejectCode.NetworkFull, $"network '{networkName}' is full", ct)
                        .ConfigureAwait(false);
                    return null;
                }

                break;
            }

            _clients[client!.Id] = client;

            _logger.LogInformation("{Player} ({Address}) joined network {Network} from {Remote}",
                client.PlayerName, Addressing.ToIPAddress(client.Address), networkName, remote);

            return client;
        }

        private byte[] Welcome(RelayClient client)
            => new PacketWriter(24)
                .U32(client.Id)
                .U32(client.Token)
                .U32(client.Address)
                .U32(_mask)
                .U16((ushort)(UdpEndPoint?.Port ?? 0))
                .U32(_options.IpxNetwork)
                .ToFrame(FrameType.Welcome);

        private static async Task RejectAsync(NetworkStream stream, RejectCode code, string message, CancellationToken ct)
        {
            try
            {
                var frame = new PacketWriter(message.Length + 8).U8((byte)code).Str(message).ToFrame(FrameType.Reject);
                await stream.WriteAsync(frame, ct).ConfigureAwait(false);
            }
            catch (IOException) { }
            catch (OperationCanceledException) { }
        }

        /// <summary>Returns false when the client asked to leave.</summary>
        private async ValueTask<bool> DispatchAsync(RelayClient client, FrameType type, ReadOnlyMemory<byte> body)
        {
            switch (type)
            {
            case FrameType.Ping:
                await client.SendAsync(PacketWriter.Frame(FrameType.Pong, body.Span)).ConfigureAwait(false);
                break;

            case FrameType.Datagram:
                // A client only falls back to TCP when it has stopped hearing our
                // UDP acks, so stop sending it UDP as well.
                client.UdpVerified = false;
                client.Network.RouteDatagram(client, body.Span);
                break;

            case FrameType.StreamOpen:
                await client.Network.OpenStreamAsync(client, body).ConfigureAwait(false);
                break;

            case FrameType.StreamAccept:
            case FrameType.StreamReject:
            case FrameType.StreamData:
            case FrameType.StreamClose:
                await client.Network.ForwardStreamAsync(client, type, body).ConfigureAwait(false);
                break;

            case FrameType.Leave:
                return false;
            }

            return true;
        }

        private static async Task<(FrameType Type, ReadOnlyMemory<byte> Body)?> ReadFrameAsync(
            NetworkStream stream, CancellationToken ct)
        {
            var header = new byte[2];
            if (await stream.ReadAtLeastAsync(header, 2, throwOnEndOfStream: false, ct).ConfigureAwait(false) < 2)
                return null;

            var length = BinaryPrimitives.ReadUInt16BigEndian(header);
            if (length == 0)
                return null;

            var buffer = new byte[length];
            if (await stream.ReadAtLeastAsync(buffer, length, throwOnEndOfStream: false, ct).ConfigureAwait(false) < length)
                return null;

            return ((FrameType)buffer[0], buffer.AsMemory(1));
        }

        // ---- UDP ---------------------------------------------------------------------

        private async Task RunUdpAsync(CancellationToken ct)
        {
            var buffer = new byte[65536];
            EndPoint any = new IPEndPoint(IPAddress.Any, 0);

            while (!ct.IsCancellationRequested)
            {
                SocketReceiveFromResult result;

                try
                {
                    result = await _udp!.ReceiveFromAsync(buffer, SocketFlags.None, any, ct).ConfigureAwait(false);
                }
                catch (OperationCanceledException) { break; }
                catch (ObjectDisposedException) { break; }
                catch (SocketException) { continue; }

                try
                {
                    HandleUdp(buffer.AsSpan(0, result.ReceivedBytes), (IPEndPoint)result.RemoteEndPoint);
                }
                catch (Exception ex)
                {
                    _logger.LogDebug(ex, "Dropped a malformed UDP packet from {Remote}", result.RemoteEndPoint);
                }
            }
        }

        private void HandleUdp(ReadOnlySpan<byte> packet, IPEndPoint remote)
        {
            if (packet.Length < RelayProtocol.UdpHeaderSize)
                return;

            var clientId = BinaryPrimitives.ReadUInt32BigEndian(packet);
            var token    = BinaryPrimitives.ReadUInt32BigEndian(packet[4..]);
            var type     = (FrameType)packet[8];

            if (!_clients.TryGetValue(clientId, out var client) || client.Token != token)
                return;

            client.Touch();
            client.CountReceived(packet.Length);

            // Follow the client across NAT rebinding: the newest source wins.
            if (!remote.Equals(client.UdpEndPoint))
                client.UdpEndPoint = remote;

            switch (type)
            {
            case FrameType.UdpHello:
                SendUdp(client, PacketWriter.UdpPacket(client.Id, client.Token, FrameType.UdpHelloAck, ReadOnlySpan<byte>.Empty));
                break;

            case FrameType.Datagram:
                client.UdpVerified = true;
                client.Network.RouteDatagram(client, packet[RelayProtocol.UdpHeaderSize..]);
                break;
            }
        }

        private void SendUdpDatagram(RelayClient client, byte[] body)
            => SendUdp(client, PacketWriter.UdpPacket(client.Id, client.Token, FrameType.Datagram, body));

        private void SendUdp(RelayClient client, byte[] packet)
        {
            var endPoint = client.UdpEndPoint;
            var socket   = _udp;

            if (endPoint == null || socket == null)
                return;

            try
            {
                socket.SendTo(packet, endPoint);
                client.CountSent(packet.Length);
            }
            catch (SocketException) { }
            catch (ObjectDisposedException) { }
        }

        // ---- housekeeping ------------------------------------------------------------

        private async Task RunSweeperAsync(CancellationToken ct)
        {
            try
            {
                while (!ct.IsCancellationRequested)
                {
                    await Task.Delay(TimeSpan.FromSeconds(5), ct).ConfigureAwait(false);

                    foreach (var client in _clients.Values)
                        if (client.Idle > _options.ClientTimeout)
                            client.Close();
                }
            }
            catch (OperationCanceledException) { }
        }
    }
}
