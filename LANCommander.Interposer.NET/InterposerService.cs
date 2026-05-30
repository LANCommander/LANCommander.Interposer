using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using LANCommander.Interposer.Events;

namespace LANCommander.Interposer
{
    /// <summary>
    /// Single entry point for LANCommander Interposer: DLL injection, plugin API
    /// (logging, config, virtual registry, rich presence), and hook events (file,
    /// registry, DNS, network, identity).
    /// <para>
    /// <b>Launcher usage:</b> set override properties, then call
    /// <see cref="Inject(Process, string)"/> or <see cref="Start(ProcessStartInfo, string)"/>
    /// to inject the Interposer DLL into a game process.
    /// </para>
    /// <para>
    /// <b>In-process usage:</b> call <see cref="EnableEvents"/> to register native
    /// event callbacks, then use the plugin API methods and subscribe to events.
    /// </para>
    /// </summary>
    public class InterposerService : IDisposable
    {
        private const string DllName = "LANCommander.Interposer.dll";

        // =================================================================
        // P/Invoke - Plugin API
        // =================================================================

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerGetUsername(
            [Out] StringBuilder buffer, uint bufferSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private static extern void InterposerLog(string verb, string message);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerGetConfigString(
            string dotPath, [Out] StringBuilder buffer, uint bufferSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerRegisterPluginConfig(
            string pluginName, string yamlDefaults);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private static extern void InterposerSetRegistryValue(
            string keyPath, string valueName, string value);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private static extern uint InterposerSetRegistryValueBySuffix(
            string keySuffix, string valueName, string value);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerSetPresenceDetails([MarshalAs(UnmanagedType.LPUTF8Str)] string details);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerSetPresenceState([MarshalAs(UnmanagedType.LPUTF8Str)] string state);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerSetPresenceTimestamps(long start, long end);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerSetPresenceLargeImage(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string text);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerSetPresenceSmallImage(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string text);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerSetPresenceParty(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string id, int size, int max);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerSetPresenceButton(
            int index,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string text,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string url);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerSetPresenceType(int type);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerSetPresenceName([MarshalAs(UnmanagedType.LPUTF8Str)] string name);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerUpdatePresence();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerClearPresence();

        // =================================================================
        // P/Invoke - Callback registration
        // =================================================================

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerRegisterFileCallback(IntPtr cb);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerRegisterRegistryCallback(IntPtr cb);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerRegisterDnsCallback(IntPtr cb);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerRegisterNetworkCallback(IntPtr cb);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerRegisterIdentityCallback(IntPtr cb);

        // =================================================================
        // Native callback delegate types
        // =================================================================

        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private delegate void NativeFileCallback(IntPtr verb, IntPtr path, IntPtr secondaryPath);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private delegate void NativeRegistryCallback(IntPtr verb, IntPtr keyPath, IntPtr valueName);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private delegate void NativeDnsCallback(IntPtr hostname, IntPtr redirectedHostname);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private delegate void NativeNetworkCallback(IntPtr address, int port);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private delegate void NativeIdentityCallback(IntPtr type, IntPtr value);

        // =================================================================
        // Instance state
        // =================================================================

        private NativeFileCallback _fileDelegate;
        private NativeRegistryCallback _registryDelegate;
        private NativeDnsCallback _dnsDelegate;
        private NativeNetworkCallback _networkDelegate;
        private NativeIdentityCallback _identityDelegate;
        private bool _eventsEnabled;

        private IntPtr _fastDlMmf;
        private IntPtr _usernameMmf;
        private IntPtr _computerNameMmf;

        private NamedPipeServerStream _eventPipe;
        private CancellationTokenSource _eventCts;

        private bool _disposed;

        // =================================================================
        // Injection properties
        // =================================================================

        /// <summary>
        /// Optional FastDL base URL override. When set, a named memory-mapped file
        /// is created so the DLL reads this URL instead of the one in Config.yml.
        /// </summary>
        public string FastDlUrl { get; set; }

        /// <summary>
        /// Optional username override. When set, a named memory-mapped file is
        /// created so the DLL returns this name from GetUserNameW/A.
        /// </summary>
        public string Username { get; set; }

        /// <summary>
        /// Optional computer name override. When set, a named memory-mapped file
        /// is created so the DLL returns this name from GetComputerNameW/A.
        /// </summary>
        public string ComputerName { get; set; }

        // =================================================================
        // Events
        // =================================================================

        /// <summary>
        /// Fires on file operations: CreateFile, GetFileAttributes, FindFirstFile,
        /// Delete, Move, Copy, LoadLibrary.
        /// Works cross-process via named pipe when using <see cref="Inject"/> or
        /// <see cref="Start"/>, and in-process via <see cref="EnableEvents"/>.
        /// </summary>
        public event EventHandler<FileEventArgs> FileAccessed;

        /// <summary>
        /// Fires on registry operations: Open, Create, Query, Set, Delete, Enum.
        /// Works cross-process via named pipe when using <see cref="Inject"/> or
        /// <see cref="Start"/>, and in-process via <see cref="EnableEvents"/>.
        /// </summary>
        public event EventHandler<RegistryEventArgs> RegistryAccessed;

        /// <summary>
        /// Fires on DNS redirect events (when a hostname lookup matches a DnsRedirects rule).
        /// Works cross-process via named pipe when using <see cref="Inject"/> or
        /// <see cref="Start"/>, and in-process via <see cref="EnableEvents"/>.
        /// </summary>
        public event EventHandler<DnsEventArgs> DnsResolved;

        /// <summary>
        /// Fires when a new network address is discovered (connect, send/recv).
        /// Each unique address fires only once per session.
        /// Works cross-process via named pipe when using <see cref="Inject"/> or
        /// <see cref="Start"/>, and in-process via <see cref="EnableEvents"/>.
        /// </summary>
        public event EventHandler<NetworkEventArgs> NetworkConnection;

        /// <summary>
        /// Fires when GetUserName or GetComputerName is called and an identity
        /// override is active.
        /// Works cross-process via named pipe when using <see cref="Inject"/> or
        /// <see cref="Start"/>, and in-process via <see cref="EnableEvents"/>.
        /// </summary>
        public event EventHandler<IdentityEventArgs> IdentityQueried;

        // =================================================================
        // Injection - Process integration
        // =================================================================

        /// <summary>
        /// Injects the Interposer DLL into an already-running process.
        /// </summary>
        /// <param name="process">The target process. Must not have exited.</param>
        /// <param name="dllPath">
        /// Path to the Interposer DLL. If null, searches for interposer.dll or
        /// LANCommander.Interposer.dll next to the application.
        /// </param>
        /// <exception cref="InjectionException">Thrown if injection fails.</exception>
        public void Inject(Process process, string dllPath = null)
        {
            if (process == null)
                throw new ArgumentNullException(nameof(process));

            Inject(process.Id, dllPath);
        }

        /// <summary>
        /// Injects the Interposer DLL into a process identified by PID.
        /// </summary>
        /// <param name="processId">Target process ID.</param>
        /// <param name="dllPath">
        /// Path to the Interposer DLL. If null, searches for interposer.dll or
        /// LANCommander.Interposer.dll next to the application.
        /// </param>
        /// <exception cref="InjectionException">Thrown if injection fails.</exception>
        public void Inject(int processId, string dllPath = null)
        {
            dllPath = ResolveDllPath(dllPath);

            var hProcess = NativeMethods.OpenProcess(
                NativeMethods.PROCESS_CREATE_THREAD |
                NativeMethods.PROCESS_VM_OPERATION |
                NativeMethods.PROCESS_VM_WRITE |
                NativeMethods.PROCESS_VM_READ |
                NativeMethods.PROCESS_QUERY_INFORMATION,
                false,
                (uint)processId);

            if (hProcess == IntPtr.Zero)
                throw new InjectionException($"OpenProcess failed for PID {processId}.", new Win32Exception());

            try
            {
                CreateMappings((uint)processId);
                InjectDll(hProcess, dllPath);
            }
            finally
            {
                NativeMethods.CloseHandle(hProcess);
            }
        }

        /// <summary>
        /// Launches a process suspended, injects the Interposer DLL, then resumes it.
        /// The returned <see cref="Process"/> is already running with the DLL loaded.
        /// <para>
        /// The <see cref="ProcessStartInfo.FileName"/> must be set. Other properties
        /// like <see cref="ProcessStartInfo.Arguments"/>,
        /// <see cref="ProcessStartInfo.WorkingDirectory"/>, and
        /// <see cref="ProcessStartInfo.Environment"/> are respected.
        /// </para>
        /// </summary>
        /// <param name="startInfo">Describes the process to launch.</param>
        /// <param name="dllPath">
        /// Path to the Interposer DLL. If null, searches for interposer.dll or
        /// LANCommander.Interposer.dll next to the application.
        /// </param>
        /// <returns>The launched <see cref="Process"/> with the Interposer DLL loaded.</returns>
        /// <exception cref="InjectionException">Thrown if the process cannot be created or injection fails.</exception>
        public Process Start(ProcessStartInfo startInfo, string dllPath = null)
        {
            if (startInfo == null)
                throw new ArgumentNullException(nameof(startInfo));

            dllPath = ResolveDllPath(dllPath);

            var exePath = Path.GetFullPath(startInfo.FileName);

            if (!File.Exists(exePath))
                throw new InjectionException($"Executable not found: {exePath}");

            var workingDirectory = string.IsNullOrEmpty(startInfo.WorkingDirectory)
                ? Path.GetDirectoryName(exePath)
                : startInfo.WorkingDirectory;

            // Build command line
            var cmdLine = new StringBuilder();
            cmdLine.Append('"').Append(exePath).Append('"');

            if (!string.IsNullOrEmpty(startInfo.Arguments))
            {
                cmdLine.Append(' ');
                cmdLine.Append(startInfo.Arguments);
            }

            // Build environment block if custom variables are set
            IntPtr envBlock = IntPtr.Zero;
            uint creationFlags = NativeMethods.CREATE_SUSPENDED;

            if (startInfo.Environment != null && startInfo.Environment.Count > 0)
            {
                var envBuilder = new StringBuilder();

                foreach (var kvp in startInfo.Environment)
                {
                    envBuilder.Append(kvp.Key);
                    envBuilder.Append('=');
                    envBuilder.Append(kvp.Value);
                    envBuilder.Append('\0');
                }

                envBuilder.Append('\0');
                envBlock = Marshal.StringToHGlobalUni(envBuilder.ToString());
                creationFlags |= NativeMethods.CREATE_UNICODE_ENVIRONMENT;
            }

            var si = new NativeMethods.STARTUPINFO();
            si.cb = Marshal.SizeOf(si);

            NativeMethods.PROCESS_INFORMATION pi;

            try
            {
                if (!NativeMethods.CreateProcessW(
                        exePath,
                        cmdLine,
                        IntPtr.Zero,
                        IntPtr.Zero,
                        false,
                        creationFlags,
                        envBlock,
                        workingDirectory,
                        ref si,
                        out pi))
                {
                    throw new InjectionException("CreateProcess failed.", new Win32Exception());
                }
            }
            finally
            {
                if (envBlock != IntPtr.Zero)
                    Marshal.FreeHGlobal(envBlock);
            }

            try
            {
                CreateMappings(pi.dwProcessId);
                InjectDll(pi.hProcess, dllPath);

                NativeMethods.ResumeThread(pi.hThread);

                return Process.GetProcessById((int)pi.dwProcessId);
            }
            catch
            {
                NativeMethods.TerminateProcess(pi.hProcess, 1);
                throw;
            }
            finally
            {
                NativeMethods.CloseHandle(pi.hThread);
                NativeMethods.CloseHandle(pi.hProcess);
            }
        }

        // =================================================================
        // Events - lifecycle
        // =================================================================

        /// <summary>
        /// Registers native event callbacks so that hook events fire on this
        /// service instance. Only callable from within a process that has the
        /// Interposer DLL loaded. Safe to call multiple times.
        /// </summary>
        public void EnableEvents()
        {
            if (_eventsEnabled)
                return;

            _fileDelegate = OnFileCallback;
            _registryDelegate = OnRegistryCallback;
            _dnsDelegate = OnDnsCallback;
            _networkDelegate = OnNetworkCallback;
            _identityDelegate = OnIdentityCallback;

            InterposerRegisterFileCallback(
                Marshal.GetFunctionPointerForDelegate(_fileDelegate));
            InterposerRegisterRegistryCallback(
                Marshal.GetFunctionPointerForDelegate(_registryDelegate));
            InterposerRegisterDnsCallback(
                Marshal.GetFunctionPointerForDelegate(_dnsDelegate));
            InterposerRegisterNetworkCallback(
                Marshal.GetFunctionPointerForDelegate(_networkDelegate));
            InterposerRegisterIdentityCallback(
                Marshal.GetFunctionPointerForDelegate(_identityDelegate));

            _eventsEnabled = true;
        }

        /// <summary>
        /// Unregisters native event callbacks. No further events will fire
        /// until <see cref="EnableEvents"/> is called again.
        /// </summary>
        public void DisableEvents()
        {
            if (!_eventsEnabled)
                return;

            InterposerRegisterFileCallback(IntPtr.Zero);
            InterposerRegisterRegistryCallback(IntPtr.Zero);
            InterposerRegisterDnsCallback(IntPtr.Zero);
            InterposerRegisterNetworkCallback(IntPtr.Zero);
            InterposerRegisterIdentityCallback(IntPtr.Zero);

            _fileDelegate = null;
            _registryDelegate = null;
            _dnsDelegate = null;
            _networkDelegate = null;
            _identityDelegate = null;

            _eventsEnabled = false;
        }

        // =================================================================
        // Dispose
        // =================================================================

        /// <summary>
        /// Unregisters native event callbacks and releases memory-mapped file handles.
        /// </summary>
        public void Dispose()
        {
            if (_disposed)
                return;
            _disposed = true;

            if (_eventsEnabled)
                DisableEvents();

            if (_eventCts != null)
            {
                _eventCts.Cancel();
                _eventCts.Dispose();
                _eventCts = null;
            }

            if (_eventPipe != null)
            {
                _eventPipe.Dispose();
                _eventPipe = null;
            }

            CloseMmf(ref _fastDlMmf);
            CloseMmf(ref _usernameMmf);
            CloseMmf(ref _computerNameMmf);
        }

        // =================================================================
        // Config & Logging (in-process only)
        // =================================================================

        /// <summary>
        /// Gets the effective username (configured override or real Windows account name).
        /// Returns null if the call fails. In-process only.
        /// </summary>
        public string GetUsername()
        {
            var sb = new StringBuilder(256);
            return InterposerGetUsername(sb, (uint)sb.Capacity) ? sb.ToString() : null;
        }

        /// <summary>
        /// Writes a line to the Interposer session log regardless of logging flags.
        /// In-process only.
        /// </summary>
        /// <param name="verb">Log verb padded to ~15 chars for alignment, e.g. "[MYPLUGIN]     "</param>
        /// <param name="message">Log message text.</param>
        public void Log(string verb, string message)
        {
            InterposerLog(verb, message);
        }

        /// <summary>
        /// Reads a scalar value from Config.yml by dot-separated YAML path.
        /// Returns null if the key is missing, not scalar, or the buffer is too small.
        /// In-process only.
        /// </summary>
        /// <param name="dotPath">Dot-separated path, e.g. "Plugins.MyPlugin.Setting"</param>
        public string GetConfigString(string dotPath)
        {
            var sb = new StringBuilder(1024);
            return InterposerGetConfigString(dotPath, sb, (uint)sb.Capacity) ? sb.ToString() : null;
        }

        /// <summary>
        /// Registers default configuration for a plugin. If a Plugins.&lt;pluginName&gt;
        /// section already exists in Config.yml, the defaults are not written.
        /// In-process only.
        /// </summary>
        /// <param name="pluginName">Key under Plugins: in Config.yml (e.g. "CDKey").</param>
        /// <param name="yamlDefaults">YAML map body with default keys/values.</param>
        /// <returns>True if the section existed or defaults were written successfully.</returns>
        public bool RegisterPluginConfig(string pluginName, string yamlDefaults)
        {
            return InterposerRegisterPluginConfig(pluginName, yamlDefaults);
        }

        // =================================================================
        // Virtual Registry (in-process only)
        // =================================================================

        /// <summary>
        /// Injects a transient REG_SZ value into the virtual registry store by exact key path.
        /// In-process only.
        /// </summary>
        public void SetRegistryValue(string keyPath, string valueName, string value)
        {
            InterposerSetRegistryValue(keyPath, valueName, value);
        }

        /// <summary>
        /// Injects a transient REG_SZ value into every virtual store key whose path ends
        /// with the given suffix (backslash-boundary, case-insensitive match).
        /// In-process only.
        /// </summary>
        /// <returns>The number of keys updated.</returns>
        public uint SetRegistryValueBySuffix(string keySuffix, string valueName, string value)
        {
            return InterposerSetRegistryValueBySuffix(keySuffix, valueName, value);
        }

        // =================================================================
        // Rich Presence (in-process only)
        // =================================================================

        /// <summary>Sets the first detail line of the rich presence.</summary>
        public void SetPresenceDetails(string details) => InterposerSetPresenceDetails(details);

        /// <summary>Sets the second detail line (state) of the rich presence.</summary>
        public void SetPresenceState(string state) => InterposerSetPresenceState(state);

        /// <summary>Sets the start/end timestamps (Unix epoch seconds, 0 = not set).</summary>
        public void SetPresenceTimestamps(long start, long end) => InterposerSetPresenceTimestamps(start, end);

        /// <summary>Sets the large image asset key and tooltip text.</summary>
        public void SetPresenceLargeImage(string key, string text) => InterposerSetPresenceLargeImage(key, text);

        /// <summary>Sets the small image asset key and tooltip text.</summary>
        public void SetPresenceSmallImage(string key, string text) => InterposerSetPresenceSmallImage(key, text);

        /// <summary>Sets the party info (id, current size, max size).</summary>
        public void SetPresenceParty(string id, int size, int max) => InterposerSetPresenceParty(id, size, max);

        /// <summary>Sets a presence button (index 0 or 1, label, URL).</summary>
        public void SetPresenceButton(int index, string text, string url) => InterposerSetPresenceButton(index, text, url);

        /// <summary>Sets the activity type (0=Playing, 1=Streaming, 2=Listening, 3=Watching, 5=Competing).</summary>
        public void SetPresenceType(int type) => InterposerSetPresenceType(type);

        /// <summary>Sets the display name shown after the activity verb.</summary>
        public void SetPresenceName(string name) => InterposerSetPresenceName(name);

        /// <summary>Pushes the current presence state to all connected backends (e.g. Discord).</summary>
        public void UpdatePresence() => InterposerUpdatePresence();

        /// <summary>Disconnects all backends and clears the presence.</summary>
        public void ClearPresence() => InterposerClearPresence();

        // =================================================================
        // Private - injection helpers
        // =================================================================

        private static string ResolveDllPath(string dllPath)
        {
            if (!string.IsNullOrEmpty(dllPath))
            {
                dllPath = Path.GetFullPath(dllPath);

                if (!File.Exists(dllPath))
                    throw new InjectionException($"DLL not found: {dllPath}");

                return dllPath;
            }

            var baseDir = AppDomain.CurrentDomain.BaseDirectory;
            var candidates = new[] { "interposer.dll", "LANCommander.Interposer.dll" };

            foreach (var candidate in candidates)
            {
                var path = Path.Combine(baseDir, candidate);

                if (File.Exists(path))
                    return path;
            }

            throw new InjectionException(
                "No DLL path specified and neither interposer.dll nor " +
                "LANCommander.Interposer.dll was found next to the application.");
        }

        private void CreateMappings(uint pid)
        {
            if (!string.IsNullOrEmpty(FastDlUrl))
                _fastDlMmf = CreateUtf8Mmf($"Local\\InterposerFastDL_{pid}", FastDlUrl, 2048);

            if (!string.IsNullOrEmpty(Username))
                _usernameMmf = CreateUtf8Mmf($"Local\\InterposerUsername_{pid}", Username, 512);

            if (!string.IsNullOrEmpty(ComputerName))
                _computerNameMmf = CreateUtf8Mmf($"Local\\InterposerComputerName_{pid}", ComputerName, 512);

            // Create a named pipe for the injected DLL to send events back.
            // The pipe must exist before the DLL is injected so it can connect
            // during DLL_PROCESS_ATTACH.
            var pipeName = $"InterposerEvents_{pid}";
            _eventPipe = new NamedPipeServerStream(
                pipeName,
                PipeDirection.In,
                1,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous);

            _eventCts = new CancellationTokenSource();
            Task.Run(() => ReadEventPipeAsync(_eventCts.Token));
        }

        private static IntPtr CreateUtf8Mmf(string name, string value, uint size)
        {
            var hMmf = NativeMethods.CreateFileMappingW(
                new IntPtr(-1),
                IntPtr.Zero,
                NativeMethods.PAGE_READWRITE,
                0, size, name);

            if (hMmf == IntPtr.Zero)
                throw new InjectionException($"CreateFileMapping failed for '{name}'.", new Win32Exception());

            var view = NativeMethods.MapViewOfFile(hMmf, NativeMethods.FILE_MAP_WRITE, 0, 0, (UIntPtr)size);

            if (view == IntPtr.Zero)
            {
                NativeMethods.CloseHandle(hMmf);
                throw new InjectionException($"MapViewOfFile failed for '{name}'.", new Win32Exception());
            }

            try
            {
                var utf8 = Encoding.UTF8.GetBytes(value + '\0');

                if (utf8.Length <= size)
                    Marshal.Copy(utf8, 0, view, utf8.Length);
            }
            finally
            {
                NativeMethods.UnmapViewOfFile(view);
            }

            return hMmf;
        }

        private static void InjectDll(IntPtr hProcess, string dllPath)
        {
            var pathBytes = Encoding.Unicode.GetBytes(dllPath + '\0');
            var pathSize = (UIntPtr)pathBytes.Length;

            var remoteMem = NativeMethods.VirtualAllocEx(
                hProcess, IntPtr.Zero, pathSize,
                NativeMethods.MEM_COMMIT | NativeMethods.MEM_RESERVE,
                NativeMethods.PAGE_READWRITE);

            if (remoteMem == IntPtr.Zero)
                throw new InjectionException("VirtualAllocEx failed.", new Win32Exception());

            try
            {
                UIntPtr written;

                if (!NativeMethods.WriteProcessMemory(hProcess, remoteMem, pathBytes, pathSize, out written)
                    || written != pathSize)
                {
                    throw new InjectionException("WriteProcessMemory failed.", new Win32Exception());
                }

                var hKernel32 = NativeMethods.GetModuleHandleW("kernel32.dll");
                var loadLibrary = NativeMethods.GetProcAddress(hKernel32, "LoadLibraryW");

                if (loadLibrary == IntPtr.Zero)
                    throw new InjectionException("Could not resolve LoadLibraryW.");

                var hThread = NativeMethods.CreateRemoteThread(
                    hProcess, IntPtr.Zero, UIntPtr.Zero,
                    loadLibrary, remoteMem,
                    0, IntPtr.Zero);

                if (hThread == IntPtr.Zero)
                    throw new InjectionException(
                        "CreateRemoteThread failed. Try running as Administrator.",
                        new Win32Exception());

                NativeMethods.WaitForSingleObject(hThread, 0xFFFFFFFF);

                uint exitCode;
                NativeMethods.GetExitCodeThread(hThread, out exitCode);
                NativeMethods.CloseHandle(hThread);

                if (exitCode == 0)
                    throw new InjectionException("Injection failed: LoadLibraryW returned NULL in the target process.");
            }
            finally
            {
                NativeMethods.VirtualFreeEx(hProcess, remoteMem, UIntPtr.Zero, NativeMethods.MEM_RELEASE);
            }
        }

        private static void CloseMmf(ref IntPtr handle)
        {
            if (handle != IntPtr.Zero)
            {
                NativeMethods.CloseHandle(handle);
                handle = IntPtr.Zero;
            }
        }

        // =================================================================
        // Private - named pipe event reader
        // =================================================================

        private async Task ReadEventPipeAsync(CancellationToken ct)
        {
            try
            {
                await _eventPipe.WaitForConnectionAsync(ct).ConfigureAwait(false);

                // Buffer for reading from the pipe. Messages are variable-length
                // binary: int32 eventType, then 3x (int32 len + wchar[len]), then int32.
                var intBuf = new byte[4];

                while (!ct.IsCancellationRequested && _eventPipe.IsConnected)
                {
                    if (!await ReadExactAsync(_eventPipe, intBuf, ct).ConfigureAwait(false))
                        break;

                    int eventType = BitConverter.ToInt32(intBuf, 0);

                    string field1 = await ReadPipeStringAsync(_eventPipe, intBuf, ct).ConfigureAwait(false);
                    string field2 = await ReadPipeStringAsync(_eventPipe, intBuf, ct).ConfigureAwait(false);
                    string field3 = await ReadPipeStringAsync(_eventPipe, intBuf, ct).ConfigureAwait(false);

                    if (!await ReadExactAsync(_eventPipe, intBuf, ct).ConfigureAwait(false))
                        break;

                    int intField = BitConverter.ToInt32(intBuf, 0);

                    DispatchPipeEvent(eventType, field1, field2, field3, intField);
                }
            }
            catch (OperationCanceledException) { }
            catch (IOException) { }
            catch (ObjectDisposedException) { }
        }

        private static async Task<string> ReadPipeStringAsync(
            NamedPipeServerStream pipe, byte[] intBuf, CancellationToken ct)
        {
            if (!await ReadExactAsync(pipe, intBuf, ct).ConfigureAwait(false))
                return null;

            int charCount = BitConverter.ToInt32(intBuf, 0);

            if (charCount <= 0)
                return null;

            var strBuf = new byte[charCount * 2]; // wchar_t = 2 bytes

            if (!await ReadExactAsync(pipe, strBuf, ct).ConfigureAwait(false))
                return null;

            return Encoding.Unicode.GetString(strBuf);
        }

        private static async Task<bool> ReadExactAsync(
            NamedPipeServerStream pipe, byte[] buffer, CancellationToken ct)
        {
            int offset = 0;

            while (offset < buffer.Length)
            {
                int read = await pipe.ReadAsync(buffer, offset, buffer.Length - offset, ct)
                    .ConfigureAwait(false);

                if (read == 0)
                    return false; // pipe closed

                offset += read;
            }

            return true;
        }

        private void DispatchPipeEvent(int eventType, string field1, string field2, string field3, int intField)
        {
            switch (eventType)
            {
                case 1: // FILE
                    FileAccessed?.Invoke(this, new FileEventArgs(field1, field2, field3));
                    break;
                case 2: // REGISTRY
                    RegistryAccessed?.Invoke(this, new RegistryEventArgs(field1, field2, field3));
                    break;
                case 3: // DNS
                    DnsResolved?.Invoke(this, new DnsEventArgs(field1, field2));
                    break;
                case 4: // NETWORK
                    NetworkConnection?.Invoke(this, new NetworkEventArgs(field1, intField));
                    break;
                case 5: // IDENTITY
                    IdentityQueried?.Invoke(this, new IdentityEventArgs(field1, field2));
                    break;
            }
        }

        // =================================================================
        // Private - native callback handlers
        // =================================================================

        private void OnFileCallback(IntPtr verb, IntPtr path, IntPtr secondaryPath)
        {
            FileAccessed?.Invoke(this, new FileEventArgs(
                Marshal.PtrToStringUni(verb),
                Marshal.PtrToStringUni(path),
                secondaryPath != IntPtr.Zero ? Marshal.PtrToStringUni(secondaryPath) : null));
        }

        private void OnRegistryCallback(IntPtr verb, IntPtr keyPath, IntPtr valueName)
        {
            RegistryAccessed?.Invoke(this, new RegistryEventArgs(
                Marshal.PtrToStringUni(verb),
                Marshal.PtrToStringUni(keyPath),
                valueName != IntPtr.Zero ? Marshal.PtrToStringUni(valueName) : null));
        }

        private void OnDnsCallback(IntPtr hostname, IntPtr redirectedHostname)
        {
            DnsResolved?.Invoke(this, new DnsEventArgs(
                Marshal.PtrToStringUni(hostname),
                Marshal.PtrToStringUni(redirectedHostname)));
        }

        private void OnNetworkCallback(IntPtr address, int port)
        {
            NetworkConnection?.Invoke(this, new NetworkEventArgs(
                Marshal.PtrToStringUni(address),
                port));
        }

        private void OnIdentityCallback(IntPtr type, IntPtr value)
        {
            IdentityQueried?.Invoke(this, new IdentityEventArgs(
                Marshal.PtrToStringUni(type),
                Marshal.PtrToStringUni(value)));
        }
    }
}
