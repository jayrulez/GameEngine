# Foundation::Mcp.Script - `foundation.mcp.script`: the scripting MCP tools (script_api). A module's
# own tool contribution against foundation.mcp; resolves backends live from the ScriptBackendRegistry
# (it links no backend; the host registers those).

util_add_engine_library(Mcp.Script ALIAS Foundation::Mcp.Script)
target_link_libraries(Mcp.Script PUBLIC
    Foundation::Mcp Foundation::Json Foundation::Script Foundation::Script.Facades
    Foundation::Core Foundation::Policy)

target_sources(Mcp.Script
    PUBLIC
        FILE_SET mcp_script_modules TYPE CXX_MODULES FILES
            ScriptTools.cppm)
