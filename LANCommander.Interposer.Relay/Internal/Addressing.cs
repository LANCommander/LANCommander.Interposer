using System.Buffers.Binary;
using System.Globalization;
using System.Net;
using System.Net.Sockets;

namespace LANCommander.Interposer.Relay.Internal
{
    /// <summary>IPv4 addresses as host-order integers, the way the wire carries them.</summary>
    internal static class Addressing
    {
        public static uint ToUInt32(IPAddress address)
        {
            if (address.AddressFamily != AddressFamily.InterNetwork)
                throw new ArgumentException("Only IPv4 addresses are supported.", nameof(address));

            Span<byte> bytes = stackalloc byte[4];
            address.TryWriteBytes(bytes, out _);
            return BinaryPrimitives.ReadUInt32BigEndian(bytes);
        }

        public static IPAddress ToIPAddress(uint address)
        {
            var bytes = new byte[4];
            BinaryPrimitives.WriteUInt32BigEndian(bytes, address);
            return new IPAddress(bytes);
        }

        /// <summary>Parse "a.b.c.d/n" into a network address and mask.</summary>
        public static (uint Network, uint Mask) ParseCidr(string cidr)
        {
            var slash = cidr.IndexOf('/');
            if (slash < 0)
                throw new FormatException($"'{cidr}' is not in CIDR form (a.b.c.d/n).");

            var address = IPAddress.Parse(cidr[..slash]);
            var prefix  = int.Parse(cidr[(slash + 1)..], CultureInfo.InvariantCulture);

            // At least four addresses: network, broadcast and two hosts.
            if (prefix < 8 || prefix > 30)
                throw new FormatException($"'{cidr}': the prefix length must be between 8 and 30.");

            var mask = uint.MaxValue << (32 - prefix);
            return (ToUInt32(address) & mask, mask);
        }
    }
}
