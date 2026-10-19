using System;

namespace LANCommander.Interposer.Events
{
    /// <summary>Data for a change in the network relay's connection state.</summary>
    public sealed class RelayStateChangedEventArgs : EventArgs
    {
        /// <summary>The new state.</summary>
        public RelayState State { get; }

        /// <summary>The game's virtual address, empty before the first connection.</summary>
        public string VirtualAddress { get; }

        internal RelayStateChangedEventArgs(RelayState state, string? virtualAddress)
        {
            State          = state;
            VirtualAddress = virtualAddress ?? string.Empty;
        }
    }

    /// <summary>Data for a peer joining or leaving the relay network.</summary>
    public sealed class RelayPeerEventArgs : EventArgs
    {
        /// <summary>The peer's relay client id.</summary>
        public uint ClientId { get; }

        /// <summary>The peer's virtual LAN address.</summary>
        public string VirtualAddress { get; }

        /// <summary>The peer's player name.</summary>
        public string Name { get; }

        internal RelayPeerEventArgs(uint clientId, string? virtualAddress, string? name)
        {
            ClientId       = clientId;
            VirtualAddress = virtualAddress ?? string.Empty;
            Name           = name ?? string.Empty;
        }
    }
}
