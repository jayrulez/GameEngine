# GUI Sandbox - first on-screen test of experimental.gui (widgets + CSS on the VG renderer).
add_executable(GUISandbox Main.cpp)
target_sources(GUISandbox PRIVATE FILE_SET sandbox_modules TYPE CXX_MODULES FILES StressPage.cppm) # the Stress page (E8a)
target_link_libraries(GUISandbox PRIVATE
    Foundation::Core Foundation::RHI Foundation::Shaders Foundation::Shaders.System
    Samples::Framework
    Foundation::Shell Foundation::Image Foundation::Image.IO Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.DistanceField.Baker
    Foundation::VG Foundation::VG.Renderer Experimental::GUI Experimental::GUI.Shell
    Foundation::Script ${SCRIPTLIB_ANGELSCRIPT}) # the showcase (G6) runs AngelScript
util_copy_sdl3_dll(GUISandbox)
