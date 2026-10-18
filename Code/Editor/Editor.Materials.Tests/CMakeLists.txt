# Editor::Materials tests: the material page's edits, undo and preview.
add_executable(Editor.Materials.Tests TestsMain.cpp MaterialPageTests.cpp)
target_link_libraries(Editor.Materials.Tests PRIVATE
    Editor::Materials Editor::Core Foundation::Content Foundation::Materials Pipeline::Materials
    doctest::doctest)
target_compile_definitions(Editor.Materials.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.Materials.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Materials.Tests COMMAND Editor.Materials.Tests)
