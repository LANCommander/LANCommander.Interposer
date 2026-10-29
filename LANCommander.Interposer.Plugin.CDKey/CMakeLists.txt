add_library(LANCommander.Interposer.Plugin.CDKey SHARED
    src/dllmain.cpp
    src/resource.rc
)

target_compile_definitions(LANCommander.Interposer.Plugin.CDKey PRIVATE _USRDLL)
set_target_properties(LANCommander.Interposer.Plugin.CDKey PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Plugins")

interposer_ship_target(LANCommander.Interposer.Plugin.CDKey)
interposer_version_resource(LANCommander.Interposer.Plugin.CDKey)
