add_executable(kcf_action_server src/server_main.cpp)
target_include_directories(kcf_action_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcf_action_server PRIVATE kcf_core rt)

add_executable(kcf_action_client src/client_main.cpp)
target_include_directories(kcf_action_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcf_action_client PRIVATE kcf_core rt)
