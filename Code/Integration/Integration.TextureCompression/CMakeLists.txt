# Integration.TextureCompression - the block-compression path exercised end to end on real GPU
# backends: encode with the cook's encoder (Pipeline::Texture.Compression), then upload and sample
# the BC texture through the Foundation render chain (Vulkan and WebGPU). The flow crosses
# collections (the Pipeline encoder and the Foundation render), so it lives in Code/Integration,
# not in a Foundation test. Skips cleanly when no GPU backend is available.
# With no GPU backend configured the probe would compile into a test that passes vacuously
# (every #ifdef block empty, one MESSAGE, ctest green with zero assertions), which reads as
# coverage. Skip the target entirely instead, as Samples::Framework does.
if(NOT TARGET Foundation::RHI.Vulkan AND NOT TARGET Foundation::RHI.WebGPU
   AND NOT TARGET Foundation::RHI.DX12)
    message(STATUS "Integration.TextureCompression: no GPU backend configured - skipped")
    return()
endif()

add_executable(Integration.TextureCompression TestsMain.cpp BcTextureProbeTests.cpp)

target_link_libraries(Integration.TextureCompression PRIVATE
    Foundation::Render Foundation::Materials.PipelineCache Foundation::RHI.TestSupport
    Pipeline::Texture.Compression doctest::doctest)

# Link each GPU backend that is present and flag it (OPTION_HAS_*); the probe iterates whichever
# are compiled in. None is hardcoded, so this builds on any platform (Vulkan and WebGPU on Linux,
# DX12 and WebGPU on Windows).
if(TARGET Foundation::RHI.Vulkan)
    target_link_libraries(Integration.TextureCompression PRIVATE Foundation::RHI.Vulkan)
    target_compile_definitions(Integration.TextureCompression PRIVATE OPTION_HAS_VULKAN)
endif()
if(TARGET Foundation::RHI.WebGPU)
    target_link_libraries(Integration.TextureCompression PRIVATE Foundation::RHI.WebGPU)
    target_compile_definitions(Integration.TextureCompression PRIVATE OPTION_HAS_WEBGPU)
endif()
if(TARGET Foundation::RHI.DX12)
    target_link_libraries(Integration.TextureCompression PRIVATE Foundation::RHI.DX12)
    target_compile_definitions(Integration.TextureCompression PRIVATE OPTION_HAS_DX12)
endif()

target_compile_definitions(Integration.TextureCompression PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Integration.TextureCompression PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Integration.TextureCompression COMMAND Integration.TextureCompression)
