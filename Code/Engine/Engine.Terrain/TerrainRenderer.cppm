// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.terrain:renderer - the chunked geo-mipmap terrain Renderer.
//
// Rides the Opaque category via the RegisterRenderer seam (no terrain code in Engine.Render, the
// sprites/particles precedent). ONE 65x65 grid VB + one index buffer PER LOD are uploaded once; every
// chunk draws that shared grid, placed + height-displaced in the VS from a per-chunk uniform and the
// R16Uint height texture. Resolve is where the per-view work lives (Resolve alone has the camera): it
// folds each terrain's world transform into a per-terrain ViewProj, replays foundation.terrain's
// ExtractVisibleChunkDraws (quadtree cull + shared-coverage LOD - the same tested CPU path), and emits
// one indexed draw per visible chunk at its LOD. Bind sets: 0 view, 1 per-chunk, 2 height texture.

module;
#include "Core/Prelude.h"
#include <initializer_list>

export module engine.terrain:renderer;

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import foundation.shaders.system;
import foundation.render; // Renderer, RenderRecordContext, ResolvedDraw, DrawItem, DynamicUniformRing
import foundation.terrain; // chunk grid mesh + ExtractVisibleChunkDraws (cull + LOD)
import :renderdata;

using namespace foundation::core;
namespace core = foundation::core;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

export namespace engine::terrain
{
    namespace render = foundation::render;
    namespace tmodel = foundation::terrain;

    class TerrainRenderer final : public render::Renderer
    {
    public:
        TerrainRenderer(rhi::Device& device, shaders::ShaderSystem& shaders,
                        u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_viewRing(device, framesInFlight, kViewSlotSize,
                         rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst, u8"terrain.view"),
              m_chunkRing(device, framesInFlight, kChunkSlotSize,
                          rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst, u8"terrain.chunk")
        {
        }
        ~TerrainRenderer() override { Shutdown(); }
        TerrainRenderer(const TerrainRenderer&) = delete;
        TerrainRenderer& operator=(const TerrainRenderer&) = delete;

        core::Status Initialize()
        {
            // set 0: per-terrain view UBO (b0, dynamic) + the CSM cascade array (t1) + a comparison
            // sampler (s0). The depth caster pass ignores t1/s0 (it only reads b0), but sharing one
            // layout keeps a single pipeline layout for both passes.
            rhi::BindGroupLayoutEntry viewEntry = rhi::BindGroupLayoutEntry::UniformBuffer(
                0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
            viewEntry.hasDynamicOffset = true;
            rhi::BindGroupLayoutEntry shadowTexEntry = rhi::BindGroupLayoutEntry::SampledTexture(
                1, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2DArray);
            shadowTexEntry.textureSampleType = rhi::TextureSampleType::Depth;
            rhi::BindGroupLayoutEntry shadowSampEntry{};
            shadowSampEntry.binding = 0;
            shadowSampEntry.visibility = rhi::ShaderStage::Fragment;
            shadowSampEntry.type = rhi::BindingType::ComparisonSampler;
            rhi::BindGroupLayoutEntry viewEntries[] = {viewEntry, shadowTexEntry, shadowSampEntry};
            rhi::BindGroupLayoutDesc vld{};
            vld.entries = Span<const rhi::BindGroupLayoutEntry>{viewEntries, 3};
            if (!m_device->CreateBindGroupLayout(vld, m_viewLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }

            // set 1: per-chunk placement UBO, dynamic offset (one slot per visible chunk).
            rhi::BindGroupLayoutEntry chunkEntry = rhi::BindGroupLayoutEntry::UniformBuffer(
                0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
            chunkEntry.hasDynamicOffset = true;
            rhi::BindGroupLayoutDesc cld{};
            cld.entries = Span<const rhi::BindGroupLayoutEntry>{&chunkEntry, 1};
            if (!m_device->CreateBindGroupLayout(cld, m_chunkLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }

            // set 2: the R16Uint height texture, read via Load in the VS (and PS for the normal).
            rhi::BindGroupLayoutEntry heightEntry = rhi::BindGroupLayoutEntry::SampledTexture(
                0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
            heightEntry.textureSampleType = rhi::TextureSampleType::Uint; // WebGPU: integer data texture
            rhi::BindGroupLayoutDesc hld{};
            hld.entries = Span<const rhi::BindGroupLayoutEntry>{&heightEntry, 1};
            if (!m_device->CreateBindGroupLayout(hld, m_heightLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            // set 2 for a HOLED chunk (the HOLES variants): the height texture plus the R8 hole
            // mask (t1) and its bilinear sampler (s0), which the pixel shaders discard by.
            rhi::BindGroupLayoutEntry holeMaskEntry =
                rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment);
            rhi::BindGroupLayoutEntry holeEntries[] = {
                heightEntry, holeMaskEntry, rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment)};
            rhi::BindGroupLayoutDesc hhld{};
            hhld.entries = Span<const rhi::BindGroupLayoutEntry>{holeEntries, 3};
            if (!m_device->CreateBindGroupLayout(hhld, m_heightHoleLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }

            // set 3: the top-K splat material - integer index map (t0, Load-only: filtering palette
            // indices is garbage) + weight map (t1) + base albedo (t2) + palette albedo array (t3) + the
            // per-layer tileScale storage buffer (t4) + base normal (t5) + normal array (t6) + base ORM
            // (t7) + ORM array (t8) + base height (t9) + height array (t10) + coverage mask array
            // (t11) + the repeat/trilinear albedo sampler (s0, all arrays).
            rhi::BindGroupLayoutEntry idxEntry =
                rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
            idxEntry.textureSampleType = rhi::TextureSampleType::Uint; // integer data texture
            rhi::BindGroupLayoutEntry wgtEntry =
                rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment);
            wgtEntry.textureSampleType =
                rhi::TextureSampleType::UnfilterableFloat; // Load-only (manual bilinear)
            rhi::BindGroupLayoutEntry matEntries[] = {
                idxEntry,
                wgtEntry,
                rhi::BindGroupLayoutEntry::SampledTexture(2, rhi::ShaderStage::Fragment),
                rhi::BindGroupLayoutEntry::SampledTexture(
                    3, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2DArray),
                rhi::BindGroupLayoutEntry::StorageBuffer(4, rhi::ShaderStage::Fragment,
                                                         /*readOnly*/ true, sizeof(f32)),
                rhi::BindGroupLayoutEntry::SampledTexture(5, rhi::ShaderStage::Fragment), // base normal
                rhi::BindGroupLayoutEntry::SampledTexture(
                    6, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2DArray),
                rhi::BindGroupLayoutEntry::SampledTexture(7, rhi::ShaderStage::Fragment), // base ORM
                rhi::BindGroupLayoutEntry::SampledTexture(
                    8, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2DArray),
                rhi::BindGroupLayoutEntry::SampledTexture(9, rhi::ShaderStage::Fragment), // base height
                rhi::BindGroupLayoutEntry::SampledTexture(
                    10, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2DArray),
                rhi::BindGroupLayoutEntry::SampledTexture(
                    11, rhi::ShaderStage::Fragment,
                    rhi::TextureViewDimension::Texture2DArray), // coverage mask array
                rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
            };
            rhi::BindGroupLayoutDesc mld{};
            mld.entries = Span<const rhi::BindGroupLayoutEntry>{matEntries, 13};
            if (!m_device->CreateBindGroupLayout(mld, m_materialLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }

            // Two pipeline layouts: the color pass binds set 3 (material); the depth pass does not (so
            // WebGPU's "every declared set must be bound" rule is satisfied without a spurious bind).
            rhi::BindGroupLayout* colorLayouts[] = {m_viewLayout, m_chunkLayout, m_heightLayout,
                                                    m_materialLayout};
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{colorLayouts, 4};
            if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::BindGroupLayout* depthLayouts[] = {m_viewLayout, m_chunkLayout, m_heightLayout};
            rhi::PipelineLayoutDesc dpld{};
            dpld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{depthLayouts, 3};
            if (!m_device->CreatePipelineLayout(dpld, m_depthPipelineLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            // The HOLES twins: set 2 = height + hole mask + sampler.
            rhi::BindGroupLayout* holeColorLayouts[] = {m_viewLayout, m_chunkLayout, m_heightHoleLayout,
                                                        m_materialLayout};
            rhi::PipelineLayoutDesc hpld{};
            hpld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{holeColorLayouts, 4};
            if (!m_device->CreatePipelineLayout(hpld, m_holePipelineLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::BindGroupLayout* holeDepthLayouts[] = {m_viewLayout, m_chunkLayout, m_heightHoleLayout};
            rhi::PipelineLayoutDesc hdpld{};
            hdpld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{holeDepthLayouts, 3};
            if (!m_device->CreatePipelineLayout(hdpld, m_holeDepthPipelineLayout).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }

            // The shared grid vertex buffer (surface + skirt copies, each vertex = u,v,skirtFlag) -
            // uploaded once, drawn for every chunk.
            Array<Float3> verts;
            tmodel::BuildChunkGridVertices(verts);
            m_gridVertexCount = static_cast<u32>(verts.Size());
            rhi::BufferDesc vbd{};
            vbd.size = verts.Size() * sizeof(Float3);
            vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
            vbd.memory = rhi::MemoryLocation::CpuToGpu;
            vbd.label = u8"terrain.grid.verts";
            if (!m_device->CreateBuffer(vbd, m_gridVertexBuffer).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            if (void* p = m_gridVertexBuffer->Map())
            {
                MemCopy(p, verts.Data(), verts.Size() * sizeof(Float3));
                m_gridVertexBuffer->Unmap();
            }

            // One index buffer per LOD (stride 2^lod over the shared grid).
            for (u32 lod = 0; lod <= tmodel::kMaxChunkLod; ++lod)
            {
                Array<u32> indices;
                tmodel::BuildChunkGridIndices(lod, indices);
                LodMesh& lm = m_lodMeshes[lod];
                lm.indexCount = static_cast<u32>(indices.Size());
                lm.surfaceIndexCount = tmodel::ChunkLodSurfaceIndexCount(lod);
                if (lm.indexCount == 0)
                {
                    continue;
                }
                rhi::BufferDesc ibd{};
                ibd.size = indices.Size() * sizeof(u32);
                ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
                ibd.memory = rhi::MemoryLocation::CpuToGpu;
                ibd.label = u8"terrain.grid.indices";
                if (!m_device->CreateBuffer(ibd, lm.indexBuffer).IsOk())
                {
                    return core::Status{core::ErrorCode::Unknown};
                }
                if (void* p = lm.indexBuffer->Map())
                {
                    MemCopy(p, indices.Data(), indices.Size() * sizeof(u32));
                    lm.indexBuffer->Unmap();
                }
            }

            // Shadow receive: a comparison sampler + a 1x1 dummy Texture2DArray depth map bound when no
            // caster exists this frame (so set 0 stays complete). The real CSM array arrives via
            // SetShadowMap. Mirrors MeshRenderer::CreateShadowResources.
            rhi::SamplerDesc ssd{};
            ssd.minFilter = rhi::FilterMode::Linear;
            ssd.magFilter = rhi::FilterMode::Linear;
            ssd.mipmapFilter = rhi::MipmapFilterMode::Nearest;
            ssd.addressU = rhi::AddressMode::ClampToEdge;
            ssd.addressV = rhi::AddressMode::ClampToEdge;
            ssd.addressW = rhi::AddressMode::ClampToEdge;
            ssd.compare = rhi::depth::NearerOrEqual(); // lit when the receiver is at or nearer than the occluder
            ssd.label = u8"terrain.shadowSampler";
            if (!m_device->CreateSampler(ssd, m_shadowSampler).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            // The hole mask's bilinear clamp sampler (the rim is its 0.5 iso-line).
            rhi::SamplerDesc hsd{};
            hsd.minFilter = rhi::FilterMode::Linear;
            hsd.magFilter = rhi::FilterMode::Linear;
            hsd.mipmapFilter = rhi::MipmapFilterMode::Nearest;
            hsd.addressU = rhi::AddressMode::ClampToEdge;
            hsd.addressV = rhi::AddressMode::ClampToEdge;
            hsd.addressW = rhi::AddressMode::ClampToEdge;
            hsd.label = u8"terrain.holeSampler";
            if (!m_device->CreateSampler(hsd, m_holeSampler).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::TextureDesc dsd{};
            dsd.format = rhi::TextureFormat::Depth32Float;
            dsd.width = 1;
            dsd.height = 1;
            dsd.arrayLayerCount = 1;
            dsd.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
            dsd.label = u8"terrain.dummyShadow";
            if (!m_device->CreateTexture(dsd, m_dummyShadowTex).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::TextureViewDesc dsvd{};
            dsvd.format = rhi::TextureFormat::Depth32Float;
            dsvd.aspect = rhi::TextureAspect::DepthOnly;
            dsvd.dimension = rhi::TextureViewDimension::Texture2DArray;
            dsvd.arrayLayerCount = 1;
            if (!m_device->CreateTextureView(m_dummyShadowTex, dsvd, m_dummyShadowView).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            m_activeShadowView = m_dummyShadowView;

            // The albedo sampler (repeat + trilinear; base + palette slices carry mips). The
            // splat rasters are Load-only - no splat sampler exists in the top-K model. 1x1
            // dummies fill absent material slots (zero weights/indices = pure base; white base).
            rhi::SamplerDesc apd{};
            apd.minFilter = rhi::FilterMode::Linear;
            apd.magFilter = rhi::FilterMode::Linear;
            apd.mipmapFilter = rhi::MipmapFilterMode::Linear; // trilinear across the albedo mips
            apd.addressU = rhi::AddressMode::Repeat;
            apd.addressV = rhi::AddressMode::Repeat;
            apd.addressW = rhi::AddressMode::Repeat;
            apd.label = u8"terrain.albedoSampler";
            if (!m_device->CreateSampler(apd, m_albedoSampler).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::TextureDesc wtd{};
            wtd.format = rhi::TextureFormat::RGBA8Unorm;
            wtd.width = 1;
            wtd.height = 1;
            wtd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            wtd.label = u8"terrain.white";
            if (!m_device->CreateTexture(wtd, m_whiteTex).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::TextureViewDesc wvd{};
            wvd.format = rhi::TextureFormat::RGBA8Unorm;
            wvd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!m_device->CreateTextureView(m_whiteTex, wvd, m_whiteView).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            // Absent-slot dummies for the top-K material: ZERO weights (= pure base) + zero
            // indices (RGBA8Uint) + a 1-slice white palette array + a single-entry tileScale
            // buffer - so an un-splatted terrain still satisfies the set-3 layout on WebGPU.
            rhi::TextureDesc ztd{};
            ztd.format = rhi::TextureFormat::RGBA8Unorm;
            ztd.width = 1;
            ztd.height = 1;
            ztd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            ztd.label = u8"terrain.zeroWeights";
            if (!m_device->CreateTexture(ztd, m_zeroWeightTex).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::TextureViewDesc zvd{};
            zvd.format = rhi::TextureFormat::RGBA8Unorm;
            zvd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!m_device->CreateTextureView(m_zeroWeightTex, zvd, m_zeroWeightView).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::TextureDesc utd{};
            utd.format = rhi::TextureFormat::RGBA8Uint;
            utd.width = 1;
            utd.height = 1;
            utd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            utd.label = u8"terrain.zeroIndices";
            if (!m_device->CreateTexture(utd, m_zeroIndexTex).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::TextureViewDesc uvd{};
            uvd.format = rhi::TextureFormat::RGBA8Uint;
            uvd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!m_device->CreateTextureView(m_zeroIndexTex, uvd, m_zeroIndexView).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::TextureDesc atd{};
            atd.format = rhi::TextureFormat::RGBA8Unorm;
            atd.width = 1;
            atd.height = 1;
            atd.arrayLayerCount = 1;
            atd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            atd.label = u8"terrain.whiteArray";
            if (!m_device->CreateTexture(atd, m_whiteArrayTex).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            rhi::TextureViewDesc avd{};
            avd.format = rhi::TextureFormat::RGBA8Unorm;
            avd.dimension = rhi::TextureViewDimension::Texture2DArray;
            avd.arrayLayerCount = 1;
            if (!m_device->CreateTextureView(m_whiteArrayTex, avd, m_whiteArrayView).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            // PBR dummies: flat-normal / default-ORM
            // / mid-height, as a 2D texture (base) and a 1-slice array (palette), bound when a terrain
            // supplies no normal / ORM / height map.
            struct DummyDesc
            {
                StringView label;
                bool array;
                rhi::Texture** tex;
                rhi::TextureView** view;
            };
            const DummyDesc dummies[] = {
                {u8"terrain.flatNormal", false, &m_flatNormalTex, &m_flatNormalView},
                {u8"terrain.flatNormalArray", true, &m_flatNormalArrayTex, &m_flatNormalArrayView},
                {u8"terrain.defaultOrm", false, &m_defaultOrmTex, &m_defaultOrmView},
                {u8"terrain.defaultOrmArray", true, &m_defaultOrmArrayTex, &m_defaultOrmArrayView},
                {u8"terrain.midHeight", false, &m_midHeightTex, &m_midHeightView},
                {u8"terrain.midHeightArray", true, &m_midHeightArrayTex, &m_midHeightArrayView},
                {u8"terrain.opaqueMaskArray", true, &m_opaqueMaskArrayTex, &m_opaqueMaskArrayView},
            };
            for (const DummyDesc& d : dummies)
            {
                rhi::TextureDesc dtd{};
                dtd.format = rhi::TextureFormat::RGBA8Unorm;
                dtd.width = 1;
                dtd.height = 1;
                dtd.arrayLayerCount = 1;
                dtd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
                dtd.label = d.label;
                if (!m_device->CreateTexture(dtd, *d.tex).IsOk())
                {
                    return core::Status{core::ErrorCode::Unknown};
                }
                rhi::TextureViewDesc dvd{};
                dvd.format = rhi::TextureFormat::RGBA8Unorm;
                dvd.dimension = d.array ? rhi::TextureViewDimension::Texture2DArray
                                        : rhi::TextureViewDimension::Texture2D;
                dvd.arrayLayerCount = 1;
                if (!m_device->CreateTextureView(*d.tex, dvd, *d.view).IsOk())
                {
                    return core::Status{core::ErrorCode::Unknown};
                }
            }
            rhi::BufferDesc tbd{};
            tbd.size = sizeof(f32);
            tbd.usage = rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst;
            tbd.memory = rhi::MemoryLocation::CpuToGpu;
            tbd.label = u8"terrain.dummyTileScales";
            if (!m_device->CreateBuffer(tbd, m_dummyTileBuffer).IsOk())
            {
                return core::Status{core::ErrorCode::Unknown};
            }
            if (void* mapped = m_dummyTileBuffer->Map())
            {
                const f32 one = 1.0f;
                MemCopy(mapped, &one, sizeof(one));
                m_dummyTileBuffer->Unmap();
            }

            if (rhi::Queue* q = m_device->GetQueue(rhi::QueueType::Graphics))
            {
                rhi::TransferBatch* tb = nullptr;
                if (q->CreateTransferBatch(tb).IsOk() && tb != nullptr)
                {
                    const u8 white[4] = {255, 255, 255, 255};
                    rhi::TextureDataLayout layout{};
                    layout.bytesPerRow = 4;
                    layout.rowsPerImage = 1;
                    tb->WriteTexture(m_whiteTex, Span<const u8>{white, 4}, layout,
                                     rhi::Extent3D{1, 1, 1});
                    const u8 zero[4] = {0, 0, 0, 0};
                    tb->WriteTexture(m_zeroWeightTex, Span<const u8>{zero, 4}, layout,
                                     rhi::Extent3D{1, 1, 1});
                    tb->WriteTexture(m_zeroIndexTex, Span<const u8>{zero, 4}, layout,
                                     rhi::Extent3D{1, 1, 1});
                    tb->WriteTexture(m_whiteArrayTex, Span<const u8>{white, 4}, layout,
                                     rhi::Extent3D{1, 1, 1});
                    const u8 flatNormal[4] = {128, 128, 255, 255}; // tangent-space +Z
                    const u8 defaultOrm[4] = {255, 255, 0, 255};   // AO 1, roughness 1, metallic 0
                    tb->WriteTexture(m_flatNormalTex, Span<const u8>{flatNormal, 4}, layout,
                                     rhi::Extent3D{1, 1, 1});
                    tb->WriteTexture(m_flatNormalArrayTex, Span<const u8>{flatNormal, 4}, layout,
                                     rhi::Extent3D{1, 1, 1});
                    tb->WriteTexture(m_defaultOrmTex, Span<const u8>{defaultOrm, 4}, layout,
                                     rhi::Extent3D{1, 1, 1});
                    tb->WriteTexture(m_defaultOrmArrayTex, Span<const u8>{defaultOrm, 4}, layout,
                                     rhi::Extent3D{1, 1, 1});
                    const u8 midHeight[4] = {128, 128, 128, 255}; // height 0.5 (.r used)
                    tb->WriteTexture(m_midHeightTex, Span<const u8>{midHeight, 4}, layout,
                                     rhi::Extent3D{1, 1, 1});
                    tb->WriteTexture(m_midHeightArrayTex, Span<const u8>{midHeight, 4}, layout,
                                     rhi::Extent3D{1, 1, 1});
                    const u8 opaqueMask[4] = {255, 255, 255, 255}; // coverage 1 (.r used)
                    tb->WriteTexture(m_opaqueMaskArrayTex, Span<const u8>{opaqueMask, 4}, layout,
                                     rhi::Extent3D{1, 1, 1});
                    (void)tb->Submit();
                    q->DestroyTransferBatch(tb);
                }
            }
            return core::Status{};
        }

        [[nodiscard]] Span<const render::RenderCategory> SupportedCategories() const override
        {
            static const render::RenderCategory cats[] = {render::RenderCategories::Opaque};
            return Span<const render::RenderCategory>{cats, 1};
        }

        void PrepareFrame(u32 maxDraws, u32 frameIndex) override
        {
            const u32 chunk = 4096u;
            // Each terrain allocates a view slot per PASS in a frame: 1 color + 1 depth prepass +
            // kCascadeCount shadow cascades (+ extra views). The chunk ring holds every pass's chunk
            // allocations (self-sized from the observed peak, both passes).
            const u32 wantViews =
                Max(kMaxTerrains * (2u + kCascadeCount), maxDraws == 0 ? 1u : maxDraws);
            m_viewRing.Reserve(wantViews);
            const u32 wantChunks = ((m_maxChunkAllocs + chunk - 1u) / chunk) * chunk;
            m_chunkRing.Reserve(wantChunks == 0 ? chunk : wantChunks);
            m_viewRing.BeginFrame(frameIndex);
            m_chunkRing.BeginFrame(frameIndex);
            m_frameChunks = 0;
            m_frameChunkAllocs = 0;
        }

        void Resolve(const render::RenderRecordContext& ctx, Span<const render::DrawItem> items,
                     Array<render::ResolvedDraw>& out) override
        {
            if (items.IsEmpty())
            {
                return;
            }
            m_depthFormat = ctx.depthFormat;
            rhi::RenderPipeline* pso = EnsurePipeline(ctx.colorFormat);
            if (pso == nullptr)
            {
                return;
            }
            rhi::BindGroup* viewBg = EnsureViewBindGroup();
            rhi::BindGroup* chunkBg = EnsureChunkBindGroup();
            if (viewBg == nullptr || chunkBg == nullptr)
            {
                return;
            }

            const Float4x4 proj =
                (ctx.view != nullptr) ? ctx.view->Camera().projection : Float4x4::Identity();
            // The scene's sun: the first directional light (GpuLight::type 0), as a direction TO the
            // light. Falls back to a fixed key light when the scene has no directional (so terrain is
            // never unlit). Point/spot lights are ignored here - terrain takes only the sun (Phase E).
            Float3 sun = Normalized(Float3{0.35f, 0.82f, 0.45f});
            for (usize li = 0; li < ctx.lights.Size(); ++li)
            {
                const render::GpuLight& light = ctx.lights[li];
                if (light.type < 0.5f) // directional
                {
                    const f32 len = Length(light.directionWS);
                    if (len > 1.0e-4f)
                    {
                        sun = light.directionWS * (-1.0f / len); // travel dir -> dir TO light
                    }
                    break;
                }
            }

            for (usize it = 0; it < items.Size(); ++it)
            {
                // Ours only (see ResolveDepthOnly): a foreign item downcast here is silent UB.
                if (items[it].data->rendererId != RendererId())
                {
                    continue;
                }
                const auto* data = static_cast<const TerrainRenderData*>(items[it].data);
                if (data == nullptr || data->chunks == nullptr || data->nodes == nullptr ||
                    data->heightView == nullptr || data->chunkCount == 0)
                {
                    continue;
                }

                // Per-terrain view UBO. The VS works in WORLD space (ChunkToWorld applied there, NOT
                // folded into ViewProj) so the PS can shadow-sample worldPos against the world-space
                // cascade matrices. Cascade fields are filled from ctx.cascades (Stage B) - here they
                // stay zero => shadowMeta.x (cascade count) 0 => SampleCSM returns fully lit.
                const render::DynamicUniformRing::Range vr = m_viewRing.Allocate();
                if (!vr.ok)
                {
                    continue;
                }
                ViewUBO ubo{};
                ubo.chunkToWorld = data->chunkToWorld;
                ubo.viewProj = ctx.viewProj;
                ubo.view = ctx.viewMatrix;
                ubo.prevViewProj = ctx.prevViewProj;
                ubo.lightDir = Float4{sun.x, sun.y, sun.z, 0.0f};
                ubo.cameraPos = Float4{ctx.cameraPos.x, ctx.cameraPos.y, ctx.cameraPos.z, 0.0f};
                ubo.jitter = Float4{ctx.jitter.x, ctx.jitter.y, ctx.prevJitter.x, ctx.prevJitter.y};
                const f32 uvYSign = m_device->NeedsClipSpaceYFlip() ? 1.0f : -1.0f;
                // z = heightBlendContrast, w = height maps bound: filled with the splat params below.
                ubo.shadowParams = Float4{0.0f, uvYSign, 0.0f, 0.0f};
                // CSM receive: copy this view's cascade matrices/splits (world-space - the VS emits
                // worldPos). Count 0 (no directional caster) leaves the PS fully lit. Mirrors
                // MeshRenderer's view-UBO shadow fill.
                if (ctx.cascades.valid)
                {
                    for (u32 c = 0; c < kCascadeCount; ++c)
                    {
                        ubo.cascadeViewProj[c] = ctx.cascades.viewProj[c];
                    }
                    ubo.cascadeSplitFar =
                        Float4{ctx.cascades.splitFar[0], ctx.cascades.splitFar[1],
                               ctx.cascades.splitFar[2], ctx.cascades.splitFar[3]};
                    ubo.cascadeTexelSize =
                        Float4{ctx.cascades.texelWorldSize[0], ctx.cascades.texelWorldSize[1],
                               ctx.cascades.texelWorldSize[2], ctx.cascades.texelWorldSize[3]};
                    ubo.shadowMeta = Float4{static_cast<f32>(kCascadeCount),
                                            static_cast<f32>(ctx.cascadeLayerBase),
                                            ctx.cascades.normalBias, ctx.cascades.depthBias};
                    ubo.shadowLight.x = ctx.cascades.strength;
                    ubo.shadowParams.x = ctx.cascades.farFade;
                }
                const bool hasWeights = data->weightView != nullptr && data->indexView != nullptr &&
                                        data->paletteArrayView != nullptr && data->paletteCount > 0;
                ubo.splatParams =
                    Float4{static_cast<f32>(data->paletteCount), hasWeights ? 1.0f : 0.0f,
                           data->baseTileScale, data->baseAlbedoView != nullptr ? 1.0f : 0.0f};
                // Height-blend: ShadowParams.z = the soft-skirt contrast,
                // .w = height maps bound (any base OR palette height map present). Off (w = 0) => the
                // PS keeps the linear weighting, byte-identical.
                const bool heightBound =
                    data->baseHeightView != nullptr || data->heightArrayView != nullptr;
                ubo.shadowParams.z = data->heightBlendContrast;
                ubo.shadowParams.w = heightBound ? 1.0f : 0.0f;
                // Coverage mask: SplatParams2.x = mask maps bound (a palette
                // mask array is present). Off (0) => the PS skips the coverage multiply, byte-identical.
                const bool maskBound = data->maskArrayView != nullptr;
                ubo.splatParams2 = Float4{maskBound ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
                MemCopy(vr.ptr, &ubo, sizeof(ubo));

                // Local-space frustum (chunkToWorld folded in) matches the chunks' local bounds.
                const BoundingFrustum frustum(data->chunkToWorld * ctx.viewProj);
                const Span<const tmodel::TerrainChunk> chunks{data->chunks, data->chunkCount};
                const Span<const f32> thresholds{data->thresholds, data->thresholdCount};
                m_draws.Clear();
                tmodel::ExtractVisibleChunkDraws(
                    Span<const tmodel::TerrainQuadtree::Node>{data->nodes, data->nodeCount}, chunks,
                    data->chunkToWorld, ctx.viewMatrix, proj, frustum, thresholds, data->lodBias,
                    m_draws);
                if (m_draws.IsEmpty())
                {
                    continue;
                }
                // Camera-independent pass (local-shadow tiles: ctx.view == null): the mesh
                // rule's terrain equivalent - every visible chunk casts at the COARSEST
                // level; shadows never render finer than any view shows. Cascades carry the
                // owning camera view (RecordShadowCasters), so their coverage LODs match the
                // main view's chunks exactly.
                if (ctx.view == nullptr)
                {
                    for (usize d = 0; d < m_draws.Size(); ++d)
                    {
                        m_draws[d].lod = tmodel::kMaxChunkLod;
                    }
                }

                rhi::BindGroup* heightBg = EnsureHeightBindGroup(data->heightView);
                rhi::BindGroup* materialBg = EnsureMaterialBindGroup(*data);
                // A terrain with holes: the HOLES twin + the mask bind group for its holed chunks.
                rhi::RenderPipeline* holePso =
                    data->holeView != nullptr ? EnsurePipeline(ctx.colorFormat, /*holes*/ true) : nullptr;
                rhi::BindGroup* holeBg = data->holeView != nullptr
                                             ? EnsureHeightHoleBindGroup(data->heightView, data->holeView)
                                             : nullptr;
                if (heightBg == nullptr || materialBg == nullptr)
                {
                    continue;
                }

                for (usize d = 0; d < m_draws.Size(); ++d)
                {
                    const tmodel::ChunkDraw& cd = m_draws[d];
                    const u32 lod = Min(cd.lod, tmodel::kMaxChunkLod);
                    const tmodel::TerrainChunk& c = data->chunks[static_cast<usize>(cd.chunkIndex)];
                    // The shared grid, or a holed chunk's own buffers (Specs/terrain-holes.md).
                    LodMesh lm = m_lodMeshes[lod];
                    if (!SelectChunkMesh(*data, c, static_cast<u32>(cd.chunkIndex), lod, lm))
                    {
                        continue;
                    }
                    if (lm.indexBuffer == nullptr || lm.indexCount == 0)
                    {
                        continue;
                    }

                    const render::DynamicUniformRing::Range cr = m_chunkRing.Allocate();
                    if (!cr.ok)
                    {
                        continue;
                    }
                    ++m_frameChunks;
                    ++m_frameChunkAllocs;
                    ChunkUBO cb = MakeChunkUBO(*data, c);
                    MemCopy(cr.ptr, &cb, sizeof(cb));

                    const bool holed = c.hasHoles && holePso != nullptr && holeBg != nullptr;
                    render::ResolvedDraw draw{};
                    draw.pso = holed ? holePso : pso;
                    draw.viewSet = viewBg;
                    draw.viewDynamic = true;
                    draw.viewOffset = vr.byteOffset;
                    draw.drawSet = chunkBg;
                    draw.drawDynamic = true;
                    draw.drawOffset = cr.byteOffset;
                    draw.materialSet = holed ? holeBg : heightBg; // set 2: height (+ hole mask)
                    draw.clusterSet = materialBg;  // set 3: splat material (splatmap + albedos)
                    draw.vertexBuffer0 = m_gridVertexBuffer;
                    draw.indexBuffer = lm.indexBuffer;
                    draw.indexFormat = rhi::IndexFormat::UInt32;
                    // Skirtless pass draws only the surface prefix (crack-plug diagnostics/tests).
                    draw.indexCount = m_skirtsEnabled ? lm.indexCount : lm.surfaceIndexCount;
                    draw.instanceCount = 1;
                    out.PushBack(draw);
                }
            }
        }

        // Depth-only caster: the camera depth prepass (ctx.depthPrepass) AND each CSM cascade (ctx.viewProj
        // = the cascade's world->light-clip). Same chunk cull/LOD as Resolve, but a vertex-only depth PSO
        // and SURFACE indices only (skirts are a shading crack-hack, not shadow casters).
        void ResolveDepthOnly(const render::RenderRecordContext& ctx, Span<const render::DrawItem> items,
                              Array<render::ResolvedDraw>& out) override
        {
            ResolveDepthLike(ctx, items, out, /*pick*/ false);
        }

        // GPU pick: the depth path's chunk cull/LOD with the `terrain_pick` PSO, whose fragment
        // writes the terrain entity's id (from the PickView layout in the view slot) into the
        // RG32Uint target (ctx.colorFormat). ctx.viewProj is the cropped camera VP, so the chunk
        // frustum cull is the crop's - a click culls to the chunks under the pointer.
        void ResolvePickIds(const render::RenderRecordContext& ctx, Span<const render::DrawItem> items,
                            Array<render::ResolvedDraw>& out) override
        {
            ResolveDepthLike(ctx, items, out, /*pick*/ true);
        }

        void ResolveDepthLike(const render::RenderRecordContext& ctx, Span<const render::DrawItem> items,
                              Array<render::ResolvedDraw>& out, bool pick)
        {
            if (items.IsEmpty())
            {
                return;
            }
            m_depthFormat = ctx.depthFormat;
            rhi::RenderPipeline* pso =
                pick ? EnsurePickPipeline(ctx.colorFormat, ctx.depthFormat)
                     : EnsureDepthPipeline(ctx.depthFormat, /*biased*/ !ctx.depthPrepass);
            // The DEPTH-pass group binds the dummy shadow view (not the live cascade being written) -
            // a cascade-cast pass would otherwise sample its own render attachment (WebGPU hazard).
            rhi::BindGroup* viewBg = EnsureDepthViewBindGroup();
            rhi::BindGroup* chunkBg = EnsureChunkBindGroup();
            if (pso == nullptr || viewBg == nullptr || chunkBg == nullptr)
            {
                return;
            }
            const Float4x4 proj =
                (ctx.view != nullptr) ? ctx.view->Camera().projection : Float4x4::Identity();

            for (usize it = 0; it < items.Size(); ++it)
            {
                // Ours only: the downcast below is valid ONLY for terrain items. Dispatchers
                // group runs by rendererId, but a foreign item slipping through is silent UB
                // (the PIE-start crash: a mesh item cast to TerrainRenderData) - gate hard.
                if (items[it].data->rendererId != RendererId())
                {
                    continue;
                }
                const auto* data = static_cast<const TerrainRenderData*>(items[it].data);
                if (data == nullptr || data->chunks == nullptr || data->nodes == nullptr ||
                    data->heightView == nullptr || data->chunkCount == 0)
                {
                    continue;
                }
                const render::DynamicUniformRing::Range vr = m_viewRing.Allocate();
                if (!vr.ok)
                {
                    continue;
                }
                if (pick)
                {
                    // The pick VS reads the PickView prefix: ChunkToWorld, the cropped VP, the id.
                    PickViewUBO pu{};
                    pu.chunkToWorld = data->chunkToWorld;
                    pu.viewProj = ctx.viewProj;
                    pu.pickIndex = render::EntityTag::Index(data->entityId) + 1u;
                    pu.pickGeneration = render::EntityTag::Generation(data->entityId);
                    MemCopy(vr.ptr, &pu, sizeof(pu));
                }
                else
                {
                    // The depth VS reads only ChunkToWorld + ViewProj; the rest of the slot is unused.
                    ViewUBO ubo{};
                    ubo.chunkToWorld = data->chunkToWorld;
                    ubo.viewProj = ctx.viewProj; // camera VP (prepass) or cascade light VP (shadow)
                    MemCopy(vr.ptr, &ubo, sizeof(ubo));
                }

                const BoundingFrustum frustum(data->chunkToWorld * ctx.viewProj);
                const Span<const tmodel::TerrainChunk> chunks{data->chunks, data->chunkCount};
                const Span<const f32> thresholds{data->thresholds, data->thresholdCount};
                m_draws.Clear();
                tmodel::ExtractVisibleChunkDraws(
                    Span<const tmodel::TerrainQuadtree::Node>{data->nodes, data->nodeCount}, chunks,
                    data->chunkToWorld, ctx.viewMatrix, proj, frustum, thresholds, data->lodBias,
                    m_draws);
                if (m_draws.IsEmpty())
                {
                    continue;
                }
                // Camera-independent pass (local-shadow tiles: ctx.view == null): the mesh
                // rule's terrain equivalent - every visible chunk casts at the COARSEST
                // level; shadows never render finer than any view shows. Cascades carry the
                // owning camera view (RecordShadowCasters), so their coverage LODs match the
                // main view's chunks exactly.
                if (ctx.view == nullptr)
                {
                    for (usize d = 0; d < m_draws.Size(); ++d)
                    {
                        m_draws[d].lod = tmodel::kMaxChunkLod;
                    }
                }
                rhi::BindGroup* heightBg = EnsureHeightBindGroup(data->heightView);
                rhi::RenderPipeline* holePso = nullptr;
                rhi::BindGroup* holeBg = nullptr;
                if (data->holeView != nullptr)
                {
                    holePso = pick ? EnsurePickPipeline(ctx.colorFormat, ctx.depthFormat, /*holes*/ true)
                                   : EnsureDepthPipeline(ctx.depthFormat, /*biased*/ !ctx.depthPrepass,
                                                         /*holes*/ true);
                    holeBg = EnsureHeightHoleBindGroup(data->heightView, data->holeView);
                }
                if (heightBg == nullptr)
                {
                    continue;
                }

                for (usize d = 0; d < m_draws.Size(); ++d)
                {
                    const tmodel::ChunkDraw& cd = m_draws[d];
                    const u32 lod = Min(cd.lod, tmodel::kMaxChunkLod);
                    const tmodel::TerrainChunk& c = data->chunks[static_cast<usize>(cd.chunkIndex)];
                    LodMesh lm = m_lodMeshes[lod];
                    if (!SelectChunkMesh(*data, c, static_cast<u32>(cd.chunkIndex), lod, lm))
                    {
                        continue;
                    }
                    if (lm.indexBuffer == nullptr || lm.surfaceIndexCount == 0)
                    {
                        continue;
                    }
                    const render::DynamicUniformRing::Range cr = m_chunkRing.Allocate();
                    if (!cr.ok)
                    {
                        continue;
                    }
                    ++m_frameChunkAllocs;
                    ChunkUBO cb = MakeChunkUBO(*data, c);
                    MemCopy(cr.ptr, &cb, sizeof(cb));

                    const bool holed = c.hasHoles && holePso != nullptr && holeBg != nullptr;
                    render::ResolvedDraw draw{};
                    draw.pso = holed ? holePso : pso;
                    draw.viewSet = viewBg;
                    draw.viewDynamic = true;
                    draw.viewOffset = vr.byteOffset;
                    draw.drawSet = chunkBg;
                    draw.drawDynamic = true;
                    draw.drawOffset = cr.byteOffset;
                    draw.materialSet = holed ? holeBg : heightBg; // set 2: height (+ hole mask)
                    draw.vertexBuffer0 = m_gridVertexBuffer;
                    draw.indexBuffer = lm.indexBuffer;
                    draw.indexFormat = rhi::IndexFormat::UInt32;
                    draw.indexCount = lm.surfaceIndexCount; // surface only - no skirt casters
                    draw.instanceCount = 1;
                    out.PushBack(draw);
                }
            }
        }

        void FinishFrame() override
        {
            m_maxChunksSeen = Max(m_maxChunksSeen, m_frameChunks);
            m_maxChunkAllocs = Max(m_maxChunkAllocs, m_frameChunkAllocs);
            m_viewRing.EndFrame();
            m_chunkRing.EndFrame();
        }

        void SetRetireQueue(render::GpuRetireQueue* retire) noexcept
        {
            m_retire = retire; // stale height bind groups retire through it too (in-flight safety)
            m_viewRing.SetRetireQueue(retire);
            m_chunkRing.SetRetireQueue(retire);
        }

        // The frame's CSM cascade array (null = no caster this frame -> the dummy map, samples fully lit).
        // Fanned out to every registered renderer by RenderFrame each frame (before PrepareFrame).
        void SetShadowMap(rhi::TextureView* view, u64 generation) override
        {
            m_activeShadowView = (view != nullptr) ? view : m_dummyShadowView;
            m_activeShadowGen = (view != nullptr) ? generation : 0;
        }
        // Terrain samples only the directional CSM, not the local (spot/point) atlas - leave the base
        // SetShadowAtlas no-op.

        // One-time: transition the out-of-graph 1x1 dummy shadow depth into the layout its descriptor
        // expects (DepthStencilRead), so a caster-less frame never SAMPLES it while still UNDEFINED
        // (VUID-vkCmdDraw-None-09600). Terrain has no skinning; it uses this per-frame encoder hook
        // (the only Renderer callback holding the OUTER command encoder, before any render pass) purely
        // for that transition. Mirrors MeshRenderer::UploadSkinning.
        void UploadSkinning(const render::ExtractedScene&, rhi::CommandEncoder& encoder) override
        {
            if (!m_dummyDepthInit && m_dummyShadowTex != nullptr)
            {
                encoder.TransitionTexture(m_dummyShadowTex, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::DepthStencilRead);
                m_dummyDepthInit = true;
            }
        }

        /// Peak per-frame chunk-draw count seen so far (diagnostics + headless verification): > 0 only
        /// once a frame emitted terrain draws, which requires the PSO (hence the shaders) to have built.
        [[nodiscard]] u32 MaxChunksDrawn() const noexcept { return m_maxChunksSeen; }

        /// Draw the LOD-seam skirts (default on). Off draws only the surface - used to prove the skirts
        /// actually plug cracks (a skirtless mixed-LOD frame leaks background through the seams).
        void SetSkirtsEnabled(bool enabled) noexcept { m_skirtsEnabled = enabled; }

    private:
        static constexpr u32 kMaxTerrains = 8;
        static constexpr u32 kCascadeCount = 4; // matches render::ShadowCascades::kCount
        static constexpr u64 kViewSlotSize = 768;  // 8 mat4 + 6 float4, padded to dynamic alignment
        static constexpr u64 kChunkSlotSize = 256; // 6 float2, padded to dynamic alignment

        // Mirrors the HLSL TerrainView cbuffer (b0, space0) - keep field order/offsets in lockstep.
        struct ViewUBO
        {
            Float4x4 chunkToWorld;
            Float4x4 viewProj;
            Float4x4 view;
            Float4x4 prevViewProj;
            Float4x4 cascadeViewProj[kCascadeCount];
            Float4 lightDir;
            Float4 cameraPos;
            Float4 jitter;
            Float4 cascadeSplitFar;
            Float4 cascadeTexelSize;
            Float4 shadowMeta;   // x = cascade count, y = layer base, z = normal bias, w = depth bias
            Float4 shadowParams; // x = far-fade width, y = uv.y sign, z = heightBlendContrast, w = height maps bound
            Float4 splatParams; // x = palette count, y = weights bound, z = base tile, w = base bound
            Float4 splatParams2; // x = mask maps bound, yzw spare
            Float4 shadowLight;  // x = the caster light's shadow strength (1 = full), yzw spare
        };

        struct LodMesh
        {
            rhi::Buffer* indexBuffer = nullptr;
            u32 indexCount = 0;        // surface + skirt walls
            u32 surfaceIndexCount = 0; // the surface prefix (skirtless draw range)
        };

        // A chunk with holes draws its OWN buffers (the render data carries them per chunk); a
        // chunk cut everywhere draws nothing; every other chunk draws the shared grid `lm` holds
        // on entry. False = nothing to draw for this chunk at this LOD.
        [[nodiscard]] static bool SelectChunkMesh(const TerrainRenderData& data,
                                                  const tmodel::TerrainChunk& chunk, u32 chunkIndex,
                                                  u32 lod, LodMesh& lm)
        {
            if (chunk.allCut)
            {
                return false;
            }
            if (!chunk.hasHoles)
            {
                return true;
            }
            for (u32 i = 0; i < data.holedMeshCount; ++i)
            {
                const HoledChunkMesh& hm = data.holedMeshes[i];
                if (hm.chunkIndex != chunkIndex)
                {
                    continue;
                }
                lm.indexBuffer = hm.indexBuffers[lod];
                lm.indexCount = hm.indexCounts[lod];
                lm.surfaceIndexCount = hm.surfaceIndexCounts[lod];
                return lm.indexBuffer != nullptr && lm.indexCount != 0;
            }
            return false; // a holed chunk without its buffers (a failed upload) draws nothing
        }


        struct DepthPso
        {
            rhi::RenderPipeline* pso = nullptr;
            rhi::TextureFormat format = rhi::TextureFormat::Undefined;
            u64 shaderVersion = 0;
        };
        struct ColorPso
        {
            rhi::RenderPipeline* pso = nullptr;
            rhi::TextureFormat format = rhi::TextureFormat::Undefined;
            u64 shaderVersion = 0;
        };

        // The pick pass's view layout, written into the same TerrainView slot (the pick VS declares
        // exactly this prefix; terrain_pick.vs.hlsl). Keep in lockstep.
        struct PickViewUBO
        {
            Float4x4 chunkToWorld;
            Float4x4 viewProj;
            u32 pickIndex = 0, pickGeneration = 0, p0 = 0, p1 = 0;
        };
        static_assert(sizeof(PickViewUBO) == 144, "cbuffer TerrainView (pick prefix) layout drift");

        struct PickPso
        {
            rhi::RenderPipeline* pso = nullptr;
            rhi::TextureFormat colorFormat = rhi::TextureFormat::Undefined;
            rhi::TextureFormat depthFormat = rhi::TextureFormat::Undefined;
            u64 shaderVersion = 0;
        };

        // Mirrors the HLSL TerrainChunk cbuffer (b0, space1). Shared by the color + depth passes.
        struct ChunkUBO
        {
            Float2 originXZ;
            Float2 sizeXZ;
            Float2 texelBase;
            Float2 texelSpan;
            Float2 heightRange;
            Float2 gridSize;
            Float2 skirt; // x = skirt depth (world), y = pad
        };

        [[nodiscard]] static ChunkUBO MakeChunkUBO(const TerrainRenderData& data,
                                                   const tmodel::TerrainChunk& c)
        {
            // Skirt depth: how far the skirt ring drops below the surface to plug an LOD seam. Bounded
            // by the chunk's own relief (a seam can't mismatch by more), with a floor for near-flat
            // terrain. Only visible AT a crack, so being generous is free (unused in the depth pass).
            const f32 skirtDepth = Max(1.0f, 0.5f * (c.bounds.max.y - c.bounds.min.y));
            return ChunkUBO{
                Float2{c.bounds.min.x, c.bounds.min.z},
                Float2{c.bounds.max.x - c.bounds.min.x, c.bounds.max.z - c.bounds.min.z},
                Float2{static_cast<f32>(c.gridX0), static_cast<f32>(c.gridZ0)},
                Float2{static_cast<f32>(tmodel::kChunkQuads), static_cast<f32>(tmodel::kChunkQuads)},
                Float2{data.minY, data.maxY},
                Float2{static_cast<f32>(data.gridSize), static_cast<f32>(data.gridSize)},
                Float2{skirtDepth, 0.0f}};
        }

        // The forward/resolve pass view bind group: t1 = the LIVE CSM cascade array (or the dummy
        // when there is no caster this frame), which the terrain PS samples for shadow receive.
        rhi::BindGroup* EnsureViewBindGroup()
        {
            return BuildViewBindGroup(m_activeShadowView, m_activeShadowGen, m_viewBg, m_viewBgGen,
                                      m_viewBgShadow, m_viewBgShadowGen);
        }

        // The depth-only pass (camera prepass + each CSM cascade cast) view bind group: t1 = the
        // DUMMY shadow view, NEVER the live cascade. The depth VS/PS never samples t1, and during a
        // cascade cast the live cascade IS the render attachment - binding it here too is a WebGPU
        // read+write hazard ("usage (TextureBinding|RenderAttachment) ... same synchronization
        // scope"). The dummy is a standalone depth texture, never an attachment, so it is always safe.
        rhi::BindGroup* EnsureDepthViewBindGroup()
        {
            return BuildViewBindGroup(m_dummyShadowView, 0, m_depthViewBg, m_depthViewBgGen,
                                      m_depthViewBgShadow, m_depthViewBgShadowGen);
        }

        rhi::BindGroup* BuildViewBindGroup(rhi::TextureView* shadowT1, u64 shadowGen,
                                           rhi::BindGroup*& bg, u32& bgGen,
                                           rhi::TextureView*& bgShadow, u64& bgShadowGen)
        {
            const u32 gen = m_viewRing.Generation();
            // Rebuild on a ring roll-over OR a change of the bound shadow map (pointer or generation -
            // a freed view's address can be reused, so the generation guards address aliasing).
            if (bg != nullptr && bgGen == gen && bgShadow == shadowT1 && bgShadowGen == shadowGen)
            {
                return bg;
            }
            if (bg != nullptr)
            {
                // May sit in a submitted frame's descriptor set - retire (frame-aged) when possible.
                if (m_retire != nullptr)
                {
                    m_retire->Retire(bg);
                }
                else
                {
                    m_device->DestroyBindGroup(bg);
                }
                bg = nullptr;
            }
            if (m_viewRing.Buffer() == nullptr || shadowT1 == nullptr)
            {
                return nullptr;
            }
            rhi::BindGroupEntry entries[] = {
                rhi::BindGroupEntry::BufferEntry(m_viewRing.Buffer(), 0, kViewSlotSize), // b0
                rhi::BindGroupEntry::TextureEntry(shadowT1),                             // t1 (CSM array)
                rhi::BindGroupEntry::SamplerEntry(m_shadowSampler),                      // s0 (compare)
            };
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_viewLayout;
            bgd.entries = Span<const rhi::BindGroupEntry>{entries, 3};
            if (!m_device->CreateBindGroup(bgd, bg).IsOk())
            {
                bg = nullptr;
                return nullptr;
            }
            bgGen = gen;
            bgShadow = shadowT1;
            bgShadowGen = shadowGen;
            return bg;
        }

        rhi::BindGroup* EnsureChunkBindGroup()
        {
            const u32 gen = m_chunkRing.Generation();
            if (m_chunkBg != nullptr && m_chunkBgGen == gen)
            {
                return m_chunkBg;
            }
            if (m_chunkBg != nullptr)
            {
                m_device->DestroyBindGroup(m_chunkBg);
                m_chunkBg = nullptr;
            }
            if (m_chunkRing.Buffer() == nullptr)
            {
                return nullptr;
            }
            rhi::BindGroupEntry e =
                rhi::BindGroupEntry::BufferEntry(m_chunkRing.Buffer(), 0, kChunkSlotSize);
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_chunkLayout;
            bgd.entries = Span<const rhi::BindGroupEntry>{&e, 1};
            if (!m_device->CreateBindGroup(bgd, m_chunkBg).IsOk())
            {
                m_chunkBg = nullptr;
                return nullptr;
            }
            m_chunkBgGen = gen;
            return m_chunkBg;
        }

        // Per-height-texture bind group (set 2), keyed by view, validated by uniqueId (address reuse).
        rhi::BindGroup* EnsureHeightBindGroup(rhi::TextureView* tex)
        {
            if (tex == nullptr)
            {
                return nullptr;
            }
            if (HeightBindGroup* found = m_heightBindGroups.Find(tex))
            {
                if (found->viewId == tex->uniqueId)
                {
                    return found->bindGroup;
                }
                if (found->bindGroup != nullptr)
                {
                    // The old group may sit in a submitted frame's descriptor bindings -
                    // retire (frame-aged) rather than destroy in place; direct destroy only
                    // when no queue is wired (Null-device tests).
                    if (m_retire != nullptr)
                    {
                        m_retire->Retire(found->bindGroup);
                    }
                    else
                    {
                        m_device->DestroyBindGroup(found->bindGroup);
                    }
                }
                m_heightBindGroups.Remove(tex);
            }
            rhi::BindGroupEntry e = rhi::BindGroupEntry::TextureEntry(tex);
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_heightLayout;
            bgd.entries = Span<const rhi::BindGroupEntry>{&e, 1};
            rhi::BindGroup* bg = nullptr;
            if (!m_device->CreateBindGroup(bgd, bg).IsOk())
            {
                return nullptr;
            }
            m_heightBindGroups.InsertOrAssign(tex, HeightBindGroup{bg, tex->uniqueId});
            return bg;
        }

        // Set 2 for a holed chunk: height + hole mask + sampler, cached per hole view and
        // validated by both views' uniqueIds (never raw pointers - the bind-group-cache rule).
        rhi::BindGroup* EnsureHeightHoleBindGroup(rhi::TextureView* height, rhi::TextureView* hole)
        {
            if (height == nullptr || hole == nullptr || m_holeSampler == nullptr)
            {
                return nullptr;
            }
            if (HeightHoleBindGroup* found = m_heightHoleBindGroups.Find(hole))
            {
                if (found->heightId == height->uniqueId && found->holeId == hole->uniqueId)
                {
                    return found->bindGroup;
                }
                if (found->bindGroup != nullptr)
                {
                    if (m_retire != nullptr)
                    {
                        m_retire->Retire(found->bindGroup);
                    }
                    else
                    {
                        m_device->DestroyBindGroup(found->bindGroup);
                    }
                }
                m_heightHoleBindGroups.Remove(hole);
            }
            // Positional, in the layout's entry order: t0 height, t1 mask, s0 sampler.
            rhi::BindGroupEntry entries[] = {rhi::BindGroupEntry::TextureEntry(height),
                                             rhi::BindGroupEntry::TextureEntry(hole),
                                             rhi::BindGroupEntry::SamplerEntry(m_holeSampler)};
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_heightHoleLayout;
            bgd.entries = Span<const rhi::BindGroupEntry>{entries, 3};
            rhi::BindGroup* bg = nullptr;
            if (!m_device->CreateBindGroup(bgd, bg).IsOk())
            {
                return nullptr;
            }
            m_heightHoleBindGroups.InsertOrAssign(
                hole, HeightHoleBindGroup{bg, height->uniqueId, hole->uniqueId});
            return bg;
        }

        // Set 3 (splat material): splatmap + 4 albedos (white dummy for absent slots) + the two
        // samplers. Cached per splatmap view, validated by the uniqueId of ALL five views (never raw
        // pointers - address reuse); a hot-swap retires the stale group through the frame-retire queue.
        rhi::BindGroup* EnsureMaterialBindGroup(const TerrainRenderData& data)
        {
            // Absent slots bind the dummies: zero weights/indices = pure base; white base/array.
            rhi::TextureView* idx =
                (data.indexView != nullptr) ? data.indexView : m_zeroIndexView;
            rhi::TextureView* wgt =
                (data.weightView != nullptr) ? data.weightView : m_zeroWeightView;
            rhi::TextureView* base =
                (data.baseAlbedoView != nullptr) ? data.baseAlbedoView : m_whiteView;
            rhi::TextureView* pal =
                (data.paletteArrayView != nullptr) ? data.paletteArrayView : m_whiteArrayView;
            rhi::Buffer* tiles =
                (data.tileScaleBuffer != nullptr) ? data.tileScaleBuffer : m_dummyTileBuffer;
            const u64 tileGen = (data.tileScaleBuffer != nullptr) ? data.tileScaleGeneration : 0;
            // PBR maps: absent = the flat-normal / default-ORM dummies.
            rhi::TextureView* baseNrm =
                (data.baseNormalView != nullptr) ? data.baseNormalView : m_flatNormalView;
            rhi::TextureView* nrmArr =
                (data.normalArrayView != nullptr) ? data.normalArrayView : m_flatNormalArrayView;
            rhi::TextureView* baseOrm =
                (data.baseOrmView != nullptr) ? data.baseOrmView : m_defaultOrmView;
            rhi::TextureView* ormArr =
                (data.ormArrayView != nullptr) ? data.ormArrayView : m_defaultOrmArrayView;
            // Height maps: absent = the mid-height dummies.
            rhi::TextureView* baseHgt =
                (data.baseHeightView != nullptr) ? data.baseHeightView : m_midHeightView;
            rhi::TextureView* hgtArr =
                (data.heightArrayView != nullptr) ? data.heightArrayView : m_midHeightArrayView;
            // Coverage mask: absent = the opaque dummy (no coverage cut).
            rhi::TextureView* maskArr =
                (data.maskArrayView != nullptr) ? data.maskArrayView : m_opaqueMaskArrayView;

            // Keyed by the WEIGHT view pointer, VALIDATED by every view's uniqueId + the buffer
            // generation (bind-group-cache-versioning: pointers alias across reloads; ids don't).
            if (MaterialBindGroup* found = m_materialBindGroups.Find(wgt))
            {
                const bool match =
                    found->ids[0] == idx->uniqueId && found->ids[1] == wgt->uniqueId &&
                    found->ids[2] == base->uniqueId && found->ids[3] == pal->uniqueId &&
                    found->ids[4] == baseNrm->uniqueId && found->ids[5] == nrmArr->uniqueId &&
                    found->ids[6] == baseOrm->uniqueId && found->ids[7] == ormArr->uniqueId &&
                    found->ids[8] == baseHgt->uniqueId && found->ids[9] == hgtArr->uniqueId &&
                    found->ids[10] == maskArr->uniqueId && found->tileGen == tileGen;
                if (match)
                {
                    return found->bindGroup;
                }
                if (found->bindGroup != nullptr)
                {
                    if (m_retire != nullptr)
                    {
                        m_retire->Retire(found->bindGroup);
                    }
                    else
                    {
                        m_device->DestroyBindGroup(found->bindGroup);
                    }
                }
                m_materialBindGroups.Remove(wgt);
            }
            const u64 tilesSize =
                (data.tileScaleBuffer != nullptr)
                    ? static_cast<u64>(data.paletteCount) * sizeof(f32)
                    : sizeof(f32);
            rhi::BindGroupEntry entries[] = {
                rhi::BindGroupEntry::TextureEntry(idx),
                rhi::BindGroupEntry::TextureEntry(wgt),
                rhi::BindGroupEntry::TextureEntry(base),
                rhi::BindGroupEntry::TextureEntry(pal),
                rhi::BindGroupEntry::BufferEntry(tiles, 0, tilesSize),
                rhi::BindGroupEntry::TextureEntry(baseNrm),
                rhi::BindGroupEntry::TextureEntry(nrmArr),
                rhi::BindGroupEntry::TextureEntry(baseOrm),
                rhi::BindGroupEntry::TextureEntry(ormArr),
                rhi::BindGroupEntry::TextureEntry(baseHgt),
                rhi::BindGroupEntry::TextureEntry(hgtArr),
                rhi::BindGroupEntry::TextureEntry(maskArr),
                rhi::BindGroupEntry::SamplerEntry(m_albedoSampler),
            };
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_materialLayout;
            bgd.entries = Span<const rhi::BindGroupEntry>{entries, 13};
            rhi::BindGroup* bg = nullptr;
            if (!m_device->CreateBindGroup(bgd, bg).IsOk())
            {
                return nullptr;
            }
            MaterialBindGroup entry{bg,
                                    {idx->uniqueId, wgt->uniqueId, base->uniqueId, pal->uniqueId,
                                     baseNrm->uniqueId, nrmArr->uniqueId, baseOrm->uniqueId,
                                     ormArr->uniqueId, baseHgt->uniqueId, hgtArr->uniqueId,
                                     maskArr->uniqueId},
                                    tileGen};
            m_materialBindGroups.InsertOrAssign(wgt, entry);
            return bg;
        }

        // `holes` = the HOLES twin for holed chunks (the hole-mask discard; set 2 carries the mask).
        rhi::RenderPipeline* EnsurePipeline(rhi::TextureFormat colorFormat, bool holes = false)
        {
            const u64 shaderVersion = m_shaders->Version(u8"terrain");
            ColorPso& cp = holes ? m_holeColorPso : m_colorPso;
            if (cp.pso != nullptr && cp.format == colorFormat && cp.shaderVersion == shaderVersion)
            {
                return cp.pso;
            }
            if (cp.pso != nullptr)
            {
                m_device->DestroyRenderPipeline(cp.pso);
                cp.pso = nullptr;
            }
            const shaders::ShaderFlags flags =
                holes ? shaders::ShaderFlags::Holes : shaders::ShaderFlags::None;
            rhi::ShaderModule* vs =
                m_shaders->GetVariant(u8"terrain", shaders::ShaderStage::Vertex, flags);
            rhi::ShaderModule* ps =
                m_shaders->GetVariant(u8"terrain", shaders::ShaderStage::Fragment, flags);
            if (vs == nullptr || ps == nullptr)
            {
                return nullptr;
            }

            const rhi::VertexAttribute attrs[] = {{rhi::VertexFormat::Float32x3, 0, 0}};
            rhi::VertexBufferLayout vbl{};
            vbl.stride = sizeof(Float3);
            vbl.stepMode = rhi::VertexStepMode::Vertex;
            vbl.attributes = Span<const rhi::VertexAttribute>{attrs, 1};

            // Opaque terrain writes the forward GBUFFER: target 0 = shaded colour, 1 = view-space
            // normal, 2 = motion vector, 3 = material (roughness/metallic), 4 = diffuse albedo. The
            // forward pass binds all five, so the PSO must declare them (WebGPU rejects a target-count
            // mismatch). No blend.
            rhi::ColorTargetState targets[5]{};
            targets[0].format = colorFormat;
            targets[1].format = render::kGNormalFormat;
            targets[2].format = render::kGVelocityFormat;
            targets[3].format = render::kGMaterialFormat;
            targets[4].format = render::kGAlbedoFormat;

            rhi::FragmentState frag{};
            frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
            frag.targets = Span<const rhi::ColorTargetState>{targets, 5};

            rhi::DepthStencilState ds{};
            ds.format = m_depthFormat;
            ds.depthTestEnabled = true;
            ds.depthWriteEnabled = true;
            ds.depthCompare = rhi::depth::NearerOrEqual();

            rhi::RenderPipelineDesc pd{};
            pd.layout = holes ? m_holePipelineLayout : m_pipelineLayout;
            pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
            pd.vertex.buffers = Span<const rhi::VertexBufferLayout>{&vbl, 1};
            pd.fragment = frag;
            pd.depthStencil = ds;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            // The grid winds CCW when seen from above (the default FrontFace::CCW), so the top
            // surface is the front face: cull backs. Verified by the Vulkan pixel probe (a wrong
            // choice culls the top surface and the frame goes black).
            pd.primitive.cullMode = rhi::CullMode::Back;
            pd.label = holes ? u8"terrain.holes" : u8"terrain";
            rhi::RenderPipeline* pso = nullptr;
            if (!m_device->CreateRenderPipeline(pd, pso).IsOk())
            {
                return nullptr;
            }
            cp.pso = pso;
            cp.format = colorFormat;
            cp.shaderVersion = shaderVersion;
            return pso;
        }

        // Depth-only PSO (vertex-only, 0 color targets). `biased` adds the shadow-pass
        // slope-scaled depth bias (the camera prepass must match the forward depth exactly, so no
        // bias). INVARIANCE HARDENING: the CAMERA prepass (unbiased) uses the MAIN "terrain" VS
        // module - the same bytecode the color pass rasterizes with - so both passes get
        // bit-identical clip positions on every driver we ship on (D3D12 guarantees same-bytecode
        // position identity; Vulkan does not formally without the Invariant decoration, but
        // same-module reuse holds in practice) and the color pass's LessEqual never loses
        // fragments to ULP divergence (a separately compiled twin VS, though source-identical,
        // may reassociate the position transform; at distance the per-pixel depth gradient
        // shrinks below ULPs and fragments drop in row bands). The prepass-vs-color LOD parity
        // itself is owned by the pipeline (ctx.viewMatrix on the prepass context - the actual
        // zoomed-out banding bug). The extra interpolants are discarded (no fragment stage) and
        // the VS touches sets 0-2 only, so the 3-set depth layout still fits. Shadow passes keep
        // the cheap terrain_depth VS - cascade depth never depth-tests against the color pass.
        // `holes` = the HOLES twin: the "terrain" VS module for BOTH the prepass and the cascades
        // (terrain_depth.ps's input is terrain.vs's VSOut; a holed chunk is rare, so the cascade's
        // extra interpolants cost nothing that matters) plus terrain_depth's fragment stage
        // discarding by the hole mask, so the prepass depth and the cascades open with the colour
        // pass, whose draws share the same HOLES "terrain" VS module.
        rhi::RenderPipeline* EnsureDepthPipeline(rhi::TextureFormat depthFormat, bool biased,
                                                 bool holes = false)
        {
            // The HOLES twin always takes the "terrain" VS module: terrain_depth.ps declares
            // terrain.vs's full VSOut (the stage interface is matched by location), and a holed
            // chunk's prepass + cascades then share the colour pass's vertex bytecode.
            const StringView shaderName = (biased && !holes) ? u8"terrain_depth" : u8"terrain";
            DepthPso& p = (holes ? m_holeDepthPso : m_depthPso)[biased ? 1u : 0u];
            const u64 shaderVersion = m_shaders->Version(shaderName);
            if (p.pso != nullptr && p.format == depthFormat && p.shaderVersion == shaderVersion)
            {
                return p.pso;
            }
            if (p.pso != nullptr)
            {
                m_device->DestroyRenderPipeline(p.pso);
                p.pso = nullptr;
            }
            const shaders::ShaderFlags flags =
                holes ? shaders::ShaderFlags::Holes : shaders::ShaderFlags::None;
            rhi::ShaderModule* vs = m_shaders->GetVariant(shaderName, shaders::ShaderStage::Vertex, flags);
            rhi::ShaderModule* holePs =
                holes ? m_shaders->GetVariant(u8"terrain_depth", shaders::ShaderStage::Fragment, flags)
                      : nullptr;
            if (vs == nullptr || (holes && holePs == nullptr))
            {
                return nullptr;
            }
            const rhi::VertexAttribute attrs[] = {{rhi::VertexFormat::Float32x3, 0, 0}};
            rhi::VertexBufferLayout vbl{};
            vbl.stride = sizeof(Float3);
            vbl.stepMode = rhi::VertexStepMode::Vertex;
            vbl.attributes = Span<const rhi::VertexAttribute>{attrs, 1};

            rhi::DepthStencilState ds{};
            ds.format = depthFormat;
            ds.depthTestEnabled = true;
            ds.depthWriteEnabled = true;
            ds.depthCompare = rhi::depth::Nearer();
            if (biased)
            {
                ds.depthBias = rhi::depth::BiasAwayFromViewer(50);
                ds.depthBiasSlopeScale = rhi::depth::SlopeBiasAwayFromViewer(1.5f);
            }

            rhi::RenderPipelineDesc pd{};
            pd.layout = holes ? m_holeDepthPipelineLayout : m_depthPipelineLayout; // 3 sets (no material)
            pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
            pd.vertex.buffers = Span<const rhi::VertexBufferLayout>{&vbl, 1};
            // No fragment stage (Optional left empty) + no color targets = depth-only; the HOLES
            // twin adds the discarding fragment stage, still with no colour target.
            rhi::FragmentState holeFrag{};
            if (holes)
            {
                holeFrag.shader = rhi::ProgrammableStage{holePs, u8"main", rhi::ShaderStage::Fragment};
                pd.fragment = holeFrag;
            }
            pd.depthStencil = ds;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = rhi::CullMode::Back;
            pd.label = holes ? u8"terrain.depth.holes" : u8"terrain.depth";
            rhi::RenderPipeline* pso = nullptr;
            if (!m_device->CreateRenderPipeline(pd, pso).IsOk())
            {
                return nullptr;
            }
            p.pso = pso;
            p.format = depthFormat;
            p.shaderVersion = shaderVersion;
            return pso;
        }

        // The pick PSO: the depth layout (3 sets) + the terrain_pick fragment writing the id target.
        rhi::RenderPipeline* EnsurePickPipeline(rhi::TextureFormat colorFormat,
                                                rhi::TextureFormat depthFormat, bool holes = false)
        {
            const StringView shaderName = u8"terrain_pick";
            PickPso& p = holes ? m_holePickPso : m_pickPso;
            const u64 shaderVersion = m_shaders->Version(shaderName);
            if (p.pso != nullptr && p.colorFormat == colorFormat && p.depthFormat == depthFormat &&
                p.shaderVersion == shaderVersion)
            {
                return p.pso;
            }
            if (p.pso != nullptr)
            {
                m_device->DestroyRenderPipeline(p.pso);
                p.pso = nullptr;
            }
            const shaders::ShaderFlags flags =
                holes ? shaders::ShaderFlags::Holes : shaders::ShaderFlags::None;
            rhi::ShaderModule* vs = m_shaders->GetVariant(shaderName, shaders::ShaderStage::Vertex, flags);
            rhi::ShaderModule* fs = m_shaders->GetVariant(shaderName, shaders::ShaderStage::Fragment, flags);
            if (vs == nullptr || fs == nullptr)
            {
                return nullptr;
            }
            const rhi::VertexAttribute attrs[] = {{rhi::VertexFormat::Float32x3, 0, 0}};
            rhi::VertexBufferLayout vbl{};
            vbl.stride = sizeof(Float3);
            vbl.stepMode = rhi::VertexStepMode::Vertex;
            vbl.attributes = Span<const rhi::VertexAttribute>{attrs, 1};

            rhi::DepthStencilState ds{};
            ds.format = depthFormat;
            ds.depthTestEnabled = true;
            ds.depthWriteEnabled = true;
            ds.depthCompare = rhi::depth::Nearer(); // nearest surface owns the texel; no bias

            rhi::ColorTargetState target{};
            target.format = colorFormat; // RG32Uint: no blend
            target.writeMask = rhi::ColorWriteMask::All;
            rhi::FragmentState frag{};
            frag.shader = rhi::ProgrammableStage{fs, u8"main", rhi::ShaderStage::Fragment};
            frag.targets = Span<const rhi::ColorTargetState>{&target, 1};

            rhi::RenderPipelineDesc pd{};
            pd.layout = holes ? m_holeDepthPipelineLayout : m_depthPipelineLayout;
            pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
            pd.vertex.buffers = Span<const rhi::VertexBufferLayout>{&vbl, 1};
            pd.fragment = frag;
            pd.depthStencil = ds;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = rhi::CullMode::Back;
            pd.label = holes ? u8"terrain.pick.holes" : u8"terrain.pick";
            rhi::RenderPipeline* pso = nullptr;
            if (!m_device->CreateRenderPipeline(pd, pso).IsOk())
            {
                return nullptr;
            }
            p.pso = pso;
            p.colorFormat = colorFormat;
            p.depthFormat = depthFormat;
            p.shaderVersion = shaderVersion;
            return pso;
        }

        void Shutdown()
        {
            for (auto& kv : m_heightBindGroups)
            {
                if (kv.value.bindGroup != nullptr)
                {
                    m_device->DestroyBindGroup(kv.value.bindGroup);
                }
            }
            m_heightBindGroups.Clear();
            for (auto& kv : m_materialBindGroups)
            {
                if (kv.value.bindGroup != nullptr)
                {
                    m_device->DestroyBindGroup(kv.value.bindGroup);
                }
            }
            m_materialBindGroups.Clear();
            if (m_viewBg != nullptr)
            {
                m_device->DestroyBindGroup(m_viewBg);
                m_viewBg = nullptr;
            }
            if (m_depthViewBg != nullptr)
            {
                m_device->DestroyBindGroup(m_depthViewBg);
                m_depthViewBg = nullptr;
            }
            if (m_chunkBg != nullptr)
            {
                m_device->DestroyBindGroup(m_chunkBg);
                m_chunkBg = nullptr;
            }
            for (ColorPso* cp : {&m_colorPso, &m_holeColorPso})
            {
                if (cp->pso != nullptr)
                {
                    m_device->DestroyRenderPipeline(cp->pso);
                    cp->pso = nullptr;
                }
            }
            for (DepthPso* dps : {m_depthPso, m_holeDepthPso})
            {
                for (u32 i = 0; i < 2; ++i)
                {
                    if (dps[i].pso != nullptr)
                    {
                        m_device->DestroyRenderPipeline(dps[i].pso);
                        dps[i].pso = nullptr;
                    }
                }
            }
            for (PickPso* pp : {&m_pickPso, &m_holePickPso})
            {
                if (pp->pso != nullptr)
                {
                    m_device->DestroyRenderPipeline(pp->pso);
                    pp->pso = nullptr;
                }
            }
            for (auto& kv : m_heightHoleBindGroups)
            {
                if (kv.value.bindGroup != nullptr)
                {
                    m_device->DestroyBindGroup(kv.value.bindGroup);
                }
            }
            m_heightHoleBindGroups.Clear();
            for (LodMesh& lm : m_lodMeshes)
            {
                if (lm.indexBuffer != nullptr)
                {
                    m_device->DestroyBuffer(lm.indexBuffer);
                    lm.indexBuffer = nullptr;
                }
            }
            if (m_gridVertexBuffer != nullptr)
            {
                m_device->DestroyBuffer(m_gridVertexBuffer);
                m_gridVertexBuffer = nullptr;
            }
            if (m_dummyShadowView != nullptr)
            {
                m_device->DestroyTextureView(m_dummyShadowView);
                m_dummyShadowView = nullptr;
            }
            if (m_dummyShadowTex != nullptr)
            {
                m_device->DestroyTexture(m_dummyShadowTex);
                m_dummyShadowTex = nullptr;
            }
            if (m_shadowSampler != nullptr)
            {
                m_device->DestroySampler(m_shadowSampler);
                m_shadowSampler = nullptr;
            }
            if (m_whiteView != nullptr)
            {
                m_device->DestroyTextureView(m_whiteView);
                m_whiteView = nullptr;
            }
            if (m_whiteTex != nullptr)
            {
                m_device->DestroyTexture(m_whiteTex);
                m_whiteTex = nullptr;
            }
            if (m_zeroWeightView != nullptr)
            {
                m_device->DestroyTextureView(m_zeroWeightView);
                m_zeroWeightView = nullptr;
            }
            if (m_zeroWeightTex != nullptr)
            {
                m_device->DestroyTexture(m_zeroWeightTex);
                m_zeroWeightTex = nullptr;
            }
            if (m_zeroIndexView != nullptr)
            {
                m_device->DestroyTextureView(m_zeroIndexView);
                m_zeroIndexView = nullptr;
            }
            if (m_zeroIndexTex != nullptr)
            {
                m_device->DestroyTexture(m_zeroIndexTex);
                m_zeroIndexTex = nullptr;
            }
            if (m_whiteArrayView != nullptr)
            {
                m_device->DestroyTextureView(m_whiteArrayView);
                m_whiteArrayView = nullptr;
            }
            if (m_whiteArrayTex != nullptr)
            {
                m_device->DestroyTexture(m_whiteArrayTex);
                m_whiteArrayTex = nullptr;
            }
            rhi::TextureView* pbrViews[] = {m_flatNormalView,     m_flatNormalArrayView,
                                            m_defaultOrmView,     m_defaultOrmArrayView,
                                            m_midHeightView,      m_midHeightArrayView,
                                            m_opaqueMaskArrayView};
            for (rhi::TextureView*& v : pbrViews)
            {
                if (v != nullptr)
                {
                    m_device->DestroyTextureView(v);
                }
            }
            m_flatNormalView = m_flatNormalArrayView = m_defaultOrmView = m_defaultOrmArrayView =
                m_midHeightView = m_midHeightArrayView = m_opaqueMaskArrayView = nullptr;
            rhi::Texture* pbrTex[] = {m_flatNormalTex,      m_flatNormalArrayTex, m_defaultOrmTex,
                                      m_defaultOrmArrayTex, m_midHeightTex,       m_midHeightArrayTex,
                                      m_opaqueMaskArrayTex};
            for (rhi::Texture*& t : pbrTex)
            {
                if (t != nullptr)
                {
                    m_device->DestroyTexture(t);
                }
            }
            m_flatNormalTex = m_flatNormalArrayTex = m_defaultOrmTex = m_defaultOrmArrayTex =
                m_midHeightTex = m_midHeightArrayTex = m_opaqueMaskArrayTex = nullptr;
            if (m_dummyTileBuffer != nullptr)
            {
                m_device->DestroyBuffer(m_dummyTileBuffer);
                m_dummyTileBuffer = nullptr;
            }
            if (m_albedoSampler != nullptr)
            {
                m_device->DestroySampler(m_albedoSampler);
                m_albedoSampler = nullptr;
            }
            if (m_depthPipelineLayout != nullptr)
            {
                m_device->DestroyPipelineLayout(m_depthPipelineLayout);
                m_depthPipelineLayout = nullptr;
            }
            if (m_holeDepthPipelineLayout != nullptr)
            {
                m_device->DestroyPipelineLayout(m_holeDepthPipelineLayout);
                m_holeDepthPipelineLayout = nullptr;
            }
            if (m_holePipelineLayout != nullptr)
            {
                m_device->DestroyPipelineLayout(m_holePipelineLayout);
                m_holePipelineLayout = nullptr;
            }
            if (m_heightHoleLayout != nullptr)
            {
                m_device->DestroyBindGroupLayout(m_heightHoleLayout);
                m_heightHoleLayout = nullptr;
            }
            if (m_holeSampler != nullptr)
            {
                m_device->DestroySampler(m_holeSampler);
                m_holeSampler = nullptr;
            }
            if (m_pipelineLayout != nullptr)
            {
                m_device->DestroyPipelineLayout(m_pipelineLayout);
                m_pipelineLayout = nullptr;
            }
            if (m_materialLayout != nullptr)
            {
                m_device->DestroyBindGroupLayout(m_materialLayout);
                m_materialLayout = nullptr;
            }
            if (m_heightLayout != nullptr)
            {
                m_device->DestroyBindGroupLayout(m_heightLayout);
                m_heightLayout = nullptr;
            }
            if (m_chunkLayout != nullptr)
            {
                m_device->DestroyBindGroupLayout(m_chunkLayout);
                m_chunkLayout = nullptr;
            }
            if (m_viewLayout != nullptr)
            {
                m_device->DestroyBindGroupLayout(m_viewLayout);
                m_viewLayout = nullptr;
            }
        }

        struct HeightBindGroup
        {
            rhi::BindGroup* bindGroup = nullptr;
            u64 viewId = 0;
        };
        struct HeightHoleBindGroup
        {
            rhi::BindGroup* bindGroup = nullptr;
            u64 heightId = 0;
            u64 holeId = 0;
        };

        struct MaterialBindGroup
        {
            rhi::BindGroup* bindGroup = nullptr;
            // index/weight/base/palette + baseNormal/normalArray/baseOrm/ormArray +
            // baseHeight/heightArray + maskArray view uniqueIds.
            u64 ids[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            u64 tileGen = 0;
        };

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        render::DynamicUniformRing m_viewRing;
        render::DynamicUniformRing m_chunkRing;
        rhi::BindGroupLayout* m_viewLayout = nullptr;
        rhi::BindGroupLayout* m_chunkLayout = nullptr;
        rhi::BindGroupLayout* m_heightLayout = nullptr;
        rhi::BindGroupLayout* m_heightHoleLayout = nullptr; // set 2 for holed chunks: + mask + sampler
        rhi::BindGroupLayout* m_materialLayout = nullptr; // set 3: splat material
        rhi::PipelineLayout* m_pipelineLayout = nullptr;      // color: 4 sets
        rhi::PipelineLayout* m_depthPipelineLayout = nullptr; // depth: 3 sets (no material)
        rhi::PipelineLayout* m_holePipelineLayout = nullptr;      // the HOLES twins
        rhi::PipelineLayout* m_holeDepthPipelineLayout = nullptr;
        rhi::Sampler* m_holeSampler = nullptr;
        HashMap<rhi::TextureView*, HeightHoleBindGroup> m_heightHoleBindGroups;
        rhi::Buffer* m_gridVertexBuffer = nullptr;
        u32 m_gridVertexCount = 0;
        LodMesh m_lodMeshes[tmodel::kMaxChunkLod + 1];
        rhi::BindGroup* m_viewBg = nullptr;
        u32 m_viewBgGen = 0;
        // The depth/shadow-cast pass's own view group (t1 = dummy shadow view, never the live
        // cascade being rendered - a WebGPU read+write hazard otherwise).
        rhi::BindGroup* m_depthViewBg = nullptr;
        u32 m_depthViewBgGen = 0;
        rhi::TextureView* m_depthViewBgShadow = nullptr;
        u64 m_depthViewBgShadowGen = 0;
        rhi::BindGroup* m_chunkBg = nullptr;
        u32 m_chunkBgGen = 0;
        HashMap<rhi::TextureView*, HeightBindGroup> m_heightBindGroups;
        // Top-K splat material (set 3): absent-slot dummies + the per-weights bind cache.
        rhi::Sampler* m_albedoSampler = nullptr;
        rhi::Texture* m_whiteTex = nullptr; // 1x1 white (absent base albedo)
        rhi::TextureView* m_whiteView = nullptr;
        rhi::Texture* m_zeroWeightTex = nullptr; // 1x1 zero weights (= pure base)
        rhi::TextureView* m_zeroWeightView = nullptr;
        rhi::Texture* m_zeroIndexTex = nullptr; // 1x1 zero indices (RGBA8Uint)
        rhi::TextureView* m_zeroIndexView = nullptr;
        rhi::Texture* m_whiteArrayTex = nullptr; // 1x1x1 white palette array
        rhi::TextureView* m_whiteArrayView = nullptr;
        rhi::Texture* m_flatNormalTex = nullptr; // 1x1 flat normal (base, absent normal map)
        rhi::TextureView* m_flatNormalView = nullptr;
        rhi::Texture* m_flatNormalArrayTex = nullptr; // 1x1x1 flat-normal palette array
        rhi::TextureView* m_flatNormalArrayView = nullptr;
        rhi::Texture* m_defaultOrmTex = nullptr; // 1x1 default ORM (base, absent ORM map)
        rhi::TextureView* m_defaultOrmView = nullptr;
        rhi::Texture* m_defaultOrmArrayTex = nullptr; // 1x1x1 default-ORM palette array
        rhi::TextureView* m_defaultOrmArrayView = nullptr;
        rhi::Texture* m_midHeightTex = nullptr; // 1x1 mid height 0.5 (base, absent height map)
        rhi::TextureView* m_midHeightView = nullptr;
        rhi::Texture* m_midHeightArrayTex = nullptr; // 1x1x1 mid-height palette array
        rhi::TextureView* m_midHeightArrayView = nullptr;
        rhi::Texture* m_opaqueMaskArrayTex = nullptr; // 1x1x1 opaque-coverage palette array
        rhi::TextureView* m_opaqueMaskArrayView = nullptr;
        rhi::Buffer* m_dummyTileBuffer = nullptr; // one f32 = 1.0
        HashMap<rhi::TextureView*, MaterialBindGroup> m_materialBindGroups;
        render::GpuRetireQueue* m_retire = nullptr; // borrowed (RenderSubsystem owns + ticks)
        // Shadow receive (set 0: t1 CSM array + s0 comparison sampler).
        rhi::Sampler* m_shadowSampler = nullptr;
        rhi::Texture* m_dummyShadowTex = nullptr;      // 1x1 Texture2DArray depth (no-caster fallback)
        rhi::TextureView* m_dummyShadowView = nullptr;
        rhi::TextureView* m_activeShadowView = nullptr; // borrowed: the CSM array, or the dummy
        bool m_dummyDepthInit = false; // the dummy depth transitioned out of UNDEFINED once (VUID-09600)
        u64 m_activeShadowGen = 0;
        rhi::TextureView* m_viewBgShadow = nullptr; // what the cached view BG was built against
        u64 m_viewBgShadowGen = 0;
        ColorPso m_colorPso;     // the colour pass
        ColorPso m_holeColorPso; // its HOLES twin (holed chunks: the hole-mask discard)
        DepthPso m_depthPso[2]; // [0] = prepass (no bias), [1] = shadow cascade (biased)
        DepthPso m_holeDepthPso[2];
        PickPso m_pickPso;      // the GPU-pick id pass
        PickPso m_holePickPso;
        rhi::TextureFormat m_depthFormat = rhi::TextureFormat::Undefined;
        Array<tmodel::ChunkDraw> m_draws; // scratch, reused each terrain (Resolve is single-threaded)
        u32 m_frameChunks = 0;      // color-pass visible chunks (the MaxChunksDrawn diagnostic)
        u32 m_maxChunksSeen = 0;
        u32 m_frameChunkAllocs = 0; // chunk-ring allocs this frame across ALL passes (sizes the ring)
        u32 m_maxChunkAllocs = 0;
        bool m_skirtsEnabled = true;
    };
}
