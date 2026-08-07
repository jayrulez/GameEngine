# Draconic::Shell - `draconic.shell` (IShell/IWindow, the raw OS/windowing service - what
# Sedulous calls the "shell"). Interfaces only; native + null backends implement them.

add_library(Draconic.Shell STATIC)
add_library(Draconic::Shell ALIAS Draconic.Shell)

target_link_libraries(Draconic.Shell PUBLIC Draconic::Foundation Draconic::Policy)

target_sources(Draconic.Shell
    PUBLIC
        FILE_SET shell_modules TYPE CXX_MODULES FILES
            InputTypes.cppm
            Input.cppm
            InputSurface.cppm
            Dialog.cppm
            ShellModule.cppm)
