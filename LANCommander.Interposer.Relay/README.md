# LANCommander.Interposer.Relay

The relay server for the LANCommander Interposer's network relay. Games running under the Interposer
connect to it instead of a VPN; players who join the same relay network see each other's LAN games
(broadcasts, UDP, TCP and IPX) as if they shared a LAN, each with a virtual LAN address.

Host it from any .NET process, such as a LANCommander server plugin or a console app:

```csharp
await using var relay = new RelayServer(
    new RelayServerOptions { Port = 41300 },
    new SharedSecretRelayAuthenticator("letmein"),
    logger);

relay.ClientJoined += (_, e) => Console.WriteLine($"{e.Client.PlayerName} joined {e.Client.Network}");

await relay.StartAsync();
```

An `IRelayAuthenticator` decides who may join, and can place a client in a network and name it,
whatever the client asked for. `GetNetworks()`, `GetClients()` and `Disconnect(clientId)` give a host
an admin view.

Games connect through the Interposer DLL, configured in `.interposer\Config.yml` or by a launcher
through `InterposerService.Relay` in the `LANCommander.Interposer` package. See
[Network Relay](https://github.com/LANCommander/LANCommander.Interposer/blob/main/LANCommander.Interposer.Documentation/NetworkRelay.md)
for the options and the wire protocol.
