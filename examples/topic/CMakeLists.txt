add_executable(kcf_topic_pub src/publisher_main.cpp)
target_include_directories(kcf_topic_pub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcf_topic_pub PRIVATE kcf_core rt)

add_executable(kcf_topic_sub src/subscriber_main.cpp)
target_include_directories(kcf_topic_sub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcf_topic_sub PRIVATE kcf_core rt)
