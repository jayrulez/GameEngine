# Samples::Framework - `samples.framework` (RHI sample base + helpers).
# Needs at least one GPU backend, the desktop shell, and the shader compiler. Backend-agnostic: it
# links whichever backends are present (OPTION_HAS_*) and picks an available one by default, so it
# builds on Vulkan (Linux) or DX12 (Windows) alike.

if(NOT ((TARGET Foundation::RHI.Vulkan OR TARGET Foundation::RHI.DX12 OR TARGET Foundation::RHI.WebGPU)
        AND TARGET Foundation::Shell.Desktop AND TARGET Foundation::Runtime.Desktop AND TARGET Foundation::Shaders))
    message(STATUS "Skipping Samples::Framework - needs a GPU backend + Desktop shell + Shaders")
    return()
endif()

util_add_engine_library(SamplesFramework ALIAS Samples::Framework)
target_link_libraries(SamplesFramework PUBLIC
    Foundation::Core
    Foundation::VFS
    Foundation::RHI
    Foundation::RHI.Validation
    Foundation::RHI.Capture
    Foundation::Shell
    Foundation::Shell.Desktop Foundation::Runtime.Desktop
    Foundation::Shaders
    Foundation::Policy)

# Link each present backend and flag it; the framework selects among the compiled-in ones.
if(TARGET Foundation::RHI.Vulkan)
    target_link_libraries(SamplesFramework PUBLIC Foundation::RHI.Vulkan)
    target_compile_definitions(SamplesFramework PUBLIC OPTION_HAS_VULKAN)
endif()

if(TARGET Foundation::RHI.DX12)
    target_link_libraries(SamplesFramework PUBLIC Foundation::RHI.DX12)
    target_compile_definitions(SamplesFramework PUBLIC OPTION_HAS_DX12)
endif()

if(TARGET Foundation::RHI.WebGPU)
    target_link_libraries(SamplesFramework PUBLIC Foundation::RHI.WebGPU)
    target_compile_definitions(SamplesFramework PUBLIC OPTION_HAS_WEBGPU)
endif()

target_sources(SamplesFramework
    PUBLIC
        FILE_SET samples_framework_modules TYPE CXX_MODULES FILES
            DepthBuffer.cppm
            ShaderHelpers.cppm
            SampleApp.cppm
            Framework.cppm)
