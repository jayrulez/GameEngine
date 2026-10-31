# Foundation::Render unit tests (doctest; Null RHI, no real GPU).

add_executable(Render.Tests
    TestsMain.cpp
    GpuMeshTests.cpp
    RenderDataTests.cpp
    PipelineTests.cpp
    PoseSelectionTests.cpp
    LodSelectionTests.cpp
    RetireQueueTests.cpp
    PickingTests.cpp
    MultiMeshLifecycleTests.cpp WindTests.cpp DebugScreenTextTests.cpp BindGroupCacheTests.cpp)

target_link_libraries(Render.Tests PRIVATE Foundation::VFS
    Foundation::Render Foundation::RenderGraph Foundation::RHI.Null Foundation::Geometry Foundation::Materials Foundation::Materials.PipelineCache Foundation::Shaders.System doctest::doctest)

target_compile_definitions(Render.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Render.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Render.Tests COMMAND Render.Tests)
