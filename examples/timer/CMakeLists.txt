add_executable(kcf_timer_test src/timer_main.cpp)
target_include_directories(kcf_timer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../topic/include)
target_link_libraries(kcf_timer_test PRIVATE kcf_core rt)
