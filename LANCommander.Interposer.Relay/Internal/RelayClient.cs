using System.Net;
using System.Net.Sockets;
using System.Threading.Channels;

namespace LANCommander.Interposer.Relay.Internal
{
    /// <summary>
    /// One connected client. Everything sent to it over TCP goes through a single
    /// queue drained by one writer task, so frames from different senders never
    /// interleave on the wire.
    /// </summary>
    internal sealed class RelayClient
    {
        private readonly Channel<byte[]> _outbound;
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

            _outbound = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(queueLimit)
            {
                SingleReader = true,
                FullMode     = BoundedChannelFullMode.Wait,
            });

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
        public bool TrySend(byte[] frame) => _outbound.Writer.TryWrite(frame);

        /// <summary>Queue a frame, waiting for room. For control and stream frames.</summary>
        public async ValueTask SendAsync(byte[] frame)
        {
            try
            {
                await _outbound.Writer.WriteAsync(frame, Closing).ConfigureAwait(false);
            }
            catch (OperationCanceledException) { }
            catch (ChannelClosedException) { }
        }

        /// <summary>Drain the queue onto the socket until the client is closed.</summary>
        public async Task RunWriterAsync(NetworkStream stream)
        {
            try
            {
                var reader = _outbound.Reader;

                while (await reader.WaitToReadAsync(Closing).ConfigureAwait(false))
                {
                    while (reader.TryRead(out var frame))
                    {
                        await stream.WriteAsync(frame, Closing).ConfigureAwait(false);
                        CountSent(frame.Length);
                    }
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
            _outbound.Writer.TryComplete();

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
