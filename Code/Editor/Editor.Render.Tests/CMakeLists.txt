# Editor::Render tests: the settings profile page's preview and its kept preview scene.
add_executable(Editor.Render.Tests TestsMain.cpp SettingsProfilePageTests.cpp)
target_link_libraries(Editor.Render.Tests PRIVATE
    Editor::Render Editor::Core Foundation::Scene Engine::Render Pipeline::Render doctest::doctest)
target_compile_definitions(Editor.Render.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.Render.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Render.Tests COMMAND Editor.Render.Tests)
