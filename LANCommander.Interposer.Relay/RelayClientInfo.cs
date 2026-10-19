using System.Net;

namespace LANCommander.Interposer.Relay
{
    /// <summary>A snapshot of one connected client.</summary>
    public sealed record RelayClientInfo
    {
        /// <summary>The id the server assigned; unique for the life of the server.</summary>
        public uint ClientId { get; init; }

        /// <summary>The network the client is a member of.</summary>
        public string Network { get; init; } = string.Empty;

        /// <summary>The name shown to the client's peers.</summary>
        public string PlayerName { get; init; } = string.Empty;

        /// <summary>The client's virtual LAN address.</summary>
        public IPAddress VirtualAddress { get; init; } = IPAddress.None;

        /// <summary>Where the client's TCP connection comes from.</summary>
        public IPEndPoint RemoteEndPoint { get; init; } = new(IPAddress.None, 0);

        /// <summary>The client's UDP endpoint, once it has registered one.</summary>
        public IPEndPoint? UdpEndPoint { get; init; }

        /// <summary>True once the server is delivering datagrams to the client over UDP.</summary>
        public bool UdpActive { get; init; }

        /// <summary>When the client joined.</summary>
        public DateTimeOffset ConnectedAt { get; init; }

        /// <summary>Bytes received from the client, TCP and UDP.</summary>
        public long BytesReceived { get; init; }

        /// <summary>Bytes sent to the client, TCP and UDP.</summary>
        public long BytesSent { get; init; }

        /// <summary>Tunnelled TCP streams the client has open.</summary>
        public int StreamsOpen { get; init; }

        /// <summary>Whatever the authenticator attached to the client.</summary>
        public object? State { get; init; }
    }

    /// <summary>A snapshot of one network.</summary>
    /// <param name="Name">The network name clients join with.</param>
    /// <param name="Clients">Its current members.</param>
    public sealed record RelayNetworkInfo(string Name, IReadOnlyList<RelayClientInfo> Clients);

    /// <summary>Raised when a client joins or leaves.</summary>
    public sealed class RelayClientEventArgs : EventArgs
    {
        /// <summary>Creates the event arguments.</summary>
        public RelayClientEventArgs(RelayClientInfo client, string? reason = null)
        {
            Client = client;
            Reason = reason;
        }

        /// <summary>The client, as it was at the time of the event.</summary>
        public RelayClientInfo Client { get; }

        /// <summary>Why a client left, when known.</summary>
        public string? Reason { get; }
    }
}
