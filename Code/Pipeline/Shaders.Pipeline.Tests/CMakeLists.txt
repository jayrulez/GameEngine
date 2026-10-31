# Pipeline::Shaders unit tests (doctest; VFS, content DB and DXC; no GPU).

add_executable(Shaders.Pipeline.Tests
    TestsMain.cpp
    ShaderCheckTests.cpp
    ShaderCreatorTests.cpp
    SurfaceShaderTests.cpp
    ShaderEditorTests.cpp
    ShaderReflectionTests.cpp)

target_link_libraries(Shaders.Pipeline.Tests PRIVATE
    Pipeline::Shaders Pipeline::Importer Foundation::Shaders Foundation::VFS doctest::doctest)

target_compile_definitions(Shaders.Pipeline.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Shaders.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Shaders.Pipeline.Tests COMMAND Shaders.Pipeline.Tests)
