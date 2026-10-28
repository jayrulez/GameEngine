add_executable(Editor.Heightfield.Tests TestsMain.cpp HeightfieldEditorPageTests.cpp HeightfieldThumbnailTests.cpp)
target_link_libraries(Editor.Heightfield.Tests PRIVATE
    Editor::Heightfield Editor::App Editor::Core
    Pipeline::Heightfield Foundation::VFS Foundation::Content Foundation::Image Foundation::Image.IO
    Foundation::GUI Foundation::GUI.Toolkit doctest::doctest)
target_compile_definitions(Editor.Heightfield.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Editor.Heightfield.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Heightfield.Tests COMMAND Editor.Heightfield.Tests)
