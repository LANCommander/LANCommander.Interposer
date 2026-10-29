/*
 * LANCommander.Interposer.Comms.Client.h -- voice chat, client API.
 *
 * Typical game-client flow:
 *   interposer_comms_client_config cfg; interposer_comms_client_config_init(&cfg);
 *   interposer_comms_client_create(&cfg, &client);
 *   interposer_comms_connect_info info; interposer_comms_connect_info_init(&info);
 *   info.host = ...; info.token = token_from_game_server;
 *   interposer_comms_client_connect(client, &info);
 *   on key down/up:  interposer_comms_client_set_push_to_talk(client, 0, 1/0)
 *   every frame:     drain interposer_comms_client_poll_event(...), optionally interposer_comms_client_get_active_speakers(...)
 */
#ifndef INTERPOSER_COMMS_CLIENT_H
#define INTERPOSER_COMMS_CLIENT_H

#include "LANCommander.Interposer.Comms.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct interposer_comms_client interposer_comms_client;

/* Number of independent push-to-talk bindings (e.g. 0 = proximity, 1 = team, 2 = squad). */
#define INTERPOSER_COMMS_MAX_PTT_SLOTS 8

typedef enum interposer_comms_capture_mode {
    INTERPOSER_COMMS_CAPTURE_DEVICE   = 0, /* library opens a microphone */
    INTERPOSER_COMMS_CAPTURE_EXTERNAL = 1  /* game pushes PCM via interposer_comms_client_push_capture */
} interposer_comms_capture_mode;

typedef enum interposer_comms_playback_mode {
    INTERPOSER_COMMS_PLAYBACK_DEVICE   = 0, /* library mixes, spatializes and plays to an output device */
    INTERPOSER_COMMS_PLAYBACK_EXTERNAL = 1  /* game pulls per-speaker PCM via interposer_comms_client_read_speaker_audio */
} interposer_comms_playback_mode;

/*
 * How the default talk targets (interposer_comms_client_set_talk_targets) are gated:
 *   PUSH_TO_TALK:   only while a PTT slot is held.
 *   VOICE_ACTIVITY: whenever voice is detected (see interposer_comms_voice_activation_settings).
 *   CONTINUOUS:     always (open mic).
 * In every mode, holding a PTT slot also transmits to that slot's targets, bypassing voice
 * detection - so "voice activation for proximity + push-to-talk for team" works out of the box.
 */
typedef enum interposer_comms_transmit_mode {
    INTERPOSER_COMMS_TRANSMIT_PUSH_TO_TALK   = 0,
    INTERPOSER_COMMS_TRANSMIT_VOICE_ACTIVITY = 1,
    INTERPOSER_COMMS_TRANSMIT_CONTINUOUS     = 2
} interposer_comms_transmit_mode;

typedef enum interposer_comms_connection_state {
    INTERPOSER_COMMS_STATE_DISCONNECTED = 0,
    INTERPOSER_COMMS_STATE_CONNECTING,
    INTERPOSER_COMMS_STATE_AUTHENTICATING,
    INTERPOSER_COMMS_STATE_CONNECTED
} interposer_comms_connection_state;

typedef struct interposer_comms_voice_activation_settings {
    uint32_t struct_size;
    float    threshold_db;   /* open the gate above this input level (dBFS), e.g. -45 */
    uint32_t attack_ms;      /* level must stay above threshold this long before opening */
    uint32_t hold_ms;        /* keep transmitting this long after dropping below threshold */
    int      auto_threshold; /* effective threshold = max(threshold_db, noise floor + 12 dB) */
} interposer_comms_voice_activation_settings;

typedef struct interposer_comms_client_config {
    uint32_t                        struct_size;
    interposer_comms_capture_mode              capture_mode;
    interposer_comms_playback_mode             playback_mode;
    interposer_comms_transmit_mode             transmit_mode;
    interposer_comms_voice_activation_settings voice_activation;
    uint32_t                        ptt_release_delay_ms; /* keep sending briefly after PTT release so last words aren't clipped */
    const char*                     capture_device_id;    /* NULL = system default */
    const char*                     playback_device_id;   /* NULL = system default */
    uint32_t                        jitter_target_ms;
} interposer_comms_client_config;

typedef struct interposer_comms_connect_info {
    uint32_t    struct_size;
    const char* host;
    uint16_t    port;           /* 0 = INTERPOSER_COMMS_DEFAULT_PORT */
    const char* token;          /* INTERPOSER_COMMS_AUTH_TOKEN */
    const char* password;       /* INTERPOSER_COMMS_AUTH_PASSWORD, optionally INTERPOSER_COMMS_AUTH_CLAIM */
    uint64_t    game_player_id; /* INTERPOSER_COMMS_AUTH_CLAIM; 0 = claim by player_name */
    const char* player_name;    /* INTERPOSER_COMMS_AUTH_CLAIM fallback; display name for PASSWORD/OPEN */
    const char* admin_key;      /* optional: grants remote admin (interposer_comms_client_send_admin_command) */
    int         admin_only;     /* with admin_key: connect as an admin console only - no player, no voice */
} interposer_comms_connect_info;

typedef enum interposer_comms_device_type {
    INTERPOSER_COMMS_DEVICE_CAPTURE  = 0,
    INTERPOSER_COMMS_DEVICE_PLAYBACK = 1
} interposer_comms_device_type;

typedef struct interposer_comms_audio_device {
    char id[256];
    char name[256];
    int  is_default;
} interposer_comms_audio_device;

typedef struct interposer_comms_client_player_info {
    interposer_comms_player_id id;
    uint64_t        game_player_id;
    char            name[64];
    int             speaking;     /* currently heard by the local player */
    int             locally_muted;
    float           local_volume; /* 0..2, 1 = unchanged */
    int             server_muted;
} interposer_comms_client_player_info;

/* A player the local client is currently hearing. */
typedef struct interposer_comms_speaker_info {
    interposer_comms_player_id   player;
    interposer_comms_target_kind kind;          /* how they reached us: proximity, group or whisper */
    interposer_comms_group_id    group;         /* kind == INTERPOSER_COMMS_TARGET_GROUP */
    float             level_db;      /* smoothed speech level, for animated talk indicators */
    float             distance;      /* kind == INTERPOSER_COMMS_TARGET_PROXIMITY; -1 if unknown */
    float             pan;           /* -1 = left .. +1 = right, as applied by device playback; 0 for non-proximity */
    int               locally_muted; /* still reported so UIs can show "muted but talking" */
} interposer_comms_speaker_info;

/* The local player's own transmit state, for "you are talking" indicators and settings UIs. */
typedef struct interposer_comms_transmit_state {
    int      transmitting;   /* audio is currently being sent */
    int      voice_detected; /* VAD gate open (independent of mode) */
    uint32_t ptt_slots_held; /* bitmask of held PTT slots */
    float    input_level_db; /* current mic level after gain, dBFS */
    float    noise_floor_db; /* estimated, used by auto_threshold */
    int      self_muted;
    int      self_deafened;
} interposer_comms_transmit_state;

typedef struct interposer_comms_client_group_info {
    interposer_comms_group_id   id;
    interposer_comms_group_type type;
    char             name[64];
    interposer_comms_player_id  owner;        /* PRIVATE groups; INTERPOSER_COMMS_INVALID_ID otherwise */
    interposer_comms_group_id   parent;       /* CHANNEL hierarchy; INTERPOSER_COMMS_INVALID_ID = root */
    uint32_t         member_count; /* connected members */
    uint32_t         max_members;  /* 0 = unlimited */
    int              is_member;
    int              listening;    /* local player is a member and hasn't paused it via interposer_comms_client_set_group_listening */
    int              can_speak;    /* local player's permission (set by the server) */
    int              has_password;
    int              can_leave;
} interposer_comms_client_group_info;

typedef struct interposer_comms_discovered_server {
    char            name[64];
    char            host[64];
    uint16_t        port;
    uint32_t        player_count;
    uint32_t        max_players;
    interposer_comms_auth_mode auth_mode;
    uint32_t        ping_ms;
} interposer_comms_discovered_server;

typedef enum interposer_comms_client_event_type {
    INTERPOSER_COMMS_CLIENT_EVENT_NONE = 0,
    INTERPOSER_COMMS_CLIENT_EVENT_STATE_CHANGED,             /* state, reason */
    INTERPOSER_COMMS_CLIENT_EVENT_PLAYER_JOINED,             /* player */
    INTERPOSER_COMMS_CLIENT_EVENT_PLAYER_LEFT,               /* player */
    INTERPOSER_COMMS_CLIENT_EVENT_PLAYER_STARTED_SPEAKING,   /* player, target_kind, group */
    INTERPOSER_COMMS_CLIENT_EVENT_PLAYER_STOPPED_SPEAKING,   /* player */
    INTERPOSER_COMMS_CLIENT_EVENT_LOCAL_TRANSMIT_STARTED,    /* local player started sending audio */
    INTERPOSER_COMMS_CLIENT_EVENT_LOCAL_TRANSMIT_STOPPED,
    INTERPOSER_COMMS_CLIENT_EVENT_GROUP_ADDED,               /* group */
    INTERPOSER_COMMS_CLIENT_EVENT_GROUP_REMOVED,             /* group */
    INTERPOSER_COMMS_CLIENT_EVENT_GROUP_MEMBER_JOINED,       /* group, player */
    INTERPOSER_COMMS_CLIENT_EVENT_GROUP_MEMBER_LEFT,         /* group, player */
    INTERPOSER_COMMS_CLIENT_EVENT_GROUP_INVITE,              /* group, player = inviter */
    INTERPOSER_COMMS_CLIENT_EVENT_REQUEST_FAILED,            /* reason = interposer_comms_result of an async request, text = details */
    INTERPOSER_COMMS_CLIENT_EVENT_KICKED,                    /* text = reason */
    INTERPOSER_COMMS_CLIENT_EVENT_AUDIO_DEVICE_LOST,
    INTERPOSER_COMMS_CLIENT_EVENT_DISCOVERY_RESULT,          /* call interposer_comms_client_get_discovered_servers */
    INTERPOSER_COMMS_CLIENT_EVENT_ADMIN_RESPONSE,            /* request_id, reason = result; full output via interposer_comms_client_get_admin_response */
    INTERPOSER_COMMS_CLIENT_EVENT_CONFIG_CHANGED             /* server changed codec settings (bitrate/frame size) */
} interposer_comms_client_event_type;

typedef struct interposer_comms_client_event {
    interposer_comms_client_event_type type;
    interposer_comms_connection_state  state;
    interposer_comms_player_id         player;
    interposer_comms_group_id          group;
    interposer_comms_target_kind       target_kind;
    int32_t                 reason;
    uint32_t                request_id; /* ADMIN_RESPONSE */
    char                    text[256];  /* may be truncated; see interposer_comms_client_get_admin_response for admin output */
} interposer_comms_client_event;

/* ---- Lifecycle ---------------------------------------------------------- */

INTERPOSER_API void         INTERPOSER_CALL interposer_comms_client_config_init(interposer_comms_client_config* config);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_create(const interposer_comms_client_config* config, interposer_comms_client** out_client);
INTERPOSER_API void         INTERPOSER_CALL interposer_comms_client_destroy(interposer_comms_client* client);

INTERPOSER_API void         INTERPOSER_CALL interposer_comms_connect_info_init(interposer_comms_connect_info* info);
/* Asynchronous; watch for STATE_CHANGED events. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_connect(interposer_comms_client* client, const interposer_comms_connect_info* info);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_disconnect(interposer_comms_client* client);
INTERPOSER_API interposer_comms_connection_state INTERPOSER_CALL interposer_comms_client_get_state(interposer_comms_client* client);
INTERPOSER_API interposer_comms_player_id INTERPOSER_CALL interposer_comms_client_get_local_player(interposer_comms_client* client);

/* Returns INTERPOSER_COMMS_OK and fills *out_event if one was pending, INTERPOSER_COMMS_ERR_NOT_FOUND if the queue is empty. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_poll_event(interposer_comms_client* client, interposer_comms_client_event* out_event);

/* ---- Discovery ---------------------------------------------------------- */

/* Broadcasts a LAN probe; results arrive as DISCOVERY_RESULT events. port 0 = default. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_discover(interposer_comms_client* client, uint16_t discovery_port, uint32_t timeout_ms);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_discovered_servers(interposer_comms_client* client, interposer_comms_discovered_server* out_servers,
                                                                      uint32_t capacity, uint32_t* out_count);

/* ---- Audio devices ------------------------------------------------------ */

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_list_audio_devices(interposer_comms_device_type type, interposer_comms_audio_device* out_devices,
                                                           uint32_t capacity, uint32_t* out_count);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_capture_device(interposer_comms_client* client, const char* device_id);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_playback_device(interposer_comms_client* client, const char* device_id);

/* ---- Transmit: mode, push-to-talk, voice activation --------------------- */

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_transmit_mode(interposer_comms_client* client, interposer_comms_transmit_mode mode);
INTERPOSER_API interposer_comms_transmit_mode INTERPOSER_CALL interposer_comms_client_get_transmit_mode(interposer_comms_client* client);

/* Default targets for VOICE_ACTIVITY/CONTINUOUS and for PTT slots without their own targets. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_talk_targets(interposer_comms_client* client, const interposer_comms_talk_target* targets,
                                                                uint32_t count);
/* Bind a PTT slot to its own targets (count 0 = use default talk targets). */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_ptt_targets(interposer_comms_client* client, uint32_t slot,
                                                               const interposer_comms_talk_target* targets, uint32_t count);
/* Call on key down (pressed = 1) and key up (pressed = 0). Holding several slots sends to the union. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_push_to_talk(interposer_comms_client* client, uint32_t slot, int pressed);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_ptt_release_delay(interposer_comms_client* client, uint32_t delay_ms);

INTERPOSER_API void         INTERPOSER_CALL interposer_comms_voice_activation_settings_init(interposer_comms_voice_activation_settings* settings);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_voice_activation(interposer_comms_client* client,
                                                                    const interposer_comms_voice_activation_settings* settings);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_voice_activation(interposer_comms_client* client,
                                                                    interposer_comms_voice_activation_settings* out_settings);

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_transmit_state(interposer_comms_client* client, interposer_comms_transmit_state* out_state);

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_self_muted(interposer_comms_client* client, int muted);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_self_deafened(interposer_comms_client* client, int deafened);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_input_gain(interposer_comms_client* client, float gain);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_master_volume(interposer_comms_client* client, float volume);

/* ---- Other players & talk indicators ------------------------------------ */

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_player_muted(interposer_comms_client* client, interposer_comms_player_id player, int muted);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_player_volume(interposer_comms_client* client, interposer_comms_player_id player, float volume);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_player_info(interposer_comms_client* client, interposer_comms_player_id player,
                                                               interposer_comms_client_player_info* out_info);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_list_players(interposer_comms_client* client, interposer_comms_player_id* out_ids,
                                                            uint32_t capacity, uint32_t* out_count);
/* Maps a game's own player id to the voice id (e.g. to draw a speaker icon over a player model). */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_find_player(interposer_comms_client* client, uint64_t game_player_id,
                                                           interposer_comms_player_id* out_player);

/* Snapshot of everyone currently being heard. Cheap; safe to call every frame. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_active_speakers(interposer_comms_client* client, interposer_comms_speaker_info* out_speakers,
                                                                   uint32_t capacity, uint32_t* out_count);
INTERPOSER_API int          INTERPOSER_CALL interposer_comms_client_is_player_speaking(interposer_comms_client* client, interposer_comms_player_id player);

/* ---- Groups ------------------------------------------------------------- */

/*
 * Visible groups: every CHANNEL, SERVER groups the local player belongs to (other teams stay hidden),
 * and PRIVATE groups the local player belongs to or has been invited to. Member lists only contain
 * connected players. GROUP_ADDED/REMOVED fire as groups become visible/hidden.
 */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_list_groups(interposer_comms_client* client, interposer_comms_group_id* out_ids,
                                                           uint32_t capacity, uint32_t* out_count);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_group_info(interposer_comms_client* client, interposer_comms_group_id group,
                                                              interposer_comms_client_group_info* out_info);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_list_group_members(interposer_comms_client* client, interposer_comms_group_id group,
                                                                  interposer_comms_player_id* out_ids, uint32_t capacity,
                                                                  uint32_t* out_count);
/*
 * Requests are asynchronous: success shows up as GROUP_* events, failure as REQUEST_FAILED with
 * reason = interposer_comms_result and group = the requested group (INTERPOSER_COMMS_INVALID_ID for create).
 */

/* Name is 1-63 bytes. The creator becomes owner; the new group arrives as GROUP_ADDED with owner = local player. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_create_private_group(interposer_comms_client* client, const char* name, const char* password);
/* CHANNEL: free to join (password if set). PRIVATE: requires an invite or the password. SERVER: never. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_join_group(interposer_comms_client* client, interposer_comms_group_id group, const char* password);
/* SERVER groups only if the server allows it. Ownership of a PRIVATE group passes on; empty PRIVATE groups are destroyed. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_leave_group(interposer_comms_client* client, interposer_comms_group_id group);
/* Members may invite to PRIVATE and CHANNEL groups; an invite bypasses the password. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_invite_to_group(interposer_comms_client* client, interposer_comms_group_id group, interposer_comms_player_id player);
/* Stop hearing a group without leaving it (the server stops forwarding it, saving bandwidth). */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_group_listening(interposer_comms_client* client, interposer_comms_group_id group, int listening);

/* ---- Positions ---------------------------------------------------------- */

/*
 * Report the local player's position to the server. Only used for routing if the server grants
 * this player INTERPOSER_COMMS_POSITION_CLIENT authority (legacy games where the server has no position data);
 * otherwise ignored. Server-reported positions are recommended to prevent abuse.
 */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_report_position(interposer_comms_client* client, interposer_comms_zone_id zone, interposer_comms_vec3 position,
                                                               const interposer_comms_vec3* forward);

/*
 * Local spatialization (never affects routing or who can hear whom).
 *
 * By default stereo pan comes from the server, which only updates as often as the game reports positions.
 * A game client that already knows player positions can instead supply its own listener pose and speaker
 * positions every frame: proximity speakers with an override are then panned locally, so head turns are
 * reflected immediately. Volume attenuation still comes from the server (authoritative falloff/occlusion);
 * interposer_comms_speaker_info.distance reports the local distance.
 *
 * `right` is the listener's right vector in world space (e.g. Unity transform.right, Unreal GetRightVector);
 * passing it instead of an up vector avoids left/right-handed ambiguity. Vectors need not be normalized.
 */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_listener(interposer_comms_client* client, interposer_comms_vec3 position, interposer_comms_vec3 forward,
                                                            interposer_comms_vec3 right);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_clear_listener(interposer_comms_client* client);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_speaker_position_override(interposer_comms_client* client, interposer_comms_player_id player,
                                                                             const interposer_comms_vec3* position /* NULL = clear */);

/* ---- Remote admin ------------------------------------------------------- */

/*
 * Runs a server admin command (same language as interposer_comms_server_execute_command, e.g. "status",
 * "kick 12 spamming"). Requires connecting with a valid admin_key. Asynchronous: the result arrives as
 * an ADMIN_RESPONSE event carrying *out_request_id (may be NULL).
 */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_send_admin_command(interposer_comms_client* client, const char* command,
                                                                  uint32_t* out_request_id);
/* Full output of a recent admin response (the last 16 are kept). Fails with BUFFER_TOO_SMALL but still
 * writes a truncated, NUL-terminated copy if text_capacity is too small; *out_length (may be NULL)
 * receives the full length excluding the terminator. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_admin_response(interposer_comms_client* client, uint32_t request_id,
                                                                  char* out_text, size_t text_capacity,
                                                                  size_t* out_length);
INTERPOSER_API int          INTERPOSER_CALL interposer_comms_client_is_admin(interposer_comms_client* client);

/* ---- External audio ----------------------------------------------------- */

/* INTERPOSER_COMMS_CAPTURE_EXTERNAL: mono float PCM at sample_rate (resampled internally to 48 kHz). */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_push_capture(interposer_comms_client* client, const float* samples,
                                                            uint32_t frame_count, uint32_t sample_rate);
/*
 * INTERPOSER_COMMS_PLAYBACK_EXTERNAL: pulls decoded mono 48 kHz float PCM for one speaker, already scaled by
 * per-player volume/mute (but NOT spatialized - feed it to an engine 3D source). Missing data is
 * filled with silence (PLC). *out_frames receives frames written.
 */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_read_speaker_audio(interposer_comms_client* client, interposer_comms_player_id player,
                                                                  float* out_samples, uint32_t frame_capacity,
                                                                  uint32_t* out_frames);

#ifdef __cplusplus
}
#endif

#endif /* INTERPOSER_COMMS_CLIENT_H */
