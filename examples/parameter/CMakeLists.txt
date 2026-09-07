add_executable(kcf_parameter_owner src/owner_main.cpp)
target_include_directories(kcf_parameter_owner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcf_parameter_owner PRIVATE kcf_core rt)

add_executable(kcf_parameter_client src/client_main.cpp)
target_include_directories(kcf_parameter_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcf_parameter_client PRIVATE kcf_core rt)
