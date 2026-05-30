using System;

namespace LANCommander.Interposer
{
    /// <summary>
    /// Thrown when a DLL injection operation fails.
    /// </summary>
    public class InjectionException : Exception
    {
        public InjectionException(string message) : base(message) { }
        public InjectionException(string message, Exception innerException) : base(message, innerException) { }
    }
}
