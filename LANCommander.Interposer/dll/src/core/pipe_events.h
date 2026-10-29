#pragma once

// Event type IDs sent over the named pipe.
enum PipeEventType : int
{
    PIPE_EVENT_FILE     = 1,
    PIPE_EVENT_REGISTRY = 2,
    PIPE_EVENT_DNS      = 3,
    PIPE_EVENT_NETWORK  = 4,
    PIPE_EVENT_IDENTITY = 5,
    PIPE_EVENT_RELAY    = 6, // field1 kind, field2 address, field3 detail, intField value
};

// Try to connect to the host's named pipe (\\.\pipe\InterposerEvents_{pid}).
// Safe to call early; no-op if the pipe doesn't exist.
void ConnectEventPipe();

// Disconnect and close the pipe handle.
void DisconnectEventPipe();

// Send an event over the pipe. No-op if the pipe isn't connected.
// All strings are nullable (sent as zero-length). intField is the port for
// PIPE_EVENT_NETWORK and the value for PIPE_EVENT_RELAY, 0 otherwise.
void SendPipeEvent(PipeEventType type,
                   const wchar_t* field1,
                   const wchar_t* field2,
                   const wchar_t* field3 = nullptr,
                   int intField = 0);
