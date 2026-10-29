#include "voice/client/client_impl.h"
#include "instance.h"

#include <cstring>

using namespace comms;
using namespace comms::client;

namespace {

const proto::GroupState::MemberState* find_member(const proto::GroupState& group, interposer_comms_player_id player)
{
    for (const auto& member : group.members) {
        if (member.player == player)
            return &member;
    }
    return nullptr;
}

void handle_state(interposer_comms_client* client, proto::Reader& r)
{
    proto::GroupState state = proto::GroupState::read(r);
    if (!r.ok())
        return;

    auto it = client->groups.find(state.id);
    if (it == client->groups.end()) {
        const interposer_comms_group_id id = state.id;
        client->groups.emplace(id, std::move(state));
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_GROUP_ADDED, INTERPOSER_COMMS_INVALID_ID, id);
        return;
    }

    // Diff membership against the previous snapshot.
    const proto::GroupState& previous = it->second;
    for (const auto& member : previous.members) {
        if (!find_member(state, member.player))
            push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_GROUP_MEMBER_LEFT, member.player, state.id);
    }
    for (const auto& member : state.members) {
        if (!find_member(previous, member.player))
            push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_GROUP_MEMBER_JOINED, member.player, state.id);
    }
    it->second = std::move(state);
}

void handle_removed(interposer_comms_client* client, proto::Reader& r)
{
    const interposer_comms_group_id id = r.u32();
    if (r.ok() && client->groups.erase(id))
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_GROUP_REMOVED, INTERPOSER_COMMS_INVALID_ID, id);
}

void handle_invite(interposer_comms_client* client, proto::Reader& r)
{
    const interposer_comms_group_id group = r.u32();
    const interposer_comms_player_id inviter = r.u32();
    if (r.ok())
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_GROUP_INVITE, inviter, group);
}

void handle_failed(interposer_comms_client* client, proto::Reader& r)
{
    r.u8(); // request message id; the group + reason are what callers need
    const interposer_comms_group_id group = r.u32();
    const int32_t result = r.i32();
    const std::string reason = r.str();
    if (r.ok())
        push_event(client, INTERPOSER_COMMS_CLIENT_EVENT_REQUEST_FAILED, INTERPOSER_COMMS_INVALID_ID, group, result, reason);
}

// Validates and sends a group request; caller holds no lock.
interposer_comms_result send_request(interposer_comms_client* client, const proto::Writer& request)
{
    std::lock_guard lock(client->mutex);
    if (client->state != INTERPOSER_COMMS_STATE_CONNECTED)
        return INTERPOSER_COMMS_ERR_INVALID_STATE;
    send(client->peer, proto::kChannelControl, request, true);
    return INTERPOSER_COMMS_OK;
}

bool valid_password(const char* password)
{
    return !password || std::strlen(password) <= proto::kMaxStringLength;
}

} // namespace

namespace comms::client {

bool handle_group_message(interposer_comms_client* client, uint8_t message, proto::Reader& r)
{
    switch (message) {
    case proto::kGroupState:    handle_state(client, r); return true;
    case proto::kGroupRemoved:  handle_removed(client, r); return true;
    case proto::kGroupInvite:   handle_invite(client, r); return true;
    case proto::kRequestFailed: handle_failed(client, r); return true;
    default:                    return false;
    }
}

} // namespace comms::client

extern "C" {

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_list_groups(interposer_comms_client* client, interposer_comms_group_id* out_ids,
                                                           uint32_t capacity, uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_list_groups, client, out_ids, capacity, out_count);
    if (!client || !out_count || (capacity > 0 && !out_ids))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    uint32_t count = 0;
    for (const auto& [id, group] : client->groups) {
        if (count < capacity)
            out_ids[count] = id;
        ++count;
    }
    *out_count = count;
    return count > capacity ? INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL : INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_get_group_info(interposer_comms_client* client, interposer_comms_group_id group,
                                                              interposer_comms_client_group_info* out_info)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_get_group_info, client, group, out_info);
    if (!client || !out_info)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    auto it = client->groups.find(group);
    if (it == client->groups.end())
        return INTERPOSER_COMMS_ERR_NOT_FOUND;

    const proto::GroupState& g = it->second;
    const proto::GroupState::MemberState* self = find_member(g, client->local_player);
    *out_info = interposer_comms_client_group_info{};
    out_info->id = g.id;
    out_info->type = static_cast<interposer_comms_group_type>(g.type);
    copy_to(out_info->name, g.name);
    out_info->owner = g.owner;
    out_info->parent = g.parent;
    out_info->member_count = static_cast<uint32_t>(g.members.size());
    out_info->max_members = g.max_members;
    out_info->is_member = self ? 1 : 0;
    out_info->listening = self && (self->flags & proto::kMemberListening) && (self->flags & proto::kMemberCanListen) ? 1 : 0;
    out_info->can_speak = self && (self->flags & proto::kMemberCanSpeak) ? 1 : 0;
    out_info->has_password = (g.flags & proto::kGroupHasPassword) ? 1 : 0;
    out_info->can_leave = (g.flags & proto::kGroupCanLeave) ? 1 : 0;
    return INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_list_group_members(interposer_comms_client* client, interposer_comms_group_id group,
                                                                  interposer_comms_player_id* out_ids, uint32_t capacity,
                                                                  uint32_t* out_count)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_list_group_members, client, group, out_ids, capacity, out_count);
    if (!client || !out_count || (capacity > 0 && !out_ids))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(client->mutex);
    auto it = client->groups.find(group);
    if (it == client->groups.end())
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    uint32_t count = 0;
    for (const auto& member : it->second.members) {
        if (count < capacity)
            out_ids[count] = member.player;
        ++count;
    }
    *out_count = count;
    return count > capacity ? INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL : INTERPOSER_COMMS_OK;
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_create_private_group(interposer_comms_client* client, const char* name, const char* password)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_create_private_group, client, name, password);
    if (!client || !name || !*name || std::strlen(name) > proto::kMaxGroupNameLength || !valid_password(password))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    proto::Writer w(proto::kCreateGroup);
    w.str(name).str(to_string(password));
    return send_request(client, w);
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_join_group(interposer_comms_client* client, interposer_comms_group_id group, const char* password)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_join_group, client, group, password);
    if (!client || group == INTERPOSER_COMMS_INVALID_ID || !valid_password(password))
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    proto::Writer w(proto::kJoinGroup);
    w.u32(group).str(to_string(password));
    return send_request(client, w);
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_leave_group(interposer_comms_client* client, interposer_comms_group_id group)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_leave_group, client, group);
    if (!client || group == INTERPOSER_COMMS_INVALID_ID)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    proto::Writer w(proto::kLeaveGroup);
    w.u32(group);
    return send_request(client, w);
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_invite_to_group(interposer_comms_client* client, interposer_comms_group_id group, interposer_comms_player_id player)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_invite_to_group, client, group, player);
    if (!client || group == INTERPOSER_COMMS_INVALID_ID || player == INTERPOSER_COMMS_INVALID_ID)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    proto::Writer w(proto::kInviteToGroup);
    w.u32(group).u32(player);
    return send_request(client, w);
}

INTERPOSER_API interposer_comms_result INTERPOSER_CALL interposer_comms_client_set_group_listening(interposer_comms_client* client, interposer_comms_group_id group, int listening)
{
    FORWARD_TO_PRIMARY(interposer_comms_client_set_group_listening, client, group, listening);
    if (!client || group == INTERPOSER_COMMS_INVALID_ID)
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    proto::Writer w(proto::kSetListening);
    w.u32(group).u8(listening ? 1 : 0);
    return send_request(client, w);
}

} // extern "C"
