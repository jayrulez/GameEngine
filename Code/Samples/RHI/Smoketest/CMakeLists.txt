add_executable(Smoketest Main.cpp)
target_link_libraries(Smoketest PRIVATE
    Foundation::RHI.Null Foundation::RHI.Validation Foundation::Shaders
    Foundation::Shell.Desktop Foundation::Runtime.Desktop Foundation::Core Foundation::Policy)
target_compile_definitions(Smoketest PRIVATE OPTION_HAS_SHADERS)

# Vulkan backend: present on Linux; its section (see Main.cpp) compiles out without it.
if(TARGET Foundation::RHI.Vulkan)
    target_link_libraries(Smoketest PRIVATE Foundation::RHI.Vulkan)
    target_compile_definitions(Smoketest PRIVATE OPTION_HAS_VULKAN)
endif()

# DX12 backend: Windows only.
if(TARGET Foundation::RHI.DX12)
    target_link_libraries(Smoketest PRIVATE Foundation::RHI.DX12)
    target_compile_definitions(Smoketest PRIVATE OPTION_HAS_DX12)
endif()

# WebGPU backend: present when the wgpu headers are vendored; the sidecar itself
# is probed at run time (section prints "skipped" when unavailable).
if(TARGET Foundation::RHI.WebGPU)
    target_link_libraries(Smoketest PRIVATE Foundation::RHI.WebGPU)
    target_compile_definitions(Smoketest PRIVATE OPTION_HAS_WEBGPU)
endif()

target_compile_options(Smoketest PRIVATE
    "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error=unused-parameter;-Wno-error=unused-variable;-Wno-error=missing-field-initializers;-Wno-error=misleading-indentation>")

# Copy SDL3.dll next to the executable on Windows.
util_copy_runtime_deps(Smoketest) # SDL3 is vendored static; no DLL to stage
