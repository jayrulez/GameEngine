// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG.Renderer::Renderer - :renderer partition.
//
// VGRenderer: draws VGContext/VGBatch content through the RHI. Owns per-frame
// vertex/index/uniform ring buffers (byte-offset sub-allocated across slices),
// the pipeline, and an on-demand ImageData->GPU texture cache. Ported from
// Sedulous.VG.Renderer/VGRenderer.bf.
//
// Deviations from Sedulous (deliberate, documented):
//   * Initialize takes the two pre-compiled rhi::ShaderModule* (vert, frag)
//     rather than a ShaderSystem - shader compilation (DXC) is the caller's job
//     via foundation.shaders, keeping this lib's dependency to pure RHI.
//   * Per-renderer external textures ARE supported (RegisterExternalTexture /
//     UnregisterExternalTexture) so a caller-owned rhi::TextureView - e.g. a
//     ui::viewport offscreen render target - can be sampled via DrawImage. The
//     SHARED cross-renderer external texture cache (VGExternalTextureCache) is
//     still omitted (a multi-renderer/multi-window sharing optimisation); add
//     later if a single RT must be sampled by more than one window's renderer.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module foundation.vg.renderer:renderer;

import foundation.core;
import foundation.rhi;
import foundation.image;
import foundation.texture;
import foundation.vg;
import :vertex;

using namespace foundation::core;
namespace vg = foundation::vg;

export namespace foundation::vg::renderer
{
    /// The stencil-capable depth-stencil format a device accepts at the given sample
    /// count, probed with a tiny texture (Vulkan drivers commonly support only one of
    /// D24S8 / D32S8). Undefined = none - the host skips the stencil config.
    [[nodiscard]] inline foundation::rhi::TextureFormat
    PickStencilCapableFormat(foundation::rhi::Device& device, foundation::core::u32 sampleCount)
    {
        namespace rhi = foundation::rhi;
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
        foundation::core::u32 sampleCount = 1;
        foundation::rhi::TextureFormat depthStencilFormat = foundation::rhi::TextureFormat::Undefined;
        /// Encoded: the target format given to Initialize is the UNORM twin of an sRGB target (a
        /// view of it), and the shaders keep colours encoded (VGBlendSpace).
        foundation::vg::VGBlendSpace blendSpace = foundation::vg::VGBlendSpace::Linear;
    };

    namespace rhi = foundation::rhi;
    namespace image = foundation::image;

    /// Projection uniform (one per slice, padded to UniformSlotSize on the GPU). The DF fields
    /// carry the MSDF spread + atlas size for the distance-field fragment shader's screen-space
    /// AA; they are ignored by the default pipeline.
    struct VGUniforms
    {
        Float4x4 projection = Float4x4::Identity();
        f32 distanceFieldPixelRange = 4.0f;
        f32 distanceFieldAtlasWidth = 512.0f;
        f32 distanceFieldAtlasHeight = 512.0f;
        f32 blendSpace = 0.0f; // 1: VGBlendSpace::Encoded (the shaders keep colours encoded)
    };

    /// One blur pass's uniform (vg_blur.ps's VGBlur), in a VGUniforms-sized slot.
    struct VGBlurUniforms
    {
        f32 step[2] = {0.0f, 0.0f};   // one texel along the pass's axis, in texture coordinates
        f32 region[2] = {1.0f, 1.0f}; // the content's extent in texture coordinates
        f32 sigma = 1.0f;
        f32 radius = 0.0f; // taps each side
        f32 unused[2] = {0.0f, 0.0f};
    };

    /// A handle to one batch's data inside the shared frame buffers. Returned by
    /// Prepare, consumed by Render. Invalid slices are no-ops.
    struct VGRenderSlice
    {
        u32 vertexByteOffset = 0;
        u32 boxVertexByteOffset = 0; // the slice's analytic boxes, after its ordinary vertices
        u32 indexByteOffset = 0;
        u32 uniformByteOffset = 0;
        i32 drawCommandStart = 0;
        i32 drawCommandCount = 0;
        i32 layerStart = 0; // the slice's offscreen layers (VGBatch::layers), drawn by RenderLayers
        i32 layerCount = 0;
        bool isValid = false;
    };

    /// Renders VGBatch content via the RHI (alpha-blended, analytical-AA).
    /// Does not own the device/swapchain.
    class VGRenderer
    {
    public:
        // The allocator (required - the owner decides) backs the texture cache.
        explicit VGRenderer(IAllocator& allocator) noexcept : m_allocator(&allocator) {}
        ~VGRenderer() { Dispose(); }
        VGRenderer(const VGRenderer&) = delete;
        VGRenderer& operator=(const VGRenderer&) = delete;

        [[nodiscard]] bool IsInitialized() const { return m_initialized; }

        /// What the last Render DID with its commands: dispatched, or SKIPPED because the
        /// pipeline the command needs was never built (a stale batch must not draw winding
        /// fans as visible colour). Observable headlessly, so the dispatch decisions are
        /// testable on the Null backend without a pixel probe.
        struct RenderStats
        {
            i32 drawn = 0;
            i32 skipped = 0;
            i32 layers = 0; // offscreen layers drawn (RenderLayers)
        };
        [[nodiscard]] RenderStats LastRenderStats() const { return m_lastRenderStats; }
        /// What the last RenderLayers did: the layers it drew and their commands.
        [[nodiscard]] RenderStats LastLayerStats() const { return m_lastLayerStats; }

        /// True when Initialize was given a stencil-capable target (hosts gate
        /// VGContext::SetStencilFills on this).
        [[nodiscard]] bool StencilFillsSupported() const { return m_coverPipeline != nullptr; }

        /// Initialize with a device + the (already compiled) vg vertex/fragment
        /// shader modules + the render-target format + frame count.
        Status Initialize(rhi::Device& device, rhi::ShaderModule& vertShader,
                          rhi::ShaderModule& fragShader, rhi::TextureFormat targetFormat,
                          i32 frameCount, rhi::ShaderModule* distanceFieldFragmentShader = nullptr,
                          rhi::ShaderModule* gradRadialFragShader = nullptr,
                          rhi::ShaderModule* gradConicFragShader = nullptr,
                          VGTargetConfig targetConfig = {},
                          rhi::ShaderModule* boxShadowFragShader = nullptr,
                          rhi::ShaderModule* boxVertShader = nullptr,
                          rhi::ShaderModule* boxFragShader = nullptr,
                          rhi::ShaderModule* layerFragShader = nullptr,
                          rhi::ShaderModule* blurVertShader = nullptr,
                          rhi::ShaderModule* blurFragShader = nullptr)
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
            m_distanceFieldModule = distanceFieldFragmentShader;
            m_gradRadialModule = gradRadialFragShader;
            m_gradConicModule = gradConicFragShader;
            m_boxShadowModule = boxShadowFragShader;
            m_boxVsModule = boxVertShader;
            m_boxFsModule = boxFragShader;
            m_layerFsModule = layerFragShader;

            if (!CreateSampler().IsOk())
                return ErrorCode::Unknown;
            if (!CreateLayouts().IsOk())
                return ErrorCode::Unknown;
            if (!CreatePipelineInto(vertShader, fragShader, m_pipeline).IsOk())
                return ErrorCode::Unknown;
            // Optional distance-field pipeline (same layout/vertex format, MSDF fragment shader).
            if (distanceFieldFragmentShader != nullptr &&
                !CreatePipelineInto(vertShader, *distanceFieldFragmentShader, m_distanceFieldPipeline).IsOk())
                return ErrorCode::Unknown;
            // Optional per-pixel radial/conic gradient pipelines (same layout; the fragment shader
            // derives the gradient parameter per pixel from the emitted gradient-space texcoord).
            if (gradRadialFragShader != nullptr &&
                !CreatePipelineInto(vertShader, *gradRadialFragShader, m_gradRadialPipeline).IsOk())
                return ErrorCode::Unknown;
            if (gradConicFragShader != nullptr &&
                !CreatePipelineInto(vertShader, *gradConicFragShader, m_gradConicPipeline).IsOk())
                return ErrorCode::Unknown;
            // Optional box-shadow pipeline (the UI's blurred rounded rects; direct fills only).
            if (boxShadowFragShader != nullptr &&
                !CreatePipelineInto(vertShader, *boxShadowFragShader, m_boxShadowPipeline).IsOk())
                return ErrorCode::Unknown;
            // Optional analytic-box pipeline (VGContext::DrawBox): its own vertex layout and shaders.
            if (boxVertShader != nullptr && boxFragShader != nullptr &&
                !CreatePipelineVariant(*boxVertShader, *boxFragShader, StencilRole::None, m_boxPipeline,
                                       vg::VGBlendMode::Normal, false, /*box*/ true)
                     .IsOk())
                return ErrorCode::Unknown;
            // Optional layer composite pipeline (VGContext::PushLayer): samples a premultiplied target.
            if (layerFragShader != nullptr &&
                !CreatePipelineInto(vertShader, *layerFragShader, m_layerPipeline).IsOk())
                return ErrorCode::Unknown;
            // Optional layer blur (a layer's `blur`): one axis of a Gaussian over its target.
            if (blurVertShader != nullptr && blurFragShader != nullptr &&
                !CreateBlurPipeline(*blurVertShader, *blurFragShader).IsOk())
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
            // Apply any pending capacity growth to this slot now that it is free for reuse.
            GrowFrameBuffers(frameIndex);
            m_drawCommands.Clear();
            m_batchTextures.Clear();
            m_layers.Clear();
            ReleaseLayerTargets(frameIndex);
        }

        /// Upload one batch into the shared frame buffers; returns a slice token.
        VGRenderSlice Prepare(foundation::vg::VGBatch& batch, i32 frameIndex, u32 width, u32 height)
        {
            // The batch's eviction list is the invalidation signal for the identity-keyed
            // texture cache (the producer freed/recycled those sources - e.g. gradient-LUT
            // cache eviction). Process it even when the batch draws nothing, and BEFORE any
            // upload: a recycled allocation may reuse an evicted address this very frame.
            for (usize i = 0; i < batch.evictedTextures.Size(); ++i)
                EvictCachedTexture(batch.evictedTextures[i]);

            const u32 vertCountIn = static_cast<u32>(batch.vertices.Size());
            const u32 idxCountIn = static_cast<u32>(batch.indices.Size());
            const u32 boxCountIn = static_cast<u32>(batch.boxVertices.Size());
            if (batch.commands.IsEmpty() || ((vertCountIn == 0 || idxCountIn == 0) && boxCountIn == 0))
                return VGRenderSlice{};

            // The slice's boxes follow its ordinary vertices in the same buffer.
            const u32 boxByteSize = boxCountIn * static_cast<u32>(sizeof(foundation::vg::VGBoxVertex));
            const u32 vertByteSize = vertCountIn * static_cast<u32>(sizeof(VGRenderVertex));
            const u32 idxByteSize = idxCountIn * static_cast<u32>(sizeof(u32));
            const u32 sliceVertOffset = m_frameVertexOffsets[static_cast<usize>(frameIndex)];
            const u32 sliceIdxOffset = m_frameIndexOffsets[static_cast<usize>(frameIndex)];
            const u32 sliceUniformSlot = m_frameUniformSlotCount[static_cast<usize>(frameIndex)];

            // Uniform slots stay a hard cap (one per slice and one per layer, MaxUniformSlots a frame).
            if (sliceUniformSlot >= static_cast<u32>(MaxUniformSlots))
                return VGRenderSlice{};
            const u32 curVertCap =
                static_cast<u32>(m_frameVertexCapacity[static_cast<usize>(frameIndex)]);
            const u32 maxVertBytes = curVertCap * static_cast<u32>(sizeof(VGRenderVertex));
            const u32 maxIdxBytes = curVertCap * 3u * static_cast<u32>(sizeof(u32));
            if (sliceVertOffset + vertByteSize + boxByteSize > maxVertBytes ||
                sliceIdxOffset + idxByteSize > maxIdxBytes)
            {
                // The per-frame buffers are too small for this frame's accumulated geometry. Bump the
                // target (grown at the next BeginFrame, when the slot is free) and skip this slice.
                // One frame may blank during a growth step, then it renders - vs the old behavior of
                // blanking the whole surface for as long as the large draw list persists.
                const u32 neededVerts =
                    (sliceVertOffset + vertByteSize + boxByteSize) / static_cast<u32>(sizeof(VGRenderVertex)) + 1u;
                if (neededVerts > static_cast<u32>(MaxVertexCapacity))
                {
                    if (!m_capacityWarned)
                    {
                        m_capacityWarned = true;
                        LOG_WARNING(u8"VG", u8"UI draw list needs {} vertices, past the {} ceiling - "
                                            u8"the surface will not fully render this frame",
                                    neededVerts, static_cast<u32>(MaxVertexCapacity));
                    }
                    return VGRenderSlice{};
                }
                i32 want = m_targetVertexCapacity;
                while (static_cast<u32>(want) < neededVerts && want < MaxVertexCapacity)
                    want *= 2;
                if (want > m_targetVertexCapacity)
                    m_targetVertexCapacity = want;
                return VGRenderSlice{};
            }

            const u32 sliceUniformOffset = sliceUniformSlot * static_cast<u32>(UniformSlotSize);
            const i32 sliceCmdStart = static_cast<i32>(m_drawCommands.Size());
            const i32 textureBase = static_cast<i32>(m_batchTextures.Size());

            // Convert + upload this slice's vertices.
            Array<VGRenderVertex> renderVerts;
            renderVerts.Reserve(batch.vertices.Size());
            for (usize i = 0; i < batch.vertices.Size(); ++i)
                renderVerts.PushBack(VGRenderVertex(batch.vertices[i]));

            // The slice's layers: each kept to its parent (the canvas for the outermost), given a
            // uniform slot of its own (its projection over its bounds) and a target size. Parents go
            // first, by nesting depth: a backdrop blur makes a layer the parent of earlier ones.
            const i32 layerBase = static_cast<i32>(m_layers.Size());
            u32 nextUniformSlot = sliceUniformSlot + 1;
            m_layers.Resize(m_layers.Size() + batch.layers.Size());
            Array<i32> order;
            order.Reserve(batch.layers.Size());
            for (usize l = 0; l < batch.layers.Size(); ++l)
            {
                i32 depth = 0;
                for (i32 p = batch.layers[l].parent; p >= 0 && depth <= static_cast<i32>(batch.layers.Size());
                     p = batch.layers[static_cast<usize>(p)].parent)
                    ++depth;
                m_layers[static_cast<usize>(layerBase) + l].depth = depth;
                order.PushBack(static_cast<i32>(l));
            }
            for (usize i = 1; i < order.Size(); ++i) // stable insertion sort by depth (layers are few)
                for (usize j = i; j > 0 && m_layers[static_cast<usize>(layerBase + order[j - 1])].depth >
                                               m_layers[static_cast<usize>(layerBase + order[j])].depth;
                     --j)
                    Swap(order[j - 1], order[j]);
            for (const i32 l : order)
            {
                const vg::VGLayer& source = batch.layers[static_cast<usize>(l)];
                PreparedLayer& layer = m_layers[static_cast<usize>(layerBase + l)];
                layer.parent = source.parent >= 0 ? source.parent + layerBase : -1;
                Rectangle outer{0.0f, 0.0f, static_cast<f32>(width), static_cast<f32>(height)};
                bool parentValid = true;
                if (layer.parent >= 0)
                {
                    outer = m_layers[static_cast<usize>(layer.parent)].bounds;
                    parentValid = m_layers[static_cast<usize>(layer.parent)].valid;
                }
                const f32 x0 = Max(source.bounds.x, outer.x);
                const f32 y0 = Max(source.bounds.y, outer.y);
                const f32 x1 = Min(source.bounds.x + source.bounds.width, outer.x + outer.width);
                const f32 y1 = Min(source.bounds.y + source.bounds.height, outer.y + outer.height);
                layer.bounds = Rectangle{x0, y0, Max(0.0f, x1 - x0), Max(0.0f, y1 - y0)};
                layer.width = static_cast<u32>(layer.bounds.width);
                layer.height = static_cast<u32>(layer.bounds.height);
                layer.targetWidth = RoundUpToBucket(layer.width);
                layer.targetHeight = RoundUpToBucket(layer.height);
                // A blur (in place, or a backdrop's copy) takes two more slots, across then down;
                // without its pipeline, none.
                layer.blur = m_blurPipeline != nullptr ? source.blur : 0.0f;
                layer.backdropBlur = m_blurPipeline != nullptr ? source.backdropBlur : 0.0f;
                const bool blurs = layer.blur > 0.0f || layer.backdropBlur > 0.0f;
                const u32 slots = blurs ? 3u : 1u;
                layer.valid = parentValid && layer.width > 0 && layer.height > 0 &&
                              nextUniformSlot + slots <= static_cast<u32>(MaxUniformSlots);
                if (layer.valid)
                {
                    layer.uniformOffset = nextUniformSlot * static_cast<u32>(UniformSlotSize);
                    ++nextUniformSlot;
                    if (blurs)
                    {
                        layer.blurUniformOffset = nextUniformSlot * static_cast<u32>(UniformSlotSize);
                        nextUniformSlot += 2;
                    }
                }
            }
            // A composite samples its layer's target where its geometry lies: texcoords from
            // positions, over the target's (bucketed) size.
            for (usize i = 0; i < batch.commands.Size(); ++i)
            {
                const vg::VGCommand& cmd = batch.commands[i];
                if (cmd.drawMode != vg::VGDrawMode::Layer || cmd.compositeLayer < 0)
                    continue;
                const PreparedLayer& layer = m_layers[static_cast<usize>(cmd.compositeLayer + layerBase)];
                if (!layer.valid)
                    continue; // the composite is skipped at Render
                for (i32 k = 0; k < cmd.indexCount; ++k)
                {
                    const u32 at = batch.indices[static_cast<usize>(cmd.startIndex + k)];
                    if (at >= renderVerts.Size())
                        continue;
                    VGRenderVertex& vertex = renderVerts[at];
                    vertex.texCoord[0] = (vertex.position[0] - layer.bounds.x) / static_cast<f32>(layer.targetWidth);
                    vertex.texCoord[1] = (vertex.position[1] - layer.bounds.y) / static_cast<f32>(layer.targetHeight);
                }
            }
            if (vertByteSize > 0)
                WriteBuffer(m_vertexBuffers[static_cast<usize>(frameIndex)], sliceVertOffset,
                            renderVerts.Data(), vertByteSize);
            if (boxByteSize > 0)
                WriteBuffer(m_vertexBuffers[static_cast<usize>(frameIndex)], sliceVertOffset + vertByteSize,
                            batch.boxVertices.Data(), boxByteSize);

            // Upload indices verbatim (relative to the slice's vertex base).
            if (idxByteSize > 0)
                WriteBuffer(m_indexBuffers[static_cast<usize>(frameIndex)], sliceIdxOffset,
                            batch.indices.Data(), idxByteSize);

            // Append textures (commands index into the shared batch-texture list).
            for (usize i = 0; i < batch.textures.Size(); ++i)
                m_batchTextures.PushBack(batch.textures[i]);
            for (usize i = 0; i < batch.commands.Size(); ++i)
            {
                foundation::vg::VGCommand cmd = batch.commands[i];
                if (cmd.textureIndex >= 0)
                    cmd.textureIndex = cmd.textureIndex + textureBase;
                if (cmd.layer >= 0)
                    cmd.layer += layerBase;
                if (cmd.compositeLayer >= 0)
                    cmd.compositeLayer += layerBase;
                m_drawCommands.PushBack(cmd);
            }

            // Write this slice's projection + distance-field metadata into its uniform slot.
            VGUniforms uniforms;
            uniforms.projection = OrthoOffCenter(static_cast<f32>(width), static_cast<f32>(height));
            uniforms.distanceFieldPixelRange = batch.distanceFieldPixelRange;
            uniforms.distanceFieldAtlasWidth = batch.distanceFieldAtlasWidth;
            uniforms.distanceFieldAtlasHeight = batch.distanceFieldAtlasHeight;
            uniforms.blendSpace = m_targetConfig.blendSpace == vg::VGBlendSpace::Encoded ? 1.0f : 0.0f;
            WriteBuffer(m_uniformBuffers[static_cast<usize>(frameIndex)], sliceUniformOffset,
                        &uniforms, sizeof(VGUniforms));
            for (usize l = static_cast<usize>(layerBase); l < m_layers.Size(); ++l)
            {
                const PreparedLayer& layer = m_layers[l];
                if (!layer.valid)
                    continue;
                uniforms.projection = OrthoOffCenter(layer.bounds.x, layer.bounds.y, static_cast<f32>(layer.width),
                                                     static_cast<f32>(layer.height));
                WriteBuffer(m_uniformBuffers[static_cast<usize>(frameIndex)], layer.uniformOffset, &uniforms,
                            sizeof(VGUniforms));
                if (layer.blur > 0.0f || layer.backdropBlur > 0.0f)
                {
                    VGBlurUniforms blur;
                    blur.region[0] = static_cast<f32>(layer.width) / static_cast<f32>(layer.targetWidth);
                    blur.region[1] = static_cast<f32>(layer.height) / static_cast<f32>(layer.targetHeight);
                    blur.sigma = Max(layer.blur, layer.backdropBlur);
                    blur.radius = Ceil(3.0f * blur.sigma);
                    blur.step[0] = 1.0f / static_cast<f32>(layer.targetWidth); // across
                    WriteBuffer(m_uniformBuffers[static_cast<usize>(frameIndex)], layer.blurUniformOffset, &blur,
                                sizeof(VGBlurUniforms));
                    blur.step[0] = 0.0f;
                    blur.step[1] = 1.0f / static_cast<f32>(layer.targetHeight); // down
                    WriteBuffer(m_uniformBuffers[static_cast<usize>(frameIndex)],
                                layer.blurUniformOffset + static_cast<u32>(UniformSlotSize), &blur,
                                sizeof(VGBlurUniforms));
                }
            }

            // Bind groups for any newly-added textures.
            for (i32 texIdx = textureBase; texIdx < static_cast<i32>(m_batchTextures.Size());
                 ++texIdx)
                UpdateTextureBindGroup(texIdx, frameIndex);

            m_frameVertexOffsets[static_cast<usize>(frameIndex)] = sliceVertOffset + vertByteSize + boxByteSize;
            m_frameIndexOffsets[static_cast<usize>(frameIndex)] = sliceIdxOffset + idxByteSize;
            m_frameUniformSlotCount[static_cast<usize>(frameIndex)] = nextUniformSlot;

            VGRenderSlice slice;
            slice.vertexByteOffset = sliceVertOffset;
            slice.boxVertexByteOffset = sliceVertOffset + vertByteSize;
            slice.indexByteOffset = sliceIdxOffset;
            slice.uniformByteOffset = sliceUniformOffset;
            slice.drawCommandStart = sliceCmdStart;
            slice.drawCommandCount = static_cast<i32>(m_drawCommands.Size()) - sliceCmdStart;
            slice.layerStart = layerBase;
            slice.layerCount = static_cast<i32>(m_layers.Size()) - layerBase;
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
            m_lastRenderStats = RenderStats{};
            if (!slice.isValid || slice.drawCommandCount == 0)
                return;
            DrawCommands(renderPass, frameIndex, slice, -1, viewportX, viewportY, width, height,
                         slice.uniformByteOffset, 0.0f, 0.0f, m_lastRenderStats);
        }

        /// Draw the slice's offscreen layers (VGContext::PushLayer) into targets of their own, each
        /// in a pass of its own, innermost first, so the slice's Render can composite them. Call it
        /// after Prepare and before the pass Render draws into (passes do not nest). A layer this
        /// does not draw (no target, or the layer composite shader was not given) has its
        /// composite skipped.
        void RenderLayers(rhi::CommandEncoder& encoder, i32 frameIndex, const VGRenderSlice& slice)
        {
            m_lastLayerStats = RenderStats{};
            if (!slice.isValid || slice.layerCount == 0 || m_layerPipeline == nullptr)
                return;
            // A parent draws its children's composites, so the deepest layers go first.
            Array<i32> order;
            order.Reserve(static_cast<usize>(slice.layerCount));
            for (i32 l = slice.layerStart; l < slice.layerStart + slice.layerCount; ++l)
                order.PushBack(l);
            for (usize i = 1; i < order.Size(); ++i)
                for (usize j = i; j > 0 && m_layers[static_cast<usize>(order[j - 1])].depth <
                                               m_layers[static_cast<usize>(order[j])].depth;
                     --j)
                    Swap(order[j - 1], order[j]);
            for (const i32 l : order)
            {
                PreparedLayer& layer = m_layers[static_cast<usize>(l)];
                if (!layer.valid)
                    continue;
                const i32 targetIndex = AcquireLayerTarget(frameIndex, layer.targetWidth, layer.targetHeight, false);
                if (targetIndex < 0)
                    continue;
                const LayerTarget& target = m_layerTargets[static_cast<usize>(targetIndex)];
                encoder.TransitionTexture(target.color, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::RenderTarget);
                if (target.msaa != nullptr)
                    encoder.TransitionTexture(target.msaa, rhi::ResourceState::Undefined,
                                              rhi::ResourceState::RenderTarget);
                if (target.depthStencil != nullptr)
                    encoder.TransitionTexture(target.depthStencil, rhi::ResourceState::Undefined,
                                              rhi::ResourceState::DepthStencilWrite);
                rhi::RenderPassDesc desc{};
                rhi::ColorAttachment color{};
                color.view = target.msaaView != nullptr ? target.msaaView : target.colorView;
                color.resolveTarget = target.msaaView != nullptr ? target.colorView : nullptr;
                color.loadOp = rhi::LoadOp::Clear;
                color.storeOp = target.msaaView != nullptr ? rhi::StoreOp::DontCare : rhi::StoreOp::Store;
                color.clearValue = rhi::ClearColor{0.0f, 0.0f, 0.0f, 0.0f};
                desc.colorAttachments.Add(color);
                if (target.depthStencilView != nullptr)
                {
                    rhi::DepthStencilAttachment ds{};
                    ds.view = target.depthStencilView;
                    ds.depthLoadOp = rhi::LoadOp::Clear;
                    ds.depthStoreOp = rhi::StoreOp::DontCare;
                    ds.stencilLoadOp = rhi::LoadOp::Clear;
                    ds.stencilStoreOp = rhi::StoreOp::DontCare;
                    ds.stencilClearValue = 0;
                    desc.depthStencilAttachment = ds;
                }
                desc.label = u8"VG layer";
                rhi::RenderPassEncoder* pass = encoder.BeginRenderPass(desc);
                if (pass == nullptr)
                    continue;
                DrawCommands(*pass, frameIndex, slice, l, 0, 0, layer.width, layer.height, layer.uniformOffset,
                             layer.bounds.x, layer.bounds.y, m_lastLayerStats);
                pass->End();
                encoder.TransitionTexture(target.color, rhi::ResourceState::RenderTarget,
                                          rhi::ResourceState::ShaderRead);
                if (layer.blur > 0.0f)
                    BlurLayer(encoder, frameIndex, layer, targetIndex, targetIndex);
                if (layer.backdropBlur > 0.0f)
                {
                    // A blurred copy for the backdrop shapes; the layer itself stays sharp.
                    const i32 copy = AcquireLayerTarget(frameIndex, layer.targetWidth, layer.targetHeight, true);
                    if (copy >= 0 && BlurLayer(encoder, frameIndex, layer, targetIndex, copy))
                        layer.backdropTarget = copy;
                }
                layer.target = targetIndex;
                ++m_lastLayerStats.layers;
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

            UniquePtr<CachedTexture> cached = MakeUnique<CachedTexture>(*m_allocator);
            cached->source = key;
            cached->sourceId = key->InstanceId();
            cached->view = view;
            cached->external = true;
            cached->bindGroups.Resize(static_cast<usize>(m_frameCount) *
                                      kSpreadCount); // nullptr-filled, built lazily
            m_textureCache.PushBack(Move(cached));
        }

        /// Drop a registered external texture. Tears down its per-frame
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
            for (usize i = 0; i < m_layerTargets.Size(); ++i)
                DisposeLayerTarget(m_layerTargets[i]);
            m_layerTargets.Clear();
            m_layers.Clear();

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
            for (usize b = 0; b < kBlendVariantCount + 1; ++b)
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
            if (m_boxPipeline)
                m_device->DestroyRenderPipeline(m_boxPipeline);
            m_boxPipeline = nullptr;
            if (m_layerPipeline)
                m_device->DestroyRenderPipeline(m_layerPipeline);
            m_layerPipeline = nullptr;
            if (m_blurPipeline)
                m_device->DestroyRenderPipeline(m_blurPipeline);
            m_blurPipeline = nullptr;
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
            if (m_distanceFieldPipeline)
                m_device->DestroyRenderPipeline(m_distanceFieldPipeline);
            if (m_gradRadialPipeline)
                m_device->DestroyRenderPipeline(m_gradRadialPipeline);
            if (m_gradConicPipeline)
                m_device->DestroyRenderPipeline(m_gradConicPipeline);
            if (m_boxShadowPipeline)
                m_device->DestroyRenderPipeline(m_boxShadowPipeline);
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
            m_distanceFieldPipeline = nullptr;
            m_stencilWriteNonZero = nullptr;
            m_stencilWriteEvenOdd = nullptr;
            m_coverPipeline = nullptr;
            m_coverGradRadialPipeline = nullptr;
            m_coverGradConicPipeline = nullptr;
            m_gradRadialPipeline = nullptr;
            m_gradConicPipeline = nullptr;
            m_boxShadowPipeline = nullptr;
            m_pipelineLayout = nullptr;
            m_bindGroupLayout = nullptr;
            m_sampler = nullptr;
            m_samplerRepeat = nullptr;
            m_samplerMirror = nullptr;
            m_initialized = false;
            m_device = nullptr;
        }

    private:
        // Dispatch the slice's commands that draw into `layer` (-1: the pass's own target) into the
        // active pass: content coordinates offset by the origin (a layer's bounds) map to the
        // viewport, through the projection at `uniformOffset`.
        void DrawCommands(rhi::RenderPassEncoder& renderPass, i32 frameIndex, const VGRenderSlice& slice, i32 layer,
                          i32 viewportX, i32 viewportY, u32 width, u32 height, u32 uniformOffset, f32 originX,
                          f32 originY, RenderStats& stats)
        {
            renderPass.SetViewport(static_cast<f32>(viewportX), static_cast<f32>(viewportY),
                                   static_cast<f32>(width), static_cast<f32>(height), 0.0f, 1.0f);
            renderPass.SetPipeline(m_pipeline);
            renderPass.SetVertexBuffer(0, m_vertexBuffers[static_cast<usize>(frameIndex)],
                                       slice.vertexByteOffset);
            renderPass.SetIndexBuffer(m_indexBuffers[static_cast<usize>(frameIndex)],
                                      rhi::IndexFormat::UInt32, slice.indexByteOffset);

            const u32 dynOffsets[1] = {uniformOffset};
            i32 currentTextureIndex = -2; // sentinel forces first SetBindGroup
            vg::VGGradientSpread currentSpread = vg::VGGradientSpread::Pad;
            rhi::RenderPipeline* currentPipeline = m_pipeline;
            i32 currentStencilRef = -1; // sentinel: set on first stencil-relevant command
            bool boxesBound = false;    // the vertex buffer sits at the slice's boxes
            if (m_coverPipeline != nullptr)
            {
                renderPass.SetStencilReference(0); // cover tests NotEqual 0
                currentStencilRef = 0;
            }

            const i32 cmdEnd = slice.drawCommandStart + slice.drawCommandCount;
            for (i32 i = slice.drawCommandStart; i < cmdEnd; ++i)
            {
                const foundation::vg::VGCommand& cmd = m_drawCommands[static_cast<usize>(i)];
                if (cmd.indexCount == 0 || cmd.layer != layer)
                    continue;

                // A layer's composite samples the target RenderLayers drew it into; none, no draw.
                rhi::BindGroup* compositeGroup = nullptr;
                if (cmd.drawMode == foundation::vg::VGDrawMode::Layer)
                {
                    const bool known = cmd.compositeLayer >= 0 && cmd.compositeLayer < static_cast<i32>(m_layers.Size());
                    const i32 target = !known ? -1
                                       : cmd.backdrop ? m_layers[static_cast<usize>(cmd.compositeLayer)].backdropTarget
                                                      : m_layers[static_cast<usize>(cmd.compositeLayer)].target;
                    if (target < 0 || m_layerTargets[static_cast<usize>(target)].bindGroup == nullptr)
                    {
                        ++stats.skipped;
                        continue;
                    }
                    compositeGroup = m_layerTargets[static_cast<usize>(target)].bindGroup;
                }

                // Pipeline per (fill phase, draw mode); falls back to the default pipeline
                // when a variant was not built. Stencil-phase commands are SKIPPED entirely
                // without stencil pipelines (a context should not emit them then, but a
                // stale batch must not draw winding fans as visible color). A pipeline swap
                // forces a bind-group rebind.
                rhi::RenderPipeline* pipeline = PipelineFor(cmd);
                if (pipeline == nullptr)
                {
                    ++stats.skipped;
                    continue;
                }
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
                        cmd.clipMode == foundation::vg::VGClipMode::Stencil ||
                        cmd.fillPhase == foundation::vg::VGFillPhase::ClipApply;
                    const i32 wantedRef = wantsClipRef ? 0x80 : 0;
                    if (wantedRef != currentStencilRef)
                    {
                        renderPass.SetStencilReference(static_cast<u32>(wantedRef));
                        currentStencilRef = wantedRef;
                    }
                }

                if (compositeGroup != nullptr)
                {
                    renderPass.SetBindGroup(0, compositeGroup, Span<const u32>(dynOffsets, 1));
                    currentTextureIndex = -2; // the next texture draw binds its own again
                }
                else if (cmd.textureIndex != currentTextureIndex ||
                    cmd.gradientSpread != currentSpread)
                {
                    if (rhi::BindGroup* bindGroup =
                            GetBindGroupForTexture(cmd.textureIndex, frameIndex,
                                                   cmd.gradientSpread))
                        renderPass.SetBindGroup(0, bindGroup, Span<const u32>(dynOffsets, 1));
                    currentTextureIndex = cmd.textureIndex;
                    currentSpread = cmd.gradientSpread;
                }

                // Scissored: a rectangle clip alone, or one inside a path clip (with the stencil).
                const bool scissored = cmd.clipMode == foundation::vg::VGClipMode::Scissor ||
                                       (cmd.clipMode == foundation::vg::VGClipMode::Stencil && cmd.scissored);
                if (scissored && cmd.clipRect.width > 0.0f && cmd.clipRect.height > 0.0f)
                {
                    const Rectangle clip{cmd.clipRect.x - originX, cmd.clipRect.y - originY, cmd.clipRect.width,
                                         cmd.clipRect.height};
                    const ScissorRect scissor = ComputeScissor(clip, viewportX, viewportY, width, height);
                    renderPass.SetScissor(scissor.x, scissor.y, scissor.width, scissor.height);
                }
                else if (scissored)
                {
                    renderPass.SetScissor(0, 0, 0, 0); // empty clip hides everything
                }
                else
                {
                    renderPass.SetScissor(viewportX, viewportY, width, height);
                }

                // Analytic boxes draw unindexed from their own part of the vertex buffer.
                const bool box = cmd.drawMode == foundation::vg::VGDrawMode::Box;
                if (box != boxesBound)
                {
                    renderPass.SetVertexBuffer(0, m_vertexBuffers[static_cast<usize>(frameIndex)],
                                               box ? slice.boxVertexByteOffset : slice.vertexByteOffset);
                    boxesBound = box;
                }
                if (box)
                    renderPass.Draw(static_cast<u32>(cmd.indexCount), 1, static_cast<u32>(cmd.startIndex), 0);
                else
                    renderPass.DrawIndexed(static_cast<u32>(cmd.indexCount), 1,
                                           static_cast<u32>(cmd.startIndex), 0, 0);
                ++stats.drawn;
            }
        }

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

        // A layer as Prepare placed it: kept within its parent, with its projection's uniform slot.
        struct PreparedLayer
        {
            i32 parent = -1;
            Rectangle bounds{};     // device bounds, whole pixels
            u32 width = 0, height = 0;             // the content's size
            u32 targetWidth = 0, targetHeight = 0; // the target's (bucketed)
            u32 uniformOffset = 0;
            f32 blur = 0.0f;            // sigma, 0 for none (or no blur pipeline)
            f32 backdropBlur = 0.0f;    // a blurred copy's sigma, for backdrop composites
            u32 blurUniformOffset = 0;  // its two passes' slots, across then down
            i32 depth = 0;              // ancestors: parents draw after their children
            i32 target = -1; // m_layerTargets, set by RenderLayers
            i32 backdropTarget = -1; // the blurred copy, set by RenderLayers
            bool valid = false;
        };

        // An offscreen target layers draw into, owned by one frame slot (reused once that slot's
        // frame has finished on the GPU).
        struct LayerTarget
        {
            rhi::Texture* color = nullptr; // single-sampled, sampled by the composite
            rhi::TextureView* colorView = nullptr;
            rhi::Texture* msaa = nullptr; // when the renderer draws multisampled (resolved into color)
            rhi::TextureView* msaaView = nullptr;
            rhi::Texture* depthStencil = nullptr; // when the renderer's pipelines carry one
            rhi::TextureView* depthStencilView = nullptr;
            rhi::BindGroup* bindGroup = nullptr; // the frame's uniforms, colorView, clamp sampler
            u32 width = 0, height = 0;
            i32 frame = 0;
            i32 idleFrames = 0;
            bool inUse = false;
            bool plain = false; // colour alone (a blur's scratch)
        };

        static constexpr usize kSpreadCount = 3; // Pad / Repeat / Reflect bind-group slots
        // Stencil bit planes (must match VGFillPhase's contract): bit 7 = clip mask,
        // bits 0..6 = fill winding.
        static constexpr u8 kClipBit = 0x80;
        static constexpr u8 kWindingMask = 0x7F;

        static constexpr i32 MaxVertices = 131072; // INITIAL per-frame vertex capacity (grows)
        static constexpr i32 MaxIndices = 131072 * 3;
        // Vertex/index buffers grow on demand up to this ceiling, so a large UI draw list (e.g. a big
        // asset grid after a bulk drop) renders instead of blanking the whole surface. Index capacity
        // always tracks vertexCapacity * 3.
        static constexpr i32 MaxVertexCapacity = 131072 * 16; // ~2.1M verts (~64 MB/frame at 32 B/vert)
        static constexpr i32 MaxUniformSlots = 256; // a slice's slot, then one per layer
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

        // The projection of a layer: device content from (x, y) over width by height onto its target.
        static Float4x4 OrthoOffCenter(f32 x, f32 y, f32 width, f32 height)
        {
            Float4x4 m = OrthoOffCenter(width, height);
            m.m[3][0] = -1.0f - 2.0f * x / width;
            m.m[3][1] = 1.0f + 2.0f * y / height;
            return m;
        }

        // Layer targets come in sizes rounded up to a bucket, so a layer that changes size a little
        // (an animated panel) keeps its target; the composite samples just the content's part.
        static constexpr u32 kLayerTargetBucket = 64;
        // BeginFrames of a frame slot a layer target may go unused before it is freed.
        static constexpr i32 kLayerTargetIdleFrames = 120;
        static u32 RoundUpToBucket(u32 size)
        {
            return (size + kLayerTargetBucket - 1) / kLayerTargetBucket * kLayerTargetBucket;
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
        [[nodiscard]] rhi::RenderPipeline* PipelineFor(const foundation::vg::VGCommand& cmd)
        {
            const bool blended = cmd.blendMode != vg::VGBlendMode::Normal;
            const bool clipped = cmd.clipMode == vg::VGClipMode::Stencil;
            switch (cmd.fillPhase)
            {
            case foundation::vg::VGFillPhase::ClipApply:
                return m_clipApplyPipeline; // null = unconfigured, command skipped
            case foundation::vg::VGFillPhase::ClipClear:
                return m_clipClearPipeline;
            case foundation::vg::VGFillPhase::StencilWrite:
                // Color-masked winding accumulation: the blend state is irrelevant, the
                // clip state is not (a clipped fill only accumulates inside the mask).
                if (clipped)
                    return cmd.fillRule == foundation::vg::FillRule::EvenOdd
                               ? ClippedWrite(StencilRole::WriteEvenOdd, m_clippedWriteEvenOdd)
                               : ClippedWrite(StencilRole::WriteNonZero, m_clippedWriteNonZero);
                return cmd.fillRule == foundation::vg::FillRule::EvenOdd ? m_stencilWriteEvenOdd
                                                                       : m_stencilWriteNonZero;
            case foundation::vg::VGFillPhase::StencilCover:
                if (m_coverPipeline == nullptr)
                    return nullptr;
                if (cmd.drawMode == foundation::vg::VGDrawMode::GradientRadial &&
                    m_coverGradRadialPipeline != nullptr)
                    return (blended || clipped)
                               ? Variant(PipelineKind::CoverRadial, cmd.blendMode, clipped)
                               : m_coverGradRadialPipeline;
                if (cmd.drawMode == foundation::vg::VGDrawMode::GradientConic &&
                    m_coverGradConicPipeline != nullptr)
                    return (blended || clipped)
                               ? Variant(PipelineKind::CoverConic, cmd.blendMode, clipped)
                               : m_coverGradConicPipeline;
                return (blended || clipped) ? Variant(PipelineKind::Cover, cmd.blendMode, clipped)
                                            : m_coverPipeline;
            case foundation::vg::VGFillPhase::Direct:
                break;
            }
            if (cmd.drawMode == foundation::vg::VGDrawMode::DistanceField && m_distanceFieldPipeline != nullptr)
                return (blended || clipped)
                           ? Variant(PipelineKind::DistanceField, cmd.blendMode, clipped)
                           : m_distanceFieldPipeline;
            if (cmd.drawMode == foundation::vg::VGDrawMode::GradientRadial &&
                m_gradRadialPipeline != nullptr)
                return (blended || clipped)
                           ? Variant(PipelineKind::GradRadial, cmd.blendMode, clipped)
                           : m_gradRadialPipeline;
            if (cmd.drawMode == foundation::vg::VGDrawMode::GradientConic &&
                m_gradConicPipeline != nullptr)
                return (blended || clipped)
                           ? Variant(PipelineKind::GradConic, cmd.blendMode, clipped)
                           : m_gradConicPipeline;
            if (cmd.drawMode == foundation::vg::VGDrawMode::Box)
                // Box vertices are not VG vertices: no other pipeline can read them. SKIP when the
                // box shaders were not given (a context draws boxes analytically only when told).
                return m_boxPipeline == nullptr
                           ? nullptr
                           : ((blended || clipped) ? Variant(PipelineKind::Box, cmd.blendMode, clipped)
                                                   : m_boxPipeline);
            if (cmd.drawMode == foundation::vg::VGDrawMode::BoxShadow)
                // No substitute: the default shader would paint the quadrant quads as flat
                // colour over everything around the box. SKIP when the shader was not given.
                return m_boxShadowPipeline == nullptr
                           ? nullptr
                           : ((blended || clipped)
                                  ? Variant(PipelineKind::BoxShadow, cmd.blendMode, clipped)
                                  : m_boxShadowPipeline);
            if (cmd.drawMode == foundation::vg::VGDrawMode::Layer)
                // A layer target holds premultiplied colour; the default shader would multiply it
                // again. SKIP when the composite shader was not given.
                return m_layerPipeline == nullptr
                           ? nullptr
                           : ((blended || clipped) ? Variant(PipelineKind::Layer, cmd.blendMode, clipped)
                                                   : m_layerPipeline);
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
            BoxShadow,
            Box,
            Layer, // keep last: the tables below are sized by it
        };
        static constexpr usize kPipelineKindCount = static_cast<usize>(PipelineKind::Layer) + 1;
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
                frag = m_distanceFieldModule;
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
            case PipelineKind::BoxShadow:
                frag = m_boxShadowModule;
                break;
            case PipelineKind::Box:
                frag = m_boxFsModule;
                break;
            case PipelineKind::Layer:
                frag = m_layerFsModule;
                break;
            }
            const bool box = kind == PipelineKind::Box;
            rhi::ShaderModule* vert = box ? m_boxVsModule : m_vsModule;
            if (vert == nullptr || frag == nullptr ||
                (clipped && m_targetConfig.depthStencilFormat == rhi::TextureFormat::Undefined) ||
                !CreatePipelineVariant(*vert, *frag, role, slot, blendMode, clipped, box).IsOk())
            {
                slot = nullptr;
                // The default pipeline is a tolerable stand-in for a DF or gradient variant
                // (flat colour), never for a box shadow (it would paint the quadrant quads as
                // flat colour over everything around the box), a box (its vertices are not
                // VG vertices), nor a layer composite (it would multiply premultiplied colour again).
                return (clipped || kind == PipelineKind::BoxShadow || box || kind == PipelineKind::Layer)
                           ? nullptr
                           : m_pipeline;
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
                                     bool clipped = false, bool box = false)
        {
            const rhi::VertexAttribute attributes[5] = {
                {rhi::VertexFormat::Float32x2, 0, 0},  // position
                {rhi::VertexFormat::Float32x2, 8, 1},  // texCoord
                {rhi::VertexFormat::Float32x4, 16, 2}, // color
                {rhi::VertexFormat::Float32, 32, 3},   // coverage
                {rhi::VertexFormat::Float32x2, 36, 4}, // effect (distance-field text effects)
            };
            // An analytic box's vertex (foundation::vg::VGBoxVertex).
            const rhi::VertexAttribute boxAttributes[7] = {
                {rhi::VertexFormat::Float32x2, 0, 0},  // position
                {rhi::VertexFormat::Float32x2, 8, 1},  // local
                {rhi::VertexFormat::Float32x2, 16, 2}, // halfSize
                {rhi::VertexFormat::Float32x4, 24, 3}, // radii
                {rhi::VertexFormat::Float32x4, 40, 4}, // borders
                {rhi::VertexFormat::Float32x4, 56, 5}, // fill
                {rhi::VertexFormat::Float32x4, 72, 6}, // borderColor
            };
            rhi::VertexBufferLayout vbLayout{};
            vbLayout.stride = box ? static_cast<u32>(sizeof(foundation::vg::VGBoxVertex))
                                  : static_cast<u32>(sizeof(VGRenderVertex));
            vbLayout.attributes = box ? Span<const rhi::VertexAttribute>(boxAttributes, 7)
                                      : Span<const rhi::VertexAttribute>(attributes, 5);
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
                rhi::StageOf(&vertShader, rhi::ShaderStage::Vertex);
            desc.vertex.buffers = Span<const rhi::VertexBufferLayout>(vertexBuffers, 1);

            rhi::FragmentState fragment{};
            fragment.shader =
                rhi::StageOf(&fragShader, rhi::ShaderStage::Fragment);
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
            m_frameVertexCapacity.Resize(static_cast<usize>(m_frameCount));

            for (i32 i = 0; i < m_frameCount; ++i)
            {
                rhi::BufferDesc vd{};
                vd.size = static_cast<u64>(m_targetVertexCapacity) * sizeof(VGRenderVertex);
                vd.usage = rhi::BufferUsage::Vertex;
                vd.memory = rhi::MemoryLocation::CpuToGpu;
                if (!m_device->CreateBuffer(vd, m_vertexBuffers[static_cast<usize>(i)]).IsOk())
                    return ErrorCode::Unknown;

                rhi::BufferDesc id{};
                id.size = static_cast<u64>(m_targetVertexCapacity) * 3u * sizeof(u32);
                id.usage = rhi::BufferUsage::Index;
                id.memory = rhi::MemoryLocation::CpuToGpu;
                if (!m_device->CreateBuffer(id, m_indexBuffers[static_cast<usize>(i)]).IsOk())
                    return ErrorCode::Unknown;
                m_frameVertexCapacity[static_cast<usize>(i)] = m_targetVertexCapacity;

                rhi::BufferDesc ud{};
                ud.size = static_cast<u64>(MaxUniformSlots) * UniformSlotSize;
                ud.usage = rhi::BufferUsage::Uniform;
                ud.memory = rhi::MemoryLocation::CpuToGpu;
                if (!m_device->CreateBuffer(ud, m_uniformBuffers[static_cast<usize>(i)]).IsOk())
                    return ErrorCode::Unknown;
            }
            return ErrorCode::Ok;
        }

        // Recreate one frame slot's vertex + index buffers at m_targetVertexCapacity. Called from
        // BeginFrame(frameIndex), where the slot's prior submission has been awaited (buffers are
        // direct-bound, no bind group, so no descriptor fixups). No-op if already at capacity.
        void GrowFrameBuffers(i32 frameIndex)
        {
            const usize fi = static_cast<usize>(frameIndex);
            if (m_frameVertexCapacity[fi] >= m_targetVertexCapacity)
                return;
            rhi::BufferDesc vd{};
            vd.size = static_cast<u64>(m_targetVertexCapacity) * sizeof(VGRenderVertex);
            vd.usage = rhi::BufferUsage::Vertex;
            vd.memory = rhi::MemoryLocation::CpuToGpu;
            rhi::Buffer* newVert = nullptr;
            if (!m_device->CreateBuffer(vd, newVert).IsOk())
                return;
            rhi::BufferDesc id{};
            id.size = static_cast<u64>(m_targetVertexCapacity) * 3u * sizeof(u32);
            id.usage = rhi::BufferUsage::Index;
            id.memory = rhi::MemoryLocation::CpuToGpu;
            rhi::Buffer* newIdx = nullptr;
            if (!m_device->CreateBuffer(id, newIdx).IsOk())
            {
                m_device->DestroyBuffer(newVert);
                return;
            }
            if (m_vertexBuffers[fi] != nullptr)
                m_device->DestroyBuffer(m_vertexBuffers[fi]);
            if (m_indexBuffers[fi] != nullptr)
                m_device->DestroyBuffer(m_indexBuffers[fi]);
            m_vertexBuffers[fi] = newVert;
            m_indexBuffers[fi] = newIdx;
            m_frameVertexCapacity[fi] = m_targetVertexCapacity;
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
            const rhi::TextureFormat fmt = foundation::texture::TextureFormatUtils::Convert(
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

            UniquePtr<CachedTexture> cached = MakeUnique<CachedTexture>(*m_allocator);
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

        // Blur a drawn layer's target into `intoIndex` (itself, or a copy): across into a scratch
        // target, then down (single-sampled, the content resolved already). False, nothing done,
        // without a scratch target. Leaves both ends in ShaderRead.
        bool BlurLayer(rhi::CommandEncoder& encoder, i32 frameIndex, const PreparedLayer& layer, i32 targetIndex,
                       i32 intoIndex)
        {
            const i32 scratchIndex = AcquireLayerTarget(frameIndex, layer.targetWidth, layer.targetHeight, true);
            if (scratchIndex < 0)
                return false;
            // Indices, not references: acquiring may have grown the pool.
            const LayerTarget& target = m_layerTargets[static_cast<usize>(targetIndex)];
            const LayerTarget& scratch = m_layerTargets[static_cast<usize>(scratchIndex)];
            const LayerTarget& into = m_layerTargets[static_cast<usize>(intoIndex)];
            const auto pass = [&](const LayerTarget& from, const LayerTarget& into, u32 uniformOffset)
            {
                rhi::RenderPassDesc desc{};
                rhi::ColorAttachment color{};
                color.view = into.colorView;
                color.loadOp = rhi::LoadOp::Clear;
                color.storeOp = rhi::StoreOp::Store;
                color.clearValue = rhi::ClearColor{0.0f, 0.0f, 0.0f, 0.0f};
                desc.colorAttachments.Add(color);
                desc.label = u8"VG layer blur";
                rhi::RenderPassEncoder* rp = encoder.BeginRenderPass(desc);
                if (rp == nullptr)
                    return;
                rp->SetViewport(0.0f, 0.0f, static_cast<f32>(layer.width), static_cast<f32>(layer.height), 0.0f,
                                1.0f);
                rp->SetScissor(0, 0, layer.width, layer.height);
                rp->SetPipeline(m_blurPipeline);
                const u32 dynOffsets[1] = {uniformOffset};
                rp->SetBindGroup(0, from.bindGroup, Span<const u32>(dynOffsets, 1));
                rp->Draw(3, 1, 0, 0);
                rp->End();
                ++m_lastLayerStats.drawn;
            };
            encoder.TransitionTexture(scratch.color, rhi::ResourceState::Undefined, rhi::ResourceState::RenderTarget);
            pass(target, scratch, layer.blurUniformOffset);
            encoder.TransitionTexture(scratch.color, rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderRead);
            encoder.TransitionTexture(into.color, intoIndex == targetIndex ? rhi::ResourceState::ShaderRead
                                                                            : rhi::ResourceState::Undefined,
                                      rhi::ResourceState::RenderTarget);
            pass(scratch, into, layer.blurUniformOffset + static_cast<u32>(UniformSlotSize));
            encoder.TransitionTexture(into.color, rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderRead);
            return true;
        }

        // The blur pipeline: no vertex buffer (one triangle from the vertex index), single-sampled,
        // no stencil, writing what it computes (the target holds only the layer).
        Status CreateBlurPipeline(rhi::ShaderModule& vertShader, rhi::ShaderModule& fragShader)
        {
            rhi::ColorTargetState colorTarget{};
            colorTarget.format = m_targetFormat;
            const rhi::ColorTargetState colorTargets[1] = {colorTarget};
            rhi::RenderPipelineDesc desc{};
            desc.layout = m_pipelineLayout;
            desc.vertex.shader = rhi::StageOf(&vertShader, rhi::ShaderStage::Vertex);
            rhi::FragmentState fragment{};
            fragment.shader = rhi::StageOf(&fragShader, rhi::ShaderStage::Fragment);
            fragment.targets = Span<const rhi::ColorTargetState>(colorTargets, 1);
            desc.fragment = fragment;
            desc.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            desc.primitive.cullMode = rhi::CullMode::None;
            desc.multisample.count = 1;
            return m_device->CreateRenderPipeline(desc, m_blurPipeline);
        }

        // A free target of the size in the frame slot's pool, or a new one; -1 when the device
        // cannot make it. `plain`: colour alone (a blur's scratch), else the renderer's sample
        // count and stencil too.
        i32 AcquireLayerTarget(i32 frameIndex, u32 width, u32 height, bool plain)
        {
            for (usize i = 0; i < m_layerTargets.Size(); ++i)
            {
                LayerTarget& target = m_layerTargets[i];
                if (target.frame == frameIndex && !target.inUse && target.width == width && target.height == height &&
                    target.plain == plain)
                {
                    target.inUse = true;
                    return static_cast<i32>(i);
                }
            }
            LayerTarget target;
            target.frame = frameIndex;
            target.width = width;
            target.height = height;
            target.inUse = true;
            target.plain = plain;
            rhi::TextureDesc cd = rhi::TextureDesc::RenderTarget(m_targetFormat, width, height, 1, u8"VG layer");
            bool made = m_device->CreateTexture(cd, target.color).IsOk() &&
                        m_device->CreateTextureView(target.color, rhi::TextureViewDesc{}, target.colorView).IsOk();
            if (made && !plain && m_targetConfig.sampleCount > 1)
            {
                rhi::TextureDesc md = rhi::TextureDesc::RenderTarget(m_targetFormat, width, height,
                                                                     m_targetConfig.sampleCount, u8"VG layer MSAA");
                md.usage = rhi::TextureUsage::RenderTarget;
                made = m_device->CreateTexture(md, target.msaa).IsOk() &&
                       m_device->CreateTextureView(target.msaa, rhi::TextureViewDesc{}, target.msaaView).IsOk();
            }
            if (made && !plain && m_targetConfig.depthStencilFormat != rhi::TextureFormat::Undefined)
            {
                rhi::TextureDesc dd{};
                dd.dimension = rhi::TextureDimension::Texture2D;
                dd.format = m_targetConfig.depthStencilFormat;
                dd.width = width;
                dd.height = height;
                dd.depth = 1;
                dd.usage = rhi::TextureUsage::DepthStencil;
                dd.sampleCount = m_targetConfig.sampleCount;
                dd.label = u8"VG layer stencil";
                made = m_device->CreateTexture(dd, target.depthStencil).IsOk() &&
                       m_device->CreateTextureView(target.depthStencil, rhi::TextureViewDesc{},
                                                   target.depthStencilView)
                           .IsOk();
            }
            if (made)
            {
                rhi::BindGroupEntry entries[3];
                entries[0] = rhi::BindGroupEntry::BufferEntry(m_uniformBuffers[static_cast<usize>(frameIndex)], 0,
                                                              sizeof(VGUniforms));
                entries[1] = rhi::BindGroupEntry::TextureEntry(target.colorView);
                entries[2] = rhi::BindGroupEntry::SamplerEntry(m_sampler);
                rhi::BindGroupDesc desc{};
                desc.layout = m_bindGroupLayout;
                desc.entries = Span<const rhi::BindGroupEntry>(entries, 3);
                made = m_device->CreateBindGroup(desc, target.bindGroup).IsOk();
            }
            if (!made)
            {
                DisposeLayerTarget(target);
                return -1;
            }
            m_layerTargets.PushBack(target);
            return static_cast<i32>(m_layerTargets.Size()) - 1;
        }

        // The frame slot's work is done: its targets are free again, and those unused for a while
        // are freed.
        void ReleaseLayerTargets(i32 frameIndex)
        {
            for (usize i = m_layerTargets.Size(); i > 0; --i)
            {
                LayerTarget& target = m_layerTargets[i - 1];
                if (target.frame != frameIndex)
                    continue;
                if (target.inUse)
                {
                    target.inUse = false;
                    target.idleFrames = 0;
                }
                else if (++target.idleFrames > kLayerTargetIdleFrames)
                {
                    DisposeLayerTarget(target);
                    m_layerTargets.RemoveAt(i - 1);
                }
            }
        }

        void DisposeLayerTarget(LayerTarget& target)
        {
            if (target.bindGroup != nullptr)
                m_device->DestroyBindGroup(target.bindGroup);
            if (target.depthStencilView != nullptr)
                m_device->DestroyTextureView(target.depthStencilView);
            if (target.depthStencil != nullptr)
                m_device->DestroyTexture(target.depthStencil);
            if (target.msaaView != nullptr)
                m_device->DestroyTextureView(target.msaaView);
            if (target.msaa != nullptr)
                m_device->DestroyTexture(target.msaa);
            if (target.colorView != nullptr)
                m_device->DestroyTextureView(target.colorView);
            if (target.color != nullptr)
                m_device->DestroyTexture(target.color);
            target = LayerTarget{};
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
        IAllocator* m_allocator;

        rhi::Device* m_device = nullptr;
        rhi::Queue* m_queue = nullptr;
        i32 m_frameCount = 0;
        rhi::TextureFormat m_targetFormat = rhi::TextureFormat::BGRA8UnormSrgb;

        rhi::BindGroupLayout* m_bindGroupLayout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        rhi::RenderPipeline* m_distanceFieldPipeline = nullptr;         // MSDF fragment variant (null if unused)
        VGTargetConfig m_targetConfig{}; // host target: sample count + DS format
        rhi::RenderPipeline* m_stencilWriteNonZero = nullptr; // winding pass (nullable)
        rhi::RenderPipeline* m_stencilWriteEvenOdd = nullptr; // parity pass (nullable)
        rhi::RenderPipeline* m_coverPipeline = nullptr;       // cover, default shading (nullable)
        rhi::RenderPipeline* m_coverGradRadialPipeline = nullptr; // cover, radial (nullable)
        rhi::RenderPipeline* m_coverGradConicPipeline = nullptr;  // cover, conic (nullable)
        rhi::RenderPipeline* m_gradRadialPipeline = nullptr; // per-pixel radial gradient (nullable)
        rhi::RenderPipeline* m_gradConicPipeline = nullptr;  // per-pixel conic gradient (nullable)
        rhi::RenderPipeline* m_boxShadowPipeline = nullptr;  // blurred rounded rect (nullable)
        rhi::ShaderModule* m_vsModule = nullptr; // borrowed (lazy blend variants)
        rhi::ShaderModule* m_fsModule = nullptr;
        rhi::ShaderModule* m_distanceFieldModule = nullptr;
        rhi::ShaderModule* m_gradRadialModule = nullptr;
        rhi::ShaderModule* m_gradConicModule = nullptr;
        rhi::ShaderModule* m_boxShadowModule = nullptr;
        rhi::ShaderModule* m_boxVsModule = nullptr; // analytic boxes (VGDrawMode::Box)
        rhi::ShaderModule* m_boxFsModule = nullptr;
        rhi::RenderPipeline* m_boxPipeline = nullptr;
        rhi::ShaderModule* m_layerFsModule = nullptr; // layer composites (VGDrawMode::Layer)
        rhi::RenderPipeline* m_layerPipeline = nullptr;
        rhi::RenderPipeline* m_blurPipeline = nullptr; // layer blur (vg_blur), nullable
        rhi::RenderPipeline* m_blendPipelines[kBlendVariantCount][kPipelineKindCount] = {}; // [blend-1][kind] (unclipped)
        rhi::RenderPipeline* m_clippedPipelines[kBlendVariantCount + 1][kPipelineKindCount] = {}; // [blend][kind], lazy
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

        // On-demand growth of the vertex/index buffers: an overflowing Prepare bumps the TARGET and
        // skips that frame's slice; each frame's buffers are (re)sized up to the target at BeginFrame,
        // where the slot's prior GPU submission has already been awaited (offsets reset -> reuse). A
        // large draw list then costs at most one blank frame per growth step instead of blanking while
        // it persists.
        i32 m_targetVertexCapacity = MaxVertices; // desired vertex capacity (verts); only grows
        Array<i32> m_frameVertexCapacity;         // each frame's ACTUAL vertex-buffer capacity
        bool m_capacityWarned = false;            // one-shot warn when a batch exceeds the hard ceiling

        // Evicted entries wait here until every in-flight frame has aged past them.
        struct RetiredTexture
        {
            UniquePtr<CachedTexture> entry;
            i32 framesLeft = 0;
        };

        Array<UniquePtr<CachedTexture>> m_textureCache;
        Array<RetiredTexture> m_retiredTextures;
        Array<const image::ImageData*> m_batchTextures;
        Array<foundation::vg::VGCommand> m_drawCommands;
        Array<PreparedLayer> m_layers;     // this frame's slices' layers (VGRenderSlice::layerStart)
        Array<LayerTarget> m_layerTargets; // every frame slot's, tagged with it

        Array<u32> m_frameVertexOffsets;
        Array<u32> m_frameIndexOffsets;
        Array<u32> m_frameUniformSlotCount;

        bool m_initialized = false;

        RenderStats m_lastRenderStats;
        RenderStats m_lastLayerStats;
    };
}
