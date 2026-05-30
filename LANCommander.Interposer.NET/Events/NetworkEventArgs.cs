using System;

namespace LANCommander.Interposer.Events
{
    /// <summary>
    /// Data for network connection events (connect, send/recv address discovery).
    /// </summary>
    public sealed class NetworkEventArgs : EventArgs
    {
        /// <summary>The IP address or hostname.</summary>
        public string Address { get; }

        /// <summary>The port number (0 if unknown).</summary>
        public int Port { get; }

        internal NetworkEventArgs(string address, int port)
        {
            Address = address;
            Port = port;
        }
    }
}
