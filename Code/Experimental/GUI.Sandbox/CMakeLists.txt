# GUI Sandbox - first on-screen test of experimental.gui (widgets + CSS on the VG renderer).
add_executable(GUISandbox Main.cpp)
target_link_libraries(GUISandbox PRIVATE
    Foundation::Core Foundation::RHI Foundation::Shaders Foundation::Shaders.System
    Samples::Framework
    Foundation::Shell Foundation::Image Foundation::Fonts Foundation::Fonts.TrueType
    Foundation::VG Foundation::VG.Renderer Experimental::GUI Experimental::GUI.Shell)
util_copy_sdl3_dll(GUISandbox)
