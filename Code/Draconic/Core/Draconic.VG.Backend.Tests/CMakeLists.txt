# Draconic::VG pixel-probe ("golden") tests: render deterministic VG scenes on REAL backends
# (Vulkan + WebGPU) single-sampled + stencil, read the pixels back, and assert SEMANTIC pixel
# properties (fill correctness, spreads, blends, clip, solid-vs-gradient color agreement).
# Structural probes instead of stored image diffs: no cross-driver golden drift, and each
# assertion names the property it guards. Skips cleanly when a backend/GPU is unavailable.

add_executable(Draconic.VG.Backend.Tests
    TestsMain.cpp
    VGPixelProbeTests.cpp)

target_link_libraries(Draconic.VG.Backend.Tests PRIVATE
    Draconic::VG Draconic::VG.Renderer Draconic::Shaders.System
    Draconic::RHI.Vulkan Draconic::RHI.WebGPU doctest::doctest)

target_compile_definitions(Draconic.VG.Backend.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    DRACONIC_ENGINE_SHADER_DIR="${CMAKE_SOURCE_DIR}/Data/Shaders")

target_compile_options(Draconic.VG.Backend.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.VG.Backend.Tests COMMAND Draconic.VG.Backend.Tests)
