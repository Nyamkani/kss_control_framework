add_executable(kcf_bringup
    src/main.cpp
    src/bringup.cpp
)

target_include_directories(kcf_bringup
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(kcf_bringup
    PRIVATE
        kcf_core
)
