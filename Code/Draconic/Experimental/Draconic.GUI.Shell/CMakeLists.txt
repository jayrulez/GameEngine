# Draconic::GUI.Shell - `draconic.gui.shell`, the platform input bridge: translates
# draconic.shell InputEvents into GUI EventDispatcher injections. The ONLY GUI module that
# depends on the platform input layer, keeping draconic.gui itself platform-agnostic.
add_library(Draconic.GUI.Shell STATIC)
add_library(Draconic::GUI.Shell ALIAS Draconic.GUI.Shell)

target_link_libraries(Draconic.GUI.Shell
    PUBLIC Draconic::GUI Draconic::Shell Draconic::Foundation Draconic::Policy)

target_sources(Draconic.GUI.Shell
    PUBLIC
        FILE_SET guishell_modules TYPE CXX_MODULES FILES
            GuiInputBridge.cppm)

