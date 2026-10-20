# Experimental::GUI.Hosting - `experimental.gui.hosting`: the lab's GUI on the runtime's multi-window
# graphics host: one GUI, a root per window, each window's VG context, renderer, pass targets and
# input; the frame; the picture baker serving every window; the VG shaders. foundation.ui's
# counterpart: UI.Runtime. Specs/gui-lab-evolution-3.md, H1 to H3.
util_add_engine_library(GUI.Hosting ALIAS Experimental::GUI.Hosting)
target_link_libraries(GUI.Hosting
    PUBLIC Foundation::Core Experimental::GUI Experimental::GUI.Shell Foundation::RHI Foundation::Image Foundation::Fonts
           Foundation::VG Foundation::VG.SVG Foundation::VG.Renderer Foundation::Graphics Foundation::Shell Foundation::VFS
           Foundation::Shaders Foundation::Shaders.System Foundation::RHI.Capture Foundation::Policy)

target_sources(GUI.Hosting
    PUBLIC
        FILE_SET guihosting_modules TYPE CXX_MODULES FILES
            HostingModule.cppm
            VgShaders.cppm
            PictureBaker.cppm
            GuiHost.cppm
            Offscreen.cppm)
