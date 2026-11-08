#include "platform.h"
#include "version_info.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>

#include <dirent.h>
#include <pthread.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace platform
{

const wchar_t kPathSeparator = L'/';

// ── Text ──────────────────────────────────────────────────────────────────────
//
// wchar_t is UTF-32 here. Everything handed to the OS is UTF-8.

namespace
{
    constexpr char32_t kReplacement = 0xFFFD;

    void AppendUtf8(std::string& out, char32_t cp)
    {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            cp = kReplacement;

        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string Narrow(const std::wstring& text)
    {
        return WideToUtf8(text);
    }
}

std::string WideToUtf8(const std::wstring& text)
{
    std::string out;
    out.reserve(text.size());

    for (wchar_t c : text)
    {
        if (c == L'\0')
            break;

        AppendUtf8(out, static_cast<char32_t>(c));
    }

    return out;
}

std::wstring Utf8ToWide(const std::string& text)
{
    std::wstring out;
    out.reserve(text.size());

    const auto* p   = reinterpret_cast<const unsigned char*>(text.data());
    const auto* end = p + text.size();

    while (p < end && *p)
    {
        const unsigned char lead = *p;
        int       extra;
        char32_t  cp;

        if (lead < 0x80)                { out += static_cast<wchar_t>(lead); ++p; continue; }
        else if ((lead & 0xE0) == 0xC0) { extra = 1; cp = lead & 0x1F; }
        else if ((lead & 0xF0) == 0xE0) { extra = 2; cp = lead & 0x0F; }
        else if ((lead & 0xF8) == 0xF0) { extra = 3; cp = lead & 0x07; }
        else                            { out += static_cast<wchar_t>(kReplacement); ++p; continue; }

        if (end - p <= extra)
        {
            out += static_cast<wchar_t>(kReplacement);
            break;
        }

        bool valid = true;

        for (int i = 1; i <= extra; ++i)
        {
            if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }

        // Overlong forms, surrogates and out-of-range values are all invalid.
        static constexpr char32_t kMinimum[] = { 0, 0x80, 0x800, 0x10000 };

        if (!valid || cp < kMinimum[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        {
            out += static_cast<wchar_t>(kReplacement);
            ++p;
            continue;
        }

        out += static_cast<wchar_t>(cp);
        p += extra + 1;
    }

    return out;
}

std::string Utf16LeToUtf8(const unsigned char* data, size_t bytes)
{
    std::string out;
    out.reserve(bytes / 2);

    for (size_t i = 0; i + 1 < bytes; i += 2)
    {
        char32_t unit = data[i] | (data[i + 1] << 8);

        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes)
        {
            const char32_t low = data[i + 2] | (data[i + 3] << 8);

            if (low >= 0xDC00 && low <= 0xDFFF)
            {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }

        AppendUtf8(out, unit);
    }

    return out;
}

// ── Paths ─────────────────────────────────────────────────────────────────────

bool IsAbsolutePath(const std::wstring& path)
{
    return !path.empty() && path[0] == L'/';
}

void TrimTrailingSeparators(std::wstring& path)
{
    while (path.size() > 1 && path.back() == L'/')
        path.pop_back();
}

bool GetFullPath(const std::wstring& path, std::wstring& out)
{
    if (path.empty())
        return false;

    std::filesystem::path full(Narrow(path));

    if (!full.is_absolute())
    {
        char cwd[PATH_MAX];

        if (!getcwd(cwd, sizeof(cwd)))
            return false;

        full = std::filesystem::path(cwd) / full;
    }

    out = Utf8ToWide(full.lexically_normal().string());
    return true;
}

namespace
{
    // The file this function's code lives in: the Interposer itself.
    std::string ModulePathUtf8()
    {
        Dl_info info{};

        if (!dladdr(reinterpret_cast<void*>(&ModulePathUtf8), &info) || !info.dli_fname)
            return {};

        char resolved[PATH_MAX];
        return realpath(info.dli_fname, resolved) ? std::string(resolved) : std::string(info.dli_fname);
    }
}

std::wstring ModulePath()
{
    return Utf8ToWide(ModulePathUtf8());
}

std::wstring ModuleDirectory()
{
    std::string path = ModulePathUtf8();
    const size_t slash = path.rfind('/');

    path.resize(slash == std::string::npos ? 0 : slash + 1);
    return Utf8ToWide(path);
}

std::wstring ExecutablePath()
{
    char buffer[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);

    return length > 0 ? Utf8ToWide(std::string(buffer, static_cast<size_t>(length))) : std::wstring();
}

std::wstring ExecutableDirectory()
{
    char buffer[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);

    if (length <= 0)
        return {};

    std::string path(buffer, static_cast<size_t>(length));
    const size_t slash = path.rfind('/');

    if (slash == std::string::npos)
        return {};

    path.resize(slash == 0 ? 1 : slash);
    return Utf8ToWide(path);
}

void CreateDirectoryTree(const std::wstring& path)
{
    const std::string narrow = Narrow(path);

    // mkdir every prefix that ends at a separator, then the whole path.
    for (size_t slash = narrow.find('/', 1); ; slash = narrow.find('/', slash + 1))
    {
        const std::string prefix = narrow.substr(0, slash);

        if (!prefix.empty())
            mkdir(prefix.c_str(), 0755);

        if (slash == std::string::npos)
            break;
    }
}

std::vector<DirectoryEntry> ListDirectory(const std::wstring& directory)
{
    std::vector<DirectoryEntry> entries;

    std::string narrow = Narrow(directory);
    DIR* dir = opendir(narrow.c_str());

    if (!dir)
        return entries;

    if (!narrow.empty() && narrow.back() != '/')
        narrow += '/';

    while (dirent* entry = readdir(dir))
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        bool isDirectory = entry->d_type == DT_DIR;

        // Not every file system fills d_type in, and a link to a directory
        // counts as one, as it would on Windows.
        if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK)
        {
            struct stat st{};
            isDirectory = stat((narrow + entry->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }

        entries.push_back({ Utf8ToWide(entry->d_name), isDirectory });
    }

    closedir(dir);

    // readdir has no order; NTFS returns names sorted, so sort to match.
    std::sort(entries.begin(), entries.end(),
        [](const DirectoryEntry& a, const DirectoryEntry& b) { return a.name < b.name; });

    return entries;
}

// ── Files ─────────────────────────────────────────────────────────────────────

FileHandle OpenForAppend(const std::wstring& path)
{
    const int fd = open(Narrow(path).c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    return fd < 0 ? kInvalidFile : static_cast<FileHandle>(fd);
}

namespace
{
    bool WriteAll(int fd, const void* data, size_t bytes)
    {
        const auto* p = static_cast<const char*>(data);

        while (bytes > 0)
        {
            const ssize_t written = write(fd, p, bytes);

            if (written < 0)
            {
                if (errno == EINTR)
                    continue;

                return false;
            }

            p     += written;
            bytes -= static_cast<size_t>(written);
        }

        return true;
    }
}

bool WriteToFile(FileHandle file, const void* data, size_t bytes)
{
    return file != kInvalidFile && WriteAll(static_cast<int>(file), data, bytes);
}

void CloseFile(FileHandle file)
{
    if (file != kInvalidFile)
        close(static_cast<int>(file));
}

bool ReadWholeFile(const std::wstring& path, size_t maxBytes, std::vector<unsigned char>& out)
{
    const int fd = open(Narrow(path).c_str(), O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return false;

    struct stat st{};

    if (fstat(fd, &st) != 0 || st.st_size <= 0 || static_cast<unsigned long long>(st.st_size) > maxBytes)
    {
        close(fd);
        return false;
    }

    out.assign(static_cast<size_t>(st.st_size), 0);

    size_t total = 0;

    while (total < out.size())
    {
        const ssize_t got = read(fd, out.data() + total, out.size() - total);

        if (got < 0 && errno == EINTR)
            continue;

        if (got <= 0)
            break;

        total += static_cast<size_t>(got);
    }

    close(fd);
    out.resize(total);
    return true;
}

bool WriteWholeFile(const std::wstring& path, const std::string& content)
{
    const int fd = open(Narrow(path).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    if (fd < 0)
        return false;

    const bool ok = WriteAll(fd, content.data(), content.size());
    close(fd);
    return ok;
}

bool PathExists(const std::wstring& path)
{
    struct stat st{};
    return stat(Narrow(path).c_str(), &st) == 0;
}

bool ReplaceFile(const std::wstring& from, const std::wstring& to)
{
    return rename(Narrow(from).c_str(), Narrow(to).c_str()) == 0;
}

void RemoveFile(const std::wstring& path)
{
    unlink(Narrow(path).c_str());
}

void RemoveEmptyDirectory(const std::wstring& path)
{
    rmdir(Narrow(path).c_str());
}

long long FileSize(const std::wstring& path)
{
    struct stat st{};

    if (::stat(Narrow(path).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return -1;

    return static_cast<long long>(st.st_size);
}

bool ReadFileRange(const std::wstring& path, long long offset, size_t maxBytes,
                   std::vector<unsigned char>& out)
{
    out.clear();

    const int fd = open(Narrow(path).c_str(), O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return false;

    out.resize(maxBytes);
    size_t total = 0;
    bool   ok    = true;

    while (total < maxBytes)
    {
        const ssize_t got = pread(fd, out.data() + total, maxBytes - total,
            static_cast<off_t>(offset + static_cast<long long>(total)));

        if (got < 0 && errno == EINTR)
            continue;

        if (got < 0)
        {
            ok = false;
            break;
        }

        if (got == 0)
            break;

        total += static_cast<size_t>(got);
    }

    close(fd);
    out.resize(ok ? total : 0);
    return ok;
}

// ── Environment ───────────────────────────────────────────────────────────────

bool GetEnvironmentValue(const std::wstring& name, std::wstring& out)
{
    if (name.empty())
        return false;

    // An empty variable counts as unset, as GetEnvironmentVariableW reports it.
    const char* value = getenv(Narrow(name).c_str());

    if (!value || !*value)
        return false;

    out = Utf8ToWide(value);
    return true;
}

// %NAME% rather than $NAME: configs are written once for every platform, and
// this is what ExpandEnvironmentStringsW does on Windows.
std::wstring ExpandEnvironment(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());

    size_t i = 0;

    while (i < text.size())
    {
        const size_t open  = text.find(L'%', i);
        const size_t close = open == std::wstring::npos ? open : text.find(L'%', open + 1);

        if (close == std::wstring::npos)
        {
            out += text.substr(i);
            break;
        }

        out += text.substr(i, open - i);

        std::wstring value;

        if (GetEnvironmentValue(text.substr(open + 1, close - open - 1), value))
            out += value;
        else
            out += text.substr(open, close - open + 1);

        i = close + 1;
    }

    return out;
}

namespace
{
    // Path-token name -> folder, filled by ResolveKnownFolders.
    std::map<std::wstring, std::wstring> g_knownFolders;

    std::string HomeDirectory()
    {
        if (const char* home = getenv("HOME"); home && *home)
            return home;

        passwd  entry{};
        passwd* result = nullptr;
        char    buffer[4096];

        if (getpwuid_r(geteuid(), &entry, buffer, sizeof(buffer), &result) == 0 && result && result->pw_dir)
            return result->pw_dir;

        return {};
    }

    // An XDG base directory: the variable when it holds an absolute path,
    // otherwise its default under the home directory.
    std::string XdgBase(const char* variable, const std::string& home, const char* fallback)
    {
        if (const char* value = getenv(variable); value && value[0] == '/')
            return value;

        return home.empty() ? std::string() : home + "/" + fallback;
    }

    // $XDG_CONFIG_HOME/user-dirs.dirs, written by xdg-user-dirs-update:
    //   XDG_DOCUMENTS_DIR="$HOME/Documents"
    std::map<std::string, std::string> ReadUserDirs(const std::string& configHome, const std::string& home)
    {
        std::map<std::string, std::string> dirs;
        std::ifstream file(configHome + "/user-dirs.dirs");
        std::string   line;

        while (std::getline(file, line))
        {
            const size_t eq = line.find('=');

            if (line.empty() || line[0] == '#' || eq == std::string::npos)
                continue;

            std::string value = line.substr(eq + 1);

            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);

            if (value.rfind("$HOME", 0) == 0)
                value = home + value.substr(5);

            if (!value.empty() && value[0] == '/')
                dirs[line.substr(0, eq)] = value;
        }

        return dirs;
    }
}

void ResolveKnownFolders()
{
    g_knownFolders.clear();

    const std::string home = HomeDirectory();
    const auto userDirs    = ReadUserDirs(XdgBase("XDG_CONFIG_HOME", home, ".config"), home);

    auto userDir = [&](const char* key, const char* fallback) -> std::string
    {
        if (auto it = userDirs.find(key); it != userDirs.end())
            return it->second;

        return home.empty() ? std::string() : home + "/" + fallback;
    };

    const std::string documents = userDir("XDG_DOCUMENTS_DIR", "Documents");

    const std::pair<const wchar_t*, std::string> folders[] = {
        // No Linux equivalent; the user's data directory is the closest in spirit.
        { L"SAVEDGAMES",  XdgBase("XDG_DATA_HOME", home, ".local/share") },
        { L"DOCUMENTS",   documents },
        { L"MYDOCUMENTS", documents },
        { L"MYGAMES",     documents.empty() ? std::string() : documents + "/My Games" },
        { L"DESKTOP",     userDir("XDG_DESKTOP_DIR",  "Desktop")  },
        { L"PICTURES",    userDir("XDG_PICTURES_DIR", "Pictures") },
        { L"MUSIC",       userDir("XDG_MUSIC_DIR",    "Music")    },
        { L"VIDEOS",      userDir("XDG_VIDEOS_DIR",   "Videos")   },
    };

    for (const auto& [name, folder] : folders)
    {
        if (folder.empty())
            continue;

        std::wstring wide = Utf8ToWide(folder);
        TrimTrailingSeparators(wide);
        g_knownFolders[name] = wide;
    }
}

bool KnownFolder(const std::wstring& upperName, std::wstring& out)
{
    auto it = g_knownFolders.find(upperName);

    if (it == g_knownFolders.end())
        return false;

    out = it->second;
    return true;
}

bool CurrentUserName(std::wstring& out)
{
    passwd  entry{};
    passwd* result = nullptr;
    char    buffer[4096];

    if (getpwuid_r(geteuid(), &entry, buffer, sizeof(buffer), &result) == 0 && result && result->pw_name)
    {
        out = Utf8ToWide(result->pw_name);
        return true;
    }

    if (const char* user = getenv("USER"); user && *user)
    {
        out = Utf8ToWide(user);
        return true;
    }

    return false;
}

// ── Time ──────────────────────────────────────────────────────────────────────

LocalTime Now()
{
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);

    return { local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec };
}

// ── Process and launcher ──────────────────────────────────────────────────────

unsigned long ProcessId()
{
    return static_cast<unsigned long>(getpid());
}

// ── Threads ───────────────────────────────────────────────────────────────────

namespace
{
    struct ThreadState
    {
        pthread_t thread{};
        void (*body)(void*) = nullptr;
        void*     arg = nullptr;
    };

    void* ThreadMain(void* param)
    {
        auto* state = static_cast<ThreadState*>(param);
        state->body(state->arg);
        return nullptr;
    }
}

Thread StartThread(void (*body)(void*), void* arg)
{
    auto* state = new ThreadState;
    state->body = body;
    state->arg  = arg;

    if (pthread_create(&state->thread, nullptr, ThreadMain, state) != 0)
    {
        delete state;
        return nullptr;
    }

    return state;
}

// No loader lock to work around here, so the thread is joined -- but only for
// waitMs. The destructors run at exit too, and a thread in the middle of a
// network request would otherwise hold the process open until it times out;
// one that does not finish in time is detached and its state leaked.
bool StopThread(Thread thread, int waitMs)
{
    auto* state = static_cast<ThreadState*>(thread);

    if (!state)
        return true;

    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += waitMs / 1000;
    deadline.tv_nsec += static_cast<long>(waitMs % 1000) * 1000000L;

    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    if (pthread_timedjoin_np(state->thread, nullptr, &deadline) != 0)
    {
        pthread_detach(state->thread);
        return false;
    }

    delete state;
    return true;
}

// The launcher cannot know the PID before the process exists, so a value comes
// through the environment, which LD_PRELOAD needs set anyway: the name in upper
// snake case, so L"InterposerBaseDirectory" is INTERPOSER_BASE_DIRECTORY.
std::string ReadLauncherValue(const wchar_t* name)
{
    std::string variable;

    for (const wchar_t* p = name; *p; ++p)
    {
        if (*p >= L'A' && *p <= L'Z' && p != name)
            variable += '_';

        variable += static_cast<char>(*p >= L'a' && *p <= L'z' ? *p - (L'a' - L'A') : *p);
    }

    const char* value = getenv(variable.c_str());
    return value ? std::string(value) : std::string();
}

FileHandle ConnectEventChannel()
{
    const std::string path = ReadLauncherValue(L"InterposerEventSocket");

    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
        return kInvalidFile;

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return kInvalidFile;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.c_str(), path.size());

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        close(fd);
        return kInvalidFile;
    }

    return static_cast<FileHandle>(fd);
}

bool WriteEventChannel(FileHandle channel, const void* data, size_t bytes)
{
    const auto* p = static_cast<const char*>(data);

    while (bytes > 0)
    {
        // MSG_NOSIGNAL: a launcher that went away must not kill the game with SIGPIPE.
        const ssize_t sent = send(static_cast<int>(channel), p, bytes, MSG_NOSIGNAL);

        if (sent < 0 && errno == EINTR)
            continue;

        if (sent <= 0)
            return false;

        p     += sent;
        bytes -= static_cast<size_t>(sent);
    }

    return true;
}

// ── Modules ───────────────────────────────────────────────────────────────────

Module SelfModule()
{
    Dl_info info{};

    if (!dladdr(reinterpret_cast<void*>(&SelfModule), &info) || !info.dli_fname)
        return nullptr;

    // RTLD_NOLOAD only looks the loaded object up, but still counts a reference.
    // Hand it straight back: the handle stays valid for as long as we are loaded,
    // and holding the reference would keep a dlclose from ever unloading us.
    void* handle = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);

    if (handle)
        dlclose(handle);

    return handle;
}

// Stamped in at build time from the git tag (build/GenerateVersion.cmake).
std::wstring ModuleVersion()
{
    if (VER_MAJOR == 0 && VER_MINOR == 0 && VER_PATCH == 0)
        return L"1.0";

    return Utf8ToWide(VER_FILEVERSION_STR);
}

Module LoadModule(const std::wstring& path, std::wstring& error)
{
    void* module = dlopen(Narrow(path).c_str(), RTLD_NOW | RTLD_LOCAL);

    if (!module)
    {
        const char* reason = dlerror();
        error = Utf8ToWide(reason ? reason : "dlopen failed");
    }

    return module;
}

void* FindSymbol(Module module, const char* name)
{
    return module ? dlsym(module, name) : nullptr;
}

void UnloadModule(Module module)
{
    if (module)
        dlclose(module);
}

const std::vector<std::wstring>& PluginExtensions()
{
    static const std::vector<std::wstring> extensions = { L".so" };
    return extensions;
}

} // namespace platform
