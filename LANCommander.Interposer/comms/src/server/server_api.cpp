#include "server/server_impl.h"

#include "common/enet_runtime.h"
#include "common/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <random>

using namespace comms;
using namespace comms::server;

namespace {

const char* copy_string(std::string& storage, const char* value)
{
    if (!value)
        return nullptr;
    storage = value;
    return storage.c_str();
}

bool valid_zone_config(const interposer_comms_zone_config* config)
{
    return config && config->struct_size >= sizeof(interposer_comms_zone_config)
        && config->min_distance >= 0.0f && config->max_distance > config->min_distance
        && std::isfinite(config->max_distance)
        && config->rolloff >= INTERPOSER_COMMS_ROLLOFF_LINEAR && config->rolloff <= INTERPOSER_COMMS_ROLLOFF_EXPONENTIAL;
}

std::string generate_token()
{
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 4; ++i) {
        uint32_t value = rd();
        for (int nibble = 0; nibble < 8; ++nibble, value >>= 4)
            token.push_back(hex[value & 0xF]);
    }
    return token;
}

template <typename Ids>
interposer_comms_result fill_ids(const Ids& ids, interposer_comms_player_id* out_ids, uint32_t capacity, uint32_t* out_count)
{
    uint32_t count = 0;
    for (auto id : ids) {
        if (count < capacity)
            out_ids[count] = id;
        ++count;
    }
    *out_count = count;
    return count > capacity ? INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL : INTERPOSER_COMMS_OK;
}

} // namespace

#define INTERPOSER_COMMS_REQUIRE_PLAYER(player_var, id)            \
    Player* player_var = find_player(server, (id));     \
    if (!player_var)                                    \
        return INTERPOSER_COMMS_ERR_NOT_FOUND

extern "C" {

/* ---- Lifecycle ---------------------------------------------------------- */

INTERPOSER_COMMS_API void INTERPOSER_COMMS_CALL interposer_comms_server_config_init(interposer_comms_server_config* config)
{
    if (!config)
        return;
    *config = interposer_comms_server_config{};
    config->struct_size = sizeof(interposer_comms_server_config);
    config->port = INTERPOSER_COMMS_DEFAULT_PORT;
    config->name = "LANCommander Comms";
    config->auth_mode = INTERPOSER_COMMS_AUTH_TOKEN;
    config->discoverable = 1;
    config->discovery_port = INTERPOSER_COMMS_DEFAULT_DISCOVERY_PORT;
    config->max_players = 64;
    config->max_groups = 256;
    config->max_private_groups_per_player = 1;
    config->max_forwarded_speakers = 8;
    config->opus_max_bitrate = 32000;
    config->opus_frame_ms = 20;
    config->join_token_lifetime_ms = 60000;
    config->position_authority = INTERPOSER_COMMS_POSITION_SERVER;
    config->max_client_speed = 0.0f;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_create(const interposer_comms_server_config* config, interposer_comms_server** out_server)
{
    if (!config || !out_server || config->struct_size < sizeof(interposer_comms_server_config))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;

    if (!valid_limits(*config) || config->auth_mode < INTERPOSER_COMMS_AUTH_TOKEN || config->auth_mode > INTERPOSER_COMMS_AUTH_OPEN)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;

    if (!EnetRuntime::acquire())
        return INTERPOSER_COMMS_ERR_NETWORK;

    auto* server = new (std::nothrow) interposer_comms_server();
    if (!server) {
        EnetRuntime::release();
        return INTERPOSER_COMMS_ERR_OUT_OF_MEMORY;
    }

    server->config = *config;
    server->config.bind_address = copy_string(server->bind_address, config->bind_address);
    server->config.name = copy_string(server->name, config->name);
    server->config.password = copy_string(server->password, config->password);
    server->config.admin_key = copy_string(server->admin_key, config->admin_key);

    interposer_comms_zone_config default_zone;
    interposer_comms_zone_config_init(&default_zone);
    server->zones[0] = default_zone;

    *out_server = server;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_start(interposer_comms_server* server)
{
    if (!server)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    if (server->running.load())
        return INTERPOSER_COMMS_ERR_INVALID_STATE;

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = server->config.port;
    if (!server->bind_address.empty() && enet_address_set_host(&address, server->bind_address.c_str()) != 0)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;

    const enet_uint32 bandwidth = server->config.max_total_kbps * 125; // kbit/s -> bytes/s; 0 = unlimited
    // Extra peer slots so handshakes can still be rejected with a reason when the server is full.
    server->host = enet_host_create(&address, server->config.max_players + 8, proto::kChannelCount,
                                    bandwidth, bandwidth);
    if (!server->host) {
        INTERPOSER_COMMS_LOG_ERROR_F("failed to bind voice server to port %u", server->config.port);
        return INTERPOSER_COMMS_ERR_NETWORK;
    }

    server->running = true;
    server->thread = std::thread(network_thread, server);
    INTERPOSER_COMMS_LOG_INFO_F("voice server listening on port %u", server->config.port);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_stop(interposer_comms_server* server)
{
    if (!server)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    if (!server->running.exchange(false))
        return INTERPOSER_COMMS_ERR_INVALID_STATE;
    if (server->thread.joinable())
        server->thread.join();

    std::lock_guard lock(server->mutex);
    shutdown_network(server);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API void INTERPOSER_COMMS_CALL interposer_comms_server_destroy(interposer_comms_server* server)
{
    if (!server)
        return;
    interposer_comms_server_stop(server);
    delete server;
    EnetRuntime::release();
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_poll_event(interposer_comms_server* server, interposer_comms_server_event* out_event)
{
    if (!server || !out_event)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    if (server->events.empty())
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    *out_event = server->events.front();
    server->events.pop_front();
    return INTERPOSER_COMMS_OK;
}

/* ---- Players ------------------------------------------------------------ */

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_add_player(interposer_comms_server* server, uint64_t game_player_id,
                                                          const char* name, interposer_comms_player_id* out_player)
{
    if (!server || !name || !*name || !out_player)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);

    if (server->players.size() >= server->config.max_players)
        return INTERPOSER_COMMS_ERR_LIMIT_REACHED;
    if (game_player_id != 0) {
        for (auto& [id, player] : server->players) {
            if (player.game_player_id == game_player_id)
                return INTERPOSER_COMMS_ERR_ALREADY_EXISTS;
        }
    }

    Player player;
    player.id = server->next_player_id++;
    player.game_player_id = game_player_id;
    player.name = name;
    player.authority = server->config.position_authority;
    *out_player = player.id;
    server->players.emplace(player.id, std::move(player));
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_remove_player(interposer_comms_server* server, interposer_comms_player_id player)
{
    if (!server)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    if (!find_player(server, player))
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    erase_player(server, player);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_find_player(interposer_comms_server* server, uint64_t game_player_id,
                                                           interposer_comms_player_id* out_player)
{
    if (!server || !out_player || game_player_id == 0)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    for (auto& [id, player] : server->players) {
        if (player.game_player_id == game_player_id) {
            *out_player = id;
            return INTERPOSER_COMMS_OK;
        }
    }
    return INTERPOSER_COMMS_ERR_NOT_FOUND;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_get_player_info(interposer_comms_server* server, interposer_comms_player_id player,
                                                               interposer_comms_server_player_info* out_info)
{
    if (!server || !out_info)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    INTERPOSER_COMMS_REQUIRE_PLAYER(p, player);

    *out_info = interposer_comms_server_player_info{};
    out_info->id = p->id;
    out_info->game_player_id = p->game_player_id;
    copy_to(out_info->name, p->name);
    out_info->connected = p->peer ? 1 : 0;
    out_info->speaking = p->speaking ? 1 : 0;
    out_info->server_muted = p->server_muted ? 1 : 0;
    out_info->zone = p->zone;
    out_info->position = p->position;
    out_info->position_authority = p->authority;
    out_info->ping_ms = p->peer ? p->peer->roundTripTime : 0;
    out_info->upstream_kbps = p->upstream_meter.kbps();
    out_info->downstream_kbps = p->downstream_meter.kbps();
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_list_players(interposer_comms_server* server, interposer_comms_player_id* out_ids,
                                                            uint32_t capacity, uint32_t* out_count)
{
    if (!server || !out_count || (capacity > 0 && !out_ids))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    std::vector<interposer_comms_player_id> ids;
    for (auto& [id, player] : server->players)
        ids.push_back(id);
    return fill_ids(ids, out_ids, capacity, out_count);
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_create_join_token(interposer_comms_server* server, interposer_comms_player_id player,
                                                                 char* out_token, size_t token_capacity)
{
    if (!server || !out_token)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    if (token_capacity < 33)
        return INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL;
    std::lock_guard lock(server->mutex);
    INTERPOSER_COMMS_REQUIRE_PLAYER(p, player);

    std::string token = generate_token();
    server->tokens[token] = Token{ p->id, now_ms() + server->config.join_token_lifetime_ms };
    copy_to(out_token, token_capacity, token);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_kick_player(interposer_comms_server* server, interposer_comms_player_id player, const char* reason)
{
    if (!server)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    return kick_player(server, player, reason ? reason : "kicked");
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_set_player_muted(interposer_comms_server* server, interposer_comms_player_id player, int muted)
{
    if (!server)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    return set_player_muted(server, player, muted != 0);
}

/* ---- Proximity ---------------------------------------------------------- */

INTERPOSER_COMMS_API void INTERPOSER_COMMS_CALL interposer_comms_zone_config_init(interposer_comms_zone_config* config)
{
    if (!config)
        return;
    *config = interposer_comms_zone_config{};
    config->struct_size = sizeof(interposer_comms_zone_config);
    config->min_distance = 2.0f;
    config->max_distance = 30.0f;
    config->rolloff = INTERPOSER_COMMS_ROLLOFF_INVERSE;
    config->up = interposer_comms_vec3{ 0.0f, 1.0f, 0.0f };
    config->left_handed = 0;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_configure_zone(interposer_comms_server* server, interposer_comms_zone_id zone,
                                                              const interposer_comms_zone_config* config)
{
    if (!server || !valid_zone_config(config))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    server->zones[zone] = *config;
    server->zones[zone].struct_size = sizeof(interposer_comms_zone_config);
    resize_zone_grid(server, zone);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_update_player_position(interposer_comms_server* server, interposer_comms_player_id player,
                                                                      interposer_comms_zone_id zone, interposer_comms_vec3 position,
                                                                      const interposer_comms_vec3* forward)
{
    if (!server || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    INTERPOSER_COMMS_REQUIRE_PLAYER(p, player);

    set_position(server, *p, zone, position, forward);
    // Restart client speed checks from the authoritative position.
    p->last_client_position_ms = 0;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_clear_player_position(interposer_comms_server* server, interposer_comms_player_id player)
{
    if (!server)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    INTERPOSER_COMMS_REQUIRE_PLAYER(p, player);
    clear_position(server, *p);
    p->last_client_position_ms = 0;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_set_position_authority(interposer_comms_server* server, interposer_comms_player_id player,
                                                                      interposer_comms_position_authority authority)
{
    if (!server || (authority != INTERPOSER_COMMS_POSITION_SERVER && authority != INTERPOSER_COMMS_POSITION_CLIENT))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    INTERPOSER_COMMS_REQUIRE_PLAYER(p, player);
    p->authority = authority;
    p->last_client_position_ms = 0;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_set_pair_attenuation(interposer_comms_server* server, interposer_comms_player_id listener,
                                                                    interposer_comms_player_id speaker, float gain)
{
    if (!server || !(gain >= 0.0f))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    INTERPOSER_COMMS_REQUIRE_PLAYER(p, listener);
    if (!find_player(server, speaker))
        return INTERPOSER_COMMS_ERR_NOT_FOUND;

    if (gain >= 1.0f)
        p->pair_gain.erase(speaker);
    else
        p->pair_gain[speaker] = gain;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_set_proximity_enabled(interposer_comms_server* server, interposer_comms_player_id player,
                                                                     int can_speak, int can_listen)
{
    if (!server)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    INTERPOSER_COMMS_REQUIRE_PLAYER(p, player);
    p->proximity_speak = can_speak != 0;
    p->proximity_listen = can_listen != 0;
    return INTERPOSER_COMMS_OK;
}

/* ---- Groups ------------------------------------------------------------- */

INTERPOSER_COMMS_API void INTERPOSER_COMMS_CALL interposer_comms_group_config_init(interposer_comms_group_config* config)
{
    if (!config)
        return;
    *config = interposer_comms_group_config{};
    config->struct_size = sizeof(interposer_comms_group_config);
    config->type = INTERPOSER_COMMS_GROUP_SERVER;
    config->parent = INTERPOSER_COMMS_INVALID_ID;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_create_group(interposer_comms_server* server, const interposer_comms_group_config* config,
                                                            interposer_comms_group_id* out_group)
{
    if (!server || !config || !out_group || config->struct_size < sizeof(interposer_comms_group_config)
        || !config->name || !*config->name || std::strlen(config->name) > proto::kMaxGroupNameLength
        || (config->type != INTERPOSER_COMMS_GROUP_SERVER && config->type != INTERPOSER_COMMS_GROUP_CHANNEL))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    return create_group(server, config->type, config->name, to_string(config->password), INTERPOSER_COMMS_INVALID_ID,
                        config->parent, config->max_members, config->players_can_leave != 0, out_group);
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_destroy_group(interposer_comms_server* server, interposer_comms_group_id group)
{
    if (!server)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    if (!server->groups.count(group))
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    destroy_group(server, group);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_add_to_group(interposer_comms_server* server, interposer_comms_group_id group, interposer_comms_player_id player)
{
    if (!server)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    return add_member(server, group, player);
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_remove_from_group(interposer_comms_server* server, interposer_comms_group_id group, interposer_comms_player_id player)
{
    if (!server)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    return remove_member(server, group, player);
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_set_member_permissions(interposer_comms_server* server, interposer_comms_group_id group,
                                                                      interposer_comms_player_id player, int can_speak, int can_listen)
{
    if (!server)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    auto it = server->groups.find(group);
    if (it == server->groups.end())
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    auto member = it->second.members.find(player);
    if (member == it->second.members.end())
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    member->second.can_speak = can_speak != 0;
    member->second.can_listen = can_listen != 0;
    sync_group(server, group);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_list_groups(interposer_comms_server* server, interposer_comms_group_id* out_ids,
                                                           uint32_t capacity, uint32_t* out_count)
{
    if (!server || !out_count || (capacity > 0 && !out_ids))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    std::vector<interposer_comms_group_id> ids;
    for (auto& [id, group] : server->groups)
        ids.push_back(id);
    return fill_ids(ids, out_ids, capacity, out_count);
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_get_group_info(interposer_comms_server* server, interposer_comms_group_id group,
                                                              interposer_comms_server_group_info* out_info)
{
    if (!server || !out_info)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    auto it = server->groups.find(group);
    if (it == server->groups.end())
        return INTERPOSER_COMMS_ERR_NOT_FOUND;

    const Group& g = it->second;
    *out_info = interposer_comms_server_group_info{};
    out_info->id = g.id;
    out_info->type = g.type;
    copy_to(out_info->name, g.name);
    out_info->owner = g.owner;
    out_info->parent = g.parent;
    out_info->member_count = static_cast<uint32_t>(g.members.size());
    out_info->max_members = g.max_members;
    out_info->has_password = g.password.empty() ? 0 : 1;
    out_info->players_can_leave = g.type != INTERPOSER_COMMS_GROUP_SERVER || g.players_can_leave ? 1 : 0;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_list_group_members(interposer_comms_server* server, interposer_comms_group_id group,
                                                                  interposer_comms_player_id* out_ids, uint32_t capacity,
                                                                  uint32_t* out_count)
{
    if (!server || !out_count || (capacity > 0 && !out_ids))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    auto it = server->groups.find(group);
    if (it == server->groups.end())
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    std::vector<interposer_comms_player_id> ids;
    for (auto& [id, member] : it->second.members)
        ids.push_back(id);
    return fill_ids(ids, out_ids, capacity, out_count);
}

/* ---- Limits & management ------------------------------------------------ */

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_update_limits(interposer_comms_server* server, const interposer_comms_server_config* config)
{
    if (!server || !config || config->struct_size < sizeof(interposer_comms_server_config))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    return apply_limits(server, *config);
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_get_config(interposer_comms_server* server, interposer_comms_server_config* out_config)
{
    if (!server || !out_config)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(server->mutex);
    *out_config = server->config;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_COMMS_API interposer_comms_result INTERPOSER_COMMS_CALL interposer_comms_server_execute_command(interposer_comms_server* server, const char* command,
                                                               char* out_text, size_t text_capacity)
{
    if (!server || !command)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::string output;
    interposer_comms_result result;
    {
        std::lock_guard lock(server->mutex);
        result = execute_command(server, command, output);
    }
    if (out_text && text_capacity > 0) {
        copy_to(out_text, text_capacity, output);
        if (output.size() >= text_capacity && result == INTERPOSER_COMMS_OK)
            result = INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL;
    }
    return result;
}

} // extern "C"
