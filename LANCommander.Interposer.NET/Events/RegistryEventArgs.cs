using System;

namespace LANCommander.Interposer.Events
{
    /// <summary>
    /// Data for registry operation events (Open, Create, Query, Set, Delete, Enum).
    /// </summary>
    public sealed class RegistryEventArgs : EventArgs
    {
        /// <summary>
        /// The operation verb: "REG OPEN", "REG CREATE", "REG READ", "REG WRITE",
        /// "REG DELETE", "REG ENUM", "REG QUERY".
        /// <para>
        /// A "REG DELETE" with a null <see cref="ValueName"/> means the key itself was
        /// deleted; with a value name, only that value was. This is the only way to tell
        /// the two apart.
        /// </para>
        /// <para>
        /// The diagnostic verbs the session log also carries -- "REG HIT", "REG MISS",
        /// "REG PARTIAL", "REG LAYER", "REG FLUSH", "REG NOTIFY", "REG COPY" -- are
        /// written to the log only and never delivered here.
        /// </para>
        /// </summary>
        public string Verb { get; }

        /// <summary>The full registry key path.</summary>
        public string KeyPath { get; }

        /// <summary>The value name, or null.</summary>
        public string ValueName { get; }

        internal RegistryEventArgs(string verb, string keyPath, string valueName)
        {
            Verb = verb;
            KeyPath = keyPath;
            ValueName = valueName;
        }
    }
}
