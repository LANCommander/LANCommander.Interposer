# Linux: libLANCommander.Interposer.so. LD_PRELOAD loads it into a game the way
# injection does on Windows; a patch can also link it like any shared library.

find_package(Threads REQUIRED)

# ── The embeddable library ────────────────────────────────────────────────────
#
# The same Interposer for a host shared object that carries it inside (a game
# server plugin): see include/LANCommander.Interposer.Embedded.h. No main.cpp --
# nothing runs at load; the host calls InterposerEmbeddedAttach/Detach
# (linux/embedded.cpp). The INTERPOSER_FEATURE_<NAME> options decide what it
# carries (Features.cmake): each is off unless the consumer turns it on.
#
# Link LANCommander::Interposer::Embedded, not this: it links the archive whole,
# and passes yaml-cpp, the voice libraries and the system libraries on. The host
# should link -static-libstdc++ -static-libgcc and export only its own entry
# points plus Interposer* (a version script), as the .so does with exports.map.

interposer_add_embedded_library(LANCommander.Interposer.Static)

add_library(LANCommander.Interposer.Embedded INTERFACE)
add_library(LANCommander::Interposer::Embedded ALIAS LANCommander.Interposer.Embedded)
target_link_libraries(LANCommander.Interposer.Embedded INTERFACE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,LANCommander.Interposer.Static>")
target_compile_definitions(LANCommander.Interposer.Embedded INTERFACE INTERPOSER_STATIC)

# A consumer that only embeds (INTERPOSER_EMBEDDED_ONLY, see the root
# CMakeLists.txt) needs nothing below.
if(INTERPOSER_EMBEDDED_ONLY)
    return()
endif()

# ── The shared object ─────────────────────────────────────────────────────────

add_library(LANCommander.Interposer SHARED
    ${INTERPOSER_ALL_SOURCES}
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
    yaml-cpp::yaml-cpp Threads::Threads ${CMAKE_DL_LIBS} m
    ${INTERPOSER_VOICE_LIBRARIES})
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
target_link_libraries(LANCommander.Interposer.Tests PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

# A stand-in for a game patch: C, linked against the .so by its soname.
add_library(LANCommander.Interposer.SdkProbe SHARED tests/linux/sdk_probe.c)
target_link_libraries(LANCommander.Interposer.SdkProbe PRIVATE LANCommander.Interposer)

# A plugin. The harness copies it into .interposer/Plugins/.
add_library(LANCommander.Interposer.TestPlugin SHARED tests/linux/test_plugin.c)
target_include_directories(LANCommander.Interposer.TestPlugin PRIVATE include)

add_dependencies(LANCommander.Interposer.Tests
    LANCommander.Interposer LANCommander.Interposer.SdkProbe LANCommander.Interposer.TestPlugin)

# Optionally, an embedding host once per feature left out (and once with none at
# all), linked with --no-undefined, so a missing guard or stub fails the build
# rather than a host's. The Windows counterpart is in CMakeLists.txt.
option(INTERPOSER_FEATURE_MATRIX "Also build an embedding host without each feature in turn" OFF)

if(INTERPOSER_FEATURE_MATRIX)
    set(matrix ${INTERPOSER_FEATURES} NONE)

    foreach(left_out IN LISTS matrix)
        if(left_out STREQUAL "NONE")
            set(features)
            set(variant NoFeatures)
        else()
            set(variant Without${left_out})
            set(features ${INTERPOSER_FEATURES})
            list(REMOVE_ITEM features ${left_out})

            foreach(feature IN LISTS INTERPOSER_FEATURES)
                if(left_out IN_LIST INTERPOSER_FEATURE_${feature}_DEPENDS)
                    list(REMOVE_ITEM features ${feature})
                endif()
            endforeach()
        endif()

        interposer_add_embedded_library(LANCommander.Interposer.Static.${variant} FEATURES ${features})

        add_library(LANCommander.Interposer.EmbeddedProbe${variant} SHARED tests/linux/embedded_probe.c)
        target_compile_definitions(LANCommander.Interposer.EmbeddedProbe${variant} PRIVATE INTERPOSER_STATIC)
        target_link_libraries(LANCommander.Interposer.EmbeddedProbe${variant} PRIVATE
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,LANCommander.Interposer.Static.${variant}>")
        target_link_options(LANCommander.Interposer.EmbeddedProbe${variant} PRIVATE
            -static-libstdc++ -static-libgcc "LINKER:--no-undefined")
        set_target_properties(LANCommander.Interposer.EmbeddedProbe${variant} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/feature-matrix/$<CONFIG>")
    endforeach()
endif()
