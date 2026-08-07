# Draconic::Runtime.Web - `draconic.runtime.web` (the browser application runner: a per-frame
# emscripten_set_main_loop callback driving an IApplication against an IShell, yielding to the
# browser between frames). The Emscripten sibling of draconic.runtime.desktop - same interface
# dependencies (Shell interface + client + graphics), different execution model. Emscripten-only.
add_library(Draconic.Runtime.Web STATIC)
add_library(Draconic::Runtime.Web ALIAS Draconic.Runtime.Web)

target_link_libraries(Draconic.Runtime.Web
    PUBLIC Draconic::Shell Draconic::Runtime.Client Draconic::Graphics Draconic::Policy)

target_sources(Draconic.Runtime.Web
    PUBLIC
        FILE_SET runtime_web_modules TYPE CXX_MODULES FILES
            WebRunner.cppm)
