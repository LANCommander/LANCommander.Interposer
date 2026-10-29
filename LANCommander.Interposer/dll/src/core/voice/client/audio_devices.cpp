#include "voice/client/client_impl.h"
#include "instance.h"

#include "voice/common/log.h"

#include <cstdio>
#include <cstring>

namespace comms::client {

bool decode_device_id(const std::string& encoded, ma_device_id* out_id)
{
    if (encoded.size() % 2 != 0 || encoded.size() / 2 > sizeof(ma_device_id))
        return false;

    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::memset(out_id, 0, sizeof(ma_device_id));
    auto* bytes = reinterpret_cast<unsigned char*>(out_id);
    for (size_t i = 0; i < encoded.size(); i += 2) {
        const int high = nibble(encoded[i]);
        const int low = nibble(encoded[i + 1]);
        if (high < 0 || low < 0)
            return false;
        bytes[i / 2] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

} // namespace comms::client

namespace {

void copy_truncated(char* dest, size_t capacity, const char* src)
{
    std::snprintf(dest, capacity, "%s", src ? src : "");
}

// miniaudio device ids are backend-specific unions; hex-encode the raw bytes so they round-trip as a string.
void encode_device_id(const ma_device_id& id, char* out, size_t capacity)
{
    static const char hex[] = "0123456789abcdef";
    const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
    size_t length = sizeof(ma_device_id);

    // Trim trailing zero bytes to keep ids short.
    while (length > 0 && bytes[length - 1] == 0)
        --length;

    size_t pos = 0;
    for (size_t i = 0; i < length && pos + 2 < capacity; ++i) {
        out[pos++] = hex[bytes[i] >> 4];
        out[pos++] = hex[bytes[i] & 0xF];
    }
    out[pos] = '\0';
}

} // namespace

extern "C" INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_list_audio_devices(interposer_comms_device_type type, interposer_comms_audio_device* out_devices,
                                                              uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_list_audio_devices, type, out_devices, capacity, out_count);
    if (!out_count || (capacity > 0 && !out_devices))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;

    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS)
        return INTERPOSER_COMMS_ERR_AUDIO_DEVICE;

    ma_device_info* playback = nullptr;
    ma_device_info* capture = nullptr;
    ma_uint32 playback_count = 0;
    ma_uint32 capture_count = 0;

    if (ma_context_get_devices(&context, &playback, &playback_count, &capture, &capture_count) != MA_SUCCESS) {
        ma_context_uninit(&context);
        return INTERPOSER_COMMS_ERR_AUDIO_DEVICE;
    }

    const ma_device_info* devices = type == INTERPOSER_COMMS_DEVICE_CAPTURE ? capture : playback;
    const ma_uint32 count = type == INTERPOSER_COMMS_DEVICE_CAPTURE ? capture_count : playback_count;

    for (ma_uint32 i = 0; i < count && i < capacity; ++i) {
        encode_device_id(devices[i].id, out_devices[i].id, sizeof(out_devices[i].id));
        copy_truncated(out_devices[i].name, sizeof(out_devices[i].name), devices[i].name);
        out_devices[i].is_default = devices[i].isDefault ? 1 : 0;
    }

    *out_count = count;
    ma_context_uninit(&context);
    return count > capacity ? INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL : INTERPOSER_COMMS_OK;
}
