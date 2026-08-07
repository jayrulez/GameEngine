/// Vulkan implementation of PipelineCache.
/// Ported from Sedulous.RHI.Vulkan/VulkanPipelineCache.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "VkIncludes.h"

export module draconic.rhi.vulkan:pipeline_cache;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::vk
{

    class VkPipelineCacheImpl : public PipelineCache
    {
    public:
        Status init(VkDevice device, const PipelineCacheDesc& desc)
        {
            m_device = device;

            VkPipelineCacheCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
            if (desc.initialData.Size() > 0)
            {
                ci.initialDataSize = desc.initialData.Size();
                ci.pInitialData = desc.initialData.Data();
            }

            if (vkCreatePipelineCache(device, &ci, nullptr, &m_cache) != VK_SUCCESS)
                return ErrorCode::Unknown;
            return ErrorCode::Ok;
        }

        void cleanup(VkDevice device)
        {
            if (m_cache != VK_NULL_HANDLE)
            {
                vkDestroyPipelineCache(device, m_cache, nullptr);
                m_cache = VK_NULL_HANDLE;
            }
        }

        u32 GetDataSize() override
        {
            usize size = 0;
            vkGetPipelineCacheData(m_device, m_cache, &size, nullptr);
            return static_cast<u32>(size);
        }

        Status GetData(Span<u8> outData) override
        {
            usize size = outData.Size();
            if (vkGetPipelineCacheData(m_device, m_cache, &size, outData.Data()) != VK_SUCCESS)
                return ErrorCode::Unknown;
            return ErrorCode::Ok;
        }

        [[nodiscard]] VkPipelineCache handle() const { return m_cache; }

    private:
        VkPipelineCache m_cache = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
    };

} // namespace draconic::rhi::vk
