find_package(minhook CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

# ── The hook DLL ──────────────────────────────────────────────────────────────

add_library(LANCommander.Interposer SHARED
    dll/src/audio.cpp
    dll/src/callbacks.cpp
    dll/src/config.cpp
    dll/src/control.cpp
    dll/src/dinput.cpp
    dll/src/discord_ipc.cpp
    dll/src/dllmain.cpp
    dll/src/fastdl.cpp
    dll/src/files.cpp
    dll/src/hooks.cpp
    dll/src/identity.cpp
    dll/src/instance.cpp
    dll/src/ipx.cpp
    dll/src/network.cpp
    dll/src/osversion.cpp
    dll/src/pipe_events.cpp
    dll/src/plugins.cpp
    dll/src/proxy.cpp
    dll/src/registry.cpp
    dll/src/relay.cpp
    dll/src/richpresence.cpp
    # The union of version.dll, dinput8.dll and dinput.dll's exports; also what
    # keeps them out of the import library (see "One DLL, four names").
    dll/src/proxy.def
    dll/src/resource.rc
)

target_compile_definitions(LANCommander.Interposer PRIVATE _USRDLL INTERPOSER_BUILDING_DLL)
target_include_directories(LANCommander.Interposer
    PRIVATE dll/src
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
