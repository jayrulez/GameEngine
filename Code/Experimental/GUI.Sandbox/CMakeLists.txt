# GUI Sandbox - first on-screen test of experimental.gui (widgets + CSS on the VG renderer).
add_executable(GUISandbox Main.cpp)
target_sources(GUISandbox PRIVATE FILE_SET sandbox_modules TYPE CXX_MODULES FILES StressPage.cppm) # the Stress page (E8a)
target_link_libraries(GUISandbox PRIVATE
    Foundation::Core Foundation::RHI Foundation::Shaders Foundation::Shaders.System
    Samples::Framework
    Foundation::Shell Foundation::Image Foundation::Image.IO Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.Bidi Foundation::Fonts.DistanceField.Baker
    Foundation::VG Foundation::VG.Renderer Experimental::GUI Experimental::GUI.Shell Experimental::GUI.Inspector Experimental::GUI.Testing Foundation::RHI.Capture
    Foundation::Script ${SCRIPTLIB_ANGELSCRIPT}) # the showcase (G6) runs AngelScript
util_copy_sdl3_dll(GUISandbox)

# The image tests (E8g of Specs/gui-lab-evolution.md): each page drawn offscreen on each backend
# and compared with its reference in Data/Tests/GUI/Images; skipped (77) without a device.
foreach(backend vulkan webgpu)
    add_test(NAME GUI.Images.${backend}
             COMMAND GUISandbox --${backend} --image-tests --image-out ${CMAKE_BINARY_DIR}/gui-images)
    set_tests_properties(GUI.Images.${backend} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
