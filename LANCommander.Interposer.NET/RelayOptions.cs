using System.Globalization;
using System.Text;

namespace LANCommander.Interposer
{
    /// <summary>
    /// Network relay settings a launcher passes to the injected DLL, overriding the
    /// <c>NetworkRelay</c> section of <c>.interposer\Config.yml</c> key by key. A
    /// property left null keeps whatever the config file says.
    /// <para>
    /// Other players see the game's player under its username -- set
    /// <see cref="InterposerService.Username"/> to choose it.
    /// </para>
    /// <para>
    /// Assign it to <see cref="InterposerService.Relay"/> before
    /// <see cref="InterposerService.Start"/> or <see cref="InterposerService.Inject(int, string)"/>,
    /// or pass it to <see cref="InterposerService.ConnectRelayAsync"/> to reconnect a
    /// running game somewhere else.
    /// </para>
    /// </summary>
    public sealed class RelayOptions
    {
        /// <summary>Connect to the relay. Null keeps the config file's setting.</summary>
        public bool? Enabled { get; set; }

        /// <summary>The relay server, as <c>host</c> or <c>host:port</c>.</summary>
        public string? Server { get; set; }

        /// <summary>The port, when <see cref="Server"/> does not name one. Defaults to 41300.</summary>
        public int? Port { get; set; }

        /// <summary>The network to join. Peers in the same network see each other's games.</summary>
        public string? Network { get; set; }

        /// <summary>The authentication ticket the relay server's authenticator checks.</summary>
        public string? Ticket { get; set; }

        /// <summary>A virtual IPv4 address to ask for. The server grants it if it is free.</summary>
        public string? Address { get; set; }

        /// <summary>Relay the game's UDP datagrams and broadcasts. Default true.</summary>
        public bool? Udp { get; set; }

        /// <summary>Tunnel the game's TCP connections. Default true.</summary>
        public bool? Tcp { get; set; }

        /// <summary>Emulate IPX on top of the relay. Default true.</summary>
        public bool? Ipx { get; set; }

        /// <summary>Deliver the game's own broadcasts back to it, as a real LAN does. Default true.</summary>
        public bool? LoopbackBroadcasts { get; set; }

        /// <summary>
        /// Milliseconds a game asking for its own address waits for the relay to assign
        /// one before it is answered with the real address instead. Default 5000.
        /// </summary>
        public int? AddressTimeout { get; set; }

        /// <summary>
        /// The options as a <c>NetworkRelay</c> YAML map containing only the keys that
        /// are set -- the form the DLL reads from its override mapping and its control
        /// pipe. Empty when nothing is set.
        /// </summary>
        public string ToYaml()
        {
            var yaml = new StringBuilder();

            void Add(string key, string? value)
            {
                if (value != null)
                    yaml.Append(key).Append(": ").Append(value).Append('\n');
            }

            static string? Bool(bool? value) => value.HasValue ? (value.Value ? "true" : "false") : null;
            static string? Int(int? value) => value?.ToString(CultureInfo.InvariantCulture);

            // Single-quoted scalars take every character literally except the quote
            // itself, which is doubled.
            static string? Quote(string? value) => value == null ? null : "'" + value.Replace("'", "''") + "'";

            Add("Enabled",            Bool(Enabled));
            Add("Server",             Quote(Server));
            Add("Port",               Int(Port));
            Add("Network",            Quote(Network));
            Add("Ticket",             Quote(Ticket));
            Add("Address",            Quote(Address));
            Add("UDP",                Bool(Udp));
            Add("TCP",                Bool(Tcp));
            Add("IPX",                Bool(Ipx));
            Add("LoopbackBroadcasts", Bool(LoopbackBroadcasts));
            Add("AddressTimeout",     Int(AddressTimeout));

            return yaml.ToString();
        }
    }
}
