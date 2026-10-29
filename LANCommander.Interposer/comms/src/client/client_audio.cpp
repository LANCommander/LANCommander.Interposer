#include "client/client_impl.h"

#include "common/log.h"

#include <algorithm>

namespace comms::client {

namespace {

constexpr uint32_t kSpeakerRingFrames = kSampleRate;     // 1 s of decoded audio per speaker
constexpr uint32_t kCaptureRingFrames = kSampleRate;     // 1 s of captured audio
constexpr uint32_t kDevicePeriodMs = 10;

void capture_callback(ma_device* device, void*, const void* input, ma_uint32 frame_count)
{
    auto* client = static_cast<interposer_comms_client*>(device->pUserData);
    write_capture(client, static_cast<const float*>(input), frame_count);
}

void playback_callback(ma_device* device, void* output, const void*, ma_uint32 frame_count)
{
    auto* client = static_cast<interposer_comms_client*>(device->pUserData);
    auto* out = static_cast<float*>(output); // interleaved stereo
    std::fill(out, out + frame_count * 2, 0.0f);

    float mono[512];
    std::lock_guard lock(client->mix_mutex);
    for (auto& speaker : client->mix_list) {
        const float left = speaker->gain_left.load(std::memory_order_relaxed);
        const float right = speaker->gain_right.load(std::memory_order_relaxed);
        ma_uint32 offset = 0;
        while (offset < frame_count) {
            const uint32_t want = std::min<uint32_t>(frame_count - offset, 512);
            const uint32_t got = ring_read(&speaker->ring, mono, want);
            for (uint32_t i = 0; i < got; ++i) {
                out[(offset + i) * 2 + 0] += mono[i] * left;
                out[(offset + i) * 2 + 1] += mono[i] * right;
            }
            if (got < want)
                break;
            offset += got;
        }
    }

    for (ma_uint32 i = 0; i < frame_count * 2; ++i)
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

bool ensure_context(interposer_comms_client* client)
{
    if (client->context_ready)
        return true;
    if (ma_context_init(nullptr, 0, nullptr, &client->context) != MA_SUCCESS)
        return false;
    client->context_ready = true;
    return true;
}

} // namespace

SpeakerAudio::SpeakerAudio()
{
    ring_ready = ma_pcm_rb_init(ma_format_f32, 1, kSpeakerRingFrames, nullptr, nullptr, &ring) == MA_SUCCESS;
}

SpeakerAudio::~SpeakerAudio()
{
    if (ring_ready)
        ma_pcm_rb_uninit(&ring);
}

uint32_t ring_write(ma_pcm_rb* ring, const float* samples, uint32_t frames)
{
    uint32_t written = 0;
    while (written < frames) {
        ma_uint32 chunk = frames - written;
        void* buffer = nullptr;
        if (ma_pcm_rb_acquire_write(ring, &chunk, &buffer) != MA_SUCCESS || chunk == 0)
            break;
        std::copy(samples + written, samples + written + chunk, static_cast<float*>(buffer));
        ma_pcm_rb_commit_write(ring, chunk);
        written += chunk;
    }
    return written;
}

uint32_t ring_read(ma_pcm_rb* ring, float* samples, uint32_t frames)
{
    uint32_t read = 0;
    while (read < frames) {
        ma_uint32 chunk = frames - read;
        void* buffer = nullptr;
        if (ma_pcm_rb_acquire_read(ring, &chunk, &buffer) != MA_SUCCESS || chunk == 0)
            break;
        const auto* source = static_cast<const float*>(buffer);
        std::copy(source, source + chunk, samples + read);
        ma_pcm_rb_commit_read(ring, chunk);
        read += chunk;
    }
    return read;
}

void write_capture(interposer_comms_client* client, const float* samples, uint32_t frames)
{
    if (client->capture_ring_ready)
        ring_write(&client->capture_ring, samples, frames); // drops on overflow
}

bool open_capture_device(interposer_comms_client* client)
{
    if (!client->capture_ring_ready) {
        client->capture_ring_ready =
            ma_pcm_rb_init(ma_format_f32, 1, kCaptureRingFrames, nullptr, nullptr, &client->capture_ring) == MA_SUCCESS;
        if (!client->capture_ring_ready)
            return false;
    }
    if (client->config.capture_mode != INTERPOSER_COMMS_CAPTURE_DEVICE)
        return true;
    if (!ensure_context(client))
        return false;

    ma_device_id id{};
    const bool has_id = !client->capture_device_id.empty() && decode_device_id(client->capture_device_id, &id);

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;
    config.capture.pDeviceID = has_id ? &id : nullptr;
    config.sampleRate = kSampleRate;
    config.periodSizeInMilliseconds = kDevicePeriodMs;
    config.dataCallback = capture_callback;
    config.pUserData = client;

    if (ma_device_init(&client->context, &config, &client->capture_device) != MA_SUCCESS)
        return false;
    if (ma_device_start(&client->capture_device) != MA_SUCCESS) {
        ma_device_uninit(&client->capture_device);
        return false;
    }
    client->capture_ready = true;
    return true;
}

bool open_playback_device(interposer_comms_client* client)
{
    if (client->config.playback_mode != INTERPOSER_COMMS_PLAYBACK_DEVICE)
        return true;
    if (!ensure_context(client))
        return false;

    ma_device_id id{};
    const bool has_id = !client->playback_device_id.empty() && decode_device_id(client->playback_device_id, &id);

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.playback.pDeviceID = has_id ? &id : nullptr;
    config.sampleRate = kSampleRate;
    config.periodSizeInMilliseconds = kDevicePeriodMs;
    config.dataCallback = playback_callback;
    config.pUserData = client;

    if (ma_device_init(&client->context, &config, &client->playback_device) != MA_SUCCESS)
        return false;
    if (ma_device_start(&client->playback_device) != MA_SUCCESS) {
        ma_device_uninit(&client->playback_device);
        return false;
    }
    client->playback_ready = true;
    return true;
}

void close_capture_device(interposer_comms_client* client)
{
    if (client->capture_ready) {
        ma_device_uninit(&client->capture_device);
        client->capture_ready = false;
    }
}

void close_playback_device(interposer_comms_client* client)
{
    if (client->playback_ready) {
        ma_device_uninit(&client->playback_device);
        client->playback_ready = false;
    }
}

} // namespace comms::client
