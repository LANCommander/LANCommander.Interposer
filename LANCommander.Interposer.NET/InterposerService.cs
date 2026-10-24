using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
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
    /// Features that are configured entirely through <c>.interposer\Config.yml</c> and
    /// need no managed API - file redirection (including <c>%TOKEN%</c> path
    /// expansion), DNS redirection, DirectInput fixes and OS version spoofing -
    /// take effect as soon as the DLL is injected. Read their settings back with
    /// <see cref="GetConfigString(string)"/>.
    /// </para>
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
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerSetConfigString(
            string dotPath, string value);

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
        // P/Invoke - Network adapter enumeration
        // =================================================================

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct NativeNetworkAdapter
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string FriendlyName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Description;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 24)]  public string MacAddress;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 16)]  public string Ipv4Address;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 46)]  public string Ipv6Address;
            public int Allowed;
        }

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern uint InterposerEnumNetworkAdapters(
            [Out] NativeNetworkAdapter[] buffer, uint capacity, out uint outCount);

        // =================================================================
        // P/Invoke - Audio device enumeration
        // =================================================================

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct NativeAudioDevice
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Name;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Description;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Id;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string InstanceId;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string HardwareId;
            public int Direction;
            public int IsDefault;
            public int Allowed;
        }

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern uint InterposerEnumAudioDevices(
            [Out] NativeAudioDevice[] buffer, uint capacity, out uint outCount);

        // =================================================================
        // P/Invoke - Network relay
        // =================================================================

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct NativeRelayStatus
        {
            public uint Size;
            public int State;
            public uint ClientId;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 16)]  public string VirtualAddress;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 16)]  public string Netmask;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Network;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Server;
            public int UdpActive;
            public uint PeerCount;
            public ulong BytesSent;
            public ulong BytesReceived;
            public ulong DatagramsSent;
            public ulong DatagramsReceived;
            public uint StreamsOpen;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string LastError;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct NativeRelayPeer
        {
            public uint ClientId;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 16)]  public string VirtualAddress;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Name;
        }

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerRelayGetStatus(ref NativeRelayStatus status);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern uint InterposerRelayEnumPeers(
            [Out] NativeRelayPeer[] buffer, uint capacity, out uint outCount);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InterposerRelayConnect(string yamlOverrides);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerRelayDisconnect();

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

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void InterposerRegisterRelayCallback(IntPtr cb);

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

        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private delegate void NativeRelayCallback(IntPtr kind, IntPtr address, IntPtr detail, int value);

        // =================================================================
        // Instance state
        // =================================================================

        private NativeFileCallback _fileDelegate;
        private NativeRegistryCallback _registryDelegate;
        private NativeDnsCallback _dnsDelegate;
        private NativeNetworkCallback _networkDelegate;
        private NativeIdentityCallback _identityDelegate;
        private NativeRelayCallback? _relayDelegate;
        private bool _eventsEnabled;

        private IntPtr _fastDlMmf;
        private IntPtr _usernameMmf;
        private IntPtr _computerNameMmf;
        private IntPtr _relayMmf;

        private ControlChannel? _control;

        private NamedPipeServerStream _eventPipe;
        private CancellationTokenSource _eventCts;
        private Task _eventPipeTask;

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

        /// <summary>
        /// Optional network relay settings, overriding the <c>NetworkRelay</c> section
        /// of Config.yml key by key. When set, a named memory-mapped file carries them
        /// to the DLL.
        /// <para>
        /// Setting this at all -- even to options with <see cref="RelayOptions.Enabled"/>
        /// false -- makes the DLL install its socket hooks, which it can only do at
        /// startup. Do that when the launcher may want to call
        /// <see cref="ConnectRelayAsync"/> later, for a game that was started before its
        /// relay session existed.
        /// </para>
        /// </summary>
        public RelayOptions? Relay { get; set; }

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

        /// <summary>
        /// Fires when the network relay connects, loses its server, reconnects, is
        /// refused, or is disconnected.
        /// Works cross-process via named pipe when using <see cref="Inject"/> or
        /// <see cref="Start"/>, and in-process via <see cref="EnableEvents"/>.
        /// </summary>
        public event EventHandler<RelayStateChangedEventArgs>? RelayStateChanged;

        /// <summary>
        /// Fires when another player joins the relay network, including once for each
        /// member already present when the game connects.
        /// Works cross-process via named pipe and in-process via <see cref="EnableEvents"/>.
        /// </summary>
        public event EventHandler<RelayPeerEventArgs>? RelayPeerJoined;

        /// <summary>
        /// Fires when another player leaves the relay network, and for every peer when
        /// the game loses its connection to the relay server.
        /// Works cross-process via named pipe and in-process via <see cref="EnableEvents"/>.
        /// </summary>
        public event EventHandler<RelayPeerEventArgs>? RelayPeerLeft;

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
        /// Injects the Interposer DLL into a process identified by PID. The process may be
        /// 32-bit or 64-bit whatever this process is; see <see cref="Start(ProcessStartInfo, string)"/>.
        /// </summary>
        /// <param name="processId">Target process ID.</param>
        /// <param name="dllPath">
        /// Path to the Interposer DLL, which must match the target's architecture. If null,
        /// the DLL for the target's architecture is found next to the application.
        /// </param>
        /// <exception cref="InjectionException">Thrown if injection fails.</exception>
        public void Inject(int processId, string dllPath = null)
        {
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
                var architecture = GetProcessArchitecture(hProcess);
                var binaries = ResolveBinaries(dllPath, architecture);

                CreateMappings((uint)processId);
                InjectInto(hProcess, (uint)processId, binaries);
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
        /// <para>
        /// The game may be 32-bit or 64-bit whatever this process is. A process of the
        /// same architecture is injected directly; one of the other architecture through
        /// <c>LANCommander.Interposer.Injector.exe</c> built for it, since a remote
        /// <c>LoadLibraryW</c> only works between processes of one architecture. The
        /// <c>LANCommander.Interposer</c> package puts both architectures' DLL and
        /// injector under <c>x86\</c> and <c>x64\</c> beside the application.
        /// </para>
        /// <para>
        /// Do not use this for a game that already loads the Interposer as a proxy DLL
        /// beside its executable: the DLL would be loaded twice. Use
        /// <see cref="StartWithProxy"/>, and <see cref="IsDeployedAsProxy"/> to tell.
        /// </para>
        /// </summary>
        /// <param name="startInfo">Describes the process to launch.</param>
        /// <param name="dllPath">
        /// Path to the Interposer DLL, which must match the game's architecture. If null,
        /// the DLL for the game's architecture is found next to the application.
        /// </param>
        /// <returns>The launched <see cref="Process"/> with the Interposer DLL loaded.</returns>
        /// <exception cref="InjectionException">Thrown if the process cannot be created or injection fails.</exception>
        public Process Start(ProcessStartInfo startInfo, string dllPath = null)
            => StartCore(startInfo, dllPath, inject: true);

        /// <summary>
        /// Launches a game that loads the Interposer itself, as a proxy DLL beside its
        /// executable (<c>version.dll</c>, <c>dinput8.dll</c>, ...), with this instance's
        /// overrides in place. Everything <see cref="Start(ProcessStartInfo, string)"/>
        /// sets up before injecting -- <see cref="Username"/>, <see cref="Relay"/> and the
        /// other overrides, the control pipe the relay methods use, and the event pipe --
        /// is set up here too, but nothing is injected: the proxy picks it all up when the
        /// game loads it.
        /// </summary>
        /// <param name="startInfo">Describes the process to launch.</param>
        /// <returns>The launched <see cref="Process"/>.</returns>
        /// <exception cref="InjectionException">Thrown if the process cannot be created.</exception>
        public Process StartWithProxy(ProcessStartInfo startInfo)
            => StartCore(startInfo, null, inject: false);

        /// <summary>
        /// True when the Interposer is deployed as a proxy DLL beside
        /// <paramref name="executablePath"/>, under one of the names Windows or an ASI
        /// loader loads by itself. Such a game must be started with
        /// <see cref="StartWithProxy"/> rather than injected. Recognises the Interposer by
        /// its exports, so a game's own <c>dinput8.dll</c> does not count.
        /// </summary>
        public static bool IsDeployedAsProxy(string executablePath)
        {
            var directory = Path.GetDirectoryName(Path.GetFullPath(executablePath));

            if (directory == null || !Directory.Exists(directory))
                return false;

            foreach (var name in ProxyNames)
            {
                var path = Path.Combine(directory, name);

                if (File.Exists(path) && IsInterposer(path))
                    return true;
            }

            foreach (var path in Directory.EnumerateFiles(directory, "*.asi"))
            {
                if (IsInterposer(path))
                    return true;
            }

            return false;
        }

        // The names the Interposer can stand in for; see "Release Variants" in the README.
        private static readonly string[] ProxyNames = { "version.dll", "dinput8.dll", "dinput.dll" };

        /// <summary>
        /// Whether the DLL at <paramref name="path"/> is the Interposer, whatever it has been
        /// renamed to: it exports the <c>Interposer*</c> plugin API. Not every build carries a
        /// version resource, so the exports are what identify it.
        /// </summary>
        private const string ExportPrefix = "Interposer";

        private static bool IsInterposer(string path)
        {
            try
            {
                using var stream = File.OpenRead(path);
                using var pe = new System.Reflection.PortableExecutable.PEReader(stream);

                var directory = pe.PEHeaders.PEHeader?.ExportTableDirectory;

                if (directory is not { Size: > 0 } exports)
                    return false;

                var image = pe.GetEntireImage();
                var reader = image.GetReader();

                int Offset(int rva)
                {
                    foreach (var section in pe.PEHeaders.SectionHeaders)
                    {
                        if (rva >= section.VirtualAddress && rva < section.VirtualAddress + Math.Max(section.VirtualSize, section.SizeOfRawData))
                            return rva - section.VirtualAddress + section.PointerToRawData;
                    }

                    throw new BadImageFormatException();
                }

                // IMAGE_EXPORT_DIRECTORY: NumberOfNames at +24, AddressOfNames at +32
                reader.Offset = Offset(exports.RelativeVirtualAddress) + 24;
                var count = reader.ReadInt32();
                reader.Offset += 4;
                var names = Offset(reader.ReadInt32());

                for (var i = 0; i < count; i++)
                {
                    reader.Offset = names + i * 4;
                    reader.Offset = Offset(reader.ReadInt32());

                    if (reader.RemainingBytes >= ExportPrefix.Length && reader.ReadUTF8(ExportPrefix.Length) == ExportPrefix)
                        return true;
                }

                return false;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or BadImageFormatException or ArgumentOutOfRangeException)
            {
                return false;
            }
        }

        private Process StartCore(ProcessStartInfo startInfo, string dllPath, bool inject)
        {
            if (startInfo == null)
                throw new ArgumentNullException(nameof(startInfo));

            var exePath = Path.GetFullPath(startInfo.FileName);

            if (!File.Exists(exePath))
                throw new InjectionException($"Executable not found: {exePath}");

            // Found before the process exists, so a missing DLL or injector fails the launch
            // rather than a game that has already started.
            var binaries = inject ? ResolveBinaries(dllPath, GetImageArchitecture(exePath)) : null;

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

                if (binaries != null)
                    InjectInto(pi.hProcess, pi.dwProcessId, binaries);

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

            // Absent from DLLs that predate the network relay.
            try
            {
                _relayDelegate = OnRelayCallback;
                InterposerRegisterRelayCallback(Marshal.GetFunctionPointerForDelegate(_relayDelegate));
            }
            catch (EntryPointNotFoundException)
            {
                _relayDelegate = null;
            }

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

            if (_relayDelegate != null)
                InterposerRegisterRelayCallback(IntPtr.Zero);

            _relayDelegate = null;
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

            _control?.Dispose();
            _control = null;

            CloseMmf(ref _fastDlMmf);
            CloseMmf(ref _usernameMmf);
            CloseMmf(ref _computerNameMmf);
            CloseMmf(ref _relayMmf);
        }

        // =================================================================
        // Network relay - cross-process control
        // =================================================================

        /// <summary>
        /// True once the injected DLL has connected its control pipe, which it does
        /// during injection. Only processes started or injected by this instance have one.
        /// </summary>
        public bool IsControlConnected => _control?.IsConnected == true;

        /// <summary>
        /// (Re)connects the injected game's network relay, applying <paramref name="options"/>
        /// on top of its current settings; null reconnects with the settings it has.
        /// Connecting implies enabling, whatever <see cref="RelayOptions.Enabled"/> says.
        /// Returns false if the DLL rejected the options.
        /// <para>
        /// The game's socket hooks must already be installed: set <see cref="Relay"/>
        /// before injecting, or enable <c>NetworkRelay</c> in Config.yml.
        /// </para>
        /// </summary>
        public async Task<bool> ConnectRelayAsync(RelayOptions? options = null, CancellationToken cancellationToken = default)
        {
            var (ok, _) = await RequireControl()
                .SendAsync(ControlChannel.CommandRelayConnect, options?.ToYaml(), cancellationToken)
                .ConfigureAwait(false);
            return ok;
        }

        /// <summary>
        /// Disconnects the injected game from the relay server and stops it retrying.
        /// Its virtual address stays assigned, so a later <see cref="ConnectRelayAsync"/>
        /// asks for the same one.
        /// </summary>
        public async Task DisconnectRelayAsync(CancellationToken cancellationToken = default)
        {
            await RequireControl()
                .SendAsync(ControlChannel.CommandRelayDisconnect, null, cancellationToken)
                .ConfigureAwait(false);
        }

        /// <summary>Reads the injected game's relay state, address, peers and traffic counters.</summary>
        public async Task<RelayStatus> GetRelayStatusAsync(CancellationToken cancellationToken = default)
        {
            var (_, json) = await RequireControl()
                .SendAsync(ControlChannel.CommandRelayStatus, null, cancellationToken)
                .ConfigureAwait(false);

            return JsonSerializer.Deserialize<RelayStatus>(json) ?? new RelayStatus();
        }

        /// <summary>
        /// Reads a scalar from the injected game's Config.yml by dot path -- the
        /// cross-process counterpart of <see cref="GetConfigString(string)"/>.
        /// Returns null when the key is missing or not a scalar.
        /// </summary>
        public async Task<string?> QueryConfigStringAsync(string dotPath, CancellationToken cancellationToken = default)
        {
            var (ok, value) = await RequireControl()
                .SendAsync(ControlChannel.CommandGetConfig, dotPath, cancellationToken)
                .ConfigureAwait(false);
            return ok ? value : null;
        }

        /// <summary>Round-trips a request to the injected DLL. True when it answered.</summary>
        public async Task<bool> PingAsync(CancellationToken cancellationToken = default)
        {
            var (ok, reply) = await RequireControl()
                .SendAsync(ControlChannel.CommandPing, null, cancellationToken)
                .ConfigureAwait(false);
            return ok && reply == "pong";
        }

        private ControlChannel RequireControl()
            => _control ?? throw new InvalidOperationException(
                "No control pipe: only a process started or injected by this InterposerService has one.");

        // =================================================================
        // Network relay - in-process
        // =================================================================

        /// <summary>
        /// Reads the relay state of the DLL loaded in this process. In-process only;
        /// a launcher uses <see cref="GetRelayStatusAsync"/>.
        /// </summary>
        public RelayStatus GetRelayStatus()
        {
            var native = new NativeRelayStatus { Size = (uint)Marshal.SizeOf<NativeRelayStatus>() };

            if (!InterposerRelayGetStatus(ref native))
                return new RelayStatus();

            var status = new RelayStatus
            {
                State             = (RelayState)native.State,
                ClientId          = native.ClientId,
                VirtualAddress    = native.VirtualAddress ?? string.Empty,
                Netmask           = native.Netmask ?? string.Empty,
                Network           = native.Network ?? string.Empty,
                Server            = native.Server ?? string.Empty,
                UdpActive         = native.UdpActive != 0,
                BytesSent         = native.BytesSent,
                BytesReceived     = native.BytesReceived,
                DatagramsSent     = native.DatagramsSent,
                DatagramsReceived = native.DatagramsReceived,
                StreamsOpen       = native.StreamsOpen,
                LastError         = native.LastError ?? string.Empty,
            };

            var total = InterposerRelayEnumPeers(null!, 0, out _);
            if (total > 0)
            {
                var peers = new NativeRelayPeer[total];
                InterposerRelayEnumPeers(peers, total, out var written);

                for (var i = 0; i < Math.Min(written, total); i++)
                    status.Peers.Add(new RelayPeerInfo
                    {
                        ClientId       = peers[i].ClientId,
                        VirtualAddress = peers[i].VirtualAddress ?? string.Empty,
                        Name           = peers[i].Name ?? string.Empty,
                    });
            }

            return status;
        }

        /// <summary>
        /// (Re)connects the relay of the DLL loaded in this process. In-process only;
        /// see <see cref="ConnectRelayAsync"/>.
        /// </summary>
        public bool ConnectRelay(RelayOptions? options = null) => InterposerRelayConnect(options?.ToYaml() ?? string.Empty);

        /// <summary>
        /// Disconnects the relay of the DLL loaded in this process. In-process only;
        /// see <see cref="DisconnectRelayAsync"/>.
        /// </summary>
        public void DisconnectRelay() => InterposerRelayDisconnect();

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
        /// <para>
        /// The path is resolved against the whole document, not just the Plugins
        /// section, so any top-level key the DLL reads is available: for example
        /// "OsVersion.Version" for the selected Windows version preset,
        /// "Player.Username", or "FastDL.BaseUrl". Sequence-valued keys such as
        /// FileRedirects are not scalar and return null.
        /// </para>
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

        /// <summary>
        /// Writes a scalar value back to Config.yml by dot-separated YAML path.
        /// The path must be inside a plugin section (Plugins.&lt;Name&gt;.&lt;Key&gt;...);
        /// missing keys are created. The value is visible to GetConfigString
        /// immediately and is patched into Config.yml in place, preserving comments.
        /// In-process only.
        /// </summary>
        /// <param name="dotPath">Dot-separated path, e.g. "Plugins.MyPlugin.LastServer"</param>
        /// <param name="value">The value to store.</param>
        /// <returns>
        /// False if the path is outside Plugins.&lt;Name&gt;, collides with an existing
        /// value of a different shape, or Config.yml could not be written.
        /// </returns>
        public bool SetConfigString(string dotPath, string value)
        {
            return InterposerSetConfigString(dotPath, value);
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
        // Network adapter enumeration (in-process only)
        // =================================================================

        /// <summary>
        /// Enumerates the machine's network adapters, including each adapter's
        /// allowed/hidden status under the current NetworkAdapters filter. This is
        /// the canonical adapter list shared with the native filtering logic, so the
        /// <see cref="NetworkAdapterInfo.Allowed"/> flag always matches what the
        /// hooks expose to the game. In-process only.
        /// </summary>
        public IReadOnlyList<NetworkAdapterInfo> EnumerateNetworkAdapters()
        {
            uint written;
            uint total = InterposerEnumNetworkAdapters(null, 0, out written);

            if (total == 0)
                return Array.Empty<NetworkAdapterInfo>();

            var buffer = new NativeNetworkAdapter[total];
            uint got = InterposerEnumNetworkAdapters(buffer, total, out written);

            uint count = Math.Min(written, (uint)buffer.Length);
            var result = new NetworkAdapterInfo[count];

            for (uint i = 0; i < count; i++)
            {
                var n = buffer[i];
                result[i] = new NetworkAdapterInfo
                {
                    FriendlyName = n.FriendlyName,
                    Description = n.Description,
                    MacAddress = n.MacAddress,
                    IPv4Address = n.Ipv4Address,
                    IPv6Address = n.Ipv6Address,
                    Allowed = n.Allowed != 0,
                };
            }

            return result;
        }

        // =================================================================
        // Audio device enumeration (in-process only)
        // =================================================================

        /// <summary>
        /// Enumerates the machine's active audio devices, output and input alike,
        /// including each device's allowed/hidden status under the current
        /// AudioDevices filter. This is the canonical device list shared with the
        /// native filtering logic, so the <see cref="AudioDeviceInfo.Allowed"/> flag
        /// always matches what the hooks expose to the game. In-process only.
        /// </summary>
        public IReadOnlyList<AudioDeviceInfo> EnumerateAudioDevices()
        {
            uint written;
            uint total = InterposerEnumAudioDevices(null, 0, out written);

            if (total == 0)
                return Array.Empty<AudioDeviceInfo>();

            var buffer = new NativeAudioDevice[total];
            InterposerEnumAudioDevices(buffer, total, out written);

            uint count = Math.Min(written, (uint)buffer.Length);
            var result = new AudioDeviceInfo[count];

            for (uint i = 0; i < count; i++)
            {
                var n = buffer[i];
                result[i] = new AudioDeviceInfo
                {
                    Name = n.Name,
                    Description = n.Description,
                    Id = n.Id,
                    InstanceId = n.InstanceId,
                    HardwareId = n.HardwareId,
                    Direction = (AudioDeviceDirection)n.Direction,
                    IsDefault = n.IsDefault != 0,
                    Allowed = n.Allowed != 0,
                };
            }

            return result;
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

        // =================================================================
        // Injection - binaries and architectures
        // =================================================================

        private const string InjectorName = "LANCommander.Interposer.Injector.exe";

        private enum Architecture { X86, X64 }

        /// <summary>The DLL to load into a target, and the injector to load it with when the target's architecture is not ours.</summary>
        private sealed record Binaries(Architecture Architecture, string DllPath, string? InjectorPath);

        private static Architecture CurrentArchitecture => Environment.Is64BitProcess ? Architecture.X64 : Architecture.X86;

        private static string FolderName(Architecture architecture) => architecture == Architecture.X64 ? "x64" : "x86";

        private static string Describe(Architecture architecture) => architecture == Architecture.X64 ? "64-bit" : "32-bit";

        private static Binaries ResolveBinaries(string dllPath, Architecture architecture)
        {
            var dll = ResolveDllPath(dllPath, architecture);

            // Only needed, and only looked for, when the target is not our own architecture.
            var injector = architecture == CurrentArchitecture ? null : ResolveInjectorPath(architecture);

            return new Binaries(architecture, dll, injector);
        }

        private static string ResolveDllPath(string dllPath, Architecture architecture)
        {
            if (!string.IsNullOrEmpty(dllPath))
            {
                dllPath = Path.GetFullPath(dllPath);

                if (!File.Exists(dllPath))
                    throw new InjectionException($"DLL not found: {dllPath}");

                return dllPath;
            }

            var candidates = new List<string>(ArchitectureCandidates(architecture, DllName));

            // The layout before the package carried both architectures: a single DLL beside
            // the application, of the application's own architecture.
            if (architecture == CurrentArchitecture)
            {
                foreach (var directory in ApplicationDirectories())
                {
                    candidates.Add(Path.Combine(directory, "interposer.dll"));
                    candidates.Add(Path.Combine(directory, DllName));
                }
            }

            foreach (var candidate in candidates)
            {
                if (File.Exists(candidate))
                    return candidate;
            }

            throw new InjectionException(
                $"No DLL path specified and no {Describe(architecture)} {DllName} was found. Looked in: " +
                string.Join(", ", candidates));
        }

        private static string ResolveInjectorPath(Architecture architecture)
        {
            var candidates = ArchitectureCandidates(architecture, InjectorName);

            foreach (var candidate in candidates)
            {
                if (File.Exists(candidate))
                    return candidate;
            }

            throw new InjectionException(
                $"The target is {Describe(architecture)} and this process is not, so it has to be injected by the " +
                $"{Describe(architecture)} {InjectorName}, which was not found. Looked in: " + string.Join(", ", candidates));
        }

        private static List<string> ArchitectureCandidates(Architecture architecture, string fileName)
        {
            var candidates = new List<string>();

            foreach (var directory in ApplicationDirectories())
            {
                // Where the package's build targets copy them
                candidates.Add(Path.Combine(directory, FolderName(architecture), fileName));
                // The package's own layout, for hosts that copy runtimes\ as is
                candidates.Add(Path.Combine(directory, "runtimes", "win-" + FolderName(architecture), "native", fileName));
            }

            return candidates;
        }

        /// <summary>
        /// Where the application's files are: its base directory, and the directory of its
        /// executable when that differs -- as it does for a single-file app that extracts its
        /// content to a temporary directory, while files kept out of the bundle stay beside it.
        /// </summary>
        private static IEnumerable<string> ApplicationDirectories()
        {
            var baseDirectory = AppContext.BaseDirectory;

            yield return baseDirectory;

            var processDirectory = Path.GetDirectoryName(Environment.ProcessPath);

            if (!string.IsNullOrEmpty(processDirectory)
                && !string.Equals(Path.GetFullPath(processDirectory).TrimEnd('\\'), Path.GetFullPath(baseDirectory).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            {
                yield return processDirectory;
            }
        }

        /// <summary>The architecture of a running process.</summary>
        private static Architecture GetProcessArchitecture(IntPtr hProcess)
        {
            if (!Environment.Is64BitOperatingSystem)
                return Architecture.X86;

            if (!NativeMethods.IsWow64Process(hProcess, out var wow64))
                throw new InjectionException("IsWow64Process failed.", new Win32Exception());

            return wow64 ? Architecture.X86 : Architecture.X64;
        }

        /// <summary>The architecture an executable runs as, read from its PE header.</summary>
        private static Architecture GetImageArchitecture(string exePath)
        {
            const ushort machineI386 = 0x014C;
            const ushort machineAmd64 = 0x8664;

            ushort machine;

            try
            {
                using var stream = File.OpenRead(exePath);
                using var reader = new BinaryReader(stream);

                if (reader.ReadUInt16() != 0x5A4D) // MZ
                    throw new InjectionException($"Not a Windows executable: {exePath}");

                stream.Position = 0x3C;
                stream.Position = reader.ReadInt32();

                if (reader.ReadUInt32() != 0x00004550) // PE\0\0
                    throw new InjectionException($"Not a Windows executable: {exePath}");

                machine = reader.ReadUInt16();
            }
            catch (EndOfStreamException)
            {
                throw new InjectionException($"Not a Windows executable: {exePath}");
            }

            return machine switch
            {
                machineI386 => Architecture.X86,
                machineAmd64 => Architecture.X64,
                _ => throw new InjectionException(
                    $"{Path.GetFileName(exePath)} is built for machine type 0x{machine:X4}; the Interposer is only built for x86 and x64."),
            };
        }

        /// <summary>
        /// Loads the DLL into a process whose overrides and pipes are already set up: directly
        /// when it shares our architecture, through the injector built for its architecture
        /// when it does not.
        /// </summary>
        private static void InjectInto(IntPtr hProcess, uint processId, Binaries binaries)
        {
            if (binaries.InjectorPath == null)
                InjectDll(hProcess, binaries.DllPath);
            else
                InjectWithInjector(processId, binaries);
        }

        private static void InjectWithInjector(uint processId, Binaries binaries)
        {
            var startInfo = new ProcessStartInfo(binaries.InjectorPath!)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };

            // The injector's "inject into a running process" mode: by PID, with no options, so
            // it creates no overrides of its own; ours are already in place.
            startInfo.ArgumentList.Add(processId.ToString(System.Globalization.CultureInfo.InvariantCulture));
            startInfo.ArgumentList.Add(binaries.DllPath);

            using var injector = Process.Start(startInfo)
                ?? throw new InjectionException($"Could not start {binaries.InjectorPath}.");

            var output = injector.StandardOutput.ReadToEndAsync();
            var error = injector.StandardError.ReadToEndAsync();

            // LoadLibraryW runs the DLL's initialisation, which installs every hook; that is
            // quick, but leave room for a slow disk.
            if (!injector.WaitForExit(60_000))
            {
                try { injector.Kill(); } catch (InvalidOperationException) { }

                throw new InjectionException($"The {Describe(binaries.Architecture)} injector did not finish injecting into PID {processId}.");
            }

            if (injector.ExitCode != 0)
            {
                // The injector reports failures on lines starting with [!]
                var reasons = string.Join(" ", (output.Result + error.Result)
                    .Split('\n')
                    .Select(line => line.Trim())
                    .Where(line => line.StartsWith("[!]", StringComparison.Ordinal))
                    .Select(line => line.Substring(3).Trim()));

                if (reasons.Length == 0)
                    reasons = $"exit code {injector.ExitCode}";

                throw new InjectionException(
                    $"The {Describe(binaries.Architecture)} injector could not inject into PID {processId}: {reasons}");
            }
        }

        private void CreateMappings(uint pid)
        {
            if (!string.IsNullOrEmpty(FastDlUrl))
                _fastDlMmf = CreateUtf8Mmf($"Local\\InterposerFastDL_{pid}", FastDlUrl, 2048);

            if (!string.IsNullOrEmpty(Username))
                _usernameMmf = CreateUtf8Mmf($"Local\\InterposerUsername_{pid}", Username, 512);

            if (!string.IsNullOrEmpty(ComputerName))
                _computerNameMmf = CreateUtf8Mmf($"Local\\InterposerComputerName_{pid}", ComputerName, 512);

            // Written even when every option is null: its mere presence tells the DLL
            // a launcher may drive the relay, so the socket hooks go in at startup.
            if (Relay != null)
                _relayMmf = CreateUtf8Mmf($"Local\\InterposerRelay_{pid}", Relay.ToYaml(), 8192);

            // Like the event pipe, listening before injection: the DLL connects once,
            // from DLL_PROCESS_ATTACH.
            _control?.Dispose();
            _control = new ControlChannel(pid);

            // Create a named pipe for the injected DLL to send events back.
            // The pipe must be in a listening state before the DLL is injected
            // so it can connect during DLL_PROCESS_ATTACH.
            var pipeName = $"InterposerEvents_{pid}";
            _eventPipe = new NamedPipeServerStream(
                pipeName,
                PipeDirection.In,
                1,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous);

            _eventCts = new CancellationTokenSource();

            // Start WaitForConnectionAsync BEFORE returning so the pipe is in
            // listening state when InjectDll triggers DllMain → ConnectEventPipe.
            // The task continues reading in the background once a client connects.
            _eventPipeTask = ReadEventPipeAsync(_eventCts.Token);
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

        /// <summary>
        /// Fires when the pipe reader encounters a diagnostic condition (connection, disconnect, error).
        /// Subscribe before calling <see cref="Start"/> to observe pipe lifecycle.
        /// </summary>
        public event EventHandler<string> PipeDiagnostic;

        private async Task ReadEventPipeAsync(CancellationToken ct)
        {
            try
            {
                PipeDiagnostic?.Invoke(this, "Pipe: waiting for connection...");
                await _eventPipe.WaitForConnectionAsync(ct).ConfigureAwait(false);
                PipeDiagnostic?.Invoke(this, "Pipe: client connected.");

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
            catch (OperationCanceledException)
            {
                PipeDiagnostic?.Invoke(this, "Pipe: cancelled.");
            }
            catch (IOException ex)
            {
                PipeDiagnostic?.Invoke(this, $"Pipe: IO error - {ex.Message}");
            }
            catch (ObjectDisposedException)
            {
                PipeDiagnostic?.Invoke(this, "Pipe: disposed.");
            }
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
                case 6: // RELAY
                    DispatchRelayEvent(field1, field2, field3, intField);
                    break;
            }
        }

        // kind is "STATE", "PEER JOIN" or "PEER LEAVE"; see InterposerRelayCallback in callbacks.h.
        private void DispatchRelayEvent(string? kind, string? address, string? detail, int value)
        {
            switch (kind)
            {
                case "STATE":
                    RelayStateChanged?.Invoke(this, new RelayStateChangedEventArgs((RelayState)value, address));
                    break;
                case "PEER JOIN":
                    RelayPeerJoined?.Invoke(this, new RelayPeerEventArgs(unchecked((uint)value), address, detail));
                    break;
                case "PEER LEAVE":
                    RelayPeerLeft?.Invoke(this, new RelayPeerEventArgs(unchecked((uint)value), address, detail));
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

        private void OnRelayCallback(IntPtr kind, IntPtr address, IntPtr detail, int value)
        {
            DispatchRelayEvent(
                Marshal.PtrToStringUni(kind),
                address != IntPtr.Zero ? Marshal.PtrToStringUni(address) : null,
                detail != IntPtr.Zero ? Marshal.PtrToStringUni(detail) : null,
                value);
        }
    }
}
