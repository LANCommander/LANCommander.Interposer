cmake_minimum_required(VERSION 3.25)

project(LANCommander.Interposer LANGUAGES C CXX)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "LANCommander.Interposer builds for Windows and Linux.")
endif()

# The pushed tag in CI; empty means "ask git" (see build/GenerateVersion.cmake).
set(INTERPOSER_VERSION "" CACHE STRING "Version tag to stamp into resources, e.g. v1.2.3")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Everything that runs lands in one directory per configuration: the DLL, the
# injector and the test EXE side by side (the EXE loads the DLL from its own
# directory), with the plugins under Plugins\ as the release ZIP lays them out.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

if(NOT WIN32)
    # Linux: libLANCommander.Interposer.so, preloaded into the game. Its build
    # settings live with the target; there is no Windows-style resource or XP
    # compatibility layer to set up.
    add_compile_options(-Wall -Wextra)
    add_subdirectory(LANCommander.Interposer)
    return()
endif()

# Static CRT on everything that ships, matching the windows-static triplets the
# dependencies are built with (see build/Compatibility.cmake). The test EXE
# overrides this.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# A PDB for Release too, as the vcxproj files always produced.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# What the vcxproj files set: /W3, SDL checks, conformance mode; and for Release
# function-level linking, intrinsics and whole-program optimization. /Zc:inline
# is an MSBuild default the VS generator does not carry over. /LTCG:incremental
# rather than /LTCG is MSBuild's Release default too, and not cosmetic: full
# /LTCG inlines more aggressively across modules and grows the DLL by ~85 KB.
add_compile_options(/W3 /sdl /permissive- /Zc:inline "$<$<CONFIG:Release>:/Gy;/Oi;/GL>")
add_link_options(/DEBUG "$<$<CONFIG:Release>:/LTCG:incremental;/OPT:REF;/OPT:ICF>")

include(build/Compatibility.cmake)

# version_info.h, regenerated on every build so a new tag needs no reconfigure.
set(INTERPOSER_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
add_custom_target(interposer_version
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT=${INTERPOSER_GENERATED_DIR}/version_info.h"
        "-DVERSION=${INTERPOSER_VERSION}"
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/build/GenerateVersion.cmake"
    BYPRODUCTS "${INTERPOSER_GENERATED_DIR}/version_info.h"
    COMMENT "Generating version_info.h"
    VERBATIM)

# Every target with a resource.rc includes version_info.h.
function(interposer_version_resource target)
    add_dependencies(${target} interposer_version)
    target_include_directories(${target} PRIVATE "${INTERPOSER_GENERATED_DIR}")
endfunction()

add_subdirectory(LANCommander.Interposer)
add_subdirectory(LANCommander.Interposer.Plugin.CDKey)
add_subdirectory(LANCommander.Interposer.Plugin.Example)
add_subdirectory(LANCommander.Interposer.Plugin.Mouse)
