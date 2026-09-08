cmake_minimum_required(VERSION 3.16)

project(kss_control_framework LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_subdirectory(kcf)
add_subdirectory(examples/dummy)
add_subdirectory(examples/topic)
add_subdirectory(examples/timer)
add_subdirectory(examples/service)
add_subdirectory(examples/parameter)
add_subdirectory(examples/action)
add_subdirectory(examples/integration)
add_subdirectory(examples/supervisor)
add_subdirectory(examples/recovery)
add_subdirectory(bringup)
