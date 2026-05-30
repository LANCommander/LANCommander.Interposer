using System;

namespace LANCommander.Interposer.Events
{
    /// <summary>
    /// Data for DNS resolution events (getaddrinfo, gethostbyname, GetAddrInfoEx).
    /// </summary>
    public sealed class DnsEventArgs : EventArgs
    {
        /// <summary>The original hostname being resolved.</summary>
        public string Hostname { get; }

        /// <summary>
        /// The redirected hostname. Same as <see cref="Hostname"/> if no redirect was applied.
        /// </summary>
        public string RedirectedHostname { get; }

        internal DnsEventArgs(string hostname, string redirectedHostname)
        {
            Hostname = hostname;
            RedirectedHostname = redirectedHostname;
        }
    }
}
