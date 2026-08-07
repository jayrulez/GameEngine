/// Vulkan implementation of Texture.
/// Ported from Sedulous.RHI.Vulkan/VulkanTexture.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

#include "VkIncludes.h"

export module draconic.rhi.vulkan:texture;

import draconic.foundation;
import draconic.rhi;
import :adapter;
import :conversions;

using namespace draconic::foundation;

namespace draconic::rhi::vk
{
    // Live VkDeviceMemory objects backing textures. One vkAllocateMemory per texture (no
    // sub-allocation), so this both trends toward the driver's maxMemoryAllocationCount and sums the
    // texture VRAM footprint - handy context in the allocation-failure warning below. Same TU as
    // init()/cleanup(), so a plain static suffices.
    static i32 g_liveTexAllocs = 0;
}

export namespace draconic::rhi::vk
{

    class VkTextureImpl : public Texture
    {
    public:
        /// Initialize from a TextureDesc (creates VkImage + allocates memory).
        Status init(VkDevice device, VkAdapterImpl* adapter, const TextureDesc& d)
        {
            desc = d;

            VkImageCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.imageType = toVkImageType(d.dimension);
            ci.format = toVkFormat(d.format);
            ci.extent = {d.width, d.height, d.depth};
            ci.mipLevels = d.mipLevelCount;
            ci.arrayLayers = d.arrayLayerCount;
            ci.samples = toVkSampleCount(d.sampleCount);
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = toVkImageUsage(d.usage);
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            if (d.arrayLayerCount >= 6 && d.dimension == TextureDimension::Texture2D)
                ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

            if (vkCreateImage(device, &ci, nullptr, &m_image) != VK_SUCCESS)
                return ErrorCode::Unknown;

            VkMemoryRequirements memReqs{};
            vkGetImageMemoryRequirements(device, m_image, &memReqs);

            i32 memType = adapter->findMemoryType(static_cast<u32>(memReqs.memoryTypeBits),
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (memType < 0)
            {
                vkDestroyImage(device, m_image, nullptr);
                m_image = VK_NULL_HANDLE;
                return ErrorCode::Unknown;
            }

            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = memReqs.size;
            ai.memoryTypeIndex = static_cast<u32>(memType);

            const VkResult memRes = vkAllocateMemory(device, &ai, nullptr, &m_memory);
            if (memRes != VK_SUCCESS)
            {
                // GPU texture allocation failed. Log it (throttled so a failure storm can't flood): the
                // VkResult distinguishes capacity (VK_ERROR_OUT_OF_DEVICE_MEMORY = -2) from the allocation
                // -count ceiling (VK_ERROR_TOO_MANY_OBJECTS = -10), and liveTexAllocs shows how many texture
                // allocations were live when it tipped over. The first failure always logs.
                static u32 s_failN = 0;
                if ((s_failN++ % 90u) == 0u)
                {
                    DRACONIC_LOG_WARNING(
                        u8"VkTexture",
                        u8"vkAllocateMemory FAILED VkResult={} size={}B liveTexAllocs={} (fail#{})",
                        static_cast<i32>(memRes), static_cast<u64>(memReqs.size), g_liveTexAllocs,
                        s_failN);
                }
                vkDestroyImage(device, m_image, nullptr);
                m_image = VK_NULL_HANDLE;
                return ErrorCode::Unknown;
            }
            ++g_liveTexAllocs;

            vkBindImageMemory(device, m_image, m_memory, 0);
            return ErrorCode::Ok;
        }

        /// Initialize from an existing VkImage (e.g. swap chain). Does not own the image.
        void initFromExisting(VkImage image, const TextureDesc& d)
        {
            m_image = image;
            desc = d;
            m_ownsImage = false;
        }

        void cleanup(VkDevice device)
        {
            if (m_memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(device, m_memory, nullptr);
                m_memory = VK_NULL_HANDLE;
                --g_liveTexAllocs;
            }
            if (m_ownsImage && m_image != VK_NULL_HANDLE)
                vkDestroyImage(device, m_image, nullptr);
            m_image = VK_NULL_HANDLE;
            m_subresourceLayouts.Clear();
        }

        // ---- Internal ----
        [[nodiscard]] VkImage handle() const { return m_image; }
        [[nodiscard]] VkFormat vkFormat() const { return toVkFormat(desc.format); }

        /// Whole-resource layout (uniform fast path).
        VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        /// Get layout for a specific subresource.
        VkImageLayout getSubresourceLayout(u32 mip, u32 layer) const
        {
            if (m_subresourceLayouts.IsEmpty())
                return currentLayout;
            u32 idx = mip + layer * desc.mipLevelCount;
            if (idx >= static_cast<u32>(m_subresourceLayouts.Size()))
                return currentLayout;
            return m_subresourceLayouts[idx];
        }

        /// Update layout for a subresource range. Promotes to per-subresource
        /// tracking when needed, collapses back to uniform when all match.
        void setSubresourceLayout(u32 baseMip, u32 mipCount, u32 baseLayer, u32 layerCount,
                                  VkImageLayout layout)
        {
            u32 totalMips = desc.mipLevelCount;
            u32 totalLayers = Max(desc.arrayLayerCount, 1u);
            u32 mipEnd = (mipCount == ~0u) ? totalMips : Min(baseMip + mipCount, totalMips);
            u32 layerEnd =
                (layerCount == ~0u) ? totalLayers : Min(baseLayer + layerCount, totalLayers);

            // All subresources? Collapse to uniform.
            if (baseMip == 0 && mipEnd >= totalMips && baseLayer == 0 && layerEnd >= totalLayers)
            {
                currentLayout = layout;
                m_subresourceLayouts.Clear();
                return;
            }

            // Promote to per-subresource.
            if (m_subresourceLayouts.IsEmpty())
            {
                if (layout == currentLayout)
                    return;
                m_subresourceLayouts.Resize(totalMips * totalLayers, currentLayout);
            }

            for (u32 l = baseLayer; l < layerEnd; ++l)
                for (u32 m = baseMip; m < mipEnd; ++m)
                    m_subresourceLayouts[m + l * totalMips] = layout;

            // Try to collapse back to uniform.
            VkImageLayout first = m_subresourceLayouts[0];
            for (usize i = 1; i < m_subresourceLayouts.Size(); ++i)
            {
                if (m_subresourceLayouts[i] != first)
                    return;
            }
            currentLayout = first;
            m_subresourceLayouts.Clear();
        }

    private:
        VkImage m_image = VK_NULL_HANDLE;
        VkDeviceMemory m_memory = VK_NULL_HANDLE;
        bool m_ownsImage = true;
        Array<VkImageLayout> m_subresourceLayouts;
    };

} // namespace draconic::rhi::vk
