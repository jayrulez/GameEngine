/// draconic.rhi.webgpu:swapchain - SwapChain over the configured WGPUSurface.
///
/// WebGPU has no swapchain object: the surface is CONFIGURED (format/size/present
/// mode) and each frame borrows the current texture. AcquireNextImage wraps the
/// borrowed WGPUTexture (WrapExternal - the surface owns it) + creates its view;
/// Present hands it to the compositor and drops the borrow. There is no image
/// index in the API - a frame counter modulo bufferCount satisfies the RHI shape.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:swapchain;

import draconic.foundation;
import draconic.rhi;
import :api;
import :conversions;
import :queue;
import :surface;
import :texture;
import :texture_view;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
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
            m_haveImage = true;
            ++m_frameIndex;
#if DRACONIC_PLATFORM_WEB
            m_api->NoteFrameOpen(); // mid-frame-yield diagnostic (see WebGpuApi)
#endif
            return ErrorCode::Ok;
        }

        Texture* CurrentTexture() override { return m_haveImage ? &m_currentTexture : nullptr; }
        TextureView* CurrentTextureView() override
        {
            return m_haveImage ? &m_currentView : nullptr;
        }

        Status Present(Queue*) override
        {
            if (!m_haveImage)
            {
                return ErrorCode::InvalidArgument;
            }
#if DRACONIC_PLATFORM_WEB
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

#if DRACONIC_PLATFORM_WEB
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
            config.width = width;
            config.height = height;
            config.presentMode = SupportedPresentMode(ToWgpuPresentMode(m_presentMode));

#if DRACONIC_PLATFORM_WEB
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
            config.format = ToWgpuTextureFormat(m_format);
#endif

#if !DRACONIC_PLATFORM_WEB
            // Refuse CLEANLY when this adapter cannot present to the surface - wgpu-native
            // PANICS inside configure otherwise ("Surface does not support the adapter's
            // queue family", seen on Windows hybrid/multi-adapter machines). Zero supported
            // formats = no present support for this (surface, adapter) pair; the backend logs
            // the adapter list at startup and DRACONIC_WEBGPU_ADAPTER=<index> overrides the pick.
            {
                WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
                if (m_api->wgpuSurfaceGetCapabilities(m_surface->Handle(), m_adapter, &caps) ==
                    WGPUStatus_Success)
                {
                    const bool presentable = caps.formatCount > 0;
                    m_api->wgpuSurfaceCapabilitiesFreeMembers(caps);
                    if (!presentable)
                    {
                        LogError("[webgpu] this adapter cannot present to the window surface - "
                                 "set DRACONIC_WEBGPU_ADAPTER=<index> (adapter list logged at "
                                 "startup)");
                        return ErrorCode::NotSupported;
                    }
                }
            }
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

        void DropCurrent()
        {
            if (!m_haveImage)
            {
                return;
            }
            m_currentView.Release();
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
        PresentMode m_presentMode = PresentMode::Fifo;
        u32 m_width = 0;
        u32 m_height = 0;
        u32 m_bufferCount = 2;
        u32 m_frameIndex = 0;
        bool m_configured = false;
        bool m_haveImage = false;
        WebGpuTexture m_currentTexture;
        WebGpuTextureView m_currentView;
        WGPUTexture m_ownedHandle = nullptr;
    };
}
