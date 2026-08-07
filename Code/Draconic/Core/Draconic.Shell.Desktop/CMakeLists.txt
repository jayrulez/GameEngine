# Draconic::Shell.Desktop - `draconic.shell.desktop` (SDL3-based IShell for Windows/Linux/macOS:
# windowing, input, events). The desktop run loop lives in draconic.runtime.desktop, so this
# backend needs only the shell interface + SDL3 - no client/graphics dependency.

add_library(Draconic.Shell.Desktop STATIC)
add_library(Draconic::Shell.Desktop ALIAS Draconic.Shell.Desktop)

target_link_libraries(Draconic.Shell.Desktop
    PUBLIC Draconic::Shell Draconic::Policy
    PRIVATE SDL3::SDL3)

target_sources(Draconic.Shell.Desktop
    PRIVATE
        SDL3ShellImpl.cpp   # out-of-line SDL backend defs (see SDL3Shell.cppm)
    PUBLIC
        FILE_SET shell_desktop_modules TYPE CXX_MODULES FILES
            SDL3Shell.cppm)

