add_library(kcf_core
    src/service_registry.cpp
    src/dynamic_service_client.cpp
    src/dynamic_access.cpp
    src/type_registry.cpp
    src/supervisor_registry.cpp
    src/endpoint_registry.cpp
    src/introspection_client.cpp
    src/runtime_registry.cpp
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
