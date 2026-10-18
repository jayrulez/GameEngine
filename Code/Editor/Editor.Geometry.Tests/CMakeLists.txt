# Editor::Geometry tests: the mesh page.
add_executable(Editor.Geometry.Tests TestsMain.cpp MeshPageTests.cpp)
target_link_libraries(Editor.Geometry.Tests PRIVATE
    Editor::Geometry Editor::Core Foundation::Content Foundation::Geometry Pipeline::Geometry
    doctest::doctest)
target_compile_definitions(Editor.Geometry.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.Geometry.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Geometry.Tests COMMAND Editor.Geometry.Tests)
