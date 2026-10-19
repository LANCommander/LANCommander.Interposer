using System.Globalization;
using System.Net;
using LANCommander.Interposer.Relay;
using Microsoft.Extensions.Logging;

// A standalone relay server for testing the Interposer's network relay without a
// LANCommander server. Everything a LANCommander server would configure is a
// command-line option here.

var options = new RelayServerOptions();
string? secret = null;
var statusInterval = TimeSpan.FromSeconds(30);
var verbose = false;

for (var i = 0; i < args.Length; i++)
{
    string Next() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"{args[i]} needs a value");

    switch (args[i])
    {
    case "--port":       options.Port = int.Parse(Next(), CultureInfo.InvariantCulture); break;
    case "--udp-port":   options.UdpPort = int.Parse(Next(), CultureInfo.InvariantCulture); break;
    case "--no-udp":     options.DisableUdp = true; break;
    case "--bind":       options.BindAddress = IPAddress.Parse(Next()); break;
    case "--subnet":     options.VirtualSubnet = Next(); break;
    case "--max-clients":options.MaxClientsPerNetwork = int.Parse(Next(), CultureInfo.InvariantCulture); break;
    case "--secret":     secret = Next(); break;
    case "--status":     statusInterval = TimeSpan.FromSeconds(int.Parse(Next(), CultureInfo.InvariantCulture)); break;
    case "--verbose":    verbose = true; break;

    case "-h":
    case "--help":
        Console.WriteLine("""
            LANCommander Interposer relay server (proof of concept)

              --port <n>         TCP port, and UDP port unless --udp-port is given (default 41300; 0 = any)
              --udp-port <n>     UDP port for datagrams
              --no-udp           carry datagrams over TCP only
              --bind <address>   address to listen on (default 0.0.0.0)
              --subnet <cidr>    virtual subnet handed out (default 10.13.37.0/24)
              --max-clients <n>  members per network (default 64)
              --secret <text>    require NetworkRelay.Ticket to equal this
              --status <s>       seconds between status lines (default 30; 0 = never)
              --verbose          debug logging

            The first line printed is "LISTENING <tcp-port> <udp-port>", for scripts.
            """);
        return 0;

    default:
        Console.Error.WriteLine($"Unknown option {args[i]}; see --help");
        return 2;
    }
}

using var loggerFactory = LoggerFactory.Create(builder => builder
    .AddSimpleConsole(o => { o.SingleLine = true; o.TimestampFormat = "HH:mm:ss "; })
    .SetMinimumLevel(verbose ? LogLevel.Debug : LogLevel.Information));

IRelayAuthenticator authenticator = secret != null
    ? new SharedSecretRelayAuthenticator(secret)
    : new AllowAllRelayAuthenticator();

await using var server = new RelayServer(options, authenticator, loggerFactory.CreateLogger<RelayServer>());

using var stop = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    stop.Cancel();
};

await server.StartAsync();

// Machine-readable, so a test harness started with --port 0 can find us.
Console.WriteLine($"LISTENING {server.TcpEndPoint!.Port} {server.UdpEndPoint?.Port ?? 0}");
Console.Out.Flush();

try
{
    while (!stop.IsCancellationRequested)
    {
        await Task.Delay(statusInterval > TimeSpan.Zero ? statusInterval : Timeout.InfiniteTimeSpan, stop.Token);

        foreach (var network in server.GetNetworks())
        {
            Console.WriteLine($"[{network.Name}] {network.Clients.Count} member(s)");

            foreach (var c in network.Clients)
                Console.WriteLine($"    #{c.ClientId} {c.VirtualAddress,-15} {c.PlayerName,-20} " +
                                  $"{(c.UdpActive ? "udp" : "tcp")}  in {c.BytesReceived} B  out {c.BytesSent} B  " +
                                  $"streams {c.StreamsOpen}");
        }
    }
}
catch (OperationCanceledException) { }

await server.StopAsync();
return 0;
