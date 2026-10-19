#pragma once
#include <string>

// Settings the network relay works from. Kept free of Winsock so config.h can
// carry it without dragging winsock2.h into every translation unit.
//
// Seeded from NetworkRelay in Config.yml, then overlaid with the launcher's
// Local\InterposerRelay_<pid> mapping, and replaceable at runtime through
// InterposerRelayConnect / the control pipe. Every overlay goes through
// ParseRelaySettings, so a key that is absent keeps its current value.
struct RelaySettings
{
    bool         enabled           = false;
    std::wstring server;                   // host or host:port
    int          port              = 41300;
    std::wstring network;                  // peers in the same network see each other
    std::wstring ticket;                   // opaque, checked by the server's authenticator
    std::wstring address;                  // requested virtual IPv4, empty = any
    bool         udp               = true; // relay game UDP datagrams
    bool         tcp               = true; // tunnel game TCP connections
    bool         ipx               = true; // emulate IPX on top of the relay
    bool         loopbackBroadcast = true; // deliver our own broadcasts back to us, as a LAN does
    int          addressTimeout    = 5000; // ms a local-address query waits for the first Welcome
};

// Overlay the keys present in a NetworkRelay YAML map (UTF-8) onto settings.
// Returns false, leaving settings untouched, when the text does not parse or is
// not a map; error then says why.
bool ParseRelaySettings(const std::string& yamlUtf8, RelaySettings& settings, std::wstring* error);
