# Foundation::RHI.Vulkan unit tests (doctest). The stage-mask rules are pure; the device
# cases spin up a real Vulkan device and skip when no adapter is present (CI runners).

add_executable(RHI.Vulkan.Tests TestsMain.cpp VulkanQueueTests.cpp VulkanQueryTests.cpp)
# Vulkan::Vulkan: the same vulkan.h the module was built against (GCC checks module ODR).
target_link_libraries(RHI.Vulkan.Tests PRIVATE Foundation::RHI.Vulkan Foundation::RHI Vulkan::Vulkan doctest::doctest)
target_compile_definitions(RHI.Vulkan.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(RHI.Vulkan.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME RHI.Vulkan.Tests COMMAND RHI.Vulkan.Tests)
