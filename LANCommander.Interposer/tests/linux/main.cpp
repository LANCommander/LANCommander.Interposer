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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
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

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);

    if (g_fail == 0)
        fs::remove_all(tmp);
    else
        printf("Kept %s for inspection\n", tmp.c_str());

    return g_fail;
}
