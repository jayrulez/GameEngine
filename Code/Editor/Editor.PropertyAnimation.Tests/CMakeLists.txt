# Editor::PropertyAnimation tests: the panel's UI-free half (the clip document, the clip source, the
# preview, the seeds, the transport) over a scratch project and scene.
add_executable(Editor.PropertyAnimation.Tests TestsMain.cpp ClipTests.cpp)
target_link_libraries(Editor.PropertyAnimation.Tests PRIVATE
    Editor::PropertyAnimation Editor::Core Foundation::Content
    Foundation::PropertyAnimation Foundation::PropertyAnimation.Resource Pipeline::PropertyAnimation
    Foundation::Scene Foundation::GUI Foundation::GUI.Toolkit
    doctest::doctest)
target_compile_definitions(Editor.PropertyAnimation.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.PropertyAnimation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.PropertyAnimation.Tests COMMAND Editor.PropertyAnimation.Tests)
