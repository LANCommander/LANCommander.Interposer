find_package(yaml-cpp CONFIG REQUIRED)

# Voice (core/voice/, the interposer_comms_* API): Opus, ENet and miniaudio from
# vcpkg. miniaudio and opus ship no CMake config, so they are located directly.
find_package(unofficial-enet CONFIG REQUIRED)

find_path(OPUS_INCLUDE_DIR opus.h PATH_SUFFIXES opus REQUIRED)
set(INTERPOSER_VCPKG_PREFIX ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET})
find_library(OPUS_LIBRARY_RELEASE opus PATHS ${INTERPOSER_VCPKG_PREFIX}/lib NO_DEFAULT_PATH REQUIRED)
find_library(OPUS_LIBRARY_DEBUG   opus PATHS ${INTERPOSER_VCPKG_PREFIX}/debug/lib NO_DEFAULT_PATH REQUIRED)
add_library(interposer_opus INTERFACE)
target_include_directories(interposer_opus INTERFACE ${OPUS_INCLUDE_DIR})
target_link_libraries(interposer_opus INTERFACE optimized ${OPUS_LIBRARY_RELEASE} debug ${OPUS_LIBRARY_DEBUG})

find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
add_library(interposer_miniaudio INTERFACE)
target_include_directories(interposer_miniaudio INTERFACE ${MINIAUDIO_INCLUDE_DIR})

set(INTERPOSER_VOICE_LIBRARIES interposer_opus unofficial::enet::enet interposer_miniaudio)

# core/ is portable: it reaches the OS only through platform/platform.h. win/ is
# everything Windows-only -- the hook subsystems, DllMain, the init sequence --
# and platform/win/ implements the platform layer; linux/ and platform/linux/
# are the same for Linux (Linux.cmake).
set(INTERPOSER_CORE_SOURCES
    dll/src/core/callbacks.cpp
    dll/src/core/config.cpp
    dll/src/core/fastdl.cpp
    dll/src/core/file_access.cpp
    dll/src/core/network_filter.cpp
    dll/src/core/pipe_events.cpp
    dll/src/core/plugins.cpp
    # Voice chat. Portable like the rest of core/, but through ENet and
    # miniaudio rather than platform.h: those are its platform layer.
    dll/src/core/voice/common/log.cpp
    dll/src/core/voice/common/miniaudio_impl.cpp
    dll/src/core/voice/common/version.cpp
    dll/src/core/voice/server/server_admin.cpp
    dll/src/core/voice/server/server_api.cpp
    dll/src/core/voice/server/server_groups.cpp
    dll/src/core/voice/server/server_limits.cpp
    dll/src/core/voice/server/server_network.cpp
    dll/src/core/voice/server/spatial_grid.cpp
    dll/src/core/voice/client/audio_devices.cpp
    dll/src/core/voice/client/client_api.cpp
    dll/src/core/voice/client/client_audio.cpp
    dll/src/core/voice/client/client_groups.cpp
    dll/src/core/voice/client/client_network.cpp
    dll/src/core/voice/client/client_voice.cpp
)

if(NOT WIN32)
    include(Linux.cmake)
    return()
endif()

find_package(minhook CONFIG REQUIRED)

# ── The hook DLL ──────────────────────────────────────────────────────────────

# Everything but the entry point and the proxy exports, shared by the DLL and by
# the static library a patch embeds.
set(INTERPOSER_WIN_SOURCES
    dll/src/platform/win/hooking.cpp
    dll/src/platform/win/http.cpp
    dll/src/platform/win/platform.cpp
    dll/src/win/attach.cpp
    dll/src/win/audio.cpp
    dll/src/win/control.cpp
    dll/src/win/dinput.cpp
    dll/src/win/discord_ipc.cpp
    dll/src/win/files.cpp
    dll/src/win/hooks.cpp
    dll/src/win/identity.cpp
    dll/src/win/instance.cpp
    dll/src/win/ipx.cpp
    dll/src/win/network.cpp
    dll/src/win/osversion.cpp
    dll/src/win/registry.cpp
    dll/src/win/relay.cpp
    dll/src/win/richpresence.cpp
)

add_library(LANCommander.Interposer SHARED
    ${INTERPOSER_CORE_SOURCES}
    ${INTERPOSER_WIN_SOURCES}
    dll/src/win/dllmain.cpp
    dll/src/win/proxy.cpp
    # The union of version.dll, dinput8.dll and dinput.dll's exports; also what
    # keeps them out of the import library (see "One DLL, four names").
    dll/src/win/proxy.def
    dll/src/win/resource.rc
)

# Portable code uses the standard C functions, not the MSVC _s variants, which
# /sdl would otherwise turn into C4996 errors.
set_source_files_properties(${INTERPOSER_CORE_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS _CRT_SECURE_NO_WARNINGS)

target_compile_definitions(LANCommander.Interposer PRIVATE _USRDLL INTERPOSER_BUILDING_DLL)
target_include_directories(LANCommander.Interposer
    PRIVATE dll/src/core dll/src/platform dll/src/win
    PUBLIC  include)
target_link_libraries(LANCommander.Interposer PRIVATE
    minhook::minhook yaml-cpp::yaml-cpp winhttp ws2_32 version winmm
    ${INTERPOSER_VOICE_LIBRARIES})

interposer_ship_target(LANCommander.Interposer)
interposer_version_resource(LANCommander.Interposer)

# ── The embeddable library ────────────────────────────────────────────────────
#
# The same Interposer, for a patch that ships as one file with it inside (see
# include/LANCommander.Interposer.Embedded.h). No DllMain -- the patch calls
# InterposerEmbeddedAttach/Detach from its own -- and no proxy exports, which
# would collide with the patch's. Still built with INTERPOSER_BUILDING_DLL, so
# the Interposer* exports leave through the patch's image; and without /GL, so
# its objects are not tied to the exact compiler that built them.
#
# Link LANCommander::Interposer::Embedded, not this: it links the archive whole,
# without which every object the patch does not call into would be dropped and
# its exports with it.

add_library(LANCommander.Interposer.Static STATIC
    ${INTERPOSER_CORE_SOURCES}
    ${INTERPOSER_WIN_SOURCES}
)

set_target_properties(LANCommander.Interposer.Static PROPERTIES INTERPOSER_NO_LTCG ON)
target_compile_definitions(LANCommander.Interposer.Static PRIVATE INTERPOSER_BUILDING_DLL)
target_include_directories(LANCommander.Interposer.Static
    PRIVATE dll/src/core dll/src/platform dll/src/win
    PUBLIC  include)
target_link_libraries(LANCommander.Interposer.Static PRIVATE
    minhook::minhook yaml-cpp::yaml-cpp winhttp ws2_32 version winmm
    ${INTERPOSER_VOICE_LIBRARIES})

interposer_version_resource(LANCommander.Interposer.Static)

add_library(LANCommander.Interposer.Embedded INTERFACE)
add_library(LANCommander::Interposer::Embedded ALIAS LANCommander.Interposer.Embedded)
target_link_libraries(LANCommander.Interposer.Embedded INTERFACE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,LANCommander.Interposer.Static>")
target_compile_definitions(LANCommander.Interposer.Embedded INTERFACE INTERPOSER_STATIC)

# ── The injector ──────────────────────────────────────────────────────────────

add_executable(LANCommander.Interposer.Injector
    injector/src/main.cpp
    injector/src/resource.rc
)

interposer_ship_target(LANCommander.Interposer.Injector)
interposer_version_resource(LANCommander.Interposer.Injector)

# ── The test EXE ──────────────────────────────────────────────────────────────
#
# A dev harness that never ships, so it is deliberately not an
# interposer_ship_target: dynamic CRT, no XP constraints. It calls CreateFile2
# and std::filesystem directly.

add_executable(LANCommander.Interposer.Tests
    tests/src/main.cpp
    # The SDK probe: C, stdcall by default, against the public header -- the way
    # a game patch compiles it.
    tests/src/sdk.c
)

set_source_files_properties(tests/src/sdk.c PROPERTIES COMPILE_OPTIONS "/Gz")
set_property(TARGET LANCommander.Interposer.Tests PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Linked the way a game patch links it: through the import library. Delay-loaded
# so the fixtures are written before anything binds; see sdk.c.
target_link_libraries(LANCommander.Interposer.Tests PRIVATE LANCommander.Interposer delayimp)
target_link_options(LANCommander.Interposer.Tests PRIVATE
    /DELAYLOAD:LANCommander.Interposer.dll
    /DELAY:UNLOAD)

# A stand-in game patch with the Interposer embedded (the EM- tests). Built as a
# shipping patch would be -- C, /MT, the XP settings -- because that is the
# configuration the embeddable library has to link into.
add_library(LANCommander.Interposer.Tests.Patch SHARED
    tests/embedded/patch.c
    tests/embedded/patch.def
)

target_link_libraries(LANCommander.Interposer.Tests.Patch PRIVATE LANCommander::Interposer::Embedded)
interposer_ship_target(LANCommander.Interposer.Tests.Patch)

add_dependencies(LANCommander.Interposer.Tests LANCommander.Interposer.Tests.Patch)
