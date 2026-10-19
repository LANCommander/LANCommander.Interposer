using System.Buffers.Binary;
using System.Text;

namespace LANCommander.Interposer.Relay.Protocol
{
    /// <summary>
    /// Builds a relay message body in big-endian order, then wraps it as a TCP frame
    /// or a UDP packet.
    /// </summary>
    public sealed class PacketWriter
    {
        private byte[] _buffer;
        private int _length;

        /// <summary>Creates a writer with room for <paramref name="capacity"/> bytes before it grows.</summary>
        public PacketWriter(int capacity = 64)
        {
            _buffer = new byte[Math.Max(capacity, 16)];
        }

        /// <summary>The bytes written so far.</summary>
        public ReadOnlySpan<byte> Written => _buffer.AsSpan(0, _length);

        /// <summary>Appends one byte.</summary>
        public PacketWriter U8(byte value)
        {
            Ensure(1);
            _buffer[_length++] = value;
            return this;
        }

        /// <summary>Appends a big-endian u16.</summary>
        public PacketWriter U16(ushort value)
        {
            Ensure(2);
            BinaryPrimitives.WriteUInt16BigEndian(_buffer.AsSpan(_length), value);
            _length += 2;
            return this;
        }

        /// <summary>Appends a big-endian u32.</summary>
        public PacketWriter U32(uint value)
        {
            Ensure(4);
            BinaryPrimitives.WriteUInt32BigEndian(_buffer.AsSpan(_length), value);
            _length += 4;
            return this;
        }

        /// <summary>Appends raw bytes.</summary>
        public PacketWriter Bytes(ReadOnlySpan<byte> data)
        {
            Ensure(data.Length);
            data.CopyTo(_buffer.AsSpan(_length));
            _length += data.Length;
            return this;
        }

        /// <summary>Appends a string: a u16 byte count, then UTF-8. Truncated at 65535 bytes.</summary>
        public PacketWriter Str(string? value)
        {
            var bytes = Encoding.UTF8.GetBytes(value ?? string.Empty);
            var count = Math.Min(bytes.Length, ushort.MaxValue);
            U16((ushort)count);
            return Bytes(bytes.AsSpan(0, count));
        }

        /// <summary>The body wrapped as one TCP frame of the given type.</summary>
        public byte[] ToFrame(FrameType type) => Frame(type, Written);

        /// <summary>Wraps a body as one TCP frame.</summary>
        public static byte[] Frame(FrameType type, ReadOnlySpan<byte> body)
        {
            if (body.Length + 1 > ushort.MaxValue)
                throw new ArgumentException("A relay frame body is limited to 65534 bytes.", nameof(body));

            var frame = new byte[3 + body.Length];
            BinaryPrimitives.WriteUInt16BigEndian(frame, (ushort)(body.Length + 1));
            frame[2] = (byte)type;
            body.CopyTo(frame.AsSpan(3));
            return frame;
        }

        /// <summary>Wraps a body as one UDP packet addressed to a client.</summary>
        public static byte[] UdpPacket(uint clientId, uint token, FrameType type, ReadOnlySpan<byte> body)
        {
            var packet = new byte[RelayProtocol.UdpHeaderSize + body.Length];
            BinaryPrimitives.WriteUInt32BigEndian(packet, clientId);
            BinaryPrimitives.WriteUInt32BigEndian(packet.AsSpan(4), token);
            packet[8] = (byte)type;
            body.CopyTo(packet.AsSpan(RelayProtocol.UdpHeaderSize));
            return packet;
        }

        private void Ensure(int more)
        {
            if (_length + more <= _buffer.Length)
                return;

            Array.Resize(ref _buffer, Math.Max(_buffer.Length * 2, _length + more));
        }
    }
}
