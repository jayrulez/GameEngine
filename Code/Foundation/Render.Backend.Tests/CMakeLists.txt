# Foundation::Render backend-orientation tests: render one asymmetric scene through the FULL
# RenderFrame chain on REAL backends (Vulkan + WebGPU) and read the pixels back - the pixel-level
# ground truth for the Y-flip bug class (each backend must put the cube on the same side of the
# screen in every TAA mode). Skips cleanly when a backend/GPU is unavailable.

add_executable(Render.Backend.Tests
    TestsMain.cpp
    BackendOrientationTests.cpp
    MsaaProbeTests.cpp
    ReverseZProbeTests.cpp
    PickProbeTests.cpp
    WindProbeTests.cpp
    InstanceFadeProbeTests.cpp
    DitherFadeProbeTests.cpp
    MaterialOverrideProbeTests.cpp
    EmissiveProbeTests.cpp
    ScaledViewProbeTests.cpp ColorProbeTests.cpp DebugGridProbeTests.cpp ShadowControlProbeTests.cpp
    ScreenSpaceProbeTests.cpp TaaProbeTests.cpp)

target_link_libraries(Render.Backend.Tests PRIVATE
    Foundation::Render Foundation::Materials.PipelineCache Foundation::RHI.TestSupport
    Foundation::RHI.Vulkan Foundation::RHI.WebGPU doctest::doctest)

target_compile_definitions(Render.Backend.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

# DX12 backend - Windows only. The MSAA probe ran on Vulkan + WebGPU only, so DX12's MSAA path
# had no coverage at all (and shipped reporting a 1x ceiling unnoticed).
if(TARGET Foundation::RHI.DX12)
    target_link_libraries(Render.Backend.Tests PRIVATE Foundation::Image Foundation::Image.IO Foundation::RHI.DX12)
    target_compile_definitions(Render.Backend.Tests PRIVATE OPTION_HAS_DX12)
endif()

target_compile_options(Render.Backend.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Render.Backend.Tests COMMAND Render.Backend.Tests)
