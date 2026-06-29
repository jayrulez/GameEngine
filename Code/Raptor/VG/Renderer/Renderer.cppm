// Raptor::VG::Renderer — :renderer partition.
//
// VGRenderer: draws VGContext/VGBatch content through the RHI. Owns per-frame
// vertex/index/uniform ring buffers (byte-offset sub-allocated across slices),
// the pipeline, and an on-demand ImageData->GPU texture cache. Ported from
// Sedulous.VG.Renderer/VGRenderer.bf.
//
// Deviations from Sedulous (deliberate, documented):
//   * Initialize takes the two pre-compiled rhi::ShaderModule* (vert, frag)
//     rather than a ShaderSystem — shader compilation (DXC) is the caller's job
//     via raptor.shaders, keeping this lib's dependency to pure RHI.
//   * The shared cross-renderer external texture cache (VGExternalTextureCache)
//     is omitted (a multi-renderer sharing optimisation); add later if needed.

module;
#include "Core/Prelude.h"

export module raptor.vg.renderer:renderer;

import raptor.core;
import raptor.rhi;
import raptor.image;
import raptor.texture;
import raptor.vg;
import :vertex;

using namespace raptor::core;

export namespace raptor::vg::renderer
{
    namespace rhi = raptor::rhi;
    namespace img = raptor::image;

    /// Projection + distance-field uniforms (one per slice, padded to UniformSlotSize).
    struct VGUniforms
    {
        Mat4 projection = Mat4::Identity();  // 64 bytes
        f32  dfPxRange  = 4.0f;              // 4 bytes
        f32  dfAtlasW   = 512.0f;            // 4 bytes
        f32  dfAtlasH   = 512.0f;            // 4 bytes
        f32  _pad       = 0.0f;              // 4 bytes  -> total = 80 bytes, fits in 256
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

        /// Initialize with a device + the (already compiled) vg vertex/fragment
        /// shader modules + the render-target format + frame count.
        /// The optional dfFragShader enables the distance-field text pipeline.
        Status Initialize(rhi::Device& device, rhi::ShaderModule& vertShader, rhi::ShaderModule& fragShader,
                          rhi::TextureFormat targetFormat, i32 frameCount,
                          rhi::ShaderModule* dfFragShader = nullptr)
        {
            m_device = &device;
            m_queue = device.GetQueue(rhi::QueueType::Graphics, 0);
            m_targetFormat = targetFormat;
            m_frameCount = frameCount;

            if (!CreateSampler().IsOk()) return ErrorCode::Unknown;
            if (!CreateLayouts().IsOk()) return ErrorCode::Unknown;
            if (!CreatePipeline(vertShader, fragShader).IsOk()) return ErrorCode::Unknown;
            if (dfFragShader)
            {
                if (!CreateDFSampler().IsOk()) return ErrorCode::Unknown;
                if (!CreateDFPipeline(vertShader, *dfFragShader).IsOk()) return ErrorCode::Unknown;
            }
            if (!CreatePerFrameResources().IsOk()) return ErrorCode::Unknown;

            m_frameVertexOffsets.Resize(static_cast<usize>(frameCount));
            m_frameIndexOffsets.Resize(static_cast<usize>(frameCount));
            m_frameUniformSlotCount.Resize(static_cast<usize>(frameCount));

            m_initialized = true;
            return ErrorCode::Ok;
        }

        /// Reset per-frame ring-offset state. Call once before the frame's first Prepare.
        void BeginFrame(i32 frameIndex)
        {
            m_frameVertexOffsets[static_cast<usize>(frameIndex)] = 0;
            m_frameIndexOffsets[static_cast<usize>(frameIndex)] = 0;
            m_frameUniformSlotCount[static_cast<usize>(frameIndex)] = 0;
            m_drawCommands.Clear();
            m_batchTextures.Clear();
        }

        /// Upload one batch into the shared frame buffers; returns a slice token.
        VGRenderSlice Prepare(raptor::vg::VGBatch& batch, i32 frameIndex, u32 width, u32 height)
        {
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
            if (sliceVertOffset + vertByteSize > maxVertBytes
                || sliceIdxOffset + idxByteSize > maxIdxBytes
                || sliceUniformSlot >= static_cast<u32>(MaxUniformSlots))
                return VGRenderSlice{}; // capacity exceeded

            const u32 sliceUniformOffset = sliceUniformSlot * static_cast<u32>(UniformSlotSize);
            const i32 sliceCmdStart = static_cast<i32>(m_drawCommands.Size());
            const i32 textureBase = static_cast<i32>(m_batchTextures.Size());

            // Convert + upload this slice's vertices.
            Array<VGRenderVertex> renderVerts;
            renderVerts.Reserve(batch.vertices.Size());
            for (usize i = 0; i < batch.vertices.Size(); ++i)
                renderVerts.PushBack(VGRenderVertex(batch.vertices[i]));
            WriteBuffer(m_vertexBuffers[static_cast<usize>(frameIndex)], sliceVertOffset, renderVerts.Data(), vertByteSize);

            // Upload indices verbatim (relative to the slice's vertex base).
            WriteBuffer(m_indexBuffers[static_cast<usize>(frameIndex)], sliceIdxOffset, batch.indices.Data(), idxByteSize);

            // Append textures (commands index into the shared batch-texture list).
            for (usize i = 0; i < batch.textures.Size(); ++i)
                m_batchTextures.PushBack(batch.textures[i]);
            for (usize i = 0; i < batch.commands.Size(); ++i)
            {
                raptor::vg::VGCommand cmd = batch.commands[i];
                if (cmd.textureIndex >= 0)
                    cmd.textureIndex = cmd.textureIndex + textureBase;
                m_drawCommands.PushBack(cmd);
            }

            // Write this slice's projection + DF metadata into its uniform slot.
            VGUniforms uniforms;
            uniforms.projection = OrthoOffCenter(static_cast<f32>(width), static_cast<f32>(height));
            uniforms.dfPxRange = batch.dfPxRange;
            uniforms.dfAtlasW  = batch.dfAtlasW;
            uniforms.dfAtlasH  = batch.dfAtlasH;
            WriteBuffer(m_uniformBuffers[static_cast<usize>(frameIndex)], sliceUniformOffset, &uniforms, sizeof(VGUniforms));

            // Bind groups for any newly-added textures.
            for (i32 texIdx = textureBase; texIdx < static_cast<i32>(m_batchTextures.Size()); ++texIdx)
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

        /// Dispatch a slice's draws into the active render pass.
        void Render(rhi::RenderPassEncoder& renderPass, u32 width, u32 height, i32 frameIndex, const VGRenderSlice& slice)
        {
            if (!slice.isValid || slice.drawCommandCount == 0)
                return;

            renderPass.SetViewport(0.0f, 0.0f, static_cast<f32>(width), static_cast<f32>(height), 0.0f, 1.0f);
            renderPass.SetPipeline(m_pipeline);
            renderPass.SetVertexBuffer(0, m_vertexBuffers[static_cast<usize>(frameIndex)], slice.vertexByteOffset);
            renderPass.SetIndexBuffer(m_indexBuffers[static_cast<usize>(frameIndex)], rhi::IndexFormat::UInt32, slice.indexByteOffset);

            const u32 dynOffsets[1] = { slice.uniformByteOffset };
            i32 currentTextureIndex = -2; // sentinel forces first SetBindGroup
            auto currentDrawMode = raptor::vg::VGDrawMode::Default;

            const i32 cmdEnd = slice.drawCommandStart + slice.drawCommandCount;
            for (i32 i = slice.drawCommandStart; i < cmdEnd; ++i)
            {
                const raptor::vg::VGCommand& cmd = m_drawCommands[static_cast<usize>(i)];
                if (cmd.indexCount == 0)
                    continue;

                // Switch pipeline on draw mode change.
                if (cmd.drawMode != currentDrawMode)
                {
                    if (cmd.drawMode == raptor::vg::VGDrawMode::DistanceField && m_dfPipeline) {
                        renderPass.SetPipeline(m_dfPipeline);
                    }
                    else
                        renderPass.SetPipeline(m_pipeline);
                    currentDrawMode = cmd.drawMode;
                    currentTextureIndex = -2; // force rebind after pipeline switch
                }

                if (cmd.textureIndex != currentTextureIndex)
                {
                    if (rhi::BindGroup* bindGroup = GetBindGroupForTexture(cmd.textureIndex, frameIndex))
                        renderPass.SetBindGroup(0, bindGroup, Span<const u32>(dynOffsets, 1));
                    currentTextureIndex = cmd.textureIndex;
                }

                if (cmd.clipMode == raptor::vg::VGClipMode::Scissor && cmd.clipRect.width > 0.0f && cmd.clipRect.height > 0.0f)
                {
                    const i32 startX = static_cast<i32>(Ceil(Max(0.0f, cmd.clipRect.x)));
                    const i32 startY = static_cast<i32>(Ceil(Max(0.0f, cmd.clipRect.y)));
                    const i32 endX = static_cast<i32>(Floor(Min(cmd.clipRect.x + cmd.clipRect.width, static_cast<f32>(width))));
                    const i32 endY = static_cast<i32>(Floor(Min(cmd.clipRect.y + cmd.clipRect.height, static_cast<f32>(height))));
                    renderPass.SetScissor(startX, startY, static_cast<u32>(Max(0, endX - startX)), static_cast<u32>(Max(0, endY - startY)));
                }
                else if (cmd.clipMode == raptor::vg::VGClipMode::Scissor)
                {
                    renderPass.SetScissor(0, 0, 0, 0); // empty clip hides everything
                }
                else
                {
                    renderPass.SetScissor(0, 0, width, height);
                }

                renderPass.DrawIndexed(static_cast<u32>(cmd.indexCount), 1, static_cast<u32>(cmd.startIndex), 0, 0);
            }
        }

        /// Clear all cached GPU textures.
        void ClearTextureCache()
        {
            for (usize i = 0; i < m_textureCache.Size(); ++i)
                DisposeCachedTexture(*m_textureCache[i]);
            m_textureCache.Clear();
        }

        void Dispose()
        {
            if (m_device == nullptr) return;

            ClearTextureCache();

            DestroyBuffers(m_uniformBuffers);
            DestroyBuffers(m_indexBuffers);
            DestroyBuffers(m_vertexBuffers);

            if (m_dfPipeline) m_device->DestroyRenderPipeline(m_dfPipeline);
            if (m_pipeline) m_device->DestroyRenderPipeline(m_pipeline);
            if (m_pipelineLayout) m_device->DestroyPipelineLayout(m_pipelineLayout);
            if (m_bindGroupLayout) m_device->DestroyBindGroupLayout(m_bindGroupLayout);
            if (m_dfSampler) m_device->DestroySampler(m_dfSampler);
            if (m_sampler) m_device->DestroySampler(m_sampler);

            m_dfPipeline = nullptr; m_dfSampler = nullptr;
            m_pipeline = nullptr; m_pipelineLayout = nullptr; m_bindGroupLayout = nullptr; m_sampler = nullptr;
            m_initialized = false;
            m_device = nullptr;
        }

    private:
        struct CachedTexture
        {
            const img::ImageData* source = nullptr;
            rhi::Texture* gpuTexture = nullptr;
            rhi::TextureView* view = nullptr;
            Array<rhi::BindGroup*> bindGroups; // per frame
        };

        static constexpr i32 MaxVertices = 131072;
        static constexpr i32 MaxIndices = 131072 * 3;
        static constexpr i32 MaxUniformSlots = 64;
        static constexpr i32 UniformSlotSize = 256; // dynamic-offset alignment (>= sizeof(VGUniforms)=64)

        static void WriteBuffer(rhi::Buffer* buf, u64 offset, const void* data, usize size)
        {
            if (buf == nullptr || size == 0) return;
            if (u8* p = static_cast<u8*>(buf->Map()))
            {
                MemCopy(p + offset, data, size);
                buf->Unmap();
            }
        }

        static Mat4 OrthoOffCenter(f32 width, f32 height)
        {
            // CreateOrthographicOffCenter(0, width, height, 0, -1, 1) (row-vector).
            Mat4 m = Mat4::Identity();
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
            rhi::SamplerDesc desc{};
            return m_device->CreateSampler(desc, m_sampler);
        }

        Status CreateLayouts()
        {
            rhi::BindGroupLayoutEntry entries[3];
            entries[0] = rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
            entries[0].hasDynamicOffset = true; // one uniform buffer shared across slices via dynamic offset
            entries[1] = rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
            entries[2] = rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);

            rhi::BindGroupLayoutDesc bglDesc{};
            bglDesc.entries = Span<const rhi::BindGroupLayoutEntry>(entries, 3);
            if (!m_device->CreateBindGroupLayout(bglDesc, m_bindGroupLayout).IsOk()) return ErrorCode::Unknown;

            rhi::BindGroupLayout* const layouts[1] = { m_bindGroupLayout };
            rhi::PipelineLayoutDesc plDesc{};
            plDesc.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(layouts, 1);
            return m_device->CreatePipelineLayout(plDesc, m_pipelineLayout);
        }

        Status CreatePipeline(rhi::ShaderModule& vertShader, rhi::ShaderModule& fragShader)
        {
            const rhi::VertexAttribute attributes[4] = {
                { rhi::VertexFormat::Float32x2, 0, 0 },   // position
                { rhi::VertexFormat::Float32x2, 8, 1 },   // texCoord
                { rhi::VertexFormat::Float32x4, 16, 2 },  // color
                { rhi::VertexFormat::Float32, 32, 3 },    // coverage
            };
            rhi::VertexBufferLayout vbLayout{};
            vbLayout.stride = static_cast<u32>(sizeof(VGRenderVertex));
            vbLayout.attributes = Span<const rhi::VertexAttribute>(attributes, 4);
            const rhi::VertexBufferLayout vertexBuffers[1] = { vbLayout };

            rhi::ColorTargetState colorTarget{};
            colorTarget.format = m_targetFormat;
            colorTarget.blend = rhi::BlendState::AlphaBlend();
            const rhi::ColorTargetState colorTargets[1] = { colorTarget };

            rhi::RenderPipelineDesc desc{};
            desc.layout = m_pipelineLayout;
            desc.vertex.shader = rhi::ProgrammableStage{ &vertShader, u8"main", rhi::ShaderStage::Vertex };
            desc.vertex.buffers = Span<const rhi::VertexBufferLayout>(vertexBuffers, 1);

            rhi::FragmentState fragment{};
            fragment.shader = rhi::ProgrammableStage{ &fragShader, u8"main", rhi::ShaderStage::Fragment };
            fragment.targets = Span<const rhi::ColorTargetState>(colorTargets, 1);
            desc.fragment = fragment;

            desc.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            desc.primitive.frontFace = rhi::FrontFace::CCW;
            desc.primitive.cullMode = rhi::CullMode::None;
            desc.multisample.count = 1;
            desc.multisample.alphaToCoverageEnabled = false;

            return m_device->CreateRenderPipeline(desc, m_pipeline);
        }

        Status CreateDFSampler()
        {
            rhi::SamplerDesc desc{};
            desc.minFilter = rhi::FilterMode::Linear;
            desc.magFilter = rhi::FilterMode::Linear;
            desc.addressU = rhi::AddressMode::ClampToEdge;
            desc.addressV = rhi::AddressMode::ClampToEdge;
            return m_device->CreateSampler(desc, m_dfSampler);
        }

        Status CreateDFPipeline(rhi::ShaderModule& vertShader, rhi::ShaderModule& dfFragShader)
        {
            const rhi::VertexAttribute attributes[4] = {
                { rhi::VertexFormat::Float32x2, 0, 0 },
                { rhi::VertexFormat::Float32x2, 8, 1 },
                { rhi::VertexFormat::Float32x4, 16, 2 },
                { rhi::VertexFormat::Float32, 32, 3 },
            };
            rhi::VertexBufferLayout vbLayout{};
            vbLayout.stride = static_cast<u32>(sizeof(VGRenderVertex));
            vbLayout.attributes = Span<const rhi::VertexAttribute>(attributes, 4);
            const rhi::VertexBufferLayout vertexBuffers[1] = { vbLayout };

            rhi::ColorTargetState colorTarget{};
            colorTarget.format = m_targetFormat;
            colorTarget.blend = rhi::BlendState::AlphaBlend();
            const rhi::ColorTargetState colorTargets[1] = { colorTarget };

            rhi::RenderPipelineDesc desc{};
            desc.layout = m_pipelineLayout;
            desc.vertex.shader = rhi::ProgrammableStage{ &vertShader, u8"main", rhi::ShaderStage::Vertex };
            desc.vertex.buffers = Span<const rhi::VertexBufferLayout>(vertexBuffers, 1);

            rhi::FragmentState fragment{};
            fragment.shader = rhi::ProgrammableStage{ &dfFragShader, u8"main", rhi::ShaderStage::Fragment };
            fragment.targets = Span<const rhi::ColorTargetState>(colorTargets, 1);
            desc.fragment = fragment;

            desc.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            desc.primitive.frontFace = rhi::FrontFace::CCW;
            desc.primitive.cullMode = rhi::CullMode::None;
            desc.multisample.count = 1;
            desc.multisample.alphaToCoverageEnabled = false;

            return m_device->CreateRenderPipeline(desc, m_dfPipeline);
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
                if (!m_device->CreateBuffer(vd, m_vertexBuffers[static_cast<usize>(i)]).IsOk()) return ErrorCode::Unknown;

                rhi::BufferDesc id{};
                id.size = static_cast<u64>(MaxIndices) * sizeof(u32);
                id.usage = rhi::BufferUsage::Index;
                id.memory = rhi::MemoryLocation::CpuToGpu;
                if (!m_device->CreateBuffer(id, m_indexBuffers[static_cast<usize>(i)]).IsOk()) return ErrorCode::Unknown;

                rhi::BufferDesc ud{};
                ud.size = static_cast<u64>(MaxUniformSlots) * UniformSlotSize;
                ud.usage = rhi::BufferUsage::Uniform;
                ud.memory = rhi::MemoryLocation::CpuToGpu;
                if (!m_device->CreateBuffer(ud, m_uniformBuffers[static_cast<usize>(i)]).IsOk()) return ErrorCode::Unknown;
            }
            return ErrorCode::Ok;
        }

        CachedTexture* GetOrCreateCachedTexture(const img::ImageData* texture)
        {
            if (texture == nullptr) return nullptr;

            for (usize i = 0; i < m_textureCache.Size(); ++i)
                if (m_textureCache[i]->source == texture)
                    return m_textureCache[i].Get();

            const Span<const u8> pixels = texture->PixelData();
            if (pixels.Size() == 0) return nullptr;

            const u32 w = texture->Width();
            const u32 h = texture->Height();
            const rhi::TextureFormat fmt = raptor::texture::TextureFormatUtils::Convert(texture->Format(), texture->ColorSpace());

            rhi::TextureDesc td{};
            td.dimension = rhi::TextureDimension::Texture2D;
            td.format = fmt;
            td.width = w; td.height = h; td.depth = 1;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = u8"VGRenderer cached texture";

            rhi::Texture* gpuTexture = nullptr;
            if (!m_device->CreateTexture(td, gpuTexture).IsOk()) return nullptr;

            if (m_queue != nullptr)
            {
                rhi::TransferBatch* batch = nullptr;
                if (m_queue->CreateTransferBatch(batch).IsOk() && batch != nullptr)
                {
                    rhi::TextureDataLayout layout{};
                    layout.bytesPerRow = w * img::BytesPerPixel(texture->Format());
                    layout.rowsPerImage = h;
                    batch->WriteTexture(gpuTexture, pixels, layout, rhi::Extent3D{ w, h, 1 });
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
            cached->gpuTexture = gpuTexture;
            cached->view = view;
            cached->bindGroups.Resize(static_cast<usize>(m_frameCount)); // nullptr-filled
            CachedTexture* raw = cached.Get();
            m_textureCache.PushBack(Move(cached));
            return raw;
        }

        void UpdateTextureBindGroup(i32 textureIndex, i32 frameIndex)
        {
            if (textureIndex >= static_cast<i32>(m_batchTextures.Size())) return;
            const img::ImageData* texture = m_batchTextures[static_cast<usize>(textureIndex)];
            if (texture == nullptr) return;

            CachedTexture* cached = GetOrCreateCachedTexture(texture);
            if (cached == nullptr || cached->view == nullptr) return;
            if (cached->bindGroups[static_cast<usize>(frameIndex)] != nullptr) return; // already built

            rhi::BindGroupEntry entries[3];
            entries[0] = rhi::BindGroupEntry::BufferEntry(m_uniformBuffers[static_cast<usize>(frameIndex)], 0, sizeof(VGUniforms));
            entries[1] = rhi::BindGroupEntry::TextureEntry(cached->view);
            entries[2] = rhi::BindGroupEntry::SamplerEntry(m_sampler);

            rhi::BindGroupDesc desc{};
            desc.layout = m_bindGroupLayout;
            desc.entries = Span<const rhi::BindGroupEntry>(entries, 3);
            rhi::BindGroup* group = nullptr;
            if (m_device->CreateBindGroup(desc, group).IsOk())
                cached->bindGroups[static_cast<usize>(frameIndex)] = group;
        }

        rhi::BindGroup* GetBindGroupForTexture(i32 textureIndex, i32 frameIndex)
        {
            if (m_batchTextures.IsEmpty()) return nullptr;
            const i32 effectiveIndex = (textureIndex < 0) ? 0 : textureIndex; // solid draws -> white at 0
            if (effectiveIndex >= static_cast<i32>(m_batchTextures.Size())) return nullptr;

            const img::ImageData* texture = m_batchTextures[static_cast<usize>(effectiveIndex)];
            if (texture == nullptr) return nullptr;

            for (usize i = 0; i < m_textureCache.Size(); ++i)
                if (m_textureCache[i]->source == texture)
                    return m_textureCache[i]->bindGroups[static_cast<usize>(frameIndex)];
            return nullptr;
        }

        void DisposeCachedTexture(CachedTexture& cached)
        {
            for (usize i = 0; i < cached.bindGroups.Size(); ++i)
                if (cached.bindGroups[i] != nullptr) m_device->DestroyBindGroup(cached.bindGroups[i]);
            if (cached.view) m_device->DestroyTextureView(cached.view);
            if (cached.gpuTexture) m_device->DestroyTexture(cached.gpuTexture);
        }

        void DestroyBuffers(Array<rhi::Buffer*>& buffers)
        {
            for (usize i = 0; i < buffers.Size(); ++i)
                if (buffers[i] != nullptr) m_device->DestroyBuffer(buffers[i]);
            buffers.Clear();
        }

        rhi::Device* m_device = nullptr;
        rhi::Queue* m_queue = nullptr;
        i32 m_frameCount = 0;
        rhi::TextureFormat m_targetFormat = rhi::TextureFormat::BGRA8UnormSrgb;

        rhi::BindGroupLayout* m_bindGroupLayout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        rhi::RenderPipeline* m_dfPipeline = nullptr;
        rhi::Sampler* m_sampler = nullptr;
        rhi::Sampler* m_dfSampler = nullptr;

        Array<rhi::Buffer*> m_vertexBuffers;
        Array<rhi::Buffer*> m_indexBuffers;
        Array<rhi::Buffer*> m_uniformBuffers;

        Array<UniquePtr<CachedTexture>> m_textureCache;
        Array<const img::ImageData*> m_batchTextures;
        Array<raptor::vg::VGCommand> m_drawCommands;

        Array<u32> m_frameVertexOffsets;
        Array<u32> m_frameIndexOffsets;
        Array<u32> m_frameUniformSlotCount;

        bool m_initialized = false;
    };
}
