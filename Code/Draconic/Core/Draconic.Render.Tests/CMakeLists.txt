# Draconic::Render unit tests (doctest; Null RHI - no real GPU).

add_executable(Draconic.Render.Tests
    TestsMain.cpp
    GpuMeshTests.cpp
    RenderDataTests.cpp
    PipelineTests.cpp
    PoseSelectionTests.cpp
    RetireQueueTests.cpp)

target_link_libraries(Draconic.Render.Tests PRIVATE
    Draconic::Render Draconic::RHI.Null doctest::doctest)

target_compile_definitions(Draconic.Render.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    # Tests read the engine built-in shaders from the source tree, like dev runs do.
    DRACONIC_ENGINE_SHADER_DIR="${CMAKE_SOURCE_DIR}/Data/Shaders")

target_compile_options(Draconic.Render.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Render.Tests COMMAND Draconic.Render.Tests)
