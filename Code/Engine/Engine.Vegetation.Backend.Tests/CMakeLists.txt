# Engine::Vegetation backend pixel probe: a splat-driven grass layer over a flat terrain, rendered
# through the full RenderFrame chain on real devices (Vulkan as the reference, WebGPU matching it,
# DX12 where it exists) and read back. It checks that the per-chunk instanced sets draw where the
# splat is painted and nowhere else, and that the fade prefix thins them with distance. GPUs are
# checked at runtime; each backend skips cleanly when unavailable.
add_executable(Engine.Vegetation.Backend.Tests
    TestsMain.cpp
    VegetationPixelProbeTests.cpp)

target_link_libraries(Engine.Vegetation.Backend.Tests PRIVATE
    Engine::Vegetation Engine::Terrain Foundation::Scene Foundation::Render Foundation::RenderGraph
    Foundation::Materials.PipelineCache Foundation::Geometry Foundation::Materials
    Foundation::Heightfield Foundation::Terrain Foundation::Terrain.Resource Foundation::Vegetation
    Foundation::Shaders Foundation::Shaders.System
    Foundation::RHI.TestSupport Foundation::RHI.Vulkan Foundation::RHI.WebGPU doctest::doctest)

target_compile_definitions(Engine.Vegetation.Backend.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

if(TARGET Foundation::RHI.DX12)
    target_link_libraries(Engine.Vegetation.Backend.Tests PRIVATE Foundation::RHI.DX12)
    target_compile_definitions(Engine.Vegetation.Backend.Tests PRIVATE OPTION_HAS_DX12)
endif()

target_compile_options(Engine.Vegetation.Backend.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Engine.Vegetation.Backend.Tests COMMAND Engine.Vegetation.Backend.Tests)
