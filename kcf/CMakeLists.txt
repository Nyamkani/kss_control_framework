include(GNUInstallDirs)
add_library(kcf_core
    src/network/network_interface.cpp
    src/network/host_identity.cpp
    src/network/network_diagnostics.cpp
    src/network/network_diagnostics_client.cpp
    src/network/remote_control_client.cpp
    src/network/topic_protocol.cpp
    src/network/topic_proxy.cpp
    src/network/topic_data_plane.cpp
    src/action_registry.cpp
    src/dynamic_action_client.cpp
    src/network/parameter_codec.cpp
    src/network/local_metadata.cpp
    src/network/control_frame.cpp
    src/network/control_node.cpp
    src/network/discovery_protocol.cpp
    src/network/peer_discovery.cpp
    src/service_registry.cpp
    src/dynamic_service_client.cpp
    src/dynamic_access.cpp
    src/type_registry.cpp
    src/supervisor_registry.cpp
    src/endpoint_registry.cpp
    src/introspection_client.cpp
    src/runtime_registry.cpp
    src/process_runtime.cpp
    src/process.cpp
    src/timer.cpp
    src/service_server.cpp
    src/service_client.cpp
)

target_include_directories(kcf_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(kcf_core PUBLIC cxx_std_17)
set_target_properties(kcf_core PROPERTIES EXPORT_NAME core)
add_library(KCF::core ALIAS kcf_core)

target_link_libraries(kcf_core
    PUBLIC
        Threads::Threads
)

# The Tools project embeds this directory with EXCLUDE_FROM_ALL. Only the
# Framework top-level project owns SDK installation/export and packaging.
if(PROJECT_NAME STREQUAL "kss_control_framework")
    install(TARGETS kcf_core EXPORT KCFTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/kcf DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
