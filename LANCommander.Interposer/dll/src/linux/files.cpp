#include "files.h"
#include "config.h"
#include "file_access.h"
#include "hooking.h"
#include "platform.h"

#include <cstdarg>
#include <string>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

// We redefine libc entry points that carry __attribute__((nonnull)) on their
// path argument, so GCC flags our deliberate null guards. The guards stay: a
// game may pass null, and the real function must still see it.
#pragma GCC diagnostic ignored "-Wnonnull-compare"

// The Linux file hooks. Where Windows detours KernelBase with MinHook, here the
// shared object exports these libc names and, preloaded, comes first in symbol
// resolution, so the game's calls land on them. The real libc function is the
// RTLD_NEXT trampoline each one calls.
//
// Every public entry point glibc offers for a job needs its own hook: glibc
// reaches `open` from `fopen` through an internal call that never leaves libc,
// so hooking `open` alone would miss `fopen`. The decisions are the shared ones
// in core/file_access.h; a hook only converts between its arguments and a path.

namespace
{
    // The real libc function behind a name. RTLD_NEXT is the right answer when
    // we are preloaded -- libc follows us in the search order -- but returns
    // null when the object is dlopened instead (as the test harness loads it),
    // because then libc precedes us. So fall back to libc by name, which also
    // works when preloaded. Either way the result is cached.
    void* RealSymbol(const char* name)
    {
        if (void* next = dlsym(RTLD_NEXT, name))
            return next;

        static void* libc = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
        return libc ? dlsym(libc, name) : nullptr;
    }

    // Lazily-resolved libc trampolines. Resolved on first use, because our own
    // exported names interpose on the file calls LoadConfig makes before the
    // hooks resolve -- and those must pass through to libc. InstallFileHooks
    // primes and logs them once the session log exists.
    template <typename Fn>
    Fn Real(Fn& slot, const char* name)
    {
        if (!slot)
            slot = reinterpret_cast<Fn>(RealSymbol(name));

        return slot;
    }

    // True when a hook must do nothing but call the trampoline: before the hooks
    // are switched on, or inside file access the Interposer itself is making.
    bool PassThrough()
    {
        return !hooking::Enabled() || InternalFileAccess::Active();
    }

    FileAccessMode OpenMode(int flags)
    {
        const int access = flags & O_ACCMODE;
        const bool writes = access == O_WRONLY || access == O_RDWR ||
            (flags & (O_CREAT | O_APPEND | O_TRUNC));
        const bool reads = access == O_RDONLY || access == O_RDWR;

        if (writes && reads) return FileAccessMode::ReadWrite;
        if (writes)          return FileAccessMode::Write;
        return                      FileAccessMode::Read;
    }

    // "r" is read; "w", "a" and any "+" update add write.
    FileAccessMode FopenMode(const char* mode)
    {
        if (!mode)
            return FileAccessMode::Read;

        const bool plus  = std::string(mode).find('+') != std::string::npos;
        const bool write = mode[0] == 'w' || mode[0] == 'a';

        if (plus || (write && mode[0] == 'a')) return FileAccessMode::ReadWrite;
        if (write)                             return FileAccessMode::Write;
        if (plus)                              return FileAccessMode::ReadWrite;
        return                                        FileAccessMode::Read;
    }

    std::string ForOpen(const char* path, FileAccessMode mode)
    {
        return platform::WideToUtf8(ResolveOpenPath(platform::Utf8ToWide(path), mode));
    }

    std::string ForVerb(const char* path, const wchar_t* verb)
    {
        return platform::WideToUtf8(ResolvePath(platform::Utf8ToWide(path), verb));
    }

    // A metadata query: redirect and log [FILE ATTR]; on a miss that was not
    // redirected, offer a FastDL overlay copy. `attempt` runs the real call on a
    // path and returns 0 on success.
    template <typename Attempt>
    int ResolveAttr(const char* path, Attempt attempt)
    {
        const std::wstring original  = platform::Utf8ToWide(path);
        const std::wstring resolved  = ResolvePath(original, L"FILE ATTR");
        const std::string  resolved8 = platform::WideToUtf8(resolved);

        const int result = attempt(resolved8.c_str());

        if (result == 0 || resolved != original)
            return result;

        const std::wstring fallback = AttributeFallbackPath(original);

        if (!fallback.empty())
            return attempt(platform::WideToUtf8(fallback).c_str());

        return result;
    }
}

// ── open family ─────────────────────────────────────────────────────────────

extern "C" INTERPOSER_API int open(const char* path, int flags, ...)
{
    static int (*real)(const char*, int, ...) = nullptr;
    Real(real, "open");

    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE))
    {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    if (!path || PassThrough())
        return real(path, flags, mode);

    return real(ForOpen(path, OpenMode(flags)).c_str(), flags, mode);
}

extern "C" INTERPOSER_API int open64(const char* path, int flags, ...)
{
    static int (*real)(const char*, int, ...) = nullptr;
    Real(real, "open64");

    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE))
    {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    if (!path || PassThrough())
        return real(path, flags, mode);

    return real(ForOpen(path, OpenMode(flags)).c_str(), flags, mode);
}

extern "C" INTERPOSER_API int openat(int dirfd, const char* path, int flags, ...)
{
    static int (*real)(int, const char*, int, ...) = nullptr;
    Real(real, "openat");

    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE))
    {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    if (!path || PassThrough())
        return real(dirfd, path, flags, mode);

    return real(dirfd, ForOpen(path, OpenMode(flags)).c_str(), flags, mode);
}

// ── fopen family ──────────────────────────────────────────────────────────────

extern "C" INTERPOSER_API FILE* fopen(const char* path, const char* mode)
{
    static FILE* (*real)(const char*, const char*) = nullptr;
    Real(real, "fopen");

    if (!path || PassThrough())
        return real(path, mode);

    return real(ForOpen(path, FopenMode(mode)).c_str(), mode);
}

extern "C" INTERPOSER_API FILE* fopen64(const char* path, const char* mode)
{
    static FILE* (*real)(const char*, const char*) = nullptr;
    Real(real, "fopen64");

    if (!path || PassThrough())
        return real(path, mode);

    return real(ForOpen(path, FopenMode(mode)).c_str(), mode);
}

extern "C" INTERPOSER_API FILE* freopen(const char* path, const char* mode, FILE* stream)
{
    static FILE* (*real)(const char*, const char*, FILE*) = nullptr;
    Real(real, "freopen");

    if (!path || PassThrough())
        return real(path, mode, stream);

    return real(ForOpen(path, FopenMode(mode)).c_str(), mode, stream);
}

// ── stat family ───────────────────────────────────────────────────────────────

extern "C" INTERPOSER_API int stat(const char* path, struct stat* buf)
{
    static int (*real)(const char*, struct stat*) = nullptr;
    Real(real, "stat");

    if (!path || PassThrough())
        return real(path, buf);

    return ResolveAttr(path, [&](const char* p) { return real(p, buf); });
}

extern "C" INTERPOSER_API int lstat(const char* path, struct stat* buf)
{
    static int (*real)(const char*, struct stat*) = nullptr;
    Real(real, "lstat");

    if (!path || PassThrough())
        return real(path, buf);

    return ResolveAttr(path, [&](const char* p) { return real(p, buf); });
}

extern "C" INTERPOSER_API int stat64(const char* path, struct stat64* buf)
{
    static int (*real)(const char*, struct stat64*) = nullptr;
    Real(real, "stat64");

    if (!path || PassThrough())
        return real(path, buf);

    return ResolveAttr(path, [&](const char* p) { return real(p, buf); });
}

extern "C" INTERPOSER_API int lstat64(const char* path, struct stat64* buf)
{
    static int (*real)(const char*, struct stat64*) = nullptr;
    Real(real, "lstat64");

    if (!path || PassThrough())
        return real(path, buf);

    return ResolveAttr(path, [&](const char* p) { return real(p, buf); });
}

// The versioned stat entry points a program built against older glibc calls.
extern "C" INTERPOSER_API int __xstat(int ver, const char* path, struct stat* buf)
{
    static int (*real)(int, const char*, struct stat*) = nullptr;
    Real(real, "__xstat");

    if (!real)
        return -1;

    if (!path || PassThrough())
        return real(ver, path, buf);

    return ResolveAttr(path, [&](const char* p) { return real(ver, p, buf); });
}

extern "C" INTERPOSER_API int __lxstat(int ver, const char* path, struct stat* buf)
{
    static int (*real)(int, const char*, struct stat*) = nullptr;
    Real(real, "__lxstat");

    if (!real)
        return -1;

    if (!path || PassThrough())
        return real(ver, path, buf);

    return ResolveAttr(path, [&](const char* p) { return real(ver, p, buf); });
}

extern "C" INTERPOSER_API int statx(int dirfd, const char* path, int flags, unsigned int mask, struct statx* buf)
{
    static int (*real)(int, const char*, int, unsigned int, struct statx*) = nullptr;
    Real(real, "statx");

    if (!real)
        return -1;

    if (!path || PassThrough())
        return real(dirfd, path, flags, mask, buf);

    return ResolveAttr(path, [&](const char* p) { return real(dirfd, p, flags, mask, buf); });
}

extern "C" INTERPOSER_API int access(const char* path, int amode)
{
    static int (*real)(const char*, int) = nullptr;
    Real(real, "access");

    if (!path || PassThrough())
        return real(path, amode);

    return ResolveAttr(path, [&](const char* p) { return real(p, amode); });
}

// ── enumeration ───────────────────────────────────────────────────────────────

extern "C" INTERPOSER_API DIR* opendir(const char* path)
{
    static DIR* (*real)(const char*) = nullptr;
    Real(real, "opendir");

    if (!path || PassThrough())
        return real(path);

    return real(ForVerb(path, L"FILE FIND").c_str());
}

// ── delete and rename ─────────────────────────────────────────────────────────

extern "C" INTERPOSER_API int unlink(const char* path)
{
    static int (*real)(const char*) = nullptr;
    Real(real, "unlink");

    if (!path || PassThrough())
        return real(path);

    return real(ForVerb(path, L"FILE DELETE").c_str());
}

extern "C" INTERPOSER_API int remove(const char* path)
{
    static int (*real)(const char*) = nullptr;
    Real(real, "remove");

    if (!path || PassThrough())
        return real(path);

    return real(ForVerb(path, L"FILE DELETE").c_str());
}

extern "C" INTERPOSER_API int rename(const char* from, const char* to)
{
    static int (*real)(const char*, const char*) = nullptr;
    Real(real, "rename");

    if (!from || !to || PassThrough())
        return real(from, to);

    std::wstring rFrom, rTo;
    ResolveTwoPaths(platform::Utf8ToWide(from), platform::Utf8ToWide(to), L"FILE MOVE", rFrom, rTo);

    return real(platform::WideToUtf8(rFrom).c_str(), platform::WideToUtf8(rTo).c_str());
}

// ── library load ──────────────────────────────────────────────────────────────

extern "C" INTERPOSER_API void* dlopen(const char* path, int flags)
{
    static void* (*real)(const char*, int) = nullptr;
    Real(real, "dlopen");

    // dlopen(nullptr) returns the main-program handle; nothing to redirect.
    if (!path || PassThrough())
        return real(path, flags);

    return real(ForVerb(path, L"DLL LOAD").c_str(), flags);
}

// ── installation ──────────────────────────────────────────────────────────────

void InstallFileHooks()
{
    // Write each hook's [HOOK INIT] line, now that the log is open. The
    // trampolines resolve themselves lazily on first call; this only records
    // which were found, with the same libc fallback so the dlopen'd test build
    // does not report them all missing.
    for (const char* name : {
            "open", "open64", "openat", "fopen", "fopen64", "freopen",
            "stat", "lstat", "stat64", "lstat64", "__xstat", "__lxstat", "statx",
            "access", "opendir", "unlink", "remove", "rename", "dlopen" })
    {
        hooking::LogHookStatus(L"libc", name,
            RealSymbol(name) ? hooking::Status::Ok : hooking::Status::FunctionNotFound);
    }
}
