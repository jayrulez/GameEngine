# Foundation::RHI.Capture tests: the flags, the row unpack, and the whole path on real GPUs.
add_executable(RHI.Capture.Tests TestsMain.cpp ScreenshotTests.cpp)
target_link_libraries(RHI.Capture.Tests PRIVATE Foundation::RHI.Capture Foundation::Image Foundation::Image.IO
    Foundation::RHI.TestSupport doctest::doctest)
# The round trip runs on whichever real backends this build has (never assumed).
if(TARGET Foundation::RHI.Vulkan)
    target_link_libraries(RHI.Capture.Tests PRIVATE Foundation::RHI.Vulkan)
    target_compile_definitions(RHI.Capture.Tests PRIVATE OPTION_HAS_VULKAN)
endif()
if(TARGET Foundation::RHI.WebGPU)
    target_link_libraries(RHI.Capture.Tests PRIVATE Foundation::RHI.WebGPU)
    target_compile_definitions(RHI.Capture.Tests PRIVATE OPTION_HAS_WEBGPU)
endif()
target_compile_definitions(RHI.Capture.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(RHI.Capture.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME RHI.Capture.Tests COMMAND RHI.Capture.Tests)
