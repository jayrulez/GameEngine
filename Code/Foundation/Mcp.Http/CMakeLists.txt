# Foundation::Mcp.Http - `foundation.mcp.http`: the MCP streamable-HTTP binding. Hosts the same
# McpServer registry the stdio host serves, over Foundation::Http: localhost with a bearer token,
# POST /mcp with one message per request (Connection: close), and GET /events as the SSE channel
# for server-initiated traffic. A separate library beside foundation.mcp, as Mcp.Reflection is,
# so the protocol core stays transport-free.

util_add_engine_library(Mcp.Http ALIAS Foundation::Mcp.Http)
target_link_libraries(Mcp.Http PUBLIC
    Foundation::Mcp Foundation::Http Foundation::Core Foundation::Policy)

target_sources(Mcp.Http
    PRIVATE
        McpHttpImpl.cpp
    PUBLIC
        FILE_SET mcp_http_modules TYPE CXX_MODULES FILES
            McpHttp.cppm)
