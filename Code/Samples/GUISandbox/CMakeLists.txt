# GUI Sandbox - foundation.gui's widgets, styles, motion and input, an application on the runtime host
# through the GUI's GuiHost (GUI.Hosting).
add_executable(GUISandbox Main.cpp ReflectedCameraImpl.cpp MockTypesImpl.cpp)
target_sources(GUISandbox PRIVATE FILE_SET sandbox_modules TYPE CXX_MODULES FILES StressPage.cppm LampSource.cppm ReflectedCamera.cppm CodePage.cppm MockTypes.cppm EditorMock.cppm) # the Stress page, the Toolkit page's lamps and camera, the Code page, the editor mock
target_link_libraries(GUISandbox PRIVATE
    Foundation::Core Foundation::RHI Foundation::VFS
    Foundation::Runtime Foundation::Runtime.Client Foundation::Runtime.Desktop Foundation::Shell.Desktop
    Foundation::Graphics Foundation::Graphics.Gpu
    Foundation::Shell Foundation::Image Foundation::Image.IO Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.Bidi Foundation::Fonts.HarfBuzz Foundation::Xml.Serialization Foundation::Fonts.DistanceField.Baker
    Foundation::VG Foundation::VG.Renderer Foundation::GUI Foundation::GUI.Shell Foundation::GUI.Inspector Foundation::GUI.Testing Foundation::GUI.Hosting Foundation::GUI.Viewport Foundation::GUI.Docking Foundation::GUI.Toolkit Foundation::GUI.Code Foundation::GUI.Gamekit Foundation::Input Foundation::Shaders Foundation::RHI.Capture
    Foundation::Script ${SCRIPTLIB_ANGELSCRIPT}) # the showcase runs AngelScript
util_copy_sdl3_dll(GUISandbox)

# A sanitized run reports only our leaks, not what the desktop's libraries keep (lsan.supp says
# which); other builds ignore the variable.
set(_sandbox_test_env "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")

# The image tests: each page drawn offscreen on each backend
# and compared with its reference in Data/Tests/GUI/Images; skipped (77) without a device.
foreach(backend vulkan webgpu)
    add_test(NAME GUI.Images.${backend}
             COMMAND GUISandbox --${backend} --image-tests --image-out ${CMAKE_BINARY_DIR}/gui-images)
    set_tests_properties(GUI.Images.${backend} PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "${_sandbox_test_env}")
endforeach()

# The sweep: every page's widgets clicked and right-clicked, every page walked by keys and the
# pad, the game menu opened, on each backend. It fails when anything warns or errs while it runs.
# It is skipped (77) without a device, and on WebGPU on a CPU adapter, where a sweep takes over
# twenty minutes (Windows CI's WARP) while the Vulkan sweep covers the same GUI in about one. Debug's sweep is slow on some machines, so the sweeps have 1200 seconds.
foreach(backend vulkan webgpu)
    add_test(NAME GUI.Sweep.${backend} COMMAND GUISandbox --${backend} --sweep)
    set_tests_properties(GUI.Sweep.${backend} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200 ENVIRONMENT "${_sandbox_test_env}")
    # The same in Arabic, right to left.
    add_test(NAME GUI.Sweep.${backend}.ar COMMAND GUISandbox --${backend} --sweep --lang ar)
    set_tests_properties(GUI.Sweep.${backend}.ar PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200 ENVIRONMENT "${_sandbox_test_env}")
endforeach()

# The mirror audit: every page laid out both ways, every widget where the mirror of its left-to-
# right place puts it. Layout only, so one backend; skipped (77) without a device.
add_test(NAME GUI.Mirror COMMAND GUISandbox --vulkan --mirror-audit)
set_tests_properties(GUI.Mirror PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "${_sandbox_test_env}")
