# Foundation::Runtime.Web - `foundation.runtime.web` (the browser application runner: a per-frame
# emscripten_set_main_loop callback driving an IApplication against an IShell, yielding to the
# browser between frames). The Emscripten sibling of foundation.runtime.desktop: the same interface
# dependencies (shell interface, client, graphics), a different execution model. Emscripten-only.
util_add_engine_library(Runtime.Web ALIAS Foundation::Runtime.Web)
target_link_libraries(Runtime.Web
    PUBLIC Foundation::Shell Foundation::Runtime.Client Foundation::Graphics Foundation::Policy)

target_sources(Runtime.Web
    PUBLIC
        FILE_SET runtime_web_modules TYPE CXX_MODULES FILES
            WebRunner.cppm)
