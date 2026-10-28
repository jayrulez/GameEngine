add_executable(Editor.Fonts.Tests TestsMain.cpp FontThumbnailTests.cpp FontEditorPageTests.cpp)
target_link_libraries(Editor.Fonts.Tests PRIVATE Foundation::VFS
    Editor::Fonts Editor::App Editor::Core Pipeline::Fonts Foundation::Content
    Foundation::Image Foundation::GUI Foundation::GUI.Toolkit doctest::doctest)
target_compile_definitions(Editor.Fonts.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup and the bundled face the tests load
target_compile_options(Editor.Fonts.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Fonts.Tests COMMAND Editor.Fonts.Tests)
