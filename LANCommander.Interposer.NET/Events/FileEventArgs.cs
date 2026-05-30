using System;

namespace LANCommander.Interposer.Events
{
    /// <summary>
    /// Data for file operation events (CreateFile, GetFileAttributes, FindFirstFile,
    /// Delete, Move, Copy, LoadLibrary).
    /// </summary>
    public sealed class FileEventArgs : EventArgs
    {
        /// <summary>
        /// The operation verb: "FILE READ", "FILE WRITE", "FILE R/W", "FILE ATTR",
        /// "FILE REDIRECT", "FILE OVERLAY", "FILE DELETE", "FILE MOVE", "FILE COPY",
        /// "FILE FIND", "DLL LOAD".
        /// </summary>
        public string Verb { get; }

        /// <summary>The primary file path.</summary>
        public string Path { get; }

        /// <summary>The redirect target, destination path, or null.</summary>
        public string SecondaryPath { get; }

        internal FileEventArgs(string verb, string path, string secondaryPath)
        {
            Verb = verb;
            Path = path;
            SecondaryPath = secondaryPath;
        }
    }
}
