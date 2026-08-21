namespace LANCommander.Interposer
{
    /// <summary>
    /// Describes a single network adapter as reported by
    /// <see cref="InterposerService.EnumerateNetworkAdapters"/>, including whether
    /// it passes the configured NetworkAdapters filter.
    /// </summary>
    public sealed class NetworkAdapterInfo
    {
        /// <summary>The adapter's friendly name (e.g. "Ethernet").</summary>
        public string FriendlyName { get; internal set; }

        /// <summary>The adapter's description (e.g. the NIC model).</summary>
        public string Description { get; internal set; }

        /// <summary>The physical (MAC) address as "00:11:22:33:44:55", empty if none.</summary>
        public string MacAddress { get; internal set; }

        /// <summary>The first IPv4 address (dotted-decimal), empty if none.</summary>
        public string IPv4Address { get; internal set; }

        /// <summary>The first IPv6 address, empty if none.</summary>
        public string IPv6Address { get; internal set; }

        /// <summary>
        /// True if the adapter passes the configured NetworkAdapters filter (or no
        /// filter is configured), meaning the game can see it; false if hidden.
        /// </summary>
        public bool Allowed { get; internal set; }
    }
}
