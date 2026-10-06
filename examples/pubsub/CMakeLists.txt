add_executable(kcf_pubsub_publisher src/publisher_main.cpp)
target_include_directories(kcf_pubsub_publisher PRIVATE include)
target_link_libraries(kcf_pubsub_publisher PRIVATE kcf_core)

add_executable(kcf_pubsub_subscriber src/subscriber_main.cpp)
target_include_directories(kcf_pubsub_subscriber PRIVATE include)
target_link_libraries(kcf_pubsub_subscriber PRIVATE kcf_core)

add_executable(kcf_pubsub_bringup src/bringup_main.cpp ../../bringup/src/bringup.cpp)
target_include_directories(kcf_pubsub_bringup PRIVATE ../../bringup/include)
target_link_libraries(kcf_pubsub_bringup PRIVATE kcf_core)
add_dependencies(kcf_pubsub_bringup kcf_pubsub_publisher kcf_pubsub_subscriber)
