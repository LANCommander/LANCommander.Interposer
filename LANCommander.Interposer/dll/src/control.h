#pragma once
#include <windows.h>

// ---------------------------------------------------------------------------
// Control pipe -- the launcher's way of talking *to* the injected DLL, where the
// event pipe (pipe_events.h) only talks back. The host creates
// \\.\pipe\InterposerControl_<pid> before injecting, the DLL connects as the
// client, and a background thread answers requests one at a time.
//
// Wire format, little-endian:
//
//   request   u32 requestId, u32 command, u32 payloadChars, wchar[payloadChars]
//   response  u32 requestId, i32 ok (1/0), u32 payloadChars, wchar[payloadChars]
//
// The managed side is InterposerService's control client; the command numbers
// are shared with it.
// ---------------------------------------------------------------------------

enum ControlCommand : DWORD
{
    CONTROL_PING             = 1, // -> "pong"
    CONTROL_RELAY_CONNECT    = 2, // payload: NetworkRelay YAML overrides (may be empty)
    CONTROL_RELAY_DISCONNECT = 3,
    CONTROL_RELAY_STATUS     = 4, // -> JSON, see RelayStatusJson
    CONTROL_GET_CONFIG       = 5, // payload: dot path -> scalar value
};

// Connect to the host's control pipe and start answering. No-op when the host
// did not create one.
void StartControlPipe();

// Stop answering. processTerminating as for ShutdownRelay.
void StopControlPipe(bool processTerminating);
