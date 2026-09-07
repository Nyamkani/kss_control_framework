cmake_minimum_required(VERSION 3.16)

project(kss_control_framework LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_subdirectory(kcf)
add_subdirectory(examples/dummy)
add_subdirectory(bringup)
