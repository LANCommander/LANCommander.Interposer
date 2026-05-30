using System;

namespace LANCommander.Interposer.Events
{
    /// <summary>
    /// Data for identity query events (GetUserName, GetComputerName).
    /// </summary>
    public sealed class IdentityEventArgs : EventArgs
    {
        /// <summary>"USERNAME" or "COMPUTERNAME".</summary>
        public string IdentityType { get; }

        /// <summary>The value returned to the caller.</summary>
        public string Value { get; }

        internal IdentityEventArgs(string identityType, string value)
        {
            IdentityType = identityType;
            Value = value;
        }
    }
}
