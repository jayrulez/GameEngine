// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Abstract swap chain for double- or triple-buffered presentation.

export module foundation.rhi:swapchain;

import foundation.core;
import :enums;
import :texture_format;
import :resources;
import :queue;

using namespace foundation::core;

export namespace foundation::rhi
{

    class SwapChain
    {
    public:
        virtual ~SwapChain() = default;

        /// Format of the swap chain back buffers.
        [[nodiscard]] virtual TextureFormat Format() const = 0;
        /// Width in pixels.
        [[nodiscard]] virtual u32 Width() const = 0;
        /// Height in pixels.
        [[nodiscard]] virtual u32 Height() const = 0;
        /// Number of back buffers.
        [[nodiscard]] virtual u32 BufferCount() const = 0;
        /// Index of the currently acquired back buffer.
        [[nodiscard]] virtual u32 CurrentImageIndex() const = 0;

        /// Acquire the next back buffer for rendering. Must be called
        /// before accessing CurrentTexture / CurrentTextureView.
        virtual Status AcquireNextImage() = 0;

        /// The texture of the currently acquired back buffer.
        [[nodiscard]] virtual Texture* CurrentTexture() = 0;
        /// A view of the currently acquired back buffer.
        [[nodiscard]] virtual TextureView* CurrentTextureView() = 0;

        /// The alternate view format the back buffers were made viewable in
        /// (SwapChainDesc::alternateViewFormat, the format's sRGB or UNORM twin), or Undefined
        /// when none was asked for or this device and surface cannot (Vulkan without
        /// VK_KHR_swapchain_mutable_format): a host then draws through CurrentTextureView alone.
        [[nodiscard]] virtual TextureFormat AlternateViewFormat() const = 0;
        /// A view of the currently acquired back buffer in AlternateViewFormat; null when it is
        /// Undefined.
        [[nodiscard]] virtual TextureView* CurrentAlternateTextureView() = 0;

        /// Present the current back buffer to the screen.
        virtual Status Present(Queue* queue) = 0;
        /// Resize the swap chain (e.g. after a window resize).
        virtual Status Resize(u32 width, u32 height) = 0;
    };

} // namespace foundation::rhi
