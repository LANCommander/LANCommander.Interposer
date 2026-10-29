#pragma once

// Wire protocol shared by server and client. All integers little-endian.
//
// ENet channels:
//   0 control   reliable, ordered
//   1 voice     unreliable, sequenced
//   2 position  unreliable, sequenced
//
// Every packet starts with a u8 message id.

#include "LANCommander.Interposer.Comms.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace comms::proto {

constexpr uint32_t kProtocolVersion = 2;
constexpr uint32_t kSampleRate = 48000;
constexpr size_t   kMaxOpusPacket = 1276;
constexpr size_t   kMaxStringLength = 255;
constexpr size_t   kMaxTextLength = 32 * 1024;   // long strings (admin output)
constexpr uint32_t kPacketOverheadBytes = 40;    // IPv4 + UDP + ENet headers, for bandwidth accounting

enum Channel : uint8_t {
    kChannelControl = 0,
    kChannelVoice = 1,
    kChannelPosition = 2,
    kChannelCount = 3,
};

enum MessageId : uint8_t {
    // Client -> server
    kHello = 1,          // u32 version, str token, str password, u64 game_player_id, str name, str admin_key, u8 admin_only
    kVoiceUp = 2,        // u16 seq, u8 flags, u8 target_count, {u8 kind, u32 id}*, opus...
    kClientPosition = 3, // u32 zone, vec3 position, u8 has_forward, [vec3 forward]
    kCreateGroup = 4,    // str name, str password
    kJoinGroup = 5,      // u32 group, str password
    kLeaveGroup = 6,     // u32 group
    kInviteToGroup = 7,  // u32 group, u32 player
    kSetListening = 8,   // u32 group, u8 listening
    kAdminCommand = 9,   // u32 request_id, text command   (admin sessions only)

    // Server -> client
    kWelcome = 64,       // u32 player_id, str server_name, u32 opus_bitrate, u16 frame_ms, u16 roster_count, roster*
    kReject = 65,        // i32 result, str reason
    kPlayerAdded = 66,   // roster entry
    kPlayerRemoved = 67, // u32 player_id
    kPlayerUpdated = 68, // roster entry
    kKicked = 69,        // str reason
    kVoiceDown = 70,     // u32 speaker, u16 seq, u8 flags, u8 kind, u32 group, u8 gain, i8 pan, u16 distance_dm, opus...
    kGroupState = 71,    // full snapshot of one group visible to the recipient (see GroupState)
    kGroupRemoved = 72,  // u32 group: destroyed or no longer visible to the recipient
    kGroupInvite = 73,   // u32 group, u32 inviter
    kRequestFailed = 74, // u8 request message id, u32 group, i32 result, str reason
    kConfigUpdate = 75,  // u32 opus_bitrate, u16 frame_ms
    kAdminResponse = 76, // u32 request_id, i32 result, text output
};

// Roster entry: u32 id, u64 game_player_id, str name, u8 server_muted

enum GroupFlags : uint8_t {
    kGroupHasPassword = 1 << 0,
    kGroupCanLeave = 1 << 1,
};

enum MemberFlags : uint8_t {
    kMemberCanSpeak = 1 << 0,
    kMemberCanListen = 1 << 1,
    kMemberListening = 1 << 2, // member hasn't paused listening via set_group_listening
};

constexpr size_t kMaxGroupNameLength = 63;

enum VoiceFlags : uint8_t {
    kVoiceEndOfSpurt = 1 << 0, // last frame of a talk spurt
};

constexpr uint16_t kDistanceUnknown = 0xFFFF;

class Writer {
public:
    explicit Writer(MessageId id) { u8(id); }

    Writer& u8(uint8_t v) { buffer_.push_back(v); return *this; }
    Writer& i8(int8_t v) { return u8(static_cast<uint8_t>(v)); }
    Writer& u16(uint16_t v) { return raw(&v, sizeof(v)); }
    Writer& u32(uint32_t v) { return raw(&v, sizeof(v)); }
    Writer& i32(int32_t v) { return raw(&v, sizeof(v)); }
    Writer& u64(uint64_t v) { return raw(&v, sizeof(v)); }
    Writer& f32(float v) { return raw(&v, sizeof(v)); }
    Writer& vec3(const interposer_comms_vec3& v) { return f32(v.x).f32(v.y).f32(v.z); }

    Writer& str(const std::string& s)
    {
        const size_t length = s.size() > kMaxStringLength ? kMaxStringLength : s.size();
        u8(static_cast<uint8_t>(length));
        return raw(s.data(), length);
    }

    // Long string with a u16 length prefix, truncated to kMaxTextLength.
    Writer& text(const std::string& s)
    {
        const size_t length = s.size() > kMaxTextLength ? kMaxTextLength : s.size();
        u16(static_cast<uint16_t>(length));
        return raw(s.data(), length);
    }

    Writer& raw(const void* data, size_t length)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + length);
        return *this;
    }

    const std::vector<uint8_t>& data() const { return buffer_; }

private:
    // Host is assumed little-endian (x86/x64/ARM64); add byte swapping here for big-endian targets.
    std::vector<uint8_t> buffer_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t length) : data_(data), length_(length) {}

    bool ok() const { return ok_; }
    size_t remaining() const { return ok_ ? length_ - offset_ : 0; }
    const uint8_t* cursor() const { return data_ + offset_; }

    uint8_t u8() { uint8_t v = 0; read(&v, sizeof(v)); return v; }
    int8_t i8() { return static_cast<int8_t>(u8()); }
    uint16_t u16() { uint16_t v = 0; read(&v, sizeof(v)); return v; }
    uint32_t u32() { uint32_t v = 0; read(&v, sizeof(v)); return v; }
    int32_t i32() { int32_t v = 0; read(&v, sizeof(v)); return v; }
    uint64_t u64() { uint64_t v = 0; read(&v, sizeof(v)); return v; }
    float f32() { float v = 0; read(&v, sizeof(v)); return v; }
    interposer_comms_vec3 vec3() { interposer_comms_vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }

    std::string str() { return read_string(u8()); }
    std::string text() { return read_string(u16()); }

private:
    std::string read_string(size_t length)
    {
        if (!ok_ || length > remaining()) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(cursor()), length);
        offset_ += length;
        return s;
    }

    void read(void* out, size_t size)
    {
        if (!ok_ || size > length_ - offset_) {
            ok_ = false;
            return;
        }
        std::memcpy(out, data_ + offset_, size);
        offset_ += size;
    }

    const uint8_t* data_;
    size_t         length_;
    size_t         offset_ = 0;
    bool           ok_ = true;
};

struct RosterEntry {
    uint32_t    id = 0;
    uint64_t    game_player_id = 0;
    std::string name;
    bool        server_muted = false;

    void write(Writer& w) const { w.u32(id).u64(game_player_id).str(name).u8(server_muted ? 1 : 0); }

    static RosterEntry read(Reader& r)
    {
        RosterEntry e;
        e.id = r.u32();
        e.game_player_id = r.u64();
        e.name = r.str();
        e.server_muted = r.u8() != 0;
        return e;
    }
};

// Group snapshot: u32 id, u8 type, str name, u32 owner, u32 parent, u32 max_members, u8 flags,
//                 u16 member_count, {u32 player, u8 member_flags}*   (connected members only)
struct GroupState {
    struct MemberState {
        uint32_t player = 0;
        uint8_t  flags = 0;
    };

    uint32_t    id = 0;
    uint8_t     type = 0;
    std::string name;
    uint32_t    owner = 0;
    uint32_t    parent = 0;
    uint32_t    max_members = 0;
    uint8_t     flags = 0;
    std::vector<MemberState> members;

    void write(Writer& w) const
    {
        w.u32(id).u8(type).str(name).u32(owner).u32(parent).u32(max_members).u8(flags);
        w.u16(static_cast<uint16_t>(members.size()));
        for (const auto& m : members)
            w.u32(m.player).u8(m.flags);
    }

    static GroupState read(Reader& r)
    {
        GroupState g;
        g.id = r.u32();
        g.type = r.u8();
        g.name = r.str();
        g.owner = r.u32();
        g.parent = r.u32();
        g.max_members = r.u32();
        g.flags = r.u8();
        const uint16_t count = r.u16();
        for (uint16_t i = 0; i < count && r.ok(); ++i) {
            MemberState m;
            m.player = r.u32();
            m.flags = r.u8();
            g.members.push_back(m);
        }
        return g;
    }
};

} // namespace comms::proto
