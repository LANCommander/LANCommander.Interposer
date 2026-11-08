using System;
using System.Runtime.InteropServices;
using System.Text;
using LANCommander.Interposer.Events;
using LANCommander.Interposer.Matches;

namespace LANCommander.Interposer.Matches
{
    /// <summary>How a player or team finished a match (InterposerMatchResult.status).</summary>
    public enum MatchResultStatus
    {
        /// <summary>Finished the match.</summary>
        Finished = 0,
        /// <summary>Did not finish.</summary>
        DidNotFinish = 1,
        /// <summary>Disqualified.</summary>
        Disqualified = 2,
        /// <summary>Defeated.</summary>
        Defeated = 3,
        /// <summary>Forfeited.</summary>
        Forfeited = 4,
    }

    /// <summary>What a file attached to a match holds.</summary>
    public enum MatchAttachmentKind
    {
        /// <summary>A game demo.</summary>
        Demo = 0,
        /// <summary>A replay.</summary>
        Replay = 1,
        /// <summary>A log file.</summary>
        Log = 2,
        /// <summary>A screenshot.</summary>
        Screenshot = 3,
        /// <summary>Anything else.</summary>
        Other = 4,
    }

    /// <summary>Where a match's reports go, decided when it starts.</summary>
    public enum MatchSink
    {
        /// <summary>Nowhere but the log and <see cref="InterposerService.MatchReported"/>.</summary>
        None = 0,

        /// <summary>Sent to the LANCommander server by the DLL (MatchStatistics.ServerUrl and Token).</summary>
        Http = 1,

        /// <summary>Left to the launcher, which receives every step through <see cref="InterposerService.MatchReported"/>.</summary>
        Launcher = 2,
    }

    /// <summary>A match to start. Everything but <see cref="ExternalId"/> is optional.</summary>
    public sealed class MatchInfo
    {
        /// <summary>The game's own id for the match; starting one again under the same id carries on with it. Null generates one.</summary>
        public string? ExternalId { get; init; }
        /// <summary>The map played.</summary>
        public string? Map { get; init; }
        /// <summary>The game mode played.</summary>
        public string? Mode { get; init; }
        /// <summary>The game's version.</summary>
        public string? GameVersion { get; init; }

        /// <summary>For a server LANCommander does not manage.</summary>
        public string? ServerName { get; init; }

        /// <summary>For a server LANCommander does not manage: the address players connect to.</summary>
        public string? ServerAddress { get; init; }

        /// <summary>The rules the match is played with, as a JSON object.</summary>
        public string? SettingsJson { get; init; }

        /// <summary>Anything else to keep with the match, as a JSON object.</summary>
        public string? MetadataJson { get; init; }

        /// <summary>When the match started; null is now.</summary>
        public DateTimeOffset? StartedOn { get; init; }
    }

    /// <summary>How delivery of a match is going.</summary>
    public sealed class MatchStatus
    {
        /// <summary>Where the match's reports go.</summary>
        public MatchSink Sink { get; init; }

        /// <summary>LANCommander's id for the match, once the server has registered it.</summary>
        public Guid? MatchId { get; init; }

        /// <summary>Events recorded and not yet accepted by the server.</summary>
        public int PendingEvents { get; init; }
        /// <summary>Events the server refused; never retried.</summary>
        public int RejectedEvents { get; init; }
        /// <summary>Files attached and not yet uploaded.</summary>
        public int PendingAttachments { get; init; }
        /// <summary>The match has ended.</summary>
        public bool Ended { get; init; }

        /// <summary>Everything, the end and attachments included, has been accepted.</summary>
        public bool Delivered { get; init; }

        /// <summary>The last delivery problem, or null.</summary>
        public string? LastError { get; init; }
    }
}

namespace LANCommander.Interposer
{
    public partial class InterposerService
    {
        // =================================================================
        // P/Invoke - Match statistics
        // =================================================================

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeMatchInfo
        {
            public uint structSize;
            public IntPtr externalId, map, mode, gameVersion, serverName, serverAddress, settingsJson, metadataJson;
            public long startedOn;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeMatchResult
        {
            public uint structSize;
            public IntPtr playerKey, team;
            public int rank, status, hasScore;
            public double score;
            public long timeMs;
            public IntPtr payloadJson;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct NativeMatchStatus
        {
            public uint size;
            public int sink;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 40)] public byte[] matchId;
            public uint pendingEvents, rejectedEvents, pendingAttachments;
            public int ended, delivered;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string lastError;
        }

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern uint InterposerMatchStart(ref NativeMatchInfo info);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerMatchPlayerJoined(uint match,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string playerKey,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? displayName,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? team, long gameTimeMs);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerMatchPlayerLeft(uint match,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string playerKey, long gameTimeMs);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerMatchEvent(uint match,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string type, long gameTimeMs, long tick,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? actorKey,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? subjectKey,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? payloadJson);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerMatchSetStatistic(uint match,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? playerKey,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? team,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string key, double value);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerMatchAddStatistic(uint match,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? playerKey,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? team,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string key, double delta);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerMatchSetResult(uint match, ref NativeMatchResult result);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerMatchEnd(uint match,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? reason, [MarshalAs(UnmanagedType.Bool)] bool abandoned);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerMatchAttachFile(uint match, string path, int kind, long startMs, long endMs);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerMatchFlush(uint match, uint timeoutMs);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerMatchGetStatus(uint match, ref NativeMatchStatus status);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerRegisterMatchCallback(IntPtr cb);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private delegate void NativeMatchCallback(IntPtr kind, uint match, IntPtr json);

        private NativeMatchCallback? _matchDelegate;

        /// <summary>
        /// Fires for every step of every match the game reports through the InterposerMatch* API --
        /// start, players, events, end, attachments -- whoever delivers it. A launcher reports the
        /// match to LANCommander itself only when <see cref="MatchStart.Forward"/> is true.
        /// Works cross-process via named pipe and in-process via <see cref="EnableEvents"/>.
        /// </summary>
        public event EventHandler<MatchReportedEventArgs>? MatchReported;

        // kind is START, PARTICIPANT, EVENT, END or ATTACH; see InterposerMatchCallback.
        private void DispatchMatchEvent(string? kind, uint match, string? json)
        {
            if (MatchReportedEventArgs.ParseKind(kind) is { } parsed && !string.IsNullOrEmpty(json))
                MatchReported?.Invoke(this, new MatchReportedEventArgs(parsed, match, json));
        }

        private void OnMatchCallback(IntPtr kind, uint match, IntPtr json)
        {
            DispatchMatchEvent(
                Marshal.PtrToStringUni(kind),
                match,
                json != IntPtr.Zero ? Marshal.PtrToStringUni(json) : null);
        }

        private void RegisterMatchCallback()
        {
            // Absent from DLLs that predate match statistics.
            try
            {
                _matchDelegate = OnMatchCallback;
                InterposerRegisterMatchCallback(Marshal.GetFunctionPointerForDelegate(_matchDelegate));
            }
            catch (EntryPointNotFoundException)
            {
                _matchDelegate = null;
            }
        }

        private void UnregisterMatchCallback()
        {
            if (_matchDelegate != null)
                InterposerRegisterMatchCallback(IntPtr.Zero);

            _matchDelegate = null;
        }

        // =================================================================
        // Match statistics (in-process only)
        // =================================================================

        /// <summary>Starts, or carries on with, a match. Returns its handle, 0 on bad arguments.</summary>
        public uint StartMatch(MatchInfo info)
        {
            var strings = new[]
            {
                info.ExternalId, info.Map, info.Mode, info.GameVersion,
                info.ServerName, info.ServerAddress, info.SettingsJson, info.MetadataJson,
            };

            var pointers = new IntPtr[strings.Length];

            try
            {
                for (var i = 0; i < strings.Length; i++)
                    pointers[i] = strings[i] == null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(strings[i]);

                var native = new NativeMatchInfo
                {
                    structSize    = (uint)Marshal.SizeOf<NativeMatchInfo>(),
                    externalId    = pointers[0],
                    map           = pointers[1],
                    mode          = pointers[2],
                    gameVersion   = pointers[3],
                    serverName    = pointers[4],
                    serverAddress = pointers[5],
                    settingsJson  = pointers[6],
                    metadataJson  = pointers[7],
                    startedOn     = info.StartedOn?.ToUnixTimeMilliseconds() ?? 0,
                };

                return InterposerMatchStart(ref native);
            }
            finally
            {
                foreach (var pointer in pointers)
                    if (pointer != IntPtr.Zero)
                        Marshal.FreeCoTaskMem(pointer);
            }
        }

        /// <summary>A player joined, or changed name or team. A negative game time is unknown.</summary>
        public bool MatchPlayerJoined(uint match, string playerKey, string? displayName = null, string? team = null, long gameTimeMilliseconds = -1)
            => InterposerMatchPlayerJoined(match, playerKey, displayName, team, gameTimeMilliseconds);

        /// <summary>A player left. A negative game time is unknown.</summary>
        public bool MatchPlayerLeft(uint match, string playerKey, long gameTimeMilliseconds = -1)
            => InterposerMatchPlayerLeft(match, playerKey, gameTimeMilliseconds);

        /// <summary>Records something that happened. <paramref name="payloadJson"/> must be a JSON object, or null.</summary>
        public bool RecordMatchEvent(uint match, string type, long gameTimeMilliseconds, string? actorKey = null,
            string? subjectKey = null, string? payloadJson = null, long tick = -1)
            => InterposerMatchEvent(match, type, gameTimeMilliseconds, tick, actorKey, subjectKey, payloadJson);

        /// <summary>Sets an end-of-match total for a player or a team.</summary>
        public bool SetMatchStatistic(uint match, string? playerKey, string? team, string key, double value)
            => InterposerMatchSetStatistic(match, playerKey, team, key, value);

        /// <summary>Adds to an end-of-match total for a player or a team.</summary>
        public bool AddMatchStatistic(uint match, string? playerKey, string? team, string key, double delta)
            => InterposerMatchAddStatistic(match, playerKey, team, key, delta);

        /// <summary>Sets a player's or team's place in the final standings.</summary>
        public bool SetMatchResult(uint match, string? playerKey, string? team, int rank,
            MatchResultStatus status = MatchResultStatus.Finished, double? score = null,
            long? timeMilliseconds = null, string? payloadJson = null)
        {
            var player  = playerKey == null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(playerKey);
            var group   = team == null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(team);
            var payload = payloadJson == null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(payloadJson);

            try
            {
                var native = new NativeMatchResult
                {
                    structSize  = (uint)Marshal.SizeOf<NativeMatchResult>(),
                    playerKey   = player,
                    team        = group,
                    rank        = rank,
                    status      = (int)status,
                    hasScore    = score.HasValue ? 1 : 0,
                    score       = score ?? 0,
                    timeMs      = timeMilliseconds ?? -1,
                    payloadJson = payload,
                };

                return InterposerMatchSetResult(match, ref native);
            }
            finally
            {
                Marshal.FreeCoTaskMem(player);
                Marshal.FreeCoTaskMem(group);
                Marshal.FreeCoTaskMem(payload);
            }
        }

        /// <summary>Ends a match. <paramref name="reason"/> is e.g. <c>completed</c>, <c>time_limit</c> or <c>aborted</c>.</summary>
        public bool EndMatch(uint match, string? reason = null, bool abandoned = false)
            => InterposerMatchEnd(match, reason, abandoned);

        /// <summary>Attaches a file to a match, uploaded in the background.</summary>
        public bool AttachMatchFile(uint match, string path, MatchAttachmentKind kind, long startMilliseconds = -1, long endMilliseconds = -1)
            => InterposerMatchAttachFile(match, path, (int)kind, startMilliseconds, endMilliseconds);

        /// <summary>Waits up to <paramref name="timeout"/> for the server to take everything recorded so far.</summary>
        public bool FlushMatch(uint match, TimeSpan timeout)
            => InterposerMatchFlush(match, (uint)Math.Clamp(timeout.TotalMilliseconds, 0, uint.MaxValue));

        /// <summary>How delivery of a match is going, or null for an unknown handle.</summary>
        public MatchStatus? GetMatchStatus(uint match)
        {
            var native = new NativeMatchStatus
            {
                size      = (uint)Marshal.SizeOf<NativeMatchStatus>(),
                matchId   = new byte[40],
                lastError = string.Empty,
            };

            if (!InterposerMatchGetStatus(match, ref native))
                return null;

            var length = Array.IndexOf(native.matchId, (byte)0);
            var id     = Encoding.ASCII.GetString(native.matchId, 0, length < 0 ? native.matchId.Length : length);

            return new MatchStatus
            {
                Sink               = (MatchSink)native.sink,
                MatchId            = Guid.TryParse(id, out var parsed) ? parsed : null,
                PendingEvents      = (int)native.pendingEvents,
                RejectedEvents     = (int)native.rejectedEvents,
                PendingAttachments = (int)native.pendingAttachments,
                Ended              = native.ended != 0,
                Delivered          = native.delivered != 0,
                LastError          = string.IsNullOrEmpty(native.lastError) ? null : native.lastError,
            };
        }
    }
}
