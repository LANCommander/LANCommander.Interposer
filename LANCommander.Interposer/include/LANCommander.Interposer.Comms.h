/*
 * LANCommander.Interposer.Comms.h -- voice chat, common definitions.
 *
 * Proximity and group voice over Opus, built into the Interposer and exported
 * beside its Interposer* API: the same DLL (or .so) and the same import
 * library. Plain C ABI. Include LANCommander.Interposer.Comms.Server.h and/or
 * LANCommander.Interposer.Comms.Client.h.
 *
 * As with the rest of the Interposer's API, a second copy loaded into a
 * process that already has one forwards every function to the first, so a
 * handle created through either copy is valid through both.
 *
 * Kept C89-compatible (no // comments, no stdbool, declarations-first friendly) so it can be
 * consumed by mod SDKs of older games built with old toolchains. Available as x86 and x64.
 */
#ifndef INTERPOSER_COMMS_H
#define INTERPOSER_COMMS_H

#include <stddef.h>

#if defined(_MSC_VER) && _MSC_VER < 1600
   /* Pre-VS2010 toolchains (e.g. older game SDKs) lack <stdint.h>. */
   typedef signed   __int32 int32_t;
   typedef unsigned __int8  uint8_t;
   typedef unsigned __int16 uint16_t;
   typedef unsigned __int32 uint32_t;
   typedef unsigned __int64 uint64_t;
#else
#  include <stdint.h>
#endif

#include "LANCommander.Interposer.Export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any ABI-breaking change. Compare with interposer_comms_get_api_version(). */
#define INTERPOSER_COMMS_API_VERSION 1

#define INTERPOSER_COMMS_DEFAULT_PORT           27800
#define INTERPOSER_COMMS_DEFAULT_DISCOVERY_PORT 27801

typedef enum interposer_comms_result {
    INTERPOSER_COMMS_OK                    =  0,
    INTERPOSER_COMMS_ERR_UNKNOWN           = -1,
    INTERPOSER_COMMS_ERR_INVALID_ARGUMENT  = -2,
    INTERPOSER_COMMS_ERR_OUT_OF_MEMORY     = -3,
    INTERPOSER_COMMS_ERR_NOT_IMPLEMENTED   = -4,
    INTERPOSER_COMMS_ERR_NOT_FOUND         = -5,
    INTERPOSER_COMMS_ERR_ALREADY_EXISTS    = -6,
    INTERPOSER_COMMS_ERR_LIMIT_REACHED     = -7,
    INTERPOSER_COMMS_ERR_PERMISSION_DENIED = -8,
    INTERPOSER_COMMS_ERR_INVALID_STATE     = -9,
    INTERPOSER_COMMS_ERR_NETWORK           = -10,
    INTERPOSER_COMMS_ERR_AUDIO_DEVICE      = -11,
    INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL  = -12,
    INTERPOSER_COMMS_ERR_VERSION_MISMATCH  = -13,
    INTERPOSER_COMMS_ERR_AUTH_FAILED       = -14,
    INTERPOSER_COMMS_ERR_TIMEOUT           = -15
} interposer_comms_result;

/* Ids are assigned by the voice server and are unique for its lifetime. 0 is never valid. */
typedef uint32_t interposer_comms_player_id;
typedef uint32_t interposer_comms_group_id;
typedef uint32_t interposer_comms_zone_id;

#define INTERPOSER_COMMS_INVALID_ID 0u

typedef struct interposer_comms_vec3 {
    float x, y, z;
} interposer_comms_vec3;

typedef enum interposer_comms_auth_mode {
    INTERPOSER_COMMS_AUTH_TOKEN    = 0, /* client presents a join token issued by the game server (games with netcode access) */
    INTERPOSER_COMMS_AUTH_CLAIM    = 1, /* client claims a registered game player by id/name, optionally + password (bridged/legacy games) */
    INTERPOSER_COMMS_AUTH_PASSWORD = 2, /* anyone with the server password (standalone voice apps) */
    INTERPOSER_COMMS_AUTH_OPEN     = 3  /* anyone */
} interposer_comms_auth_mode;

typedef enum interposer_comms_position_authority {
    INTERPOSER_COMMS_POSITION_SERVER = 0, /* only interposer_comms_server_update_player_position is used; client reports ignored (recommended) */
    INTERPOSER_COMMS_POSITION_CLIENT = 1  /* client reports accepted, subject to server sanity checks (legacy games with no server data) */
} interposer_comms_position_authority;

typedef enum interposer_comms_group_type {
    INTERPOSER_COMMS_GROUP_PROXIMITY = 0, /* implicit, one per zone */
    INTERPOSER_COMMS_GROUP_SERVER    = 1, /* server-managed membership (teams) */
    INTERPOSER_COMMS_GROUP_PRIVATE   = 2, /* player-created, invite/password */
    INTERPOSER_COMMS_GROUP_CHANNEL   = 3  /* persistent, free join/leave */
} interposer_comms_group_type;

typedef enum interposer_comms_target_kind {
    INTERPOSER_COMMS_TARGET_PROXIMITY = 0, /* id ignored; uses the speaker's current zone */
    INTERPOSER_COMMS_TARGET_GROUP     = 1, /* id = interposer_comms_group_id */
    INTERPOSER_COMMS_TARGET_WHISPER   = 2  /* id = interposer_comms_player_id */
} interposer_comms_target_kind;

typedef struct interposer_comms_talk_target {
    interposer_comms_target_kind kind;
    uint32_t          id;
} interposer_comms_talk_target;

/* Maximum targets in one target set (default targets or a PTT slot). */
#define INTERPOSER_COMMS_MAX_TALK_TARGETS 16

typedef enum interposer_comms_rolloff {
    INTERPOSER_COMMS_ROLLOFF_LINEAR      = 0,
    INTERPOSER_COMMS_ROLLOFF_INVERSE     = 1,
    INTERPOSER_COMMS_ROLLOFF_EXPONENTIAL = 2
} interposer_comms_rolloff;

typedef enum interposer_comms_log_level {
    INTERPOSER_COMMS_LOG_TRACE = 0,
    INTERPOSER_COMMS_LOG_DEBUG = 1,
    INTERPOSER_COMMS_LOG_INFO  = 2,
    INTERPOSER_COMMS_LOG_WARN  = 3,
    INTERPOSER_COMMS_LOG_ERROR = 4,
    INTERPOSER_COMMS_LOG_OFF   = 5
} interposer_comms_log_level;

/* May be invoked from any library thread; must be thread-safe and must not call back into the library. */
typedef void (INTERPOSER_CALL *interposer_comms_log_fn)(interposer_comms_log_level level, const char* message, void* user_data);

INTERPOSER_API uint32_t     INTERPOSER_CALL interposer_comms_get_api_version(void);
INTERPOSER_API const char*  INTERPOSER_CALL interposer_comms_get_version_string(void);
INTERPOSER_API const char*  INTERPOSER_CALL interposer_comms_result_to_string(interposer_comms_result result);

/* Global, process-wide. Pass NULL to disable. */
INTERPOSER_API void         INTERPOSER_CALL interposer_comms_set_log_handler(interposer_comms_log_fn fn, interposer_comms_log_level min_level, void* user_data);

/* Also write the library's messages at min_level and above to the Interposer's
 * session log (.interposer\Logs\<timestamp>.log), as [VOICE], [VOICE WARNING]
 * and [VOICE ERROR] lines. Off (INTERPOSER_COMMS_LOG_OFF) until a patch turns it
 * on; independent of any handler set above. Process-wide. */
INTERPOSER_API void         INTERPOSER_CALL interposer_comms_set_session_logging(interposer_comms_log_level min_level);

#ifdef __cplusplus
}
#endif

#endif /* INTERPOSER_COMMS_H */
