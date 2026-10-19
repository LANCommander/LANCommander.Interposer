using System.Buffers.Binary;
using System.Text;

namespace LANCommander.Interposer.Relay.Protocol
{
    /// <summary>
    /// Reads a relay message body. A read past the end does not throw: it returns a
    /// default value and clears <see cref="Ok"/>, so a handler parses everything and
    /// checks once.
    /// </summary>
    public ref struct PacketReader
    {
        private readonly ReadOnlySpan<byte> _data;
        private int _position;

        /// <summary>Creates a reader over a message body.</summary>
        public PacketReader(ReadOnlySpan<byte> data)
        {
            _data = data;
            _position = 0;
            Ok = true;
        }

        /// <summary>False once any read ran past the end of the body.</summary>
        public bool Ok { get; private set; }

        /// <summary>Everything not yet read.</summary>
        public readonly ReadOnlySpan<byte> Remaining => Ok ? _data[_position..] : ReadOnlySpan<byte>.Empty;

        /// <summary>Reads one byte.</summary>
        public byte U8()
        {
            if (!Need(1)) return 0;
            return _data[_position++];
        }

        /// <summary>Reads a big-endian u16.</summary>
        public ushort U16()
        {
            if (!Need(2)) return 0;
            var value = BinaryPrimitives.ReadUInt16BigEndian(_data[_position..]);
            _position += 2;
            return value;
        }

        /// <summary>Reads a big-endian u32.</summary>
        public uint U32()
        {
            if (!Need(4)) return 0;
            var value = BinaryPrimitives.ReadUInt32BigEndian(_data[_position..]);
            _position += 4;
            return value;
        }

        /// <summary>Reads a string: a u16 byte count, then UTF-8.</summary>
        public string Str()
        {
            var length = U16();
            if (!Need(length)) return string.Empty;
            var value = Encoding.UTF8.GetString(_data.Slice(_position, length));
            _position += length;
            return value;
        }

        private bool Need(int count)
        {
            if (Ok && _data.Length - _position >= count)
                return true;

            Ok = false;
            return false;
        }
    }
}
