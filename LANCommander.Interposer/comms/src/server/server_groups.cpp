#include "server/server_impl.h"

#include "common/log.h"

#include <vector>

namespace comms::server {

namespace {

bool visible_to(const Group& group, interposer_comms_player_id player)
{
    return group.type == INTERPOSER_COMMS_GROUP_CHANNEL || group.members.count(player) || group.invited.count(player);
}

bool can_leave(const Group& group)
{
    return group.type != INTERPOSER_COMMS_GROUP_SERVER || group.players_can_leave;
}

proto::GroupState snapshot(interposer_comms_server* server, const Group& group)
{
    proto::GroupState state;
    state.id = group.id;
    state.type = static_cast<uint8_t>(group.type);
    state.name = group.name;
    state.owner = group.owner;
    state.parent = group.parent;
    state.max_members = group.max_members;
    state.flags = (group.password.empty() ? 0 : proto::kGroupHasPassword) | (can_leave(group) ? proto::kGroupCanLeave : 0);
    for (const auto& [id, member] : group.members) {
        const Player* player = find_player(server, id);
        if (!player || !player->peer)
            continue;
        proto::GroupState::MemberState m;
        m.player = id;
        m.flags = (member.can_speak ? proto::kMemberCanSpeak : 0) | (member.can_listen ? proto::kMemberCanListen : 0)
                | (member.listening ? proto::kMemberListening : 0);
        state.members.push_back(m);
    }
    return state;
}

void send_state(Player& player, const proto::Writer& state, interposer_comms_group_id group)
{
    send(player.peer, proto::kChannelControl, state, true);
    player.known_groups.insert(group);
}

void send_removed(Player& player, interposer_comms_group_id group)
{
    proto::Writer w(proto::kGroupRemoved);
    w.u32(group);
    send(player.peer, proto::kChannelControl, w, true);
    player.known_groups.erase(group);
}

void fail(Player& player, uint8_t request, interposer_comms_group_id group, interposer_comms_result result, const std::string& reason)
{
    proto::Writer w(proto::kRequestFailed);
    w.u8(request).u32(group).i32(result).str(reason);
    send(player.peer, proto::kChannelControl, w, true);
}

uint32_t owned_private_groups(interposer_comms_server* server, interposer_comms_player_id player)
{
    uint32_t count = 0;
    for (const auto& [id, group] : server->groups)
        count += group.type == INTERPOSER_COMMS_GROUP_PRIVATE && group.owner == player ? 1 : 0;
    return count;
}

const char* describe(interposer_comms_result result)
{
    switch (result) {
    case INTERPOSER_COMMS_ERR_NOT_FOUND:      return "group or player not found";
    case INTERPOSER_COMMS_ERR_ALREADY_EXISTS: return "already a member";
    case INTERPOSER_COMMS_ERR_LIMIT_REACHED:  return "limit reached";
    default:                       return interposer_comms_result_to_string(result);
    }
}

// ---- Client requests -------------------------------------------------------

void handle_create(interposer_comms_server* server, Player& player, proto::Reader& r)
{
    const std::string name = r.str();
    const std::string password = r.str();
    if (!r.ok() || name.empty() || name.size() > proto::kMaxGroupNameLength)
        return fail(player, proto::kCreateGroup, INTERPOSER_COMMS_INVALID_ID, INTERPOSER_COMMS_ERR_INVALID_ARGUMENT, "invalid group name");

    const uint32_t limit = server->config.max_private_groups_per_player;
    if (limit == 0)
        return fail(player, proto::kCreateGroup, INTERPOSER_COMMS_INVALID_ID, INTERPOSER_COMMS_ERR_PERMISSION_DENIED,
                    "this server does not allow player-created groups");
    if (owned_private_groups(server, player.id) >= limit)
        return fail(player, proto::kCreateGroup, INTERPOSER_COMMS_INVALID_ID, INTERPOSER_COMMS_ERR_LIMIT_REACHED,
                    "you already own the maximum number of groups");

    interposer_comms_group_id id = INTERPOSER_COMMS_INVALID_ID;
    const interposer_comms_result result = create_group(server, INTERPOSER_COMMS_GROUP_PRIVATE, name, password, player.id,
                                             INTERPOSER_COMMS_INVALID_ID, 0, true, &id);
    if (result != INTERPOSER_COMMS_OK)
        fail(player, proto::kCreateGroup, INTERPOSER_COMMS_INVALID_ID, result, describe(result));
}

void handle_join(interposer_comms_server* server, Player& player, proto::Reader& r)
{
    const interposer_comms_group_id id = r.u32();
    const std::string password = r.str();
    if (!r.ok())
        return;

    auto it = server->groups.find(id);
    if (it == server->groups.end())
        return fail(player, proto::kJoinGroup, id, INTERPOSER_COMMS_ERR_NOT_FOUND, "group not found");
    Group& group = it->second;
    if (group.members.count(player.id))
        return fail(player, proto::kJoinGroup, id, INTERPOSER_COMMS_ERR_ALREADY_EXISTS, "already a member");

    const bool invited = group.invited.count(player.id) != 0;
    switch (group.type) {
    case INTERPOSER_COMMS_GROUP_CHANNEL:
        if (!invited && !group.password.empty() && password != group.password)
            return fail(player, proto::kJoinGroup, id, INTERPOSER_COMMS_ERR_AUTH_FAILED, "wrong password");
        break;
    case INTERPOSER_COMMS_GROUP_PRIVATE:
        if (!invited && (group.password.empty() || password != group.password))
            return fail(player, proto::kJoinGroup, id,
                        password.empty() ? INTERPOSER_COMMS_ERR_PERMISSION_DENIED : INTERPOSER_COMMS_ERR_AUTH_FAILED,
                        password.empty() ? "an invite is required" : "wrong password");
        break;
    default:
        return fail(player, proto::kJoinGroup, id, INTERPOSER_COMMS_ERR_PERMISSION_DENIED, "membership is managed by the server");
    }

    const interposer_comms_result result = add_member(server, id, player.id);
    if (result != INTERPOSER_COMMS_OK)
        fail(player, proto::kJoinGroup, id, result, describe(result));
}

void handle_leave(interposer_comms_server* server, Player& player, proto::Reader& r)
{
    const interposer_comms_group_id id = r.u32();
    if (!r.ok())
        return;
    auto it = server->groups.find(id);
    if (it == server->groups.end() || !it->second.members.count(player.id))
        return fail(player, proto::kLeaveGroup, id, INTERPOSER_COMMS_ERR_NOT_FOUND, "not a member");
    if (!can_leave(it->second))
        return fail(player, proto::kLeaveGroup, id, INTERPOSER_COMMS_ERR_PERMISSION_DENIED, "this group can't be left");
    remove_member(server, id, player.id);
}

void handle_invite(interposer_comms_server* server, Player& player, proto::Reader& r)
{
    const interposer_comms_group_id id = r.u32();
    const interposer_comms_player_id target_id = r.u32();
    if (!r.ok())
        return;

    auto it = server->groups.find(id);
    if (it == server->groups.end() || !it->second.members.count(player.id))
        return fail(player, proto::kInviteToGroup, id, INTERPOSER_COMMS_ERR_NOT_FOUND, "not a member");
    Group& group = it->second;
    if (group.type != INTERPOSER_COMMS_GROUP_PRIVATE && group.type != INTERPOSER_COMMS_GROUP_CHANNEL)
        return fail(player, proto::kInviteToGroup, id, INTERPOSER_COMMS_ERR_PERMISSION_DENIED, "membership is managed by the server");

    Player* target = find_player(server, target_id);
    if (!target || !target->peer || target_id == player.id)
        return fail(player, proto::kInviteToGroup, id, INTERPOSER_COMMS_ERR_NOT_FOUND, "player not found");
    if (group.members.count(target_id))
        return fail(player, proto::kInviteToGroup, id, INTERPOSER_COMMS_ERR_ALREADY_EXISTS, "already a member");

    group.invited.insert(target_id);
    sync_group(server, id); // the invitee can now see the group

    proto::Writer w(proto::kGroupInvite);
    w.u32(id).u32(player.id);
    send(target->peer, proto::kChannelControl, w, true);
}

void handle_set_listening(interposer_comms_server* server, Player& player, proto::Reader& r)
{
    const interposer_comms_group_id id = r.u32();
    const bool listening = r.u8() != 0;
    if (!r.ok())
        return;
    auto it = server->groups.find(id);
    if (it == server->groups.end())
        return fail(player, proto::kSetListening, id, INTERPOSER_COMMS_ERR_NOT_FOUND, "not a member");
    auto member = it->second.members.find(player.id);
    if (member == it->second.members.end())
        return fail(player, proto::kSetListening, id, INTERPOSER_COMMS_ERR_NOT_FOUND, "not a member");
    if (member->second.listening == listening)
        return;
    member->second.listening = listening;
    sync_group(server, id);
}

} // namespace

// ---- Mutations -------------------------------------------------------------

interposer_comms_result create_group(interposer_comms_server* server, interposer_comms_group_type type, const std::string& name,
                          const std::string& password, interposer_comms_player_id owner, interposer_comms_group_id parent,
                          uint32_t max_members, bool players_can_leave, interposer_comms_group_id* out_group)
{
    if (server->groups.size() >= server->config.max_groups)
        return INTERPOSER_COMMS_ERR_LIMIT_REACHED;
    if (parent != INTERPOSER_COMMS_INVALID_ID && !server->groups.count(parent))
        return INTERPOSER_COMMS_ERR_NOT_FOUND;

    Group group;
    group.id = server->next_group_id++;
    group.type = type;
    group.name = name.substr(0, proto::kMaxGroupNameLength);
    group.password = password;
    group.owner = owner;
    group.parent = parent;
    group.max_members = max_members;
    group.players_can_leave = players_can_leave;
    if (owner != INTERPOSER_COMMS_INVALID_ID)
        group.members[owner] = Member{};

    const interposer_comms_group_id id = group.id;
    server->groups.emplace(id, std::move(group));
    push_event(server, INTERPOSER_COMMS_SERVER_EVENT_GROUP_CREATED, owner, id);
    if (owner != INTERPOSER_COMMS_INVALID_ID)
        push_event(server, INTERPOSER_COMMS_SERVER_EVENT_GROUP_MEMBER_JOINED, owner, id);
    sync_group(server, id);

    if (out_group)
        *out_group = id;
    return INTERPOSER_COMMS_OK;
}

void destroy_group(interposer_comms_server* server, interposer_comms_group_id id)
{
    auto it = server->groups.find(id);
    if (it == server->groups.end())
        return;

    const interposer_comms_group_id grandparent = it->second.parent;
    std::vector<interposer_comms_group_id> children;
    for (auto& [other_id, other] : server->groups) {
        if (other.parent == id) {
            other.parent = grandparent;
            children.push_back(other_id);
        }
    }

    server->groups.erase(it);
    push_event(server, INTERPOSER_COMMS_SERVER_EVENT_GROUP_DESTROYED, INTERPOSER_COMMS_INVALID_ID, id);
    sync_group(server, id); // sends kGroupRemoved to everyone who knew it
    for (interposer_comms_group_id child : children)
        sync_group(server, child);
}

interposer_comms_result add_member(interposer_comms_server* server, interposer_comms_group_id id, interposer_comms_player_id player)
{
    auto it = server->groups.find(id);
    if (it == server->groups.end() || !find_player(server, player))
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    Group& group = it->second;
    if (group.members.count(player))
        return INTERPOSER_COMMS_ERR_ALREADY_EXISTS;
    if (group.max_members != 0 && group.members.size() >= group.max_members)
        return INTERPOSER_COMMS_ERR_LIMIT_REACHED;

    group.members[player] = Member{};
    group.invited.erase(player);
    push_event(server, INTERPOSER_COMMS_SERVER_EVENT_GROUP_MEMBER_JOINED, player, id);
    sync_group(server, id);
    return INTERPOSER_COMMS_OK;
}

interposer_comms_result remove_member(interposer_comms_server* server, interposer_comms_group_id id, interposer_comms_player_id player)
{
    auto it = server->groups.find(id);
    if (it == server->groups.end() || !it->second.members.erase(player))
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    Group& group = it->second;
    push_event(server, INTERPOSER_COMMS_SERVER_EVENT_GROUP_MEMBER_LEFT, player, id);

    if (group.type == INTERPOSER_COMMS_GROUP_PRIVATE) {
        if (group.members.empty()) {
            destroy_group(server, id);
            return INTERPOSER_COMMS_OK;
        }
        if (group.owner == player) {
            // Prefer a connected member as the new owner.
            group.owner = group.members.begin()->first;
            for (const auto& [member_id, member] : group.members) {
                const Player* candidate = find_player(server, member_id);
                if (candidate && candidate->peer) {
                    group.owner = member_id;
                    break;
                }
            }
        }
    }
    sync_group(server, id);
    return INTERPOSER_COMMS_OK;
}

void sync_group(interposer_comms_server* server, interposer_comms_group_id id)
{
    auto it = server->groups.find(id);
    proto::Writer state(proto::kGroupState);
    if (it != server->groups.end())
        snapshot(server, it->second).write(state);

    for (auto& [player_id, player] : server->players) {
        if (!player.peer)
            continue;
        if (it != server->groups.end() && visible_to(it->second, player_id))
            send_state(player, state, id);
        else if (player.known_groups.count(id))
            send_removed(player, id);
    }
}

void sync_player_connected(interposer_comms_server* server, Player& player)
{
    player.known_groups.clear();
    for (auto& [id, group] : server->groups) {
        if (group.members.count(player.id)) {
            sync_group(server, id); // other members now see this player in the list
        } else if (visible_to(group, player.id)) {
            proto::Writer state(proto::kGroupState);
            snapshot(server, group).write(state);
            send_state(player, state, id);
        }
    }
}

void sync_player_disconnected(interposer_comms_server* server, interposer_comms_player_id player)
{
    if (Player* p = find_player(server, player))
        p->known_groups.clear();
    for (auto& [id, group] : server->groups) {
        if (group.members.count(player))
            sync_group(server, id);
    }
}

void handle_group_request(interposer_comms_server* server, Player& player, uint8_t message, proto::Reader& r)
{
    switch (message) {
    case proto::kCreateGroup:   handle_create(server, player, r); break;
    case proto::kJoinGroup:     handle_join(server, player, r); break;
    case proto::kLeaveGroup:    handle_leave(server, player, r); break;
    case proto::kInviteToGroup: handle_invite(server, player, r); break;
    case proto::kSetListening:  handle_set_listening(server, player, r); break;
    default: break;
    }
}

} // namespace comms::server
