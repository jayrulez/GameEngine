# Foundation::Shell - `foundation.shell` (IShell/IWindow, the raw OS and windowing service).
# Interfaces only; the native and null backends implement them.

util_add_engine_library(Shell ALIAS Foundation::Shell)
target_link_libraries(Shell PUBLIC Foundation::Core Foundation::Policy)

target_sources(Shell
    PRIVATE
        ShellReflectionImpl.cpp # the input code enums' cases (type_info names them)
    PUBLIC
        FILE_SET shell_modules TYPE CXX_MODULES FILES
            InputTypes.cppm
            Input.cppm
            InputSurface.cppm
            Dialog.cppm
            ShellModule.cppm)
