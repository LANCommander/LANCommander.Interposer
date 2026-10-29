# Shared by every target that ships: the hook DLL, the injector and the plugins.
# Call interposer_ship_target(<target>) on each of them.
#
# Goal: one binary per architecture that loads on every Windows from XP SP3
# (x86) / XP x64 and Server 2003 (x64) up, with no runtime installed. Three
# parts, all load-bearing:
#
# * Static CRT (/MT). The Visual C++ Redistributable cannot be assumed on a
#   game machine, and the 14.x runtime does not install on XP at all. This is
#   safe for plugins because the plugin ABI is plain C with caller-owned
#   buffers - nothing allocated by one module is freed by another. Set
#   project-wide in the root CMakeLists.txt, because the vcpkg triplets build
#   the dependencies the same way.
#
# * Subsystem version 5.01 / 5.02. The linker defaults to 6.00, and the XP
#   loader rejects such an image outright as "not a valid Win32 application".
#
# * YY-Thunks. The v143 CRT and STL import Vista+ kernel32 functions directly
#   (SRW locks and condition variables behind std::mutex / std::shared_mutex,
#   among others), so the image would still fail to load with a missing-entry-
#   point error. YY_Thunks_for_WinXP.obj supplies those imports itself: on a
#   system that has the real function it forwards to it, on XP it falls back
#   to an equivalent. Its entry point additionally gives a LoadLibrary-loaded
#   DLL working thread_local on XP, where the loader only allocates implicit
#   TLS for modules present at process start - without it the injector and
#   ASI load methods would fault on first access.
#
# Fetch YY-Thunks with build\Get-YYThunks.ps1, which is also where its version
# is pinned (beside the archive's SHA-256). Verify a build with
# build\Test-XPImports.ps1, which checks every import against the XP export
# lists that ship with YY-Thunks.

file(READ "${CMAKE_CURRENT_LIST_DIR}/Get-YYThunks.ps1" _yy_script)
if(NOT _yy_script MATCHES "\\$Version = '([0-9.]+)'")
    message(FATAL_ERROR "Could not read the YY-Thunks version from build/Get-YYThunks.ps1")
endif()
set(INTERPOSER_YYTHUNKS_VERSION "${CMAKE_MATCH_1}")
unset(_yy_script)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(_yy_arch x86)
    set(INTERPOSER_XP_SUBSYSTEM 5.01)
else()
    set(_yy_arch x64)
    set(INTERPOSER_XP_SUBSYSTEM 5.02)
endif()

set(INTERPOSER_YYTHUNKS_OBJ
    "${CMAKE_CURRENT_LIST_DIR}/YY-Thunks/${INTERPOSER_YYTHUNKS_VERSION}/objs/${_yy_arch}/YY_Thunks_for_WinXP.obj")

if(NOT EXISTS "${INTERPOSER_YYTHUNKS_OBJ}")
    message(FATAL_ERROR
        "YY-Thunks ${INTERPOSER_YYTHUNKS_VERSION} not found at ${INTERPOSER_YYTHUNKS_OBJ}. Run: "
        "powershell -NoProfile -ExecutionPolicy Bypass -File build\\Get-YYThunks.ps1")
endif()

function(interposer_ship_target target)
    get_target_property(_type ${target} TYPE)

    target_sources(${target} PRIVATE "${INTERPOSER_YYTHUNKS_OBJ}")

    if(_type STREQUAL "SHARED_LIBRARY")
        target_link_options(${target} PRIVATE "/SUBSYSTEM:WINDOWS,${INTERPOSER_XP_SUBSYSTEM}")

        # The thread_local fix: YY-Thunks' entry point runs first and then chains
        # to the CRT's, which it finds through the alternatename.
        target_link_options(${target} PRIVATE "/ENTRY:DllMainCRTStartupForYY_Thunks")
        if(CMAKE_SIZEOF_VOID_P EQUAL 4)
            target_link_options(${target} PRIVATE
                "/alternatename:_YY_ThunksOriginalDllMainCRTStartup@12=__DllMainCRTStartup@12")
        else()
            target_link_options(${target} PRIVATE
                "/alternatename:YY_ThunksOriginalDllMainCRTStartup=_DllMainCRTStartup")
        endif()
    else()
        target_link_options(${target} PRIVATE "/SUBSYSTEM:CONSOLE,${INTERPOSER_XP_SUBSYSTEM}")
    endif()
endfunction()
