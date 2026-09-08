add_executable(kcf_supervisor_normal src/normal_element_main.cpp)
target_link_libraries(kcf_supervisor_normal PRIVATE kcf_core)

add_executable(kcf_supervisor_crash src/crash_element_main.cpp)
target_link_libraries(kcf_supervisor_crash PRIVATE kcf_core)

add_executable(kcf_supervisor_safe src/safe_element_main.cpp)
target_link_libraries(kcf_supervisor_safe PRIVATE kcf_core rt)

add_executable(kcf_supervisor_runtime_test src/runtime_element_main.cpp)
target_link_libraries(kcf_supervisor_runtime_test PRIVATE kcf_core)

add_executable(kcf_runtime_supervision_test src/runtime_supervision_test.cpp)
target_link_libraries(kcf_runtime_supervision_test PRIVATE kcf_core)

add_executable(kcf_reset_test src/reset_test.cpp ../../bringup/src/bringup.cpp)
target_include_directories(kcf_reset_test PRIVATE ../../bringup/include)
target_link_libraries(kcf_reset_test PRIVATE kcf_core)

add_executable(kcf_system_status_recovery_test src/system_status_recovery_test.cpp)
target_include_directories(kcf_system_status_recovery_test PRIVATE ../recovery/include)
target_link_libraries(kcf_system_status_recovery_test PRIVATE kcf_core)
