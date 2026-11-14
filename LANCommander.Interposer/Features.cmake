# Feature switches for the embeddable library.
#
# A patch that embeds the Interposer links it whole-archive, because the
# Interposer* exports have to leave through the patch's image -- so whatever
# the library carries ends up in the patch. Every feature is therefore opt-in:
# each INTERPOSER_FEATURE_<NAME> option below is OFF by default, and
# LANCommander.Interposer.Static carries only the subsystems turned on. A
# feature not turned on is replaced by its stubs (dll/src/core/stubs/,
# dll/src/win/stubs/): the same exports, answering as though the feature were
# switched off, so the API, plugins and a second copy's forwarding still work.
# What is always there is the core: config, the session log, the callbacks, the
# launcher's event channel and the API itself. See core/interposer_features.h.
#
# The shipped DLL and .so ignore these and always carry everything; only the
# embeddable library (and interposer_add_embedded_library, which the tests use
# for their own variants) is built from them. A consumer that pulls this
# repository in with add_subdirectory turns on what it uses first:
#
#     set(INTERPOSER_FEATURE_MATCHES ON CACHE BOOL "" FORCE)
#     add_subdirectory(LANCommander.Interposer interposer EXCLUDE_FROM_ALL)

set(INTERPOSER_FEATURES
    REGISTRY FILES FASTDL DIRECTINPUT AUDIO OSVERSION IDENTITY NETWORK
    RELAY RICH_PRESENCE MATCHES VOICE PLUGINS)

option(INTERPOSER_FEATURE_REGISTRY      "Embedded library: the virtual registry (Windows)" OFF)
option(INTERPOSER_FEATURE_FILES         "Embedded library: the file hooks and FileRedirects" OFF)
option(INTERPOSER_FEATURE_FASTDL        "Embedded library: FastDL downloads (needs FILES)" OFF)
option(INTERPOSER_FEATURE_DIRECTINPUT   "Embedded library: the DirectInput bridge, device filter and mouse transform (Windows)" OFF)
option(INTERPOSER_FEATURE_AUDIO         "Embedded library: the audio device filter (Windows)" OFF)
option(INTERPOSER_FEATURE_OSVERSION     "Embedded library: OS version reporting (Windows)" OFF)
option(INTERPOSER_FEATURE_IDENTITY      "Embedded library: the user and computer name hooks" OFF)
option(INTERPOSER_FEATURE_NETWORK       "Embedded library: DNS redirects, the adapter filter and enumeration" OFF)
option(INTERPOSER_FEATURE_RELAY         "Embedded library: the network relay and IPX emulation (Windows; needs NETWORK)" OFF)
option(INTERPOSER_FEATURE_RICH_PRESENCE "Embedded library: rich presence and Discord (Windows)" OFF)
option(INTERPOSER_FEATURE_MATCHES       "Embedded library: match statistics" OFF)
option(INTERPOSER_FEATURE_VOICE         "Embedded library: voice chat, with ENet, Opus and miniaudio" OFF)
option(INTERPOSER_FEATURE_PLUGINS       "Embedded library: loading .interposer/Plugins" OFF)

# What a feature needs: turning the first on without the second is a mistake the
# configure step reports, rather than quietly carrying more than was asked for.
set(INTERPOSER_FEATURE_FASTDL_DEPENDS FILES)
set(INTERPOSER_FEATURE_RELAY_DEPENDS  NETWORK)

set(INTERPOSER_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(WIN32)
    set(INTERPOSER_PLATFORM_DIR win)
else()
    set(INTERPOSER_PLATFORM_DIR linux)
endif()

set(_src "${CMAKE_CURRENT_LIST_DIR}/dll/src")

# ── Sources every build carries ───────────────────────────────────────────────
#
# core/ is portable: it reaches the OS only through platform/platform.h. voice's
# log and version are here rather than with VOICE because they are cheap, need
# none of its libraries, and let a patch still ask what it is talking to.

set(INTERPOSER_ALWAYS_CORE_SOURCES
    ${_src}/core/callbacks.cpp
    ${_src}/core/config.cpp
    ${_src}/core/interposer_features.cpp
    ${_src}/core/file_access.cpp
    ${_src}/core/json.cpp
    ${_src}/core/pipe_events.cpp
    ${_src}/core/voice/common/log.cpp
    ${_src}/core/voice/common/version.cpp)

set(INTERPOSER_ALWAYS_WIN_SOURCES
    ${_src}/platform/win/hooking.cpp
    ${_src}/platform/win/http.cpp
    ${_src}/platform/win/platform.cpp
    ${_src}/win/attach.cpp
    ${_src}/win/control.cpp
    ${_src}/win/hooks.cpp
    ${_src}/win/instance.cpp)

# On Linux the Windows-only features are always stubs.
set(INTERPOSER_ALWAYS_LINUX_SOURCES
    ${_src}/platform/linux/hooking.cpp
    ${_src}/platform/linux/http.cpp
    ${_src}/platform/linux/platform.cpp
    ${_src}/linux/hooks.cpp
    ${_src}/linux/instance.cpp
    ${_src}/core/stubs/audio_off.cpp
    ${_src}/core/stubs/mouse_off.cpp
    ${_src}/core/stubs/presence_off.cpp
    ${_src}/core/stubs/registry_off.cpp
    ${_src}/core/stubs/relay_off.cpp)

# ── Per feature: the real sources, and the stubs that stand in for them ──────
#
# INTERPOSER_FEATURE_<NAME>_CORE / _WIN / _LINUX are the sources the feature
# adds; _STUBS_WIN / _STUBS_LINUX replace them when it is off.

set(INTERPOSER_FEATURE_REGISTRY_WIN        ${_src}/win/registry.cpp)
set(INTERPOSER_FEATURE_REGISTRY_STUBS_WIN  ${_src}/core/stubs/registry_off.cpp)

set(INTERPOSER_FEATURE_FILES_WIN           ${_src}/win/files.cpp)
set(INTERPOSER_FEATURE_FILES_LINUX         ${_src}/linux/files.cpp)

set(INTERPOSER_FEATURE_FASTDL_CORE         ${_src}/core/fastdl.cpp)
set(INTERPOSER_FEATURE_FASTDL_STUBS_WIN    ${_src}/core/stubs/fastdl_off.cpp)
set(INTERPOSER_FEATURE_FASTDL_STUBS_LINUX  ${_src}/core/stubs/fastdl_off.cpp)

set(INTERPOSER_FEATURE_DIRECTINPUT_WIN       ${_src}/win/dinput.cpp)
set(INTERPOSER_FEATURE_DIRECTINPUT_STUBS_WIN ${_src}/win/stubs/dinput_off.cpp ${_src}/core/stubs/mouse_off.cpp)

set(INTERPOSER_FEATURE_AUDIO_WIN           ${_src}/win/audio.cpp)
set(INTERPOSER_FEATURE_AUDIO_STUBS_WIN     ${_src}/core/stubs/audio_off.cpp)

set(INTERPOSER_FEATURE_OSVERSION_WIN       ${_src}/win/osversion.cpp)

set(INTERPOSER_FEATURE_IDENTITY_WIN        ${_src}/win/identity.cpp)
set(INTERPOSER_FEATURE_IDENTITY_LINUX      ${_src}/linux/identity.cpp)

set(INTERPOSER_FEATURE_NETWORK_CORE        ${_src}/core/network_filter.cpp)
set(INTERPOSER_FEATURE_NETWORK_WIN         ${_src}/win/network.cpp)
set(INTERPOSER_FEATURE_NETWORK_LINUX       ${_src}/linux/network.cpp)
set(INTERPOSER_FEATURE_NETWORK_STUBS_WIN   ${_src}/core/stubs/network_off.cpp)
set(INTERPOSER_FEATURE_NETWORK_STUBS_LINUX ${_src}/core/stubs/network_off.cpp)

set(INTERPOSER_FEATURE_RELAY_WIN           ${_src}/win/relay.cpp ${_src}/win/ipx.cpp)
set(INTERPOSER_FEATURE_RELAY_STUBS_WIN     ${_src}/core/stubs/relay_off.cpp)

set(INTERPOSER_FEATURE_RICH_PRESENCE_WIN       ${_src}/win/richpresence.cpp ${_src}/win/discord_ipc.cpp)
set(INTERPOSER_FEATURE_RICH_PRESENCE_STUBS_WIN ${_src}/core/stubs/presence_off.cpp)

set(INTERPOSER_FEATURE_MATCHES_CORE        ${_src}/core/matches.cpp)
set(INTERPOSER_FEATURE_MATCHES_STUBS_WIN   ${_src}/core/stubs/matches_off.cpp)
set(INTERPOSER_FEATURE_MATCHES_STUBS_LINUX ${_src}/core/stubs/matches_off.cpp)

# Voice chat. Portable like the rest of core/, but through ENet and miniaudio
# rather than platform.h: those are its platform layer.
set(INTERPOSER_FEATURE_VOICE_CORE
    ${_src}/core/voice/common/miniaudio_impl.cpp
    ${_src}/core/voice/server/server_admin.cpp
    ${_src}/core/voice/server/server_api.cpp
    ${_src}/core/voice/server/server_groups.cpp
    ${_src}/core/voice/server/server_limits.cpp
    ${_src}/core/voice/server/server_network.cpp
    ${_src}/core/voice/server/spatial_grid.cpp
    ${_src}/core/voice/client/audio_devices.cpp
    ${_src}/core/voice/client/client_api.cpp
    ${_src}/core/voice/client/client_audio.cpp
    ${_src}/core/voice/client/client_groups.cpp
    ${_src}/core/voice/client/client_network.cpp
    ${_src}/core/voice/client/client_voice.cpp)
set(INTERPOSER_FEATURE_VOICE_STUBS_WIN     ${_src}/core/stubs/voice_off.cpp)
set(INTERPOSER_FEATURE_VOICE_STUBS_LINUX   ${_src}/core/stubs/voice_off.cpp)

set(INTERPOSER_FEATURE_PLUGINS_CORE        ${_src}/core/plugins.cpp)

unset(_src)

# interposer_feature_sources(<out-var> <ON|OFF per feature, in INTERPOSER_FEATURES order>...)
#
# The sources for a build with the given features: the always-on set, the real
# sources of each feature that is on and the stubs of each that is off. Core
# sources come first, then the platform's, de-duplicated (two features may share
# a stub).
function(interposer_feature_sources out)
    if(WIN32)
        set(platform WIN)
    else()
        set(platform LINUX)
    endif()

    set(sources ${INTERPOSER_ALWAYS_CORE_SOURCES} ${INTERPOSER_ALWAYS_${platform}_SOURCES})
    set(index 1)

    foreach(feature IN LISTS INTERPOSER_FEATURES)
        if(ARGV${index})
            list(APPEND sources ${INTERPOSER_FEATURE_${feature}_CORE} ${INTERPOSER_FEATURE_${feature}_${platform}})
        else()
            list(APPEND sources ${INTERPOSER_FEATURE_${feature}_STUBS_${platform}})
        endif()

        math(EXPR index "${index} + 1")
    endforeach()

    list(REMOVE_DUPLICATES sources)
    set(${out} ${sources} PARENT_SCOPE)
endfunction()

# Every feature's real sources: what the shipped DLL and .so are built from.
interposer_feature_sources(INTERPOSER_ALL_SOURCES ON ON ON ON ON ON ON ON ON ON ON ON ON)

# interposer_add_embedded_library(<target> [FEATURES <NAME>...])
#
# For this directory's CMakeLists.txt and Linux.cmake, after the dependencies
# are found; a consumer sets the options rather than calling it.
#
# A static library for a patch to embed, carrying the named features (any of
# INTERPOSER_FEATURES) and stubs for the rest. Without FEATURES the
# INTERPOSER_FEATURE_<NAME> options decide (all off unless turned on), which is how
# LANCommander.Interposer.Static is made. Link it through an INTERFACE target
# that adds WHOLE_ARCHIVE and INTERPOSER_STATIC, as LANCommander::Interposer::Embedded
# does for that one.
function(interposer_add_embedded_library target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "FEATURES")

    # FEATURES with nothing after it is a build with no feature at all, not "use
    # the options".
    if(DEFINED arg_FEATURES OR "FEATURES" IN_LIST arg_KEYWORDS_MISSING_VALUES)
        set(explicit ON)
    else()
        set(explicit OFF)
    endif()

    set(switches)
    set(definitions)

    foreach(feature IN LISTS INTERPOSER_FEATURES)
        if(explicit)
            if(feature IN_LIST arg_FEATURES)
                set(enabled ON)
            else()
                set(enabled OFF)
            endif()
        elseif(INTERPOSER_FEATURE_${feature})
            set(enabled ON)
        else()
            set(enabled OFF)
        endif()

        set(on_${feature} ${enabled})
        list(APPEND switches ${enabled})

        if(enabled)
            list(APPEND definitions INTERPOSER_FEATURE_${feature}=1)
        else()
            list(APPEND definitions INTERPOSER_FEATURE_${feature}=0)
        endif()
    endforeach()

    foreach(feature IN LISTS arg_FEATURES)
        if(NOT feature IN_LIST INTERPOSER_FEATURES)
            message(FATAL_ERROR "${target}: unknown Interposer feature '${feature}'. "
                "Known: ${INTERPOSER_FEATURES}")
        endif()
    endforeach()

    foreach(feature IN LISTS INTERPOSER_FEATURES)
        foreach(needed IN LISTS INTERPOSER_FEATURE_${feature}_DEPENDS)
            if(on_${feature} AND NOT on_${needed})
                message(FATAL_ERROR "${target}: INTERPOSER_FEATURE_${feature} needs "
                    "INTERPOSER_FEATURE_${needed}. Turn ${needed} on as well.")
            endif()
        endforeach()
    endforeach()

    interposer_feature_sources(sources ${switches})

    add_library(${target} STATIC ${sources})

    # Built as the DLL is, so the Interposer* exports leave through the image
    # that embeds it.
    target_compile_definitions(${target} PRIVATE INTERPOSER_BUILDING_DLL ${definitions})
    target_include_directories(${target}
        PRIVATE
            "${INTERPOSER_SOURCE_DIR}/dll/src/core"
            "${INTERPOSER_SOURCE_DIR}/dll/src/platform"
            "${INTERPOSER_SOURCE_DIR}/dll/src/${INTERPOSER_PLATFORM_DIR}"
        PUBLIC
            "${INTERPOSER_SOURCE_DIR}/include")

    if(on_VOICE)
        set(voice ${INTERPOSER_VOICE_LIBRARIES})
    else()
        set(voice)
    endif()

    if(WIN32)
        # Portable code uses the standard C functions, not the MSVC _s variants,
        # which /sdl would otherwise turn into C4996 errors.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        # Without /GL, so its objects link with any v14x toolset, not only the
        # exact compiler that built them.
        set_target_properties(${target} PROPERTIES INTERPOSER_NO_LTCG ON)
        target_link_libraries(${target} PRIVATE
            minhook::minhook yaml-cpp::yaml-cpp winhttp ws2_32 version winmm ${voice})
    else()
        # Position-independent, since it ends up in a shared object.
        set_target_properties(${target} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            C_VISIBILITY_PRESET       hidden
            CXX_VISIBILITY_PRESET     hidden
            VISIBILITY_INLINES_HIDDEN ON)
        target_sources(${target} PRIVATE "${INTERPOSER_SOURCE_DIR}/dll/src/linux/embedded.cpp")
        target_link_libraries(${target} PUBLIC
            yaml-cpp::yaml-cpp Threads::Threads ${CMAKE_DL_LIBS} m ${voice})
    endif()

    interposer_version_resource(${target})
endfunction()
