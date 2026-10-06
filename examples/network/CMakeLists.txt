add_executable(kcf_network_contract_test src/network_contract_test.cpp)
target_link_libraries(kcf_network_contract_test PRIVATE kcf_core)
target_compile_options(kcf_network_contract_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

add_executable(kcf_peer_discovery_test src/peer_discovery_test.cpp)
target_link_libraries(kcf_peer_discovery_test PRIVATE kcf_core)
target_compile_options(kcf_peer_discovery_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

add_executable(kcf_peer_discovery src/peer_discovery_main.cpp)
target_link_libraries(kcf_peer_discovery PRIVATE kcf_core)
target_compile_options(kcf_peer_discovery PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kcf_network_compatibility_test src/network_compatibility_test.cpp)
target_link_libraries(kcf_network_compatibility_test PRIVATE kcf_core)
target_compile_options(kcf_network_compatibility_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

add_executable(kcf_control_parameter_test src/control_parameter_test.cpp)
target_link_libraries(kcf_control_parameter_test PRIVATE kcf_core)
target_compile_options(kcf_control_parameter_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

add_executable(kcf_parameter_peer src/parameter_peer_main.cpp)
target_link_libraries(kcf_parameter_peer PRIVATE kcf_core)
target_compile_options(kcf_parameter_peer PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kcf_control_service_test src/control_service_test.cpp)
target_link_libraries(kcf_control_service_test PRIVATE kcf_core)
target_compile_options(kcf_control_service_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

add_executable(kcf_control_action_test src/control_action_test.cpp)
target_link_libraries(kcf_control_action_test PRIVATE kcf_core)
target_compile_options(kcf_control_action_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

add_executable(kcf_topic_data_test src/topic_data_test.cpp)
target_link_libraries(kcf_topic_data_test PRIVATE kcf_core ${CMAKE_DL_LIBS})
target_compile_options(kcf_topic_data_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)

# Builds without privileges; Virtual LAN execution is an explicit opt-in runner.
add_executable(kcf_virtual_host_fixture ../../tests/network_virtual/host_fixture.cpp)
target_include_directories(kcf_virtual_host_fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kcf_virtual_host_fixture PRIVATE kcf_core)
target_compile_options(kcf_virtual_host_fixture PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kcf_proxy_recovery_test src/proxy_recovery_test.cpp)
target_link_libraries(kcf_proxy_recovery_test PRIVATE kcf_core)
target_compile_options(kcf_proxy_recovery_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
