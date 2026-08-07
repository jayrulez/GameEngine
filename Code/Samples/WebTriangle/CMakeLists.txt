# WebTriangle - the first browser-running Draconic app. Emscripten-only: it links the web app
# stack (WebShell + the web runner + the WebGPU graphics factory) and emits an HTML shell + .js +
# .wasm. ASYNCIFY is required because device bring-up blocks-and-yields to the browser event loop
# (see the WebGPU backend's pump). The emdawnwebgpu port flag rides in transitively from
# Draconic::RHI.WebGPU (PUBLIC), so it does not need repeating here.
if(NOT EMSCRIPTEN)
    return()
endif()

add_executable(WebTriangle main.cpp)

target_link_libraries(WebTriangle PRIVATE
    Draconic::Foundation
    Draconic::RHI
    Draconic::Runtime
    Draconic::Runtime.Client
    Draconic::Shell.Web
    Draconic::Runtime.Web
    Draconic::Graphics
    Draconic::Graphics.Gpu
    Draconic::Policy)

target_link_options(WebTriangle PRIVATE
    "-sASYNCIFY"              # synchronous device init yields to the browser (emscripten_sleep)
    "-sALLOW_MEMORY_GROWTH=1"
    "-sASSERTIONS=1")

# Emit an HTML page (default Emscripten shell, which provides <canvas id="canvas">) alongside the
# .js/.wasm, rather than a bare .js that only runs under node.
set_target_properties(WebTriangle PROPERTIES SUFFIX ".html")
