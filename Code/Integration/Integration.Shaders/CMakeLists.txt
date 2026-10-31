# Integration.Shaders - a surface shader against the engine's own, on real GPU backends: the
# built-in PBR written as a surface shader (Data/Tests/Shaders/surface_pbr) is cooked by the
# shader pipeline (Pipeline::Shaders) and drawn by the Foundation render chain beside `forward`,
# and the two images must agree. The flow crosses collections (the Pipeline cook and the Foundation
# render), so it lives in Code/Integration. Skips cleanly when no GPU backend is available.
# With no GPU backend configured the probe would compile into a test that passes vacuously
# (every #ifdef block empty, one MESSAGE, ctest green with zero assertions), which reads as
# coverage. Skip the target entirely instead, as Samples::Framework does.
if(NOT TARGET Foundation::RHI.Vulkan AND NOT TARGET Foundation::RHI.WebGPU
   AND NOT TARGET Foundation::RHI.DX12)
    message(STATUS "Integration.Shaders: no GPU backend configured - skipped")
    return()
endif()

add_executable(Integration.Shaders TestsMain.cpp SurfaceEquivalenceProbeTests.cpp)

target_link_libraries(Integration.Shaders PRIVATE
    Foundation::Render Foundation::Materials.PipelineCache Foundation::RHI.TestSupport
    Pipeline::Shaders Foundation::VFS doctest::doctest)

# Link each GPU backend that is present and flag it (OPTION_HAS_*); the probe iterates whichever
# are compiled in. None is hardcoded, so this builds on any platform (Vulkan and WebGPU on Linux,
# DX12 and WebGPU on Windows).
if(TARGET Foundation::RHI.Vulkan)
    target_link_libraries(Integration.Shaders PRIVATE Foundation::RHI.Vulkan)
    target_compile_definitions(Integration.Shaders PRIVATE OPTION_HAS_VULKAN)
endif()
if(TARGET Foundation::RHI.WebGPU)
    target_link_libraries(Integration.Shaders PRIVATE Foundation::RHI.WebGPU)
    target_compile_definitions(Integration.Shaders PRIVATE OPTION_HAS_WEBGPU)
endif()
if(TARGET Foundation::RHI.DX12)
    target_link_libraries(Integration.Shaders PRIVATE Foundation::RHI.DX12)
    target_compile_definitions(Integration.Shaders PRIVATE OPTION_HAS_DX12)
endif()

target_compile_definitions(Integration.Shaders PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Integration.Shaders PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Integration.Shaders COMMAND Integration.Shaders)
