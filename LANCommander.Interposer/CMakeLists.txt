# Feature switches for the embeddable library, and the per-feature source lists
# every target here is built from (Features.cmake).
include(${CMAKE_CURRENT_SOURCE_DIR}/Features.cmake)

find_package(yaml-cpp CONFIG REQUIRED)

# Voice (core/voice/, the interposer_comms_* API): Opus, ENet and miniaudio from
# vcpkg. miniaudio and opus ship no CMake config, so they are located directly.
# Not looked for at all by a build that only makes an embeddable library and
# has not turned voice on, so such a consumer need not install them.
if(INTERPOSER_EMBEDDED_ONLY AND NOT INTERPOSER_FEATURE_VOICE)
    set(INTERPOSER_VOICE_LIBRARIES)
else()
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
endif()

# Every source a full build carries (INTERPOSER_ALL_SOURCES) splits into core/,
# which is portable and reaches the OS only through platform/platform.h, and the
# platform's own: win/ and platform/win/ here, linux/ and platform/linux/ in
# Linux.cmake.
set(INTERPOSER_CORE_SOURCES ${INTERPOSER_ALL_SOURCES})
list(FILTER INTERPOSER_CORE_SOURCES INCLUDE REGEX "/dll/src/core/")

if(NOT WIN32)
    include(Linux.cmake)
    return()
endif()

find_package(minhook CONFIG REQUIRED)

# ── The embeddable library ────────────────────────────────────────────────────
#
# The same Interposer, for a patch that ships as one file with it inside (see
# include/LANCommander.Interposer.Embedded.h). No DllMain -- the patch calls
# InterposerEmbeddedAttach/Detach from its own -- and no proxy exports, which
# would collide with the patch's. The INTERPOSER_FEATURE_<NAME> options decide
# what it carries (Features.cmake): each is off unless the consumer turns it on.
#
# Link LANCommander::Interposer::Embedded, not this: it links the archive whole,
# without which every object the patch does not call into would be dropped and
# its exports with it.

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

# ── The hook DLL ──────────────────────────────────────────────────────────────

# Every feature, plus the entry point and the proxy exports.
add_library(LANCommander.Interposer SHARED
    ${INTERPOSER_ALL_SOURCES}
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
# configuration the embeddable library has to link into. It embeds every
# feature, named explicitly rather than through the INTERPOSER_FEATURE_<NAME>
# options, which are all off unless a consumer turns them on; and it is linked
# the way LANCommander::Interposer::Embedded links the library those options
# make.
interposer_add_embedded_library(LANCommander.Interposer.Static.Full FEATURES ${INTERPOSER_FEATURES})

add_library(LANCommander.Interposer.Tests.Patch SHARED
    tests/embedded/patch.c
    tests/embedded/patch.def
)

target_compile_definitions(LANCommander.Interposer.Tests.Patch PRIVATE INTERPOSER_STATIC)
target_link_libraries(LANCommander.Interposer.Tests.Patch PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,LANCommander.Interposer.Static.Full>")
interposer_ship_target(LANCommander.Interposer.Tests.Patch)

add_dependencies(LANCommander.Interposer.Tests LANCommander.Interposer.Tests.Patch)

# The same patch, embedding an Interposer built with nothing but the file hooks
# (the trimmed EM- tests): every other feature is its stubs.
interposer_add_embedded_library(LANCommander.Interposer.Static.Trimmed FEATURES FILES)

add_library(LANCommander.Interposer.Tests.PatchTrimmed SHARED
    tests/embedded/patch.c
    tests/embedded/patch.def
)

target_compile_definitions(LANCommander.Interposer.Tests.PatchTrimmed PRIVATE INTERPOSER_STATIC)
target_link_libraries(LANCommander.Interposer.Tests.PatchTrimmed PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,LANCommander.Interposer.Static.Trimmed>")
interposer_ship_target(LANCommander.Interposer.Tests.PatchTrimmed)

add_dependencies(LANCommander.Interposer.Tests LANCommander.Interposer.Tests.PatchTrimmed)

# Optionally, the test patch once per feature left out (and once with none at
# all), so a missing guard or stub shows up as a compile or link error rather
# than in a patch author's build. Not run by default: it builds the library
# fifteen more times. A feature's dependents go with it.
option(INTERPOSER_FEATURE_MATRIX "Also build the test patch without each feature in turn" OFF)

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

        add_library(LANCommander.Interposer.Tests.Patch${variant} SHARED
            tests/embedded/patch.c
            tests/embedded/patch.def)
        target_compile_definitions(LANCommander.Interposer.Tests.Patch${variant} PRIVATE INTERPOSER_STATIC)
        target_link_libraries(LANCommander.Interposer.Tests.Patch${variant} PRIVATE
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,LANCommander.Interposer.Static.${variant}>")
        interposer_ship_target(LANCommander.Interposer.Tests.Patch${variant})
        set_target_properties(LANCommander.Interposer.Tests.Patch${variant} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/feature-matrix/$<CONFIG>")
    endforeach()
endif()
