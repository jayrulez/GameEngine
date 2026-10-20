# GUI Sandbox - first on-screen test of experimental.gui (widgets + CSS on the VG renderer).
add_executable(GUISandbox Main.cpp)
target_sources(GUISandbox PRIVATE FILE_SET sandbox_modules TYPE CXX_MODULES FILES StressPage.cppm) # the Stress page (E8a)
target_link_libraries(GUISandbox PRIVATE
    Foundation::Core Foundation::RHI Foundation::Shaders Foundation::Shaders.System
    Samples::Framework
    Foundation::Shell Foundation::Image Foundation::Image.IO Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.Bidi Foundation::Fonts.HarfBuzz Foundation::Xml.Serialization Foundation::Fonts.DistanceField.Baker
    Foundation::VG Foundation::VG.Renderer Experimental::GUI Experimental::GUI.Shell Experimental::GUI.Inspector Experimental::GUI.Testing Experimental::GUI.Bake Foundation::RHI.Capture
    Foundation::Script ${SCRIPTLIB_ANGELSCRIPT}) # the showcase (G6) runs AngelScript
util_copy_sdl3_dll(GUISandbox)

# A sanitized run reports only our leaks, not what the desktop's libraries keep (lsan.supp says
# which); other builds ignore the variable.
set(_sandbox_test_env "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")

# The image tests (E8g of Specs/gui-lab-evolution.md): each page drawn offscreen on each backend
# and compared with its reference in Data/Tests/GUI/Images; skipped (77) without a device.
foreach(backend vulkan webgpu)
    add_test(NAME GUI.Images.${backend}
             COMMAND GUISandbox --${backend} --image-tests --image-out ${CMAKE_BINARY_DIR}/gui-images)
    set_tests_properties(GUI.Images.${backend} PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "${_sandbox_test_env}")
endforeach()

# The sweep (E10 of Specs/gui-lab-evolution-2.md): every page's widgets clicked and right-clicked,
# every page walked by keys and the pad, the game menu opened, on each backend; it fails when
# anything warns or errs while it runs, and is skipped (77) without a device.
foreach(backend vulkan webgpu)
    add_test(NAME GUI.Sweep.${backend} COMMAND GUISandbox --${backend} --sweep)
    set_tests_properties(GUI.Sweep.${backend} PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "${_sandbox_test_env}")
    # The same in Arabic, right to left (E17a of Specs/gui-lab-evolution-2.md).
    add_test(NAME GUI.Sweep.${backend}.ar COMMAND GUISandbox --${backend} --sweep --lang ar)
    set_tests_properties(GUI.Sweep.${backend}.ar PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "${_sandbox_test_env}")
endforeach()

# The mirror audit (E17a): every page laid out both ways, every widget where the mirror of its left-to-
# right place puts it. Layout only, so one backend; skipped (77) without a device.
add_test(NAME GUI.Mirror COMMAND GUISandbox --vulkan --mirror-audit)
set_tests_properties(GUI.Mirror PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "${_sandbox_test_env}")
