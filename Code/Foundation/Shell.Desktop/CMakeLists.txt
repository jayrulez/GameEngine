# Foundation::Shell.Desktop - `foundation.shell.desktop` (SDL3-based IShell for Windows/Linux/macOS:
# windowing, input, events). The desktop run loop lives in foundation.runtime.desktop, so this
# backend needs only the shell interface and SDL3, with no client or graphics dependency.

util_add_engine_library(Shell.Desktop ALIAS Foundation::Shell.Desktop)
target_link_libraries(Shell.Desktop
    PUBLIC Foundation::Shell Foundation::Policy
    PRIVATE ${ENGINE_SDL_TARGET}) # vendored static SDL3 (or system SDL3 under ENGINE_SYSTEM_SDL)

target_sources(Shell.Desktop
    PRIVATE
        SDL3ShellImpl.cpp   # out-of-line SDL backend defs (see SDL3Shell.cppm)
    PUBLIC
        FILE_SET shell_desktop_modules TYPE CXX_MODULES FILES
            SDL3Shell.cppm)

