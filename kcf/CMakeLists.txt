add_library(kcf_core
    src/process_runtime.cpp
    src/process.cpp
)

target_include_directories(kcf_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(kcf_core
    PUBLIC
        Threads::Threads
)
