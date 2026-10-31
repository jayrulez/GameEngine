# The old UI framework, foundation.ui, with its satellites and suites, and the
# UISandbox that shows it. Deprecated: the engine, the editor and the games are on foundation.gui.
# Nothing outside Code/Deprecated links these; they build only with -DOPTION_BUILD_DEPRECATED=ON.
add_subdirectory(UI)
add_subdirectory(UI.VFS)
add_subdirectory(UI.Shell)
add_subdirectory(UI.Toolkit)
# The desktop hosts (UIHost and the docking workbench) link Graphics and Runtime.Client.
if(NOT EMSCRIPTEN)
    add_subdirectory(UI.Runtime)
    add_subdirectory(UI.Application)
endif()

if(OPTION_BUILD_TESTS)
    util_test_suite(UI.Tests             Foundation::UI)
    util_test_suite(UI.VFS.Tests         Foundation::UI.VFS)
    util_test_suite(UI.Shell.Tests       Foundation::UI.Shell)
    util_test_suite(UI.Toolkit.Tests     Foundation::UI.Toolkit)
    util_test_suite(UI.Application.Tests Foundation::UI.Application)
endif()

if(OPTION_BUILD_SAMPLES AND TARGET Foundation::UI.Application)
    add_subdirectory(UISandbox)
endif()
