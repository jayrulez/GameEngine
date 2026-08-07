# Draconic::Shaders.Resource unit tests (doctest; real DXC + Null RHI + content DB).

add_executable(Draconic.Shaders.Resource.Tests
    TestsMain.cpp
    ShaderResourceTests.cpp)

target_link_libraries(Draconic.Shaders.Resource.Tests PRIVATE
    Draconic::Shaders.Resource Draconic::RHI.Null Draconic::VFS doctest::doctest)

target_compile_definitions(Draconic.Shaders.Resource.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Shaders.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Shaders.Resource.Tests COMMAND Draconic.Shaders.Resource.Tests)
