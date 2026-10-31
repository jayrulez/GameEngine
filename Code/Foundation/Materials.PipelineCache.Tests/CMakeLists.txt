# Foundation::Materials.PipelineCache unit tests (doctest; real DXC and the Null RHI).

add_executable(Materials.PipelineCache.Tests
    TestsMain.cpp
    PipelineCacheTests.cpp)

target_link_libraries(Materials.PipelineCache.Tests PRIVATE
    Foundation::Materials.PipelineCache Foundation::RHI.Null doctest::doctest)

target_compile_definitions(Materials.PipelineCache.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Materials.PipelineCache.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Materials.PipelineCache.Tests COMMAND Materials.PipelineCache.Tests)
