// LANCommander.Interposer.Tests for Linux.
//
// The same shape as the Windows suite: write .interposer fixtures beside a copy
// of the shared object, dlopen it (which runs its constructor, as LoadLibraryW
// runs DllMain), call the exports, dlclose it, and read back what it wrote.
// Cases that need a process of their own -- launcher values arrive through the
// environment at startup, two copies in one process, LD_PRELOAD itself -- re-run
// this executable in a child mode, as the Windows suite does.
//
// Exit code is the number of failures. A run that fails keeps its temporary
// directory and prints where it is.

#include "LANCommander.Interposer.h"
#include "LANCommander.Interposer.Comms.Server.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ── Harness ───────────────────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool ok, const char* message)
{
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", message);
    fflush(stdout);
    (ok ? g_pass : g_fail)++;
}

#define ASSERT(cond, message) Check((cond), (message))

static std::string ReadText(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

static void WriteText(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

static bool Contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

static int CountLinesContaining(const std::string& text, const char* a, const char* b)
{
    int count = 0;
    std::istringstream lines(text);

    for (std::string line; std::getline(lines, line); )
    {
        if (Contains(line, a) && Contains(line, b))
            ++count;
    }

    return count;
}

// The session log under <dir>/.interposer/Logs, or empty.
static fs::path FindLog(const fs::path& dir)
{
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(dir / ".interposer" / "Logs", ec))
    {
        if (entry.path().extension() == ".log")
            return entry.path();
    }

    return {};
}

static fs::path ExecutablePath()
{
    return fs::read_symlink("/proc/self/exe");
}

// Run this executable with `args`, adding `env` to the environment. Returns the
// child's exit code, or -1 if it did not exit normally.
static int RunChild(const std::vector<std::string>& args, const std::vector<std::string>& env)
{
    const std::string exe = ExecutablePath().string();

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    std::vector<std::string> environment;
    for (char** e = environ; *e; ++e) environment.push_back(*e);
    for (const auto& e : env) environment.push_back(e);

    std::vector<char*> envp;
    for (auto& e : environment) envp.push_back(e.data());
    envp.push_back(nullptr);

    fflush(stdout);
    const pid_t pid = fork();

    if (pid == 0)
    {
        execve(exe.c_str(), argv.data(), envp.data());
        _exit(127);
    }

    int status = 0;

    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
        return -1;

    return WEXITSTATUS(status);
}

using FnGetConfig = BOOL (*)(const wchar_t*, wchar_t*, DWORD);
using FnSetConfig = BOOL (*)(const wchar_t*, const wchar_t*);
using FnGetName   = BOOL (*)(wchar_t*, DWORD);
using FnLog       = void (*)(const wchar_t*, const wchar_t*);

template <typename Fn>
static Fn Resolve(void* module, const char* name)
{
    return reinterpret_cast<Fn>(dlsym(module, name));
}

// A voice server on loopback, an ephemeral port, through the given copy's
// exports. Starting one logs "voice server listening on port".
static void* StartVoiceServer(void* module)
{
    auto init   = Resolve<decltype(&interposer_comms_server_config_init)>(module, "interposer_comms_server_config_init");
    auto create = Resolve<decltype(&interposer_comms_server_create)>(module, "interposer_comms_server_create");
    auto start  = Resolve<decltype(&interposer_comms_server_start)>(module, "interposer_comms_server_start");
    auto destroy = Resolve<decltype(&interposer_comms_server_destroy)>(module, "interposer_comms_server_destroy");

    if (!init || !create || !start || !destroy)
        return nullptr;

    interposer_comms_server_config config;
    init(&config);
    config.port         = 0;
    config.bind_address = "127.0.0.1";

    interposer_comms_server* server = nullptr;

    if (create(&config, &server) != INTERPOSER_COMMS_OK)
        return nullptr;

    if (start(server) != INTERPOSER_COMMS_OK)
    {
        destroy(server);
        return nullptr;
    }

    return server;
}

static void StopVoiceServer(void* module, void* server)
{
    auto destroy = Resolve<decltype(&interposer_comms_server_destroy)>(module, "interposer_comms_server_destroy");

    if (destroy && server)
        destroy(static_cast<interposer_comms_server*>(server));
}

// An interposer_comms_log_fn that counts the server's "listening" line.
static void CountVoiceListening(interposer_comms_log_level, const char* message, void* userData)
{
    if (strstr(message, "voice server listening"))
        ++*static_cast<int*>(userData);
}

static std::wstring GetConfig(FnGetConfig get, const wchar_t* path)
{
    wchar_t buffer[512]{};
    return get && get(path, buffer, 512) ? std::wstring(buffer) : std::wstring(L"<missing>");
}

static std::wstring GetName(FnGetName get)
{
    wchar_t buffer[256]{};
    return get && get(buffer, 256) ? std::wstring(buffer) : std::wstring(L"<missing>");
}

static const char* kSo = "libLANCommander.Interposer.so";

// A directory holding a copy of the shared object, as a game directory would.
static fs::path StageCopy(const fs::path& dir)
{
    fs::create_directories(dir / ".interposer");
    fs::copy_file(ExecutablePath().parent_path() / kSo, dir / kSo, fs::copy_options::overwrite_existing);
    return dir / kSo;
}

// ── Exports ───────────────────────────────────────────────────────────────────

// Every function the public header declares, so a patch links on Linux too.
static const char* const kHeaderExports[] = {
    "InterposerGetUsername", "InterposerLog", "InterposerGetConfigString",
    "InterposerRegisterPluginConfig", "InterposerSetConfigString",
    "InterposerSetRegistryValue", "InterposerSetRegistryValueBySuffix",
    "InterposerRegisterFileCallback", "InterposerRegisterRegistryCallback",
    "InterposerRegisterDnsCallback", "InterposerRegisterNetworkCallback",
    "InterposerRegisterIdentityCallback", "InterposerRegisterRelayCallback",
    "InterposerEnumNetworkAdapters", "InterposerEnumAudioDevices",
    "InterposerRegisterMouseTransform",
    "InterposerRelayGetStatus", "InterposerRelayEnumPeers",
    "InterposerRelayConnect", "InterposerRelayDisconnect",
    "InterposerSetPresenceDetails", "InterposerSetPresenceState",
    "InterposerSetPresenceTimestamps", "InterposerSetPresenceLargeImage",
    "InterposerSetPresenceSmallImage", "InterposerSetPresenceParty",
    "InterposerSetPresenceButton", "InterposerSetPresenceType",
    "InterposerSetPresenceName", "InterposerUpdatePresence", "InterposerClearPresence",
    "InterposerMatchStart", "InterposerMatchPlayerJoined", "InterposerMatchPlayerLeft",
    "InterposerMatchEvent", "InterposerMatchSetStatistic", "InterposerMatchAddStatistic",
    "InterposerMatchSetResult", "InterposerMatchEnd", "InterposerMatchAttachFile",
    "InterposerMatchFlush", "InterposerMatchGetStatus", "InterposerRegisterMatchCallback",
};

static void RunExportTests(void* so)
{
    printf("\n--- Export Tests ---\n");

    std::string missing;

    for (const char* name : kHeaderExports)
    {
        if (!dlsym(so, name))
            missing += std::string(" ") + name;
    }

    if (!missing.empty())
        printf("        missing:%s\n", missing.c_str());

    ASSERT(missing.empty(), "E-01: every function in the public header is exported");

    // The version script: nothing of ours, of yaml-cpp, or of a std:: template
    // our code instantiates leaks into the dynamic symbol table, where it could
    // interpose on the game's own.
    ASSERT(!dlsym(so, "_ZN4YAML4LoadERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE") &&
           !dlsym(so, "_ZNSt8__detail8_ScannerIwE10_M_advanceEv") &&
           !dlsym(so, "_Z10LoadConfigv"),
        "E-02: internal, statically linked and std:: template symbols are not exported");

    InterposerRelayStatus status{};
    status.size = sizeof(status);
    auto getStatus = Resolve<BOOL (*)(InterposerRelayStatus*)>(so, "InterposerRelayGetStatus");

    ASSERT(getStatus && getStatus(&status) && status.state == 0,
        "E-03: an API without a Linux implementation answers as though switched off");
}

// ── Plugin config ─────────────────────────────────────────────────────────────

static void RunPluginConfigTests(void* so)
{
    printf("\n--- Plugin Config Tests ---\n");

    auto get = Resolve<FnGetConfig>(so, "InterposerGetConfigString");
    auto set = Resolve<FnSetConfig>(so, "InterposerSetConfigString");

    ASSERT(get && set, "C-01: InterposerGetConfigString and InterposerSetConfigString are exported");
    if (!get || !set) return;

    ASSERT(GetConfig(get, L"Plugins.Tester.Existing") == L"old" &&
           GetConfig(get, L"Plugins.Tester.Existing") == L"old" &&
           GetConfig(get, L"Player.Username") == L"TestPlayer",
        "C-02: repeated reads leave the config tree intact");

    ASSERT(set(L"Plugins.Tester.Existing", L"new") && GetConfig(get, L"Plugins.Tester.Existing") == L"new",
        "C-03: overwriting an existing value is visible to the next read");

    ASSERT(set(L"Plugins.Tester.Added.Leaf", L"leaf") && GetConfig(get, L"Plugins.Tester.Added.Leaf") == L"leaf",
        "C-04: a missing key and its intermediate map are created");

    ASSERT(set(L"Plugins.Tester.Nulled", L"filled") && GetConfig(get, L"Plugins.Tester.Nulled") == L"filled",
        "C-05: a key with a null value takes a scalar");

    ASSERT(set(L"Plugins.Tester.Empty", L"") && GetConfig(get, L"Plugins.Tester.Empty").empty(),
        "C-06: an empty string round-trips as empty, not null");

    ASSERT(!set(L"Player.Username", L"Intruder") && GetConfig(get, L"Player.Username") == L"TestPlayer",
        "C-07: a write outside Plugins is refused and changes nothing");

    ASSERT(!set(L"Plugins.Tester", L"x"), "C-08: a write to a whole plugin section is refused");

    ASSERT(!set(L"Plugins.Tester.Nested", L"x") && GetConfig(get, L"Plugins.Tester.Nested.Deep") == L"1",
        "C-09: a scalar is not written over an existing map");

    ASSERT(!set(L"Plugins.Tester.Existing.Sub", L"x") && GetConfig(get, L"Plugins.Tester.Existing") == L"new",
        "C-10: a map is not written over an existing scalar");

    ASSERT(set(L"Plugins.Flow.B", L"2") &&
           GetConfig(get, L"Plugins.Flow.A") == L"1" && GetConfig(get, L"Plugins.Flow.B") == L"2",
        "C-11: a write into a flow-style section succeeds and keeps its siblings");

    ASSERT(set(L"Plugins.NewPlugin.Key", L"true") && GetConfig(get, L"Plugins.NewPlugin.Key") == L"true",
        "C-12: a write creates a plugin section that did not exist");

    // wchar_t is UTF-32 here; the file and the YAML tree are UTF-8.
    ASSERT(set(L"Plugins.Tester.Unicode", L"café \U0001F3AE") &&
           GetConfig(get, L"Plugins.Tester.Unicode") == L"café \U0001F3AE",
        "C-13: text outside the BMP survives the UTF-32 -> UTF-8 -> UTF-32 round trip");

    wchar_t tiny[4]{};
    ASSERT(!get(L"Player.Username", tiny, 4), "C-14: a buffer too small for the value is refused");
}

static void RunPluginConfigPersistenceTests(const fs::path& yamlPath)
{
    printf("\n--- Plugin Config Persistence Tests ---\n");

    const std::string yml = ReadText(yamlPath);
    auto has = [&](const char* s) { return Contains(yml, s); };

    ASSERT(has("    Existing: new   # kept across the write\n"),
        "CP-01: an overwritten value is patched in place, keeping its comment");
    ASSERT(has("    Nulled: filled\n"), "CP-02: a null value is filled in on its own line");
    ASSERT(has("    Added:\n      Leaf: leaf\n"), "CP-03: a created key is written as a nested block");
    ASSERT(has("    Empty: ''\n"), "CP-04: an empty string is written quoted");
    ASSERT(has("  Flow:\n    A: 1\n    B: 2\n") && !has("{A:"), "CP-05: the flow-style section is re-rendered as a block");
    ASSERT(has("  NewPlugin:\n    Key: true\n"), "CP-06: a new plugin section is appended under Plugins");
    ASSERT(has("# trailing comment\n") && has("  Username: TestPlayer\n"),
        "CP-07: content outside the written keys is untouched");
    ASSERT(has("caf\xc3\xa9 \xf0\x9f\x8e\xae"), "CP-08: non-ASCII text is written to Config.yml as UTF-8");
}

// ── SDK ───────────────────────────────────────────────────────────────────────

static const wchar_t* kSdkVerb    = L"SDK PROBE";
static const wchar_t* kSdkMessage = L"logged through the linked probe";

static void RunSdkTests(void* so)
{
    printf("\n--- SDK Tests (linked by soname) ---\n");

    void* probe = dlopen((ExecutablePath().parent_path() / "libLANCommander.Interposer.SdkProbe.so").c_str(),
        RTLD_NOW | RTLD_LOCAL);

    ASSERT(probe != nullptr, "SDK-00: the probe loads");
    if (!probe)
    {
        printf("        %s\n", dlerror());
        return;
    }

    auto getConfig = Resolve<BOOL (*)(const wchar_t*, wchar_t*, DWORD)>(probe, "SdkGetConfigString");
    auto address   = Resolve<void* (*)()>(probe, "SdkAddressOfGetConfigString");
    auto username  = Resolve<BOOL (*)(wchar_t*, DWORD)>(probe, "SdkGetUsername");
    auto log       = Resolve<void (*)(const wchar_t*, const wchar_t*)>(probe, "SdkLog");
    auto registry  = Resolve<DWORD (*)(const wchar_t*, const wchar_t*, const wchar_t*)>(probe, "SdkSetRegistryValueBySuffix");

    wchar_t buffer[256]{};

    ASSERT(getConfig && getConfig(L"Player.Username", buffer, 256) && wcscmp(buffer, L"TestPlayer") == 0,
        "SDK-01: InterposerGetConfigString through the linked probe reads the fixture");

    // The probe's DT_NEEDED names the soname, and the copy already loaded
    // carries it, so the dynamic linker must reuse that copy.
    ASSERT(address && address() == dlsym(so, "InterposerGetConfigString"),
        "SDK-02: the link binds to the Interposer already loaded");

    buffer[0] = L'\0';
    ASSERT(username && username(buffer, 256) && wcscmp(buffer, L"TestPlayer") == 0,
        "SDK-03: InterposerGetUsername through the linked probe");

    ASSERT(registry && registry(L"Any\\Key", L"Name", L"Value") == 0,
        "SDK-04: a Windows-only export links and reports nothing done");

    if (log)
        log(kSdkVerb, kSdkMessage);

    // Its reference on the Interposer goes with it, so the dlclose that
    // follows really unloads.
    dlclose(probe);
}

// ── Plugins ───────────────────────────────────────────────────────────────────

static void RunPluginTests(const fs::path& dir)
{
    printf("\n--- Plugin Tests ---\n");

    const std::string log = ReadText(FindLog(dir));

    ASSERT(CountLinesContaining(log, "[PLUGIN LOAD]", "Plugins/libLANCommander.Interposer.TestPlugin.so") == 1,
        "PL-01: a plugin in .interposer/Plugins is loaded once");

    // Two copies of the plugin are installed (see PL-03), so two inits.
    ASSERT(CountLinesContaining(log, "[TESTPLUGIN]", "initialised through the handle") == 2,
        "PL-02: InterposerPluginInit receives a handle the plugin can resolve the API through");

    // Nested/ holds a .txt and a subdirectory with a second copy of the plugin.
    ASSERT(CountLinesContaining(log, "[PLUGIN LOAD]", "Nested/libLANCommander.Interposer.TestPlugin.so") == 1 &&
           !Contains(log, "notaplugin"),
        "PL-03: subdirectories are searched and only .so files are loaded");
}

// ── Launcher values ───────────────────────────────────────────────────────────
//
// On Linux the launcher cannot name the process before it exists, so its values
// arrive in the environment: INTERPOSER_BASE_DIRECTORY, INTERPOSER_CONFIG.

static int RunLauncherConfigChild(const fs::path& soPath, const fs::path& baseDir, const std::string& mode)
{
    const bool merge = mode == "merge";

    printf(merge ? "\n--- Launcher Config Tests: overlay over Config.yml (child process) ---\n"
                 : "\n--- Launcher Config Tests: overlay only (child process) ---\n");

    void* so = dlopen(soPath.c_str(), RTLD_NOW | RTLD_LOCAL);

    if (!so)
    {
        printf("  FAIL  LC-00: child could not load the shared object: %s\n", dlerror());
        return 1;
    }

    auto get  = Resolve<FnGetConfig>(so, "InterposerGetConfigString");
    auto set  = Resolve<FnSetConfig>(so, "InterposerSetConfigString");
    auto user = Resolve<FnGetName>(so, "InterposerGetUsername");

    if (merge)
    {
        ASSERT(GetName(user) == L"OverlayUser", "LC-01: a key the overlay sets wins over Config.yml");

        ASSERT(GetConfig(get, L"Player.ComputerName") == L"FileComputer",
            "LC-02: Config.yml comes from the base directory and keys the overlay omits survive");

        ASSERT(GetConfig(get, L"OsVersion.Version") == L"<missing>",
            "LC-03: a null in the overlay removes the section Config.yml set");

        ASSERT(GetConfig(get, L"Plugins.Probe.Shared") == L"overlay" &&
               GetConfig(get, L"Plugins.Probe.FromFile") == L"file" &&
               GetConfig(get, L"Plugins.Probe.FromOverlay") == L"overlay",
            "LC-04: maps merge key by key, all the way down");

        ASSERT(set && set(L"Plugins.Probe.Written", L"yes") && GetConfig(get, L"Plugins.Probe.Written") == L"yes",
            "LC-05: a plugin write succeeds under an overlay");
    }
    else
    {
        ASSERT(GetName(user) == L"OnlyOverlay", "LO-01: the overlay applies with no Config.yml at all");

        ASSERT(GetConfig(get, L"Player.ComputerName") != L"DecoyComputer",
            "LO-02: the Config.yml beside the shared object is not read");

        ASSERT(GetConfig(get, L"Plugins.Probe.Key") == L"value", "LO-03: the overlay is what config reads see");
    }

    dlclose(so);

    const std::string log = ReadText(FindLog(baseDir));

    if (merge)
    {
        ASSERT(Contains(log, "applied the launcher's config overlay") && FindLog(soPath.parent_path()).empty(),
            "LC-09: the session log is written under the base directory and records the overlay");

        const std::string yml = ReadText(baseDir / ".interposer" / "Config.yml");

        ASSERT(Contains(yml, "Written: yes") && Contains(yml, "FromFile: file") &&
               !Contains(yml, "overlay") && !Contains(yml, "OverlayUser"),
            "LC-10: a plugin write reaches Config.yml without baking the overlay into it");
    }
    else
    {
        ASSERT(Contains(log, "no Config.yml"),
            "LO-04: the base directory is created, and the log there records the overlay");
    }

    return g_fail;
}

static void RunLauncherConfigTests(const fs::path& tmp)
{
    for (const std::string mode : { "merge", "overlay-only" })
    {
        const bool merge = mode == "merge";

        const fs::path childDir = tmp / "LauncherConfig" / mode;
        const fs::path soPath   = StageCopy(childDir / "Lib");

        // The overlay-only base directory is left uncreated, two levels deep.
        const fs::path baseDir = merge ? childDir / "Base" : childDir / "Base" / "Nested" / "Deeper";

        // The decoy: read only if the base directory override were ignored.
        WriteText(childDir / "Lib" / ".interposer" / "Config.yml", "Player:\n  ComputerName: DecoyComputer\n");

        if (merge)
        {
            WriteText(baseDir / ".interposer" / "Config.yml",
                "Player:\n"
                "  Username: FileUser\n"
                "  ComputerName: FileComputer\n"
                "\n"
                "OsVersion:\n"
                "  Version: 'Windows XP'\n"
                "\n"
                "Plugins:\n"
                "  Probe: { FromFile: file, Shared: file }\n");
        }

        const std::string overlay = merge
            ? "Player:\n  Username: OverlayUser\nOsVersion: ~\nPlugins:\n  Probe:\n    Shared: overlay\n    FromOverlay: overlay\n"
            : "Player:\n  Username: OnlyOverlay\nPlugins:\n  Probe:\n    Key: value\n";

        const int code = RunChild({ "--launcher-config", soPath.string(), baseDir.string(), mode },
            { "INTERPOSER_BASE_DIRECTORY=" + baseDir.string(), "INTERPOSER_CONFIG=" + overlay });

        ASSERT(code == 0, merge ? "LC-00: the overlay-over-Config.yml child reported no failures"
                                : "LO-00: the overlay-only child reported no failures");
    }
}

// ── Second instance ───────────────────────────────────────────────────────────

static int RunSecondInstanceChild(const fs::path& primaryDir, const fs::path& secondaryDir)
{
    printf("\n--- Second Instance Tests (child process) ---\n");

    void* primary   = dlopen((primaryDir / kSo).c_str(), RTLD_NOW | RTLD_LOCAL);
    void* secondary = primary ? dlopen((secondaryDir / kSo).c_str(), RTLD_NOW | RTLD_LOCAL) : nullptr;

    if (!primary || !secondary || primary == secondary)
    {
        printf("  FAIL  MI-00: child could not load both copies: %s\n", dlerror());
        return 1;
    }

    auto get = Resolve<FnGetConfig>(secondary, "InterposerGetConfigString");
    auto log = Resolve<FnLog>(secondary, "InterposerLog");

    ASSERT(GetConfig(get, L"Plugins.Probe.Owner") == L"primary",
        "MI-01: the second copy's exports answer from the first copy's configuration");

    ASSERT(!fs::exists(secondaryDir / ".interposer" / "Logs"),
        "MI-02: the second copy loads no configuration and opens no session log");

    if (log)
        log(L"INSTANCE", L"forwarded by the second copy");

    ASSERT(CountLinesContaining(ReadText(FindLog(primaryDir)), "[INSTANCE]", "forwarded by the second copy") == 1,
        "MI-04: InterposerLog through the second copy writes to the first copy's log");

    // Voice forwards like the rest of the API. A handler set through the second
    // copy is only heard by a server started through the first if the handler
    // was forwarded; a server started through the second copy is only in the
    // first copy's log if the server functions were.
    {
        int heard = 0;
        auto setHandler = Resolve<decltype(&interposer_comms_set_log_handler)>(secondary, "interposer_comms_set_log_handler");
        auto setSession = Resolve<decltype(&interposer_comms_set_session_logging)>(secondary, "interposer_comms_set_session_logging");

        // Through the second copy too, so MI-08 also proves it forwards: the
        // second copy has no session log of its own to write to.
        if (setSession)
            setSession(INTERPOSER_COMMS_LOG_INFO);

        if (setHandler)
            setHandler(CountVoiceListening, INTERPOSER_COMMS_LOG_INFO, &heard);

        StopVoiceServer(primary, StartVoiceServer(primary));

        ASSERT(heard == 1, "MI-07: a voice log handler set through the second copy hears the first copy's server");

        if (setHandler)
            setHandler(nullptr, INTERPOSER_COMMS_LOG_OFF, nullptr);

        StopVoiceServer(secondary, StartVoiceServer(secondary));

        ASSERT(CountLinesContaining(ReadText(FindLog(primaryDir)), "[VOICE]", "voice server listening") == 2,
            "MI-08: voice servers started through either copy run in the first copy and log to its session log");
    }

    dlclose(secondary);
    dlclose(primary);

    // RTLD_NODELETE from the second copy's pin outlives both handles.
    ASSERT(dlopen((primaryDir / kSo).c_str(), RTLD_NOW | RTLD_NOLOAD) != nullptr,
        "MI-06: the first copy stays loaded once a second copy has pinned it");

    return g_fail;
}

static void RunSecondInstanceTests(const fs::path& tmp)
{
    printf("\n--- Second Instance Tests ---\n");

    const fs::path primaryDir   = tmp / "SecondInstance" / "Primary";
    const fs::path secondaryDir = tmp / "SecondInstance" / "Secondary";

    StageCopy(primaryDir);
    StageCopy(secondaryDir);

    WriteText(primaryDir / ".interposer" / "Config.yml",   "Plugins:\n  Probe:\n    Owner: primary\n");
    WriteText(secondaryDir / ".interposer" / "Config.yml", "Plugins:\n  Probe:\n    Owner: secondary\n");

    ASSERT(RunChild({ "--second-instance", primaryDir.string(), secondaryDir.string() }, {}) == 0,
        "MI-00: the second-instance child reported no failures");
}

// ── LD_PRELOAD ────────────────────────────────────────────────────────────────

// In the child, LD_PRELOAD has already loaded the Interposer before main().
static int RunPreloadChild(const std::string& expectedUser)
{
    auto user = reinterpret_cast<FnGetName>(dlsym(RTLD_DEFAULT, "InterposerGetUsername"));

    // Report through the exit code only: the parent judges.
    if (!user)
        return 2;

    return GetName(user) == std::wstring(expectedUser.begin(), expectedUser.end()) ? 0 : 3;
}

static void RunPreloadTests(const fs::path& tmp)
{
    printf("\n--- LD_PRELOAD Tests ---\n");

    const std::string exeName = ExecutablePath().filename().string();

    struct Case { const char* name; std::string target; bool expectLog; const char* message; };

    const Case cases[] = {
        { "Any",      "",                         true,  "P-01: a preloaded copy configures itself before main() and its exports resolve" },
        { "Target",   "bash," + exeName,          true,  "P-02: INTERPOSER_TARGET naming this executable among others activates it" },
        { "Elsewhere", "some-other-game",         false, "P-03: INTERPOSER_TARGET naming another executable leaves this process untouched" },
    };

    for (const Case& c : cases)
    {
        const fs::path dir    = tmp / "Preload" / c.name;
        const fs::path soPath = StageCopy(dir);

        WriteText(dir / ".interposer" / "Config.yml", "Player:\n  Username: PreloadUser\n");

        std::vector<std::string> env = { "LD_PRELOAD=" + soPath.string() };

        if (!c.target.empty())
            env.push_back("INTERPOSER_TARGET=" + c.target);

        // A dormant copy has no configuration, so its username is the account's.
        const char* account = getenv("USER");
        const std::string expected = c.expectLog ? "PreloadUser" : (account ? account : "root");

        const int code = RunChild({ "--preload-child", expected }, env);

        ASSERT(code == 0 && FindLog(dir).empty() != c.expectLog, c.message);
    }
}

// ── File hooks ────────────────────────────────────────────────────────────────
//
// The child runs with the shared object preloaded, so its own fopen/open/stat
// interpose; it performs the operations. The parent is not preloaded, so its
// stat is unhooked and it can check the real filesystem outcome.

static std::string ReadFd(const std::string& path)
{
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return {};

    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    close(fd);
    return out;
}

static int RunFileHookChild(const fs::path& dir)
{
    printf("\n--- File Hook Tests (child process) ---\n");

    const std::string probe = (dir / "redirect_probe.txt").string();

    // A write to the probe path lands at the redirect target.
    if (FILE* f = fopen(probe.c_str(), "w"))
    {
        fputs("hello", f);
        ASSERT(fclose(f) == 0, "F-01: fopen of a redirected path for write succeeds");
    }
    else
    {
        ASSERT(false, "F-01: fopen of a redirected path for write succeeds");
    }

    // A read of the same path is redirected to the same target.
    ASSERT(ReadFd(probe) == "hello", "F-02: open of a redirected path for read follows the redirect");

    // A read of a missing game file is served from the FastDL overlay.
    ASSERT(ReadFd((dir / "maps" / "test.bsp").string()) == "OVERLAY",
        "F-03: a missing file is served from the FastDL download overlay");

    // stat of the redirected path reaches the target, which exists.
    struct stat st{};
    ASSERT(stat(probe.c_str(), &st) == 0, "F-04: stat of a redirected path reaches the target");

    return g_fail;
}

static void RunFileHookTests(const fs::path& tmp)
{
    printf("\n--- File Hook Tests ---\n");

    const fs::path dir    = tmp / "FileHooks";
    const fs::path soPath = StageCopy(dir);

    WriteText(dir / ".interposer" / "Config.yml",
        "Logging:\n"
        "  Files: true\n"
        "\n"
        "FileRedirects:\n"
        "  - Pattern: '.*/redirect_probe\\.txt$'\n"
        "    Replacement: '" + (dir / "redirected.txt").string() + "'\n"
        "\n"
        "FastDL:\n"
        "  Enabled: true\n"
        "  ProbeConnections: false\n"
        "  UseDownloadDirectory: true\n"
        "  Paths:\n"
        "    - Local: '" + (dir / "maps").string() + "'\n"
        "      Remote: maps\n");

    // The overlay copy the FastDL read is served from (no server, offline).
    WriteText(dir / ".interposer" / "Downloads" / "maps" / "test.bsp", "OVERLAY");

    const int code = RunChild({ "--file-hook-child", dir.string() },
        { "LD_PRELOAD=" + soPath.string(),
          "INTERPOSER_TARGET=" + ExecutablePath().filename().string(),
          "INTERPOSER_BASE_DIRECTORY=" + dir.string() });

    ASSERT(code == 0, "F-00: the file-hook child reported no failures");

    // Checked here, in a process whose stat is not hooked.
    ASSERT(ReadText(dir / "redirected.txt") == "hello" && !fs::exists(dir / "redirect_probe.txt"),
        "F-05: the write reached the redirect target, not the original path");
}

// ── Adapter enumeration ───────────────────────────────────────────────────────

using FnEnumAdapters = DWORD (*)(InterposerNetworkAdapter*, DWORD, DWORD*);

static void RunAdapterEnumTests(void* so)
{
    printf("\n--- Adapter Enumeration Tests ---\n");

    auto enumerate = Resolve<FnEnumAdapters>(so, "InterposerEnumNetworkAdapters");
    ASSERT(enumerate != nullptr, "N-01: InterposerEnumNetworkAdapters is exported");
    if (!enumerate) return;

    DWORD total = enumerate(nullptr, 0, nullptr);
    ASSERT(total >= 1, "N-02: at least one adapter is reported (loopback)");

    std::vector<InterposerNetworkAdapter> adapters(total);
    DWORD written = 0;
    enumerate(adapters.data(), total, &written);

    bool loopbackAllowed = false;
    for (DWORD i = 0; i < written; ++i)
    {
        if (std::wcscmp(adapters[i].friendlyName, L"lo") == 0 && adapters[i].allowed)
            loopbackAllowed = true;
    }

    // The main fixture sets no NetworkAdapters filter, so every adapter is allowed.
    ASSERT(loopbackAllowed, "N-03: with no filter, loopback is present and allowed");
}

// ── Identity, DNS, adapter filter (preloaded) ──────────────────────────────────

static int RunNetChild()
{
    printf("\n--- Network Tests (child process) ---\n");

    // Computer-name override through both interposers.
    char host[256]{};
    ASSERT(gethostname(host, sizeof(host)) == 0 && std::string(host) == "NetBox",
        "ID-01: gethostname returns the configured computer name");

    struct utsname u{};
    ASSERT(uname(&u) == 0 && std::string(u.nodename) == "NetBox",
        "ID-02: uname's nodename returns the configured computer name");

    // A DNS redirect: redirect.invalid never resolves, 127.0.0.1 always does,
    // so a success means the rule rewrote it.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    const int rc = getaddrinfo("redirect.invalid", nullptr, &hints, &res);
    ASSERT(rc == 0, "DNS-01: getaddrinfo of a redirected host resolves via the replacement");
    if (res) freeaddrinfo(res);

    // The adapter filter names only loopback, so lo is allowed and anything else
    // is hidden.
    auto enumerate = reinterpret_cast<FnEnumAdapters>(dlsym(RTLD_DEFAULT, "InterposerEnumNetworkAdapters"));
    DWORD total = enumerate ? enumerate(nullptr, 0, nullptr) : 0;
    std::vector<InterposerNetworkAdapter> adapters(total);
    DWORD written = 0;
    if (enumerate) enumerate(adapters.data(), total, &written);

    bool loAllowed = false, othersHidden = true;
    for (DWORD i = 0; i < written; ++i)
    {
        const bool isLo = std::wcscmp(adapters[i].friendlyName, L"lo") == 0;
        if (isLo && adapters[i].allowed) loAllowed = true;
        if (!isLo && adapters[i].allowed) othersHidden = false;
    }

    ASSERT(loAllowed && othersHidden,
        "NA-01: the NetworkAdapters filter keeps the named adapter and hides the rest");

    // The game's own getifaddrs is filtered: only the kept interface(s) remain.
    ifaddrs* ifaces = nullptr;
    bool sawLo = false, sawOther = false;
    if (getifaddrs(&ifaces) == 0)
    {
        for (ifaddrs* ifa = ifaces; ifa; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_name && std::strcmp(ifa->ifa_name, "lo") == 0) sawLo = true;
            else                                                        sawOther = true;
        }
        freeifaddrs(ifaces);
    }
    ASSERT(sawLo && !sawOther, "NA-02: the game's own getifaddrs returns only the kept adapter");

    return g_fail;
}

static void RunNetTests(const fs::path& tmp)
{
    printf("\n--- Network Tests ---\n");

    const fs::path dir    = tmp / "Net";
    const fs::path soPath = StageCopy(dir);

    WriteText(dir / ".interposer" / "Config.yml",
        "Logging:\n"
        "  Identity: true\n"
        "  DnsRedirects: true\n"
        "\n"
        "Player:\n"
        "  ComputerName: NetBox\n"
        "\n"
        "DnsRedirects:\n"
        "  - Pattern: '^redirect\\.invalid$'\n"
        "    Replacement: '127.0.0.1'\n"
        "\n"
        "NetworkAdapters:\n"
        "  Enabled: true\n"
        "  Names: ['^lo$']\n");

    const int code = RunChild({ "--net-child" },
        { "LD_PRELOAD=" + soPath.string(),
          "INTERPOSER_TARGET=" + ExecutablePath().filename().string(),
          "INTERPOSER_BASE_DIRECTORY=" + dir.string() });

    ASSERT(code == 0, "N-00: the network child reported no failures");

    const std::string log = ReadText(FindLog(dir));
    ASSERT(Contains(log, "[DNS REDIRECT]") && Contains(log, "redirect.invalid"),
        "DNS-02: the redirect is recorded in the session log");
    ASSERT(Contains(log, "[IDENTITY]") && Contains(log, "NetBox"),
        "ID-03: the computer-name override is recorded in the session log");
}

// ── Voice (Comms) ─────────────────────────────────────────────────────────────

static void RunCommsTests(void* so)
{
    printf("\n--- Voice (Comms) Tests ---\n");

    // The voice API, exported beside Interposer* under the interposer_comms_ namespace.
    auto apiVersion    = reinterpret_cast<uint32_t (*)()>(dlsym(so, "interposer_comms_get_api_version"));
    auto versionString = reinterpret_cast<const char* (*)()>(dlsym(so, "interposer_comms_get_version_string"));

    ASSERT(apiVersion && apiVersion() == 1,
        "VC-01: interposer_comms_get_api_version is re-exported and returns the API version");

    ASSERT(versionString && versionString() && versionString()[0] != '\0',
        "VC-02: interposer_comms_get_version_string is re-exported");

    // A representative server and client entry point from each half.
    ASSERT(dlsym(so, "interposer_comms_server_create") && dlsym(so, "interposer_comms_client_create"),
        "VC-03: the server and client voice API are both present");

    // The original comms_ namespace must be gone -- the rebrand is complete.
    ASSERT(!dlsym(so, "comms_get_api_version") && !dlsym(so, "comms_server_create"),
        "VC-04: the un-prefixed comms_ names are not exported");

    // Checked in the session log after dlclose. The first server starts before
    // session logging is on, and must not be in it.
    StopVoiceServer(so, StartVoiceServer(so));

    auto setSession = Resolve<decltype(&interposer_comms_set_session_logging)>(so, "interposer_comms_set_session_logging");

    if (setSession)
        setSession(INTERPOSER_COMMS_LOG_INFO);

    void* server = StartVoiceServer(so);
    ASSERT(server != nullptr, "VC-05: a voice server starts on loopback");
    StopVoiceServer(so, server);

    if (setSession)
        setSession(INTERPOSER_COMMS_LOG_OFF);
}

// ── Main suite ────────────────────────────────────────────────────────────────

static void WriteFixture(const fs::path& dir)
{
    WriteText(dir / ".interposer" / "Config.yml",
        "Logging:\n"
        "  Plugins: true\n"
        "\n"
        "Player:\n"
        "  Username: TestPlayer\n"
        "  ComputerName: TestMachine\n"
        "\n"
        "Plugins:\n"
        "  Tester:\n"
        "    Existing: 'old'   # kept across the write\n"
        "    Nulled:\n"
        "    Nested:\n"
        "      Deep: 1\n"
        "  Flow: {A: '1'}\n"
        "# trailing comment\n");

    const fs::path plugin  = ExecutablePath().parent_path() / "libLANCommander.Interposer.TestPlugin.so";
    const fs::path plugins = dir / ".interposer" / "Plugins";

    fs::create_directories(plugins / "Nested" / "Deeper");
    fs::copy_file(plugin, plugins / plugin.filename(), fs::copy_options::overwrite_existing);
    fs::copy_file(plugin, plugins / "Nested" / plugin.filename(), fs::copy_options::overwrite_existing);
    WriteText(plugins / "Nested" / "notaplugin.txt", "not a shared object\n");
}

// ── Match statistics (MS-) ────────────────────────────────────────────────────
//
// The Windows suite's MS- tests, on the same plan: in process the default
// fixture has no MatchStatistics section, so a match only reaches the
// callback; the HTTP sink runs in child processes (--match-http <dir> <phase>)
// against a fake LANCommander server on loopback, run by a thread here.

static std::vector<std::tuple<std::wstring, DWORD, std::wstring>> g_matchEvents;

static void OnMatchEvent(const wchar_t* kind, DWORD match, const wchar_t* json)
{
    g_matchEvents.emplace_back(kind ? kind : L"", match, json ? json : L"<null>");
}

static bool WContains(const std::wstring& text, const wchar_t* needle)
{
    return text.find(needle) != std::wstring::npos;
}

struct MatchApi
{
    decltype(&InterposerMatchStart)            start = nullptr;
    decltype(&InterposerMatchPlayerJoined)     joined = nullptr;
    decltype(&InterposerMatchEvent)            event = nullptr;
    decltype(&InterposerMatchAddStatistic)     addStat = nullptr;
    decltype(&InterposerMatchSetStatistic)     setStat = nullptr;
    decltype(&InterposerMatchSetResult)        setResult = nullptr;
    decltype(&InterposerMatchEnd)              end = nullptr;
    decltype(&InterposerMatchAttachFile)       attach = nullptr;
    decltype(&InterposerMatchFlush)            flush = nullptr;
    decltype(&InterposerMatchGetStatus)        status = nullptr;
    decltype(&InterposerRegisterMatchCallback) registerCb = nullptr;

    bool Load(void* so)
    {
        start      = Resolve<decltype(start)>(so, "InterposerMatchStart");
        joined     = Resolve<decltype(joined)>(so, "InterposerMatchPlayerJoined");
        event      = Resolve<decltype(event)>(so, "InterposerMatchEvent");
        addStat    = Resolve<decltype(addStat)>(so, "InterposerMatchAddStatistic");
        setStat    = Resolve<decltype(setStat)>(so, "InterposerMatchSetStatistic");
        setResult  = Resolve<decltype(setResult)>(so, "InterposerMatchSetResult");
        end        = Resolve<decltype(end)>(so, "InterposerMatchEnd");
        attach     = Resolve<decltype(attach)>(so, "InterposerMatchAttachFile");
        flush      = Resolve<decltype(flush)>(so, "InterposerMatchFlush");
        status     = Resolve<decltype(status)>(so, "InterposerMatchGetStatus");
        registerCb = Resolve<decltype(registerCb)>(so, "InterposerRegisterMatchCallback");

        return start && joined && event && addStat && setStat && setResult && end && attach && flush &&
               status && registerCb;
    }

    InterposerMatchStatus Status(DWORD match) const
    {
        InterposerMatchStatus st{};
        st.size = sizeof(st);
        status(match, &st);
        return st;
    }
};

static void RunMatchTests(void* so, const fs::path& dir)
{
    printf("\n--- Match Statistics Tests ---\n");

    MatchApi api;

    ASSERT(api.Load(so), "MS-01: every InterposerMatch* function is exported");

    if (!api.start)
        return;

    api.registerCb(OnMatchEvent);

    InterposerMatchInfo info{};
    info.structSize   = sizeof(info);
    info.externalId   = "round-7";
    info.map          = "de_dust2";
    info.settingsJson = "{\"timelimit\":20}";

    const DWORD match = api.start(&info);

    ASSERT(match != 0 && g_matchEvents.size() == 1 && std::get<0>(g_matchEvents[0]) == L"START",
        "MS-02: InterposerMatchStart returns a handle and fires START");

    if (!match || g_matchEvents.empty())
        return;

    ASSERT(WContains(std::get<2>(g_matchEvents[0]), L"\"settings\":{\"timelimit\":20}") &&
           WContains(std::get<2>(g_matchEvents[0]), L"\"forward\":false"),
        "MS-03: START carries the settings verbatim, and forward:false with no launcher");

    ASSERT(api.start(&info) == match, "MS-04: starting the same external id again carries on with the open match");

    api.joined(match, "name:Pat", "Pat caf\xC3\xA9", "red", 0);

    const BOOL kill = api.event(match, "kill", 4200, 268, "name:Pat", "bot:3", "{\"weapon\":\"ak47\"}");

    {
        std::wstring killJson, participant;

        for (const auto& [kind, handle, json] : g_matchEvents)
        {
            if (kind == L"EVENT")       killJson = json;
            if (kind == L"PARTICIPANT") participant = json;
        }

        ASSERT(kill && WContains(killJson, L"\"sequence\":1") && WContains(killJson, L"\"tick\":268") &&
               WContains(killJson, L"\"payload\":{\"weapon\":\"ak47\"}"),
            "MS-05: EVENT carries a sequence, the tick and the payload");

        ASSERT(WContains(participant, L"\"displayName\":\"Pat café\""),
            "MS-06: PARTICIPANT names are decoded from UTF-8");
    }

    ASSERT(!api.event(match, "kill", 1, -1, nullptr, nullptr, "{\"a\":1,}") &&
           !api.event(match + 1000, "kill", 1, -1, nullptr, nullptr, nullptr) &&
           !api.setStat(match, nullptr, nullptr, "kills", 1),
        "MS-07: malformed JSON, an unknown handle and an ownerless statistic are refused");

    api.addStat(match, "name:Pat", nullptr, "kills", 1);
    api.addStat(match, "name:Pat", nullptr, "kills", 2);
    api.end(match, "completed", FALSE);

    {
        std::wstring endJson;

        for (const auto& [kind, handle, json] : g_matchEvents)
            if (kind == L"END")
                endJson = json;

        ASSERT(WContains(endJson, L"{\"playerKey\":\"name:Pat\",\"key\":\"kills\",\"value\":3}"),
            "MS-08: END carries the summed statistics");
    }

    ASSERT(!api.event(match, "kill", 1, -1, nullptr, nullptr, nullptr) &&
           api.Status(match).sink == INTERPOSER_MATCH_SINK_NONE && api.Status(match).ended == 1,
        "MS-09: an ended match takes no more events");

    (void)dir;
    api.registerCb(nullptr);
}

// ── The fake LANCommander server ──────────────────────────────────────────────

struct FakeRequest
{
    std::string method, path, headers, body;
    int         status = 0;
};

class FakeMatchServer
{
public:
    std::function<std::tuple<int, std::string, std::string>(const FakeRequest&)> handler;

    bool Start()
    {
        listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (listener_ < 0 || bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(listener_, 16) != 0)
            return false;

        socklen_t length = sizeof(addr);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &length);
        port_ = ntohs(addr.sin_port);

        thread_ = std::thread([this] { Run(); });
        return true;
    }

    void Stop()
    {
        stop_ = true;

        if (thread_.joinable())
            thread_.join();

        if (listener_ >= 0)
            close(listener_);

        listener_ = -1;
    }

    int Port() const { return port_; }

    std::vector<FakeRequest> Requests()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.clear();
    }

private:
    int                      listener_ = -1;
    int                      port_     = 0;
    std::thread              thread_;
    std::atomic<bool>        stop_{ false };
    std::mutex               mutex_;
    std::vector<FakeRequest> requests_;

    void Run()
    {
        while (!stop_)
        {
            pollfd p{ listener_, POLLIN, 0 };

            if (poll(&p, 1, 100) <= 0)
                continue;

            const int client = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);

            if (client >= 0)
                Serve(client);
        }
    }

    void Serve(int client)
    {
        timeval tv{ 5, 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string data;
        char        chunk[65536];
        size_t      headerEnd;

        while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos)
        {
            const ssize_t n = recv(client, chunk, sizeof(chunk), 0);

            if (n <= 0)
            {
                close(client);
                return;
            }

            data.append(chunk, static_cast<size_t>(n));
        }

        FakeRequest request;
        request.headers = data.substr(0, headerEnd);

        const size_t space1 = request.headers.find(' ');
        const size_t space2 = request.headers.find(' ', space1 + 1);
        request.method = request.headers.substr(0, space1);
        request.path   = request.headers.substr(space1 + 1, space2 - space1 - 1);

        std::string lower = request.headers;
        for (char& c : lower)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

        size_t contentLength = 0;
        const size_t cl = lower.find("content-length:");

        if (cl != std::string::npos)
            contentLength = strtoul(lower.c_str() + cl + 15, nullptr, 10);

        request.body = data.substr(headerEnd + 4);

        while (request.body.size() < contentLength)
        {
            const ssize_t n = recv(client, chunk, sizeof(chunk), 0);

            if (n <= 0)
                break;

            request.body.append(chunk, static_cast<size_t>(n));
        }

        const auto [status, body, extra] = handler(request);
        request.status = status;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
        }

        const std::string response = "HTTP/1.1 " + std::to_string(status) + " X\r\n" +
            "Content-Type: application/json\r\n" +
            "Content-Length: " + std::to_string(body.size()) + "\r\n" +
            "Connection: close\r\n" + extra + "\r\n" + body;

        send(client, response.data(), response.size(), MSG_NOSIGNAL);
        shutdown(client, SHUT_WR);
        close(client);
    }
};

static const char* kFakeMatchId    = "11111111-1111-1111-1111-111111111111";
static const char* kFakeAttachment = "33333333-3333-3333-3333-333333333333";

static std::vector<long long> SequencesIn(const std::string& body)
{
    std::vector<long long> out;
    size_t at = 0;

    while ((at = body.find("\"sequence\":", at)) != std::string::npos)
    {
        at += 11;
        out.push_back(strtoll(body.c_str() + at, nullptr, 10));
    }

    return out;
}

static std::tuple<int, std::string, std::string> AnswerMatchRequest(const FakeRequest& r, const std::string& phase,
                                                                   long long& lastAccepted)
{
    const bool post = r.method == "POST";

    if (post && r.path == "/api/Matches")
        return { 201, std::string("{\"matchId\":\"") + kFakeMatchId +
            "\",\"reporterId\":\"22222222-2222-2222-2222-222222222222\",\"lastSequence\":" +
            std::to_string(lastAccepted) + "}", "" };

    if (phase == "crash")
        return { 503, "", "" };

    if (post && Contains(r.path, "/Events"))
    {
        for (long long sequence : SequencesIn(r.body))
            lastAccepted = std::max(lastAccepted, sequence);

        return { 202, "{\"queued\":1,\"rejected\":[]}", "" };
    }

    if (post && (Contains(r.path, "/Participants") || Contains(r.path, "/End")))
        return { 204, "", "" };

    if (post && Contains(r.path, "/Complete"))
        return { 200, std::string("{\"id\":\"") + kFakeAttachment + "\"}", "" };

    if (post && Contains(r.path, "/Attachments"))
        return { 201, std::string("{\"id\":\"") + kFakeAttachment + "\"}", "" };

    if (r.method == "PUT")
        return { 204, "", "" };

    return { 404, "", "" };
}

static const char kDemoContent[] = "DEMO-CONTENT-0123456789";

static int RunMatchHttpChild(const fs::path& dir, const std::string& phase)
{
    printf("\n--- Match Statistics HTTP Tests (child, %s) ---\n", phase.c_str());

    void* so = dlopen((dir / kSo).c_str(), RTLD_NOW | RTLD_LOCAL);
    MatchApi api;

    if (!so || !api.Load(so))
    {
        printf("FAIL  MS-20: child could not load the shared object\n");
        return 1;
    }

    InterposerMatchInfo info{};
    info.structSize = sizeof(info);
    info.map        = "de_test";

    if (phase == "basic")
    {
        info.externalId = "ms-basic";
        const DWORD match = api.start(&info);

        ASSERT(api.Status(match).sink == INTERPOSER_MATCH_SINK_HTTP,
            "MS-20: with ServerUrl, Token and GameId the match is sent over HTTP");

        api.joined(match, "name:Alice", "Alice", "red", 0);
        api.event(match, "kill", 1000, -1, "name:Alice", nullptr, "{\"weapon\":\"rocket\"}");
        api.event(match, "kill", 2000, -1, "name:Alice", nullptr, nullptr);
        api.addStat(match, "name:Alice", nullptr, "kills", 2);
        api.attach(match, (dir / "demo.dem").wstring().c_str(), INTERPOSER_MATCH_ATTACHMENT_DEMO, -1, -1);
        api.end(match, "completed", FALSE);

        ASSERT(api.flush(match, 15000), "MS-21: InterposerMatchFlush returns once the server has everything");

        for (int i = 0; i < 50 && !api.Status(match).delivered; ++i)
            usleep(100 * 1000);

        ASSERT(api.Status(match).delivered == 1 && strcmp(api.Status(match).matchId, kFakeMatchId) == 0,
            "MS-22: the status reports the match delivered with LANCommander's id");
    }
    else if (phase == "crash")
    {
        info.externalId = "ms-crash";
        const DWORD match = api.start(&info);

        for (int i = 1; i <= 4; ++i)
            api.event(match, "tick", i * 1000, -1, nullptr, nullptr, nullptr);

        bool spooled = false;

        for (int i = 0; i < 100 && !spooled; ++i)
        {
            usleep(100 * 1000);

            if (!fs::exists(dir / ".interposer" / "Matches"))
                continue;

            for (const auto& entry : fs::directory_iterator(dir / ".interposer" / "Matches"))
                spooled |= SequencesIn(ReadText(entry.path() / "events.jsonl")).size() == 4 &&
                           Contains(ReadText(entry.path() / "match.json"), "\"sequencesFinal\":true") &&
                           api.Status(match).lastError[0] != 0;
        }

        ASSERT(spooled, "MS-23: events the server will not take are kept in .interposer/Matches");

        // A crash: no dlclose, no destructors.
        fflush(stdout);
        _exit(g_fail);
    }
    else if (phase == "resume")
    {
        info.externalId = "ms-crash";
        const DWORD match = api.start(&info);

        api.event(match, "tick", 5000, -1, nullptr, nullptr, nullptr);
        api.end(match, "completed", FALSE);

        ASSERT(api.flush(match, 15000), "MS-24: the match left by the crashed run is carried on and finished");

        for (int i = 0; i < 50 && !api.Status(match).delivered; ++i)
            usleep(100 * 1000);
    }

    dlclose(so);
    return g_fail;
}

static bool AnySpoolLeft(const fs::path& root)
{
    if (!fs::exists(root))
        return false;

    for (const auto& entry : fs::directory_iterator(root))
        if (entry.is_directory())
            return true;

    return false;
}

static void RunMatchHttpTests(const fs::path& tmp)
{
    printf("\n--- Match Statistics HTTP Tests ---\n");

    FakeMatchServer server;
    std::string     phase = "basic";
    long long       lastAccepted = 0;
    std::mutex      phaseMutex;

    server.handler = [&](const FakeRequest& r)
    {
        std::lock_guard<std::mutex> lock(phaseMutex);
        return AnswerMatchRequest(r, phase, lastAccepted);
    };

    if (!server.Start())
    {
        ASSERT(false, "MS-10: starting the fake LANCommander server");
        return;
    }

    const fs::path dir       = tmp / "Matches";
    const fs::path spoolRoot = dir / ".interposer" / "Matches";

    StageCopy(dir);
    WriteText(dir / ".interposer" / "Config.yml",
        "Logging:\n"
        "  Matches: true\n"
        "MatchStatistics:\n"
        "  ServerUrl: 'http://127.0.0.1:" + std::to_string(server.Port()) + "/'\n"
        "  Token: test-token\n"
        "  GameId: 44444444-4444-4444-4444-444444444444\n"
        "  ReporterName: test-server\n"
        "  FlushInterval: 200\n");
    WriteText(dir / "demo.dem", kDemoContent);

    ASSERT(RunChild({ "--match-http", dir.string(), "basic" }, {}) == 0,
        "MS-10: the basic child process reported no failures");

    {
        bool authorised = false, participants = false, ended = false, uploaded = false;
        std::vector<long long> sequences;

        for (const FakeRequest& r : server.Requests())
        {
            std::string lower = r.headers;
            for (char& c : lower)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

            if (r.path == "/api/Matches")
                authorised = Contains(lower, "authorization: bearer test-token") &&
                             Contains(r.body, "\"gameId\":\"44444444-4444-4444-4444-444444444444\"") &&
                             Contains(r.body, "\"reporterName\":\"test-server\"");

            participants |= Contains(r.path, "/Participants") && Contains(r.body, "\"playerKey\":\"name:Alice\"");
            ended        |= Contains(r.path, "/End") && Contains(r.body, "\"key\":\"kills\",\"value\":2");
            uploaded     |= r.method == "PUT" && Contains(r.path, "Content?offset=0") && r.body == kDemoContent;

            if (Contains(r.path, "/Events"))
                for (long long s : SequencesIn(r.body))
                    sequences.push_back(s);
        }

        const std::vector<long long> expected{ 1, 2 };

        ASSERT(authorised, "MS-11: the match is registered with the token, the game id and the reporter");
        ASSERT(participants && sequences == expected && ended,
            "MS-12: participants, events in sequence, and the end with its statistics are sent");
        ASSERT(uploaded, "MS-15: the attachment is uploaded over plain HTTP");
        ASSERT(!AnySpoolLeft(spoolRoot), "MS-16: a delivered match leaves nothing in .interposer/Matches");
    }

    {
        std::lock_guard<std::mutex> lock(phaseMutex);
        phase = "crash";
        lastAccepted = 0;
    }

    server.Clear();

    ASSERT(RunChild({ "--match-http", dir.string(), "crash" }, {}) == 0,
        "MS-17: the crashing child spooled its events before it died");
    ASSERT(AnySpoolLeft(spoolRoot), "MS-18: the spool survives the crash");

    {
        std::lock_guard<std::mutex> lock(phaseMutex);
        phase = "resume";
        lastAccepted = 2;
    }

    server.Clear();

    ASSERT(RunChild({ "--match-http", dir.string(), "resume" }, {}) == 0,
        "MS-19: the resuming child process reported no failures");

    {
        std::multiset<long long> sent;

        for (const FakeRequest& r : server.Requests())
            if (Contains(r.path, "/Events") && r.status == 202)
                for (long long s : SequencesIn(r.body))
                    sent.insert(s);

        const std::multiset<long long> expected{ 3, 4, 5 };

        ASSERT(sent == expected, "MS-25: only the events after the server's last are resent, the new one follows them");
        ASSERT(!AnySpoolLeft(spoolRoot), "MS-26: the spool is removed once the resumed match is delivered");
    }

    server.Stop();
}

int main(int argc, char** argv)
{
    if (argc >= 5 && strcmp(argv[1], "--launcher-config") == 0)
        return RunLauncherConfigChild(argv[2], argv[3], argv[4]);

    if (argc >= 4 && strcmp(argv[1], "--second-instance") == 0)
        return RunSecondInstanceChild(argv[2], argv[3]);

    if (argc >= 3 && strcmp(argv[1], "--preload-child") == 0)
        return RunPreloadChild(argv[2]);

    if (argc >= 3 && strcmp(argv[1], "--file-hook-child") == 0)
        return RunFileHookChild(argv[2]);

    if (argc >= 2 && strcmp(argv[1], "--net-child") == 0)
        return RunNetChild();

    if (argc >= 4 && strcmp(argv[1], "--match-http") == 0)
        return RunMatchHttpChild(argv[2], argv[3]);

    char tmpTemplate[] = "/tmp/InterposerTest.XXXXXX";

    if (!mkdtemp(tmpTemplate))
    {
        printf("FAIL  could not create a temporary directory\n");
        return 1;
    }

    const fs::path tmp     = tmpTemplate;
    const fs::path mainDir = tmp / "Main";
    const fs::path marker  = tmp / "plugin_unloaded";

    printf("=== LANCommander.Interposer Linux tests (%s) ===\n", sizeof(void*) == 8 ? "x64" : "x86");

    const fs::path soPath = StageCopy(mainDir);
    WriteFixture(mainDir);
    setenv("INTERPOSER_TEST_PLUGIN_MARKER", marker.c_str(), 1);

    const std::string claim = "INTERPOSER_INSTANCE_" + std::to_string(getpid());

    void* so = dlopen(soPath.c_str(), RTLD_NOW | RTLD_LOCAL);

    if (!so)
    {
        printf("FAIL  could not load %s: %s\n", soPath.c_str(), dlerror());
        return 1;
    }

    ASSERT(getenv(claim.c_str()) != nullptr, "I-01: the first copy in a process claims it");

    RunExportTests(so);
    RunPluginConfigTests(so);
    RunSdkTests(so);
    RunPluginTests(mainDir);
    RunAdapterEnumTests(so);
    RunCommsTests(so);
    RunMatchTests(so, mainDir);

    dlclose(so);

    printf("\n--- Unload Tests ---\n");

    ASSERT(dlopen(soPath.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr,
        "U-01: dlclose unloads the shared object (nothing pins it)");
    ASSERT(fs::exists(marker), "U-02: plugins are unloaded with it");
    ASSERT(getenv(claim.c_str()) == nullptr, "U-03: the claim on the process is released");

    const fs::path logPath = FindLog(mainDir);
    const std::string log  = ReadText(logPath);

    printf("\n--- Log Tests ---\n");
    ASSERT(!logPath.empty() && Contains(log, "# === Session started "),
        "L-01: the session log is opened under .interposer/Logs with its header");
    ASSERT(CountLinesContaining(log, "[SDK PROBE]", "logged through the linked probe") == 1,
        "SDK-06: InterposerLog through the linked probe writes one session log line");
    ASSERT(CountLinesContaining(log, "[VOICE]", "voice server listening on port") == 1,
        "VC-06: voice output reaches the session log once, and only, after interposer_comms_set_session_logging");

    RunPluginConfigPersistenceTests(mainDir / ".interposer" / "Config.yml");

    {
        const std::string yml = ReadText(mainDir / ".interposer" / "Config.yml");
        ASSERT(Contains(yml, "  TestPlugin:\n    Greeting: hello\n"),
            "PL-04: a plugin's registered defaults are written to Config.yml");
    }

    RunLauncherConfigTests(tmp);
    RunSecondInstanceTests(tmp);
    RunPreloadTests(tmp);
    RunFileHookTests(tmp);
    RunNetTests(tmp);
    RunMatchHttpTests(tmp);

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);

    if (g_fail == 0)
        fs::remove_all(tmp);
    else
        printf("Kept %s for inspection\n", tmp.c_str());

    return g_fail;
}
