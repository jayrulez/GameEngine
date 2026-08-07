# Draconic::Render backend-orientation tests: render one asymmetric scene through the FULL
# RenderFrame chain on REAL backends (Vulkan + WebGPU) and read the pixels back - the pixel-level
# ground truth for the Y-flip bug class (each backend must put the cube on the same side of the
# screen in every TAA mode). Skips cleanly when a backend/GPU is unavailable.

add_executable(Draconic.Render.Backend.Tests
    TestsMain.cpp
    BackendOrientationTests.cpp)

target_link_libraries(Draconic.Render.Backend.Tests PRIVATE
    Draconic::Render Draconic::Materials.PipelineCache Draconic::RHI.Vulkan Draconic::RHI.WebGPU doctest::doctest)

target_compile_definitions(Draconic.Render.Backend.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    DRACONIC_ENGINE_SHADER_DIR="${CMAKE_SOURCE_DIR}/Data/Shaders")

target_compile_options(Draconic.Render.Backend.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Render.Backend.Tests COMMAND Draconic.Render.Backend.Tests)
