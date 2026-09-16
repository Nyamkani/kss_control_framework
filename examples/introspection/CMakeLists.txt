add_executable(kcf_runtime_discovery_test src/runtime_discovery_test.cpp)
target_link_libraries(kcf_runtime_discovery_test PRIVATE kcf_core)
# Deterministic observer death while holding the real SharedParameter mutex.
# Link wrapping is confined to this test; production has no injection hooks.
target_link_options(kcf_runtime_discovery_test PRIVATE "-Wl,--wrap=pthread_mutex_lock")

add_executable(kcf_endpoint_registry_test src/endpoint_registry_test.cpp)
target_link_libraries(kcf_endpoint_registry_test PRIVATE kcf_core)
target_link_options(kcf_endpoint_registry_test PRIVATE "-Wl,--wrap=pthread_mutex_lock")

add_executable(kcf_supervisor_discovery_test src/supervisor_discovery_test.cpp ../../bringup/src/bringup.cpp)
target_include_directories(kcf_supervisor_discovery_test PRIVATE ../../bringup/include)
target_link_libraries(kcf_supervisor_discovery_test PRIVATE kcf_core)

add_executable(kcf_type_descriptor_test src/type_descriptor_test.cpp ../../bringup/src/bringup.cpp)
target_include_directories(kcf_type_descriptor_test PRIVATE include ../../bringup/include)
target_link_libraries(kcf_type_descriptor_test PRIVATE kcf_core)

# Compile the real dynamic implementation with memcpy interception enabled only
# in this executable, so the crash fixture works even in optimized builds.
add_executable(kcf_dynamic_access_test src/dynamic_access_test.cpp
    ../../kcf/src/dynamic_access.cpp ../../bringup/src/bringup.cpp)
target_include_directories(kcf_dynamic_access_test PRIVATE include ../../bringup/include)
target_compile_options(kcf_dynamic_access_test PRIVATE -fno-builtin-memcpy)
target_link_options(kcf_dynamic_access_test PRIVATE "-Wl,--wrap=memcpy")
target_link_libraries(kcf_dynamic_access_test PRIVATE kcf_core)

add_executable(kcf_service_introspection_test src/service_introspection_test.cpp
    ../../kcf/src/introspection_client.cpp ../../bringup/src/bringup.cpp)
target_include_directories(kcf_service_introspection_test PRIVATE include ../../bringup/include)
target_compile_options(kcf_service_introspection_test PRIVATE -fno-builtin-memcpy)
target_link_options(kcf_service_introspection_test PRIVATE "-Wl,--wrap=memcpy")
target_link_libraries(kcf_service_introspection_test PRIVATE kcf_core)

add_executable(kcf_shm_incarnation_test src/shm_incarnation_test.cpp)
target_include_directories(kcf_shm_incarnation_test PRIVATE include)
target_link_libraries(kcf_shm_incarnation_test PRIVATE kcf_core)
target_link_options(kcf_shm_incarnation_test PRIVATE "-Wl,--wrap=fstat")
