add_executable(Editor.Texture.Tests TestsMain.cpp TextureEditorPageTests.cpp)
target_link_libraries(Editor.Texture.Tests PRIVATE
    Editor::Texture Editor::App Editor::Core Foundation::GUI Foundation::GUI.Toolkit
    Foundation::Texture Pipeline::Texture Pipeline::Texture.Compression Foundation::RHI Foundation::Image Foundation::Image.IO
    Foundation::Content Foundation::VFS doctest::doctest)
target_compile_definitions(Editor.Texture.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Editor.Texture.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Texture.Tests COMMAND Editor.Texture.Tests)
