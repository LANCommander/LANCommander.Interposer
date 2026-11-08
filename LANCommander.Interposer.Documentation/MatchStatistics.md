---
sidebar_label: Match Statistics
sidebar_position: 14
---

# Match Statistics

LANCommander records multiplayer matches: who played, what happened, and how each match ended.
It keeps leaderboards from those records and links in-game names to LANCommander users. The
Interposer gives game patches and dedicated servers a small C API that reports matches to it.

The game decides what a match is and what happened in it. The Interposer delivers that record:

- It numbers and batches the events.
- It retries while the server cannot be reached.
- It keeps anything not yet delivered on disk until it is.

A dedicated server and a client patch call the same functions. Configuration, not the caller,
decides where the reports go.

## Where reports go

The destination is chosen once per match, when it starts:

| Condition | Who delivers it | Trusted as |
|---|---|---|
| `MatchStatistics.ServerUrl`, `Token` and `GameId` are set | The Interposer, straight to `/api/Matches` | The token's owner. An administrator's token is authoritative. |
| Otherwise, LANCommander's launcher started the game | The launcher, with the signed-in player's login | The player's own client |
| Otherwise | Nobody. The match is logged and passed to the callback only | (none) |

Use the first row for a dedicated server. Create a personal access token with the
**matches:report** scope, under an administrator account if its reports should count as
authoritative, and set:

```yaml
MatchStatistics:
  ServerUrl: 'http://lancommander.lan:1337'
  Token: '%LANCOMMANDER_MATCH_TOKEN%'   # %VARIABLE% is expanded
  GameId: '6f1e2c64-0d47-4b8e-9a43-3a1c2bde0f11'
  ServerId: ''                           # the server's id, if LANCommander manages it
  ReporterKind: Server                   # Server, Host or Client
  ReporterName: ''                       # default: the computer name
  FlushInterval: 2000                    # ms between batches
```

A client patch needs no configuration. When the launcher started the game, the launcher reports
the matches.

### Settings from the host

A host that keeps its settings elsewhere can set them at run time with
`InterposerMatchConfigure` instead. For example, a game server plugin can read them from the
server's own config files. The function takes the same fields as the YAML section. Any field left
`NULL` keeps its current value, so `Config.yml` still supplies whatever the host leaves out.

```c
InterposerMatchConfig config = { sizeof(config) };
config.enabled      = -1;                      /* unchanged */
config.serverUrl    = "http://lancommander.lan:1337";
config.token        = token;                   /* %VARIABLE% is expanded here too */
config.gameId       = "6f1e2c64-0d47-4b8e-9a43-3a1c2bde0f11";
config.reporterName = "CS:S server 1";

if (!InterposerMatchConfigure(&config))
    /* a value was invalid; nothing was changed, and the log says which */;
```

The values are checked the way `Config.yml`'s are. If any field is invalid, the call changes
nothing and returns `FALSE`.

- **Matches started afterwards** use the new settings. A match already open keeps its game,
  server and reporter.
- **Requests sent from then on** use the new URL and token, including requests for matches
  already open. A request that was backing off is retried at once.
- **Unsent matches from a previous run** in `.interposer/Matches/` are picked up by the first call
  that completes the URL, token and game, as they are at startup when `Config.yml` has them.

On Linux the Interposer only speaks plain `http://`. An `https://` server URL is logged once and
nothing is sent.

## The API

All strings are UTF-8. File paths are `wchar_t`, as everywhere else in the API. The declarations
and their full comments are in `LANCommander.Interposer.h`.

```c
#include <LANCommander.Interposer.h>

static DWORD g_match;

void OnRoundStart(const char* roundId, const char* map)
{
    InterposerMatchInfo info = { sizeof(info) };
    info.externalId   = roundId;          /* the game's own id; the same id again carries on with the match */
    info.map          = map;
    info.mode         = "ctf";
    info.settingsJson = "{\"time_limit_ms\":1200000,\"capture_limit\":3}";

    g_match = InterposerMatchStart(&info);
}

void OnPlayerJoin(const char* steamId, const char* name, const char* team, long long gameTimeMs)
{
    char key[96];
    snprintf(key, sizeof(key), "steam:%s", steamId);
    InterposerMatchPlayerJoined(g_match, key, name, team, gameTimeMs);
}

void OnKill(const char* killerKey, const char* victimKey, const char* weapon, long long gameTimeMs)
{
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"weapon\":\"%s\"}", weapon);

    InterposerMatchEvent(g_match, "kill", gameTimeMs, -1, killerKey, victimKey, payload);
    InterposerMatchAddStatistic(g_match, killerKey, NULL, "kills", 1);
    InterposerMatchAddStatistic(g_match, victimKey, NULL, "deaths", 1);
}

void OnRoundEnd(const char* winningTeam)
{
    InterposerMatchResult winner = { sizeof(winner) };
    winner.team   = winningTeam;
    winner.rank   = 1;
    winner.timeMs = -1;
    InterposerMatchSetResult(g_match, &winner);

    InterposerMatchEnd(g_match, "completed", FALSE);
    InterposerMatchAttachFile(g_match, L"demos\\round.dem", INTERPOSER_MATCH_ATTACHMENT_DEMO, -1, -1);
}
```

| Function | What it does |
|---|---|
| `InterposerMatchConfigure` | Sets the `MatchStatistics` settings at run time (see [Settings from the host](#settings-from-the-host)). |
| `InterposerMatchStart` | Starts a match and returns its handle. Starting again under the same external id, in this process or after a restart, carries on with the same match. |
| `InterposerMatchPlayerJoined` / `PlayerLeft` | A player joined (or changed name or team), or left. |
| `InterposerMatchEvent` | Something happened. The payload is a JSON object or `NULL`. |
| `InterposerMatchSetStatistic` / `AddStatistic` | An end-of-match total for a player or team. `Add` keeps a running total, so a patch can count as it goes. |
| `InterposerMatchSetResult` | A place in the final standings. |
| `InterposerMatchEnd` | Ends the match and sends the standings and totals with it. |
| `InterposerMatchAttachFile` | A demo, replay, log or screenshot, uploaded in the background. Allowed after the end. |
| `InterposerMatchFlush` | Waits until the server has everything. Call it before a server shuts down. |
| `InterposerMatchGetStatus` | Where the match goes, what is still pending, what the server rejected, and the last error. |
| `InterposerRegisterMatchCallback` | Sees every step of every match, whatever the destination. |

A call returns `FALSE` only for a caller's mistake:

- an unknown handle
- a missing key or type
- a payload that is not a single JSON object (comments, trailing commas and single quotes are all refused)
- a change after the match ended

A call never returns `FALSE` because LANCommander can't be reached. The game never waits on the
network; delivery happens on a background thread.

### Conventions

These are LANCommander's conventions, and the server's leaderboards rely on them:

- **Player keys** name players everywhere: `steam:STEAM_0:1:123`, `name:Pat`, `bot:3`. The prefix
  is the kind of id.
- **Event types and statistic keys** are lowercase with underscores: `kill`, `flag_captured`,
  `best_lap_ms`.
- **Game time** is milliseconds since the match started, by the game's own clock, so pauses do not
  count.
- **Payloads** refer to other players by key and give durations in whole milliseconds with an
  `_ms` suffix. Leave out values that can be derived, such as rates.

## Delivery

With the HTTP destination, a background thread:

1. registers the match (`POST /api/Matches`). LANCommander answers with the last event it already
   has from this reporter.
2. sends players, then events in batches of up to 500, every `FlushInterval` or as soon as the match
   ends. A `429` is waited out for as long as `Retry-After` says. An unreachable server or a `5xx`
   is retried with backoff up to 30 seconds, and so is a `401` or `403`, in case the token is fixed.
3. sends the end, with results, statistics and players.
4. uploads attachments in 8 MiB chunks.

Each event gets its id and sequence number when it is recorded. A batch that is sent twice is
recognised by the server, not counted twice.

**Nothing is lost to a crash.** Before anything is sent it is written to
`.interposer\Matches\<external id>\`. A run that finds that folder sends what is left. LANCommander
reports the last event it holds, and only the events after it are resent. If the game starts the
match again under the same external id, recording carries on where it stopped. The folder is
deleted once the end and every attachment are in. A match nobody resumes is delivered as far as it
got, and LANCommander's own housekeeping closes it.

A request that LANCommander will never accept, such as a `400` or a `404` because match statistics
are turned off, is logged and dropped. It is not retried forever.

## Logging

`Logging.Matches: true` logs matches started, ended and delivered: `[MATCH START]`,
`[MATCH END]`, `[MATCH SEND]`, `[MATCH SENT]`, `[MATCH ATTACH]`. At `Level: Debug` it also logs
every event, player and batch (`[MATCH EVENT]`, `[MATCH PLAYER]`). `[MATCH ERROR]` lines are
written whatever the setting, once each time the error changes. The token is never logged.

## The callback and the launcher

Every step reaches `InterposerRegisterMatchCallback` and the launcher's event pipe as JSON, shaped
like LANCommander's own request models:

| Kind | JSON |
|---|---|
| `START` | A `MatchCreateRequest`, plus `forward` |
| `PARTICIPANT` | A `MatchParticipantSubmission` |
| `EVENT` | A `MatchEventSubmission`: `id`, `sequence`, `type`, `gameTimeMilliseconds`, `tick`, `actorKey`, `subjectKey`, `payload`, `clientTime` |
| `END` | A `MatchEndRequest` with `results` and `statistics` |
| `ATTACH` | `path`, `kind`, `fileName`, `contentType`, `size`, `startMilliseconds`, `endMilliseconds` |

`forward` is `true` only when the launcher is the one to deliver the match, that is, when the
Interposer has no server of its own. A launcher must ignore a match whose `START` says `false`, or
the match would be reported twice. In .NET these arrive as `InterposerService.MatchReported`, with
typed accessors (`e.Start`, `e.Event`, `e.End`, ...).
