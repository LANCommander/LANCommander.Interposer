// Voice chat's exports (the interposer_comms_* API), for an embedded build with
// INTERPOSER_FEATURE_VOICE off (see core/interposer_features.h). Without them the build
// needs neither ENet, Opus nor miniaudio.
//
// Every call that would create, drive or query a client or server returns
// INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED, so no handle is ever handed out. The
// *_init helpers stay real, so caller code that fills a config before asking
// is unchanged; so do the version, result-string and logging functions, which
// are in voice/common/ and still built.
//
// Generated from the Comms headers: when a voice export is added there, add it
// here too, or an embedded build without voice fails to link the patch.

#include "LANCommander.Interposer.Comms.Client.h"
#include "LANCommander.Interposer.Comms.Server.h"
#include "instance.h"

extern "C" {

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
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API void INTERPOSER_CALL interposer_comms_client_destroy(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_destroy, client);
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
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_disconnect(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_disconnect, client);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_connection_state INTERPOSER_CALL interposer_comms_client_get_state(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_state, client);
    return INTERPOSER_COMMS_STATE_DISCONNECTED;
}

INTERPOSER_API interposer_comms_player_id INTERPOSER_CALL interposer_comms_client_get_local_player(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_local_player, client);
    return INTERPOSER_COMMS_INVALID_ID;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_poll_event(interposer_comms_client* client, interposer_comms_client_event* out_event)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_poll_event, client, out_event);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_discover(interposer_comms_client* client, uint16_t discovery_port, uint32_t timeout_ms)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_discover, client, discovery_port, timeout_ms);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_discovered_servers(interposer_comms_client* client, interposer_comms_discovered_server* out_servers, uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_discovered_servers, client, out_servers, capacity, out_count);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_list_audio_devices(interposer_comms_device_type type, interposer_comms_audio_device* out_devices, uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_list_audio_devices, type, out_devices, capacity, out_count);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_capture_device(interposer_comms_client* client, const char* device_id)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_capture_device, client, device_id);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_playback_device(interposer_comms_client* client, const char* device_id)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_playback_device, client, device_id);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_transmit_mode(interposer_comms_client* client, interposer_comms_transmit_mode mode)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_transmit_mode, client, mode);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_transmit_mode INTERPOSER_CALL interposer_comms_client_get_transmit_mode(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_transmit_mode, client);
    return INTERPOSER_COMMS_TRANSMIT_PUSH_TO_TALK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_talk_targets(interposer_comms_client* client, const interposer_comms_talk_target* targets, uint32_t count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_talk_targets, client, targets, count);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_ptt_targets(interposer_comms_client* client, uint32_t slot, const interposer_comms_talk_target* targets, uint32_t count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_ptt_targets, client, slot, targets, count);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_push_to_talk(interposer_comms_client* client, uint32_t slot, int pressed)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_push_to_talk, client, slot, pressed);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_ptt_release_delay(interposer_comms_client* client, uint32_t delay_ms)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_ptt_release_delay, client, delay_ms);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

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

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_voice_activation(interposer_comms_client* client, const interposer_comms_voice_activation_settings* settings)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_voice_activation, client, settings);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_voice_activation(interposer_comms_client* client, interposer_comms_voice_activation_settings* out_settings)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_voice_activation, client, out_settings);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_transmit_state(interposer_comms_client* client, interposer_comms_transmit_state* out_state)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_transmit_state, client, out_state);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_self_muted(interposer_comms_client* client, int muted)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_self_muted, client, muted);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_self_deafened(interposer_comms_client* client, int deafened)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_self_deafened, client, deafened);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_input_gain(interposer_comms_client* client, float gain)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_input_gain, client, gain);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_master_volume(interposer_comms_client* client, float volume)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_master_volume, client, volume);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_player_muted(interposer_comms_client* client, interposer_comms_player_id player, int muted)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_player_muted, client, player, muted);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_player_volume(interposer_comms_client* client, interposer_comms_player_id player, float volume)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_player_volume, client, player, volume);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_player_info(interposer_comms_client* client, interposer_comms_player_id player, interposer_comms_client_player_info* out_info)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_player_info, client, player, out_info);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_list_players(interposer_comms_client* client, interposer_comms_player_id* out_ids, uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_list_players, client, out_ids, capacity, out_count);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_find_player(interposer_comms_client* client, uint64_t game_player_id, interposer_comms_player_id* out_player)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_find_player, client, game_player_id, out_player);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_active_speakers(interposer_comms_client* client, interposer_comms_speaker_info* out_speakers, uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_active_speakers, client, out_speakers, capacity, out_count);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API int INTERPOSER_CALL interposer_comms_client_is_player_speaking(interposer_comms_client* client, interposer_comms_player_id player)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_is_player_speaking, client, player);
    return 0;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_list_groups(interposer_comms_client* client, interposer_comms_group_id* out_ids, uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_list_groups, client, out_ids, capacity, out_count);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_group_info(interposer_comms_client* client, interposer_comms_group_id group, interposer_comms_client_group_info* out_info)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_group_info, client, group, out_info);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_list_group_members(interposer_comms_client* client, interposer_comms_group_id group, interposer_comms_player_id* out_ids, uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_list_group_members, client, group, out_ids, capacity, out_count);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_create_private_group(interposer_comms_client* client, const char* name, const char* password)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_create_private_group, client, name, password);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_join_group(interposer_comms_client* client, interposer_comms_group_id group, const char* password)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_join_group, client, group, password);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_leave_group(interposer_comms_client* client, interposer_comms_group_id group)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_leave_group, client, group);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_invite_to_group(interposer_comms_client* client, interposer_comms_group_id group, interposer_comms_player_id player)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_invite_to_group, client, group, player);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_group_listening(interposer_comms_client* client, interposer_comms_group_id group, int listening)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_group_listening, client, group, listening);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_report_position(interposer_comms_client* client, interposer_comms_zone_id zone, interposer_comms_vec3 position, const interposer_comms_vec3* forward)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_report_position, client, zone, position, forward);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_listener(interposer_comms_client* client, interposer_comms_vec3 position, interposer_comms_vec3 forward, interposer_comms_vec3 right)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_listener, client, position, forward, right);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_clear_listener(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_clear_listener, client);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_speaker_position_override(interposer_comms_client* client, interposer_comms_player_id player, const interposer_comms_vec3* position)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_speaker_position_override, client, player, position);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_send_admin_command(interposer_comms_client* client, const char* command, uint32_t* out_request_id)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_send_admin_command, client, command, out_request_id);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_admin_response(interposer_comms_client* client, uint32_t request_id, char* out_text, size_t text_capacity, size_t* out_length)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_admin_response, client, request_id, out_text, text_capacity, out_length);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API int INTERPOSER_CALL interposer_comms_client_is_admin(interposer_comms_client* client)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_is_admin, client);
    return 0;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_push_capture(interposer_comms_client* client, const float* samples, uint32_t frame_count, uint32_t sample_rate)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_push_capture, client, samples, frame_count, sample_rate);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_read_speaker_audio(interposer_comms_client* client, interposer_comms_player_id player, float* out_samples, uint32_t frame_capacity, uint32_t* out_frames)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_read_speaker_audio, client, player, out_samples, frame_capacity, out_frames);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API void INTERPOSER_CALL interposer_comms_server_config_init(interposer_comms_server_config* config)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_config_init, config);
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

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_create(const interposer_comms_server_config* config, interposer_comms_server** out_server)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_create, config, out_server);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_start(interposer_comms_server* server)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_start, server);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_stop(interposer_comms_server* server)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_stop, server);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API void INTERPOSER_CALL interposer_comms_server_destroy(interposer_comms_server* server)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_destroy, server);
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_poll_event(interposer_comms_server* server, interposer_comms_server_event* out_event)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_poll_event, server, out_event);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_add_player(interposer_comms_server* server, uint64_t game_player_id, const char* name, interposer_comms_player_id* out_player)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_add_player, server, game_player_id, name, out_player);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_remove_player(interposer_comms_server* server, interposer_comms_player_id player)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_remove_player, server, player);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_find_player(interposer_comms_server* server, uint64_t game_player_id, interposer_comms_player_id* out_player)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_find_player, server, game_player_id, out_player);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_get_player_info(interposer_comms_server* server, interposer_comms_player_id player, interposer_comms_server_player_info* out_info)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_get_player_info, server, player, out_info);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_list_players(interposer_comms_server* server, interposer_comms_player_id* out_ids, uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_list_players, server, out_ids, capacity, out_count);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_create_join_token(interposer_comms_server* server, interposer_comms_player_id player, char* out_token, size_t token_capacity)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_create_join_token, server, player, out_token, token_capacity);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_kick_player(interposer_comms_server* server, interposer_comms_player_id player, const char* reason)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_kick_player, server, player, reason);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_set_player_muted(interposer_comms_server* server, interposer_comms_player_id player, int muted)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_set_player_muted, server, player, muted);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API void INTERPOSER_CALL interposer_comms_zone_config_init(interposer_comms_zone_config* config)
{
    FORWARD_TO_PRIMARY(interposer_comms_zone_config_init, config);
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

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_configure_zone(interposer_comms_server* server, interposer_comms_zone_id zone, const interposer_comms_zone_config* config)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_configure_zone, server, zone, config);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_update_player_position(interposer_comms_server* server, interposer_comms_player_id player, interposer_comms_zone_id zone, interposer_comms_vec3 position, const interposer_comms_vec3* forward)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_update_player_position, server, player, zone, position, forward);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_clear_player_position(interposer_comms_server* server, interposer_comms_player_id player)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_clear_player_position, server, player);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_set_position_authority(interposer_comms_server* server, interposer_comms_player_id player, interposer_comms_position_authority authority)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_set_position_authority, server, player, authority);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_set_pair_attenuation(interposer_comms_server* server, interposer_comms_player_id listener, interposer_comms_player_id speaker, float gain)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_set_pair_attenuation, server, listener, speaker, gain);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_set_proximity_enabled(interposer_comms_server* server, interposer_comms_player_id player, int can_speak, int can_listen)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_set_proximity_enabled, server, player, can_speak, can_listen);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API void INTERPOSER_CALL interposer_comms_group_config_init(interposer_comms_group_config* config)
{
    FORWARD_TO_PRIMARY(interposer_comms_group_config_init, config);
    if (!config)
        return;
    *config = interposer_comms_group_config{};
    config->struct_size = sizeof(interposer_comms_group_config);
    config->type = INTERPOSER_COMMS_GROUP_SERVER;
    config->parent = INTERPOSER_COMMS_INVALID_ID;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_create_group(interposer_comms_server* server, const interposer_comms_group_config* config, interposer_comms_group_id* out_group)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_create_group, server, config, out_group);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_destroy_group(interposer_comms_server* server, interposer_comms_group_id group)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_destroy_group, server, group);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_add_to_group(interposer_comms_server* server, interposer_comms_group_id group, interposer_comms_player_id player)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_add_to_group, server, group, player);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_remove_from_group(interposer_comms_server* server, interposer_comms_group_id group, interposer_comms_player_id player)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_remove_from_group, server, group, player);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_set_member_permissions(interposer_comms_server* server, interposer_comms_group_id group, interposer_comms_player_id player, int can_speak, int can_listen)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_set_member_permissions, server, group, player, can_speak, can_listen);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_list_groups(interposer_comms_server* server, interposer_comms_group_id* out_ids, uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_list_groups, server, out_ids, capacity, out_count);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_get_group_info(interposer_comms_server* server, interposer_comms_group_id group, interposer_comms_server_group_info* out_info)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_get_group_info, server, group, out_info);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_list_group_members(interposer_comms_server* server, interposer_comms_group_id group, interposer_comms_player_id* out_ids, uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_list_group_members, server, group, out_ids, capacity, out_count);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_update_limits(interposer_comms_server* server, const interposer_comms_server_config* config)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_update_limits, server, config);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_get_config(interposer_comms_server* server, interposer_comms_server_config* out_config)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_get_config, server, out_config);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_execute_command(interposer_comms_server* server, const char* command, char* out_text, size_t text_capacity)
{
    FORWARD_TO_PRIMARY(interposer_comms_server_execute_command, server, command, out_text, text_capacity);
    return INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED;
}

} // extern "C"
