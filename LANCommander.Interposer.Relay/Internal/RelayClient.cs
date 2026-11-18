using System.Net;
using System.Net.Sockets;
using System.Threading.Channels;
using LANCommander.Interposer.Relay.Protocol;

namespace LANCommander.Interposer.Relay.Internal
{
    /// <summary>
    /// One connected client. Everything sent to it over TCP is drained by one
    /// writer task, so frames from different senders never interleave on the wire.
    /// It drains two queues: control frames and datagrams falling back from UDP,
    /// which are small and latency-sensitive, ahead of tunnelled stream frames,
    /// which can be bulk. Every stream frame goes through the one stream queue, so
    /// stream frames keep their order relative to each other.
    /// </summary>
    internal sealed class RelayClient
    {
        private readonly Channel<byte[]> _urgent;
        private readonly Channel<byte[]> _streams;
        private readonly CancellationTokenSource _cts;
        private long _bytesReceived;
        private long _bytesSent;
        private long _lastReceived;

        public RelayClient(uint id, uint token, uint address, string playerName, RelayNetwork network,
            IPEndPoint remoteEndPoint, object? state, int queueLimit, CancellationToken serverToken)
        {
            Id             = id;
            Token          = token;
            Address        = address;
            PlayerName     = playerName;
            Network        = network;
            RemoteEndPoint = remoteEndPoint;
            State          = state;
            ConnectedAt    = DateTimeOffset.UtcNow;

            var options = new BoundedChannelOptions(queueLimit)
            {
                SingleReader = true,
                FullMode     = BoundedChannelFullMode.Wait,
            };

            _urgent  = Channel.CreateBounded<byte[]>(options);
            _streams = Channel.CreateBounded<byte[]>(options);

            _cts = CancellationTokenSource.CreateLinkedTokenSource(serverToken);
            Touch();
        }

        public uint Id { get; }
        public uint Token { get; }
        public uint Address { get; }
        public string PlayerName { get; }
        public RelayNetwork Network { get; }
        public IPEndPoint RemoteEndPoint { get; }
        public object? State { get; }
        public DateTimeOffset ConnectedAt { get; }

        /// <summary>Cancelled when the client is being dropped.</summary>
        public CancellationToken Closing => _cts.Token;

        /// <summary>The client's UDP endpoint, learned from its UdpHello packets.</summary>
        public volatile IPEndPoint? UdpEndPoint;

        /// <summary>
        /// True once the client has sent a datagram over UDP, which it only does after
        /// it has heard our UdpHelloAck -- the proof that UDP works in both directions.
        /// Until then, and again after it falls back to TCP, it is sent datagrams over TCP.
        /// </summary>
        public volatile bool UdpVerified;

        /// <summary>Stream ends this client holds, by the id it knows them by. Guarded by the network's lock.</summary>
        public Dictionary<uint, StreamEnd> Streams { get; } = new();

        public long BytesReceived => Interlocked.Read(ref _bytesReceived);
        public long BytesSent => Interlocked.Read(ref _bytesSent);

        public TimeSpan Idle => TimeSpan.FromMilliseconds(Environment.TickCount64 - Interlocked.Read(ref _lastReceived));

        public void Touch() => Interlocked.Exchange(ref _lastReceived, Environment.TickCount64);

        public void CountReceived(int bytes) => Interlocked.Add(ref _bytesReceived, bytes);
        public void CountSent(int bytes) => Interlocked.Add(ref _bytesSent, bytes);

        /// <summary>Queue a frame, dropping it when the queue is full. For datagrams.</summary>
        public bool TrySend(byte[] frame) => QueueFor(frame).Writer.TryWrite(frame);

        /// <summary>Queue a frame, waiting for room. For control and stream frames.</summary>
        public async ValueTask SendAsync(byte[] frame)
        {
            try
            {
                await QueueFor(frame).Writer.WriteAsync(frame, Closing).ConfigureAwait(false);
            }
            catch (OperationCanceledException) { }
            catch (ChannelClosedException) { }
        }

        // A frame is u16 length, u8 type, body.
        private Channel<byte[]> QueueFor(byte[] frame)
            => frame.Length > 2 && IsStreamFrame((FrameType)frame[2]) ? _streams : _urgent;

        private static bool IsStreamFrame(FrameType type)
            => type is FrameType.StreamOpen or FrameType.StreamAccept or FrameType.StreamReject
                    or FrameType.StreamData or FrameType.StreamClose;

        /// <summary>Drain the queues onto the socket until the client is closed.</summary>
        public async Task RunWriterAsync(NetworkStream stream)
        {
            try
            {
                var urgent  = _urgent.Reader;
                var streams = _streams.Reader;

                for (;;)
                {
                    // Every urgent frame, then one stream frame, then look again:
                    // an urgent frame waits behind at most one stream frame.
                    if (urgent.TryRead(out var frame) || streams.TryRead(out frame))
                    {
                        await stream.WriteAsync(frame, Closing).ConfigureAwait(false);
                        CountSent(frame.Length);
                        continue;
                    }

                    if (urgent.Completion.IsCompleted && streams.Completion.IsCompleted)
                        break;

                    await Task.WhenAny(
                            urgent.WaitToReadAsync(Closing).AsTask(),
                            streams.WaitToReadAsync(Closing).AsTask())
                        .ConfigureAwait(false);

                    Closing.ThrowIfCancellationRequested();
                }
            }
            catch (OperationCanceledException) { }
            catch (IOException) { }
            catch (ObjectDisposedException) { }
            finally
            {
                Close();
            }
        }

        public void Close()
        {
            _urgent.Writer.TryComplete();
            _streams.Writer.TryComplete();

            try { _cts.Cancel(); }
            catch (ObjectDisposedException) { }
        }

        public RelayClientInfo Snapshot() => new()
        {
            ClientId       = Id,
            Network        = Network.Name,
            PlayerName     = PlayerName,
            VirtualAddress = Addressing.ToIPAddress(Address),
            RemoteEndPoint = RemoteEndPoint,
            UdpEndPoint    = UdpEndPoint,
            UdpActive      = UdpVerified,
            ConnectedAt    = ConnectedAt,
            BytesReceived  = BytesReceived,
            BytesSent      = BytesSent,
            StreamsOpen    = Network.StreamCount(this),
            State          = State,
        };
    }

    /// <summary>One side of a tunnelled stream.</summary>
    internal sealed class StreamEnd
    {
        public StreamEnd(StreamPair pair, bool isOpener)
        {
            Pair     = pair;
            IsOpener = isOpener;
        }

        public StreamPair Pair { get; }
        public bool IsOpener { get; }

        public RelayClient Owner => IsOpener ? Pair.Opener : Pair.Target;
        public RelayClient Other => IsOpener ? Pair.Target : Pair.Opener;
        public uint OtherId => IsOpener ? Pair.TargetId : Pair.OpenerId;
    }

    /// <summary>A tunnelled stream between two clients, each knowing it by its own id.</summary>
    internal sealed class StreamPair
    {
        public required RelayClient Opener { get; init; }
        public required uint OpenerId { get; init; }
        public required RelayClient Target { get; init; }
        public required uint TargetId { get; init; }
        public bool OpenerClosed { get; set; }
        public bool TargetClosed { get; set; }
    }
}
