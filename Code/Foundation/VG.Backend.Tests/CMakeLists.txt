# Foundation::VG pixel-probe ("golden") tests: render deterministic VG scenes on REAL backends
# (Vulkan + WebGPU) single-sampled + stencil, read the pixels back, and assert SEMANTIC pixel
# properties (fill correctness, spreads, blends, clip, solid-vs-gradient color agreement).
# Structural probes instead of stored image diffs: no cross-driver golden drift, and each
# assertion names the property it guards. Skips cleanly when a backend/GPU is unavailable.

add_executable(VG.Backend.Tests
    TestsMain.cpp
    VGPixelProbeTests.cpp)

target_link_libraries(VG.Backend.Tests PRIVATE
    Foundation::VG Foundation::VG.Renderer Foundation::Shaders.System
    Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.Coverage.Baker Foundation::Fonts.Coverage
    Foundation::Fonts.Resource Foundation::Fonts.DistanceField Foundation::Fonts.DistanceField.Baker
    Foundation::Image Foundation::Image.IO
    Foundation::RHI.TestSupport Foundation::RHI.Vulkan Foundation::RHI.WebGPU doctest::doctest)

target_compile_definitions(VG.Backend.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(VG.Backend.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME VG.Backend.Tests COMMAND VG.Backend.Tests)
