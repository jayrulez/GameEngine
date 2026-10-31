# Foundation::Shell.Web - `foundation.shell.web` (a single-canvas IShell over Emscripten's HTML5 API).
# The web counterpart of the SDL3 desktop backend; Emscripten-only. Self-contained: it does not
# borrow the Null shell's stubs, so the HTML5 input/dialog wiring has a home here. One partition per
# component: window, window manager, input devices, dialogs, and the composing shell.
util_add_engine_library(Shell.Web ALIAS Foundation::Shell.Web)
target_link_libraries(Shell.Web PUBLIC Foundation::Shell Foundation::Policy)

target_sources(Shell.Web
    PUBLIC
        FILE_SET shell_web_modules TYPE CXX_MODULES FILES
            WebWindow.cppm
            WebWindowManager.cppm
            WebInput.cppm
            WebDialog.cppm
            WebShell.cppm)
