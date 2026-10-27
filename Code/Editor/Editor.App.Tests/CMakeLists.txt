add_executable(Editor.App.Tests TestsMain.cpp PropertyNamesTests.cpp EditorWindowToolsTests.cpp EditorInputToolsTests.cpp CodeLanguagesTests.cpp ProblemsListTests.cpp EditorProfilerToolsTests.cpp
    FontAtlasDiskCacheTests.cpp EditorMcpHostTests.cpp EditorProjectOperationsTests.cpp EditorPageToolsTests.cpp
    EditorSplashTests.cpp UIPageTests.cpp PageActionBarTests.cpp ToolPanelTests.cpp EditorSessionTests.cpp PropertySectionsTests.cpp EditorWindowTests.cpp EditorStatusBarTests.cpp EditorDockTests.cpp EditorPagesTests.cpp ActionMenusTests.cpp ActionShortcutsTests.cpp CommandPaletteTests.cpp PreferencesDialogTests.cpp ShortcutCaptureTests.cpp ProjectHomeTests.cpp ProjectLauncherTests.cpp EditorLayoutToolsTests.cpp ActionToolsTests.cpp LogViewTests.cpp AssetDragTests.cpp AssetPickerSlotTests.cpp AssetPickerWindowTests.cpp ResourceRefEditorTests.cpp PickerTests.cpp AssetCreateTests.cpp ProjectSettingsTests.cpp TemplatesDialogTests.cpp ExportDialogTests.cpp AssetsViewTests.cpp AssetOperationsTests.cpp AssetImportTests.cpp ImportDialogTests.cpp EditorNativeModuleTests.cpp EditorExportTests.cpp EditorAssetCreationTests.cpp EditorAppActionsTests.cpp LayoutStoreTests.cpp ImageInspectViewTests.cpp ProfilerPanelTests.cpp) # the shell's files on the GUI so far; the others' suites return with them
target_link_libraries(Editor.App.Tests PRIVATE Foundation::VFS Foundation::Content Pipeline::Importer Pipeline::Core
    Foundation::Net Foundation::Http Foundation::Json Foundation::Mcp Foundation::Mcp.Http Editor::Mcp # the MCP host over loopback
    Editor::App Editor::Core doctest::doctest)
target_compile_definitions(Editor.App.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the branding files the splash and logo tests read
target_compile_options(Editor.App.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.App.Tests COMMAND Editor.App.Tests)
