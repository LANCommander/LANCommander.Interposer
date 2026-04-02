#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

// Initialize WinHTTP session and apply named-MMF URL override (if any).
// Call after LoadConfig(), before MH_EnableHook.
void InitFastDL();

// Shut down WinHTTP session. Call from RemoveHooks() before MH_DisableHook.
void ShutdownFastDL();

// If FastDL is enabled and the path is eligible, ensure the local file is
// up-to-date by downloading from the HTTP server. Returns true if the file
// was downloaded (or already current). Returns false if FastDL was skipped.
bool TryFastDLDownload(const std::wstring& localPath);

// Returns true if the file is available on the FastDL server (HEAD check).
// Used by GetFileAttributes hooks to indicate the file will be available
// without triggering the actual download yet.
bool FastDLFileExists(const std::wstring& localPath);

// If FastDL is enabled, the download overlay is active, and an overlay copy of
// localPath already exists on disk, returns the absolute overlay path.
// Returns an empty string otherwise (no overlay, or file not yet downloaded).
std::wstring GetExistingOverlayPath(const std::wstring& localPath);

// Probe http://<host>:<probePort><ProbePath> for a LANCommander FastDL endpoint.
// Sends X-LANCommander-GameServer-Host and (if > 0) X-LANCommander-GameServer-Port
// so the server can route to the correct game-server instance.
// A response with the X-LANCommander-FastDL header is "verified" and preferred
// over a plain HTTP 200 ("unverified"). The first verified result wins; an
// unverified result is kept only when no verified probe has succeeded.
// Safe to call from any thread.
void ProbeServerForFastDL(const std::wstring& host, int probePort, int gameServerPort);
