/*
 * LANCommander.Interposer.Comms.Server.h -- voice chat, server API.
 *
 * Typical game-server flow:
 *   interposer_comms_server_config cfg; interposer_comms_server_config_init(&cfg); cfg.port = ...;
 *   interposer_comms_server_create(&cfg, &server); interposer_comms_server_start(server);
 *   on game player join:  interposer_comms_server_add_player(...), interposer_comms_server_create_join_token(...)
 *                         -> send host/port/token to the player via game netcode
 *   every tick:           interposer_comms_server_update_player_position(...), drain interposer_comms_server_poll_event(...)
 *   on game player leave: interposer_comms_server_remove_player(...)
 *
 * The "game server" side may also be an out-of-process bridge for a legacy game (reading the
 * player list/positions via query protocol, RCON, logs, etc.). Bridges typically use
 * INTERPOSER_COMMS_AUTH_CLAIM since they cannot hand tokens to clients through the game's netcode.
 */
#ifndef INTERPOSER_COMMS_SERVER_H
#define INTERPOSER_COMMS_SERVER_H

#include "LANCommander.Interposer.Comms.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct interposer_comms_server interposer_comms_server;

typedef struct interposer_comms_server_config {
    uint32_t                 struct_size;                   /* set by interposer_comms_server_config_init */

    const char*              bind_address;                  /* NULL = all interfaces */
    uint16_t                 port;                          /* default INTERPOSER_COMMS_DEFAULT_PORT */
    const char*              name;                          /* shown in discovery */

    interposer_comms_auth_mode          auth_mode;
    const char*              password;                      /* INTERPOSER_COMMS_AUTH_PASSWORD; optional extra check for INTERPOSER_COMMS_AUTH_CLAIM */
    const char*              admin_key;                     /* NULL disables the remote admin channel */

    int                      discoverable;                  /* answer LAN discovery probes */
    uint16_t                 discovery_port;                /* default INTERPOSER_COMMS_DEFAULT_DISCOVERY_PORT */

    /* Limits */
    uint32_t                 max_players;
    uint32_t                 max_groups;
    uint32_t                 max_private_groups_per_player; /* 0 = players may not create groups */
    uint32_t                 max_forwarded_speakers;        /* per listener, at once */

    /* Bandwidth (kilobits per second; 0 = unlimited) */
    uint32_t                 max_total_kbps;
    uint32_t                 max_upstream_kbps_per_client;
    uint32_t                 max_downstream_kbps_per_client;

    /* Codec policy pushed to clients */
    uint32_t                 opus_max_bitrate;              /* bits per second */
    uint32_t                 opus_frame_ms;                 /* 10, 20, 40 or 60 */

    uint32_t                 join_token_lifetime_ms;

    /* Proximity position source. Per-player override: interposer_comms_server_set_position_authority. */
    interposer_comms_position_authority position_authority;
    float                    max_client_speed;              /* INTERPOSER_COMMS_POSITION_CLIENT: units/sec; faster moves are rejected. 0 = unchecked */
} interposer_comms_server_config;

typedef struct interposer_comms_zone_config {
    uint32_t      struct_size;
    float         min_distance; /* full volume within this range */
    float         max_distance; /* inaudible (not forwarded) beyond this range */
    interposer_comms_rolloff rolloff;
    /* Coordinate system, used for stereo panning only. */
    interposer_comms_vec3    up;           /* world up axis; default (0,1,0). Use (0,0,1) for Z-up games (Unreal, Quake, Source) */
    int           left_handed;  /* 1 for Unity/Unreal, 0 for Quake/Source/Godot/OpenGL (default) */
} interposer_comms_zone_config;

typedef struct interposer_comms_group_config {
    uint32_t         struct_size;
    interposer_comms_group_type type;              /* INTERPOSER_COMMS_GROUP_SERVER or INTERPOSER_COMMS_GROUP_CHANNEL from the server API */
    const char*      name;
    const char*      password;          /* CHANNEL only; NULL = none */
    interposer_comms_group_id   parent;            /* CHANNEL hierarchy; INTERPOSER_COMMS_INVALID_ID = root */
    uint32_t         max_members;       /* 0 = unlimited */
    int              players_can_leave; /* SERVER groups default to 0 */
} interposer_comms_group_config;

typedef struct interposer_comms_server_group_info {
    interposer_comms_group_id   id;
    interposer_comms_group_type type;
    char             name[64];
    interposer_comms_player_id  owner;        /* PRIVATE groups */
    interposer_comms_group_id   parent;
    uint32_t         member_count; /* all members, connected or not */
    uint32_t         max_members;
    int              has_password;
    int              players_can_leave;
} interposer_comms_server_group_info;

typedef struct interposer_comms_server_player_info {
    interposer_comms_player_id          id;
    uint64_t                 game_player_id;
    char                     name[64];
    int                      connected; /* voice client currently connected */
    int                      speaking;
    int                      server_muted;
    interposer_comms_zone_id            zone;
    interposer_comms_vec3               position;
    interposer_comms_position_authority position_authority;
    uint32_t                 upstream_kbps;
    uint32_t                 downstream_kbps;
    uint32_t                 ping_ms;
} interposer_comms_server_player_info;

typedef enum interposer_comms_server_event_type {
    INTERPOSER_COMMS_SERVER_EVENT_NONE = 0,
    INTERPOSER_COMMS_SERVER_EVENT_PLAYER_CONNECTED,      /* voice client authenticated */
    INTERPOSER_COMMS_SERVER_EVENT_PLAYER_DISCONNECTED,
    INTERPOSER_COMMS_SERVER_EVENT_PLAYER_STARTED_SPEAKING,
    INTERPOSER_COMMS_SERVER_EVENT_PLAYER_STOPPED_SPEAKING,
    INTERPOSER_COMMS_SERVER_EVENT_GROUP_CREATED,         /* includes player-created private groups */
    INTERPOSER_COMMS_SERVER_EVENT_GROUP_DESTROYED,
    INTERPOSER_COMMS_SERVER_EVENT_GROUP_MEMBER_JOINED,
    INTERPOSER_COMMS_SERVER_EVENT_GROUP_MEMBER_LEFT,
    INTERPOSER_COMMS_SERVER_EVENT_AUTH_FAILED,
    INTERPOSER_COMMS_SERVER_EVENT_ADMIN_COMMAND,         /* remote admin executed a command; text in event.text */
    INTERPOSER_COMMS_SERVER_EVENT_POSITION_REJECTED      /* client-reported position failed sanity checks */
} interposer_comms_server_event_type;

typedef struct interposer_comms_server_event {
    interposer_comms_server_event_type type;
    interposer_comms_player_id         player;
    interposer_comms_group_id          group;
    int32_t                 reason; /* interposer_comms_result or disconnect reason */
    char                    text[256];
} interposer_comms_server_event;

/* ---- Lifecycle ---------------------------------------------------------- */

INTERPOSER_API void         INTERPOSER_CALL interposer_comms_server_config_init(interposer_comms_server_config* config);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_create(const interposer_comms_server_config* config, interposer_comms_server** out_server);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_start(interposer_comms_server* server);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_stop(interposer_comms_server* server);
INTERPOSER_API void         INTERPOSER_CALL interposer_comms_server_destroy(interposer_comms_server* server);

/* Returns INTERPOSER_COMMS_OK and fills *out_event if one was pending, INTERPOSER_COMMS_ERR_NOT_FOUND if the queue is empty. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_poll_event(interposer_comms_server* server, interposer_comms_server_event* out_event);

/* ---- Players ------------------------------------------------------------ */

/* Registers a game player. game_player_id is opaque to the library (e.g. a SteamID or slot). */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_add_player(interposer_comms_server* server, uint64_t game_player_id,
                                                          const char* name, interposer_comms_player_id* out_player);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_remove_player(interposer_comms_server* server, interposer_comms_player_id player);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_find_player(interposer_comms_server* server, uint64_t game_player_id,
                                                           interposer_comms_player_id* out_player);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_get_player_info(interposer_comms_server* server, interposer_comms_player_id player,
                                                               interposer_comms_server_player_info* out_info);
/* Fills up to capacity ids; *out_count receives the total number of players. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_list_players(interposer_comms_server* server, interposer_comms_player_id* out_ids,
                                                            uint32_t capacity, uint32_t* out_count);

/*
 * INTERPOSER_COMMS_AUTH_CLAIM: a connecting client claims a registered player by game_player_id (preferred) or
 * exact name. The claim succeeds only if that player exists and has no voice client connected.
 */

/* INTERPOSER_COMMS_AUTH_TOKEN: single-use token the game hands to its client. Writes a NUL-terminated string. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_create_join_token(interposer_comms_server* server, interposer_comms_player_id player,
                                                                 char* out_token, size_t token_capacity);

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_kick_player(interposer_comms_server* server, interposer_comms_player_id player, const char* reason);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_set_player_muted(interposer_comms_server* server, interposer_comms_player_id player, int muted);

/* ---- Proximity ---------------------------------------------------------- */

INTERPOSER_API void         INTERPOSER_CALL interposer_comms_zone_config_init(interposer_comms_zone_config* config);
/* Zone 0 always exists with default config. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_configure_zone(interposer_comms_server* server, interposer_comms_zone_id zone,
                                                              const interposer_comms_zone_config* config);

/*
 * Server-authoritative position report; always accepted regardless of authority mode.
 * forward may be NULL. Players without a position never hear or are heard via proximity.
 */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_update_player_position(interposer_comms_server* server, interposer_comms_player_id player,
                                                                      interposer_comms_zone_id zone, interposer_comms_vec3 position,
                                                                      const interposer_comms_vec3* forward);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_clear_player_position(interposer_comms_server* server, interposer_comms_player_id player);
/* Whether this player's client-reported positions (interposer_comms_client_report_position) are accepted. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_set_position_authority(interposer_comms_server* server, interposer_comms_player_id player,
                                                                      interposer_comms_position_authority authority);

/* Extra gain [0..1] applied from speaker to listener (occlusion, walls, game rules). 1 = clear. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_set_pair_attenuation(interposer_comms_server* server, interposer_comms_player_id listener,
                                                                    interposer_comms_player_id speaker, float gain);
/* Allow/deny proximity voice for a player entirely (e.g. dead/spectating). */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_set_proximity_enabled(interposer_comms_server* server, interposer_comms_player_id player,
                                                                     int can_speak, int can_listen);

/* ---- Groups ------------------------------------------------------------- */

INTERPOSER_API void         INTERPOSER_CALL interposer_comms_group_config_init(interposer_comms_group_config* config);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_create_group(interposer_comms_server* server, const interposer_comms_group_config* config,
                                                            interposer_comms_group_id* out_group);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_destroy_group(interposer_comms_server* server, interposer_comms_group_id group);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_add_to_group(interposer_comms_server* server, interposer_comms_group_id group, interposer_comms_player_id player);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_remove_from_group(interposer_comms_server* server, interposer_comms_group_id group, interposer_comms_player_id player);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_set_member_permissions(interposer_comms_server* server, interposer_comms_group_id group,
                                                                      interposer_comms_player_id player, int can_speak, int can_listen);
/* All groups, including player-created PRIVATE groups. */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_list_groups(interposer_comms_server* server, interposer_comms_group_id* out_ids,
                                                           uint32_t capacity, uint32_t* out_count);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_get_group_info(interposer_comms_server* server, interposer_comms_group_id group,
                                                              interposer_comms_server_group_info* out_info);
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_list_group_members(interposer_comms_server* server, interposer_comms_group_id group,
                                                                  interposer_comms_player_id* out_ids, uint32_t capacity,
                                                                  uint32_t* out_count);

/* ---- Limits & management ------------------------------------------------ */

/*
 * Applies the runtime-changeable fields of config: limits, bandwidth, codec policy, join_token_lifetime_ms,
 * position_authority (default for players added afterwards) and max_client_speed. Network, auth and
 * discovery fields are ignored. Codec changes are pushed to connected clients immediately. Fields are
 * validated as a whole; nothing is applied if any is invalid.
 *
 * Bandwidth figures include estimated IP/UDP/ENet overhead. When max_upstream_kbps_per_client is set, the
 * Opus bitrate each client is told to use is lowered to fit within it.
 */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_update_limits(interposer_comms_server* server, const interposer_comms_server_config* config);
/* Current configuration (string fields point into server-owned storage, valid until destroy). */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_get_config(interposer_comms_server* server, interposer_comms_server_config* out_config);

/*
 * Executes a text admin command and writes human-readable output to out_text (may be NULL). Intended to be
 * wired into the game's existing RCON, e.g. forward "voice <args>" here. Remote admins (interposer_comms_client_send_admin_command)
 * use the same language. "help" lists commands:
 *
 *   status | players | groups | limits
 *   kick <player> [reason]    mute <player>    unmute <player>
 *   group create <name> [channel] | group destroy <group> | group add <group> <player> | group remove <group> <player>
 *   limit <name> <value>      (bitrate, frame_ms, max_players, max_groups, private_groups, forwarded,
 *                              total_kbps, up_kbps, down_kbps)
 *
 * <player> is a voice player id, "#id", or an exact (case-insensitive) name; quote names with spaces.
 * Returns the command's result; INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL if out_text was truncated (the command still ran).
 */
INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_server_execute_command(interposer_comms_server* server, const char* command,
                                                               char* out_text, size_t text_capacity);

#ifdef __cplusplus
}
#endif

#endif /* INTERPOSER_COMMS_SERVER_H */
