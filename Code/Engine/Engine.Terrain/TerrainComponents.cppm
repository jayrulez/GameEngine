// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.terrain:components - the TerrainComponent and its manager.
//
// TerrainComponent references a cooked TerrainResource (heightfield, splatmap and layers, all
// resolved through the manager) and carries per-instance cast-shadows and visible flags. The manager
// is also the scene's IRenderDataProvider: it builds the chunk model once per heightfield, gets the
// GPU height texture from the cache, and emits one TerrainRenderData per visible terrain (the
// per-view cull and LOD run later, in the renderer's Resolve). Physics is a separate
// ShapeKind::Heightfield collider over the same heightfield.

module;
#include "Core/Prelude.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (compiles to nothing when disabled)

export module engine.terrain:components;

import foundation.core;
import engine.domain;
import foundation.profiler;
import foundation.rhi;              // Device, TextureView (the GPU height texture)
import foundation.scene;
import foundation.resource;
import foundation.render;           // IRenderDataProvider, ExtractedScene, RenderCategories
import foundation.heightfield;      // Heightfield (the shared grid)
import foundation.terrain;          // chunk model (BuildChunks, TerrainQuadtree, thresholds)
import foundation.terrain.resource; // TerrainResource
import foundation.texture.resource; // texture::Texture::View() (splatmap and layer albedos)
import :heighttexture;              // TerrainHeightTextureCache
import :splattexture;               // TerrainSplatTextureCache (RGBA8 splat, paint re-upload)
import :renderdata;                 // TerrainRenderData
import :holedmesh;                  // TerrainHoledMeshCache

using namespace foundation::core;

export namespace engine::terrain
{
    namespace render = foundation::render;
    namespace rhi = foundation::rhi;
    namespace scene = foundation::scene;
    namespace heightfield = foundation::heightfield;
    namespace tmodel = foundation::terrain;
    namespace texture = foundation::texture;

    using foundation::terrain::TerrainResource;

    struct TerrainComponent
    {
        // The cooked terrain product (Ref<TerrainResource>): heightfield, splatmap and layers. Bound
        // through the manager; the heightfield inside is the source of truth shared with the collider.
        foundation::resource::Ref<TerrainResource> terrain;
        bool castShadows = true; // this instance casts into the CSM (terrain default-on)
        bool visible = true;
        f32 lodBias = 0.0f; // +ve = coarser sooner (fewer triangles), -ve = hold detail further out
    };

    inline void Serialize(ISerializer& ar, TerrainComponent& c)
    {
        foundation::core::Serialize(ar, "terrain", c.terrain);
        foundation::core::Serialize(ar, "castShadows", c.castShadows);
        foundation::core::Serialize(ar, "visible", c.visible);
        foundation::core::Serialize(ar, "lodBias", c.lodBias);
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager, TerrainComponent& c)
    {
        c.terrain.Bind(manager);
    }

    class TerrainComponentManager final
        : public foundation::scene::SerializableComponentManager<TerrainComponent>,
          public render::IRenderDataProvider,
          public foundation::scene::IStaticGeometrySource
    {
    public:
        TerrainComponentManager()
            : SerializableComponentManager<TerrainComponent>(u8"terrain")
        {
        }

        /// A terrain is level ground: its surface (holes left open) is static geometry, sampled no
        /// finer than `detail`, so navigation walks it whether or not a heightfield collider
        /// covers it too (the same surface twice rasterizes the same).
        [[nodiscard]] foundation::scene::IStaticGeometrySource* AsStaticGeometrySource() noexcept override
        {
            return this;
        }
        void CollectStaticGeometry(scene::Scene& scene, const AABB& bounds, f32 detail,
                                   Array<Float3>& outTriangles) override;

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return false; }

        /// Wired by the TerrainSubsystem after it creates and registers the TerrainRenderer: the
        /// device for the height-texture uploads, the renderer's dispatch id stamped on each render
        /// item, and the frame-aged retire queue so a regenerate or version bump never destroys a
        /// texture an in-flight frame still samples.
        void SetRenderContext(rhi::Device* device, u16 rendererId,
                              render::GpuRetireQueue* retire = nullptr) noexcept
        {
            m_device = device;
            m_rendererId = rendererId;
            m_heightTextures.SetRetireQueue(retire);
            m_splatTextures.SetRetireQueue(retire);
            m_paletteTextures.SetRetireQueue(retire);
            m_holedMeshes.SetRetireQueue(retire);
            m_holeTextures.SetRetireQueue(retire);
        }

        /// Tears down this manager's GPU-side state through the wired device. Called by the
        /// TerrainSubsystem on scene destroy and at its own shutdown, both points where the device
        /// is still alive. The manager's destructor cannot do this: the scene can be torn down
        /// after the render device is gone. Safe to call repeatedly; later extracts no-op (the
        /// device is nulled).
        void ClearGpu()
        {
            if (m_device != nullptr)
            {
                m_heightTextures.Clear(*m_device);
                m_splatTextures.Clear(*m_device);
                m_paletteTextures.Clear(*m_device);
                m_holedMeshes.Clear(*m_device);
                m_holeTextures.Clear(*m_device);
            }
            m_device = nullptr;
        }

        [[nodiscard]] usize HeightTextureCount() const noexcept { return m_heightTextures.Size(); }
        /// Holed chunk meshes cached for a heightfield (the holes test observable).
        [[nodiscard]] usize HoledMeshCount(u64 heightfieldUid) const noexcept
        {
            return m_holedMeshes.MeshCount(heightfieldUid);
        }
        [[nodiscard]] usize HoleTextureCount() const noexcept { return m_holeTextures.Size(); }
        [[nodiscard]] usize SplatTextureCount() const noexcept { return m_splatTextures.Size(); }
        [[nodiscard]] usize PaletteTextureCount() const noexcept
        {
            return m_paletteTextures.Size();
        }

        /// render::IRenderDataProvider: one TerrainRenderData per visible terrain (the whole terrain
        /// is one draw-list item; the renderer culls and LODs its chunks per view). Builds the chunk
        /// model on first sight of a heightfield, and the GPU height texture through the cache.
        void ExtractRenderData(render::ExtractedScene& snapshot) override
        {
            PROFILE_SCOPE("Terrain.Extract");
            if (m_device == nullptr)
            {
                return; // no GPU device wired (headless scene consumer): nothing to draw
            }
            ForEach(
                [&](TerrainComponent& c, scene::EntityHandle owner)
                {
                    if (!c.visible || (m_scene != nullptr && !m_scene->IsEffectivelyActive(owner)))
                    {
                        return;
                    }
                    TerrainResource* res = c.terrain.Get();
                    if (res == nullptr)
                    {
                        return;
                    }
                    heightfield::Heightfield* hf = res->heightfield.Get();
                    if (hf == nullptr || hf->IsEmpty())
                    {
                        return;
                    }
                    const ChunkCache& cache = GetOrBuildChunks(hf);
                    if (cache.chunks.IsEmpty())
                    {
                        return;
                    }
                    // The GPU height texture (keyed by the heightfield; re-uploads on a version bump).
                    rhi::TextureView* heightView =
                        m_heightTextures.GetOrCreate(*m_device, *hf, hf->Version());
                    if (heightView == nullptr)
                    {
                        return;
                    }

                    // Copy the chunk grid and quadtree nodes into the frame arena: the snapshot is
                    // read at record time, after arbitrary mid-frame scene and manager mutations, so
                    // a borrowed pointer into the cache could be freed by then.
                    const Span<const tmodel::TerrainChunk> chunkCopy = snapshot.AddArray(
                        Span<const tmodel::TerrainChunk>{cache.chunks.Data(), cache.chunks.Size()});
                    const Span<const tmodel::TerrainQuadtree::Node> nodeCopy =
                        snapshot.AddArray(cache.quadtree.Nodes());
                    if (chunkCopy.IsEmpty() || nodeCopy.IsEmpty())
                    {
                        return; // arena exhausted: skip rather than snapshot dangling state
                    }
                    // Holed chunks' own index buffers (none for the common terrain), copied too.
                    const Span<const HoledChunkMesh> holed = m_holedMeshes.GetOrBuild(
                        *m_device, *hf,
                        Span<const tmodel::TerrainChunk>{cache.chunks.Data(), cache.chunks.Size()},
                        hf->Version());
                    const Span<const HoledChunkMesh> holedCopy =
                        holed.IsEmpty() ? Span<const HoledChunkMesh>{} : snapshot.AddArray(holed);
                    rhi::TextureView* holeView =
                        hf->HasHoles() ? m_holeTextures.GetOrCreate(*m_device, *hf, hf->Version())
                                       : nullptr;

                    TerrainRenderData* rd = snapshot.Add<TerrainRenderData>();
                    if (rd == nullptr)
                    {
                        return;
                    }
                    rd->category = render::RenderCategories::Opaque;
                    rd->rendererId = m_rendererId;
                    rd->entityId = render::EntityTag::Pack(owner.index, owner.generation); // GPU pick
                    rd->chunks = chunkCopy.Data();
                    rd->nodes = nodeCopy.Data();
                    rd->chunkCount = static_cast<u32>(chunkCopy.Size());
                    rd->nodeCount = static_cast<u32>(nodeCopy.Size());
                    rd->heightView = heightView;
                    rd->holedMeshes = holedCopy.Data();
                    rd->holedMeshCount = static_cast<u32>(holedCopy.Size());
                    rd->holeView = holeView;
                    rd->chunkToWorld = (m_scene != nullptr) ? m_scene->GetWorldMatrix(owner)
                                                            : Float4x4::Identity();
                    rd->gridSize = hf->Size();
                    rd->worldSizeXZ = hf->WorldSize();
                    rd->minY = hf->MinY();
                    rd->maxY = hf->MaxY();

                    const u32 n = Min(static_cast<u32>(kDefaultThresholdCount), kMaxLodThresholds);
                    for (u32 i = 0; i < n; ++i)
                    {
                        rd->thresholds[i] = kDefaultThresholds[i];
                    }
                    rd->thresholdCount = n;
                    rd->lodBias = c.lodBias;

                    // Top-K splat material: the CPU SplatWeights (the painted source of truth)
                    // derives the weight and index texture pair, cached by uid and version. A
                    // paint's version bump re-uploads, and the set-3 bind cache rebuilds on the new
                    // view ids (the live repaint path).
                    if (foundation::terrain::SplatWeights* sw = res->weights.Get())
                    {
                        const SplatTextureViews views =
                            m_splatTextures.GetOrCreate(*m_device, *sw, sw->Version());
                        rd->weightView = views.weightView;
                        rd->indexView = views.indexView;
                    }
                    if (texture::Texture* baseAlb = res->base.albedo.Get())
                    {
                        rd->baseAlbedoView = baseAlb->View();
                    }
                    if (texture::Texture* baseNrm = res->base.normal.Get())
                    {
                        rd->baseNormalView = baseNrm->View();
                    }
                    if (texture::Texture* baseOrm = res->base.orm.Get())
                    {
                        rd->baseOrmView = baseOrm->View();
                    }
                    if (texture::Texture* baseHgt = res->base.height.Get())
                    {
                        rd->baseHeightView = baseHgt->View();
                    }
                    rd->baseTileScale = res->base.tileScale;
                    rd->heightBlendContrast = res->heightBlendContrast;
                    // The palette Texture2DArray and tileScale buffer (cook-built texels, cached by
                    // TerrainPaletteData::uid and the tile-scale hash; a palette edit re-cooks to a
                    // new uid, which rebuilds and retires the old array).
                    if (res->paletteData.Get() != nullptr && res->paletteData->IsValid())
                    {
                        Array<f32> scales;
                        for (usize li = 0; li < res->palette.Size(); ++li)
                        {
                            scales.PushBack(res->palette[li].tileScale);
                        }
                        const PaletteGpu palette = m_paletteTextures.GetOrCreate(
                            *m_device, *res->paletteData,
                            Span<const f32>{scales.Data(), scales.Size()});
                        rd->paletteArrayView = palette.arrayView;
                        rd->normalArrayView = palette.normalArrayView;
                        rd->ormArrayView = palette.ormArrayView;
                        rd->heightArrayView = palette.heightArrayView;
                        rd->maskArrayView = palette.maskArrayView;
                        rd->tileScaleBuffer = palette.tileScaleBuffer;
                        rd->tileScaleGeneration = palette.generation;
                        rd->paletteCount = res->paletteData->sliceCount;
                    }
                    else
                    {
                        rd->paletteCount = 0; // no cooked palette: pure base (or the height ramp)
                    }

                    // Whole-terrain bounding sphere (world), so the framework does not cull the
                    // terrain while any chunk is visible.
                    const Float3 localCenter = cache.localBounds.Center();
                    rd->worldCenter = TransformPoint(localCenter, rd->chunkToWorld);
                    rd->worldRadius = Length(cache.localBounds.Extents());
                });
        }

    private:
        struct ChunkCache
        {
            Array<tmodel::TerrainChunk> chunks;
            tmodel::TerrainQuadtree quadtree;
            AABB localBounds = AABB::Empty();
            u64 version = 0;
        };

        // Chunk model per heightfield (shared by every terrain referencing it). Keyed by the
        // heightfield's uid, never its pointer, as the height-texture cache is: a dead grid's
        // address reused by a fresh one at an equal version would serve the dead grid's chunk
        // bounds. A hot-reload swap (a new object, so a new uid) or a sculpt or regen (a version
        // bump) rebuilds; the chunk Y bounds move with the heights, so cull and LOD stay correct.
        const ChunkCache& GetOrBuildChunks(const heightfield::Heightfield* hf)
        {
            if (ChunkCache* found = m_chunkCache.Find(hf->uid); found != nullptr &&
                found->version == hf->Version())
            {
                return *found;
            }
            ChunkCache cache;
            cache.version = hf->Version();
            tmodel::BuildChunks(*hf, cache.chunks);
            cache.quadtree.Build(
                Span<const tmodel::TerrainChunk>{cache.chunks.Data(), cache.chunks.Size()},
                tmodel::ChunksPerSide(hf->Size()));
            for (usize i = 0; i < cache.chunks.Size(); ++i)
            {
                cache.localBounds = Merge(cache.localBounds, cache.chunks[i].bounds);
            }
            m_chunkCache.InsertOrAssign(hf->uid, Move(cache));
            return *m_chunkCache.Find(hf->uid);
        }

        // Descending coverage thresholds over the 7 chunk LODs (thresholds[0] = 1.0 = level-0 fallback).
        static constexpr f32 kDefaultThresholds[] = {1.0f,   0.25f,  0.08f, 0.03f,
                                                      0.012f, 0.005f, 0.002f};
        static constexpr usize kDefaultThresholdCount =
            sizeof(kDefaultThresholds) / sizeof(kDefaultThresholds[0]);

        scene::Scene* m_scene = nullptr;
        rhi::Device* m_device = nullptr;
        u16 m_rendererId = 0;
        TerrainHeightTextureCache m_heightTextures;
        TerrainSplatTextureCache m_splatTextures;
        TerrainPaletteTextureCache m_paletteTextures;
        TerrainHoledMeshCache m_holedMeshes; // holed chunks' index buffers
        TerrainHoleTextureCache m_holeTextures; // the R8 hole mask the HOLES shaders sample
        HashMap<u64, ChunkCache> m_chunkCache; // key = Heightfield::uid (never a pointer)
    };

    // Scene-composition hooks (the SceneModule pair every domain has).
    void AddTerrainSceneManagers(foundation::scene::Scene& scene);
    void RegisterTerrainComponentReflection();
}

export namespace engine::terrain
{
    /// This domain's declaration: what it brings to a scene, to reflection, to the script surface
    /// and which resource modules come with it. Defined in the implementation unit (one instance
    /// per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& TerrainDomain() noexcept;
}
