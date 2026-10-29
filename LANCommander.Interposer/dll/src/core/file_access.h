#pragma once

// The decisions every file hook makes, whatever the platform: whether a path is
// redirected, what the access is called in the log, and whether a FastDL
// overlay copy stands in for it. The hooks themselves only translate between
// their API's arguments and these calls.

#include <string>

// How an open is going to use the file. Decides the log verb, whether the
// destination directory of a redirect is created, and whether FastDL may serve
// the file.
enum class FileAccessMode
{
    Read,
    Write,
    ReadWrite,
};

// Apply FileRedirects to `path`, writing the [REDIRECT HIT] / [REDIRECT MISS] /
// [REDIRECT RULE] diagnostics at Debug and Trace. Logs nothing at Info.
std::wstring RedirectFilePath(const std::wstring& path);

// The whole decision for an open: redirect (logged as [FILE REDIRECT], or the
// plain [FILE READ] / [FILE WRITE] / [FILE R/W] verb), create the redirect
// destination's directory for a write, and for a read let FastDL fetch the file
// and serve it from the overlay ([FILE OVERLAY]). Returns the path to open.
std::wstring ResolveOpenPath(const std::wstring& path, FileAccessMode mode);

// Every other single-path operation -- a metadata query, a find, a delete, a
// library load: redirect, logged as [FILE REDIRECT] or as `verb` ("FILE ATTR",
// "FILE FIND", "FILE DELETE", "DLL LOAD"). Returns the path to use.
std::wstring ResolvePath(const std::wstring& path, const wchar_t* verb);

// The FastDL overlay copy to answer a failed metadata query from, or empty.
// For a hook whose query on the unredirected path found nothing.
std::wstring AttributeFallbackPath(const std::wstring& path);

// For two-path operations (move, copy, rename): redirect both, log, and create
// the destination's directory. `verb` is the plain verb when neither path is
// redirected ("FILE MOVE", "FILE COPY").
void ResolveTwoPaths(const std::wstring& source, const std::wstring& destination,
                     const wchar_t* verb, std::wstring& outSource, std::wstring& outDestination);

// Create every directory above `filePath`.
void EnsureParentDirectoryExists(const std::wstring& filePath);

// File access the Interposer makes for its own purposes -- a FastDL download
// writing its temp file, reading a file back for its CRC, checking whether an
// overlay copy exists -- must not be redirected, logged or probed again by the
// hooks it passes through. Every file hook passes straight through while one of
// these is alive on its thread.
class InternalFileAccess
{
public:
    InternalFileAccess();
    ~InternalFileAccess();
    InternalFileAccess(const InternalFileAccess&) = delete;
    InternalFileAccess& operator=(const InternalFileAccess&) = delete;

    // True on a thread inside such an access.
    static bool Active();
};
