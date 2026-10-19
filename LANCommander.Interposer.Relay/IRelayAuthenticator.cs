using System.Net;
using System.Security.Cryptography;
using System.Text;

namespace LANCommander.Interposer.Relay
{
    /// <summary>
    /// Decides whether a client may join, and which network it lands in. A
    /// LANCommander server implements this to check the ticket its launcher handed
    /// the game against its own session store.
    /// </summary>
    public interface IRelayAuthenticator
    {
        /// <summary>Authenticate one Hello.</summary>
        ValueTask<RelayAuthenticationResult> AuthenticateAsync(
            RelayAuthenticationRequest request, CancellationToken cancellationToken);
    }

    /// <summary>What a client presented in its Hello.</summary>
    /// <param name="Network">The network the client asked to join.</param>
    /// <param name="Ticket">The opaque ticket from <c>NetworkRelay.Ticket</c>.</param>
    /// <param name="PlayerName">The name the client wants shown to its peers.</param>
    /// <param name="RemoteEndPoint">Where the client connected from.</param>
    public sealed record RelayAuthenticationRequest(
        string Network, string Ticket, string PlayerName, IPEndPoint RemoteEndPoint);

    /// <summary>An authenticator's verdict.</summary>
    public sealed class RelayAuthenticationResult
    {
        /// <summary>True to let the client in.</summary>
        public bool Allowed { get; init; }

        /// <summary>
        /// The network to place the client in, overriding the one it asked for -- so
        /// the ticket, not the client, decides who it can see. Null keeps its choice.
        /// </summary>
        public string? Network { get; init; }

        /// <summary>The name to show peers, overriding the client's own. Null keeps it.</summary>
        public string? PlayerName { get; init; }

        /// <summary>Sent to a refused client; it appears in the game's session log.</summary>
        public string? Message { get; init; }

        /// <summary>Anything the host wants to carry on <see cref="RelayClientInfo.State"/>.</summary>
        public object? State { get; init; }

        /// <summary>Let the client in.</summary>
        public static RelayAuthenticationResult Allow(string? network = null, string? playerName = null, object? state = null)
            => new() { Allowed = true, Network = network, PlayerName = playerName, State = state };

        /// <summary>Refuse the client.</summary>
        public static RelayAuthenticationResult Deny(string message)
            => new() { Allowed = false, Message = message };
    }

    /// <summary>Lets everyone in, into the network they asked for. For testing.</summary>
    public sealed class AllowAllRelayAuthenticator : IRelayAuthenticator
    {
        /// <inheritdoc />
        public ValueTask<RelayAuthenticationResult> AuthenticateAsync(
            RelayAuthenticationRequest request, CancellationToken cancellationToken)
            => ValueTask.FromResult(RelayAuthenticationResult.Allow());
    }

    /// <summary>Lets in any client whose ticket equals a shared secret.</summary>
    public sealed class SharedSecretRelayAuthenticator : IRelayAuthenticator
    {
        private readonly byte[] _secret;

        /// <summary>Creates an authenticator for the given secret.</summary>
        public SharedSecretRelayAuthenticator(string secret)
        {
            if (string.IsNullOrEmpty(secret))
                throw new ArgumentException("The secret must not be empty.", nameof(secret));

            _secret = Encoding.UTF8.GetBytes(secret);
        }

        /// <inheritdoc />
        public ValueTask<RelayAuthenticationResult> AuthenticateAsync(
            RelayAuthenticationRequest request, CancellationToken cancellationToken)
        {
            var ticket = Encoding.UTF8.GetBytes(request.Ticket ?? string.Empty);

            return ValueTask.FromResult(CryptographicOperations.FixedTimeEquals(ticket, _secret)
                ? RelayAuthenticationResult.Allow()
                : RelayAuthenticationResult.Deny("invalid ticket"));
        }
    }
}
