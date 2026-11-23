add_library(kcf_bringup_support src/bringup.cpp)
set_target_properties(kcf_bringup_support PROPERTIES EXPORT_NAME bringup)
add_library(KCF::bringup ALIAS kcf_bringup_support)
target_include_directories(kcf_bringup_support PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(kcf_bringup_support PUBLIC kcf_core)

add_executable(kcf_bringup src/main.cpp)

target_include_directories(kcf_bringup
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(kcf_bringup
    PRIVATE
        kcf_bringup_support
)
install(TARGETS kcf_bringup_support EXPORT KCFTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS kcf_bringup RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/bringup DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
