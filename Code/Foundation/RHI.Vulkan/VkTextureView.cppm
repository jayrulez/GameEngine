// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Vulkan implementation of TextureView.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

export module foundation.rhi.vulkan:texture_view;

import foundation.core;
import foundation.rhi;
import :conversions;
import :texture;

using namespace foundation::core;

export namespace foundation::rhi::vk
{

    class VkTextureViewImpl : public TextureView
    {
    public:
        Status init(VkDevice device, VkTextureImpl* tex, const TextureViewDesc& d)
        {
            desc = d;
            texture = tex;
            // Dimensions are the view's base mip extent, not the texture's mip-0 size: a view onto
            // mip N is half-sized per level. The render-pass renderArea and framebuffer derive from
            // these, so a mip-0 size here would overrun a mip>0 attachment (GPU fault).
            m_width = tex->desc.width >> d.baseMipLevel;
            if (m_width == 0)
            {
                m_width = 1;
            }
            m_height = tex->desc.height >> d.baseMipLevel;
            if (m_height == 0)
            {
                m_height = 1;
            }

            TextureFormat fmt =
                (d.format == TextureFormat::Undefined) ? tex->desc.format : d.format;
            m_format = fmt;

            VkImageViewCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ci.image = tex->handle();
            ci.viewType = toVkImageViewType(d.dimension);
            ci.format = toVkFormat(fmt);
            ci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

            u32 mipCount =
                d.mipLevelCount > 0 ? d.mipLevelCount : tex->desc.mipLevelCount - d.baseMipLevel;
            u32 layerCount = d.arrayLayerCount > 0 ? d.arrayLayerCount
                                                   : tex->desc.arrayLayerCount - d.baseArrayLayer;

            VkImageAspectFlags aspect;
            switch (d.aspect)
            {
            case TextureAspect::DepthOnly:
                aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
                break;
            case TextureAspect::StencilOnly:
                aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
                break;
            default:
                aspect = getAspectMask(fmt);
                break;
            }

            ci.subresourceRange = {aspect, d.baseMipLevel, mipCount, d.baseArrayLayer, layerCount};

            if (vkCreateImageView(device, &ci, nullptr, &m_imageView) != VK_SUCCESS)
                return ErrorCode::Unknown;
            return ErrorCode::Ok;
        }

        void cleanup(VkDevice device)
        {
            if (m_imageView != VK_NULL_HANDLE)
            {
                vkDestroyImageView(device, m_imageView, nullptr);
                m_imageView = VK_NULL_HANDLE;
            }
        }

        [[nodiscard]] VkImageView handle() const { return m_imageView; }
        [[nodiscard]] TextureFormat Format() const { return m_format; }
        [[nodiscard]] u32 Width() const { return m_width; }
        [[nodiscard]] u32 Height() const { return m_height; }

    private:
        VkImageView m_imageView = VK_NULL_HANDLE;
        TextureFormat m_format = TextureFormat::Undefined;
        u32 m_width = 0;
        u32 m_height = 0;
    };

} // namespace foundation::rhi::vk
