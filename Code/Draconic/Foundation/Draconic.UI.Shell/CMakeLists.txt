# Draconic::UI.Shell - `draconic.ui.shell`: the platform input bridge for draconic.ui.
# Reimplemented (not ported from Sedulous.UI.Shell): translates draconic.shell input
# events / an InputSurface into UIContext InputManager calls and drives window text
# input (IME) from UI focus. Kept separate so the core draconic.ui stays platform-free.
add_library(Draconic.UI.Shell STATIC)
add_library(Draconic::UI.Shell ALIAS Draconic.UI.Shell)

target_link_libraries(Draconic.UI.Shell
    PUBLIC Draconic::Foundation Draconic::UI Draconic::Shell Draconic::Policy)

target_sources(Draconic.UI.Shell
    PUBLIC
        FILE_SET uishell_modules TYPE CXX_MODULES FILES
            UiInputBridge.cppm)

