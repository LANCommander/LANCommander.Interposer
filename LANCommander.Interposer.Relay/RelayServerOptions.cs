using System.Net;
using LANCommander.Interposer.Relay.Protocol;

namespace LANCommander.Interposer.Relay
{
    /// <summary>Configuration for a <see cref="RelayServer"/>.</summary>
    public sealed class RelayServerOptions
    {
        /// <summary>The address to listen on. Defaults to every IPv4 interface.</summary>
        public IPAddress BindAddress { get; set; } = IPAddress.Any;

        /// <summary>
        /// The TCP port clients connect to, and the UDP port for datagrams unless
        /// <see cref="UdpPort"/> says otherwise. 0 picks a free port for each,
        /// independently; read them back from <see cref="RelayServer.TcpEndPoint"/>
        /// and <see cref="RelayServer.UdpEndPoint"/>.
        /// </summary>
        public int Port { get; set; } = RelayProtocol.DefaultPort;

        /// <summary>
        /// The UDP port, when it differs from <see cref="Port"/>. Clients learn it
        /// from the Welcome message, so it may be anything. 0 picks a free port.
        /// </summary>
        public int? UdpPort { get; set; }

        /// <summary>
        /// Disable the UDP path entirely; datagrams then travel over each client's
        /// TCP connection. For hosts that cannot open a UDP port.
        /// </summary>
        public bool DisableUdp { get; set; }

        /// <summary>
        /// The virtual subnet every network hands addresses out of, in CIDR form.
        /// Networks are isolated from each other, so they all share it. Defaults to a
        /// private range, because many games only list LAN servers whose address is
        /// private.
        /// </summary>
        public string VirtualSubnet { get; set; } = "10.13.37.0/24";

        /// <summary>The IPX network number reported to emulated IPX sockets.</summary>
        public uint IpxNetwork { get; set; } = 1;

        /// <summary>The most members one network may have.</summary>
        public int MaxClientsPerNetwork { get; set; } = 64;

        /// <summary>The most tunnelled TCP streams one client may have open.</summary>
        public int MaxStreamsPerClient { get; set; } = 256;

        /// <summary>How long a new connection has to send its Hello.</summary>
        public TimeSpan HandshakeTimeout { get; set; } = TimeSpan.FromSeconds(10);

        /// <summary>
        /// How long a client may go without sending anything before it is dropped.
        /// Clients ping every 10 seconds.
        /// </summary>
        public TimeSpan ClientTimeout { get; set; } = TimeSpan.FromSeconds(45);

        /// <summary>
        /// Frames queued for one client before datagrams to it are dropped and
        /// stream data waits.
        /// </summary>
        public int SendQueueLimit { get; set; } = 4096;
    }
}
