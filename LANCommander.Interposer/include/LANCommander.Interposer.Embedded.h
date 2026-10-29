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
 * Windows only.
 */
#ifndef LANCOMMANDER_INTERPOSER_EMBEDDED_H
#define LANCOMMANDER_INTERPOSER_EMBEDDED_H

#include "LANCommander.Interposer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* From DllMain, DLL_PROCESS_ATTACH, before the patch's own hooks: what
 * LANCommander.Interposer.dll does in its DLL_PROCESS_ATTACH. module is the
 * patch's own HINSTANCE. Returns TRUE when this copy now owns the process and
 * installed its hooks; FALSE when another Interposer already did, in which
 * case nothing was installed and every Interposer* call forwards to that one.
 * Either way the API is usable once this returns. Call it once. */
BOOL INTERPOSER_CALL InterposerEmbeddedAttach(HMODULE module);

/* From DllMain, DLL_PROCESS_DETACH, after the patch has removed its own hooks.
 * processTerminating is (lpvReserved != NULL). Does nothing when attach
 * returned FALSE. */
void INTERPOSER_CALL InterposerEmbeddedDetach(BOOL processTerminating);

#ifdef __cplusplus
}
#endif

#endif /* LANCOMMANDER_INTERPOSER_EMBEDDED_H */
