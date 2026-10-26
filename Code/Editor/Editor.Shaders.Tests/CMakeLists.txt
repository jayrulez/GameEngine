# Editor::Shaders tests: the shader page's preview slot, and the page over its markup.
add_executable(Editor.Shaders.Tests TestsMain.cpp PreviewShaderSlotTests.cpp ShaderPageTests.cpp)
target_link_libraries(Editor.Shaders.Tests PRIVATE
    Editor::Shaders Editor::App Editor::Core Foundation::Core Foundation::Shaders Foundation::Shaders.System
    Foundation::RHI.Null Foundation::Content Foundation::VFS Foundation::GUI Foundation::GUI.Toolkit Foundation::GUI.Code
    Pipeline::Shaders doctest::doctest)
target_compile_definitions(Editor.Shaders.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup, the languages and the engine's shaders
target_compile_options(Editor.Shaders.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Shaders.Tests COMMAND Editor.Shaders.Tests)
