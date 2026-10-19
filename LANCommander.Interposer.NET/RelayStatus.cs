using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace LANCommander.Interposer
{
    /// <summary>The network relay's connection state. Values match the DLL's.</summary>
    public enum RelayState
    {
        /// <summary>Not enabled, or disconnected on request.</summary>
        Disabled = 0,
        /// <summary>Connecting for the first time.</summary>
        Connecting = 1,
        /// <summary>Connected; a virtual address is assigned.</summary>
        Connected = 2,
        /// <summary>Lost the server and retrying, keeping the address it had.</summary>
        Reconnecting = 3,
        /// <summary>Refused by the server; not retried until reconnected with new options.</summary>
        Failed = 4,
    }

    /// <summary>
    /// A snapshot of the injected DLL's network relay, from
    /// <see cref="InterposerService.GetRelayStatusAsync"/> or
    /// <see cref="InterposerService.GetRelayStatus"/>.
    /// </summary>
    public sealed class RelayStatus
    {
        /// <summary>The connection state.</summary>
        [JsonPropertyName("state")]
        public RelayState State { get; set; }

        /// <summary>The id the relay server assigned, 0 before the first connection.</summary>
        [JsonPropertyName("clientId")]
        public uint ClientId { get; set; }

        /// <summary>The game's virtual LAN address, empty before the first connection.</summary>
        [JsonPropertyName("virtualAddress")]
        public string VirtualAddress { get; set; } = string.Empty;

        /// <summary>The virtual network's netmask.</summary>
        [JsonPropertyName("netmask")]
        public string Netmask { get; set; } = string.Empty;

        /// <summary>The network joined.</summary>
        [JsonPropertyName("network")]
        public string Network { get; set; } = string.Empty;

        /// <summary>The relay server, as configured.</summary>
        [JsonPropertyName("server")]
        public string Server { get; set; } = string.Empty;

        /// <summary>True when datagrams travel over UDP; false while they fall back to TCP.</summary>
        [JsonPropertyName("udpActive")]
        public bool UdpActive { get; set; }

        /// <summary>Bytes sent to the relay server.</summary>
        [JsonPropertyName("bytesSent")]
        public ulong BytesSent { get; set; }

        /// <summary>Bytes received from the relay server.</summary>
        [JsonPropertyName("bytesReceived")]
        public ulong BytesReceived { get; set; }

        /// <summary>Game datagrams relayed out.</summary>
        [JsonPropertyName("datagramsSent")]
        public ulong DatagramsSent { get; set; }

        /// <summary>Game datagrams delivered in.</summary>
        [JsonPropertyName("datagramsReceived")]
        public ulong DatagramsReceived { get; set; }

        /// <summary>Tunnelled TCP connections currently open.</summary>
        [JsonPropertyName("streamsOpen")]
        public uint StreamsOpen { get; set; }

        /// <summary>Why the last connection attempt failed, empty when it did not.</summary>
        [JsonPropertyName("lastError")]
        public string LastError { get; set; } = string.Empty;

        /// <summary>The other members of the network.</summary>
        [JsonPropertyName("peers")]
        public List<RelayPeerInfo> Peers { get; set; } = new();
    }

    /// <summary>Another member of the relay network.</summary>
    public sealed class RelayPeerInfo
    {
        /// <summary>The peer's relay client id.</summary>
        [JsonPropertyName("clientId")]
        public uint ClientId { get; set; }

        /// <summary>The peer's virtual LAN address.</summary>
        [JsonPropertyName("virtualAddress")]
        public string VirtualAddress { get; set; } = string.Empty;

        /// <summary>The peer's player name.</summary>
        [JsonPropertyName("name")]
        public string Name { get; set; } = string.Empty;
    }
}
