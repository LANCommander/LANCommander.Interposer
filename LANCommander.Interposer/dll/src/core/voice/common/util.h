#pragma once

#include "voice/common/protocol.h"

#include <enet/enet.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace comms {

inline uint64_t now_ms()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

template <size_t N>
void copy_to(char (&dest)[N], const std::string& src)
{
    std::snprintf(dest, N, "%s", src.c_str());
}

inline void copy_to(char* dest, size_t capacity, const std::string& src)
{
    if (dest && capacity > 0)
        std::snprintf(dest, capacity, "%s", src.c_str());
}

inline std::string to_string(const char* s)
{
    return s ? std::string(s) : std::string();
}

// Caller must hold whatever lock serializes access to the peer's ENet host.
inline void send(ENetPeer* peer, proto::Channel channel, const proto::Writer& writer, bool reliable)
{
    if (!peer)
        return;
    const auto& data = writer.data();
    ENetPacket* packet = enet_packet_create(data.data(), data.size(),
                                            reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (packet && enet_peer_send(peer, channel, packet) != 0)
        enet_packet_destroy(packet);
}

// Blocks up to timeout_ms until the host socket is readable. Safe to call without holding the host lock
// as long as the host outlives the call.
inline void wait_for_socket(ENetHost* host, uint32_t timeout_ms)
{
    enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
    enet_socket_wait(host->socket, &condition, timeout_ms);
}

} // namespace comms
