# Draconic::Graphics - `draconic.graphics` (GraphicsDevice + RenderWindow
# + FrameContext: the shared RHI render host). Backend-agnostic - imports only the
# base RHI, so it builds everywhere (incl. headless, no Vulkan). Backend factories
# (Null / Vulkan) are sibling modules so heavy backend modules never enter this
# interface (GCC's module reader rejects that).
add_library(Draconic.Graphics STATIC)
add_library(Draconic::Graphics ALIAS Draconic.Graphics)

target_link_libraries(Draconic.Graphics
    PUBLIC
        Draconic::Foundation
        Draconic::RHI
        Draconic::Shell
        Draconic::Policy)

target_sources(Draconic.Graphics
    PUBLIC
        FILE_SET graphics_modules TYPE CXX_MODULES FILES
            Graphics.cppm)

# Draconic::Graphics.Null - `draconic.graphics.null` (headless device
# factory over the Null RHI backend). No GPU/Vulkan required.
add_library(Draconic.Graphics.Null STATIC)
add_library(Draconic::Graphics.Null ALIAS Draconic.Graphics.Null)

target_link_libraries(Draconic.Graphics.Null
    PUBLIC
        Draconic::Foundation
        Draconic::RHI
        Draconic::RHI.Null
        Draconic::Graphics
        Draconic::Policy)

target_sources(Draconic.Graphics.Null
    PUBLIC
        FILE_SET graphics_null_modules TYPE CXX_MODULES FILES
            GraphicsNull.cppm)

# Draconic::Graphics.Gpu - `draconic.graphics.gpu` (the GPU backend factory). Built when at least
# one GPU RHI backend is available - Vulkan/DX12 on desktop, WebGPU on web - and links + flags each
# present backend (DRACONIC_HAS_*). The backend imports live in the implementation unit, off every
# consumer's module closure.
if(TARGET Draconic::RHI.Vulkan OR TARGET Draconic::RHI.DX12 OR TARGET Draconic::RHI.WebGPU)
    add_library(Draconic.Graphics.Gpu STATIC)
    add_library(Draconic::Graphics.Gpu ALIAS Draconic.Graphics.Gpu)

    target_link_libraries(Draconic.Graphics.Gpu
        PUBLIC
            Draconic::Foundation
            Draconic::RHI
            Draconic::RHI.Validation
            Draconic::Graphics
            Draconic::Graphics.Null
            Draconic::Policy)

    if(TARGET Draconic::RHI.Vulkan)
        target_link_libraries(Draconic.Graphics.Gpu PUBLIC Draconic::RHI.Vulkan)
        target_compile_definitions(Draconic.Graphics.Gpu PUBLIC DRACONIC_HAS_VULKAN)
    endif()

    if(TARGET Draconic::RHI.DX12)
        target_link_libraries(Draconic.Graphics.Gpu PUBLIC Draconic::RHI.DX12)
        target_compile_definitions(Draconic.Graphics.Gpu PUBLIC DRACONIC_HAS_DX12)
    endif()

    if(TARGET Draconic::RHI.WebGPU)
        target_link_libraries(Draconic.Graphics.Gpu PUBLIC Draconic::RHI.WebGPU)
        target_compile_definitions(Draconic.Graphics.Gpu PUBLIC DRACONIC_HAS_WEBGPU)
    endif()

    target_sources(Draconic.Graphics.Gpu
        PUBLIC
            FILE_SET graphics_gpu_modules TYPE CXX_MODULES FILES
                GraphicsGpu.cppm
        PRIVATE
            GraphicsGpu.cpp)   # implementation unit: backend imports live here, off consumers' closure
else()
    message(STATUS "No GPU RHI backend available - skipping Draconic::Graphics.Gpu")
endif()

