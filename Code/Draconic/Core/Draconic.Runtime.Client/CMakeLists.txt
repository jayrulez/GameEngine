# Draconic::Runtime.Client - `draconic.runtime.client` (the Application base class).
# Loop-agnostic: the desktop runner (draconic.runtime.desktop) drives Start/Tick/Stop against the shell.

add_library(Draconic.Runtime.Client STATIC)
add_library(Draconic::Runtime.Client ALIAS Draconic.Runtime.Client)

target_link_libraries(Draconic.Runtime.Client
    PUBLIC Draconic::Runtime Draconic::Shell Draconic::Graphics Draconic::Profiler Draconic::Policy)

target_sources(Draconic.Runtime.Client
    PUBLIC
        FILE_SET runtime_client_modules TYPE CXX_MODULES FILES
            Application.cppm
            EmbeddedHost.cppm
            ApplicationHost.cppm)


# Build stamp: regenerated EVERY build (content-compared, so no-op builds stay no-op) and
# compiled into Runtime.Client - every app that uses DRACONIC_APP_MAIN logs it at startup.
# The definitive "which build is this" answer, most valuable on web where .wasm caches hard.
set(DRACONIC_BUILD_STAMP_CPP "${CMAKE_CURRENT_BINARY_DIR}/BuildStamp.cpp")
execute_process(COMMAND ${CMAKE_COMMAND}
    -DOUT=${DRACONIC_BUILD_STAMP_CPP} -DSRC_ROOT=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_SOURCE_DIR}/cmake/GenerateBuildStamp.cmake) # exists at configure
add_custom_target(Draconic.BuildStamp.Tick
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${DRACONIC_BUILD_STAMP_CPP} -DSRC_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/GenerateBuildStamp.cmake
    BYPRODUCTS ${DRACONIC_BUILD_STAMP_CPP}
    COMMENT "Refreshing build stamp")
add_dependencies(Draconic.Runtime.Client Draconic.BuildStamp.Tick)
target_sources(Draconic.Runtime.Client PRIVATE ${DRACONIC_BUILD_STAMP_CPP})
