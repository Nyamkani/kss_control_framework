# Preserve the existing low-level kcf_service_server target.
add_executable(kcf_service_app_server src/server_main.cpp)
set_target_properties(kcf_service_app_server PROPERTIES OUTPUT_NAME kcf_service_server)
target_include_directories(kcf_service_app_server PRIVATE include)
target_link_libraries(kcf_service_app_server PRIVATE kcf_core)

add_executable(kcf_service_app_bringup src/bringup_main.cpp ../../bringup/src/bringup.cpp)
target_include_directories(kcf_service_app_bringup PRIVATE ../../bringup/include)
target_link_libraries(kcf_service_app_bringup PRIVATE kcf_core)
add_dependencies(kcf_service_app_bringup kcf_service_app_server)
