// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu:swapchain - SwapChain over the configured WGPUSurface.
///
/// WebGPU has no swapchain object: the surface is CONFIGURED (format/size/present
/// mode) and each frame borrows the current texture. AcquireNextImage wraps the
/// borrowed WGPUTexture (WrapExternal - the surface owns it) + creates its view;
/// Present hands it to the compositor and drops the borrow. There is no image
/// index in the API - a frame counter modulo bufferCount satisfies the RHI shape.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

export module foundation.rhi.webgpu:swapchain;

import foundation.core;
import foundation.rhi;
import :api;
import :conversions;
import :queue;
import :surface;
import :texture;
import :texture_view;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    /// The same 8-bit colour format in the other channel order; identity where there is no pair.
    [[nodiscard]] inline TextureFormat SwappedChannelOrder(TextureFormat format) noexcept
    {
        switch (format)
        {
        case TextureFormat::RGBA8Unorm:
            return TextureFormat::BGRA8Unorm;
        case TextureFormat::BGRA8Unorm:
            return TextureFormat::RGBA8Unorm;
        case TextureFormat::RGBA8UnormSrgb:
            return TextureFormat::BGRA8UnormSrgb;
        case TextureFormat::BGRA8UnormSrgb:
            return TextureFormat::RGBA8UnormSrgb;
        default:
            return format;
        }
    }

    /// The RHI colour format of a surface-capability format the swap chain can adopt
    /// (Undefined for anything else).
    [[nodiscard]] inline TextureFormat FromWgpuSurfaceColorFormat(WGPUTextureFormat format) noexcept
    {
        switch (format)
        {
        case WGPUTextureFormat_RGBA8Unorm:
            return TextureFormat::RGBA8Unorm;
        case WGPUTextureFormat_RGBA8UnormSrgb:
            return TextureFormat::RGBA8UnormSrgb;
        case WGPUTextureFormat_BGRA8Unorm:
            return TextureFormat::BGRA8Unorm;
        case WGPUTextureFormat_BGRA8UnormSrgb:
            return TextureFormat::BGRA8UnormSrgb;
        default:
            return TextureFormat::Undefined;
        }
    }

    /// Log spelling for the surface formats a swap chain negotiates between.
    [[nodiscard]] inline const char* SurfaceFormatName(TextureFormat format) noexcept
    {
        switch (format)
        {
        case TextureFormat::RGBA8Unorm:
            return "RGBA8Unorm";
        case TextureFormat::RGBA8UnormSrgb:
            return "RGBA8UnormSrgb";
        case TextureFormat::BGRA8Unorm:
            return "BGRA8Unorm";
        case TextureFormat::BGRA8UnormSrgb:
            return "BGRA8UnormSrgb";
        default:
            return "(other)";
        }
    }

    /// The requested format when the surface offers it, else the closest thing it does: the
    /// requested format's sibling in the other channel order (sRGB-ness is what the renderer's
    /// output depends on; channel order is the pipeline's business and it reads Format() back),
    /// else the surface's first ADOPTABLE choice, else the request unchanged. `offered` is the
    /// surface's capability list in RHI terms, Undefined for entries the swap chain cannot adopt
    /// (see FromWgpuSurfaceColorFormat) - pure, so a test can hand it any list.
    [[nodiscard]] inline TextureFormat NegotiateSurfaceFormat(TextureFormat requested,
                                                              Span<const TextureFormat> offered) noexcept
    {
        for (usize i = 0; i < offered.Size(); ++i)
        {
            if (offered[i] == requested)
            {
                return requested;
            }
        }
        const TextureFormat sibling = SwappedChannelOrder(requested);
        if (sibling != requested)
        {
            for (usize i = 0; i < offered.Size(); ++i)
            {
                if (offered[i] == sibling)
                {
                    return sibling;
                }
            }
        }
        for (usize i = 0; i < offered.Size(); ++i)
        {
            if (offered[i] != TextureFormat::Undefined)
            {
                return offered[i];
            }
        }
        return requested;
    }

    class WebGpuSwapChain final : public SwapChain
    {
    public:
        Status Initialize(const WebGpuApi& api, WGPUAdapter adapter, WGPUDevice device,
                          WebGpuQueue* queue, WebGpuSurface* surface,
                          const SwapChainDesc& swapDesc)
        {
            m_api = &api;
            m_adapter = adapter;
            m_device = device;
            m_queue = queue;
            m_surface = surface;
            m_format = swapDesc.format;
            m_requestedAlternate = swapDesc.alternateViewFormat;
            m_presentMode = swapDesc.presentMode;
            m_bufferCount = swapDesc.bufferCount;
            return Configure(swapDesc.width, swapDesc.height);
        }

        TextureFormat Format() const override { return m_format; }
        u32 Width() const override { return m_width; }
        u32 Height() const override { return m_height; }
        u32 BufferCount() const override { return m_bufferCount; }
        u32 CurrentImageIndex() const override { return m_frameIndex % m_bufferCount; }

        Status AcquireNextImage() override
        {
            DropCurrent();

            WGPUSurfaceTexture surfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
            m_api->wgpuSurfaceGetCurrentTexture(m_surface->Handle(), &surfaceTexture);
            if (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
                surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
            {
                if (surfaceTexture.texture != nullptr)
                {
                    m_api->wgpuTextureRelease(surfaceTexture.texture);
                }
                return ErrorCode::Unknown; // lost/outdated: host resizes + retries
            }

            // Size the frame from the TEXTURE WE GOT, not the size we configured: on the
            // browser the canvas can resize between configure and acquire, and Chrome hands
            // back the canvas's CURRENT backing texture - rendering with the configured size
            // then fails validation ("Scissor rect ... not contained in the render area")
            // and the whole frame drops. Width()/Height() feed FrameContext AFTER acquire,
            // so every downstream viewport/scissor agrees with the real attachment.
            const u32 acquiredWidth = m_api->wgpuTextureGetWidth(surfaceTexture.texture);
            const u32 acquiredHeight = m_api->wgpuTextureGetHeight(surfaceTexture.texture);
            if (acquiredWidth != 0 && acquiredHeight != 0 &&
                (acquiredWidth != m_width || acquiredHeight != m_height))
            {
                m_width = acquiredWidth;
                m_height = acquiredHeight;
            }
            TextureDesc textureDesc;
            textureDesc.format = m_format;
            textureDesc.width = m_width;
            textureDesc.height = m_height;
            textureDesc.usage = TextureUsage::RenderTarget;
            textureDesc.alternateViewFormat = AlternateViewFormat();
            m_currentTexture.WrapExternal(*m_api, surfaceTexture.texture, textureDesc);
            m_ownedHandle = surfaceTexture.texture; // released at the next AcquireNextImage / Cleanup

            TextureViewDesc viewDesc;
            viewDesc.format = m_format;
            const Status viewStatus =
                m_currentView.Initialize(*m_api, &m_currentTexture, viewDesc);
            if (!viewStatus.IsOk())
            {
                DropCurrent();
                return viewStatus;
            }
            if (AlternateViewFormat() != TextureFormat::Undefined)
            {
                TextureViewDesc alternateDesc;
                alternateDesc.format = AlternateViewFormat();
                if (!m_currentAlternateView.Initialize(*m_api, &m_currentTexture, alternateDesc).IsOk())
                {
                    m_haveImage = true; // so DropCurrent releases the view made above, and the image
                    DropCurrent();
                    return ErrorCode::Unknown;
                }
            }
            m_haveImage = true;
            ++m_frameIndex;
#if PLATFORM_WEB
            m_api->NoteFrameOpen(); // mid-frame-yield diagnostic (see WebGpuApi)
#endif
            return ErrorCode::Ok;
        }

        Texture* CurrentTexture() override { return m_haveImage ? &m_currentTexture : nullptr; }
        TextureView* CurrentTextureView() override
        {
            return m_haveImage ? &m_currentView : nullptr;
        }
        // On the web the canvas is configured in its UNORM base format with the sRGB format as a
        // view (Configure), so a UNORM view of it is always allowed: an asked-for twin is
        // honoured. Desktop wgpu-native would need surface view formats, which webgpu.h gives no
        // way to ask about ahead of configuring, so it says Undefined and the host draws through
        // CurrentTextureView alone.
        TextureFormat AlternateViewFormat() const override
        {
#if PLATFORM_WEB
            return m_requestedAlternate != TextureFormat::Undefined && m_requestedAlternate == SrgbTwin(m_format)
                       ? m_requestedAlternate
                       : TextureFormat::Undefined;
#else
            return TextureFormat::Undefined;
#endif
        }
        TextureView* CurrentAlternateTextureView() override
        {
            return m_haveImage && AlternateViewFormat() != TextureFormat::Undefined ? &m_currentAlternateView
                                                                                     : nullptr;
        }

        Status Present(Queue*) override
        {
            if (!m_haveImage)
            {
                return ErrorCode::InvalidArgument;
            }
#if PLATFORM_WEB
            // The browser presents the canvas automatically once the requestAnimationFrame
            // callback (the web runner's frame) returns; emdawnwebgpu ABORTS on an explicit
            // wgpuSurfacePresent.
            //
            // Do NOT release the borrowed surface texture here (or on any fixed frame boundary).
            // On web wgpuQueueSubmit validates and executes ASYNCHRONOUSLY (the browser drains the
            // queue after the rAF returns), so releasing our only reference before this frame's
            // submit has been consumed destroys the texture out from under it: "Destroyed texture
            // used in a submit", and Dawn drops the whole command buffer (the startup race that
            // silently killed the one-shot IBL env bake, and every resize's reconfigure). Hand the
            // borrow to the queue, which releases it from a wgpuQueueOnSubmittedWorkDone callback -
            // provably after the submit has been consumed, whatever the drain latency.
            if (m_queue != nullptr && m_ownedHandle != nullptr)
            {
                m_queue->ReleaseTextureWhenConsumed(m_ownedHandle);
                m_ownedHandle = nullptr; // the callback owns it now
            }
            m_api->NoteFrameClosed();
            return ErrorCode::Ok;
#else
            const WGPUStatus status = m_api->wgpuSurfacePresent(m_surface->Handle());
            DropCurrent();
            return status == WGPUStatus_Success ? Status(ErrorCode::Ok)
                                                : Status(ErrorCode::Unknown);
#endif
        }

        Status Resize(u32 width, u32 height) override
        {
            DropCurrent();
            return Configure(width, height);
        }

        void Cleanup()
        {
            DropCurrent();
            if (m_configured)
            {
                m_api->wgpuSurfaceUnconfigure(m_surface->Handle());
                m_configured = false;
            }
        }

    private:
        // The non-sRGB companion of an sRGB color format (identity otherwise). A WebGPU canvas
        // context only accepts a non-sRGB config format, so an sRGB swapchain is configured with
        // this base format plus the sRGB format as a viewFormat.
        static TextureFormat BaseColorFormat(TextureFormat format)
        {
            switch (format)
            {
            case TextureFormat::BGRA8UnormSrgb:
                return TextureFormat::BGRA8Unorm;
            case TextureFormat::RGBA8UnormSrgb:
                return TextureFormat::RGBA8Unorm;
            default:
                return format;
            }
        }

        // The sRGB companion of a non-sRGB 8-bit color format (identity otherwise) - the inverse of
        // BaseColorFormat, for adopting the browser's preferred canvas format as an sRGB backbuffer.
        static TextureFormat SrgbColorFormat(TextureFormat format)
        {
            switch (format)
            {
            case TextureFormat::BGRA8Unorm:
                return TextureFormat::BGRA8UnormSrgb;
            case TextureFormat::RGBA8Unorm:
                return TextureFormat::RGBA8UnormSrgb;
            default:
                return format;
            }
        }

#if PLATFORM_WEB
        // The browser's preferred canvas base format (formats[0] of the surface caps). Configuring
        // the canvas with anything else forces an extra copy at present. Falls back to the engine
        // default if caps are unavailable / not an 8-bit unorm format we understand.
        TextureFormat PreferredBaseFormat(TextureFormat fallback)
        {
            WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
            if (m_api->wgpuSurfaceGetCapabilities(m_surface->Handle(), m_adapter, &caps) !=
                    WGPUStatus_Success)
            {
                return fallback;
            }
            if (caps.formatCount == 0)
            {
                // A successful query still allocated the caps members - free before bailing.
                m_api->wgpuSurfaceCapabilitiesFreeMembers(caps);
                return fallback;
            }
            const WGPUTextureFormat preferred = caps.formats[0];
            m_api->wgpuSurfaceCapabilitiesFreeMembers(caps);
            if (preferred == WGPUTextureFormat_RGBA8Unorm)
            {
                return TextureFormat::RGBA8Unorm;
            }
            if (preferred == WGPUTextureFormat_BGRA8Unorm)
            {
                return TextureFormat::BGRA8Unorm;
            }
            return fallback;
        }
#endif

        Status Configure(u32 width, u32 height)
        {
            m_width = width;
            m_height = height;
            if (width == 0 || height == 0)
            {
                // A zero-size surface is illegal (WebGPU errors "size is zero"); a canvas hits this
                // transiently across a fullscreen/minimize transition. Skip configuring - the last
                // valid configuration stays, AcquireNextImage fails, the host skips the frame, and
                // the resize pump reconfigures once a real size arrives.
                return ErrorCode::Ok;
            }

            WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
            config.device = m_device;
            // The pipeline's final hop COPIES the tonemapped output into the
            // backbuffer, so the surface needs CopyDst alongside RenderAttachment
            // (universally supported by wgpu surfaces).
            config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
            // CopySrc when the surface offers it: what a screenshot (ScreenshotCapture) reads the
            // presented backbuffer through. Not guaranteed by the spec, so it is asked for, not assumed.
            if (SupportsUsage(WGPUTextureUsage_CopySrc))
            {
                config.usage |= WGPUTextureUsage_CopySrc;
            }
            config.width = width;
            config.height = height;
            config.presentMode = SupportedPresentMode(ToWgpuPresentMode(m_presentMode));

#if PLATFORM_WEB
            // A WebGPU canvas context does not accept an sRGB config format, so configure with the
            // base format and expose the sRGB format as a viewFormat. AcquireNextImage then creates
            // the per-frame view in m_format (the sRGB format), so the engine's default sRGB
            // swapchain renders correctly with no app-side change. Desktop wgpu-native accepts sRGB
            // config formats directly, so it is left exactly as before.
            //
            // ADOPT THE BROWSER'S PREFERRED base format (rgba8unorm vs bgra8unorm varies by device):
            // configuring the canvas with a non-preferred format forces the browser to copy the whole
            // frame at every present ("configured with a different format than preferred" warning).
            // Take the preferred base + retarget the engine to its sRGB companion so the renderer
            // still produces an sRGB backbuffer, just in the format the compositor wants.
            const TextureFormat baseFormat = PreferredBaseFormat(BaseColorFormat(m_format));
            m_format = SrgbColorFormat(baseFormat);
            config.format = ToWgpuTextureFormat(baseFormat);
            WGPUTextureFormat viewFormat = ToWgpuTextureFormat(m_format);
            if (baseFormat != m_format)
            {
                config.viewFormatCount = 1;
                config.viewFormats = &viewFormat;
            }
#else
            // Refuse CLEANLY when this adapter cannot present to the surface - wgpu-native
            // PANICS inside configure otherwise ("Surface does not support the adapter's
            // queue family", seen on Windows hybrid/multi-adapter machines). Zero supported
            // formats = no present support for this (surface, adapter) pair; the backend logs
            // the adapter list at startup and ENV_WEBGPU_ADAPTER=<index> overrides the pick.
            //
            // And NEGOTIATE the format from what the surface offers, the way the Vulkan swap
            // chain does: wgpu-native also panics inside configure when the format is not one
            // the surface lists. A Windows surface offers both channel orders and a Wayland one
            // RGBA, so the engine's RGBA8UnormSrgb default passed straight through; an X11
            // surface through wgpu's Vulkan backend offers only the BGRA pair and killed every
            // sample at startup (found by the Beef port). Callers read Format() back for their
            // colour targets, so the negotiated format follows through.
            {
                WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
                if (m_api->wgpuSurfaceGetCapabilities(m_surface->Handle(), m_adapter, &caps) ==
                    WGPUStatus_Success)
                {
                    const bool presentable = caps.formatCount > 0;
                    if (presentable)
                    {
                        Array<TextureFormat> offered;
                        offered.Reserve(static_cast<usize>(caps.formatCount));
                        for (usize i = 0; i < caps.formatCount; ++i)
                        {
                            offered.PushBack(FromWgpuSurfaceColorFormat(caps.formats[i]));
                        }
                        const TextureFormat negotiated = NegotiateSurfaceFormat(
                            m_format, Span<const TextureFormat>{offered.Data(), offered.Size()});
                        LogInfof("[RHI] swapchain surface format: requested %s, negotiated %s",
                                 SurfaceFormatName(m_format), SurfaceFormatName(negotiated));
                        if (!IsSrgb(negotiated))
                        {
                            LogWarningf("[RHI] swapchain negotiated a NON-sRGB format (%s) - the "
                                        "surface offered no sRGB target; output may look washed "
                                        "out where the final pass assumes encode-on-write",
                                        SurfaceFormatName(negotiated));
                        }
                        m_format = negotiated;
                    }
                    m_api->wgpuSurfaceCapabilitiesFreeMembers(caps);
                    if (!presentable)
                    {
                        LogError("[webgpu] this adapter cannot present to the window surface - "
                                 "set ENV_WEBGPU_ADAPTER=<index> (adapter list logged at "
                                 "startup)");
                        return ErrorCode::NotSupported;
                    }
                }
            }
            config.format = ToWgpuTextureFormat(m_format);
#endif
            m_api->wgpuSurfaceConfigure(m_surface->Handle(), &config);
            m_configured = true;
            m_width = width;
            m_height = height;
            return ErrorCode::Ok;
        }

        /// The requested mode when the surface offers it, else the closest match
        /// (Immediate/Mailbox degrade toward each other, everything else to Fifo -
        /// the only mode WebGPU guarantees).
        /// Whether the surface's capabilities include `usage` (false when the query fails).
        bool SupportsUsage(WGPUTextureUsage usage)
        {
            WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
            if (m_api->wgpuSurfaceGetCapabilities(m_surface->Handle(), m_adapter,
                                                  &capabilities) != WGPUStatus_Success)
            {
                return false;
            }
            const bool supported = (capabilities.usages & usage) != 0;
            m_api->wgpuSurfaceCapabilitiesFreeMembers(capabilities);
            return supported;
        }

        WGPUPresentMode SupportedPresentMode(WGPUPresentMode requested)
        {
            WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
            if (m_api->wgpuSurfaceGetCapabilities(m_surface->Handle(), m_adapter,
                                                  &capabilities) != WGPUStatus_Success)
            {
                return WGPUPresentMode_Fifo;
            }
            const auto supported = [&](WGPUPresentMode mode)
            {
                for (usize i = 0; i < capabilities.presentModeCount; ++i)
                {
                    if (capabilities.presentModes[i] == mode)
                    {
                        return true;
                    }
                }
                return false;
            };
            WGPUPresentMode chosen = WGPUPresentMode_Fifo;
            if (supported(requested))
            {
                chosen = requested;
            }
            else if ((requested == WGPUPresentMode_Immediate ||
                      requested == WGPUPresentMode_Mailbox) &&
                     supported(WGPUPresentMode_Mailbox))
            {
                chosen = WGPUPresentMode_Mailbox;
            }
            m_api->wgpuSurfaceCapabilitiesFreeMembers(capabilities);
            return chosen;
        }

        // Releases whatever of the current image exists: the view when it was built, and the
        // borrowed surface texture whenever one is held. NOT gated on m_haveImage - that flag
        // is set only after the view succeeds, and gating on it stranded the surface texture
        // when the view failed to build (the Beef port's swap chain frees it inline; this is
        // the same fix from the other side).
        void DropCurrent()
        {
            if (m_haveImage)
            {
                m_currentView.Release();
                m_currentAlternateView.Release();
            }
            if (m_ownedHandle != nullptr)
            {
                m_api->wgpuTextureRelease(m_ownedHandle);
                m_ownedHandle = nullptr;
            }
            m_haveImage = false;
        }

        const WebGpuApi* m_api = nullptr;
        WGPUAdapter m_adapter = nullptr;
        WGPUDevice m_device = nullptr;
        WebGpuQueue* m_queue = nullptr; // deferred surface-texture release on web
        WebGpuSurface* m_surface = nullptr;
        TextureFormat m_format = TextureFormat::BGRA8UnormSrgb;
        TextureFormat m_requestedAlternate = TextureFormat::Undefined; // SwapChainDesc's
        PresentMode m_presentMode = PresentMode::Fifo;
        u32 m_width = 0;
        u32 m_height = 0;
        u32 m_bufferCount = 2;
        u32 m_frameIndex = 0;
        bool m_configured = false;
        bool m_haveImage = false;
        WebGpuTexture m_currentTexture;
        WebGpuTextureView m_currentView;
        WebGpuTextureView m_currentAlternateView; // in AlternateViewFormat, when it is honoured
        WGPUTexture m_ownedHandle = nullptr;
    };
}
