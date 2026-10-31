# Foundation::Runtime.Client - `foundation.runtime.client` (the Application base class).
# Loop-agnostic: the desktop runner (foundation.runtime.desktop) drives Start/Tick/Stop against the shell.

util_add_engine_library(Runtime.Client ALIAS Foundation::Runtime.Client)
target_link_libraries(Runtime.Client
    PUBLIC Foundation::Runtime Foundation::Shell Foundation::Graphics Foundation::Profiler
           Foundation::Profiler.Capture Foundation::Policy)

target_sources(Runtime.Client
    PUBLIC
        FILE_SET runtime_client_modules TYPE CXX_MODULES FILES
            Application.cppm
            EmbeddedHost.cppm
            DeviceCounterPublisher.cppm
            ApplicationHost.cppm)


# Build stamp: regenerated when git state changes (content-compared) and
# compiled into Runtime.Client; every app that uses APP_MAIN logs it at startup.
# It answers "which build is this", most usefully on web, where .wasm files cache hard.
set(BUILDSYSTEM_BUILD_STAMP_CPP "${CMAKE_CURRENT_BINARY_DIR}/BuildStamp.cpp")
execute_process(COMMAND ${CMAKE_COMMAND}
    -DOUT=${BUILDSYSTEM_BUILD_STAMP_CPP} -DSRC_ROOT=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_SOURCE_DIR}/cmake/GenerateBuildStamp.cmake) # exists at configure
# Regenerate as a normal build edge keyed on git state (HEAD/index): the stamp changes
# when a commit, checkout or stage changes it, and the edge is clean otherwise. An always-run
# custom target with this file as a BYPRODUCT corrupts ninja's dyndep plan on incremental
# module builds (Plan::RefreshDyndepDependents assertion): the scanner cannot model an
# always-dirty phony rewriting a compiled source mid-build. The cost is that a dirty-tree
# fingerprint refreshes on git operations, not on every save, so it can be stale until the
# next stage or commit, but the build never breaks.
set(_stamp_git_deps "")
if(EXISTS "${CMAKE_SOURCE_DIR}/.git/HEAD")
    list(APPEND _stamp_git_deps "${CMAKE_SOURCE_DIR}/.git/HEAD")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/.git/index")
    list(APPEND _stamp_git_deps "${CMAKE_SOURCE_DIR}/.git/index")
endif()
add_custom_command(OUTPUT ${BUILDSYSTEM_BUILD_STAMP_CPP}
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${BUILDSYSTEM_BUILD_STAMP_CPP} -DSRC_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/GenerateBuildStamp.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/cmake/GenerateBuildStamp.cmake ${_stamp_git_deps}
    COMMENT "Refreshing build stamp")
target_sources(Runtime.Client PRIVATE ${BUILDSYSTEM_BUILD_STAMP_CPP})
# The stamp TU is plain extern "C" with no imports: exclude it from C++20 module scanning
# (a dyndep edge on a regenerated file destabilizes incremental ninja builds, and there is
# nothing to scan anyway).
set_source_files_properties(${BUILDSYSTEM_BUILD_STAMP_CPP} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
