cmake_minimum_required(VERSION 3.16)

project(kss_control_framework VERSION 5.6.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(KCF_ENABLE_PACKAGING "Enable phase-1 static Core DEB packaging" OFF)

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

add_subdirectory(examples/introspection)

add_subdirectory(examples/pubsub)

add_subdirectory(examples/parameter_app)

add_subdirectory(examples/service_app)

add_subdirectory(examples/timer_app)

add_subdirectory(examples/action_app)

add_subdirectory(examples/network)

add_subdirectory(examples/application_composition)

add_subdirectory(gateway)

install(EXPORT KCFTargets NAMESPACE KCF:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KCF)
configure_package_config_file(cmake/KCFConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/KCFConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KCF)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/KCFConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/KCFConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/KCFConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KCF)
if(KCF_ENABLE_PACKAGING)
    include(cmake/Packaging.cmake)
endif()
