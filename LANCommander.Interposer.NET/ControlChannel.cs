using System;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace LANCommander.Interposer
{
    /// <summary>
    /// The launcher's end of the control pipe (<c>\\.\pipe\InterposerControl_&lt;pid&gt;</c>):
    /// requests to the injected DLL, answered one at a time. The native end and the
    /// wire format are in <c>dll\src\control.h</c>.
    /// </summary>
    internal sealed class ControlChannel : IDisposable
    {
        internal const uint CommandPing            = 1;
        internal const uint CommandRelayConnect    = 2;
        internal const uint CommandRelayDisconnect = 3;
        internal const uint CommandRelayStatus     = 4;
        internal const uint CommandGetConfig       = 5;

        private readonly NamedPipeServerStream _pipe;
        private readonly Task _connected;
        private readonly SemaphoreSlim _gate = new(1, 1);
        private uint _nextRequestId;

        /// <summary>
        /// Starts listening. Must be constructed before the DLL is injected: the DLL
        /// connects once, during DLL_PROCESS_ATTACH.
        /// </summary>
        public ControlChannel(uint processId)
        {
            _pipe = new NamedPipeServerStream(
                $"InterposerControl_{processId}",
                PipeDirection.InOut,
                1,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous);

            _connected = _pipe.WaitForConnectionAsync();
        }

        public bool IsConnected => _pipe.IsConnected;

        public async Task<(bool Ok, string Payload)> SendAsync(uint command, string? payload, CancellationToken ct)
        {
            await _connected.WaitAsync(ct).ConfigureAwait(false);
            await _gate.WaitAsync(ct).ConfigureAwait(false);

            try
            {
                var requestId = ++_nextRequestId;
                var text      = Encoding.Unicode.GetBytes(payload ?? string.Empty);
                var request   = new byte[12 + text.Length];

                BitConverter.TryWriteBytes(request.AsSpan(0), requestId);
                BitConverter.TryWriteBytes(request.AsSpan(4), command);
                BitConverter.TryWriteBytes(request.AsSpan(8), (uint)(text.Length / 2));
                text.CopyTo(request, 12);

                await _pipe.WriteAsync(request, ct).ConfigureAwait(false);
                await _pipe.FlushAsync(ct).ConfigureAwait(false);

                var header = new byte[12];
                await ReadExactAsync(header, ct).ConfigureAwait(false);

                var responseId = BitConverter.ToUInt32(header, 0);
                var ok         = BitConverter.ToInt32(header, 4) != 0;
                var chars      = BitConverter.ToUInt32(header, 8);

                if (responseId != requestId || chars > 1024 * 1024)
                    throw new IOException("The Interposer control pipe answered out of turn.");

                var body = new byte[chars * 2];
                await ReadExactAsync(body, ct).ConfigureAwait(false);

                return (ok, Encoding.Unicode.GetString(body));
            }
            finally
            {
                _gate.Release();
            }
        }

        private async Task ReadExactAsync(byte[] buffer, CancellationToken ct)
        {
            var offset = 0;

            while (offset < buffer.Length)
            {
                var read = await _pipe.ReadAsync(buffer.AsMemory(offset), ct).ConfigureAwait(false);
                if (read == 0)
                    throw new IOException("The injected process closed the Interposer control pipe.");

                offset += read;
            }
        }

        public void Dispose()
        {
            _pipe.Dispose();
            _gate.Dispose();
        }
    }
}
