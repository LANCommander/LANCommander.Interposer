add_executable(BorderlessInjector
    src/main.cpp
)

target_link_libraries(BorderlessInjector PRIVATE)

set_target_properties(BorderlessInjector PROPERTIES
    OUTPUT_NAME "BorderlessInjector"
)
