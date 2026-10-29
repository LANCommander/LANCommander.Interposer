#include "voice/client/client_impl.h"

#include "voice/common/log.h"

#include <algorithm>

namespace comms::client {

namespace {

constexpr uint32_t kLoopIntervalMs = 5;

void add_roster_player(interposer_comms_client* client, const proto::RosterEntry& entry, bool announce)
{
    RemotePlayer& player = client->players[entry.id];
    player.id = entry.id;
    player.game_player_id = entry.game_player_id;
    player.name = entry.name;
    player.server_muted = entry.server_muted;
    if (announce && entry.id != client->local_player)
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_PLAYER_JOINED, entry.id);
}

void handle_welcome(interposer_comms_client* client, proto::Reader& r)
{
    const interposer_comms_player_id local = r.u32();
    const std::string server_name = r.str();
    const uint32_t bitrate = r.u32();
    const uint16_t frame_ms = r.u16();
    const uint16_t roster_count = r.u16();

    std::vector<proto::RosterEntry> roster;
    for (uint16_t i = 0; i < roster_count && r.ok(); ++i)
        roster.push_back(proto::RosterEntry::read(r));
    if (!r.ok() || (frame_ms != 10 && frame_ms != 20 && frame_ms != 40 && frame_ms != 60)) {
        client->disconnect_reason = INTERPOSER_COMMS_ERR_VERSION_MISMATCH;
        enet_peer_disconnect(client->peer, 0);
        return;
    }

    client->local_player = local;
    client->server_name = server_name;
    client->frame_ms = frame_ms;
    client->is_admin = !client->admin_key.empty(); // the server rejects the connection on a bad key
    update_encoder_settings(client, bitrate);

    for (const auto& entry : roster)
        add_roster_player(client, entry, false);

    set_state(client, INTERPOSER_COMMS_STATE_CONNECTED);
    for (const auto& entry : roster) {
        if (entry.id != local)
            push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_PLAYER_JOINED, entry.id);
    }
    INTERPOSER_COMMS_LOG_INFO_F("connected to '%s' as player %u", server_name.c_str(), local);
}

void handle_receive(interposer_comms_client* client, const ENetPacket* packet)
{
    proto::Reader r(packet->data, packet->dataLength);
    const uint8_t id = r.u8();
    if (!r.ok())
        return;

    if (client->state == INTERPOSER_COMMS_STATE_AUTHENTICATING) {
        if (id == proto::kWelcome) {
            handle_welcome(client, r);
        } else if (id == proto::kReject) {
            const int32_t result = r.i32();
            client->disconnect_reason = r.ok() ? result : INTERPOSER_COMMS_ERR_AUTH_FAILED;
            client->disconnect_text = r.str();
        }
        return;
    }
    if (client->state != INTERPOSER_COMMS_STATE_CONNECTED)
        return;

    switch (id) {
    case proto::kVoiceDown:
        handle_voice_down(client, r);
        break;
    case proto::kPlayerAdded:
    case proto::kPlayerUpdated: {
        const proto::RosterEntry entry = proto::RosterEntry::read(r);
        if (r.ok())
            add_roster_player(client, entry, id == proto::kPlayerAdded && !find_player(client, entry.id));
        break;
    }
    case proto::kPlayerRemoved: {
        const interposer_comms_player_id player_id = r.u32();
        if (RemotePlayer* player = r.ok() ? find_player(client, player_id) : nullptr) {
            if (player->speaking)
                push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_PLAYER_STOPPED_SPEAKING, player_id);
            release_player_audio(client, *player);
            client->players.erase(player_id);
            push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_PLAYER_LEFT, player_id);
        }
        break;
    }
    case proto::kKicked: {
        const std::string reason = r.str();
        client->disconnect_reason = INTERPOSER_COMMS_ERR_PERMISSION_DENIED;
        client->disconnect_text = reason;
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_KICKED, INTERPOSER_COMMS_INVALID_ID, INTERPOSER_COMMS_INVALID_ID,
                   INTERPOSER_COMMS_ERR_PERMISSION_DENIED, reason);
        break;
    }
    case proto::kConfigUpdate: {
        const uint32_t bitrate = r.u32();
        const uint16_t frame_ms = r.u16();
        if (!r.ok() || (frame_ms != 10 && frame_ms != 20 && frame_ms != 40 && frame_ms != 60))
            break;
        update_encoder_settings(client, bitrate);
        client->frame_ms = frame_ms;
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_CONFIG_CHANGED, INTERPOSER_COMMS_INVALID_ID, INTERPOSER_COMMS_INVALID_ID,
                   static_cast<int32_t>(bitrate));
        break;
    }
    case proto::kAdminResponse: {
        const uint32_t request_id = r.u32();
        const int32_t result = r.i32();
        std::string output = r.text();
        if (!r.ok())
            break;
        constexpr size_t kMaxKeptResponses = 16;
        if (client->admin_responses.size() >= kMaxKeptResponses)
            client->admin_responses.pop_front();
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_ADMIN_RESPONSE, INTERPOSER_COMMS_INVALID_ID, INTERPOSER_COMMS_INVALID_ID, result, output);
        client->events.back().request_id = request_id;
        client->admin_responses.emplace_back(request_id, std::move(output));
        break;
    }
    default:
        handle_group_message(client, id, r);
        break;
    }
}

void handle_disconnect(interposer_comms_client* client)
{
    int32_t reason = client->disconnect_reason;
    if (reason == INTERPOSER_COMMS_OK && !client->user_disconnect)
        reason = client->state == INTERPOSER_COMMS_STATE_CONNECTING ? INTERPOSER_COMMS_ERR_TIMEOUT : INTERPOSER_COMMS_ERR_NETWORK;

    const std::string text = client->disconnect_text;
    client->peer = nullptr;
    clear_session(client);
    set_state(client, INTERPOSER_COMMS_STATE_DISCONNECTED, reason);
    if (!text.empty() && !client->events.empty() && client->events.back().type == INTERPOSER_COMMS_CLIENT_EVENT_STATE_CHANGED)
        copy_to(client->events.back().text, text);
}

} // namespace

void push_event(interposer_comms_client* client, interposer_comms_client_event_type type, interposer_comms_player_id player, interposer_comms_group_id group,
                int32_t reason, const std::string& text)
{
    constexpr size_t kMaxQueuedEvents = 4096;
    if (client->events.size() >= kMaxQueuedEvents)
        client->events.pop_front();

    interposer_comms_client_event event{};
    event.type = type;
    event.state = client->state;
    event.player = player;
    event.group = group;
    event.reason = reason;
    copy_to(event.text, text);
    client->events.push_back(event);
}

void set_state(interposer_comms_client* client, interposer_comms_connection_state state, int32_t reason)
{
    if (client->state == state)
        return;
    client->state = state;
    push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_STATE_CHANGED, INTERPOSER_COMMS_INVALID_ID, INTERPOSER_COMMS_INVALID_ID, reason);
}

RemotePlayer* find_player(interposer_comms_client* client, interposer_comms_player_id id)
{
    auto it = client->players.find(id);
    return it == client->players.end() ? nullptr : &it->second;
}

void release_player_audio(interposer_comms_client* client, RemotePlayer& player)
{
    if (player.audio) {
        std::lock_guard lock(client->mix_mutex);
        client->mix_list.erase(std::remove(client->mix_list.begin(), client->mix_list.end(), player.audio),
                               client->mix_list.end());
        player.audio.reset();
    }
    if (player.decoder) {
        opus_decoder_destroy(player.decoder);
        player.decoder = nullptr;
    }
}

void clear_session(interposer_comms_client* client)
{
    for (auto& [id, player] : client->players)
        release_player_audio(client, player);
    client->players.clear();
    client->groups.clear();
    client->is_admin = false;
    client->local_player = INTERPOSER_COMMS_INVALID_ID;
    client->server_name.clear();
    if (client->transmitting) {
        client->transmitting = false;
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_LOCAL_TRANSMIT_STOPPED);
    }
}

void network_thread(interposer_comms_client* client)
{
    while (client->running.load()) {
        wait_for_socket(client->host, kLoopIntervalMs);

        std::lock_guard lock(client->mutex);
        ENetEvent event;
        while (enet_host_service(client->host, &event, 0) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                set_state(client, INTERPOSER_COMMS_STATE_AUTHENTICATING);
                proto::Writer hello(proto::kHello);
                hello.u32(proto::kProtocolVersion).str(client->token).str(client->password)
                     .u64(client->game_player_id).str(client->player_name)
                     .str(client->admin_key).u8(client->admin_only ? 1 : 0);
                send(client->peer, proto::kChannelControl, hello, true);
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE:
                handle_receive(client, event.packet);
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                handle_disconnect(client);
                break;
            default:
                break;
            }
        }

        process_capture(client);
        pump_speakers(client);
        enet_host_flush(client->host);
    }
}

} // namespace comms::client
