add_executable(kcf_topic_pub src/publisher_main.cpp)
target_include_directories(kcf_topic_pub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcf_topic_pub PRIVATE kcf_core rt)

add_executable(kcf_topic_sub src/subscriber_main.cpp)
target_include_directories(kcf_topic_sub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcf_topic_sub PRIVATE kcf_core rt)

add_executable(kcf_command_freshness_test src/command_freshness_test.cpp)
target_link_libraries(kcf_command_freshness_test PRIVATE kcf_core)
target_compile_options(kcf_command_freshness_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
