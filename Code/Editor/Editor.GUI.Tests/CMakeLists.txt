# Editor::GUI tests: the document and sheet pages over their markup.
add_executable(Editor.GUI.Tests TestsMain.cpp GuiPagesTests.cpp)
target_link_libraries(Editor.GUI.Tests PRIVATE
    Editor::GUI Editor::App Editor::Core Foundation::Content Foundation::VFS Foundation::Settings
    Foundation::GUI Foundation::GUI.Toolkit Foundation::GUI.Code Pipeline::GUI doctest::doctest)
target_compile_definitions(Editor.GUI.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup and the languages the tests load
target_compile_options(Editor.GUI.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.GUI.Tests COMMAND Editor.GUI.Tests)
