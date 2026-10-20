# Experimental::GUI.Gamekit - `experimental.gui.gamekit`: a game's screens: a screen stack, input modes, transitions, prompts, toasts.
# foundation.ui's counterpart: UI.Gamekit. Specs/gui-lab-evolution-3.md.
util_add_engine_library(GUI.Gamekit ALIAS Experimental::GUI.Gamekit)
target_link_libraries(GUI.Gamekit
    PUBLIC Foundation::Core Experimental::GUI Foundation::Policy)

target_sources(GUI.Gamekit
    PUBLIC
        FILE_SET guigamekit_modules TYPE CXX_MODULES FILES
            GamekitModule.cppm)
