add_executable(kcf_mecanum_motor
    src/main.cpp
    src/serial_uart.cpp
    src/arduino_motor_driver.cpp
    src/motor_element.cpp
)
target_include_directories(kcf_mecanum_motor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(kcf_mecanum_motor PRIVATE kcf_core rt)
