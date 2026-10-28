add_executable(Editor.Physics.Tests TestsMain.cpp CollisionShapeEditorTests.cpp)
target_link_libraries(Editor.Physics.Tests PRIVATE
    Editor::Physics Editor::App Editor::Core Foundation::Physics.Resource Pipeline::Physics Foundation::Geometry.Resource
    Foundation::GUI Foundation::GUI.Toolkit
    Foundation::Geometry Foundation::Scene Foundation::Resource Foundation::Content Foundation::VFS doctest::doctest)
target_compile_definitions(Editor.Physics.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Editor.Physics.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Physics.Tests COMMAND Editor.Physics.Tests)
