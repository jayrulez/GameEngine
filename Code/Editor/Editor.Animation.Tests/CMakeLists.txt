# Editor::Animation tests: the animation pages over their markup and a scratch project (the clip
# page so far; the graph page's old tests come back with its part).
add_executable(Editor.Animation.Tests TestsMain.cpp AnimationClipPageTests.cpp)
target_link_libraries(Editor.Animation.Tests PRIVATE
    Editor::Animation Editor::App Editor::Core Foundation::Content Foundation::Animation
    Foundation::Animation.Resource Pipeline::Animation Pipeline::Core
    Foundation::GUI Foundation::GUI.Toolkit doctest::doctest)
target_compile_definitions(Editor.Animation.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the pages' markup the tests load
target_compile_options(Editor.Animation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Animation.Tests COMMAND Editor.Animation.Tests)
