add_executable(Editor.App.Tests TestsMain.cpp ShellTests.cpp EditorWindowToolsTests.cpp AssetsViewTests.cpp ActionSurfacesTests.cpp PageToolbarTests.cpp ActionToolsTests.cpp LogViewTests.cpp ToolPanelTests.cpp AssetPickerSlotTests.cpp ContainerListEditorTests.cpp FontAtlasDiskCacheTests.cpp ImportDialogTests.cpp EditorIconsTests.cpp EditorMcpHostTests.cpp EditorProjectOperationsTests.cpp EditorPageToolsTests.cpp CommandPaletteTests.cpp ShortcutCaptureTests.cpp TemplatesDialogTests.cpp ExportDialogTests.cpp EditorSplashTests.cpp ProjectLauncherTests.cpp)
target_link_libraries(Editor.App.Tests PRIVATE Foundation::VFS Foundation::Content Pipeline::Importer Pipeline::Core
    Foundation::Net Foundation::Http Foundation::Json Foundation::Mcp Foundation::Mcp.Http Editor::Mcp # the MCP host over loopback
    Editor::App Editor::Core Editor::ViewportTools doctest::doctest)
target_compile_definitions(Editor.App.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the branding files the splash and logo tests read
target_compile_options(Editor.App.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.App.Tests COMMAND Editor.App.Tests)
