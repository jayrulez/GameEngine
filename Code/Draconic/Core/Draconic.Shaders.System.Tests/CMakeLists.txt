# Draconic::Shaders.System unit tests (doctest; real DXC + Null RHI).

add_executable(Draconic.Shaders.System.Tests
    TestsMain.cpp
    ShaderSystemTests.cpp
    FileProviderTests.cpp)

target_link_libraries(Draconic.Shaders.System.Tests PRIVATE
    Draconic::Shaders.System Draconic::RHI.Null doctest::doctest)

target_compile_definitions(Draconic.Shaders.System.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Shaders.System.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Shaders.System.Tests COMMAND Draconic.Shaders.System.Tests)
