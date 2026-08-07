# Draconic::SamplesFramework - `draconic.samples.framework` (RHI sample base + helpers).
# Needs the Vulkan backend, the desktop shell, and the shader compiler.

if(NOT (TARGET Draconic::RHI.Vulkan AND TARGET Draconic::Shell.Desktop AND TARGET Draconic::Runtime.Desktop AND TARGET Draconic::Shaders))
    message(STATUS "Skipping Draconic::SamplesFramework - needs Vulkan + Desktop shell + Shaders")
    return()
endif()

add_library(DraconicSamplesFramework STATIC)
add_library(Draconic::SamplesFramework ALIAS DraconicSamplesFramework)

target_link_libraries(DraconicSamplesFramework PUBLIC
    Draconic::Foundation
    Draconic::RHI
    Draconic::RHI.Vulkan
    Draconic::RHI.Validation
    Draconic::Shell
    Draconic::Shell.Desktop Draconic::Runtime.Desktop
    Draconic::Shaders
    Draconic::Policy)

# DX12 backend - Windows only.
if(TARGET Draconic::RHI.DX12)
    target_link_libraries(DraconicSamplesFramework PUBLIC Draconic::RHI.DX12)
    target_compile_definitions(DraconicSamplesFramework PUBLIC DRACONIC_HAS_DX12)
endif()

if(TARGET Draconic::RHI.WebGPU)
    target_link_libraries(DraconicSamplesFramework PUBLIC Draconic::RHI.WebGPU)
    target_compile_definitions(DraconicSamplesFramework PUBLIC DRACONIC_HAS_WEBGPU)
endif()

target_sources(DraconicSamplesFramework
    PUBLIC
        FILE_SET samples_framework_modules TYPE CXX_MODULES FILES
            DepthBuffer.cppm
            ShaderHelpers.cppm
            SampleApp.cppm
            Framework.cppm)
