# Foundation::Http.Client - `foundation.http.client`: the game-facing HTTP client interface
# (IHttpClient), URL parsing, and the native client (HTTP/1.1 on a worker thread, over a
# PlainStream for http:// and a TlsStream for https://, the TLS backend handed in).

util_add_engine_library(Http.Client ALIAS Foundation::Http.Client)
# The User-Agent a request sends unless it names its own: the engine's name and version, from the
# root CMakeLists (scoped here, so a version bump rebuilds only this library).
target_compile_definitions(Http.Client PRIVATE HTTP_USER_AGENT=u8\"${ENGINE_PRODUCT_NAME}/${ENGINE_VERSION_DISPLAY}\")
target_link_libraries(Http.Client PUBLIC Foundation::Core Foundation::Http Foundation::Net Foundation::Net.Tls Foundation::Policy)

target_sources(Http.Client
    PRIVATE
        HttpClientImpl.cpp
    PUBLIC
        FILE_SET http_client_modules TYPE CXX_MODULES FILES
            HttpClient.cppm)

# The browser client: whoever links this library on wasm needs emscripten's fetch. INTERFACE, as
# Net.WebSocket's websocket shim; an executable reaching it through a static private dependency
# restates it.
if(EMSCRIPTEN)
    target_link_options(Http.Client INTERFACE "-sFETCH=1")
endif()
