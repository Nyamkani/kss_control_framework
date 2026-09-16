# Keep the existing low-level kcf_parameter_owner target unchanged.
add_executable(kcf_parameter_app_owner src/owner_main.cpp)
set_target_properties(kcf_parameter_app_owner PROPERTIES OUTPUT_NAME kcf_parameter_owner)
target_include_directories(kcf_parameter_app_owner PRIVATE include)
target_link_libraries(kcf_parameter_app_owner PRIVATE kcf_core)

add_executable(kcf_parameter_app_bringup src/bringup_main.cpp ../../bringup/src/bringup.cpp)
target_include_directories(kcf_parameter_app_bringup PRIVATE ../../bringup/include)
target_link_libraries(kcf_parameter_app_bringup PRIVATE kcf_core)
add_dependencies(kcf_parameter_app_bringup kcf_parameter_app_owner)
