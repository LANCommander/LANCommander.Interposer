add_library(LANCommander.Interposer.Plugin.Mouse SHARED
    src/dllmain.cpp
    src/resource.rc
)

target_compile_definitions(LANCommander.Interposer.Plugin.Mouse PRIVATE _USRDLL)
set_target_properties(LANCommander.Interposer.Plugin.Mouse PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Plugins")

interposer_ship_target(LANCommander.Interposer.Plugin.Mouse)
interposer_version_resource(LANCommander.Interposer.Plugin.Mouse)
