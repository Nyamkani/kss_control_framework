add_library(kcf_core
    src/process_runtime.cpp
    src/process.cpp
    src/timer.cpp
    src/service_server.cpp
    src/service_client.cpp
)

target_include_directories(kcf_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(kcf_core
    PUBLIC
        Threads::Threads
)
