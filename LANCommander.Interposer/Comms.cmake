# LANCommander Comms, vendored under comms/ and built into the Interposer.
#
# The Interposer binary links this and re-exports the API under the
# interposer_comms_* names (the namespace was rebranded from comms_* on
# vendoring, so a consumer sees it is the Interposer's). Built as an OBJECT
# library so every exported symbol is retained in the DLL/.so even though the
# Interposer itself never calls them.
#
# Opus, ENet and miniaudio come from vcpkg, as for the standalone Comms build;
# miniaudio and opus ship no CMake config, so they are located directly.

find_package(unofficial-enet CONFIG REQUIRED)

find_path(OPUS_INCLUDE_DIR opus.h PATH_SUFFIXES opus REQUIRED)
set(INTERPOSER_COMMS_VCPKG_PREFIX ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET})
find_library(OPUS_LIBRARY_RELEASE opus PATHS ${INTERPOSER_COMMS_VCPKG_PREFIX}/lib NO_DEFAULT_PATH REQUIRED)
find_library(OPUS_LIBRARY_DEBUG   opus PATHS ${INTERPOSER_COMMS_VCPKG_PREFIX}/debug/lib NO_DEFAULT_PATH REQUIRED)
add_library(interposer_opus INTERFACE)
target_include_directories(interposer_opus INTERFACE ${OPUS_INCLUDE_DIR})
target_link_libraries(interposer_opus INTERFACE optimized ${OPUS_LIBRARY_RELEASE} debug ${OPUS_LIBRARY_DEBUG})

find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
add_library(interposer_miniaudio INTERFACE)
target_include_directories(interposer_miniaudio INTERFACE ${MINIAUDIO_INCLUDE_DIR})

add_library(interposer_comms OBJECT
    comms/src/common/version.cpp
    comms/src/common/log.cpp
    comms/src/common/miniaudio_impl.cpp
    comms/src/server/server_api.cpp
    comms/src/server/server_network.cpp
    comms/src/server/server_groups.cpp
    comms/src/server/spatial_grid.cpp
    comms/src/server/server_limits.cpp
    comms/src/server/server_admin.cpp
    comms/src/client/client_api.cpp
    comms/src/client/client_groups.cpp
    comms/src/client/client_audio.cpp
    comms/src/client/client_network.cpp
    comms/src/client/client_voice.cpp
    comms/src/client/audio_devices.cpp
)

# Linked into the shared Interposer, so the objects must be position-independent.
set_target_properties(interposer_comms PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(interposer_comms
    PUBLIC  comms/include
    PRIVATE comms/src)

# INTERPOSER_COMMS_BUILDING turns INTERPOSER_COMMS_API into an export, so the
# Interposer binary that links these objects re-exports the voice API.
target_compile_definitions(interposer_comms PRIVATE
    INTERPOSER_COMMS_BUILDING
    INTERPOSER_COMMS_VERSION_STRING="0.1.0")

target_link_libraries(interposer_comms PUBLIC
    interposer_opus unofficial::enet::enet interposer_miniaudio)

if(WIN32)
    target_link_libraries(interposer_comms PUBLIC ws2_32 winmm)
    target_compile_definitions(interposer_comms PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # Comms builds clean at /W4; keep it off the Interposer's /sdl, which it was
    # not written against.
    target_compile_options(interposer_comms PRIVATE /W3)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(interposer_comms PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
