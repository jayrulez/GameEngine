// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Vulkan implementation of SwapChain.
/// Ported from Sedulous.RHI.Vulkan/VulkanSwapChain.bf.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

export module foundation.rhi.vulkan:swap_chain;

import foundation.core;
import foundation.rhi;
import :conversions;
import :adapter;
import :surface;
import :texture;
import :texture_view;

using namespace foundation::core;

export namespace foundation::rhi::vk
{

    // Minimal reverse format mapping for swap chain format negotiation.
    inline TextureFormat fromVkFormat(VkFormat f)
    {
        switch (f)
        {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return TextureFormat::RGBA8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return TextureFormat::RGBA8UnormSrgb;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return TextureFormat::BGRA8Unorm;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return TextureFormat::BGRA8UnormSrgb;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return TextureFormat::RGBA16Float;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return TextureFormat::RGB10A2Unorm;
        default:
            return TextureFormat::Undefined;
        }
    }

    // Readable names for the swapchain-format negotiation log (I8: silent non-sRGB fallbacks washed
    // out exported players - name the negotiated format so a bad machine is a one-line diagnosis).
    // const char* for the printf-style RHI logger (LogInfof/LogWarningf).
    inline const char* vkFormatName(VkFormat f)
    {
        switch (f)
        {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB:
            return "R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM:
            return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB:
            return "B8G8R8A8_SRGB";
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return "R16G16B16A16_SFLOAT";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return "A2B10G10R10_UNORM_PACK32";
        default:
            return "(other)";
        }
    }

    inline const char* vkColorSpaceName(VkColorSpaceKHR cs)
    {
        switch (cs)
        {
        case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
            return "SRGB_NONLINEAR";
        default:
            return "(other)";
        }
    }

    class VkDeviceImpl; // forward

    class VkSwapChainImpl : public SwapChain
    {
    public:
        Status init(VkDevice device, VkPhysicalDevice physDevice, VkSurfaceKHR surface,
                    const SwapChainDesc& desc, VkDeviceImpl* owner, IAllocator& allocator,
                    bool mutableFormat)
        {
            m_device = device;
            m_physDevice = physDevice;
            m_surface = surface;
            m_owner = owner;
            m_allocator = &allocator;
            m_presentMode = desc.presentMode;
            m_mutableFormat = mutableFormat;
            m_requestedAlternate = desc.alternateViewFormat;
            return CreateSwapChain(desc.width, desc.height, desc.format, desc.bufferCount,
                                   VK_NULL_HANDLE);
        }

        // ---- SwapChain interface ----
        TextureFormat Format() const override { return m_format; }
        u32 Width() const override { return m_width; }
        u32 Height() const override { return m_height; }
        u32 BufferCount() const override { return m_bufferCount; }
        u32 CurrentImageIndex() const override { return m_currentImageIndex; }

        Status AcquireNextImage() override;
        Texture* CurrentTexture() override
        {
            return m_currentImageIndex < m_textures.Size() ? m_textures[m_currentImageIndex]
                                                           : nullptr;
        }
        TextureView* CurrentTextureView() override
        {
            return m_currentImageIndex < m_views.Size() ? m_views[m_currentImageIndex] : nullptr;
        }
        TextureFormat AlternateViewFormat() const override { return m_alternate; }
        TextureView* CurrentAlternateTextureView() override
        {
            return m_currentImageIndex < m_alternateViews.Size() ? m_alternateViews[m_currentImageIndex]
                                                                 : nullptr;
        }
        Status Present(Queue* queue) override;
        Status Resize(u32 w, u32 h) override;

        void cleanup();

        // ---- Internal ----
        [[nodiscard]] VkSwapchainKHR handle() const { return m_swapchain; }

        // Semaphore accessors for queue submit integration.
        VkSemaphore currentAcquireSemaphore() const { return m_acquireSems[m_frameIndex]; }
        VkSemaphore currentPresentSemaphore() const { return m_presentSems[m_currentImageIndex]; }

    private:
        Status CreateSwapChain(u32 w, u32 h, TextureFormat reqFormat, u32 reqCount,
                               VkSwapchainKHR old);
        Status retrieveImages(VkFormat format);
        void createSyncObjects();
        void cleanupImages();
        void destroySyncObjects();

        VkSurfaceFormatKHR chooseSurfaceFormat(TextureFormat requested);
        VkPresentModeKHR choosePresentMode(PresentMode requested);

        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_physDevice = VK_NULL_HANDLE;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        VkDeviceImpl* m_owner = nullptr;
        IAllocator* m_allocator = nullptr;

        TextureFormat m_format = TextureFormat::Undefined;
        PresentMode m_presentMode = PresentMode::Fifo;
        u32 m_width = 0, m_height = 0, m_bufferCount = 0;
        u32 m_currentImageIndex = 0;
        u32 m_frameIndex = 0;

        Array<VkTextureImpl*> m_textures;
        Array<VkTextureViewImpl*> m_views;
        Array<VkTextureViewImpl*> m_alternateViews; // in m_alternate, when honoured
        bool m_mutableFormat = false;                // VK_KHR_swapchain_mutable_format enabled
        TextureFormat m_requestedAlternate = TextureFormat::Undefined;
        TextureFormat m_alternate = TextureFormat::Undefined;
        Array<VkSemaphore> m_acquireSems;
        Array<VkSemaphore> m_presentSems;
    };

    // ---- Implementation (inline in module) ----

    inline Status VkSwapChainImpl::CreateSwapChain(u32 w, u32 h, TextureFormat reqFormat,
                                                   u32 reqCount, VkSwapchainKHR old)
    {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physDevice, m_surface, &caps);

        if (caps.currentExtent.width != ~0u)
        {
            m_width = caps.currentExtent.width;
            m_height = caps.currentExtent.height;
        }
        else
        {
            m_width = Clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
            m_height = Clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (m_width == 0 || m_height == 0)
            return ErrorCode::Unknown;

        auto presentMode = choosePresentMode(m_presentMode);

        // Mailbox needs a 3rd image to race ahead of the display without stalling; 2 is fine for
        // FIFO/Immediate.
        u32 wantCount = reqCount;
        if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR && wantCount < 3)
        {
            wantCount = 3;
        }
        m_bufferCount = Max(wantCount, caps.minImageCount);
        if (caps.maxImageCount > 0)
            m_bufferCount = Min(m_bufferCount, caps.maxImageCount);

        auto surfFmt = chooseSurfaceFormat(reqFormat);
        m_format = fromVkFormat(surfFmt.format);
        if (m_format == TextureFormat::Undefined)
            m_format = reqFormat;

        // Surface-format negotiation is a known cause of washed-out exported players: if a machine's
        // surface lacks our sRGB target, chooseSurfaceFormat silently falls to a UNORM format while
        // the final pass still assumes encode-on-write. Log request vs result (answers the issue's
        // own "what is the swapchain format?") and WARN loudly on a non-sRGB negotiation so the next
        // report is a one-line diagnosis rather than a hunt.
        LogInfof("[RHI] swapchain surface format: requested %s, negotiated %s [%s]",
                 vkFormatName(toVkFormat(reqFormat)), vkFormatName(surfFmt.format),
                 vkColorSpaceName(surfFmt.colorSpace));
        if (!IsSrgb(m_format))
        {
            LogWarningf("[RHI] swapchain negotiated a NON-sRGB format (%s) - the surface offered no "
                        "sRGB target; output may look washed out where the final pass assumes "
                        "encode-on-write",
                        vkFormatName(surfFmt.format));
        }

        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        VkCompositeAlphaFlagBitsKHR compositeAlpha =
            (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

        VkSwapchainCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = m_surface;
        ci.minImageCount = m_bufferCount;
        ci.imageFormat = surfFmt.format;
        ci.imageColorSpace = surfFmt.colorSpace;
        ci.imageExtent = {m_width, m_height};
        ci.imageArrayLayers = 1;
        ci.imageUsage = usage;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = compositeAlpha;
        ci.presentMode = presentMode;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = old;

        // The images viewable in their sRGB or UNORM twin too, when asked and the device can.
        m_alternate = (m_mutableFormat && m_requestedAlternate != TextureFormat::Undefined &&
                       m_requestedAlternate == SrgbTwin(m_format))
                          ? m_requestedAlternate
                          : TextureFormat::Undefined;
        if (m_requestedAlternate != TextureFormat::Undefined)
        {
            LogInfof("[RHI] swapchain alternate view format %s: %s", vkFormatName(toVkFormat(m_requestedAlternate)),
                     m_alternate != TextureFormat::Undefined ? "honoured"
                     : m_mutableFormat ? "not the swapchain format's sRGB or UNORM twin"
                                       : "unavailable (no VK_KHR_swapchain_mutable_format)");
        }
        VkFormat viewFormats[2] = {surfFmt.format, toVkFormat(m_alternate)};
        VkImageFormatListCreateInfo formatList{};
        if (m_alternate != TextureFormat::Undefined)
        {
            formatList.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
            formatList.viewFormatCount = 2;
            formatList.pViewFormats = viewFormats;
            ci.pNext = &formatList;
            ci.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        }

        if (vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain) != VK_SUCCESS)
            return ErrorCode::Unknown;
        if (old != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(m_device, old, nullptr);

        if (retrieveImages(surfFmt.format) != ErrorCode::Ok)
            return ErrorCode::Unknown;
        createSyncObjects();
        m_frameIndex = 0;
        return ErrorCode::Ok;
    }

    inline VkSurfaceFormatKHR VkSwapChainImpl::chooseSurfaceFormat(TextureFormat requested)
    {
        u32 count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, m_surface, &count, nullptr);
        Array<VkSurfaceFormatKHR> fmts(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, m_surface, &count, fmts.Data());

        VkFormat desired = toVkFormat(requested);
        for (auto& f : fmts)
            if (f.format == desired)
                return f;
        for (auto& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return f;
        for (auto& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM)
                return f;
        return fmts[0];
    }

    inline VkPresentModeKHR VkSwapChainImpl::choosePresentMode(PresentMode requested)
    {
        u32 count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physDevice, m_surface, &count, nullptr);
        Array<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physDevice, m_surface, &count, modes.Data());
        auto has = [&](VkPresentModeKHR m)
        {
            for (auto x : modes)
                if (x == m)
                    return true;
            return false;
        };

        const VkPresentModeKHR desired = toVkPresentMode(requested);
        if (has(desired))
        {
            return desired;
        }

        // Requested mode unavailable: preserve the request's INTENT rather than dropping straight to
        // vsync. Immediate and Mailbox both mean "don't block on the refresh" - so fall back to whichever
        // uncapped mode the surface does expose (Wayland commonly offers Mailbox but NOT Immediate) before
        // settling for FIFO. FIFO is the only mode guaranteed present by the spec.
        VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
        if (requested == PresentMode::Immediate || requested == PresentMode::Mailbox)
        {
            if (has(VK_PRESENT_MODE_MAILBOX_KHR))
            {
                chosen = VK_PRESENT_MODE_MAILBOX_KHR;
            }
            else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            {
                chosen = VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
        }
        else if (requested == PresentMode::FifoRelaxed && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR))
        {
            chosen = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        }

        String msg;
        AppendFormat(msg, u8"[swapchain] requested present mode (vk {}) unavailable; using vk {}\n",
                     static_cast<u32>(desired), static_cast<u32>(chosen));
        ConsoleWrite(msg.AsView());
        return chosen;
    }

    inline Status VkSwapChainImpl::retrieveImages(VkFormat format)
    {
        u32 imgCount = 0;
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, nullptr);
        Array<VkImage> images(imgCount);
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, images.Data());
        m_bufferCount = imgCount;

        TextureFormat texFmt = fromVkFormat(format);
        if (texFmt == TextureFormat::Undefined)
            texFmt = m_format;

        for (u32 i = 0; i < imgCount; ++i)
        {
            TextureDesc td{};
            td.dimension = TextureDimension::Texture2D;
            td.format = texFmt;
            td.width = m_width;
            td.height = m_height;
            td.arrayLayerCount = 1;
            td.mipLevelCount = 1;
            td.sampleCount = 1;
            td.usage = TextureUsage::RenderTarget;
            td.alternateViewFormat = m_alternate;

            auto* tex = m_allocator->New<VkTextureImpl>();
            tex->initFromExisting(images[i], td);
            m_textures.PushBack(tex);

            TextureViewDesc vd{};
            vd.format = texFmt;
            vd.dimension = TextureViewDimension::Texture2D;
            vd.mipLevelCount = 1;
            vd.arrayLayerCount = 1;
            auto* view = m_allocator->New<VkTextureViewImpl>();
            if (view->init(m_device, tex, vd) != ErrorCode::Ok)
            {
                m_allocator->Delete(view);
                return ErrorCode::Unknown;
            }
            m_views.PushBack(view);

            if (m_alternate != TextureFormat::Undefined)
            {
                TextureViewDesc ad = vd;
                ad.format = m_alternate;
                auto* alternate = m_allocator->New<VkTextureViewImpl>();
                if (alternate->init(m_device, tex, ad) != ErrorCode::Ok)
                {
                    m_allocator->Delete(alternate);
                    return ErrorCode::Unknown;
                }
                m_alternateViews.PushBack(alternate);
            }
        }
        return ErrorCode::Ok;
    }

    inline void VkSwapChainImpl::createSyncObjects()
    {
        VkSemaphoreCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (u32 i = 0; i < m_bufferCount; ++i)
        {
            VkSemaphore a = VK_NULL_HANDLE, p = VK_NULL_HANDLE;
            vkCreateSemaphore(m_device, &ci, nullptr, &a);
            vkCreateSemaphore(m_device, &ci, nullptr, &p);
            m_acquireSems.PushBack(a);
            m_presentSems.PushBack(p);
        }
    }

    inline void VkSwapChainImpl::cleanupImages()
    {
        for (auto* v : m_views)
        {
            v->cleanup(m_device);
            m_allocator->Delete(v);
        }
        m_views.Clear();
        for (auto* v : m_alternateViews)
        {
            v->cleanup(m_device);
            m_allocator->Delete(v);
        }
        m_alternateViews.Clear();
        for (auto* t : m_textures)
        {
            t->cleanup(m_device);
            m_allocator->Delete(t);
        }
        m_textures.Clear();
    }

    inline void VkSwapChainImpl::destroySyncObjects()
    {
        for (auto s : m_acquireSems)
        {
            vkDestroySemaphore(m_device, s, nullptr);
        }
        m_acquireSems.Clear();
        for (auto s : m_presentSems)
        {
            vkDestroySemaphore(m_device, s, nullptr);
        }
        m_presentSems.Clear();
    }

    inline Status VkSwapChainImpl::Resize(u32 w, u32 h)
    {
        vkDeviceWaitIdle(m_device);
        cleanupImages();
        destroySyncObjects();
        return CreateSwapChain(w, h, m_format, m_bufferCount, m_swapchain);
    }

    inline void VkSwapChainImpl::cleanup()
    {
        vkDeviceWaitIdle(m_device);
        cleanupImages();
        destroySyncObjects();
        if (m_swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

} // namespace foundation::rhi::vk
