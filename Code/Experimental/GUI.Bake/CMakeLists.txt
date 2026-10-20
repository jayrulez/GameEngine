# Experimental::GUI.Bake - `experimental.gui.bake`: the GUI's picture baker on the GPU (an SVG skin
# drawn once, at the size it shows, into a texture the VG renderer samples). Kept out of the core
# Experimental::GUI, which only asks through IPictureBaker, so the GUI stays free of RHI (the host
# links this and hands the baker to its GUIContext).
util_add_engine_library(GUI.Bake ALIAS Experimental::GUI.Bake)
target_link_libraries(GUI.Bake
    PUBLIC Foundation::Core Experimental::GUI Foundation::RHI Foundation::Image Foundation::Fonts Foundation::VG Foundation::VG.SVG
           Foundation::VG.Renderer Foundation::Policy)

target_sources(GUI.Bake
    PUBLIC
        FILE_SET guibake_modules TYPE CXX_MODULES FILES
            GpuPictureBaker.cppm)
