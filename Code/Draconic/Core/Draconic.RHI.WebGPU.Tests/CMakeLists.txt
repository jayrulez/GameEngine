# Draconic::RHI.WebGPU unit tests (doctest). Environment-dependent: they skip
# (vacuously pass) when the wgpu-native sidecar or a GPU is unavailable.

add_executable(Draconic.RHI.WebGPU.Tests
    TestsMain.cpp
    WebGpuRhiTests.cpp)

target_link_libraries(Draconic.RHI.WebGPU.Tests PRIVATE Draconic::RHI.WebGPU doctest::doctest)

target_compile_definitions(Draconic.RHI.WebGPU.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.RHI.WebGPU.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.RHI.WebGPU.Tests COMMAND Draconic.RHI.WebGPU.Tests)
