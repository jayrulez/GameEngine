# Foundation::Shaders.Resource unit tests (doctest; real DXC, the Null RHI and a content DB).

add_executable(Shaders.Resource.Tests
    TestsMain.cpp
    ShaderResourceTests.cpp)

target_link_libraries(Shaders.Resource.Tests PRIVATE
    Foundation::Shaders.Resource Foundation::RHI.Null Foundation::VFS doctest::doctest)

target_compile_definitions(Shaders.Resource.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Shaders.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Shaders.Resource.Tests COMMAND Shaders.Resource.Tests)
