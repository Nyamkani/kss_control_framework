add_library(mecanum_imu_support STATIC
    vendor/src/serial.c vendor/src/wit_c_sdk.c
    src/wt901c_driver.cpp src/imu_element.cpp)
target_include_directories(mecanum_imu_support PUBLIC include vendor/include)
target_link_libraries(mecanum_imu_support PUBLIC kcf_core)
target_compile_features(mecanum_imu_support PUBLIC cxx_std_17)
target_compile_options(mecanum_imu_support PRIVATE -Wall -Wextra -Wpedantic)
add_executable(kcf_mecanum_imu src/main.cpp)
target_link_libraries(kcf_mecanum_imu PRIVATE mecanum_imu_support)
option(MECANUM_BUILD_IMU_TESTS "Build hardware-free WT901C tests" OFF)
if(MECANUM_BUILD_IMU_TESTS)
    add_executable(mecanum_imu_tests tests/imu_tests.cpp)
    target_link_libraries(mecanum_imu_tests PRIVATE mecanum_imu_support)
endif()
