add_executable(kcf_gateway src/main.cpp)
target_link_libraries(kcf_gateway PRIVATE kcf_core)
target_compile_options(kcf_gateway PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kcf_gateway_test src/gateway_test.cpp)
target_link_libraries(kcf_gateway_test PRIVATE kcf_core)
target_include_directories(kcf_gateway_test PRIVATE ${PROJECT_SOURCE_DIR}/examples/network/src)
target_compile_options(kcf_gateway_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

add_executable(kcf_gateway_lifecycle_test src/lifecycle_test.cpp)
target_link_libraries(kcf_gateway_lifecycle_test PRIVATE kcf_core)
target_compile_options(kcf_gateway_lifecycle_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

add_executable(kcf_gateway_topic_policy_test src/topic_policy_test.cpp)
target_link_libraries(kcf_gateway_topic_policy_test PRIVATE kcf_core)
target_include_directories(kcf_gateway_topic_policy_test PRIVATE ${PROJECT_SOURCE_DIR}/examples/network/src)
target_compile_options(kcf_gateway_topic_policy_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

add_executable(kcf_gateway_local_ipc_test src/local_ipc_test.cpp)
target_link_libraries(kcf_gateway_local_ipc_test PRIVATE kcf_core)
target_compile_options(kcf_gateway_local_ipc_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

add_executable(kcf_gateway_topic_rate_test src/topic_rate_test.cpp)
target_link_libraries(kcf_gateway_topic_rate_test PRIVATE kcf_core ${CMAKE_DL_LIBS})
target_compile_options(kcf_gateway_topic_rate_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

# Scheduler timing hook exists only in this test's DataPlane translation unit.
target_sources(kcf_gateway_topic_rate_test PRIVATE ${PROJECT_SOURCE_DIR}/kcf/src/network/topic_data_plane.cpp)
target_compile_definitions(kcf_gateway_topic_rate_test PRIVATE KCF_TOPIC_SCHEDULER_TEST)
