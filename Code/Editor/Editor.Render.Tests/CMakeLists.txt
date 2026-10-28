# Editor::Render tests: the settings profile page's rows, edits and preview, and its kept preview scene.
add_executable(Editor.Render.Tests TestsMain.cpp SettingsProfilePageTests.cpp)
target_link_libraries(Editor.Render.Tests PRIVATE
    Editor::Render Editor::App Editor::Core Foundation::Content Foundation::Settings Foundation::Scene Engine::Render Pipeline::Render
    Foundation::GUI Foundation::GUI.Toolkit doctest::doctest)
target_compile_definitions(Editor.Render.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Editor.Render.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Render.Tests COMMAND Editor.Render.Tests)
