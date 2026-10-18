# Editor::Play tests: the PIE tools over headless Game pages, and the resolutions a Game tab offers.
add_executable(Editor.Play.Tests TestsMain.cpp PieMcpToolsTests.cpp GameResolutionTests.cpp)
target_link_libraries(Editor.Play.Tests PRIVATE
    Editor::Play Editor::Scene Editor::Core Engine::Project Foundation::Input Foundation::Shell
    Foundation::Scene Foundation::Mcp Foundation::Json doctest::doctest)
target_compile_definitions(Editor.Play.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.Play.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Play.Tests COMMAND Editor.Play.Tests)
