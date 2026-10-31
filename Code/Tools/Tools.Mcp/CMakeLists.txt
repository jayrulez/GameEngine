# Tools.Mcp - headless MCP stdio server. JSON-RPC over stdin/stdout; logs to stderr.
# Its tools are the ones the engine's modules contribute (reflection, scripting, the project tools).

add_executable(Tools.Mcp Main.cpp)

# Per-executable build stamp (as Runtime.Client has): regenerated on git-state changes,
# content-compared; host_info reports it for stale-binary detection.
set(MCP_BUILD_STAMP_CPP "${CMAKE_CURRENT_BINARY_DIR}/BuildStamp.cpp")
execute_process(COMMAND ${CMAKE_COMMAND}
    -DOUT=${MCP_BUILD_STAMP_CPP} -DSRC_ROOT=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_SOURCE_DIR}/cmake/GenerateBuildStamp.cmake)
# Regenerate as a normal build edge keyed on git state (see Runtime.Client: an always-run
# custom target with a compiled BYPRODUCT corrupts ninja's dyndep plan on incremental builds).
set(_stamp_git_deps "")
if(EXISTS "${CMAKE_SOURCE_DIR}/.git/HEAD")
    list(APPEND _stamp_git_deps "${CMAKE_SOURCE_DIR}/.git/HEAD")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/.git/index")
    list(APPEND _stamp_git_deps "${CMAKE_SOURCE_DIR}/.git/index")
endif()
add_custom_command(OUTPUT ${MCP_BUILD_STAMP_CPP}
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${MCP_BUILD_STAMP_CPP} -DSRC_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/GenerateBuildStamp.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/cmake/GenerateBuildStamp.cmake ${_stamp_git_deps}
    COMMENT "Refreshing Tools.Mcp build stamp")
target_sources(Tools.Mcp PRIVATE ${MCP_BUILD_STAMP_CPP})
# The stamp TU is plain extern "C" with no imports: exclude it from C++20 module scanning
# (a dyndep edge on a regenerated file destabilizes incremental ninja builds, and there is
# nothing to scan anyway).
set_source_files_properties(${MCP_BUILD_STAMP_CPP} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(Tools.Mcp PRIVATE Foundation::VFS
    Foundation::Mcp.Reflection Foundation::Mcp.Script Editor::Mcp
    # The complete script surface (all facades), and the full manager set and component reflection
    # (scene_validate payload parsing). Both script backends (AngelScript, Luau) come transitively
    # from Pipeline::Registration below.
    Engine::Composition
    Pipeline::Registration Pipeline::Core Pipeline::Importer # the write tools' builder/importer set
    Foundation::Mcp Foundation::Json Foundation::Core Foundation::Policy)
