add_executable(kcf_topic_recovery_test src/topic_recovery_main.cpp)
target_include_directories(kcf_topic_recovery_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcf_topic_recovery_test PRIVATE kcf_core rt)

add_executable(kcf_parameter_recovery_test src/parameter_recovery_main.cpp)
target_include_directories(kcf_parameter_recovery_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcf_parameter_recovery_test PRIVATE kcf_core rt)

# Schedule real initialization flock calls in the test only; no production hooks.
add_executable(kcf_incomplete_recovery_test src/incomplete_race_main.cpp)
target_include_directories(kcf_incomplete_recovery_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcf_incomplete_recovery_test PRIVATE kcf_core rt)
target_link_options(kcf_incomplete_recovery_test PRIVATE "-Wl,--wrap=flock")

add_executable(kcf_topic_queue_test src/topic_queue_test.cpp)
target_include_directories(kcf_topic_queue_test PRIVATE include)
target_compile_options(kcf_topic_queue_test PRIVATE -fno-builtin-memcpy -UNDEBUG)
target_link_options(kcf_topic_queue_test PRIVATE "-Wl,--wrap=memcpy" "-Wl,--wrap=pthread_cond_broadcast")
target_link_libraries(kcf_topic_queue_test PRIVATE kcf_core rt)
