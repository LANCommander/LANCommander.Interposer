/*
 * LANCommander.Interposer.Embedded.h -- the Interposer compiled into a patch.
 *
 * For a game patch that ships as ONE file -- a dinput.dll proxy, an .asi --
 * and wants the Interposer inside it rather than beside it. The patch links
 * LANCommander.Interposer.Static whole (MSVC: /WHOLEARCHIVE; in this repo's
 * CMake, the LANCommander::Interposer::Embedded target does both that and the
 * INTERPOSER_STATIC define), and calls the two functions below from its own
 * DllMain. Everything else is the ordinary API in LANCommander.Interposer.h,
 * called directly.
 *
 * Linking the whole archive is not optional. Every Interposer* function is
 * exported from the patch's image, as it is from LANCommander.Interposer.dll:
 * a second Interposer in the same process -- LANCommander's own, deployed as
 * version.dll -- finds the first one's functions by name, and so do the
 * plugins in .interposer\Plugins\. An object the linker dropped because the
 * patch never called into it would take its exports with it.
 *
 * The embedded Interposer behaves as though it were the DLL deployed under the
 * patch's file name. Its base directory is the patch's directory, so its
 * configuration is the .interposer\Config.yml beside the patch, its plugins
 * load from the .interposer\Plugins\ there, and with neither its hooks are
 * installed but change nothing. A Debug build of the library waits for a
 * debugger in InterposerEmbeddedAttach, exactly as the Debug DLL does.
 *
 * Linux: the same arrangement for a host shared object -- a game server
 * plugin -- linking LANCommander.Interposer.Static whole (the same CMake target
 * does it, with -Wl,--whole-archive). There is no DllMain to call from: call
 * InterposerEmbeddedAttach once the host is loaded (its init hook, after every
 * static constructor has run) and InterposerEmbeddedDetach before it unloads.
 * Attach brings up the config, the session log, the launcher's event channel,
 * match statistics and plugins -- the API, not the hooks: the libc
 * interposers are linked in but stay disabled. The host must export
 * Interposer* from its dynamic symbol table (a version script naming them),
 * for the same reason the Windows image must: a second copy and the plugins
 * find the first by name.
 */
#ifndef LANCOMMANDER_INTERPOSER_EMBEDDED_H
#define LANCOMMANDER_INTERPOSER_EMBEDDED_H

#include "LANCommander.Interposer.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32

/* From DllMain, DLL_PROCESS_ATTACH, before the patch's own hooks: what
 * LANCommander.Interposer.dll does in its DLL_PROCESS_ATTACH. module is the
 * patch's own HINSTANCE. Returns TRUE when this copy now owns the process and
 * installed its hooks; FALSE when another Interposer already did, in which
 * case nothing was installed and every Interposer* call forwards to that one.
 * Either way the API is usable once this returns. Call it once. */
BOOL INTERPOSER_CALL InterposerEmbeddedAttach(HMODULE module);

/* Instead of InterposerEmbeddedAttach, for a host that only uses the API -- a
 * game server plugin reporting matches -- and installs no hooks: the config,
 * the session log, the launcher's event channel, match statistics and
 * plugins, and no detour anywhere. With no detour pointing into it, the host
 * may be unloaded while the process runs on. Need not be called from DllMain;
 * call it once the host is loaded. Returns as InterposerEmbeddedAttach does,
 * and is undone by the same InterposerEmbeddedDetach. What
 * InterposerEmbeddedAttach always does on Linux. */
BOOL INTERPOSER_CALL InterposerEmbeddedAttachApiOnly(HMODULE module);

/* From DllMain, DLL_PROCESS_DETACH, after the patch has removed its own hooks.
 * processTerminating is (lpvReserved != NULL). Does nothing when attach
 * returned FALSE. */
void INTERPOSER_CALL InterposerEmbeddedDetach(BOOL processTerminating);

#else

/* Once the host is loaded. Returns TRUE when this copy now owns the process;
 * FALSE when another Interposer (a preloaded one) already does, in which case
 * every Interposer* call forwards to that one. Either way the API is usable
 * once this returns. Calling it again does nothing. */
BOOL INTERPOSER_CALL InterposerEmbeddedAttach(void);

/* Before the host unloads. processTerminating skips stopping the match
 * statistics thread, as at process exit. Does nothing when attach returned
 * FALSE. */
void INTERPOSER_CALL InterposerEmbeddedDetach(BOOL processTerminating);

#endif

#ifdef __cplusplus
}
#endif

#endif /* LANCOMMANDER_INTERPOSER_EMBEDDED_H */
