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
        /// "REG DELETE", "REG ENUM", "REG ENUMKEY", "REG INFO".
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
