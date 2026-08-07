// Draconic::VG::Renderer - :renderer partition.
//
// VGRenderer: draws VGContext/VGBatch content through the RHI. Owns per-frame
// vertex/index/uniform ring buffers (byte-offset sub-allocated across slices),
// the pipeline, and an on-demand ImageData->GPU texture cache. Ported from
// Sedulous.VG.Renderer/VGRenderer.bf.
//
// Deviations from Sedulous (deliberate, documented):
//   * Initialize takes the two pre-compiled rhi::ShaderModule* (vert, frag)
//     rather than a ShaderSystem - shader compilation (DXC) is the caller's job
//     via draconic.shaders, keeping this lib's dependency to pure RHI.
//   * Per-renderer external textures ARE supported (RegisterExternalTexture /
//     UnregisterExternalTexture) so a caller-owned rhi::TextureView - e.g. a
//     ui::viewport offscreen render target - can be sampled via DrawImage. The
//     SHARED cross-renderer external texture cache (VGExternalTextureCache) is
//     still omitted (a multi-renderer/multi-window sharing optimisation); add
//     later if a single RT must be sampled by more than one window's renderer.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vg.renderer:renderer;

import draconic.foundation;
import draconic.rhi;
import draconic.image;
import draconic.texture;
import draconic.vg;
import :vertex;

using namespace draconic::foundation;

export namespace draconic::vg::renderer
{
    /// The stencil-capable depth-stencil format a device accepts at the given sample
    /// count, probed with a tiny texture (Vulkan drivers commonly support only one of
    /// D24S8 / D32S8). Undefined = none - the host skips the stencil config.
    [[nodiscard]] inline draconic::rhi::TextureFormat
    PickStencilCapableFormat(draconic::rhi::Device& device, draconic::foundation::u32 sampleCount)
    {
        namespace rhi = draconic::rhi;
        const rhi::TextureFormat candidates[3] = {rhi::TextureFormat::Depth24PlusStencil8,
                                                  rhi::TextureFormat::Depth32FloatStencil8,
                                                  rhi::TextureFormat::Stencil8};
        for (rhi::TextureFormat format : candidates)
        {
            rhi::TextureDesc desc{};
            desc.dimension = rhi::TextureDimension::Texture2D;
            desc.format = format;
            desc.width = 4;
            desc.height = 4;
            desc.depth = 1;
            desc.usage = rhi::TextureUsage::DepthStencil;
            desc.sampleCount = sampleCount;
            rhi::Texture* probe = nullptr;
            if (device.CreateTexture(desc, probe).IsOk() && probe != nullptr)
            {
                device.DestroyTexture(probe);
                return format;
            }
        }
        return rhi::TextureFormat::Undefined;
    }

    /// What the HOST's render target looks like. sampleCount > 1 = the host renders VG
    /// into an MSAA target (and resolves it itself); depthStencilFormat != Undefined =
    /// the pass carries that depth-stencil attachment (stencil cleared to 0 by the
    /// host), which unlocks the stencil-then-cover fill pipelines. Every pipeline must
    /// match the pass, so BOTH values apply to ALL pipelines, not just the stencil ones.
    struct VGTargetConfig
    {
        draconic::foundation::u32 sampleCount = 1;
        draconic::rhi::TextureFormat depthStencilFormat = draconic::rhi::TextureFormat::Undefined;
    };

    namespace rhi = draconic::rhi;
    namespace image = draconic::image;

    /// Projection uniform (one per slice, padded to UniformSlotSize on the GPU). The DF fields
    /// carry the MSDF spread + atlas size for the distance-field fragment shader's screen-space
    /// AA; they are ignored by the default pipeline.
    struct VGUniforms
    {
        Float4x4 projection = Float4x4::Identity();
        f32 dfPxRange = 4.0f;
        f32 dfAtlasW = 512.0f;
        f32 dfAtlasH = 512.0f;
        f32 pad = 0.0f;
    };

    /// A handle to one batch's data inside the shared frame buffers. Returned by
    /// Prepare, consumed by Render. Invalid slices are no-ops.
    struct VGRenderSlice
    {
        u32 vertexByteOffset = 0;
        u32 indexByteOffset = 0;
        u32 uniformByteOffset = 0;
        i32 drawCommandStart = 0;
        i32 drawCommandCount = 0;
        bool isValid = false;
    };

    /// Renders VGBatch content via the RHI (alpha-blended, analytical-AA).
    /// Does not own the device/swapchain.
    class VGRenderer
    {
    public:
        VGRenderer() = default;
        ~VGRenderer() { Dispose(); }
        VGRenderer(const VGRenderer&) = delete;
        VGRenderer& operator=(const VGRenderer&) = delete;

        [[nodiscard]] bool IsInitialized() const { return m_initialized; }

        /// True when Initialize was given a stencil-capable target (hosts gate
        /// VGContext::SetStencilFills on this).
        [[nodiscard]] bool StencilFillsSupported() const { return m_coverPipeline != nullptr; }

        /// Initialize with a device + the (already compiled) vg vertex/fragment
        /// shader modules + the render-target format + frame count.
        Status Initialize(rhi::Device& device, rhi::ShaderModule& vertShader,
                          rhi::ShaderModule& fragShader, rhi::TextureFormat targetFormat,
                          i32 frameCount, rhi::ShaderModule* dfFragShader = nullptr,
                          rhi::ShaderModule* gradRadialFragShader = nullptr,
                          rhi::ShaderModule* gradConicFragShader = nullptr,
                          VGTargetConfig targetConfig = {})
        {
            m_device = &device;
            m_queue = device.GetQueue(rhi::QueueType::Graphics, 0);
            m_targetFormat = targetFormat;
            m_frameCount = frameCount;
            m_targetConfig = targetConfig;
            // Kept for LAZY pipeline variants (non-Normal blend modes build on first
            // use). Borrowed - the shader system owns them and outlives this renderer.
            m_vsModule = &vertShader;
            m_fsModule = &fragShader;
            m_dfModule = dfFragShader;
            m_gradRadialModule = gradRadialFragShader;
            m_gradConicModule = gradConicFragShader;

            if (!CreateSampler().IsOk())
                return ErrorCode::Unknown;
            if (!CreateLayouts().IsOk())
                return ErrorCode::Unknown;
            if (!CreatePipelineInto(vertShader, fragShader, m_pipeline).IsOk())
                return ErrorCode::Unknown;
            // Optional distance-field pipeline (same layout/vertex format, MSDF fragment shader).
            if (dfFragShader != nullptr &&
                !CreatePipelineInto(vertShader, *dfFragShader, m_dfPipeline).IsOk())
                return ErrorCode::Unknown;
            // Optional per-pixel radial/conic gradient pipelines (same layout; the fragment shader
            // derives the gradient parameter per pixel from the emitted gradient-space texcoord).
            if (gradRadialFragShader != nullptr &&
                !CreatePipelineInto(vertShader, *gradRadialFragShader, m_gradRadialPipeline).IsOk())
                return ErrorCode::Unknown;
            if (gradConicFragShader != nullptr &&
                !CreatePipelineInto(vertShader, *gradConicFragShader, m_gradConicPipeline).IsOk())
                return ErrorCode::Unknown;
            // Stencil-then-cover pipelines - only with a host-provided stencil attachment.
            // Write pass: color-masked fans accumulate winding (incr/decr-wrap = NonZero,
            // invert = EvenOdd). Cover pass: draw where stencil != 0 and ZERO it behind
            // (invert only ever yields 0x00/0xFF, so NotEqual-0 serves BOTH fill rules and
            // the cover needs no per-rule variant). Cover exists per fragment variant so
            // gradient fills cover with their exact per-pixel shader.
            if (targetConfig.depthStencilFormat != rhi::TextureFormat::Undefined)
            {
                if (!CreatePipelineVariant(vertShader, fragShader, StencilRole::WriteNonZero,
                                           m_stencilWriteNonZero)
                         .IsOk())
                    return ErrorCode::Unknown;
                if (!CreatePipelineVariant(vertShader, fragShader, StencilRole::WriteEvenOdd,
                                           m_stencilWriteEvenOdd)
                         .IsOk())
                    return ErrorCode::Unknown;
                if (!CreatePipelineVariant(vertShader, fragShader, StencilRole::Cover,
                                           m_coverPipeline)
                         .IsOk())
                    return ErrorCode::Unknown;
                if (gradRadialFragShader != nullptr &&
                    !CreatePipelineVariant(vertShader, *gradRadialFragShader, StencilRole::Cover,
                                           m_coverGradRadialPipeline)
                         .IsOk())
                    return ErrorCode::Unknown;
                if (gradConicFragShader != nullptr &&
                    !CreatePipelineVariant(vertShader, *gradConicFragShader, StencilRole::Cover,
                                           m_coverGradConicPipeline)
                         .IsOk())
                    return ErrorCode::Unknown;
                // Path clipping (VGContext::PushClipPath): the mask writer + eraser.
                if (!CreatePipelineVariant(vertShader, fragShader, StencilRole::ClipApply,
                                           m_clipApplyPipeline)
                         .IsOk())
                    return ErrorCode::Unknown;
                if (!CreatePipelineVariant(vertShader, fragShader, StencilRole::ClipClear,
                                           m_clipClearPipeline)
                         .IsOk())
                    return ErrorCode::Unknown;
            }
            if (!CreatePerFrameResources().IsOk())
                return ErrorCode::Unknown;

            m_frameVertexOffsets.Resize(static_cast<usize>(frameCount));
            m_frameIndexOffsets.Resize(static_cast<usize>(frameCount));
            m_frameUniformSlotCount.Resize(static_cast<usize>(frameCount));

            m_initialized = true;
            return ErrorCode::Ok;
        }

        /// Reset per-frame ring-offset state. Call once before the frame's first Prepare.
        void BeginFrame(i32 frameIndex)
        {
            // Age the retired texture entries (evicted via VGBatch::evictedTextures) and
            // free the ones every in-flight frame is done with. Immediate disposal at
            // eviction time would destroy bind groups a submitted frame still references.
            for (usize i = m_retiredTextures.Size(); i > 0; --i)
            {
                RetiredTexture& retired = m_retiredTextures[i - 1];
                if (--retired.framesLeft <= 0)
                {
                    DisposeCachedTexture(*retired.entry);
                    m_retiredTextures.RemoveAt(i - 1);
                }
            }
            m_frameVertexOffsets[static_cast<usize>(frameIndex)] = 0;
            m_frameIndexOffsets[static_cast<usize>(frameIndex)] = 0;
            m_frameUniformSlotCount[static_cast<usize>(frameIndex)] = 0;
            m_drawCommands.Clear();
            m_batchTextures.Clear();
        }

        /// Upload one batch into the shared frame buffers; returns a slice token.
        VGRenderSlice Prepare(draconic::vg::VGBatch& batch, i32 frameIndex, u32 width, u32 height)
        {
            // The batch's eviction list is the invalidation signal for the identity-keyed
            // texture cache (the producer freed/recycled those sources - e.g. gradient-LUT
            // cache eviction). Process it even when the batch draws nothing, and BEFORE any
            // upload: a recycled allocation may reuse an evicted address this very frame.
            for (usize i = 0; i < batch.evictedTextures.Size(); ++i)
                EvictCachedTexture(batch.evictedTextures[i]);

            const u32 vertCountIn = static_cast<u32>(batch.vertices.Size());
            const u32 idxCountIn = static_cast<u32>(batch.indices.Size());
            if (vertCountIn == 0 || idxCountIn == 0)
                return VGRenderSlice{};

            const u32 vertByteSize = vertCountIn * static_cast<u32>(sizeof(VGRenderVertex));
            const u32 idxByteSize = idxCountIn * static_cast<u32>(sizeof(u32));
            const u32 sliceVertOffset = m_frameVertexOffsets[static_cast<usize>(frameIndex)];
            const u32 sliceIdxOffset = m_frameIndexOffsets[static_cast<usize>(frameIndex)];
            const u32 sliceUniformSlot = m_frameUniformSlotCount[static_cast<usize>(frameIndex)];

            const u32 maxVertBytes = static_cast<u32>(MaxVertices * sizeof(VGRenderVertex));
            const u32 maxIdxBytes = static_cast<u32>(MaxIndices * sizeof(u32));
            if (sliceVertOffset + vertByteSize > maxVertBytes ||
                sliceIdxOffset + idxByteSize > maxIdxBytes ||
                sliceUniformSlot >= static_cast<u32>(MaxUniformSlots))
                return VGRenderSlice{}; // capacity exceeded

            const u32 sliceUniformOffset = sliceUniformSlot * static_cast<u32>(UniformSlotSize);
            const i32 sliceCmdStart = static_cast<i32>(m_drawCommands.Size());
            const i32 textureBase = static_cast<i32>(m_batchTextures.Size());

            // Convert + upload this slice's vertices.
            Array<VGRenderVertex> renderVerts;
            renderVerts.Reserve(batch.vertices.Size());
            for (usize i = 0; i < batch.vertices.Size(); ++i)
                renderVerts.PushBack(VGRenderVertex(batch.vertices[i]));
            WriteBuffer(m_vertexBuffers[static_cast<usize>(frameIndex)], sliceVertOffset,
                        renderVerts.Data(), vertByteSize);

            // Upload indices verbatim (relative to the slice's vertex base).
            WriteBuffer(m_indexBuffers[static_cast<usize>(frameIndex)], sliceIdxOffset,
                        batch.indices.Data(), idxByteSize);

            // Append textures (commands index into the shared batch-texture list).
            for (usize i = 0; i < batch.textures.Size(); ++i)
                m_batchTextures.PushBack(batch.textures[i]);
            for (usize i = 0; i < batch.commands.Size(); ++i)
            {
                draconic::vg::VGCommand cmd = batch.commands[i];
                if (cmd.textureIndex >= 0)
                    cmd.textureIndex = cmd.textureIndex + textureBase;
                m_drawCommands.PushBack(cmd);
            }

            // Write this slice's projection + distance-field metadata into its uniform slot.
            VGUniforms uniforms;
            uniforms.projection = OrthoOffCenter(static_cast<f32>(width), static_cast<f32>(height));
            uniforms.dfPxRange = batch.dfPxRange;
            uniforms.dfAtlasW = batch.dfAtlasW;
            uniforms.dfAtlasH = batch.dfAtlasH;
            WriteBuffer(m_uniformBuffers[static_cast<usize>(frameIndex)], sliceUniformOffset,
                        &uniforms, sizeof(VGUniforms));

            // Bind groups for any newly-added textures.
            for (i32 texIdx = textureBase; texIdx < static_cast<i32>(m_batchTextures.Size());
                 ++texIdx)
                UpdateTextureBindGroup(texIdx, frameIndex);

            m_frameVertexOffsets[static_cast<usize>(frameIndex)] = sliceVertOffset + vertByteSize;
            m_frameIndexOffsets[static_cast<usize>(frameIndex)] = sliceIdxOffset + idxByteSize;
            m_frameUniformSlotCount[static_cast<usize>(frameIndex)] = sliceUniformSlot + 1;

            VGRenderSlice slice;
            slice.vertexByteOffset = sliceVertOffset;
            slice.indexByteOffset = sliceIdxOffset;
            slice.uniformByteOffset = sliceUniformOffset;
            slice.drawCommandStart = sliceCmdStart;
            slice.drawCommandCount = static_cast<i32>(m_drawCommands.Size()) - sliceCmdStart;
            slice.isValid = true;
            return slice;
        }

        /// A scissor rect in FRAMEBUFFER coordinates (what SetScissor takes).
        struct ScissorRect
        {
            i32 x = 0, y = 0;
            u32 width = 0, height = 0;
        };

        /// Map a command's content-space clip rect into framebuffer coordinates for a
        /// viewport whose origin sits at (viewportX, viewportY) with the given content
        /// extent: clamp to the content box first, then offset. Pure (unit-tested).
        [[nodiscard]] static ScissorRect ComputeScissor(const Rectangle& clipRect, i32 viewportX,
                                                        i32 viewportY, u32 width, u32 height)
        {
            const i32 startX = static_cast<i32>(Ceil(Max(0.0f, clipRect.x)));
            const i32 startY = static_cast<i32>(Ceil(Max(0.0f, clipRect.y)));
            const i32 endX =
                static_cast<i32>(Floor(Min(clipRect.x + clipRect.width, static_cast<f32>(width))));
            const i32 endY = static_cast<i32>(
                Floor(Min(clipRect.y + clipRect.height, static_cast<f32>(height))));
            ScissorRect rect;
            rect.x = viewportX + startX;
            rect.y = viewportY + startY;
            rect.width = static_cast<u32>(Max(0, endX - startX));
            rect.height = static_cast<u32>(Max(0, endY - startY));
            return rect;
        }

        /// Dispatch a slice's draws into the active render pass (full-target viewport).
        void Render(rhi::RenderPassEncoder& renderPass, u32 width, u32 height, i32 frameIndex,
                    const VGRenderSlice& slice)
        {
            Render(renderPass, 0, 0, width, height, frameIndex, slice);
        }

        /// Dispatch a slice's draws into a VIEWPORT SUB-RECT of the active pass's target
        /// (split-screen views): content coordinates (0..width, 0..height) map to the
        /// rect at (viewportX, viewportY); every scissor - including the default - is
        /// clamped to that rect, so content never bleeds into a neighboring view. The
        /// slice must have been Prepared with the SAME width/height (the projection).
        void Render(rhi::RenderPassEncoder& renderPass, i32 viewportX, i32 viewportY, u32 width,
                    u32 height, i32 frameIndex, const VGRenderSlice& slice)
        {
            if (!slice.isValid || slice.drawCommandCount == 0)
                return;

            renderPass.SetViewport(static_cast<f32>(viewportX), static_cast<f32>(viewportY),
                                   static_cast<f32>(width), static_cast<f32>(height), 0.0f, 1.0f);
            renderPass.SetPipeline(m_pipeline);
            renderPass.SetVertexBuffer(0, m_vertexBuffers[static_cast<usize>(frameIndex)],
                                       slice.vertexByteOffset);
            renderPass.SetIndexBuffer(m_indexBuffers[static_cast<usize>(frameIndex)],
                                      rhi::IndexFormat::UInt32, slice.indexByteOffset);

            const u32 dynOffsets[1] = {slice.uniformByteOffset};
            i32 currentTextureIndex = -2; // sentinel forces first SetBindGroup
            vg::VGGradientSpread currentSpread = vg::VGGradientSpread::Pad;
            rhi::RenderPipeline* currentPipeline = m_pipeline;
            i32 currentStencilRef = -1; // sentinel: set on first stencil-relevant command
            if (m_coverPipeline != nullptr)
            {
                renderPass.SetStencilReference(0); // cover tests NotEqual 0
                currentStencilRef = 0;
            }

            const i32 cmdEnd = slice.drawCommandStart + slice.drawCommandCount;
            for (i32 i = slice.drawCommandStart; i < cmdEnd; ++i)
            {
                const draconic::vg::VGCommand& cmd = m_drawCommands[static_cast<usize>(i)];
                if (cmd.indexCount == 0)
                    continue;

                // Pipeline per (fill phase, draw mode); falls back to the default pipeline
                // when a variant was not built. Stencil-phase commands are SKIPPED entirely
                // without stencil pipelines (a context should not emit them then, but a
                // stale batch must not draw winding fans as visible color). A pipeline swap
                // forces a bind-group rebind.
                rhi::RenderPipeline* pipeline = PipelineFor(cmd);
                if (pipeline == nullptr)
                    continue;
                if (pipeline != currentPipeline)
                {
                    renderPass.SetPipeline(pipeline);
                    currentPipeline = pipeline;
                    currentTextureIndex = -2;
                }

                // Dynamic stencil reference: 0x80 whenever the clip mask is involved
                // (clipped draws test Equal against it; ClipApply/clipped-cover REPLACE
                // with it), 0 otherwise (unclipped covers compare-to-zero; ClipClear
                // replaces with 0).
                if (m_coverPipeline != nullptr || m_clipApplyPipeline != nullptr)
                {
                    const bool wantsClipRef =
                        cmd.clipMode == draconic::vg::VGClipMode::Stencil ||
                        cmd.fillPhase == draconic::vg::VGFillPhase::ClipApply;
                    const i32 wantedRef = wantsClipRef ? 0x80 : 0;
                    if (wantedRef != currentStencilRef)
                    {
                        renderPass.SetStencilReference(static_cast<u32>(wantedRef));
                        currentStencilRef = wantedRef;
                    }
                }

                if (cmd.textureIndex != currentTextureIndex ||
                    cmd.gradientSpread != currentSpread)
                {
                    if (rhi::BindGroup* bindGroup =
                            GetBindGroupForTexture(cmd.textureIndex, frameIndex,
                                                   cmd.gradientSpread))
                        renderPass.SetBindGroup(0, bindGroup, Span<const u32>(dynOffsets, 1));
                    currentTextureIndex = cmd.textureIndex;
                    currentSpread = cmd.gradientSpread;
                }

                if (cmd.clipMode == draconic::vg::VGClipMode::Scissor &&
                    cmd.clipRect.width > 0.0f && cmd.clipRect.height > 0.0f)
                {
                    const ScissorRect scissor =
                        ComputeScissor(cmd.clipRect, viewportX, viewportY, width, height);
                    renderPass.SetScissor(scissor.x, scissor.y, scissor.width, scissor.height);
                }
                else if (cmd.clipMode == draconic::vg::VGClipMode::Scissor)
                {
                    renderPass.SetScissor(0, 0, 0, 0); // empty clip hides everything
                }
                else
                {
                    renderPass.SetScissor(viewportX, viewportY, width, height);
                }

                renderPass.DrawIndexed(static_cast<u32>(cmd.indexCount), 1,
                                       static_cast<u32>(cmd.startIndex), 0, 0);
            }
        }

        /// Clear all cached GPU textures.
        void ClearTextureCache()
        {
            for (usize i = 0; i < m_textureCache.Size(); ++i)
                DisposeCachedTexture(*m_textureCache[i]);
            m_textureCache.Clear();
            for (usize i = 0; i < m_retiredTextures.Size(); ++i)
                DisposeCachedTexture(*m_retiredTextures[i].entry);
            m_retiredTextures.Clear();
        }

        /// Drop the cache entry for `key` (an identity, never dereferenced): the GPU
        /// resources move to the retired list and are freed once every in-flight frame
        /// has aged past them. External (caller-owned) entries are untouched - those are
        /// invalidated via UnregisterExternalTexture by their owner.
        void EvictCachedTexture(const image::ImageData* key)
        {
            if (key == nullptr)
                return;
            for (usize i = 0; i < m_textureCache.Size(); ++i)
            {
                if (m_textureCache[i]->source != key || m_textureCache[i]->external)
                    continue;
                RetiredTexture retired;
                retired.entry = Move(m_textureCache[i]);
                retired.framesLeft = m_frameCount;
                m_retiredTextures.PushBack(Move(retired));
                m_textureCache.RemoveAt(i);
                return;
            }
        }

        /// Cache introspection (tests/diagnostics).
        [[nodiscard]] usize CachedTextureCount() const { return m_textureCache.Size(); }
        [[nodiscard]] usize RetiredTextureCount() const { return m_retiredTextures.Size(); }

        /// Register a caller-owned rhi::TextureView (e.g. a viewport's offscreen
        /// render target) under an ImageData identity key, so DrawImage(key, ...)
        /// samples that GPU texture directly instead of uploading CPU pixels.
        /// The view is NOT owned - the caller must UnregisterExternalTexture before
        /// destroying it. The cache is identity-keyed (raw ImageData*), so EVERY
        /// producer of transient sources must send an invalidation: external views via
        /// UnregisterExternalTexture, batch-produced sources (gradient LUTs) via
        /// VGBatch::evictedTextures - without one, a freed-and-recycled allocation
        /// cache-hits a stale GPU texture.
        /// Re-registering an existing key rebinds it to the new view (bind groups
        /// are torn down and rebuilt lazily).
        void RegisterExternalTexture(const image::ImageData* key, rhi::TextureView* view)
        {
            if (key == nullptr || view == nullptr || m_device == nullptr)
                return;

            for (usize i = 0; i < m_textureCache.Size(); ++i)
            {
                if (m_textureCache[i]->source != key)
                    continue;
                CachedTexture& c = *m_textureCache[i];
                for (usize f = 0; f < c.bindGroups.Size(); ++f)
                {
                    if (c.bindGroups[f] != nullptr)
                        m_device->DestroyBindGroup(c.bindGroups[f]);
                    c.bindGroups[f] = nullptr;
                }
                c.view = view;
                c.external = true;
                c.gpuTexture = nullptr;
                c.sourceId = key->InstanceId(); // explicit re-register refreshes identity
                return;
            }

            UniquePtr<CachedTexture> cached = MakeUnique<CachedTexture>(DefaultAllocator());
            cached->source = key;
            cached->sourceId = key->InstanceId();
            cached->view = view;
            cached->external = true;
            cached->bindGroups.Resize(static_cast<usize>(m_frameCount) *
                                      kSpreadCount); // nullptr-filled, built lazily
            m_textureCache.PushBack(Move(cached));
        }

        /// Drop a previously-registered external texture. Tears down its per-frame
        /// bind groups (but never the caller-owned view/texture). Safe to call for
        /// an unknown key. Call before the underlying view is destroyed.
        void UnregisterExternalTexture(const image::ImageData* key)
        {
            if (key == nullptr)
                return;
            for (usize i = 0; i < m_textureCache.Size(); ++i)
            {
                if (m_textureCache[i]->source != key)
                    continue;
                DisposeCachedTexture(*m_textureCache[i]);
                m_textureCache.RemoveAt(i);
                return;
            }
        }

        /// Whether an external (caller-owned) view is currently registered for key.
        [[nodiscard]] bool IsExternalTextureRegistered(const image::ImageData* key) const
        {
            if (key == nullptr)
                return false;
            for (usize i = 0; i < m_textureCache.Size(); ++i)
                if (m_textureCache[i]->source == key)
                    return m_textureCache[i]->external;
            return false;
        }

        void Dispose()
        {
            if (m_device == nullptr)
                return;

            ClearTextureCache();

            DestroyBuffers(m_uniformBuffers);
            DestroyBuffers(m_indexBuffers);
            DestroyBuffers(m_vertexBuffers);

            for (usize b = 0; b < kBlendVariantCount; ++b)
                for (usize k = 0; k < kPipelineKindCount; ++k)
                {
                    if (m_blendPipelines[b][k] != nullptr)
                        m_device->DestroyRenderPipeline(m_blendPipelines[b][k]);
                    m_blendPipelines[b][k] = nullptr;
                }
            for (usize b = 0; b < 4; ++b)
                for (usize k = 0; k < kPipelineKindCount; ++k)
                {
                    if (m_clippedPipelines[b][k] != nullptr)
                        m_device->DestroyRenderPipeline(m_clippedPipelines[b][k]);
                    m_clippedPipelines[b][k] = nullptr;
                }
            if (m_clippedWriteNonZero)
                m_device->DestroyRenderPipeline(m_clippedWriteNonZero);
            if (m_clippedWriteEvenOdd)
                m_device->DestroyRenderPipeline(m_clippedWriteEvenOdd);
            if (m_clipApplyPipeline)
                m_device->DestroyRenderPipeline(m_clipApplyPipeline);
            if (m_clipClearPipeline)
                m_device->DestroyRenderPipeline(m_clipClearPipeline);
            m_clippedWriteNonZero = nullptr;
            m_clippedWriteEvenOdd = nullptr;
            m_clipApplyPipeline = nullptr;
            m_clipClearPipeline = nullptr;
            if (m_pipeline)
                m_device->DestroyRenderPipeline(m_pipeline);
            if (m_dfPipeline)
                m_device->DestroyRenderPipeline(m_dfPipeline);
            if (m_gradRadialPipeline)
                m_device->DestroyRenderPipeline(m_gradRadialPipeline);
            if (m_gradConicPipeline)
                m_device->DestroyRenderPipeline(m_gradConicPipeline);
            if (m_stencilWriteNonZero)
                m_device->DestroyRenderPipeline(m_stencilWriteNonZero);
            if (m_stencilWriteEvenOdd)
                m_device->DestroyRenderPipeline(m_stencilWriteEvenOdd);
            if (m_coverPipeline)
                m_device->DestroyRenderPipeline(m_coverPipeline);
            if (m_coverGradRadialPipeline)
                m_device->DestroyRenderPipeline(m_coverGradRadialPipeline);
            if (m_coverGradConicPipeline)
                m_device->DestroyRenderPipeline(m_coverGradConicPipeline);
            if (m_pipelineLayout)
                m_device->DestroyPipelineLayout(m_pipelineLayout);
            if (m_bindGroupLayout)
                m_device->DestroyBindGroupLayout(m_bindGroupLayout);
            if (m_sampler)
                m_device->DestroySampler(m_sampler);
            if (m_samplerRepeat)
                m_device->DestroySampler(m_samplerRepeat);
            if (m_samplerMirror)
                m_device->DestroySampler(m_samplerMirror);

            m_pipeline = nullptr;
            m_dfPipeline = nullptr;
            m_stencilWriteNonZero = nullptr;
            m_stencilWriteEvenOdd = nullptr;
            m_coverPipeline = nullptr;
            m_coverGradRadialPipeline = nullptr;
            m_coverGradConicPipeline = nullptr;
            m_gradRadialPipeline = nullptr;
            m_gradConicPipeline = nullptr;
            m_pipelineLayout = nullptr;
            m_bindGroupLayout = nullptr;
            m_sampler = nullptr;
            m_samplerRepeat = nullptr;
            m_samplerMirror = nullptr;
            m_initialized = false;
            m_device = nullptr;
        }

    private:
        struct CachedTexture
        {
            const image::ImageData* source = nullptr;
            u64 sourceId = 0; // ImageData::InstanceId() - guards against address reuse
            rhi::Texture* gpuTexture = nullptr;
            rhi::TextureView* view = nullptr;
            // Per (frame, spread): slot = frame * kSpreadCount + spread. Spread picks
            // the LUT sampler, and the sampler lives in the bind group.
            Array<rhi::BindGroup*> bindGroups;
            bool external =
                false; // view is caller-owned (e.g. a viewport RT) - never destroyed here
        };

        static constexpr usize kSpreadCount = 3; // Pad / Repeat / Reflect bind-group slots
        // Stencil bit planes (must match VGFillPhase's contract): bit 7 = clip mask,
        // bits 0..6 = fill winding.
        static constexpr u8 kClipBit = 0x80;
        static constexpr u8 kWindingMask = 0x7F;

        static constexpr i32 MaxVertices = 131072;
        static constexpr i32 MaxIndices = 131072 * 3;
        static constexpr i32 MaxUniformSlots = 64;
        static constexpr i32 UniformSlotSize =
            256; // dynamic-offset alignment (>= sizeof(VGUniforms)=64)

        static void WriteBuffer(rhi::Buffer* buf, u64 offset, const void* data, usize size)
        {
            if (buf == nullptr || size == 0)
                return;
            if (u8* p = static_cast<u8*>(buf->Map()))
            {
                MemCopy(p + offset, data, size);
                buf->Unmap();
            }
        }

        static Float4x4 OrthoOffCenter(f32 width, f32 height)
        {
            // CreateOrthographicOffCenter(0, width, height, 0, -1, 1) (row-vector).
            Float4x4 m = Float4x4::Identity();
            m.m[0][0] = 2.0f / width;
            m.m[1][1] = -2.0f / height;
            m.m[2][2] = -0.5f;
            m.m[3][0] = -1.0f;
            m.m[3][1] = 1.0f;
            m.m[3][2] = 0.5f;
            return m;
        }

        Status CreateSampler()
        {
            // Default: CLAMP. Images and pad-spread gradients both want edge clamping
            // (repeat would bleed the opposite edge into bilinear taps at u/v 0 and 1);
            // the pad LUT path additionally relies on it for its out-of-range clamp.
            rhi::SamplerDesc desc{};
            desc.addressU = rhi::AddressMode::ClampToEdge;
            desc.addressV = rhi::AddressMode::ClampToEdge;
            desc.addressW = rhi::AddressMode::ClampToEdge;
            if (!m_device->CreateSampler(desc, m_sampler).IsOk())
            {
                return ErrorCode::Unknown;
            }
            // Spread samplers: the gradient LUT wraps (Repeat) or mirrors (Reflect) so
            // the shader's raw parameter tiles per pixel (see VGGradientSpread).
            desc.addressU = rhi::AddressMode::Repeat;
            desc.addressV = rhi::AddressMode::Repeat;
            desc.addressW = rhi::AddressMode::Repeat;
            if (!m_device->CreateSampler(desc, m_samplerRepeat).IsOk())
            {
                return ErrorCode::Unknown;
            }
            desc.addressU = rhi::AddressMode::MirrorRepeat;
            desc.addressV = rhi::AddressMode::MirrorRepeat;
            desc.addressW = rhi::AddressMode::MirrorRepeat;
            return m_device->CreateSampler(desc, m_samplerMirror);
        }

        [[nodiscard]] rhi::Sampler* SamplerForSpread(vg::VGGradientSpread spread) const
        {
            switch (spread)
            {
            case vg::VGGradientSpread::Repeat:
                return m_samplerRepeat;
            case vg::VGGradientSpread::Reflect:
                return m_samplerMirror;
            case vg::VGGradientSpread::Pad:
            default:
                return m_sampler;
            }
        }

        Status CreateLayouts()
        {
            rhi::BindGroupLayoutEntry entries[3];
            entries[0] = rhi::BindGroupLayoutEntry::UniformBuffer(
                0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
            entries[0].hasDynamicOffset =
                true; // one uniform buffer shared across slices via dynamic offset
            entries[1] = rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
            entries[2] = rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);

            rhi::BindGroupLayoutDesc bglDesc{};
            bglDesc.entries = Span<const rhi::BindGroupLayoutEntry>(entries, 3);
            if (!m_device->CreateBindGroupLayout(bglDesc, m_bindGroupLayout).IsOk())
                return ErrorCode::Unknown;

            rhi::BindGroupLayout* const layouts[1] = {m_bindGroupLayout};
            rhi::PipelineLayoutDesc plDesc{};
            plDesc.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(layouts, 1);
            return m_device->CreatePipelineLayout(plDesc, m_pipelineLayout);
        }

        // Build a VG pipeline (shared layout + vertex format) with the given fragment shader into
        // `outPipeline` - used for both the default and the distance-field variants.
        /// The pipeline a command renders with: stencil write/cover variants for
        /// stencil-phase commands (null = unconfigured, caller skips), else the
        /// draw-mode variant with default fallback.
        [[nodiscard]] rhi::RenderPipeline* PipelineFor(const draconic::vg::VGCommand& cmd)
        {
            const bool blended = cmd.blendMode != vg::VGBlendMode::Normal;
            const bool clipped = cmd.clipMode == vg::VGClipMode::Stencil;
            switch (cmd.fillPhase)
            {
            case draconic::vg::VGFillPhase::ClipApply:
                return m_clipApplyPipeline; // null = unconfigured, command skipped
            case draconic::vg::VGFillPhase::ClipClear:
                return m_clipClearPipeline;
            case draconic::vg::VGFillPhase::StencilWrite:
                // Color-masked winding accumulation: the blend state is irrelevant, the
                // clip state is not (a clipped fill only accumulates inside the mask).
                if (clipped)
                    return cmd.fillRule == draconic::vg::FillRule::EvenOdd
                               ? ClippedWrite(StencilRole::WriteEvenOdd, m_clippedWriteEvenOdd)
                               : ClippedWrite(StencilRole::WriteNonZero, m_clippedWriteNonZero);
                return cmd.fillRule == draconic::vg::FillRule::EvenOdd ? m_stencilWriteEvenOdd
                                                                       : m_stencilWriteNonZero;
            case draconic::vg::VGFillPhase::StencilCover:
                if (m_coverPipeline == nullptr)
                    return nullptr;
                if (cmd.drawMode == draconic::vg::VGDrawMode::GradientRadial &&
                    m_coverGradRadialPipeline != nullptr)
                    return (blended || clipped)
                               ? Variant(PipelineKind::CoverRadial, cmd.blendMode, clipped)
                               : m_coverGradRadialPipeline;
                if (cmd.drawMode == draconic::vg::VGDrawMode::GradientConic &&
                    m_coverGradConicPipeline != nullptr)
                    return (blended || clipped)
                               ? Variant(PipelineKind::CoverConic, cmd.blendMode, clipped)
                               : m_coverGradConicPipeline;
                return (blended || clipped) ? Variant(PipelineKind::Cover, cmd.blendMode, clipped)
                                            : m_coverPipeline;
            case draconic::vg::VGFillPhase::Direct:
                break;
            }
            if (cmd.drawMode == draconic::vg::VGDrawMode::DistanceField && m_dfPipeline != nullptr)
                return (blended || clipped)
                           ? Variant(PipelineKind::DistanceField, cmd.blendMode, clipped)
                           : m_dfPipeline;
            if (cmd.drawMode == draconic::vg::VGDrawMode::GradientRadial &&
                m_gradRadialPipeline != nullptr)
                return (blended || clipped)
                           ? Variant(PipelineKind::GradRadial, cmd.blendMode, clipped)
                           : m_gradRadialPipeline;
            if (cmd.drawMode == draconic::vg::VGDrawMode::GradientConic &&
                m_gradConicPipeline != nullptr)
                return (blended || clipped)
                           ? Variant(PipelineKind::GradConic, cmd.blendMode, clipped)
                           : m_gradConicPipeline;
            return (blended || clipped) ? Variant(PipelineKind::Default, cmd.blendMode, clipped)
                                        : m_pipeline;
        }


        /// The fragment-shader/role families that need per-blend pipeline variants
        /// (stencil WRITE pipelines are color-masked and blend-agnostic).
        enum class PipelineKind : u8
        {
            Default,
            DistanceField,
            GradRadial,
            GradConic,
            Cover,
            CoverRadial,
            CoverConic,
        };
        static constexpr usize kPipelineKindCount = 7;
        static constexpr usize kBlendVariantCount = 3; // Additive / Multiply / Screen

        /// Get-or-create the (kind, blend, clipped) pipeline. Built on FIRST use - most
        /// content never leaves (Normal, unclipped), so the matrices usually stay empty.
        /// Blend-fallback goes to the Normal pipeline (wrong-blend draw beats no draw);
        /// a failed CLIPPED variant returns null instead - drawing unclipped would paint
        /// outside the clip.
        [[nodiscard]] rhi::RenderPipeline* Variant(PipelineKind kind, vg::VGBlendMode blendMode,
                                                   bool clipped)
        {
            rhi::RenderPipeline*& slot =
                clipped ? m_clippedPipelines[static_cast<usize>(blendMode)][static_cast<usize>(
                              kind)]
                        : m_blendPipelines[static_cast<usize>(blendMode) - 1]
                                          [static_cast<usize>(kind)];
            if (slot != nullptr)
            {
                return slot;
            }
            rhi::ShaderModule* frag = nullptr;
            StencilRole role = StencilRole::None;
            switch (kind)
            {
            case PipelineKind::Default:
                frag = m_fsModule;
                break;
            case PipelineKind::DistanceField:
                frag = m_dfModule;
                break;
            case PipelineKind::GradRadial:
                frag = m_gradRadialModule;
                break;
            case PipelineKind::GradConic:
                frag = m_gradConicModule;
                break;
            case PipelineKind::Cover:
                frag = m_fsModule;
                role = StencilRole::Cover;
                break;
            case PipelineKind::CoverRadial:
                frag = m_gradRadialModule;
                role = StencilRole::Cover;
                break;
            case PipelineKind::CoverConic:
                frag = m_gradConicModule;
                role = StencilRole::Cover;
                break;
            }
            if (m_vsModule == nullptr || frag == nullptr ||
                (clipped && m_targetConfig.depthStencilFormat == rhi::TextureFormat::Undefined) ||
                !CreatePipelineVariant(*m_vsModule, *frag, role, slot, blendMode, clipped).IsOk())
            {
                slot = nullptr;
                return clipped ? nullptr : m_pipeline;
            }
            return slot;
        }

        /// Which stencil-then-cover role a pipeline plays (None = ordinary color draw).
        enum class StencilRole : u8
        {
            None,
            WriteNonZero, ///< color-masked; front incr-wrap / back decr-wrap (winding count)
            WriteEvenOdd, ///< color-masked; invert both faces (parity)
            Cover,        ///< test NotEqual, zero/restore behind the covered pixels
            ClipApply,    ///< color-masked; winding -> the 0x80 clip mask (Replace)
            ClipClear,    ///< color-masked; unconditional Replace 0 over the clip bounds
        };

        /// Lazy clipped stencil-write pair (needs a stencil attachment).
        [[nodiscard]] rhi::RenderPipeline* ClippedWrite(StencilRole role,
                                                        rhi::RenderPipeline*& slot)
        {
            if (slot != nullptr)
                return slot;
            if (m_vsModule == nullptr || m_fsModule == nullptr ||
                m_targetConfig.depthStencilFormat == rhi::TextureFormat::Undefined ||
                !CreatePipelineVariant(*m_vsModule, *m_fsModule, role, slot,
                                       vg::VGBlendMode::Normal, /*clipped*/ true)
                     .IsOk())
            {
                slot = nullptr;
                return nullptr; // skip the command rather than corrupt the mask
            }
            return slot;
        }

        Status CreatePipelineInto(rhi::ShaderModule& vertShader, rhi::ShaderModule& fragShader,
                                  rhi::RenderPipeline*& outPipeline)
        {
            return CreatePipelineVariant(vertShader, fragShader, StencilRole::None, outPipeline);
        }

        Status CreatePipelineVariant(rhi::ShaderModule& vertShader, rhi::ShaderModule& fragShader,
                                     StencilRole role, rhi::RenderPipeline*& outPipeline,
                                     vg::VGBlendMode blendMode = vg::VGBlendMode::Normal,
                                     bool clipped = false)
        {
            const rhi::VertexAttribute attributes[4] = {
                {rhi::VertexFormat::Float32x2, 0, 0},  // position
                {rhi::VertexFormat::Float32x2, 8, 1},  // texCoord
                {rhi::VertexFormat::Float32x4, 16, 2}, // color
                {rhi::VertexFormat::Float32, 32, 3},   // coverage
            };
            rhi::VertexBufferLayout vbLayout{};
            vbLayout.stride = static_cast<u32>(sizeof(VGRenderVertex));
            vbLayout.attributes = Span<const rhi::VertexAttribute>(attributes, 4);
            const rhi::VertexBufferLayout vertexBuffers[1] = {vbLayout};

            rhi::ColorTargetState colorTarget{};
            colorTarget.format = m_targetFormat;
            // Premultiplied-alpha compositing: both VG fragment shaders output premultiplied color
            // (rgb *= a). This removes the dark halo on straight-alpha AA edges and the double-blend
            // seams at fringe/join overlaps, and lets stencil-cover fills composite exactly. The
            // non-Normal modes are the premultiplied-source formulations, alpha-aware so a fill's
            // transparent surround leaves the destination untouched.
            switch (blendMode)
            {
            case vg::VGBlendMode::Additive: // src + dst
                colorTarget.blend = rhi::BlendState::Additive();
                break;
            case vg::VGBlendMode::Multiply: // src*dst + dst*(1-srcA)
                colorTarget.blend =
                    rhi::BlendState{{rhi::BlendFactor::Dst, rhi::BlendFactor::OneMinusSrcAlpha,
                                     rhi::BlendOperation::Add},
                                    {rhi::BlendFactor::One, rhi::BlendFactor::OneMinusSrcAlpha,
                                     rhi::BlendOperation::Add}};
                break;
            case vg::VGBlendMode::Screen: // src + dst*(1-src)
                colorTarget.blend =
                    rhi::BlendState{{rhi::BlendFactor::One, rhi::BlendFactor::OneMinusSrc,
                                     rhi::BlendOperation::Add},
                                    {rhi::BlendFactor::One, rhi::BlendFactor::OneMinusSrcAlpha,
                                     rhi::BlendOperation::Add}};
                break;
            case vg::VGBlendMode::Normal:
            default:
                colorTarget.blend = rhi::BlendState::PremultipliedAlpha();
                break;
            }
            const bool isWrite =
                role == StencilRole::WriteNonZero || role == StencilRole::WriteEvenOdd ||
                role == StencilRole::ClipApply || role == StencilRole::ClipClear;
            if (isWrite)
            {
                colorTarget.writeMask = rhi::ColorWriteMask::None; // stencil only, no color
            }
            const rhi::ColorTargetState colorTargets[1] = {colorTarget};

            rhi::RenderPipelineDesc desc{};
            desc.layout = m_pipelineLayout;
            desc.vertex.shader =
                rhi::ProgrammableStage{&vertShader, u8"main", rhi::ShaderStage::Vertex};
            desc.vertex.buffers = Span<const rhi::VertexBufferLayout>(vertexBuffers, 1);

            rhi::FragmentState fragment{};
            fragment.shader =
                rhi::ProgrammableStage{&fragShader, u8"main", rhi::ShaderStage::Fragment};
            fragment.targets = Span<const rhi::ColorTargetState>(colorTargets, 1);
            desc.fragment = fragment;

            desc.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            desc.primitive.frontFace = rhi::FrontFace::CCW;
            desc.primitive.cullMode = rhi::CullMode::None; // write pass NEEDS both faces
            desc.multisample.count = m_targetConfig.sampleCount;
            desc.multisample.alphaToCoverageEnabled = false;

            // Every pipeline must declare the pass's depth-stencil attachment when the host
            // provides one (pass compatibility), with depth fully disabled - VG never
            // touches depth. Stencil state per role.
            if (m_targetConfig.depthStencilFormat != rhi::TextureFormat::Undefined)
            {
                rhi::DepthStencilState ds{};
                ds.format = m_targetConfig.depthStencilFormat;
                ds.depthTestEnabled = false;
                ds.depthWriteEnabled = false;
                ds.depthCompare = rhi::CompareFunction::Always;
                switch (role)
                {
                case StencilRole::WriteNonZero:
                    // Winding accumulates in the LOW bits only - the clip mask survives.
                    // Clipped: only accumulate where the clip bit is set (ref 0x80).
                    ds.stencilEnabled = true;
                    ds.stencilWriteMask = kWindingMask;
                    ds.stencilReadMask = kClipBit;
                    ds.stencilFront = rhi::StencilFaceState{clipped ? rhi::CompareFunction::Equal
                                                                    : rhi::CompareFunction::Always,
                                                            rhi::StencilOperation::Keep,
                                                            rhi::StencilOperation::Keep,
                                                            rhi::StencilOperation::IncrementWrap};
                    ds.stencilBack = rhi::StencilFaceState{clipped ? rhi::CompareFunction::Equal
                                                                   : rhi::CompareFunction::Always,
                                                           rhi::StencilOperation::Keep,
                                                           rhi::StencilOperation::Keep,
                                                           rhi::StencilOperation::DecrementWrap};
                    break;
                case StencilRole::WriteEvenOdd:
                    ds.stencilEnabled = true;
                    ds.stencilWriteMask = kWindingMask; // invert flips winding bits ONLY
                    ds.stencilReadMask = kClipBit;
                    ds.stencilFront = rhi::StencilFaceState{clipped ? rhi::CompareFunction::Equal
                                                                    : rhi::CompareFunction::Always,
                                                            rhi::StencilOperation::Keep,
                                                            rhi::StencilOperation::Keep,
                                                            rhi::StencilOperation::Invert};
                    ds.stencilBack = ds.stencilFront;
                    break;
                case StencilRole::Cover:
                    ds.stencilEnabled = true;
                    if (clipped)
                    {
                        // Inside = clip set AND winding != 0: value != 0x80 under a FULL
                        // read mask. Pass op REPLACE (ref 0x80) zeroes the winding while
                        // RESTORING the clip mask.
                        ds.stencilReadMask = 0xFF;
                        ds.stencilWriteMask = 0xFF;
                        ds.stencilFront =
                            rhi::StencilFaceState{rhi::CompareFunction::NotEqual,
                                                  rhi::StencilOperation::Keep,
                                                  rhi::StencilOperation::Keep,
                                                  rhi::StencilOperation::Replace};
                    }
                    else
                    {
                        // Inside = winding != 0 (clip bit ignored + preserved: both the
                        // read and the zeroing write stay in the winding bits).
                        ds.stencilReadMask = kWindingMask;
                        ds.stencilWriteMask = kWindingMask;
                        ds.stencilFront =
                            rhi::StencilFaceState{rhi::CompareFunction::NotEqual,
                                                  rhi::StencilOperation::Zero,
                                                  rhi::StencilOperation::Zero,
                                                  rhi::StencilOperation::Zero};
                    }
                    ds.stencilBack = ds.stencilFront;
                    break;
                case StencilRole::ClipApply:
                    // Winding != 0 (winding-bit compare: ref 0x80 & 0x7F == 0) -> REPLACE
                    // with ref 0x80: the clip mask is set and the winding zeroed in one
                    // op. Fail (winding == 0) zeroes any stray bits.
                    ds.stencilEnabled = true;
                    ds.stencilReadMask = kWindingMask;
                    ds.stencilWriteMask = 0xFF;
                    ds.stencilFront = rhi::StencilFaceState{rhi::CompareFunction::NotEqual,
                                                            rhi::StencilOperation::Zero,
                                                            rhi::StencilOperation::Zero,
                                                            rhi::StencilOperation::Replace};
                    ds.stencilBack = ds.stencilFront;
                    break;
                case StencilRole::ClipClear:
                    // Unconditional REPLACE with ref 0 over the clip bounds.
                    ds.stencilEnabled = true;
                    ds.stencilReadMask = 0xFF;
                    ds.stencilWriteMask = 0xFF;
                    ds.stencilFront = rhi::StencilFaceState{rhi::CompareFunction::Always,
                                                            rhi::StencilOperation::Keep,
                                                            rhi::StencilOperation::Keep,
                                                            rhi::StencilOperation::Replace};
                    ds.stencilBack = ds.stencilFront;
                    break;
                case StencilRole::None:
                    if (clipped)
                    {
                        // Ordinary color draw confined to the clip mask (read-only test).
                        ds.stencilEnabled = true;
                        ds.stencilReadMask = kClipBit;
                        ds.stencilWriteMask = 0;
                        ds.stencilFront = rhi::StencilFaceState{rhi::CompareFunction::Equal,
                                                                rhi::StencilOperation::Keep,
                                                                rhi::StencilOperation::Keep,
                                                                rhi::StencilOperation::Keep};
                        ds.stencilBack = ds.stencilFront;
                    }
                    break;
                }
                desc.depthStencil = ds;
            }

            return m_device->CreateRenderPipeline(desc, outPipeline);
        }

        Status CreatePerFrameResources()
        {
            m_vertexBuffers.Resize(static_cast<usize>(m_frameCount));
            m_indexBuffers.Resize(static_cast<usize>(m_frameCount));
            m_uniformBuffers.Resize(static_cast<usize>(m_frameCount));

            for (i32 i = 0; i < m_frameCount; ++i)
            {
                rhi::BufferDesc vd{};
                vd.size = static_cast<u64>(MaxVertices) * sizeof(VGRenderVertex);
                vd.usage = rhi::BufferUsage::Vertex;
                vd.memory = rhi::MemoryLocation::CpuToGpu;
                if (!m_device->CreateBuffer(vd, m_vertexBuffers[static_cast<usize>(i)]).IsOk())
                    return ErrorCode::Unknown;

                rhi::BufferDesc id{};
                id.size = static_cast<u64>(MaxIndices) * sizeof(u32);
                id.usage = rhi::BufferUsage::Index;
                id.memory = rhi::MemoryLocation::CpuToGpu;
                if (!m_device->CreateBuffer(id, m_indexBuffers[static_cast<usize>(i)]).IsOk())
                    return ErrorCode::Unknown;

                rhi::BufferDesc ud{};
                ud.size = static_cast<u64>(MaxUniformSlots) * UniformSlotSize;
                ud.usage = rhi::BufferUsage::Uniform;
                ud.memory = rhi::MemoryLocation::CpuToGpu;
                if (!m_device->CreateBuffer(ud, m_uniformBuffers[static_cast<usize>(i)]).IsOk())
                    return ErrorCode::Unknown;
            }
            return ErrorCode::Ok;
        }

        CachedTexture* GetOrCreateCachedTexture(const image::ImageData* texture)
        {
            if (texture == nullptr)
                return nullptr;

            for (usize i = 0; i < m_textureCache.Size(); ++i)
            {
                if (m_textureCache[i]->source != texture)
                    continue;
                if (m_textureCache[i]->sourceId == texture->InstanceId())
                    return m_textureCache[i].Get();
                // Same address, different instance: the cached image was deleted and the
                // allocator reused its address. Retire the stale entry (GPU resources age
                // out with the in-flight frames) and build a fresh one below.
                if (m_textureCache[i]->external)
                    return nullptr; // owner must re-register the external view
                RetiredTexture retired;
                retired.entry = Move(m_textureCache[i]);
                retired.framesLeft = m_frameCount;
                m_retiredTextures.PushBack(Move(retired));
                m_textureCache.RemoveAt(i);
                break;
            }

            const Span<const u8> pixels = texture->PixelData();
            if (pixels.Size() == 0)
                return nullptr;

            const u32 w = texture->Width();
            const u32 h = texture->Height();
            const rhi::TextureFormat fmt = draconic::texture::TextureFormatUtils::Convert(
                texture->Format(), texture->ColorSpace());

            rhi::TextureDesc td{};
            td.dimension = rhi::TextureDimension::Texture2D;
            td.format = fmt;
            td.width = w;
            td.height = h;
            td.depth = 1;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = u8"VGRenderer cached texture";

            rhi::Texture* gpuTexture = nullptr;
            if (!m_device->CreateTexture(td, gpuTexture).IsOk())
                return nullptr;

            if (m_queue != nullptr)
            {
                rhi::TransferBatch* batch = nullptr;
                if (m_queue->CreateTransferBatch(batch).IsOk() && batch != nullptr)
                {
                    rhi::TextureDataLayout layout{};
                    layout.bytesPerRow = w * image::BytesPerPixel(texture->Format());
                    layout.rowsPerImage = h;
                    batch->WriteTexture(gpuTexture, pixels, layout, rhi::Extent3D{w, h, 1});
                    (void)batch->Submit();
                    m_queue->DestroyTransferBatch(batch);
                }
            }

            rhi::TextureViewDesc vd{};
            vd.format = fmt;
            rhi::TextureView* view = nullptr;
            if (!m_device->CreateTextureView(gpuTexture, vd, view).IsOk())
            {
                m_device->DestroyTexture(gpuTexture);
                return nullptr;
            }

            UniquePtr<CachedTexture> cached = MakeUnique<CachedTexture>(DefaultAllocator());
            cached->source = texture;
            cached->sourceId = texture->InstanceId();
            cached->gpuTexture = gpuTexture;
            cached->view = view;
            cached->bindGroups.Resize(static_cast<usize>(m_frameCount) *
                                      kSpreadCount); // nullptr-filled
            CachedTexture* raw = cached.Get();
            m_textureCache.PushBack(Move(cached));
            return raw;
        }

        void UpdateTextureBindGroup(i32 textureIndex, i32 frameIndex,
                                    vg::VGGradientSpread spread = vg::VGGradientSpread::Pad)
        {
            if (textureIndex >= static_cast<i32>(m_batchTextures.Size()))
                return;
            const image::ImageData* texture = m_batchTextures[static_cast<usize>(textureIndex)];
            if (texture == nullptr)
                return;

            CachedTexture* cached = GetOrCreateCachedTexture(texture);
            if (cached == nullptr || cached->view == nullptr)
                return;
            const usize slot =
                static_cast<usize>(frameIndex) * kSpreadCount + static_cast<usize>(spread);
            if (cached->bindGroups[slot] != nullptr)
                return; // already built

            rhi::BindGroupEntry entries[3];
            entries[0] = rhi::BindGroupEntry::BufferEntry(
                m_uniformBuffers[static_cast<usize>(frameIndex)], 0, sizeof(VGUniforms));
            entries[1] = rhi::BindGroupEntry::TextureEntry(cached->view);
            entries[2] = rhi::BindGroupEntry::SamplerEntry(SamplerForSpread(spread));

            rhi::BindGroupDesc desc{};
            desc.layout = m_bindGroupLayout;
            desc.entries = Span<const rhi::BindGroupEntry>(entries, 3);
            rhi::BindGroup* group = nullptr;
            if (m_device->CreateBindGroup(desc, group).IsOk())
                cached->bindGroups[slot] = group;
        }

        rhi::BindGroup* GetBindGroupForTexture(i32 textureIndex, i32 frameIndex,
                                               vg::VGGradientSpread spread)
        {
            if (m_batchTextures.IsEmpty())
                return nullptr;
            const i32 effectiveIndex =
                (textureIndex < 0) ? 0 : textureIndex; // solid draws -> white at 0
            if (effectiveIndex >= static_cast<i32>(m_batchTextures.Size()))
                return nullptr;

            const image::ImageData* texture = m_batchTextures[static_cast<usize>(effectiveIndex)];
            if (texture == nullptr)
                return nullptr;

            // Pad groups are built eagerly in Prepare; the (rarer) repeat/mirror groups
            // build on first use (a device call - legal while the pass records).
            UpdateTextureBindGroup(effectiveIndex, frameIndex, spread);
            const usize slot =
                static_cast<usize>(frameIndex) * kSpreadCount + static_cast<usize>(spread);
            for (usize i = 0; i < m_textureCache.Size(); ++i)
                if (m_textureCache[i]->source == texture)
                    return m_textureCache[i]->bindGroups[slot];
            return nullptr;
        }

        void DisposeCachedTexture(CachedTexture& cached)
        {
            for (usize i = 0; i < cached.bindGroups.Size(); ++i)
                if (cached.bindGroups[i] != nullptr)
                    m_device->DestroyBindGroup(cached.bindGroups[i]);
            if (cached.external)
                return; // view/texture are caller-owned
            if (cached.view)
                m_device->DestroyTextureView(cached.view);
            if (cached.gpuTexture)
                m_device->DestroyTexture(cached.gpuTexture);
        }

        void DestroyBuffers(Array<rhi::Buffer*>& buffers)
        {
            for (usize i = 0; i < buffers.Size(); ++i)
                if (buffers[i] != nullptr)
                    m_device->DestroyBuffer(buffers[i]);
            buffers.Clear();
        }

        rhi::Device* m_device = nullptr;
        rhi::Queue* m_queue = nullptr;
        i32 m_frameCount = 0;
        rhi::TextureFormat m_targetFormat = rhi::TextureFormat::BGRA8UnormSrgb;

        rhi::BindGroupLayout* m_bindGroupLayout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        rhi::RenderPipeline* m_dfPipeline = nullptr;         // MSDF fragment variant (null if unused)
        VGTargetConfig m_targetConfig{}; // host target: sample count + DS format
        rhi::RenderPipeline* m_stencilWriteNonZero = nullptr; // winding pass (nullable)
        rhi::RenderPipeline* m_stencilWriteEvenOdd = nullptr; // parity pass (nullable)
        rhi::RenderPipeline* m_coverPipeline = nullptr;       // cover, default shading (nullable)
        rhi::RenderPipeline* m_coverGradRadialPipeline = nullptr; // cover, radial (nullable)
        rhi::RenderPipeline* m_coverGradConicPipeline = nullptr;  // cover, conic (nullable)
        rhi::RenderPipeline* m_gradRadialPipeline = nullptr; // per-pixel radial gradient (nullable)
        rhi::RenderPipeline* m_gradConicPipeline = nullptr;  // per-pixel conic gradient (nullable)
        rhi::ShaderModule* m_vsModule = nullptr; // borrowed (lazy blend variants)
        rhi::ShaderModule* m_fsModule = nullptr;
        rhi::ShaderModule* m_dfModule = nullptr;
        rhi::ShaderModule* m_gradRadialModule = nullptr;
        rhi::ShaderModule* m_gradConicModule = nullptr;
        rhi::RenderPipeline* m_blendPipelines[3][7] = {}; // [blend-1][PipelineKind] (unclipped)
        rhi::RenderPipeline* m_clippedPipelines[4][7] = {}; // [blend][PipelineKind], lazy
        rhi::RenderPipeline* m_clippedWriteNonZero = nullptr; // lazy clipped write pair
        rhi::RenderPipeline* m_clippedWriteEvenOdd = nullptr;
        rhi::RenderPipeline* m_clipApplyPipeline = nullptr; // winding -> clip mask
        rhi::RenderPipeline* m_clipClearPipeline = nullptr; // clip mask eraser
        rhi::Sampler* m_sampler = nullptr; // clamp: images + pad gradients
        rhi::Sampler* m_samplerRepeat = nullptr;
        rhi::Sampler* m_samplerMirror = nullptr;

        Array<rhi::Buffer*> m_vertexBuffers;
        Array<rhi::Buffer*> m_indexBuffers;
        Array<rhi::Buffer*> m_uniformBuffers;

        // Evicted entries wait here until every in-flight frame has aged past them.
        struct RetiredTexture
        {
            UniquePtr<CachedTexture> entry;
            i32 framesLeft = 0;
        };

        Array<UniquePtr<CachedTexture>> m_textureCache;
        Array<RetiredTexture> m_retiredTextures;
        Array<const image::ImageData*> m_batchTextures;
        Array<draconic::vg::VGCommand> m_drawCommands;

        Array<u32> m_frameVertexOffsets;
        Array<u32> m_frameIndexOffsets;
        Array<u32> m_frameUniformSlotCount;

        bool m_initialized = false;
    };
}
