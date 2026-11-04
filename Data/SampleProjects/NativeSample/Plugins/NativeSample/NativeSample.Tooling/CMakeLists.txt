# NativeSample.Tooling - the reference game's MCP tool: its plugin package's Tooling part, which the
# editor, the exporter and the MCP host load.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PRIVATE Editor::Mcp Foundation::Json Foundation::Mcp Foundation::Runtime Foundation::Policy)
target_sources(${PLUGIN_PART} PRIVATE NativeSampleTooling.cpp)
