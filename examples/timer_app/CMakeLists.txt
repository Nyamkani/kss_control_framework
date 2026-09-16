add_executable(kcf_timer_element src/timer_main.cpp)
target_link_libraries(kcf_timer_element PRIVATE kcf_core)

add_executable(kcf_timer_app_bringup src/bringup_main.cpp ../../bringup/src/bringup.cpp)
target_include_directories(kcf_timer_app_bringup PRIVATE ../../bringup/include)
target_link_libraries(kcf_timer_app_bringup PRIVATE kcf_core)
add_dependencies(kcf_timer_app_bringup kcf_timer_element)
