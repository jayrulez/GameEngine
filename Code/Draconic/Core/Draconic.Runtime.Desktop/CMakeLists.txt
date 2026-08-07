# Draconic::Runtime.Desktop - `draconic.runtime.desktop` (the desktop application runner:
# a blocking wall-clock loop driving an IApplication against an IShell). Works through the
# abstract IShell interface, so it needs the shell interface (not a backend) plus the client
# (IApplication/ApplicationHost) and graphics (GraphicsDevice). The web/emscripten runner will
# be its sibling.

add_library(Draconic.Runtime.Desktop STATIC)
add_library(Draconic::Runtime.Desktop ALIAS Draconic.Runtime.Desktop)

target_link_libraries(Draconic.Runtime.Desktop
    PUBLIC Draconic::Shell Draconic::Runtime.Client Draconic::Graphics Draconic::Policy)

target_sources(Draconic.Runtime.Desktop
    PUBLIC
        FILE_SET runtime_desktop_modules TYPE CXX_MODULES FILES
            DesktopRunner.cppm)
