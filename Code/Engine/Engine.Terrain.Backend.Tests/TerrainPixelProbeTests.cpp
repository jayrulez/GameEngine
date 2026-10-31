// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Terrain pixel-level ground truth on a real Vulkan device. Renders terrain through the full
// RenderFrame chain, reads pixels back, and asserts render outcomes the Null test can't:
//   1. a lit dome covers the view and shades (normals and the lit path work, not a flat fill);
//   2. the scene's directional sun drives the shading (flipping it inverts the asymmetry);
//   3. LOD-seam skirts plug see-through cracks (a skirtless mixed-LOD frame leaks the background).
// The Null test covers compilation; this covers correct pixels. Skips with no Vulkan GPU.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <cstdio>

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
import foundation.rhi.webgpu; // desktop backend too, not just web
#ifdef OPTION_HAS_DX12
import foundation.rhi.dx12; // Windows only
#endif
import foundation.rhi.testsupport;
import foundation.shaders.system;
import foundation.render;
import foundation.heightfield;
import foundation.terrain;
import foundation.texture.resource; // texture::Texture (Adopt) for in-memory albedo fixtures
import foundation.terrain.resource; // SplatWeights, PaintTopK, TerrainPaletteData (top-K model)
import engine.terrain;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
namespace shaders = foundation::shaders;
namespace hf = foundation::heightfield;
namespace tmodel = foundation::terrain;
namespace texture = foundation::texture;

namespace
{
    constexpr u32 kSize = 256;

    // A radial dome: 1.0 at the center, 0 at the rim (varying normals -> varying shading).
    RefPtr<hf::Heightfield> MakeDome()
    {
        constexpr i32 n = 129;
        RefPtr<hf::Heightfield> h =
            MakeRef<hf::Heightfield>(DefaultAllocator(), n, Float2{130.0f, 130.0f}, 0.0f, 30.0f);
        const f32 c = static_cast<f32>(n - 1) * 0.5f;
        for (i32 z = 0; z < n; ++z)
        {
            for (i32 x = 0; x < n; ++x)
            {
                const f32 dx = (static_cast<f32>(x) - c) / c;
                const f32 dz = (static_cast<f32>(z) - c) / c;
                const f32 r = Min(Sqrt(dx * dx + dz * dz), 1.0f);
                const f32 hgt = Cos(r * 3.14159265f) * 0.5f + 0.5f; // 1 center .. 0 rim
                h->SetSample(x, z, static_cast<hf::Height>(hgt * 65535.0f));
            }
        }
        return h;
    }

    struct ProbeCfg
    {
        RefPtr<hf::Heightfield> terrain;
        Float3 eye{0, 120, 0.001f};
        Float3 target{0, 0, 0};
        Float3 up{0, 0, 1};
        f32 fov = 1.0f;
        rhi::ClearColor clear = rhi::ClearColor::Black();
        bool skirts = true;
        bool holeMask = true;               // bind the R8 hole mask (false = geometry rule only)
        const Float3* toLight = nullptr;    // direction to the light; null = renderer fallback sun
        const f32* thresholds = nullptr;    // LOD coverage thresholds override (null = default set)
        u32 thresholdCount = 0;
        // Top-K splat material (all null / 0 = no material, the height-lit fallback).
        rhi::TextureView* weightView = nullptr;     // RGBA8Unorm slot weights
        rhi::TextureView* indexView = nullptr;      // RGBA8Uint palette indices
        rhi::TextureView* baseAlbedoView = nullptr; // the base layer (erase reveals)
        rhi::TextureView* baseNormalView = nullptr; // base tangent-space normal map (terrain PBR)
        rhi::TextureView* baseOrmView = nullptr;    // base ORM (R=AO G=rough B=metal)
        f32 baseTileScale = 1.0f;
        rhi::TextureView* paletteArrayView = nullptr; // Texture2DArray, one slice per layer
        rhi::TextureView* normalArrayView = nullptr;  // per-layer normal array (terrain PBR)
        rhi::TextureView* ormArrayView = nullptr;     // per-layer ORM array
        rhi::TextureView* heightArrayView = nullptr;  // per-layer height array (height-blend)
        rhi::TextureView* maskArrayView = nullptr;    // per-layer coverage mask array (coverage-mask)
        f32 heightBlendContrast = 0.25f;              // soft-skirt width (only when a height map binds)
        Float4x4 chunkToWorld = Float4x4::Identity(); // the tangent frame follows this
        rhi::Buffer* tileScaleBuffer = nullptr;       // f32[paletteCount]
        u64 tileScaleGeneration = 0;
        u32 paletteCount = 0;
    };

    constexpr u32 kBands = 6; // vertical screen bands for the stripe-fixture colour readout

    struct Probe
    {
        bool valid = false;
        u32 filled = 0; // pixels brighter than the black background
        f64 leftLuma = 0, rightLuma = 0, topLuma = 0, bottomLuma = 0, total = 0;
        f64 centerLuma = 0; // the middle 16 x 16 block (the terrain holes probe reads it)
        f64 leftR = 0, leftB = 0, rightR = 0, rightB = 0; // per-channel halves (colour checks)
        f64 leftG = 0, rightG = 0;                        // green halves (mask-reveal A/B checks)
        // Band-centre channel means (x split into kBands, centre half of each band, centre half
        // in y, clear of band boundaries and the terrain rim) for the >4-layer stripe fixture.
        f64 bandR[kBands] = {};
        f64 bandG[kBands] = {};
        f64 bandB[kBands] = {};
    };

    Probe RenderTerrainProbe(rhi::Device& device, const ProbeCfg& cfg)
    {
        Probe probe;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return probe;
        }
        {
            shaders::ShaderSystem& shaderSystem = *host.System();
            engine::terrain::TerrainRenderer renderer(device, shaderSystem, /*framesInFlight*/ 2);
            REQUIRE(renderer.Initialize().IsOk());
            renderer.SetSkirtsEnabled(cfg.skirts);
            RendererRegistry registry;
            registry.Register(&renderer);
            RenderFrame frame(DefaultAllocator(), device, registry, /*framesInFlight*/ 2);

            const hf::Heightfield& terrain = *cfg.terrain;
            Array<tmodel::TerrainChunk> chunks;
            tmodel::BuildChunks(terrain, chunks);
            tmodel::TerrainQuadtree tree;
            tree.Build(Span<const tmodel::TerrainChunk>{chunks.Data(), chunks.Size()},
                       tmodel::ChunksPerSide(terrain.Size()));
            engine::terrain::TerrainHeightTextureCache heightCache;
            rhi::TextureView* heightView = heightCache.GetOrCreate(device, terrain, 1);
            REQUIRE(heightView != nullptr);
            // Holed chunks' own index buffers (none for a solid field).
            engine::terrain::TerrainHoledMeshCache holedCache;
            const Span<const engine::terrain::HoledChunkMesh> holed = holedCache.GetOrBuild(
                device, terrain, Span<const tmodel::TerrainChunk>{chunks.Data(), chunks.Size()}, 1);
            engine::terrain::TerrainHoleTextureCache holeTextures;
            rhi::TextureView* holeView =
                (cfg.holeMask && terrain.HasHoles()) ? holeTextures.GetOrCreate(device, terrain, 1)
                                                      : nullptr;

            static const f32 defaultThresholds[] = {1.0f, 0.25f, 0.08f, 0.03f, 0.012f, 0.005f, 0.002f};
            const f32* thresholds = cfg.thresholds != nullptr ? cfg.thresholds : defaultThresholds;
            const u32 thresholdCount = cfg.thresholds != nullptr ? cfg.thresholdCount : 7u;
            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            if (cfg.toLight != nullptr)
            {
                GpuLight sun{};
                sun.type = 0.0f; // directional
                sun.directionWS = *cfg.toLight * -1.0f;
                sun.color = Float3{1.0f, 1.0f, 1.0f};
                sun.intensity = 1.0f;
                scene.AddLight(sun);
            }
            engine::terrain::TerrainRenderData* rd = scene.Add<engine::terrain::TerrainRenderData>();
            REQUIRE(rd != nullptr);
            rd->category = RenderCategories::Opaque;
            rd->rendererId = renderer.RendererId();
            rd->chunks = chunks.Data();
            rd->nodes = tree.Nodes().Data();
            rd->nodeCount = static_cast<u32>(tree.Nodes().Size());
        rd->chunkCount = static_cast<u32>(chunks.Size());
            rd->holedMeshes = holed.Data();
            rd->holedMeshCount = static_cast<u32>(holed.Size());
            rd->holeView = holeView;
            rd->heightView = heightView;
            rd->chunkToWorld = cfg.chunkToWorld;
            rd->gridSize = terrain.Size();
            rd->worldSizeXZ = terrain.WorldSize();
            rd->minY = terrain.MinY();
            rd->maxY = terrain.MaxY();
            for (u32 i = 0; i < thresholdCount; ++i)
            {
                rd->thresholds[i] = thresholds[i];
            }
            rd->thresholdCount = thresholdCount;
            rd->weightView = cfg.weightView;
            rd->indexView = cfg.indexView;
            rd->baseAlbedoView = cfg.baseAlbedoView;
            rd->baseNormalView = cfg.baseNormalView;
            rd->baseOrmView = cfg.baseOrmView;
            rd->baseTileScale = cfg.baseTileScale;
            rd->paletteArrayView = cfg.paletteArrayView;
            rd->normalArrayView = cfg.normalArrayView;
            rd->ormArrayView = cfg.ormArrayView;
            rd->heightArrayView = cfg.heightArrayView;
            rd->maskArrayView = cfg.maskArrayView;
            rd->heightBlendContrast = cfg.heightBlendContrast;
            rd->tileScaleBuffer = cfg.tileScaleBuffer;
            rd->tileScaleGeneration = cfg.tileScaleGeneration;
            rd->paletteCount = cfg.paletteCount;
            rd->worldCenter = Float3{0.0f, 0.5f * (terrain.MaxY() + terrain.MinY()), 0.0f};
            rd->worldRadius = Length(terrain.WorldSize()) + (terrain.MaxY() - terrain.MinY());

            ViewCamera camera;
            camera.view = Float4x4::LookAtRH(cfg.eye, cfg.target, cfg.up);
            camera.projection = Float4x4::PerspectiveFovRH(cfg.fov, 1.0f, 1.0f, 4000.0f);

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"terrain.probe.target";
            rhi::Texture* target = nullptr;
            REQUIRE(device.CreateTexture(td, target).IsOk());
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            rhi::TextureView* targetView = nullptr;
            REQUIRE(device.CreateTextureView(target, vd, targetView).IsOk());

            rhi::CommandPool* pool = nullptr;
            REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
            rhi::Fence* fence = nullptr;
            REQUIRE(device.CreateFence(0, fence).IsOk());
            rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);
            REQUIRE(queue != nullptr);

            ViewSettings settings;
            settings.clear = cfg.clear;
            settings.targetTexture = target;
            settings.targetFinalState = rhi::ResourceState::CopySrc;
            settings.post.bloomEnabled = false;

            for (u32 i = 0; i < 2; ++i)
            {
                rhi::CommandEncoder* encoder = nullptr;
                REQUIRE(pool->CreateEncoder(encoder).IsOk());
                settings.targetCurrentState =
                    (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                frame.Begin(*encoder, i % 2);
                frame.AddView(scene, camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm,
                              kSize, kSize);
                frame.End();
                rhi::CommandBuffer* commandBuffer = encoder->Finish();
                REQUIRE(commandBuffer != nullptr);
                rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
                queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
                REQUIRE(fence->Wait(i + 1, ~0ull));
                pool->DestroyEncoder(encoder); // fence waited: safe to destroy
            }

            const testsupport::CapturedImage img =
                testsupport::Readback(device, target, kSize, kSize);
            REQUIRE(img.valid);
            for (u32 y = 0; y < kSize; ++y)
            {
                for (u32 x = 0; x < kSize; ++x)
                {
                    const u8* p = img.At(x, y);
                    const u32 luma = static_cast<u32>(p[0]) + p[1] + p[2];
                    probe.total += luma;
                    if (luma > 30)
                    {
                        ++probe.filled;
                    }
                    (x < kSize / 2 ? probe.leftLuma : probe.rightLuma) += luma;
                    (y < kSize / 2 ? probe.topLuma : probe.bottomLuma) += luma;
                    if (x >= kSize / 2 - 8 && x < kSize / 2 + 8 && y >= kSize / 2 - 8 && y < kSize / 2 + 8)
                    {
                        probe.centerLuma += luma;
                    }
                    if (x < kSize / 2) { probe.leftR += p[0]; probe.leftG += p[1]; probe.leftB += p[2]; }
                    else { probe.rightR += p[0]; probe.rightG += p[1]; probe.rightB += p[2]; }
                    // Band-centre sums (the stripe fixture): centre half in y, centre half of
                    // each band in x, clear of stripe-boundary bilinear blends and the rim.
                    if (y >= kSize / 4 && y < kSize * 3 / 4)
                    {
                        constexpr u32 bandWidth = kSize / kBands;
                        const u32 band = Min(x / bandWidth, kBands - 1);
                        const u32 xin = x - band * bandWidth;
                        if (xin >= bandWidth / 4 && xin < bandWidth * 3 / 4)
                        {
                            probe.bandR[band] += p[0];
                            probe.bandG[band] += p[1];
                            probe.bandB[band] += p[2];
                        }
                    }
                }
            }
            probe.valid = true;

            device.WaitIdle();
            heightCache.Clear(device); // owns the height texture/view; must release before the device
            holedCache.Clear(device);
            holeTextures.Clear(device);
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return probe;
    }

    rhi::Device* MakeVulkan(rhi::Backend*& backendOut)
    {
        backendOut = nullptr;
        (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, backendOut);
        return backendOut != nullptr ? testsupport::MakeTestDevice(backendOut) : nullptr;
    }

    // Renders the dome on a backend and returns its probe; {valid=false} if no device (skip). Owns
    // the device (created and destroyed here); the caller owns the backend.
    Probe DomeProbeOn(rhi::Backend* backend)
    {
        rhi::Device* device = backend != nullptr ? testsupport::MakeTestDevice(backend) : nullptr;
        if (device == nullptr)
        {
            return Probe{};
        }
        ProbeCfg cfg;
        cfg.terrain = MakeDome();
        const Probe probe = RenderTerrainProbe(*device, cfg);
        device->Destroy();
        return probe;
    }

    // Require a backend's dome to match the Vulkan reference (a shader-cook divergence in the height
    // fetch or the normal/lit path shows up as a coverage or luma mismatch).
    void CheckMatchesReference(const char* name, const Probe& ref, const Probe& p)
    {
        std::printf("[terrain-%s] filled=%u total=%.0f (ref filled=%u total=%.0f)\n", name, p.filled,
                    p.total, ref.filled, ref.total);
        CHECK(p.filled > 32000u); // it rendered terrain, not a black/failed frame
        CHECK(static_cast<f64>(p.filled) == doctest::Approx(static_cast<f64>(ref.filled)).epsilon(0.02));
        CHECK(p.total == doctest::Approx(ref.total).epsilon(0.05));
        CHECK(p.leftLuma == doctest::Approx(ref.leftLuma).epsilon(0.05));
        CHECK(p.bottomLuma == doctest::Approx(ref.bottomLuma).epsilon(0.05));
    }

    // Flat ground and a tall thin N-S ridge at the center X. A low +X sun makes the ridge cast a long
    // shadow across the flat -X ground, a clean caster and receiver: the flat ground has a uniform
    // normal, so any left/right darkening there is purely the cast shadow, not n.l shading.
    RefPtr<hf::Heightfield> MakeRidge()
    {
        constexpr i32 n = 257;
        RefPtr<hf::Heightfield> h =
            MakeRef<hf::Heightfield>(DefaultAllocator(), n, Float2{256.0f, 256.0f}, 0.0f, 60.0f);
        const i32 c = (n - 1) / 2;
        for (i32 z = 0; z < n; ++z)
        {
            for (i32 x = 0; x < n; ++x)
            {
                const bool wall = (x >= c - 3 && x <= c + 3); // a few cells wide, spanning Z
                h->SetSample(x, z, static_cast<hf::Height>((wall ? 0.92f : 0.05f) * 65535.0f));
            }
        }
        return h;
    }

    // A flat terrain (constant mid height): a clean canvas for the splat colour check.
    RefPtr<hf::Heightfield> MakeFlat()
    {
        constexpr i32 n = 129;
        RefPtr<hf::Heightfield> h =
            MakeRef<hf::Heightfield>(DefaultAllocator(), n, Float2{130.0f, 130.0f}, 0.0f, 30.0f);
        for (i32 z = 0; z < n; ++z)
        {
            for (i32 x = 0; x < n; ++x)
            {
                h->SetSample(x, z, static_cast<hf::Height>(0.3f * 65535.0f));
            }
        }
        return h;
    }

    // Wraps an RGBA8 texture (uploaded from `pixels`, w*h*4 bytes) as an in-memory texture::Texture
    // (Adopt owns and frees the GPU objects when the returned RefPtr is destroyed).
    RefPtr<texture::Texture> MakeRGBA(rhi::Device& device, u32 w, u32 h, const u8* pixels)
    {
        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::RGBA8Unorm;
        td.width = w;
        td.height = h;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        td.label = u8"probe.albedo";
        rhi::Texture* tex = nullptr;
        REQUIRE(device.CreateTexture(td, tex).IsOk());
        rhi::TextureViewDesc vd{};
        vd.format = rhi::TextureFormat::RGBA8Unorm;
        vd.dimension = rhi::TextureViewDimension::Texture2D;
        rhi::TextureView* view = nullptr;
        REQUIRE(device.CreateTextureView(tex, vd, view).IsOk());
        if (rhi::Queue* q = device.GetQueue(rhi::QueueType::Graphics))
        {
            rhi::TransferBatch* tb = nullptr;
            if (q->CreateTransferBatch(tb).IsOk() && tb != nullptr)
            {
                rhi::TextureDataLayout layout{};
                layout.bytesPerRow = w * 4;
                layout.rowsPerImage = h;
                tb->WriteTexture(tex, Span<const u8>{pixels, static_cast<usize>(w) * h * 4}, layout,
                                 rhi::Extent3D{w, h, 1});
                (void)tb->Submit();
                q->DestroyTransferBatch(tb);
            }
        }
        RefPtr<texture::Texture> t = MakeRef<texture::Texture>(DefaultAllocator());
        t->Adopt(&device, tex, view, nullptr, w, h, rhi::TextureFormat::RGBA8Unorm, false);
        return t;
    }

    RefPtr<texture::Texture> MakeSolid(rhi::Device& device, u8 r, u8 g, u8 b)
    {
        const u8 px[4] = {r, g, b, 255};
        return MakeRGBA(device, 1, 1, px);
    }

    // A CPU weights raster of `bands` full-strength vertical stripes: stripe i is one-hot on
    // palette layer i. With bands > 4 this needs more layers than four fixed splat slots allow.
    RefPtr<tmodel::SplatWeights> MakeStripeWeights(u32 bands)
    {
        constexpr i32 n = 32;
        RefPtr<tmodel::SplatWeights> sw =
            MakeRef<tmodel::SplatWeights>(DefaultAllocator(), n, n);
        Span<u8> idx = sw->Indices();
        Span<u8> wts = sw->Weights();
        for (i32 y = 0; y < n; ++y)
        {
            for (i32 x = 0; x < n; ++x)
            {
                const usize at = sw->TexelOffset(x, y);
                const u32 layer = Min(static_cast<u32>(x) * bands / n, bands - 1);
                idx[at + 0] = static_cast<u8>(layer);
                wts[at + 0] = 255; // one-hot: pure layer, no base
            }
        }
        sw->BumpVersion();
        return sw;
    }

    // A weights raster with two active slots everywhere: slot 0 is palette layer 0 at weight w0/255,
    // slot 1 is palette layer 1 at weight w1/255 (the base owns 1 minus their sum). For the
    // height-blend fixture: equal w0/w1 make a 50/50 tie the height maps break.
    RefPtr<tmodel::SplatWeights> MakeTwoLayerWeights(u8 w0, u8 w1)
    {
        constexpr i32 n = 32;
        RefPtr<tmodel::SplatWeights> sw = MakeRef<tmodel::SplatWeights>(DefaultAllocator(), n, n);
        Span<u8> idx = sw->Indices();
        Span<u8> wts = sw->Weights();
        for (i32 y = 0; y < n; ++y)
        {
            for (i32 x = 0; x < n; ++x)
            {
                const usize at = sw->TexelOffset(x, y);
                idx[at + 0] = 0;
                idx[at + 1] = 1;
                wts[at + 0] = w0;
                wts[at + 1] = w1;
            }
        }
        sw->BumpVersion();
        return sw;
    }

    // A cook-shaped palette texel blob of solid-colour 4x4 single-mip slices.
    RefPtr<tmodel::TerrainPaletteData> MakePaletteData(Span<const Float3> colors)
    {
        auto data = MakeRef<tmodel::TerrainPaletteData>(DefaultAllocator());
        data->sliceSize = 4;
        data->mipCount = 1;
        data->sliceCount = static_cast<u32>(colors.Size());
        const usize sliceBytes = tmodel::TerrainPaletteData::SliceBytes(4, 1);
        data->texels.Resize(sliceBytes * colors.Size());
        for (usize s = 0; s < colors.Size(); ++s)
        {
            for (usize t = 0; t < sliceBytes / 4; ++t)
            {
                u8* p = data->texels.Data() + s * sliceBytes + t * 4;
                p[0] = static_cast<u8>(colors[s].x * 255.0f);
                p[1] = static_cast<u8>(colors[s].y * 255.0f);
                p[2] = static_cast<u8>(colors[s].z * 255.0f);
                p[3] = 255;
            }
        }
        return data;
    }

    struct ShadowProbe
    {
        bool valid = false;
        f64 leftGround = 0, rightGround = 0, total = 0; // luma over the flat ground bands
    };

    // Renders the ridge top-down under a low +X sun, optionally with a CSM ShadowSystem, and measures
    // the luma of the flat ground on each side of the ridge (excluding the bright ridge stripe).
    ShadowProbe RenderShadowProbe(rhi::Device& device, bool shadowsEnabled)
    {
        ShadowProbe probe;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return probe;
        }
        {
            shaders::ShaderSystem& shaderSystem = *host.System();
            engine::terrain::TerrainRenderer renderer(device, shaderSystem, /*framesInFlight*/ 2);
            REQUIRE(renderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&renderer);

            UniquePtr<ShadowSystem> shadows;
            if (shadowsEnabled)
            {
                shadows = MakeUnique<ShadowSystem>(DefaultAllocator(), device, /*framesInFlight*/ 2);
                REQUIRE(shadows->Initialize().IsOk());
            }
            RenderFrame frame(DefaultAllocator(), device, registry, /*framesInFlight*/ 2, /*clusters*/ nullptr,
                              /*tonemap*/ nullptr, shadows.Get());

            RefPtr<hf::Heightfield> ridge = MakeRidge();
            Array<tmodel::TerrainChunk> chunks;
            tmodel::BuildChunks(*ridge, chunks);
            tmodel::TerrainQuadtree tree;
            tree.Build(Span<const tmodel::TerrainChunk>{chunks.Data(), chunks.Size()},
                       tmodel::ChunksPerSide(ridge->Size()));
            engine::terrain::TerrainHeightTextureCache heightCache;
            rhi::TextureView* heightView = heightCache.GetOrCreate(device, *ridge, 1);
            REQUIRE(heightView != nullptr);

            // Sun low from +X: the direction to the light; the travel direction is its negation.
            const Float3 toLight = Normalized(Float3{0.9f, 0.42f, 0.0f});
            const Float3 travel = toLight * -1.0f;

            static const f32 thresholds[] = {1.0f, 0.25f, 0.08f, 0.03f, 0.012f, 0.005f, 0.002f};
            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            GpuLight sun{};
            sun.type = 0.0f;
            sun.directionWS = travel;
            sun.color = Float3{1.0f, 1.0f, 1.0f};
            sun.intensity = 1.0f;
            scene.AddLight(sun);
            DirectionalShadow ds{};
            ds.direction = travel;
            ds.valid = true;
            scene.SetDirectionalShadow(ds);
            scene.SetShadowSettings(SceneShadowSettings{800.0f, 0.5f, 20.0f}); // reach, split, fade

            engine::terrain::TerrainRenderData* rd = scene.Add<engine::terrain::TerrainRenderData>();
            REQUIRE(rd != nullptr);
            rd->category = RenderCategories::Opaque;
            rd->rendererId = renderer.RendererId();
            rd->chunks = chunks.Data();
            rd->nodes = tree.Nodes().Data();
            rd->nodeCount = static_cast<u32>(tree.Nodes().Size());
        rd->chunkCount = static_cast<u32>(chunks.Size());
            rd->heightView = heightView;
            rd->chunkToWorld = Float4x4::Identity();
            rd->gridSize = ridge->Size();
            rd->worldSizeXZ = ridge->WorldSize();
            rd->minY = ridge->MinY();
            rd->maxY = ridge->MaxY();
            for (u32 i = 0; i < 7; ++i)
            {
                rd->thresholds[i] = thresholds[i];
            }
            rd->thresholdCount = 7;
            rd->worldCenter = Float3{0.0f, 30.0f, 0.0f};
            rd->worldRadius = 400.0f;

            ViewCamera camera;
            camera.view = Float4x4::LookAtRH(Float3{0.0f, 260.0f, 0.01f}, Float3{0.0f, 0.0f, 0.0f},
                                             Float3{0.0f, 0.0f, 1.0f});
            camera.projection = Float4x4::PerspectiveFovRH(1.0f, 1.0f, 1.0f, 900.0f);

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"terrain.shadowprobe";
            rhi::Texture* target = nullptr;
            REQUIRE(device.CreateTexture(td, target).IsOk());
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            rhi::TextureView* targetView = nullptr;
            REQUIRE(device.CreateTextureView(target, vd, targetView).IsOk());

            rhi::CommandPool* pool = nullptr;
            REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
            rhi::Fence* fence = nullptr;
            REQUIRE(device.CreateFence(0, fence).IsOk());
            rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);
            REQUIRE(queue != nullptr);

            ViewSettings settings;
            settings.clear = rhi::ClearColor::Black();
            settings.targetTexture = target;
            settings.targetFinalState = rhi::ResourceState::CopySrc;
            settings.post.bloomEnabled = false;

            for (u32 i = 0; i < 2; ++i)
            {
                rhi::CommandEncoder* encoder = nullptr;
                REQUIRE(pool->CreateEncoder(encoder).IsOk());
                settings.targetCurrentState =
                    (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                frame.Begin(*encoder, i % 2);
                frame.AddView(scene, camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm,
                              kSize, kSize);
                frame.End();
                rhi::CommandBuffer* commandBuffer = encoder->Finish();
                REQUIRE(commandBuffer != nullptr);
                rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
                queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
                REQUIRE(fence->Wait(i + 1, ~0ull));
                pool->DestroyEncoder(encoder); // fence waited: safe to destroy
            }

            const testsupport::CapturedImage img =
                testsupport::Readback(device, target, kSize, kSize);
            REQUIRE(img.valid);
            // Two flat-ground bands well clear of the center ridge stripe.
            for (u32 y = 0; y < kSize; ++y)
            {
                for (u32 x = 0; x < kSize; ++x)
                {
                    const f64 luma = static_cast<f64>(img.Luma(x, y));
                    probe.total += luma;
                    const f32 fx = static_cast<f32>(x) / kSize;
                    if (fx > 0.12f && fx < 0.38f)
                    {
                        probe.leftGround += luma;
                    }
                    else if (fx > 0.62f && fx < 0.88f)
                    {
                        probe.rightGround += luma;
                    }
                }
            }
            probe.valid = true;

            device.WaitIdle();
            heightCache.Clear(device); // owns the height texture/view; must release before the device
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return probe;
    }
}

TEST_CASE("terrain probe: a lit dome renders + shades on Vulkan")
{
    rhi::Backend* vulkan = nullptr;
    rhi::Device* device = MakeVulkan(vulkan);
    if (device == nullptr)
    {
        MESSAGE("Vulkan unavailable - terrain probe skipped");
        if (vulkan != nullptr)
        {
            vulkan->Destroy();
        }
        return;
    }

    ProbeCfg cfg;
    cfg.terrain = MakeDome();
    const Probe p = RenderTerrainProbe(*device, cfg);
    REQUIRE(p.valid);
    const f64 pixels = static_cast<f64>(kSize) * kSize;
    std::printf("[terrain-probe] filled=%u/%.0f left=%.0f right=%.0f top=%.0f bottom=%.0f\n",
                p.filled, pixels, p.leftLuma, p.rightLuma, p.topLuma, p.bottomLuma);

    CHECK(static_cast<f64>(p.filled) > pixels * 0.5); // it rendered and covers the view
    const f64 axisAsymmetry = Abs(p.leftLuma - p.rightLuma) + Abs(p.topLuma - p.bottomLuma);
    CHECK(axisAsymmetry > p.total * 0.02); // directional shading, not a flat fill

    device->Destroy();
    vulkan->Destroy();
}

TEST_CASE("terrain probe: the scene's directional sun drives the shading (flip inverts it)")
{
    rhi::Backend* vulkan = nullptr;
    rhi::Device* device = MakeVulkan(vulkan);
    if (device == nullptr)
    {
        MESSAGE("Vulkan unavailable - terrain sun probe skipped");
        if (vulkan != nullptr)
        {
            vulkan->Destroy();
        }
        return;
    }

    const Float3 toLightPlusX = Normalized(Float3{0.85f, 0.5f, 0.0f});
    const Float3 toLightMinusX = Normalized(Float3{-0.85f, 0.5f, 0.0f});
    ProbeCfg cfg;
    cfg.terrain = MakeDome();
    cfg.toLight = &toLightPlusX;
    const Probe a = RenderTerrainProbe(*device, cfg);
    cfg.toLight = &toLightMinusX;
    const Probe b = RenderTerrainProbe(*device, cfg);
    REQUIRE(a.valid);
    REQUIRE(b.valid);

    const f64 da = a.leftLuma - a.rightLuma;
    const f64 db = b.leftLuma - b.rightLuma;
    std::printf("[terrain-sun] +X: L-R=%.0f   -X: L-R=%.0f\n", da, db);
    CHECK(da * db < 0.0);            // asymmetry inverted with the sun
    CHECK(Abs(da) > a.total * 0.02); // each a real, sizable asymmetry
    CHECK(Abs(db) > b.total * 0.02);

    device->Destroy();
    vulkan->Destroy();
}

TEST_CASE("terrain probe: every backend matches Vulkan (cross-backend shader-cook parity)")
{
    // Vulkan is the reference; every other backend we support must render the same dome pixel for
    // pixel (within driver rounding). This is where a shader-cook divergence surfaces: the integer
    // Load on the R16Uint height texture (which WebGPU/WGSL portability depends on), the GBuffer MRT
    // layout, or the normal and lit path differing between the SPIR-V, WGSL (WebGPU, desktop and
    // web) and DXIL (DX12) frontends. GPUs are checked at runtime; each backend skips if absent.
    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
#ifdef OPTION_HAS_DX12
    rhi::Backend* dx12 = nullptr;
    (void)rhi::dx12::CreateDxBackend(rhi::dx12::DxBackendDesc{}, dx12);
#endif

    const Probe reference = DomeProbeOn(vulkan);
    if (!reference.valid)
    {
        MESSAGE("Vulkan unavailable - cross-backend terrain probe skipped");
    }
    else
    {
        if (const Probe w = DomeProbeOn(webgpu); w.valid)
        {
            CheckMatchesReference("webgpu", reference, w);
        }
        else
        {
            MESSAGE("WebGPU unavailable - skipped");
        }
#ifdef OPTION_HAS_DX12
        if (const Probe d = DomeProbeOn(dx12); d.valid)
        {
            CheckMatchesReference("dx12", reference, d);
        }
        else
        {
            MESSAGE("DX12 unavailable - skipped");
        }
#endif
    }

    if (vulkan != nullptr) { vulkan->Destroy(); }
    if (webgpu != nullptr) { webgpu->Destroy(); }
#ifdef OPTION_HAS_DX12
    if (dx12 != nullptr) { dx12->Destroy(); }
#endif
}

TEST_CASE("terrain probe: the ridge casts a CSM shadow onto the flat ground (cast + receive)")
{
    rhi::Backend* vulkan = nullptr;
    rhi::Device* device = MakeVulkan(vulkan);
    if (device == nullptr)
    {
        MESSAGE("Vulkan unavailable - terrain shadow probe skipped");
        if (vulkan != nullptr) { vulkan->Destroy(); }
        return;
    }

    // Same scene, shadows off then on. The flat ground has one normal, so without shadows both bands
    // are equally lit; with the CSM the ridge darkens one side (its cast shadow). Terrain has to both
    // cast (into the cascade) and receive (sample it) for this to appear.
    const ShadowProbe off = RenderShadowProbe(*device, /*shadowsEnabled*/ false);
    const ShadowProbe on = RenderShadowProbe(*device, /*shadowsEnabled*/ true);
    REQUIRE(off.valid);
    REQUIRE(on.valid);

    const f64 offAsym = Abs(off.leftGround - off.rightGround) / (off.leftGround + off.rightGround);
    const f64 onAsym = Abs(on.leftGround - on.rightGround) / (on.leftGround + on.rightGround);
    std::printf("[terrain-shadow] off L=%.0f R=%.0f (asym %.3f) | on L=%.0f R=%.0f (asym %.3f) "
                "| total off=%.0f on=%.0f\n",
                off.leftGround, off.rightGround, offAsym, on.leftGround, on.rightGround, onAsym,
                off.total, on.total);

    CHECK(offAsym < 0.06);              // flat ground, no shadow -> both sides ~equal
    CHECK(onAsym > 0.15);               // the cast shadow darkens one side
    CHECK(on.total < off.total * 0.99); // shadows only remove light

    device->Destroy();
    vulkan->Destroy();

    // WebGPU leg: the cascade-cast pass is where binding the live CSM while it is attached would be
    // a hazard. Vulkan tolerates the read and write bind; WebGPU rejects the pass. Running the same
    // shadowed scene here catches it: with that bind, WebGPU drops the cast and receive work and the
    // asymmetry vanishes.
    rhi::Backend* webgpuBackend = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpuBackend, DefaultAllocator());
    rhi::Device* webgpuDevice =
        webgpuBackend != nullptr ? testsupport::MakeTestDevice(webgpuBackend) : nullptr;
    if (webgpuDevice == nullptr)
    {
        MESSAGE("WebGPU unavailable - terrain shadow probe WebGPU leg skipped");
        if (webgpuBackend != nullptr) { webgpuBackend->Destroy(); }
        return;
    }
    const ShadowProbe wOn = RenderShadowProbe(*webgpuDevice, /*shadowsEnabled*/ true);
    REQUIRE(wOn.valid);
    const f64 wOnAsym =
        Abs(wOn.leftGround - wOn.rightGround) / (wOn.leftGround + wOn.rightGround);
    std::printf("[terrain-shadow-webgpu] on L=%.0f R=%.0f (asym %.3f) total=%.0f\n", wOn.leftGround,
                wOn.rightGround, wOnAsym, wOn.total);
    CHECK(wOnAsym > 0.15); // the cast shadow survives the WebGPU binding rules
    webgpuDevice->Destroy();
    webgpuBackend->Destroy();
}

TEST_CASE("terrain probe: SIX palette layers render distinct stripes (R5 - the 4-layer cap is gone)")
{
    // Flat terrain, a 6-stripe one-hot weights raster (stripe i = palette layer i) and a 6-colour
    // palette array: six distinct layers on one terrain. Every stripe shows its own palette colour,
    // matching across Vulkan and WebGPU. GPU objects come through the production caches (splat
    // pair, palette array, tileScale buffer).
    static const Float3 kColors[kBands] = {
        Float3{0.86f, 0.12f, 0.12f}, // red
        Float3{0.12f, 0.86f, 0.12f}, // green
        Float3{0.12f, 0.12f, 0.86f}, // blue
        Float3{0.86f, 0.86f, 0.12f}, // yellow
        Float3{0.12f, 0.86f, 0.86f}, // cyan
        Float3{0.86f, 0.12f, 0.86f}, // magenta
    };

    auto run = [](rhi::Backend* backend) -> Probe
    {
        rhi::Device* dev = (backend != nullptr) ? testsupport::MakeTestDevice(backend) : nullptr;
        if (dev == nullptr)
        {
            return Probe{};
        }
        Probe p;
        {
            engine::terrain::TerrainSplatTextureCache splatCache;
            engine::terrain::TerrainPaletteTextureCache paletteCache;
            RefPtr<tmodel::SplatWeights> sw = MakeStripeWeights(kBands);
            RefPtr<tmodel::TerrainPaletteData> palette =
                MakePaletteData(Span<const Float3>{kColors, kBands});
            f32 scales[kBands];
            for (u32 i = 0; i < kBands; ++i)
            {
                scales[i] = 1000.0f; // ~1 tile -> solid colour per stripe
            }

            ProbeCfg cfg;
            cfg.terrain = MakeFlat();
            const engine::terrain::SplatTextureViews views =
                splatCache.GetOrCreate(*dev, *sw, sw->Version());
            const engine::terrain::PaletteGpu gpu =
                paletteCache.GetOrCreate(*dev, *palette, Span<const f32>{scales, kBands});
            REQUIRE(views.weightView != nullptr);
            REQUIRE(gpu.arrayView != nullptr);
            cfg.weightView = views.weightView;
            cfg.indexView = views.indexView;
            cfg.paletteArrayView = gpu.arrayView;
            cfg.tileScaleBuffer = gpu.tileScaleBuffer;
            cfg.tileScaleGeneration = gpu.generation;
            cfg.paletteCount = kBands;
            p = RenderTerrainProbe(*dev, cfg);

            splatCache.Clear(*dev);
            paletteCache.Clear(*dev);
        }
        dev->Destroy();
        return p;
    };

    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());

    const Probe v = run(vulkan);
    if (!v.valid)
    {
        MESSAGE("Vulkan unavailable - terrain top-K stripe probe skipped");
        if (vulkan != nullptr) { vulkan->Destroy(); }
        if (webgpu != nullptr) { webgpu->Destroy(); }
        return;
    }
    for (u32 b = 0; b < kBands; ++b)
    {
        std::printf("[terrain-topk] vk band %u: R=%.0f G=%.0f B=%.0f\n", b, v.bandR[b],
                    v.bandG[b], v.bandB[b]);
    }

    // Classify each screen band by which channels are "on" (> 60% of the band max) and require the
    // palette order, forward or mirrored (flip-agnostic).
    const auto bandClass = [](const Probe& p, u32 b) -> u32
    {
        const f64 mx = Max(p.bandR[b], Max(p.bandG[b], p.bandB[b]));
        u32 bits = 0;
        bits |= (p.bandR[b] > 0.6 * mx) ? 1u : 0u;
        bits |= (p.bandG[b] > 0.6 * mx) ? 2u : 0u;
        bits |= (p.bandB[b] > 0.6 * mx) ? 4u : 0u;
        return bits;
    };
    const u32 expected[kBands] = {1u, 2u, 4u, 3u, 6u, 5u}; // R,G,B,R+G,G+B,R+B
    bool forward = true;
    bool mirrored = true;
    for (u32 b = 0; b < kBands; ++b)
    {
        forward = forward && (bandClass(v, b) == expected[b]);
        mirrored = mirrored && (bandClass(v, b) == expected[kBands - 1 - b]);
    }
    CHECK((forward || mirrored)); // all six distinct layers appear, in palette order

    const Probe w = run(webgpu);
    if (w.valid)
    {
        for (u32 b = 0; b < kBands; ++b)
        {
            CHECK(w.bandR[b] == doctest::Approx(v.bandR[b]).epsilon(0.05));
            CHECK(w.bandG[b] == doctest::Approx(v.bandG[b]).epsilon(0.05));
            CHECK(w.bandB[b] == doctest::Approx(v.bandB[b]).epsilon(0.05));
        }
    }
    else
    {
        MESSAGE("WebGPU unavailable - top-K stripe cross-check skipped");
    }

    if (vulkan != nullptr) { vulkan->Destroy(); }
    if (webgpu != nullptr) { webgpu->Destroy(); }
}

TEST_CASE("terrain probe: painting the CPU weights re-uploads and changes the blended pixel")
{
    // The editor's Splat Paint loop end to end: CPU SplatWeights, then the version-keyed GPU splat
    // pair, then the top-K blend. All-zero weights over a red base render red; PaintTopK raises
    // palette 0 (blue) and bumps the version, the cache re-uploads a new pair, and the same terrain
    // now renders blue, so the paint (and, in reverse, erase revealing the base) reached the GPU.
    // Runs on Vulkan and WebGPU.
    auto run = [](rhi::Backend* backend, Probe& before, Probe& after)
    {
        rhi::Device* dev = (backend != nullptr) ? testsupport::MakeTestDevice(backend) : nullptr;
        if (dev == nullptr)
        {
            return;
        }
        {
            engine::terrain::TerrainSplatTextureCache cache;
            engine::terrain::TerrainPaletteTextureCache paletteCache;
            RefPtr<tmodel::SplatWeights> sw =
                MakeRef<tmodel::SplatWeights>(DefaultAllocator(), 8, 8); // all-zero = pure base

            RefPtr<texture::Texture> red = MakeSolid(*dev, 220, 30, 30); // the base albedo
            const Float3 blue{0.12f, 0.12f, 0.86f};                      // palette 0
            RefPtr<tmodel::TerrainPaletteData> palette =
                MakePaletteData(Span<const Float3>{&blue, 1});
            const f32 scale = 1000.0f;
            const engine::terrain::PaletteGpu gpu =
                paletteCache.GetOrCreate(*dev, *palette, Span<const f32>{&scale, 1});
            REQUIRE(gpu.arrayView != nullptr);

            ProbeCfg cfg;
            cfg.terrain = MakeFlat();
            cfg.baseAlbedoView = red->View();
            cfg.baseTileScale = 1000.0f; // ~1 tile -> solid colour
            cfg.paletteArrayView = gpu.arrayView;
            cfg.tileScaleBuffer = gpu.tileScaleBuffer;
            cfg.tileScaleGeneration = gpu.generation;
            cfg.paletteCount = 1;

            // Before: all-zero weights show the base (red) across the terrain.
            engine::terrain::SplatTextureViews views =
                cache.GetOrCreate(*dev, *sw, sw->Version());
            cfg.weightView = views.weightView;
            cfg.indexView = views.indexView;
            before = RenderTerrainProbe(*dev, cfg);

            // Paint palette 0 over the whole footprint (a few dabs to converge one-hot), then
            // re-fetch: the version bump makes the cache rebuild and re-upload a new pair.
            for (i32 i = 0; i < 24; ++i)
            {
                (void)tmodel::PaintTopK(*sw, 0.5f, 0.5f, 2.0f, 2.0f, 0u, 1.0f);
            }
            views = cache.GetOrCreate(*dev, *sw, sw->Version());
            cfg.weightView = views.weightView;
            cfg.indexView = views.indexView;
            after = RenderTerrainProbe(*dev, cfg);

            cache.Clear(*dev);
            paletteCache.Clear(*dev);
        }
        dev->Destroy();
    };

    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());

    Probe vBefore, vAfter;
    run(vulkan, vBefore, vAfter);
    if (!vBefore.valid || !vAfter.valid)
    {
        MESSAGE("Vulkan unavailable - splat re-upload probe skipped");
        if (vulkan != nullptr) { vulkan->Destroy(); }
        if (webgpu != nullptr) { webgpu->Destroy(); }
        return;
    }
    std::printf("[terrain-splat-repaint] vk before(R=%.0f B=%.0f) after(R=%.0f B=%.0f)\n",
                vBefore.leftR, vBefore.leftB, vAfter.leftR, vAfter.leftB);

    // Seeded: red-dominant; after painting layer 1: blue-dominant. The blend changed, so the paint
    // re-uploaded to the GPU.
    CHECK(vBefore.leftR > vBefore.leftB * 1.5);
    CHECK(vBefore.rightR > vBefore.rightB * 1.5);
    CHECK(vAfter.leftB > vAfter.leftR * 1.5);
    CHECK(vAfter.rightB > vAfter.rightR * 1.5);

    Probe wBefore, wAfter;
    run(webgpu, wBefore, wAfter);
    if (wBefore.valid && wAfter.valid)
    {
        std::printf("[terrain-splat-repaint] wg before(R=%.0f B=%.0f) after(R=%.0f B=%.0f)\n",
                    wBefore.leftR, wBefore.leftB, wAfter.leftR, wAfter.leftB);
        CHECK(wBefore.leftR == doctest::Approx(vBefore.leftR).epsilon(0.05));
        CHECK(wAfter.leftB == doctest::Approx(vAfter.leftB).epsilon(0.05));
    }
    else
    {
        MESSAGE("WebGPU unavailable - splat re-upload cross-check skipped");
    }

    if (vulkan != nullptr) { vulkan->Destroy(); }
    if (webgpu != nullptr) { webgpu->Destroy(); }
}

TEST_CASE("terrain probe: a base normal map perturbs the flat-ground shading (R5 - Vk + WebGPU)")
{
    // A flat terrain has one geometric normal, so a directional sun shades it uniformly. A base
    // normal map tilted toward world +X makes the ground respond to sun direction: a +X sun
    // brightens it (compared with the flat-normal control) and a -X sun darkens it. Checks that the
    // analytic tangent frame and the blend perturb the normal in the correct direction, and that the
    // SampleGrad-on-array normal tap matches across Vulkan and WebGPU.
    auto run = [](rhi::Backend* backend, const Float3& toLight, bool withNormal) -> Probe
    {
        rhi::Device* dev = (backend != nullptr) ? testsupport::MakeTestDevice(backend) : nullptr;
        if (dev == nullptr)
        {
            return Probe{};
        }
        Probe p;
        {
            RefPtr<texture::Texture> albedo = MakeSolid(*dev, 170, 170, 170);
            // Tangent-space normal (0.6, 0, 0.8): under the flat-terrain frame (T=+X, up=+Y) this
            // tilts the world normal toward +X. Encode n*0.5+0.5 -> (204, 128, 229).
            RefPtr<texture::Texture> normal = MakeSolid(*dev, 204, 128, 229);
            ProbeCfg cfg;
            cfg.terrain = MakeFlat();
            cfg.toLight = &toLight;
            cfg.baseAlbedoView = albedo->View();
            cfg.baseTileScale = 1.0f;
            if (withNormal)
            {
                cfg.baseNormalView = normal->View();
            }
            p = RenderTerrainProbe(*dev, cfg);
        }
        dev->Destroy();
        return p;
    };

    const Float3 plusX = Normalized(Float3{0.85f, 0.5f, 0.0f});
    const Float3 minusX = Normalized(Float3{-0.85f, 0.5f, 0.0f});

    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());

    const Probe aPlus = run(vulkan, plusX, true); // normal, aligned sun
    if (!aPlus.valid)
    {
        MESSAGE("Vulkan unavailable - terrain normal-map probe skipped");
        if (vulkan != nullptr) { vulkan->Destroy(); }
        if (webgpu != nullptr) { webgpu->Destroy(); }
        return;
    }
    const Probe aMinus = run(vulkan, minusX, true);  // normal, opposed sun
    const Probe cPlus = run(vulkan, plusX, false);   // flat control, +X
    const Probe cMinus = run(vulkan, minusX, false); // flat control, -X

    std::printf("[terrain-normal] normal +X=%.0f -X=%.0f | flat +X=%.0f -X=%.0f\n", aPlus.total,
                aMinus.total, cPlus.total, cMinus.total);

    // Flat control: +X and -X sun shade the one-normal ground ~equally.
    CHECK(cPlus.total == doctest::Approx(cMinus.total).epsilon(0.06));
    // The normal map makes it directional: aligned sun brighter, opposed sun darker, than flat.
    CHECK(aPlus.total > cPlus.total * 1.10);
    CHECK(aMinus.total < cMinus.total * 0.90);

    // WebGPU parity on the aligned case (the SampleGrad-on-array normal tap is where naga diverges).
    const Probe wPlus = run(webgpu, plusX, true);
    if (wPlus.valid)
    {
        CHECK(wPlus.total == doctest::Approx(aPlus.total).epsilon(0.05));
    }
    else
    {
        MESSAGE("WebGPU unavailable - terrain normal-map parity skipped");
    }

    if (vulkan != nullptr) { vulkan->Destroy(); }
    if (webgpu != nullptr) { webgpu->Destroy(); }
}

TEST_CASE("terrain probe: ARRAY normal + ORM blend through top-K, and the R2 rotation pin")
{
    // The per-layer (Texture2DArray) PBR path, which the base-normal probe does not touch, and
    // where the WGSL cook can diverge (SampleGrad on the second and third array):
    //   1. a one-hot painted layer whose array normal tilts toward local +X shades directionally
    //      (aligned sun brighter than opposed);
    //   2. an ORM array with AO=0 removes the ambient term (darker than AO=1);
    //   3. on a 90-degree-rotated terrain the perturbation follows the chunk frame: the world-X
    //      sun pair becomes symmetric and the world-Z pair carries the asymmetry (a world-axis
    //      tangent frame would keep it on X and shear);
    //   4. WebGPU parity on the aligned case.
    auto run = [](rhi::Backend* backend, const Float3& toLight, u8 ao,
                  const Float4x4& chunkToWorld) -> Probe
    {
        rhi::Device* dev = (backend != nullptr) ? testsupport::MakeTestDevice(backend) : nullptr;
        if (dev == nullptr)
        {
            return Probe{};
        }
        Probe p;
        {
            engine::terrain::TerrainSplatTextureCache splatCache;
            engine::terrain::TerrainPaletteTextureCache paletteCache;
            RefPtr<tmodel::SplatWeights> sw = MakeStripeWeights(1); // one-hot layer 0 everywhere
            const Float3 gray{0.66f, 0.66f, 0.66f};
            RefPtr<tmodel::TerrainPaletteData> palette =
                MakePaletteData(Span<const Float3>{&gray, 1});
            // Array normal: tangent-space (0.6, 0, 0.8), encoded (204, 128, 229); tilts the shading
            // normal toward the map's U axis (local +X rotated by chunkToWorld).
            const usize sliceBytes = tmodel::TerrainPaletteData::SliceBytes(4, 1);
            palette->normalTexels.Resize(sliceBytes);
            palette->ormTexels.Resize(sliceBytes);
            for (usize t = 0; t < sliceBytes / 4; ++t)
            {
                u8* n = palette->normalTexels.Data() + t * 4;
                n[0] = 204; n[1] = 128; n[2] = 229; n[3] = 255;
                u8* o = palette->ormTexels.Data() + t * 4;
                o[0] = ao; o[1] = 255; o[2] = 0; o[3] = 255;
            }
            REQUIRE(palette->HasNormal());
            REQUIRE(palette->HasOrm());

            const f32 scale = 1000.0f;
            const engine::terrain::PaletteGpu gpu =
                paletteCache.GetOrCreate(*dev, *palette, Span<const f32>{&scale, 1});
            REQUIRE(gpu.arrayView != nullptr);
            REQUIRE(gpu.normalArrayView != nullptr);
            REQUIRE(gpu.ormArrayView != nullptr);
            const engine::terrain::SplatTextureViews views =
                splatCache.GetOrCreate(*dev, *sw, sw->Version());
            REQUIRE(views.weightView != nullptr);

            ProbeCfg cfg;
            cfg.terrain = MakeFlat();
            cfg.toLight = &toLight;
            cfg.chunkToWorld = chunkToWorld;
            cfg.weightView = views.weightView;
            cfg.indexView = views.indexView;
            cfg.paletteArrayView = gpu.arrayView;
            cfg.normalArrayView = gpu.normalArrayView;
            cfg.ormArrayView = gpu.ormArrayView;
            cfg.tileScaleBuffer = gpu.tileScaleBuffer;
            cfg.tileScaleGeneration = gpu.generation;
            cfg.paletteCount = 1;
            p = RenderTerrainProbe(*dev, cfg);

            splatCache.Clear(*dev);
            paletteCache.Clear(*dev);
        }
        dev->Destroy();
        return p;
    };

    const Float3 plusX = Normalized(Float3{0.85f, 0.5f, 0.0f});
    const Float3 minusX = Normalized(Float3{-0.85f, 0.5f, 0.0f});
    const Float3 plusZ = Normalized(Float3{0.0f, 0.5f, 0.85f});
    const Float3 minusZ = Normalized(Float3{0.0f, 0.5f, -0.85f});
    const Float4x4 identity = Float4x4::Identity();
    const Float4x4 rotated = Float4x4::RotationY(kPi * 0.5f);

    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());

    const Probe nPlus = run(vulkan, plusX, 255, identity);
    if (!nPlus.valid)
    {
        MESSAGE("Vulkan unavailable - terrain array-PBR probe skipped");
        if (vulkan != nullptr) { vulkan->Destroy(); }
        if (webgpu != nullptr) { webgpu->Destroy(); }
        return;
    }
    const Probe nMinus = run(vulkan, minusX, 255, identity);
    const Probe aoDark = run(vulkan, plusX, 0, identity);
    std::printf("[terrain-arraypbr] +X=%.0f -X=%.0f ao0=%.0f\n", nPlus.total, nMinus.total,
                aoDark.total);
    // 1. The array normal makes flat ground directional (aligned sun brighter than opposed).
    CHECK(nPlus.total > nMinus.total * 1.10);
    // 2. AO=0 removes the ambient term (strictly darker, with a margin).
    CHECK(aoDark.total < nPlus.total * 0.95);

    // 3. Rotate the terrain 90 degrees about Y: the perturbation follows the chunk frame, so the
    // X-sun pair evens out and the Z-sun pair carries the asymmetry.
    const Probe rotPX = run(vulkan, plusX, 255, rotated);
    const Probe rotMX = run(vulkan, minusX, 255, rotated);
    const Probe rotPZ = run(vulkan, plusZ, 255, rotated);
    const Probe rotMZ = run(vulkan, minusZ, 255, rotated);
    std::printf("[terrain-arraypbr] rot +X=%.0f -X=%.0f +Z=%.0f -Z=%.0f\n", rotPX.total,
                rotMX.total, rotPZ.total, rotMZ.total);
    CHECK(rotPX.total == doctest::Approx(rotMX.total).epsilon(0.05)); // the asymmetry left world X
    const f64 zAsym = Abs(rotPZ.total - rotMZ.total);
    CHECK(zAsym > 0.10 * rotPZ.total); // and moved to world Z (flip-agnostic magnitude)

    // 4. WebGPU parity on the aligned array-normal case (where naga can diverge).
    const Probe wPlus = run(webgpu, plusX, 255, identity);
    if (wPlus.valid)
    {
        CHECK(wPlus.total == doctest::Approx(nPlus.total).epsilon(0.05));
    }
    else
    {
        MESSAGE("WebGPU unavailable - array-PBR parity skipped");
    }

    if (vulkan != nullptr) { vulkan->Destroy(); }
    if (webgpu != nullptr) { webgpu->Destroy(); }
}

TEST_CASE("terrain probe: height-blend biases the top-K toward the tallest layer (Vk + WebGPU)")
{
    // A flat 2-layer terrain, painted 50/50 (a tie the linear blend renders as an equal red/blue
    // mix). Layer 0 is red, layer 1 blue; each carries a constant height slice. With height blend on,
    // the tie breaks toward whichever layer is taller; swapping the tall slice flips the winner. With
    // equal heights the greater weight wins (the crossover sits at the weight tie, so an
    // implementation that dropped the weight term from the score fails here). Off (no height array)
    // holds the linear 50/50 mix, showing the height binding is what tips the result.
    auto run = [](rhi::Backend* backend, u8 h0, u8 h1, u8 w0, u8 w1, bool bindHeight,
                  f32 contrast) -> Probe
    {
        rhi::Device* dev = (backend != nullptr) ? testsupport::MakeTestDevice(backend) : nullptr;
        if (dev == nullptr)
        {
            return Probe{};
        }
        Probe p;
        {
            engine::terrain::TerrainSplatTextureCache splatCache;
            engine::terrain::TerrainPaletteTextureCache paletteCache;
            RefPtr<tmodel::SplatWeights> sw = MakeTwoLayerWeights(w0, w1);
            const Float3 colors[2] = {Float3{0.9f, 0.05f, 0.05f}, Float3{0.05f, 0.05f, 0.9f}};
            RefPtr<tmodel::TerrainPaletteData> palette =
                MakePaletteData(Span<const Float3>{colors, 2});
            // Constant per-layer height slices (.r used): layer 0 = h0, layer 1 = h1.
            const usize sliceBytes =
                tmodel::TerrainPaletteData::SliceBytes(palette->sliceSize, palette->mipCount);
            palette->heightTexels.Resize(sliceBytes * 2);
            for (usize t = 0; t < sliceBytes; t += 4)
            {
                u8* p0 = palette->heightTexels.Data() + t;
                p0[0] = h0; p0[1] = h0; p0[2] = h0; p0[3] = 255;
                u8* p1 = palette->heightTexels.Data() + sliceBytes + t;
                p1[0] = h1; p1[1] = h1; p1[2] = h1; p1[3] = 255;
            }
            f32 scales[2] = {1000.0f, 1000.0f};

            ProbeCfg cfg;
            cfg.terrain = MakeFlat();
            const engine::terrain::SplatTextureViews views =
                splatCache.GetOrCreate(*dev, *sw, sw->Version());
            const engine::terrain::PaletteGpu gpu =
                paletteCache.GetOrCreate(*dev, *palette, Span<const f32>{scales, 2});
            REQUIRE(views.weightView != nullptr);
            REQUIRE(gpu.arrayView != nullptr);
            if (bindHeight)
            {
                REQUIRE(gpu.heightArrayView != nullptr); // HasHeight(): the cache built the array
            }
            cfg.weightView = views.weightView;
            cfg.indexView = views.indexView;
            cfg.paletteArrayView = gpu.arrayView;
            cfg.tileScaleBuffer = gpu.tileScaleBuffer;
            cfg.tileScaleGeneration = gpu.generation;
            cfg.paletteCount = 2;
            cfg.heightArrayView = bindHeight ? gpu.heightArrayView : nullptr;
            cfg.heightBlendContrast = contrast;
            p = RenderTerrainProbe(*dev, cfg);

            splatCache.Clear(*dev);
            paletteCache.Clear(*dev);
        }
        dev->Destroy();
        return p;
    };

    const auto R = [](const Probe& p) { return p.leftR + p.rightR; };
    const auto B = [](const Probe& p) { return p.leftB + p.rightB; };

    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());

    const Probe redTall = run(vulkan, 255, 0, 128, 128, true, 0.15f);
    if (!redTall.valid)
    {
        MESSAGE("Vulkan unavailable - terrain height-blend probe skipped");
        if (vulkan != nullptr) { vulkan->Destroy(); }
        if (webgpu != nullptr) { webgpu->Destroy(); }
        return;
    }
    const Probe blueTall = run(vulkan, 0, 255, 128, 128, true, 0.15f);
    const Probe off = run(vulkan, 255, 0, 128, 128, false, 0.15f);      // linear 50/50 control
    const Probe weightWins = run(vulkan, 128, 128, 180, 60, true, 0.15f); // equal height

    std::printf("[terrain-heightblend] redTall(R=%.0f B=%.0f) blueTall(R=%.0f B=%.0f) "
                "off(R=%.0f B=%.0f) weightWins(R=%.0f B=%.0f)\n",
                R(redTall), B(redTall), R(blueTall), B(blueTall), R(off), B(off), R(weightWins),
                B(weightWins));

    // Taller layer wins the 50/50 tie; swapping the tall slice flips the winner.
    CHECK(R(redTall) > B(redTall) * 1.5);
    CHECK(B(blueTall) > R(blueTall) * 1.5);
    // Off control: the same 50/50 weights render an about equal red/blue mix (no height steering),
    CHECK(R(off) == doctest::Approx(B(off)).epsilon(0.15));
    // and turning height blend on is what pushed red up (binding the array changed the result).
    CHECK(R(redTall) > R(off) * 1.2);
    // Equal heights: the greater weight wins (the score keeps the weight term).
    CHECK(R(weightWins) > B(weightWins) * 1.5);

    // WebGPU parity on the red-tall case (SampleGrad on the height array is where naga can diverge).
    const Probe wRedTall = run(webgpu, 255, 0, 128, 128, true, 0.15f);
    if (wRedTall.valid)
    {
        CHECK(wRedTall.total == doctest::Approx(redTall.total).epsilon(0.05));
        CHECK(R(wRedTall) == doctest::Approx(R(redTall)).epsilon(0.05));
    }
    else
    {
        MESSAGE("WebGPU unavailable - terrain height-blend parity skipped");
    }

    if (vulkan != nullptr) { vulkan->Destroy(); }
    if (webgpu != nullptr) { webgpu->Destroy(); }
}

TEST_CASE("terrain probe: a coverage mask cuts a layer to reveal the base (Vk + WebGPU)")
{
    // A flat terrain, one palette layer (blue) painted one-hot over a distinct base (red). A coverage
    // mask with the layer's tileScale equal to the terrain world size (exactly one repeat spans the
    // footprint) splits the footprint: the opaque half renders the layer (blue), the zero half
    // reveals the base (red). Flipping the mask swaps the halves; a uniform zero mask reveals base
    // everywhere; no mask (off) renders the layer everywhere.
    enum Pattern { Off = 0, SplitA = 1, SplitB = 2, Cut = 3 };
    constexpr f32 kWorld = 130.0f; // MakeFlat's world size: one mask repeat spans the footprint

    auto run = [](rhi::Backend* backend, int pattern) -> Probe
    {
        rhi::Device* dev = (backend != nullptr) ? testsupport::MakeTestDevice(backend) : nullptr;
        if (dev == nullptr)
        {
            return Probe{};
        }
        Probe p;
        {
            engine::terrain::TerrainSplatTextureCache splatCache;
            engine::terrain::TerrainPaletteTextureCache paletteCache;
            RefPtr<tmodel::SplatWeights> sw = MakeStripeWeights(1); // layer 0 one-hot everywhere
            const Float3 blue[1] = {Float3{0.05f, 0.05f, 0.9f}};
            RefPtr<tmodel::TerrainPaletteData> palette = MakePaletteData(Span<const Float3>{blue, 1});
            const usize sliceBytes =
                tmodel::TerrainPaletteData::SliceBytes(palette->sliceSize, palette->mipCount);
            if (pattern != Off)
            {
                palette->maskTexels.Resize(sliceBytes); // one slice
                const u32 side = palette->sliceSize;
                for (u32 y = 0; y < side; ++y)
                {
                    for (u32 x = 0; x < side; ++x)
                    {
                        u8 v = 255;
                        if (pattern == SplitA) { v = (x < side / 2) ? 255 : 0; }
                        else if (pattern == SplitB) { v = (x < side / 2) ? 0 : 255; }
                        else if (pattern == Cut) { v = 0; }
                        u8* t = palette->maskTexels.Data() + (y * side + x) * 4;
                        t[0] = v; t[1] = v; t[2] = v; t[3] = 255;
                    }
                }
            }
            RefPtr<texture::Texture> red = MakeSolid(*dev, 230, 30, 30); // base albedo (revealed)
            f32 scales[1] = {kWorld}; // one mask repeat across the footprint

            ProbeCfg cfg;
            cfg.terrain = MakeFlat();
            const engine::terrain::SplatTextureViews views =
                splatCache.GetOrCreate(*dev, *sw, sw->Version());
            const engine::terrain::PaletteGpu gpu =
                paletteCache.GetOrCreate(*dev, *palette, Span<const f32>{scales, 1});
            REQUIRE(views.weightView != nullptr);
            REQUIRE(gpu.arrayView != nullptr);
            if (pattern != Off)
            {
                REQUIRE(gpu.maskArrayView != nullptr); // HasMask(): the cache built the array
            }
            cfg.weightView = views.weightView;
            cfg.indexView = views.indexView;
            cfg.baseAlbedoView = red->View();
            cfg.paletteArrayView = gpu.arrayView;
            cfg.tileScaleBuffer = gpu.tileScaleBuffer;
            cfg.tileScaleGeneration = gpu.generation;
            cfg.paletteCount = 1;
            cfg.maskArrayView = (pattern != Off) ? gpu.maskArrayView : nullptr;
            p = RenderTerrainProbe(*dev, cfg);

            splatCache.Clear(*dev);
            paletteCache.Clear(*dev);
        }
        dev->Destroy();
        return p;
    };

    const auto R = [](const Probe& p) { return p.leftR + p.rightR; };
    const auto B = [](const Probe& p) { return p.leftB + p.rightB; };

    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());

    const Probe off = run(vulkan, Off);
    if (!off.valid)
    {
        MESSAGE("Vulkan unavailable - terrain coverage-mask probe skipped");
        if (vulkan != nullptr) { vulkan->Destroy(); }
        if (webgpu != nullptr) { webgpu->Destroy(); }
        return;
    }
    const Probe cut = run(vulkan, Cut);
    const Probe splitA = run(vulkan, SplitA);
    const Probe splitB = run(vulkan, SplitB);

    std::printf("[terrain-mask] off(R=%.0f B=%.0f) cut(R=%.0f B=%.0f) "
                "splitA(lB=%.0f lR=%.0f rB=%.0f rR=%.0f) splitB(lB=%.0f lR=%.0f rB=%.0f rR=%.0f)\n",
                R(off), B(off), R(cut), B(cut), splitA.leftB, splitA.leftR, splitA.rightB,
                splitA.rightR, splitB.leftB, splitB.leftR, splitB.rightB, splitB.rightR);

    // Off: with no mask the layer (blue) shows everywhere.
    CHECK(B(off) > R(off) * 1.5);
    // Cut: an all-zero mask reveals the base (red) everywhere; coverage falls to base.
    CHECK(R(cut) > B(cut) * 1.5);
    // Split: the two screen halves differ; one shows the layer (blue), the other the base (red).
    const bool aLeftBlue = splitA.leftB > splitA.leftR;
    const bool aRightBlue = splitA.rightB > splitA.rightR;
    CHECK(aLeftBlue != aRightBlue); // a real spatial split is present on screen
    // Flip: swapping the mask swaps which half shows the layer.
    const bool bLeftBlue = splitB.leftB > splitB.leftR;
    CHECK(bLeftBlue != aLeftBlue);

    // WebGPU parity on the split fixture (mask SampleGrad on the array is where naga can diverge).
    const Probe wA = run(webgpu, SplitA);
    if (wA.valid)
    {
        CHECK((wA.leftB > wA.leftR) == aLeftBlue);
        CHECK((wA.rightB > wA.rightR) == aRightBlue);
        CHECK(wA.total == doctest::Approx(splitA.total).epsilon(0.05));
    }
    else
    {
        MESSAGE("WebGPU unavailable - terrain coverage-mask parity skipped");
    }

    if (vulkan != nullptr) { vulkan->Destroy(); }
    if (webgpu != nullptr) { webgpu->Destroy(); }
}

TEST_CASE("terrain probe: a +green base normal leans -Z (R5 - pins GL green-up, no author flip)")
{
    // The terrain tangent frame's B = cross(n, T) points toward -Z, which under the top-left UV
    // origin is glTF/GL green-up, so Poly Haven `_nor_gl_` maps import as they are. The layer PBR
    // probe pins only the U axis (its test normal has green = 0); this pins the green sign. A base
    // normal tilted toward +green (128, 204, 229) brightens under a -Z sun and darkens under +Z.
    // If this fails, fix B's sign in the shader rather than asking authors to flip their maps.
    auto run = [](rhi::Backend* backend, const Float3& toLight, bool useNormal) -> Probe
    {
        rhi::Device* dev = (backend != nullptr) ? testsupport::MakeTestDevice(backend) : nullptr;
        if (dev == nullptr)
        {
            return Probe{};
        }
        Probe p;
        {
            RefPtr<texture::Texture> albedo = MakeSolid(*dev, 170, 170, 170);
            RefPtr<texture::Texture> normal = MakeSolid(*dev, 128, 204, 229); // tangent (0, +.6, +.8)
            ProbeCfg cfg;
            cfg.terrain = MakeFlat();
            cfg.eye = Float3{0, 120, 0.001f};
            cfg.toLight = &toLight;
            cfg.baseAlbedoView = albedo->View();
            if (useNormal)
            {
                cfg.baseNormalView = normal->View();
            }
            cfg.baseTileScale = 1000.0f;
            p = RenderTerrainProbe(*dev, cfg);
        }
        dev->Destroy();
        return p;
    };

    const Float3 plusZ = Normalized(Float3{0.0f, 0.5f, 0.85f});
    const Float3 minusZ = Normalized(Float3{0.0f, 0.5f, -0.85f});

    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);

    const Probe nMinus = run(vulkan, minusZ, true); // +green normal, -Z sun (aligned with lean)
    if (!nMinus.valid)
    {
        MESSAGE("Vulkan unavailable - terrain green-sign probe skipped");
        if (vulkan != nullptr) { vulkan->Destroy(); }
        return;
    }
    const Probe nPlus = run(vulkan, plusZ, true);   // +green normal, +Z sun (opposed)
    const Probe cMinus = run(vulkan, minusZ, false); // flat control
    const Probe cPlus = run(vulkan, plusZ, false);

    std::printf("[terrain-green] normal -Z=%.0f +Z=%.0f | flat -Z=%.0f +Z=%.0f\n", nMinus.total,
                nPlus.total, cMinus.total, cPlus.total);

    // Flat control: the two sun directions shade the flat ground ~equally.
    CHECK(cMinus.total == doctest::Approx(cPlus.total).epsilon(0.06));
    // +green normal leans toward -Z (B = cross(n,T) = -Z): the -Z sun brightens, the +Z sun darkens.
    CHECK(nMinus.total > cMinus.total * 1.10);
    CHECK(nPlus.total < cPlus.total * 0.90);

    if (vulkan != nullptr) { vulkan->Destroy(); }
}

TEST_CASE("terrain probe: no base albedo -> the RAMP is the implicit base (never white)")
{
    // A terrain with paint layers bound but no base albedo and nothing painted keeps its
    // fresh-terrain look: the height/slope ramp (greenish), not the white dummy. Adding a paint
    // layer does not turn the unpainted terrain white.
    rhi::Backend* vulkan = nullptr;
    rhi::Device* device = MakeVulkan(vulkan);
    if (device == nullptr)
    {
        MESSAGE("Vulkan unavailable - ramp-base probe skipped");
        if (vulkan != nullptr) { vulkan->Destroy(); }
        return;
    }
    Probe p;
    {
        engine::terrain::TerrainSplatTextureCache splatCache;
        engine::terrain::TerrainPaletteTextureCache paletteCache;
        RefPtr<tmodel::SplatWeights> sw =
            MakeRef<tmodel::SplatWeights>(DefaultAllocator(), 16, 16); // all-zero: nothing painted
        const Float3 blue{0.12f, 0.12f, 0.86f};
        RefPtr<tmodel::TerrainPaletteData> palette = MakePaletteData(Span<const Float3>{&blue, 1});
        const f32 scale = 1000.0f;
        const engine::terrain::PaletteGpu gpu =
            paletteCache.GetOrCreate(*device, *palette, Span<const f32>{&scale, 1});
        const engine::terrain::SplatTextureViews views =
            splatCache.GetOrCreate(*device, *sw, sw->Version());
        ProbeCfg cfg;
        cfg.terrain = MakeFlat();
        cfg.weightView = views.weightView;
        cfg.indexView = views.indexView;
        cfg.paletteArrayView = gpu.arrayView;
        cfg.tileScaleBuffer = gpu.tileScaleBuffer;
        cfg.tileScaleGeneration = gpu.generation;
        cfg.paletteCount = 1; // hasWeights true, hasBase false: the confusing case
        p = RenderTerrainProbe(*device, cfg);
        splatCache.Clear(*device);
        paletteCache.Clear(*device);
    }
    REQUIRE(p.valid);
    const f64 half = static_cast<f64>(kSize) * kSize / 2.0;
    std::printf("[terrain-rampbase] left R=%.1f B=%.1f per px\n", p.leftR / half, p.leftB / half);
    CHECK(p.leftR / half < 150.0);     // not the white dummy (white lit reads near-saturated)
    CHECK(p.leftR > p.leftB);          // the ramp is warm-green (R > B at low heightT)
    CHECK(p.filled > 30000u);          // and the terrain still rendered
    device->Destroy();
    vulkan->Destroy();
}

TEST_CASE("terrain probe: a masked layer reveals the PAINTED layer beneath, not base (Vk + WebGPU)")
{
    // Two painted palette layers, ground (red, layer 0) and grass (green, layer 1), each at about
    // 0.5 weight, over a distinct base (blue). Grass carries a fully zero coverage mask; ground has
    // none (opaque). The freed grass coverage flows to the painted ground (red), not the base canvas
    // (blue): what you painted is revealed. If coverage went to the base instead, blue would
    // dominate and this flips.
    auto run = [](rhi::Backend* backend, bool maskGrass) -> Probe
    {
        rhi::Device* dev = (backend != nullptr) ? testsupport::MakeTestDevice(backend) : nullptr;
        if (dev == nullptr)
        {
            return Probe{};
        }
        Probe p;
        {
            engine::terrain::TerrainSplatTextureCache splatCache;
            engine::terrain::TerrainPaletteTextureCache paletteCache;
            RefPtr<tmodel::SplatWeights> sw = MakeTwoLayerWeights(128, 128); // ground and grass about 0.5 each
            const Float3 colors[2] = {Float3{0.9f, 0.05f, 0.05f},  // layer 0 ground = red
                                      Float3{0.05f, 0.9f, 0.05f}}; // layer 1 grass = green
            RefPtr<tmodel::TerrainPaletteData> palette = MakePaletteData(Span<const Float3>{colors, 2});
            const usize sliceBytes =
                tmodel::TerrainPaletteData::SliceBytes(palette->sliceSize, palette->mipCount);
            if (maskGrass)
            {
                // slice 0 (ground) opaque, slice 1 (grass) fully zero, so grass coverage is freed.
                palette->maskTexels.Resize(sliceBytes * 2);
                for (usize t = 0; t < sliceBytes; t += 4)
                {
                    u8* g = palette->maskTexels.Data() + t; // ground slice
                    g[0] = 255; g[1] = 255; g[2] = 255; g[3] = 255;
                    u8* r = palette->maskTexels.Data() + sliceBytes + t; // grass slice
                    r[0] = 0; r[1] = 0; r[2] = 0; r[3] = 255;
                }
            }
            RefPtr<texture::Texture> blue = MakeSolid(*dev, 30, 30, 230); // base (must not win)
            f32 scales[2] = {1000.0f, 1000.0f};

            ProbeCfg cfg;
            cfg.terrain = MakeFlat();
            const engine::terrain::SplatTextureViews views =
                splatCache.GetOrCreate(*dev, *sw, sw->Version());
            const engine::terrain::PaletteGpu gpu =
                paletteCache.GetOrCreate(*dev, *palette, Span<const f32>{scales, 2});
            REQUIRE(views.weightView != nullptr);
            REQUIRE(gpu.arrayView != nullptr);
            cfg.weightView = views.weightView;
            cfg.indexView = views.indexView;
            cfg.baseAlbedoView = blue->View();
            cfg.paletteArrayView = gpu.arrayView;
            cfg.tileScaleBuffer = gpu.tileScaleBuffer;
            cfg.tileScaleGeneration = gpu.generation;
            cfg.paletteCount = 2;
            cfg.maskArrayView = maskGrass ? gpu.maskArrayView : nullptr;
            p = RenderTerrainProbe(*dev, cfg);

            splatCache.Clear(*dev);
            paletteCache.Clear(*dev);
        }
        dev->Destroy();
        return p;
    };

    const auto R = [](const Probe& p) { return p.leftR + p.rightR; };
    const auto B = [](const Probe& p) { return p.leftB + p.rightB; };

    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());

    const Probe masked = run(vulkan, true);
    if (!masked.valid)
    {
        MESSAGE("Vulkan unavailable - terrain reveal-painted-layer probe skipped");
        if (vulkan != nullptr) { vulkan->Destroy(); }
        if (webgpu != nullptr) { webgpu->Destroy(); }
        return;
    }

    std::printf("[terrain-reveal] masked-grass R(ground)=%.0f B(base)=%.0f\n", R(masked), B(masked));

    // The freed grass coverage went to the painted ground (red), not the base (blue).
    CHECK(R(masked) > B(masked) * 1.5);

    // Unmasked A/B baseline: without the mask the same scene splits ground and grass evenly, and the
    // masked run's red total clearly exceeds it (the redistribution happened, not just "red exists").
    const Probe unmasked = run(vulkan, false);
    REQUIRE(unmasked.valid);
    const auto G = [](const Probe& p) { return p.leftG + p.rightG; };
    std::printf("[terrain-reveal] unmasked R=%.0f G=%.0f B=%.0f\n", R(unmasked), G(unmasked),
                B(unmasked));
    CHECK(G(unmasked) > B(unmasked));          // grass visible before masking
    CHECK(R(masked) > R(unmasked) * 1.3);      // masking grass boosted ground coverage
    CHECK(G(masked) < G(unmasked) * 0.3);      // and actually removed the grass

    const Probe wMasked = run(webgpu, true);
    if (wMasked.valid)
    {
        CHECK(R(wMasked) > B(wMasked) * 1.5);
        CHECK(R(wMasked) == doctest::Approx(R(masked)).epsilon(0.05));
    }
    else
    {
        MESSAGE("WebGPU unavailable - terrain reveal-painted-layer parity skipped");
    }

    if (vulkan != nullptr) { vulkan->Destroy(); }
    if (webgpu != nullptr) { webgpu->Destroy(); }
}

// Terrain holes: a block of cut samples at the dome's centre draws
// nothing there, so the clear colour shows through where the reference dome is lit.
TEST_CASE("terrain holes probe: a cut at the dome's centre shows the clear colour - Vulkan + WebGPU")
{
    const auto probeOn = [](rhi::Device& device)
    {
        ProbeCfg solid;
        solid.terrain = MakeDome();
        const Probe ref = RenderTerrainProbe(device, solid);
        REQUIRE(ref.valid);
        ProbeCfg holed;
        holed.terrain = MakeDome();
        const i32 c = holed.terrain->Size() / 2;
        for (i32 z = c - 8; z <= c + 8; ++z)
        {
            for (i32 x = c - 8; x <= c + 8; ++x)
            {
                holed.terrain->SetHole(x, z, true); // 17 x 17 cut samples: the centre block lies inside
            }
        }
        const Probe cut = RenderTerrainProbe(device, holed);
        REQUIRE(cut.valid);
        std::printf("[terrain-holes] ref center=%.0f filled=%u | cut center=%.0f filled=%u\n",
                    ref.centerLuma, ref.filled, cut.centerLuma, cut.filled);
        CHECK(ref.centerLuma > 0.0);   // the dome's top is lit
        CHECK(cut.centerLuma == 0.0);  // the hole: nothing drawn, black clear
        CHECK(cut.filled < ref.filled);
        CHECK(cut.filled > ref.filled / 2u); // the rest of the dome is still there
        // The alpha-tested rim: the same cut with the mask unbound draws the geometry rule alone
        // (a quad stays while one corner is solid), so the ring the pixel shader discards (half a
        // cell around the cut) comes back as filled pixels.
        holed.holeMask = false;
        const Probe geometryOnly = RenderTerrainProbe(device, holed);
        REQUIRE(geometryOnly.valid);
        std::printf("[terrain-holes] geometry-only filled=%u (mask discards %u)\n", geometryOnly.filled,
                    geometryOnly.filled - cut.filled);
        CHECK(geometryOnly.centerLuma == 0.0); // the centre block is inside the dropped quads
        CHECK(geometryOnly.filled > cut.filled + 64u); // the rim ring only the mask can remove
    };
    // Vulkan runs with validation here: the HOLES depth PSO pairs the "terrain" VS with
    // terrain_depth.ps, and a stage-interface mismatch between them is a validation error the
    // driver otherwise tolerates (the prepass then discards by garbage). Any error fails the test.
    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{.enableValidation = true}, vulkan);
    if (rhi::Device* device = vulkan != nullptr ? testsupport::MakeTestDevice(vulkan) : nullptr)
    {
        const u32 errorsBefore = rhi::vk::ValidationErrorCount();
        probeOn(*device);
        device->Destroy();
        CHECK(rhi::vk::ValidationErrorCount() == errorsBefore);
    }
    else
    {
        MESSAGE("Vulkan unavailable - vulkan holes probe skipped");
    }
    if (vulkan != nullptr)
    {
        vulkan->Destroy();
    }
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        probeOn(*device);
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu holes probe skipped");
    }
    if (webgpu != nullptr)
    {
        webgpu->Destroy();
    }
}

