# Draconic::RHI.Vulkan - `draconic.rhi.vulkan` (the Vulkan RHI backend). Links the Vulkan
# loader; built only when the Vulkan SDK/headers are available.

add_library(Draconic.RHI.Vulkan STATIC)
add_library(Draconic::RHI.Vulkan ALIAS Draconic.RHI.Vulkan)

target_link_libraries(Draconic.RHI.Vulkan
    PUBLIC Draconic::RHI Draconic::Foundation Draconic::Policy
    PRIVATE Vulkan::Vulkan)

target_sources(Draconic.RHI.Vulkan
    PUBLIC
        FILE_SET rhi_vk_modules TYPE CXX_MODULES FILES
            VkConversions.cppm
            VkSurface.cppm
            VkAdapter.cppm
            VkBackend.cppm
            VkBuffer.cppm
            VkTexture.cppm
            VkTextureView.cppm
            VkSampler.cppm
            VkShaderModule.cppm
            VkFence.cppm
            VkQuerySet.cppm
            VkBarrierHelper.cppm
            VkCommandBuffer.cppm
            VkDescriptorPoolManager.cppm
            VkBindingShifts.cppm
            VkBindGroupLayout.cppm
            VkPipelineLayout.cppm
            VkPipelineCache.cppm
            VkRenderPipeline.cppm
            VkComputePipeline.cppm
            VkBindGroup.cppm
            VkSwapChain.cppm
            VkQueue.cppm
            VkCommandPool.cppm
            VkRenderBundleEncoder.cppm
            VkRenderPassEncoder.cppm
            VkComputePassEncoder.cppm
            VkMeshPipeline.cppm
            VkAccelStruct.cppm
            VkRayTracingPipeline.cppm
            VkTransferBatch.cppm
            VkCommandEncoder.cppm
            VkDevice.cppm
            VkModule.cppm)
