# Foundation::UI.Shell - `foundation.ui.shell`: the platform input bridge for foundation.ui.
# Reimplemented (not ported from Sedulous.UI.Shell): translates foundation.shell input
# events / an InputSurface into UIContext InputManager calls and drives window text
# input (IME) from UI focus. Kept separate so the core foundation.ui stays platform-free.
util_add_engine_library(UI.Shell ALIAS Foundation::UI.Shell)
target_link_libraries(UI.Shell
    PUBLIC Foundation::Core Foundation::UI Foundation::Shell Foundation::Policy)

target_sources(UI.Shell
    PUBLIC
        FILE_SET uishell_modules TYPE CXX_MODULES FILES
            UiInputBridge.cppm)

