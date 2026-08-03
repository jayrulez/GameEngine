# Draconic::Shaders.Editor unit tests (doctest; VFS + content DB; no GPU/DXC).

add_executable(Draconic.Shaders.Editor.Tests
    TestsMain.cpp
    ShaderEditorTests.cpp
    ShaderReflectionTests.cpp)

target_link_libraries(Draconic.Shaders.Editor.Tests PRIVATE
    Draconic::Shaders.Editor doctest::doctest)

target_compile_definitions(Draconic.Shaders.Editor.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Shaders.Editor.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Shaders.Editor.Tests COMMAND Draconic.Shaders.Editor.Tests)
