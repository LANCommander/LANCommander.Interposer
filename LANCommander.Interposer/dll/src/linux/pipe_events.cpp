#include "pipe_events.h"

// The event channel to the launcher is not implemented on Linux yet. Events
// still reach in-process consumers through the Interposer*Callback exports,
// which callbacks.cpp fires before calling SendPipeEvent.

void ConnectEventPipe()
{
}

void DisconnectEventPipe()
{
}

void SendPipeEvent(PipeEventType, const wchar_t*, const wchar_t*, const wchar_t*, int)
{
}
