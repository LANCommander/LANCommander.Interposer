#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// What DLL_PROCESS_ATTACH / DLL_PROCESS_DETACH do, for both images the
// Interposer is built into: its own DllMain (dllmain.cpp), and a patch DLL that
// embeds LANCommander.Interposer.Static and calls InterposerEmbeddedAttach /
// InterposerEmbeddedDetach from its DllMain instead.

// Claim the process and install everything. Returns false when another copy
// already owns it, in which case nothing was installed.
bool AttachInterposer(HMODULE self);

// Undo AttachInterposer. A no-op for a copy that deferred to another.
void DetachInterposer(bool processTerminating);
