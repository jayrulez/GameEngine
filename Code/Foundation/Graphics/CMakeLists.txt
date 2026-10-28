# Foundation::Graphics - `foundation.graphics` (GraphicsDevice + RenderWindow
# + FrameContext: the shared RHI render host). Backend-agnostic - imports only the
# base RHI, so it builds everywhere (incl. headless, no Vulkan). Backend factories
# (Null / Vulkan) are sibling modules so heavy backend modules never enter this
# interface (GCC's module reader rejects that).
util_add_engine_library(Graphics ALIAS Foundation::Graphics)
target_link_libraries(Graphics
    PUBLIC
        Foundation::Core
        Foundation::Profiler
        Foundation::RHI
        Foundation::Shell
        Foundation::Policy)

target_sources(Graphics
    PUBLIC
        FILE_SET graphics_modules TYPE CXX_MODULES FILES
            Graphics.cppm)

# Foundation::Graphics.Null - `foundation.graphics.null` (headless device
# factory over the Null RHI backend). No GPU/Vulkan required.
util_add_engine_library(Graphics.Null ALIAS Foundation::Graphics.Null)
target_link_libraries(Graphics.Null
    PUBLIC
        Foundation::Core
        Foundation::RHI
        Foundation::RHI.Null
        Foundation::Graphics
        Foundation::Policy)

target_sources(Graphics.Null
    PUBLIC
        FILE_SET graphics_null_modules TYPE CXX_MODULES FILES
            GraphicsNull.cppm)

# Foundation::Graphics.Gpu - `foundation.graphics.gpu` (the GPU backend factory). Built when at least
# one GPU RHI backend is available - Vulkan/DX12 on desktop, WebGPU on web - and links + flags each
# present backend (OPTION_HAS_*). The backend imports live in the implementation unit, off every
# consumer's module closure.
if(TARGET Foundation::RHI.Vulkan OR TARGET Foundation::RHI.DX12 OR TARGET Foundation::RHI.WebGPU)
    util_add_engine_library(Graphics.Gpu ALIAS Foundation::Graphics.Gpu)
    target_link_libraries(Graphics.Gpu
        PUBLIC
            Foundation::Core
            Foundation::RHI
            Foundation::RHI.Validation
            Foundation::Graphics
            Foundation::Graphics.Null
            Foundation::Policy)

    if(TARGET Foundation::RHI.Vulkan)
        target_link_libraries(Graphics.Gpu PUBLIC Foundation::RHI.Vulkan)
        target_compile_definitions(Graphics.Gpu PUBLIC OPTION_HAS_VULKAN)
    endif()

    if(TARGET Foundation::RHI.DX12)
        target_link_libraries(Graphics.Gpu PUBLIC Foundation::RHI.DX12)
        target_compile_definitions(Graphics.Gpu PUBLIC OPTION_HAS_DX12)
    endif()

    if(TARGET Foundation::RHI.WebGPU)
        target_link_libraries(Graphics.Gpu PUBLIC Foundation::RHI.WebGPU)
        target_compile_definitions(Graphics.Gpu PUBLIC OPTION_HAS_WEBGPU)
    endif()

    target_sources(Graphics.Gpu
        PUBLIC
            FILE_SET graphics_gpu_modules TYPE CXX_MODULES FILES
                GraphicsGpu.cppm
        PRIVATE
            GraphicsGpu.cpp)   # implementation unit: backend imports live here, off consumers' closure
else()
    message(STATUS "No GPU RHI backend available - skipping Foundation::Graphics.Gpu")
endif()

