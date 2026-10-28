# Integration.Mcp - the golden agent-shaped MCP tool-call flow (create -> open -> inspect a project),
# crossing foundation.mcp x editor.mcp x editor.core x VFS/content. The collection's first MCP resident.

add_executable(Integration.Mcp TestsMain.cpp McpProjectFlowTests.cpp McpSceneFlowTests.cpp McpAssetUsesTests.cpp McpLogToolsTests.cpp McpScriptValidateTests.cpp McpShaderValidateTests.cpp McpGuiValidateTests.cpp McpExportTests.cpp McpFullFlowTests.cpp McpSceneReferenceTests.cpp SampleProjectTests.cpp)
target_link_libraries(Integration.Mcp PRIVATE Foundation::VFS
    Editor::Mcp Foundation::Mcp.Reflection Foundation::Mcp Foundation::Json Foundation::Content
    Foundation::Scene Foundation::Scene.Resource Foundation::Net.Replication Engine::Composition
    Foundation::Resource Foundation::Geometry.Resource Pipeline::Materials Pipeline::Texture Pipeline::Audio Engine::Render Pipeline::Script Foundation::Script.Resource
    Pipeline::Registration Pipeline::Core Pipeline::Importer
    Engine::Composition Foundation::Mcp.Script ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU}
    Foundation::Core doctest::doctest)
target_compile_definitions(Integration.Mcp PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Integration.Mcp PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Integration.Mcp COMMAND Integration.Mcp)
