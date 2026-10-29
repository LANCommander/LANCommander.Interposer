#include "voice/server/server_impl.h"

#include "voice/common/log.h"

#include <algorithm>
#include <cmath>

namespace comms::server {

namespace {

constexpr uint32_t kMinBitrate = 6000;
constexpr uint32_t kMaxBitrate = 510000;
constexpr double   kBandwidthBurstSeconds = 0.5;
constexpr double   kMinBurstBytes = 1500; // roughly one maximum-size packet

// Request rates per client. Generous for legitimate use; flooding beyond them is dropped and eventually kicked.
constexpr double   kControlPerSecond = 10, kControlBurst = 40;
constexpr double   kPositionPerSecond = 60, kPositionBurst = 60;
constexpr double   kVoicePacketsPerSecond = 120, kVoicePacketBurst = 60;
constexpr uint32_t kFloodKickStrikes = 100;
constexpr uint32_t kStrikeDecayPerSecond = 5;

void configure_bandwidth(TokenBucket& bucket, uint32_t kbps)
{
    const double bytes_per_second = kbps * 125.0;
    bucket.configure(bytes_per_second, std::max(bytes_per_second * kBandwidthBurstSeconds, kMinBurstBytes));
}

void send_config_update(interposer_comms_server* server, Player& player)
{
    const uint32_t bitrate = client_bitrate(server);
    proto::Writer w(proto::kConfigUpdate);
    w.u32(bitrate).u16(static_cast<uint16_t>(server->config.opus_frame_ms));
    send(player.peer, proto::kChannelControl, w, true);
    player.sent_bitrate = bitrate;
}

} // namespace

bool valid_limits(const interposer_comms_server_config& config)
{
    switch (config.opus_frame_ms) {
    case 10: case 20: case 40: case 60: break;
    default: return false;
    }
    return config.opus_max_bitrate >= kMinBitrate && config.opus_max_bitrate <= kMaxBitrate
        && config.max_players > 0
        && (config.position_authority == INTERPOSER_COMMS_POSITION_SERVER || config.position_authority == INTERPOSER_COMMS_POSITION_CLIENT)
        && config.max_client_speed >= 0.0f && std::isfinite(config.max_client_speed);
}

uint32_t client_bitrate(const interposer_comms_server* server)
{
    uint32_t bitrate = server->config.opus_max_bitrate;
    const uint32_t cap_kbps = server->config.max_upstream_kbps_per_client;
    if (cap_kbps != 0) {
        // Voice header (~8 bytes + targets) plus transport overhead on every packet.
        const uint32_t packets_per_second = 1000 / server->config.opus_frame_ms;
        const int64_t overhead_bps = static_cast<int64_t>(packets_per_second) * (proto::kPacketOverheadBytes + 16) * 8;
        const int64_t budget = static_cast<int64_t>(cap_kbps) * 1000 - overhead_bps;
        bitrate = static_cast<uint32_t>(std::clamp<int64_t>(budget, kMinBitrate, bitrate));
    }
    return bitrate;
}

void configure_limits(interposer_comms_server* server, Player& player)
{
    configure_bandwidth(player.upstream, server->config.max_upstream_kbps_per_client);
    configure_bandwidth(player.downstream, server->config.max_downstream_kbps_per_client);
    player.control_rate.configure(kControlPerSecond, kControlBurst);
    player.position_rate.configure(kPositionPerSecond, kPositionBurst);
    player.voice_rate.configure(kVoicePacketsPerSecond, kVoicePacketBurst);
}

interposer_comms_result apply_limits(interposer_comms_server* server, const interposer_comms_server_config& config)
{
    if (!valid_limits(config))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;

    interposer_comms_server_config& c = server->config;
    const bool codec_changed = c.opus_frame_ms != config.opus_frame_ms;
    c.max_players = config.max_players;
    c.max_groups = config.max_groups;
    c.max_private_groups_per_player = config.max_private_groups_per_player;
    c.max_forwarded_speakers = config.max_forwarded_speakers;
    c.max_total_kbps = config.max_total_kbps;
    c.max_upstream_kbps_per_client = config.max_upstream_kbps_per_client;
    c.max_downstream_kbps_per_client = config.max_downstream_kbps_per_client;
    c.opus_max_bitrate = config.opus_max_bitrate;
    c.opus_frame_ms = config.opus_frame_ms;
    c.join_token_lifetime_ms = config.join_token_lifetime_ms;
    c.position_authority = config.position_authority;
    c.max_client_speed = config.max_client_speed;

    if (server->host) {
        const enet_uint32 bandwidth = c.max_total_kbps * 125;
        enet_host_bandwidth_limit(server->host, bandwidth, bandwidth);
    }

    const uint32_t bitrate = client_bitrate(server);
    for (auto& [id, player] : server->players) {
        configure_limits(server, player);
        if (player.peer && (codec_changed || player.sent_bitrate != bitrate))
            send_config_update(server, player);
    }
    INTERPOSER_COMMS_LOG_INFO_F("limits updated: bitrate %u, frame %u ms, up %u kbps, down %u kbps, forwarded %u",
                     bitrate, c.opus_frame_ms, c.max_upstream_kbps_per_client, c.max_downstream_kbps_per_client,
                     c.max_forwarded_speakers);
    return INTERPOSER_COMMS_OK;
}

bool allow_control_request(interposer_comms_server* server, Player& player)
{
    if (player.control_rate.consume(1, now_ms()))
        return true;
    if (++player.flood_strikes >= kFloodKickStrikes && player.peer) {
        INTERPOSER_COMMS_LOG_WARN_F("kicking player %u for flooding", player.id);
        kick(server, player, "flooding");
    }
    return false;
}

void tick_meters(interposer_comms_server* server, uint64_t now)
{
    const bool decay = now - server->last_strike_decay_ms >= 1000;
    if (decay)
        server->last_strike_decay_ms = now;

    for (auto& [id, player] : server->players) {
        player.upstream_meter.tick(now);
        player.downstream_meter.tick(now);
        if (decay)
            player.flood_strikes = player.flood_strikes > kStrikeDecayPerSecond ? player.flood_strikes - kStrikeDecayPerSecond : 0;
    }
}

} // namespace comms::server
