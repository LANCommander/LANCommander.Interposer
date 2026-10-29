/*
 * LANCommander.Interposer.Export.h -- how the Interposer's exports are declared.
 *
 * Shared by LANCommander.Interposer.h and the voice headers
 * (LANCommander.Interposer.Comms*.h), so every function the Interposer exports
 * is marked by one macro and called with one convention. Included by those
 * headers; there is no reason to include it directly.
 *
 * Deliberately self-contained -- no <windows.h>, no Win32 type names -- so the
 * voice headers stay as light as they were: C89, nothing a game SDK built with
 * an old toolchain could collide with.
 */
#ifndef LANCOMMANDER_INTERPOSER_EXPORT_H
#define LANCOMMANDER_INTERPOSER_EXPORT_H

#ifdef _WIN32
   /* INTERPOSER_STATIC: the consumer links LANCommander.Interposer.Static into
    * its own image (see LANCommander.Interposer.Embedded.h), so there is no
    * import to go through. The library itself is still built with
    * INTERPOSER_BUILDING_DLL: the exports leave through the embedding image. */
#  if defined(INTERPOSER_BUILDING_DLL)
#    define INTERPOSER_API __declspec(dllexport)
#  elif defined(INTERPOSER_STATIC)
#    define INTERPOSER_API
#  else
#    define INTERPOSER_API __declspec(dllimport)
#  endif
   /* Explicit, so a 32-bit consumer built with /Gz or /Gr still calls correctly. */
#  define INTERPOSER_CALL    __cdecl
   /* Callbacks the Interposer calls that predate INTERPOSER_CALL: WINAPI. */
#  define INTERPOSER_STDCALL __stdcall
#else
#  define INTERPOSER_API __attribute__((visibility("default")))
#  define INTERPOSER_CALL
#  define INTERPOSER_STDCALL
#endif

#endif /* LANCOMMANDER_INTERPOSER_EXPORT_H */
