# Draconic::Shell.Null - `draconic.shell.null` (headless IShell).

add_library(Draconic.Shell.Null STATIC)
add_library(Draconic::Shell.Null ALIAS Draconic.Shell.Null)

target_link_libraries(Draconic.Shell.Null PUBLIC Draconic::Shell Draconic::Policy)

target_sources(Draconic.Shell.Null
    PUBLIC
        FILE_SET shell_null_modules TYPE CXX_MODULES FILES
            NullShell.cppm)

