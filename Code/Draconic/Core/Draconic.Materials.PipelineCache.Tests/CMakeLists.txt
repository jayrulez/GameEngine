# Draconic::Materials.PipelineCache unit tests (doctest; real DXC + Null RHI).

add_executable(DraconicPipelineCacheTests
    TestsMain.cpp
    PipelineCacheTests.cpp)

target_link_libraries(DraconicPipelineCacheTests PRIVATE
    Draconic::Materials.PipelineCache Draconic::RHI.Null doctest::doctest)

target_compile_definitions(DraconicPipelineCacheTests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(DraconicPipelineCacheTests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME DraconicPipelineCacheTests COMMAND DraconicPipelineCacheTests)
