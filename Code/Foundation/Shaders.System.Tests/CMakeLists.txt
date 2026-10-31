# Foundation::Shaders.System unit tests (doctest; real DXC and the Null RHI).

add_executable(Shaders.System.Tests
    TestsMain.cpp
    ShaderSystemTests.cpp
    FileProviderTests.cpp)

target_link_libraries(Shaders.System.Tests PRIVATE
    Foundation::Shaders.System Foundation::RHI.Null doctest::doctest)

target_compile_definitions(Shaders.System.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Shaders.System.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Shaders.System.Tests COMMAND Shaders.System.Tests)
