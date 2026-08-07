/// Vulkan implementation of Sampler.
/// Ported from Sedulous.RHI.Vulkan/VulkanSampler.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "VkIncludes.h"

export module draconic.rhi.vulkan:sampler;

import draconic.foundation;
import draconic.rhi;
import :conversions;

using namespace draconic::foundation;

export namespace draconic::rhi::vk
{

    class VkSamplerImpl : public Sampler
    {
    public:
        Status init(VkDevice device, const SamplerDesc& d)
        {
            desc = d;

            VkSamplerCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            ci.magFilter = toVkFilter(d.magFilter);
            ci.minFilter = toVkFilter(d.minFilter);
            ci.mipmapMode = toVkMipmapMode(d.mipmapFilter);
            ci.addressModeU = toVkAddressMode(d.addressU);
            ci.addressModeV = toVkAddressMode(d.addressV);
            ci.addressModeW = toVkAddressMode(d.addressW);
            ci.mipLodBias = d.mipLodBias;
            ci.anisotropyEnable = d.maxAnisotropy > 1 ? VK_TRUE : VK_FALSE;
            ci.maxAnisotropy = static_cast<f32>(d.maxAnisotropy);
            ci.minLod = d.minLod;
            ci.maxLod = d.maxLod;
            ci.borderColor = toVkBorderColor(d.borderColor);
            ci.unnormalizedCoordinates = VK_FALSE;

            if (d.compare.HasValue())
            {
                ci.compareEnable = VK_TRUE;
                ci.compareOp = toVkCompareOp(d.compare.Value());
            }

            if (vkCreateSampler(device, &ci, nullptr, &m_sampler) != VK_SUCCESS)
                return ErrorCode::Unknown;
            return ErrorCode::Ok;
        }

        void cleanup(VkDevice device)
        {
            if (m_sampler != VK_NULL_HANDLE)
            {
                vkDestroySampler(device, m_sampler, nullptr);
                m_sampler = VK_NULL_HANDLE;
            }
        }

        [[nodiscard]] VkSampler handle() const { return m_sampler; }

    private:
        VkSampler m_sampler = VK_NULL_HANDLE;
    };

} // namespace draconic::rhi::vk
