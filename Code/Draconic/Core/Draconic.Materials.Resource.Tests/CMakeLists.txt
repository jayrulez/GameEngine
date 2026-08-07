# Draconic::Materials.Resource unit tests (doctest; real DXC + Null RHI + content DB).

add_executable(Draconic.Materials.Resource.Tests
    TestsMain.cpp
    MaterialResourceTests.cpp)

target_link_libraries(Draconic.Materials.Resource.Tests PRIVATE
    Draconic::Materials.Resource Draconic::Shaders.System Draconic::RHI.Null Draconic::VFS doctest::doctest)

target_compile_definitions(Draconic.Materials.Resource.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Materials.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Materials.Resource.Tests COMMAND Draconic.Materials.Resource.Tests)
