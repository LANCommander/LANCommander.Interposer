# Linux: libLANCommander.Interposer.so. LD_PRELOAD loads it into a game the way
# injection does on Windows; a patch can also link it like any shared library.

find_package(Threads REQUIRED)

# ── The shared object ─────────────────────────────────────────────────────────

add_library(LANCommander.Interposer SHARED
    ${INTERPOSER_CORE_SOURCES}
    dll/src/platform/linux/hooking.cpp
    dll/src/platform/linux/http.cpp
    dll/src/platform/linux/platform.cpp
    dll/src/linux/files.cpp
    dll/src/linux/hooks.cpp
    dll/src/linux/instance.cpp
    dll/src/linux/network.cpp
    dll/src/linux/unsupported.cpp
    # Last, always: its constructor must run after every other file's static
    # initialisers, and .init_array runs in link order. See the top of main.cpp.
    dll/src/linux/main.cpp
)

# Only what INTERPOSER_API marks is exported, so the public API is the whole
# dynamic symbol table and nothing of ours interposes on the game's symbols.
set_target_properties(LANCommander.Interposer PROPERTIES
    OUTPUT_NAME               "LANCommander.Interposer"
    C_VISIBILITY_PRESET       hidden
    CXX_VISIBILITY_PRESET     hidden
    VISIBILITY_INLINES_HIDDEN ON)

target_compile_definitions(LANCommander.Interposer PRIVATE INTERPOSER_BUILDING_DLL)
target_include_directories(LANCommander.Interposer
    PRIVATE dll/src/core dll/src/platform dll/src/linux
    PUBLIC  include)
target_link_libraries(LANCommander.Interposer PRIVATE
    yaml-cpp::yaml-cpp Threads::Threads ${CMAKE_DL_LIBS})
interposer_version_resource(LANCommander.Interposer)

# The Linux counterpart of /MT. The C++ runtime is linked in statically and kept
# out of the dynamic symbol table (--exclude-libs), because the process it lands
# in may carry a libstdc++ of its own -- an old game server ships whatever it was
# built against -- and ours must neither require a newer one nor interpose on it.
# yaml-cpp is hidden the same way.
target_link_options(LANCommander.Interposer PRIVATE
    -static-libstdc++ -static-libgcc
    "LINKER:--exclude-libs,ALL"
    "LINKER:--no-undefined"
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/dll/src/linux/exports.map")
set_property(TARGET LANCommander.Interposer APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/dll/src/linux/exports.map")

# ── The test harness ──────────────────────────────────────────────────────────
#
# The Windows test EXE is Win32 through and through, so Linux has its own, built
# from the same idea: write fixtures, load the .so, call it, read the log back.

add_executable(LANCommander.Interposer.Tests tests/linux/main.cpp)
target_include_directories(LANCommander.Interposer.Tests PRIVATE include)
target_link_libraries(LANCommander.Interposer.Tests PRIVATE ${CMAKE_DL_LIBS})

# A stand-in for a game patch: C, linked against the .so by its soname.
add_library(LANCommander.Interposer.SdkProbe SHARED tests/linux/sdk_probe.c)
target_link_libraries(LANCommander.Interposer.SdkProbe PRIVATE LANCommander.Interposer)

# A plugin. The harness copies it into .interposer/Plugins/.
add_library(LANCommander.Interposer.TestPlugin SHARED tests/linux/test_plugin.c)
target_include_directories(LANCommander.Interposer.TestPlugin PRIVATE include)

add_dependencies(LANCommander.Interposer.Tests
    LANCommander.Interposer LANCommander.Interposer.SdkProbe LANCommander.Interposer.TestPlugin)
