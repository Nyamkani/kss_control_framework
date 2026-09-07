add_executable(kcf_dummy
    src/main.cpp
    src/dummy_element.cpp
)

target_include_directories(kcf_dummy
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(kcf_dummy
    PRIVATE
        kcf_core
)
