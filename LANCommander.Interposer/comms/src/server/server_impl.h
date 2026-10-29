#pragma once

#include <comms/server.h>

#include "common/protocol.h"
#include "common/rate_limit.h"
#include "common/util.h"
#include "server/spatial_grid.h"

#include <enet/enet.h>

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

namespace comms::server {

struct Player {
    interposer_comms_player_id id = INTERPOSER_COMMS_INVALID_ID;
    uint64_t        game_player_id = 0;
    std::string     name;
    bool            ephemeral = false;      // created by password/open auth; removed when the client leaves
    ENetPeer*       peer = nullptr;         // non-null while a voice client is attached
    bool            server_muted = false;

    bool            speaking = false;
    uint64_t        last_voice_ms = 0;

    bool            has_position = false;
    interposer_comms_zone_id   zone = 0;
    interposer_comms_vec3      position{};
    bool            has_forward = false;
    interposer_comms_vec3      forward{};
    interposer_comms_position_authority authority = INTERPOSER_COMMS_POSITION_SERVER;
    uint64_t        last_client_position_ms = 0;
    uint64_t        last_rejection_event_ms = 0;
    bool            proximity_speak = true;
    bool            proximity_listen = true;

    // Extra gain applied when this player listens to the keyed speaker.
    std::unordered_map<interposer_comms_player_id, float> pair_gain;

    // Groups this player's client currently has a snapshot of (reset on connect).
    std::set<interposer_comms_group_id> known_groups;

    // Speakers currently being forwarded to this player as a listener (max_forwarded_speakers).
    struct ForwardSlot {
        uint32_t score = 0;
        uint64_t last_ms = 0;
    };
    std::unordered_map<interposer_comms_player_id, ForwardSlot> forward_slots;

    // Remote admin (connected with a valid admin key)
    bool admin = false;

    // Rate limiting and accounting (reconfigured by configure_limits)
    TokenBucket     upstream;        // voice bytes received
    TokenBucket     downstream;      // voice bytes forwarded
    TokenBucket     control_rate;    // control-channel requests
    TokenBucket     position_rate;   // client position reports
    TokenBucket     voice_rate;      // voice packets
    ThroughputMeter upstream_meter;
    ThroughputMeter downstream_meter;
    uint32_t        flood_strikes = 0;
    uint32_t        sent_bitrate = 0; // last codec bitrate pushed to the client
};

// A connection authenticated only with the admin key: no player, no voice.
struct AdminSession {
    TokenBucket control_rate;
};

struct Member {
    bool can_speak = true;   // server permission
    bool can_listen = true;  // server permission
    bool listening = true;   // member's own choice (interposer_comms_client_set_group_listening)
};

struct Group {
    interposer_comms_group_id   id = INTERPOSER_COMMS_INVALID_ID;
    interposer_comms_group_type type = INTERPOSER_COMMS_GROUP_SERVER;
    std::string      name;
    std::string      password;
    interposer_comms_player_id  owner = INTERPOSER_COMMS_INVALID_ID; // PRIVATE groups
    interposer_comms_group_id   parent = INTERPOSER_COMMS_INVALID_ID;
    uint32_t         max_members = 0;
    bool             players_can_leave = false;
    std::map<interposer_comms_player_id, Member> members;
    std::set<interposer_comms_player_id> invited;
};

struct Token {
    interposer_comms_player_id player = INTERPOSER_COMMS_INVALID_ID;
    uint64_t        expires_ms = 0;
};

} // namespace comms::server

struct interposer_comms_server {
    std::mutex          mutex;   // guards everything below, including all ENet calls
    interposer_comms_server_config config{};
    std::string         bind_address;
    std::string         name;
    std::string         password;
    std::string         admin_key;

    ENetHost*           host = nullptr;
    std::thread         thread;
    std::atomic<bool>   running{ false };

    interposer_comms_player_id     next_player_id = 1;
    interposer_comms_group_id      next_group_id = 1;
    std::map<interposer_comms_player_id, comms::server::Player> players;
    std::map<interposer_comms_group_id, comms::server::Group>   groups;
    std::map<interposer_comms_zone_id, interposer_comms_zone_config>       zones;
    std::map<interposer_comms_zone_id, comms::server::SpatialGrid> grids; // positioned players per zone
    std::unordered_map<std::string, comms::server::Token> tokens;
    std::unordered_map<ENetPeer*, uint64_t> pending_peers; // connected, not yet authenticated -> connect time
    std::unordered_map<ENetPeer*, comms::server::AdminSession> admin_peers; // admin-only sessions

    std::deque<interposer_comms_server_event> events;
    uint64_t last_strike_decay_ms = 0;
};

namespace comms::server {

// All functions below require server->mutex to be held.

void push_event(interposer_comms_server* server, interposer_comms_server_event_type type, interposer_comms_player_id player = INTERPOSER_COMMS_INVALID_ID,
                interposer_comms_group_id group = INTERPOSER_COMMS_INVALID_ID, int32_t reason = 0, const std::string& text = {});

Player* find_player(interposer_comms_server* server, interposer_comms_player_id id);
const interposer_comms_zone_config& zone_config(interposer_comms_server* server, interposer_comms_zone_id zone);

// Moves a player's position (and zone) in the spatial index; clear_position removes it.
void set_position(interposer_comms_server* server, Player& player, interposer_comms_zone_id zone, const interposer_comms_vec3& position,
                  const interposer_comms_vec3* forward);
void clear_position(interposer_comms_server* server, Player& player);
// Re-buckets grids after a zone's hearing range changed (zone 0 affects every unconfigured zone).
void resize_zone_grid(interposer_comms_server* server, interposer_comms_zone_id zone);

proto::RosterEntry roster_entry(const Player& player);
void broadcast(interposer_comms_server* server, const proto::Writer& writer, interposer_comms_player_id except = INTERPOSER_COMMS_INVALID_ID);

// Detaches a connected voice client from its player (notifying everyone), then disconnects the peer.
void kick(interposer_comms_server* server, Player& player, const std::string& reason);
// Removes the player entirely: kicks it if connected, drops group memberships and tokens.
void erase_player(interposer_comms_server* server, interposer_comms_player_id id);

// ---- Groups (server_groups.cpp) ----
// Every mutation goes through these so connected clients get consistent snapshots and events fire once.

interposer_comms_result create_group(interposer_comms_server* server, interposer_comms_group_type type, const std::string& name,
                          const std::string& password, interposer_comms_player_id owner, interposer_comms_group_id parent,
                          uint32_t max_members, bool players_can_leave, interposer_comms_group_id* out_group);
void destroy_group(interposer_comms_server* server, interposer_comms_group_id group);
interposer_comms_result add_member(interposer_comms_server* server, interposer_comms_group_id group, interposer_comms_player_id player);
interposer_comms_result remove_member(interposer_comms_server* server, interposer_comms_group_id group, interposer_comms_player_id player);
// Re-sends the group to every connected client it is (or was) visible to.
void sync_group(interposer_comms_server* server, interposer_comms_group_id group);
// Brings a newly connected client up to date and refreshes groups it is a member of for everyone else.
void sync_player_connected(interposer_comms_server* server, Player& player);
void sync_player_disconnected(interposer_comms_server* server, interposer_comms_player_id player);
// Handles kCreateGroup/kJoinGroup/kLeaveGroup/kInviteToGroup/kSetListening from a client.
void handle_group_request(interposer_comms_server* server, Player& player, uint8_t message, proto::Reader& r);

// ---- Limits (server_limits.cpp) ----

bool valid_limits(const interposer_comms_server_config& config);
// Applies runtime-changeable fields and pushes codec changes to clients.
interposer_comms_result apply_limits(interposer_comms_server* server, const interposer_comms_server_config& config);
// Opus bitrate clients should use, accounting for the per-client upstream cap.
uint32_t client_bitrate(const interposer_comms_server* server);
void configure_limits(interposer_comms_server* server, Player& player);
// Consumes one control request; returns false (and may kick the player) when flooding.
bool allow_control_request(interposer_comms_server* server, Player& player);
void tick_meters(interposer_comms_server* server, uint64_t now);

// ---- Admin (server_admin.cpp) ----

interposer_comms_result execute_command(interposer_comms_server* server, const std::string& command, std::string& output);
interposer_comms_result kick_player(interposer_comms_server* server, interposer_comms_player_id player, const std::string& reason);
interposer_comms_result set_player_muted(interposer_comms_server* server, interposer_comms_player_id player, bool muted);

void network_thread(interposer_comms_server* server);
void shutdown_network(interposer_comms_server* server);

} // namespace comms::server
