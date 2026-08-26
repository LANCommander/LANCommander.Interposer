add_library(InterposerDLL SHARED
    src/dllmain.cpp
    src/hooks.cpp
    src/registry.cpp
)

target_include_directories(InterposerDLL PRIVATE src)

target_link_libraries(InterposerDLL PRIVATE minhook)

# Ensure we produce a clean DLL name
set_target_properties(InterposerDLL PROPERTIES
    OUTPUT_NAME "InterposerDLL"
    PREFIX ""
)
