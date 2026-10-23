add_executable(Editor.GameUI.Tests TestsMain.cpp GameUIEditorTests.cpp)
target_link_libraries(Editor.GameUI.Tests PRIVATE
    Editor::GameUI Editor::Core Foundation::UI Foundation::UI.Toolkit Foundation::UI.Resource Foundation::VG.Resource
    Foundation::Resource Foundation::Content Foundation::VFS doctest::doctest)
target_compile_definitions(Editor.GameUI.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.GameUI.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.GameUI.Tests COMMAND Editor.GameUI.Tests)
