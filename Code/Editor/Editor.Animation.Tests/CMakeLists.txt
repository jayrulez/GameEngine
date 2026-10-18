# Editor::Animation tests: the animation graph page's edits and the graph creator's path through a
# project.
add_executable(Editor.Animation.Tests TestsMain.cpp AnimationGraphPageTests.cpp AnimationGraphCreatorTests.cpp)
target_link_libraries(Editor.Animation.Tests PRIVATE
    Editor::Animation Editor::Core Foundation::Content Foundation::Animation
    Foundation::Animation.Resource Pipeline::Animation Pipeline::Core doctest::doctest)
target_compile_definitions(Editor.Animation.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.Animation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Animation.Tests COMMAND Editor.Animation.Tests)
