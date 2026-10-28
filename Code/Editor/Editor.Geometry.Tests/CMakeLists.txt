# Editor::Geometry tests: the mesh page.
add_executable(Editor.Geometry.Tests TestsMain.cpp MeshPageTests.cpp)
target_link_libraries(Editor.Geometry.Tests PRIVATE
    Editor::Geometry Editor::App Editor::Core Foundation::Content Foundation::Settings Foundation::Geometry Pipeline::Geometry
    Foundation::GUI Foundation::GUI.Toolkit doctest::doctest)
target_compile_definitions(Editor.Geometry.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Editor.Geometry.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Geometry.Tests COMMAND Editor.Geometry.Tests)
