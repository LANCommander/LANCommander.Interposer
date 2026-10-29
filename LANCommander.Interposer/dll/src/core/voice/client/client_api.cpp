#include "voice/client/client_impl.h"
#include "instance.h"

#include "voice/common/enet_runtime.h"
#include "voice/common/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

using namespace comms;
using namespace comms::client;

namespace {

bool valid_targets(const interposer_comms_talk_target* targets, uint32_t count)
{
    if (count > INTERPOSER_COMMS_MAX_TALK_TARGETS || (count > 0 && !targets))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        switch (targets[i].kind) {
        case INTERPOSER_COMMS_TARGET_PROXIMITY:
            break;
        case INTERPOSER_COMMS_TARGET_GROUP:
        case INTERPOSER_COMMS_TARGET_WHISPER:
            if (targets[i].id == INTERPOSER_COMMS_INVALID_ID)
                return false;
            break;
        default:
            return false;
        }
    }
    return true;
}

bool valid_voice_activation(const interposer_comms_voice_activation_settings* settings)
{
    return settings
        && settings->struct_size >= sizeof(interposer_comms_voice_activation_settings)
        && settings->threshold_db >= -100.0f && settings->threshold_db <= 0.0f
        && settings->attack_ms <= 1000
        && settings->hold_ms <= 5000;
}

bool is_finite(const interposer_comms_vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool valid_transmit_mode(interposer_comms_transmit_mode mode)
{
    return mode == INTERPOSER_COMMS_TRANSMIT_PUSH_TO_TALK || mode == INTERPOSER_COMMS_TRANSMIT_VOICE_ACTIVITY || mode == INTERPOSER_COMMS_TRANSMIT_CONTINUOUS;
}

void destroy_client(interposer_comms_client* client)
{
    close_capture_device(client);
    close_playback_device(client);
    if (client->context_ready)
        ma_context_uninit(&client->context);
    if (client->resampler_rate)
        ma_linear_resampler_uninit(&client->resampler, nullptr);
    if (client->capture_ring_ready)
        ma_pcm_rb_uninit(&client->capture_ring);
    for (auto& [id, player] : client->players)
        release_player_audio(client, player);
    if (client->encoder)
        opus_encoder_destroy(client->encoder);
    if (client->host)
        enet_host_destroy(client->host);
    delete client;
}

void fill_player_info(const RemotePlayer& player, interposer_comms_client_player_info* out)
{
    *out = interposer_comms_client_player_info{};
    out->id = player.id;
    out->game_player_id = player.game_player_id;
    copy_to(out->name, player.name);
    out->speaking = player.speaking ? 1 : 0;
    out->locally_muted = player.locally_muted ? 1 : 0;
    out->local_volume = player.local_volume;
    out->server_muted = player.server_muted ? 1 : 0;
}

} // namespace

#define INTERPOSER_COMMS_NOT_IMPLEMENTED(client) \
    do { if (!(client)) return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT; return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED; } while (0)

extern "C" {

/* ---- Lifecycle ---------------------------------------------------------- */

INTERPOSER_API void INTERPOSER_CALL interposer_comms_voice_activation_settings_init(interposer_comms_voice_activation_settings* settings)
{
    FORWARD_TO_PRIMARY(interposer_comms_voice_activation_settings_init, settings);
    if (!settings)
        return;
    *settings = interposer_comms_voice_activation_settings{};
    settings->struct_size = sizeof(interposer_comms_voice_activation_settings);
    settings->threshold_db = -45.0f;
    settings->attack_ms = 10;
    settings->hold_ms = 300;
    settings->auto_threshold = 0;
}

INTERPOSER_API void INTERPOSER_CALL interposer_comms_client_config_init(interposer_comms_client_config* config)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_config_init, config);
    if (!config)
        return;
    *config = interposer_comms_client_config{};
    config->struct_size = sizeof(interposer_comms_client_config);
    config->capture_mode = INTERPOSER_COMMS_CAPTURE_DEVICE;
    config->playback_mode = INTERPOSER_COMMS_PLAYBACK_DEVICE;
    config->transmit_mode = INTERPOSER_COMMS_TRANSMIT_PUSH_TO_TALK;
    interposer_comms_voice_activation_settings_init(&config->voice_activation);
    config->ptt_release_delay_ms = 150;
    config->jitter_target_ms = 60;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_create(const interposer_comms_client_config* config, interposer_comms_client** out_client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_create, config, out_client);
    if (!config || !out_client || config->struct_size < sizeof(interposer_comms_client_config))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    if (!valid_transmit_mode(config->transmit_mode) || !valid_voice_activation(&config->voice_activation)
        || config->jitter_target_ms > 1000 || config->ptt_release_delay_ms > 2000)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;

    if (!EnetRuntime::acquire())
        return INTERPOSER_COMMS_ERR_NETWORK;

    auto* client = new (std::nothrow) interposer_comms_client();
    if (!client) {
        EnetRuntime::release();
        return INTERPOSER_COMMS_ERR_OUT_OF_MEMORY;
    }

    client->config = *config;
    client->capture_device_id = to_string(config->capture_device_id);
    client->playback_device_id = to_string(config->playback_device_id);
    client->config.capture_device_id = nullptr;
    client->config.playback_device_id = nullptr;

    int opus_error = OPUS_OK;
    client->encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &opus_error);
    client->host = enet_host_create(nullptr, 1, proto::kChannelCount, 0, 0);
    if (opus_error != OPUS_OK || !client->host) {
        INTERPOSER_COMMS_LOG_ERROR_F("client init failed: %s", opus_error != OPUS_OK ? opus_strerror(opus_error) : "enet host");
        if (opus_error != OPUS_OK)
            client->encoder = nullptr;
        destroy_client(client);
        EnetRuntime::release();
        return opus_error != OPUS_OK ? INTERPOSER_COMMS_ERR_UNKNOWN : INTERPOSER_COMMS_ERR_NETWORK;
    }
    update_encoder_settings(client, 32000);

    // Missing audio devices are not fatal: a listen-only or mic-less client is still useful.
    if (!open_capture_device(client)) {
        INTERPOSER_COMMS_LOG_WARN_F("could not open capture device");
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_AUDIO_DEVICE_LOST, INTERPOSER_COMMS_INVALID_ID, INTERPOSER_COMMS_INVALID_ID,
                   INTERPOSER_COMMS_ERR_AUDIO_DEVICE, "capture");
    }
    if (!open_playback_device(client)) {
        INTERPOSER_COMMS_LOG_WARN_F("could not open playback device");
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_AUDIO_DEVICE_LOST, INTERPOSER_COMMS_INVALID_ID, INTERPOSER_COMMS_INVALID_ID,
                   INTERPOSER_COMMS_ERR_AUDIO_DEVICE, "playback");
    }

    client->running = true;
    client->thread = std::thread(network_thread, client);

    INTERPOSER_COMMS_LOG_DEBUG_F("client created (%s)", opus_get_version_string());
    *out_client = client;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API void INTERPOSER_CALL interposer_comms_client_destroy(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_destroy, client);
    if (!client)
        return;
    client->running = false;
    if (client->thread.joinable())
        client->thread.join();

    if (client->peer) {
        enet_peer_disconnect_now(client->peer, 0);
        client->peer = nullptr;
    }
    destroy_client(client);
    EnetRuntime::release();
}

INTERPOSER_API void INTERPOSER_CALL interposer_comms_connect_info_init(interposer_comms_connect_info* info)
{
    FORWARD_TO_PRIMARY(interposer_comms_connect_info_init, info);
    if (!info)
        return;
    *info = interposer_comms_connect_info{};
    info->struct_size = sizeof(interposer_comms_connect_info);
    info->port = INTERPOSER_COMMS_DEFAULT_PORT;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_connect(interposer_comms_client* client, const interposer_comms_connect_info* info)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_connect, client, info);
    if (!client || !info || info->struct_size < sizeof(interposer_comms_connect_info) || !info->host || !*info->host)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    if (info->admin_only && (!info->admin_key || !*info->admin_key))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;

    ENetAddress address{};
    if (enet_address_set_host(&address, info->host) != 0) // may block on DNS
        return INTERPOSER_COMMS_ERR_NETWORK;
    address.port = info->port ? info->port : INTERPOSER_COMMS_DEFAULT_PORT;

    std::lock_guard lock(client->mutex);
    if (client->state != INTERPOSER_COMMS_STATE_DISCONNECTED || client->peer)
        return INTERPOSER_COMMS_ERR_INVALID_STATE;

    client->peer = enet_host_connect(client->host, &address, proto::kChannelCount, proto::kProtocolVersion);
    if (!client->peer)
        return INTERPOSER_COMMS_ERR_NETWORK;
    enet_peer_timeout(client->peer, 32, 3000, 8000);

    client->token = to_string(info->token);
    client->password = to_string(info->password);
    client->player_name = to_string(info->player_name);
    client->game_player_id = info->game_player_id;
    client->admin_key = to_string(info->admin_key);
    client->admin_only = info->admin_only != 0;
    client->disconnect_reason = INTERPOSER_COMMS_OK;
    client->disconnect_text.clear();
    client->user_disconnect = false;
    set_state(client, INTERPOSER_COMMS_STATE_CONNECTING);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_disconnect(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_disconnect, client);
    if (!client)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    if (!client->peer)
        return INTERPOSER_COMMS_ERR_INVALID_STATE;

    client->user_disconnect = true;
    if (client->state == INTERPOSER_COMMS_STATE_CONNECTED) {
        // Graceful: the DISCONNECT event completes the transition.
        enet_peer_disconnect(client->peer, 0);
        return INTERPOSER_COMMS_OK;
    }
    enet_peer_reset(client->peer);
    client->peer = nullptr;
    clear_session(client);
    set_state(client, INTERPOSER_COMMS_STATE_DISCONNECTED, INTERPOSER_COMMS_OK);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_connection_state INTERPOSER_CALL interposer_comms_client_get_state(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_state, client);
    if (!client)
        return INTERPOSER_COMMS_STATE_DISCONNECTED;
    std::lock_guard lock(client->mutex);
    return client->state;
}

INTERPOSER_API interposer_comms_player_id INTERPOSER_CALL interposer_comms_client_get_local_player(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_local_player, client);
    if (!client)
        return INTERPOSER_COMMS_INVALID_ID;
    std::lock_guard lock(client->mutex);
    return client->local_player;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_poll_event(interposer_comms_client* client, interposer_comms_client_event* out_event)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_poll_event, client, out_event);
    if (!client || !out_event)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    if (client->events.empty())
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    *out_event = client->events.front();
    client->events.pop_front();
    return INTERPOSER_COMMS_OK;
}

/* ---- Discovery (M5) ----------------------------------------------------- */

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_discover(interposer_comms_client* client, uint16_t discovery_port, uint32_t timeout_ms) { FORWARD_TO_PRIMARY(interposer_comms_client_discover, client, discovery_port, timeout_ms); INTERPOSER_COMMS_NOT_IMPLEMENTED(client); }
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_discovered_servers(interposer_comms_client* client, interposer_comms_discovered_server* out_servers, uint32_t capacity, uint32_t* out_count) { FORWARD_TO_PRIMARY(interposer_comms_client_get_discovered_servers, client, out_servers, capacity, out_count); INTERPOSER_COMMS_NOT_IMPLEMENTED(client); }

/* ---- Audio devices ------------------------------------------------------ */

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_capture_device(interposer_comms_client* client, const char* device_id)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_capture_device, client, device_id);
    if (!client)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    if (client->config.capture_mode != INTERPOSER_COMMS_CAPTURE_DEVICE)
        return INTERPOSER_COMMS_ERR_INVALID_STATE;
    client->capture_device_id = to_string(device_id);
    close_capture_device(client);
    return open_capture_device(client) ? INTERPOSER_COMMS_OK : INTERPOSER_COMMS_ERR_AUDIO_DEVICE;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_playback_device(interposer_comms_client* client, const char* device_id)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_playback_device, client, device_id);
    if (!client)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    if (client->config.playback_mode != INTERPOSER_COMMS_PLAYBACK_DEVICE)
        return INTERPOSER_COMMS_ERR_INVALID_STATE;
    client->playback_device_id = to_string(device_id);
    close_playback_device(client);
    return open_playback_device(client) ? INTERPOSER_COMMS_OK : INTERPOSER_COMMS_ERR_AUDIO_DEVICE;
}

/* ---- Transmit ----------------------------------------------------------- */

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_transmit_mode(interposer_comms_client* client, interposer_comms_transmit_mode mode)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_transmit_mode, client, mode);
    if (!client || !valid_transmit_mode(mode))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    client->config.transmit_mode = mode;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_transmit_mode INTERPOSER_CALL interposer_comms_client_get_transmit_mode(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_transmit_mode, client);
    if (!client)
        return INTERPOSER_COMMS_TRANSMIT_PUSH_TO_TALK;
    std::lock_guard lock(client->mutex);
    return client->config.transmit_mode;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_talk_targets(interposer_comms_client* client, const interposer_comms_talk_target* targets, uint32_t count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_talk_targets, client, targets, count);
    if (!client || !valid_targets(targets, count))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    client->default_targets.assign(targets, targets + count);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_ptt_targets(interposer_comms_client* client, uint32_t slot,
                                                               const interposer_comms_talk_target* targets, uint32_t count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_ptt_targets, client, slot, targets, count);
    if (!client || slot >= INTERPOSER_COMMS_MAX_PTT_SLOTS || !valid_targets(targets, count))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    client->ptt_slots[slot].targets.assign(targets, targets + count);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_push_to_talk(interposer_comms_client* client, uint32_t slot, int pressed)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_push_to_talk, client, slot, pressed);
    if (!client || slot >= INTERPOSER_COMMS_MAX_PTT_SLOTS)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    PttSlot& ptt = client->ptt_slots[slot];
    if (ptt.pressed && !pressed)
        ptt.release_until_ms = now_ms() + client->config.ptt_release_delay_ms;
    ptt.pressed = pressed != 0;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_ptt_release_delay(interposer_comms_client* client, uint32_t delay_ms)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_ptt_release_delay, client, delay_ms);
    if (!client || delay_ms > 2000)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    client->config.ptt_release_delay_ms = delay_ms;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_voice_activation(interposer_comms_client* client, const interposer_comms_voice_activation_settings* settings)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_voice_activation, client, settings);
    if (!client || !valid_voice_activation(settings))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    client->config.voice_activation = *settings;
    client->config.voice_activation.struct_size = sizeof(interposer_comms_voice_activation_settings);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_voice_activation(interposer_comms_client* client, interposer_comms_voice_activation_settings* out_settings)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_voice_activation, client, out_settings);
    if (!client || !out_settings)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    *out_settings = client->config.voice_activation;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_transmit_state(interposer_comms_client* client, interposer_comms_transmit_state* out_state)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_transmit_state, client, out_state);
    if (!client || !out_state)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);

    *out_state = interposer_comms_transmit_state{};
    for (uint32_t i = 0; i < INTERPOSER_COMMS_MAX_PTT_SLOTS; ++i) {
        if (client->ptt_slots[i].pressed)
            out_state->ptt_slots_held |= 1u << i;
    }
    out_state->transmitting = client->transmitting ? 1 : 0;
    out_state->voice_detected = client->vad_open ? 1 : 0;
    out_state->input_level_db = client->input_level_db;
    out_state->noise_floor_db = client->noise_floor_db;
    out_state->self_muted = client->self_muted ? 1 : 0;
    out_state->self_deafened = client->self_deafened ? 1 : 0;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_self_muted(interposer_comms_client* client, int muted)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_self_muted, client, muted);
    if (!client)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    client->self_muted = muted != 0;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_self_deafened(interposer_comms_client* client, int deafened)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_self_deafened, client, deafened);
    if (!client)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    client->self_deafened = deafened != 0;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_input_gain(interposer_comms_client* client, float gain)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_input_gain, client, gain);
    if (!client || !(gain >= 0.0f && gain <= 4.0f))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    client->input_gain = gain;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_master_volume(interposer_comms_client* client, float volume)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_master_volume, client, volume);
    if (!client || !(volume >= 0.0f && volume <= 2.0f))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    client->master_volume = volume;
    return INTERPOSER_COMMS_OK;
}

/* ---- Other players & talk indicators ------------------------------------ */

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_player_muted(interposer_comms_client* client, interposer_comms_player_id player, int muted)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_player_muted, client, player, muted);
    if (!client)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    RemotePlayer* p = find_player(client, player);
    if (!p)
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    p->locally_muted = muted != 0;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_player_volume(interposer_comms_client* client, interposer_comms_player_id player, float volume)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_player_volume, client, player, volume);
    if (!client || !(volume >= 0.0f && volume <= 2.0f))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    RemotePlayer* p = find_player(client, player);
    if (!p)
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    p->local_volume = volume;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_player_info(interposer_comms_client* client, interposer_comms_player_id player,
                                                               interposer_comms_client_player_info* out_info)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_player_info, client, player, out_info);
    if (!client || !out_info)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    RemotePlayer* p = find_player(client, player);
    if (!p)
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    fill_player_info(*p, out_info);
    if (player == client->local_player)
        out_info->speaking = client->transmitting ? 1 : 0;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_list_players(interposer_comms_client* client, interposer_comms_player_id* out_ids,
                                                            uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_list_players, client, out_ids, capacity, out_count);
    if (!client || !out_count || (capacity > 0 && !out_ids))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    uint32_t count = 0;
    for (auto& [id, player] : client->players) {
        if (count < capacity)
            out_ids[count] = id;
        ++count;
    }
    *out_count = count;
    return count > capacity ? INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL : INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_find_player(interposer_comms_client* client, uint64_t game_player_id,
                                                           interposer_comms_player_id* out_player)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_find_player, client, game_player_id, out_player);
    if (!client || !out_player || game_player_id == 0)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    for (auto& [id, player] : client->players) {
        if (player.game_player_id == game_player_id) {
            *out_player = id;
            return INTERPOSER_COMMS_OK;
        }
    }
    return INTERPOSER_COMMS_ERR_NOT_FOUND;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_active_speakers(interposer_comms_client* client, interposer_comms_speaker_info* out_speakers,
                                                                   uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_active_speakers, client, out_speakers, capacity, out_count);
    if (!client || !out_count || (capacity > 0 && !out_speakers))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    uint32_t count = 0;
    for (auto& [id, player] : client->players) {
        if (!player.speaking)
            continue;
        if (count < capacity) {
            interposer_comms_speaker_info& info = out_speakers[count];
            info.player = id;
            info.kind = player.kind;
            info.group = player.group;
            info.level_db = player.level_db;
            info.distance = player.kind == INTERPOSER_COMMS_TARGET_PROXIMITY ? player.distance : -1.0f;
            info.pan = player.kind == INTERPOSER_COMMS_TARGET_PROXIMITY ? player.pan : 0.0f;
            info.locally_muted = player.locally_muted ? 1 : 0;
        }
        ++count;
    }
    *out_count = count;
    return count > capacity ? INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL : INTERPOSER_COMMS_OK;
}

INTERPOSER_API int INTERPOSER_CALL interposer_comms_client_is_player_speaking(interposer_comms_client* client, interposer_comms_player_id player)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_is_player_speaking, client, player);
    if (!client)
        return 0;
    std::lock_guard lock(client->mutex);
    if (player != INTERPOSER_COMMS_INVALID_ID && player == client->local_player)
        return client->transmitting ? 1 : 0;
    RemotePlayer* p = find_player(client, player);
    return p && p->speaking ? 1 : 0;
}

/* Groups: client_groups.cpp */

/* ---- Positions ---------------------------------------------------------- */

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_report_position(interposer_comms_client* client, interposer_comms_zone_id zone, interposer_comms_vec3 position,
                                                               const interposer_comms_vec3* forward)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_report_position, client, zone, position, forward);
    if (!client || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    if (client->state != INTERPOSER_COMMS_STATE_CONNECTED)
        return INTERPOSER_COMMS_ERR_INVALID_STATE;

    proto::Writer w(proto::kClientPosition);
    w.u32(zone).vec3(position).u8(forward ? 1 : 0);
    if (forward)
        w.vec3(*forward);
    send(client->peer, proto::kChannelPosition, w, false);
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_listener(interposer_comms_client* client, interposer_comms_vec3 position, interposer_comms_vec3 forward,
                                                            interposer_comms_vec3 right)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_listener, client, position, forward, right);
    (void)forward; // reserved for front/back cues (HRTF)
    if (!client || !is_finite(position) || !is_finite(right))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    client->has_listener = true;
    client->listener_position = position;
    client->listener_right = right;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_clear_listener(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_clear_listener, client);
    if (!client)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    client->has_listener = false;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_speaker_position_override(interposer_comms_client* client, interposer_comms_player_id player,
                                                                             const interposer_comms_vec3* position)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_speaker_position_override, client, player, position);
    if (!client || (position && !is_finite(*position)))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    RemotePlayer* p = find_player(client, player);
    if (!p)
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    p->has_position_override = position != nullptr;
    if (position)
        p->position_override = *position;
    return INTERPOSER_COMMS_OK;
}

/* ---- Remote admin ------------------------------------------------------- */

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_send_admin_command(interposer_comms_client* client, const char* command,
                                                                  uint32_t* out_request_id)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_send_admin_command, client, command, out_request_id);
    if (!client || !command || !*command || std::strlen(command) > proto::kMaxTextLength)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    if (client->state != INTERPOSER_COMMS_STATE_CONNECTED)
        return INTERPOSER_COMMS_ERR_INVALID_STATE;
    if (!client->is_admin)
        return INTERPOSER_COMMS_ERR_PERMISSION_DENIED;

    const uint32_t request_id = client->next_admin_request++;
    proto::Writer w(proto::kAdminCommand);
    w.u32(request_id).text(command);
    send(client->peer, proto::kChannelControl, w, true);
    if (out_request_id)
        *out_request_id = request_id;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_admin_response(interposer_comms_client* client, uint32_t request_id,
                                                                  char* out_text, size_t text_capacity,
                                                                  size_t* out_length)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_admin_response, client, request_id, out_text, text_capacity, out_length);
    if (!client || (text_capacity > 0 && !out_text))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    for (const auto& [id, output] : client->admin_responses) {
        if (id != request_id)
            continue;
        if (out_length)
            *out_length = output.size();
        copy_to(out_text, text_capacity, output);
        return output.size() < text_capacity ? INTERPOSER_COMMS_OK : INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL;
    }
    return INTERPOSER_COMMS_ERR_NOT_FOUND;
}

INTERPOSER_API int INTERPOSER_CALL interposer_comms_client_is_admin(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_is_admin, client);
    if (!client)
        return 0;
    std::lock_guard lock(client->mutex);
    return client->is_admin && client->state == INTERPOSER_COMMS_STATE_CONNECTED ? 1 : 0;
}

/* ---- External audio ----------------------------------------------------- */

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_push_capture(interposer_comms_client* client, const float* samples,
                                                            uint32_t frame_count, uint32_t sample_rate)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_push_capture, client, samples, frame_count, sample_rate);
    if (!client || (frame_count > 0 && !samples) || sample_rate < 8000 || sample_rate > 192000)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    if (client->config.capture_mode != INTERPOSER_COMMS_CAPTURE_EXTERNAL)
        return INTERPOSER_COMMS_ERR_INVALID_STATE;

    if (sample_rate == kSampleRate) {
        write_capture(client, samples, frame_count);
        return INTERPOSER_COMMS_OK;
    }

    if (client->resampler_rate != sample_rate) {
        if (client->resampler_rate)
            ma_linear_resampler_uninit(&client->resampler, nullptr);
        client->resampler_rate = 0;
        ma_linear_resampler_config config = ma_linear_resampler_config_init(ma_format_f32, 1, sample_rate, kSampleRate);
        if (ma_linear_resampler_init(&config, nullptr, &client->resampler) != MA_SUCCESS)
            return INTERPOSER_COMMS_ERR_UNKNOWN;
        client->resampler_rate = sample_rate;
    }

    float out[1024];
    uint32_t consumed = 0;
    while (consumed < frame_count) {
        ma_uint64 in_frames = frame_count - consumed;
        ma_uint64 out_frames = sizeof(out) / sizeof(out[0]);
        if (ma_linear_resampler_process_pcm_frames(&client->resampler, samples + consumed, &in_frames, out, &out_frames) != MA_SUCCESS)
            return INTERPOSER_COMMS_ERR_UNKNOWN;
        write_capture(client, out, static_cast<uint32_t>(out_frames));
        consumed += static_cast<uint32_t>(in_frames);
        if (in_frames == 0 && out_frames == 0)
            break;
    }
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_read_speaker_audio(interposer_comms_client* client, interposer_comms_player_id player,
                                                                  float* out_samples, uint32_t frame_capacity,
                                                                  uint32_t* out_frames)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_read_speaker_audio, client, player, out_samples, frame_capacity, out_frames);
    if (!client || !out_frames || (frame_capacity > 0 && !out_samples))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    if (client->config.playback_mode != INTERPOSER_COMMS_PLAYBACK_EXTERNAL)
        return INTERPOSER_COMMS_ERR_INVALID_STATE;
    RemotePlayer* p = find_player(client, player);
    if (!p)
        return INTERPOSER_COMMS_ERR_NOT_FOUND;

    uint32_t read = 0;
    if (p->audio) {
        read = ring_read(&p->audio->ring, out_samples, frame_capacity);
        const float gain = p->audio->gain_external.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < read; ++i)
            out_samples[i] *= gain;
    }
    std::fill(out_samples + read, out_samples + frame_capacity, 0.0f);
    *out_frames = read;
    return INTERPOSER_COMMS_OK;
}

} // extern "C"
