#include "voice/server/server_impl.h"

#include "voice/common/log.h"

#include <algorithm>
#include <cmath>

namespace comms::server {

namespace {

constexpr uint64_t kHandshakeTimeoutMs = 5000;
constexpr uint64_t kSpeakingTimeoutMs = 400;
constexpr uint64_t kForwardSlotTimeoutMs = 400;
constexpr uint64_t kRejectionEventIntervalMs = 1000;
constexpr uint32_t kLoopIntervalMs = 5;

enum RoutePriority : int { kPriorityProximity = 1, kPriorityGroup = 2, kPriorityWhisper = 3 };

struct Route {
    int             priority = 0;
    interposer_comms_target_kind kind = INTERPOSER_COMMS_TARGET_PROXIMITY;
    interposer_comms_group_id  group = INTERPOSER_COMMS_INVALID_ID;
    uint8_t         gain = 255;
    int8_t          pan = 0;
    uint16_t        distance_dm = proto::kDistanceUnknown;
};

interposer_comms_player_id peer_player(ENetPeer* peer)
{
    return static_cast<interposer_comms_player_id>(reinterpret_cast<uintptr_t>(peer->data));
}

void set_peer_player(ENetPeer* peer, interposer_comms_player_id id)
{
    peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
}

bool constant_time_equal(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

void reject(interposer_comms_server* server, ENetPeer* peer, interposer_comms_result result, const std::string& reason)
{
    proto::Writer w(proto::kReject);
    w.i32(result).str(reason);
    send(peer, proto::kChannelControl, w, true);
    enet_peer_disconnect_later(peer, 0);
    server->pending_peers.erase(peer);
    push_event(server, INTERPOSER_COMMS_SERVER_EVENT_AUTH_FAILED, INTERPOSER_COMMS_INVALID_ID, INTERPOSER_COMMS_INVALID_ID, result, reason);
    INTERPOSER_COMMS_LOG_INFO_F("rejected client: %s", reason.c_str());
}

// ---- Proximity math --------------------------------------------------------

interposer_comms_vec3 sub(const interposer_comms_vec3& a, const interposer_comms_vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
float dot(const interposer_comms_vec3& a, const interposer_comms_vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(const interposer_comms_vec3& v) { return std::sqrt(dot(v, v)); }
interposer_comms_vec3 cross(const interposer_comms_vec3& a, const interposer_comms_vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

float rolloff_gain(const interposer_comms_zone_config& zone, float distance)
{
    if (distance <= zone.min_distance)
        return 1.0f;
    if (distance >= zone.max_distance)
        return 0.0f;

    float gain = 1.0f;
    switch (zone.rolloff) {
    case INTERPOSER_COMMS_ROLLOFF_LINEAR:
        return 1.0f - (distance - zone.min_distance) / (zone.max_distance - zone.min_distance);
    case INTERPOSER_COMMS_ROLLOFF_INVERSE:
        gain = zone.min_distance > 0.0f ? zone.min_distance / distance : 1.0f;
        break;
    case INTERPOSER_COMMS_ROLLOFF_EXPONENTIAL:
        gain = zone.min_distance > 0.0f ? (zone.min_distance / distance) * (zone.min_distance / distance) : 1.0f;
        break;
    }

    // Non-linear curves never reach zero; fade out over the last 10% so the cut-off at max_distance is inaudible.
    const float fade_start = zone.max_distance * 0.9f;
    if (distance > fade_start)
        gain *= (zone.max_distance - distance) / (zone.max_distance - fade_start);
    return gain;
}

float stereo_pan(const interposer_comms_zone_config& zone, const Player& listener, const interposer_comms_vec3& to_speaker, float distance)
{
    if (!listener.has_forward || distance < 1e-3f)
        return 0.0f;
    interposer_comms_vec3 up = zone.up;
    if (length(up) < 1e-6f)
        up = { 0.0f, 1.0f, 0.0f };
    const interposer_comms_vec3 right = zone.left_handed ? cross(up, listener.forward) : cross(listener.forward, up);
    const float right_length = length(right);
    if (right_length < 1e-6f)
        return 0.0f;
    return std::clamp(dot(to_speaker, right) / (right_length * distance), -1.0f, 1.0f);
}

void offer(std::unordered_map<interposer_comms_player_id, Route>& routes, interposer_comms_player_id listener, const Route& route)
{
    auto [it, inserted] = routes.try_emplace(listener, route);
    if (!inserted && route.priority > it->second.priority)
        it->second = route;
}

void route_proximity(interposer_comms_server* server, const Player& speaker, std::unordered_map<interposer_comms_player_id, Route>& routes)
{
    if (!speaker.has_position || !speaker.proximity_speak)
        return;
    const interposer_comms_zone_config& zone = zone_config(server, speaker.zone);
    auto grid = server->grids.find(speaker.zone);
    if (grid == server->grids.end())
        return;

    grid->second.query(speaker.position, zone.max_distance, [&](interposer_comms_player_id id) {
        const Player* found = find_player(server, id);
        if (!found || id == speaker.id || !found->peer || !found->has_position || !found->proximity_listen
            || found->zone != speaker.zone)
            return;
        const Player& listener = *found;

        const interposer_comms_vec3 to_speaker = sub(speaker.position, listener.position);
        const float distance = length(to_speaker);
        float gain = rolloff_gain(zone, distance);
        if (auto pair = listener.pair_gain.find(speaker.id); pair != listener.pair_gain.end())
            gain *= pair->second;
        if (gain <= 0.0f)
            return;

        Route route;
        route.priority = kPriorityProximity;
        route.kind = INTERPOSER_COMMS_TARGET_PROXIMITY;
        route.gain = static_cast<uint8_t>(std::lround(std::clamp(gain, 0.0f, 1.0f) * 255.0f));
        route.pan = static_cast<int8_t>(std::lround(stereo_pan(zone, listener, to_speaker, distance) * 127.0f));
        route.distance_dm = static_cast<uint16_t>(std::min(distance * 10.0f, 65534.0f));
        offer(routes, id, route);
    });
}

// Higher is better: route priority first, then (for proximity) nearer speakers.
uint32_t route_score(const Route& route)
{
    const uint32_t closeness = route.kind == INTERPOSER_COMMS_TARGET_PROXIMITY ? 0xFFFFu - route.distance_dm : 0xFFFFu;
    return static_cast<uint32_t>(route.priority) << 16 | closeness;
}

// Enforces max_forwarded_speakers for one listener. Returns whether this speaker may be forwarded.
bool claim_forward_slot(interposer_comms_server* server, Player& listener, interposer_comms_player_id speaker, const Route& route,
                        uint64_t now)
{
    const uint32_t limit = server->config.max_forwarded_speakers;
    if (limit == 0)
        return true;

    auto& slots = listener.forward_slots;
    for (auto it = slots.begin(); it != slots.end();) {
        if (now - it->second.last_ms > kForwardSlotTimeoutMs)
            it = slots.erase(it);
        else
            ++it;
    }

    const uint32_t score = route_score(route);
    if (auto it = slots.find(speaker); it != slots.end()) {
        it->second = { score, now };
        return true;
    }
    if (slots.size() < limit) {
        slots[speaker] = { score, now };
        return true;
    }

    auto worst = std::min_element(slots.begin(), slots.end(),
                                  [](const auto& a, const auto& b) { return a.second.score < b.second.score; });
    if (score <= worst->second.score)
        return false;
    slots.erase(worst); // the evicted speaker's stream times out on the listener's side
    slots[speaker] = { score, now };
    return true;
}

void route_group(interposer_comms_server* server, const Player& speaker, interposer_comms_group_id group_id,
                 std::unordered_map<interposer_comms_player_id, Route>& routes)
{
    auto group = server->groups.find(group_id);
    if (group == server->groups.end())
        return;
    auto self = group->second.members.find(speaker.id);
    if (self == group->second.members.end() || !self->second.can_speak)
        return;

    Route route;
    route.priority = kPriorityGroup;
    route.kind = INTERPOSER_COMMS_TARGET_GROUP;
    route.group = group_id;
    for (auto& [id, member] : group->second.members) {
        if (id == speaker.id || !member.can_listen || !member.listening)
            continue;
        const Player* listener = find_player(server, id);
        if (listener && listener->peer)
            offer(routes, id, route);
    }
}

void route_whisper(interposer_comms_server* server, const Player& speaker, interposer_comms_player_id target,
                   std::unordered_map<interposer_comms_player_id, Route>& routes)
{
    const Player* listener = find_player(server, target);
    if (!listener || !listener->peer || target == speaker.id)
        return;
    Route route;
    route.priority = kPriorityWhisper;
    route.kind = INTERPOSER_COMMS_TARGET_WHISPER;
    offer(routes, target, route);
}

// ---- Message handlers ------------------------------------------------------

void handle_hello(interposer_comms_server* server, ENetPeer* peer, proto::Reader& r)
{
    const uint32_t version = r.u32();
    const std::string token = r.str();
    const std::string password = r.str();
    const uint64_t game_player_id = r.u64();
    const std::string name = r.str();
    const std::string admin_key = r.str();
    const bool admin_only = r.u8() != 0;
    if (version != proto::kProtocolVersion)
        return reject(server, peer, INTERPOSER_COMMS_ERR_VERSION_MISMATCH, "protocol version mismatch");
    if (!r.ok())
        return reject(server, peer, INTERPOSER_COMMS_ERR_INVALID_ARGUMENT, "malformed hello");

    bool admin = false;
    if (!admin_key.empty() || admin_only) {
        if (server->admin_key.empty() || !constant_time_equal(admin_key, server->admin_key))
            return reject(server, peer, INTERPOSER_COMMS_ERR_AUTH_FAILED, "invalid admin key");
        admin = true;
    }
    if (admin_only) {
        server->pending_peers.erase(peer);
        server->admin_peers[peer].control_rate.configure(10, 40);
        proto::Writer welcome(proto::kWelcome);
        welcome.u32(INTERPOSER_COMMS_INVALID_ID).str(server->name).u32(client_bitrate(server))
               .u16(static_cast<uint16_t>(server->config.opus_frame_ms)).u16(0);
        send(peer, proto::kChannelControl, welcome, true);
        INTERPOSER_COMMS_LOG_INFO_F("admin console connected");
        return;
    }

    Player* player = nullptr;
    switch (server->config.auth_mode) {
    case INTERPOSER_COMMS_AUTH_TOKEN: {
        auto it = server->tokens.find(token);
        if (it == server->tokens.end() || it->second.expires_ms < now_ms())
            return reject(server, peer, INTERPOSER_COMMS_ERR_AUTH_FAILED, "invalid or expired join token");
        player = find_player(server, it->second.player);
        server->tokens.erase(it);
        break;
    }
    case INTERPOSER_COMMS_AUTH_CLAIM:
        if (!server->password.empty() && password != server->password)
            return reject(server, peer, INTERPOSER_COMMS_ERR_AUTH_FAILED, "wrong password");
        for (auto& [id, candidate] : server->players) {
            if (game_player_id != 0 ? candidate.game_player_id == game_player_id : candidate.name == name) {
                player = &candidate;
                break;
            }
        }
        break;
    case INTERPOSER_COMMS_AUTH_PASSWORD:
    case INTERPOSER_COMMS_AUTH_OPEN:
        if (server->config.auth_mode == INTERPOSER_COMMS_AUTH_PASSWORD && password != server->password)
            return reject(server, peer, INTERPOSER_COMMS_ERR_AUTH_FAILED, "wrong password");
        if (server->players.size() >= server->config.max_players)
            return reject(server, peer, INTERPOSER_COMMS_ERR_LIMIT_REACHED, "server is full");
        {
            Player created;
            created.id = server->next_player_id++;
            created.game_player_id = game_player_id;
            created.name = name.empty() ? "Player " + std::to_string(created.id) : name;
            created.ephemeral = true;
            created.authority = server->config.position_authority;
            player = &server->players.emplace(created.id, std::move(created)).first->second;
        }
        break;
    }

    if (!player)
        return reject(server, peer, INTERPOSER_COMMS_ERR_AUTH_FAILED, "no matching player registered on the server");
    if (player->peer)
        return reject(server, peer, INTERPOSER_COMMS_ERR_ALREADY_EXISTS, "player already has a voice client connected");

    player->peer = peer;
    player->admin = admin;
    player->flood_strikes = 0;
    player->forward_slots.clear();
    configure_limits(server, *player);
    set_peer_player(peer, player->id);
    server->pending_peers.erase(peer);

    player->sent_bitrate = client_bitrate(server);
    proto::Writer welcome(proto::kWelcome);
    welcome.u32(player->id).str(server->name).u32(player->sent_bitrate)
           .u16(static_cast<uint16_t>(server->config.opus_frame_ms));
    uint16_t connected = 0;
    for (auto& [id, p] : server->players)
        connected += p.peer ? 1 : 0;
    welcome.u16(connected);
    for (auto& [id, p] : server->players) {
        if (p.peer)
            roster_entry(p).write(welcome);
    }
    send(peer, proto::kChannelControl, welcome, true);

    proto::Writer added(proto::kPlayerAdded);
    roster_entry(*player).write(added);
    broadcast(server, added, player->id);
    sync_player_connected(server, *player);

    push_event(server, INTERPOSER_COMMS_SERVER_EVENT_PLAYER_CONNECTED, player->id);
    INTERPOSER_COMMS_LOG_INFO_F("player %u (%s) connected", player->id, player->name.c_str());
}

void handle_voice(interposer_comms_server* server, Player& speaker, proto::Reader& r, size_t packet_bytes)
{
    const uint64_t received_ms = now_ms();
    const size_t wire_bytes = packet_bytes + proto::kPacketOverheadBytes;
    speaker.upstream_meter.add(wire_bytes);
    if (!speaker.voice_rate.consume(1, received_ms) || !speaker.upstream.consume(static_cast<double>(wire_bytes), received_ms))
        return; // over the client's packet rate or upstream bandwidth cap

    const uint16_t seq = r.u16();
    const uint8_t flags = r.u8();
    const uint8_t target_count = r.u8();
    if (!r.ok() || target_count > INTERPOSER_COMMS_MAX_TALK_TARGETS)
        return;

    interposer_comms_talk_target targets[INTERPOSER_COMMS_MAX_TALK_TARGETS];
    for (uint8_t i = 0; i < target_count; ++i) {
        targets[i].kind = static_cast<interposer_comms_target_kind>(r.u8());
        targets[i].id = r.u32();
    }
    const size_t opus_length = r.remaining();
    if (!r.ok() || opus_length == 0 || opus_length > proto::kMaxOpusPacket || speaker.server_muted)
        return;

    const bool end_of_spurt = (flags & proto::kVoiceEndOfSpurt) != 0;
    speaker.last_voice_ms = now_ms();
    if (!speaker.speaking && !end_of_spurt) {
        speaker.speaking = true;
        push_event(server, INTERPOSER_COMMS_SERVER_EVENT_PLAYER_STARTED_SPEAKING, speaker.id);
    } else if (speaker.speaking && end_of_spurt) {
        speaker.speaking = false;
        push_event(server, INTERPOSER_COMMS_SERVER_EVENT_PLAYER_STOPPED_SPEAKING, speaker.id);
    }

    std::unordered_map<interposer_comms_player_id, Route> routes;
    for (uint8_t i = 0; i < target_count; ++i) {
        switch (targets[i].kind) {
        case INTERPOSER_COMMS_TARGET_PROXIMITY: route_proximity(server, speaker, routes); break;
        case INTERPOSER_COMMS_TARGET_GROUP:     route_group(server, speaker, targets[i].id, routes); break;
        case INTERPOSER_COMMS_TARGET_WHISPER:   route_whisper(server, speaker, targets[i].id, routes); break;
        default: break;
        }
    }

    const uint8_t* opus = r.cursor();
    const uint64_t now = received_ms;
    const bool limited = server->config.max_forwarded_speakers != 0;
    for (auto& [listener_id, route] : routes) {
        Player* listener = find_player(server, listener_id);
        if (end_of_spurt) {
            // Only listeners that were actually receiving this speaker need the end marker; it frees their slot.
            if (limited && !listener->forward_slots.erase(speaker.id))
                continue;
        } else if (!claim_forward_slot(server, *listener, speaker.id, route, now)) {
            continue;
        }

        proto::Writer w(proto::kVoiceDown);
        w.u32(speaker.id).u16(seq).u8(flags).u8(static_cast<uint8_t>(route.kind)).u32(route.group)
         .u8(route.gain).i8(route.pan).u16(route.distance_dm).raw(opus, opus_length);

        // Downstream cap: proximity may not dip into the last quarter of the budget, which is kept for
        // whispers and group (radio) traffic.
        const double cost = static_cast<double>(w.data().size() + proto::kPacketOverheadBytes);
        const double reserve = route.kind == INTERPOSER_COMMS_TARGET_PROXIMITY ? listener->downstream.capacity() * 0.25 : 0.0;
        if (!listener->downstream.consume(cost, now, reserve))
            continue;

        listener->downstream_meter.add(static_cast<uint64_t>(cost));
        send(listener->peer, proto::kChannelVoice, w, false);
    }
}

void handle_admin_command(interposer_comms_server* server, ENetPeer* peer, proto::Reader& r, bool authorized,
                          interposer_comms_player_id invoker)
{
    const uint32_t request_id = r.u32();
    const std::string command = r.text();
    if (!r.ok())
        return;

    std::string output;
    interposer_comms_result result = INTERPOSER_COMMS_ERR_PERMISSION_DENIED;
    if (authorized) {
        INTERPOSER_COMMS_LOG_INFO_F("admin command from %s: %s", invoker ? ("player " + std::to_string(invoker)).c_str() : "console",
                         command.c_str());
        push_event(server, INTERPOSER_COMMS_SERVER_EVENT_ADMIN_COMMAND, invoker, INTERPOSER_COMMS_INVALID_ID, 0, command);
        result = execute_command(server, command, output); // may kick/remove the invoker; don't touch Player after
    } else {
        output = "not an admin";
    }

    proto::Writer response(proto::kAdminResponse);
    response.u32(request_id).i32(result).text(output);
    send(peer, proto::kChannelControl, response, true);
}

void handle_client_position(interposer_comms_server* server, Player& player, proto::Reader& r)
{
    const interposer_comms_zone_id zone = r.u32();
    const interposer_comms_vec3 position = r.vec3();
    const bool has_forward = r.u8() != 0;
    const interposer_comms_vec3 forward = has_forward ? r.vec3() : interposer_comms_vec3{};
    if (!r.ok() || player.authority != INTERPOSER_COMMS_POSITION_CLIENT)
        return;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        return;

    const uint64_t now = now_ms();
    const float max_speed = server->config.max_client_speed;
    if (max_speed > 0.0f && player.has_position && player.zone == zone && player.last_client_position_ms != 0) {
        const float seconds = std::max(1, static_cast<int>(now - player.last_client_position_ms)) / 1000.0f;
        // 25% tolerance plus a small constant for jitter in report timing.
        if (length(sub(position, player.position)) > max_speed * seconds * 1.25f + 0.5f) {
            if (now - player.last_rejection_event_ms >= kRejectionEventIntervalMs) {
                player.last_rejection_event_ms = now;
                push_event(server, INTERPOSER_COMMS_SERVER_EVENT_POSITION_REJECTED, player.id, INTERPOSER_COMMS_INVALID_ID,
                           INTERPOSER_COMMS_ERR_PERMISSION_DENIED, "position change exceeds max_client_speed");
            }
            return;
        }
    }

    set_position(server, player, zone, position, has_forward ? &forward : nullptr);
    player.last_client_position_ms = now;
}

void handle_receive(interposer_comms_server* server, ENetPeer* peer, const ENetPacket* packet)
{
    proto::Reader r(packet->data, packet->dataLength);
    const uint8_t id = r.u8();
    if (!r.ok())
        return;

    const interposer_comms_player_id player_id = peer_player(peer);
    if (player_id == INTERPOSER_COMMS_INVALID_ID) {
        if (id == proto::kHello && server->pending_peers.count(peer)) {
            handle_hello(server, peer, r);
        } else if (auto admin = server->admin_peers.find(peer); admin != server->admin_peers.end()) {
            if (id == proto::kAdminCommand && admin->second.control_rate.consume(1, now_ms()))
                handle_admin_command(server, peer, r, true, INTERPOSER_COMMS_INVALID_ID);
        }
        return;
    }

    Player* player = find_player(server, player_id);
    if (!player || player->peer != peer)
        return;

    switch (id) {
    case proto::kVoiceUp:
        handle_voice(server, *player, r, packet->dataLength);
        break;
    case proto::kClientPosition:
        if (player->position_rate.consume(1, now_ms()))
            handle_client_position(server, *player, r);
        break;
    case proto::kCreateGroup:
    case proto::kJoinGroup:
    case proto::kLeaveGroup:
    case proto::kInviteToGroup:
    case proto::kSetListening:
        if (allow_control_request(server, *player))
            handle_group_request(server, *player, id, r);
        break;
    case proto::kAdminCommand:
        if (allow_control_request(server, *player))
            handle_admin_command(server, peer, r, player->admin, player_id);
        break;
    default:
        break;
    }
}

void handle_disconnect(interposer_comms_server* server, ENetPeer* peer)
{
    server->pending_peers.erase(peer);
    server->admin_peers.erase(peer);
    const interposer_comms_player_id player_id = peer_player(peer);
    set_peer_player(peer, INTERPOSER_COMMS_INVALID_ID);
    if (player_id == INTERPOSER_COMMS_INVALID_ID)
        return;

    Player* player = find_player(server, player_id);
    if (!player || player->peer != peer)
        return;

    player->peer = nullptr;
    player->speaking = false;

    proto::Writer removed(proto::kPlayerRemoved);
    removed.u32(player_id);
    broadcast(server, removed);
    sync_player_disconnected(server, player_id);
    push_event(server, INTERPOSER_COMMS_SERVER_EVENT_PLAYER_DISCONNECTED, player_id);
    INTERPOSER_COMMS_LOG_INFO_F("player %u disconnected", player_id);

    if (player->ephemeral)
        erase_player(server, player_id);
}

void check_timeouts(interposer_comms_server* server)
{
    const uint64_t now = now_ms();
    tick_meters(server, now);

    for (auto it = server->pending_peers.begin(); it != server->pending_peers.end();) {
        if (now - it->second > kHandshakeTimeoutMs) {
            enet_peer_disconnect(it->first, 0);
            it = server->pending_peers.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& [id, player] : server->players) {
        if (player.speaking && now - player.last_voice_ms > kSpeakingTimeoutMs) {
            player.speaking = false;
            push_event(server, INTERPOSER_COMMS_SERVER_EVENT_PLAYER_STOPPED_SPEAKING, id);
        }
    }

    for (auto it = server->tokens.begin(); it != server->tokens.end();) {
        if (it->second.expires_ms < now)
            it = server->tokens.erase(it);
        else
            ++it;
    }
}

} // namespace

// ---- Shared helpers --------------------------------------------------------

void push_event(interposer_comms_server* server, interposer_comms_server_event_type type, interposer_comms_player_id player, interposer_comms_group_id group,
                int32_t reason, const std::string& text)
{
    constexpr size_t kMaxQueuedEvents = 4096;
    if (server->events.size() >= kMaxQueuedEvents)
        server->events.pop_front();

    interposer_comms_server_event event{};
    event.type = type;
    event.player = player;
    event.group = group;
    event.reason = reason;
    copy_to(event.text, text);
    server->events.push_back(event);
}

Player* find_player(interposer_comms_server* server, interposer_comms_player_id id)
{
    auto it = server->players.find(id);
    return it == server->players.end() ? nullptr : &it->second;
}

const interposer_comms_zone_config& zone_config(interposer_comms_server* server, interposer_comms_zone_id zone)
{
    auto it = server->zones.find(zone);
    if (it != server->zones.end())
        return it->second;
    return server->zones.at(0);
}

void set_position(interposer_comms_server* server, Player& player, interposer_comms_zone_id zone, const interposer_comms_vec3& position,
                  const interposer_comms_vec3* forward)
{
    if (player.has_position && player.zone != zone)
        clear_position(server, player);

    auto grid = server->grids.find(zone);
    if (grid == server->grids.end())
        grid = server->grids.emplace(zone, SpatialGrid(zone_config(server, zone).max_distance)).first;
    grid->second.update(player.id, position);

    player.has_position = true;
    player.zone = zone;
    player.position = position;
    player.has_forward = forward != nullptr;
    player.forward = forward ? *forward : interposer_comms_vec3{};
}

void clear_position(interposer_comms_server* server, Player& player)
{
    if (!player.has_position)
        return;
    auto grid = server->grids.find(player.zone);
    if (grid != server->grids.end()) {
        grid->second.remove(player.id);
        if (grid->second.size() == 0)
            server->grids.erase(grid);
    }
    player.has_position = false;
}

void resize_zone_grid(interposer_comms_server* server, interposer_comms_zone_id zone)
{
    if (zone != 0) {
        auto grid = server->grids.find(zone);
        if (grid != server->grids.end())
            grid->second.set_cell_size(zone_config(server, zone).max_distance);
        return;
    }
    // Zone 0's config is the fallback for unconfigured zones, so any grid may be affected (no-op if unchanged).
    for (auto& [id, grid] : server->grids)
        grid.set_cell_size(zone_config(server, id).max_distance);
}

proto::RosterEntry roster_entry(const Player& player)
{
    proto::RosterEntry entry;
    entry.id = player.id;
    entry.game_player_id = player.game_player_id;
    entry.name = player.name;
    entry.server_muted = player.server_muted;
    return entry;
}

void broadcast(interposer_comms_server* server, const proto::Writer& writer, interposer_comms_player_id except)
{
    for (auto& [id, player] : server->players) {
        if (player.peer && id != except)
            send(player.peer, proto::kChannelControl, writer, true);
    }
}

void kick(interposer_comms_server* server, Player& player, const std::string& reason)
{
    if (!player.peer)
        return;

    proto::Writer kicked(proto::kKicked);
    kicked.str(reason);
    send(player.peer, proto::kChannelControl, kicked, true);
    set_peer_player(player.peer, INTERPOSER_COMMS_INVALID_ID);
    enet_peer_disconnect_later(player.peer, 0);
    player.peer = nullptr;
    player.speaking = false;

    proto::Writer removed(proto::kPlayerRemoved);
    removed.u32(player.id);
    broadcast(server, removed);
    sync_player_disconnected(server, player.id);
    push_event(server, INTERPOSER_COMMS_SERVER_EVENT_PLAYER_DISCONNECTED, player.id, INTERPOSER_COMMS_INVALID_ID, 0, reason);
}

void erase_player(interposer_comms_server* server, interposer_comms_player_id id)
{
    Player* player = find_player(server, id);
    if (!player)
        return;
    kick(server, *player, "removed from server");

    // remove_member may destroy private groups, so collect first.
    std::vector<interposer_comms_group_id> memberships;
    for (auto& [group_id, group] : server->groups) {
        group.invited.erase(id);
        if (group.members.count(id))
            memberships.push_back(group_id);
    }
    for (interposer_comms_group_id group_id : memberships)
        remove_member(server, group_id, id);
    for (auto it = server->tokens.begin(); it != server->tokens.end();) {
        if (it->second.player == id)
            it = server->tokens.erase(it);
        else
            ++it;
    }
    for (auto& [other_id, other] : server->players) {
        other.pair_gain.erase(id);
        other.forward_slots.erase(id);
    }

    clear_position(server, *player);
    server->players.erase(id);
}

void network_thread(interposer_comms_server* server)
{
    while (server->running.load()) {
        wait_for_socket(server->host, kLoopIntervalMs);

        std::lock_guard lock(server->mutex);
        ENetEvent event;
        while (enet_host_service(server->host, &event, 0) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                set_peer_player(event.peer, INTERPOSER_COMMS_INVALID_ID);
                enet_peer_timeout(event.peer, 32, 5000, 10000);
                server->pending_peers[event.peer] = now_ms();
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                handle_receive(server, event.peer, event.packet);
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                handle_disconnect(server, event.peer);
                break;
            default:
                break;
            }
        }
        check_timeouts(server);
        enet_host_flush(server->host);
    }
}

void shutdown_network(interposer_comms_server* server)
{
    for (auto& [id, player] : server->players) {
        if (!player.peer)
            continue;
        enet_peer_disconnect_now(player.peer, 0);
        player.peer = nullptr;
        player.speaking = false;
        push_event(server, INTERPOSER_COMMS_SERVER_EVENT_PLAYER_DISCONNECTED, id, INTERPOSER_COMMS_INVALID_ID, 0, "server stopped");
    }
    for (auto& [peer, since] : server->pending_peers)
        enet_peer_disconnect_now(peer, 0);
    server->pending_peers.clear();
    for (auto& [peer, session] : server->admin_peers)
        enet_peer_disconnect_now(peer, 0);
    server->admin_peers.clear();

    for (auto it = server->players.begin(); it != server->players.end();) {
        if (it->second.ephemeral) {
            const interposer_comms_player_id id = it->first;
            ++it;
            erase_player(server, id);
        } else {
            ++it;
        }
    }

    enet_host_destroy(server->host);
    server->host = nullptr;
}

} // namespace comms::server
