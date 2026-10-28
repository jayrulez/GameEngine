# UI Sandbox - on-screen test of foundation.ui (the Sedulous.UI port) on the VG renderer.
# Ports Sedulous's UISandbox demo (sans the Toolkit tabs) to prove the port end-to-end.
add_executable(UISandbox Main.cpp)
target_link_libraries(UISandbox PRIVATE
    Foundation::Core
    Foundation::Runtime Foundation::Runtime.Client Foundation::Runtime.Desktop Foundation::Shell.Desktop
    Foundation::Graphics Foundation::Graphics.Gpu Foundation::UI.Runtime Foundation::UI.Application
    Foundation::Shell Foundation::Image Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.HarfBuzz Foundation::Fonts.Bidi
    Foundation::Fonts.DistanceField.Baker
    Foundation::VG Foundation::VG.Renderer Foundation::UI Foundation::UI.Toolkit Foundation::UI.Shell Foundation::UI.VFS Foundation::VFS)
util_copy_sdl3_dll(UISandbox)
