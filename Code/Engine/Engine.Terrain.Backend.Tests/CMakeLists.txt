# Engine::Terrain backend pixel probe: render a lit terrain through the full RenderFrame chain on real
# devices and read the pixels back. The Null test proves the terrain shaders compile; this proves they
# draw correct, shaded terrain, and that the backends agree: Vulkan is the reference, and WebGPU
# (desktop and the web WGSL cook) and DX12 (Windows DXIL) must match it. GPUs are checked at runtime;
# each backend skips cleanly when unavailable.
add_executable(Engine.Terrain.Backend.Tests
    TestsMain.cpp
    TerrainPixelProbeTests.cpp)

target_link_libraries(Engine.Terrain.Backend.Tests PRIVATE
    Engine::Terrain Foundation::Render Foundation::Terrain Foundation::Terrain.Resource
    Foundation::Heightfield
    Foundation::Texture.Resource Foundation::Shaders Foundation::Shaders.System
    Foundation::RHI.TestSupport Foundation::RHI.Vulkan Foundation::RHI.WebGPU doctest::doctest)

target_compile_definitions(Engine.Terrain.Backend.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

# DX12 backend, Windows only. Cross-check the terrain shaders' DXIL cook too.
if(TARGET Foundation::RHI.DX12)
    target_link_libraries(Engine.Terrain.Backend.Tests PRIVATE Foundation::RHI.DX12)
    target_compile_definitions(Engine.Terrain.Backend.Tests PRIVATE OPTION_HAS_DX12)
endif()

target_compile_options(Engine.Terrain.Backend.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Engine.Terrain.Backend.Tests COMMAND Engine.Terrain.Backend.Tests)
