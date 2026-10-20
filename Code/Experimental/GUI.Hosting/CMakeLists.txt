# Experimental::GUI.Hosting - `experimental.gui.hosting`: the host on foundation.graphics: one GUI, a root per window, each window's VG context, renderer, pass targets and input; the frame; pictures drawn offscreen; the picture baker.
# foundation.ui's counterpart: UI.Runtime. Specs/gui-lab-evolution-3.md.
util_add_engine_library(GUI.Hosting ALIAS Experimental::GUI.Hosting)
target_link_libraries(GUI.Hosting
    PUBLIC Foundation::Core Experimental::GUI Foundation::Policy)

target_sources(GUI.Hosting
    PUBLIC
        FILE_SET guihosting_modules TYPE CXX_MODULES FILES
            HostingModule.cppm)
