#include "client/client_impl.h"

#include "common/log.h"

#include <algorithm>
#include <cmath>

namespace comms::client {

namespace {

constexpr uint64_t kSpeakingTimeoutMs = 300;
constexpr uint32_t kMaxJitterPackets = 50;
constexpr float    kAutoThresholdMarginDb = 12.0f;
constexpr float    kNoiseFloorRiseDbPerSecond = 2.5f;

float level_db(const float* samples, uint32_t count)
{
    double sum = 0.0;
    for (uint32_t i = 0; i < count; ++i)
        sum += static_cast<double>(samples[i]) * samples[i];
    const double rms = count ? std::sqrt(sum / count) : 0.0;
    return rms > 1e-6 ? static_cast<float>(20.0 * std::log10(rms)) : kSilenceDb;
}

uint32_t frame_samples(const interposer_comms_client* client)
{
    return kSampleRate * client->frame_ms / 1000;
}

bool same_target(const interposer_comms_talk_target& a, const interposer_comms_talk_target& b)
{
    return a.kind == b.kind && (a.kind == INTERPOSER_COMMS_TARGET_PROXIMITY || a.id == b.id);
}

void add_targets(std::vector<interposer_comms_talk_target>& out, const std::vector<interposer_comms_talk_target>& targets)
{
    for (const auto& target : targets) {
        if (out.size() >= INTERPOSER_COMMS_MAX_TALK_TARGETS)
            return;
        if (std::none_of(out.begin(), out.end(), [&](const interposer_comms_talk_target& t) { return same_target(t, target); }))
            out.push_back(target);
    }
}

void update_voice_activation(interposer_comms_client* client, float level)
{
    const interposer_comms_voice_activation_settings& vad = client->config.voice_activation;
    const uint32_t ms = client->frame_ms;

    // Noise floor: drops instantly to quieter levels, rises slowly so speech doesn't drag it up.
    const float rise = kNoiseFloorRiseDbPerSecond * ms / 1000.0f;
    client->noise_floor_db = level < client->noise_floor_db ? level : std::min(level, client->noise_floor_db + rise);

    float threshold = vad.threshold_db;
    if (vad.auto_threshold)
        threshold = std::max(threshold, client->noise_floor_db + kAutoThresholdMarginDb);

    if (level > threshold) {
        client->vad_below_ms = 0;
        client->vad_above_ms += ms;
        if (client->vad_above_ms >= vad.attack_ms)
            client->vad_open = true;
    } else {
        client->vad_above_ms = 0;
        if (client->vad_open) {
            client->vad_below_ms += ms;
            if (client->vad_below_ms > vad.hold_ms)
                client->vad_open = false;
        }
    }
}

std::vector<interposer_comms_talk_target> gather_targets(interposer_comms_client* client)
{
    std::vector<interposer_comms_talk_target> targets;
    const uint64_t now = now_ms();
    for (const auto& slot : client->ptt_slots) {
        if (slot.pressed || now < slot.release_until_ms)
            add_targets(targets, slot.targets.empty() ? client->default_targets : slot.targets);
    }

    const interposer_comms_transmit_mode mode = client->config.transmit_mode;
    if (mode == INTERPOSER_COMMS_TRANSMIT_CONTINUOUS || (mode == INTERPOSER_COMMS_TRANSMIT_VOICE_ACTIVITY && client->vad_open))
        add_targets(targets, client->default_targets);
    return targets;
}

void send_frame(interposer_comms_client* client, const float* pcm, uint32_t samples, const std::vector<interposer_comms_talk_target>& targets,
                uint8_t flags)
{
    unsigned char opus[proto::kMaxOpusPacket];
    const opus_int32 length = opus_encode_float(client->encoder, pcm, static_cast<int>(samples), opus, sizeof(opus));
    if (length <= 0) {
        INTERPOSER_COMMS_LOG_WARN_F("opus_encode_float failed: %s", opus_strerror(length));
        return;
    }

    proto::Writer w(proto::kVoiceUp);
    w.u16(client->send_seq++).u8(flags).u8(static_cast<uint8_t>(targets.size()));
    for (const auto& target : targets)
        w.u8(static_cast<uint8_t>(target.kind)).u32(target.id);
    w.raw(opus, static_cast<size_t>(length));
    send(client->peer, proto::kChannelVoice, w, false);
}

uint32_t unwrap_sequence(RemotePlayer& player, uint16_t seq)
{
    if (!player.seq_initialized) {
        player.seq_initialized = true;
        player.last_seq = 0x100000u + seq; // headroom so early reordering never goes below zero
        return player.last_seq;
    }
    const int16_t delta = static_cast<int16_t>(seq - static_cast<uint16_t>(player.last_seq));
    const uint32_t unwrapped = player.last_seq + delta;
    if (delta > 0)
        player.last_seq = unwrapped;
    return unwrapped;
}

void ensure_receive_pipeline(interposer_comms_client* client, RemotePlayer& player)
{
    if (!player.decoder) {
        int error = OPUS_OK;
        player.decoder = opus_decoder_create(kSampleRate, 1, &error);
        if (error != OPUS_OK) {
            player.decoder = nullptr;
            INTERPOSER_COMMS_LOG_ERROR_F("opus_decoder_create failed: %s", opus_strerror(error));
            return;
        }
    }
    if (!player.audio) {
        player.audio = std::make_shared<SpeakerAudio>();
        std::lock_guard lock(client->mix_mutex);
        client->mix_list.push_back(player.audio);
    }
}

void stop_speaking(interposer_comms_client* client, RemotePlayer& player)
{
    if (!player.speaking)
        return;
    player.speaking = false;
    player.level_db = kSilenceDb;
    push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_PLAYER_STOPPED_SPEAKING, player.id);
}

// Server pan/distance, unless the game supplied both a listener pose and this speaker's position.
void update_spatialization(const interposer_comms_client* client, RemotePlayer& player)
{
    player.pan = player.server_pan;
    player.distance = player.server_distance;
    if (!client->has_listener || !player.has_position_override || player.kind != INTERPOSER_COMMS_TARGET_PROXIMITY)
        return;

    const interposer_comms_vec3& a = player.position_override;
    const interposer_comms_vec3& b = client->listener_position;
    const interposer_comms_vec3 offset = { a.x - b.x, a.y - b.y, a.z - b.z };
    const interposer_comms_vec3& right = client->listener_right;
    const float distance = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    const float right_length = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);

    player.distance = distance;
    player.pan = distance > 1e-3f && right_length > 1e-6f
        ? std::clamp((offset.x * right.x + offset.y * right.y + offset.z * right.z) / (distance * right_length), -1.0f, 1.0f)
        : 0.0f;
}

void update_gains(interposer_comms_client* client, RemotePlayer& player)
{
    update_spatialization(client, player);
    if (!player.audio)
        return;
    const bool silenced = player.locally_muted || player.server_muted || client->self_deafened;
    const float volume = silenced ? 0.0f : player.local_volume;
    const float spatial = volume * player.route_gain * client->master_volume;
    // Linear balance: centre keeps unity gain on both channels, hard pan silences the far side.
    player.audio->gain_left.store(spatial * std::min(1.0f, 1.0f - player.pan), std::memory_order_relaxed);
    player.audio->gain_right.store(spatial * std::min(1.0f, 1.0f + player.pan), std::memory_order_relaxed);
    player.audio->gain_external.store(volume, std::memory_order_relaxed);
}

void decode_into_ring(RemotePlayer& player, const JitterPacket* packet, const JitterPacket* fec_source, uint32_t samples)
{
    float pcm[kMaxFrameSamples];
    int decoded;
    if (packet)
        decoded = opus_decode_float(player.decoder, packet->opus.data(), static_cast<opus_int32>(packet->opus.size()),
                                    pcm, kMaxFrameSamples, 0);
    else if (fec_source)
        decoded = opus_decode_float(player.decoder, fec_source->opus.data(),
                                    static_cast<opus_int32>(fec_source->opus.size()), pcm, static_cast<int>(samples), 1);
    else
        decoded = opus_decode_float(player.decoder, nullptr, 0, pcm, static_cast<int>(samples), 0);

    if (decoded <= 0)
        return;

    const float level = level_db(pcm, static_cast<uint32_t>(decoded));
    player.level_db = player.level_db <= kSilenceDb ? level : player.level_db * 0.7f + level * 0.3f;
    ring_write(&player.audio->ring, pcm, static_cast<uint32_t>(decoded));
}

void pump_speaker(interposer_comms_client* client, RemotePlayer& player, uint64_t now)
{
    if (player.speaking && now - player.last_packet_ms > kSpeakingTimeoutMs)
        stop_speaking(client, player);

    update_gains(client, player);
    if (!player.audio || !player.decoder)
        return;

    const uint32_t samples = frame_samples(client);
    const uint32_t target_packets = std::max<uint32_t>(1, client->config.jitter_target_ms / client->frame_ms);

    while (player.jitter.size() > target_packets + kMaxJitterPackets / 2) // clock drift or nobody reading
        player.jitter.erase(player.jitter.begin());

    if (!player.playing) {
        const bool spurt_complete = !player.jitter.empty()
            && (std::prev(player.jitter.end())->second.flags & proto::kVoiceEndOfSpurt);
        if (player.jitter.size() < target_packets && !spurt_complete)
            return;
        player.playing = true;
        player.next_seq = player.jitter.begin()->first;
    }

    // Keep two frames decoded ahead of the consumer.
    while (player.playing && ma_pcm_rb_available_read(&player.audio->ring) < samples * 2) {
        while (!player.jitter.empty() && player.jitter.begin()->first < player.next_seq)
            player.jitter.erase(player.jitter.begin());

        if (player.jitter.empty()) {
            player.playing = false; // underrun or end of stream; rebuffer on next packet
            break;
        }

        auto it = player.jitter.find(player.next_seq);
        if (it != player.jitter.end()) {
            const bool end = (it->second.flags & proto::kVoiceEndOfSpurt) != 0;
            decode_into_ring(player, &it->second, nullptr, samples);
            player.jitter.erase(it);
            ++player.next_seq;
            if (end) {
                player.playing = false;
                opus_decoder_ctl(player.decoder, OPUS_RESET_STATE);
            }
        } else {
            // Lost packet with later ones waiting: recover from the next packet's FEC data if present.
            auto next = player.jitter.find(player.next_seq + 1);
            decode_into_ring(player, nullptr, next != player.jitter.end() ? &next->second : nullptr, samples);
            ++player.next_seq;
        }
    }
}

} // namespace

void update_encoder_settings(interposer_comms_client* client, uint32_t bitrate)
{
    opus_encoder_ctl(client->encoder, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate)));
    opus_encoder_ctl(client->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(client->encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(client->encoder, OPUS_SET_PACKET_LOSS_PERC(10));
}

void process_capture(interposer_comms_client* client)
{
    if (!client->capture_ring_ready)
        return;
    const uint32_t samples = frame_samples(client);
    client->capture_frame.resize(samples);
    float* pcm = client->capture_frame.data();

    while (ma_pcm_rb_available_read(&client->capture_ring) >= samples) {
        ring_read(&client->capture_ring, pcm, samples);
        if (client->input_gain != 1.0f) {
            for (uint32_t i = 0; i < samples; ++i)
                pcm[i] = std::clamp(pcm[i] * client->input_gain, -1.0f, 1.0f);
        }

        const float level = level_db(pcm, samples);
        client->input_level_db = level;
        update_voice_activation(client, level);

        // Admin-only sessions have no player and never transmit.
        const bool connected = client->state == INTERPOSER_COMMS_STATE_CONNECTED && client->local_player != INTERPOSER_COMMS_INVALID_ID;
        std::vector<interposer_comms_talk_target> targets = gather_targets(client);
        const bool transmit = connected && !client->self_muted && !targets.empty();

        if (transmit) {
            if (!client->transmitting) {
                client->transmitting = true;
                push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_LOCAL_TRANSMIT_STARTED, client->local_player);
            }
            client->active_targets = std::move(targets);
            send_frame(client, pcm, samples, client->active_targets, 0);
        } else if (client->transmitting) {
            // Close the talk spurt so receivers stop immediately instead of waiting for a timeout.
            if (connected)
                send_frame(client, pcm, samples, client->active_targets, proto::kVoiceEndOfSpurt);
            client->transmitting = false;
            push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_LOCAL_TRANSMIT_STOPPED, client->local_player);
        }
    }
}

void handle_voice_down(interposer_comms_client* client, proto::Reader& r)
{
    const interposer_comms_player_id speaker = r.u32();
    const uint16_t seq = r.u16();
    const uint8_t flags = r.u8();
    const auto kind = static_cast<interposer_comms_target_kind>(r.u8());
    const interposer_comms_group_id group = r.u32();
    const uint8_t gain = r.u8();
    const int8_t pan = r.i8();
    const uint16_t distance_dm = r.u16();
    const size_t length = r.remaining();
    if (!r.ok() || length == 0 || length > proto::kMaxOpusPacket)
        return;

    RemotePlayer* player = find_player(client, speaker);
    if (!player || speaker == client->local_player)
        return;

    ensure_receive_pipeline(client, *player);
    if (!player->decoder)
        return;

    player->route_gain = gain / 255.0f;
    player->server_pan = std::clamp(pan / 127.0f, -1.0f, 1.0f);
    player->server_distance = distance_dm == proto::kDistanceUnknown ? -1.0f : distance_dm / 10.0f;
    player->last_packet_ms = now_ms();

    const bool end = (flags & proto::kVoiceEndOfSpurt) != 0;
    if (!player->speaking && !end) {
        player->speaking = true;
        player->kind = kind;
        player->group = group;
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_PLAYER_STARTED_SPEAKING, speaker, group);
        client->events.back().target_kind = kind;
    } else if (player->speaking) {
        player->kind = kind;
        player->group = group;
    }

    const uint32_t unwrapped = unwrap_sequence(*player, seq);
    if (!player->playing || unwrapped >= player->next_seq) {
        JitterPacket& packet = player->jitter[unwrapped];
        packet.opus.assign(r.cursor(), r.cursor() + length);
        packet.flags = flags;
    }

    if (end)
        stop_speaking(client, *player);
}

void pump_speakers(interposer_comms_client* client)
{
    const uint64_t now = now_ms();
    for (auto& [id, player] : client->players) {
        if (id != client->local_player)
            pump_speaker(client, player, now);
    }
}

} // namespace comms::client
