#include "voice/server/server_impl.h"

#include "voice/common/log.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <vector>

namespace comms::server {

namespace {

const char* const kHelp =
    "Commands:\n"
    "  status                          server summary\n"
    "  players                         list players\n"
    "  groups                          list groups\n"
    "  limits                          show limits\n"
    "  kick <player> [reason]          disconnect a player's voice client\n"
    "  mute <player> | unmute <player> server mute\n"
    "  group create <name> [channel]   create a server group (or channel)\n"
    "  group destroy <group>\n"
    "  group add <group> <player>\n"
    "  group remove <group> <player>\n"
    "  limit <name> <value>            bitrate, frame_ms, max_players, max_groups, private_groups,\n"
    "                                  forwarded, total_kbps, up_kbps, down_kbps\n"
    "<player>/<group>: id, #id, or exact name (quote names with spaces).\n";

void appendf(std::string& out, const char* fmt, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    out += buffer;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> tokenize(const std::string& line)
{
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i >= line.size())
            break;
        std::string token;
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"')
                token += line[i++];
            ++i; // closing quote (or end)
        } else {
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
                token += line[i++];
        }
        tokens.push_back(token);
    }
    return tokens;
}

bool parse_uint(const std::string& text, uint32_t& out)
{
    if (text.empty() || text.size() > 10 || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); }))
        return false;
    const unsigned long long value = std::strtoull(text.c_str(), nullptr, 10);
    if (value > 0xFFFFFFFFull)
        return false;
    out = static_cast<uint32_t>(value);
    return true;
}

// id, "#id" or exact case-insensitive name.
template <typename Map, typename NameOf>
uint32_t resolve(const Map& items, const std::string& ref, NameOf name_of, std::string& error, const char* what)
{
    uint32_t id = 0;
    const std::string digits = !ref.empty() && ref[0] == '#' ? ref.substr(1) : ref;
    if (parse_uint(digits, id)) {
        if (items.count(id))
            return id;
        appendf(error, "no %s with id %u\n", what, id);
        return INTERPOSER_COMMS_INVALID_ID;
    }

    const std::string wanted = lower(ref);
    uint32_t found = INTERPOSER_COMMS_INVALID_ID;
    for (const auto& [item_id, item] : items) {
        if (lower(name_of(item)) != wanted)
            continue;
        if (found != INTERPOSER_COMMS_INVALID_ID) {
            appendf(error, "'%s' matches more than one %s; use the id\n", ref.c_str(), what);
            return INTERPOSER_COMMS_INVALID_ID;
        }
        found = item_id;
    }
    if (found == INTERPOSER_COMMS_INVALID_ID)
        appendf(error, "no %s named '%s'\n", what, ref.c_str());
    return found;
}

interposer_comms_player_id resolve_player(interposer_comms_server* server, const std::string& ref, std::string& out)
{
    return resolve(server->players, ref, [](const Player& p) { return p.name; }, out, "player");
}

interposer_comms_group_id resolve_group(interposer_comms_server* server, const std::string& ref, std::string& out)
{
    return resolve(server->groups, ref, [](const Group& g) { return g.name; }, out, "group");
}

const char* group_type_name(interposer_comms_group_type type)
{
    switch (type) {
    case INTERPOSER_COMMS_GROUP_PROXIMITY: return "proximity";
    case INTERPOSER_COMMS_GROUP_SERVER:    return "server";
    case INTERPOSER_COMMS_GROUP_PRIVATE:   return "private";
    case INTERPOSER_COMMS_GROUP_CHANNEL:   return "channel";
    }
    return "?";
}

const char* auth_mode_name(interposer_comms_auth_mode mode)
{
    switch (mode) {
    case INTERPOSER_COMMS_AUTH_TOKEN:    return "token";
    case INTERPOSER_COMMS_AUTH_CLAIM:    return "claim";
    case INTERPOSER_COMMS_AUTH_PASSWORD: return "password";
    case INTERPOSER_COMMS_AUTH_OPEN:     return "open";
    }
    return "?";
}

struct LimitField {
    const char* name;
    uint32_t interposer_comms_server_config::*field;
    const char* unit;
};

const LimitField kLimitFields[] = {
    { "bitrate",        &interposer_comms_server_config::opus_max_bitrate,               "bps" },
    { "frame_ms",       &interposer_comms_server_config::opus_frame_ms,                  "ms" },
    { "max_players",    &interposer_comms_server_config::max_players,                    "" },
    { "max_groups",     &interposer_comms_server_config::max_groups,                     "" },
    { "private_groups", &interposer_comms_server_config::max_private_groups_per_player,  "per player" },
    { "forwarded",      &interposer_comms_server_config::max_forwarded_speakers,         "speakers per listener (0 = unlimited)" },
    { "total_kbps",     &interposer_comms_server_config::max_total_kbps,                 "kbps (0 = unlimited)" },
    { "up_kbps",        &interposer_comms_server_config::max_upstream_kbps_per_client,   "kbps per client (0 = unlimited)" },
    { "down_kbps",      &interposer_comms_server_config::max_downstream_kbps_per_client, "kbps per client (0 = unlimited)" },
};

// ---- Commands ----------------------------------------------------------------

interposer_comms_result cmd_status(interposer_comms_server* server, std::string& out)
{
    uint32_t connected = 0, up = 0, down = 0;
    for (const auto& [id, player] : server->players) {
        connected += player.peer ? 1 : 0;
        up += player.upstream_meter.kbps();
        down += player.downstream_meter.kbps();
    }
    appendf(out, "Server '%s' on port %u (%s auth)\n", server->name.c_str(), server->config.port,
            auth_mode_name(server->config.auth_mode));
    appendf(out, "Players: %u connected, %zu registered (max %u)\n", connected, server->players.size(),
            server->config.max_players);
    appendf(out, "Groups: %zu   Admin consoles: %zu\n", server->groups.size(), server->admin_peers.size());
    appendf(out, "Voice traffic: %u kbps in, %u kbps out\n", up, down);
    return INTERPOSER_COMMS_OK;
}

interposer_comms_result cmd_players(interposer_comms_server* server, std::string& out)
{
    if (server->players.empty()) {
        out += "No players.\n";
        return INTERPOSER_COMMS_OK;
    }
    for (const auto& [id, p] : server->players) {
        appendf(out, "#%-4u %-24s game=%-12llu %s", id, p.name.c_str(), static_cast<unsigned long long>(p.game_player_id),
                p.peer ? "connected" : "offline");
        if (p.peer)
            appendf(out, " ping=%ums up=%ukbps down=%ukbps", p.peer->roundTripTime, p.upstream_meter.kbps(),
                    p.downstream_meter.kbps());
        if (p.speaking) out += " [speaking]";
        if (p.server_muted) out += " [muted]";
        if (p.admin) out += " [admin]";
        if (p.has_position) appendf(out, " zone=%u pos=(%.1f, %.1f, %.1f)", p.zone, p.position.x, p.position.y, p.position.z);
        out += "\n";
    }
    return INTERPOSER_COMMS_OK;
}

interposer_comms_result cmd_groups(interposer_comms_server* server, std::string& out)
{
    if (server->groups.empty()) {
        out += "No groups.\n";
        return INTERPOSER_COMMS_OK;
    }
    for (const auto& [id, g] : server->groups) {
        appendf(out, "#%-4u %-8s %-24s members=%zu", id, group_type_name(g.type), g.name.c_str(), g.members.size());
        if (g.owner) appendf(out, " owner=#%u", g.owner);
        if (g.parent) appendf(out, " parent=#%u", g.parent);
        if (!g.password.empty()) out += " [password]";
        out += "\n";
    }
    return INTERPOSER_COMMS_OK;
}

interposer_comms_result cmd_limits(interposer_comms_server* server, std::string& out)
{
    for (const auto& field : kLimitFields)
        appendf(out, "%-15s %u %s\n", field.name, server->config.*field.field, field.unit);
    appendf(out, "%-15s %u bps (after upstream cap)\n", "client_bitrate", client_bitrate(server));
    return INTERPOSER_COMMS_OK;
}

interposer_comms_result cmd_kick(interposer_comms_server* server, const std::vector<std::string>& args, std::string& out)
{
    if (args.size() < 2) {
        out += "usage: kick <player> [reason]\n";
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    }
    const interposer_comms_player_id id = resolve_player(server, args[1], out);
    if (id == INTERPOSER_COMMS_INVALID_ID)
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    std::string reason;
    for (size_t i = 2; i < args.size(); ++i)
        reason += (i > 2 ? " " : "") + args[i];
    const std::string name = server->players.at(id).name;
    const interposer_comms_result result = kick_player(server, id, reason.empty() ? "kicked by admin" : reason);
    if (result == INTERPOSER_COMMS_ERR_INVALID_STATE)
        appendf(out, "%s is not connected\n", name.c_str());
    else if (result == INTERPOSER_COMMS_OK)
        appendf(out, "Kicked %s\n", name.c_str());
    return result;
}

interposer_comms_result cmd_mute(interposer_comms_server* server, const std::vector<std::string>& args, bool muted, std::string& out)
{
    if (args.size() != 2) {
        appendf(out, "usage: %s <player>\n", muted ? "mute" : "unmute");
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    }
    const interposer_comms_player_id id = resolve_player(server, args[1], out);
    if (id == INTERPOSER_COMMS_INVALID_ID)
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    set_player_muted(server, id, muted);
    appendf(out, "%s %s\n", muted ? "Muted" : "Unmuted", server->players.at(id).name.c_str());
    return INTERPOSER_COMMS_OK;
}

interposer_comms_result cmd_group(interposer_comms_server* server, const std::vector<std::string>& args, std::string& out)
{
    const std::string sub = args.size() > 1 ? lower(args[1]) : "";
    if (sub == "create" && (args.size() == 3 || args.size() == 4)) {
        const std::string kind = args.size() == 4 ? lower(args[3]) : "server";
        if (kind != "server" && kind != "channel") {
            out += "group type must be 'server' or 'channel'\n";
            return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
        }
        if (args[2].empty() || args[2].size() > proto::kMaxGroupNameLength) {
            out += "group name must be 1-63 characters\n";
            return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
        }
        interposer_comms_group_id id = INTERPOSER_COMMS_INVALID_ID;
        const interposer_comms_result result = create_group(server, kind == "channel" ? INTERPOSER_COMMS_GROUP_CHANNEL : INTERPOSER_COMMS_GROUP_SERVER,
                                                 args[2], {}, INTERPOSER_COMMS_INVALID_ID, INTERPOSER_COMMS_INVALID_ID, 0, false, &id);
        if (result == INTERPOSER_COMMS_OK)
            appendf(out, "Created %s group #%u '%s'\n", kind.c_str(), id, args[2].c_str());
        else
            appendf(out, "Could not create group: %s\n", interposer_comms_result_to_string(result));
        return result;
    }
    if (sub == "destroy" && args.size() == 3) {
        const interposer_comms_group_id id = resolve_group(server, args[2], out);
        if (id == INTERPOSER_COMMS_INVALID_ID)
            return INTERPOSER_COMMS_ERR_NOT_FOUND;
        destroy_group(server, id);
        appendf(out, "Destroyed group #%u\n", id);
        return INTERPOSER_COMMS_OK;
    }
    if ((sub == "add" || sub == "remove") && args.size() == 4) {
        const interposer_comms_group_id group = resolve_group(server, args[2], out);
        const interposer_comms_player_id player = group ? resolve_player(server, args[3], out) : INTERPOSER_COMMS_INVALID_ID;
        if (group == INTERPOSER_COMMS_INVALID_ID || player == INTERPOSER_COMMS_INVALID_ID)
            return INTERPOSER_COMMS_ERR_NOT_FOUND;
        const interposer_comms_result result = sub == "add" ? add_member(server, group, player) : remove_member(server, group, player);
        if (result == INTERPOSER_COMMS_OK)
            appendf(out, "%s #%u %s group #%u\n", sub == "add" ? "Added" : "Removed", player,
                    sub == "add" ? "to" : "from", group);
        else
            appendf(out, "Could not %s member: %s\n", sub.c_str(), interposer_comms_result_to_string(result));
        return result;
    }
    out += "usage: group create <name> [channel] | group destroy <group> | group add <group> <player> | "
           "group remove <group> <player>\n";
    return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
}

interposer_comms_result cmd_limit(interposer_comms_server* server, const std::vector<std::string>& args, std::string& out)
{
    uint32_t value = 0;
    if (args.size() != 3 || !parse_uint(args[2], value)) {
        out += "usage: limit <name> <value>   (see 'limits' for names)\n";
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    }
    const std::string name = lower(args[1]);
    for (const auto& field : kLimitFields) {
        if (name != field.name)
            continue;
        interposer_comms_server_config updated = server->config;
        updated.*field.field = value;
        const interposer_comms_result result = apply_limits(server, updated);
        if (result == INTERPOSER_COMMS_OK)
            appendf(out, "%s = %u\n", field.name, value);
        else
            appendf(out, "invalid value %u for %s\n", value, field.name);
        return result;
    }
    appendf(out, "unknown limit '%s' (see 'limits')\n", args[1].c_str());
    return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
}

} // namespace

interposer_comms_result execute_command(interposer_comms_server* server, const std::string& command, std::string& out)
{
    const std::vector<std::string> args = tokenize(command);
    if (args.empty()) {
        out += kHelp;
        return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
    }

    const std::string name = lower(args[0]);
    if (name == "help" || name == "?") { out += kHelp; return INTERPOSER_COMMS_OK; }
    if (name == "status")  return cmd_status(server, out);
    if (name == "players") return cmd_players(server, out);
    if (name == "groups")  return cmd_groups(server, out);
    if (name == "limits")  return cmd_limits(server, out);
    if (name == "kick")    return cmd_kick(server, args, out);
    if (name == "mute")    return cmd_mute(server, args, true, out);
    if (name == "unmute")  return cmd_mute(server, args, false, out);
    if (name == "group")   return cmd_group(server, args, out);
    if (name == "limit")   return cmd_limit(server, args, out);

    appendf(out, "unknown command '%s' (try 'help')\n", args[0].c_str());
    return INTERPOSER_COMMS_ERR_INVALID_ARGUMENT;
}

interposer_comms_result kick_player(interposer_comms_server* server, interposer_comms_player_id id, const std::string& reason)
{
    Player* player = find_player(server, id);
    if (!player)
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    if (!player->peer)
        return INTERPOSER_COMMS_ERR_INVALID_STATE;
    const bool ephemeral = player->ephemeral;
    kick(server, *player, reason);
    if (ephemeral)
        erase_player(server, id);
    return INTERPOSER_COMMS_OK;
}

interposer_comms_result set_player_muted(interposer_comms_server* server, interposer_comms_player_id id, bool muted)
{
    Player* player = find_player(server, id);
    if (!player)
        return INTERPOSER_COMMS_ERR_NOT_FOUND;
    if (player->server_muted == muted)
        return INTERPOSER_COMMS_OK;
    player->server_muted = muted;
    if (player->peer) {
        proto::Writer updated(proto::kPlayerUpdated);
        roster_entry(*player).write(updated);
        broadcast(server, updated);
    }
    return INTERPOSER_COMMS_OK;
}

} // namespace comms::server
