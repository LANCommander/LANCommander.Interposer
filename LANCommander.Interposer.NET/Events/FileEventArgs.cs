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
        /// <para>
        /// The redirect diagnostics the DLL emits at Logging.Level Debug and Trace
        /// ("REDIRECT HIT", "REDIRECT MISS", "REDIRECT RULE") are written to the
        /// session log only and are never delivered here, so this list stays stable
        /// as the log gains detail.
        /// </para>
        /// </summary>
        public string Verb { get; }

        /// <summary>The primary file path.</summary>
        public string Path { get; }

        /// <summary>
        /// The redirect target, destination path, or null. For "FILE REDIRECT" this
        /// is the fully resolved destination - capture groups and any <c>%TOKEN%</c>
        /// path references in the rule have already been expanded.
        /// </summary>
        public string SecondaryPath { get; }

        internal FileEventArgs(string verb, string path, string secondaryPath)
        {
            Verb = verb;
            Path = path;
            SecondaryPath = secondaryPath;
        }
    }
}
