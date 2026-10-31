# Foundation::Http - `foundation.http`: a small HTTP/1.1 layer over the Core/System TCP sockets
# (through Foundation::Net's TcpSocket/TcpListener). Consumers: the MCP streamable-HTTP host, the
# WebSocket upgrade path (a WebSocket connection begins as an HTTP request), loopback tooling and
# tests. One request per connection (Connection: close) and SSE streams; no TLS, since it serves
# localhost.

util_add_engine_library(Http ALIAS Foundation::Http)
target_link_libraries(Http PUBLIC Foundation::Net Foundation::Core Foundation::Policy)

target_sources(Http
    PRIVATE
        HttpImpl.cpp
    PUBLIC
        FILE_SET http_modules TYPE CXX_MODULES FILES
            Http.cppm)
