# Draconic::RHI.DX12 - `draconic.rhi.dx12` (the DX12 RHI backend). Links the DX12
# system libraries; built only on Windows.

add_library(Draconic.RHI.DX12 STATIC)
add_library(Draconic::RHI.DX12 ALIAS Draconic.RHI.DX12)

target_link_libraries(Draconic.RHI.DX12
    PUBLIC Draconic::RHI Draconic::Foundation Draconic::Policy
    PRIVATE d3d12 dxgi dxguid d3dcompiler)

# DX12 headers use __uuidof (MSVC extension) via IID_PPV_ARGS. Clang supports
# it but warns under -Wlanguage-extension-token; suppress for this target.
target_compile_options(Draconic.RHI.DX12 PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-Wno-language-extension-token>)

target_sources(Draconic.RHI.DX12
    PUBLIC
        FILE_SET rhi_dx12_modules TYPE CXX_MODULES FILES
            DxConversions.cppm
            DxSurface.cppm
            DxAdapter.cppm
            DxBackend.cppm
            DxBuffer.cppm
            DxTexture.cppm
            DxTextureView.cppm
            DxSampler.cppm
            DxShaderModule.cppm
            DxFence.cppm
            DxQuerySet.cppm
            DxDescriptorHeap.cppm
            DxGpuDescriptorHeap.cppm
            DxDescriptorStaging.cppm
            DxBindGroupLayout.cppm
            DxPipelineLayout.cppm
            DxPipelineCache.cppm
            DxRenderPipeline.cppm
            DxComputePipeline.cppm
            DxBindGroup.cppm
            DxSwapChain.cppm
            DxQueue.cppm
            DxCommandBuffer.cppm
            DxCommandPool.cppm
            DxRenderPassEncoder.cppm
            DxRenderBundleEncoder.cppm
            DxComputePassEncoder.cppm
            DxMeshPipeline.cppm
            DxAccelStruct.cppm
            DxRayTracingPipeline.cppm
            DxTransferBatch.cppm
            DxCommandEncoder.cppm
            DxDevice.cppm
            DxModule.cppm)
