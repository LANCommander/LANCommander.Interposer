#pragma once

#include <comms/client.h>

#include "common/protocol.h"
#include "common/util.h"

#include <enet/enet.h>
#include <opus.h>

#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace comms::client {

constexpr uint32_t kSampleRate = proto::kSampleRate;
constexpr uint32_t kMaxFrameSamples = kSampleRate * 120 / 1000; // largest Opus frame
constexpr float    kSilenceDb = -120.0f;

// Decoded PCM for one remote speaker. Written by the network thread, read by the audio callback
// (device playback) or the game (external playback). Gains are precomputed by the network thread.
struct SpeakerAudio {
    ma_pcm_rb          ring{};
    bool               ring_ready = false;
    std::atomic<float> gain_left{ 0.0f };
    std::atomic<float> gain_right{ 0.0f };
    std::atomic<float> gain_external{ 0.0f }; // volume/mute only, no spatialization

    SpeakerAudio();
    ~SpeakerAudio();
    SpeakerAudio(const SpeakerAudio&) = delete;
    SpeakerAudio& operator=(const SpeakerAudio&) = delete;
};

struct JitterPacket {
    std::vector<uint8_t> opus;
    uint8_t              flags = 0;
};

struct RemotePlayer {
    interposer_comms_player_id id = INTERPOSER_COMMS_INVALID_ID;
    uint64_t        game_player_id = 0;
    std::string     name;
    bool            server_muted = false;
    bool            locally_muted = false;
    float           local_volume = 1.0f;

    // Receive pipeline (created lazily on first voice packet)
    OpusDecoder*                  decoder = nullptr;
    std::shared_ptr<SpeakerAudio> audio;
    std::map<uint32_t, JitterPacket> jitter; // keyed by unwrapped sequence number
    bool                          seq_initialized = false;
    uint32_t                      last_seq = 0;
    bool                          playing = false;
    uint32_t                      next_seq = 0;

    // Talk indicator state
    bool              speaking = false;
    uint64_t          last_packet_ms = 0;
    interposer_comms_target_kind kind = INTERPOSER_COMMS_TARGET_PROXIMITY;
    interposer_comms_group_id    group = INTERPOSER_COMMS_INVALID_ID;
    float             route_gain = 1.0f;
    float             server_pan = 0.0f;      // as computed by the server
    float             server_distance = -1.0f;
    float             pan = 0.0f;             // effective (server or local override)
    float             distance = -1.0f;
    float             level_db = kSilenceDb;

    bool              has_position_override = false;
    interposer_comms_vec3        position_override{};
};

struct PttSlot {
    std::vector<interposer_comms_talk_target> targets; // empty = default talk targets
    bool                           pressed = false;
    uint64_t                       release_until_ms = 0;
};

} // namespace comms::client

struct interposer_comms_client {
    std::mutex             mutex; // guards everything not marked otherwise, including all ENet calls
    interposer_comms_client_config    config{};
    std::string            capture_device_id;
    std::string            playback_device_id;

    // Network
    ENetHost*              host = nullptr;
    ENetPeer*              peer = nullptr;
    std::thread            thread;
    std::atomic<bool>      running{ false };
    interposer_comms_connection_state state = INTERPOSER_COMMS_STATE_DISCONNECTED;
    std::string            token;
    std::string            password;
    std::string            player_name;
    uint64_t               game_player_id = 0;
    std::string            admin_key;
    bool                   admin_only = false;
    int32_t                disconnect_reason = INTERPOSER_COMMS_OK;
    std::string            disconnect_text;
    bool                   user_disconnect = false;

    // Session (valid while connected)
    interposer_comms_player_id        local_player = INTERPOSER_COMMS_INVALID_ID;
    std::string            server_name;
    uint32_t               frame_ms = 20;
    std::map<interposer_comms_player_id, comms::client::RemotePlayer> players;
    std::map<interposer_comms_group_id, comms::proto::GroupState>     groups; // visible groups, as last sent by the server
    bool                   is_admin = false;
    uint32_t               next_admin_request = 1;
    std::deque<std::pair<uint32_t, std::string>> admin_responses; // most recent last, capped

    // Transmit
    OpusEncoder*                   encoder = nullptr;
    std::vector<interposer_comms_talk_target> default_targets{ { INTERPOSER_COMMS_TARGET_PROXIMITY, 0 } };
    std::array<comms::client::PttSlot, INTERPOSER_COMMS_MAX_PTT_SLOTS> ptt_slots;
    std::vector<interposer_comms_talk_target> active_targets;
    bool                           transmitting = false;
    uint16_t                       send_seq = 0;
    bool                           self_muted = false;
    bool                           self_deafened = false;
    float                          input_gain = 1.0f;
    float                          master_volume = 1.0f;

    // Local listener pose for spatialization overrides
    bool                           has_listener = false;
    interposer_comms_vec3                     listener_position{};
    interposer_comms_vec3                     listener_right{};

    // Voice activation
    bool     vad_open = false;
    uint32_t vad_above_ms = 0;
    uint32_t vad_below_ms = 0;
    float    input_level_db = comms::client::kSilenceDb;
    float    noise_floor_db = -60.0f;

    // Audio I/O
    ma_context             context{};
    bool                   context_ready = false;
    ma_device              capture_device{};
    bool                   capture_ready = false;
    ma_device              playback_device{};
    bool                   playback_ready = false;
    ma_pcm_rb              capture_ring{};       // SPSC: capture callback / push_capture -> network thread
    bool                   capture_ring_ready = false;
    ma_linear_resampler    resampler{};          // external capture at non-48 kHz rates
    uint32_t               resampler_rate = 0;
    std::vector<float>     capture_frame;        // network thread scratch

    std::mutex             mix_mutex; // guards mix_list (taken by the playback callback)
    std::vector<std::shared_ptr<comms::client::SpeakerAudio>> mix_list;

    std::deque<interposer_comms_client_event> events;
};

namespace comms::client {

// All functions below require client->mutex unless noted.

void push_event(interposer_comms_client* client, interposer_comms_client_event_type type, interposer_comms_player_id player = INTERPOSER_COMMS_INVALID_ID,
                interposer_comms_group_id group = INTERPOSER_COMMS_INVALID_ID, int32_t reason = 0, const std::string& text = {});
void set_state(interposer_comms_client* client, interposer_comms_connection_state state, int32_t reason = INTERPOSER_COMMS_OK);
RemotePlayer* find_player(interposer_comms_client* client, interposer_comms_player_id id);
void release_player_audio(interposer_comms_client* client, RemotePlayer& player);
void clear_session(interposer_comms_client* client);

void network_thread(interposer_comms_client* client);
void handle_voice_down(interposer_comms_client* client, proto::Reader& r);
void process_capture(interposer_comms_client* client);
void pump_speakers(interposer_comms_client* client);
void update_encoder_settings(interposer_comms_client* client, uint32_t bitrate);

// Groups (client_groups.cpp). Returns true if the message was a group message.
bool handle_group_message(interposer_comms_client* client, uint8_t message, proto::Reader& r);

// Audio devices (client_audio.cpp)
bool open_capture_device(interposer_comms_client* client);
bool open_playback_device(interposer_comms_client* client);
void close_capture_device(interposer_comms_client* client);
void close_playback_device(interposer_comms_client* client);
void write_capture(interposer_comms_client* client, const float* samples, uint32_t frames); // any thread (single producer)

// audio_devices.cpp
bool decode_device_id(const std::string& encoded, ma_device_id* out_id);

// ring helpers (client_audio.cpp); lock-free, one producer and one consumer
uint32_t ring_write(ma_pcm_rb* ring, const float* samples, uint32_t frames);
uint32_t ring_read(ma_pcm_rb* ring, float* samples, uint32_t frames);

} // namespace comms::client
