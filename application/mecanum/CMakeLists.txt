add_library(mecanum_motor_support STATIC
    src/serial_uart.cpp src/arduino_motor_driver.cpp src/kinematics.cpp
    src/motor_element.cpp)
target_include_directories(mecanum_motor_support PUBLIC include)
target_link_libraries(mecanum_motor_support PUBLIC kcf_core)
target_compile_options(mecanum_motor_support PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kcf_mecanum_motor src/main.cpp)
target_link_libraries(kcf_mecanum_motor PRIVATE mecanum_motor_support)

option(MECANUM_BUILD_TESTS "Build hardware-free mecanum tests" OFF)
if(MECANUM_BUILD_TESTS)
    add_executable(mecanum_motor_tests tests/motor_tests.cpp)
    target_link_libraries(mecanum_motor_tests PRIVATE mecanum_motor_support)
    target_link_options(mecanum_motor_tests PRIVATE "-Wl,--wrap=write")
    # Compile the actual sketch/PID with hardware doubles; never access a board.
    add_executable(mecanum_firmware_tests tests/firmware_watchdog_tests.cpp)
    target_include_directories(mecanum_firmware_tests PRIVATE
        tests/arduino_stubs firmware/Mecanum4WD)
    target_compile_features(mecanum_firmware_tests PRIVATE cxx_std_17)
    # Synthetic test limit only. Production firmware has no default RPM limit.
    target_compile_definitions(mecanum_firmware_tests PRIVATE MECANUM_MAX_ABS_RPM=120)
    target_compile_options(mecanum_firmware_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
