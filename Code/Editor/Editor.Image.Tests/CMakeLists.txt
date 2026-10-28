add_executable(Editor.Image.Tests TestsMain.cpp ImageEditorPageTests.cpp)
target_link_libraries(Editor.Image.Tests PRIVATE
    Editor::Image Editor::App Editor::Core
    Foundation::Image Foundation::Image.IO Pipeline::Image Foundation::Content Foundation::GUI Foundation::GUI.Toolkit doctest::doctest)
target_compile_definitions(Editor.Image.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Editor.Image.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Image.Tests COMMAND Editor.Image.Tests)
