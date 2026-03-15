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
