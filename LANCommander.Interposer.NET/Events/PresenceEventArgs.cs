using System;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace LANCommander.Interposer.Events
{
    /// <summary>The activity a rich presence update shows, as the game last set it.</summary>
    /// <remarks>
    /// Strings are empty and numbers 0 when unset. Image fields hold whatever the game or its
    /// plugin set: a Discord asset key or an <c>https://</c> URL. Only the URLs can be shown
    /// anywhere but Discord.
    /// </remarks>
    public sealed class RichPresence
    {
        /// <summary>Activity type: 0 Playing, 1 Streaming, 2 Listening, 3 Watching, 5 Competing.</summary>
        [JsonPropertyName("type")] public int Type { get; init; }

        /// <summary>Display name, shown as "Playing <i>name</i>".</summary>
        [JsonPropertyName("name")] public string Name { get; init; } = string.Empty;

        /// <summary>First detail line.</summary>
        [JsonPropertyName("details")] public string Details { get; init; } = string.Empty;

        /// <summary>URL associated with <see cref="Details"/>.</summary>
        [JsonPropertyName("detailsUrl")] public string DetailsUrl { get; init; } = string.Empty;

        /// <summary>Second detail line.</summary>
        [JsonPropertyName("state")] public string State { get; init; } = string.Empty;

        /// <summary>URL associated with <see cref="State"/>.</summary>
        [JsonPropertyName("stateUrl")] public string StateUrl { get; init; } = string.Empty;

        /// <summary>Elapsed-time start, Unix epoch seconds; 0 when unset.</summary>
        [JsonPropertyName("timestampStart")] public long TimestampStart { get; init; }

        /// <summary>Remaining-time end, Unix epoch seconds; 0 when unset.</summary>
        [JsonPropertyName("timestampEnd")] public long TimestampEnd { get; init; }

        /// <summary>Large image: a Discord asset key or a URL.</summary>
        [JsonPropertyName("largeImage")] public string LargeImage { get; init; } = string.Empty;

        /// <summary>Tooltip for the large image.</summary>
        [JsonPropertyName("largeImageText")] public string LargeImageText { get; init; } = string.Empty;

        /// <summary>Small image: a Discord asset key or a URL.</summary>
        [JsonPropertyName("smallImage")] public string SmallImage { get; init; } = string.Empty;

        /// <summary>Tooltip for the small image.</summary>
        [JsonPropertyName("smallImageText")] public string SmallImageText { get; init; } = string.Empty;

        /// <summary>Arbitrary party identifier.</summary>
        [JsonPropertyName("partyId")] public string PartyId { get; init; } = string.Empty;

        /// <summary>Current party member count; 0 when unset.</summary>
        [JsonPropertyName("partySize")] public int PartySize { get; init; }

        /// <summary>Maximum party size; 0 when unset.</summary>
        [JsonPropertyName("partyMax")] public int PartyMax { get; init; }

        /// <summary>Label for the first button.</summary>
        [JsonPropertyName("button1Text")] public string Button1Text { get; init; } = string.Empty;

        /// <summary>URL the first button opens.</summary>
        [JsonPropertyName("button1Url")] public string Button1Url { get; init; } = string.Empty;

        /// <summary>Label for the second button.</summary>
        [JsonPropertyName("button2Text")] public string Button2Text { get; init; } = string.Empty;

        /// <summary>URL the second button opens.</summary>
        [JsonPropertyName("button2Url")] public string Button2Url { get; init; } = string.Empty;

        /// <summary>Reads the JSON the DLL sends with a presence update, or null when it isn't valid.</summary>
        public static RichPresence? FromJson(string? json)
        {
            if (string.IsNullOrWhiteSpace(json))
                return null;

            try
            {
                return JsonSerializer.Deserialize<RichPresence>(json);
            }
            catch (JsonException)
            {
                return null;
            }
        }
    }

    /// <summary>Data for a change in the game's rich presence.</summary>
    public sealed class PresenceChangedEventArgs : EventArgs
    {
        /// <summary>The presence now shown, or null when the game cleared it.</summary>
        public RichPresence? Presence { get; }

        internal PresenceChangedEventArgs(RichPresence? presence)
        {
            Presence = presence;
        }
    }
}
