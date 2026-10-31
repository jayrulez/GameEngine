# Foundation::Materials.Resource unit tests (doctest; real DXC, the Null RHI and a content DB).

add_executable(Materials.Resource.Tests
    TestsMain.cpp
    MaterialResourceTests.cpp)

target_link_libraries(Materials.Resource.Tests PRIVATE
    Foundation::Materials.Resource Foundation::Shaders.System Foundation::RHI.Null Foundation::VFS doctest::doctest)

target_compile_definitions(Materials.Resource.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Materials.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Materials.Resource.Tests COMMAND Materials.Resource.Tests)
