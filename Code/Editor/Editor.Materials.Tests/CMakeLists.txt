# Editor::Materials tests: the material page's edits, undo and preview.
add_executable(Editor.Materials.Tests TestsMain.cpp MaterialPageTests.cpp)
target_link_libraries(Editor.Materials.Tests PRIVATE
    Editor::Materials Editor::App Editor::Core Foundation::Content Foundation::Settings Foundation::Materials Foundation::Materials.Resource Pipeline::Materials
    Foundation::Texture.Resource Foundation::GUI Foundation::GUI.Toolkit doctest::doctest)
target_compile_definitions(Editor.Materials.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Editor.Materials.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Materials.Tests COMMAND Editor.Materials.Tests)
