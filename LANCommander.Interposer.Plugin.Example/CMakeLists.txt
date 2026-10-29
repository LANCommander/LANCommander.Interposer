add_library(LANCommander.Interposer.Plugin.Example SHARED
    src/dllmain.cpp
    src/resource.rc
)

target_compile_definitions(LANCommander.Interposer.Plugin.Example PRIVATE _USRDLL)
set_target_properties(LANCommander.Interposer.Plugin.Example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Plugins")

interposer_ship_target(LANCommander.Interposer.Plugin.Example)
interposer_version_resource(LANCommander.Interposer.Plugin.Example)
