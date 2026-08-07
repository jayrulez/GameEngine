/// draconic.rhi.webgpu:command_encoder - CommandEncoder over WGPUCommandEncoder.
///
/// WebGPU encoders are ONE-SHOT; the wrapper is reusable - after Finish, the next
/// Begin* / copy call lazily opens a fresh WGPUCommandEncoder, which is exactly the
/// RenderWindow frame loop's Reset-and-reencode shape. Barriers are no-ops (WebGPU
/// tracks hazards itself; the RHI's explicit transitions carry no information here).
///
/// Blit and GenerateMipmaps ride the internal fullscreen blit pass (:blit_helper) -
/// same-extent same-format blits stay plain copies. ResolveTexture is a resolve-only
/// render pass (load the MSAA attachment, discard it, resolve out).

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:command_encoder;

import draconic.foundation;
import draconic.rhi;
import :api;
import :blit_helper;
import :conversions;
import :buffer;
import :texture;
import :texture_view;
import :query_set;
import :command_buffer;
import :render_pass_encoder;
import :compute_pass_encoder;
import :render_bundle_encoder;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuCommandEncoder final : public CommandEncoder
    {
    public:
        void Initialize(const WebGpuApi& api, WGPUDevice device, IAllocator& allocator,
                        WebGpuBlitHelper& blitHelper)
        {
            m_api = &api;
            m_device = device;
            m_allocator = &allocator;
            m_blitHelper = &blitHelper;
        }

        RenderPassEncoder* BeginRenderPass(const RenderPassDesc& passDesc) override
        {
            EnsureOpen();

            WGPURenderPassColorAttachment colors[MaxColorAttachments];
            for (usize i = 0; i < passDesc.colorAttachments.Size(); ++i)
            {
                const ColorAttachment& attachment = passDesc.colorAttachments[i];
                WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
                color.view =
                    static_cast<WebGpuTextureView*>(attachment.view)->Handle();
                if (attachment.resolveTarget != nullptr)
                {
                    color.resolveTarget =
                        static_cast<WebGpuTextureView*>(attachment.resolveTarget)->Handle();
                }
                color.loadOp = ToWgpuLoadOp(attachment.loadOp);
                color.storeOp = ToWgpuStoreOp(attachment.storeOp);
                color.clearValue = WGPUColor{attachment.clearValue.r, attachment.clearValue.g,
                                             attachment.clearValue.b, attachment.clearValue.a};
                colors[i] = color;
            }

            WGPURenderPassDescriptor wgpuDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(passDesc.label);
            wgpuDesc.colorAttachmentCount = passDesc.colorAttachments.Size();
            wgpuDesc.colorAttachments = colors;

            WGPURenderPassDepthStencilAttachment depth =
                WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
            if (passDesc.depthStencilAttachment.HasValue())
            {
                const DepthStencilAttachment& attachment =
                    passDesc.depthStencilAttachment.Value();
                depth.view = static_cast<WebGpuTextureView*>(attachment.view)->Handle();
                // depthClearValue must be a FINITE value even on a read-only / Load-op plane. The
                // wgpu INIT sentinel is WGPU_DEPTH_CLEAR_VALUE_UNDEFINED (== NaN); native wgpu
                // ignores it when not clearing, but the browser's WebGPU validation rejects a
                // non-finite depthClearValue unconditionally. Always pass the attachment's value
                // (finite 1.0 by default) so read-only depth passes (e.g. the forward pass reading
                // the prepass depth) validate in a browser.
                depth.depthClearValue = attachment.depthClearValue;
                if (attachment.depthReadOnly)
                {
                    depth.depthReadOnly = 1u; // read-only planes must leave load/store ops undefined
                }
                else
                {
                    depth.depthLoadOp = ToWgpuLoadOp(attachment.depthLoadOp);
                    depth.depthStoreOp = ToWgpuStoreOp(attachment.depthStoreOp);
                }
                // A view created with a default desc INHERITS the texture's format (the
                // desc keeps Undefined) - resolve through the owner, or a stencil-capable
                // attachment is misjudged as depth-only and the browser rejects the pass
                // ("Both stencilLoadOp and stencilStoreOp must be set...").
                TextureFormat dsFormat = attachment.view->desc.format;
                if (dsFormat == TextureFormat::Undefined && attachment.view->texture != nullptr)
                {
                    dsFormat = attachment.view->texture->desc.format;
                }
                const bool hasStencil = HasStencil(dsFormat);
                if (hasStencil)
                {
                    if (attachment.stencilReadOnly)
                    {
                        depth.stencilReadOnly = 1u;
                    }
                    else
                    {
                        depth.stencilLoadOp = ToWgpuLoadOp(attachment.stencilLoadOp);
                        depth.stencilStoreOp = ToWgpuStoreOp(attachment.stencilStoreOp);
                        depth.stencilClearValue = attachment.stencilClearValue;
                    }
                }
                wgpuDesc.depthStencilAttachment = &depth;
            }

            if (passDesc.occlusionQuerySet != nullptr)
            {
                wgpuDesc.occlusionQuerySet =
                    static_cast<WebGpuQuerySet*>(passDesc.occlusionQuerySet)->Handle();
            }

            WGPUPassTimestampWrites timestamps = WGPU_PASS_TIMESTAMP_WRITES_INIT;
            if (passDesc.timestampQuerySet != nullptr)
            {
                timestamps.querySet =
                    static_cast<WebGpuQuerySet*>(passDesc.timestampQuerySet)->Handle();
                timestamps.beginningOfPassWriteIndex = passDesc.beginTimestampIndex;
                timestamps.endOfPassWriteIndex = passDesc.endTimestampIndex;
                wgpuDesc.timestampWrites = &timestamps;
            }

            const WGPURenderPassEncoder pass =
                m_api->wgpuCommandEncoderBeginRenderPass(m_encoder, &wgpuDesc);
            m_renderPass.Begin(*m_api, m_device, pass);
            return &m_renderPass;
        }

        ComputePassEncoder* BeginComputePass(StringView label) override
        {
            EnsureOpen();
            WGPUComputePassDescriptor wgpuDesc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(label);
            const WGPUComputePassEncoder pass =
                m_api->wgpuCommandEncoderBeginComputePass(m_encoder, &wgpuDesc);
            m_computePass.Begin(*m_api, m_device, pass);
            return &m_computePass;
        }

        RenderBundleEncoder* CreateRenderBundleEncoder(const RenderBundleDesc& bundleDesc) override
        {
            auto* encoder = m_allocator->New<WebGpuRenderBundleEncoder>();
            if (!encoder->Initialize(*m_api, m_device, *m_allocator, bundleDesc).IsOk())
            {
                m_allocator->Delete(encoder);
                return nullptr;
            }
            m_bundleEncoders.PushBack(encoder);
            return encoder;
        }

        void Barrier(const BarrierGroup&) override {} // WebGPU tracks hazards itself

        void CopyBufferToBuffer(Buffer* source, u64 sourceOffset, Buffer* destination,
                                u64 destinationOffset, u64 size) override
        {
            EnsureOpen();
            m_api->wgpuCommandEncoderCopyBufferToBuffer(
                m_encoder, static_cast<WebGpuBuffer*>(source)->Handle(), sourceOffset,
                static_cast<WebGpuBuffer*>(destination)->Handle(), destinationOffset, size);
        }

        void CopyBufferToTexture(Buffer* source, Texture* destination,
                                 const BufferTextureCopyRegion& region) override
        {
            EnsureOpen();
            WGPUTexelCopyBufferInfo src = MakeBufferInfo(source, region);
            WGPUTexelCopyTextureInfo dst = MakeTextureInfo(destination, region);
            const WGPUExtent3D extent{region.textureExtent.width, region.textureExtent.height,
                                      region.textureExtent.depth};
            m_api->wgpuCommandEncoderCopyBufferToTexture(m_encoder, &src, &dst, &extent);
        }

        void CopyTextureToBuffer(Texture* source, Buffer* destination,
                                 const BufferTextureCopyRegion& region) override
        {
            EnsureOpen();
            WGPUTexelCopyTextureInfo src = MakeTextureInfo(source, region);
            WGPUTexelCopyBufferInfo dst = MakeBufferInfo(destination, region);
            const WGPUExtent3D extent{region.textureExtent.width, region.textureExtent.height,
                                      region.textureExtent.depth};
            m_api->wgpuCommandEncoderCopyTextureToBuffer(m_encoder, &src, &dst, &extent);
        }

        void CopyTextureToTexture(Texture* source, Texture* destination,
                                  const TextureCopyRegion& region) override
        {
            EnsureOpen();
            WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            src.texture = static_cast<WebGpuTexture*>(source)->Handle();
            src.mipLevel = region.srcMipLevel;
            src.origin.z = region.srcArrayLayer;
            WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            dst.texture = static_cast<WebGpuTexture*>(destination)->Handle();
            dst.mipLevel = region.dstMipLevel;
            dst.origin.z = region.dstArrayLayer;
            const WGPUExtent3D extent{region.extent.width, region.extent.height,
                                      region.extent.depth};
            m_api->wgpuCommandEncoderCopyTextureToTexture(m_encoder, &src, &dst, &extent);
        }

        void Blit(Texture* source, Texture* destination) override
        {
            // Same-extent same-format = a plain copy; anything else goes through the
            // internal fullscreen blit pass (linear-sampled scale + convert).
            if (source->desc.width == destination->desc.width &&
                source->desc.height == destination->desc.height &&
                source->desc.format == destination->desc.format)
            {
                TextureCopyRegion region;
                region.extent = Extent3D{source->desc.width, source->desc.height, 1};
                CopyTextureToTexture(source, destination, region);
                return;
            }
            EnsureOpen();
            const WGPUTextureView sourceView =
                MipView(source, 0, 0, WGPUTextureAspect_All, true);
            const WGPUTextureView destinationView =
                MipView(destination, 0, 0, WGPUTextureAspect_All, false);
            m_blitHelper->Blit(m_encoder, sourceView, destinationView,
                               ToWgpuTextureFormat(destination->desc.format));
            m_api->wgpuTextureViewRelease(sourceView);
            m_api->wgpuTextureViewRelease(destinationView);
        }

        void GenerateMipmaps(Texture* texture) override
        {
            // The blit-chain: each mip renders from the one above, per array layer
            // (cubemaps are 6 layers). Texture creation widened the usage for
            // blit-capable mip chains; anything else is not generatable here.
            if (texture->desc.mipLevelCount < 2 ||
                texture->desc.dimension != TextureDimension::Texture2D ||
                !IsBlitCapableFormat(texture->desc.format))
            {
                return;
            }
            EnsureOpen();
            const WGPUTextureFormat format = ToWgpuTextureFormat(texture->desc.format);
            for (u32 layer = 0; layer < texture->desc.arrayLayerCount; ++layer)
            {
                for (u32 mip = 1; mip < texture->desc.mipLevelCount; ++mip)
                {
                    const WGPUTextureView sourceView =
                        MipView(texture, mip - 1, layer, WGPUTextureAspect_All, true);
                    const WGPUTextureView destinationView =
                        MipView(texture, mip, layer, WGPUTextureAspect_All, false);
                    m_blitHelper->Blit(m_encoder, sourceView, destinationView, format);
                    m_api->wgpuTextureViewRelease(sourceView);
                    m_api->wgpuTextureViewRelease(destinationView);
                }
            }
        }

        void ResolveTexture(Texture* source, Texture* destination) override
        {
            // WebGPU resolves via a pass resolveTarget - a standalone resolve is a
            // pass that loads the MSAA attachment, discards it, and resolves out.
            EnsureOpen();
            const WGPUTextureView sourceView =
                MipView(source, 0, 0, WGPUTextureAspect_All, false);
            const WGPUTextureView destinationView =
                MipView(destination, 0, 0, WGPUTextureAspect_All, false);
            WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.view = sourceView;
            color.resolveTarget = destinationView;
            color.loadOp = WGPULoadOp_Load;
            color.storeOp = WGPUStoreOp_Discard; // the resolve is the output
            WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            passDesc.colorAttachmentCount = 1;
            passDesc.colorAttachments = &color;
            const WGPURenderPassEncoder pass =
                m_api->wgpuCommandEncoderBeginRenderPass(m_encoder, &passDesc);
            m_api->wgpuRenderPassEncoderEnd(pass);
            m_api->wgpuRenderPassEncoderRelease(pass);
            m_api->wgpuTextureViewRelease(sourceView);
            m_api->wgpuTextureViewRelease(destinationView);
        }

        void ResetQuerySet(QuerySet*, u32, u32) override
        {
            // No WebGPU shape; queries are implicitly reset by resolve semantics.
        }

        void WriteTimestamp(QuerySet* querySet, u32 index) override
        {
            EnsureOpen();
#if DRACONIC_PLATFORM_WEB
            // Encoder-level timestamps are a wgpu-native extension; the browser (emdawnwebgpu) has
            // no timestamp-query feature and ABORTS on wgpuCommandEncoderWriteTimestamp. The GPU
            // profiler's timings are simply unavailable on web (a no-op, not a crash).
            (void)querySet;
            (void)index;
#else
            m_api->wgpuCommandEncoderWriteTimestamp(
                m_encoder, static_cast<WebGpuQuerySet*>(querySet)->Handle(), index);
#endif
        }

        void ResolveQuerySet(QuerySet* querySet, u32 firstQuery, u32 queryCount,
                             Buffer* destination, u64 destinationOffset) override
        {
            EnsureOpen();
            // WebGPU requires QUERY_RESOLVE usage on the resolve destination - which a
            // mappable readback buffer can never carry (MapRead combines only with
            // CopyDst). Resolve into an internal scratch buffer and copy over; the RHI
            // contract (Vulkan-shaped: resolve into any CopyDst buffer) is preserved.
            const u64 size = static_cast<u64>(queryCount) * sizeof(u64);
            EnsureQueryScratch(size);
            m_api->wgpuCommandEncoderResolveQuerySet(
                m_encoder, static_cast<WebGpuQuerySet*>(querySet)->Handle(), firstQuery,
                queryCount, m_queryScratch, 0);
            m_api->wgpuCommandEncoderCopyBufferToBuffer(
                m_encoder, m_queryScratch, 0,
                static_cast<WebGpuBuffer*>(destination)->Handle(), destinationOffset, size);
        }

        void BeginDebugLabel(StringView label, f32, f32, f32, f32) override
        {
            EnsureOpen();
            m_api->wgpuCommandEncoderPushDebugGroup(m_encoder, ToWgpuStringView(label));
        }

        void EndDebugLabel() override
        {
            EnsureOpen();
            m_api->wgpuCommandEncoderPopDebugGroup(m_encoder);
        }

        void InsertDebugLabel(StringView label, f32, f32, f32, f32) override
        {
            EnsureOpen();
            m_api->wgpuCommandEncoderInsertDebugMarker(m_encoder, ToWgpuStringView(label));
        }

        CommandBuffer* Finish() override
        {
            EnsureOpen();
            WGPUCommandBufferDescriptor wgpuDesc = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
            const WGPUCommandBuffer commandBuffer =
                m_api->wgpuCommandEncoderFinish(m_encoder, &wgpuDesc);
            m_api->wgpuCommandEncoderRelease(m_encoder);
            m_encoder = nullptr; // next use opens a fresh one
            m_commandBuffer.Adopt(*m_api, commandBuffer);
            return &m_commandBuffer;
        }

        ~WebGpuCommandEncoder() override
        {
            if (m_encoder != nullptr)
            {
                m_api->wgpuCommandEncoderRelease(m_encoder);
                m_encoder = nullptr;
            }
            m_commandBuffer.ReleaseHandle();
            if (m_queryScratch != nullptr)
            {
                m_api->wgpuBufferRelease(m_queryScratch);
            }
            for (WebGpuRenderBundleEncoder* encoder : m_bundleEncoders)
            {
                m_allocator->Delete(encoder);
            }
        }

    private:
        void EnsureQueryScratch(u64 size)
        {
            if (m_queryScratch != nullptr && m_queryScratchSize >= size)
            {
                return;
            }
            if (m_queryScratch != nullptr)
            {
                m_api->wgpuBufferRelease(m_queryScratch);
            }
            WGPUBufferDescriptor scratchDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
            scratchDesc.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
            scratchDesc.size = size;
            m_queryScratch = m_api->wgpuDeviceCreateBuffer(m_device, &scratchDesc);
            m_queryScratchSize = size;
        }

        void EnsureOpen()
        {
            if (m_encoder == nullptr)
            {
                WGPUCommandEncoderDescriptor wgpuDesc = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
                m_encoder = m_api->wgpuDeviceCreateCommandEncoder(m_device, &wgpuDesc);
            }
        }

        static WGPUTexelCopyBufferInfo MakeBufferInfo(Buffer* buffer,
                                                      const BufferTextureCopyRegion& region)
        {
            WGPUTexelCopyBufferInfo info = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
            info.buffer = static_cast<WebGpuBuffer*>(buffer)->Handle();
            info.layout.offset = region.bufferOffset;
            info.layout.bytesPerRow = region.bytesPerRow;
            info.layout.rowsPerImage = region.rowsPerImage;
            return info;
        }

        static WGPUTexelCopyTextureInfo MakeTextureInfo(Texture* texture,
                                                        const BufferTextureCopyRegion& region)
        {
            WGPUTexelCopyTextureInfo info = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            info.texture = static_cast<WebGpuTexture*>(texture)->Handle();
            info.mipLevel = region.textureMipLevel;
            info.origin = WGPUOrigin3D{region.textureOrigin.x, region.textureOrigin.y,
                                       region.textureOrigin.z};
            info.origin.z = region.textureOrigin.z != 0 ? region.textureOrigin.z
                                                        : region.textureArrayLayer;
            return info;
        }

        /// A single-mip single-layer 2D view for the blit pass (sampled or target).
        WGPUTextureView MipView(Texture* texture, u32 mipLevel, u32 arrayLayer,
                                WGPUTextureAspect aspect, bool /*sampled*/)
        {
            WGPUTextureViewDescriptor viewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            viewDesc.format = ToWgpuTextureFormat(texture->desc.format);
            viewDesc.dimension = WGPUTextureViewDimension_2D;
            viewDesc.baseMipLevel = mipLevel;
            viewDesc.mipLevelCount = 1;
            viewDesc.baseArrayLayer = arrayLayer;
            viewDesc.arrayLayerCount = 1;
            viewDesc.aspect = aspect;
            return m_api->wgpuTextureCreateView(
                static_cast<WebGpuTexture*>(texture)->Handle(), &viewDesc);
        }

        static bool HasStencil(TextureFormat format)
        {
            return format == TextureFormat::Depth24PlusStencil8 ||
                   format == TextureFormat::Depth32FloatStencil8 ||
                   format == TextureFormat::Stencil8;
        }

        const WebGpuApi* m_api = nullptr;
        WGPUDevice m_device = nullptr;
        IAllocator* m_allocator = nullptr;
        WebGpuBlitHelper* m_blitHelper = nullptr;
        WGPUCommandEncoder m_encoder = nullptr;
        WebGpuRenderPassEncoder m_renderPass;
        WebGpuComputePassEncoder m_computePass;
        WebGpuCommandBuffer m_commandBuffer;
        WGPUBuffer m_queryScratch = nullptr;
        u64 m_queryScratchSize = 0;
        Array<WebGpuRenderBundleEncoder*> m_bundleEncoders;
    };
}
