# Draconic::Shell.Web - `draconic.shell.web` (a single-canvas IShell over Emscripten's HTML5 API).
# The web counterpart of the SDL3 desktop backend; Emscripten-only. Self-contained - it does not
# borrow the Null shell's stubs, so the HTML5 input/dialog wiring has a home here. One partition per
# component: window, window manager, input devices, dialogs, and the composing shell.
add_library(Draconic.Shell.Web STATIC)
add_library(Draconic::Shell.Web ALIAS Draconic.Shell.Web)

target_link_libraries(Draconic.Shell.Web PUBLIC Draconic::Shell Draconic::Policy)

target_sources(Draconic.Shell.Web
    PUBLIC
        FILE_SET shell_web_modules TYPE CXX_MODULES FILES
            WebWindow.cppm
            WebWindowManager.cppm
            WebInput.cppm
            WebDialog.cppm
            WebShell.cppm)
