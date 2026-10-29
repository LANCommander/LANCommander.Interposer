find_package(yaml-cpp CONFIG REQUIRED)

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
)

if(NOT WIN32)
    include(Linux.cmake)
    return()
endif()

find_package(minhook CONFIG REQUIRED)

# ── The hook DLL ──────────────────────────────────────────────────────────────

add_library(LANCommander.Interposer SHARED
    ${INTERPOSER_CORE_SOURCES}
    dll/src/platform/win/hooking.cpp
    dll/src/platform/win/http.cpp
    dll/src/platform/win/platform.cpp
    dll/src/win/audio.cpp
    dll/src/win/control.cpp
    dll/src/win/dinput.cpp
    dll/src/win/discord_ipc.cpp
    dll/src/win/dllmain.cpp
    dll/src/win/files.cpp
    dll/src/win/hooks.cpp
    dll/src/win/identity.cpp
    dll/src/win/instance.cpp
    dll/src/win/ipx.cpp
    dll/src/win/network.cpp
    dll/src/win/osversion.cpp
    dll/src/win/proxy.cpp
    dll/src/win/registry.cpp
    dll/src/win/relay.cpp
    dll/src/win/richpresence.cpp
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
    minhook::minhook yaml-cpp::yaml-cpp winhttp ws2_32 version)

interposer_ship_target(LANCommander.Interposer)
interposer_version_resource(LANCommander.Interposer)

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
