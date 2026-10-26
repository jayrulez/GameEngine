# Editor::Input tests: the input map's edits, the grid's rows, and the page over its markup.
add_executable(Editor.Input.Tests TestsMain.cpp InputMapEditTests.cpp)
target_link_libraries(Editor.Input.Tests PRIVATE
    Editor::Input Editor::App Editor::Core Foundation::Input Pipeline::Input
    Foundation::Content Foundation::GUI Foundation::GUI.Toolkit doctest::doctest)
target_compile_definitions(Editor.Input.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Editor.Input.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Input.Tests COMMAND Editor.Input.Tests)
