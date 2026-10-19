using System.Buffers.Binary;
using LANCommander.Interposer.Relay.Protocol;

namespace LANCommander.Interposer.Relay.Internal
{
    /// <summary>
    /// One virtual LAN: its members, their addresses, and the streams between them.
    /// Routing decisions are made under the lock; anything sent is sent after it is
    /// released, so a slow client's queue never holds up the whole network.
    /// </summary>
    internal sealed class RelayNetwork
    {
        private readonly object _lock = new();
        private readonly Dictionary<uint, RelayClient> _byAddress = new();
        private readonly List<RelayClient> _members = new();
        private readonly uint _subnet;
        private readonly uint _mask;
        private readonly int _maxMembers;
        private readonly int _maxStreams;
        private readonly Action<RelayClient, byte[]>? _sendUdpDatagram;
        private uint _nextHost = 1;
        private uint _nextStreamId;
        private bool _retired;

        public RelayNetwork(string name, uint subnet, uint mask, int maxMembers, int maxStreams,
            Action<RelayClient, byte[]>? sendUdpDatagram)
        {
            Name             = name;
            _subnet          = subnet;
            _mask            = mask;
            _maxMembers      = maxMembers;
            _maxStreams      = maxStreams;
            _sendUdpDatagram = sendUdpDatagram;
        }

        public string Name { get; }

        public enum JoinStatus { Joined, Full, Retired }

        /// <summary>
        /// Add a client, assigning it an address -- the requested one when it is free
        /// and valid. The Welcome and every PeerJoined go out inside the lock, so the
        /// new client hears Welcome first and nobody hears about a member twice.
        /// </summary>
        public JoinStatus Join(uint requested, Func<uint, RelayClient> create,
            Func<RelayClient, byte[]> welcome, out RelayClient? client)
        {
            client = null;

            lock (_lock)
            {
                if (_retired)
                    return JoinStatus.Retired;

                if (_members.Count >= _maxMembers)
                    return JoinStatus.Full;

                var address = IsAssignable(requested) && !_byAddress.ContainsKey(requested)
                    ? requested
                    : NextFreeAddress();

                if (address == 0)
                    return JoinStatus.Full;

                client = create(address);
                _members.Add(client);
                _byAddress[address] = client;

                client.TrySend(welcome(client));

                var joined = PeerJoined(client);
                foreach (var other in _members)
                {
                    if (other == client)
                        continue;

                    client.TrySend(PeerJoined(other));
                    other.TrySend(joined);
                }

                return JoinStatus.Joined;
            }
        }

        /// <summary>
        /// Remove a client, telling its peers, and close every stream it was part of.
        /// Returns true when the network is now empty and has been retired.
        /// </summary>
        public bool Leave(RelayClient client)
        {
            var notices = new List<(RelayClient To, byte[] Frame)>();
            bool retired;

            lock (_lock)
            {
                if (!_members.Remove(client))
                    return false;

                _byAddress.Remove(client.Address);

                var left = new PacketWriter(4).U32(client.Id).ToFrame(FrameType.PeerLeft);
                foreach (var other in _members)
                    notices.Add((other, left));

                foreach (var end in client.Streams.Values)
                {
                    end.Other.Streams.Remove(end.OtherId);
                    notices.Add((end.Other, new PacketWriter(4).U32(end.OtherId).ToFrame(FrameType.StreamClose)));
                }
                client.Streams.Clear();

                retired = _retired = _members.Count == 0;
            }

            foreach (var (to, frame) in notices)
                to.TrySend(frame);

            return retired;
        }

        public IReadOnlyList<RelayClient> Members()
        {
            lock (_lock)
                return _members.ToArray();
        }

        public int StreamCount(RelayClient client)
        {
            lock (_lock)
                return client.Streams.Count;
        }

        // ---- datagrams ----------------------------------------------------------

        /// <summary>
        /// Forward a datagram body from <paramref name="sender"/>. The header is
        /// rewritten for the receiving side: the sender becomes the peer, the
        /// destination port becomes the local one.
        /// </summary>
        public void RouteDatagram(RelayClient sender, ReadOnlySpan<byte> body)
        {
            if (body.Length < RelayProtocol.DatagramHeaderSize ||
                body.Length - RelayProtocol.DatagramHeaderSize > RelayProtocol.MaxDatagram)
                return;

            var destination = BinaryPrimitives.ReadUInt32BigEndian(body[1..]);
            var dstPort     = BinaryPrimitives.ReadUInt16BigEndian(body[5..]);
            var srcPort     = BinaryPrimitives.ReadUInt16BigEndian(body[7..]);

            var outgoing = body.ToArray();
            BinaryPrimitives.WriteUInt32BigEndian(outgoing.AsSpan(1), sender.Address);
            BinaryPrimitives.WriteUInt16BigEndian(outgoing.AsSpan(5), srcPort);
            BinaryPrimitives.WriteUInt16BigEndian(outgoing.AsSpan(7), dstPort);

            RelayClient[] targets;

            lock (_lock)
            {
                if (IsBroadcast(destination))
                    targets = _members.Where(m => m != sender).ToArray();
                else if (_byAddress.TryGetValue(destination, out var target) && target != sender)
                    targets = new[] { target };
                else
                    return;
            }

            foreach (var target in targets)
                Deliver(target, outgoing);
        }

        private void Deliver(RelayClient target, byte[] body)
        {
            if (_sendUdpDatagram != null && target.UdpVerified && target.UdpEndPoint != null)
                _sendUdpDatagram(target, body);
            else
                target.TrySend(PacketWriter.Frame(FrameType.Datagram, body));
        }

        // ---- streams -------------------------------------------------------------

        public async ValueTask OpenStreamAsync(RelayClient sender, ReadOnlyMemory<byte> body)
        {
            var reader      = new PacketReader(body.Span);
            var id          = reader.U32();
            var destination = reader.U32();
            var dstPort     = reader.U16();
            var srcPort     = reader.U16();

            if (!reader.Ok)
                return;

            RelayClient? target = null;
            uint targetId = 0;
            StreamRejectReason? reject = null;

            lock (_lock)
            {
                if (sender.Streams.ContainsKey(id))
                    reject = StreamRejectReason.Refused;
                else if (sender.Streams.Count >= _maxStreams)
                    reject = StreamRejectReason.Limit;
                else if (!_byAddress.TryGetValue(destination, out target) || target == sender)
                    reject = StreamRejectReason.NoSuchPeer;
                else if (target.Streams.Count >= _maxStreams)
                    reject = StreamRejectReason.Limit;
                else
                {
                    do
                        targetId = RelayProtocol.ServerStreamBit | (++_nextStreamId & ~RelayProtocol.ServerStreamBit);
                    while (target.Streams.ContainsKey(targetId));

                    var pair = new StreamPair { Opener = sender, OpenerId = id, Target = target, TargetId = targetId };
                    sender.Streams[id]       = new StreamEnd(pair, isOpener: true);
                    target.Streams[targetId] = new StreamEnd(pair, isOpener: false);
                }
            }

            if (reject is { } reason)
            {
                await sender.SendAsync(new PacketWriter(5).U32(id).U8((byte)reason).ToFrame(FrameType.StreamReject))
                    .ConfigureAwait(false);
                return;
            }

            await target!.SendAsync(new PacketWriter(12)
                    .U32(targetId).U32(sender.Address).U16(srcPort).U16(dstPort)
                    .ToFrame(FrameType.StreamOpen))
                .ConfigureAwait(false);
        }

        /// <summary>Accept, Reject, Data and Close: forwarded under the other side's id.</summary>
        public async ValueTask ForwardStreamAsync(RelayClient sender, FrameType type, ReadOnlyMemory<byte> body)
        {
            if (body.Length < 4)
                return;

            var id = BinaryPrimitives.ReadUInt32BigEndian(body.Span);

            RelayClient other;
            uint otherId;

            lock (_lock)
            {
                if (!sender.Streams.TryGetValue(id, out var end))
                    return;

                other   = end.Other;
                otherId = end.OtherId;

                if (type == FrameType.StreamReject)
                {
                    sender.Streams.Remove(id);
                    other.Streams.Remove(otherId);
                }
                else if (type == FrameType.StreamClose)
                {
                    if (end.IsOpener) end.Pair.OpenerClosed = true;
                    else              end.Pair.TargetClosed = true;

                    if (end.Pair.OpenerClosed && end.Pair.TargetClosed)
                    {
                        sender.Streams.Remove(id);
                        other.Streams.Remove(otherId);
                    }
                }
            }

            var frame = PacketWriter.Frame(type, body.Span);
            BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(3), otherId);

            // Waits for room in the other side's queue: backpressure for a fast sender.
            await other.SendAsync(frame).ConfigureAwait(false);
        }

        // ---- addressing ------------------------------------------------------------

        private bool IsBroadcast(uint address)
            => address == RelayProtocol.Broadcast ||
               ((address & _mask) == _subnet && (address & ~_mask) == ~_mask);

        // A host address that is in the subnet, is neither the network nor the
        // broadcast address, and does not end in .0 or .255 -- plenty of old games
        // treat those as special whatever the netmask says.
        private bool IsAssignable(uint address)
        {
            if (address == 0 || (address & _mask) != _subnet)
                return false;

            var host = address & ~_mask;
            if (host == 0 || host == ~_mask)
                return false;

            var last = address & 0xFF;
            return last != 0 && last != 255;
        }

        private uint NextFreeAddress()
        {
            var hosts = ~_mask;

            for (uint tries = 0; tries < hosts; ++tries)
            {
                var candidate = _subnet | (_nextHost & hosts);
                _nextHost = (_nextHost % hosts) + 1;

                if (IsAssignable(candidate) && !_byAddress.ContainsKey(candidate))
                    return candidate;
            }

            return 0;
        }

        private static byte[] PeerJoined(RelayClient client)
            => new PacketWriter(16).U32(client.Id).U32(client.Address).Str(client.PlayerName)
                .ToFrame(FrameType.PeerJoined);
    }
}
